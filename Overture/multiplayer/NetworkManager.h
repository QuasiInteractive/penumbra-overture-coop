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
class cVoiceChat;
namespace hpl { class iPhysicsBody; }

//-----------------------------------------------------------------------
/** One discovered host (LAN pong or master-server entry), ready for the
    browser UI (RUNG 2). */
struct cDiscoveredServer
{
	hpl::tString msAddress; /**< "ip:port" — feed straight to JoinGame(). */
	hpl::tString msName;
	hpl::tString msMap;
	uint8_t mlPlayerCount;
	uint8_t mlMaxPlayers;
	/** false = server speaks another kNetProtocolVersion; list greyed out, do not join. */
	bool mbVersionMatch;
	/** Host requires a join password (master listing flag; LAN pongs: false).
	    The browser asks for one before JoinGame — see SetJoinPassword. */
	bool mbPassword;
	/** Internet rows: seconds since the host's last master beacon (0..65535);
	    LAN rows: 0. */
	uint16_t mlAgeSeconds;
	/** true = came from the master server, false = LAN pong. */
	bool mbInternet;

	cDiscoveredServer()
		: mlPlayerCount(0), mlMaxPlayers(0), mbVersionMatch(false),
		  mbPassword(false), mlAgeSeconds(0), mbInternet(false)
	{
	}
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
	/** Host lobby UI: the EFFECTIVE player cap (v17) = min(multiplayer.cfg
	    max_players (2..31, default 4), number of characters) — every player
	    is a different character, so there are never more players than
	    characters. The same value discovery pongs / the master listing
	    advertise and CONNECT enforces. */
	uint8_t GetMaxPlayers() const;
	/** v17: characters in the list (mvGhostMeshPaths), at least 1, at most
	    kNetMaxCharacterSlots — the number of character slots a host hands out. */
	uint8_t GetCharacterCount() const;
	/** v17: the character of any id, ours included: the base name of its
	    model without ".dae" ("fisherman"), "" while its host-assigned slot
	    is not known (offline, or before the host's table arrived). v18: show
	    it through GetCharacterDisplayName ("The Fisherman"). */
	hpl::tString GetPlayerCharacterName(uint8_t alId) const;

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

	/** INTERNET server browser (master server, raw UDP — NetworkPackets.h
	    'MASTER SERVER protocol'). RefreshInternetServers() sends a List to
	    the configured master (cfg `master_server=host:port`, resolved once
	    per refresh — blocking, called from a button, never per frame) and
	    collects Entries for kNetMasterListWindowSeconds; results persist
	    until the next refresh. Shares the LAN browse socket. */
	void RefreshInternetServers();
	void StopInternetRefresh();
	bool IsInternetRefreshActive() const { return mbInternetActive; }
	const std::vector<cDiscoveredServer> &GetInternetServers() const { return mvInternet; }
	/** Why the last refresh produced nothing ("" = fine / not tried yet):
	    no master configured, unresolvable host, socket failure. */
	const hpl::tString &GetInternetFailReason() const { return msInternetFailReason; }

	/** Host lobby 'Public (list on master)' checkbox: register with the
	    master while hosting. cfg `public=1`; `master_server=` alone implies
	    public unless `public=0` is given. Toggling while hosting sends a
	    Register / Unregister right away. */
	bool IsPublic() const { return mbPublic; }
	void SetPublic(bool abPublic);
	/** true = multiplayer.cfg had an explicit `public=` — the menu must not
	    override it with its remembered toggle. */
	bool IsPublicFromCfg() const { return mbPublicExplicit; }
	/** "host:port" the host registers with / the browser lists from ("" =
	    none configured; the placeholder default is only used with public=1). */
	hpl::tString GetMasterServer() const;
	/* The browser's password prompt feeds SetJoinPassword (v15 auth, below);
	   a host's HasServerPassword() sets the [pw] flag in its master beacon. */

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
	bool mbPendingMapChangeDeferred; /**< guest: the queued follow arrived while
	    our own transition was armed — kept, re-evaluated on our census frame
	    (same map = discard, else follow: every MapChange a guest receives is
	    the host's destination, see IsHostMapBusy) */
	bool mbBeaconAfterTransition;   /**< host: a guest's MapChange was refused
	    while we were mid-transition — beacon our map once we are settled */
	float mfLocalMapChangeArmedAge; /**< seconds armed with no fade running and
	    no census (failed load / same-map ChangeMap) — failsafe unarm at 5 s */
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
	/** Host: armed, following, fading, or the current world's census not
	    taken yet (incl. no world). A guest MapChange arriving then is refused
	    (not applied, not relayed) and the beacon goes out once settled, so
	    guests only ever receive the host's own destination. */
	bool IsHostMapBusy() const;
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
	/** v17: cfg host=1 — Startup hosts AFTER ResolveGhostModels, so the
	    character count (= the player cap) is final when the host opens. */
	bool mbCfgAutoHost;

