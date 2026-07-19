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
	  , mlBatchSeqIn(0)
	  , mbBatchSeqInKnown(false)
	  , mlMapGen(0)
	  , mlRemoteMapGen(0)
	  , mbRemoteMapGenKnown(false)
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
		m_mapGuestTargets.clear();
		mlGuestHeldHash = 0;
		mbGuestHeldHasTarget = false;
		mlWorldBodyCount = -1;
		mlRoundRobinCursor = 0;
		mbCensusDone = false;
		mbCensusPairChecked = false;
		mbCensusMatched = false;
		mbBatchSeqInKnown = false;
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
		if (IsReplicable(pBody) == false)
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

		/* Prime the send record so the delta path doesn't immediately resend
		   what the snapshot just carried (and no phantom sleep edge either). */
		cSendRecord &rec = m_mapSent[lHash];
		rec.mvPos = vPos;
		rec.mqRot = qRot;
		rec.mbEverSent = true;
		rec.mbWasEnabled = bEnabled;

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

void cBodySync::ApplyStateBatch(const void *apData, size_t alLen)
{
	if (!apData || alLen < sizeof(cNetObjectStateBatch))
		return;

	const cNetObjectStateBatch *pHdr = (const cNetObjectStateBatch *)apData;
	/* Stale-map guard (v4): only apply batches from the generation the
	   census verified. Packets in flight across a level change die here. */
	if (mbRemoteMapGenKnown && pHdr->mMapGen != mlRemoteMapGen)
		return;

	const unsigned char *pRaw = (const unsigned char *)apData;
	size_t lCount = pHdr->mCount;
	const size_t lWhole = (alLen - sizeof(cNetObjectStateBatch)) / sizeof(cNetObjectState);
	if (lCount > lWhole)
		lCount = lWhole; /* truncated/corrupt length: apply only whole entries */

	/* v7 reorder guard. Only PURE-MOVING batches (unreliable channel) are
	   droppable — the next tick replaces them anyway. Anything carrying a
	   rest pose, and the reliable snapshot chunks, is always applied. */
	{
		bool bPureMoving = true;
		for (size_t i = 0; i < lCount && bPureMoving; ++i)
		{
			cNetObjectState st;
			memcpy(&st, pRaw + sizeof(cNetObjectStateBatch) + i * sizeof(cNetObjectState), sizeof(st));
			if (st.mFlags & kNetObjectFlag_Sleeping)
				bPureMoving = false;
		}
		if (bPureMoving && mbBatchSeqInKnown &&
			(int16_t)(pHdr->mSeq - mlBatchSeqIn) <= 0)
			return; /* stale reordered batch: applying it pops bodies backward */
		if (!mbBatchSeqInKnown || (int16_t)(pHdr->mSeq - mlBatchSeqIn) > 0)
		{
			mlBatchSeqIn = pHdr->mSeq;
			mbBatchSeqInKnown = true;
		}
	}

	RebuildIndexIfNeeded();
	if (m_mapBodies.empty())
		return;

	for (size_t i = 0; i < lCount; ++i)
	{
		cNetObjectState st;
		memcpy(&st, pRaw + sizeof(cNetObjectStateBatch) + i * sizeof(cNetObjectState), sizeof(st));

		std::map<uint32_t, iPhysicsBody *>::iterator bi = m_mapBodies.find(st.mlNameHash);
		if (bi == m_mapBodies.end())
			continue; /* unknown hash: map mismatch or a destroyed body */
		iPhysicsBody *pBody = bi->second;

		/* Wire floats are untrusted: a degenerate quaternion would scale the
		   body's rotation matrix. Normalize; drop the entry if it's garbage. */
		const float fQLen = sqrtf(st.mfRotX * st.mfRotX + st.mfRotY * st.mfRotY +
			st.mfRotZ * st.mfRotZ + st.mfRotW * st.mfRotW);
		if (!(fQLen > 0.5f && fQLen < 2.0f))
			continue;
		cQuaternion qRot;
		qRot.v.x = st.mfRotX / fQLen;
		qRot.v.y = st.mfRotY / fQLen;
		qRot.v.z = st.mfRotZ / fQLen;
		qRot.w = st.mfRotW / fQLen;
		const cVector3f vPos(st.mfPosX, st.mfPosY, st.mfPosZ);

		if (st.mFlags & kNetObjectFlag_Sleeping)
		{
			/* Rest pose (reliable, sent once): pin it exactly, right now. */
			cMatrixf mtx = cMath::MatrixQuaternion(qRot);
			mtx.SetTranslation(vPos);
			pBody->SetMatrix(mtx);
			pBody->SetLinearVelocity(cVector3f(0, 0, 0));
			pBody->SetAngularVelocity(cVector3f(0, 0, 0));
			pBody->SetEnabled(false);
			m_mapGuestTargets.erase(st.mlNameHash);
		}
		else
		{
			/* Moving: becomes a blend target — UpdateGuestBlend eases the
			   body onto it each frame, so 20 Hz packets over an internet
			   link render smooth instead of steppy. Our own predicted held
			   body ignores the (round-trip lagged) host echo entirely. */
			if (st.mlNameHash == mlGuestHeldHash && !mbGuestHeldPick)
				continue;
			cGuestTarget &tgt = m_mapGuestTargets[st.mlNameHash];
			tgt.mvPos = vPos;
			tgt.mqRot = qRot;
			tgt.mfAge = 0;
			if (pBody->GetEnabled() == false)
				pBody->SetEnabled(true); /* remote says it moves — wake the twin */
		}

		++mlStatApplied;
		/* NOTE: no per-entry Log here on purpose — at 20 Hz x N bodies the old
		   trace wrote hundreds of lines/second into hpl.log (OneDrive-synced!)
		   and could hitch the frame. The 1 Hz stats line is the health signal. */
	}
}

