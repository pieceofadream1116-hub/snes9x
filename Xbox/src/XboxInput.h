#pragma once
// Xbox controllers. The UI thread reads them through SDL and publishes a snapshot that the emulation thread reads
// when the core polls the controllers.

#include <SDL3/SDL.h>

#include <array>
#include <atomic>
#include <cstdint>


namespace pad {

enum Button : uint16_t
{
	A = 0, B, X, Y, Back, Guide, Start, LeftStick, RightStick, LeftShoulder, RightShoulder,
	Up, Down, Left, Right,
	// Synthesized from the analog sticks and triggers
	StickUp = 32, StickDown, StickLeft, StickRight, LeftTrigger, RightTrigger,
	Count = 40
};

} // namespace pad

class XboxInput
{
public:
	static constexpr int kMaxPlayers = 4;

	XboxInput() = default;
	~XboxInput();

	/// UI thread: opens newly connected pads and publishes the current button state.
	void Poll();

	/// Buttons held on one pad (bit per pad::Button). Safe from any thread.
	uint64_t PadButtons(int player) const { return player >= 0 && player < kMaxPlayers ? _state[player].load() : 0; }

	/// The two sticks of one pad: left x, left y, right x, right y (-32768..32767, down is positive). Safe from any thread.
	void PadAxes(int player, int16_t axes[4]) const
	{
		uint64_t packed = player >= 0 && player < kMaxPlayers ? _axes[player].load() : 0;
		for(int i = 0; i < 4; i++) {
			axes[i] = (int16_t)(uint16_t)(packed >> (16 * i));
		}
	}

	/// Buttons held on any pad (bit per pad::Button), for menu navigation.
	uint64_t AnyPadButtons() const { return _anyPad; }

	/// How many times the core has read the controllers (for the on-screen info).
	uint64_t ReadCount() const { return _readCount; }
	void CountRead() { _readCount++; }

private:
	std::array<std::atomic<uint64_t>, kMaxPlayers> _state{};
	std::array<std::atomic<uint64_t>, kMaxPlayers> _axes{};
	std::atomic<uint64_t> _anyPad = 0;
	std::atomic<uint64_t> _readCount = 0;
	std::array<SDL_Gamepad*, kMaxPlayers> _pads{};
};
