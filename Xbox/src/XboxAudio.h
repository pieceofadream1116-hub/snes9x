#pragma once
// Plays the core's sound through XAudio2, the Xbox's own sound system.
//
// SDL's sound path (WASAPI) cannot be used on Xbox: opening the device waits forever for an activation that the
// console never delivers (found while making HD Eight and the Saturn app). XAudio2 starts instantly.
// Each frame's sound is copied into one of a few slots and queued on a source voice. The emulation thread waits
// while about 50 ms of sound is already queued, which keeps the game at its real speed with no crackle.

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

struct IXAudio2;
struct IXAudio2MasteringVoice;
struct IXAudio2SourceVoice;

class XboxAudio
{
public:
	XboxAudio();
	~XboxAudio();

	/// Starts XAudio2 (UI thread, at startup). Returns false if there is no sound; games then play silently.
	bool Open();
	bool Working() const { return _voice != nullptr; }

	/// What the sound is doing, for the on-screen info.
	const char* State() const { return _state; }

	/// 0-100
	void SetVolume(int percent);

	/// Emulation thread: queue interleaved stereo 16-bit sound.
	void Push(const int16_t* frames, uint32_t count, uint32_t sampleRate);
	/// Emulation thread: wait until the queue has room for another frame (or `stop` turns true).
	void WaitForRoom(const std::atomic<bool>& stop);
	/// Drop whatever is queued (when pausing or leaving a game).
	void Flush();

	uint64_t BufferCount() const { return _bufferCount; }
	uint32_t QueuedMs() const;

	/// Called from XAudio2's thread when a slot has finished playing.
	void OnSlotDone(size_t slot);

private:
	static constexpr size_t kSlots = 16;
	static constexpr uint32_t kTargetMs = 50;

	bool CreateVoice(uint32_t sampleRate);
	void Release();

	std::mutex _lock;
	IXAudio2* _engine = nullptr;
	IXAudio2MasteringVoice* _master = nullptr;
	IXAudio2SourceVoice* _voice = nullptr;
	class Callback;
	Callback* _callback = nullptr;

	std::array<std::vector<int16_t>, kSlots> _slots;
	std::array<std::atomic<bool>, kSlots> _busy{};
	std::array<uint32_t, kSlots> _slotFrames{};
	std::array<size_t, kSlots> _slotIds{};
	std::atomic<uint32_t> _queuedFrames = 0;
	size_t _next = 0;
	bool _started = false;
	uint32_t _voiceRate = 0;
	float _volume = 1.0f;

	std::atomic<const char*> _state = "not started";
	std::atomic<uint64_t> _bufferCount = 0;
};
