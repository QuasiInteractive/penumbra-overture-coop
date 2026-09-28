#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

#include "StdAfx.h"
#include "NetworkPackets.h"
#include "GhostPlayer.h"

#include <map>
#include <set>
#include <vector>

class cInit;
class cBodySync;
namespace hpl { class iPhysicsBody; }

//-----------------------------------------------------------------------
/** One LAN-discovered host, ready for the browser UI (RUNG 2). */
struct cDiscoveredServer
{
	hpl::tString msAddress; /**< "ip:port" — feed straight to JoinGame(). */
	hpl::tString msName;
	hpl::tString msMap;
	uint8_t mlPlayerCount;
	uint8_t mlMaxPlayers;
	/** false = server speaks another kNetProtocolVersion; list greyed out, do not join. */
	bool mbVersionMatch;
};

//-----------------------------------------------------------------------
/** v12 party health: one connected remote player as the HUD / inventory
    party line / co-op respawn see it. Health is the newest mirrored value
    (0 = dead or Dead flag); the positions are only valid when the flags
    say so (no state yet, or the ghost is between worlds on a map change). */
struct cNetPartyMember
{
	uint8_t mlId;
	float mfHealth;               /**< 0-100, 0 = dead */
	bool mbHasFeetPos;            /**< newest wire feet position known */
	hpl::cVector3f mvFeetPos;
	bool mbHasRenderPos;          /**< ghost mesh drawn this frame */
	hpl::cVector3f mvRenderFeetPos;

	cNetPartyMember()
		: mlId(0), mfHealth(0.0f), mbHasFeetPos(false), mvFeetPos(0, 0, 0),
		  mbHasRenderPos(false), mvRenderFeetPos(0, 0, 0)
	{
	}
};

//-----------------------------------------------------------------------
/** ENet session: listen-server relay. Player pose, host-authoritative bodies
    and enemies stream at 30 Hz (kNetSendPeriodSeconds); remote players are
    drawn as interpolated cGhostPlayer bodies. */
class cNetworkManager
{
public:
	explicit cNetworkManager(cInit *apInit);
	~cNetworkManager();

	/** Register F9/F10/F11 actions and optionally read multiplayer.cfg — call after cGame + input exist. */
	void Startup();

	void HostGame(uint16_t alPort = 7777);
	void JoinGame(const char *aszHostPort);
	void Disconnect();

	void Update(float afTimeStep);

	bool IsHosting() const { return mbHosting; }
	bool IsClientSynced() const { return mbClientConnected && mbHadJoinPacket; }

	uint8_t GetLocalPlayerID() const { return mlLocalPlayerId; }
	uint16_t GetDefaultPort() const { return mlDefaultPort; }
	void SetDefaultPort(uint16_t p) { mlDefaultPort = p; }

	/** Phase 5 rung 3 — forwarded intent, called from the player interaction
	    states (PlayerState_Interact.cpp). A TRUE return from a *Begin means
	    REMOTE-DRIVEN: we are a connected guest and the body is replicated, so
	    the caller must NOT apply local forces — the intent is forwarded and
	    the motion comes back via body sync. Always false on the host and
	    offline, so single-player behaves exactly as before.

	    Grab = exclusive (holder tracked, LAST GRAB WINS — snatching works
	    both directions); Move/Push = stateless summed impulses. */
	bool NetGrabBegin(hpl::iPhysicsBody *apBody, bool abPickAtPoint,
		const hpl::cVector3f &avRelPick, float afMassMul);
	void NetGrabTarget(hpl::iPhysicsBody *apBody, const hpl::cVector3f &avTarget);
	void NetGrabEnd(hpl::iPhysicsBody *apBody, const hpl::cVector3f &avImpulse);
	bool NetMoveBegin(hpl::iPhysicsBody *apBody);
	void NetMoveForce(hpl::iPhysicsBody *apBody, const hpl::cVector3f &avForce,
		const hpl::cVector3f &avPoint, float afTimeStep);
	void NetMoveStop(hpl::iPhysicsBody *apBody);
	void NetMoveEnd(hpl::iPhysicsBody *apBody);

	/** This machine's IPv4 addresses, one display line each, tagged by kind —
	    "26.104.x.x (Radmin)", "192.168.1.5 (LAN)" — so the HOST can read the
	    address to give a friend straight off the lobby screen. Best-effort;
	    empty when nothing is up. */
	void GetLocalAddressLines(std::vector<hpl::tString> &avOut) const;

