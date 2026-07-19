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
/** ENet session: listen-server relay, non-authoritative pose @ ~20 Hz. */
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
	bool BuildLocalSnapshot(cNetPlayerState *apOut) const;
	void EmitLocalSnapshots();
	void DispatchIncoming(const void *data, size_t len);
	void DropRemotePlayer(uint8_t id);
	void EnsureGhost(uint8_t id);

	void RegisterInputActions();
	void TryLoadMultiplayerCfg();

#ifdef PENUMBRA_MULTIPLAYER
	void Service(int alTimeoutMs);
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
