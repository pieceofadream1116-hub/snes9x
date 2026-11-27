#include "Library.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <unordered_map>

namespace {

std::string Lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

const char* SystemForExtension(const std::string& ext)
{
	static const std::set<std::string> extensions = {".sfc", ".smc", ".swc", ".fig", ".bs", ".st"};
	return extensions.count(ext) ? "Super Nintendo" : nullptr;
}

} // namespace

std::vector<GameEntry> ScanGames(const std::vector<std::filesystem::path>& folders)
{
	std::vector<GameEntry> games;
	std::set<std::filesystem::path> seen;
	for(const auto& folder : folders) {
		std::error_code error;
		if(folder.empty() || !std::filesystem::is_directory(folder, error)) {
			continue;
		}
		auto options = std::filesystem::directory_options::skip_permission_denied;
		for(auto it = std::filesystem::recursive_directory_iterator(folder, options, error);
			it != std::filesystem::recursive_directory_iterator(); it.increment(error)) {
			if(error) {
				break;
			}
			if(!it->is_regular_file(error)) {
				continue;
			}
			std::string ext = Lower(it->path().extension().string());
			const char* system = SystemForExtension(ext);
			if(!system) {
				continue;
			}
			auto canonical = std::filesystem::weakly_canonical(it->path(), error);
			if(!seen.insert(error ? it->path() : canonical).second) {
				continue;
			}
			games.push_back({it->path(), it->path().stem().string(), system});
		}
	}
	std::sort(games.begin(), games.end(), [](const GameEntry& a, const GameEntry& b) {
		return Lower(a.title) < Lower(b.title);
	});
	return games;
}

void GameCheats::Load(const std::filesystem::path& file)
{
	custom.clear();
	enabledCodes.clear();
	std::ifstream in(file);
	std::string line;
	while(std::getline(in, line)) {
		if(!line.empty() && line.back() == '\r') {
			line.pop_back();
		}
		if(line.rfind("on|", 0) == 0) {
			enabledCodes.push_back(line.substr(3));
		} else if(line.rfind("custom|", 0) == 0) {
			std::string rest = line.substr(7);
			size_t bar = rest.find('|');
			CheatEntry entry;
			entry.custom = true;
			entry.code = rest.substr(0, bar);
			entry.desc = bar == std::string::npos ? entry.code : rest.substr(bar + 1);
			custom.push_back(entry);
		}
	}
}

void GameCheats::Save(const std::filesystem::path& file) const
{
	std::error_code error;
	std::filesystem::create_directories(file.parent_path(), error);
	std::ofstream out(file, std::ios::trunc);
	for(const auto& entry : custom) {
		out << "custom|" << entry.code << "|" << entry.desc << "\n";
	}
	for(const auto& code : enabledCodes) {
		out << "on|" << code << "\n";
	}
}

bool GameCheats::IsEnabled(const std::string& code) const
{
	return std::find(enabledCodes.begin(), enabledCodes.end(), code) != enabledCodes.end();
}

void GameCheats::SetEnabled(const std::string& code, bool enabled)
{
	auto it = std::find(enabledCodes.begin(), enabledCodes.end(), code);
	if(enabled && it == enabledCodes.end()) {
		enabledCodes.push_back(code);
	} else if(!enabled && it != enabledCodes.end()) {
		enabledCodes.erase(it);
	}
}
