#include "App.h"

#include "Core.h"
#include "Covers.h"
#include "EdgeScaler.h"
#include "KnownCheats.h"
#include "UwpPlatform.h"
#include "XboxAudio.h"
#include "XboxInput.h"

#include "libretro.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cctype>
#include <cstring>
#include <iterator>
#include <fstream>
#include <system_error>

namespace fs = std::filesystem;

namespace {

// Set by the crash catcher (any thread), shown in a red box by the UI thread
std::mutex g_crashLock;
std::string g_crashText;
std::atomic<bool> g_crashed = false;

// The lookout: the screen loop stamps where it is; a separate thread notices when it stops moving
std::atomic<const char*> g_uiMarker = "starting up";
std::atomic<uint64_t> g_uiBeat = 0;
std::atomic<const char*> g_workerMarker = "idle";
std::atomic<const char*> g_coreMarker = "no game"; // copied from Core::Marker() by the screen loop

void Mark(const char* where)
{
	g_uiMarker = where;
}

const char* XboxButtonName(int button)
{
	switch(button) {
		case pad::A: return "A";
		case pad::B: return "B";
		case pad::X: return "X";
		case pad::Y: return "Y";
		case pad::Back: return "View";
		case pad::Start: return "Menu";
		case pad::LeftStick: return "Left stick click";
		case pad::RightStick: return "Right stick click";
		case pad::LeftShoulder: return "LB";
		case pad::RightShoulder: return "RB";
		case pad::LeftTrigger: return "LT";
		case pad::RightTrigger: return "RT";
		default: return "?";
	}
}

// Buttons a Super Nintendo button can be moved to (the right stick click opens the menu, the D-pad moves)
constexpr pad::Button kAssignable[] = {pad::A, pad::B, pad::X, pad::Y, pad::LeftShoulder, pad::RightShoulder,
	pad::LeftTrigger, pad::RightTrigger, pad::Back, pad::Start, pad::LeftStick};

// The keys for typing codes: Game Genie (DD62-6DAD) and Pro Action Replay (7E0DBF63) are both hex
constexpr const char* kCodeKeys = "0123456789ABCDEF-:+";

bool IsHex(char ch)
{
	return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'F');
}

bool AllHex(const std::string& s)
{
	return !s.empty() && std::all_of(s.begin(), s.end(), IsHex);
}

// Game Genie XXXX-XXXX, Pro Action Replay XXXXXXXX or XXXXXX:XX; several joined with '+'
bool ValidCode(const std::string& code)
{
	size_t start = 0;
	while(true) {
		size_t end = code.find('+', start);
		std::string part = code.substr(start, end == std::string::npos ? std::string::npos : end - start);
		bool ok = (part.size() == 9 && part[4] == '-' && AllHex(part.substr(0, 4)) && AllHex(part.substr(5))) ||
			(part.size() == 8 && AllHex(part)) ||
			(part.size() == 9 && part[6] == ':' && AllHex(part.substr(0, 6)) && AllHex(part.substr(7)));
		if(!ok) {
			return false;
		}
		if(end == std::string::npos) {
			return true;
		}
		start = end + 1;
	}
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

// File names cannot hold these characters
std::string SafeFileName(std::string name)
{
	for(char& ch : name) {
		if(std::string("<>:\"/\\|?*").find(ch) != std::string::npos) {
			ch = '_';
		}
	}
	return name;
}

std::string Upper(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::toupper(c); });
	return s;
}

// A full-width button: easy to see and to land on with the D-pad
bool BigButton(const char* label)
{
	return ImGui::Button(label, ImVec2(-FLT_MIN, 0));
}

// "<  value  >" row for numbers, driven with the D-pad (left/right buttons)
bool Stepper(const char* label, int& value, int minValue, int maxValue, int step, const char* suffix = "")
{
	bool changed = false;
	ImGui::PushID(label);
	ImGui::TextUnformatted(label);
	ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.45f);
	if(ImGui::Button(" - ")) {
		value = std::max(minValue, value - step);
		changed = true;
	}
	ImGui::SameLine();
	ImGui::Text("%d%s", value, suffix);
	ImGui::SameLine();
	if(ImGui::Button(" + ")) {
		value = std::min(maxValue, value + step);
		changed = true;
	}
	ImGui::PopID();
	return changed;
}

} // namespace

App::App() = default;

App::~App()
{
	Shutdown();
}

void App::SetUpProfile()
{
	_profile = uwp::FindExternalProfilePath();
	if(_profile.empty()) {
		_profile = uwp::LocalStatePath() / uwp::kProfileFolderName;
	}
	std::error_code ec;
	fs::create_directories(_profile / "Games", ec);
	fs::create_directories(_profile / "Cheats", ec);

	// Keep the last run's log if it ended with a problem (crash, freeze, game that would not start) to show on the TV
	std::vector<std::string> previous = uwp::ReadLogFile(_profile);
	bool problem = std::any_of(previous.begin(), previous.end(), [](const std::string& line) { return line.find("[!]") != std::string::npos; });
	if(problem) {
		size_t start = previous.size() > 14 ? previous.size() - 14 : 0;
		_lastSession.assign(previous.begin() + start, previous.end());
	}
	uwp::OpenLog(_profile);
	uwp::Log("SNES " APP_VERSION " starting, profile: " + U8(_profile));

	const char* base = SDL_GetBasePath();
	_resFolder = base ? FromU8(base) / "res" : fs::path("res");

	_settings.Load(_profile / "snes.ini");
	fs::create_directories(_profile / "System", ec);
	fs::create_directories(_profile / "Saves", ec);
}

