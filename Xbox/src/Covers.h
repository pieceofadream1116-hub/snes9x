#pragma once
// Box art and a short description for the highlighted game: fetched on demand, with no account or key, from the
// Libretro thumbnails library (box art, matched against its full list of names), or the game's Wikipedia article
// (description, and its picture when Libretro has none). Kept in <profile>\Covers so each game is only downloaded
// once. Downloads run on a thread of their own.

#include "Library.h"

#include <SDL3/SDL.h>

#include <winrt/Windows.Web.Http.h>

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// Decodes a PNG, JPEG or BMP file's bytes to RGBA pixels, made smaller if an edge is longer than maxEdge.
/// Blocks: call it on a worker thread (multi-threaded apartment), never the UI thread.
bool DecodeImage(const std::string& bytes, std::vector<uint8_t>& rgba, int& width, int& height, uint32_t maxEdge);

class Covers
{
public:
	struct Info
	{
		SDL_Texture* texture = nullptr;
		int width = 0;
		int height = 0;
		std::string description;
		bool coverDone = false;        // finished looking (texture may still be null: no box art found)
		bool descriptionDone = false;
	};

	explicit Covers(std::filesystem::path folder);
	~Covers();

	/// UI thread: the box art and description for a game, starting the download the first time it is asked for.
	const Info& Get(SDL_Renderer* renderer, const GameEntry& game);

private:
	struct Result
	{
		std::vector<uint8_t> rgba;
		int width = 0;
		int height = 0;
		std::string description;
		bool coverDone = false;
		bool descriptionDone = false;
	};

	void Worker();
	void Fetch(const GameEntry& game);
	const std::vector<std::string>& Index(winrt::Windows::Web::Http::HttpClient& client, const std::string& system);

	std::filesystem::path _folder;
	std::unordered_map<std::string, Info> _info;      // UI thread only, by game title

	std::mutex _lock;
	std::condition_variable _wake;
	std::deque<GameEntry> _queue;
	std::unordered_map<std::string, Result> _results; // filled by the worker, picked up by Get()
	std::unordered_map<std::string, std::vector<std::string>> _indexes; // worker thread only: box art names per system
	bool _stop = false;
	std::thread _thread;
};
