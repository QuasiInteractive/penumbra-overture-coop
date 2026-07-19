/*
 * Phase 1 — binary UDP payloads carried by ENet (same-endian peers; Windows x86 coop).
 *
 * Reliable vs unreliable: join/leave are tiny control messages worth sending reliably;
 * player pose is resent every tick (~20/sec) so unreliable is fine — a dropped packet is
 * replaced by the next one (no need for TCP-style ordering here).
 *
 * Discovery (types 5/6) does NOT travel over ENet: it is raw UDP broadcast on
 * kNetDiscoveryPort so browsers can find hosts without knowing any address.
 */
#ifndef NETWORK_PACKETS_H
#define NETWORK_PACKETS_H

#include <cstdint>

/** 'PNMP' — filters random UDP noise on the discovery port. Same-endian peers,
    so this is compared as a plain uint32 (bytes 50 4E 4D 50 = "PNMP"). */
static const uint32_t kNetProtocolMagic = 0x504E4D50u;

/** Bump when cNetPlayerState (or any wire struct) changes layout. Browsers list
    mismatched servers greyed out instead of letting a join fail silently.
    v2: cNetPlayerState grew mMoveState (stance byte for ghost pose/clips).
    v3: Phase 5 rung 2 — cNetObjectState slimmed (velocities dropped: guests
        snap the pose and ZERO motion, so sending them was pure bandwidth)
        + cNetBodyCensus. Both machines must rebuild.
    v4: rung 4 — map-generation byte in cNetObjectStateBatch + cNetBodyCensus
        (packets from the previous map can no longer touch the new one). */
static const uint16_t kNetProtocolVersion = 6;

/** Well-known discovery port = default game port (7777) + 1.
 *
 * Why not SO_REUSEADDR on the game port itself: with two UDP sockets bound to
 * the same port on Windows (ENet's + ours), *broadcast* datagrams go to both,
 * but *unicast* delivery is bind-order dependent — the discovery socket could
 * steal ENet game traffic. A fixed side port is deterministic, and it also
 * means a browser finds hosts running on ANY custom game port: the ping always
 * goes to 7778 and the pong carries the real game port (mlGamePort).
 */
static const uint16_t kNetDiscoveryPort = 7778;

enum eNetPacketType : uint8_t
{
	eNetPacketType_PlayerState = 1,
	eNetPacketType_PlayerJoin = 2,
	eNetPacketType_PlayerLeave = 3,
	eNetPacketType_ChatMessage = 4, /* reserved for later; not used in Phase 1 */
	eNetPacketType_DiscoveryPing = 5, /* browser -> broadcast */
	eNetPacketType_DiscoveryPong = 6, /* host -> browser, direct reply */
	eNetPacketType_ObjectState = 7, /* Phase 5: batched physics body states.
	                                   Unreliable @ send tick for deltas; the
	                                   same packet type travels RELIABLY once
	                                   as the on-connect full snapshot. */
	eNetPacketType_BodyCensus = 8,  /* Phase 5: host -> guest map-load census
	                                   (reliable). */
	/* Phase 5 rung 3 — FORWARDED INTENT: a guest's grab/push does not run its
	   local physics; the intent goes to the host, the host applies the same
	   style of force in the ONE authoritative sim, and the motion comes back
	   via ObjectState. Begin/End/Deny reliable ch0; Target/Push unreliable ch1
	   at the send tick. */
	eNetPacketType_BodyGrabBegin = 9,   /* guest -> host */
	eNetPacketType_BodyGrabTarget = 10, /* guest -> host, streamed while held */
	eNetPacketType_BodyGrabEnd = 11,    /* guest -> host, impulse = throw */
	eNetPacketType_BodyPush = 12,       /* guest -> host, move/push impulses */
	eNetPacketType_BodyGrabDeny = 13,   /* host -> guest: lost the body (snatch
	                                       or refusal) — release cleanly */
	/* v5 shared-world events (reliable ch0, either direction; host relays a
	   guest's event to the other guests). */
	eNetPacketType_MapChange = 14,  /* one player took a level transition — the
	                                   whole party follows to the same start */
	eNetPacketType_ItemPickup = 15, /* one-of-each items: the OTHER machines
	                                   deactivate their copy of the entity */
	eNetPacketType_ItemDrop = 16,   /* loot sharing: an inventory item dragged
	                                   out re-enters the world on EVERY machine
	                                   (reactivate the deactivated twin, or
	                                   spawn it fresh) */
};

