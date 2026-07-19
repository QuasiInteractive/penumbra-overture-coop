/*
 * Phase 5 shared physics — host-authoritative moveable-object replication.
 *
 * cBodySync owns everything object-sync: the per-world body index, the wire
 * send records, the map-load census, and the batch build/apply paths.
 * cNetworkManager keeps only the transport — when to build a batch and which
 * ENet peers receive it.
 *
 * Identity on the wire = FNV-1a of the body NAME (NetHashName). HPL1 body
 * names are unique per map and both machines load the same map, so equal
 * hash = same object — independent of body creation ORDER, which an index
 * id would depend on. The census (count + one FNV run over all names in
 * creation order) is the LOUD canary that both worlds really do contain the
 * same bodies; if it ever fires, identities cannot be trusted.
 *
 * RUNG 1: identity + census.
 * RUNG 2: host streams awake dynamic bodies (pose-only deltas, sleep-edge
 * final states, reliable full snapshot for late joiners); the guest snaps
 * pose + zeroes motion — its Newton only settles bodies between packets.
 * Guest intent forwarding (grabs/pushes acting on the host sim) is RUNG 3.
 */
#ifndef BODY_SYNC_H
#define BODY_SYNC_H

#include "StdAfx.h"
#include "NetworkPackets.h"
#include "math/PidController.h"

#include <map>

class cBodySync
{
public:
	/** Per-tick cap on states in one batch — keeps the packet well under MTU
	    (2 + 12*33 = 398 bytes) and spreads a mass upset (shelf collapse) over
	    a few ticks; the round-robin cursor keeps that fair. */
	enum { kMaxStatesPerBatch = 12 };
	enum { kMaxBatchBytes = sizeof(cNetObjectStateBatch) +
		kMaxStatesPerBatch * sizeof(cNetObjectState) };

	cBodySync();

	/** Frame tick. Adopts the scene's CURRENT world (a change drops all
	    per-world state — cached iPhysicsBody pointers die with their world)
	    and takes the map-load census once the new world's physics exists.
	    Returns true only on the frame the local census was computed: that is
	    the host's cue to broadcast it (a guest self-verifies internally). */
	bool Update(hpl::cWorld3D *apWorld);

	bool HasCensus() const { return mbCensusDone; }
	void BuildCensusPacket(cNetBodyCensus *apOut) const;

	/** Guest: the host's census arrived — store it and verify against ours
	    (now, or as soon as our own map load produces a local census). */
	void OnCensusReceived(const cNetBodyCensus &aCensus);

	/** Host: serialize changed-body states. Moving bodies land in apMoving
	    (unreliable — the next tick replaces a lost one); awake->asleep REST
	    poses land in apSleep (send RELIABLY: it's the one state that must
	    arrive and it is never resent). Both buffers >= kMaxBatchBytes. */
	void BuildStateBatches(unsigned char *apMoving, size_t *apMovingLen,
		unsigned char *apSleep, size_t *apSleepLen);

	/** Host, late-join snapshot: serialize the next up-to-kMaxStatesPerBatch
	    bodies AFTER *apCursor regardless of movement, advancing the cursor.
	    Start with *apCursor = 0 and loop until 0 is returned; send each chunk
	    RELIABLY so the joiner starts from the host's exact current poses. */
	size_t BuildSnapshotChunk(unsigned char *apBuf, uint32_t *apCursor);

	/** Guest: ingest one received eNetPacketType_ObjectState payload.
	    Rest poses pin immediately; moving states become blend TARGETS —
	    UpdateGuestBlend eases the local bodies onto them every frame, so
	    20 Hz packets at internet latency render smooth instead of steppy. */
	void ApplyStateBatch(const void *apData, size_t alLen);

	/** Guest, every frame: ease replicated bodies toward their latest
	    received states (snaps only on teleport-sized error). */
	void UpdateGuestBlend(float afTimeStep);

