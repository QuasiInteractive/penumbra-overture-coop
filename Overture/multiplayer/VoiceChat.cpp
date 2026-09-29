/*
 * v16 proximity voice chat — see VoiceChat.h for the contract with
 * cNetworkManager.
 *
 * Pipeline (all on the main thread, once per cNetworkManager::Update):
 *
 *   mic (alcCaptureSamples, 16 kHz mono16)
 *     -> pending PCM -> 20 ms frames -> Opus encode (24 kbps VBR, VOIP)
 *     -> 2 frames per cNetVoice packet -> outbox (the manager sends them
 *        unsequenced on ch1; the host relays guest->guest)
 *
 *   cNetVoice from player N
 *     -> seq check (stale/dup dropped, small gaps concealed with Opus PLC)
 *     -> decoder N -> decoded-frame queue N (jitter buffer: playback starts
 *        once 3 packets = 6 frames are in, or after a short wait for a
 *        short utterance)
 *     -> OpenAL streaming source N: processed buffers unqueued, new frames
 *        queued, restarted after an underrun, PLC frames generated when the
 *        queue is about to starve while the sender is still talking
 *     -> source placed at the ghost's head every frame; gain = inverse
 *        distance clamped (ref 2 m, max 25 m) * voice_volume, computed here
 *        because the engine runs the AL context with the distance model set
 *        to NONE (LowLevelSoundOpenAL.cpp: OAL_SetDistanceModel(None) — it
 *        attenuates its own sounds in software). AL still pans by position.
 *
 * OpenAL is used RAW (<AL/al.h>, <AL/alc.h>) against the context OALWrapper
 * created for the engine: alcGetCurrentContext() finds it, no wrapper
 * header is needed and nothing here depends on the wrapper's stream API.
 * OpenAL Soft (vcpkg openal-soft) is what ships, and its capture and
 * per-source calls are thread-safe against the wrapper's own update thread.
 *
 * Failure policy: any AL/opus/device failure is logged ONCE per session and
 * voice is switched off (mbFailed) — the game never sees an exception, a
 * crash or a log flood because of a missing microphone.
 */
#include "StdAfx.h"
#include "VoiceChat.h"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cmath>

#if defined(PENUMBRA_MULTIPLAYER) && defined(PENUMBRA_VOICE)
#include <AL/al.h>
#include <AL/alc.h>
#include <opus/opus.h>
#include <deque>
#include <map>
#endif

using namespace hpl;

//======================================================================
// Shared by the real and the stub build (ctor/dtor at the file tail, after
// Impl is complete in either build).
//======================================================================

void cVoiceChat::SetEnabled(bool abEnabled)
{
	mbEnabled = abEnabled;
}

void cVoiceChat::SetVolume(float afVolume)
{
	if (afVolume < 0.0f) afVolume = 0.0f;
	else if (afVolume > 2.0f) afVolume = 2.0f;
	mfVolume = afVolume;
}

void cVoiceChat::SetOpenMic(bool abOpenMic)
{
	mbOpenMic = abOpenMic;
}

#if !(defined(PENUMBRA_MULTIPLAYER) && defined(PENUMBRA_VOICE))

//======================================================================
// Stub build (no Opus / no PENUMBRA_MULTIPLAYER): voice is simply absent.
//======================================================================

struct cVoiceChat::Impl {};

bool cVoiceChat::IsCompiledIn() { return false; }
bool cVoiceChat::Init() { return false; }
void cVoiceChat::Shutdown() { mbInitialized = false; mbMicOpen = false; mvOutgoing.clear(); }
void cVoiceChat::Update(float, bool) {}
void cVoiceChat::SetRemoteHeadPos(uint8_t, const hpl::cVector3f &, bool) {}
void cVoiceChat::OnVoicePacket(uint8_t, const void *, size_t) {}
void cVoiceChat::DropPlayer(uint8_t) {}
void cVoiceChat::DropAllPlayers() {}
bool cVoiceChat::IsTalking(uint8_t) const { return false; }

#else /* PENUMBRA_MULTIPLAYER && PENUMBRA_VOICE */

//======================================================================
// Real build.
//======================================================================

