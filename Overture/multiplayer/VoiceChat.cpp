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
 *        Our sources also get AL_ROLLOFF_FACTOR 0, which makes EVERY AL
 *        distance model a no-op for them, so a future global-model change can
 *        never double-attenuate. (The first version tried to pin the model
 *        per source with alSourcei(src, AL_SOURCE_DISTANCE_MODEL = 0x200,
 *        AL_NONE): 0x200 is the alEnable() capability, not a source property
 *        — OpenAL Soft answers AL_INVALID_ENUM, the "source setup" check
 *        failed and EVERY remote stream was torn down at creation: total
 *        silence on the receiving side.) Until the ghost's head position is
 *        known (or when it goes stale: ghost in another map) the source is
 *        listener-relative at the origin — centered, at voice_volume.
 *
 * OpenAL is used RAW (<AL/al.h>, <AL/alc.h>) against the context OALWrapper
 * created for the engine: alcGetCurrentContext() finds it, no wrapper
 * header is needed and nothing here depends on the wrapper's stream API.
 * OpenAL Soft (vcpkg openal-soft) is what ships, and its capture and
 * per-source calls are thread-safe against the wrapper's own update thread.
 * The error slot (alGetError) is per CONTEXT, though, and the wrapper's
 * thread reads/writes it too: an error we see may be the wrapper's. So gen
 * failures are judged by the returned names (alIsSource / alIsBuffer), and
 * a per-frame error only tears a stream down when the source is really gone
 * or the errors persist for kMaxErrorFrames consecutive frames.
 *
 * Failure policy: every failure is logged ONCE per session. No AL context /
 * no Opus encoder = voice off (mbFailed). A missing microphone only stops
 * SENDING (retried every few seconds while the key is held); repeated
 * playback failures only stop PLAYBACK — neither takes the other half down.
 * The game never sees an exception, a crash or a log flood.
 *
 * Diagnostics (" voice:" lines in hpl.log, each once): context/device and
 * source budget at Init, the capture device list, the device opened (or
 * every ALC error), capture started, first samples (or "no samples"), first
 * packet queued for the wire, the first few sent bursts with the mic level
 * (pure digital silence = privacy switch), first packet from each player,
 * stream created / playback started / first underrun restart, the first few
 * received bursts with distance and gain, and a per-stream summary when it
 * closes.
 */
#include "StdAfx.h"
#include "VoiceChat.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
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

