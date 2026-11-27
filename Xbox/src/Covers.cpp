#include "Covers.h"

#include "Net.h"
#include "UwpPlatform.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <string_view>
#include <fstream>
#include <optional>
#include <sstream>

namespace fs = std::filesystem;
using namespace winrt;
using namespace winrt::Windows::Graphics::Imaging;
using namespace winrt::Windows::Storage::Streams;
using namespace net;

namespace {

constexpr uint32_t kMaxEdge = 512; // sharp enough for a TV

struct WikiResult
{
	std::string text;
	std::string imageUrl; // the article's lead picture (for a game, nearly always its box)
};

// nullopt when Wikipedia could not be reached (then nothing is saved, so it is tried again next time)
std::optional<WikiResult> WikipediaLookup(Windows::Web::Http::HttpClient& client, const std::string& title, const char* system)
{
	std::string url = "https://en.wikipedia.org/w/api.php?action=query&format=json&generator=search&gsrsearch=" +
		UrlEncode(title + " " + system) + "&gsrlimit=1&prop=extracts|pageimages&exintro=1&explaintext=1&exchars=520"
		"&piprop=thumbnail&pithumbsize=512";
	int status = 0;
	auto body = HttpGet(client, url, &status);
	if(!body) {
		uwp::Log("Wikipedia answered " + std::to_string(status) + " for " + title);
		return std::nullopt;
	}
	WikiResult result;
	result.text = JsonValue(*body, "extract");
	size_t thumb = body->find("\"thumbnail\":");
	if(thumb != std::string::npos) {
		result.imageUrl = JsonValue(body->substr(thumb), "source");
	}
	return result;
}

} // namespace

bool DecodeImage(const std::string& bytes, std::vector<uint8_t>& rgba, int& width, int& height, uint32_t maxEdge)
{
	try {
		InMemoryRandomAccessStream stream;
		DataWriter writer(stream);
		writer.WriteBytes(array_view<const uint8_t>((const uint8_t*)bytes.data(), (const uint8_t*)bytes.data() + bytes.size()));
		writer.StoreAsync().get();
		writer.DetachStream();
		stream.Seek(0);

		BitmapDecoder decoder = BitmapDecoder::CreateAsync(stream).get();
		uint32_t w = decoder.PixelWidth();
		uint32_t h = decoder.PixelHeight();
		if(w == 0 || h == 0) {
			return false;
		}
		double scale = std::min(1.0, (double)maxEdge / std::max(w, h));
		BitmapTransform transform;
		transform.ScaledWidth(std::max(1u, (uint32_t)(w * scale)));
		transform.ScaledHeight(std::max(1u, (uint32_t)(h * scale)));
		transform.InterpolationMode(BitmapInterpolationMode::Fant);
		PixelDataProvider pixels = decoder.GetPixelDataAsync(BitmapPixelFormat::Rgba8, BitmapAlphaMode::Straight, transform,
			ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage).get();
		com_array<uint8_t> data = pixels.DetachPixelData();
		width = (int)transform.ScaledWidth();
		height = (int)transform.ScaledHeight();
		if(data.size() < (size_t)width * height * 4) {
			return false;
		}
		rgba.assign(data.begin(), data.end());
		return true;
	} catch(...) {
		return false;
	}
}

Covers::Covers(fs::path folder) : _folder(std::move(folder))
{
	std::error_code ec;
	fs::create_directories(_folder, ec);
	_thread = std::thread([this] { Worker(); });
}

Covers::~Covers()
{
	{
		std::lock_guard lock(_lock);
		_stop = true;
		_queue.clear();
	}
	_wake.notify_all();
	if(_thread.joinable()) {
		_thread.detach(); // a download may be in progress; the app is closing anyway
	}
	for(auto& [title, info] : _info) {
		if(info.texture) {
			SDL_DestroyTexture(info.texture);
		}
	}
}

