/*
 * Phase 5 shared physics — see BodySync.h for the design overview.
 */
#include "StdAfx.h"
#include "BodySync.h"

#include "scene/World3D.h"
#include "physics/PhysicsWorld.h"
#include "physics/PhysicsBody.h"
#include "math/Math.h"

#include <cmath>
#include <cstring>

using namespace hpl;

/* Same trace gate the loader/ghost instrumentation uses (its home is the
   engine's MeshLoaderCollada.h); declared locally so this file doesn't pull
   collada loader headers. Set 0 here to silence object-sync tracing. */
#ifndef GHOST_LOADER_TRACE
#define GHOST_LOADER_TRACE 1
#endif

//-----------------------------------------------------------------------

cBodySync::cBodySync()
	: mpWorld(NULL)
	  , mlWorldBodyCount(-1)
	  , mlRoundRobinCursor(0)
	  , mlKeyframeCursor(0)
	  , mbCensusDone(false)
	  , mlCensusCount(0)
	  , mlCensusChecksum(0)
	  , mbRemoteCensusKnown(false)
	  , mbCensusPairChecked(false)
	  , mbCensusMatched(false)
	  , mfStatAccum(0)
	  , mlStatStreamed(0)
	  , mlStatApplied(0)
	  , mlStatBytesOut(0)
	  , mlBatchSeqOut(0)
	  , mlMapGen(0)
	  , mlRemoteMapGen(0)
	  , mbRemoteMapGenKnown(false)
	  , mfGuestClock(0)
	  , mlGuestHeldHash(0)
	  , mbGuestHeldPick(false)
	  , mvGuestHeldRelPick(0, 0, 0)
	  , mvGuestHeldTarget(0, 0, 0)
	  , mbGuestHeldHasTarget(false)
	  , mfMaxPidForce(80.0f)      /* game.cfg Interaction_Grab defaults */
	  , mfMaxThrowImpulse(13.0f)
{
	memset(&mRemoteCensus, 0, sizeof(mRemoteCensus));
}

//-----------------------------------------------------------------------

/** Streamable = something physics can toss around. Mass 0 covers both static
    geometry AND player/enemy capsules (cCharacterBody sets its internal
    bodies to mass 0); IsCharacter is belt-and-braces so a character tweak can
    never put a player capsule on the wire. Ghost players are mesh-only (no
    physics body), so nothing extra to skip there. */
bool cBodySync::IsReplicable(iPhysicsBody *apBody)
{
	if (apBody == NULL)
		return false;
	if (apBody->GetMass() <= 0.0f || apBody->IsCharacter())
		return false;
	return apBody->GetName().empty() == false;
}

/** Ragdoll bones (Mesh.cpp creates "<entity>_<bone>" bodies, mass 1, for
    every enemy at load) stay in the census — both sides load them, and the
    census must not change meaning between builds — but are NOT indexed:
    the enemy code owns them (inactive while alive, the LOCAL death ragdoll
    after), and driving each bone toward a host pose one by one fights the
    ragdoll joints (the bones spin/jitter). Not indexed also means a guest's
    grab on a corpse bone is not forwarded: it moves the local ragdoll. */
bool cBodySync::IsStreamable(iPhysicsBody *apBody)
{
	return IsReplicable(apBody) && !apBody->IsRagDoll();
}

//-----------------------------------------------------------------------

bool cBodySync::Update(cWorld3D *apWorld)
{
	if (apWorld != mpWorld)
	{
		/* Cached iPhysicsBody pointers died with the old world, and the send
		   records refer to objects that no longer exist. The RECEIVED census
		   is deliberately kept — see the header note. */
		mpWorld = apWorld;
		m_mapBodies.clear();
		m_mapSent.clear();
		m_mapRemoteGrabs.clear(); /* held bodies died with their world */
		m_mapHolders.clear();
		m_mapPendingRestore.clear();
		m_mapGuestBodies.clear(); /* per-body seq knowledge dies with the world too */
		mlGuestHeldHash = 0;
		mbGuestHeldHasTarget = false;
		mlWorldBodyCount = -1;
		mlRoundRobinCursor = 0;
		mlKeyframeCursor = 0;
		mbCensusDone = false;
		mbCensusPairChecked = false;
		mbCensusMatched = false;
		++mlMapGen; /* stale-map packets identify themselves by this */
	}

	if (mbCensusDone || mpWorld == NULL)
		return false;
	iPhysicsWorld *pPhysics = mpWorld->GetPhysicsWorld();
	if (pPhysics == NULL)
		return false;

	ComputeCensus(pPhysics);
	Log(" multiplayer: physics census: %u replicable bodies, checksum 0x%08X\n",
		(unsigned)mlCensusCount, mlCensusChecksum);
	VerifyCensus();
	return true;
}

//-----------------------------------------------------------------------

void cBodySync::ComputeCensus(iPhysicsWorld *apPhysics)
{
	uint32_t lHash = 2166136261u; /* FNV-1a basis, same run as NetHashName */
	uint32_t lCount = 0;

	cPhysicsBodyIterator it = apPhysics->GetBodyIterator();
	while (it.HasNext())
	{
		iPhysicsBody *pBody = it.Next();
		if (IsReplicable(pBody) == false)
			continue;
		/* The terminating NUL is folded in as the name separator. */
		const char *s = pBody->GetName().c_str();
		for (;; ++s)
		{
			lHash ^= (uint8_t)*s;
			lHash *= 16777619u;
			if (*s == '\0')
				break;
		}
		++lCount;
	}

	mlCensusCount = (uint16_t)(lCount > 0xFFFFu ? 0xFFFFu : lCount);
	mlCensusChecksum = lHash;
	mbCensusDone = true;
	mbCensusPairChecked = false;
}

void cBodySync::BuildCensusPacket(cNetBodyCensus *apOut) const
{
	apOut->mType = eNetPacketType_BodyCensus;
	apOut->mMapGen = mlMapGen;
	apOut->mlBodyCount = mlCensusCount;
	apOut->mlChecksum = mlCensusChecksum;
}

void cBodySync::OnCensusReceived(const cNetBodyCensus &aCensus)
{
	mRemoteCensus = aCensus;
	mbRemoteCensusKnown = true;
	mlRemoteMapGen = aCensus.mMapGen; /* state batches must match this now */
	mbRemoteMapGenKnown = true;
	mbCensusPairChecked = false;
	/* A census starts a new pairing (map load, reconnect, a restarted host
	   whose batch counter began again at 1): forget the per-body sequence
	   knowledge so the first states of the new pairing are never mistaken
	   for stale ones. */
	for (std::map<uint32_t, cGuestBody>::iterator it = m_mapGuestBodies.begin();
		 it != m_mapGuestBodies.end(); ++it)
		it->second.mfRecvClock = -1.0e9f;
	VerifyCensus();
}

/** Compare host census vs local census once both exist; logged exactly once
    per pairing (a new packet or a new map load re-arms it). */