bool App::Init()
{
	uwp::InstallCrashLogging();
	std::thread([] {
		uint64_t lastBeat = g_uiBeat;
		auto lastChange = std::chrono::steady_clock::now();
		int reports = 0;
		while(true) {
			std::this_thread::sleep_for(std::chrono::milliseconds(250));
			uint64_t beat = g_uiBeat;
			auto now = std::chrono::steady_clock::now();
			if(beat != lastBeat) {
				lastBeat = beat;
				lastChange = now;
				reports = 0;
				continue;
			}
			double stuck = std::chrono::duration<double>(now - lastChange).count();
			if((reports == 0 && stuck > 3) || (reports == 1 && stuck > 15)) {
				reports++;
				uwp::LogUrgent(std::string("[!] The screen has been stuck for ") + std::to_string((int)stuck) +
					" seconds while " + g_uiMarker.load() + ". Game loader: " + g_workerMarker.load() +
					". Core: " + g_coreMarker.load());
			}
		}
	}).detach();
	SetUpProfile();

	// Keep the Xbox B button from sending the app to the background, and turn on the Xbox controller driver
	SDL_SetHint(SDL_HINT_WINRT_HANDLE_BACK_BUTTON, "1");
	SDL_SetHint(SDL_HINT_JOYSTICK_WGI, "1");

	if(!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
		uwp::Log(std::string("SDL_Init failed: ") + SDL_GetError());
		return false;
	}
	uwp::UseFullCoreWindowBounds();

	_window = SDL_CreateWindow("SNES", 1920, 1080, SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
	if(!_window) {
		uwp::Log(std::string("SDL_CreateWindow failed: ") + SDL_GetError());
		return false;
	}
	_sdlRenderer = SDL_CreateRenderer(_window, nullptr);
	if(!_sdlRenderer) {
		uwp::Log(std::string("SDL_CreateRenderer failed: ") + SDL_GetError());
		return false;
	}
	SDL_SetRenderVSync(_sdlRenderer, 1);
	uwp::Log(std::string("Renderer: ") + SDL_GetRendererName(_sdlRenderer));

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO& io = ImGui::GetIO();
	io.IniFilename = nullptr;
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NoMouseCursorChange;
	io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;

	ImGui::StyleColorsDark();
	ImGuiStyle& style = ImGui::GetStyle();
	style.WindowRounding = 0;
	style.FrameRounding = 10;
	style.FramePadding = ImVec2(24, 14);
	style.ItemSpacing = ImVec2(18, 16);
	style.WindowPadding = ImVec2(48, 36);
	style.ScrollbarSize = 28;
	style.Colors[ImGuiCol_WindowBg] = ImVec4(0.04f, 0.07f, 0.16f, 0.96f);
	style.Colors[ImGuiCol_Button] = ImVec4(0.12f, 0.20f, 0.42f, 1.0f);
	style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.80f, 0.16f, 0.18f, 1.0f);
	style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.95f, 0.70f, 0.15f, 1.0f);
	style.Colors[ImGuiCol_Header] = ImVec4(0.12f, 0.20f, 0.42f, 1.0f);
	style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.80f, 0.16f, 0.18f, 1.0f);
	style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.95f, 0.70f, 0.15f, 1.0f);
	style.Colors[ImGuiCol_FrameBg] = ImVec4(0.12f, 0.20f, 0.42f, 1.0f);
	style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.80f, 0.16f, 0.18f, 1.0f);
	style.Colors[ImGuiCol_CheckMark] = ImVec4(1.0f, 0.85f, 0.25f, 1.0f);
	style.Colors[ImGuiCol_NavCursor] = ImVec4(1.0f, 0.85f, 0.25f, 1.0f);

	ImGui_ImplSDL3_InitForSDLRenderer(_window, _sdlRenderer);
	ImGui_ImplSDLRenderer3_Init(_sdlRenderer);

	// Sound, controllers and the core
	_audio = std::make_unique<XboxAudio>();
	_audio->Open();
	_input = std::make_unique<XboxInput>();
	_core = std::make_unique<Core>(_audio.get(), [this](int player) { return ReadPad(player); });
	_core->SetFolders(_profile / "System", _profile / "Saves");
	ApplyCoreSettings();
	_knownCheats = std::make_unique<KnownCheats>(_profile / "Cheats" / "Known");
	uwp::Log("Snes9x is ready");

	// The profile's Games folder, your ES-DE folders, then the folder picked with "Choose games folder"
	std::vector<fs::path> folders{_profile / "Games"};
	for(const fs::path& folder : uwp::EsDeFolders({"snes", "sfc", "snesna", "superfamicom", "satellaview", "sufami"})) {
		folders.push_back(folder);
	}
	if(!_settings.gamesFolder.empty()) {
		folders.push_back(FromU8(_settings.gamesFolder));
	}
	_games = ScanGames(folders);
	uwp::Log("Games found: " + std::to_string(_games.size()));
	_covers = std::make_unique<Covers>(_profile / "Covers");
	LoadBackground();
	return true;
}

void App::ApplyCoreSettings()
{
	Core::Options options;
	options.tvLook = _settings.picture == PictureStyle::TvLook;
	options.noSpriteLimit = _settings.noFlicker;
	options.fastSuperFx = _settings.fastSuperFx;
	_core->SetOptions(options);
	_audio->SetVolume(_settings.volume);
}

// Runs on the emulation thread each time the core reads the controllers: the player's button choices, turbo, and
// the D-pad or left stick for directions
PadState App::ReadPad(int player)
{
	// libretro pad button for each Super Nintendo button, in kPadButtonNames order
	static constexpr int kRetroId[AppSettings::kButtonCount] = {
		RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_Y, RETRO_DEVICE_ID_JOYPAD_X,
		RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_START};
	_input->CountRead();
	PadState state;
	uint64_t held = _input->PadButtons(player);
	_input->PadAxes(player, state.axes);
	auto on = [&](int xboxButton) { return ((held >> xboxButton) & 1) != 0; };
	if(on(pad::Up) || on(pad::StickUp)) state.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
	if(on(pad::Down) || on(pad::StickDown)) state.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
	if(on(pad::Left) || on(pad::StickLeft)) state.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
	if(on(pad::Right) || on(pad::StickRight)) state.buttons |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
	// Turbo: on for 2 reads, off for 2 (about 15 presses a second)
	uint64_t tick = player == 0 ? _turboTick++ : _turboTick.load();
	bool turboPhase = ((tick >> 1) & 1) == 0;
	for(int i = 0; i < AppSettings::kButtonCount; i++) {
		if(!on(_settings.buttons[i])) {
			continue;
		}
		if(i < kTurboCount && _settings.turbo[i] && !turboPhase) {
			continue;
		}
		state.buttons |= 1u << kRetroId[i];
	}
	return state;
}

void App::ReloadFont()
{
	int w = 1920, h = 1080;
	SDL_GetCurrentRenderOutputSize(_sdlRenderer, &w, &h);
	// About 50 pixels tall on a 1080p TV at 100%
	int pixels = std::max(20, h * _settings.uiScale / 2200);
	if(pixels == _fontPixels) {
		_fontDirty = false;
		return;
	}
	ImGuiIO& io = ImGui::GetIO();
	io.Fonts->Clear();
	ImFont* font = io.Fonts->AddFontFromFileTTF(U8(_resFolder / "SplineSans-Bold.ttf").c_str(), (float)pixels);
	if(!font) {
		ImFontConfig config;
		config.SizePixels = (float)pixels;
		io.Fonts->AddFontDefault(&config);
	}
	ImGui_ImplSDLRenderer3_DestroyDeviceObjects();
	_fontPixels = pixels;
	_fontDirty = false;
}