	/** Guest, rung 3 at high ping — held-object PREDICTION: while WE hold a
	    forwarded free-grab, the local copy tracks our own crosshair target
	    directly and ignores the (round-trip lagged) host states for that one
	    body; the host stays authoritative and everything reconciles on
	    release. Doors/drawers (pick-at-point) are never predicted — their
	    joints live in the host's sim. */
	void SetGuestHeld(uint32_t alHash, bool abPickAtPoint, const hpl::cVector3f &avRelPick);
	void UpdateGuestHeldTarget(const hpl::cVector3f &avTarget);
	void ClearGuestHeld();

	/** True once host+guest censuses compared equal for the current map. */
	bool CensusMatched() const { return mbCensusMatched; }

	/** 1 Hz streamed/applied/bytes trace while anything flows. */
	void LogStatsTick(float afTimeStep);

	//-------------------------------------------------------------------
	// Rung 3 — forwarded intent. Host side: guests' grabs acting on OUR sim.
	//-------------------------------------------------------------------

	/** Grab feel constants, read from game.cfg Interaction_Grab once at
	    Startup (MaxPidForce caps the spring, MaxThrowImpulse caps throws). */
	void SetGrabTuning(float afMaxPidForce, float afMaxThrowImpulse);

	/** Wire identity for a body this sync knows; false = not replicated. */
	bool GetHashForBody(hpl::iPhysicsBody *apBody, uint32_t *apOut);
	hpl::iPhysicsBody *GetBodyByHash(uint32_t alHash);

	/** Who holds a body: 0 = nobody, 1 = the host player, 2+ = a guest.
	    SetHolder returns the PREVIOUS holder so the caller can evict it
	    (LAST GRAB WINS — snatching is a feature). */
	uint8_t SetHolder(uint32_t alHash, uint8_t alPlayerId);
	uint8_t GetHolder(uint32_t alHash) const;
	void ClearHolder(uint32_t alHash);

	/** Host: a guest's grab lifecycle. Begin attaches the same style of PID
	    spring the local grab state uses; Target feeds it; End restores the
	    body and applies the (clamped) throw impulse. */
	void RemoteGrabBegin(uint32_t alHash, uint8_t alPeerId, bool abPickAtPoint,
		const hpl::cVector3f &avRelPick, float afMassMul);
	void RemoteGrabTarget(uint32_t alHash, uint8_t alPeerId, const hpl::cVector3f &avTarget);
	/** alPeerId 0 = force (any holder). Returns true if a grab was live. */
	bool RemoteGrabEnd(uint32_t alHash, uint8_t alPeerId, const hpl::cVector3f &avImpulse);

	/** Host: stateless move/push intent — impulse at a world point, with an
	    optional motion stop (the move state's brake). Sanity-clamped. */
	void RemotePush(uint32_t alHash, const hpl::cVector3f &avImpulse,
		const hpl::cVector3f &avPoint, bool abStop);

	/** Peer vanished: drop everything it held. Returns grabs released. */
	int ReleaseAllHeldBy(uint8_t alPeerId);

	/** Drive the remote grab springs; host calls this every frame. */
	void UpdateRemoteGrabs(float afTimeStep);

private:
	void RebuildIndexIfNeeded();
	void ComputeCensus(hpl::iPhysicsWorld *apPhysics);
	void VerifyCensus();

	/** Streamable = something physics can toss around (see the .cpp note on
	    why mass>0 alone almost — but not quite — covers it). */
	static bool IsReplicable(hpl::iPhysicsBody *apBody);

	struct cSendRecord
	{
		hpl::cVector3f mvPos; /* last state put on the wire */
		hpl::cQuaternion mqRot;
		bool mbEverSent;
		bool mbWasEnabled; /* awake->asleep edge sends one final Sleeping state */
		cSendRecord() : mvPos(0, 0, 0), mbEverSent(false), mbWasEnabled(false) {}
	};

