/*
 * Ghost = optional decorative mesh + cyan body marker + weak spotlight aligned to remote pitch/yaw (roll = 0 on wire).
 *
 * The ghost meshes are experimental assimp-converted COLLADA and the engine
 * trusts mesh data completely, so everything is validated HERE: a bad file
 * must cost one hpl.log line and a marker-light-only ghost — never the host.
 */
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "StdAfx.h"
#include "GhostPlayer.h"
#include "graphics/Mesh.h"
#include "graphics/SubMesh.h"
#include "graphics/Skeleton.h"
#include "graphics/VertexBuffer.h"
#include "graphics/Animation.h"
#include "resources/MeshManager.h"
#include "resources/AnimationManager.h"
#include "resources/Resources.h"
#include "scene/MeshEntity.h"
#include "scene/AnimationState.h"
#include "scene/World3D.h"
#include "system/String.h"
#include "system/LowLevelSystem.h"
#include "math/Math.h"

#include <cstdio>
#include <cmath>

using namespace hpl;

//-----------------------------------------------------------------------

namespace
{

/* cMeshManager::CreateMesh does not survive every bad DAE: the host crash of
   2026-07-13 was an access violation INSIDE the loader (LoadControllerVec) —
   it never got the chance to return NULL for that failure mode. SEH is the
   only mod-side way to keep the host alive; the engine may leak the aborted
   load, which beats dying. No C++ objects may live in this function (C2712),
   so the caller owns all strings. */
static cMesh *CreateMeshGuarded(cMeshManager *apMeshManager, const tString &asFile,
	unsigned long *apExceptionOut)
{
	*apExceptionOut = 0;
#if defined(_MSC_VER)
	__try
	{
		return apMeshManager->CreateMesh(asFile);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*apExceptionOut = (unsigned long)GetExceptionCode();
		return NULL;
	}
#else
	return apMeshManager->CreateMesh(asFile);
#endif
}

/* Same rationale as CreateMeshGuarded: LoadAnimation runs the identical
   collada parse path (LoadControllerVec included), so a bad clip file must
   cost a log line, not the process. No C++ objects in here (C2712). */
static cAnimation *CreateAnimationGuarded(cAnimationManager *apAnimManager, const tString &asFile,
	unsigned long *apExceptionOut)
{
	*apExceptionOut = 0;
#if defined(_MSC_VER)
	__try
	{
		return apAnimManager->CreateAnimation(asFile);
	}
	__except (EXCEPTION_EXECUTE_HANDLER)
	{
		*apExceptionOut = (unsigned long)GetExceptionCode();
		return NULL;
	}
#else
	return apAnimManager->CreateAnimation(asFile);
#endif
}

/* cSubMeshEntity::UpdateGraphics software-skins EVERY submesh of a mesh that
   has a skeleton and assumes compiled vertex weights with in-range bone
   indices. assimp output breaks both: a controller that matches no geometry
   leaves cSubMesh::mpVertexWeights NULL, and a joint name missing from the
   skeleton becomes bone index -1 (255 once truncated to a byte) — either way
   the host dies per-frame in the renderer where nothing can catch it. So
   reject such meshes while they are still only data. */
static bool GhostMeshIsRenderSafe(cMesh *apMesh, char *apReasonOut, size_t alReasonSize)
{
	cSkeleton *pSkeleton = apMesh->GetSkeleton();

	for (int sub = 0; sub < apMesh->GetSubMeshNum(); ++sub)
	{
		cSubMesh *pSubMesh = apMesh->GetSubMesh(sub);
		iVertexBuffer *pVtxBuffer = pSubMesh ? pSubMesh->GetVertexBuffer() : NULL;
		if (pVtxBuffer == NULL)
		{
			snprintf(apReasonOut, alReasonSize, "submesh %d has no vertex buffer", sub);
			return false;
		}

		/* The loader leaves the material NULL when the DAE's triangles
		   material attribute or image file name resolves to no .mat — the
		   renderer then dies in cRenderList::Add on the first frame. */
		if (pSubMesh->GetMaterial() == NULL)
		{
			snprintf(apReasonOut, alReasonSize,
				"submesh %d has no material (triangles material attribute / image init_from must resolve to a .mat)", sub);
			return false;
		}

		if (pSkeleton == NULL)
			continue; /* no skeleton -> the skinning path never runs */

		if (pVtxBuffer->GetArray(eVertexFlag_Position) == NULL ||
			pVtxBuffer->GetArray(eVertexFlag_Normal) == NULL ||
			pVtxBuffer->GetArray(eVertexFlag_Texture1) == NULL)
		{
			snprintf(apReasonOut, alReasonSize,
				"skinned submesh %d lacks position/normal/tangent arrays", sub);
			return false;
		}

		const int lPairNum = pSubMesh->GetVertexBonePairNum();
		if (lPairNum == 0)
		{
			snprintf(apReasonOut, alReasonSize,
				"submesh %d has a skeleton but no vertex weights (controller/geometry target mismatch)", sub);
			return false;
		}

		const unsigned int lBoneNum = (unsigned int)pSkeleton->GetBoneNum();
		const unsigned int lVtxNum = (unsigned int)pVtxBuffer->GetVertexNum();
		for (int i = 0; i < lPairNum; ++i)
		{
			const cVertexBonePair &Pair = pSubMesh->GetVertexBonePair(i);
			if (Pair.vtxIdx >= lVtxNum ||
				(Pair.weight != 0 && (Pair.boneIdx >= lBoneNum || Pair.boneIdx > 255)))
			{
				snprintf(apReasonOut, alReasonSize,
					"submesh %d vertex-bone pair %d out of range (vtx %u of %u, bone %u of %u — unmatched joint name?)",
					sub, i, Pair.vtxIdx, lVtxNum, Pair.boneIdx, lBoneNum);
				return false;
			}
		}
	}

	return true;
}

/* Locomotion state -> clip name + playback speed. Standing idle prefers the
   real idle clip (new asset sets ship <base>_idle.dae); without one it falls
   back to the walk clip paused at its first frame. */
static void ResolveGhostClip(cMeshEntity *apBody, const tString &asTarget,
	tString &asClipOut, float &afSpeedOut)
{
	if (asTarget == "idle" && apBody->GetAnimationStateFromName("idle") == NULL)
	{
		asClipOut = "walk";
		afSpeedOut = 0.0f;
		return;
	}
	asClipOut = asTarget;
	afSpeedOut = 1.0f;
}

/* Play-once clips: they own the pose until they finish, then the locomotion
   selector reclaims the ghost. Never a valid selector target, which is what
   keeps the no-change early-out from trapping the final frame. */
static bool IsOneShotAnim(const tString &asName)
{
	return asName == "jump" || asName == "stand_to_crouch" ||
		asName == "crouch_to_stand";
}

/* Locomotion direction sectors, in the ghost's local (view-yaw) frame. */
enum eGhostMoveSector
{
	eGhostMoveSector_Forward = 0,
	eGhostMoveSector_StrafeR,
	eGhostMoveSector_StrafeL,
	eGhostMoveSector_Back
};

/* Sector layout over the movement angle (0 = moving where the view faces,
   positive = toward the view's right): forward within +-50 deg, strafes
   50..130, backpedal beyond 130. The CURRENT sector keeps ownership 15 deg
   past its nominal edge, so wiggling the mouse at a boundary reselects the
   same sector instead of flickering clips. */
static int ClassifyMoveSector(float afMoveAngleDeg, int alCurrentSector)
{
	const float kFwdEdge = 50.0f;
	const float kBackEdge = 130.0f;
	const float kHyst = 15.0f;
	const float a = afMoveAngleDeg;
	const float fAbs = fabsf(a);

	switch (alCurrentSector)
	{
	case eGhostMoveSector_Forward:
		if (fAbs < kFwdEdge + kHyst) return alCurrentSector;
		break;
	case eGhostMoveSector_Back:
		if (fAbs > kBackEdge - kHyst) return alCurrentSector;
		break;
	case eGhostMoveSector_StrafeR:
		if (a > kFwdEdge - kHyst && a < kBackEdge + kHyst) return alCurrentSector;
		break;
	case eGhostMoveSector_StrafeL:
		if (a < -(kFwdEdge - kHyst) && a > -(kBackEdge + kHyst)) return alCurrentSector;
		break;
	}

	if (fAbs < kFwdEdge) return eGhostMoveSector_Forward;
	if (fAbs > kBackEdge) return eGhostMoveSector_Back;
	return (a > 0.0f) ? eGhostMoveSector_StrafeR : eGhostMoveSector_StrafeL;
}

/* Missing-clip degradation, one step per call: every directional clip falls
   back toward the forward set (strafe runs to run, everything walk-ish to
   walk), run to walk, crouch_idle to idle — chained by the caller until the
   resolved clip exists. "walk" maps to itself, which terminates the chain. */
static tString FallbackAnimTarget(const tString &asTarget)
{
	if (asTarget == "strafe_run_l" || asTarget == "strafe_run_r")
		return "run";
	if (asTarget == "crouch_idle")
		return "idle";
	return "walk";
}

}

