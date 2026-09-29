#ifndef VOICE_CHAT_H
#define VOICE_CHAT_H

#include "StdAfx.h"
#include "NetworkPackets.h"

#include <vector>

//-----------------------------------------------------------------------
/** v16 proximity voice chat — Opus over ENet (unsequenced ch1), raw OpenAL
    Soft for capture and 3D playback.

    Owned by cNetworkManager, which is the ONLY caller. Everything runs on
    the main thread from cNetworkManager::Update:

      NetworkManager                     cVoiceChat
      ----------------------------       ------------------------------------
      SetRemoteHeadPos(id, pos)  ---->   per-player source position
      Update(dt, talkHeld)       ---->   capture -> encode -> outbox
                                         inbox  -> AL queue, PLC, gain
      GetOutgoing, ClearOutgoing <----   ready cNetVoice packets to send
      OnVoicePacket(id, ...)     ---->   decode into that player's jitter buffer
      DropPlayer / DropAllPlayers---->   free decoder + source
      IsTalking / IsMicOpen      <----   HUD indicators

    Transport, relay and peer ids stay in cNetworkManager; this class never
    touches ENet. The OpenAL side uses the CURRENT context (the one
    OALWrapper created for the engine's sound system — alcGetCurrentContext
    finds it without any wrapper header). The capture device is opened
    LAZILY on the first push-to-talk press (or the first Update with an open
    mic), never in single-player, and every AL error is checked; an AL
    failure is logged once and voice is switched off for the session rather
    than spamming hpl.log.

    Without PENUMBRA_MULTIPLAYER + PENUMBRA_VOICE every method is a no-op
    that reports "unavailable", so the manager and the HUD need no #ifdefs. */
class cVoiceChat
{
public:
	cVoiceChat();
	~cVoiceChat();

	/** Compiled with Opus + OpenAL (PENUMBRA_VOICE)? False in the stub build. */
	static bool IsCompiledIn();

	/** multiplayer.cfg voice_enabled / voice_volume / voice_open_mic. Safe to
	    call any time (before or after Init); volume is clamped to 0..2. */
	void SetEnabled(bool abEnabled);
	void SetVolume(float afVolume);
	void SetOpenMic(bool abOpenMic);
	bool IsEnabled() const { return mbEnabled; }
	float GetVolume() const { return mfVolume; }
	bool IsOpenMic() const { return mbOpenMic; }

	/** Create the Opus encoder and check for an OpenAL context. Called when
	    a session goes live (hosting, or a synced guest). Idempotent; returns
	    false (and logs once) when voice cannot run — the object stays
	    usable, every call is then a no-op. Does NOT open the microphone. */
	bool Init();
	/** Free capture device, encoder, every decoder and every AL source.
	    Must run while the AL context still exists (cNetworkManager dies
	    before cGame in cInit::Exit). Idempotent. */
	void Shutdown();
	bool IsInitialized() const { return mbInitialized; }

	/** Our wire id, stamped into outgoing packets (0 = not joined: nothing
	    is captured or sent). */
	void SetLocalPlayerId(uint8_t alId) { mlLocalPlayerId = alId; }

	/** Per frame, after the ghosts updated. abTalkHeld = push-to-talk key
	    down this frame (ignored with an open mic, which gates on level).
	    Captures + encodes into the outbox, unqueues/queues AL buffers for
	    every remote stream, applies PLC on gaps, positions and attenuates
	    the sources. */
	void Update(float afTimeStep, bool abTalkHeld);

	/** Where the remote player's HEAD is this frame (ghost render feet +
	    eye height). Unknown position (ghost between worlds) = abKnown
	    false: the stream keeps playing at its last position. */
	void SetRemoteHeadPos(uint8_t alId, const hpl::cVector3f &avPos, bool abKnown);

	/** A type-29 packet from alFromId (the relay stamped the author). The
	    header is validated here (frame count, per-frame lengths). */
	void OnVoicePacket(uint8_t alFromId, const void *apData, size_t alLen);

	/** Free the decoder + source of one remote player (left / dropped). */
	void DropPlayer(uint8_t alId);
	/** Free every remote stream (session ended, ClearGhostsInternal). */
	void DropAllPlayers();

	/** Outbox: packets built by Update this frame, ready for the wire
	    (unsequenced ch1). The manager sends and then ClearOutgoing()s. */
	size_t GetOutgoingCount() const { return mvOutgoing.size(); }
	const std::vector<uint8_t> &GetOutgoing(size_t alIdx) const { return mvOutgoing[alIdx]; }
	void ClearOutgoing() { mvOutgoing.clear(); }

	/** HUD: a remote player's voice arrived within the last ~0.35 s. For
	    our own id this is IsMicOpen(). */
	bool IsTalking(uint8_t alId) const;
	/** HUD: the microphone is live right now (PTT held with a working
	    capture device, or the open-mic gate is open). */
	bool IsMicOpen() const { return mbMicOpen; }
	/** Voice was disabled at runtime by an unrecoverable error (device or
	    context missing, AL error) — the HUD can show "no mic". */
	bool IsFailed() const { return mbFailed; }

private:
	cVoiceChat(const cVoiceChat &);            /* not copyable */
	cVoiceChat &operator=(const cVoiceChat &);

	bool mbEnabled;
	bool mbOpenMic;
	float mfVolume;
	bool mbInitialized;
	bool mbFailed;
	bool mbMicOpen;
	uint8_t mlLocalPlayerId;
	std::vector<std::vector<uint8_t> > mvOutgoing;

	/** Opus / OpenAL state lives in the .cpp (no third-party headers leak
	    into the manager's translation unit). NULL in the stub build. */
	struct Impl;
	Impl *mpImpl;
};
//-----------------------------------------------------------------------

#endif /* VOICE_CHAT_H */