	/** OS clipboard as plain ASCII ("" if empty/non-text) — for Ctrl+V in the
	    join-address field. Win32 only; other platforms return "". */
	static hpl::tString GetClipboardTextAscii();

	/** v5 shared world. Called by cMapHandler::ChangeMap: the party follows a
	    level transition (no-op while APPLYING a remote one — no ping-pong). */
	void NetOnLocalMapChange(const hpl::tString &asMap, const hpl::tString &asPos);

	/** v5 shared world. Called when the local player pockets an item — the
	    other machines deactivate their copy (one-of-each items). */
	void NetOnItemPicked(const hpl::tString &asEntityName);

	/** Host lobby UI: guests currently on the socket. */
	int GetConnectedGuestCount() const;

	/** Join screen UI: why the last join attempt died ("" = no failure).
	    Cleared by the next JoinGame call. */
	const hpl::tString &GetJoinFailReason() const { return msJoinFailReason; }

	//---------------- Phase 6: shared enemies ----------------
	/** Guest with a live session: local enemies become host-driven puppets. */
	bool IsEnemyPuppetMode() const;
	/** Host AI: every connected ghost's latest camera position. */
	void GetGhostCamPositions(std::vector<std::pair<uint8_t, hpl::cVector3f> > &avOut);
	/** Host: an enemy's attack landed on that guest's position. */
	void SendPlayerDamage(uint8_t alPlayerId, float afDamage);
	/** Guest: my weapon hit a puppet enemy — the host applies it for real. */
	void NetOnEnemyDamaged(const hpl::tString &asName, float afDamage, int alStrength);

	//---------------- v9: shared script state ----------------
	/** Script HasItem(): true when ANY party member holds the item. */
	bool PartyHasItem(const hpl::tString &asName) const;
	/** A local script mutated shared state (var write, entity active, door
	    lock, item consumed) — replicate it. No-op while APPLYING a remote
	    event, and offline. */
	void NetOnScriptEvent(int alOp, const hpl::tString &asName, int alVal);

	/** v10: a weapon damaged a breakable object — replicate the hit so the
	    object (explosives door!) breaks in every world. */
	void NetOnEntityDamaged(const hpl::tString &asName, float afDamage, int alStrength);

	/** v6 loot sharing. Called by cInventoryItem::Drop after it spawned the
	    entity locally: every other machine materializes the same item. */
	void NetOnItemDropped(const hpl::tString &asName, const hpl::tString &asFile,
		const hpl::cVector3f &avPos, const hpl::cVector3f &avImpulse);

	/** LAN/Hamachi server browser (raw UDP broadcast, not ENet).
	    StartDiscovery() clears old results, pings every interface's subnet and
	    collects pongs for ~1.5s; IsDiscoveryActive() turns false when the
	    window closes. Results persist until the next StartDiscovery(). */
	void StartDiscovery();
	void StopDiscovery();
	bool IsDiscoveryActive() const { return mbDiscoveryActive; }
	const std::vector<cDiscoveredServer> &GetDiscoveredServers() const { return mvDiscovered; }

private:
	typedef std::map<uint8_t, cGhostPlayer *> tGhostMap;

	cInit *mpInit;

	bool mbHosting;
	bool mbClientConnected;
	bool mbHadJoinPacket;
	bool mbSpawnedAtHost; /**< once per connection: walked to the host's side */
	bool mbGotVersionAck; /**< host proved it speaks OUR protocol version */
	uint16_t mlStateSeqOut; /**< our PlayerState counter (v7 reorder guard) */
	uint16_t mlEnemySeqOut;  /**< host: enemy batch counter */
	uint16_t mlEnemySeqIn;   /**< guest: newest enemy batch applied */
	bool mbEnemySeqInKnown;
	void EmitEnemyStates();
	void ApplyEnemyBatch(const void *apData, size_t alLen);
	std::map<uint8_t, uint16_t> m_mapGhostSeq; /**< per-author newest seq seen */
	hpl::tString msJoinFailReason;