	/** Character meshes. v17: a PlayerID uses entry (its host-assigned slot)
	    mod N (m_mapPlayerSlots; (id-1) mod N until the slot is known). Built in
	    Startup by ResolveGhostModels: `ghost_models=a,b,c` / `ghost_model=a`
	    in cfg order minus files that do not resolve, else auto-discovered
	    characters in multiplayer/models (sorted), else malik + phillip. */
	std::vector<hpl::tString> mvGhostMeshPaths;
	/** Startup, after multiplayer/models is a resource dir: finalise
	    mvGhostMeshPaths (see above) and keep a per-mesh grounding list that
	    matched the cfg list entry for entry aligned with it. */
	void ResolveGhostModels();
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

	/** Master server (internet browser) state — see RefreshInternetServers. */
	hpl::tString msMasterServer;     /**< cfg `master_server=host:port` ("" = none) */
	bool mbPublic;                   /**< register with the master while hosting */
	bool mbPublicExplicit;           /**< cfg had `public=` */
	std::vector<cDiscoveredServer> mvInternet;
	bool mbInternetActive;
	float mfInternetTimeLeft;
	hpl::tString msInternetFailReason;
	float mfMasterRegisterAccum;
	float mfMasterResolveAge;       /**< host: seconds since the master was last resolved */     /**< host: seconds since the last Register */
	bool mbMasterRegistered;         /**< host: at least one Register went out
	    this session -> send an Unregister on stop */

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
	/** Browser-side ephemeral UDP socket shared by the LAN scan and the
	    master List: opened on demand, closed once neither window is open. */
	bool OpenBrowseSocket();
	void CloseBrowseSocketIfIdle();
	/** Master server: resolve cfg host:port once (blocking; cached until the
	    string changes), Register/Unregister from the host discovery socket,
	    parse an Entries datagram into mvInternet. */
	bool ResolveMasterAddress();
	void SendMasterRegister();
	void SendMasterUnregister();
	void HandleMasterEntries(const char *apBuf, int alLen);
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
	/** v17: id -> character slot (index into mvGhostMeshPaths, mod its size),
	    ours included. HOST: the slot table itself — id 1 = slot 0 (HostGame),
	    each accepted guest the lowest free slot (HostAcceptPeer), erased in
	    DropRemotePlayer = freed. GUEST: copied from the host's name table. */
	std::map<uint8_t, uint8_t> m_mapPlayerSlots;
	uint32_t mlSlotWarned; /**< guest: bit per slot >= our character count already logged; bit 31 also = v19 'host character not ours' logged */
	/** Mesh list index for a ghost of alId: its slot, else (id-1). */
	size_t GhostMeshIndexFor(uint8_t alId) const;
	/** Host: lowest slot in 1..GetCharacterCount()-1 no entry uses,
	    kNetCharacterUnknown when every slot is taken. */
	uint8_t AllocCharacterSlot() const;
	/** Guest: a table entry's slot arrived (kNetCharacterUnknown = forget).
	    v19: asCharacter is the host's base name for it; when our list has
	    that name the entry stores OUR index for it (lists may differ in
	    order), otherwise the host's slot number (modulo on use).
	    An existing ghost whose mesh index changes is deleted — the next
	    state packet re-creates it with the right character (EnsureGhost). */
	void OnPlayerSlotReceived(uint8_t alId, uint8_t alSlot, const hpl::tString &asCharacter);
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
	//---------------- v14: world snapshot — appended, impl at the file tail ----------------
	/* Late join / reconnect / host save+load: the moment a guest has paired
	   its physics census with the host's for the world it stands in, it
	   sends MapReady; the host answers with one chunked, reliable
	   WorldSnapshot (script vars, entity actives/locks/lit/health, taken +
	   party items, resting body poses, enemy roster, local timers). The
	   guest buffers the chunks and applies them ATOMICALLY on End under
	   gbNetScriptApplying, so its world scripts never see a half state. */
private:
	std::vector<std::vector<uint8_t> > mvSnapChunks; /**< guest: buffered section chunks (header included) */
	uint8_t mlSnapId;        /**< guest: id of the snapshot being buffered */
	uint8_t mlSnapGen;       /**< guest: host generation it was sent for */
	bool mbSnapBuffering;    /**< guest: a Begin arrived, End not yet */
	float mfSnapAge;         /**< guest: seconds since Begin; > 10 s = dropped */
	uint8_t mlSnapIdOut;     /**< host: per-snapshot counter */
	/** Enemy stream chunking (5c): 5 + 40*29 = 1165 B per packet. */
	enum { kMaxEnemiesPerBatch = 40 };
#ifdef PENUMBRA_MULTIPLAYER
	/** Guest: our census is paired with the host's — ask for the world state. */
	void SendMapReady();
	/** Host: a guest's MapReady (needs the peer to answer it). */
	void HandleMapReady(struct _ENetPeer *apPeer, const void *apData, size_t alLen);
	/** Host -> one guest: Begin, every section, body poses, End. */
	void SendWorldSnapshot(struct _ENetPeer *apPeer);
	/** Host -> one guest: every replicable body's pose, reliable ObjectState
	    chunks (primes the delta path). Returns chunks sent, adds bytes. */
	int SendBodySnapshot(struct _ENetPeer *apPeer, size_t *apBytesOut);
	/** Guest: one WorldSnapshot chunk off the wire (buffer / apply on End). */
	void HandleSnapshotChunk(const void *apData, size_t alLen);
	/** Guest: apply the buffered snapshot to the current world, atomically. */
	void ApplyWorldSnapshot();
	/** Guest: forget any half-buffered snapshot. */
	void ResetSnapshotBuffer();
#endif