void cBodySync::VerifyCensus()
{
	if (mbCensusPairChecked || !mbCensusDone || !mbRemoteCensusKnown)
		return;
	mbCensusPairChecked = true;
	mbCensusMatched = false;

	if (mRemoteCensus.mlBodyCount == mlCensusCount &&
		mRemoteCensus.mlChecksum == mlCensusChecksum)
	{
		mbCensusMatched = true;
		Log(" multiplayer: physics census MATCH — %u bodies, checksum 0x%08X\n",
			(unsigned)mlCensusCount, mlCensusChecksum);
		return;
	}

	Log(" multiplayer: !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
	Log(" multiplayer: !!! PHYSICS CENSUS MISMATCH — object identities can\n");
	Log(" multiplayer: !!! NOT be trusted, object sync will misbehave.\n");
	Log(" multiplayer: !!!   host : %u bodies, checksum 0x%08X\n",
		(unsigned)mRemoteCensus.mlBodyCount, mRemoteCensus.mlChecksum);
	Log(" multiplayer: !!!   local: %u bodies, checksum 0x%08X\n",
		(unsigned)mlCensusCount, mlCensusChecksum);
	Log(" multiplayer: !!! Divergent load order or a different map. If both\n");
	Log(" multiplayer: !!! machines are on the same map, REPORT THIS LINE.\n");
	Log(" multiplayer: !!! (Transient during a level change: re-verified on\n");
	Log(" multiplayer: !!! the next census packet.)\n");
	Log(" multiplayer: !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!\n");
}

//-----------------------------------------------------------------------

/** (Re)index name-hash -> body for the CURRENT world. Cheap staleness probe:
    any body create/destroy changes the total count and forces a rebuild, so a
    cached pointer never outlives its body by more than one probe. (A same-tick
    destroy+create pair keeping the count equal would slip through — rare in
    these maps; the rung 4 keyframes are the designed recovery.) */
void cBodySync::RebuildIndexIfNeeded()
{
	iPhysicsWorld *pPhysics = mpWorld ? mpWorld->GetPhysicsWorld() : NULL;
	if (pPhysics == NULL)
	{
		m_mapBodies.clear();
		mlWorldBodyCount = -1;
		return;
	}

	int lTotal = 0;
	cPhysicsBodyIterator cntIt = pPhysics->GetBodyIterator();
	while (cntIt.HasNext())
	{
		cntIt.Next();
		++lTotal;
	}
	if (lTotal == mlWorldBodyCount && m_mapBodies.empty() == false)
		return;

	/* m_mapSent survives rebuilds on purpose: hashes stay valid within a
	   map, and keeping the records avoids a re-send burst of every body. */
	m_mapBodies.clear();
	int lDynamic = 0;
	cPhysicsBodyIterator it = pPhysics->GetBodyIterator();
	while (it.HasNext())
	{
		iPhysicsBody *pBody = it.Next();
		if (IsStreamable(pBody) == false)
			continue;
		const tString &sName = pBody->GetName();
		const uint32_t lHash = NetHashName(sName.c_str());
		std::map<uint32_t, iPhysicsBody *>::iterator dup = m_mapBodies.find(lHash);
		if (dup != m_mapBodies.end())
		{
			Log(" multiplayer: object name-hash collision '%s' vs '%s' — second body NOT synced\n",
				dup->second->GetName().c_str(), sName.c_str());
			continue;
		}
		m_mapBodies[lHash] = pBody;
		++lDynamic;
	}
	mlWorldBodyCount = lTotal;
#if GHOST_LOADER_TRACE
	Log(" multiplayer: object sync indexed %d dynamic bodies (%d total in world)\n",
		lDynamic, lTotal);
#endif
}

//-----------------------------------------------------------------------

void cBodySync::BuildStateBatches(unsigned char *apMoving, size_t *apMovingLen,
	unsigned char *apSleep, size_t *apSleepLen)
{
	*apMovingLen = 0;
	*apSleepLen = 0;

	RebuildIndexIfNeeded();
	if (m_mapBodies.empty())
		return;

	int lMoving = 0;
	int lSleep = 0;

	/* Round-robin from just past the last body written, so the per-tick cap
	   cannot starve anything during a mass upset (shelf collapse). */
	typedef std::map<uint32_t, iPhysicsBody *>::iterator tBodyIt;
	tBodyIt it = m_mapBodies.upper_bound(mlRoundRobinCursor);
	if (it == m_mapBodies.end())
		it = m_mapBodies.begin();

	const size_t lBodies = m_mapBodies.size();
	for (size_t n = 0; n < lBodies && lMoving < kMaxStatesPerBatch &&
		 lSleep < kMaxStatesPerBatch; ++n)
	{
		const uint32_t lHash = it->first;
		iPhysicsBody *pBody = it->second;
		++it;
		if (it == m_mapBodies.end())
			it = m_mapBodies.begin();

		cSendRecord &rec = m_mapSent[lHash];
		const bool bEnabled = pBody->GetEnabled();

		/* Sleeping bodies are wire-silent EXCEPT one final state on the
		   awake->asleep edge — the rest pose that must match. It goes in the
		   RELIABLE batch: it is sent exactly once, so it may not be lost. */
		const bool bSleepEdge = (rec.mbWasEnabled && !bEnabled);
		rec.mbWasEnabled = bEnabled;
		if (!bEnabled && !bSleepEdge)
			continue;

		const cMatrixf mtx = pBody->GetWorldMatrix();
		const cVector3f vPos = mtx.GetTranslation();
		cQuaternion qRot;
		qRot.FromRotationMatrix(mtx.GetRotation());

		if (bEnabled && rec.mbEverSent)
		{
			/* moved > 1 cm or rotated > 1 deg since the last send, else silent.
			   |q·q'| > cos(0.5 deg) means the orientation delta is under 1 deg. */
			const cVector3f vD = vPos - rec.mvPos;
			const float fDistSq = vD.x * vD.x + vD.y * vD.y + vD.z * vD.z;
			const float fQDot = fabsf(qRot.v.x * rec.mqRot.v.x + qRot.v.y * rec.mqRot.v.y +
				qRot.v.z * rec.mqRot.v.z + qRot.w * rec.mqRot.w);
			if (fDistSq < 0.01f * 0.01f && fQDot > 0.9999619f)
				continue;
		}

		cNetObjectState st;
		st.mlNameHash = lHash;
		st.mfPosX = vPos.x; st.mfPosY = vPos.y; st.mfPosZ = vPos.z;
		st.mfRotX = qRot.v.x; st.mfRotY = qRot.v.y; st.mfRotZ = qRot.v.z; st.mfRotW = qRot.w;
		st.mFlags = bEnabled ? 0 : kNetObjectFlag_Sleeping;

		if (bSleepEdge)
		{
			memcpy(apSleep + sizeof(cNetObjectStateBatch) + (size_t)lSleep * sizeof(cNetObjectState),
				&st, sizeof(st));
			++lSleep;
		}
		else
		{
			memcpy(apMoving + sizeof(cNetObjectStateBatch) + (size_t)lMoving * sizeof(cNetObjectState),
				&st, sizeof(st));
			++lMoving;
		}
		rec.mvPos = vPos;
		rec.mqRot = qRot;
		rec.mbEverSent = true;
		mlRoundRobinCursor = lHash;
	}

	/* Keyframe trickle: re-announce a couple of SLEEPING bodies per tick in
	   the moving batch's spare slots (unreliable — the next sweep repeats
	   it). Before this, a guest copy that drifted from a host body that is
	   asleep (a guest-only character bump, a body the host never woke, a
	   rest pose applied before the guest's own load settle finished) stayed
	   wrong until the host happened to move it — then it popped, or the
	   host's pose appeared inside/through geometry. Guests pin these like
	   any rest pose; one already pinned at that pose is left untouched.
	   Never touches m_mapSent: the delta stream's bookkeeping is unchanged. */
	if (lMoving < kMaxStatesPerBatch)
	{
		tBodyIt kit = m_mapBodies.upper_bound(mlKeyframeCursor);
		int lKeys = 0;
		for (size_t n = 0; n < lBodies && lKeys < kKeyframesPerTick &&
			 lMoving < kMaxStatesPerBatch; ++n)
		{
			if (kit == m_mapBodies.end())
				kit = m_mapBodies.begin();
			const uint32_t lHash = kit->first;
			iPhysicsBody *pBody = kit->second;
			++kit;
			mlKeyframeCursor = lHash;

			if (pBody->GetEnabled() || !pBody->IsActive())
				continue; /* awake: the delta stream covers it */
			std::map<uint32_t, cSendRecord>::const_iterator ri = m_mapSent.find(lHash);
			if (ri != m_mapSent.end() && ri->second.mbWasEnabled)
				continue; /* its reliable sleep edge is still due */

			const cMatrixf mtx = pBody->GetWorldMatrix();
			const cVector3f vPos = mtx.GetTranslation();
			cQuaternion qRot;
			qRot.FromRotationMatrix(mtx.GetRotation());

			cNetObjectState st;
			st.mlNameHash = lHash;
			st.mfPosX = vPos.x; st.mfPosY = vPos.y; st.mfPosZ = vPos.z;
			st.mfRotX = qRot.v.x; st.mfRotY = qRot.v.y; st.mfRotZ = qRot.v.z; st.mfRotW = qRot.w;
			st.mFlags = kNetObjectFlag_Sleeping;
			memcpy(apMoving + sizeof(cNetObjectStateBatch) + (size_t)lMoving * sizeof(cNetObjectState),
				&st, sizeof(st));
			++lMoving;
			++lKeys;
		}
	}

	cNetObjectStateBatch hdr;
	hdr.mType = eNetPacketType_ObjectState;
	hdr.mMapGen = mlMapGen;
	if (lMoving > 0)
	{
		hdr.mCount = (uint8_t)lMoving;
		hdr.mSeq = ++mlBatchSeqOut;
		memcpy(apMoving, &hdr, sizeof(hdr));
		*apMovingLen = sizeof(hdr) + (size_t)lMoving * sizeof(cNetObjectState);
	}
	if (lSleep > 0)
	{
		hdr.mCount = (uint8_t)lSleep;
		hdr.mSeq = ++mlBatchSeqOut;
		memcpy(apSleep, &hdr, sizeof(hdr));
		*apSleepLen = sizeof(hdr) + (size_t)lSleep * sizeof(cNetObjectState);
	}
	mlStatStreamed += lMoving + lSleep;
	mlStatBytesOut += (int)(*apMovingLen + *apSleepLen);
}

//-----------------------------------------------------------------------

size_t cBodySync::BuildSnapshotChunk(unsigned char *apBuf, uint32_t *apCursor)
{
	RebuildIndexIfNeeded();

	typedef std::map<uint32_t, iPhysicsBody *>::iterator tBodyIt;
	tBodyIt it = m_mapBodies.upper_bound(*apCursor);
	int lCount = 0;

	for (; it != m_mapBodies.end() && lCount < kMaxStatesPerBatch; ++it)
	{
		const uint32_t lHash = it->first;
		iPhysicsBody *pBody = it->second;
		const bool bEnabled = pBody->GetEnabled();

		const cMatrixf mtx = pBody->GetWorldMatrix();
		const cVector3f vPos = mtx.GetTranslation();
		cQuaternion qRot;
		qRot.FromRotationMatrix(mtx.GetRotation());

		cNetObjectState st;
		st.mlNameHash = lHash;
		st.mfPosX = vPos.x; st.mfPosY = vPos.y; st.mfPosZ = vPos.z;
		st.mfRotX = qRot.v.x; st.mfRotY = qRot.v.y; st.mfRotZ = qRot.v.z; st.mfRotW = qRot.w;
		st.mFlags = bEnabled ? 0 : kNetObjectFlag_Sleeping;

		memcpy(apBuf + sizeof(cNetObjectStateBatch) + (size_t)lCount * sizeof(cNetObjectState),
			&st, sizeof(st));

		/* READ-ONLY with respect to m_mapSent. The send records are the
		   delta bookkeeping of the stream EVERY guest receives; this chunk
		   goes to ONE peer (a MapReady joiner). Priming them here (as up to
		   v16 did) overwrote mbWasEnabled of a body that fell asleep since
		   the last batch, so the reliable awake->asleep rest pose never went
		   out to the guests that were ALREADY connected — every 3rd/4th
		   player joining cost the others their rest poses. The price of not
		   priming is at most one redundant unreliable state per awake body
		   for the joiner; a missing record defaults to "was asleep, never
		   sent", which cannot fake a sleep edge either. */

		*apCursor = lHash;
		++lCount;
	}

	if (lCount == 0)
		return 0;

	cNetObjectStateBatch hdr;
	hdr.mType = eNetPacketType_ObjectState;
	hdr.mCount = (uint8_t)lCount;
	hdr.mMapGen = mlMapGen;
	hdr.mSeq = ++mlBatchSeqOut;
	memcpy(apBuf, &hdr, sizeof(hdr));
	mlStatBytesOut += (int)(sizeof(hdr) + (size_t)lCount * sizeof(cNetObjectState));
	return sizeof(hdr) + (size_t)lCount * sizeof(cNetObjectState);
}

//-----------------------------------------------------------------------
// Guest apply. The host's world is the truth; ours only has to LOOK like
// it, without fighting our own Newton. Tuning in seconds / metres / radians.
//-----------------------------------------------------------------------

namespace
{
/* P term: the velocity that closes the error in this long (~6 frames) —
   stiff enough to track a thrown object, soft enough that contacts win. */
const float kDriveTime = 0.1f;
/* Speed clamps. Free bodies 10 m/s = 17 cm per 60 Hz step, under the wall
   thickness of these maps (Newton 1.x runs without CCD). Jointed bodies get
   gentle clamps: a pose the local joint cannot reach (lock state, limits)
   must never turn into a constraint force big enough to break the joint. */
const float kMaxDriveSpeed = 10.0f;
const float kMaxDriveOmega = 20.0f;
const float kMaxJointSpeed = 4.0f;
const float kMaxJointOmega = 6.0f;
/* Feed-forward: host motion estimated from consecutive states. */
const float kMaxFfSpeed = 20.0f;
const float kMaxFfOmega = 25.0f;
const int kMaxFfSeqGap = 6;     /* a sample pair further apart says nothing */
const float kExtrapMax = 0.1f;  /* dead-reckon the target at most 3 ticks */
/* Hard snaps: pose set, motion zeroed. */
const float kSnapDist = 1.5f;   /* teleport-sized: no sliding across the room */
const float kSnapAngle = 2.6f;  /* ~150 deg, also where the axis degenerates */
const float kStuckDist = 0.35f;
const float kStuckAngle = 1.0f;
const float kStuckTime = 0.5f;  /* blocked by something only we have */
const float kStuckTimeJoint = 1.5f;
/* Rest handling. */
const float kStaleTime = 1.0f;  /* host silent on an awake body = holding still */
const float kPinDist = 0.02f;
const float kPinAngle = 0.035f; /* ~2 deg */
const float kSettleTime = 0.75f;
const float kDisturbGrace = 0.5f;
const float kQuietDist = 0.005f;
const float kQuietAngle = 0.009f;
const float kSamePoseDist = 0.002f;
const float kSamePoseDot = 0.99999f; /* |q.q'| ~ 0.5 deg */
/* Per-body seq guard: a reorder never spans this long; past it the uint16
   counter may have wrapped (~18 min at 30 Hz), so the new arrival wins. */
const float kSeqWindow = 10.0f;
/* Held prediction. */
const float kHeldGain = 14.0f;
const float kMaxHeldSpeed = 8.0f;

/* cVector3f::Length() is not const-qualified in this engine. */
float VecLen(const cVector3f &avV)
{
	return sqrtf(avV.x * avV.x + avV.y * avV.y + avV.z * avV.z);
}

cVector3f ClampLength(const cVector3f &avV, float afMax)
{
	const float fLen = VecLen(avV);
	if (fLen > afMax && fLen > 0.0f)
		return avV * (afMax / fLen);
	return avV;
}

/* Wire floats are untrusted: NaN/inf/absurd positions never reach Newton. */
bool IsSaneCoord(float afX)
{
	return afX == afX && afX < 1.0e5f && afX > -1.0e5f;
}

/** Axis-angle of the WORLD-frame rotation taking aCur's orientation onto
    aTgt's: E = Rt * Rc^T (HPL matrices act on column vectors, v' = M v, the
    same convention cQuaternion::To/FromRotationMatrix use). Axis from the
    skew part, angle via atan2: sign-agnostic (q and -q are one matrix, so
    no hemisphere flip can make it spin the long way) and well-conditioned
    at every angle; near 180 deg the axis is undefined and comes back zero
    (callers hard-snap there). Newton's omega is world-space, right-handed:
    axis * angle / T turns aCur toward aTgt. */
void RotationError(const cMatrixf &aTgt, const cMatrixf &aCur, cVector3f *apAxis, float *apAngle)
{
	float e[3][3];
	for (int i = 0; i < 3; ++i)
		for (int j = 0; j < 3; ++j)
			e[i][j] = aTgt.m[i][0] * aCur.m[j][0] + aTgt.m[i][1] * aCur.m[j][1] +
				aTgt.m[i][2] * aCur.m[j][2];
	const cVector3f vSkew(e[2][1] - e[1][2], e[0][2] - e[2][0], e[1][0] - e[0][1]);
	const float fSkew = VecLen(vSkew); /* = 2 sin(angle) */
	float fCos = 0.5f * (e[0][0] + e[1][1] + e[2][2] - 1.0f);
	if (fCos > 1.0f)
		fCos = 1.0f;
	if (fCos < -1.0f)
		fCos = -1.0f;
	*apAngle = atan2f(0.5f * fSkew, fCos);
	if (fSkew > 1.0e-6f)
		*apAxis = vSkew / fSkew;
	else
		*apAxis = cVector3f(0, 0, 0);
}

/** aRot turned by the world-space rotation vector avRotVec (axis * angle). */
cMatrixf RotateWorld(const cMatrixf &aRot, const cVector3f &avRotVec)
{
	const float fAngle = VecLen(avRotVec);
	if (fAngle < 1.0e-6f)
		return aRot;
	cQuaternion q;
	q.FromAngleAxis(fAngle, avRotVec / fAngle);
	return cMath::MatrixMul(cMath::MatrixQuaternion(q), aRot);
}

float QuatAbsDot(const cQuaternion &a, const cQuaternion &b)
{
	return fabsf(a.v.x * b.v.x + a.v.y * b.v.y + a.v.z * b.v.z + a.w * b.w);
}
} // namespace

//-----------------------------------------------------------------------

void cBodySync::PinGuestBody(iPhysicsBody *apBody, cGuestBody &aRec)
{
	cMatrixf mtx = cMath::MatrixQuaternion(aRec.mqRot);
	mtx.SetTranslation(aRec.mvPos);
	apBody->SetMatrix(mtx);
	apBody->SetLinearVelocity(cVector3f(0, 0, 0));
	apBody->SetAngularVelocity(cVector3f(0, 0, 0));
	apBody->SetEnabled(false); /* Newton freeze: exactly where the host has it */
	aRec.mbPinned = true;
	aRec.mfSettleTime = 0;
	aRec.mfStuckTime = 0;
	aRec.mfDisturbTime = 0;
}

void cBodySync::ApplyStateBatch(const void *apData, size_t alLen)
{
	if (!apData || alLen < sizeof(cNetObjectStateBatch))
		return;

	cNetObjectStateBatch hdr;
	memcpy(&hdr, apData, sizeof(hdr));
	/* Stale-map guard (v4): only apply batches from the generation the
	   census verified. Packets in flight across a level change die here. */
	if (mbRemoteMapGenKnown && hdr.mMapGen != mlRemoteMapGen)
		return;

	const unsigned char *pRaw = (const unsigned char *)apData;
	size_t lCount = hdr.mCount;
	const size_t lWhole = (alLen - sizeof(cNetObjectStateBatch)) / sizeof(cNetObjectState);
	if (lCount > lWhole)
		lCount = lWhole; /* truncated/corrupt length: apply only whole entries */

	RebuildIndexIfNeeded();
	if (m_mapBodies.empty())
		return;

	for (size_t i = 0; i < lCount; ++i)
	{
		cNetObjectState st;
		memcpy(&st, pRaw + sizeof(cNetObjectStateBatch) + i * sizeof(cNetObjectState), sizeof(st));

		std::map<uint32_t, iPhysicsBody *>::iterator bi = m_mapBodies.find(st.mlNameHash);
		if (bi == m_mapBodies.end())
			continue; /* unknown hash: map mismatch, destroyed body or a ragdoll bone */
		iPhysicsBody *pBody = bi->second;

		/* A degenerate quaternion would scale the body's rotation matrix,
		   a NaN position would poison Newton. Normalize / drop. */
		const float fQLen = sqrtf(st.mfRotX * st.mfRotX + st.mfRotY * st.mfRotY +
			st.mfRotZ * st.mfRotZ + st.mfRotW * st.mfRotW);
		if (!(fQLen > 0.5f && fQLen < 2.0f))
			continue;
		if (!IsSaneCoord(st.mfPosX) || !IsSaneCoord(st.mfPosY) || !IsSaneCoord(st.mfPosZ))
			continue;
		cQuaternion qRot;
		qRot.v.x = st.mfRotX / fQLen;
		qRot.v.y = st.mfRotY / fQLen;
		qRot.v.z = st.mfRotZ / fQLen;
		qRot.w = st.mfRotW / fQLen;
		const cVector3f vPos(st.mfPosX, st.mfPosY, st.mfPosZ);

		/* PER-BODY reorder guard. Moving batches ride the unsequenced
		   channel, rest poses / snapshot chunks the reliable one, keyframes
		   the unsequenced one again — all stamped from one host counter. The
		   old whole-batch guard only protected pure-moving batches, so a
		   reliable rest pose delayed by a retransmit re-pinned (and froze) a
		   body the host had long since thrown again, and a snapshot chunk of
		   all-awake bodies could be dropped whole. Newest per body wins. */
		std::map<uint32_t, cGuestBody>::iterator gi = m_mapGuestBodies.find(st.mlNameHash);
		const bool bKnown = gi != m_mapGuestBodies.end();
		if (bKnown && (mfGuestClock - gi->second.mfRecvClock) < kSeqWindow &&
			(int16_t)(hdr.mSeq - gi->second.mlSeq) <= 0)
			continue;
		cGuestBody &rec = bKnown ? gi->second : m_mapGuestBodies[st.mlNameHash];

		const bool bRest = (st.mFlags & kNetObjectFlag_Sleeping) != 0;
		const bool bHeldByUs = st.mlNameHash == mlGuestHeldHash && !mbGuestHeldPick;
		/* the body was following a live stream until now (not a snapshot,
		   keyframe or first contact) */
		const bool bWasStreaming = bKnown && !rec.mbRest && rec.mfAge < kStaleTime;
		const bool bRecentSample = bKnown && (mfGuestClock - rec.mfRecvClock) < 1.0f;
		const int lSeqGap = (int16_t)(hdr.mSeq - rec.mlSeq);

		if (bRest)
		{
			const bool bSamePose = bKnown && rec.mbRest && rec.mbPinned &&
				VecLen(vPos - rec.mvPos) < kSamePoseDist &&
				QuatAbsDot(qRot, rec.mqRot) > kSamePoseDot;
			rec.mvPos = vPos;
			rec.mqRot = qRot;
			rec.mvVel = cVector3f(0, 0, 0);
			rec.mvOmega = cVector3f(0, 0, 0);
			rec.mbHaveMotion = false;
			rec.mbRest = true;
			if (!bSamePose)
			{
				/* keyframe of an already pinned pose: leave the body alone */
				rec.mbPinned = false;
				rec.mfSettleTime = 0;
				rec.mfStuckTime = 0;
				rec.mfDisturbTime = 0;
			}
			/* Not being driven (late-join snapshot, keyframe correction, first
			   word about this body): pin right now instead of sliding into
			   place. A body that was streaming a moment ago eases the last
			   centimetres in UpdateGuestBlend and pins there. */
			if (!rec.mbPinned && !bWasStreaming && !bHeldByUs && pBody->IsActive())
				PinGuestBody(pBody, rec);
		}
		else
		{
			/* Feed-forward sample: host motion between this state and the
			   previous one, over the HOST's clock (batch seq x send period;
			   receive-time deltas would carry all the internet jitter). */
			if (bRecentSample && lSeqGap >= 1 && lSeqGap <= kMaxFfSeqGap)
			{
				const float fDt = (float)lSeqGap * kNetSendPeriodSeconds;
				const cVector3f vVel = ClampLength((vPos - rec.mvPos) / fDt, kMaxFfSpeed);
				cVector3f vAxis;
				float fAngle;
				RotationError(cMath::MatrixQuaternion(qRot), cMath::MatrixQuaternion(rec.mqRot),
					&vAxis, &fAngle);
				const cVector3f vOmega = ClampLength(vAxis * (fAngle / fDt), kMaxFfOmega);
				if (rec.mbHaveMotion)
				{
					rec.mvVel = (rec.mvVel + vVel) * 0.5f; /* light low-pass */
					rec.mvOmega = (rec.mvOmega + vOmega) * 0.5f;
				}
				else
				{
					rec.mvVel = vVel;
					rec.mvOmega = vOmega;
				}
				rec.mbHaveMotion = true;
			}
			else
			{
				rec.mvVel = cVector3f(0, 0, 0);
				rec.mvOmega = cVector3f(0, 0, 0);
				rec.mbHaveMotion = false;
			}
			rec.mvPos = vPos;
			rec.mqRot = qRot;
			rec.mbRest = false;
			rec.mbPinned = false;
			rec.mfSettleTime = 0;
			rec.mfDisturbTime = 0;
			/* waking happens in the drive, which needs the body awake */
		}
		rec.mlSeq = hdr.mSeq;
		rec.mfRecvClock = mfGuestClock;
		rec.mfAge = 0;

		++mlStatApplied;
		/* NOTE: no per-entry Log here on purpose — at 30 Hz x N bodies the old
		   trace wrote hundreds of lines/second into hpl.log (OneDrive-synced!)
		   and could hitch the frame. The 1 Hz stats line is the health signal. */
	}
}

//-----------------------------------------------------------------------

void cBodySync::UpdateGuestBlend(float afTimeStep)
{
	mfGuestClock += afTimeStep;
	if (afTimeStep <= 0.0f || mpWorld == NULL)
		return;
	if (m_mapGuestBodies.empty() && mlGuestHeldHash == 0)
		return;
	iPhysicsWorld *pPhysics = mpWorld->GetPhysicsWorld();
	if (pPhysics == NULL)
		return;
	/* ONE index probe per frame (GetBodyByHash per record would recount the
	   whole physics world for every body). */
	RebuildIndexIfNeeded();
	/* Newton adds g*dt to every gravity body in the coming step; commanding
	   (v - g*dt) makes the body move at exactly v instead of sagging. */
	const cVector3f vGravityStep = pPhysics->GetGravity() * afTimeStep;

	/* ---- held-object prediction: our copy tracks OUR crosshair target ---- */
	if (mlGuestHeldHash != 0 && !mbGuestHeldPick && mbGuestHeldHasTarget)
	{
		std::map<uint32_t, iPhysicsBody *>::iterator hb = m_mapBodies.find(mlGuestHeldHash);
		iPhysicsBody *pBody = hb == m_mapBodies.end() ? NULL : hb->second;
		if (pBody && pBody->IsActive())
		{
			/* Same "current pick point" math as the grab spring — but driven
			   by VELOCITY. It used to SetMatrix the body toward the crosshair:
			   a teleport is not swept, so aiming behind a wall pulled the box
			   straight through it. Now Newton's contacts stop it at the wall,
			   exactly like the local grab. */
			const cVector3f vCurrent = cMath::MatrixMul(pBody->GetWorldMatrix(),
				pBody->GetMassCentre()) + mvGuestHeldRelPick;
			cVector3f vVel = ClampLength((mvGuestHeldTarget - vCurrent) * kHeldGain, kMaxHeldSpeed);
			if (pBody->GetGravity())
				vVel -= vGravityStep; /* the free grab turned it off; belt and braces */
			pBody->SetEnabled(true);
			pBody->SetLinearVelocity(vVel);
			pBody->SetAngularVelocity(pBody->GetAngularVelocity() * 0.85f);
		}
	}

	/* ---- everything else follows the host ---- */
	for (std::map<uint32_t, cGuestBody>::iterator it = m_mapGuestBodies.begin();
		 it != m_mapGuestBodies.end();)
	{
		const uint32_t lHash = it->first;
		cGuestBody &rec = it->second;
		std::map<uint32_t, iPhysicsBody *>::iterator bi = m_mapBodies.find(lHash);
		if (bi == m_mapBodies.end())
		{
			m_mapGuestBodies.erase(it++); /* body destroyed */
			continue;
		}
		++it;
		iPhysicsBody *pBody = bi->second;
		rec.mfAge += afTimeStep;

		if (lHash == mlGuestHeldHash && !mbGuestHeldPick)
			continue; /* our prediction owns it; rec keeps the host's latest */
		if (!pBody->IsActive())
			continue; /* hidden/disabled entity: not ours to move */

		/* Pinned: frozen at the host's rest pose. If OUR physics woke it (the
		   guest's character bumped it, a local-only contact) the host never
		   saw that — let the bump play out briefly, then bring it back. */
		if (rec.mbPinned)
		{
			if (!pBody->GetEnabled())
			{
				rec.mfDisturbTime = 0;
				continue;
			}
			rec.mfDisturbTime += afTimeStep;
			if (rec.mfDisturbTime < kDisturbGrace)
				continue;
			rec.mbPinned = false;
			rec.mfSettleTime = 0;
			rec.mfStuckTime = 0;
		}

		/* Host silent on an awake body for a second: it moves < 1 cm/tick,
		   i.e. it is holding still there (held in the air, wedged, about to
		   sleep). Hold it at that pose. This used to RELEASE the body to
		   local physics — often already auto-frozen by Newton (its velocity
		   was zeroed every frame), so it stayed floating mid-air. */
		if (!rec.mbRest && rec.mfAge > kStaleTime)
		{
			rec.mbRest = true;
			rec.mbHaveMotion = false;
			rec.mvVel = cVector3f(0, 0, 0);
			rec.mvOmega = cVector3f(0, 0, 0);
			rec.mfSettleTime = 0;
		}

		const bool bJointed = pBody->GetJointNum() > 0;
		const bool bMotion = !rec.mbRest && rec.mbHaveMotion;
		const bool bFeedForward = bMotion && rec.mfAge < kExtrapMax;
		const float fExtrap = bMotion ? (rec.mfAge < kExtrapMax ? rec.mfAge : kExtrapMax) : 0.0f;

		/* Target = host pose dead-reckoned to now (bounded). */
		cMatrixf mtxTgt = cMath::MatrixQuaternion(rec.mqRot);
		if (fExtrap > 0.0f)
			mtxTgt = RotateWorld(mtxTgt, rec.mvOmega * fExtrap);
		mtxTgt.SetTranslation(rec.mvPos + rec.mvVel * fExtrap);

		/* Errors at the CENTRE OF MASS: Newton's velocity is the COM's and
		   its omega turns about the COM, so this is what they close. */
		const cMatrixf mtxCur = pBody->GetWorldMatrix();
		const cVector3f vMassCentre = pBody->GetMassCentre();
		const cVector3f vErr = cMath::MatrixMul(mtxTgt, vMassCentre) -
			cMath::MatrixMul(mtxCur, vMassCentre);
		const float fDist = VecLen(vErr);
		cVector3f vAxis;
		float fAngle;
		RotationError(mtxTgt, mtxCur, &vAxis, &fAngle);

		bool bSnap = fDist > kSnapDist || fAngle > kSnapAngle;
		if (fDist > kStuckDist || fAngle > kStuckAngle)
			rec.mfStuckTime += afTimeStep;
		else
			rec.mfStuckTime = 0;
		if (rec.mfStuckTime > (bJointed ? kStuckTimeJoint : kStuckTime))
			bSnap = true;

		if (rec.mbRest)
		{
			rec.mfSettleTime += afTimeStep;
			if (bSnap || (fDist < kPinDist && fAngle < kPinAngle) || rec.mfSettleTime > kSettleTime)
			{
				PinGuestBody(pBody, rec);
				continue;
			}
		}
		else if (bSnap)
		{
			/* Teleport-sized or stuck: the host's pose is collision-free in
			   the host's world (same static geometry), so snap — and zero
			   the motion so Newton starts clean from it. */
			pBody->SetMatrix(mtxTgt);
			pBody->SetLinearVelocity(cVector3f(0, 0, 0));
			pBody->SetAngularVelocity(cVector3f(0, 0, 0));
			pBody->SetEnabled(true);
			rec.mfStuckTime = 0;
			continue;
		}

		/* Velocity drive: P term + host motion, clamped. Newton integrates it
		   WITH collision, so the body can be blocked but never tunnels, and
		   there is no teleport into penetration for the contact solver to
		   blow apart (the old per-frame SetMatrix + zeroed velocity: the
		   barrel "break dance", the spinning, the auto-freeze mid-air). */
		cVector3f vVel = vErr * (1.0f / kDriveTime);
		cVector3f vOmega = vAxis * (fAngle / kDriveTime);
		if (bFeedForward)
		{
			vVel += rec.mvVel;
			vOmega += rec.mvOmega;
		}
		vVel = ClampLength(vVel, bJointed ? kMaxJointSpeed : kMaxDriveSpeed);
		vOmega = ClampLength(vOmega, bJointed ? kMaxJointOmega : kMaxDriveOmega);

		if (!pBody->GetEnabled())
		{
			/* Newton put it to sleep right on target and the host shows no
			   motion: nothing to do. Otherwise wake it — a frozen body
			   ignores velocities. */
			const bool bQuiet = fDist < kQuietDist && fAngle < kQuietAngle &&
				(!bFeedForward || (VecLen(rec.mvVel) < 0.05f && VecLen(rec.mvOmega) < 0.05f));
			if (bQuiet)
				continue;
			pBody->SetEnabled(true);
		}
		if (pBody->GetGravity())
			vVel -= vGravityStep;
		pBody->SetLinearVelocity(vVel);
		pBody->SetAngularVelocity(vOmega);
	}
}

void cBodySync::SetGuestHeld(uint32_t alHash, bool abPickAtPoint, const cVector3f &avRelPick)
{
	mlGuestHeldHash = alHash;
	mbGuestHeldPick = abPickAtPoint;
	mvGuestHeldRelPick = avRelPick;
	mbGuestHeldHasTarget = false;
	/* The host-state record stays: while the prediction owns the body it
	   keeps collecting the host's echo, which takes over on release. */
}

void cBodySync::UpdateGuestHeldTarget(const cVector3f &avTarget)
{
	mvGuestHeldTarget = avTarget;
	mbGuestHeldHasTarget = true;
}

void cBodySync::ClearGuestHeld()
{
	/* No body mutation to undo: the prediction only ever set velocities
	   (the grab state itself restores gravity/mass/auto-disable). */
	mlGuestHeldHash = 0;
	mbGuestHeldHasTarget = false;
}

//-----------------------------------------------------------------------

//-----------------------------------------------------------------------
// Rung 3 — forwarded intent, host side. The guest sent WHERE it wants the
// body; we run the same style of PID spring cPlayerState_Grab uses, in the
// one authoritative sim, and body sync carries the motion back.
//-----------------------------------------------------------------------

void cBodySync::SetGrabTuning(float afMaxPidForce, float afMaxThrowImpulse)
{
	if (afMaxPidForce > 0)
		mfMaxPidForce = afMaxPidForce;
	if (afMaxThrowImpulse > 0)
		mfMaxThrowImpulse = afMaxThrowImpulse;
}

bool cBodySync::GetHashForBody(iPhysicsBody *apBody, uint32_t *apOut)
{
	if (apBody == NULL || apBody->GetName().empty())
		return false;
	const uint32_t lHash = NetHashName(apBody->GetName().c_str());
	RebuildIndexIfNeeded();
	std::map<uint32_t, iPhysicsBody *>::iterator it = m_mapBodies.find(lHash);
	if (it == m_mapBodies.end() || it->second != apBody)
		return false; /* not replicated (or the collision-skipped twin) */
	*apOut = lHash;
	return true;
}

iPhysicsBody *cBodySync::GetBodyByHash(uint32_t alHash)
{
	RebuildIndexIfNeeded();
	std::map<uint32_t, iPhysicsBody *>::iterator it = m_mapBodies.find(alHash);
	return it == m_mapBodies.end() ? NULL : it->second;
}

//-----------------------------------------------------------------------

uint8_t cBodySync::SetHolder(uint32_t alHash, uint8_t alPlayerId)
{
	uint8_t lPrev = GetHolder(alHash);
	m_mapHolders[alHash] = alPlayerId;
	return lPrev;
}

uint8_t cBodySync::GetHolder(uint32_t alHash) const
{
	std::map<uint32_t, uint8_t>::const_iterator it = m_mapHolders.find(alHash);
	return it == m_mapHolders.end() ? 0 : it->second;
}

void cBodySync::ClearHolder(uint32_t alHash)
{
	m_mapHolders.erase(alHash);
}

//-----------------------------------------------------------------------

void cBodySync::RemoteGrabBegin(uint32_t alHash, uint8_t alPeerId, bool abPickAtPoint,
	const cVector3f &avRelPick, float afMassMul)
{
	iPhysicsBody *pBody = GetBodyByHash(alHash);
	if (pBody == NULL)
		return;

	/* The host player snatched this body from a guest earlier and has let go
	   since: put the true defaults back BEFORE capturing them below. */
	ApplyPendingRestore(alHash, pBody);

	/* A snatch may land on a body another guest still "holds" — restore that
	   grab's body mutations before stacking new ones. */
	std::map<uint32_t, cRemoteGrab>::iterator old = m_mapRemoteGrabs.find(alHash);
	if (old != m_mapRemoteGrabs.end())
	{
		RestoreGrabbedBody(pBody, old->second);
		m_mapRemoteGrabs.erase(old);
	}

	cRemoteGrab grab;
	grab.mlPeerId = alPeerId;
	grab.mbPickAtPoint = abPickAtPoint;
	grab.mvRelPick = avRelPick;
	grab.mbHasTarget = false;
	grab.mfNoTargetTime = 0;
	/* Malformed packet cannot turn the spring into a catapult. */
	grab.mfMassMul = afMassMul < 0.1f ? 0.1f : (afMassMul > 20.0f ? 20.0f : afMassMul);
	grab.mfDefaultMass = pBody->GetMass();
	grab.mbHadGravity = pBody->GetGravity();
	grab.mbHadAutoDisable = pBody->GetAutoDisable();
	grab.mGrabPid.SetErrorNum(10);
	grab.mGrabPid.Reset();
	/* Same gains the grab state uses (PlayerState_Interact.cpp OnUpdate). */
	if (abPickAtPoint)
	{
		grab.mGrabPid.p = 80.0f; grab.mGrabPid.i = 0; grab.mGrabPid.d = 8.0f;
	}
	else
	{
		grab.mGrabPid.p = 180.0f; grab.mGrabPid.i = 0; grab.mGrabPid.d = 40.0f;
		/* Free grab floats the object exactly like EnterState does: gravity
		   off, control mass lowered. (Doors/drawers keep both — the joint
		   carries them.) */
		pBody->SetGravity(false);
		pBody->SetMass(grab.mfDefaultMass / 5.0f);
	}
	pBody->SetAutoDisable(false);
	pBody->SetEnabled(true);

	m_mapRemoteGrabs[alHash] = grab;
}

void cBodySync::RemoteGrabTarget(uint32_t alHash, uint8_t alPeerId, const cVector3f &avTarget)
{
	std::map<uint32_t, cRemoteGrab>::iterator it = m_mapRemoteGrabs.find(alHash);
	if (it == m_mapRemoteGrabs.end() || it->second.mlPeerId != alPeerId)
		return;
	it->second.mvTarget = avTarget;
	it->second.mbHasTarget = true;
	it->second.mfNoTargetTime = 0;
}

void cBodySync::RestoreGrabbedBody(iPhysicsBody *apBody, const cRemoteGrab &aGrab)
{
	if (aGrab.mbPickAtPoint == false)
	{
		apBody->SetGravity(aGrab.mbHadGravity);
		apBody->SetMass(aGrab.mfDefaultMass);
	}
	apBody->SetAutoDisable(aGrab.mbHadAutoDisable);
	apBody->SetEnabled(true);
}

void cBodySync::ApplyPendingRestore(uint32_t alHash, iPhysicsBody *apBody)
{
	std::map<uint32_t, cPendingRestore>::iterator it = m_mapPendingRestore.find(alHash);
	if (it == m_mapPendingRestore.end())
		return;
	if (apBody)
	{
		apBody->SetGravity(it->second.mbGravity);
		apBody->SetMass(it->second.mfMass);
		apBody->SetAutoDisable(it->second.mbAutoDisable);
		apBody->SetEnabled(true); /* fall now if gravity just came back */
	}
	m_mapPendingRestore.erase(it);
}

bool cBodySync::RemoteGrabEnd(uint32_t alHash, uint8_t alPeerId, const cVector3f &avImpulse)
{
	std::map<uint32_t, cRemoteGrab>::iterator it = m_mapRemoteGrabs.find(alHash);
	if (it == m_mapRemoteGrabs.end())
		return false;
	if (alPeerId != 0 && it->second.mlPeerId != alPeerId)
		return false; /* stale end from a peer that already lost the body */

	iPhysicsBody *pBody = GetBodyByHash(alHash);
	/* HOST SNATCH: NetGrabBegin marks the host as holder, THEN ends the
	   guest's grab — from inside cPlayerState_Grab::EnterState, which has
	   already recorded the body's "defaults" (gravity + mass) with this
	   grab's gravity-off and mass/5 still applied, and will write them back
	   on LeaveState: the body used to end up floating, at a fifth of its
	   mass, for the rest of the map. Leave the host's hold alone now and
	   re-apply the true defaults once the host lets go (UpdateRemoteGrabs). */
	const bool bHostSnatch = GetHolder(alHash) == 1 && !it->second.mbPickAtPoint;
	if (pBody && bHostSnatch)
	{
		cPendingRestore pend;
		pend.mfMass = it->second.mfDefaultMass;
		pend.mbGravity = it->second.mbHadGravity;
		pend.mbAutoDisable = it->second.mbHadAutoDisable;
		m_mapPendingRestore[alHash] = pend;
	}
	else if (pBody)
	{
		RestoreGrabbedBody(pBody, it->second);

		cVector3f vImp = avImpulse;
		const float fLen = vImp.Length();
		const float fCap = mfMaxThrowImpulse * 1.5f; /* wire floats are untrusted */
		if (fLen > fCap)
			vImp = vImp * (fCap / fLen);
		if (fLen > 0.001f)
		{
			/* Throw exactly like OnStartExamine: motion reset, then impulse. */
			pBody->SetLinearVelocity(cVector3f(0, 0, 0));
			pBody->SetAngularVelocity(cVector3f(0, 0, 0));
			pBody->AddImpulse(vImp);
		}
	}

	m_mapRemoteGrabs.erase(it);
	if (GetHolder(alHash) == alPeerId || alPeerId == 0)
		ClearHolder(alHash);
	return true;
}

//-----------------------------------------------------------------------

void cBodySync::RemotePush(uint32_t alHash, const cVector3f &avImpulse,
	const cVector3f &avPoint, bool abStop)
{
	iPhysicsBody *pBody = GetBodyByHash(alHash);
	if (pBody == NULL)
		return;

	pBody->SetEnabled(true);
	if (abStop)
	{
		/* The move state's brake: no input this tick -> the body holds still. */
		pBody->SetLinearVelocity(cVector3f(0, 0, 0));
		pBody->SetAngularVelocity(cVector3f(0, 0, 0));
	}

	cVector3f vImp = avImpulse;
	const float fLen = vImp.Length();
	/* Mass-scaled cap (~10 m/s of delta-v): a malformed packet cannot launch
	   a barrel to orbit; honest move-state impulses sit far below this. */
	const float fCap = pBody->GetMass() * 10.0f;
	if (fLen > fCap)
		vImp = vImp * (fCap / fLen);
	if (fLen > 0.0001f)
	{
		pBody->AddImpulseAtPosition(vImp, avPoint);

		/* The guest's own speed limit checks run against ITS zero-velocity
		   copy, so they never brake — enforce the push speed ceiling here. */
		cVector3f vVel = pBody->GetLinearVelocity();
		const float fSpeed = vVel.Length();
		if (fSpeed > 4.0f)
			pBody->SetLinearVelocity(vVel * (4.0f / fSpeed));
	}
}

//-----------------------------------------------------------------------

int cBodySync::ReleaseAllHeldBy(uint8_t alPeerId)
{
	int lReleased = 0;
	for (;;)
	{
		uint32_t lHash = 0;
		for (std::map<uint32_t, cRemoteGrab>::iterator it = m_mapRemoteGrabs.begin();
			 it != m_mapRemoteGrabs.end(); ++it)
		{
			if (it->second.mlPeerId == alPeerId)
			{
				lHash = it->first;
				break;
			}
		}
		if (lHash == 0)
			break;
		RemoteGrabEnd(lHash, alPeerId, cVector3f(0, 0, 0));
		++lReleased;
	}
	/* holder marks without a grab record (shouldn't happen, but cheap) */
	for (std::map<uint32_t, uint8_t>::iterator it = m_mapHolders.begin();
		 it != m_mapHolders.end();)
	{
		if (it->second == alPeerId)
			m_mapHolders.erase(it++);
		else
			++it;
	}
	return lReleased;
}

//-----------------------------------------------------------------------

void cBodySync::UpdateRemoteGrabs(float afTimeStep)
{
	/* Host snatches whose hold has ended: the grab state's LeaveState has
	   run (it clears the holder first thing, in the same call), so the
	   wrong "defaults" it wrote back can be corrected now. */
	for (std::map<uint32_t, cPendingRestore>::iterator pit = m_mapPendingRestore.begin();
		 pit != m_mapPendingRestore.end();)
	{
		const uint32_t lHash = pit->first;
		++pit; /* ApplyPendingRestore erases the entry */
		if (GetHolder(lHash) == 1)
			continue; /* host still holding */
		ApplyPendingRestore(lHash, GetBodyByHash(lHash));
	}

	if (m_mapRemoteGrabs.empty())
		return;

	iPhysicsWorld *pPhysics = mpWorld ? mpWorld->GetPhysicsWorld() : NULL;
	if (pPhysics == NULL)
	{
		m_mapRemoteGrabs.clear();
		return;
	}

	for (std::map<uint32_t, cRemoteGrab>::iterator it = m_mapRemoteGrabs.begin();
		 it != m_mapRemoteGrabs.end();)
	{
		const uint32_t lHash = it->first;
		cRemoteGrab &grab = it->second;
		iPhysicsBody *pBody = GetBodyByHash(lHash);

		/* Watchdog: body gone, or the target stream died (guest crashed,
		   packets lost past the reliable End) -> clean release. */
		grab.mfNoTargetTime += afTimeStep;
		if (pBody == NULL || grab.mfNoTargetTime > 2.0f)
		{
			++it; /* step off before RemoteGrabEnd erases */
			RemoteGrabEnd(lHash, 0, cVector3f(0, 0, 0));
			continue;
		}
		++it;

		if (grab.mbHasTarget == false)
			continue;

		/* Same math as cPlayerState_Grab::OnUpdate, minus the local player's
		   yaw-drift rotation (the guest streams an absolute target). */
		cVector3f vCurrent;
		if (grab.mbPickAtPoint)
			vCurrent = cMath::MatrixMul(pBody->GetLocalMatrix(), grab.mvRelPick);
		else
			vCurrent = cMath::MatrixMul(pBody->GetWorldMatrix(), pBody->GetMassCentre())
				+ grab.mvRelPick;

		cVector3f vForce(0, 0, 0);
		if (pBody->GetGravity())
			vForce = pPhysics->GetGravity() * -1.0f;
		vForce += grab.mGrabPid.Output(grab.mvTarget - vCurrent, afTimeStep);

		const float fForceSize = vForce.Length();
		if (fForceSize > mfMaxPidForce)
			vForce = (vForce / fForceSize) * mfMaxPidForce;

		if (grab.mbPickAtPoint)
		{
			cVector3f vLocalPos = vCurrent - pBody->GetLocalPosition();
			cVector3f vMassCentre = pBody->GetMassCentre();
			vMassCentre = cMath::MatrixMul(pBody->GetLocalMatrix().GetRotation(), vMassCentre);
			vLocalPos -= vMassCentre;

			pBody->AddForce(vForce * pBody->GetMass() * grab.mfMassMul);

			cVector3f vTorque = cMath::Vector3Cross(vLocalPos, vForce);
			vTorque = cMath::MatrixMul(pBody->GetInertiaMatrix(), vTorque);
			pBody->AddTorque(vTorque);
		}
		else
		{
			pBody->AddForce(vForce * pBody->GetMass() * grab.mfMassMul);

			/* The grab state's rotate PID with no player input: bleed spin
			   off so a floating box doesn't windmill. */
			cVector3f vOmega = pBody->GetAngularVelocity();
			pBody->AddTorque(vOmega * -0.9f * pBody->GetMass());
		}
	}
}

//-----------------------------------------------------------------------

void cBodySync::LogStatsTick(float afTimeStep)
{
#if GHOST_LOADER_TRACE
	/* Object-sync throughput, one line per second while anything flows. */
	mfStatAccum += afTimeStep;
	if (mfStatAccum >= 1.0f)
	{
		if (mlStatStreamed > 0 || mlStatApplied > 0)
			Log(" multiplayer: objects streamed=%d/s (%d B/s) applied=%d/s\n",
				mlStatStreamed, mlStatBytesOut, mlStatApplied);
		mfStatAccum = 0;
		mlStatStreamed = 0;
		mlStatApplied = 0;
		mlStatBytesOut = 0;
	}
#else
	(void)afTimeStep;
#endif
}