const Covers::Info& Covers::Get(SDL_Renderer* renderer, const GameEntry& game)
{
	auto [it, added] = _info.try_emplace(game.title);
	Info& info = it->second;
	if(added) {
		{
			std::lock_guard lock(_lock);
			// Newest request first: the player is looking at it now
			_queue.push_front(game);
		}
		_wake.notify_one();
	}
	if(!info.coverDone || !info.descriptionDone) {
		Result result;
		bool have = false;
		{
			std::lock_guard lock(_lock);
			auto found = _results.find(game.title);
			if(found != _results.end()) {
				result = std::move(found->second);
				_results.erase(found);
				have = true;
			}
		}
		if(have) {
			if(result.descriptionDone) {
				info.description = std::move(result.description);
				info.descriptionDone = true;
			}
			if(result.coverDone) {
				if(!result.rgba.empty()) {
					info.texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, result.width, result.height);
					if(info.texture) {
						SDL_UpdateTexture(info.texture, nullptr, result.rgba.data(), result.width * 4);
						SDL_SetTextureScaleMode(info.texture, SDL_SCALEMODE_LINEAR);
						info.width = result.width;
						info.height = result.height;
					}
				}
				info.coverDone = true;
			}
		}
	}
	return info;
}

void Covers::Worker()
{
	uwp::InitThreadForWinRT();
	while(true) {
		GameEntry game;
		{
			std::unique_lock lock(_lock);
			_wake.wait(lock, [this] { return _stop || !_queue.empty(); });
			if(_stop) {
				return;
			}
			game = _queue.front();
			_queue.pop_front();
		}
		Fetch(game);
	}
}

// The names of every box picture Libretro has for a system, from one GitHub request (kept for a week in the
// Covers folder). Matching against this list finds the picture even when the file name is spelled differently.
const std::vector<std::string>& Covers::Index(Windows::Web::Http::HttpClient& client, const std::string& system)
{
	auto found = _indexes.find(system);
	if(found != _indexes.end()) {
		return found->second;
	}
	std::vector<std::string>& names = _indexes[system];
	const SystemInfo info = InfoFor(system);
	const fs::path file = _folder / FromU8(std::string(info.repo) + ".list");
	std::error_code ec;
	bool fresh = fs::exists(file, ec) &&
		fs::last_write_time(file, ec) > fs::file_time_type::clock::now() - std::chrono::hours(24 * 7);
	if(fresh) {
		std::ifstream in(file);
		for(std::string line; std::getline(in, line);) {
			if(!line.empty()) {
				names.push_back(line);
			}
		}
		if(!names.empty()) {
			return names;
		}
	}
	int status = 0;
	auto body = HttpGet(client, std::string("https://api.github.com/repos/libretro-thumbnails/") + info.repo +
		"/git/trees/master?recursive=1", &status);
	if(!body) {
		uwp::Log("Box art list for " + system + " not available (" + std::to_string(status) + ")");
		_indexes.erase(system); // try again next time
		static const std::vector<std::string> empty;
		return empty;
	}
	const std::string prefix = "\"path\":\"Named_Boxarts/";
	for(size_t at = body->find(prefix); at != std::string::npos; at = body->find(prefix, at + 1)) {
		size_t start = at + prefix.size();
		size_t end = body->find(".png\"", start);
		if(end == std::string::npos) {
			break;
		}
		names.push_back(ReadJsonString("\"" + body->substr(start, end - start) + "\"", 0));
	}
	std::ofstream out(file, std::ios::trunc);
	for(const auto& name : names) {
		out << name << "\n";
	}
	uwp::Log("Box art list for " + system + ": " + std::to_string(names.size()) + " pictures");
	return names;
}