	//---------------- v15: internet hardening — appended, impl at the file tail ----------------
	/* Join authentication (challenge/response, see NetworkPackets.h v15),
	   packet role gating, per-guest payload validation before apply/relay,
	   strikes + kick, reliable-packet rate limit, max_players at CONNECT,
	   guest id reuse, discovery reflector limits. README "Security". */
public:
	/** The password a GUEST sends when joining (cfg join_password=, or the
	    join screen). Sanitised: printable ASCII, kNetPasswordMaxChars max.
	    Takes effect on the next JoinGame. */
	void SetJoinPassword(const hpl::tString &asPassword);
	const hpl::tString &GetJoinPassword() const { return msJoinPassword; }
	/** The password a HOST demands (cfg server_password=). Empty = open
	    server. Takes effect for peers connecting after the call. */
	void SetServerPassword(const hpl::tString &asPassword);
	bool HasServerPassword() const { return !msServerPassword.empty(); }
	/** Printable ASCII only, kNetPasswordMaxChars max, blanks kept inside. */
	static hpl::tString SanitizePassword(const hpl::tString &asPassword);
	/** THE role table: may a packet of this type be accepted from a peer of
	    that role? abFromHost = the packet was authored by the host (we are a
	    guest). Unknown types are never allowed. One call per packet, in
	    Service, before any dispatch or relay. */
	static bool IsAllowedFrom(uint8_t alType, bool abFromHost);
	/** Wire strings are untrusted: true when [apStr, apStr+alCap) holds only
	    printable ASCII up to its first NUL (a full field without NUL counts
	    as terminated at alCap). abAllowEmpty = "" passes. */
	static bool NetStringOk(const char *apStr, size_t alCap, bool abAllowEmpty);
	/** A bare file name for the world/entity loaders: no path separators,
	    no drive, no "..", printable, non-empty, ending in ".<asExt>"
	    (case-insensitive). */
	static bool NetBareFileNameOk(const char *apStr, size_t alCap, const char *asExt);
private:
	hpl::tString msServerPassword; /**< host: cfg server_password ("" = open) */
	hpl::tString msJoinPassword;   /**< guest: cfg join_password / menu */
	bool mbAuthSent;               /**< guest: answered the host's challenge (this connection) */
	uint64_t mlGuestViolationsLogged; /**< guest: bit per packet type already logged (log-once) */
	std::vector<uint8_t> mvFreeGuestIds; /**< host: ids of departed guests, reused first */
	/** Host: next free wire id (free list first), 0 = none left. */
	uint8_t AllocGuestId();
	void FreeGuestId(uint8_t alId);
#ifdef PENUMBRA_MULTIPLAYER
	/** Host: a connected peer that passed authentication (has a wire id). */
	static bool PeerLive(const struct _ENetPeer *apPeer);
	/** Host: connected peers, all of them or only the accepted ones. */
	int CountConnectedPeers(bool abAcceptedOnly) const;
	/** Host: a fresh per-connection nonce (time, address, counter, mixed). */
	void MakeNonce(const struct _ENetPeer *apPeer, uint8_t aOut[16]);
	/** Host: an accepted guest (wire id alId) is gone — PlayerLeave to the
	    rest, ghost/name/voice/slot dropped, name table re-sent, held bodies
	    released, id freed. From DISCONNECT and the dead-slot sweep. */
	void HostForgetGuest(struct _ENetPeer *apPeer, uint8_t alId);
	/** Host: the guest's cNetAuth arrived — accept (id, ack, join, census,
	    beacon, name table) or refuse with kNetDisconnectBadAuth. */
	void HostAcceptPeer(struct _ENetPeer *apPeer, const cNetAuth &aAuth);
	/** Guest: the host's challenge arrived — answer it once. */
	void SendAuthResponse(const cNetChallenge &aChallenge);
	/** Host: one protocol violation by a peer — logged once per (peer,
	    type), counted; kNetMaxStrikes -> kNetDisconnectKicked. */
	void NoteViolation(struct _ENetPeer *apPeer, uint8_t alType, const char *asWhy);
	/** Both roles: bounds/shape checks common to every event, clamping in
	    place (impulses, damage). false = drop. abFromHost picks the role
	    of the AUTHOR. Host-only extras (file searcher, distance to the
	    guest) live in ValidateGuestPacket. */
	bool ValidateEventPacket(unsigned char *apData, size_t alLen, bool abFromHost);
	/** Host: everything a guest-authored packet must satisfy before the host
	    applies or relays it (clamps in place). false = drop + strike. */
	bool ValidateGuestPacket(struct _ENetPeer *apPeer, unsigned char *apData, size_t alLen);
	/** Host: auth timeouts, rate windows, strike decay, dead-slot sweep. */
	void UpdatePeerGuards(float afTimeStep);
	/** Host: discovery reflector limiter — may we pong this source now? */
	bool DiscoveryPongAllowed(uint32_t alAddr);
	/** Host: forget every per-peer record (HostGame / Disconnect). */
	void ResetPeerGuards();
#endif

