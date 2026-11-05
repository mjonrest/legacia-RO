// Microphone capture + Opus encode, and per-speaker jitter buffers mixed to the speakers.
// Uses the plain waveIn/waveOut API: 48 kHz, mono, 16-bit, 20 ms frames.
#include "voice.h"

#include <math.h>
#include <mmsystem.h>
#include <opus/opus.h>
#include <string.h>

#include <map>

static const int RATE = 48000;
static const int FRAME = 960; // 20 ms
static const int IN_BUFFERS = 4;
static const int OUT_BUFFERS = 6;
static const int JITTER_SLOTS = 16;
static const int START_DELAY_MS = 60;
static const int MAX_MISSES = 5;

// ---------------------------------------------------------------- speakers

struct Slot {
	bool has;
	uint16_t seq;
	int len;
	uint8_t data[1000];
};

struct Speaker {
	uint32_t id;
	OpusDecoder* dec;
	std::string name;
	Slot slots[JITTER_SLOTS];
	bool playing;
	uint16_t nextSeq;
	int misses;
	DWORD firstQueued;
	DWORD lastPacket;
	bool muted;
	int volume;
	int channel;
};

static CRITICAL_SECTION g_spkLock;
static std::map<uint32_t, Speaker*> g_speakers;

static Speaker* get_speaker(uint32_t id) {
	auto it = g_speakers.find(id);
	if (it != g_speakers.end())
		return it->second;
	int err = 0;
	Speaker* s = new Speaker();
	s->id = id;
	s->dec = opus_decoder_create(RATE, 1, &err);
	s->volume = 100;
	s->lastPacket = GetTickCount();
	char fallback[32];
	wsprintfA(fallback, "#%u", id);
	s->name = fallback;
	g_speakers[id] = s;
	return s;
}

static void reset_speaker(Speaker* s) {
	s->playing = false;
	s->misses = 0;
	s->firstQueued = 0;
	for (Slot& sl : s->slots)
		sl.has = false;
	opus_decoder_ctl(s->dec, OPUS_RESET_STATE);
}

void audio_on_packet(uint32_t speakerId, uint16_t seq, uint8_t channel, const uint8_t* data, int len) {
	if (len <= 0 || len > 1000)
		return;
	Lock l(g_spkLock);
	Speaker* s = get_speaker(speakerId);
	DWORD now = GetTickCount();
	s->lastPacket = now;
	s->channel = channel;
	if (s->muted)
		return;
	if (s->playing) {
		int16_t ahead = (int16_t)(seq - s->nextSeq);
		if (ahead < 0)
			return; // too late
		if (ahead >= JITTER_SLOTS) {
			reset_speaker(s); // fell far behind, restart from this packet
		}
	}
	if (!s->playing && !s->firstQueued) {
		s->firstQueued = now;
		s->nextSeq = seq;
	} else if (!s->playing && (int16_t)(seq - s->nextSeq) < 0) {
		s->nextSeq = seq; // an older packet arrived late, start from it
	}
	Slot& sl = s->slots[seq % JITTER_SLOTS];
	sl.has = true;
	sl.seq = seq;
	sl.len = len;
	memcpy(sl.data, data, len);
}

void audio_on_speaker_name(uint32_t speakerId, const char* name) {
	Lock l(g_spkLock);
	get_speaker(speakerId)->name = name;
}

std::vector<SpeakerInfo> audio_speakers() {
	std::vector<SpeakerInfo> out;
	Lock l(g_spkLock);
	DWORD now = GetTickCount();
	for (auto& it : g_speakers) {
		Speaker* s = it.second;
		SpeakerInfo i;
		i.charId = s->id;
		i.name = s->name;
		i.talking = now - s->lastPacket < 300;
		i.muted = s->muted;
		i.volume = s->volume;
		i.channel = s->channel;
		out.push_back(i);
	}
	return out;
}

void audio_set_speaker_muted(uint32_t id, bool muted) {
	Lock l(g_spkLock);
	auto it = g_speakers.find(id);
	if (it != g_speakers.end()) {
		it->second->muted = muted;
		reset_speaker(it->second);
	}
}

void audio_set_speaker_volume(uint32_t id, int volume) {
	Lock l(g_spkLock);
	auto it = g_speakers.find(id);
	if (it != g_speakers.end())
		it->second->volume = volume;
}

// Decode the next 20 ms of one speaker into pcm. Returns false when silent.
static bool speaker_frame(Speaker* s, DWORD now, opus_int16* pcm) {
	if (!s->playing) {
		if (!s->firstQueued || now - s->firstQueued < START_DELAY_MS)
			return false;
		s->playing = true;
	}
	Slot& cur = s->slots[s->nextSeq % JITTER_SLOTS];
	Slot& nxt = s->slots[(uint16_t)(s->nextSeq + 1) % JITTER_SLOTS];
	int n;
	if (cur.has && cur.seq == s->nextSeq) {
		n = opus_decode(s->dec, cur.data, cur.len, pcm, FRAME, 0);
		cur.has = false;
		s->misses = 0;
	} else if (nxt.has && nxt.seq == (uint16_t)(s->nextSeq + 1)) {
		n = opus_decode(s->dec, nxt.data, nxt.len, pcm, FRAME, 1); // recover from FEC
		s->misses++;
	} else {
		n = opus_decode(s->dec, nullptr, 0, pcm, FRAME, 0); // packet loss concealment
		s->misses++;
	}
	s->nextSeq++;
	if (s->misses > MAX_MISSES) {
		reset_speaker(s);
		return false;
	}
	return n == FRAME;
}

