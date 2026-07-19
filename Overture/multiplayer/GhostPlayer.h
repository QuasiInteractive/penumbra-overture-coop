#ifndef GHOST_PLAYER_H
#define GHOST_PLAYER_H

#include "StdAfx.h"
#include "NetworkPackets.h"

namespace hpl { class cMeshEntity; }

//-----------------------------------------------------------------------
/** Remote peer visual only — owns engine lights in the loaded world (not cPlayer / not cGameEntity). */
class cGhostPlayer
{
public:
	cGhostPlayer(hpl::cWorld3D *apWorld, uint8_t alPlayerID,
		const hpl::tString &asBodyMeshFile, float afBodyYOffsetFromCamera,
		float afBodyYOffsetFromCameraCrouch);
	~cGhostPlayer();

	uint8_t GetPlayerID() const { return mlPlayerID; }
	void ApplyState(const cNetPlayerState &aState);

	/** The world that owned this ghost's entities is gone (map change/unload):
	    drop every pointer WITHOUT destroying through them — the dead cWorld3D
	    already tore the entities down. Call before hplDelete on a world switch. */
	void OrphanWorld();

private:
	/** Load the sibling clip DAEs (<base>_idle/_walk/_run/_walk_back/
	    _strafe_walk_l/r/_strafe_run_l/r/_crouch_idle/_crouch_walk/_jump/
	    _stand_to_crouch/_crouch_to_stand/_turn_l/_turn_r) onto the body
	    entity. Failures cost a log line each — with no idle clip the ghost
	    stands at the walk clip's first frame; with no clips at all, at
	    whatever pose the mesh renders (never fatal). */
	void LoadAnimations(const hpl::tString &asBodyMeshFile);
	/** Derive locomotion from the wire position stream (no velocity on the
	    wire) plus the synced stance byte for the crouch clips, and switch the
	    body entity's animation state accordingly. Standing locomotion is
	    direction-aware: the measured velocity rotated into the view-yaw local
	    frame picks forward walk/run, strafe, or backpedal clips per sector
	    (hysteresis at the boundaries; missing clips degrade to the forward
	    set). Jump and the two stance transitions are play-once overlays:
	    triggered on stance byte edges, they suppress the locomotion selector
	    until the clip ends, then normal selection resumes. A stance
	    transition also stretches the body Y-offset lerp (mfStanceLerpSeconds)
	    to the clip's duration so height and pose change together. */
	void UpdateAnimationFromMovement(const hpl::cVector3f &avPos, float afYaw,
		bool abCrouch, bool abJump);

	hpl::cWorld3D *mpWorld;
	uint8_t mlPlayerID;
	float mfBodyYOffsetFromCamera;        /* standing */
	float mfBodyYOffsetFromCameraCrouch;  /* crouched: camera drops, feet don't */
	float mfBodyYOffsetCurrent;           /* smoothed between the two */
	float mfStanceLerpSeconds;            /* offset lerp window: 0.15 s direct, clip length while a transition plays */
	unsigned long mlLastStanceTimeMs;
	hpl::cMeshEntity *mpBodyEntity;
	hpl::cLight3DPoint *mpMarkerLight;
	hpl::cLight3DSpot *mpFlashlight;

	/* animation driving state */
	bool mbAnimsLoaded;
	hpl::tString msCurrentAnim;
	bool mbPrevJumpState;   /* last packet's jump flag — triggers on rising edge only */
	bool mbPrevCrouchState; /* last packet's crouch flag — stance transitions fire on edges */
	bool mbHasPrevCrouchState; /* no transition on the very first packet */
	bool mbHasPrevPos;
	hpl::cVector3f mvPrevPos;
	unsigned long mlPrevPosTimeMs;
	float mfSmoothedSpeed;
	float mfSmoothedVelX;  /* smoothed planar wire velocity — the direction */
	float mfSmoothedVelZ;  /* source for locomotion sector selection */
	int mlMoveSector;      /* eGhostMoveSector — current sector, hysteresis state */
	unsigned long mlLastAnimTraceMs; /* activeAnims diagnosis trace throttle */
};
//-----------------------------------------------------------------------

#endif