	//---------------- v16: proximity voice chat — appended, impl at the file tail ----------------
	/* cVoiceChat (multiplayer/VoiceChat.h) owns Opus + the OpenAL capture
	   device and streaming sources; this class owns the object, feeds it
	   the push-to-talk key ("VoiceTalk", V), the ghosts' head positions
	   and the incoming type-29 packets, sends its outbox unsequenced on
	   ch1 and, as host, relays a guest's voice to the other guests with the
	   author id stamped (same trust rule as PlayerState). The object is
	   created in Startup() when voice_enabled=1 and the build has
	   PENUMBRA_VOICE; Opus/AL are initialised only while a session is live
	   (UpdateVoice) and torn down in Disconnect — single-player never
	   touches the microphone or the AL context. */
public:
	/** HUD: that player's voice is being heard right now (our own id: the
	    microphone is open). Always false offline / without voice. */
	bool IsPlayerTalking(uint8_t alId) const;
	/** HUD: our microphone is live (PTT held / open-mic gate open). */
	bool IsMicOpen() const;
	/** Voice compiled in and voice_enabled=1 (the panel can show hints). */
	bool IsVoiceAvailable() const;
private:
	cVoiceChat *mpVoice;   /**< NULL: voice off (cfg) or not compiled in */
	bool mbVoiceEnabled;   /**< multiplayer.cfg voice_enabled (default 1) */
	bool mbVoiceOpenMic;   /**< multiplayer.cfg voice_open_mic (default 0) */
	float mfVoiceVolume;   /**< multiplayer.cfg voice_volume (default 1.0) */
#ifdef PENUMBRA_MULTIPLAYER
	/** Per frame after UpdateGhosts: (de)initialise with the session,
	    positions, PTT, capture/encode/playback, send the outbox. */
	void UpdateVoice(float afTimeStep);
	/** Unreliable twin of SendReliableEvent (ch1, unsequenced). */
	void SendUnreliableEvent(const void *apData, size_t alLen);
	/** Host: a guest's voice packet — stamp the author, relay to the other
	    guests, play it here. */
	void RelayVoice(struct _ENetPeer *apFrom, uint8_t alAuthor, const void *apData, size_t alLen);
#endif

