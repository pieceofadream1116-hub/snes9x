#pragma once
// Known cheat codes for a game, from Libretro's free cheat database (github.com/libretro/libretro-database, the
// cht folder). The list of cheat files for each system is fetched once a week; the game's own file is fetched the
// first time its cheats page is opened, then kept in <profile>\Cheats\Known. Runs on a thread of its own.

#include "Library.h"

#include <atomic>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class KnownCheats
{
public:
	explicit KnownCheats(std::filesystem::path folder);
	~KnownCheats();

	/// Starts looking up a game's cheats (UI thread). Does nothing if that game is already loaded or loading.
	void Request(const GameEntry& game);

	/// True once the lookup for the requested game has finished.
	bool Done() const { return _done; }
	/// The codes found (UI thread, after Done()).
	std::vector<CheatEntry> Codes();
	/// What happened, for the cheats page ("Found 24 codes", "No codes known for this game"...).
	std::string Status();

private:
	void Lookup(GameEntry game);

	std::filesystem::path _folder;
	std::string _requested;
	std::thread _thread;
	std::atomic<bool> _done = false;
	std::mutex _lock;
	std::vector<CheatEntry> _codes;
	std::string _status;
};
