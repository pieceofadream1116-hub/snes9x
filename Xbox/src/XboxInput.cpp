#include "XboxInput.h"


namespace {

constexpr Sint16 kStickThreshold = 16000;
constexpr Sint16 kTriggerThreshold = 12000;

} // namespace

XboxInput::~XboxInput()
{
	for(auto& gamepad : _pads) {
		if(gamepad) {
			SDL_CloseGamepad(gamepad);
			gamepad = nullptr;
		}
	}
}

void XboxInput::Poll()
{
	// Keep the open pads in step with what is connected; players get the lowest free slot
	for(auto& gamepad : _pads) {
		if(gamepad && !SDL_GamepadConnected(gamepad)) {
			SDL_CloseGamepad(gamepad);
			gamepad = nullptr;
		}
	}
	int count = 0;
	SDL_JoystickID* ids = SDL_GetGamepads(&count);
	if(ids) {
		for(int i = 0; i < count; i++) {
			bool open = false;
			for(auto* gamepad : _pads) {
				if(gamepad && SDL_GetGamepadID(gamepad) == ids[i]) {
					open = true;
				}
			}
			if(open) {
				continue;
			}
			for(auto& gamepad : _pads) {
				if(!gamepad) {
					gamepad = SDL_OpenGamepad(ids[i]);
					break;
				}
			}
		}
		SDL_free(ids);
	}

	uint64_t any = 0;
	for(int player = 0; player < kMaxPlayers; player++) {
		SDL_Gamepad* gamepad = _pads[player];
		uint64_t bits = 0;
		uint64_t axes = 0;
		if(gamepad) {
			for(int b = 0; b <= pad::Right; b++) {
				if(SDL_GetGamepadButton(gamepad, (SDL_GamepadButton)b)) {
					bits |= 1ull << b;
				}
			}
			Sint16 lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
			Sint16 ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
			if(ly < -kStickThreshold) bits |= 1ull << pad::StickUp;
			if(ly > kStickThreshold) bits |= 1ull << pad::StickDown;
			if(lx < -kStickThreshold) bits |= 1ull << pad::StickLeft;
			if(lx > kStickThreshold) bits |= 1ull << pad::StickRight;
			Sint16 rx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTX);
			Sint16 ry = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHTY);
			axes = (uint64_t)(uint16_t)lx | ((uint64_t)(uint16_t)ly << 16) | ((uint64_t)(uint16_t)rx << 32) | ((uint64_t)(uint16_t)ry << 48);
			if(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > kTriggerThreshold) bits |= 1ull << pad::LeftTrigger;
			if(SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > kTriggerThreshold) bits |= 1ull << pad::RightTrigger;
		}
		_state[player] = bits;
		_axes[player] = axes;
		any |= bits;
	}
	_anyPad = any;
}