namespace
{
const int kFrameSamples = kNetVoiceFrameSamples;          /* 320 = 20 ms */
const int kFrameBytes = kNetVoiceFrameSamples * 2;        /* mono16 */
const int kMaxFramesPerPacket = kNetVoiceMaxFramesPerPacket;
const int kBitrateBps = 24000;
const int kCaptureRingSamples = 4096;      /* alcCaptureOpenDevice buffer: 256 ms */
const int kQueueBuffers = 8;               /* AL buffers per stream: 160 ms */
const int kJitterFrames = 3 * kNetVoiceMaxFramesPerPacket; /* 3 packets before playback starts */
const float kPrimeWaitSeconds = 0.15f;     /* ...or this long, for utterances shorter than that */
const int kMaxPlcRun = 5;                  /* 100 ms of concealment, then the stream idles */
const int kMaxLostPacketsConcealed = 3;    /* bigger seq gap = treated as a new burst */
const int kMaxQueuedFrames = 25;           /* 500 ms decoded backlog per stream (receiver stalled) */
const float kTalkIndicatorSeconds = 0.35f; /* HUD "(talking)" hold after the last packet */
const float kBurstGapSeconds = 0.5f;       /* silence longer than this = new burst */
const int kMaxPendingCaptureSamples = kNetVoiceSampleRate; /* 1 s of mic backlog max */
const int kMaxFramesEncodedPerUpdate = 10; /* 200 ms per frame — bounds a stall catch-up */
const float kRefDistance = 2.0f;           /* AL_REFERENCE_DISTANCE */
const float kMaxDistance = 25.0f;          /* AL_MAX_DISTANCE */
const float kRolloff = 1.0f;
const float kOpenMicThresholdDbfs = -40.0f;
const float kOpenMicHoldSeconds = 0.4f;    /* gate stays open this long after the level drops */
const float kLoneFrameHoldSeconds = 0.03f; /* a single encoded frame waits this long for a 2nd */

/* alext.h constant (AL_EXT_source_distance_model); defined locally so the
   old dependencies/include AL headers work too. */
#ifndef AL_SOURCE_DISTANCE_MODEL
#define AL_SOURCE_DISTANCE_MODEL 0x200
#endif

/** One remote player's decoder + OpenAL streaming source. */
struct cVoiceStream
{
	OpusDecoder *mpDecoder;
	ALuint mlSource;
	ALuint mvBuffers[kQueueBuffers];
	bool mbBuffersOk;
	bool mbSourceOk;
	std::vector<ALuint> mvFreeBuffers;               /* generated, not queued */
	std::deque<std::vector<int16_t> > mFrames;       /* decoded, waiting for a buffer */
	bool mbPrimed;          /* jitter buffer filled for this burst: feeding */
	float mfPrimeWait;      /* seconds frames sat unprimed (short-utterance start) */
	bool mbHaveSeq;
	uint16_t mlLastSeq;
	float mfSinceLastPacket;
	int mlPlcRun;           /* consecutive concealment frames */
	float mfTalkTimer;
	cVector3f mvPos;
	bool mbHavePos;

	cVoiceStream()
		: mpDecoder(NULL)
		  , mlSource(0)
		  , mbBuffersOk(false)
		  , mbSourceOk(false)
		  , mvFreeBuffers()
		  , mFrames()
		  , mbPrimed(false)
		  , mfPrimeWait(0.0f)
		  , mbHaveSeq(false)
		  , mlLastSeq(0)
		  , mfSinceLastPacket(1000.0f)
		  , mlPlcRun(0)
		  , mfTalkTimer(0.0f)
		  , mvPos(0, 0, 0)
		  , mbHavePos(false)
	{
		for (int i = 0; i < kQueueBuffers; ++i)
			mvBuffers[i] = 0;
	}
};

/** RMS level of one frame in dBFS (-inf clamped to -100). */
float FrameLevelDbfs(const int16_t *apPcm, int alSamples)
{
	if (!apPcm || alSamples <= 0)
		return -100.0f;
	double acc = 0.0;
	for (int i = 0; i < alSamples; ++i)
	{
		const double v = (double)apPcm[i];
		acc += v * v;
	}
	const double rms = sqrt(acc / (double)alSamples);
	if (rms < 1.0)
		return -100.0f;
	return (float)(20.0 * log10(rms / 32768.0));
}
}

struct cVoiceChat::Impl
{
	OpusEncoder *mpEncoder;
	ALCdevice *mpCapture;
	bool mbCapturing;        /* alcCaptureStart issued */
	bool mbCaptureFailed;    /* open failed once — no retry until Shutdown/Init */
	bool mbSourceDistanceModelExt;
	bool mbAlErrorLogged;
	bool mbOpusErrorLogged;
	bool mbBacklogLogged;
	int mlStreamFailures;    /* streams torn down by AL errors; 3 = give up */
	std::vector<int16_t> mvPending;   /* captured PCM waiting to be encoded */
	std::vector<uint8_t> mvPacket;    /* packet under construction (header + frames) */
	int mlFramesInPacket;
	float mfPacketAge;       /* seconds since the packet's first frame */
	uint16_t mlSeqOut;
	float mfGateHold;        /* open mic: seconds the gate stays open */
	std::map<uint8_t, cVoiceStream *> m_mapStreams;
	unsigned char mvEncodeBuf[kNetVoiceMaxPayload];
	int16_t mvDecodeBuf[kNetVoiceFrameSamples];

