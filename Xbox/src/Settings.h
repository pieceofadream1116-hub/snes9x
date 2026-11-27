#pragma once
// The app's own settings (picture, sound, buttons, folders), saved as a small text file in the profile folder.

#include <filesystem>
#include <string>

/// The picture styles shown in the menu.
enum class PictureStyle
{
	SharpPixels = 0,  // original pixels, no filter
	Smooth,           // original pixels, smoothed when scaled up
	TvLook,           // NTSC (Blargg) composite TV signal, made by the core
	MaxHd,            // edges rounded off (Scale2x twice), then smoothed up to the TV's size
	Count
};

const char* PictureStyleName(PictureStyle style);
const char* PictureStyleHelp(PictureStyle style);

enum class ScreenShape
{
	OriginalTv = 0,   // the shape the game was made for (4:3 on a TV of the time)
	PixelPerfect,     // whole multiples of the picture, square pixels
	FillScreen,       // stretch to the whole TV
	Count
};

const char* ScreenShapeName(ScreenShape shape);

/// The Super Nintendo buttons that can be moved to another Xbox button, in AppSettings::buttons order.
inline constexpr const char* kPadButtonNames[] = {"B", "A", "Y", "X", "L", "R", "Select", "Start"};

/// Buttons that can have turbo: the face buttons and the shoulders (the first kTurboCount of kPadButtonNames)
inline constexpr int kTurboCount = 6;

struct AppSettings
{
	std::string gamesFolder;                 // extra folder picked by the player (the profile's Games folder is always scanned)
	PictureStyle picture = PictureStyle::Smooth;
	ScreenShape shape = ScreenShape::OriginalTv;
	int scanlines = 0;                       // 0-100
	bool noFlicker = true;                   // no sprite flicker
	bool fastSuperFx = false;                // Super FX chip at 150% (Star Fox runs smoother)
	int volume = 100;                        // 0-100
	int uiScale = 100;                       // text size in percent (100 = big, made for a TV)

	std::string backgroundPicture;           // the player's own picture behind the game list (empty: the moving blue one)

	/// Xbox button (pad::Button number) for each Super Nintendo button, in the order of kPadButtonNames.
	/// Default: the same places as on a Super Nintendo pad (Xbox A = B, B = A, X = Y, Y = X, LB/RB = L/R, View = Select,
	/// Menu = Start).
	static constexpr int kButtonCount = 8;
	int buttons[kButtonCount] = {0, 1, 2, 3, 9, 10, 4, 6};
	bool turbo[kTurboCount] = {};            // these buttons fire by themselves while held

	void Load(const std::filesystem::path& file);
	void Save() const;

private:
	std::filesystem::path _file;
};