void Covers::Fetch(const GameEntry& game)
{
	const std::string base = SafeFileName(game.title);
	const fs::path coverFile = _folder / FromU8(base + ".png");
	const fs::path noCoverFile = _folder / FromU8(base + ".noart");
	const fs::path textFile = _folder / FromU8(base + ".txt");
	const std::string clean = CleanTitle(game.title);
	const SystemInfo info = InfoFor(game.system);

	Result result;
	std::optional<Windows::Web::Http::HttpClient> client;
	auto http = [&]() -> Windows::Web::Http::HttpClient& {
		if(!client) {
			client.emplace(MakeClient());
		}
		return *client;
	};

	// Description (an empty file from an earlier failed try counts as missing)
	std::optional<WikiResult> wiki;
	auto text = ReadFile(textFile);
	if(text && !text->empty()) {
		result.description = *text;
	} else if((wiki = WikipediaLookup(http(), clean, info.wikipedia))) {
		result.description = wiki->text;
		if(!wiki->text.empty()) {
			WriteFile(textFile, wiki->text);
		}
	}
	result.descriptionDone = true;
	{
		std::lock_guard lock(_lock);
		_results[game.title] = result;
	}

	std::optional<std::string> image = ReadFile(coverFile);
	std::error_code ec;
	if(!image && !fs::exists(noCoverFile, ec)) {
		// 1. Libretro's box art: the closest name in its list, best region first
		std::vector<std::string> candidates;
		const std::string key = MatchKey(game.title);
		std::vector<std::pair<int, std::string>> matches;
		for(const std::string& name : Index(http(), game.system)) {
			if(MatchKey(name) == key) {
				matches.emplace_back(RegionRank(name), name);
			}
		}
		std::sort(matches.begin(), matches.end());
		for(const auto& [rank, name] : matches) {
			candidates.push_back(name);
		}
		if(candidates.empty()) {
			// No list (offline GitHub) or no match in it: guess the usual spellings
			candidates.push_back(ToLibretroName(game.title));
			for(const char* region : {"USA", "USA, Europe", "World", "Europe", "Japan"}) {
				candidates.push_back(ToLibretroName(clean) + " (" + region + ")");
			}
		}
		bool reached = true;
		for(size_t i = 0; i < candidates.size() && i < 6 && !image; i++) {
			const std::string encoded = UrlEncode(candidates[i]);
			int status = 0;
			image = HttpGet(http(), std::string("https://thumbnails.libretro.com/") + UrlEncode(info.thumbnails) +
				"/Named_Boxarts/" + encoded + ".png", &status);
			if(!image) {
				// The same picture straight from GitHub, in case the thumbnail site is down
				int githubStatus = 0;
				image = HttpGet(http(), std::string("https://raw.githubusercontent.com/libretro-thumbnails/") + info.repo +
					"/master/Named_Boxarts/" + encoded + ".png", &githubStatus);
				if(!image && status != 404 && githubStatus != 404) {
					reached = false;
					uwp::Log("Box art sites answered " + std::to_string(status) + " and " + std::to_string(githubStatus) +
						" for " + candidates[i]);
				}
			}
			if(image) {
				uwp::Log("Box art found: " + candidates[i]);
			}
		}
		// 2. The picture at the top of the game's Wikipedia article
		if(!image) {
			if(!wiki) {
				wiki = WikipediaLookup(http(), clean, info.wikipedia);
			}
			if(wiki && !wiki->imageUrl.empty()) {
				image = HttpGet(http(), wiki->imageUrl);
				if(image) {
					uwp::Log("Box art from Wikipedia for " + game.title);
				}
			}
		}
		if(image) {
			WriteFile(coverFile, *image);
		} else if(reached) {
			WriteFile(noCoverFile, ""); // only when the sites really said they have none
			uwp::Log("No box art for " + game.title);
		}
	}
	if(image) {
		DecodeImage(*image, result.rgba, result.width, result.height, kMaxEdge);
	}
	result.coverDone = true;
	{
		std::lock_guard lock(_lock);
		_results[game.title] = std::move(result);
	}
}