	hpl::cWorld3D *mpWorld;
	std::map<uint32_t, hpl::iPhysicsBody *> m_mapBodies; /**< name-hash -> body, CURRENT world only */
	std::map<uint32_t, cSendRecord> m_mapSent;           /**< sender bookkeeping per hash */
	int mlWorldBodyCount;        /**< total body count when m_mapBodies was built; -1 = dirty */
	uint32_t mlRoundRobinCursor; /**< last hash written — the per-tick cap starves nobody */

	/** Map-load census: taken ONCE per world, on the first frame its physics
	    world exists (map load is synchronous, so every load-time entity is
	    already there). Bodies scripts spawn later change the INDEX via the
	    count probe but not the census — both sides ran the same load, that is
	    what the census certifies. */
	bool mbCensusDone;
	uint16_t mlCensusCount;
	uint32_t mlCensusChecksum;
	/** Received census survives a world change on purpose: the host announces
	    a map's census while the guest is still on the load screen. */
	bool mbRemoteCensusKnown;
	cNetBodyCensus mRemoteCensus;
	bool mbCensusPairChecked;
	bool mbCensusMatched; /**< last verify: identities are trustworthy */ /* don't re-log until either side changes */

	float mfStatAccum;
	int mlStatStreamed;
	int mlStatApplied;
	int mlStatBytesOut; /**< payload bytes built per second (host side) */

	/** Map generation: bumped on every world change; the HOST's value rides
	    the census + state batches so a guest can drop stale-map packets. */
	uint16_t mlBatchSeqOut;   /**< host: stamped on every state batch */
	uint16_t mlBatchSeqIn;    /**< guest: newest batch seq applied */
	bool mbBatchSeqInKnown;
	uint8_t mlMapGen;
	uint8_t mlRemoteMapGen; /**< guest: host generation we verified against */
	bool mbRemoteMapGenKnown;

	/** Guest blend target: the latest received pose for one awake body. */
	struct cGuestTarget
	{
		hpl::cVector3f mvPos;
		hpl::cQuaternion mqRot;
		float mfAge; /* seconds since last packet; stale targets get dropped */
		cGuestTarget() : mvPos(0, 0, 0), mfAge(0) {}
	};
	std::map<uint32_t, cGuestTarget> m_mapGuestTargets;

	/** Guest held-object prediction state (0 = not holding). */
	uint32_t mlGuestHeldHash;
	bool mbGuestHeldPick;
	hpl::cVector3f mvGuestHeldRelPick;
	hpl::cVector3f mvGuestHeldTarget;
	bool mbGuestHeldHasTarget;

	/** One guest grab acting on the host sim (rung 3). */
	struct cRemoteGrab
	{
		uint8_t mlPeerId;
		bool mbPickAtPoint;
		hpl::cVector3f mvRelPick;
		hpl::cVector3f mvTarget;
		bool mbHasTarget;
		float mfNoTargetTime; /* watchdog: stale grabs self-release */
		float mfMassMul;
		float mfDefaultMass;  /* restored on end (grab halves control mass) */
		bool mbHadGravity;
		hpl::cPidControllerVec3 mGrabPid;
		cRemoteGrab()
			: mlPeerId(0), mbPickAtPoint(false), mvTarget(0, 0, 0)
			  , mbHasTarget(false), mfNoTargetTime(0), mfMassMul(1.0f)
			  , mfDefaultMass(0), mbHadGravity(true) {}
	};
	std::map<uint32_t, cRemoteGrab> m_mapRemoteGrabs;
	std::map<uint32_t, uint8_t> m_mapHolders; /**< hash -> player id, both local + remote */
	float mfMaxPidForce;
	float mfMaxThrowImpulse;

	/** Restore mass/gravity/autodisable after a remote grab. */
	void RestoreGrabbedBody(hpl::iPhysicsBody *apBody, const cRemoteGrab &aGrab);
};

#endif /* BODY_SYNC_H */
