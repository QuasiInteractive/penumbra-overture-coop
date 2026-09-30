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
 * final states, reliable full snapshot for late joiners, and a slow
 * unreliable keyframe trickle of sleeping bodies).
 * Guest intent forwarding (grabs/pushes acting on the host sim) is RUNG 3.
 *
 * Guest apply (rewritten after the first internet test: "barrel break
 * dancing", spinning, phasing through walls, floating objects). The guest
 * NEVER teleports a moving body every frame any more — that fought its own
 * Newton (gravity + contact penetration recovery -> jitter/spin), dragged
 * bodies through walls (a SetMatrix is not swept) and let Newton auto-freeze
 * bodies whose velocity was zeroed every frame (-> frozen mid-air). Instead:
 *  - moving states are velocity TARGETS: each frame the body gets the
 *    linear/angular velocity that closes the error (P term at the centre of
 *    mass + a host-motion feed-forward, clamped, gravity pre-compensated),
 *    so Newton's own collision keeps it out of walls. Hard snap (motion
 *    zeroed) only on a teleport-sized error or when stuck > 0.5 s.
 *  - rest poses pin: exact pose, zero motion, frozen. A pinned body the
 *    guest's local physics wakes (character bump) is eased back after a
 *    short grace — the host never saw that bump.
 *  - a body the host goes silent on (> 1 s) is holding still there: it is
 *    pinned at the last host pose rather than released to local physics.
 *  - per-body sequence guard: an older state (unsequenced reorder, a late
 *    reliable rest pose) never overwrites a newer one for that body.
 *  - jointed bodies (doors, levers, chains) get gentler velocity clamps so
 *    the drive never out-muscles (or breaks) the joint.
 *  - ragdoll bodies are not streamed (the enemy code owns them).
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
	/** Sleeping bodies re-announced per tick (unreliable, Sleeping flag) in
	    the moving batch's spare slots: repairs any guest copy that drifted
	    from the host's rest pose (a guest-only bump, a body the host never
	    woke). 2/tick at 30 Hz sweeps 300 bodies in 5 s for ~2 KB/s. */
	enum { kKeyframesPerTick = 2 };
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
	    RELIABLY so the joiner starts from the host's exact current poses.
	    Never touches the shared delta bookkeeping (m_mapSent): the snapshot
	    goes to one peer, the delta stream (and its sleep edges) to all. */
	size_t BuildSnapshotChunk(unsigned char *apBuf, uint32_t *apCursor);

	/** Guest: ingest one received eNetPacketType_ObjectState payload.
	    Per-body sequence guard first (older than what we have = dropped).
	    Rest poses pin (immediately when the body is not being driven);
	    moving states become velocity-drive TARGETS for UpdateGuestBlend. */
	void ApplyStateBatch(const void *apData, size_t alLen);

	/** Guest, every frame: drive replicated bodies toward their latest
	    received states with velocities (collision-aware), pin rest poses,
	    ease locally disturbed rest bodies back. Hard snaps only on a
	    teleport-sized or stuck error. */
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

	/** v14 world snapshot: generation bookkeeping for the MapReady handshake
	    and the snapshot/enemy-batch stale-map guards. */
	uint8_t GetMapGen() const { return mlMapGen; }
	bool HasRemoteCensus() const { return mbRemoteCensusKnown; }
	uint8_t GetRemoteMapGen() const { return mlRemoteMapGen; }
	/** Guest: is this the host generation we paired our census with? */
	bool IsRemoteGen(uint8_t alGen) const { return mbRemoteMapGenKnown && alGen == mlRemoteMapGen; }

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

	/** Census member = something physics can toss around (see the .cpp note
	    on why mass>0 alone almost — but not quite — covers it). */
	static bool IsReplicable(hpl::iPhysicsBody *apBody);
	/** Indexed + streamed = replicable minus ragdoll bones (census unchanged). */
	static bool IsStreamable(hpl::iPhysicsBody *apBody);

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
	uint32_t mlKeyframeCursor;   /**< host: last sleeping body re-announced (keyframe trickle) */

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
	uint8_t mlMapGen;
	uint8_t mlRemoteMapGen; /**< guest: host generation we verified against */
	bool mbRemoteMapGenKnown;

	/** Guest: what the host last said about one body, and how our local
	    copy is being brought onto it. */
	struct cGuestBody
	{
		hpl::cVector3f mvPos;   /* latest host pose (body origin, world) */
		hpl::cQuaternion mqRot;
		hpl::cVector3f mvVel;   /* host motion estimate from consecutive */
		hpl::cVector3f mvOmega; /* moving states (feed-forward), world   */
		bool mbHaveMotion;
		uint16_t mlSeq;         /* batch seq of the state above */
		float mfRecvClock;      /* mfGuestClock when it was applied */
		float mfAge;            /* seconds since the last state */
		float mfStuckTime;      /* large error that the drive cannot close */
		float mfSettleTime;     /* rest pose not pinned yet */
		float mfDisturbTime;    /* pinned but woken by local physics */
		bool mbRest;            /* host: asleep (or silent = holding still) */
		bool mbPinned;          /* rest pose applied exactly + frozen */
		cGuestBody()
			: mvPos(0, 0, 0), mvVel(0, 0, 0), mvOmega(0, 0, 0), mbHaveMotion(false)
			  , mlSeq(0), mfRecvClock(0), mfAge(0), mfStuckTime(0), mfSettleTime(0)
			  , mfDisturbTime(0), mbRest(false), mbPinned(false) {}
	};
	std::map<uint32_t, cGuestBody> m_mapGuestBodies;
	float mfGuestClock; /**< guest: seconds of UpdateGuestBlend, for the seq window */

	/** Guest: exact pose, zero motion, frozen. */
	void PinGuestBody(hpl::iPhysicsBody *apBody, cGuestBody &aRec);

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
		bool mbHadAutoDisable;
		hpl::cPidControllerVec3 mGrabPid;
		cRemoteGrab()
			: mlPeerId(0), mbPickAtPoint(false), mvTarget(0, 0, 0)
			  , mbHasTarget(false), mfNoTargetTime(0), mfMassMul(1.0f)
			  , mfDefaultMass(0), mbHadGravity(true), mbHadAutoDisable(true) {}
	};
	std::map<uint32_t, cRemoteGrab> m_mapRemoteGrabs;
	std::map<uint32_t, uint8_t> m_mapHolders; /**< hash -> player id, both local + remote */
	float mfMaxPidForce;
	float mfMaxThrowImpulse;

	/** Host: the HOST player snatched a body out of a guest's free grab.
	    cPlayerState_Grab::EnterState had already captured the remote grab's
	    gravity-off + mass/5 as the body's "defaults" (NetGrabBegin runs at
	    its end), so its LeaveState would leave the body floating at a fifth
	    of its mass forever. The true defaults wait here and are re-applied
	    once the host lets go (or a guest grabs it first). */
	struct cPendingRestore
	{
		float mfMass;
		bool mbGravity;
		bool mbAutoDisable;
	};
	std::map<uint32_t, cPendingRestore> m_mapPendingRestore;
	void ApplyPendingRestore(uint32_t alHash, hpl::iPhysicsBody *apBody);

	/** Restore mass/gravity/autodisable after a remote grab. */
	void RestoreGrabbedBody(hpl::iPhysicsBody *apBody, const cRemoteGrab &aGrab);
};

#endif /* BODY_SYNC_H */