int App::Run()
{
	if(!Init()) {
		return 1;
	}

	while(_running) {
		g_uiBeat++;
		Mark("reading Xbox events");
		SDL_Event event;
		while(SDL_PollEvent(&event)) {
			ImGui_ImplSDL3_ProcessEvent(&event);
			switch(event.type) {
				case SDL_EVENT_QUIT:
				case SDL_EVENT_TERMINATING:
					_running = false;
					break;
				case SDL_EVENT_WILL_ENTER_BACKGROUND:
					_settings.Save();
					if(_screen == Screen::Playing && !_menuOpen) {
						OpenMenu();
					}
					break;
				default:
					break;
			}
		}

		Mark("checking the folder picker");
		{
			std::lock_guard lock(_pickLock);
			if(_pickDone) {
				_pickDone = false;
				if(!_pickedFolder.empty()) {
					_settings.gamesFolder = _pickedFolder;
					_settings.Save();
					std::vector<fs::path> folders{_profile / "Games"};
					for(const fs::path& folder : uwp::EsDeFolders({"snes", "sfc", "snesna", "superfamicom", "satellaview", "sufami"})) {
						folders.push_back(folder);
					}
					folders.push_back(FromU8(_pickedFolder));
					_games = ScanGames(folders);
					Toast("Found " + std::to_string(_games.size()) + " games");
				}
			}
			if(_pictureDone) {
				_pictureDone = false;
				if(!_pickedPicture.empty()) {
					UseBackgroundPicture(FromU8(_pickedPicture));
				}
			}
		}

		Mark("reading the controllers");
		_input->Poll();
		if(_loading && _loadDone) {
			Mark("finishing the game load");
			FinishLoad();
		}
		if(!_loading) {
			Mark("checking the menu buttons");
			HandleHotkeys();
		}
		Mark("copying the game picture");
		UploadFrame();
		Mark("checking on the game");
		Watchdog();

		if(_fontDirty) {
			ReloadFont();
		}
		Mark("starting the menus");
		ImGui_ImplSDLRenderer3_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();

		Mark("drawing the game list or menu");
		if(_screen == Screen::Library && !_loading) {
			DrawLibrary();
		} else if(_screen == Screen::Playing && _menuOpen) {
			DrawMenu();
		}
		Mark("drawing the info box");
		DrawInfo();
		DrawToast();

		Mark("drawing the screen");
		ImGui::Render();
		SDL_SetRenderDrawColor(_sdlRenderer, 0, 0, 0, 255);
		SDL_RenderClear(_sdlRenderer);
		if(_screen == Screen::Playing) {
			DrawGame();
		}
		ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), _sdlRenderer);
		Mark("showing the frame on the TV");
		SDL_RenderPresent(_sdlRenderer);
	}

	Shutdown();
	return 0;
}

void App::Shutdown()
{
	if(_loadThread.joinable()) {
		if(_loadDone) {
			_loadThread.join();
		} else {
			uwp::Log("[!] Closing while a game is still loading (step: " + _step + ")");
			_loadThread.detach();
			return; // the core is stuck inside the load: leave it, the app is closing anyway
		}
	}
	if(_core) {
		uwp::Log("SNES closed normally");
		_settings.Save();
		_core.reset(); // writes the battery save
		_input.reset();
		_audio.reset();
	}
	_knownCheats.reset();
	if(ImGui::GetCurrentContext()) {
		ImGui_ImplSDLRenderer3_Shutdown();
		ImGui_ImplSDL3_Shutdown();
		ImGui::DestroyContext();
	}
	_covers.reset();
	if(_bgThread.joinable()) {
		_bgThread.join();
	}
	FreeBackground();
	if(_gameTexture) {
		SDL_DestroyTexture(_gameTexture);
		_gameTexture = nullptr;
	}
	if(_sdlRenderer) {
		SDL_DestroyRenderer(_sdlRenderer);
		_sdlRenderer = nullptr;
	}
	if(_window) {
		SDL_DestroyWindow(_window);
		_window = nullptr;
		SDL_Quit();
	}
}

void App::HandleHotkeys()
{
	uint64_t buttons = _input->AnyPadButtons();
	uint64_t pressed = buttons & ~_lastPadButtons;
	_lastPadButtons = buttons;

	auto held = [&](pad::Button b) { return (buttons >> b) & 1; };
	auto down = [&](pad::Button b) { return (pressed >> b) & 1; };

	if(_screen == Screen::Playing && !_menuOpen) {
		// Menu: click the right stick, or press View and Menu together
		bool combo = held(pad::Back) && held(pad::Start) && (down(pad::Back) || down(pad::Start));
		if(down(pad::RightStick) || combo) {
			OpenMenu();
		}
	} else if(_screen == Screen::Playing && _menuOpen && _page == MenuPage::Main && down(pad::RightStick)) {
		CloseMenu();
	}
}

void App::UploadFrame()
{
	uint32_t w = 0, h = 0;
	if(!_core || !_core->TakeFrame(_frameBuffer, w, h)) {
		return;
	}
	// Max HD: the texture holds the picture made 4 times bigger with its edges rounded off
	const bool maxHd = _settings.picture == PictureStyle::MaxHd;
	const uint32_t texW = maxHd ? w * 4 : w;
	const uint32_t texH = maxHd ? h * 4 : h;
	if(maxHd) {
		edge::Scale4x(_frameBuffer, w, h, _scaleScratch, _scaled);
	}
	if(!_gameTexture || texW != _gameTexW || texH != _gameTexH) {
		if(_gameTexture) {
			SDL_DestroyTexture(_gameTexture);
		}
		_gameTexture = SDL_CreateTexture(_sdlRenderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, (int)texW, (int)texH);
		_gameTexW = texW;
		_gameTexH = texH;
	}
	// The game's own picture size (scanlines and pixel perfect are worked out from it)
	_textureWidth = w;
	_textureHeight = h;
	if(_gameTexture) {
		const std::vector<uint32_t>& pixels = maxHd ? _scaled : _frameBuffer;
		SDL_UpdateTexture(_gameTexture, nullptr, pixels.data(), (int)(texW * sizeof(uint32_t)));
		_hasFrame = true;
	}
}