//-----------------------------------------------------------------------

cGhostPlayer::cGhostPlayer(cWorld3D *apWorld, uint8_t alPlayerID, const tString &asBodyMeshFile,
	float afBodyYOffsetFromCamera, float afBodyYOffsetFromCameraCrouch)
{
	mpWorld = apWorld;
	mlPlayerID = alPlayerID;
	mpBodyEntity = NULL;
	mpMarkerLight = NULL;
	mpFlashlight = NULL;
	mfBodyYOffsetFromCamera = afBodyYOffsetFromCamera;
	mfBodyYOffsetFromCameraCrouch = afBodyYOffsetFromCameraCrouch;
	mfBodyYOffsetCurrent = afBodyYOffsetFromCamera;
	mfStanceLerpSeconds = 0.15f;
	mlLastStanceTimeMs = 0;

	mbAnimsLoaded = false;
	msCurrentAnim = "";
	mbPrevJumpState = false;
	mbPrevCrouchState = false;
	mbHasPrevCrouchState = false;
	mbHasPrevPos = false;
	mvPrevPos = cVector3f(0, 0, 0);
	mlPrevPosTimeMs = 0;
	mfSmoothedSpeed = 0.0f;
	mfSmoothedVelX = 0.0f;
	mfSmoothedVelZ = 0.0f;
	mlMoveSector = eGhostMoveSector_Forward;
	mlLastAnimTraceMs = 0;

	if (!mpWorld)
		return;

	if (asBodyMeshFile.empty() == false)
	{
		cMeshManager *pMeshManager = mpWorld->GetResources()->GetMeshManager();

		unsigned long lException = 0;
		cMesh *pMesh = CreateMeshGuarded(pMeshManager, asBodyMeshFile, &lException);

		if (pMesh == NULL)
		{
			if (lException != 0)
				Log(" multiplayer: GhostPlayer id=%u mesh '%s' CRASHED the loader (exception 0x%08lX) — using marker light only, fix/reconvert the DAE\n",
					(unsigned)mlPlayerID, asBodyMeshFile.c_str(), lException);
			else
				Log(" multiplayer: GhostPlayer id=%u failed to load mesh '%s' — using marker light only\n",
					(unsigned)mlPlayerID, asBodyMeshFile.c_str());
		}
		else
		{
			char sReason[256];
			sReason[0] = '\0';
			if (GhostMeshIsRenderSafe(pMesh, sReason, sizeof(sReason)) == false)
			{
				Log(" multiplayer: GhostPlayer id=%u rejected mesh '%s': %s — using marker light only, fix/reconvert the DAE\n",
					(unsigned)mlPlayerID, asBodyMeshFile.c_str(), sReason);
				pMeshManager->Destroy(pMesh);
				pMesh = NULL;
			}
		}

		if (pMesh)
		{
			tString sEntity = "GhostBody_" + cString::ToString((int)mlPlayerID);
			mpBodyEntity = mpWorld->CreateMeshEntity(sEntity, pMesh);
			if (mpBodyEntity)
			{
				mpBodyEntity->SetCastsShadows(false);

				/* Skinned body: load the movement clips so the skeleton is
				   driven by animation. Without any active animation state the
				   engine renders a skinned mesh at bind pose (raw geometry). */
				if (pMesh->GetSkeleton())
					LoadAnimations(asBodyMeshFile);
			}
			else
				Log(" multiplayer: GhostPlayer id=%u could not create body entity for '%s' — using marker light only\n",
					(unsigned)mlPlayerID, asBodyMeshFile.c_str());
		}
	}

	tString sMarker = "GhostMarker_" + cString::ToString((int)mlPlayerID);
	mpMarkerLight = mpWorld->CreateLightPoint(sMarker, true);
	if (mpMarkerLight)
	{
		mpMarkerLight->SetDiffuseColor(cColor(0.25f, 0.95f, 0.95f, 1.0f));
		mpMarkerLight->SetFarAttenuation(2.25f);
		mpMarkerLight->SetCastShadows(false);
		mpMarkerLight->SetVisible(true);
	}

	tString sFlash = "GhostFlash_" + cString::ToString((int)mlPlayerID);
	mpFlashlight = mpWorld->CreateLightSpot(sFlash, "", true);
	if (mpFlashlight)
	{
		mpFlashlight->SetDiffuseColor(cColor(0.55f, 0.55f, 0.45f, 1.0f));
		mpFlashlight->SetFarAttenuation(10.0f);
		mpFlashlight->SetFOV(cMath::ToRad(65));
		mpFlashlight->SetAspect(1.0f);
		mpFlashlight->SetNearClipPlane(0.12f);
		mpFlashlight->SetCastShadows(false);
		mpFlashlight->SetVisible(false);
	}
}

