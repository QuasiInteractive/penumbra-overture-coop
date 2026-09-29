/*
 * Binary UDP payloads carried by ENet (same-endian peers; Windows x86 coop).
 *
 * Reliable vs unreliable: join/leave are tiny control messages worth sending reliably;
 * player pose is resent every tick (30/sec, kSendPeriodSeconds) so unreliable is fine —
 * a dropped packet is replaced by the next one (no need for TCP-style ordering here).
 *
 * Discovery (types 5/6) does NOT travel over ENet: it is raw UDP broadcast on
 * kNetDiscoveryPort so browsers can find hosts without knowing any address.
 */
#ifndef NETWORK_PACKETS_H
#define NETWORK_PACKETS_H

#include <cstddef> /* v13: size_t for kNetPlayerNameMaxChars */
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
        (packets from the previous map can no longer touch the new one).
    v11: cNetPlayerState carries the sender's body-local planar velocity and
        an on-ground/crouch/run/jump flag byte, and mfPosY is the FEET height
        (character body) instead of the head-bobbing camera Y — the ghost
        animates from exact sender truth instead of inferring it 0.3-0.5 s
        late from positions.
    v12: party health — cNetPlayerState carries the sender's health (0-100)
        and a Dead flag bit, so the host's enemy AI stops biting a corpse,
        friends see each other's health (world bars + inventory party
        line) and the co-op respawn rule knows whether anybody is alive.
    v13: player names — reliable cNetPlayerName (type 26): a guest sends
        its multiplayer.cfg player_name after PlayerJoin, the host keeps
        the table and re-broadcasts it (one packet per player) on every
        name arrival / join / leave. Old builds do not speak it.
    v14: world snapshot — MapReady (24) + WorldSnapshot (25): a guest that
        pairs its census with the host's asks for the host's world state and
        the host answers with script vars, entity actives/locks/lit, taken +
        party items, resting body poses, the enemy roster and local timers,
        applied atomically. Also LampLit script op 9 and the enemy stream's
        map-gen byte, which was reserved (0) until now.
    v15: internet hardening — challenge/response join: the host answers
        CONNECT with cNetChallenge (28, a 16-byte nonce) and the guest's
        FIRST packet must be cNetAuth (27: version + a 128-bit digest of
        server_password + nonce + its name). Nothing else is processed or
        sent to a peer until it is accepted; no/wrong answer within
        kNetAuthTimeoutSeconds -> kNetDisconnectBadAuth. max_players is
        enforced at CONNECT (kNetDisconnectFull). Packet types are role
        gated (IsAllowedFrom) and every guest-authored payload is validated
        before the host applies or relays it. The digest input includes the
        version, so a v14 build can never answer a v15 challenge.
    v16: voice — unsequenced cNetVoice (type 29): Opus frames from a
        push-to-talk microphone, played back by every other machine as a
        3D source at the speaker's ghost head (proximity falloff). Both
        directions (guest -> host, host -> guests; the host relays with the
        author stamped from the peer), validated like every other
        guest-authored payload (ValidateEventPacket).
    v17: host-assigned characters — cNetPlayerName grew mCharacter: the
        host's slot for that player (slot 0 = the host, each accepted guest
        the lowest free one, freed on leave), i.e. an index into the
        character list, so every player in a lobby is a DIFFERENT character.
        The effective player cap is min(max_players, character count).
    v18: character picker — reliable cNetCharacterRequest (type 30), guest
        -> host: "I want to play <file base name>" (by NAME, so machines
        whose lists are ordered differently still agree). Sent right after
        the guest's name (after PlayerJoin) when multiplayer.cfg has a
        character= preference, and whenever the player picks another one
        while connected. The host moves the requester to that slot if the
        character exists in ITS list, is not slot 0 (the host's) and is
        free, then re-broadcasts the name table; otherwise the request is
        ignored and the table stays as it is.
    v19: characters by NAME on the wire too — cNetPlayerName grew
        msCharacter (the file base name of that player's character, as the
        HOST's list spells it). A receiver looks the name up in its own
        list, so two machines whose lists differ in order or content still
        show every player as the same character (v17/v18 sent only the
        host's list index, and a guest with another order showed the host
        as Malik). mCharacter stays as the fallback when the name is not in
        the receiver's list. Every list is also put in the fixed character
        order now (phillip first), ghost_models= included. */
static const uint16_t kNetProtocolVersion = 19;


/** v13: cNetPlayerName::msName capacity. A name is at most this many
    printable ASCII characters; it is NOT required to be NUL-terminated on
    the wire (a 24-char name fills the field), so receivers copy with a
    bounded scan (cNetworkManager::SanitizePlayerName). */
static const size_t kNetPlayerNameMaxChars = 24;

/** v17: cNetPlayerName::mCharacter — "no slot known" (a guest's own name
    packet always carries it; the host ignores the byte from a guest). */
static const uint8_t kNetCharacterUnknown = 255;
/** v17: character slots are < this (ValidateEventPacket). The effective cap
    min(max_players <= 31, character count) keeps real slots well below. */
static const uint8_t kNetMaxCharacterSlots = 32;
/** v18: cNetCharacterRequest::msCharacter capacity — a character's file
    base name ("fisherman"), printable ASCII, NUL-padded, not necessarily
    NUL-terminated (a 24-char name fills the field). */
static const size_t kNetCharacterNameMaxChars = 24;

/** Snapshot send period, every sender (cNetworkManager::kSendPeriodSeconds
    is this value). The receiver uses cNetPlayerState::mSeq * this period as
    the SENDER's clock for ghost interpolation, so it must be one constant
    on both machines — it is protocol, not tuning. */
static const float kNetSendPeriodSeconds = 1.0f / 30.0f;

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
	eNetPacketType_VersionAck = 17, /* host -> guest, FIRST reliable packet: the
	                                   host really speaks this protocol. A guest
	                                   that gets a PlayerJoin with no ack first
	                                   knows the host is an OLD build. */
	/* Phase 6 — SHARED ENEMIES. The host runs the only real AI; guests puppet
	   their local enemy entities from EnemyState batches. */
	eNetPacketType_EnemyState = 18,  /* host -> guests, unreliable ch1, batched */
	eNetPacketType_EnemyEvent = 19,  /* host -> guests, reliable: death etc. */
	eNetPacketType_EnemyDamage = 20, /* guest -> host, reliable: my hit landed */
	eNetPacketType_PlayerDamage = 21,/* host -> ONE guest, reliable: an enemy's
	                                    attack landed on YOUR position */
	eNetPacketType_ScriptEvent = 22, /* reliable, any direction (host relays):
	                                    a script mutated shared state — var
	                                    writes, entity active, door locks,
	                                    item consumption. The party's worlds
	                                    stay in agreement. */
	eNetPacketType_EntityDamage = 23,/* reliable, any direction (host relays):
	                                    a weapon damaged a BREAKABLE object —
	                                    every world applies the same hit, so a
	                                    pickaxed door is broken for the whole
	                                    party (was: broken for one player,
	                                    intact wall for the other). */
	/* v14 — WORLD SNAPSHOT (late join, reconnect, host save/load). */
	eNetPacketType_MapReady = 24,      /* guest -> host, reliable ch0: my census
	                                      is paired with yours for THIS world —
	                                      send me its state (cNetMapReady) */
	eNetPacketType_WorldSnapshot = 25, /* host -> ONE guest, reliable ch0,
	                                      chunked (cNetSnapshotHdr + entries);
	                                      Begin ... sections ... End, the body
	                                      poses travel as ObjectState chunks
	                                      in between */
	eNetPacketType_PlayerName = 26,  /* v13, reliable ch0: guest -> host (my
	                                    name, right after PlayerJoin), host ->
	                                    every guest (the full table, one
	                                    packet per player; the host is id 1;
	                                    v17: + the player's character slot) */
	/* v15 — JOIN AUTHENTICATION (reliable ch0). */
	eNetPacketType_Auth = 27,      /* guest -> host, the guest's FIRST packet:
	                                  answer to the challenge (cNetAuth) */
	eNetPacketType_Challenge = 28, /* host -> guest, right after CONNECT and
	                                  before anything else (cNetChallenge) */
	/* v16 — VOICE. Keep cNetworkManager::IsAllowedFrom (NetworkManager.cpp)
	   in step with every new type: a type the table does not know is
	   DROPPED from either direction. */
	eNetPacketType_Voice = 29,       /* v16, UNSEQUENCED ch1, both directions
	                                    (host relays a guest's voice to the
	                                    other guests with the author id
	                                    stamped from the PEER, like
	                                    PlayerState): cNetVoice +
	                                    1..kNetVoiceMaxFramesPerPacket Opus
	                                    frames. A lost packet is concealed by
	                                    the decoder (PLC), never resent. */
	eNetPacketType_CharacterRequest = 30, /* v18, reliable ch0, guest -> host
	                                         only: the character the guest
	                                         wants (cNetCharacterRequest, by
	                                         file base name). The host answers
	                                         with the name table (moved) or
	                                         not at all (refused). */
};

/** v16 voice: fixed codec parameters — both ends must agree, so they are
    protocol, not tuning. 16 kHz mono, 20 ms frames (320 samples). */
static const int kNetVoiceSampleRate = 16000;
static const int kNetVoiceFrameSamples = 320; /* 20 ms at 16 kHz */
static const int kNetVoiceMaxFramesPerPacket = 2; /* 40 ms per packet */
/** Upper bound on the Opus bytes after the cNetVoice header (all frames,
    length prefixes included). 24 kbps VBR peaks well under 100 B/frame.
    ValidateEventPacket refuses anything longer. */
static const size_t kNetVoiceMaxPayload = 400;

/** eNetPacketType_ScriptEvent ops. */
enum eNetScriptOp : uint8_t
{
	eNetScriptOp_LocalVarSet = 0,
	eNetScriptOp_LocalVarAdd = 1,
	eNetScriptOp_GlobalVarSet = 2,
	eNetScriptOp_GlobalVarAdd = 3,
	eNetScriptOp_EntityActive = 6, /* mlVal = 0/1 */
	eNetScriptOp_DoorLocked = 7,   /* mlVal = 0/1 */
	eNetScriptOp_RemoveItem = 8,   /* an item was CONSUMED (key used etc.) */
	eNetScriptOp_LampLit = 9,      /* v14: SetLampLit — mlVal bit0 = lit,
	                                  bit1 = fade */
};

/** v14: eNetPacketType_WorldSnapshot section ids (cNetSnapshotHdr::mSection).
    Every section is sent as at least ONE chunk (possibly with mCount 0) so
    the guest can tell "host has no timers" from "section never arrived". */
enum eNetSnapSection : uint8_t
{
	eNetSnap_Begin = 0,     /* mCount = section chunks to follow (informational) */
	eNetSnap_LocalVar = 1,  /* cNetSnapVar[]    */
	eNetSnap_GlobalVar = 2, /* cNetSnapVar[]    */
	eNetSnap_Entity = 3,    /* cNetSnapEntity[] — every non-enemy iGameEntity */
	eNetSnap_TakenItem = 4, /* uint32_t qualified item hash[] (whole session) */
	eNetSnap_PartyItem = 5, /* char name[32][] — inventory names the party holds */
	eNetSnap_Enemy = 6,     /* cNetEnemyState[] */
	eNetSnap_Timer = 7,     /* cNetSnapTimer[]  — the host's LOCAL timers */
	eNetSnap_End = 255,     /* mCount = ObjectState body chunks that were sent
	                           between Begin and End (informational) */
};

/** cNetSnapEntity::mFlags */
static const uint8_t kNetSnapEntityFlag_Active = 1;
static const uint8_t kNetSnapEntityFlag_Locked = 2; /* SwingDoor only */
static const uint8_t kNetSnapEntityFlag_Lit = 4;    /* Lamp only */

/** Snapshot chunk payload cap (entries after the header). ENet fragments
    reliable packets above the MTU itself; staying under one datagram means
    one lost datagram costs one small retransmit (same policy as
    cBodySync::kMaxStatesPerBatch). */
static const unsigned kNetSnapMaxChunkPayload = 1200;

/** ENet connect data: sent inside the connection handshake itself, so a host
    can refuse an incompatible exe BEFORE any game packet flows. Old builds
    send 0 and get refused with kNetDisconnectBadVersion as the reason. This
    exists because direct-IP joins used to skip the version check entirely —
    a v3 host + v6 guest would connect and HALF-work (ghosts fine, physics
    garbage, no item sync), which reads as "the mod is broken" instead of
    "grab the same zip". */
static const uint32_t kNetConnectData = kNetProtocolMagic ^ (uint32_t)kNetProtocolVersion;
static const uint32_t kNetDisconnectBadVersion = 0xBADF00D5u;
/** v15 disconnect reasons (ENet disconnect data, shown by the join screen). */
static const uint32_t kNetDisconnectBadAuth = 0xBADAC0DEu; /* wrong/missing password, or no cNetAuth within kNetAuthTimeoutSeconds */
static const uint32_t kNetDisconnectFull = 0x5E12FA11u;    /* max_players reached (the browser row already shows counts) */
static const uint32_t kNetDisconnectKicked = 0xC1C0FFEEu;  /* too many protocol violations (kNetMaxStrikes) */

/** v15 join authentication. The host answers CONNECT with a 16-byte nonce;
    the guest replies with NetAuthDigest(password, nonce, version) and its
    name. server_password= empty means an OPEN server — the exchange still
    runs (with the empty password) so the state machine is one path, and a
    peer that never answers is dropped after kNetAuthTimeoutSeconds. */
static const size_t kNetPasswordMaxChars = 48;   /* cfg server_password / join_password */
static const float kNetAuthTimeoutSeconds = 5.0f;
static const unsigned kNetMaxStrikes = 20;       /* violations before kNetDisconnectKicked */
static const unsigned kNetMaxReliablePerSec = 200; /* reliable (ch0) packets per peer per second before a strike */

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

/** v11: cNetPlayerState::mFlags bits. The stance/ground truth the sender
    knows for free (iCharacterBody::IsOnGround, the move state and the state
    a jump was entered FROM) — mMoveState alone lost the crouch during a
    crouch-jump and never said whether the feet touch the floor. */
enum eNetPlayerFlags : uint8_t
{
	eNetPlayerFlag_OnGround = 1, /* character body has ground contact */
	eNetPlayerFlag_Crouch = 2,   /* crouched, ALSO while airborne from a crouch */
	eNetPlayerFlag_RunKey = 4,   /* run move state (shift) */
	eNetPlayerFlag_Jump = 8,     /* jump move state (jump key pressed) */
	eNetPlayerFlag_Dead = 16,    /* v12: cPlayer::IsDead() (death sequence
	                                running) — receivers read the health as
	                                0 whatever mHealth says */
};

/** v11: cNetPlayerState::mVelFwd / mVelRight scale — int8 units of 1/40 m/s
    (+-3.175 m/s covers Movement_Run ForwardSpeed with margin). */
static const float kNetPlayerVelScale = 40.0f;

#pragma pack(push, 1)
struct cNetPlayerState
{
	uint8_t mType; /**< eNetPacketType_PlayerState */
	uint8_t mPlayerID;
	uint16_t mSeq; /**< per-author counter (wraps). UNSEQUENCED delivery means
	    real internet paths REORDER these — applying a stale position after a
	    newer one snaps the ghost backward and flips its measured movement
	    direction 180 deg (the "impossible backpedal at 4.6 m/s" in the first
	    live-session log). Receivers drop anything not newer. */
	float mfPosX, mfPosY, mfPosZ; /**< v11: mfPosY is the sender's FEET height
	    (iCharacterBody::GetFeetPosition) — no head-bob, and the ghost mesh
	    origin (at its feet) lands on it directly. X/Z stay the camera's. */
	float mfPitch, mfYaw; /**< radians, FPS view (roll omitted on wire) */
	uint8_t mbFlashlightOn;
	uint8_t mMoveState; /**< eNetMoveState value (Walk/Run/Still/Jump/Crouch) —
	                         kept for the stance Y-offset overrides; the ghost
	                         animates from the v11 fields below */
	int8_t mVelFwd;   /**< v11: planar body velocity along the view forward,
	                       units 1/kNetPlayerVelScale m/s */
	int8_t mVelRight; /**< v11: same along the view right */
	uint8_t mFlags;   /**< v11: eNetPlayerFlags */
	uint8_t mHealth;  /**< v12: sender's cPlayer health, 0-100 rounded */
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

/** v13: one player's display name. Guest -> host: mPlayerID is ignored
    (the host trusts the peer it came from). Host -> guests: one per known
    player. msName is NUL-padded, NOT necessarily NUL-terminated (see
    kNetPlayerNameMaxChars); an empty name means "no name known" and the
    receiver shows "Player <id>".
    v17: mCharacter = the host-assigned character slot of mPlayerID (index
    into the sorted character list, modulo its length on the receiver);
    kNetCharacterUnknown from a guest (the host never reads it).
    v19: msCharacter = that slot's file base name in the HOST's list
    ("fisherman"), NUL-padded like msName; empty from a guest or when the
    player has no slot. Receivers prefer it over mCharacter. */
struct cNetPlayerName
{
	uint8_t mType; /**< eNetPacketType_PlayerName */
	uint8_t mPlayerID;
	char msName[kNetPlayerNameMaxChars];
	uint8_t mCharacter; /**< v17: slot, < kNetMaxCharacterSlots or kNetCharacterUnknown */
	char msCharacter[kNetCharacterNameMaxChars]; /**< v19: base name, "" = unknown */
};

/** v18: guest -> host, "move me to this character". msCharacter is the
    file base name without ".dae" as the guest's list spells it
    ("fisherman"); the host matches it ASCII-case-insensitively against ITS
    list. Printable ASCII, non-empty, NUL-padded (ValidateEventPacket). */
struct cNetCharacterRequest
{
	uint8_t mType; /**< eNetPacketType_CharacterRequest */
	char msCharacter[kNetCharacterNameMaxChars];
};

/** v16 voice packet header. Payload layout after the header, mFrames
    times: uint16_t length, then that many Opus bytes (one 20 ms frame).
    A zero length = the encoder produced nothing for that frame (the
    receiver runs PLC for it). Total payload <= kNetVoiceMaxPayload.
    mSeq counts PACKETS per author (wraps); the receiver drops stale or
    duplicate ones (unsequenced delivery reorders) and conceals gaps. */
struct cNetVoice
{
	uint8_t mType; /**< eNetPacketType_Voice */
	uint8_t mPlayerID; /**< author (the host overwrites it with the peer id on relay) */
	uint16_t mSeq;
	uint8_t mFrames; /**< 1..kNetVoiceMaxFramesPerPacket */
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
	char msItemName[32]; /**< v9: plain entity/inventory name — feeds the
	    PARTY inventory, so script HasItem() checks pass when ANY member
	    holds the item (a split torch+glowstick could otherwise deadlock
	    the boat-cabin door for everyone). */
};

/** Host -> guest immediately on connect (reliable ch0, BEFORE PlayerJoin). */
struct cNetVersionAck
{
	uint8_t mType; /**< eNetPacketType_VersionAck */
	uint16_t mlVersion;
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
	uint16_t mSeq;   /**< host batch counter (wraps): guards PURE-MOVING batches
	    against unsequenced-channel reordering (a stale batch pops every body
	    in it backward for a frame). Batches carrying rest poses and snapshot
	    chunks travel reliably and are always applied. */
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

/** One shared enemy's pose+vitals. Identity = FNV-1a of the ENTITY name
    (same scheme as bodies). Anim = FNV-1a of the clip name the host is
    playing; the guest reverse-maps it against its own mesh's clip list. */
struct cNetEnemyState
{
	uint32_t mlNameHash;
	float mfPosX, mfPosY, mfPosZ; /**< character body FEET position */
	float mfYaw;
	float mfHealth;
	uint32_t mlAnimHash; /**< 0 = none commanded yet */
	uint8_t mFlags;      /**< bit0 = anim loops, bit1 = entity active */
};

struct cNetEnemyBatch
{
	uint8_t mType;   /**< eNetPacketType_EnemyState */
	uint8_t mCount;
	uint8_t mMapGen;
	uint16_t mSeq;   /**< reorder guard, same int16-diff scheme as bodies */
};

struct cNetEnemyEvent
{
	uint8_t mType;  /**< eNetPacketType_EnemyEvent */
	uint32_t mlNameHash;
	uint8_t mEvent; /**< 0 = died (guest runs its LOCAL death for the ragdoll) */
};

struct cNetEnemyDamage
{
	uint8_t mType; /**< eNetPacketType_EnemyDamage */
	uint32_t mlNameHash;
	float mfDamage; /**< RAW damage — the host applies its own scaling */
	int8_t mlStrength;
};

struct cNetPlayerDamage
{
	uint8_t mType; /**< eNetPacketType_PlayerDamage */
	uint8_t mPlayerID; /**< the guest whose player takes it */
	float mfDamage;
};

/** A weapon hit a breakable non-enemy entity. Identity = qualified
    map:name hash (same scheme as item pickups). Both sims apply the same
    damage; same health + same hits = the object breaks everywhere. */
struct cNetEntityDamage
{
	uint8_t mType; /**< eNetPacketType_EntityDamage */
	uint32_t mlQualHash;
	float mfDamage;
	int8_t mlStrength;
};

/** A script mutated shared state on one machine; everyone else applies the
    same mutation directly (never back through the script hooks — the
    gbNetScriptApplying flag suppresses re-broadcast). */
struct cNetScriptEvent
{
	uint8_t mType; /**< eNetPacketType_ScriptEvent */
	uint8_t mOp;   /**< eNetScriptOp */
	char msName[48];
	int32_t mlVal;
};

/** v14, guest -> host: our census is paired with the host's for the world
    we are standing in (match or not) — the host answers with a
    WorldSnapshot if the generation and map still agree. */
struct cNetMapReady
{
	uint8_t mType;             /**< eNetPacketType_MapReady */
	uint8_t mMapGen;           /**< HOST generation we just paired with (census) */
	uint32_t mlMapNameHash;    /**< NetHashName(lowercase, ext-stripped current map) */
	uint16_t mlLocalBodyCount; /**< our census, for the host-side log */
	uint32_t mlLocalChecksum;
};

/** v14: header of every WorldSnapshot chunk. mMapGen = host generation (the
    census/body-batch value); mSnapId = host counter per snapshot so a Begin
    with a new id discards a half-buffered one and stray chunks of another
    id are dropped. mCount = entries that follow (see eNetSnapSection). */
struct cNetSnapshotHdr
{
	uint8_t mType;    /**< eNetPacketType_WorldSnapshot */
	uint8_t mSection; /**< eNetSnapSection */
	uint8_t mMapGen;
	uint8_t mSnapId;
	uint16_t mCount;
};

/** One script var (local or global). 48 matches cNetScriptEvent::msName. */
struct cNetSnapVar
{
	char msName[48];
	int32_t mlVal;
};

/** One game entity: identity = NetHashName(entity name), unqualified (the
    generation guard scopes the map). mType = eGameEntityType. */
struct cNetSnapEntity
{
	uint32_t mlNameHash;
	uint8_t mType;
	uint8_t mFlags; /**< kNetSnapEntityFlag_* */
	float mfHealth;
};

/** One of the host's LOCAL script timers (global ones outlive the map and
    are not part of a world's state). */
struct cNetSnapTimer
{
	char msName[48];
	char msCallback[48];
	float mfTime;
	uint8_t mbPaused;
};

/** v15, host -> guest, the host's FIRST packet (before VersionAck). */
struct cNetChallenge
{
	uint8_t mType; /**< eNetPacketType_Challenge */
	uint8_t mNonce[16];
};

/** v15, guest -> host, the guest's FIRST packet. mDigest =
    NetAuthDigest(join_password, mNonce, mlVersion); msName is the guest's
    player name (same NUL-padded rules as cNetPlayerName — the guest still
    sends cNetPlayerName after PlayerJoin, this copy only lets the host
    log/announce it at accept time). */
struct cNetAuth
{
	uint8_t mType; /**< eNetPacketType_Auth */
	uint16_t mlVersion; /**< kNetProtocolVersion of the guest */
	uint8_t mDigest[16];
	char msName[kNetPlayerNameMaxChars];
};
#pragma pack(pop)

/** v15: 128-bit digest of (nonce || version || password || lengths) for the
    join challenge. NOT a cryptographic hash: four independent FNV-1a-style
    lanes with cross-lane rotation feedback and a murmur3 finalizer, then
    two full re-mix rounds. It makes replaying a captured answer against a
    fresh nonce impossible and keeps a password out of the packets in the
    clear; an attacker who captures (nonce, digest) pairs can still test
    password guesses offline, exactly like any unsalted challenge scheme —
    pick a real password for a public server. No external libs, same
    result on both machines (byte-wise, endian-free). */
static inline uint32_t NetAuthRotl(uint32_t x, unsigned r)
{
	return (x << r) | (x >> (32u - r));
}
static inline uint32_t NetAuthFmix(uint32_t h)
{
	h ^= h >> 16; h *= 0x85EBCA6Bu;
	h ^= h >> 13; h *= 0xC2B2AE35u;
	h ^= h >> 16;
	return h;
}
static inline void NetAuthDigest(const char *apPassword, size_t alPasswordLen,
	const uint8_t aNonce[16], uint16_t alVersion, uint8_t aOut[16])
{
	uint32_t h[4] = { 2166136261u ^ 0xA5A5A5A5u, 2166136261u ^ 0x3C3C3C3Cu,
	                  2166136261u ^ 0x0F0F0F0Fu, 2166136261u ^ 0x96969696u };
	static const uint32_t kPrime[4] = { 16777619u, 0x01000193u * 3u + 2u, 0x9E3779B1u, 0x27D4EB2Fu };
	/* input stream: nonce, version (2 bytes), password, then both lengths */
	uint8_t aHead[18];
	for (int i = 0; i < 16; ++i)
		aHead[i] = aNonce ? aNonce[i] : 0;
	aHead[16] = (uint8_t)(alVersion & 0xFFu);
	aHead[17] = (uint8_t)(alVersion >> 8);
	uint8_t aTail[8];
	for (int i = 0; i < 4; ++i)
	{
		aTail[i] = (uint8_t)((uint32_t)alPasswordLen >> (8 * i));
		aTail[4 + i] = (uint8_t)(16u >> (8 * i));
	}
	const uint8_t *aPart[3] = { aHead, (const uint8_t *)apPassword, aTail };
	const size_t aPartLen[3] = { sizeof(aHead), apPassword ? alPasswordLen : 0, sizeof(aTail) };
	unsigned lLane = 0;
	for (int p = 0; p < 3; ++p)
	{
		for (size_t i = 0; i < aPartLen[p]; ++i)
		{
			const uint32_t b = aPart[p][i];
			h[lLane] ^= b;
			h[lLane] *= kPrime[lLane];
			h[(lLane + 1u) & 3u] += NetAuthRotl(h[lLane], 13u) ^ (b * 0x9E3779B9u);
			lLane = (lLane + 1u) & 3u;
		}
	}
	for (int round = 0; round < 2; ++round)
	{
		for (int i = 0; i < 4; ++i)
		{
			h[i] = NetAuthFmix(h[i] + NetAuthRotl(h[(i + 3) & 3], 7u) + (uint32_t)(round * 4 + i + 1));
			h[(i + 1) & 3] ^= NetAuthRotl(h[i], 21u);
		}
	}
	for (int i = 0; i < 4; ++i)
	{
		aOut[i * 4 + 0] = (uint8_t)(h[i] & 0xFFu);
		aOut[i * 4 + 1] = (uint8_t)((h[i] >> 8) & 0xFFu);
		aOut[i * 4 + 2] = (uint8_t)((h[i] >> 16) & 0xFFu);
		aOut[i * 4 + 3] = (uint8_t)((h[i] >> 24) & 0xFFu);
	}
}

static_assert(sizeof(cNetPlayerJoin) == 2, "");
static_assert(sizeof(cNetPlayerLeave) == 2, "");
static_assert(sizeof(cNetPlayerName) == 51, ""); /* v13; v17: +mCharacter; v19: +msCharacter */
static_assert(sizeof(cNetCharacterRequest) == 25, ""); /* v18 */
static_assert(sizeof(cNetVoice) == 5, "");       /* v16 */
static_assert(sizeof(cNetPlayerState) == 30, ""); /* v7: +mSeq; v11: +vel/flags; v12: +mHealth */
static_assert(sizeof(cNetDiscoveryPing) == 7, "");
static_assert(sizeof(cNetDiscoveryPong) == 75, "");
static_assert(sizeof(cNetObjectState) == 33, "");
static_assert(sizeof(cNetObjectStateBatch) == 5, ""); /* v7: +mSeq */
static_assert(sizeof(cNetBodyCensus) == 8, "");
static_assert(sizeof(cNetBodyGrabBegin) == 22, "");
static_assert(sizeof(cNetBodyGrabTarget) == 17, "");
static_assert(sizeof(cNetBodyGrabEnd) == 17, "");
static_assert(sizeof(cNetBodyPush) == 30, "");
static_assert(sizeof(cNetBodyGrabDeny) == 5, "");
static_assert(sizeof(cNetMapChange) == 129, "");
static_assert(sizeof(cNetItemPickup) == 37, ""); /* v9: +msItemName */
static_assert(sizeof(cNetItemDrop) == 137, "");
static_assert(sizeof(cNetVersionAck) == 3, "");
static_assert(sizeof(cNetEnemyBatch) == 5, "");
static_assert(sizeof(cNetEnemyState) == 29, "");
static_assert(sizeof(cNetEnemyEvent) == 6, "");
static_assert(sizeof(cNetEnemyDamage) == 10, "");
static_assert(sizeof(cNetPlayerDamage) == 6, "");
static_assert(sizeof(cNetScriptEvent) == 54, "");
static_assert(sizeof(cNetEntityDamage) == 10, "");
static_assert(sizeof(cNetMapReady) == 12, "");     /* v14 */
static_assert(sizeof(cNetSnapshotHdr) == 6, "");   /* v14 */
static_assert(sizeof(cNetSnapVar) == 52, "");      /* v14 */
static_assert(sizeof(cNetSnapEntity) == 10, "");   /* v14 */
static_assert(sizeof(cNetSnapTimer) == 101, "");   /* v14 */
static_assert(sizeof(cNetChallenge) == 17, "");    /* v15 */
static_assert(sizeof(cNetAuth) == 43, "");         /* v15 */

//-----------------------------------------------------------------------
// MASTER SERVER protocol (internet server browser). Raw UDP like discovery,
// NOT ENet, and a protocol of its own: its own magic, its own version, its own
// type enum. It never touches kNetProtocolVersion — a host advertises the game
// protocol it speaks inside the packet, and browsers grey out mismatches.
//
//   host    -> master   Register     every kNetMasterRegisterSeconds while
//                                    hosting with public=1 (+ master_server=)
//   host    -> master   Unregister   on stop hosting (best effort)
//   browser -> master   List
//   master  -> browser  Entries      one or more datagrams, up to
//                                    kNetMasterMaxEntriesPerDatagram each
//
// The master records the SOURCE IP of a Register plus the game port GIVEN in
// it (the host cannot know its public address; the source port is whatever
// the NAT picked for the discovery socket and is useless). Entries therefore
// carry the public IPv4 as 4 raw bytes in network order, a.b.c.d — no
// htonl/ntohl question on either side, and the Python master packs it with
// inet_aton. Everything else is little-endian (same-endian x86 peers, exactly
// like the ENet payloads; the master uses '<' formats).
//
// Reference implementation: tools/master_server.py (Python 3, stdlib only).
//-----------------------------------------------------------------------

/** 'PNMS' as a plain little-endian uint32 (bytes 53 4D 4E 50 on the wire). */
static const uint32_t kNetMasterMagic = 0x504E4D53u;

/** Bump when any cNetMaster* struct changes layout. */
static const uint16_t kNetMasterProtocolVersion = 1;

/** Default master listen port (udp). */
static const uint16_t kNetMasterDefaultPort = 7779;

/** Hosts re-register at this interval; the master expires an entry after
    kNetMasterExpirySeconds without one (3 missed beacons). */
static const float kNetMasterRegisterSeconds = 20.0f;
static const uint16_t kNetMasterExpirySeconds = 60;

/** Entries per reply datagram: 8 + 10*77 = 778 bytes, safely under any MTU. */
static const uint8_t kNetMasterMaxEntriesPerDatagram = 10;

/** The browser collects Entries datagrams for this long after a List. */
static const float kNetMasterListWindowSeconds = 3.0f;

/** Placeholder default master for `public=1` WITHOUT `master_server=`: it is
    an RFC 2606 reserved name that resolves to nothing, so nobody registers
    anywhere they did not explicitly opt into. Replace it with your own
    master's host:port (see README 'Public servers'), or set master_server=. */
static const char *const kNetMasterDefaultHost = "master.example.invalid:7779";

enum eNetMasterPacketType : uint8_t
{
	eNetMasterPacketType_Register = 1,   /* host -> master */
	eNetMasterPacketType_Unregister = 2, /* host -> master */
	eNetMasterPacketType_List = 3,       /* browser -> master */
	eNetMasterPacketType_Entries = 4,    /* master -> browser, header + N entries */
};

/** cNetMasterRegister::mFlags / cNetMasterEntry::mFlags */
static const uint8_t kNetMasterFlag_Password = 1; /* host requires a join password */

#pragma pack(push, 1)
/** Common 7-byte prefix of every master packet: type, magic, master version.
    The master drops anything shorter than this or with another magic. */
struct cNetMasterHeader
{
	uint8_t mType;       /**< eNetMasterPacketType */
	uint32_t mlMagic;    /**< kNetMasterMagic */
	uint16_t mlMasterVer;/**< kNetMasterProtocolVersion */
};

/** Host -> master, periodic. Source IP + mlGamePort is the server's key. */
struct cNetMasterRegister
{
	uint8_t mType;       /**< eNetMasterPacketType_Register */
	uint32_t mlMagic;
	uint16_t mlMasterVer;
	uint16_t mlGamePort;    /**< ENet port guests connect to (the one to forward) */
	uint8_t mlPlayerCount;
	uint8_t mlMaxPlayers;
	uint8_t mFlags;         /**< kNetMasterFlag_* */
	uint16_t mlProtocolVer; /**< kNetProtocolVersion this host speaks */
	char msServerName[32];  /**< NUL-terminated, truncated */
	char msMapName[32];
};

/** Host -> master when it stops hosting. Same key as Register. */
struct cNetMasterUnregister
{
	uint8_t mType;       /**< eNetMasterPacketType_Unregister */
	uint32_t mlMagic;
	uint16_t mlMasterVer;
	uint16_t mlGamePort;
};

/** Browser -> master. The browser's game protocol version travels along so a
    master can log/filter; the reference master returns every live server and
    lets the browser grey out mismatches. */
struct cNetMasterList
{
	uint8_t mType;       /**< eNetMasterPacketType_List */
	uint32_t mlMagic;
	uint16_t mlMasterVer;
	uint16_t mlProtocolVer;
};

/** Master -> browser: header, then mCount cNetMasterEntry records back to
    back. A master with more than kNetMasterMaxEntriesPerDatagram live
    servers sends several datagrams; a browser merges them by (ip, port). */
struct cNetMasterEntries
{
	uint8_t mType;       /**< eNetMasterPacketType_Entries */
	uint32_t mlMagic;
	uint16_t mlMasterVer;
	uint8_t mCount;      /**< 0..kNetMasterMaxEntriesPerDatagram */
};

struct cNetMasterEntry
{
	uint8_t mIp4[4];        /**< public IPv4, network order (a.b.c.d) */
	uint16_t mlGamePort;
	uint8_t mlPlayerCount;
	uint8_t mlMaxPlayers;
	uint8_t mFlags;         /**< kNetMasterFlag_* */
	uint16_t mlProtocolVer; /**< the HOST's kNetProtocolVersion */
	uint16_t mlAgeSeconds;  /**< seconds since the host's last Register */
	char msServerName[32];
	char msMapName[32];
};
#pragma pack(pop)

static_assert(sizeof(cNetMasterHeader) == 7, "");
static_assert(sizeof(cNetMasterRegister) == 78, "");
static_assert(sizeof(cNetMasterUnregister) == 9, "");
static_assert(sizeof(cNetMasterList) == 9, "");
static_assert(sizeof(cNetMasterEntries) == 8, "");
static_assert(sizeof(cNetMasterEntry) == 77, "");
static_assert(sizeof(cNetMasterEntries) + kNetMasterMaxEntriesPerDatagram * sizeof(cNetMasterEntry) <= 1024,
	"master reply must fit the browser's receive buffer");

#endif /* NETWORK_PACKETS_H */