void App::DrawGame()
{
	if(!_gameTexture || !_hasFrame) {
		return;
	}
	SDL_SetTextureScaleMode(_gameTexture, _settings.picture == PictureStyle::SharpPixels ? SDL_SCALEMODE_NEAREST : SDL_SCALEMODE_LINEAR);

	int outW = 0, outH = 0;
	SDL_GetCurrentRenderOutputSize(_sdlRenderer, &outW, &outH);
	SDL_FRect dst{0, 0, (float)outW, (float)outH};
	if(_settings.shape != ScreenShape::FillScreen) {
		double aspect = _core->AspectRatio();
		if(_settings.shape == ScreenShape::PixelPerfect) {
			aspect = (double)_textureWidth / std::max(1u, _textureHeight);
		}
		float w = (float)outW;
		float h = (float)(outW / aspect);
		if(h > outH) {
			h = (float)outH;
			w = (float)(outH * aspect);
		}
		if(_settings.shape == ScreenShape::PixelPerfect && _textureHeight > 0) {
			// Whole multiples of the picture, so every pixel is the same size
			int scale = std::max(1, (int)(outH / _textureHeight));
			while(scale > 1 && _textureWidth * scale > (uint32_t)outW) {
				scale--;
			}
			h = (float)(_textureHeight * scale);
			w = (float)(_textureWidth * scale);
		}
		dst = SDL_FRect{(outW - w) / 2, (outH - h) / 2, w, h};
	}
	SDL_RenderTexture(_sdlRenderer, _gameTexture, nullptr, &dst);
	DrawScanlines(dst);
}

// Dark lines between the picture's rows, like an old TV
void App::DrawScanlines(const SDL_FRect& dst)
{
	if(_settings.scanlines <= 0 || _textureHeight == 0) {
		return;
	}
	float row = dst.h / (float)_textureHeight;
	if(row < 2.0f) {
		return; // too small to show
	}
	SDL_SetRenderDrawBlendMode(_sdlRenderer, SDL_BLENDMODE_BLEND);
	SDL_SetRenderDrawColor(_sdlRenderer, 0, 0, 0, (Uint8)(_settings.scanlines * 255 / 100));
	std::vector<SDL_FRect> lines;
	lines.reserve(_textureHeight);
	float thickness = std::max(1.0f, row * 0.4f);
	for(uint32_t y = 0; y < _textureHeight; y++) {
		lines.push_back(SDL_FRect{dst.x, dst.y + y * row + row - thickness, dst.w, thickness});
	}
	SDL_RenderFillRects(_sdlRenderer, lines.data(), (int)lines.size());
	SDL_SetRenderDrawBlendMode(_sdlRenderer, SDL_BLENDMODE_NONE);
}

void App::SetStep(std::string step)
{
	uwp::Log("Step: " + step);
	std::lock_guard lock(_stepLock);
	_step = std::move(step);
	_stepTime = std::chrono::steady_clock::now();
}

void App::StartGame(const GameEntry& game)
{
	if(_loading) {
		return;
	}
	if(_loadThread.joinable()) {
		_loadThread.join();
	}
	ApplyCoreSettings();
	_loadGame = game;
	_loadOk = false;
	_loadError.clear();
	_loadDone = false;
	_warnedSlowLoad = false;
	_loadStart = std::chrono::steady_clock::now();
	_loading = true;
	SetStep("Getting ready to start " + game.title);
	_loadThread = std::thread([this, game] { LoadOnWorker(game); });
}

// Runs on the load thread. Only touches the core and the step text; the UI thread waits for _loadDone.
void App::LoadOnWorker(GameEntry game)
{
	try {
		std::error_code ec;
		SetStep("Opening the game file: " + U8(game.path));
		if(!fs::exists(game.path, ec)) {
			_loadError = "The app could not find the file. It may be on a drive the app is not allowed to open.";
			_loadDone = true;
			return;
		}
		SetStep("Snes9x is starting the game");
		g_workerMarker = "inside the core's load";
		bool ok = _core->Load(game.path);
		g_workerMarker = "load finished";
		_loadOk = ok;
		if(!ok) {
			_loadError = _core->Error();
		}
	} catch(const std::exception& e) {
		_loadError = std::string("Error while starting the game: ") + e.what();
	} catch(...) {
		_loadError = "Unknown error while starting the game";
	}
	_loadDone = true;
}

void App::FinishLoad()
{
	if(_loadThread.joinable()) {
		_loadThread.join();
	}
	_loading = false;
	if(!_loadOk) {
		uwp::Log("[!] Load failed: " + _loadError);
		SetStep("Could not start " + _loadGame.title);
		Toast(_loadError, 10);
		_focusNext = true;
		return;
	}
	SetStep("Game started, waiting for the first picture");
	_currentGame = _loadGame;
	_cheatFile = _profile / "Cheats" / FromU8(SafeFileName(_currentGame.title) + ".txt");
	_cheats = GameCheats{};
	_cheats.Load(_cheatFile);
	ApplyCheats();
	_hasFrame = false;
	_screen = Screen::Playing;
	_menuOpen = false;
	_playStart = std::chrono::steady_clock::now();
	_framesAtStart = _core->FrameCount();
	_warnedNoPicture = false;
	Toast("Click the right stick for the menu");
}

void App::Watchdog()
{
	g_coreMarker = _core ? _core->Marker() : "no core";
	auto now = std::chrono::steady_clock::now();
	if(now - _lastWatch < std::chrono::milliseconds(500)) {
		return;
	}
	_lastWatch = now;

	if(_loading && !_warnedSlowLoad && now - _loadStart > std::chrono::seconds(10)) {
		_warnedSlowLoad = true;
		std::lock_guard lock(_stepLock);
		uwp::Log("[!] Still loading after 10 seconds, stuck at: " + _step + ". Loader: " + g_workerMarker.load() +
			". Core: " + g_coreMarker.load());
	}
	if(_screen == Screen::Playing && !_menuOpen && !_warnedNoPicture && now - _playStart > std::chrono::seconds(5) &&
		_core->FrameCount() == _framesAtStart) {
		_warnedNoPicture = true;
		uwp::Log(std::string("[!] The game is running but no picture arrived after 5 seconds. Core: ") +
			g_coreMarker.load() + ". Sounds: " + std::to_string(_audio->BufferCount()));
	}
	if(_screen == Screen::Playing && _hasFrame) {
		std::lock_guard lock(_stepLock);
		if(_step.rfind("Game started", 0) == 0) {
			_step = "Playing";
		}
	}
}