/** Wire encoding of cNetPlayerState::mMoveState. The values are FROZEN for v2
    compatibility: they equal the engine's ePlayerMoveState order (GameTypes.h),
    which earlier v2 builds cast onto the wire raw — renumbering would need a
    kNetProtocolVersion bump. BuildLocalSnapshot maps engine->wire explicitly so
    an engine enum reorder can no longer silently change the protocol. A
    receiver treats any value it does not know as Run. */
enum eNetMoveState : uint8_t
{
	eNetMoveState_Walk = 0,
	eNetMoveState_Run = 1,
	eNetMoveState_Still = 2,
	eNetMoveState_Jump = 3,  /* ghost plays its play-once jump clip */
	eNetMoveState_Crouch = 4,
};

#pragma pack(push, 1)
struct cNetPlayerState
{
	uint8_t mType; /**< eNetPacketType_PlayerState */
	uint8_t mPlayerID;
	float mfPosX, mfPosY, mfPosZ;
	float mfPitch, mfYaw; /**< radians, FPS view (roll omitted on wire) */
	uint8_t mbFlashlightOn;
	uint8_t mMoveState; /**< eNetMoveState value (Walk/Run/Still/Jump/Crouch) —
	                         drives the ghost's stance offset and clip choice */
};

/** Server tells a joining peer their wire id (= eNetPacketType_PlayerJoin). */
struct cNetPlayerJoin
{
	uint8_t mType;
	uint8_t mPlayerID;
};

struct cNetPlayerLeave
{
	uint8_t mType;
	uint8_t mPlayerID;
};

/** A level transition happened; everyone follows. Fixed-size NUL-padded
    strings: map file name + PlayerStart name, exactly what ChangeMap takes. */
struct cNetMapChange
{
	uint8_t mType; /**< eNetPacketType_MapChange */
	char msMap[64];
	char msPos[64];
};

/** Somebody pocketed an item. Identity = FNV-1a of "mapname:entityname"
    (map name lowercased, extension stripped) so the same entity name on two
    different maps can never cross-delete. */
struct cNetItemPickup
{
	uint8_t mType; /**< eNetPacketType_ItemPickup */
	uint32_t mlQualHash;
};

/** An item left somebody's inventory back into the world. Entity name + file
    let a machine that never saw the original spawn an identical twin; the
    name-hash then keeps it in body sync and pickup-able by anyone. */
struct cNetItemDrop
{
	uint8_t mType; /**< eNetPacketType_ItemDrop */
	char msName[48];
	char msFile[64];
	float mfPosX, mfPosY, mfPosZ;       /**< dropper's camera position */
	float mfImpX, mfImpY, mfImpZ;       /**< forward toss impulse */
};

/** Browser -> 255.255.255.255:kNetDiscoveryPort (+ per-subnet directed broadcasts). */
struct cNetDiscoveryPing
{
	uint8_t mType;            /* = DiscoveryPing */
	uint32_t mlProtocolMagic; /* 'PNMP' — filters random UDP noise */
	uint16_t mlProtocolVer;   /* bump when cNetPlayerState changes */
};

/** Host -> ping sender (unicast). Everything the browser row needs. */
struct cNetDiscoveryPong
{
	uint8_t mType;            /* = DiscoveryPong */
	uint32_t mlProtocolMagic;
	uint16_t mlProtocolVer;
	uint16_t mlGamePort;      /* ENet port to connect to */
	uint8_t mlPlayerCount;
	uint8_t mlMaxPlayers;
	char msServerName[32];    /* null-terminated, truncated */
	char msMapName[32];
};
#pragma pack(pop)

/** FNV-1a 32-bit over a map entity/body NAME — object identity on the wire.
    HPL1 body names are unique per map and both sides load the same map, so
    equal hash = same object (collisions are detected and logged at map index
    time, the second body is simply not synced). */
static inline uint32_t NetHashName(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s)
	{
		h ^= (uint8_t)*s++;
		h *= 16777619u;
	}
	return h;
}

/** cNetObjectState::mFlags */
static const uint8_t kNetObjectFlag_Sleeping = 1; /* body at rest — apply pose, zero motion, disable */

/** cNetBodyGrabBegin::mFlags */
static const uint8_t kNetGrabFlag_PickAtPoint = 1; /* doors/drawers: force+torque at the grip point */

/** cNetBodyPush::mFlags */
static const uint8_t kNetPushFlag_Stop = 1; /* guest's move-state brake: zero the body's motion */