void cVoiceChat::SetOpenMicThresholdDb(float afDbfs)
{
	if (!(afDbfs == afDbfs)) afDbfs = -45.0f; /* NaN from a bad cfg value */
	if (afDbfs < -70.0f) afDbfs = -70.0f;
	else if (afDbfs > -10.0f) afDbfs = -10.0f;
	mfGateDbfs = afDbfs;
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
const char *cVoiceChat::GetStatusHint() const { return NULL; }
void cVoiceChat::SetCaptureDevice(const tString &asName) { msCaptureDevice = asName; }
void cVoiceChat::GetCaptureDeviceNames(std::vector<tString> &avOut) { avOut.clear(); }
void cVoiceChat::UpdateMicTest(float, bool) {}
float cVoiceChat::GetMicTestLevel() const { return 0.0f; }
int cVoiceChat::GetMicTestState() const { return 0; }
tString cVoiceChat::GetMicTestDeviceName() const { return tString(""); }

#else /* PENUMBRA_MULTIPLAYER && PENUMBRA_VOICE */

//======================================================================
// Real build.
//======================================================================

namespace
{
const int kFrameSamples = kNetVoiceFrameSamples;          /* 320 = 20 ms */
const int kMaxFramesPerPacket = kNetVoiceMaxFramesPerPacket;
const int kBitrateBps = 24000;
const int kCaptureRingSamples = 4096;      /* alcCaptureOpenDevice buffer: 256 ms */
const int kCaptureRingSamplesBig = 16000;  /* fallback: 1 s (backends with a big period) */
const float kCaptureRetrySeconds = 5.0f;   /* no mic: retry this often while the key is held */
const float kNoSamplesWarnSeconds = 1.0f;  /* capture running this long with 0 samples = blocked */
const int kQueueBuffers = 8;               /* AL buffers per stream: 160 ms */
const int kJitterFrames = 3 * kNetVoiceMaxFramesPerPacket; /* 3 packets before playback starts */
const float kPrimeWaitSeconds = 0.15f;     /* ...or this long, for utterances shorter than that */
const int kMaxPlcRun = 5;                  /* 100 ms of concealment, then the stream idles */
const int kMaxLostPacketsConcealed = 3;    /* bigger seq gap = treated as a new burst */
const int kMaxQueuedFrames = 25;           /* 500 ms decoded backlog per stream (receiver stalled) */
const float kTalkIndicatorSeconds = 0.35f; /* HUD "(talking)" hold after the last packet */
const float kBurstGapSeconds = 0.5f;       /* silence longer than this = new burst */
const float kBurstSummaryAfterSeconds = 2.0f; /* receive summary even if the source never stopped */
const int kMaxPendingCaptureSamples = kNetVoiceSampleRate; /* 1 s of mic backlog max */
const int kMaxFramesEncodedPerUpdate = 10; /* 200 ms per frame — bounds a stall catch-up */
const float kRefDistance = 2.0f;           /* software inverse-distance reference */
const float kMaxDistance = 25.0f;          /* ...and clamp */
const float kRolloff = 1.0f;
const float kOpenMicHoldSeconds = 0.4f;    /* gate stays open this long after the level drops */
const float kOpenMicQuietWarnSeconds = 3.0f; /* open mic: nothing above the gate this long = log */
const float kLoneFrameHoldSeconds = 0.03f; /* a single encoded frame waits this long for a 2nd */
const float kPosStaleSeconds = 0.5f;       /* no head position this long = centered playback */
const int kMaxErrorFrames = 10;            /* consecutive AL-error frames before a stream is rebuilt */
const int kMaxStreamFailures = 3;          /* rebuilt streams per session before playback is off */
const float kCreateRetrySeconds = 2.0f;    /* stream creation failed: next try for that player */
const int kBurstsLogged = 3;               /* sent / received bursts summarised in hpl.log */
const int kSilentBurstFrames = 40;         /* 0.8 s of pure zeros = the mic is blocked */

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
	cVector3f mvPos;
	bool mbHavePos;
	float mfSincePos;       /* seconds since SetRemoteHeadPos */
	bool mbRelative;        /* AL_SOURCE_RELATIVE currently set (centered) */
	bool mbStarted;         /* alSourcePlay issued at least once */
	bool mbPlayingBurst;    /* played in this burst: a later stop = underrun */
	bool mbLoggedUnderrun;
	bool mbStarved;         /* stopped empty while the sender was active */
	int mlErrorFrames;      /* consecutive frames with an AL error */
	/* diagnostics */
	bool mbBurstOpen;
	int mlBurstsLogged;
	int mlPackets, mlFramesPlayed, mlPlc, mlUnderruns, mlLost;
	int mlBurstPackets, mlBurstFrames, mlBurstPlc, mlBurstUnderruns;
	float mfLastGain, mfLastDist;

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
		  , mvPos(0, 0, 0)
		  , mbHavePos(false)
		  , mfSincePos(1000.0f)
		  , mbRelative(true)
		  , mbStarted(false)
		  , mbPlayingBurst(false)
		  , mbLoggedUnderrun(false)
		  , mbStarved(false)
		  , mlErrorFrames(0)
		  , mbBurstOpen(false)
		  , mlBurstsLogged(0)
		  , mlPackets(0), mlFramesPlayed(0), mlPlc(0), mlUnderruns(0), mlLost(0)
		  , mlBurstPackets(0), mlBurstFrames(0), mlBurstPlc(0), mlBurstUnderruns(0)
		  , mfLastGain(0.0f), mfLastDist(-1.0f)
	{
		for (int i = 0; i < kQueueBuffers; ++i)
			mvBuffers[i] = 0;
	}

	bool OwnsBuffer(ALuint alBuffer) const
	{
		if (alBuffer == 0)
			return false;
		for (int i = 0; i < kQueueBuffers; ++i)
			if (mvBuffers[i] == alBuffer)
				return true;
		return false;
	}
};

/** Per remote player, independent of the AL stream (which may be missing:
    creation failed, playback off) — the HUD "(talking)" indicator and the
    first-packet log must not depend on playback working. */
struct cPeerRx
{
	float mfTalk;
	float mfRetry;      /* stream creation back-off */
	int mlPackets;
	bool mbFirstLogged;
	cPeerRx() : mfTalk(0.0f), mfRetry(0.0f), mlPackets(0), mbFirstLogged(false) {}
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

int FramePeakAbs(const int16_t *apPcm, int alSamples)
{
	int peak = 0;
	for (int i = 0; apPcm && i < alSamples; ++i)
	{
		const int v = abs((int)apPcm[i]);
		if (v > peak)
			peak = v;
	}
	return peak;
}

float PeakDbfs(int alPeak)
{
	if (alPeak <= 0)
		return -100.0f;
	return (float)(20.0 * log10((double)alPeak / 32768.0));
}
}

struct cVoiceChat::Impl
{
	OpusEncoder *mpEncoder;
	ALCdevice *mpCapture;
	int mlCaptureRing;
	bool mbCapturing;        /* alcCaptureStart issued */
	bool mbCaptureFailed;    /* no device opened: retried every kCaptureRetrySeconds */
	float mfCaptureRetry;
	bool mbCaptureListLogged;
	bool mbCaptureStartLogged;
	bool mbAlErrorLogged;
	bool mbOpusErrorLogged;
	bool mbBacklogLogged;
	bool mbCreateFailLogged;
	bool mbPlaybackOff;      /* repeated AL failures: we still SEND */
	int mlStreamFailures;    /* streams torn down by AL errors */
	std::vector<int16_t> mvPending;   /* captured PCM waiting to be encoded */
	std::vector<uint8_t> mvPacket;    /* packet under construction (header + frames) */
	int mlFramesInPacket;
	float mfPacketAge;       /* seconds since the packet's first frame */
	uint16_t mlSeqOut;
	float mfGateHold;        /* open mic: seconds the gate stays open */
	std::map<uint8_t, cVoiceStream *> m_mapStreams;
	std::map<uint8_t, cPeerRx> m_mapPeers;
	unsigned char mvEncodeBuf[kNetVoiceMaxPayload];
	int16_t mvDecodeBuf[kNetVoiceFrameSamples];

	/* send-side diagnostics */
	float mfCaptureRunTime;
	bool mbFirstSamplesLogged;
	bool mbNoSamplesLogged;
	bool mbFirstPacketOutLogged;
	bool mbOpenMicQuietLogged;
	float mfOpenMicRunTime;
	float mfLoudestDbfs;     /* loudest frame since the capture started (gate tuning) */
	int mlPacketsOut;
	int mlBurstsOut;
	bool mbTxBurst;
	int mlTxBurstFrames;
	int mlTxBurstPackets;
	int mlTxBurstPeak;
	float mfTxBurstMaxDb;
	float mfTxBurstSeconds;
	int mlTxBurstsLogged;
	bool mbMicSilent;        /* HUD hint: the device delivers nothing / pure zeros */
	bool mbSilentLogged;
	bool mbTalkKeyLogged;

	Impl()
		: mpEncoder(NULL)
		  , mpCapture(NULL)
		  , mlCaptureRing(kCaptureRingSamples)
		  , mbCapturing(false)
		  , mbCaptureFailed(false)
		  , mfCaptureRetry(0.0f)
		  , mbCaptureListLogged(false)
		  , mbCaptureStartLogged(false)
		  , mbAlErrorLogged(false)
		  , mbOpusErrorLogged(false)
		  , mbBacklogLogged(false)
		  , mbCreateFailLogged(false)
		  , mbPlaybackOff(false)
		  , mlStreamFailures(0)
		  , mvPending()
		  , mvPacket()
		  , mlFramesInPacket(0)
		  , mfPacketAge(0.0f)
		  , mlSeqOut(0)
		  , mfGateHold(0.0f)
		  , m_mapStreams()
		  , m_mapPeers()
		  , mfCaptureRunTime(0.0f)
		  , mbFirstSamplesLogged(false)
		  , mbNoSamplesLogged(false)
		  , mbFirstPacketOutLogged(false)
		  , mbOpenMicQuietLogged(false)
		  , mfOpenMicRunTime(0.0f)
		  , mfLoudestDbfs(-100.0f)
		  , mlPacketsOut(0)
		  , mlBurstsOut(0)
		  , mbTxBurst(false)
		  , mlTxBurstFrames(0)
		  , mlTxBurstPackets(0)
		  , mlTxBurstPeak(0)
		  , mfTxBurstMaxDb(-100.0f)
		  , mfTxBurstSeconds(0.0f)
		  , mlTxBurstsLogged(0)
		  , mbMicSilent(false)
		  , mbSilentLogged(false)
		  , mpTestCapture(NULL)
		  , mfTestLevel(0.0f)
		  , mfTestRetry(0.0f)
		  , mbTestFailed(false)
		  , mbTalkKeyLogged(false)
	{
		memset(mvEncodeBuf, 0, sizeof(mvEncodeBuf));
		memset(mvDecodeBuf, 0, sizeof(mvDecodeBuf));
	}

	/** Reset everything that is per session (Shutdown). */
	void ResetSession()
	{
		mvPending.clear();
		mvPacket.clear();
		mlFramesInPacket = 0;
		mfPacketAge = 0.0f;
		mfGateHold = 0.0f;
		mbCaptureFailed = false;
		mfCaptureRetry = 0.0f;
		mbCaptureListLogged = false;
		mbCaptureStartLogged = false;
		mbAlErrorLogged = false;
		mbOpusErrorLogged = false;
		mbBacklogLogged = false;
		mbCreateFailLogged = false;
		mbPlaybackOff = false;
		mlStreamFailures = 0;
		mfCaptureRunTime = 0.0f;
		mbFirstSamplesLogged = false;
		mbNoSamplesLogged = false;
		mbFirstPacketOutLogged = false;
		mbOpenMicQuietLogged = false;
		mfOpenMicRunTime = 0.0f;
		mfLoudestDbfs = -100.0f;
		mlPacketsOut = 0;
		mlBurstsOut = 0;
		mbTxBurst = false;
		mlTxBurstsLogged = 0;
		mbMicSilent = false;
		mbSilentLogged = false;
		mbTalkKeyLogged = false;
		m_mapPeers.clear();
	}

	//---------------- capture ----------------

	ALCdevice *TryOpenCapture(const char *asName, int alRing, bool abQuiet)
	{
		alcGetError(NULL);
		ALCdevice *d = alcCaptureOpenDevice(asName, (ALCuint)kNetVoiceSampleRate,
			AL_FORMAT_MONO16, (ALCsizei)alRing);
		if (d == NULL && !abQuiet)
		{
			const ALCenum e = alcGetError(NULL);
			Log(" voice: alcCaptureOpenDevice(\"%s\", 16000 Hz, mono16, %d samples) failed - ALC error 0x%04X\n",
				asName ? asName : "<system default>", alRing, (unsigned)e);
		}
		if (d)
			mlCaptureRing = alRing;
		return d;
	}

	/** Once per session: what OpenAL can record from. */
	void LogCaptureDevices()
	{
		if (mbCaptureListLogged)
			return;
		mbCaptureListLogged = true;
		const ALCchar *def = alcGetString(NULL, ALC_CAPTURE_DEFAULT_DEVICE_SPECIFIER);
		Log(" voice: default capture device: \"%s\"\n", (def && *def) ? def : "(none)");
		const ALCchar *list = alcGetString(NULL, ALC_CAPTURE_DEVICE_SPECIFIER);
		int n = 0;
		for (const ALCchar *p = list; p && *p && n < 16; p += strlen(p) + 1, ++n)
			Log(" voice: capture device: \"%s\"\n", p);
		if (n == 0)
			Log(" voice: OpenAL lists NO capture device (no microphone, or every recording device is disabled in Windows Sound settings)\n");
	}

	/** Open (lazily, with fallbacks) and start the microphone. false =
	    no microphone right now (logged; retried after kCaptureRetrySeconds). */
	bool EnsureCapture(const tString &asWanted, float afTimeStep)
	{
		if (mpCapture == NULL)
		{
			if (mbCaptureFailed)
			{
				mfCaptureRetry -= afTimeStep;
				if (mfCaptureRetry > 0.0f)
					return false;
			}
			const bool bQuiet = mbCaptureFailed; /* retries: only a success is logged */
			LogCaptureDevices();
			ALCdevice *d = NULL;
			if (!asWanted.empty())
			{
				d = TryOpenCapture(asWanted.c_str(), kCaptureRingSamples, bQuiet);
				if (d == NULL && !bQuiet)
					Log(" voice: voice_capture_device \"%s\" did not open - trying the default device\n",
						asWanted.c_str());
			}
			if (d == NULL)
				d = TryOpenCapture(NULL, kCaptureRingSamples, bQuiet);
			if (d == NULL)
				d = TryOpenCapture(NULL, kCaptureRingSamplesBig, bQuiet);
			if (d == NULL)
			{
				const ALCchar *list = alcGetString(NULL, ALC_CAPTURE_DEVICE_SPECIFIER);
				int n = 0;
				for (const ALCchar *p = list; d == NULL && p && *p && n < 16; p += strlen(p) + 1, ++n)
					d = TryOpenCapture(p, kCaptureRingSamples, bQuiet);
			}
			if (d == NULL)
			{
				if (!mbCaptureFailed)
					Log(" voice: NO MICROPHONE - no capture device opened; push-to-talk sends nothing (retried every %.0f s while the key is held)\n",
						kCaptureRetrySeconds);
				mbCaptureFailed = true;
				mfCaptureRetry = kCaptureRetrySeconds;
				return false;
			}
			mpCapture = d;
			mbCaptureFailed = false;
			const ALCchar *nm = alcGetString(d, ALC_CAPTURE_DEVICE_SPECIFIER);
			Log(" voice: microphone opened: \"%s\" (16 kHz mono16, %d-sample ring)\n",
				(nm && *nm) ? nm : "?", mlCaptureRing);
		}
		if (!mbCapturing)
		{
			alcGetError(mpCapture);
			alcCaptureStart(mpCapture);
			const ALCenum e = alcGetError(mpCapture);
			if (e != ALC_NO_ERROR)
			{
				Log(" voice: alcCaptureStart failed - ALC error 0x%04X; microphone closed, retried in %.0f s\n",
					(unsigned)e, kCaptureRetrySeconds);
				alcCaptureCloseDevice(mpCapture);
				mpCapture = NULL;
				mbCaptureFailed = true;
				mfCaptureRetry = kCaptureRetrySeconds;
				return false;
			}
			if (!mbCaptureStartLogged)
			{
				Log(" voice: capture started\n");
				mbCaptureStartLogged = true;
			}
			mbCapturing = true;
			mvPending.clear();
			mfCaptureRunTime = 0.0f;
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

	//---------------- menu microphone test ----------------
	ALCdevice *mpTestCapture;
	float mfTestLevel;       /* 0..1, peak-held */
	float mfTestRetry;
	bool mbTestFailed;
	tString msTestOpened;

	void CloseTestCapture()
	{
		if (mpTestCapture)
		{
			alcCaptureStop(mpTestCapture);
			alcCaptureCloseDevice(mpTestCapture);
			mpTestCapture = NULL;
		}
		mfTestLevel = 0.0f;
		mfTestRetry = 0.0f;
		mbTestFailed = false;
		msTestOpened = "";
	}

	/** Pull everything the device has into mvPending (bounded).
	    ALC_CAPTURE_SAMPLES counts sample FRAMES (= samples, mono). */
	void PullCapture(float afTimeStep)
	{
		if (!mpCapture || !mbCapturing)
			return;
		mfCaptureRunTime += afTimeStep;
		ALCint avail = 0;
		alcGetIntegerv(mpCapture, ALC_CAPTURE_SAMPLES, 1, &avail);
		if (avail <= 0)
		{
			if (!mbFirstSamplesLogged && !mbNoSamplesLogged && mfCaptureRunTime > kNoSamplesWarnSeconds)
			{
				Log(" voice: capture has run %.1f s and the device delivered NO samples - microphone blocked "
					"(Windows Settings > Privacy > Microphone: allow desktop apps) or held exclusively by another app\n",
					mfCaptureRunTime);
				mbNoSamplesLogged = true;
				mbMicSilent = true;
			}
			return;
		}
		if (!mbFirstSamplesLogged)
		{
			Log(" voice: first microphone samples: %d frame(s), %.0f ms after capture start\n",
				(int)avail, mfCaptureRunTime * 1000.0f);
			mbFirstSamplesLogged = true;
		}
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
				Log(" voice: capture backlog > 1 s - oldest audio dropped (frame stall while talking)\n");
				mbBacklogLogged = true;
			}
		}
	}

	//---------------- send-side burst diagnostics ----------------

	void BeginTxBurst()
	{
		mbTxBurst = true;
		mlTxBurstFrames = 0;
		mlTxBurstPackets = 0;
		mlTxBurstPeak = 0;
		mfTxBurstMaxDb = -100.0f;
		mfTxBurstSeconds = 0.0f;
	}

	void NoteTxFrame(float afDbfs, int alPeak)
	{
		++mlTxBurstFrames;
		if (alPeak > mlTxBurstPeak)
			mlTxBurstPeak = alPeak;
		if (afDbfs > mfTxBurstMaxDb)
			mfTxBurstMaxDb = afDbfs;
	}

	void EndTxBurst()
	{
		if (!mbTxBurst)
			return;
		mbTxBurst = false;
		++mlBurstsOut;
		const bool bSilent = mlTxBurstFrames >= kSilentBurstFrames && mlTxBurstPeak == 0;
		if (bSilent)
			mbMicSilent = true;
		else if (mlTxBurstPeak > 0)
			mbMicSilent = false;
		if (mlTxBurstsLogged < kBurstsLogged || (bSilent && !mbSilentLogged))
		{
			++mlTxBurstsLogged;
			if (bSilent)
				mbSilentLogged = true;
			const char *sNote = "";
			if (bSilent)
				sNote = " - PURE DIGITAL SILENCE: mic muted/blocked (Windows Settings > Privacy > Microphone: "
					"allow desktop apps) or the wrong default recording device (see voice_capture_device)";
			else if (mlTxBurstFrames == 0)
				sNote = " - nothing captured (key released before the microphone delivered audio)";
			else if (mfTxBurstMaxDb < -50.0f)
				sNote = " - very quiet: raise the microphone level in Windows Sound settings";
			Log(" voice: sent burst #%d: %.2f s, %d frame(s) -> %d packet(s), mic max %.1f dBFS RMS, peak %.1f dBFS%s\n",
				mlBurstsOut, mfTxBurstSeconds, mlTxBurstFrames, mlTxBurstPackets, mfTxBurstMaxDb,
				PeakDbfs(mlTxBurstPeak), sNote);
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
		++mlPacketsOut;
		++mlTxBurstPackets;
		if (!mbFirstPacketOutLogged)
		{
			Log(" voice: first voice packet handed to the network (%u B, %d frame(s), seq %u, local id %u)\n",
				(unsigned)mvPacket.size(), mlFramesInPacket, (unsigned)hdr.mSeq, (unsigned)alLocalId);
			mbFirstPacketOutLogged = true;
		}
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

	void LogStreamSummary(uint8_t alId, const cVoiceStream *s)
	{
		if (!s || s->mlPackets <= 0)
			return;
		Log(" voice: player %u stream closed: %d packet(s), %d lost, %d frame(s) played, %d PLC, %d underrun(s)\n",
			(unsigned)alId, s->mlPackets, s->mlLost, s->mlFramesPlayed, s->mlPlc, s->mlUnderruns);
	}

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
		else
		{
			for (int i = 0; i < kQueueBuffers; ++i)
				if (apStream->mvBuffers[i] != 0 && alIsBuffer(apStream->mvBuffers[i]))
					alDeleteBuffers(1, &apStream->mvBuffers[i]);
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
		{
			LogStreamSummary(it->first, it->second);
			FreeStream(it->second);
		}
		m_mapStreams.clear();
	}

	void DropStream(uint8_t alId)
	{
		std::map<uint8_t, cVoiceStream *>::iterator it = m_mapStreams.find(alId);
		if (it == m_mapStreams.end())
			return;
		LogStreamSummary(it->first, it->second);
		FreeStream(it->second);
		m_mapStreams.erase(it);
	}

	/** Mono sources the output device offers (0 = unknown). */
	static int DeviceMonoSources()
	{
		ALCcontext *ctx = alcGetCurrentContext();
		ALCdevice *dev = ctx ? alcGetContextsDevice(ctx) : NULL;
		ALCint n = 0;
		if (dev)
		{
			alcGetIntegerv(dev, ALC_MONO_SOURCES, 1, &n);
			alcGetError(dev);
		}
		return (int)n;
	}

	/** Decoder + source + buffers for a remote id; NULL when creation
	    failed (logged once; the caller backs off before the next try).
	    Success is judged by the NAMES AL returned, not by alGetError alone:
	    the wrapper's stream thread shares the context's error slot. */
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
		ALenum e = alGetError();
		bool bOk = true;
		for (int i = 0; i < kQueueBuffers; ++i)
			if (s->mvBuffers[i] == 0 || !alIsBuffer(s->mvBuffers[i]))
				bOk = false;
		if (!bOk)
		{
			if (!mbCreateFailLogged)
			{
				Log(" voice: alGenBuffers failed (AL error 0x%04X) - no playback for player %u (retrying)\n",
					(unsigned)e, (unsigned)alId);
				mbCreateFailLogged = true;
			}
			FreeStream(s);
			return NULL;
		}
		s->mbBuffersOk = true;
		for (int i = 0; i < kQueueBuffers; ++i)
			s->mvFreeBuffers.push_back(s->mvBuffers[i]);

		s->mlSource = 0;
		alGenSources(1, &s->mlSource);
		e = alGetError();
		if (s->mlSource == 0 || !alIsSource(s->mlSource))
		{
			if (!mbCreateFailLogged)
			{
				Log(" voice: alGenSources failed (AL error 0x%04X) - no free OpenAL source for player %u's voice "
					"(the device offers %d mono sources and the engine's sound system reserves its own; lower "
					"MaxSoundChannels in settings.cfg, or raise 'sources' in alsoft.ini) - retrying\n",
					(unsigned)e, (unsigned)alId, DeviceMonoSources());
				mbCreateFailLogged = true;
			}
			s->mlSource = 0;
			FreeStream(s);
			return NULL;
		}
		s->mbSourceOk = true;
		/* Centered (listener-relative at the origin) until the ghost's head
		   position arrives; UpdateStreamAudio switches to world space. */
		alSourcei(s->mlSource, AL_SOURCE_RELATIVE, AL_TRUE);
		alSourcei(s->mlSource, AL_LOOPING, AL_FALSE);
		alSourcef(s->mlSource, AL_PITCH, 1.0f);
		alSourcef(s->mlSource, AL_GAIN, afVolume);
		alSourcef(s->mlSource, AL_REFERENCE_DISTANCE, kRefDistance);
		alSourcef(s->mlSource, AL_MAX_DISTANCE, kMaxDistance);
		/* rolloff 0 = no AL attenuation under ANY distance model: the gain
		   is ours (see the file header — this replaces the invalid
		   per-source AL_SOURCE_DISTANCE_MODEL call that killed playback) */
		alSourcef(s->mlSource, AL_ROLLOFF_FACTOR, 0.0f);
		alSource3f(s->mlSource, AL_POSITION, 0.0f, 0.0f, 0.0f);
		alSource3f(s->mlSource, AL_VELOCITY, 0.0f, 0.0f, 0.0f);
		e = alGetError();
		if (e != AL_NO_ERROR && !mbAlErrorLogged)
		{
			/* not fatal: every property above is optional for playback,
			   and the error may be the wrapper thread's */
			Log(" voice: AL error 0x%04X during source setup for player %u - ignored\n",
				(unsigned)e, (unsigned)alId);
			mbAlErrorLogged = true;
		}
		s->mbRelative = true;
		m_mapStreams[alId] = s;
		Log(" voice: playback stream for player %u created (Opus decoder, AL source %u, %d buffers)\n",
			(unsigned)alId, (unsigned)s->mlSource, kQueueBuffers);
		return s;
	}

	/** Decode one Opus frame (apData NULL / alLen 0 = PLC) onto the
	    stream's frame queue (bounded). true = a frame was produced. */
	bool PushDecoded(cVoiceStream *apStream, const unsigned char *apData, int alLen)
	{
		if (!apStream || !apStream->mpDecoder)
			return false;
		const int n = opus_decode(apStream->mpDecoder, (alLen > 0) ? apData : NULL,
			(alLen > 0) ? alLen : 0, mvDecodeBuf, kFrameSamples, 0);
		if (n != kFrameSamples)
		{
			if (!mbOpusErrorLogged)
			{
				Log(" voice: opus_decode returned %d (expected %d samples) - frame skipped\n", n, kFrameSamples);
				mbOpusErrorLogged = true;
			}
			return false;
		}
		if ((int)apStream->mFrames.size() >= kMaxQueuedFrames)
			apStream->mFrames.pop_front(); /* receiver stalled: keep the newest */
		apStream->mFrames.push_back(std::vector<int16_t>(mvDecodeBuf, mvDecodeBuf + kFrameSamples));
		return true;
	}

	/** An AL error in the per-frame work. Logged once; true = the stream
	    must go (source really gone, or the errors persist). */
	bool StreamError(cVoiceStream *s, uint8_t alId, const char *asWhere, ALenum aErr)
	{
		++s->mlErrorFrames;
		if (!mbAlErrorLogged)
		{
			Log(" voice: OpenAL error 0x%04X at %s (player %u) - tolerated unless it persists %d frames\n",
				(unsigned)aErr, asWhere ? asWhere : "?", (unsigned)alId, kMaxErrorFrames);
			mbAlErrorLogged = true;
		}
		return !alIsSource(s->mlSource) || s->mlErrorFrames >= kMaxErrorFrames;
	}

	/** Per frame for one stream: reclaim, PLC, feed, restart, position,
	    gain. Returns false when the stream must be torn down. */
	bool UpdateStreamAudio(cVoiceStream *s, uint8_t alId, float afTimeStep, float afVolume)
	{
		if (!s || !s->mbSourceOk)
			return false;
		const ALuint src = s->mlSource;
		ALenum e = AL_NO_ERROR;
		alGetError();

		/* 0. state FIRST, then reclaim. Read the other way round, a source
		   that ran dry between the two reads would look STOPPED while its
		   last buffers (finished after the PROCESSED read) are still
		   queued, and alSourcePlay would replay them. This order can only
		   delay a restart by one frame. */
		ALint state = AL_INITIAL;
		alGetSourcei(src, AL_SOURCE_STATE, &state);
		if ((e = alGetError()) != AL_NO_ERROR)
			return !StreamError(s, alId, "AL_SOURCE_STATE", e);

		/* 1. reclaim processed buffers */
		ALint processed = 0;
		alGetSourcei(src, AL_BUFFERS_PROCESSED, &processed);
		if ((e = alGetError()) != AL_NO_ERROR)
			return !StreamError(s, alId, "AL_BUFFERS_PROCESSED", e);
		while (processed > 0)
		{
			ALuint b = 0;
			alSourceUnqueueBuffers(src, 1, &b);
			if ((e = alGetError()) != AL_NO_ERROR)
				return !StreamError(s, alId, "alSourceUnqueueBuffers", e);
			if (s->OwnsBuffer(b))
				s->mvFreeBuffers.push_back(b);
			--processed;
		}

		ALint queued = 0;
		alGetSourcei(src, AL_BUFFERS_QUEUED, &queued);
		if ((e = alGetError()) != AL_NO_ERROR)
			return !StreamError(s, alId, "AL_BUFFERS_QUEUED", e);

		const bool bSenderActive = s->mfSinceLastPacket < kBurstGapSeconds;
		bool bLogStart = false;

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
				if (PushDecoded(s, NULL, 0))
				{
					++s->mlPlc;
					++s->mlBurstPlc;
				}
				++s->mlPlcRun;
			}

			/* 4. feed */
			while (!s->mFrames.empty() && !s->mvFreeBuffers.empty())
			{
				const ALuint b = s->mvFreeBuffers.back();
				const std::vector<int16_t> &pcm = s->mFrames.front();
				alBufferData(b, AL_FORMAT_MONO16, &pcm[0], (ALsizei)(pcm.size() * sizeof(int16_t)),
					(ALsizei)kNetVoiceSampleRate);
				if ((e = alGetError()) != AL_NO_ERROR)
					return !StreamError(s, alId, "alBufferData", e);
				alSourceQueueBuffers(src, 1, &b);
				if ((e = alGetError()) != AL_NO_ERROR)
					return !StreamError(s, alId, "alSourceQueueBuffers", e);
				s->mvFreeBuffers.pop_back();
				s->mFrames.pop_front();
				++queued;
				++s->mlFramesPlayed;
				++s->mlBurstFrames;
			}

			/* 5. (re)start at burst start or after an underrun: a streaming
			   source that drained its queue is AL_STOPPED and stays so until
			   alSourcePlay, whatever is queued afterwards */
			if (state != AL_PLAYING && queued > 0)
			{
				const bool bUnderrun = s->mbPlayingBurst && state == AL_STOPPED;
				alSourcePlay(src);
				if ((e = alGetError()) != AL_NO_ERROR)
					return !StreamError(s, alId, "alSourcePlay", e);
				if (!s->mbStarted)
				{
					bLogStart = true; /* after the gain below is known */
					s->mbStarted = true;
				}
				else if (bUnderrun)
				{
					++s->mlUnderruns;
					++s->mlBurstUnderruns;
					if (!s->mbLoggedUnderrun)
					{
						Log(" voice: player %u stream ran dry mid-burst and was restarted (underrun; further ones are counted in the summaries)\n",
							(unsigned)alId);
						s->mbLoggedUnderrun = true;
					}
				}
				s->mbPlayingBurst = true;
			}
			else if (state != AL_PLAYING && queued == 0 && s->mFrames.empty())
			{
				/* burst over (or starved beyond PLC): the next packet primes
				   again. Stopping while the sender still counts as active is
				   either the burst's end (PLC tail) or a starvation — only a
				   packet of the SAME burst arriving later (OnVoicePacket)
				   makes it an underrun. */
				s->mbStarved = s->mbPlayingBurst && bSenderActive;
				s->mbPrimed = false;
				s->mfPrimeWait = 0.0f;
				s->mlPlcRun = 0;
				s->mbPlayingBurst = false;
			}
		}

		/* 6. position + proximity gain (software inverse-distance clamped).
		   Known head position: world space at the ghost's head. Unknown or
		   stale (ghost not spawned / in another map): centered, full voice
		   volume — never silent just because the ghost is missing. */
		const bool bPosKnown = s->mbHavePos && s->mfSincePos < kPosStaleSeconds;
		float fGain = afVolume;
		float fDist = -1.0f;
		if (bPosKnown)
		{
			if (s->mbRelative)
			{
				alSourcei(src, AL_SOURCE_RELATIVE, AL_FALSE);
				s->mbRelative = false;
			}
			alSource3f(src, AL_POSITION, s->mvPos.x, s->mvPos.y, s->mvPos.z);
			ALfloat lx = 0, ly = 0, lz = 0;
			alGetListener3f(AL_POSITION, &lx, &ly, &lz);
			const float dx = s->mvPos.x - lx, dy = s->mvPos.y - ly, dz = s->mvPos.z - lz;
			float d = sqrtf(dx * dx + dy * dy + dz * dz);
			fDist = d;
			if (d < kRefDistance) d = kRefDistance;
			else if (d > kMaxDistance) d = kMaxDistance;
			fGain *= kRefDistance / (kRefDistance + kRolloff * (d - kRefDistance));
		}
		else if (!s->mbRelative)
		{
			alSourcei(src, AL_SOURCE_RELATIVE, AL_TRUE);
			alSource3f(src, AL_POSITION, 0.0f, 0.0f, 0.0f);
			s->mbRelative = true;
		}
		alSourcef(src, AL_GAIN, fGain);
		if ((e = alGetError()) != AL_NO_ERROR)
			return !StreamError(s, alId, "position/gain", e);
		s->mfLastGain = fGain;
		s->mfLastDist = fDist;
		s->mlErrorFrames = 0; /* a clean frame */
		if (bLogStart)
		{
			ALfloat lg = 1.0f;
			alGetListenerf(AL_GAIN, &lg);
			alGetError();
			char sDist[48];
			if (fDist >= 0.0f)
				snprintf(sDist, sizeof(sDist), "%.1f m away", fDist);
			else
				snprintf(sDist, sizeof(sDist), "position unknown (centered)");
			Log(" voice: playback started for player %u (%d buffer(s) queued, %s, voice gain %.2f, listener gain %.2f)%s\n",
				(unsigned)alId, (int)queued, sDist, fGain, lg,
				lg < 0.05f ? " - the game's master sound volume is ~0: voice follows it" : "");
		}
		return true;
	}

	/** First few received bursts per player: what arrived, what played,
	    how loud (distance / gain) — once the burst is over. */
	void MaybeLogRxBurst(uint8_t alId, cVoiceStream *s)
	{
		if (!s->mbBurstOpen || s->mfSinceLastPacket < kBurstGapSeconds)
			return;
		if (!s->mFrames.empty() || (s->mbPlayingBurst && s->mfSinceLastPacket < kBurstSummaryAfterSeconds))
			return;
		s->mbBurstOpen = false;
		if (s->mlBurstsLogged < kBurstsLogged)
		{
			++s->mlBurstsLogged;
			ALfloat lg = 1.0f;
			alGetListenerf(AL_GAIN, &lg);
			alGetError();
			char sDist[48];
			if (s->mfLastDist >= 0.0f)
				snprintf(sDist, sizeof(sDist), "%.1f m away", s->mfLastDist);
			else
				snprintf(sDist, sizeof(sDist), "position unknown (centered)");
			Log(" voice: heard player %u burst #%d: %d packet(s), %d frame(s) played, %d PLC, %d underrun(s), %s, voice gain %.2f x listener %.2f%s\n",
				(unsigned)alId, s->mlBurstsLogged, s->mlBurstPackets, s->mlBurstFrames, s->mlBurstPlc,
				s->mlBurstUnderruns, sDist, s->mfLastGain, lg,
				s->mlBurstFrames == 0 ? " - NOTHING PLAYED" : "");
		}
		s->mlBurstPackets = 0;
		s->mlBurstFrames = 0;
		s->mlBurstPlc = 0;
		s->mlBurstUnderruns = 0;
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

	ALCcontext *ctx = alcGetCurrentContext();
	if (ctx == NULL)
	{
		Log(" voice: no current OpenAL context (sound disabled or not initialised) - voice chat off\n");
		mbFailed = true;
		return false;
	}
	alGetError(); /* start clean */
	{
		ALCdevice *dev = alcGetContextsDevice(ctx);
		const ALCchar *sDev = dev ? alcGetString(dev, ALC_DEVICE_SPECIFIER) : NULL;
		const ALchar *sVendor = alGetString(AL_VENDOR);
		const ALchar *sVersion = alGetString(AL_VERSION);
		const ALchar *sRenderer = alGetString(AL_RENDERER);
		Log(" voice: OpenAL context on \"%s\" (%s, %s, %s), %d mono sources on the device\n",
			(sDev && *sDev) ? sDev : "?", sVendor ? sVendor : "?", sVersion ? sVersion : "?",
			sRenderer ? sRenderer : "?", Impl::DeviceMonoSources());
		/* Source budget probe: the wrapper grabs its sources at engine
		   init; if none is left, say so now instead of at the first packet. */
		ALuint probe = 0;
		alGenSources(1, &probe);
		const ALenum e = alGetError();
		if (probe != 0 && alIsSource(probe))
			alDeleteSources(1, &probe);
		else
			Log(" voice: WARNING - no free OpenAL source right now (AL error 0x%04X): the engine holds them all, "
				"remote voices cannot play (lower MaxSoundChannels in settings.cfg)\n", (unsigned)e);
		alGetError();
	}

	int err = 0;
	mpImpl->mpEncoder = opus_encoder_create(kNetVoiceSampleRate, 1, OPUS_APPLICATION_VOIP, &err);
	if (mpImpl->mpEncoder == NULL || err != OPUS_OK)
	{
		Log(" voice: opus_encoder_create failed (%d) - voice chat off\n", err);
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
	char sMode[64];
	if (mbOpenMic)
		snprintf(sMode, sizeof(sMode), "open mic, gate %.0f dBFS", mfGateDbfs);
	else
		snprintf(sMode, sizeof(sMode), "push-to-talk V");
	Log(" voice: ready (%s encoder+decoder, 16 kHz mono, 20 ms frames, %d kbps VBR, %s, volume %.2f, capture device %s)\n",
		opus_get_version_string(), kBitrateBps / 1000, sMode, mfVolume,
		msCaptureDevice.empty() ? "<system default>" : msCaptureDevice.c_str());
	return true;
}

void cVoiceChat::Shutdown()
{
	if (mpImpl)
	{
		if (mbInitialized)
		{
			mpImpl->EndTxBurst();
			Log(" voice: session end - sent %d packet(s) in %d burst(s)\n",
				mpImpl->mlPacketsOut, mpImpl->mlBurstsOut);
		}
		mpImpl->CloseCapture();
		if (mpImpl->mpEncoder)
		{
			opus_encoder_destroy(mpImpl->mpEncoder);
			mpImpl->mpEncoder = NULL;
		}
		/* sources/buffers need the context — alive here, see the header */
		mpImpl->FreeAllStreams();
		mpImpl->ResetSession();
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
	Impl *im = mpImpl;

	//------------------ capture -> encode -> outbox ------------------
	if (abTalkHeld && !mbOpenMic && !im->mbTalkKeyLogged)
	{
		/* proves the VoiceTalk action is registered, bound and polled */
		Log(" voice: push-to-talk key held for the first time this session (local id %u%s)\n",
			(unsigned)mlLocalPlayerId, mlLocalPlayerId == 0 ? " - NOT joined yet: nothing is sent" : "");
		im->mbTalkKeyLogged = true;
	}
	const bool bWantMic = mbEnabled && mlLocalPlayerId != 0 && (mbOpenMic || abTalkHeld);
	bool bMicOpen = false;
	if (bWantMic && im->mpEncoder && im->EnsureCapture(msCaptureDevice, afTimeStep))
	{
		im->PullCapture(afTimeStep);
		std::vector<int16_t> &pend = im->mvPending;
		size_t consumed = 0;
		int encoded = 0;
		bool bGateOpen = !mbOpenMic; /* PTT: always open while held */
		if (!mbOpenMic && !im->mbTxBurst)
			im->BeginTxBurst();
		if (mbOpenMic)
		{
			im->mfGateHold -= afTimeStep;
			if (im->mfGateHold < 0.0f) im->mfGateHold = 0.0f;
			im->mfOpenMicRunTime += afTimeStep;
		}
		while (pend.size() - consumed >= (size_t)kFrameSamples && encoded < kMaxFramesEncodedPerUpdate)
		{
			const int16_t *frame = &pend[consumed];
			consumed += (size_t)kFrameSamples;
			++encoded;
			const float fDb = FrameLevelDbfs(frame, kFrameSamples);
			if (fDb > im->mfLoudestDbfs)
				im->mfLoudestDbfs = fDb;
			if (mbOpenMic)
			{
				/* energy gate: open on speech level, hold briefly after */
				if (fDb > mfGateDbfs)
					im->mfGateHold = kOpenMicHoldSeconds;
				if (im->mfGateHold <= 0.0f)
					continue; /* silence: nothing on the wire */
				bGateOpen = true;
				if (!im->mbTxBurst)
					im->BeginTxBurst();
			}
			im->NoteTxFrame(fDb, FramePeakAbs(frame, kFrameSamples));
			const int n = opus_encode(im->mpEncoder, frame, kFrameSamples, im->mvEncodeBuf,
				(opus_int32)(kNetVoiceMaxPayload / kMaxFramesPerPacket - 2));
			if (n < 0)
			{
				if (!im->mbOpusErrorLogged)
				{
					Log(" voice: opus_encode failed (%d) - frame skipped\n", n);
					im->mbOpusErrorLogged = true;
				}
				continue;
			}
			/* n <= 2 is an Opus "silence / DTX" frame: still sent so the far
			   end's PLC state stays sane */
			im->AppendFrame(im->mvEncodeBuf, n, mlLocalPlayerId, mvOutgoing);
		}
		if (consumed > 0)
			pend.erase(pend.begin(), pend.begin() + (std::ptrdiff_t)consumed);
		/* A lone frame waits at most ~30 ms for its twin (the mic delivers
		   one every 20 ms), then goes out alone: a half packet only costs a
		   header, a held frame costs latency. */
		if (im->mlFramesInPacket > 0)
		{
			im->mfPacketAge += afTimeStep;
			if (im->mfPacketAge >= kLoneFrameHoldSeconds)
				im->FlushPacket(mlLocalPlayerId, mvOutgoing);
		}
		if (mbOpenMic && im->mfGateHold > 0.0f)
			bGateOpen = true;
		if (im->mbTxBurst)
			im->mfTxBurstSeconds += afTimeStep;
		if (mbOpenMic && !bGateOpen && im->mbTxBurst)
		{
			im->FlushPacket(mlLocalPlayerId, mvOutgoing); /* gate closed: tail out now */
			im->EndTxBurst();
		}
		if (mbOpenMic && !im->mbOpenMicQuietLogged && im->mlBurstsOut == 0 && !im->mbTxBurst &&
			im->mfOpenMicRunTime > kOpenMicQuietWarnSeconds)
		{
			Log(" voice: open mic - nothing above the %.0f dBFS gate in the first %.0f s (loudest frame %.1f dBFS); "
				"speak up, raise the mic level, or lower voice_gate_db\n",
				mfGateDbfs, kOpenMicQuietWarnSeconds, im->mfLoudestDbfs);
			im->mbOpenMicQuietLogged = true;
		}
		bMicOpen = bGateOpen;
	}
	else
	{
		if (im->mbCapturing)
		{
			/* key released: the last partial packet goes out now */
			im->FlushPacket(mlLocalPlayerId, mvOutgoing);
			im->StopCapture();
		}
		im->EndTxBurst();
		im->mfGateHold = 0.0f;
	}
	mbMicOpen = bMicOpen;

	//------------------ inbox -> AL sources ------------------
	for (std::map<uint8_t, cPeerRx>::iterator pit = im->m_mapPeers.begin(); pit != im->m_mapPeers.end(); ++pit)
	{
		if (pit->second.mfTalk > 0.0f)
			pit->second.mfTalk -= afTimeStep;
		if (pit->second.mfRetry > 0.0f)
			pit->second.mfRetry -= afTimeStep;
	}
	if (!mbEnabled || im->mbPlaybackOff)
		return;
	std::vector<uint8_t> vDead;
	for (std::map<uint8_t, cVoiceStream *>::iterator it = im->m_mapStreams.begin();
		it != im->m_mapStreams.end(); ++it)
	{
		cVoiceStream *s = it->second;
		if (!s)
			continue;
		s->mfSinceLastPacket += afTimeStep;
		s->mfSincePos += afTimeStep;
		if (!im->UpdateStreamAudio(s, it->first, afTimeStep, mfVolume))
			vDead.push_back(it->first);
		else
			im->MaybeLogRxBurst(it->first, s);
	}
	for (size_t i = 0; i < vDead.size(); ++i)
	{
		Log(" voice: playback stream for player %u rebuilt after OpenAL errors (re-created by its next packet)\n",
			(unsigned)vDead[i]);
		im->DropStream(vDead[i]);
		if (++im->mlStreamFailures >= kMaxStreamFailures)
		{
			Log(" voice: repeated OpenAL playback failures - voice PLAYBACK off for this session (the microphone keeps sending)\n");
			im->FreeAllStreams();
			im->mbPlaybackOff = true;
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
	it->second->mfSincePos = 0.0f;
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

	/* the HUD indicator and the first-packet log do not depend on playback */
	cPeerRx &peer = mpImpl->m_mapPeers[alFromId];
	peer.mfTalk = kTalkIndicatorSeconds;
	++peer.mlPackets;
	if (!peer.mbFirstLogged)
	{
		Log(" voice: first voice packet received from player %u (%u B, %u frame(s), seq %u)%s\n",
			(unsigned)alFromId, (unsigned)alLen, (unsigned)hdr.mFrames, (unsigned)hdr.mSeq,
			mpImpl->mbPlaybackOff ? " - playback is off this session" : "");
		peer.mbFirstLogged = true;
	}
	if (mpImpl->mbPlaybackOff || peer.mfRetry > 0.0f)
		return;

	cVoiceStream *s = mpImpl->GetOrCreateStream(alFromId, mfVolume);
	if (!s)
	{
		peer.mfRetry = kCreateRetrySeconds;
		return;
	}

	/* sequence: stale/duplicate dropped, small gaps concealed, big gaps
	   or a fresh burst reset the decoder */
	const bool bContinuing = s->mbHaveSeq && s->mfSinceLastPacket < kBurstGapSeconds;
	if (bContinuing)
	{
		const int16_t diff = (int16_t)(hdr.mSeq - s->mlLastSeq);
		if (diff <= 0)
			return; /* reordered late packet: already concealed / played */
		if (s->mbStarved)
		{
			/* the stream ran dry beyond PLC and the burst goes on: underrun */
			s->mbStarved = false;
			++s->mlUnderruns;
			++s->mlBurstUnderruns;
			if (!s->mbLoggedUnderrun)
			{
				Log(" voice: player %u stream starved mid-burst (packets late or lost; further ones are counted in the summaries)\n",
					(unsigned)alFromId);
				s->mbLoggedUnderrun = true;
			}
		}
		if (diff > 1)
		{
			const int lost = diff - 1;
			s->mlLost += lost;
			if (lost <= kMaxLostPacketsConcealed)
			{
				for (int i = 0; i < lost * (int)hdr.mFrames; ++i)
					if (mpImpl->PushDecoded(s, NULL, 0))
					{
						++s->mlPlc;
						++s->mlBurstPlc;
					}
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
		s->mbPlayingBurst = false;
		s->mbStarved = false;
	}
	s->mlLastSeq = hdr.mSeq;
	s->mbHaveSeq = true;
	s->mfSinceLastPacket = 0.0f;
	s->mlPlcRun = 0;
	s->mbBurstOpen = true;
	++s->mlPackets;
	++s->mlBurstPackets;

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
	{
		mpImpl->DropStream(alId);
		mpImpl->m_mapPeers.erase(alId);
	}
}

void cVoiceChat::DropAllPlayers()
{
	if (mpImpl)
	{
		mpImpl->FreeAllStreams();
		mpImpl->m_mapPeers.clear();
	}
}

bool cVoiceChat::IsTalking(uint8_t alId) const
{
	if (alId != 0 && alId == mlLocalPlayerId)
		return mbMicOpen;
	if (!mpImpl)
		return false;
	std::map<uint8_t, cPeerRx>::const_iterator it = mpImpl->m_mapPeers.find(alId);
	if (it == mpImpl->m_mapPeers.end())
		return false;
	return it->second.mfTalk > 0.0f;
}

const char *cVoiceChat::GetStatusHint() const
{
	if (!mpImpl)
		return NULL;
	if (mbFailed || mpImpl->mbPlaybackOff)
		return "VOICE OFF";
	if (!mbInitialized)
		return NULL;
	if (mpImpl->mbCaptureFailed)
		return "NO MIC";
	if (mpImpl->mbMicSilent)
		return "MIC SILENT";
	return NULL;
}

#endif /* PENUMBRA_MULTIPLAYER && PENUMBRA_VOICE */

//======================================================================
// Shared by the real and the stub build.
//======================================================================

#if defined(PENUMBRA_MULTIPLAYER) && defined(PENUMBRA_VOICE)
//-----------------------------------------------------------------------
// Microphone picker + menu test
//-----------------------------------------------------------------------

void cVoiceChat::SetCaptureDevice(const tString &asName)
{
	if (asName == msCaptureDevice)
		return;
	msCaptureDevice = asName;
	if (mpImpl)
	{
		/* the session mic reopens on the next push-to-talk with the new
		   device; the menu test reopens on its next frame */
		mpImpl->CloseCapture();
		mpImpl->mbCaptureFailed = false;
		mpImpl->mfCaptureRetry = 0.0f;
		mpImpl->mbCaptureStartLogged = false;
		mpImpl->mbMicSilent = false;
		mpImpl->CloseTestCapture();
	}
	mbMicOpen = false;
	Log(" voice: microphone set to \"%s\"\n",
		asName.empty() ? "<system default>" : asName.c_str());
}

void cVoiceChat::GetCaptureDeviceNames(std::vector<tString> &avOut)
{
	avOut.clear();
	const ALCchar *list = alcGetString(NULL, ALC_CAPTURE_DEVICE_SPECIFIER);
	for (const ALCchar *d = list; d && *d && avOut.size() < 32; d += strlen(d) + 1)
		avOut.push_back(tString(d));
}

void cVoiceChat::UpdateMicTest(float afTimeStep, bool abActive)
{
	if (!mpImpl)
		return;
	Impl *im = mpImpl;
	if (!abActive)
	{
		if (im->mpTestCapture || im->mbTestFailed)
			im->CloseTestCapture();
		return;
	}
	if (afTimeStep < 0.0f) afTimeStep = 0.0f;
	else if (afTimeStep > 0.25f) afTimeStep = 0.25f;

	if (im->mpTestCapture == NULL)
	{
		if (im->mbTestFailed)
		{
			im->mfTestRetry -= afTimeStep;
			if (im->mfTestRetry > 0.0f)
				return;
		}
		const char *sName = msCaptureDevice.empty() ? NULL : msCaptureDevice.c_str();
		ALCdevice *d = alcCaptureOpenDevice(sName, (ALCuint)kNetVoiceSampleRate,
			AL_FORMAT_MONO16, (ALCsizei)kCaptureRingSamples);
		if (d)
		{
			alcCaptureStart(d);
			if (alcGetError(d) != ALC_NO_ERROR)
			{
				alcCaptureCloseDevice(d);
				d = NULL;
			}
		}
		if (d == NULL)
		{
			if (!im->mbTestFailed)
				Log(" voice: mic test could not open \"%s\"\n",
					sName ? sName : "<system default>");
			im->mbTestFailed = true;
			im->mfTestRetry = 2.0f;
			im->mfTestLevel = 0.0f;
			return;
		}
		im->mpTestCapture = d;
		im->mbTestFailed = false;
		const ALCchar *nm = alcGetString(d, ALC_CAPTURE_DEVICE_SPECIFIER);
		im->msTestOpened = (nm && *nm) ? tString(nm) : tString("");
	}

	/* read everything available; the loudest 20 ms frame drives the meter */
	float fLoudest = -100.0f;
	for (int lGuard = 0; lGuard < 16; ++lGuard)
	{
		ALCint avail = 0;
		alcGetIntegerv(im->mpTestCapture, ALC_CAPTURE_SAMPLES, 1, &avail);
		if (avail < kFrameSamples)
			break;
		int16_t frame[kFrameSamples];
		alcCaptureSamples(im->mpTestCapture, frame, kFrameSamples);
		const float fDb = FrameLevelDbfs(frame, kFrameSamples);
		if (fDb > fLoudest)
			fLoudest = fDb;
	}
	/* -60 dBFS (room hum) .. -6 dBFS (shouting) -> 0..1 */
	float fNow = (fLoudest + 60.0f) / 54.0f;
	if (fNow < 0.0f) fNow = 0.0f;
	else if (fNow > 1.0f) fNow = 1.0f;
	const float fDecayed = im->mfTestLevel - afTimeStep * 1.5f;
	im->mfTestLevel = (fNow > fDecayed) ? fNow : (fDecayed > 0.0f ? fDecayed : 0.0f);
}

float cVoiceChat::GetMicTestLevel() const
{
	return mpImpl ? mpImpl->mfTestLevel : 0.0f;
}

int cVoiceChat::GetMicTestState() const
{
	if (!mpImpl)
		return 0;
	if (mpImpl->mpTestCapture)
		return 2;
	return mpImpl->mbTestFailed ? 1 : 0;
}

tString cVoiceChat::GetMicTestDeviceName() const
{
	return mpImpl ? mpImpl->msTestOpened : tString("");
}

#endif

cVoiceChat::cVoiceChat()
	: mbEnabled(true)
	  , mbOpenMic(false)
	  , mfVolume(1.0f)
	  , mfGateDbfs(-45.0f)
	  , msCaptureDevice()
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
#if defined(PENUMBRA_MULTIPLAYER) && defined(PENUMBRA_VOICE)
	if (mpImpl)
		mpImpl->CloseTestCapture();
#endif
	delete mpImpl; /* NULL in the stub build */
	mpImpl = NULL;
}