	/* v5 shared world */
	bool mbApplyingRemoteMapChange; /**< suppresses NetOnLocalMapChange */
	bool mbLocalMapChangeArmed;     /**< we initiated one; ignore remote
	    follows until our new map is up (else two doors at once SWAP maps) */
	bool mbHavePendingMapChange;    /**< a follow is queued for a safe point */
	hpl::tString msPendingMap, msPendingPos;
	hpl::tString msRemoteCurrentMap; /**< last map the party announced (kept
	    after apply) — lets spawn-at-friend work even when a save-loaded host's
	    census differs from our virgin map */
	std::set<uint32_t> m_setTakenItems; /**< qualified hashes, whole session —
	    revisited maps re-hide items a friend pocketed while we were elsewhere */
	std::set<hpl::tString> m_setPartyItems; /**< v9: inventory names OTHER
	    members hold — script HasItem() consults this so split pickups can
	    still satisfy multi-item gates */
	uint32_t QualifiedItemHash(const hpl::tString &asEntityName) const;
	void ApplyTakenItems();       /**< deactivate current-map matches */
	void ApplyRemoteDrop(const cNetItemDrop &aDrop);
	void ApplyPendingMapChange(); /**< runs the queued ChangeMap */
	void SendMapBeacon(struct _ENetPeer *apOnlyTo); /**< NULL = every guest */
	void SendReliableEvent(const void *apData, size_t alLen); /**< host: all
	    peers; guest: the server (which relays to other guests) */

	float mfSendAccum;
	static const float kSendPeriodSeconds;

	hpl::cWorld3D *mpWorld;
	tGhostMap m_mapGhosts;

	uint8_t mlLocalPlayerId;
	uint16_t mlListenPort;
	uint8_t mlNextGuestId;
	uint16_t mlDefaultPort;

	bool mbActionsRegistered;
	hpl::tString msDeferredJoinAddress;

	/** Mesh paths per remote PlayerID — from `ghost_models=a,b,c` or a single `ghost_model=a`. Index = (id-1) mod N. */
	std::vector<hpl::tString> mvGhostMeshPaths;
	/** Offset along Y from synced camera (eye) to mesh origin; depends on DAE pivot, not model “height in meters”.
	    Retune when switching from placeholder props to ~1.75m-tall character meshes. */
	float mfGhostMeshBodyYOffset;
	/** Same, while the remote player crouches (their camera drops ~0.7 m but their
	    feet don't). 9999 = not set in cfg -> resolved to stand offset + 0.7 at
	    ghost creation. Cfg key: ghost_body_y_crouch. */
	float mfGhostMeshBodyYOffsetCrouch;
	/** PER-MESH stand/crouch overrides (`ghost_body_ys=`, `ghost_body_ys_crouch=`),
	    CSV indexed exactly like ghost_models — different rigs ground at
	    different heights. Empty = use the single-value keys above. A stand
	    list without a crouch list keeps each mesh's stand->crouch delta. */
	std::vector<float> mvGhostBodyYList;
	std::vector<float> mvGhostBodyYCrouchList;

	/** v11 ghost tuning from multiplayer.cfg (ghost_interp_ms, ghost_turn_rate,
	    ghost_anim_trace, ghost_gait_*), copied into every ghost at creation. */
	cGhostTuning mGhostTuning;

	/** ghost_preview=1: a LOCAL ghost (id kPreviewGhostId, never in
	    m_mapGhosts, never sent) stands 2 m ahead of the player and is fed
	    synthetic states through the normal ApplyState/Update path, so clips,
	    crossfades, gait scaling and interpolation can be eyeballed offline.
	    F6/F7 cycle clips, F8 toggles crouch, F2 cycles the treadmill
	    (off / walk / run in a circle). */
	bool mbGhostPreview;
	int mlGhostPreviewModel;      /**< ghost_preview_model: index into ghost_models */
	cGhostPlayer *mpPreviewGhost;
	uint16_t mlPreviewSeq;        /**< synthetic sender counter */
	float mfPreviewSendAccum;     /**< synthetic states go out at kSendPeriodSeconds */
	float mfPreviewSpawnDelay;    /**< seconds after a world change before spawning */
	hpl::cVector3f mvPreviewCenter; /**< feet position 2 m ahead of the player at spawn */
	float mfPreviewFacingYaw;     /**< faces the player (player yaw + pi) */
	float mfPreviewCircleAngle;   /**< treadmill: angle on the 1 m circle */
	int mlPreviewClipIdx;         /**< forced clip index into cGhostPlayer::GetClipName, -1 = automatic */
	bool mbPreviewCrouch;
	int mlPreviewTreadmill;       /**< 0 off, 1 walk, 2 run */

	/** Browser results + window state (see StartDiscovery). */
	std::vector<cDiscoveredServer> mvDiscovered;
	bool mbDiscoveryActive;
	float mfDiscoveryTimeLeft;
	static const float kDiscoveryWindowSeconds;

