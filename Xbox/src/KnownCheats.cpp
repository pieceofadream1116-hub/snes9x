#include "KnownCheats.h"

#include "Net.h"
#include "UwpPlatform.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
using namespace net;

namespace {

// Finds the "sha" of the tree entry called `name` in a GitHub trees answer
std::string TreeSha(const std::string& body, const std::string& name)
{
	const std::string needle = "\"path\":\"" + name + "\"";
	size_t at = body.find(needle);
	if(at == std::string::npos) {
		return {};
	}
	size_t object = body.rfind('{', at);
	size_t end = body.find('}', at);
	if(object == std::string::npos || end == std::string::npos) {
		return {};
	}
	return JsonValue(body.substr(object, end - object), "sha");
}

// cheat0_desc = "Infinite lives"  ->  key "cheat0_desc", value "Infinite lives"
std::vector<CheatEntry> ParseCht(const std::string& text)
{
	std::vector<std::string> desc(512), code(512);
	std::istringstream in(text);
	for(std::string line; std::getline(in, line);) {
		size_t eq = line.find('=');
		if(eq == std::string::npos || line.rfind("cheat", 0) != 0) {
			continue;
		}
		std::string key = line.substr(0, eq);
		key.erase(key.find_last_not_of(" \t") + 1);
		std::string value = line.substr(eq + 1);
		size_t first = value.find('"');
		size_t last = value.rfind('"');
		if(first == std::string::npos || last <= first) {
			continue;
		}
		value = value.substr(first + 1, last - first - 1);
		size_t underscore = key.find('_');
		if(underscore == std::string::npos) {
			continue;
		}
		int index = 0;
		try {
			index = std::stoi(key.substr(5, underscore - 5));
		} catch(...) {
			continue;
		}
		if(index < 0 || index >= 512) {
			continue;
		}
		std::string field = key.substr(underscore + 1);
		if(field == "desc") {
			desc[index] = value;
		} else if(field == "code") {
			code[index] = value;
		}
	}
	std::vector<CheatEntry> codes;
	for(size_t i = 0; i < code.size(); i++) {
		if(!code[i].empty()) {
			codes.push_back({desc[i].empty() ? "Code " + std::to_string(i + 1) : desc[i], code[i], false});
		}
	}
	return codes;
}

} // namespace

KnownCheats::KnownCheats(fs::path folder) : _folder(std::move(folder))
{
	std::error_code ec;
	fs::create_directories(_folder, ec);
}

KnownCheats::~KnownCheats()
{
	if(_thread.joinable()) {
		_thread.detach(); // a download may be in progress; the app is closing anyway
	}
}

void KnownCheats::Request(const GameEntry& game)
{
	std::string key = U8(game.path);
	{
		std::lock_guard lock(_lock);
		if(key == _requested) {
			return;
		}
	}
	if(_thread.joinable()) {
		if(!_done) {
			_thread.detach();
		} else {
			_thread.join();
		}
	}
	_done = false;
	{
		std::lock_guard lock(_lock);
		_requested = key;
		_codes.clear();
		_status = "Looking for known codes...";
	}
	_thread = std::thread([this, game] { Lookup(game); });
}

std::vector<CheatEntry> KnownCheats::Codes()
{
	std::lock_guard lock(_lock);
	return _codes;
}

std::string KnownCheats::Status()
{
	std::lock_guard lock(_lock);
	return _status;
}

void KnownCheats::Lookup(GameEntry game)
{
	uwp::InitThreadForWinRT();
	const std::string me = U8(game.path);
	// retry: forget this request, so opening the page again tries again
	auto finish = [&](std::vector<CheatEntry> codes, std::string status, bool retry = false) {
		std::lock_guard lock(_lock);
		if(_requested == me) {
			_codes = std::move(codes);
			_status = std::move(status);
			_done = true;
			if(retry) {
				_requested.clear();
			}
		}
	};
	try {
		const SystemInfo info = InfoFor(game.system);
		const fs::path saved = _folder / FromU8(SafeFileName(game.title) + ".cht");
		if(auto text = ReadFile(saved); text && !text->empty()) {
			auto codes = ParseCht(*text);
			finish(codes, "Known codes: " + std::to_string(codes.size()));
			return;
		}

		auto client = MakeClient();
		// The names of the system's cheat files, kept for a week
		const fs::path listFile = _folder / FromU8(std::string(info.thumbnails) + ".list");
		std::vector<std::string> names;
		std::error_code ec;
		if(fs::exists(listFile, ec) && fs::last_write_time(listFile, ec) > fs::file_time_type::clock::now() - std::chrono::hours(24 * 7)) {
			std::ifstream in(listFile);
			for(std::string line; std::getline(in, line);) {
				if(!line.empty()) {
					names.push_back(line);
				}
			}
		}
		if(names.empty()) {
			// libretro-database is too big to list at once: walk master -> cht -> the system's folder
			const std::string api = "https://api.github.com/repos/libretro/libretro-database/git/trees/";
			auto root = HttpGet(client, api + "master");
			std::string chtSha = root ? TreeSha(*root, "cht") : std::string{};
			auto cht = chtSha.empty() ? std::nullopt : HttpGet(client, api + chtSha);
			std::string systemSha = cht ? TreeSha(*cht, info.thumbnails) : std::string{};
			auto folder = systemSha.empty() ? std::nullopt : HttpGet(client, api + systemSha);
			if(!folder) {
				finish({}, "Could not reach the cheat library. Check the Xbox is online, then open this page again.", true);
				uwp::Log("Cheat list for " + game.system + " not available");
				return;
			}
			const std::string prefix = "\"path\":\"";
			for(size_t at = folder->find(prefix); at != std::string::npos; at = folder->find(prefix, at + 1)) {
				std::string name = ReadJsonString(*folder, at + prefix.size() - 1);
				if(name.size() > 4 && name.compare(name.size() - 4, 4, ".cht") == 0) {
					names.push_back(name.substr(0, name.size() - 4));
				}
			}
			std::ofstream out(listFile, std::ios::trunc);
			for(const auto& name : names) {
				out << name << "\n";
			}
			uwp::Log("Cheat list for " + game.system + ": " + std::to_string(names.size()) + " games");
		}

		const std::string key = MatchKey(game.title);
		std::vector<std::pair<int, std::string>> matches;
		for(const auto& name : names) {
			if(MatchKey(name) == key) {
				matches.emplace_back(RegionRank(name), name);
			}
		}
		if(matches.empty()) {
			finish({}, "No known codes for this game. You can type one in.");
			return;
		}
		std::sort(matches.begin(), matches.end());
		const std::string& best = matches.front().second;
		auto text = HttpGet(client, "https://raw.githubusercontent.com/libretro/libretro-database/master/cht/" +
			UrlEncode(info.thumbnails) + "/" + UrlEncode(best) + ".cht");
		if(!text) {
			finish({}, "Could not download the codes. Try again later.", true);
			return;
		}
		WriteFile(saved, *text);
		auto codes = ParseCht(*text);
		uwp::Log("Known cheats for " + game.title + ": " + std::to_string(codes.size()) + " (" + best + ")");
		finish(codes, "Known codes: " + std::to_string(codes.size()));
	} catch(...) {
		finish({}, "Could not look up codes.", true);
	}
}
