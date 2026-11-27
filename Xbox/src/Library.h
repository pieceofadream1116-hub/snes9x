#pragma once
// The game list (files found in the games folders) and the cheats switched on for each game.

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct GameEntry
{
	std::filesystem::path path;
	std::string title;   // file name without extension
	std::string system;  // "Super Nintendo"
};

/// Lists the games in the folders (and their subfolders), sorted by title.
std::vector<GameEntry> ScanGames(const std::vector<std::filesystem::path>& folders);

struct CheatEntry
{
	std::string desc;
	std::string code;      // one or more codes separated by '+' (Game Genie DD62-6DAD or Pro Action Replay 7E0DBF63)
	bool custom = false;   // typed in by the player, not from the database
};

/// The cheats a player switched on (and the ones they typed in) for one game, saved in Cheats/<game>.txt.
struct GameCheats
{
	std::vector<CheatEntry> custom;
	std::vector<std::string> enabledCodes;

	void Load(const std::filesystem::path& file);
	void Save(const std::filesystem::path& file) const;
	bool IsEnabled(const std::string& code) const;
	void SetEnabled(const std::string& code, bool enabled);
};