void App::QuitGame()
{
	_core->Unload();
	_audio->Flush();
	_screen = Screen::Library;
	_menuOpen = false;
	_page = MenuPage::Main;
	_focusNext = true;
	_hasFrame = false;
}

void App::OpenMenu()
{
	_core->Pause(true);
	_audio->Flush();
	_menuOpen = true;
	_page = MenuPage::Main;
	_focusNext = true;
}

void App::CloseMenu()
{
	_menuOpen = false;
	_core->Pause(false);
}

void App::ApplyCheats()
{
	_core->SetCheats(_cheats.enabledCodes);
}

void App::Toast(std::string text, int seconds)
{
	_toast = std::move(text);
	_toastUntil = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
}

bool App::BackPressed() const
{
	return ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false);
}

// ------------------------------------------------------------------------------------------------------------------
// Screens

static void BeginFullScreen(const char* name, bool& focusNext, float widthFraction = 1.0f)
{
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImVec2 size(viewport->Size.x * widthFraction, viewport->Size.y);
	ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + (viewport->Size.x - size.x) / 2, viewport->Pos.y));
	ImGui::SetNextWindowSize(size);
	if(focusNext) {
		ImGui::SetNextWindowFocus();
	}
	ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
	if(focusNext) {
		ImGui::SetKeyboardFocusHere();
		focusNext = false;
	}
}

static void Title(const char* text)
{
	ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.25f, 1.0f));
	ImGui::SetWindowFontScale(1.4f);
	ImGui::TextUnformatted(text);
	ImGui::SetWindowFontScale(1.0f);
	ImGui::PopStyleColor();
	ImGui::Separator();
}

// The moving backdrop behind the game list: a deep blue gradient with soft lights drifting slowly across it and a
// faint band of light sweeping by now and then (the same look as EMU's game list)
static void DrawMovingBackground()
{
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	const ImVec2 size = ImGui::GetMainViewport()->Size;
	const float t = (float)ImGui::GetTime();
	dl->AddRectFilledMultiColor(ImVec2(0, 0), size, IM_COL32(12, 30, 78, 255), IM_COL32(8, 22, 62, 255),
		IM_COL32(2, 7, 24, 255), IM_COL32(4, 12, 36, 255));

	struct Light
	{
		float periodX, periodY, phase, radius;
		int r, g, b;
	};
	static const Light lights[] = {
		{47.0f, 31.0f, 0.0f, 0.55f, 40, 110, 230},
		{61.0f, 43.0f, 2.1f, 0.45f, 20, 70, 200},
		{37.0f, 53.0f, 4.2f, 0.38f, 70, 140, 255},
	};
	constexpr float kTau = 6.2831853f;
	const float extent = std::max(size.x, size.y);
	for(const Light& light : lights) {
		ImVec2 centre(size.x * (0.5f + 0.42f * std::sin(t * kTau / light.periodX + light.phase)),
			size.y * (0.5f + 0.38f * std::cos(t * kTau / light.periodY + light.phase * 1.3f)));
		const int rings = 14;
		for(int i = rings; i >= 1; i--) {
			dl->AddCircleFilled(centre, extent * light.radius * i / rings, IM_COL32(light.r, light.g, light.b, 6), 48);
		}
	}

	const float sweep = std::fmod(t / 14.0f, 1.0f);
	const float x = -size.x * 0.4f + sweep * size.x * 1.8f;
	const float width = size.x * 0.18f;
	const ImU32 clear = IM_COL32(120, 170, 255, 0);
	const ImU32 glow = IM_COL32(120, 170, 255, 13);
	dl->AddRectFilledMultiColor(ImVec2(x, 0), ImVec2(x + width, size.y), clear, glow, glow, clear);
	dl->AddRectFilledMultiColor(ImVec2(x + width, 0), ImVec2(x + width * 2, size.y), glow, clear, clear, glow);
}

// The player's own picture: copied into the profile folder (so it stays even if the original is moved), then decoded
// on a thread of its own
void App::UseBackgroundPicture(const fs::path& picked)
{
	std::error_code ec;
	std::string ext = picked.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
	for(const char* old : {".jpg", ".jpeg", ".png", ".bmp"}) {
		fs::remove(_profile / (std::string("Background") + old), ec);
	}
	fs::path copy = _profile / ("Background" + ext);
	if(!fs::copy_file(picked, copy, fs::copy_options::overwrite_existing, ec)) {
		uwp::Log("Could not copy the background picture " + U8(picked) + ": " + ec.message());
		copy = picked; // use it where it is
	}
	_settings.backgroundPicture = U8(copy);
	_settings.Save();
	LoadBackground();
	Toast("Your background picture is on");
}

void App::LoadBackground()
{
	FreeBackground();
	if(_settings.backgroundPicture.empty()) {
		return;
	}
	if(_bgThread.joinable()) {
		_bgThread.join();
	}
	fs::path file = FromU8(_settings.backgroundPicture);
	_bgThread = std::thread([this, file] {
		uwp::InitThreadForWinRT();
		std::ifstream in(file, std::ios::binary);
		std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
		std::vector<uint8_t> rgba;
		int w = 0, h = 0;
		bool ok = !bytes.empty() && DecodeImage(bytes, rgba, w, h, 2560);
		if(!ok) {
			uwp::Log("Could not read the background picture " + U8(file));
		}
		std::lock_guard lock(_bgLock);
		_bgPixels = ok ? std::move(rgba) : std::vector<uint8_t>{};
		_bgPixW = w;
		_bgPixH = h;
		_bgReady = ok;
	});
}

void App::FreeBackground()
{
	if(_bgTexture) {
		SDL_DestroyTexture(_bgTexture);
		_bgTexture = nullptr;
	}
	std::lock_guard lock(_bgLock);
	_bgReady = false;
	_bgPixels.clear();
}

