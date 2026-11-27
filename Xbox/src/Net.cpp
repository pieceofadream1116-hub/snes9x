#include "Net.h"

#include "UwpPlatform.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/Windows.Web.Http.Headers.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;
using namespace winrt;
using namespace winrt::Windows::Storage::Streams;

namespace net {

namespace {
constexpr uint32_t kMaxBytes = 8 * 1024 * 1024;
}

std::string U8(const fs::path& path)
{
	auto s = path.u8string();
	return std::string(s.begin(), s.end());
}

fs::path FromU8(const std::string& s)
{
	return fs::path(std::u8string(s.begin(), s.end()));
}

std::string UrlEncode(const std::string& text)
{
	static const char* hex = "0123456789ABCDEF";
	std::string out;
	for(unsigned char ch : text) {
		if(std::isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
			out.push_back((char)ch);
		} else {
			out.push_back('%');
			out.push_back(hex[ch >> 4]);
			out.push_back(hex[ch & 15]);
		}
	}
	return out;
}

// Libretro thumbnail names replace these characters with '_'
std::string ToLibretroName(std::string name)
{
	for(char& ch : name) {
		if(std::string_view("&*/:`<>?\\|\"").find(ch) != std::string_view::npos) {
			ch = '_';
		}
	}
	return name;
}

// "After Burner (USA) (Unl)" -> "After Burner"
std::string CleanTitle(const std::string& title)
{
	std::string out;
	int depth = 0;
	for(char ch : title) {
		if(ch == '(' || ch == '[') {
			depth++;
		} else if(ch == ')' || ch == ']') {
			depth = std::max(0, depth - 1);
		} else if(depth == 0) {
			out.push_back(ch);
		}
	}
	while(!out.empty() && out.back() == ' ') {
		out.pop_back();
	}
	// No-Intro puts "The" at the end: "Legend of Zelda, The" -> "The Legend of Zelda"
	if(out.size() > 5 && out.compare(out.size() - 5, 5, ", The") == 0) {
		out = "The " + out.substr(0, out.size() - 5);
	}
	return out;
}

// status: the HTTP status code, or 0 when the request never got an answer (no network, blocked...)
std::optional<std::string> HttpGet(Windows::Web::Http::HttpClient& client, const std::string& url, int* status)
{
	if(status) {
		*status = 0;
	}
	try {
		auto response = client.GetAsync(Windows::Foundation::Uri(to_hstring(url))).get();
		if(status) {
			*status = (int)response.StatusCode();
		}
		if(!response.IsSuccessStatusCode()) {
			return std::nullopt;
		}
		auto buffer = response.Content().ReadAsBufferAsync().get();
		uint32_t length = buffer.Length();
		if(length == 0 || length > kMaxBytes) {
			return std::nullopt;
		}
		std::string bytes(length, '\0');
		DataReader::FromBuffer(buffer).ReadBytes(array_view<uint8_t>((uint8_t*)bytes.data(), (uint8_t*)bytes.data() + length));
		return bytes;
	} catch(const hresult_error& e) {
		static std::atomic<int> logged = 0;
		if(logged++ < 3) {
			uwp::Log("[!] Download failed: " + to_string(e.message()) + " (" + url.substr(0, 60) + ")");
		}
		return std::nullopt;
	} catch(...) {
		return std::nullopt;
	}
}

// Reads the JSON string that starts at text[pos] == '"' (handles \n, \", \uXXXX...)
std::string ReadJsonString(const std::string& text, size_t pos)
{
	std::string out;
	for(pos++; pos < text.size() && text[pos] != '"'; pos++) {
		char ch = text[pos];
		if(ch != '\\' || pos + 1 >= text.size()) {
			out.push_back(ch);
			continue;
		}
		char esc = text[++pos];
		switch(esc) {
			case 'n': case 'r': case 't': out.push_back(' '); break;
			case 'u': {
				if(pos + 4 >= text.size()) {
					return out;
				}
				unsigned code = (unsigned)std::stoul(text.substr(pos + 1, 4), nullptr, 16);
				pos += 4;
				if(code >= 0xD800 && code <= 0xDBFF && pos + 6 < text.size() && text[pos + 1] == '\\' && text[pos + 2] == 'u') {
					unsigned low = (unsigned)std::stoul(text.substr(pos + 3, 4), nullptr, 16);
					code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
					pos += 6;
				}
				if(code < 0x80) {
					out.push_back((char)code);
				} else if(code < 0x800) {
					out.push_back((char)(0xC0 | (code >> 6)));
					out.push_back((char)(0x80 | (code & 0x3F)));
				} else if(code < 0x10000) {
					out.push_back((char)(0xE0 | (code >> 12)));
					out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
					out.push_back((char)(0x80 | (code & 0x3F)));
				} else {
					out.push_back((char)(0xF0 | (code >> 18)));
					out.push_back((char)(0x80 | ((code >> 12) & 0x3F)));
					out.push_back((char)(0x80 | ((code >> 6) & 0x3F)));
					out.push_back((char)(0x80 | (code & 0x3F)));
				}
				break;
			}
			default: out.push_back(esc); break;
		}
	}
	return out;
}


std::optional<std::string> ReadFile(const fs::path& file)
{
	std::ifstream in(file, std::ios::binary);
	if(!in) {
		return std::nullopt;
	}
	std::stringstream ss;
	ss << in.rdbuf();
	return ss.str();
}

void WriteFile(const fs::path& file, const std::string& data)
{
	std::ofstream out(file, std::ios::binary | std::ios::trunc);
	out.write(data.data(), (std::streamsize)data.size());
}

std::string SafeFileName(std::string name)
{
	for(char& ch : name) {
		if(std::string_view("<>:\"/\\|?*").find(ch) != std::string_view::npos) {
			ch = '_';
		}
	}
	return name;
}


SystemInfo InfoFor(const std::string& /*system*/)
{
	return {"Nintendo - Super Nintendo Entertainment System", "Nintendo_-_Super_Nintendo_Entertainment_System", "Super NES video game"};
}

// Letters and digits only, lower case: "Sonic the Hedgehog 2 (World)" and "SONIC THE HEDGEHOG 2" both give "sonicthehedgehog2"
std::string MatchKey(const std::string& title)
{
	std::string clean = CleanTitle(title);
	std::string key;
	for(unsigned char ch : clean) {
		if(std::isalnum(ch)) {
			key.push_back((char)std::tolower(ch));
		}
	}
	return key;
}

// Lower is better: which of several pictures of the same game to use
int RegionRank(const std::string& name)
{
	auto has = [&](const char* text) { return name.find(text) != std::string::npos; };
	int rank = 50;
	if(has("(USA)")) rank = 0;
	else if(has("(USA, Europe)") || has("(USA, Europe, Brazil)")) rank = 1;
	else if(has("(World)")) rank = 2;
	else if(has("USA")) rank = 3;
	else if(has("(Europe)")) rank = 10;
	else if(has("(Japan)")) rank = 20;
	if(has("Beta") || has("Proto") || has("Sample") || has("Demo") || has("Pirate") || has("Unl")) rank += 100;
	if(has("(Rev")) rank += 1;
	return rank;
}


std::string JsonValue(const std::string& body, const char* key)
{
	std::string needle = std::string("\"") + key + "\":";
	size_t at = body.find(needle);
	if(at == std::string::npos) {
		return {};
	}
	size_t quote = body.find_first_not_of(' ', at + needle.size());
	return quote == std::string::npos || body[quote] != '"' ? std::string{} : ReadJsonString(body, quote);
}


Windows::Web::Http::HttpClient MakeClient()
{
	Windows::Web::Http::HttpClient client;
	// Wikipedia and GitHub refuse requests that do not say which app is asking
	client.DefaultRequestHeaders().UserAgent().TryParseAdd(L"SNESXbox/0.1 (Xbox UWP Super Nintendo emulator; github.com/pieceofadream1116-hub)");
	return client;
}

} // namespace net
