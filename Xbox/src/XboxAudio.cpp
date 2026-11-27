#include "XboxAudio.h"

#include "UwpPlatform.h"

#include <windows.h>
#include <xaudio2.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>

class XboxAudio::Callback final : public IXAudio2VoiceCallback
{
public:
	explicit Callback(XboxAudio& owner) : _owner(owner) {}
	void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
	void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
	void STDMETHODCALLTYPE OnStreamEnd() override {}
	void STDMETHODCALLTYPE OnBufferStart(void*) override {}
	void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
	void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT) override {}
	void STDMETHODCALLTYPE OnBufferEnd(void* context) override
	{
		_owner.OnSlotDone(*static_cast<size_t*>(context));
	}

private:
	XboxAudio& _owner;
};

XboxAudio::XboxAudio()
{
	for(size_t i = 0; i < kSlots; i++) {
		_slotIds[i] = i;
	}
	_callback = new Callback(*this);
}

XboxAudio::~XboxAudio()
{
	{
		std::lock_guard lock(_lock);
		Release();
	}
	delete _callback;
}

void XboxAudio::Release()
{
	if(_voice) {
		_voice->Stop(0);
		_voice->FlushSourceBuffers();
		_voice->DestroyVoice();
		_voice = nullptr;
	}
	if(_master) {
		_master->DestroyVoice();
		_master = nullptr;
	}
	if(_engine) {
		_engine->Release();
		_engine = nullptr;
	}
	for(auto& busy : _busy) {
		busy = false;
	}
	_queuedFrames = 0;
	_started = false;
}

bool XboxAudio::Open()
{
	std::lock_guard lock(_lock);
	_state = "starting XAudio2";
	if(FAILED(XAudio2Create(&_engine, 0, XAUDIO2_DEFAULT_PROCESSOR)) || !_engine) {
		_engine = nullptr;
		_state = "no sound (XAudio2 would not start)";
		uwp::Log("[!] XAudio2Create failed");
		return false;
	}
	if(FAILED(_engine->CreateMasteringVoice(&_master, 2, 48000, 0, nullptr, nullptr, AudioCategory_GameEffects)) || !_master) {
		_master = nullptr;
		Release();
		_state = "no sound (no speakers found)";
		uwp::Log("[!] XAudio2 CreateMasteringVoice failed");
		return false;
	}
	_master->SetVolume(_volume);
	// Snes9x makes 32040 Hz sound; the voice is made again if a game asks for another rate
	if(!CreateVoice(44100)) {
		Release();
		return false;
	}
	return true;
}

bool XboxAudio::CreateVoice(uint32_t sampleRate)
{
	if(_voice) {
		_voice->Stop(0);
		_voice->FlushSourceBuffers();
		_voice->DestroyVoice();
		_voice = nullptr;
		for(auto& busy : _busy) {
			busy = false;
		}
		_queuedFrames = 0;
		_started = false;
	}
	WAVEFORMATEX format{};
	format.wFormatTag = WAVE_FORMAT_PCM;
	format.nChannels = 2;
	format.nSamplesPerSec = sampleRate;
	format.wBitsPerSample = 16;
	format.nBlockAlign = 4;
	format.nAvgBytesPerSec = sampleRate * 4;
	if(FAILED(_engine->CreateSourceVoice(&_voice, &format, 0, 2.0f, _callback)) || !_voice) {
		_voice = nullptr;
		_state = "no sound (could not make a voice)";
		uwp::Log("[!] XAudio2 CreateSourceVoice failed");
		return false;
	}
	_voiceRate = sampleRate;
	_state = "sound on (XAudio2)";
	uwp::Log("Sound on (XAudio2, " + std::to_string(sampleRate) + " Hz stereo)");
	return true;
}

void XboxAudio::SetVolume(int percent)
{
	std::lock_guard lock(_lock);
	_volume = std::clamp(percent, 0, 100) / 100.0f;
	if(_master) {
		_master->SetVolume(_volume);
	}
}

void XboxAudio::OnSlotDone(size_t slot)
{
	_queuedFrames -= _slotFrames[slot];
	_busy[slot] = false;
}

uint32_t XboxAudio::QueuedMs() const
{
	uint32_t rate = _voiceRate ? _voiceRate : 44100;
	return (uint32_t)((uint64_t)_queuedFrames * 1000 / rate);
}

void XboxAudio::Push(const int16_t* frames, uint32_t count, uint32_t sampleRate)
{
	_bufferCount++;
	std::lock_guard lock(_lock);
	if(!_engine || count == 0) {
		return;
	}
	if(sampleRate && sampleRate != _voiceRate && !CreateVoice(sampleRate)) {
		return;
	}
	if(!_voice) {
		return;
	}
	size_t slot = _next;
	if(_busy[slot]) {
		return; // every slot is still playing: skip this bit rather than make the game wait
	}
	_next = (_next + 1) % kSlots;

	auto& data = _slots[slot];
	data.assign(frames, frames + (size_t)count * 2);
	_slotFrames[slot] = count;
	_busy[slot] = true;
	_queuedFrames += count;

	XAUDIO2_BUFFER buffer{};
	buffer.AudioBytes = (UINT32)(data.size() * sizeof(int16_t));
	buffer.pAudioData = reinterpret_cast<const BYTE*>(data.data());
	buffer.pContext = &_slotIds[slot];
	if(FAILED(_voice->SubmitSourceBuffer(&buffer))) {
		_queuedFrames -= count;
		_busy[slot] = false;
		return;
	}
	if(!_started) {
		_voice->Start(0);
		_started = true;
	}
}

void XboxAudio::WaitForRoom(const std::atomic<bool>& stop)
{
	// Give up after a moment so a stalled sound device can never freeze the game
	auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(100);
	while(!stop && QueuedMs() > kTargetMs && std::chrono::steady_clock::now() < until) {
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
}

void XboxAudio::Flush()
{
	std::lock_guard lock(_lock);
	if(_voice) {
		_voice->Stop(0);
		_voice->FlushSourceBuffers();
		_started = false;
	}
}