// Behind the game list: the player's own picture (filling the screen, darkened a little so the text stays easy to
// read), or the moving blue backdrop
void App::DrawBackground()
{
	{
		std::lock_guard lock(_bgLock);
		if(_bgReady) {
			_bgReady = false;
			if(_bgTexture) {
				SDL_DestroyTexture(_bgTexture);
			}
			_bgTexture = SDL_CreateTexture(_sdlRenderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC, _bgPixW, _bgPixH);
			if(_bgTexture) {
				SDL_UpdateTexture(_bgTexture, nullptr, _bgPixels.data(), _bgPixW * 4);
				SDL_SetTextureScaleMode(_bgTexture, SDL_SCALEMODE_LINEAR);
				_bgTexW = _bgPixW;
				_bgTexH = _bgPixH;
			}
			_bgPixels.clear();
		}
	}
	if(!_bgTexture || _bgTexW <= 0 || _bgTexH <= 0) {
		DrawMovingBackground();
		return;
	}
	ImDrawList* dl = ImGui::GetBackgroundDrawList();
	const ImVec2 size = ImGui::GetMainViewport()->Size;
	// Fill the screen, keeping the picture's shape (the edges that do not fit are cut off)
	const float scale = std::max(size.x / _bgTexW, size.y / _bgTexH);
	const float w = _bgTexW * scale, h = _bgTexH * scale;
	const ImVec2 min((size.x - w) * 0.5f, (size.y - h) * 0.5f);
	dl->AddImage((ImTextureID)(intptr_t)_bgTexture, min, ImVec2(min.x + w, min.y + h));
	dl->AddRectFilled(ImVec2(0, 0), size, IM_COL32(0, 0, 0, 120));
}

void App::DrawLibrary()
{
	DrawBackground();
	ImGui::SetNextWindowBgAlpha(0.0f); // let the backdrop show through
	BeginFullScreen("Library", _focusNext);
	Title("SNES   (version " APP_VERSION ")");
	DrawLastSession();

	if(BigButton("Choose games folder")) {
		uwp::PickFolder([this](std::string folder) {
			std::lock_guard lock(_pickLock);
			_pickedFolder = std::move(folder);
			_pickDone = true;
		});
	}
	if(ImGui::CollapsingHeader("Picture and sound")) {
		DrawPictureOptions();
		DrawSoundOptions();
	}
	if(ImGui::CollapsingHeader("Background")) {
		if(BigButton("Choose my own background picture")) {
			uwp::PickPicture([this](std::string file) {
				std::lock_guard lock(_pickLock);
				_pickedPicture = std::move(file);
				_pictureDone = true;
			});
		}
		ImGui::TextDisabled("A .jpg or .png picture from your drive");
		if(!_settings.backgroundPicture.empty() && BigButton("Use the moving blue background")) {
			_settings.backgroundPicture.clear();
			_settings.Save();
			FreeBackground();
			Toast("Moving blue background is back");
		}
	}

	if(_games.empty()) {
		ImGui::Spacing();
		ImGui::TextWrapped("No games yet. Put your Super Nintendo games (.sfc, .smc) in this folder:");
		ImGui::TextWrapped("%s", U8(_profile / "Games").c_str());
		ImGui::TextWrapped("or press A on \"Choose games folder\".");
	} else {
		ImGui::Text("%d games. Press A to play.", (int)_games.size());
		const float listWidth = ImGui::GetContentRegionAvail().x * 0.5f;
		ImGui::BeginChild("games", ImVec2(listWidth, 0), ImGuiChildFlags_NavFlattened);
		for(size_t i = 0; i < _games.size(); i++) {
			ImGui::PushID((int)i);
			bool chosen = ImGui::Selectable(_games[i].title.c_str(), false);
			if(ImGui::IsItemFocused()) {
				_focusedGame = i;
			}
			if(chosen) {
				GameEntry game = _games[i];
				ImGui::PopID();
				ImGui::EndChild();
				ImGui::End();
				StartGame(game);
				return;
			}
			ImGui::PopID();
		}
		ImGui::EndChild();

		// Box art and description of the highlighted game
		ImGui::SameLine();
		ImGui::BeginChild("details", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_NoNav);
		if(_covers && _focusedGame < _games.size()) {
			const Covers::Info& info = _covers->Get(_sdlRenderer, _games[_focusedGame]);
			ImVec2 avail = ImGui::GetContentRegionAvail();
			if(info.texture) {
				float scale = std::min(avail.x / info.width, avail.y * 0.6f / info.height);
				float w = info.width * scale;
				ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - w) / 2);
				ImGui::Image((ImTextureID)(intptr_t)info.texture, ImVec2(w, info.height * scale));
			} else if(!info.coverDone) {
				ImGui::TextDisabled("Looking for the box art...");
			} else {
				ImGui::TextDisabled("No box art found");
			}
			ImGui::SetWindowFontScale(0.75f);
			if(!info.descriptionDone) {
				ImGui::TextDisabled("Looking for a description...");
			} else if(!info.description.empty()) {
				ImGui::TextWrapped("%s", info.description.c_str());
			}
			ImGui::SetWindowFontScale(1.0f);
		}
		ImGui::EndChild();
	}
	ImGui::End();
}

void App::DrawMenu()
{
	BeginFullScreen("Menu", _focusNext, 0.6f);
	switch(_page) {
		case MenuPage::Main: DrawMenuMain(); break;
		case MenuPage::Picture:
			Title("Picture");
			DrawPictureOptions();
			if(BigButton("Back")) {
				_page = MenuPage::Main;
				_focusNext = true;
			}
			break;
		case MenuPage::Sound:
			Title("Sound and text");
			DrawSoundOptions();
			if(BigButton("Back")) {
				_page = MenuPage::Main;
				_focusNext = true;
			}
			break;
		case MenuPage::Controller: DrawController(); break;
		case MenuPage::Cheats: DrawCheats(); break;
		case MenuPage::AddCode: DrawAddCode(); break;
	}
	ImGui::End();

	if(_capturing < 0 && BackPressed()) {
		if(_page == MenuPage::Main) {
			CloseMenu();
		} else if(_page == MenuPage::AddCode) {
			_page = MenuPage::Cheats;
			_focusNext = true;
		} else {
			_page = MenuPage::Main;
			_focusNext = true;
		}
	}
}

void App::DrawMenuMain()
{
	Title(_currentGame.title.c_str());
	if(BigButton("Back to game")) {
		CloseMenu();
		return;
	}
	Stepper("Save slot", _saveSlot, 1, 10, 1);
	if(BigButton("Save game")) {
		Toast(_core->SaveState(_saveSlot) ? "Saved to slot " + std::to_string(_saveSlot) : std::string("Could not save"));
	}
	if(BigButton("Load game")) {
		if(_core->LoadState(_saveSlot)) {
			Toast("Loaded slot " + std::to_string(_saveSlot));
			CloseMenu();
			return;
		}
		Toast("Slot " + std::to_string(_saveSlot) + " is empty");
	}
	if(BigButton("Cheat codes")) {
		_knownCheats->Request(_currentGame);
		_page = MenuPage::Cheats;
		_focusNext = true;
	}
	if(BigButton("Picture")) {
		_page = MenuPage::Picture;
		_focusNext = true;
	}
	if(BigButton("Sound and text size")) {
		_page = MenuPage::Sound;
		_focusNext = true;
	}
	if(BigButton("Controller buttons")) {
		_page = MenuPage::Controller;
		_focusNext = true;
	}
	if(BigButton("Reset game")) {
		_core->Reset();
		CloseMenu();
		return;
	}
	if(BigButton(_showInfo ? "Hide info on screen" : "Show info on screen")) {
		_showInfo = !_showInfo;
	}
	if(BigButton("Quit to game list")) {
		QuitGame();
	}
}

