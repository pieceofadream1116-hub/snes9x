#pragma once
// Small helpers for the downloads (box art, descriptions, cheat lists): HTTP through WinRT, a little JSON reading,
// and the name matching used to find a game in the Libretro libraries.

#include <winrt/Windows.Web.Http.h>

#include <filesystem>
#include <optional>
#include <string>

namespace net {

std::string U8(const std::filesystem::path& path);
std::filesystem::path FromU8(const std::string& s);
std::string UrlEncode(const std::string& text);

/// Libretro file names replace &*/:`<>?\|" with '_'
std::string ToLibretroName(std::string name);
/// "Sonic the Hedgehog (USA, Europe)" -> "Sonic the Hedgehog"; "Lion King, The" -> "The Lion King"
std::string CleanTitle(const std::string& title);
/// Letters and digits of the clean title, lower case, for comparing names spelled differently.
std::string MatchKey(const std::string& title);
/// Lower is better: which of several versions of the same game to use (USA first, betas last).
int RegionRank(const std::string& name);

/// An HttpClient that says which app is asking (Wikipedia and GitHub require it).
winrt::Windows::Web::Http::HttpClient MakeClient();
/// The body, or nullopt. status: the HTTP status code, or 0 when there was no answer at all.
std::optional<std::string> HttpGet(winrt::Windows::Web::Http::HttpClient& client, const std::string& url, int* status = nullptr);

/// Reads the JSON string starting at text[pos] == '"'.
std::string ReadJsonString(const std::string& text, size_t pos);
/// The string value of "key" in a small JSON answer (first match), or empty.
std::string JsonValue(const std::string& body, const char* key);

std::optional<std::string> ReadFile(const std::filesystem::path& file);
void WriteFile(const std::filesystem::path& file, const std::string& data);
std::string SafeFileName(std::string name);

struct SystemInfo
{
	const char* thumbnails; // folder name on thumbnails.libretro.com and in libretro-database
	const char* repo;       // github.com/libretro-thumbnails/<repo>
	const char* wikipedia;  // added to the title when searching Wikipedia
};
SystemInfo InfoFor(const std::string& system);

} // namespace net