	/** Advertised in discovery pongs; multiplayer.cfg `server_name=` / `max_players=`. */
	hpl::tString msServerName;
	uint8_t mlMaxPlayers;

	/** Phase 5 object sync lives in cBodySync (multiplayer/BodySync.h) —
	    body identity + census, host state batches, guest apply. This class
	    only decides WHEN to build a packet and WHICH peers receive it. */
	cBodySync *mpBodySync;

	struct Impl;
	Impl *mpImpl;

	void ClearGhostsInternal();
	/** Ghost for a wire id using mesh list entry alMeshIdx (mod list size),
	    offsets/eye heights from game.cfg + cfg overrides, shared tuning.
	    NULL without a world. Caller owns the result. */
	cGhostPlayer *CreateGhost(uint8_t alId, size_t alMeshIdx);
	/** Delete the preview ghost (abOrphan = the world already died). */
	void DestroyPreviewGhost(bool abOrphan);
	bool BuildLocalSnapshot(cNetPlayerState *apOut) const;
	void EmitLocalSnapshots();
	void DispatchIncoming(const void *data, size_t len);
	void DropRemotePlayer(uint8_t id);
	void EnsureGhost(uint8_t id);

	void RegisterInputActions();
	void TryLoadMultiplayerCfg();

#ifdef PENUMBRA_MULTIPLAYER
	void Service(int alTimeoutMs);
	/** Per-frame ghost interpolation/animation (every ghost + the preview),
	    after the last Service(0) so this tick's packets are in the buffers. */
	void UpdateGhosts(float afTimeStep);
	/** Preview spawn / keys / synthetic sender (ghost_preview=1). */
	void UpdatePreviewGhost(float afTimeStep);
	void OpenHostDiscovery();
	void CloseHostDiscovery();
	void SendDiscoveryPings();
	void PollDiscovery(float afTimeStep);
	void EmitObjectStates();
	/** Reliable census to one peer, or every connected peer when NULL. */
	void SendCensus(struct _ENetPeer *apOnlyTo);
	/** Guest: flush pending grab-target / push intent at the send tick. */
	void FlushIntentPackets();
	/** Host: grab-family packet from a guest (needs the author id). */
	void HandleBodyIntent(uint8_t alAuthor, const void *apData, size_t alLen);
	/** Host -> one guest: you lost this body; release cleanly. */
	void SendGrabDeny(uint8_t alPeerId, uint32_t alHash);
	/** Both roles: our player holds this body -> force-release (snatched). */
	void ForceReleaseIfHolding(uint32_t alHash);
#endif

	//---------------- enemy senses (host) — appended, impl at the file tail ----------------
public:
	/** Host AI: one ghost's LATEST wire camera position and eNetMoveState.
	    false = no such connected ghost / no state yet / not hosting — the
	    enemy code reads that as "gone". Either out pointer may be NULL. Used
	    every frame by the focus accessors (live tracking between sight
	    ticks), by the stealth approximation (crouch) and by the host-side
	    ghost footstep triggers (hearing). */
	bool GetGhostSense(uint8_t alId, hpl::cVector3f *apCamPos, uint8_t *apMoveState) const;
private:
	std::map<uint8_t, uint8_t> m_mapGhostMoveState; /**< host: last wire move state per ghost id */

	//---------------- v12: party health — appended, impl at the file tail ----------------
public:
	/** Mirrored health of one remote player (both roles). false = no such
	    connected player / no state from it yet; *apHealth is 0 when it is
	    dead (Dead flag or health 0). Survives our own map change (kept per
	    id, not per ghost entity). */
	bool GetGhostHealth(uint8_t alId, float *apHealth) const;
	/** Every remote player we have heard from, with health + positions
	    (see cNetPartyMember). Empty offline. Never includes the preview
	    ghost or ourselves. */
	void GetPartyStatus(std::vector<cNetPartyMember> &avOut) const;
	/** A real session with somebody on the other end: hosting with >= 1
	    connected guest, or a synced client. The co-op death rule only
	    branches when this is true — single-player is untouched. */
	bool IsSessionLive() const;
	/** multiplayer.cfg coop_respawn (default 1). */
	bool IsCoopRespawnEnabled() const { return mbCoopRespawn; }
private:
	std::map<uint8_t, uint8_t> m_mapGhostHealth; /**< both roles: newest health per remote id, 0 = dead */
	bool mbCoopRespawn;