void App::DrawPictureOptions()
{
	bool changed = false;
	ImGui::TextUnformatted("Picture style");
	for(int i = 0; i < (int)PictureStyle::Count; i++) {
		PictureStyle style = (PictureStyle)i;
		if(ImGui::RadioButton(PictureStyleName(style), _settings.picture == style)) {
			_settings.picture = style;
			changed = true;
		}
		if(ImGui::IsItemFocused()) {
			ImGui::SameLine();
			ImGui::TextDisabled("%s", PictureStyleHelp(style));
		}
	}
	ImGui::Separator();
	ImGui::TextUnformatted("Screen shape");
	for(int i = 0; i < (int)ScreenShape::Count; i++) {
		ScreenShape shape = (ScreenShape)i;
		if(ImGui::RadioButton(ScreenShapeName(shape), _settings.shape == shape)) {
			_settings.shape = shape;
			changed = true;
		}
	}
	ImGui::Separator();
	changed |= Stepper("TV lines", _settings.scanlines, 0, 100, 10, "%");
	changed |= ImGui::Checkbox("No sprite flicker", &_settings.noFlicker);
	changed |= ImGui::Checkbox("Faster Super FX chip (Star Fox, Yoshi's Island run smoother)", &_settings.fastSuperFx);
	if(changed) {
		_settings.Save();
		ApplyCoreSettings();
	}
}

void App::DrawSoundOptions()
{
	if(Stepper("Volume", _settings.volume, 0, 100, 10, "%")) {
		_settings.Save();
		ApplyCoreSettings();
	}
	if(Stepper("Text size", _settings.uiScale, 60, 160, 10, "%")) {
		_settings.Save();
		_fontDirty = true;
	}
}

void App::DrawCheats()
{
	Title("Cheat codes");
	bool changed = false;
	if(BigButton("Type a new code")) {
		_newCode.clear();
		_page = MenuPage::AddCode;
		_focusNext = true;
		return;
	}
	if(BigButton("Back")) {
		_page = MenuPage::Main;
		_focusNext = true;
		return;
	}

	auto row = [&](const CheatEntry& cheat, int id) {
		bool on = _cheats.IsEnabled(cheat.code);
		ImGui::PushID(id);
		std::string label = cheat.desc + "   (" + cheat.code + ")";
		if(ImGui::Checkbox(label.c_str(), &on)) {
			_cheats.SetEnabled(cheat.code, on);
			changed = true;
		}
		ImGui::PopID();
	};

	int id = 0;
	if(!_cheats.custom.empty()) {
		ImGui::TextUnformatted("Your codes");
		for(const CheatEntry& cheat : _cheats.custom) {
			row(cheat, id++);
		}
	}
	ImGui::Separator();
	ImGui::TextWrapped("%s", _knownCheats->Status().c_str());
	if(_knownCheats->Done()) {
		for(const CheatEntry& cheat : _knownCheats->Codes()) {
			row(cheat, id++);
		}
	}

	if(changed) {
		_cheats.Save(_cheatFile);
		ApplyCheats();
		Toast("Cheats updated");
	}
}

void App::DrawAddCode()
{
	Title("Type a cheat code");
	ImGui::SetWindowFontScale(1.6f);
	ImGui::Text("%s_", _newCode.c_str());
	ImGui::SetWindowFontScale(1.0f);
	ImGui::TextDisabled("Game Genie: DD62-6DAD     Pro Action Replay: 7E0DBF63     (+ joins codes)");

	const float size = ImGui::GetFontSize() * 2.0f;
	const int keys = (int)std::strlen(kCodeKeys);
	for(int i = 0; i < keys; i++) {
		char letter[2] = {kCodeKeys[i], 0};
		if(i % 10 != 0) {
			ImGui::SameLine();
		}
		if(ImGui::Button(letter, ImVec2(size, size)) && _newCode.size() < 9 * 8 + 7) {
			_newCode += letter;
		}
	}
	if(ImGui::Button("Delete letter") && !_newCode.empty()) {
		_newCode.pop_back();
	}
	bool valid = ValidCode(_newCode);
	ImGui::BeginDisabled(!valid);
	if(BigButton("Add this code")) {
		CheatEntry entry;
		entry.code = _newCode;
		entry.desc = "My code " + std::to_string(_cheats.custom.size() + 1);
		entry.custom = true;
		_cheats.custom.push_back(entry);
		_cheats.SetEnabled(entry.code, true);
		_cheats.Save(_cheatFile);
		ApplyCheats();
		Toast("Code " + _newCode + " is on");
		_page = MenuPage::Cheats;
		_focusNext = true;
	}
	ImGui::EndDisabled();
	if(BigButton("Cancel")) {
		_page = MenuPage::Cheats;
		_focusNext = true;
	}
}

void App::DrawToast()
{
	if(_toast.empty() || std::chrono::steady_clock::now() > _toastUntil) {
		return;
	}
	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x / 2, viewport->Pos.y + viewport->Size.y * 0.9f), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGui::Begin("Toast", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
	ImGui::TextUnformatted(_toast.c_str());
	ImGui::End();
}

void App::DrawLastSession()
{
	if(_lastSession.empty()) {
		return;
	}
	ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.35f, 0.08f, 0.08f, 1.0f));
	ImGui::BeginChild("lastSession", ImVec2(0, 0), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders);
	ImGui::TextWrapped("Last time, the SNES app had a problem. These are its last notes. Take a photo of this for Claude:");
	ImGui::SetWindowFontScale(0.6f);
	for(const std::string& line : _lastSession) {
		ImGui::TextWrapped("%s", line.c_str());
	}
	ImGui::SetWindowFontScale(1.0f);
	if(ImGui::Button("Hide this")) {
		_lastSession.clear();
		_focusNext = true;
	}
	ImGui::EndChild();
	ImGui::PopStyleColor();
}