#pragma pack(push, 1)
/** One physics body state. Never travels alone: cNetObjectStateBatch::mCount
    of these follow the batch header in one packet. Pose only, NO velocities:
    the guest snaps the transform and zeroes motion (v3) — its own Newton only
    settles the body visually between packets, the next state always wins. */
struct cNetObjectState
{
	uint32_t mlNameHash; /**< NetHashName(body name) */
	float mfPosX, mfPosY, mfPosZ;
	float mfRotX, mfRotY, mfRotZ, mfRotW; /**< orientation quaternion (x,y,z,w) */
	uint8_t mFlags;
};

/** Header of an eNetPacketType_ObjectState packet. mMapGen is the HOST's
    map-load counter (announced in the census): a guest drops batches whose
    generation is not the one it verified against — packets in flight during
    a level change can never move the new map's objects. */
struct cNetObjectStateBatch
{
	uint8_t mType;   /**< eNetPacketType_ObjectState */
	uint8_t mCount;  /**< cNetObjectState entries that follow */
	uint8_t mMapGen; /**< host map generation (wraps; equality only) */
};

/** Host -> guest once per map load (reliable): what the host's physics world
    contains. The guest takes the same census locally after ITS map load and
    compares — a mismatch is the loud canary that the two worlds diverged
    (different map, or different entity creation), i.e. name-hash identities
    would not line up. Checksum = one FNV-1a run over every replicable body
    name in creation order, each name's terminating NUL included (so the fold
    also detects renames that only move a boundary, "ab"+"c" vs "a"+"bc"). */
struct cNetBodyCensus
{
	uint8_t mType;        /**< eNetPacketType_BodyCensus */
	uint8_t mMapGen;      /**< host map generation this census describes */
	uint16_t mlBodyCount; /**< replicable bodies (dynamic, named, non-character) */
	uint32_t mlChecksum;  /**< FNV-1a over the names, creation order */
};

/** Guest latched a replicated body with its grab. mfMassMul is the entity's
    GrabMassMul (the guest reads it at pick time; the host clamps it) so the
    remote grab moves an object exactly as hard as a local one would. */
struct cNetBodyGrabBegin
{
	uint8_t mType;
	uint8_t mFlags; /**< kNetGrabFlag_* */
	uint32_t mlNameHash;
	float mfRelX, mfRelY, mfRelZ; /**< pick point: body-local (pick-at-point)
	                                   or offset from world mass centre */
	float mfMassMul;
};

/** Where the guest's grab wants the body — same target the local spring
    would chase (camera position + crosshair ray * grab distance). */
struct cNetBodyGrabTarget
{
	uint8_t mType;
	uint32_t mlNameHash;
	float mfX, mfY, mfZ;
};

/** Release. A non-zero impulse is a throw (host clamps it). */
struct cNetBodyGrabEnd
{
	uint8_t mType;
	uint32_t mlNameHash;
	float mfImpX, mfImpY, mfImpZ;
};

/** Move/push-state intent: force integrated over the send tick (an impulse)
    at a world point. Stateless on the host — two players shoving one crate
    just sum, which is exactly what physics would do. */
struct cNetBodyPush
{
	uint8_t mType;
	uint8_t mFlags; /**< kNetPushFlag_* */
	uint32_t mlNameHash;
	float mfImpX, mfImpY, mfImpZ;
	float mfPtX, mfPtY, mfPtZ;
};

/** Host -> guest: you no longer hold this body (someone snatched it, or the
    begin was refused). The guest's grab state releases cleanly. */
struct cNetBodyGrabDeny
{
	uint8_t mType;
	uint32_t mlNameHash;
};
#pragma pack(pop)

static_assert(sizeof(cNetPlayerJoin) == 2, "");
static_assert(sizeof(cNetPlayerLeave) == 2, "");
static_assert(sizeof(cNetPlayerState) == 24, "");
static_assert(sizeof(cNetDiscoveryPing) == 7, "");
static_assert(sizeof(cNetDiscoveryPong) == 75, "");
static_assert(sizeof(cNetObjectState) == 33, "");
static_assert(sizeof(cNetObjectStateBatch) == 3, "");
static_assert(sizeof(cNetBodyCensus) == 8, "");
static_assert(sizeof(cNetBodyGrabBegin) == 22, "");
static_assert(sizeof(cNetBodyGrabTarget) == 17, "");
static_assert(sizeof(cNetBodyGrabEnd) == 17, "");
static_assert(sizeof(cNetBodyPush) == 30, "");
static_assert(sizeof(cNetBodyGrabDeny) == 5, "");

#endif /* NETWORK_PACKETS_H */