//-----------------------------------------------------------------------

cGhostPlayer::~cGhostPlayer()
{
	if (mpWorld)
	{
		if (mpFlashlight)
			mpWorld->DestroyLight(mpFlashlight);
		if (mpMarkerLight)
			mpWorld->DestroyLight(mpMarkerLight);
		if (mpBodyEntity)
			mpWorld->DestroyMeshEntity(mpBodyEntity);
	}
	mpBodyEntity = NULL;
	mpFlashlight = NULL;
	mpMarkerLight = NULL;
	mpWorld = NULL;
}

//-----------------------------------------------------------------------

void cGhostPlayer::OrphanWorld()
{
	mpBodyEntity = NULL;
	mpFlashlight = NULL;
	mpMarkerLight = NULL;
	mpWorld = NULL;
}

//-----------------------------------------------------------------------

void cGhostPlayer::LoadAnimations(const tString &asBodyMeshFile)
{
	/* turn_l/turn_r load now but nothing drives them yet — turn-in-place is
	   the next rung; a loaded-but-unplayed clip costs nothing. */
	static const char *kpClipNames[] =
		{ "idle", "walk", "run", "walk_back",
		  "strafe_walk_l", "strafe_walk_r", "strafe_run_l", "strafe_run_r",
		  "crouch_idle", "crouch_walk", "jump",
		  "stand_to_crouch", "crouch_to_stand",
		  "turn_l", "turn_r" };
	const int kClipNum = 15;

	cAnimationManager *pAnimManager = mpWorld->GetResources()->GetAnimationManager();
	const tString sBase = cString::SetFileExt(asBodyMeshFile, "");

	int lLoaded = 0;
	for (int i = 0; i < kClipNum; ++i)
	{
		tString sFile = sBase + "_" + kpClipNames[i] + ".dae";

		unsigned long lException = 0;
		cAnimation *pAnim = CreateAnimationGuarded(pAnimManager, sFile, &lException);
		if (pAnim == NULL)
		{
			if (lException != 0)
				Log(" multiplayer: GhostPlayer id=%u clip '%s' CRASHED the loader (exception 0x%08lX) — clip skipped\n",
					(unsigned)mlPlayerID, sFile.c_str(), lException);
			else
				Log(" multiplayer: GhostPlayer id=%u failed to load clip '%s' — clip skipped\n",
					(unsigned)mlPlayerID, sFile.c_str());
			continue;
		}

		/* TIME UNITS CONTRACT: the loader converts frame-number key times to
		   SECONDS at load (CreateAnimTrack, docs without HPL's <extra> scene
		   block are frames @30fps), so clip lengths and time positions are
		   seconds and the base speed here is 1.0 — real time. The old scheme
		   (frame units in keys + base speed 30 here) is gone; reintroducing
		   either half alone plays everything 30x off. */
		mpBodyEntity->AddAnimation(pAnim, kpClipNames[i], 1.0f);
		++lLoaded;
	}

	Log(" multiplayer: GhostPlayer id=%u loaded %d/%d animation clips for '%s'\n",
		(unsigned)mlPlayerID, lLoaded, kClipNum, asBodyMeshFile.c_str());

	/* Idle stance until the first movement. New asset sets ship a real looping
	   <base>_idle.dae; old sets fall back to the walk clip frozen at its first
	   frame (verified clean standing pose). With neither clip the mesh just
	   renders unanimated. */
	cAnimationState *pIdle = mpBodyEntity->GetAnimationStateFromName("idle");
	cAnimationState *pWalk = mpBodyEntity->GetAnimationStateFromName("walk");
	if (pIdle)
	{
		mbAnimsLoaded = true;
		mpBodyEntity->PlayName("idle", true, true);
		msCurrentAnim = "idle";
	}
	else if (pWalk)
	{
		mbAnimsLoaded = true;
		mpBodyEntity->PlayName("walk", true, true);
		pWalk->SetSpeed(0.0f);
		msCurrentAnim = "idle";
	}
}