// The info panel: what the app is doing right now, the core's counters and the newest log lines
void App::DrawInfo()
{
	if(g_crashed) {
		std::string text;
		{
			std::lock_guard lock(g_crashLock);
			text = g_crashText;
		}
		const ImGuiViewport* viewport = ImGui::GetMainViewport();
		ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 24, viewport->Pos.y + 24));
		ImGui::SetNextWindowSize(ImVec2(viewport->Size.x - 48, 0));
		ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.45f, 0.05f, 0.05f, 0.97f));
		ImGui::Begin("Crash", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
		ImGui::TextWrapped("%s", text.c_str());
		ImGui::TextWrapped("Take a photo of this for Claude, then close the SNES app with the Xbox button.");
		ImGui::SetWindowFontScale(0.6f);
		for(const std::string& line : uwp::RecentLog(8)) {
			ImGui::TextWrapped("%s", line.c_str());
		}
		ImGui::SetWindowFontScale(1.0f);
		ImGui::End();
		ImGui::PopStyleColor();
		return;
	}
	bool noPicture = _screen == Screen::Playing && !_hasFrame;
	if(!_loading && !noPicture && !(_showInfo && _screen == Screen::Playing && !_menuOpen)) {
		return;
	}
	auto now = std::chrono::steady_clock::now();
	std::string step;
	double stepSeconds = 0;
	{
		std::lock_guard lock(_stepLock);
		step = _step;
		stepSeconds = std::chrono::duration<double>(now - _stepTime).count();
	}

	const ImGuiViewport* viewport = ImGui::GetMainViewport();
	bool big = _loading || noPicture;
	ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + 24, viewport->Pos.y + viewport->Size.y - 24), ImGuiCond_Always, ImVec2(0.0f, 1.0f));
	ImGui::SetNextWindowSize(ImVec2(big ? viewport->Size.x - 48 : viewport->Size.x * 0.42f, 0));
	ImGui::SetNextWindowBgAlpha(0.85f);
	ImGui::Begin("Info", nullptr,
		ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
			ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
	if(!big) {
		ImGui::SetWindowFontScale(0.6f);
	}

	// A moving dot shows the screen itself is not frozen
	static const char* spinner[] = {"|", "/", "-", "\\"};
	int tick = (int)(std::chrono::duration<double>(now.time_since_epoch()).count() * 4) % 4;
	if(_loading) {
		double total = std::chrono::duration<double>(now - _loadStart).count();
		ImGui::Text("%s  Starting %s ... %.0f seconds", spinner[tick], _loadGame.title.c_str(), total);
	} else {
		ImGui::Text("%s  %s", spinner[tick], _screen == Screen::Playing ? _currentGame.title.c_str() : "SNES");
	}
	ImGui::TextWrapped("Now: %s (%.0f s)", step.c_str(), stepSeconds);
	ImGui::Text("Frames run: %llu   Pictures: %llu   Sounds: %llu   Controller reads: %llu",
		(unsigned long long)_core->FrameCount(), (unsigned long long)_core->FrameCount(),
		(unsigned long long)_audio->BufferCount(), (unsigned long long)_input->ReadCount());
	ImGui::TextWrapped("Core: %s", g_coreMarker.load());
	ImGui::TextWrapped("Sound queued: %u ms", _audio->QueuedMs());
	ImGui::TextWrapped("Sound: %s", _audio->State());
	if(_loading) {
		ImGui::TextWrapped("Loader: %s", g_workerMarker.load());
	}

	if(big) {
		ImGui::Separator();
		ImGui::SetWindowFontScale(0.6f);
		for(const std::string& line : uwp::RecentLog(8)) {
			bool problem = line.find("[!]") != std::string::npos;
			if(problem) {
				ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.45f, 0.4f, 1.0f));
			}
			ImGui::TextWrapped("%s", line.c_str());
			if(problem) {
				ImGui::PopStyleColor();
			}
		}
		ImGui::SetWindowFontScale(1.0f);
	}
	ImGui::SetWindowFontScale(1.0f);
	ImGui::End();
}

// Controller page: pick a Super Nintendo button, then press the Xbox button it should be on
void App::DrawController()
{
	Title("Controller buttons");
	uint64_t held = _input->AnyPadButtons();

	if(_capturing >= 0) {
		ImGui::TextWrapped("Press the Xbox button you want for Super Nintendo %s", kPadButtonNames[_capturing]);
		ImGui::TextDisabled("(A, B, X, Y, LB, RB, LT, RT, View, Menu or a left stick click)");
		if(!_captureReleased) {
			// Wait until the button that opened this is let go
			_captureReleased = held == 0;
			return;
		}
		for(pad::Button button : kAssignable) {
			if((held >> button) & 1) {
				_settings.buttons[_capturing] = button;
				_settings.Save();
				ApplyCoreSettings();
				Toast(std::string("SNES ") + kPadButtonNames[_capturing] + " is now Xbox " + XboxButtonName(button));
				_capturing = -1;
				_focusNext = true;
				_ignoreUntilReleased = true;
				break;
			}
		}
		return;
	}
	if(_ignoreUntilReleased) {
		// Do not let the button just pressed also click something on this page
		if(held != 0) {
			ImGui::GetIO().ClearInputKeys();
			return;
		}
		_ignoreUntilReleased = false;
	}

	ImGui::TextWrapped("Pick a Super Nintendo button, then press the Xbox button you want for it.");
	for(int i = 0; i < AppSettings::kButtonCount; i++) {
		std::string label = std::string("SNES ") + kPadButtonNames[i] + ":   Xbox " + XboxButtonName(_settings.buttons[i]);
		ImGui::PushID(i);
		if(BigButton(label.c_str())) {
			_capturing = i;
			_captureReleased = false;
		}
		ImGui::PopID();
	}
	ImGui::Separator();
	ImGui::TextUnformatted("Turbo (the button fires by itself while you hold it)");
	bool turboChanged = false;
	for(int i = 0; i < kTurboCount; i++) {
		std::string label = std::string("Turbo on ") + kPadButtonNames[i];
		turboChanged |= ImGui::Checkbox(label.c_str(), &_settings.turbo[i]);
	}
	if(turboChanged) {
		_settings.Save();
		ApplyCoreSettings();
	}
	ImGui::Separator();
	if(BigButton("Put buttons back the way they were")) {
		AppSettings defaults;
		std::copy(std::begin(defaults.buttons), std::end(defaults.buttons), _settings.buttons);
		for(bool& turbo : _settings.turbo) {
			turbo = false;
		}
		_settings.Save();
		ApplyCoreSettings();
		Toast("Buttons are back to normal");
	}
	if(BigButton("Back")) {
		_page = MenuPage::Main;
		_focusNext = true;
	}
}
