/*
 * Ghost = optional decorative mesh + cyan body marker + weak spotlight aligned to remote pitch/yaw (roll = 0 on wire).
 *
 * The ghost meshes are experimental assimp-converted COLLADA and the engine
 * trusts mesh data completely, so everything is validated HERE: a bad file
 * must cost one hpl.log line and a marker-light-only ghost — never the host.
 *
 * Runtime (v11): received states go into a ring buffer stamped with the
 * SENDER's clock (mSeq * kNetSendPeriodSeconds); Update() runs every logic
 * tick, renders ~100 ms behind the newest sample (interpolated position,
 * shortest-arc yaw), extrapolates briefly across gaps, snaps on teleports,
 * selects the clip from the wire velocity/flags and crossfades with
 * FadeIn/FadeOut so the animation weight sum stays exactly 1.
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
#include "resources/FileSearcher.h"
#include "resources/Resources.h"
#include "scene/MeshEntity.h"
#include "scene/AnimationState.h"
#include "scene/World3D.h"
#include "system/String.h"
#include "system/LowLevelSystem.h"
#include "math/Math.h"

#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <cmath>
#include <string>

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

/* The clip table: file suffixes tried by LoadAnimations, state names on the
   body entity, F6/F7 preview cycle order. turn_l/turn_r load but nothing
   drives them yet — turn-in-place is a later rung. */
static const char *kpClipNames[] =
	{ "idle", "walk", "run", "walk_back",
	  "strafe_walk_l", "strafe_walk_r", "strafe_run_l", "strafe_run_r",
	  "crouch_idle", "crouch_walk", "jump",
	  "stand_to_crouch", "crouch_to_stand",
	  "turn_l", "turn_r" };
static const int kClipNum = (int)(sizeof(kpClipNames) / sizeof(kpClipNames[0]));

/* ONE gait table: metres per second the character covers at playback speed
   1.0, i.e. the hips' XZ drift the exporter removed divided by the clip
   length. Numbers are the shipped phillip set's gait_speed_mps
   (models/phillip_clips.json); other rigs override per model from their
   own <base>_clips.json, and multiplayer.cfg ghost_gait_* overrides both.
   Clips absent here (idle, one-shots, turns) never get speed-scaled. */
struct cGhostGaitDefault
{
	const char *mpClip;
	float mfGait;
};
static const cGhostGaitDefault kGhostGaitDefaults[] =
{
	{ "walk",          1.2753f },
	{ "run",           1.9178f },
	{ "crouch_walk",   0.7610f },
	{ "walk_back",     0.7154f },
	{ "strafe_walk_l", 0.5363f },
	{ "strafe_walk_r", 0.7712f },
	{ "strafe_run_l",  2.1027f },
	{ "strafe_run_r",  1.8368f },
};
static const int kGhostGaitDefaultNum =
	(int)(sizeof(kGhostGaitDefaults) / sizeof(kGhostGaitDefaults[0]));

/* Clip speed = locomotion speed / gait, clamped: below 0.6 the feet plant
   too slowly to read as walking, above 1.8 the limbs blur. */
static const float kClipSpeedMin = 0.6f;
static const float kClipSpeedMax = 1.8f;

/* Crossfade windows (seconds). Locomotion<->locomotion is a full blend;
   one-shots cut in/out fast so a jump/crouch reads as an event. */
static const float kFadeLocomotion = 0.20f;
static const float kFadeOneShot = 0.12f;

/* Interpolation: hold the last known motion this long past the newest sample
   (packet loss / jitter), then freeze. Teleport = a jump this large between
   consecutive samples (60 m/s at 30 Hz — never a movement). */
static const float kGhostExtrapMaxSec = 0.25f;
static const float kGhostSnapDistance = 2.0f;

/* The converted models' forward axis is opposite the camera yaw convention —
   one fixed half-turn corrects the facing. */
static const float kMeshYawOffset = kPif;

/* Play-once clips: they own the pose until they finish, then the locomotion
   selector reclaims the ghost. Never a valid selector target. */
static bool IsOneShotAnim(const tString &asName)
{
	return asName == "jump" || asName == "stand_to_crouch" ||
		asName == "crouch_to_stand";
}

static bool IsIdleTarget(const tString &asName)
{
	return asName == "idle" || asName == "crouch_idle";
}

/* Shortest signed arc into [-pi, pi]. */
static float WrapPi(float afAngle)
{
	if (afAngle > -kPif && afAngle <= kPif)
		return afAngle;
	float a = fmodf(afAngle + kPif, 2.0f * kPif);
	if (a < 0.0f)
		a += 2.0f * kPif;
	return a - kPif;
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

int cGhostPlayer::GetClipCount()
{
	return kClipNum;
}

const char *cGhostPlayer::GetClipName(int alIdx)
{
	if (alIdx < 0 || alIdx >= kClipNum)
		return "";
	return kpClipNames[alIdx];
}

float cGhostPlayer::GetDefaultGait(const tString &asClip)
{
	for (int i = 0; i < kGhostGaitDefaultNum; ++i)
		if (asClip == kGhostGaitDefaults[i].mpClip)
			return kGhostGaitDefaults[i].mfGait;
	return 0.0f;
}

float cGhostPlayer::GaitFor(const tString &asClip) const
{
	std::map<tString, float>::const_iterator it = m_mapGaits.find(asClip);
	if (it != m_mapGaits.end() && it->second > 0.05f)
		return it->second;
	return GetDefaultGait(asClip);
}

//-----------------------------------------------------------------------

cGhostPlayer::cGhostPlayer(cWorld3D *apWorld, uint8_t alPlayerID, const tString &asBodyMeshFile,
	float afBodyYOffsetStand, float afBodyYOffsetCrouch,
	float afEyeHeightStand, float afEyeHeightCrouch, const cGhostTuning *apTuning)
{
	mpWorld = apWorld;
	mlPlayerID = alPlayerID;
	mpBodyEntity = NULL;
	mpMarkerLight = NULL;
	mpFlashlight = NULL;
	mfBodyYOffsetStand = afBodyYOffsetStand;
	mfBodyYOffsetCrouch = afBodyYOffsetCrouch;
	mfBodyYOffsetCurrent = afBodyYOffsetStand;
	mfStanceLerpSeconds = 0.15f;
	mfEyeHeightStand = afEyeHeightStand;
	mfEyeHeightCrouch = afEyeHeightCrouch;

	mfInterpDelaySec = 0.10f;
	mfTurnRateRadPerSec = 12.566f;
	mbAnimTrace = false;
	if (apTuning)
	{
		mfInterpDelaySec = apTuning->mfInterpDelaySec;
		mfTurnRateRadPerSec = apTuning->mfTurnRateRadPerSec;
		mbAnimTrace = apTuning->mbAnimTrace;
		m_mapGaits = apTuning->m_mapGaitOverrides; /* cfg wins over the json */
	}
	if (mfInterpDelaySec < 0.0f) mfInterpDelaySec = 0.0f;
	else if (mfInterpDelaySec > 0.25f) mfInterpDelaySec = 0.25f;
	if (mfTurnRateRadPerSec < 0.5f) mfTurnRateRadPerSec = 0.5f;

	for (int i = 0; i < kSampleNum; ++i)
		mvSamples[i] = cGhostSample();
	mlSampleHead = 0;
	mlSampleCount = 0;
	mlLastSeq = 0;
	mbHaveSeq = false;
	mlSeqUnwrapped = 0;
	mlLastHealth = 100;
	mbLastDead = false;

	mfLocalClock = 0.0;
	mfClockOffset = 0.0;
	mbClockKnown = false;
	mfClockWinMin = 0.0;
	mfClockWinStart = 0.0;

	mbRenderValid = false;
	mvRenderPos = cVector3f(0, 0, 0);
	mfRenderYaw = 0.0f;
	mfRenderPitch = 0.0f;
	mfRenderSpeed = 0.0f;
	mfRenderVelX = 0.0f;
	mfRenderVelZ = 0.0f;
	mbSnapPending = true;

	mbAnimsLoaded = false;
	msCurrentTarget = "";
	msCurrentClip = "";
	msOneShot = "";
	msForcedClip = "";
	mfForcedOverTime = 0.0f;
	mbMoving = false;
	mbRunning = false;
	mlMoveSector = eGhostMoveSector_Forward;
	mbHavePrevFlags = false;
	mbPrevOnGround = true;
	mbPrevCrouch = false;
	mfAirborneTime = 0.0f;
	mbJumpClipFired = false;
	mfSinceLanded = 0.0f;
	mfLocomotionSpeed = 0.0f;
	mfLastMoveAngleDeg = 0.0f;
	mlLastAnimTraceMs = 0;
	mlWantHeld[0] = mlWantHeld[1] = eNetHeldItem_None;
	mlShownHeld[0] = mlShownHeld[1] = eNetHeldItem_None;
	mpHeldProp[0] = mpHeldProp[1] = NULL;
	mpHeldGlow = NULL;
	mbHeldBoneWarned = false;
	mpCollider = NULL;
	mvColliderLastFeet = cVector3f(0, 0, 0);
	mbColliderCrouch = false;
	mfColliderHeight = 1.9f;

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
				{
					LoadGaitsFromClipsJson(asBodyMeshFile);
					LoadAnimations(asBodyMeshFile);
				}
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
		/* The player's own flashlight is defined by this file: projection
		   image (the round beam) + falloff image. A spot light with NO
		   projection image is not masked to a cone — it floods its whole
		   square frustum, which on plank ceilings read as hard "prison
		   bar" stripes. Load the same definition, then override shadows
		   (cost) and the near plane (the lens sits in the ghost's hand). */
		mpFlashlight->LoadXMLProperties("light_player_flashlight_spot.lnt");
		mpFlashlight->SetDiffuseColor(cColor(0.55f, 0.55f, 0.45f, 1.0f));
		mpFlashlight->SetFarAttenuation(10.0f);
		mpFlashlight->SetFOV(cMath::ToRad(85)); /* the hud flashlight's falloff angle */
		mpFlashlight->SetAspect(1.0f);
		mpFlashlight->SetNearClipPlane(0.12f);
		mpFlashlight->SetCastShadows(false);
		mpFlashlight->SetVisible(false);
	}
}

