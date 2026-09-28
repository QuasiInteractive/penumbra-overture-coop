#ifndef GHOST_PLAYER_H
#define GHOST_PLAYER_H

#include "StdAfx.h"
#include "NetworkPackets.h"

#include <map>

namespace hpl { class cMeshEntity; class cAnimationState; }

//-----------------------------------------------------------------------
/** Tuning shared by every ghost, read from multiplayer.cfg by cNetworkManager
    (ghost_interp_ms, ghost_turn_rate, ghost_anim_trace, ghost_gait_*). */
struct cGhostTuning
{
	float mfInterpDelaySec;    /**< render this far behind the newest sample */
	float mfTurnRateRadPerSec; /**< body yaw rate limit toward the view yaw */
	bool mbAnimTrace;          /**< per-change anim logs + 5 s state trace */
	/** clip name -> gait speed (m/s at playback speed 1.0); overrides the
	    built-in table (cGhostPlayer::GetDefaultGait). */
	std::map<hpl::tString, float> m_mapGaitOverrides;

	cGhostTuning()
		: mfInterpDelaySec(0.10f)
		  , mfTurnRateRadPerSec(12.566f) /* 720 deg/s */
		  , mbAnimTrace(false)
		  , m_mapGaitOverrides()
	{
	}
};

//-----------------------------------------------------------------------
/** One received (or synthetic) player state, stamped with the SENDER's
    clock: seq * kSendPeriodSeconds, unwrapped. */
struct cGhostSample
{
	double mfTSend;          /**< sender time, seconds (seq * send period) */
	hpl::cVector3f mvPos;    /**< feet position (v11 wire semantics) */
	float mfYaw, mfPitch;    /**< radians, FPS view */
	float mfVelFwd;          /**< body-local planar velocity, m/s (decoded) */
	float mfVelRight;
	uint8_t mFlags;          /**< eNetPlayerFlags */
	uint8_t mMoveState;      /**< eNetMoveState */
	bool mbFlashlightOn;

	cGhostSample()
		: mfTSend(0.0)
		  , mvPos(0, 0, 0)
		  , mfYaw(0)
		  , mfPitch(0)
		  , mfVelFwd(0)
		  , mfVelRight(0)
		  , mFlags(0)
		  , mMoveState(0)
		  , mbFlashlightOn(false)
	{
	}
};

//-----------------------------------------------------------------------
/** Remote peer visual only — owns engine lights in the loaded world (not
    cPlayer / not cGameEntity).

    Data flow: ApplyState() ENQUEUES a wire state into a small ring buffer
    stamped with the sender's clock; Update() runs every frame, renders
    ~100 ms behind the newest sample (interpolating position, shortest-arc
    yaw and pitch, extrapolating briefly across gaps, snapping on
    teleports), picks the locomotion clip from the wire velocity/flags and
    crossfades between clips with the engine's FadeIn/FadeOut so the weight
    sum stays 1 (no bind-pose collapse, no double-weighted joints). */
class cGhostPlayer
{
public:
	/** afEyeHeightStand/Crouch: how far the sender's camera sits above its
	    feet — the wire carries FEET, the enemy senses and the lights want
	    the eyes. afBodyYOffset*: extra mesh offsets from the feet (cfg
	    overrides, 0 = mesh origin on the wire feet). apTuning may be NULL. */
	cGhostPlayer(hpl::cWorld3D *apWorld, uint8_t alPlayerID,
		const hpl::tString &asBodyMeshFile, float afBodyYOffsetStand,
		float afBodyYOffsetCrouch, float afEyeHeightStand, float afEyeHeightCrouch,
		const cGhostTuning *apTuning);
	~cGhostPlayer();

	uint8_t GetPlayerID() const { return mlPlayerID; }

	/** Push a wire state into the interpolation buffer (newest-seq only;
	    a stale/duplicate seq is dropped). Nothing is rendered here. */
	void ApplyState(const cNetPlayerState &aState);

	/** Per-frame: interpolate, place body/lights, select + fade clips, scale
	    playback speed. Call once per logic tick with the tick's dt. */
	void Update(float afTimeStep);

	/** Latest received EYE position (feet + eye height; false = no state
	    yet) — the enemy senses use this as the ghost's whereabouts. */
	bool GetLastStatePos(hpl::cVector3f *apOut) const;

	/** v12 party health. Latest received FEET position (raw wire sample,
	    no interpolation; false = no state yet) — the co-op respawn walks
	    the dead player next to this. */
	bool GetLastFeetPos(hpl::cVector3f *apOut) const;
	/** Where the ghost mesh is DRAWN this frame (interpolated feet; false
	    before the first Update resolved a pose) — the world-anchored
	    health bar hangs above this so it tracks the visible body. */
	bool GetRenderFeetPos(hpl::cVector3f *apOut) const;
	/** Sender's health from the newest state, 0 when it flagged Dead
	    (100 until the first state arrives). */
	float GetLastHealth() const { return mbLastDead ? 0.0f : (float)mlLastHealth; }

	/** The world that owned this ghost's entities is gone (map change/unload):
	    drop every pointer WITHOUT destroying through them — the dead cWorld3D
	    already tore the entities down. Call before hplDelete on a world switch. */
	void OrphanWorld();

	/** Preview/debug: force a named clip (crossfaded in; one-shots replay
	    every ~1 s), "" = back to automatic selection. Returns false when the
	    clip is not loaded on this body (nothing changes). */
	bool DebugPlayClip(const hpl::tString &asClip);

	/** The clip table LoadAnimations tries (index order = F6/F7 cycle order). */
	static int GetClipCount();
	static const char *GetClipName(int alIdx);

