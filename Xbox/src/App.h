#pragma once
// The SNES app: the game list, the pause menu and the main loop. Snes9x runs the games on a thread of
// its own (Core); this class owns the window and draws the core's finished frames plus the big-text menus.

#include "Library.h"
#include "Settings.h"

#include <SDL3/SDL.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class Core;
class XboxAudio;
class XboxInput;
class Covers;
class KnownCheats;
struct PadState;

class App
{
public:
	App();
	~App();

	int Run();

private:
	enum class Screen { Library, Playing };
	enum class MenuPage { Main, Picture, Cheats, AddCode, Sound, Controller };

	bool Init();
	void Shutdown();
	void SetUpProfile();
	void ReloadFont();
	void ApplyCoreSettings();
	PadState ReadPad(int player);

	void StartGame(const GameEntry& game);
	void LoadOnWorker(GameEntry game);
	void FinishLoad();
	void SetStep(std::string step);
	void Watchdog();
	void DrawInfo();
	void DrawLastSession();
	void QuitGame();
	void OpenMenu();
	void CloseMenu();
	void ApplyCheats();
	void Toast(std::string text, int seconds = 3);

	void HandleHotkeys();
	void UploadFrame();
	void DrawGame();
	void DrawScanlines(const SDL_FRect& dst);
	void DrawLibrary();
	void DrawBackground();
	void LoadBackground();
	void FreeBackground();
	void UseBackgroundPicture(const std::filesystem::path& picked);
	void DrawMenu();
	void DrawMenuMain();
	void DrawPictureOptions();
	void DrawSoundOptions();
	void DrawCheats();
	void DrawController();
	void DrawAddCode();
	void DrawToast();
	bool BackPressed() const;

	SDL_Window* _window = nullptr;
	SDL_Renderer* _sdlRenderer = nullptr;
	SDL_Texture* _gameTexture = nullptr;
	uint32_t _textureWidth = 0;
	uint32_t _textureHeight = 0;
	std::vector<uint32_t> _frameBuffer;
	std::vector<uint32_t> _scaled, _scaleScratch; // Max HD
	uint32_t _gameTexW = 0, _gameTexH = 0;       // size of _gameTexture
	bool _hasFrame = false;
	bool _running = true;
	bool _fontDirty = true;
	int _fontPixels = 0;

	std::unique_ptr<XboxAudio> _audio;
	std::unique_ptr<Core> _core;
	std::unique_ptr<KnownCheats> _knownCheats;
	std::unique_ptr<XboxInput> _input;
	std::unique_ptr<Covers> _covers;
	size_t _focusedGame = 0;

	std::filesystem::path _profile;
	std::filesystem::path _resFolder;
	AppSettings _settings;
	std::vector<GameEntry> _games;

	Screen _screen = Screen::Library;
	bool _menuOpen = false;
	MenuPage _page = MenuPage::Main;
	bool _focusNext = true;
	int _saveSlot = 1;
	int _capturing = -1;          // Super Nintendo button waiting for an Xbox button (Controller page)
	bool _captureReleased = false;
	bool _ignoreUntilReleased = false;
	uint64_t _lastPadButtons = 0;

	GameEntry _currentGame;
	std::filesystem::path _cheatFile;
	GameCheats _cheats;
	std::string _newCode;

	std::string _toast;
	std::chrono::steady_clock::time_point _toastUntil;

	// Loading runs on its own thread so the TV picture never freezes; the info panel shows each step
	std::thread _loadThread;
	std::atomic<bool> _loading = false;
	std::atomic<bool> _loadDone = false;
	bool _loadOk = false;
	std::string _loadError;
	GameEntry _loadGame;
	std::mutex _stepLock;
	std::string _step;
	std::chrono::steady_clock::time_point _stepTime;
	std::chrono::steady_clock::time_point _loadStart;
	bool _warnedSlowLoad = false;
	std::chrono::steady_clock::time_point _playStart;
	uint64_t _framesAtStart = 0;
	bool _warnedNoPicture = false;
	bool _showInfo = false;   // the info box still shows by itself while a game loads or has no picture
	std::chrono::steady_clock::time_point _lastWatch;
	std::atomic<uint64_t> _turboTick = 0;
	std::vector<std::string> _lastSession;   // the previous run's log, shown when it ended with a problem

	std::mutex _pickLock;
	std::string _pickedFolder;
	bool _pickDone = false;
	std::string _pickedPicture;
	bool _pictureDone = false;

	// The player's own background picture: decoded on _bgThread, made into _bgTexture on the UI thread
	std::thread _bgThread;
	std::mutex _bgLock;
	std::vector<uint8_t> _bgPixels;
	int _bgPixW = 0, _bgPixH = 0;
	bool _bgReady = false;
	SDL_Texture* _bgTexture = nullptr;
	int _bgTexW = 0, _bgTexH = 0;
};