	//---------------- v18: character picker — appended, impl at the file tail ----------------
	/* The host is always slot 0 (Philip with the shipped list) and never
	   picks. A guest keeps a preference (multiplayer.cfg character=, the
	   Multiplayer / Direct-connect screens' 'Character: < X >' line) and
	   asks the host for it BY NAME (cNetCharacterRequest) after its name
	   and on every change while connected; the host moves it if that
	   character exists there, is not slot 0 and is free, then re-sends the
	   name table (ghosts are rebuilt everywhere from the new slot). */
public:
	/** "phillip" -> "Philip", "fisherman" -> "The Fisherman", "red" ->
	    "Red", "malik" -> "Malik"; any other base name with its first letter
	    capitalised; "" stays "". Case-insensitive lookup. */
	static hpl::tString GetCharacterDisplayName(const hpl::tString &asBase);
	/** Printable ASCII, no blanks at either end, at most
	    kNetCharacterNameMaxChars (cfg, menu and wire all go through it). */
	static hpl::tString SanitizeCharacterName(const hpl::tString &asName);
	/** Our character list (final order: slot i = entry i). */
	size_t GetCharacterListSize() const { return mvGhostMeshPaths.size(); }
	/** Base name of list entry alIdx ("fisherman"), "" when out of range. */
	hpl::tString GetCharacterBaseName(size_t alIdx) const;
	/** List index of a base name (ASCII-case-insensitive), -1 = not ours. */
	int FindCharacterIndex(const hpl::tString &asBase) const;
	/** multiplayer.cfg character= (sanitised, "" = no preference). */
	const hpl::tString &GetCharacterPreference() const { return msCharacterPref; }
	/** Stores the preference, writes character= to multiplayer.cfg (every
	    other line kept) and, as a connected guest, asks the host at once. */
	void SetCharacterPreference(const hpl::tString &asBase);
	/** Somebody other than us holds that character in the current session
	    (host's table / the name table we got). Always false offline. */
	bool IsCharacterTakenByOther(const hpl::tString &asBase) const;
	/** Picker: may we ask for list entry alIdx? Not slot 0 (the host's),
	    in range, and not taken by somebody else. */
	bool IsCharacterSelectable(size_t alIdx) const;
	/** Picker: the next selectable base name after asCurrent (wrapping;
	    "" or unknown = from the start). Returns asCurrent's own entry when
	    it is the only selectable one, "" when none is. */
	hpl::tString GetNextSelectableCharacter(const hpl::tString &asCurrent) const;
	/** Our own character right now ("" offline / not assigned yet). */
	hpl::tString GetLocalCharacterName() const;
private:
	hpl::tString msCharacterPref; /**< cfg character= ("" = no preference) */
	/** An existing ghost of alId built with mesh index alOldIdx is deleted
	    when its slot now points at a different mesh; the next state packet
	    re-creates it (EnsureGhost). Both roles. */
	void RebuildGhostIfMeshChanged(uint8_t alId, size_t alOldIdx);
#ifdef PENUMBRA_MULTIPLAYER
	/** Guest: msCharacterPref to the host (no-op when empty / not joined). */
	void SendCharacterRequest();
	/** Host: a validated cNetCharacterRequest from an accepted peer —
	    applied now, or held as the peer's pending request while its
	    cooldown runs (UpdatePeerGuards applies the newest one). */
	void HostHandleCharacterRequest(struct _ENetPeer *apPeer, uint8_t alAuthor,
		const void *apData, size_t alLen);
	/** Host: move alAuthor to asBase's slot. NULL = done (or already
	    there); otherwise why it was ignored (the caller logs once). */
	const char *HostApplyCharacterRequest(uint8_t alAuthor, const hpl::tString &asBase);
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