//-----------------------------------------------------------------------

/** Normalized lerp between two quaternions (shortest arc) — plenty for the
    small per-frame steps the blender takes. */
static cQuaternion QuatNlerp(const cQuaternion &a, const cQuaternion &b, float t)
{
	float fDot = a.v.x * b.v.x + a.v.y * b.v.y + a.v.z * b.v.z + a.w * b.w;
	const float fSign = fDot < 0 ? -1.0f : 1.0f;
	cQuaternion q;
	q.v.x = a.v.x + (b.v.x * fSign - a.v.x) * t;
	q.v.y = a.v.y + (b.v.y * fSign - a.v.y) * t;
	q.v.z = a.v.z + (b.v.z * fSign - a.v.z) * t;
	q.w = a.w + (b.w * fSign - a.w) * t;
	const float fLen = sqrtf(q.v.x * q.v.x + q.v.y * q.v.y + q.v.z * q.v.z + q.w * q.w);
	if (fLen > 0.0001f)
	{
		q.v.x /= fLen; q.v.y /= fLen; q.v.z /= fLen; q.w /= fLen;
	}
	return q;
}

void cBodySync::UpdateGuestBlend(float afTimeStep)
{
	/* ---- held-object prediction: our copy tracks OUR crosshair target ---- */
	if (mlGuestHeldHash != 0 && !mbGuestHeldPick && mbGuestHeldHasTarget)
	{
		iPhysicsBody *pBody = GetBodyByHash(mlGuestHeldHash);
		if (pBody)
		{
			/* same "current pick point" math as the grab spring */
			const cMatrixf mtx = pBody->GetWorldMatrix();
			cVector3f vCurrent = cMath::MatrixMul(mtx, pBody->GetMassCentre())
				+ mvGuestHeldRelPick;
			const cVector3f vDelta = mvGuestHeldTarget - vCurrent;
			const float k = 1.0f - expf(-afTimeStep * 14.0f); /* snappy but not teleporty */
			cMatrixf mtxNew = mtx;
			mtxNew.SetTranslation(mtx.GetTranslation() + vDelta * k);
			pBody->SetMatrix(mtxNew);
			pBody->SetLinearVelocity(cVector3f(0, 0, 0));
			pBody->SetAngularVelocity(pBody->GetAngularVelocity() * 0.9f);
			pBody->SetEnabled(true);
		}
	}

	/* ---- everything else eases onto the latest received state ---- */
	if (m_mapGuestTargets.empty())
		return;
	const float k = 1.0f - expf(-afTimeStep * 15.0f);
	for (std::map<uint32_t, cGuestTarget>::iterator it = m_mapGuestTargets.begin();
		 it != m_mapGuestTargets.end();)
	{
		cGuestTarget &tgt = it->second;
		tgt.mfAge += afTimeStep;
		iPhysicsBody *pBody = GetBodyByHash(it->first);
		/* Stale (host quiet > 1 s: lost sleep edge or lost body) — release the
		   body to plain local physics; it settles where it stands. */
		if (pBody == NULL || tgt.mfAge > 1.0f)
		{
			m_mapGuestTargets.erase(it++);
			continue;
		}
		++it;

		const cMatrixf mtx = pBody->GetWorldMatrix();
		const cVector3f vCur = mtx.GetTranslation();
		const cVector3f vErr = tgt.mvPos - vCur;
		const float fErrSq = vErr.x * vErr.x + vErr.y * vErr.y + vErr.z * vErr.z;

		cQuaternion qCur;
		qCur.FromRotationMatrix(mtx.GetRotation());

		cMatrixf mtxNew;
		if (fErrSq > 1.5f * 1.5f)
		{
			/* teleport-sized error: snap, no easing across the room */
			mtxNew = cMath::MatrixQuaternion(tgt.mqRot);
			mtxNew.SetTranslation(tgt.mvPos);
		}
		else
		{
			mtxNew = cMath::MatrixQuaternion(QuatNlerp(qCur, tgt.mqRot, k));
			mtxNew.SetTranslation(vCur + vErr * k);
		}
		pBody->SetMatrix(mtxNew);
		pBody->SetLinearVelocity(cVector3f(0, 0, 0));
		pBody->SetAngularVelocity(cVector3f(0, 0, 0));
	}
}

void cBodySync::SetGuestHeld(uint32_t alHash, bool abPickAtPoint, const cVector3f &avRelPick)
{
	mlGuestHeldHash = alHash;
	mbGuestHeldPick = abPickAtPoint;
	mvGuestHeldRelPick = avRelPick;
	mbGuestHeldHasTarget = false;
	if (!abPickAtPoint)
		m_mapGuestTargets.erase(alHash); /* prediction owns it while held */
}

void cBodySync::UpdateGuestHeldTarget(const cVector3f &avTarget)
{
	mvGuestHeldTarget = avTarget;
	mbGuestHeldHasTarget = true;
}

void cBodySync::ClearGuestHeld()
{
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
	apBody->SetAutoDisable(true);
	apBody->SetEnabled(true);
}

bool cBodySync::RemoteGrabEnd(uint32_t alHash, uint8_t alPeerId, const cVector3f &avImpulse)
{
	std::map<uint32_t, cRemoteGrab>::iterator it = m_mapRemoteGrabs.find(alHash);
	if (it == m_mapRemoteGrabs.end())
		return false;
	if (alPeerId != 0 && it->second.mlPeerId != alPeerId)
		return false; /* stale end from a peer that already lost the body */

	iPhysicsBody *pBody = GetBodyByHash(alHash);
	if (pBody)
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