//-----------------------------------------------------------------------

cGhostPlayer::~cGhostPlayer()
{
	DestroyHeldProps(); /* before the body: it owns the hand bones */
	if (mpWorld && mpCollider && mpWorld->GetPhysicsWorld())
		mpWorld->GetPhysicsWorld()->DestroyCharacterBody(mpCollider);
	mpCollider = NULL;
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
// v21: held items. One row per eNetHeldItem.
//
// Grip geometry is measured, not eyeballed (analyze_grip.py over the rest
// poses and the item DAEs, all Y_UP so engine space == file space):
//  * every character hand bone (mixamo rig): local +Y runs along the
//    fingers, +Z is the palm side, the thumb points +X on the RIGHT hand and
//    -X on the LEFT;
//  * each item's long axis, which end is the heavy head (hammer head, lens,
//    bristles), and where along that axis a hand closes (tools near the
//    handle end, lights and throwables mid-body).
// The item's long axis is laid across the fist along the thumb line (head
// on the thumb side), its width axis along the fingers, then tilted by
// mfTiltDeg from the thumb line toward the fingers (a hanging arm carries a
// hammer head-down-forward, a broom nearly vertical).
//-----------------------------------------------------------------------

namespace
{
struct cHeldItemDef
{
	uint8_t mlId;
	const char *msHudName;  /* iHudModel::msName ("" = not selectable by name) */
	int mlHudSlot;          /* 0 left, 1 right, -1 either */
	const char *msFile;     /* world model */
	int mlLongAxis;         /* 0 X, 1 Y, 2 Z in item space */
	float mfHeadSign;       /* +1/-1: which end of the long axis is the head */
	int mlWidthAxis;        /* laid along the fingers */
	float mfGripAlong;      /* long-axis coordinate the fist closes on (m) */
	float mfTiltDeg;        /* thumb line -> fingers */
};

const cHeldItemDef gvHeldItems[] = {
	{ eNetHeldItem_Flashlight, "Flashlight",  0, "item_flashlight.dae",   1, -1.0f, 0,  0.030f,  0.0f },
	{ eNetHeldItem_Glowstick,  "Glowstick",   0, "item_glowstick.dae",    2, +1.0f, 0, -0.010f,  0.0f },
	{ eNetHeldItem_Flare,      "Flare",       0, "item_flare.dae",        2, +1.0f, 0,  0.000f,  0.0f },
	{ eNetHeldItem_Hammer,     "Hammer",      1, "item_hammer.dae",       2, +1.0f, 1, -0.120f, 25.0f },
	{ eNetHeldItem_PickAxe,    "PickAxe",     1, "items_pickaxe.dae",     0, +1.0f, 2, -0.470f, 25.0f },
	{ eNetHeldItem_Broom,      "BroomWeapon", 1, "modern_mine_broom.dae", 1, -1.0f, 0,  0.550f, 70.0f },
	{ eNetHeldItem_Dynamite,   "Dynamite",    1, "item_dynamite.dae",     1, +1.0f, 0,  0.020f,  0.0f },
	{ eNetHeldItem_Meat,       "Meat",        1, "item_meat.dae",         0, +1.0f, 1,  0.000f,  0.0f },
	{ eNetHeldItem_FlareThrow, "Flare",       1, "items_flare.dae",       1, +1.0f, 0,  0.030f,  0.0f },
};
const int kHeldItemDefNum = (int)(sizeof(gvHeldItems) / sizeof(gvHeldItems[0]));

/* flashlight lens: the wide end of item_flashlight.dae's long axis */
const float kFlashlightLensAlong = -0.150f;

const cHeldItemDef *FindHeldDef(uint8_t alId)
{
	for (int i = 0; i < kHeldItemDefNum; ++i)
		if (gvHeldItems[i].mlId == alId)
			return &gvHeldItems[i];
	return NULL;
}

cVector3f AxisVec(int alAxis)
{
	return cVector3f(alAxis == 0 ? 1.0f : 0.0f, alAxis == 1 ? 1.0f : 0.0f, alAxis == 2 ? 1.0f : 0.0f);
}

/** Item-space -> hand-bone-local transform for one item in one hand. */
cMatrixf HeldGripMatrix(const cHeldItemDef &aDef, bool abRightHand)
{
	const cVector3f vFingers(0.0f, 1.0f, 0.0f);
	const cVector3f vPalm(0.0f, 0.0f, 1.0f);
	const cVector3f vThumb(abRightHand ? 1.0f : -1.0f, 0.0f, 0.0f);

	const float fTilt = cMath::ToRad(aDef.mfTiltDeg);
	const cVector3f vT = vThumb * cosf(fTilt) + vFingers * sinf(fTilt);   /* long axis goes here */
	const cVector3f vF = vThumb * -sinf(fTilt) + vFingers * cosf(fTilt);  /* width axis goes here */

	const int lL = aDef.mlLongAxis;
	const int lW = aDef.mlWidthAxis;
	const int lC = 3 - lL - lW;
	/* u_L x u_W = eps * u_C; a proper rotation needs R(u_C) = eps * s * (R u_L x R u_W) / s */
	const float fEps = (((lL + 1) % 3) == lW) ? 1.0f : -1.0f;
	const float fS = aDef.mfHeadSign;

	cVector3f vCol[3];
	vCol[lL] = vT * fS;
	vCol[lW] = vF;
	vCol[lC] = cMath::Vector3Cross(vT, vF) * (fEps * fS);

	cMatrixf m = cMatrixf::Identity;
	for (int c = 0; c < 3; ++c)
	{
		m.m[0][c] = vCol[c].x;
		m.m[1][c] = vCol[c].y;
		m.m[2][c] = vCol[c].z;
	}

	/* the fist closes a finger-length past the wrist, just off the palm */
	const cVector3f vGripHand = vFingers * 0.080f + vPalm * 0.030f;
	const cVector3f vGripItem = AxisVec(lL) * aDef.mfGripAlong;
	const cVector3f vT0 = vGripHand - cMath::MatrixMul(m, vGripItem);
	m.SetTranslation(vT0);
	return m;
}
}

uint8_t cGhostPlayer::HeldItemFromHudName(const tString &asHudName, int alSlot)
{
	if (asHudName.empty())
		return eNetHeldItem_None;
	const tString sLow = cString::ToLowerCase(asHudName);
	for (int i = 0; i < kHeldItemDefNum; ++i)
	{
		const cHeldItemDef &d = gvHeldItems[i];
		if (d.mlHudSlot >= 0 && d.mlHudSlot != alSlot)
			continue;
		if (cString::ToLowerCase(d.msHudName) == sLow)
			return d.mlId;
	}
	return eNetHeldItem_None;
}

void cGhostPlayer::DestroyHeldProps()
{
	if (mpWorld)
	{
		for (int h = 0; h < 2; ++h)
			if (mpHeldProp[h])
				mpWorld->DestroyMeshEntity(mpHeldProp[h]);
		if (mpHeldGlow)
			mpWorld->DestroyLight(mpHeldGlow);
	}
	mpHeldProp[0] = mpHeldProp[1] = NULL;
	mpHeldGlow = NULL;
	mlShownHeld[0] = mlShownHeld[1] = eNetHeldItem_None;
}

void cGhostPlayer::UpdateHeldProps()
{
	if (mpWorld == NULL || mpBodyEntity == NULL)
		return;

	for (int h = 0; h < 2; ++h)
	{
		const uint8_t lWant = mbLastDead ? (uint8_t)eNetHeldItem_None : mlWantHeld[h];
		if (lWant == mlShownHeld[h])
			continue;
		if (mpHeldProp[h])
		{
			mpWorld->DestroyMeshEntity(mpHeldProp[h]);
			mpHeldProp[h] = NULL;
		}
		mlShownHeld[h] = lWant;

		const cHeldItemDef *pDef = FindHeldDef(lWant);
		if (pDef == NULL)
			continue;

		const char *sBoneName = (h == 0) ? "mixamorigwLeftHand" : "mixamorigwRightHand";
		cBoneState *pBone = mpBodyEntity->GetBoneStateFromName(sBoneName);
		if (pBone == NULL)
		{
			if (mbHeldBoneWarned == false)
			{
				mbHeldBoneWarned = true;
				Log(" multiplayer: GhostPlayer id=%u has no '%s' bone - held items are not shown for this character\n",
					(unsigned)mlPlayerID, sBoneName);
			}
			continue;
		}

		cMeshManager *pMeshManager = mpWorld->GetResources()->GetMeshManager();
		unsigned long lException = 0;
		cMesh *pMesh = CreateMeshGuarded(pMeshManager, pDef->msFile, &lException);
		if (pMesh == NULL)
		{
			Log(" multiplayer: GhostPlayer id=%u could not load held item '%s'\n",
				(unsigned)mlPlayerID, pDef->msFile);
			continue;
		}
		const tString sName = "GhostHeld_" + cString::ToString((int)mlPlayerID) + (h == 0 ? "_L" : "_R");
		cMeshEntity *pProp = mpWorld->CreateMeshEntity(sName, pMesh);
		if (pProp == NULL)
			continue;
		pProp->SetCastsShadows(false);
		pBone->AddEntity(pProp);
		pProp->SetMatrix(HeldGripMatrix(*pDef, h == 1));
		mpHeldProp[h] = pProp;
		Log(" multiplayer: GhostPlayer id=%u holds '%s' in the %s hand\n",
			(unsigned)mlPlayerID, pDef->msHudName, h == 0 ? "left" : "right");
	}

	/* glow: a lit glowstick or flare in the left hand lights its holder */
	const uint8_t lLeft = mlShownHeld[0];
	const bool bGlow = mpHeldProp[0] &&
		(lLeft == eNetHeldItem_Glowstick || lLeft == eNetHeldItem_Flare);
	if (bGlow && mpHeldGlow == NULL)
	{
		mpHeldGlow = mpWorld->CreateLightPoint("GhostHeldGlow_" + cString::ToString((int)mlPlayerID), true);
		if (mpHeldGlow)
			mpHeldGlow->SetCastShadows(false);
	}
	if (mpHeldGlow)
	{
		mpHeldGlow->SetVisible(bGlow);
		if (bGlow)
		{
			/* colours from the first-person hud models' own lights */
			if (lLeft == eNetHeldItem_Glowstick)
			{
				mpHeldGlow->SetDiffuseColor(cColor(0.53f, 0.83f, 0.73f, 1.0f));
				mpHeldGlow->SetFarAttenuation(4.0f);
			}
			else
			{
				mpHeldGlow->SetDiffuseColor(cColor(1.0f, 0.3f, 0.3f, 1.0f));
				mpHeldGlow->SetFarAttenuation(7.0f);
			}
			mpHeldGlow->SetPosition(mpHeldProp[0]->GetWorldMatrix().GetTranslation());
		}
	}
}

//-----------------------------------------------------------------------

void cGhostPlayer::CreateCollider(const cVector3f &avSize, float afCrouchHeight)
{
	if (mpWorld == NULL || mpCollider != NULL || mpWorld->GetPhysicsWorld() == NULL)
		return;
	iPhysicsWorld *pPhysics = mpWorld->GetPhysicsWorld();
	mpCollider = pPhysics->CreateCharacterBody("GhostCollider_" + cString::ToString((int)mlPlayerID), avSize);
	if (mpCollider == NULL)
		return;
	mpCollider->AddExtraSize(cVector3f(avSize.x, afCrouchHeight, avSize.z)); /* size 1 = crouch */
	/* We place it; it must not fall, climb or push by itself. With gravity
	   off the character update only copies its position to the body. */
	mpCollider->SetGravityActive(false);
	mpCollider->SetMaxPushMass(0);
	mpCollider->SetPushForce(0);
	mpCollider->SetActive(false); /* until the first state places it */
	mfColliderHeight = avSize.y;
}

iPhysicsBody *cGhostPlayer::GetHitBody() const
{
	if (mpCollider == NULL || mpCollider->IsActive() == false)
		return NULL;
	return mpCollider->GetBody();
}

void cGhostPlayer::UpdateCollider(bool abCrouch)
{
	if (mpCollider == NULL)
		return;
	const bool bWant = (mbLastDead == false) && mbRenderValid;
	if (mpCollider->IsActive() != bWant)
		mpCollider->SetActive(bWant);
	if (bWant == false)
		return;
	if (abCrouch != mbColliderCrouch)
	{
		mpCollider->SetActiveSize(abCrouch ? 1 : 0);
		mbColliderCrouch = abCrouch;
	}
	mpCollider->SetFeetPosition(mvRenderPos);

	/* Wake sleeping objects inside the friend's footprint: a teleported
	   body resolves overlaps only with AWAKE bodies, so without this a
	   friend walks straight into a resting crate. On the host that push is
	   the real one and replicates; on a guest it is only a local preview
	   the host's stream corrects. */
	const cVector3f vMoved = mvRenderPos - mvColliderLastFeet;
	mvColliderLastFeet = mvRenderPos;
	if (vMoved.x * vMoved.x + vMoved.z * vMoved.z < 0.0001f)
		return;
	const float kWakeRadius = 0.9f;
	cPhysicsBodyIterator it = mpWorld->GetPhysicsWorld()->GetBodyIterator();
	while (it.HasNext())
	{
		iPhysicsBody *pBody = it.Next();
		if (pBody == NULL || pBody->GetMass() <= 0.0f || pBody->IsCharacter() || pBody->GetEnabled())
			continue;
		const cVector3f v = pBody->GetWorldPosition();
		const float fDx = v.x - mvRenderPos.x, fDz = v.z - mvRenderPos.z;
		if (fDx * fDx + fDz * fDz > kWakeRadius * kWakeRadius)
			continue;
		if (v.y < mvRenderPos.y - 0.3f || v.y > mvRenderPos.y + mfColliderHeight)
			continue;
		pBody->SetEnabled(true);
	}
}

//-----------------------------------------------------------------------

void cGhostPlayer::OrphanWorld()
{
	mpHeldProp[0] = mpHeldProp[1] = NULL; /* the dying world destroys them */
	mpHeldGlow = NULL;
	mpCollider = NULL;
	mpBodyEntity = NULL;
	mpFlashlight = NULL;
	mpMarkerLight = NULL;
	mpWorld = NULL;
}

//-----------------------------------------------------------------------

bool cGhostPlayer::GetLastStatePos(cVector3f *apOut) const
{
	if (apOut == NULL || mlSampleCount <= 0)
		return false;
	const cGhostSample &s = mvSamples[mlSampleHead];
	const bool bCrouch = (s.mFlags & eNetPlayerFlag_Crouch) != 0;
	*apOut = s.mvPos + cVector3f(0, bCrouch ? mfEyeHeightCrouch : mfEyeHeightStand, 0);
	return true;
}

bool cGhostPlayer::GetLastFeetPos(cVector3f *apOut) const
{
	if (apOut == NULL || mlSampleCount <= 0)
		return false;
	*apOut = mvSamples[mlSampleHead].mvPos;
	return true;
}

bool cGhostPlayer::GetRenderFeetPos(cVector3f *apOut) const
{
	if (apOut == NULL || mbRenderValid == false)
		return false;
	*apOut = mvRenderPos;
	return true;
}

//-----------------------------------------------------------------------

void cGhostPlayer::AnimLog(const char *asFmt, ...) const
{
	if (mbAnimTrace == false)
		return;
	char sBuf[512];
	va_list ap;
	va_start(ap, asFmt);
	vsnprintf(sBuf, sizeof(sBuf), asFmt, ap);
	va_end(ap);
	sBuf[sizeof(sBuf) - 1] = '\0';
	Log(" multiplayer: GhostPlayer id=%u %s\n", (unsigned)mlPlayerID, sBuf);
}

//-----------------------------------------------------------------------

void cGhostPlayer::LoadGaitsFromClipsJson(const tString &asBodyMeshFile)
{
	if (mpWorld == NULL || mpWorld->GetResources() == NULL)
		return;
	hpl::cFileSearcher *pSearcher = mpWorld->GetResources()->GetFileSearcher();
	if (pSearcher == NULL)
		return;

	const tString sName = cString::SetFileExt(asBodyMeshFile, "") + "_clips.json";
	const tString sPath = pSearcher->GetFilePath(sName);
	if (sPath.empty())
		return; /* no per-model table: the built-in phillip numbers stand */

	FILE *fp = fopen(sPath.c_str(), "rb");
	if (fp == NULL)
		return;
	std::string sJson;
	char sChunk[4096];
	size_t lRead;
	while ((lRead = fread(sChunk, 1, sizeof(sChunk), fp)) > 0)
	{
		sJson.append(sChunk, lRead);
		if (sJson.size() > (size_t)(4 * 1024 * 1024))
			break; /* not a clips table */
	}
	fclose(fp);

	/* Minimal scan (no JSON parser in the engine): each clip block starts
	   with "<clip>": { and its own gait_speed_mps is the first one after
	   that key — the exporter writes every clip block with the field. */
	char sLine[512];
	sLine[0] = '\0';
	size_t lLineLen = 0;
	int lFound = 0;
	for (int i = 0; i < kGhostGaitDefaultNum; ++i)
	{
		const tString sClip = kGhostGaitDefaults[i].mpClip;
		if (m_mapGaits.find(sClip) != m_mapGaits.end())
			continue; /* multiplayer.cfg override wins */
		const std::string sKey = "\"" + sClip + "\": {";
		const size_t lAt = sJson.find(sKey);
		if (lAt == std::string::npos)
			continue;
		const char *kpField = "\"gait_speed_mps\":";
		const size_t lField = sJson.find(kpField, lAt);
		if (lField == std::string::npos)
			continue;
		const float fGait = (float)atof(sJson.c_str() + lField + strlen(kpField));
		if (fGait > 0.05f && fGait < 10.0f)
		{
			m_mapGaits[sClip] = fGait;
			++lFound;
			if (lLineLen < sizeof(sLine) - 48)
			{
				const int n = snprintf(sLine + lLineLen, sizeof(sLine) - lLineLen,
					" %s=%.2f", sClip.c_str(), fGait);
				if (n > 0) lLineLen += (size_t)n;
			}
		}
	}
	if (lFound > 0)
		Log(" multiplayer: GhostPlayer id=%u gait speeds from '%s' (m/s at 1.0x):%s\n",
			(unsigned)mlPlayerID, sName.c_str(), sLine);
}

//-----------------------------------------------------------------------

void cGhostPlayer::LoadAnimations(const tString &asBodyMeshFile)
{
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
	   renders unanimated. Nothing is active yet, so the crossfade sets the
	   first clip to weight 1 outright (no blend from the bind pose). */
	cAnimationState *pIdle = mpBodyEntity->GetAnimationStateFromName("idle");
	cAnimationState *pWalk = mpBodyEntity->GetAnimationStateFromName("walk");
	if (pIdle)
	{
		mbAnimsLoaded = true;
		msCurrentTarget = "idle";
		CrossfadeTo("idle", true, kFadeLocomotion, false);
	}
	else if (pWalk)
	{
		mbAnimsLoaded = true;
		msCurrentTarget = "idle";
		CrossfadeTo("walk", true, kFadeLocomotion, false);
		pWalk->SetSpeed(0.0f);
	}
}

//-----------------------------------------------------------------------

void cGhostPlayer::ApplyState(const cNetPlayerState &aState)
{
	/* Sender clock from the seq counter: one constant period per packet,
	   unwrapped with the same int16 difference the manager's reorder guard
	   uses. Stale/duplicate seqs never enter the buffer. */
	if (mbHaveSeq)
	{
		const int lDiff = (int)(int16_t)(uint16_t)(aState.mSeq - mlLastSeq);
		if (lDiff <= 0)
			return;
		mlSeqUnwrapped += lDiff;
		if (lDiff > 90)
			mlSampleCount = 0; /* > 3 s of silence: never interpolate across it */
	}
	else
	{
		mbHaveSeq = true;
		mlSeqUnwrapped = 0;
	}
	mlLastSeq = aState.mSeq;
	/* v12: vitals ride on the newest state (no interpolation) */
	mlLastHealth = (aState.mHealth > 100) ? (uint8_t)100 : aState.mHealth;
	mbLastDead = (aState.mFlags & eNetPlayerFlag_Dead) != 0;
	/* v21: held items ride on the newest state too; unknown ids = empty */
	mlWantHeld[0] = (aState.mHeldLeft < eNetHeldItem_Count) ? aState.mHeldLeft : (uint8_t)eNetHeldItem_None;
	mlWantHeld[1] = (aState.mHeldRight < eNetHeldItem_Count) ? aState.mHeldRight : (uint8_t)eNetHeldItem_None;

	cGhostSample s;
	s.mfTSend = (double)mlSeqUnwrapped * (double)kNetSendPeriodSeconds;
	s.mvPos = cVector3f(aState.mfPosX, aState.mfPosY, aState.mfPosZ);
	s.mfYaw = aState.mfYaw;
	s.mfPitch = aState.mfPitch;
	s.mfVelFwd = (float)aState.mVelFwd / kNetPlayerVelScale;
	s.mfVelRight = (float)aState.mVelRight / kNetPlayerVelScale;
	s.mFlags = aState.mFlags;
	s.mMoveState = aState.mMoveState;
	s.mbFlashlightOn = aState.mbFlashlightOn != 0;

	/* Local-minus-sender clock offset. Jitter only ever DELAYS a packet, so
	   the running minimum is the jitter-free mapping; a 2 s window lets it
	   creep back up when the sender's clock runs slow; a second or more of
	   disagreement (sender paused on a loading screen) is a fresh start. */
	/* Wall time, not summed logic dt: after a RECEIVER hitch (autosave,
	   alt-tab) the first catch-up logic step drains every queued packet
	   while a dt-clock is still stale, which would drop the running-min
	   offset by the hitch length and leave the ghost extrapolating ahead of
	   its data for the whole 2 s window. */
	mfLocalClock = (double)GetApplicationTime() / 1000.0;
	const double fOff = mfLocalClock - s.mfTSend;
	if (mbClockKnown == false)
	{
		mbClockKnown = true;
		mfClockOffset = fOff;
		mfClockWinMin = fOff;
		mfClockWinStart = mfLocalClock;
	}
	else if (fabs(fOff - mfClockOffset) > 1.0)
	{
		mfClockOffset = fOff;
		mfClockWinMin = fOff;
		mfClockWinStart = mfLocalClock;
		mlSampleCount = 0;
	}
	else
	{
		if (fOff < mfClockOffset)
			mfClockOffset = fOff;
		if (fOff < mfClockWinMin)
			mfClockWinMin = fOff;
		if (mfLocalClock - mfClockWinStart >= 2.0)
		{
			mfClockOffset = mfClockWinMin;
			mfClockWinMin = fOff;
			mfClockWinStart = mfLocalClock;
		}
	}

	/* Teleport (spawn-at-host, map start) or a non-monotonic stamp: no
	   interpolation across it — the buffer restarts and the render snaps. */
	if (mlSampleCount > 0)
	{
		const cGhostSample &n = mvSamples[mlSampleHead];
		const cVector3f d = s.mvPos - n.mvPos;
		if (d.x * d.x + d.y * d.y + d.z * d.z > kGhostSnapDistance * kGhostSnapDistance ||
			s.mfTSend <= n.mfTSend)
			mlSampleCount = 0;
	}

	if (mlSampleCount == 0)
	{
		mlSampleHead = 0;
		mvSamples[0] = s;
		mlSampleCount = 1;
		mbSnapPending = true;
	}
	else
	{
		mlSampleHead = (mlSampleHead + 1) % kSampleNum;
		mvSamples[mlSampleHead] = s;
		if (mlSampleCount < kSampleNum)
			++mlSampleCount;
	}
}

//-----------------------------------------------------------------------

bool cGhostPlayer::ResolveRenderSample(cGhostSample *apOut)
{
	if (apOut == NULL || mlSampleCount <= 0 || mbClockKnown == false)
		return false;

	const double fTRender = mfLocalClock - mfClockOffset - (double)mfInterpDelaySec;
	const cGhostSample &newest = mvSamples[mlSampleHead];
	const cGhostSample &oldest =
		mvSamples[(mlSampleHead - (mlSampleCount - 1) + kSampleNum) % kSampleNum];

	if (fTRender >= newest.mfTSend)
	{
		/* Past the newest sample: continue its motion briefly, then hold. */
		*apOut = newest;
		double fAhead = fTRender - newest.mfTSend;
		if (fAhead > (double)kGhostExtrapMaxSec)
			fAhead = (double)kGhostExtrapMaxSec;
		if (fAhead > 0.0)
		{
			float fVx = 0.0f, fVz = 0.0f;
			const float fWire = sqrtf(newest.mfVelFwd * newest.mfVelFwd +
				newest.mfVelRight * newest.mfVelRight);
			if (fWire > 0.05f)
			{
				/* body-local -> world: forward (-sin y, 0, -cos y), right (cos y, 0, -sin y) */
				const float fSinY = sinf(newest.mfYaw);
				const float fCosY = cosf(newest.mfYaw);
				fVx = -newest.mfVelFwd * fSinY + newest.mfVelRight * fCosY;
				fVz = -newest.mfVelFwd * fCosY - newest.mfVelRight * fSinY;
			}
			else if (mlSampleCount >= 2)
			{
				const cGhostSample &prev = mvSamples[(mlSampleHead - 1 + kSampleNum) % kSampleNum];
				const double fSpan = newest.mfTSend - prev.mfTSend;
				if (fSpan > 1e-4 && fSpan <= 0.2)
				{
					fVx = (float)((newest.mvPos.x - prev.mvPos.x) / fSpan);
					fVz = (float)((newest.mvPos.z - prev.mvPos.z) / fSpan);
				}
			}
			apOut->mvPos.x += fVx * (float)fAhead;
			apOut->mvPos.z += fVz * (float)fAhead;
		}
		return true;
	}

	if (fTRender <= oldest.mfTSend)
	{
		*apOut = oldest; /* right after the first sample: hold it for the delay */
		return true;
	}

	/* Bracketing pair: samples are strictly increasing in send time from
	   oldest to newest (ApplyState guarantees it), so walk newest-first. */
	for (int i = 1; i < mlSampleCount; ++i)
	{
		const cGhostSample &b = mvSamples[(mlSampleHead - (i - 1) + kSampleNum) % kSampleNum];
		const cGhostSample &a = mvSamples[(mlSampleHead - i + kSampleNum) % kSampleNum];
		if (a.mfTSend <= fTRender && fTRender < b.mfTSend)
		{
			const double fSpan = b.mfTSend - a.mfTSend;
			float u = (fSpan > 1e-6) ? (float)((fTRender - a.mfTSend) / fSpan) : 1.0f;
			if (u < 0.0f) u = 0.0f;
			else if (u > 1.0f) u = 1.0f;

			*apOut = a; /* flags/stance: the state in force at that time */
			apOut->mfTSend = fTRender;
			apOut->mvPos = a.mvPos + (b.mvPos - a.mvPos) * u;
			apOut->mfYaw = WrapPi(a.mfYaw + WrapPi(b.mfYaw - a.mfYaw) * u);
			apOut->mfPitch = a.mfPitch + (b.mfPitch - a.mfPitch) * u;
			apOut->mfVelFwd = a.mfVelFwd + (b.mfVelFwd - a.mfVelFwd) * u;
			apOut->mfVelRight = a.mfVelRight + (b.mfVelRight - a.mfVelRight) * u;
			return true;
		}
	}

	*apOut = newest; /* unreachable with a monotonic buffer; never leave garbage */
	return true;
}

//-----------------------------------------------------------------------

void cGhostPlayer::Update(float afTimeStep)
{
	if (afTimeStep < 0.0f)
		afTimeStep = 0.0f;
	mfLocalClock = (double)GetApplicationTime() / 1000.0; /* wall clock, see ApplyState */
	/* smoothing/EMA step: a hitch must not become a 1-second lerp */
	const float fDt = (afTimeStep > 0.1f) ? 0.1f : afTimeStep;

	cGhostSample s;
	if (ResolveRenderSample(&s) == false)
		return; /* no state yet: the body stays wherever it was created */

	if (mbSnapPending || mbRenderValid == false)
	{
		mvRenderPos = s.mvPos;
		mfRenderYaw = WrapPi(s.mfYaw);
		mfRenderSpeed = 0.0f;
		mfRenderVelX = 0.0f;
		mfRenderVelZ = 0.0f;
		mbSnapPending = false;
		mbRenderValid = true;
	}
	else
	{
		if (fDt > 1e-5f)
		{
			float fVx = (s.mvPos.x - mvRenderPos.x) / fDt;
			float fVz = (s.mvPos.z - mvRenderPos.z) / fDt;
			float fSpeed = sqrtf(fVx * fVx + fVz * fVz);
			if (fSpeed > 15.0f)
			{
				fVx = 0.0f; /* teleport-sized step: not a movement measurement */
				fVz = 0.0f;
				fSpeed = 0.0f;
			}
			const float fAlpha = fDt / (0.1f + fDt); /* EMA, tau ~0.1 s */
			mfRenderSpeed += (fSpeed - mfRenderSpeed) * fAlpha;
			mfRenderVelX += (fVx - mfRenderVelX) * fAlpha;
			mfRenderVelZ += (fVz - mfRenderVelZ) * fAlpha;
		}
		mvRenderPos = s.mvPos;

		/* Body yaw follows the (already interpolated) view yaw at a limited
		   rate: a glance sideways turns the whole body, but not in one frame. */
		float fDiff = WrapPi(s.mfYaw - mfRenderYaw);
		const float fMaxStep = mfTurnRateRadPerSec * fDt;
		if (fDiff > fMaxStep) fDiff = fMaxStep;
		else if (fDiff < -fMaxStep) fDiff = -fMaxStep;
		mfRenderYaw = WrapPi(mfRenderYaw + fDiff);
	}
	mfRenderPitch = s.mfPitch;

	const bool bCrouch = (s.mFlags & eNetPlayerFlag_Crouch) != 0;

	if (mpBodyEntity)
	{
		/* Selection FIRST: a stance transition trigger stretches
		   mfStanceLerpSeconds to the clip's duration, and the offset lerp
		   below must run over that same window from this very frame. */
		SelectLocomotion(s, mfRenderSpeed, fDt);
		ApplyClipSpeeds();

		/* Stance Y-offset overrides (cfg): feet are on the wire, so both
		   default to 0 and nothing moves — a tuned rig can still drop or
		   raise the mesh per stance, slid over the transition window. */
		const float fTargetOffset = bCrouch ? mfBodyYOffsetCrouch : mfBodyYOffsetStand;
		float fLerpWindow = mfStanceLerpSeconds;
		if (fLerpWindow < 0.05f) fLerpWindow = 0.05f;
		const float fMaxStep =
			fabsf(mfBodyYOffsetCrouch - mfBodyYOffsetStand) * (fDt / fLerpWindow);
		float fDiff = fTargetOffset - mfBodyYOffsetCurrent;
		if (fMaxStep <= 0.0f)
			mfBodyYOffsetCurrent = fTargetOffset;
		else
		{
			if (fDiff > fMaxStep) fDiff = fMaxStep;
			else if (fDiff < -fMaxStep) fDiff = -fMaxStep;
			mfBodyYOffsetCurrent += fDiff;
		}

		const cVector3f vBody(mvRenderPos.x, mvRenderPos.y + mfBodyYOffsetCurrent, mvRenderPos.z);
		const cMatrixf mtx = cMath::MatrixMul(cMath::MatrixTranslate(vBody),
			cMath::MatrixRotateY(mfRenderYaw + kMeshYawOffset));
		mpBodyEntity->SetWorldMatrix(mtx);
		UpdateHeldProps(); /* v21 */

		/* Diagnosis trace (ghost_anim_trace=1): activeAnims=0 is the ONLY
		   state that renders the skin bind pose — anything else playing
		   wrong is a units or retarget problem, not a selection problem. */
		if (mbAnimTrace)
		{
			const unsigned long lTraceNow = GetApplicationTime();
			if (lTraceNow - mlLastAnimTraceMs >= 5000)
			{
				mlLastAnimTraceMs = lTraceNow;
				int lActive = 0;
				float fWeightSum = 0.0f;
				cAnimationState *pCur = mpBodyEntity->GetAnimationStateFromName(msCurrentClip);
				const int lStateNum = mpBodyEntity->GetAnimationStateNum();
				for (int i = 0; i < lStateNum; ++i)
				{
					cAnimationState *pS = mpBodyEntity->GetAnimationState(i);
					if (pS && pS->IsActive())
					{
						++lActive;
						fWeightSum += pS->GetWeight();
					}
				}
				if (pCur)
					Log(" multiplayer: GhostPlayer id=%u activeAnims=%d/%d weightSum=%.2f target='%s' clip='%s' t=%.2f len=%.2f speed=%.2f | %.2f m/s wire, %.2f m/s render, buf=%d delay=%.0f ms\n",
						(unsigned)mlPlayerID, lActive, lStateNum, fWeightSum,
						msCurrentTarget.c_str(), msCurrentClip.c_str(),
						pCur->GetTimePosition(), pCur->GetLength(), pCur->GetSpeed(),
						mfLocomotionSpeed, mfRenderSpeed, mlSampleCount, mfInterpDelaySec * 1000.0f);
				else
					Log(" multiplayer: GhostPlayer id=%u activeAnims=%d/%d target='%s' clip='%s' — NOTHING PLAYING, skin renders bind pose\n",
						(unsigned)mlPlayerID, lActive, lStateNum,
						msCurrentTarget.c_str(), msCurrentClip.c_str());
			}
		}
	}

	UpdateCollider(bCrouch); /* solid friend */

	const cVector3f vEye = mvRenderPos +
		cVector3f(0, bCrouch ? mfEyeHeightCrouch : mfEyeHeightStand, 0);

	if (mpMarkerLight)
		mpMarkerLight->SetPosition(vEye);

	if (mpFlashlight)
	{
		/* roll is not on the wire — ghost aim uses pitch/yaw only */
		const float roll = 0.0f;
		/* v21: a held flashlight shines from its lens; the aim stays the
		   sender's view direction (that is where their real beam points) */
		cVector3f vBeamFrom = vEye;
		if (mpHeldProp[0] && mlShownHeld[0] == eNetHeldItem_Flashlight)
			vBeamFrom = cMath::MatrixMul(mpHeldProp[0]->GetWorldMatrix(),
				cVector3f(0.0f, kFlashlightLensAlong, 0.0f));
		cMatrixf mtx = cMath::MatrixMul(
			cMath::MatrixTranslate(vBeamFrom),
			cMath::MatrixRotate(cVector3f(mfRenderPitch, s.mfYaw, roll), eEulerRotationOrder_XYZ));
		mpFlashlight->SetMatrix(mtx);
		mpFlashlight->SetVisible(s.mbFlashlightOn);
	}
}

//-----------------------------------------------------------------------

void cGhostPlayer::CrossfadeTo(const tString &asClip, bool abLoop, float afFadeSec,
	bool abMatchPhase)
{
	if (mpBodyEntity == NULL)
		return;
	cAnimationState *pNew = mpBodyEntity->GetAnimationStateFromName(asClip);
	if (pNew == NULL)
		return;

	float fT = afFadeSec;
	if (fT < 0.02f)
		fT = 0.02f;

	/* Phase source: the clip being left, while it is still playing. */
	float fPhase = 0.0f;
	if (abMatchPhase && msCurrentClip.empty() == false)
	{
		cAnimationState *pOld = mpBodyEntity->GetAnimationStateFromName(msCurrentClip);
		if (pOld && pOld != pNew && pOld->IsActive() && pOld->GetLength() > 0.0f)
		{
			fPhase = pOld->GetRelativeTimePosition();
			if (fPhase < 0.0f) fPhase = 0.0f;
			else if (fPhase > 1.0f) fPhase = 1.0f;
		}
	}

	/* 1. every OTHER active state fades to 0 exactly at T (step = -w/T), and
	   the engine deactivates it when it gets there. Weights that already
	   reached 0 just deactivate. Sum of the others: if nothing else is
	   contributing, the new clip must take weight 1 outright — fading it
	   in from 0 alone would blend up from the bind pose. */
	float fOthers = 0.0f;
	const int lStateNum = mpBodyEntity->GetAnimationStateNum();
	for (int i = 0; i < lStateNum; ++i)
	{
		cAnimationState *pS = mpBodyEntity->GetAnimationState(i);
		if (pS == NULL || pS == pNew || pS->IsActive() == false)
			continue;
		const float w = pS->GetWeight();
		if (w <= 0.001f)
		{
			pS->SetActive(false);
			continue;
		}
		fOthers += w;
		pS->FadeOut(fT / w);
	}

	/* 2. the new state. */
	if (pNew->IsActive() == false)
	{
		pNew->SetActive(true); /* clears fadeStep/paused — BEFORE FadeIn */
		pNew->SetLoop(abLoop);
		pNew->SetTimePosition(abMatchPhase ? fPhase * pNew->GetLength() : 0.0f);
		if (fOthers < 0.01f)
		{
			pNew->SetWeight(1.0f);
			pNew->SetFadeStep(0.0f);
		}
		else
		{
			pNew->SetWeight(0.0f); /* FadeIn does not zero it */
			pNew->FadeIn(fT);      /* 0 -> 1 in T */
		}
	}
	else
	{
		/* Already active (an interrupted fade-out, A->B->A): resume from the
		   current weight so it reaches 1 at the same T the others reach 0. */
		pNew->SetLoop(abLoop);
		const float w = pNew->GetWeight();
		if (w < 0.999f)
			pNew->FadeIn(fT / (1.0f - w));
		else
		{
			pNew->SetWeight(1.0f);
			pNew->SetFadeStep(0.0f);
		}
		if (abLoop == false)
			pNew->SetTimePosition(0.0f); /* a re-triggered one-shot restarts */
	}

	msCurrentClip = asClip;
}

//-----------------------------------------------------------------------

void cGhostPlayer::ApplyClipSpeeds()
{
	if (mpBodyEntity == NULL)
		return;
	const int lStateNum = mpBodyEntity->GetAnimationStateNum();
	for (int i = 0; i < lStateNum; ++i)
	{
		cAnimationState *pS = mpBodyEntity->GetAnimationState(i);
		if (pS == NULL || pS->IsActive() == false)
			continue;
		const tString sName = pS->GetName();
		const float fGait = GaitFor(sName);
		float fSpeed = 1.0f;
		if (fGait > 0.05f)
		{
			if (mbMoving)
			{
				fSpeed = mfLocomotionSpeed / fGait;
				if (fSpeed < kClipSpeedMin) fSpeed = kClipSpeedMin;
				else if (fSpeed > kClipSpeedMax) fSpeed = kClipSpeedMax;
			}
			else if (sName == "walk" &&
				mpBodyEntity->GetAnimationStateFromName("idle") == NULL)
				fSpeed = 0.0f; /* no idle clip: the walk stand-in stays frozen,
				                  also while it fades out toward another clip */
		}
		pS->SetSpeed(fSpeed);
	}
}

//-----------------------------------------------------------------------

void cGhostPlayer::SelectLocomotion(const cGhostSample &aS, float afRenderSpeed, float afTimeStep)
{
	if (mbAnimsLoaded == false || mpBodyEntity == NULL)
		return;

	const bool bOnGround = (aS.mFlags & eNetPlayerFlag_OnGround) != 0;
	const bool bCrouch = (aS.mFlags & eNetPlayerFlag_Crouch) != 0;
	const bool bRunKey = (aS.mFlags & eNetPlayerFlag_RunKey) != 0;
	const bool bJumpState = (aS.mFlags & eNetPlayerFlag_Jump) != 0;

	/* Locomotion speed + direction: the wire velocity is exact and body-local
	   already. If the wire says still but the position moves (a guest the
	   host teleported, a moving platform), fall back to the render velocity
	   projected on the view frame (forward = (-sin y, 0, -cos y), right =
	   (cos y, 0, -sin y)). */
	float fVelFwd = aS.mfVelFwd;
	float fVelRight = aS.mfVelRight;
	float fSpeed = sqrtf(fVelFwd * fVelFwd + fVelRight * fVelRight);
	if (fSpeed < 0.05f && afRenderSpeed > 0.30f)
	{
		const float fSinY = sinf(aS.mfYaw);
		const float fCosY = cosf(aS.mfYaw);
		fVelFwd = -mfRenderVelX * fSinY - mfRenderVelZ * fCosY;
		fVelRight = mfRenderVelX * fCosY - mfRenderVelZ * fSinY;
		fSpeed = afRenderSpeed;
	}
	mfLocomotionSpeed = fSpeed;

	/* Enter/leave hysteresis on the exact signal: 1 frame reaction, no
	   flapping across a single boundary. */
	const float fMoveEnter = bCrouch ? 0.18f : 0.25f;
	const float fMoveLeave = bCrouch ? 0.10f : 0.15f;
	mbMoving = fSpeed >= (mbMoving ? fMoveLeave : fMoveEnter);

	/* --- preview: a forced clip owns the pose ------------------------- */
	if (msForcedClip.empty() == false)
	{
		cAnimationState *pForced = mpBodyEntity->GetAnimationStateFromName(msForcedClip);
		if (pForced)
		{
			if (msCurrentClip != msForcedClip)
			{
				const tString sPrev = msCurrentClip;
				CrossfadeTo(msForcedClip, IsOneShotAnim(msForcedClip) == false, kFadeLocomotion, false);
				msCurrentTarget = msForcedClip;
				msOneShot = "";
				mfForcedOverTime = 0.0f;
				AnimLog("anim '%s' -> '%s' (forced, len %.2f s)", sPrev.c_str(),
					msForcedClip.c_str(), pForced->GetLength());
			}
			else if (pForced->IsLooping() == false && pForced->IsOver())
			{
				mfForcedOverTime += afTimeStep;
				if (mfForcedOverTime >= 1.0f)
				{
					pForced->SetTimePosition(0.0f); /* replay the one-shot */
					mfForcedOverTime = 0.0f;
				}
			}
			else
				mfForcedOverTime = 0.0f;
		}
		return;
	}

	/* --- edges ---------------------------------------------------------- */
	const bool bLanded = mbHavePrevFlags && mbPrevOnGround == false && bOnGround;
	const bool bCrouchEdge = mbHavePrevFlags && bCrouch != mbPrevCrouch;
	mbHavePrevFlags = true;
	mbPrevOnGround = bOnGround;
	mbPrevCrouch = bCrouch;

	if (bOnGround)
	{
		mfAirborneTime = 0.0f;
		mbJumpClipFired = false; /* re-armed by ground contact */
		mfSinceLanded = bLanded ? 0.0f : mfSinceLanded + afTimeStep;
		if (mfSinceLanded > 10.0f)
			mfSinceLanded = 10.0f;
	}
	else
		mfAirborneTime += afTimeStep;

	/* --- jump: once per airborne stretch ------------------------------
	   Jump key + feet off the floor = the clip starts now; a plain fall
	   (walked off a ledge) starts it after 0.1 s so a stair step does not
	   twitch. The clip then owns the pose until the landing. */
	if (bOnGround == false && mbJumpClipFired == false &&
		(bJumpState || mfAirborneTime > 0.10f))
	{
		mbJumpClipFired = true;
		if (mpBodyEntity->GetAnimationStateFromName("jump"))
		{
			const tString sPrev = msCurrentClip;
			CrossfadeTo("jump", false, kFadeOneShot, false);
			msOneShot = "jump";
			msCurrentTarget = "jump";
			AnimLog("anim '%s' -> 'jump' (play once, %.2f m/s%s)", sPrev.c_str(),
				fSpeed, bJumpState ? "" : ", fall");
		}
	}

	/* --- stance transitions: play-once on the crouch flag's edges -------- */
	if (bCrouchEdge && msOneShot != "jump")
	{
		const char *pTransName = bCrouch ? "stand_to_crouch" : "crouch_to_stand";
		cAnimationState *pTrans = mpBodyEntity->GetAnimationStateFromName(pTransName);

		/* The transition clips are authored in place — playing one while the
		   remote is measurably moving renders a slide, which looks worse than
		   a straight blend into the crouch/stand locomotion clip. */
		const bool bTooFast = (fSpeed >= 0.45f);

		if (pTrans && bTooFast == false && bOnGround)
		{
			const tString sPrev = msCurrentClip;
			CrossfadeTo(pTransName, false, kFadeOneShot, false);
			msOneShot = pTransName;
			msCurrentTarget = pTransName;

			/* Height and pose must arrive together: stretch the body Y-offset
			   lerp over exactly the clip's duration. */
			float fBase = pTrans->GetBaseSpeed();
			if (fBase <= 0.0f)
				fBase = 1.0f;
			mfStanceLerpSeconds = pTrans->GetLength() / fBase;
			if (mfStanceLerpSeconds < 0.15f) mfStanceLerpSeconds = 0.15f;
			else if (mfStanceLerpSeconds > 2.0f) mfStanceLerpSeconds = 2.0f;

			AnimLog("anim '%s' -> '%s' (play once, %.2f s, %.2f m/s)", sPrev.c_str(),
				pTransName, mfStanceLerpSeconds, fSpeed);
		}
		else
		{
			/* Direct blend (clip missing, moving, or airborne): quick offset
			   lerp, and an opposite transition still playing (crouch spam)
			   hands the pose back to the selector this very frame. */
			mfStanceLerpSeconds = 0.15f;
			if (bTooFast && pTrans)
				AnimLog("skipping '%s' (moving %.2f m/s) — direct blend", pTransName, fSpeed);
			if (msOneShot == "stand_to_crouch" || msOneShot == "crouch_to_stand")
				msOneShot = "";
		}
	}

	/* --- a one-shot owns the pose? -------------------------------------- */
	float fFade = kFadeLocomotion;
	if (msOneShot.empty() == false)
	{
		cAnimationState *pShot = mpBodyEntity->GetAnimationStateFromName(msOneShot);
		bool bOwns = (pShot != NULL && pShot->IsActive());
		if (bOwns)
		{
			if (msOneShot == "jump")
			{
				/* Airborne: hold (the clip's end frames are the landing pose,
				   clamped). On the ground: let a standing landing play out
				   briefly, but a player who lands running gets locomotion now. */
				if (bOnGround)
					bOwns = pShot->IsOver() == false && mfSinceLanded < 0.35f && fSpeed < 0.25f;
			}
			else
				bOwns = pShot->IsOver() == false;
		}
		if (bOwns)
			return; /* locomotion selector hands off while a play-once clip runs */
		msOneShot = "";
		fFade = kFadeOneShot; /* reclaim NOW, fast — no frozen last frame */
	}

	/* --- locomotion ------------------------------------------------------ */
	if (mbMoving)
	{
		mfLastMoveAngleDeg = atan2f(fVelRight, fVelFwd) * (180.0f / kPif);
		mlMoveSector = ClassifyMoveSector(mfLastMoveAngleDeg, mlMoveSector);
	}

	tString sTarget;
	if (bCrouch)
	{
		/* crouch overrides direction — no directional crouch clips */
		sTarget = mbMoving ? "crouch_walk" : "crouch_idle";
		mbRunning = false;
	}
	else if (mbMoving == false)
	{
		sTarget = "idle";
		mbRunning = false;
	}
	else
	{
		/* Run = shift held AND actually moving at a run-ish pace (relative
		   to the run clip's gait, enter/leave hysteresis); shared by the
		   forward and strafe sectors (backpedal has no run clip). */
		float fGaitRun = GaitFor("run");
		if (fGaitRun < 0.05f)
			fGaitRun = 1.9f;
		mbRunning = bRunKey && fSpeed >= (mbRunning ? 0.35f * fGaitRun : 0.50f * fGaitRun);

		switch (mlMoveSector)
		{
		case eGhostMoveSector_StrafeR:
			sTarget = mbRunning ? "strafe_run_r" : "strafe_walk_r";
			break;
		case eGhostMoveSector_StrafeL:
			sTarget = mbRunning ? "strafe_run_l" : "strafe_walk_l";
			break;
		case eGhostMoveSector_Back:
			sTarget = "walk_back";
			break;
		default:
			sTarget = mbRunning ? "run" : "walk";
			break;
		}
	}

	/* Map the target to a loaded clip. Standing idle prefers the real idle
	   clip; without one it is the walk clip frozen at frame 0 (speed 0 via
	   ApplyClipSpeeds). Missing clips degrade one step at a time toward the
	   forward set, so a state whose clip failed to load never restarts the
	   fallback every frame. */
	tString sClip;
	{
		tString sTry = sTarget;
		for (int lStep = 0; lStep < 4; ++lStep)
		{
			tString sCandidate = sTry;
			if (sTry == "idle" && mpBodyEntity->GetAnimationStateFromName("idle") == NULL)
				sCandidate = "walk";
			if (mpBodyEntity->GetAnimationStateFromName(sCandidate))
			{
				sClip = sCandidate;
				break;
			}
			const tString sFallback = FallbackAnimTarget(sTry);
			if (sFallback == sTry)
				break;
			sTry = sFallback;
		}
	}
	if (sClip.empty())
		return; /* keep whatever is playing rather than warn-spam per frame */

	if (sTarget == msCurrentTarget && sClip == msCurrentClip)
		return; /* no change; speeds are re-applied every frame anyway */

	const tString sPrevTarget = msCurrentTarget;
	if (sClip != msCurrentClip)
	{
		/* Gait cycle <-> gait cycle keeps the foot phase; idle and one-shots
		   start from their first frame. */
		const bool bMatchPhase = GetDefaultGait(sClip) > 0.0f &&
			GetDefaultGait(msCurrentClip) > 0.0f;
		CrossfadeTo(sClip, true, fFade, bMatchPhase);
	}
	else if (IsIdleTarget(sTarget) && sClip != sTarget)
	{
		/* same clip, now standing in for a missing idle: freeze it on its
		   verified standing first frame, not mid-stride (speed 0 follows in
		   ApplyClipSpeeds) */
		cAnimationState *pSame = mpBodyEntity->GetAnimationStateFromName(sClip);
		if (pSame)
			pSame->SetTimePosition(0.0f);
	}
	msCurrentTarget = sTarget;

	AnimLog("anim '%s' -> '%s' (clip '%s', %.2f m/s, dir %.0f deg, fade %.2f s)",
		sPrevTarget.c_str(), sTarget.c_str(), sClip.c_str(), fSpeed,
		mfLastMoveAngleDeg, fFade);
}

//-----------------------------------------------------------------------

bool cGhostPlayer::DebugPlayClip(const tString &asClip)
{
	if (asClip.empty())
	{
		msForcedClip = "";
		mfForcedOverTime = 0.0f;
		return true; /* automatic selection resumes on the next Update */
	}
	if (mpBodyEntity == NULL || mbAnimsLoaded == false ||
		mpBodyEntity->GetAnimationStateFromName(asClip) == NULL)
		return false;
	msForcedClip = asClip;
	mfForcedOverTime = 0.0f;
	return true;
}

//-----------------------------------------------------------------------