	Impl()
		: mpEncoder(NULL)
		  , mpCapture(NULL)
		  , mbCapturing(false)
		  , mbCaptureFailed(false)
		  , mbSourceDistanceModelExt(false)
		  , mbAlErrorLogged(false)
		  , mbOpusErrorLogged(false)
		  , mbBacklogLogged(false)
		  , mlStreamFailures(0)
		  , mvPending()
		  , mvPacket()
		  , mlFramesInPacket(0)
		  , mfPacketAge(0.0f)
		  , mlSeqOut(0)
		  , mfGateHold(0.0f)
		  , m_mapStreams()
	{
		memset(mvEncodeBuf, 0, sizeof(mvEncodeBuf));
		memset(mvDecodeBuf, 0, sizeof(mvDecodeBuf));
	}

	/** alGetError after an operation. true = there WAS an error (logged
	    once per session with the call site). */
	bool CheckAl(const char *asWhere)
	{
		const ALenum e = alGetError();
		if (e == AL_NO_ERROR)
			return false;
		if (!mbAlErrorLogged)
		{
			Log(" voice: OpenAL error 0x%04X at %s — voice playback for that player is dropped\n",
				(unsigned)e, asWhere ? asWhere : "?");
			mbAlErrorLogged = true;
		}
		return true;
	}

	//---------------- capture ----------------

	bool EnsureCapture()
	{
		if (mpCapture == NULL)
		{
			if (mbCaptureFailed)
				return false;
			mpCapture = alcCaptureOpenDevice(NULL, (ALCuint)kNetVoiceSampleRate,
				AL_FORMAT_MONO16, (ALCsizei)kCaptureRingSamples);
			if (mpCapture == NULL)
			{
				Log(" voice: no microphone (alcCaptureOpenDevice failed) — push-to-talk is off for this session\n");
				mbCaptureFailed = true;
				return false;
			}
			Log(" voice: microphone opened (16 kHz mono)\n");
		}
		if (!mbCapturing)
		{
			alcCaptureStart(mpCapture);
			mbCapturing = true;
			mvPending.clear();
		}
		return true;
	}

	void StopCapture()
	{
		if (mpCapture && mbCapturing)
		{
			alcCaptureStop(mpCapture);
			/* drain what is left in the ring so the next press starts fresh */
			ALCint avail = 0;
			alcGetIntegerv(mpCapture, ALC_CAPTURE_SAMPLES, 1, &avail);
			while (avail > 0)
			{
				int16_t junk[512];
				ALCsizei n = (avail > 512) ? 512 : (ALCsizei)avail;
				alcCaptureSamples(mpCapture, junk, n);
				avail -= (ALCint)n;
			}
		}
		mbCapturing = false;
		mvPending.clear();
	}

	void CloseCapture()
	{
		StopCapture();
		if (mpCapture)
		{
			alcCaptureCloseDevice(mpCapture);
			mpCapture = NULL;
		}
	}

	/** Pull everything the device has into mvPending (bounded). */
	void PullCapture()
	{
		if (!mpCapture || !mbCapturing)
			return;
		ALCint avail = 0;
		alcGetIntegerv(mpCapture, ALC_CAPTURE_SAMPLES, 1, &avail);
		if (avail <= 0)
			return;
		const size_t old = mvPending.size();
		mvPending.resize(old + (size_t)avail);
		alcCaptureSamples(mpCapture, &mvPending[old], (ALCsizei)avail);
		/* A long frame stall (map load with the key held) must not turn
		   into seconds of delayed speech: keep the newest second only. */
		if (mvPending.size() > (size_t)kMaxPendingCaptureSamples)
		{
			const size_t drop = mvPending.size() - (size_t)kMaxPendingCaptureSamples;
			mvPending.erase(mvPending.begin(), mvPending.begin() + drop);
			if (!mbBacklogLogged)
			{
				Log(" voice: capture backlog > 1 s — oldest audio dropped (frame stall while talking)\n");
				mbBacklogLogged = true;
			}
		}
	}

	//---------------- packet builder ----------------

	void BeginPacket()
	{
		mvPacket.resize(sizeof(cNetVoice));
		mlFramesInPacket = 0;
		mfPacketAge = 0.0f;
	}

	void FlushPacket(uint8_t alLocalId, std::vector<std::vector<uint8_t> > &avOutbox)
	{
		if (mlFramesInPacket <= 0 || mvPacket.size() < sizeof(cNetVoice))
		{
			BeginPacket();
			return;
		}
		cNetVoice hdr;
		hdr.mType = eNetPacketType_Voice;
		hdr.mPlayerID = alLocalId;
		hdr.mSeq = ++mlSeqOut;
		hdr.mFrames = (uint8_t)mlFramesInPacket;
		memcpy(&mvPacket[0], &hdr, sizeof(hdr));
		avOutbox.push_back(mvPacket);
		BeginPacket();
	}