	/** Built-in gait table (m/s at playback speed 1.0, measured from the
	    shipped phillip clips' root motion); 0 = not a locomotion clip. */
	static float GetDefaultGait(const hpl::tString &asClip);
	/** Effective gait for this ghost: cfg override, else per-model
	    <base>_clips.json, else the built-in table. */
	float GaitFor(const hpl::tString &asClip) const;

	const hpl::tString &GetCurrentClip() const { return msCurrentClip; }

private:
	/** Load the sibling clip DAEs (<base>_idle/_walk/_run/_walk_back/
	    _strafe_walk_l/r/_strafe_run_l/r/_crouch_idle/_crouch_walk/_jump/
	    _stand_to_crouch/_crouch_to_stand/_turn_l/_turn_r) onto the body
	    entity. Failures cost a log line each — with no idle clip the ghost
	    stands at the walk clip's first frame; with no clips at all, at
	    whatever pose the mesh renders (never fatal). */
	void LoadAnimations(const hpl::tString &asBodyMeshFile);
	/** Per-model gait speeds from <base>_clips.json (the exporter's
	    gait_speed_mps per clip); silently keeps the defaults without one. */
	void LoadGaitsFromClipsJson(const hpl::tString &asBodyMeshFile);

	/** Interpolation buffer -> render pose for this frame. Returns false
	    when the buffer is empty. apOut gets the pose + the flags/velocity of
	    the sample at (or just before) the render time. */
	bool ResolveRenderSample(cGhostSample *apOut);

	/** Weight-preserving clip switch (see the class comment): every other
	    active state fades out to 0 exactly when the new one reaches 1.
	    abMatchPhase copies the relative time of the clip being left
	    (gait cycles stay in step). Never calls Play/PlayName/Stop. */
	void CrossfadeTo(const hpl::tString &asClip, bool abLoop, float afFadeSec,
		bool abMatchPhase);

	/** Clip selection from the render sample's velocity/flags (plus the
	    render-speed fallback), one-shot ownership (jump, stance
	    transitions), preview forcing. */
	void SelectLocomotion(const cGhostSample &aSample, float afRenderSpeed,
		float afTimeStep);

	/** Playback speed = locomotion speed / clip gait for every active gait
	    clip (clamped), 1.0 for everything else, 0 for the frozen-walk idle. */
	void ApplyClipSpeeds();

	void AnimLog(const char *asFmt, ...) const;

	hpl::cWorld3D *mpWorld;
	uint8_t mlPlayerID;
	float mfBodyYOffsetStand;   /* cfg override offsets from the wire feet */
	float mfBodyYOffsetCrouch;
	float mfBodyYOffsetCurrent; /* smoothed between the two */
	float mfStanceLerpSeconds;  /* offset lerp window: 0.15 s direct, clip length while a transition plays */
	float mfEyeHeightStand;     /* sender camera height above its feet */
	float mfEyeHeightCrouch;
	hpl::cMeshEntity *mpBodyEntity;
	hpl::cLight3DPoint *mpMarkerLight;
	hpl::cLight3DSpot *mpFlashlight;

	/* tuning (copied from cGhostTuning at creation) */
	float mfInterpDelaySec;
	float mfTurnRateRadPerSec;
	bool mbAnimTrace;
	std::map<hpl::tString, float> m_mapGaits; /* json + cfg overrides */

	/* interpolation buffer (ring, newest at mlSampleHead) */
	static const int kSampleNum = 8;
	cGhostSample mvSamples[kSampleNum];
	int mlSampleHead;
	int mlSampleCount;
	uint16_t mlLastSeq;
	bool mbHaveSeq;
	int64_t mlSeqUnwrapped;
	uint8_t mlLastHealth;     /* v12: newest state's mHealth */
	bool mbLastDead;          /* v12: newest state had eNetPlayerFlag_Dead */

	/* sender clock -> local clock mapping */
	double mfLocalClock;      /* sum of Update dts */
	double mfClockOffset;     /* local - sender, running minimum (jitter-free) */
	bool mbClockKnown;
	double mfClockWinMin;     /* min offset seen in the current 2 s window */
	double mfClockWinStart;

	/* render outputs */
	bool mbRenderValid;
	hpl::cVector3f mvRenderPos;
	float mfRenderYaw;        /* rate-limited body yaw */
	float mfRenderPitch;
	float mfRenderSpeed;      /* planar |dpos|/dt, EMA tau ~0.1 s */
	float mfRenderVelX;       /* planar render velocity (EMA) — direction fallback */
	float mfRenderVelZ;
	bool mbSnapPending;       /* first sample / teleport: no smoothing this frame */

	/* animation driving state */
	bool mbAnimsLoaded;
	hpl::tString msCurrentTarget; /* logical locomotion state ("idle", "run", ...) */
	hpl::tString msCurrentClip;   /* clip actually playing (after missing-clip fallback) */
	hpl::tString msOneShot;       /* one-shot owning the pose ("" = none) */
	hpl::tString msForcedClip;    /* DebugPlayClip target ("" = automatic) */
	float mfForcedOverTime;       /* seconds a forced one-shot has been over */
	bool mbMoving;                /* hysteresis state */
	bool mbRunning;
	int mlMoveSector;             /* eGhostMoveSector — current sector, hysteresis state */
	bool mbHavePrevFlags;
	bool mbPrevOnGround;
	bool mbPrevCrouch;
	float mfAirborneTime;         /* seconds since ground contact was lost */
	bool mbJumpClipFired;         /* once per airborne stretch */
	float mfSinceLanded;          /* seconds since the last landing edge */
	float mfLocomotionSpeed;      /* m/s driving the clip speed scaling */
	float mfLastMoveAngleDeg;     /* for the logs */
	unsigned long mlLastAnimTraceMs; /* 5 s state trace throttle (ghost_anim_trace) */
};
//-----------------------------------------------------------------------

#endif