//-----------------------------------------------------------------------

void cGhostPlayer::UpdateAnimationFromMovement(const cVector3f &avPos, float afYaw,
	bool abCrouch, bool abJump)
{
	if (mbAnimsLoaded == false || mpBodyEntity == NULL)
		return;

	const unsigned long lNow = GetApplicationTime();

	/* Is a play-once clip currently owning the pose? A finished non-loop clip
	   clamps at its LAST frame and STAYS active — the engine never hands
	   control back, so the end is detected here and the selector reclaims the
	   ghost (a one-shot name never equals a locomotion target, so the
	   no-change early-out cannot trap us in the frozen pose). */
	bool bOneShotPlaying = false;
	if (IsOneShotAnim(msCurrentAnim))
	{
		cAnimationState *pShot = mpBodyEntity->GetAnimationStateFromName(msCurrentAnim);
		bOneShotPlaying = (pShot && pShot->IsActive() && pShot->IsOver() == false);
	}

	/* --- jump: play-once clip, triggered on the stance byte's rising edge ---
	   The wire says Jump for the whole airborne stretch; only the first packet
	   of a stretch (re)starts the clip. PlayName rewinds to frame 0, so a
	   spammed jump restarts the clip cleanly instead of sticking at its end. */
	const bool bJumpRise = (abJump && mbPrevJumpState == false);
	mbPrevJumpState = abJump;

	/* --- stance transitions: play-once on the crouch flag's edges ---------- */
	const bool bCrouchEdge = (mbHasPrevCrouchState && abCrouch != mbPrevCrouchState);
	mbHasPrevCrouchState = true;
	mbPrevCrouchState = abCrouch;

	if (bJumpRise)
	{
		cAnimationState *pJump = mpBodyEntity->GetAnimationStateFromName("jump");
		if (pJump)
		{
			mpBodyEntity->PlayName("jump", false, true); /* play ONCE, stop others */
			pJump->SetSpeed(1.0f);
			Log(" multiplayer: GhostPlayer id=%u anim '%s' -> 'jump' (play once, measured %.2f m/s)\n",
				(unsigned)mlPlayerID, msCurrentAnim.c_str(), mfSmoothedSpeed);
			msCurrentAnim = "jump";
			bOneShotPlaying = true;
		}
		/* no jump clip loaded: nothing to trigger — the selector below keeps
		   deriving idle/walk/run from measured speed, as before jump support */
	}
	else if (bCrouchEdge && !(msCurrentAnim == "jump" && bOneShotPlaying))
	{
		const char *pTransName = abCrouch ? "stand_to_crouch" : "crouch_to_stand";
		cAnimationState *pTrans = mpBodyEntity->GetAnimationStateFromName(pTransName);

		/* The transition clips are authored in place — playing one while the
		   remote is measurably moving renders a slide, which looks worse than
		   a straight cut to the crouch/stand locomotion clip. */
		const bool bTooFast = (mfSmoothedSpeed >= 0.45f);

		if (pTrans && bTooFast == false)
		{
			mpBodyEntity->PlayName(pTransName, false, true); /* play ONCE, stop others */
			pTrans->SetSpeed(1.0f);

			/* Height and pose must arrive together: stretch the body Y-offset
			   lerp over exactly the clip's duration. Length is SECONDS now
			   (loader converts at load) and base speed is 1.0 — the divide
			   stays so a deliberate future base-speed change keeps working. */
			float fBase = pTrans->GetBaseSpeed();
			if (fBase <= 0.0f)
				fBase = 1.0f;
			mfStanceLerpSeconds = pTrans->GetLength() / fBase;
			if (mfStanceLerpSeconds < 0.15f) mfStanceLerpSeconds = 0.15f;
			else if (mfStanceLerpSeconds > 2.0f) mfStanceLerpSeconds = 2.0f;

			Log(" multiplayer: GhostPlayer id=%u anim '%s' -> '%s' (play once, %.2f s, measured %.2f m/s)\n",
				(unsigned)mlPlayerID, msCurrentAnim.c_str(), pTransName,
				mfStanceLerpSeconds, mfSmoothedSpeed);
			msCurrentAnim = pTransName;
			bOneShotPlaying = true;
		}
		else
		{
			/* Direct cut (clip missing or moving): quick offset lerp, and if
			   the OPPOSITE transition is still playing (crouch spam), mark it
			   ended so the selector reclaims the pose this very packet — the
			   "one-shot name + not playing" combination below forces an
			   immediate reselect. */
			mfStanceLerpSeconds = 0.15f;
			if (bTooFast && pTrans)
				Log(" multiplayer: GhostPlayer id=%u skipping '%s' (moving %.2f m/s) — direct cut\n",
					(unsigned)mlPlayerID, pTransName, mfSmoothedSpeed);
			if (msCurrentAnim == "stand_to_crouch" || msCurrentAnim == "crouch_to_stand")
				bOneShotPlaying = false;
		}
	}

	if (mbHasPrevPos == false)
	{
		mbHasPrevPos = true;
		mvPrevPos = avPos;
		mlPrevPosTimeMs = lNow;
		return;
	}

	const float fDt = (float)(lNow - mlPrevPosTimeMs) / 1000.0f;
	if (fDt >= 0.12f)
	{
		/* Measurement keeps running during a one-shot too, so landing/standing
		   resumes on a fresh speed instead of a stale one. */
		const float fDx = avPos.x - mvPrevPos.x;
		const float fDz = avPos.z - mvPrevPos.z;
		float fSpeed = sqrtf(fDx * fDx + fDz * fDz) / fDt;
		float fVelX = fDx / fDt;
		float fVelZ = fDz / fDt;

		/* a long silence (map change, packet loss burst) or a teleport-sized jump
		   is not a movement measurement */
		if (fDt > 0.5f || fSpeed > 15.0f)
		{
			fSpeed = 0.0f;
			fVelX = 0.0f;
			fVelZ = 0.0f;
		}

		mvPrevPos = avPos;
		mlPrevPosTimeMs = lNow;

		mfSmoothedSpeed = mfSmoothedSpeed * 0.6f + fSpeed * 0.4f;
		mfSmoothedVelX = mfSmoothedVelX * 0.6f + fVelX * 0.4f;
		mfSmoothedVelZ = mfSmoothedVelZ * 0.6f + fVelZ * 0.4f;
	}
	else if (bOneShotPlaying || IsOneShotAnim(msCurrentAnim) == false)
	{
		return; /* keep accumulating — per-packet windows measured jittery
		           speeds (0.9 -> 0.1 m/s while steadily walking), flapping
		           the walk/idle states. prev pos/time deliberately not
		           updated here, so the window keeps growing until 0.12 s. */
	}
	/* else: the one-shot ended mid-window — reselect NOW from the last
	   smoothed speed instead of holding the final frame for up to 0.12 s */

	if (bOneShotPlaying)
		return; /* locomotion selector hands off while a play-once clip runs */

	/* Thresholds calibrated against MEASURED wire speeds (hpl.log
	   'anim ... measured X m/s'): steady walking reads only ~0.8-0.9 m/s,
	   so running should read ~1.6-2 and crouch-walking ~0.4. Enter
	   thresholds sit above leave thresholds — flapping across a single
	   boundary would toggle the clip several times a second. */
	const bool bWasIdle = (msCurrentAnim == "idle" || msCurrentAnim == "crouch_idle");
	const float fMoveEnter = abCrouch ? 0.22f : 0.40f;
	const float fMoveLeave = abCrouch ? 0.10f : 0.18f;
	const bool bMoving = mfSmoothedSpeed >= (bWasIdle ? fMoveEnter : fMoveLeave);

	/* Movement direction in the ghost's local frame: the smoothed wire
	   velocity projected on the view yaw's forward/right axes (the engine's
	   camera move matrix: forward = (-sin y, 0, -cos y), right =
	   (cos y, 0, -sin y)). Angle 0 = moving where the view faces, positive =
	   toward the view's right. Below the movement threshold the direction is
	   estimation noise, so the sector only updates while measurably moving —
	   the last sector wins otherwise. */
	float fMoveAngleDeg = 0.0f;
	if (bMoving)
	{
		const float fSinY = sinf(afYaw);
		const float fCosY = cosf(afYaw);
		const float fFwd = -mfSmoothedVelX * fSinY - mfSmoothedVelZ * fCosY;
		const float fLat = mfSmoothedVelX * fCosY - mfSmoothedVelZ * fSinY;
		fMoveAngleDeg = atan2f(fLat, fFwd) * (180.0f / kPif);
		mlMoveSector = ClassifyMoveSector(fMoveAngleDeg, mlMoveSector);
	}

	tString sTarget;
	if (abCrouch)
	{
		/* crouch overrides direction — no directional crouch clips */
		sTarget = bMoving ? "crouch_walk" : "crouch_idle";
	}
	else if (bMoving == false)
		sTarget = "idle";
	else
	{
		/* walk/run split with enter/leave hysteresis, shared by the forward
		   and strafe sectors (backpedal has no run clip) */
		const bool bWasRun = (msCurrentAnim == "run" ||
			msCurrentAnim == "strafe_run_l" || msCurrentAnim == "strafe_run_r");
		const bool bRun = mfSmoothedSpeed >= (bWasRun ? 1.15f : 1.40f);

		switch (mlMoveSector)
		{
		case eGhostMoveSector_StrafeR:
			sTarget = bRun ? "strafe_run_r" : "strafe_walk_r";
			break;
		case eGhostMoveSector_StrafeL:
			sTarget = bRun ? "strafe_run_l" : "strafe_walk_l";
			break;
		case eGhostMoveSector_Back:
			sTarget = "walk_back";
			break;
		default:
			sTarget = bRun ? "run" : "walk";
			break;
		}
	}

	/* Map state to clip. Standing idle prefers the real idle clip (new asset
	   sets); without one it falls back to the walk clip paused at frame 0.
	   crouch_idle is a real looping clip. */
	tString sClip;
	float fClipSpeed;
	ResolveGhostClip(mpBodyEntity, sTarget, sClip, fClipSpeed);

	/* Normalize missing clips BEFORE the changed-check, or a state whose clip
	   failed to load would restart the fallback clip on every packet. Chained:
	   a missing strafe run degrades to run, which may itself degrade to walk. */
	for (int lStep = 0;
		mpBodyEntity->GetAnimationStateFromName(sClip) == NULL && lStep < 3;
		++lStep)
	{
		const tString sFallback = FallbackAnimTarget(sTarget);
		if (sFallback == sTarget)
			break;
		sTarget = sFallback;
		ResolveGhostClip(mpBodyEntity, sTarget, sClip, fClipSpeed);
	}

	/* No run_back clip: backpedaling at run speed plays walk_back scaled up
	   (measured walk gait is ~0.9 m/s at speed 1.0), capped at x1.5. */
	if (sClip == "walk_back")
	{
		fClipSpeed = mfSmoothedSpeed / 0.9f;
		if (fClipSpeed < 1.0f) fClipSpeed = 1.0f;
		else if (fClipSpeed > 1.5f) fClipSpeed = 1.5f;
	}

	cAnimationState *pClip = mpBodyEntity->GetAnimationStateFromName(sClip);
	if (pClip == NULL)
		return; /* keep whatever is playing rather than warn-spam per packet */

	if (sTarget == msCurrentAnim)
	{
		/* Same state: never PlayName (it rewinds to frame 0) — but keep the
		   speed tracking, walk_back rescales with the measured speed. */
		pClip->SetSpeed(fClipSpeed);
		return;
	}

	/* Same clip, different speed (idle<->walk): only change the speed. A
	   PlayName here would rewind to frame 0 on every transition. */
	if (pClip->IsActive() == false)
		mpBodyEntity->PlayName(sClip, true, true);
	pClip->SetSpeed(fClipSpeed);

	Log(" multiplayer: GhostPlayer id=%u anim '%s' -> '%s' (clip '%s' speed %.2f, measured %.2f m/s, dir %.0f deg)\n",
		(unsigned)mlPlayerID, msCurrentAnim.c_str(), sTarget.c_str(),
		sClip.c_str(), fClipSpeed, mfSmoothedSpeed, fMoveAngleDeg);

	msCurrentAnim = sTarget;
}