	/** Append one encoded frame (alLen may be 0 = "encoder produced
	    nothing", the receiver conceals it). Flushes first when full. */
	void AppendFrame(const unsigned char *apData, int alLen, uint8_t alLocalId,
		std::vector<std::vector<uint8_t> > &avOutbox)
	{
		if (alLen < 0)
			alLen = 0;
		if (mvPacket.size() < sizeof(cNetVoice))
			BeginPacket();
		const size_t payload = mvPacket.size() - sizeof(cNetVoice);
		if (mlFramesInPacket >= kMaxFramesPerPacket ||
			payload + 2 + (size_t)alLen > kNetVoiceMaxPayload)
			FlushPacket(alLocalId, avOutbox);
		const uint16_t len16 = (uint16_t)alLen;
		const size_t at = mvPacket.size();
		mvPacket.resize(at + 2 + (size_t)alLen);
		memcpy(&mvPacket[at], &len16, 2);
		if (alLen > 0 && apData)
			memcpy(&mvPacket[at + 2], apData, (size_t)alLen);
		++mlFramesInPacket;
		if (mlFramesInPacket >= kMaxFramesPerPacket)
			FlushPacket(alLocalId, avOutbox);
	}

	//---------------- streams ----------------

	void FreeStream(cVoiceStream *apStream)
	{
		if (!apStream)
			return;
		if (apStream->mbSourceOk)
		{
			alSourceStop(apStream->mlSource);
			alSourcei(apStream->mlSource, AL_BUFFER, 0); /* detaches every queued buffer */
			alDeleteSources(1, &apStream->mlSource);
			alGetError(); /* swallow: we are tearing down anyway */
		}
		if (apStream->mbBuffersOk)
		{
			alDeleteBuffers(kQueueBuffers, apStream->mvBuffers);
			alGetError();
		}
		if (apStream->mpDecoder)
			opus_decoder_destroy(apStream->mpDecoder);
		delete apStream;
	}

	void FreeAllStreams()
	{
		for (std::map<uint8_t, cVoiceStream *>::iterator it = m_mapStreams.begin();
			it != m_mapStreams.end(); ++it)
			FreeStream(it->second);
		m_mapStreams.clear();
	}

	void DropStream(uint8_t alId)
	{
		std::map<uint8_t, cVoiceStream *>::iterator it = m_mapStreams.find(alId);
		if (it == m_mapStreams.end())
			return;
		FreeStream(it->second);
		m_mapStreams.erase(it);
	}