static void mix_frame(int16_t* out) {
	static int acc[FRAME];
	static opus_int16 pcm[FRAME];
	memset(acc, 0, sizeof(acc));
	DWORD now = GetTickCount();
	int master = g_cfg.deafen ? 0 : g_cfg.speakerVolume;
	{
		Lock l(g_spkLock);
		for (auto it = g_speakers.begin(); it != g_speakers.end();) {
			Speaker* s = it->second;
			if (now - s->lastPacket > 120000 && !s->playing) {
				opus_decoder_destroy(s->dec);
				delete s;
				it = g_speakers.erase(it);
				continue;
			}
			if (!s->muted && speaker_frame(s, now, pcm)) {
				int vol = s->volume * master; // percent * percent
				for (int i = 0; i < FRAME; i++)
					acc[i] += pcm[i] * vol / 10000;
			}
			++it;
		}
	}
	for (int i = 0; i < FRAME; i++)
		out[i] = (int16_t)(acc[i] > 32767 ? 32767 : acc[i] < -32768 ? -32768 : acc[i]);
}

// ---------------------------------------------------------------- devices

static volatile LONG g_reopenIn = 1, g_reopenOut = 1;
static volatile LONG g_ptt;
static volatile LONG g_sending;
static volatile LONG g_levelMilli; // mic level 0..1000

void audio_set_transmit(bool on) { InterlockedExchange(&g_ptt, on ? 1 : 0); }
bool audio_transmitting() { return g_sending != 0; }
float audio_mic_level() { return g_levelMilli / 1000.0f; }

void audio_restart_devices() {
	InterlockedExchange(&g_reopenIn, 1);
	InterlockedExchange(&g_reopenOut, 1);
}

std::vector<std::string> audio_input_devices() {
	std::vector<std::string> out;
	UINT n = waveInGetNumDevs();
	for (UINT i = 0; i < n; i++) {
		WAVEINCAPSA caps;
		out.push_back(waveInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? caps.szPname : "?");
	}
	return out;
}

std::vector<std::string> audio_output_devices() {
	std::vector<std::string> out;
	UINT n = waveOutGetNumDevs();
	for (UINT i = 0; i < n; i++) {
		WAVEOUTCAPSA caps;
		out.push_back(waveOutGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR ? caps.szPname : "?");
	}
	return out;
}

static WAVEFORMATEX wave_format() {
	WAVEFORMATEX f = {};
	f.wFormatTag = WAVE_FORMAT_PCM;
	f.nChannels = 1;
	f.nSamplesPerSec = RATE;
	f.wBitsPerSample = 16;
	f.nBlockAlign = 2;
	f.nAvgBytesPerSec = RATE * 2;
	return f;
}

static UINT device_id(int cfg) { return cfg < 0 ? WAVE_MAPPER : (UINT)cfg; }

// ---------------------------------------------------------------- capture