//-----------------------------------------------------------------------

void cGhostPlayer::ApplyState(const cNetPlayerState &aState)
{
	const cVector3f vPos(aState.mfPosX, aState.mfPosY, aState.mfPosZ);

	const bool bCrouch = (aState.mMoveState == (uint8_t)eNetMoveState_Crouch);
	const bool bJump = (aState.mMoveState == (uint8_t)eNetMoveState_Jump);

	if (mpBodyEntity)
	{
		/* The converted models' forward axis is opposite the camera yaw
		   convention — one fixed half-turn corrects the facing. */
		const float kMeshYawOffset = kPif;

		/* Anim update FIRST: a stance transition trigger stretches
		   mfStanceLerpSeconds to the clip's duration, and the offset lerp
		   below must run over that same window from this very packet. */
		UpdateAnimationFromMovement(vPos, aState.mfYaw, bCrouch, bJump);

		/* Four-numbers diagnosis trace (with 'loaded N/M', the loader-trace
		   clip lengths and the engine's M-I skin trace): activeAnims=0 is the
		   ONLY state that renders the skin bind pose — anything else playing
		   wrong is a units or retarget problem, not a selection problem.
		   Keep until the units migration is verified in-game, then gate. */
		{
			const unsigned long lTraceNow = GetApplicationTime();
			if (lTraceNow - mlLastAnimTraceMs >= 5000)
			{
				mlLastAnimTraceMs = lTraceNow;
				int lActive = 0;
				cAnimationState *pFirstActive = NULL;
				const int lStateNum = mpBodyEntity->GetAnimationStateNum();
				for (int i = 0; i < lStateNum; ++i)
				{
					cAnimationState *pS = mpBodyEntity->GetAnimationState(i);
					if (pS && pS->IsActive())
					{
						++lActive;
						if (pFirstActive == NULL) pFirstActive = pS;
					}
				}
				if (pFirstActive)
					Log(" multiplayer: GhostPlayer id=%u activeAnims=%d/%d state='%s' clip t=%.2f len=%.2f s speed=%.2f base=%.2f\n",
						(unsigned)mlPlayerID, lActive, lStateNum, msCurrentAnim.c_str(),
						pFirstActive->GetTimePosition(), pFirstActive->GetLength(),
						pFirstActive->GetSpeed(), pFirstActive->GetBaseSpeed());
				else
					Log(" multiplayer: GhostPlayer id=%u activeAnims=0/%d state='%s' — NOTHING PLAYING, skin renders bind pose\n",
						(unsigned)mlPlayerID, lStateNum, msCurrentAnim.c_str());
			}
		}

		/* Stance-aware Y offset: crouching drops the remote CAMERA ~0.7 m but
		   not their feet — a fixed offset sank the model through the floor.
		   Slide between the two offsets over mfStanceLerpSeconds (0.15 s on a
		   direct cut, the transition clip's length when one is playing) so
		   height and pose arrive together. */
		const float fTargetOffset =
			bCrouch ? mfBodyYOffsetFromCameraCrouch : mfBodyYOffsetFromCamera;
		const unsigned long lNow = GetApplicationTime();
		float fDt = (mlLastStanceTimeMs == 0)
			? 1.0f /* first state: snap */
			: (float)(lNow - mlLastStanceTimeMs) / 1000.0f;
		mlLastStanceTimeMs = lNow;
		if (fDt > 0.25f) fDt = 0.25f;
		float fLerpWindow = mfStanceLerpSeconds;
		if (fLerpWindow < 0.05f) fLerpWindow = 0.05f;
		const float fMaxStep =
			fabsf(mfBodyYOffsetFromCameraCrouch - mfBodyYOffsetFromCamera) * (fDt / fLerpWindow);
		float fDiff = fTargetOffset - mfBodyYOffsetCurrent;
		if (fDiff > fMaxStep) fDiff = fMaxStep;
		else if (fDiff < -fMaxStep) fDiff = -fMaxStep;
		mfBodyYOffsetCurrent += fDiff;

		const cVector3f vBody(
			vPos.x, vPos.y + mfBodyYOffsetCurrent, vPos.z);
		const cMatrixf mtx = cMath::MatrixMul(cMath::MatrixTranslate(vBody),
			cMath::MatrixRotateY(aState.mfYaw + kMeshYawOffset));
		mpBodyEntity->SetWorldMatrix(mtx);
	}

	if (mpMarkerLight)
		mpMarkerLight->SetPosition(vPos);

	if (mpFlashlight)
	{
		/* mfRoll not synced in Phase 1 — ghost aim uses pitch/yaw only */
		const float roll = 0.0f;
		cMatrixf mtx = cMath::MatrixMul(
			cMath::MatrixTranslate(vPos),
			cMath::MatrixRotate(cVector3f(aState.mfPitch, aState.mfYaw, roll), eEulerRotationOrder_XYZ));
		mpFlashlight->SetMatrix(mtx);
		mpFlashlight->SetVisible(aState.mbFlashlightOn != 0);
	}
}

//-----------------------------------------------------------------------