	/** Decoder + source + buffers for a remote id; NULL when creation
	    failed (logged once; the packet is dropped, the next one retries). */
	cVoiceStream *GetOrCreateStream(uint8_t alId, float afVolume)
	{
		std::map<uint8_t, cVoiceStream *>::iterator it = m_mapStreams.find(alId);
		if (it != m_mapStreams.end())
			return it->second;

		cVoiceStream *s = new cVoiceStream();
		int err = 0;
		s->mpDecoder = opus_decoder_create(kNetVoiceSampleRate, 1, &err);
		if (s->mpDecoder == NULL || err != OPUS_OK)
		{
			if (!mbOpusErrorLogged)
			{
				Log(" voice: opus_decoder_create failed (%d) for player %u\n", err, (unsigned)alId);
				mbOpusErrorLogged = true;
			}
			s->mpDecoder = NULL;
			FreeStream(s);
			return NULL;
		}

		alGetError();
		alGenBuffers(kQueueBuffers, s->mvBuffers);
		if (CheckAl("alGenBuffers"))
		{
			FreeStream(s);
			return NULL;
		}
		s->mbBuffersOk = true;
		for (int i = 0; i < kQueueBuffers; ++i)
			s->mvFreeBuffers.push_back(s->mvBuffers[i]);

		alGenSources(1, &s->mlSource);
		if (CheckAl("alGenSources"))
		{
			FreeStream(s);
			return NULL;
		}
		s->mbSourceOk = true;
		alSourcei(s->mlSource, AL_SOURCE_RELATIVE, AL_FALSE);
		alSourcei(s->mlSource, AL_LOOPING, AL_FALSE);
		alSourcef(s->mlSource, AL_PITCH, 1.0f);
		alSourcef(s->mlSource, AL_GAIN, afVolume);
		alSourcef(s->mlSource, AL_REFERENCE_DISTANCE, kRefDistance);
		alSourcef(s->mlSource, AL_MAX_DISTANCE, kMaxDistance);
		alSourcef(s->mlSource, AL_ROLLOFF_FACTOR, kRolloff);
		alSource3f(s->mlSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
		alSource3f(s->mlSource, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
		/* The engine runs the context with the distance model NONE and
		   attenuates in software; we do the same (UpdateStreamAudio), so pin
		   this source to NONE where the extension exists — a future wrapper
		   change of the global model can then never double-attenuate. */
		if (mbSourceDistanceModelExt)
			alSourcei(s->mlSource, AL_SOURCE_DISTANCE_MODEL, AL_NONE);
		if (CheckAl("source setup"))
		{
			FreeStream(s);
			return NULL;
		}
		m_mapStreams[alId] = s;
		return s;
	}

	/** Decode one Opus frame (apData NULL / alLen 0 = PLC) onto the
	    stream's frame queue (bounded). */
	void PushDecoded(cVoiceStream *apStream, const unsigned char *apData, int alLen)
	{
		if (!apStream || !apStream->mpDecoder)
			return;
		const int n = opus_decode(apStream->mpDecoder, (alLen > 0) ? apData : NULL,
			(alLen > 0) ? alLen : 0, mvDecodeBuf, kFrameSamples, 0);
		if (n != kFrameSamples)
		{
			if (n < 0 && !mbOpusErrorLogged)
			{
				Log(" voice: opus_decode failed (%d) — frame skipped\n", n);
				mbOpusErrorLogged = true;
			}
			return;
		}
		if ((int)apStream->mFrames.size() >= kMaxQueuedFrames)
			apStream->mFrames.pop_front(); /* receiver stalled: keep the newest */
		apStream->mFrames.push_back(std::vector<int16_t>(mvDecodeBuf, mvDecodeBuf + kFrameSamples));
	}

	/** Per frame for one stream: reclaim, PLC, feed, restart, position,
	    gain. Returns false when an AL error tore the stream down. */
	bool UpdateStreamAudio(cVoiceStream *s, float afTimeStep, float afVolume)
	{
		if (!s || !s->mbSourceOk)
			return false;
		const ALuint src = s->mlSource;
		alGetError();

		/* 1. reclaim processed buffers */
		ALint processed = 0;
		alGetSourcei(src, AL_BUFFERS_PROCESSED, &processed);
		if (CheckAl("AL_BUFFERS_PROCESSED"))
			return false;
		while (processed > 0)
		{
			ALuint b = 0;
			alSourceUnqueueBuffers(src, 1, &b);
			if (CheckAl("alSourceUnqueueBuffers"))
				return false;
			s->mvFreeBuffers.push_back(b);
			--processed;
		}

		ALint queued = 0;
		ALint state = AL_INITIAL;
		alGetSourcei(src, AL_BUFFERS_QUEUED, &queued);
		alGetSourcei(src, AL_SOURCE_STATE, &state);
		if (CheckAl("alGetSourcei"))
			return false;

		const bool bSenderActive = s->mfSinceLastPacket < kBurstGapSeconds;

		/* 2. jitter buffer: start feeding once enough is in, or after a
		   short wait so a one-word utterance is not held back for ever */
		if (!s->mbPrimed && !s->mFrames.empty())
		{
			s->mfPrimeWait += afTimeStep;
			if ((int)s->mFrames.size() >= kJitterFrames || s->mfPrimeWait >= kPrimeWaitSeconds ||
				!bSenderActive)
				s->mbPrimed = true;
		}

		if (s->mbPrimed)
		{
			/* 3. about to starve while the sender is still talking: conceal */
			if (s->mFrames.empty() && bSenderActive && queued <= 1 && s->mlPlcRun < kMaxPlcRun)
			{
				PushDecoded(s, NULL, 0);
				++s->mlPlcRun;
			}

			/* 4. feed */
			while (!s->mFrames.empty() && !s->mvFreeBuffers.empty())
			{
				const ALuint b = s->mvFreeBuffers.back();
				const std::vector<int16_t> &pcm = s->mFrames.front();
				alBufferData(b, AL_FORMAT_MONO16, &pcm[0], (ALsizei)(pcm.size() * sizeof(int16_t)),
					(ALsizei)kNetVoiceSampleRate);
				if (CheckAl("alBufferData"))
					return false;
				alSourceQueueBuffers(src, 1, &b);
				if (CheckAl("alSourceQueueBuffers"))
					return false;
				s->mvFreeBuffers.pop_back();
				s->mFrames.pop_front();
				++queued;
			}

			/* 5. (re)start after an underrun, or at burst start */
			if (state != AL_PLAYING && queued > 0)
			{
				alSourcePlay(src);
				if (CheckAl("alSourcePlay"))
					return false;
			}
			else if (state != AL_PLAYING && queued == 0 && s->mFrames.empty())
			{
				/* burst over (or starved beyond PLC): the next packet primes again */
				s->mbPrimed = false;
				s->mfPrimeWait = 0.0f;
				s->mlPlcRun = 0;
			}
		}

		/* 6. position + proximity gain (software inverse-distance clamped) */
		if (s->mbHavePos)
			alSource3f(src, AL_POSITION, s->mvPos.x, s->mvPos.y, s->mvPos.z);
		ALfloat lx = 0, ly = 0, lz = 0;
		alGetListener3f(AL_POSITION, &lx, &ly, &lz);
		float fGain = afVolume;
		if (s->mbHavePos)
		{
			const float dx = s->mvPos.x - lx, dy = s->mvPos.y - ly, dz = s->mvPos.z - lz;
			float d = sqrtf(dx * dx + dy * dy + dz * dz);
			if (d < kRefDistance) d = kRefDistance;
			else if (d > kMaxDistance) d = kMaxDistance;
			fGain *= kRefDistance / (kRefDistance + kRolloff * (d - kRefDistance));
		}
		alSourcef(src, AL_GAIN, fGain);
		if (CheckAl("position/gain"))
			return false;
		return true;
	}
};

//-----------------------------------------------------------------------

bool cVoiceChat::IsCompiledIn()
{
	return true;
}

bool cVoiceChat::Init()
{
	if (mbInitialized)
		return true;
	if (mbFailed || !mpImpl || !mbEnabled)
		return false;

	if (alcGetCurrentContext() == NULL)
	{
		Log(" voice: no OpenAL context (sound disabled?) — voice chat off\n");
		mbFailed = true;
		return false;
	}
	alGetError(); /* start clean */
	mpImpl->mbSourceDistanceModelExt =
		(alIsExtensionPresent("AL_EXT_source_distance_model") == AL_TRUE);
	alGetError();

	int err = 0;
	mpImpl->mpEncoder = opus_encoder_create(kNetVoiceSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
	if (mpImpl->mpEncoder == NULL || err != OPUS_OK)
	{
		Log(" voice: opus_encoder_create failed (%d) — voice chat off\n", err);
		mpImpl->mpEncoder = NULL;
		mbFailed = true;
		return false;
	}
	opus_encoder_ctl(mpImpl->mpEncoder, OPUS_SET_BITRATE(kBitrateBps));
	opus_encoder_ctl(mpImpl->mpEncoder, OPUS_SET_VBR(1));
	opus_encoder_ctl(mpImpl->mpEncoder, OPUS_SET_COMPLEXITY(5));
	opus_encoder_ctl(mpImpl->mpEncoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
	opus_encoder_ctl(mpImpl->mpEncoder, OPUS_SET_MAX_BANDWIDTH(OPUS_BANDWIDTH_WIDEBAND));
	opus_encoder_ctl(mpImpl->mpEncoder, OPUS_SET_INBAND_FEC(0));
	opus_encoder_ctl(mpImpl->mpEncoder, OPUS_SET_DTX(0));

	mpImpl->BeginPacket();
	mpImpl->mlSeqOut = 0;
	mbInitialized = true;
	Log(" voice: ready (%s, 16 kHz mono, 20 ms frames, %d kbps VBR, %s, volume %.2f, source distance model ext %s)\n",
		opus_get_version_string(), kBitrateBps / 1000,
		mbOpenMic ? "open mic (-40 dBFS gate)" : "push-to-talk V",
		mfVolume, mpImpl->mbSourceDistanceModelExt ? "yes" : "no");
	return true;
}

void cVoiceChat::Shutdown()
{
	if (mpImpl)
	{
		mpImpl->CloseCapture();
		if (mpImpl->mpEncoder)
		{
			opus_encoder_destroy(mpImpl->mpEncoder);
			mpImpl->mpEncoder = NULL;
		}
		/* sources/buffers need the context — alive here, see the header */
		mpImpl->FreeAllStreams();
		mpImpl->mvPending.clear();
		mpImpl->mvPacket.clear();
		mpImpl->mlFramesInPacket = 0;
		mpImpl->mfPacketAge = 0.0f;
		mpImpl->mfGateHold = 0.0f;
		mpImpl->mbCaptureFailed = false;
		mpImpl->mbAlErrorLogged = false;
		mpImpl->mbOpusErrorLogged = false;
		mpImpl->mbBacklogLogged = false;
		mpImpl->mlStreamFailures = 0;
	}
	mvOutgoing.clear();
	mbInitialized = false;
	mbFailed = false; /* a new session may try again (device plugged in) */
	mbMicOpen = false;
}

//-----------------------------------------------------------------------

void cVoiceChat::Update(float afTimeStep, bool abTalkHeld)
{
	if (!mpImpl || !mbInitialized || mbFailed)
	{
		mbMicOpen = false;
		return;
	}
	if (afTimeStep < 0.0f) afTimeStep = 0.0f;
	else if (afTimeStep > 1.0f) afTimeStep = 1.0f;

	//------------------ capture -> encode -> outbox ------------------
	const bool bWantMic = mbEnabled && mlLocalPlayerId != 0 && (mbOpenMic || abTalkHeld);
	bool bMicOpen = false;
	if (bWantMic && mpImpl->mpEncoder && mpImpl->EnsureCapture())
	{
		mpImpl->PullCapture();
		std::vector<int16_t> &pend = mpImpl->mvPending;
		size_t consumed = 0;
		int encoded = 0;
		bool bGateOpen = !mbOpenMic; /* PTT: always open while held */
		if (mbOpenMic)
		{
			mpImpl->mfGateHold -= afTimeStep;
			if (mpImpl->mfGateHold < 0.0f) mpImpl->mfGateHold = 0.0f;
		}
		while (pend.size() - consumed >= (size_t)kFrameSamples && encoded < kMaxFramesEncodedPerUpdate)
		{
			const int16_t *frame = &pend[consumed];
			consumed += (size_t)kFrameSamples;
			++encoded;
			if (mbOpenMic)
			{
				/* energy gate: open on speech level, hold briefly after */
				if (FrameLevelDbfs(frame, kFrameSamples) > kOpenMicThresholdDbfs)
					mpImpl->mfGateHold = kOpenMicHoldSeconds;
				if (mpImpl->mfGateHold <= 0.0f)
					continue; /* silence: nothing on the wire */
				bGateOpen = true;
			}
			const int n = opus_encode(mpImpl->mpEncoder, frame, kFrameSamples, mpImpl->mvEncodeBuf,
				(opus_int32)(kNetVoiceMaxPayload / kMaxFramesPerPacket - 2));
			if (n < 0)
			{
				if (!mpImpl->mbOpusErrorLogged)
				{
					Log(" voice: opus_encode failed (%d) — frame skipped\n", n);
					mpImpl->mbOpusErrorLogged = true;
				}
				continue;
			}
			/* n <= 2 is an Opus "silence / DTX" frame: still sent so the far
			   end's PLC state stays sane */
			mpImpl->AppendFrame(mpImpl->mvEncodeBuf, n, mlLocalPlayerId, mvOutgoing);
		}
		if (consumed > 0)
			pend.erase(pend.begin(), pend.begin() + (std::ptrdiff_t)consumed);
		/* A lone frame waits at most ~30 ms for its twin (the mic delivers
		   one every 20 ms), then goes out alone: a half packet only costs a
		   header, a held frame costs latency. */
		if (mpImpl->mlFramesInPacket > 0)
		{
			mpImpl->mfPacketAge += afTimeStep;
			if (mpImpl->mfPacketAge >= kLoneFrameHoldSeconds)
				mpImpl->FlushPacket(mlLocalPlayerId, mvOutgoing);
		}
		if (mbOpenMic && mpImpl->mfGateHold > 0.0f)
			bGateOpen = true;
		bMicOpen = bGateOpen;
	}
	else
	{
		if (mpImpl->mbCapturing)
		{
			/* key released: the last partial packet goes out now */
			mpImpl->FlushPacket(mlLocalPlayerId, mvOutgoing);
			mpImpl->StopCapture();
		}
		mpImpl->mfGateHold = 0.0f;
	}
	mbMicOpen = bMicOpen;

	//------------------ inbox -> AL sources ------------------
	if (!mbEnabled)
		return;
	std::vector<uint8_t> vDead;
	for (std::map<uint8_t, cVoiceStream *>::iterator it = mpImpl->m_mapStreams.begin();
		it != mpImpl->m_mapStreams.end(); ++it)
	{
		cVoiceStream *s = it->second;
		if (!s)
			continue;
		s->mfSinceLastPacket += afTimeStep;
		if (s->mfTalkTimer > 0.0f)
			s->mfTalkTimer -= afTimeStep;
		if (!mpImpl->UpdateStreamAudio(s, afTimeStep, mfVolume))
			vDead.push_back(it->first);
	}
	for (size_t i = 0; i < vDead.size(); ++i)
	{
		mpImpl->DropStream(vDead[i]);
		if (++mpImpl->mlStreamFailures >= 3)
		{
			Log(" voice: repeated OpenAL failures — voice playback off for this session\n");
			mpImpl->CloseCapture();
			mpImpl->FreeAllStreams();
			mbFailed = true;
			mbMicOpen = false;
			return;
		}
	}
}

//-----------------------------------------------------------------------

void cVoiceChat::SetRemoteHeadPos(uint8_t alId, const hpl::cVector3f &avPos, bool abKnown)
{
	if (!mpImpl || !abKnown)
		return;
	std::map<uint8_t, cVoiceStream *>::iterator it = mpImpl->m_mapStreams.find(alId);
	if (it == mpImpl->m_mapStreams.end() || !it->second)
		return;
	it->second->mvPos = avPos;
	it->second->mbHavePos = true;
}

//-----------------------------------------------------------------------

void cVoiceChat::OnVoicePacket(uint8_t alFromId, const void *apData, size_t alLen)
{
	if (!mpImpl || !mbInitialized || mbFailed || !mbEnabled)
		return;
	if (!apData || alLen < sizeof(cNetVoice) || alLen > sizeof(cNetVoice) + kNetVoiceMaxPayload)
		return;
	if (alFromId == 0 || alFromId == mlLocalPlayerId)
		return;

	cNetVoice hdr;
	memcpy(&hdr, apData, sizeof(hdr));
	if (hdr.mType != eNetPacketType_Voice || hdr.mFrames == 0 || hdr.mFrames > kMaxFramesPerPacket)
		return;

	/* validate the frame table before touching the decoder (wire = untrusted) */
	const unsigned char *p = (const unsigned char *)apData + sizeof(cNetVoice);
	const unsigned char *end = (const unsigned char *)apData + alLen;
	{
		const unsigned char *q = p;
		for (int f = 0; f < (int)hdr.mFrames; ++f)
		{
			if (end - q < 2)
				return;
			uint16_t l = 0;
			memcpy(&l, q, 2);
			q += 2;
			if (end - q < (std::ptrdiff_t)l)
				return;
			q += l;
		}
	}

	cVoiceStream *s = mpImpl->GetOrCreateStream(alFromId, mfVolume);
	if (!s)
		return;

	/* sequence: stale/duplicate dropped, small gaps concealed, big gaps
	   or a fresh burst reset the decoder */
	const bool bContinuing = s->mbHaveSeq && s->mfSinceLastPacket < kBurstGapSeconds;
	if (bContinuing)
	{
		const int16_t diff = (int16_t)(hdr.mSeq - s->mlLastSeq);
		if (diff <= 0)
			return; /* reordered late packet: already concealed / played */
		if (diff > 1)
		{
			const int lost = diff - 1;
			if (lost <= kMaxLostPacketsConcealed)
			{
				for (int i = 0; i < lost * (int)hdr.mFrames; ++i)
					mpImpl->PushDecoded(s, NULL, 0);
			}
			else
				opus_decoder_ctl(s->mpDecoder, OPUS_RESET_STATE);
		}
	}
	else
	{
		/* new burst (first packet, or after silence): clean decoder, fresh
		   jitter buffer */
		opus_decoder_ctl(s->mpDecoder, OPUS_RESET_STATE);
		s->mFrames.clear();
		s->mbPrimed = false;
		s->mfPrimeWait = 0.0f;
	}
	s->mlLastSeq = hdr.mSeq;
	s->mbHaveSeq = true;
	s->mfSinceLastPacket = 0.0f;
	s->mfTalkTimer = kTalkIndicatorSeconds;
	s->mlPlcRun = 0;

	for (int f = 0; f < (int)hdr.mFrames; ++f)
	{
		uint16_t l = 0;
		memcpy(&l, p, 2);
		p += 2;
		mpImpl->PushDecoded(s, (l > 0) ? p : NULL, (int)l);
		p += l;
	}
}

//-----------------------------------------------------------------------

void cVoiceChat::DropPlayer(uint8_t alId)
{
	if (mpImpl)
		mpImpl->DropStream(alId);
}

void cVoiceChat::DropAllPlayers()
{
	if (mpImpl)
		mpImpl->FreeAllStreams();
}

bool cVoiceChat::IsTalking(uint8_t alId) const
{
	if (alId != 0 && alId == mlLocalPlayerId)
		return mbMicOpen;
	if (!mpImpl)
		return false;
	std::map<uint8_t, cVoiceStream *>::const_iterator it = mpImpl->m_mapStreams.find(alId);
	if (it == mpImpl->m_mapStreams.end() || !it->second)
		return false;
	return it->second->mfTalkTimer > 0.0f;
}

#endif /* PENUMBRA_MULTIPLAYER && PENUMBRA_VOICE */

//======================================================================
// Shared by the real and the stub build.
//======================================================================

cVoiceChat::cVoiceChat()
	: mbEnabled(true)
	  , mbOpenMic(false)
	  , mfVolume(1.0f)
	  , mbInitialized(false)
	  , mbFailed(false)
	  , mbMicOpen(false)
	  , mlLocalPlayerId(0)
	  , mvOutgoing()
	  , mpImpl(NULL)
{
#if defined(PENUMBRA_MULTIPLAYER) && defined(PENUMBRA_VOICE)
	mpImpl = new Impl();
#endif
}

cVoiceChat::~cVoiceChat()
{
	Shutdown();
	delete mpImpl; /* NULL in the stub build */
	mpImpl = NULL;
}