static DWORD WINAPI capture_thread(LPVOID) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	HWAVEIN in = nullptr;
	static int16_t bufs[IN_BUFFERS][FRAME];
	WAVEHDR hdr[IN_BUFFERS] = {};
	int err = 0;
	OpusEncoder* enc = opus_encoder_create(RATE, 1, OPUS_APPLICATION_VOIP, &err);
	opus_encoder_ctl(enc, OPUS_SET_BITRATE(24000));
	opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(5));
	opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
	opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(10));
	opus_encoder_ctl(enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	uint16_t seq = 0;
	float dcPrevIn = 0, dcPrevOut = 0, level = 0;
	DWORD gateUntil = 0;
	bool wasSending = false;
	uint8_t packet[1000];
	opus_int16 frame[FRAME];
	int next = 0; // buffers complete in the order they were queued

	for (;;) {
		if (InterlockedExchange(&g_reopenIn, 0)) {
			if (in) {
				waveInReset(in);
				for (int i = 0; i < IN_BUFFERS; i++)
					waveInUnprepareHeader(in, &hdr[i], sizeof(WAVEHDR));
				waveInClose(in);
				in = nullptr;
			}
			WAVEFORMATEX f = wave_format();
			if (waveInOpen(&in, device_id(g_cfg.inDevice), &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
				log_line("audio: cannot open microphone %d", g_cfg.inDevice);
				in = nullptr;
			} else {
				for (int i = 0; i < IN_BUFFERS; i++) {
					hdr[i] = WAVEHDR();
					hdr[i].lpData = (LPSTR)bufs[i];
					hdr[i].dwBufferLength = sizeof(bufs[i]);
					waveInPrepareHeader(in, &hdr[i], sizeof(WAVEHDR));
					waveInAddBuffer(in, &hdr[i], sizeof(WAVEHDR));
				}
				waveInStart(in);
				next = 0;
			}
		}
		if (!in) {
			InterlockedExchange(&g_levelMilli, 0);
			Sleep(1000);
			continue;
		}
		WaitForSingleObject(ev, 200);

		while (hdr[next].dwFlags & WHDR_DONE) {
			int i = next;
			next = (next + 1) % IN_BUFFERS;
			int samples = hdr[i].dwBytesRecorded / 2;
			float gain = g_cfg.micVolume / 100.0f;
			double sum = 0;
			for (int k = 0; k < FRAME; k++) {
				float x = k < samples ? bufs[i][k] * gain : 0.0f;
				// remove DC offset / rumble
				float y = x - dcPrevIn + 0.995f * dcPrevOut;
				dcPrevIn = x;
				dcPrevOut = y;
				if (y > 32767) y = 32767;
				if (y < -32768) y = -32768;
				frame[k] = (opus_int16)y;
				sum += (double)y * y;
			}
			float rms = (float)sqrt(sum / FRAME) / 32768.0f;
			float db = rms > 0 ? 20.0f * log10f(rms) : -100.0f;
			// meter: 0 at -60 dBFS, full at 0 dBFS
			float meter = (db + 60.0f) / 60.0f;
			meter = meter < 0 ? 0 : meter > 1 ? 1 : meter;
			level = meter > level ? meter : level * 0.85f + meter * 0.15f;
			InterlockedExchange(&g_levelMilli, (LONG)(level * 1000));

			DWORD now = GetTickCount();
			bool send;
			if (g_cfg.mode == MODE_PTT)
				send = g_ptt != 0;
			else if (!g_cfg.voiceDetect)
				send = true;
			else {
				if (db > -(float)g_cfg.sensitivity)
					gateUntil = now + 400;
				send = (int)(gateUntil - now) > 0;
			}
			if (!g_cfg.enabled || g_cfg.micMuted || net_state() != NET_CONNECTED)
				send = false;
			if (g_cfg.channel == CHANNEL_WHISPER && !g_cfg.whisper[0])
				send = false;

			if (send) {
				if (!wasSending)
					opus_encoder_ctl(enc, OPUS_RESET_STATE);
				int n = opus_encode(enc, frame, FRAME, packet, sizeof(packet));
				if (n > 0)
					net_send_audio(seq++, (uint8_t)g_cfg.channel, packet, n);
			}
			wasSending = send;
			InterlockedExchange(&g_sending, send ? 1 : 0);

			hdr[i].dwFlags &= ~WHDR_DONE;
			waveInAddBuffer(in, &hdr[i], sizeof(WAVEHDR));
		}
	}
}

// ---------------------------------------------------------------- playback

static DWORD WINAPI playback_thread(LPVOID) {
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
	HANDLE ev = CreateEvent(nullptr, FALSE, FALSE, nullptr);
	HWAVEOUT out = nullptr;
	static int16_t bufs[OUT_BUFFERS][FRAME];
	WAVEHDR hdr[OUT_BUFFERS] = {};
	int next = 0;

	for (;;) {
		if (InterlockedExchange(&g_reopenOut, 0)) {
			if (out) {
				waveOutReset(out);
				for (int i = 0; i < OUT_BUFFERS; i++)
					waveOutUnprepareHeader(out, &hdr[i], sizeof(WAVEHDR));
				waveOutClose(out);
				out = nullptr;
			}
			WAVEFORMATEX f = wave_format();
			if (waveOutOpen(&out, device_id(g_cfg.outDevice), &f, (DWORD_PTR)ev, 0, CALLBACK_EVENT) != MMSYSERR_NOERROR) {
				log_line("audio: cannot open speakers %d", g_cfg.outDevice);
				out = nullptr;
			} else {
				for (int i = 0; i < OUT_BUFFERS; i++) {
					hdr[i] = WAVEHDR();
					hdr[i].lpData = (LPSTR)bufs[i];
					hdr[i].dwBufferLength = sizeof(bufs[i]);
					waveOutPrepareHeader(out, &hdr[i], sizeof(WAVEHDR));
					memset(bufs[i], 0, sizeof(bufs[i]));
					waveOutWrite(out, &hdr[i], sizeof(WAVEHDR));
				}
				next = 0;
			}
		}
		if (!out) {
			Sleep(1000);
			continue;
		}
		WaitForSingleObject(ev, 200);
		while (hdr[next].dwFlags & WHDR_DONE) {
			int i = next;
			next = (next + 1) % OUT_BUFFERS;
			mix_frame(bufs[i]);
			hdr[i].dwFlags &= ~WHDR_DONE;
			waveOutWrite(out, &hdr[i], sizeof(WAVEHDR));
		}
	}
}

void audio_start() {
	InitializeCriticalSection(&g_spkLock);
	HANDLE t = CreateThread(nullptr, 0, capture_thread, nullptr, 0, nullptr);
	if (t)
		CloseHandle(t);
	t = CreateThread(nullptr, 0, playback_thread, nullptr, 0, nullptr);
	if (t)
		CloseHandle(t);
}