	//---------------- v13: player names + party event feed — appended, impl at the file tail ----------------
public:
	/** One line of the HUD party feed ("<name> joined" ...). mfAge counts up
	    in seconds; the line is dropped at kPartyEventLifeSeconds. */
	struct cNetPartyEvent
	{
		hpl::tString msText;
		float mfAge;
		cNetPartyEvent() : msText(), mfAge(0.0f) {}
	};
	static const size_t kPartyEventMax = 4;       /**< feed depth (oldest drops first) */
	static const float kPartyEventLifeSeconds;    /**< 6 s, then the line is gone */
	static const float kPartyJoinGraceSeconds;    /**< 2 s after our join: table
	    entries are the EXISTING party (listed silently), later ones "joined" */

	/** multiplayer.cfg player_name (sanitised, "" = not set). */
	const hpl::tString &GetLocalPlayerName() const { return msPlayerName; }
	/** Sanitises + stores the name, rewrites player_name= in multiplayer.cfg
	    (every other line kept) and, in a live session, announces it (host:
	    the table to every guest; guest: to the host). */
	void SetLocalPlayerName(const hpl::tString &asName);
	/** Display name for ANY id, ours included: the known name, else
	    "Player <id>". Never empty. */
	hpl::tString GetPlayerName(uint8_t alId) const;
	/** Feed for cPlayer::DrawPartyPanel, oldest first. Empty offline. */
	const std::vector<cNetPartyEvent> &GetPartyEvents() const { return mvPartyEvents; }
	/** Push one feed line (bounded to kPartyEventMax, oldest dropped). */
	void AddPartyEvent(const hpl::tString &asText);
	/** Printable ASCII (32..126) only, leading/trailing blanks trimmed,
	    at most kNetPlayerNameMaxChars characters. Applied on cfg load, on
	    menu input and on every name that arrives from the wire. */
	static hpl::tString SanitizePlayerName(const hpl::tString &asName);
	/** Sets `asKey=asValue` in multiplayer.cfg: the first matching line is
	    replaced in place (commented "# key=" lines are left alone), a missing
	    key is appended, every other line is copied verbatim. Creates the
	    file when there is none. false = could not write. */
	static bool UpdateMultiplayerCfgKey(const char *asKey, const hpl::tString &asValue);
private:
	hpl::tString msPlayerName;                       /**< ours (cfg player_name) */
	std::map<uint8_t, hpl::tString> m_mapPlayerNames; /**< REMOTE ids -> name (never ours, never empty) */
	std::vector<cNetPartyEvent> mvPartyEvents;
	std::set<uint8_t> m_setJoinAnnounced; /**< ids whose "joined" line went out (so "left" is only shown for them) */
	float mfSinceJoinSeconds;             /**< guest: seconds since our PlayerJoin (kPartyJoinGraceSeconds) */
	/** Ages the feed, drops dead lines. Called from Update (both builds). */
	void UpdatePartyEvents(float afTimeStep);
	/** A name for a remote id arrived (host: from the peer itself; guest:
	    from the host's table). Stores/erases, emits the "joined" line the
	    first time the id is seen (outside the join grace window). */
	void OnPlayerNameReceived(uint8_t alId, const char *apName, size_t alLen);
	/** Health transition -> "died" / "respawned" feed lines. Call BEFORE the
	    m_mapGhostHealth write with the value about to be stored. */
	void NotePartyHealth(uint8_t alId, uint8_t alNewHealth);
	/** Forget a remote id's name; "<name> left" if it had been announced. */
	void ForgetPlayerName(uint8_t alId);
#ifdef PENUMBRA_MULTIPLAYER
	/** Host: every known name, one cNetPlayerName each, to one peer or (NULL) to all. */
	void SendNameTable(struct _ENetPeer *apOnlyTo);
	/** Guest: our name to the host (after PlayerJoin, and on a rename). */
	void SendLocalName();
#endif
};
//-----------------------------------------------------------------------
/** Pumps cNetworkManager from the GLOBAL updater state, so hosting and
    discovery stay alive in every screen (PreMenu, MainMenu, MapLoadText,
    in-game "Default") — container-registered updates only tick in their own
    screen, which left the host deaf everywhere but in-game. */
class cNetworkUpdater : public hpl::iUpdateable
{
public:
	explicit cNetworkUpdater(cNetworkManager *apManager)
		: hpl::iUpdateable("NetworkUpdater")
		  , mpManager(apManager)
	{
	}

	void Update(float afTimeStep) { if (mpManager) mpManager->Update(afTimeStep); }

private:
	cNetworkManager *mpManager;
};
//-----------------------------------------------------------------------

#endif
