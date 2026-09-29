#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "StdAfx.h"
#include "NetworkManager.h"
#include "BodySync.h"
#include "VoiceChat.h" /* v16: proximity voice chat */
#include "system/String.h"
#include "Init.h"
#include "MapHandler.h"
#include "GameEntity.h" /* iGameEntity for the one-of-each item sweep */
#include "GameEnemy.h"   /* Phase 6: shared-enemy streaming */
#include "CharacterMove.h"
#include "GameScripts.h" /* v9: NetApplyScriptEvent; v14: gbNetScriptPlayerContext */
#include "GameSwingDoor.h" /* v14 world snapshot: door locks */
#include "GameLamp.h"      /* v14 world snapshot: lamp lit */
#include "Inventory.h"     /* v14 world snapshot: party item names */
#include "resources/FileSearcher.h" /* v15: ItemDrop/MapChange file names must resolve */

/* v9: the engine's script-var writes fire this (see engine ScriptFuncs.cpp) */
namespace hpl { extern void (*gpScriptVarNetCallback)(int alOp, const char* asName, int alVal); }
extern bool gbNetScriptApplying; /* GameScripts.cpp */
static cNetworkManager *gpNetMgrForScript = NULL;
static void ScriptVarNetThunk(int alOp, const char* asName, int alVal)
{
	if (gpNetMgrForScript)
		gpNetMgrForScript->NetOnScriptEvent(alOp, asName, alVal);
}
#include "GraphicsHelper.h" /* loading screen for the lobby auto-launch */
#include "MainMenu.h" 
#include "Player.h"
#include "PlayerHelper.h"
#include "input/ActionKeyboard.h"
#include "scene/Camera3D.h"
#include "scene/World3D.h"
#include "physics/PhysicsWorld.h"
#include "physics/PhysicsBody.h"
#include "math/Math.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime> /* v15: nonce seed */

//======================================================================
// Shared by the real and the stub build.
//======================================================================

namespace
{
/** Wire id of the local preview ghost (ghost_preview=1) — above any real
    guest id, never sent, never in m_mapGhosts. */
const uint8_t kPreviewGhostId = 250;

/** m/s -> cNetPlayerState::mVelFwd/mVelRight (1/kNetPlayerVelScale m/s, saturating). */
int8_t EncodeNetVel(float afMetersPerSec)
{
	float f = afMetersPerSec * kNetPlayerVelScale;
	if (f > 127.0f) f = 127.0f;
	else if (f < -127.0f) f = -127.0f;
	const int l = (f >= 0.0f) ? (int)(f + 0.5f) : -(int)(-f + 0.5f);
	return (int8_t)l;
}
}

bool cNetworkManager::BuildLocalSnapshot(cNetPlayerState *apOut) const
{
	if (!apOut || !mpInit || !mpInit->mpPlayer)
		return false;
	cCamera3D *cam = mpInit->mpPlayer->GetCamera();
	if (!cam)
		return false;
	iCharacterBody *pBody = mpInit->mpPlayer->GetCharacterBody();

	memset(apOut, 0, sizeof(*apOut));
	const cVector3f p = cam->GetPosition();
	apOut->mType = eNetPacketType_PlayerState;
	apOut->mPlayerID = mlLocalPlayerId;
	apOut->mfPosX = p.x;
	/* v11: FEET height, not the camera's — the camera carries the head-bob
	   (cPlayerHeadMove) and the ghost mesh origin is at its feet. */
	apOut->mfPosY = pBody ? pBody->GetFeetPosition().y : p.y;
	apOut->mfPosZ = p.z;
	apOut->mfPitch = cam->GetPitch();
	apOut->mfYaw = cam->GetYaw();
	cPlayerFlashLight *fl = mpInit->mpPlayer->GetFlashLight();
	apOut->mbFlashlightOn = (uint8_t)(fl && fl->IsActive() && !fl->IsDisabled());
	/* Engine -> wire stance mapping, explicit per state so an engine enum
	   reorder cannot silently change the protocol (the wire values are frozen
	   — see eNetMoveState). Unknown states go out as Run. */
	const ePlayerMoveState eMove = mpInit->mpPlayer->GetMoveState();
	switch (eMove)
	{
	case ePlayerMoveState_Walk:   apOut->mMoveState = eNetMoveState_Walk;   break;
	case ePlayerMoveState_Run:    apOut->mMoveState = eNetMoveState_Run;    break;
	case ePlayerMoveState_Still:  apOut->mMoveState = eNetMoveState_Still;  break;
	case ePlayerMoveState_Jump:   apOut->mMoveState = eNetMoveState_Jump;   break;
	case ePlayerMoveState_Crouch: apOut->mMoveState = eNetMoveState_Crouch; break;
	default:                      apOut->mMoveState = eNetMoveState_Run;    break;
	}

	/* v11 flags: ground contact from the character body; crouch/run survive
	   a jump because the jump state remembers what it was entered from. */
	bool bCrouch = (eMove == ePlayerMoveState_Crouch);
	bool bRun = (eMove == ePlayerMoveState_Run);
	if (eMove == ePlayerMoveState_Jump)
	{
		iPlayerMoveState *pJump = mpInit->mpPlayer->GetMoveStateData(ePlayerMoveState_Jump);
		if (pJump)
		{
			bCrouch = (pJump->mPrevMoveState == ePlayerMoveState_Crouch);
			bRun = (pJump->mPrevMoveState == ePlayerMoveState_Run);
		}
	}
	uint8_t lFlags = 0;
	if (pBody == NULL || pBody->IsOnGround())
		lFlags |= eNetPlayerFlag_OnGround;
	if (bCrouch)
		lFlags |= eNetPlayerFlag_Crouch;
	if (bRun)
		lFlags |= eNetPlayerFlag_RunKey;
	if (eMove == ePlayerMoveState_Jump)
		lFlags |= eNetPlayerFlag_Jump;
	/* v12 party health: 0-100 rounded; the Dead bit covers the one-frame
	   gap between health reaching 0 and the death sequence starting, and a
	   scripted death (IsDead() is what cPlayer::Damage keys off). */
	float fHealth = mpInit->mpPlayer->GetHealth();
	if (fHealth < 0.0f) fHealth = 0.0f;
	else if (fHealth > 100.0f) fHealth = 100.0f;
	apOut->mHealth = (uint8_t)(fHealth + 0.5f);
	if (apOut->mHealth == 0 && fHealth > 0.0f)
		apOut->mHealth = 1; /* 0 means DEAD to every receiver; 0.3 hp is alive */
	if (mpInit->mpPlayer->IsDead() || fHealth <= 0.0f)
	{
		lFlags |= eNetPlayerFlag_Dead;
		apOut->mHealth = 0;
	}
	apOut->mFlags = lFlags;

	/* v11 velocity: the body's true world velocity over the last physics
	   step, projected on the view frame (forward = (-sin y, 0, -cos y),
	   right = (cos y, 0, -sin y) — cCamera3D::UpdateMoveMatrix). */
	if (pBody)
	{
		float fStep = mpInit->mpGame ? mpInit->mpGame->GetStepSize() : 0.0f;
		if (fStep <= 0.0f)
			fStep = 1.0f / 60.0f;
		const cVector3f v = pBody->GetVelocity(fStep);
		const float fSinY = sinf(apOut->mfYaw);
		const float fCosY = cosf(apOut->mfYaw);
		apOut->mVelFwd = EncodeNetVel(-v.x * fSinY - v.z * fCosY);
		apOut->mVelRight = EncodeNetVel(v.x * fCosY - v.z * fSinY);
	}
	return mlLocalPlayerId != 0;
}

cGhostPlayer *cNetworkManager::CreateGhost(uint8_t alId, size_t alMeshIdx)
{
	if (!mpWorld)
		return NULL;
	hpl::tString sMesh;
	if (mvGhostMeshPaths.empty() == false)
		sMesh = mvGhostMeshPaths[alMeshIdx % mvGhostMeshPaths.size()];

	/* v11: the wire carries the sender's FEET and the exported meshes have
	   their origin at the feet, so the mesh lands on the wire position with
	   NO offset. The sender's camera sits feetY + Height/2 + CameraHeightAdd
	   + 0.71 above that (Player.cpp:376,968 plus the empirical +0.71 that four
	   rounds of eyeball-grounding against verified-grounded clips measured
	   in the pre-v11 camera-relative days) — that eye height is what the
	   enemy senses and the lights get. */
	float fCamAdd = 0.0f, fStandH = 1.9f, fCrouchH = 1.0f;
	if (mpInit && mpInit->mpGameConfig)
	{
		fCamAdd = mpInit->mpGameConfig->GetFloat("Player", "CameraHeightAdd", 0);
		fStandH = mpInit->mpGameConfig->GetFloat("Player", "Height", 1.9f);
		fCrouchH = mpInit->mpGameConfig->GetFloat("Player", "CrouchHeight", 1.0f);
	}
	const float kCamFeetCorrection = 0.71f;
	const float fEyeStand = fStandH * 0.5f + fCamAdd + kCamFeetCorrection;
	const float fEyeCrouch = fCrouchH * 0.5f + fCamAdd + kCamFeetCorrection;

	/* multiplayer.cfg overrides are offsets from the FEET now (any value <
	   9000 wins). A pre-v11 cfg still holds camera-relative values like
	   -1.45 — anything below -0.9 m can only be one of those, so convert it
	   (camera - eye height = feet) instead of sinking the mesh. */
	float fStandOffset = (mfGhostMeshBodyYOffset > 9000.0f) ? 0.0f : mfGhostMeshBodyYOffset;
	bool bCrouchGiven = (mfGhostMeshBodyYOffsetCrouch <= 9000.0f);
	float fCrouchOffset = bCrouchGiven ? mfGhostMeshBodyYOffsetCrouch : 0.0f;
	if (mvGhostMeshPaths.empty() == false && mvGhostBodyYList.empty() == false)
	{
		const size_t lMeshIdx = alMeshIdx % mvGhostMeshPaths.size();
		fStandOffset = mvGhostBodyYList[lMeshIdx % mvGhostBodyYList.size()];
		bCrouchGiven = (mvGhostBodyYCrouchList.empty() == false);
		if (bCrouchGiven)
			fCrouchOffset = mvGhostBodyYCrouchList[lMeshIdx % mvGhostBodyYCrouchList.size()];
	}
	/* Convert each GIVEN value with its own eye height first; an unset
	   crouch value then inherits the CONVERTED stand offset (copying the raw
	   camera-relative stand value and converting it with the crouch eye
	   height sank the crouched ghost ~0.35 m). */
	bool bLegacy = false;
	if (fStandOffset < -0.9f) { fStandOffset += fEyeStand; bLegacy = true; }
	if (bCrouchGiven)
	{
		if (fCrouchOffset < -0.9f) { fCrouchOffset += fEyeCrouch; bLegacy = true; }
	}
	else
		fCrouchOffset = fStandOffset;
	if (bLegacy)
		Log(" multiplayer: ghost_body_y* look camera-relative (pre-v11); v11 offsets are from the FEET — using %.2f/%.2f, set 0 or remove the keys\n",
			fStandOffset, fCrouchOffset);

	return hplNew(cGhostPlayer, (mpWorld, alId, sMesh, fStandOffset, fCrouchOffset,
		fEyeStand, fEyeCrouch, &mGhostTuning));
}

void cNetworkManager::DestroyPreviewGhost(bool abOrphan)
{
	if (mpPreviewGhost == NULL)
		return;
	if (abOrphan)
		mpPreviewGhost->OrphanWorld();
	hplDelete(mpPreviewGhost);
	mpPreviewGhost = NULL;
}

//======================================================================
// Both builds: master-server address (no sockets involved).

hpl::tString cNetworkManager::GetMasterServer() const
{
	if (!msMasterServer.empty())
		return msMasterServer;
	if (mbPublic)
		return kNetMasterDefaultHost;
	return "";
}

//======================================================================

#ifndef PENUMBRA_MULTIPLAYER

struct cNetworkManager::Impl {};

const float cNetworkManager::kSendPeriodSeconds = kNetSendPeriodSeconds; /* v10:
    20 -> 30 Hz — noticeably smoother object/enemy motion; ~18 KB/s peak
    is still nothing for any internet link. v11: the receiver clocks ghost
    interpolation off seq * period, so the value lives in NetworkPackets.h */
const float cNetworkManager::kDiscoveryWindowSeconds = 1.5f;

cNetworkManager::cNetworkManager(cInit *apInit)
	: mpInit(apInit)
	  , mbHosting(false)
	  , mbClientConnected(false)
	  , mbHadJoinPacket(false)
	  , mbSpawnedAtHost(false)
	  , mbGotVersionAck(false)
	  , mlStateSeqOut(0)
	  , mlEnemySeqOut(0)
	  , mlEnemySeqIn(0)
	  , mbEnemySeqInKnown(false)
	  , mbApplyingRemoteMapChange(false)
	  , mbLocalMapChangeArmed(false)
	  , mbHavePendingMapChange(false)
	  , mfSendAccum(0)
	  , mpWorld(NULL)
	  , mlLocalPlayerId(0)
	  , mlListenPort(7777)
	  , mlNextGuestId(2)
	  , mlDefaultPort(7777)
	  , mbActionsRegistered(false)
	  , mpImpl(new Impl())
	  , mvGhostMeshPaths()
	  , mfGhostMeshBodyYOffset(9999.0f)  /* AUTO: derived from game.cfg Player Height/CameraHeightAdd */
	  , mfGhostMeshBodyYOffsetCrouch(9999.0f) /* AUTO: derived from CrouchHeight/CameraHeightAdd */
	  , mvGhostBodyYList()
	  , mvGhostBodyYCrouchList()
	  , mGhostTuning()
	  , mbGhostPreview(false)
	  , mlGhostPreviewModel(0)
	  , mpPreviewGhost(NULL)
	  , mlPreviewSeq(0)
	  , mfPreviewSendAccum(0)
	  , mfPreviewSpawnDelay(0.75f)
	  , mvPreviewCenter(0, 0, 0)
	  , mfPreviewFacingYaw(0)
	  , mfPreviewCircleAngle(0)
	  , mlPreviewClipIdx(-1)
	  , mbPreviewCrouch(false)
	  , mlPreviewTreadmill(0)
	  , mvDiscovered()
	  , mbDiscoveryActive(false)
	  , mfDiscoveryTimeLeft(0)
	  , msServerName("") /* v13: empty = "<player_name>'s game" in the pong */
	  , mlMaxPlayers(4)
	  , msMasterServer("")
	  , mbPublic(false)
	  , mbPublicExplicit(false)
	  , mvInternet()
	  , mbInternetActive(false)
	  , mfInternetTimeLeft(0)
	  , msInternetFailReason("")
	  , mfMasterRegisterAccum(0)
	  , mbMasterRegistered(false)
	  , mpBodySync(new cBodySync())
	  , mbCoopRespawn(true) /* v12: multiplayer.cfg coop_respawn */
	  , msPlayerName()      /* v13: multiplayer.cfg player_name */
	  , m_mapPlayerNames()
	  , mvPartyEvents()
	  , m_setJoinAnnounced()
	  , mfSinceJoinSeconds(0)
	  , mvSnapChunks()      /* v14: world snapshot */
	  , mlSnapId(0)
	  , mlSnapGen(0)
	  , mbSnapBuffering(false)
	  , mfSnapAge(0)
	  , mlSnapIdOut(0)
	  , msServerPassword()  /* v15: multiplayer.cfg server_password ("" = open) */
	  , msJoinPassword()    /* v15: multiplayer.cfg join_password */
	  , mbAuthSent(false)
	  , mlGuestViolationsLogged(0)
	  , mvFreeGuestIds()
	  , mpVoice(NULL)       /* v16: voice chat (created in Startup) */
	  , mbVoiceEnabled(true)
	  , mbVoiceOpenMic(false)
	  , mfVoiceVolume(1.0f)
{
	/* v9: listen to the engine's script-var writes for replication (the
	   stub build registers too — its NetOnScriptEvent is a no-op) */
	gpNetMgrForScript = this;
	hpl::gpScriptVarNetCallback = ScriptVarNetThunk;
}

cNetworkManager::~cNetworkManager()
{
	hpl::gpScriptVarNetCallback = NULL;
	gpNetMgrForScript = NULL;
	Disconnect(); /* v16: also shuts the voice chat down (AL context still alive: cGame dies later) */
	ClearGhostsInternal();
	if (mpVoice)
		hplDelete(mpVoice);
	mpVoice = NULL;
	delete mpBodySync;
	mpBodySync = NULL;
	delete mpImpl;
	mpImpl = NULL;
}

void cNetworkManager::Startup()
{
}

void cNetworkManager::HostGame(uint16_t)
{
	Log(" Multiplayer: rebuild with PENUMBRA_MULTIPLAYER + vcpkg enet.\n");
}

void cNetworkManager::JoinGame(const char *)
{
}

void cNetworkManager::StartDiscovery()
{
	Log(" Multiplayer: rebuild with PENUMBRA_MULTIPLAYER + vcpkg enet.\n");
}

void cNetworkManager::StopDiscovery()
{
}

void cNetworkManager::RefreshInternetServers()
{
	msInternetFailReason = "rebuild with PENUMBRA_MULTIPLAYER";
	Log(" Multiplayer: rebuild with PENUMBRA_MULTIPLAYER + vcpkg enet.\n");
}

void cNetworkManager::StopInternetRefresh()
{
}

void cNetworkManager::SetPublic(bool abPublic)
{
	mbPublic = abPublic;
}

void cNetworkManager::Disconnect()
{
	mbHosting = false;
	mbClientConnected = false;
	mbHadJoinPacket = false;
	mlLocalPlayerId = 0;
	mlNextGuestId = 2;
	msDeferredJoinAddress = "";
}

void cNetworkManager::Update(float afTimeStep)
{
	UpdatePartyEvents(afTimeStep); /* v13: the feed ages out even here */
}

void cNetworkManager::ClearGhostsInternal()
{
	m_mapGhostSeq.clear(); /* new session, new counters */
	m_mapGhostMoveState.clear();
	m_mapGhostHealth.clear();
	m_mapPlayerNames.clear(); /* v13: names + "joined" bookkeeping are per session */
	m_setJoinAnnounced.clear();
	if (mpVoice)
		mpVoice->DropAllPlayers(); /* v16: every remote decoder + AL source */
	for (std::map<uint8_t, cGhostPlayer *>::iterator it = m_mapGhosts.begin(); it != m_mapGhosts.end(); ++it)
		hplDelete(it->second);
	m_mapGhosts.clear();
	DestroyPreviewGhost(false);
}

void cNetworkManager::EmitLocalSnapshots()
{
}

void cNetworkManager::DispatchIncoming(const void *, size_t)
{
}

void cNetworkManager::DropRemotePlayer(uint8_t id)
{
	m_mapGhostSeq.erase(id); /* a rejoiner restarts its counter */
	m_mapGhostMoveState.erase(id);
	m_mapGhostHealth.erase(id); /* v12: gone = not alive for the respawn rule */
	ForgetPlayerName(id);       /* v13: "<name> left" + name table entry */
	if (mpVoice)
		mpVoice->DropPlayer(id); /* v16: decoder + AL source freed */
	std::map<uint8_t, cGhostPlayer *>::iterator it = m_mapGhosts.find(id);
	if (it != m_mapGhosts.end())
	{
		hplDelete(it->second);
		m_mapGhosts.erase(it);
	}
}

void cNetworkManager::EnsureGhost(uint8_t id)
{
	(void)id;
}

void cNetworkManager::RegisterInputActions()
{
}

void cNetworkManager::TryLoadMultiplayerCfg()
{
}

/* Rung 3 intent hooks: without multiplayer nothing is ever remote-driven. */
bool cNetworkManager::NetGrabBegin(hpl::iPhysicsBody *, bool, const hpl::cVector3f &, float) { return false; }
void cNetworkManager::NetGrabTarget(hpl::iPhysicsBody *, const hpl::cVector3f &) {}
void cNetworkManager::NetGrabEnd(hpl::iPhysicsBody *, const hpl::cVector3f &) {}
bool cNetworkManager::NetMoveBegin(hpl::iPhysicsBody *) { return false; }
void cNetworkManager::NetMoveForce(hpl::iPhysicsBody *, const hpl::cVector3f &, const hpl::cVector3f &, float) {}
void cNetworkManager::NetMoveStop(hpl::iPhysicsBody *) {}
void cNetworkManager::NetMoveEnd(hpl::iPhysicsBody *) {}

void cNetworkManager::GetLocalAddressLines(std::vector<hpl::tString> &avOut) const
{
	avOut.clear();
}

hpl::tString cNetworkManager::GetClipboardTextAscii()
{
	return "";
}

void cNetworkManager::NetOnLocalMapChange(const hpl::tString &, const hpl::tString &) {}
void cNetworkManager::NetOnItemPicked(const hpl::tString &) {}
void cNetworkManager::NetOnItemDropped(const hpl::tString &, const hpl::tString &,
	const hpl::cVector3f &, const hpl::cVector3f &) {}
int cNetworkManager::GetConnectedGuestCount() const { return 0; }
bool cNetworkManager::IsEnemyPuppetMode() const { return false; }
void cNetworkManager::GetGhostCamPositions(std::vector<std::pair<uint8_t, hpl::cVector3f> > &avOut) { avOut.clear(); }
bool cNetworkManager::GetGhostSense(uint8_t, hpl::cVector3f *, uint8_t *) const { return false; }
void cNetworkManager::SendPlayerDamage(uint8_t, float) {}
void cNetworkManager::NetOnEnemyDamaged(const hpl::tString &, float, int) {}
bool cNetworkManager::PartyHasItem(const hpl::tString &) const { return false; }
void cNetworkManager::NetOnScriptEvent(int, const hpl::tString &, int) {}
void cNetworkManager::NetOnEntityDamaged(const hpl::tString &, float, int) {}
/* v12 party health: nobody is ever connected in the stub build. */
bool cNetworkManager::GetGhostHealth(uint8_t, float *) const { return false; }
void cNetworkManager::GetPartyStatus(std::vector<cNetPartyMember> &avOut) const { avOut.clear(); }
bool cNetworkManager::IsSessionLive() const { return false; }

#else /* PENUMBRA_MULTIPLAYER */

#include <enet/enet.h>
#ifdef _WIN32
#include <iphlpapi.h> /* GetAdaptersInfo — per-subnet directed broadcast (Hamachi et al.) */
/* Sending UDP to a port nobody listens on makes Windows queue an ICMP
   port-unreachable that later recvfrom() reports as WSAECONNRESET — which a
   broadcast browser triggers constantly. This ioctl turns that behavior off. */
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#endif

struct cNetworkManager::Impl
{
	ENetHost *mpHost;
	ENetPeer *mpServerPeer;
	/* Raw UDP (not ENet — ENet sockets don't broadcast cleanly).
	   Listen: host side, bound 0.0.0.0:kNetDiscoveryPort, answers pings.
	   Browse: browser side, ephemeral port, sends pings / collects pongs. */
	SOCKET mDiscoveryListenSock;
	SOCKET mDiscoveryBrowseSock;
	bool mbBrowseHoldsNetRef; /* StartDiscovery can run with no ENet host alive */

	/* Master server address (internet browser). Resolved ONCE per distinct
	   cfg string by ResolveMasterAddress — blocking DNS belongs in a button
	   click / HostGame, never in Update. */
	sockaddr_in mMasterAddr;
	bool mbMasterResolved;
	hpl::tString msMasterResolvedFor; /* the host:port mMasterAddr stands for */
	bool mbMasterWarned;              /* log the "cannot resolve" line once */

	/* Rung 3, guest side: pending forwarded intent, flushed at the send tick.
	   Begin/End go out immediately (reliable); these are the streams. */
	uint32_t mlHeldHash;   /* forwarded grab in progress (0 = none) */
	hpl::cVector3f mvGrabTarget;
	bool mbHaveGrabTarget;
	uint32_t mlPushHash;   /* forwarded move/push in progress (0 = none) */
	hpl::cVector3f mvPushImpulse; /* sum of force*dt since the last send */
	hpl::cVector3f mvPushPoint;
	bool mbPushStop;
	bool mbPushAny;

	/* v15: per-connection guard record, HOST side, keyed by ENet peer slot
	   (created at CONNECT, erased at DISCONNECT / by the sweep). A peer has
	   no wire id until mbAuthed; a refused one is only waiting for its
	   disconnect to complete and everything it sends is dropped. */
	struct cPeerGuard
	{
		uint8_t mNonce[16];
		bool mbAuthed;
		bool mbRefused;
		float mfAge;            /* seconds since CONNECT (kNetAuthTimeoutSeconds) */
		unsigned mlStrikes;
		float mfStrikeDecay;    /* seconds since the last decay tick */
		float mfRateWindow;     /* seconds into the current 1 s window */
		unsigned mlReliableInWindow;
		bool mbRateStruck;      /* one strike per window */
		uint64_t mlLoggedTypes; /* log-once mask, bit = packet type */
		bool mbHavePos;         /* newest validated PlayerState position */
		hpl::cVector3f mvLastPos;

		cPeerGuard()
			: mbAuthed(false), mbRefused(false), mfAge(0), mlStrikes(0), mfStrikeDecay(0),
			  mfRateWindow(0), mlReliableInWindow(0), mbRateStruck(false), mlLoggedTypes(0),
			  mbHavePos(false), mvLastPos(0, 0, 0)
		{
			memset(mNonce, 0, sizeof(mNonce));
		}
	};
	std::map<const ENetPeer *, cPeerGuard> m_mapGuards;
	uint32_t mlNonceCounter;

	/* v15: discovery reflector limiter — the kPongBuckets most recent
	   source addresses, kMaxPongPerSource pongs each per 1 s window, and a
	   global cap per window (aged in PollDiscovery). */
	enum { kPongBuckets = 16, kMaxPongPerSource = 5, kMaxPongPerSecond = 60 };
	struct cPongBucket
	{
		uint32_t mlAddr;
		float mfWindowLeft;
		unsigned mlCount;
	};
	cPongBucket mPong[kPongBuckets];
	float mfPongGlobalWindow;
	unsigned mlPongGlobalCount;

	Impl()
		: mpHost(NULL)
		  , mpServerPeer(NULL)
		  , mDiscoveryListenSock(INVALID_SOCKET)
		  , mDiscoveryBrowseSock(INVALID_SOCKET)
		  , mbBrowseHoldsNetRef(false)
		  , mbMasterResolved(false)
		  , msMasterResolvedFor("")
		  , mbMasterWarned(false)
		  , mlHeldHash(0)
		  , mvGrabTarget(0, 0, 0)
		  , mbHaveGrabTarget(false)
		  , mlPushHash(0)
		  , mvPushImpulse(0, 0, 0)
		  , mvPushPoint(0, 0, 0)
		  , mbPushStop(false)
		  , mbPushAny(false)
		  , m_mapGuards()
		  , mlNonceCounter(0)
		  , mfPongGlobalWindow(0)
		  , mlPongGlobalCount(0)
	{
		memset(mPong, 0, sizeof(mPong));
		memset(&mMasterAddr, 0, sizeof(mMasterAddr));
	}

	void ResetIntent()
	{
		mlHeldHash = 0;
		mbHaveGrabTarget = false;
		mlPushHash = 0;
		mvPushImpulse = hpl::cVector3f(0, 0, 0);
		mbPushStop = false;
		mbPushAny = false;
	}
};

const float cNetworkManager::kSendPeriodSeconds = kNetSendPeriodSeconds; /* v10:
    20 -> 30 Hz — noticeably smoother object/enemy motion; ~18 KB/s peak
    is still nothing for any internet link. v11: the receiver clocks ghost
    interpolation off seq * period, so the value lives in NetworkPackets.h */
const float cNetworkManager::kDiscoveryWindowSeconds = 1.5f;

namespace
{
int gEnetUses = 0;

static bool NetAcquire()
{
	if (gEnetUses == 0 && enet_initialize() != 0)
	{
		Log(" multiplayer: enet_initialize failed\n");
		return false;
	}
	gEnetUses++;
	return true;
}

static void NetRelease()
{
	if (gEnetUses <= 0)
		return;
	gEnetUses--;
	if (gEnetUses == 0)
		enet_deinitialize();
}

static void PeerSetId(ENetPeer *p, uint8_t id)
{
	if (p)
		p->data = reinterpret_cast<void *>(static_cast<uintptr_t>(id));
}

static uint8_t PeerGetId(const ENetPeer *p)
{
	if (!p)
		return 0;
	return static_cast<uint8_t>(reinterpret_cast<uintptr_t>(p->data));
}

static bool SplitHostPort(const char *src, char *hostOut, size_t hostSz, uint16_t &outPort)
{
	if (!src || !hostOut || hostSz < 6)
		return false;
	char tmp[280];
	strncpy(tmp, src, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';

	const char *colon = strrchr(tmp, ':');
	if (colon && colon != tmp && colon[1])
	{
		size_t lh = static_cast<size_t>(colon - tmp);
		if (lh >= hostSz)
			lh = hostSz - 1;
		memcpy(hostOut, tmp, lh);
		hostOut[lh] = 0;
		int pv = atoi(colon + 1);
		outPort = (uint16_t)(pv > 0 && pv <= 65535 ? pv : 7777);
		return hostOut[0] != 0;
	}
	strncpy(hostOut, tmp, hostSz);
	hostOut[hostSz - 1] = 0;
	return hostOut[0] != 0;
}

static void TrimGhostModelToken(char *s)
{
	size_t len = strlen(s);
	size_t a = 0;
	while (a < len && (s[a] == ' ' || s[a] == '\t'))
		++a;
	size_t b = len;
	while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t'))
		--b;
	memmove(s, s + a, b - a);
	s[b - a] = '\0';
}

static void ParseCsvGhostModels(const char *src, std::vector<hpl::tString> &out)
{
	out.clear();
	if (!src || !src[0])
		return;
	char tmp[384];
	strncpy(tmp, src, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';

	char *p = tmp;
	for (;;)
	{
		char *comma = strchr(p, ',');
		if (comma)
			*comma = '\0';

		TrimGhostModelToken(p);
		if (*p)
			out.push_back(hpl::tString(p));

		if (!comma)
			break;
		p = comma + 1;
	}
}

static void ParseCsvFloats(const char *src, std::vector<float> &out)
{
	out.clear();
	if (!src || !src[0])
		return;
	char tmp[384];
	strncpy(tmp, src, sizeof(tmp) - 1);
	tmp[sizeof(tmp) - 1] = '\0';

	char *p = tmp;
	for (;;)
	{
		char *comma = strchr(p, ',');
		if (comma)
			*comma = '\0';
		TrimGhostModelToken(p);
		if (*p)
			out.push_back((float)atof(p));
		if (!comma)
			break;
		p = comma + 1;
	}
}

/** Reliable = channel 0 (control), else channel 1 unsequenced (streams). */
static void SendStructToPeer(ENetPeer *apPeer, const void *apData, size_t alLen, bool abReliable)
{
	if (!apPeer || apPeer->state != ENET_PEER_STATE_CONNECTED)
		return;
	ENetPacket *pk = enet_packet_create(apData, alLen,
		abReliable ? ENET_PACKET_FLAG_RELIABLE : ENET_PACKET_FLAG_UNSEQUENCED);
	if (pk)
		enet_peer_send(apPeer, abReliable ? 0 : 1, pk);
}

static bool SendPlayerJoin(ENetPeer *peer, uint8_t assignedId)
{
	cNetPlayerJoin j;
	j.mType = eNetPacketType_PlayerJoin;
	j.mPlayerID = assignedId;
	ENetPacket *pk = enet_packet_create(&j, sizeof(j), ENET_PACKET_FLAG_RELIABLE);
	if (!pk)
		return false;
	enet_peer_send(peer, 0, pk);
	return true;
}

static void BlastLeaves(ENetHost *host, uint8_t who, ENetPeer *skip)
{
	cNetPlayerLeave lv;
	lv.mType = eNetPacketType_PlayerLeave;
	lv.mPlayerID = who;
	for (size_t i = 0; i < host->peerCount; ++i)
	{
		ENetPeer *rp = &host->peers[i];
		if (rp == skip || rp->state != ENET_PEER_STATE_CONNECTED || PeerGetId(rp) == 0)
			continue; /* v15: id 0 = still answering the challenge, gets nothing */
		ENetPacket *pkg = enet_packet_create(&lv, sizeof(lv), ENET_PACKET_FLAG_RELIABLE);
		if (pkg)
			enet_peer_send(rp, 0, pkg);
	}
}

/** Non-blocking + no WSAECONNRESET surprises; returns false if setup failed. */
static bool TameUdpSocket(SOCKET s)
{
	if (s == INVALID_SOCKET)
		return false;
	u_long nb = 1;
	if (ioctlsocket(s, FIONBIO, &nb) != 0)
		return false;
	BOOL noReset = FALSE;
	DWORD got = 0;
	WSAIoctl(s, SIO_UDP_CONNRESET, &noReset, sizeof(noReset), NULL, 0, &got, NULL, NULL);
	return true;
}

static void CloseUdpSocket(SOCKET &s)
{
	if (s != INVALID_SOCKET)
	{
		closesocket(s);
		s = INVALID_SOCKET;
	}
}

/** "a.b.c.d:port" for log lines and cDiscoveredServer::msAddress. */
static void FormatAddrPort(const sockaddr_in &sin, uint16_t port, char *out, size_t outSz)
{
	const unsigned char *b = (const unsigned char *)&sin.sin_addr;
	_snprintf(out, outSz, "%u.%u.%u.%u:%u",
			  (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3], (unsigned)port);
	out[outSz - 1] = '\0';
}

static void CopyPacketString(char *dst, size_t dstSz, const char *src)
{
	strncpy(dst, src ? src : "", dstSz);
	dst[dstSz - 1] = '\0';
}

/** v15: "a.b.c.d:port" of an ENet peer, for log lines. */
static void FormatPeerAddr(const ENetPeer *apPeer, char *out, size_t outSz)
{
	if (!out || outSz == 0)
		return;
	if (!apPeer)
	{
		out[0] = '\0';
		return;
	}
	const unsigned char *b = (const unsigned char *)&apPeer->address.host;
	_snprintf(out, outSz, "%u.%u.%u.%u:%u",
			  (unsigned)b[0], (unsigned)b[1], (unsigned)b[2], (unsigned)b[3],
			  (unsigned)apPeer->address.port);
	out[outSz - 1] = '\0';
}
}

//-----------------------------------------------------------------------

cNetworkManager::cNetworkManager(cInit *apInit)
	: mpInit(apInit)
	  , mbHosting(false)
	  , mbClientConnected(false)
	  , mbHadJoinPacket(false)
	  , mbSpawnedAtHost(false)
	  , mbGotVersionAck(false)
	  , mlStateSeqOut(0)
	  , mlEnemySeqOut(0)
	  , mlEnemySeqIn(0)
	  , mbEnemySeqInKnown(false)
	  , mbApplyingRemoteMapChange(false)
	  , mbLocalMapChangeArmed(false)
	  , mbHavePendingMapChange(false)
	  , mfSendAccum(0)
	  , mpWorld(NULL)
	  , mlLocalPlayerId(0)
	  , mlListenPort(7777)
	  , mlNextGuestId(2)
	  , mlDefaultPort(7777)
	  , mbActionsRegistered(false)
	  , mpImpl(new Impl())
	  , mvGhostMeshPaths()
	  , mfGhostMeshBodyYOffset(9999.0f)  /* AUTO: derived from game.cfg Player Height/CameraHeightAdd */
	  , mfGhostMeshBodyYOffsetCrouch(9999.0f) /* AUTO: derived from CrouchHeight/CameraHeightAdd */
	  , mvGhostBodyYList()
	  , mvGhostBodyYCrouchList()
	  , mGhostTuning()
	  , mbGhostPreview(false)
	  , mlGhostPreviewModel(0)
	  , mpPreviewGhost(NULL)
	  , mlPreviewSeq(0)
	  , mfPreviewSendAccum(0)
	  , mfPreviewSpawnDelay(0.75f)
	  , mvPreviewCenter(0, 0, 0)
	  , mfPreviewFacingYaw(0)
	  , mfPreviewCircleAngle(0)
	  , mlPreviewClipIdx(-1)
	  , mbPreviewCrouch(false)
	  , mlPreviewTreadmill(0)
	  , mvDiscovered()
	  , mbDiscoveryActive(false)
	  , mfDiscoveryTimeLeft(0)
	  , msServerName("") /* v13: empty = "<player_name>'s game" in the pong */
	  , mlMaxPlayers(4)
	  , msMasterServer("")
	  , mbPublic(false)
	  , mbPublicExplicit(false)
	  , mvInternet()
	  , mbInternetActive(false)
	  , mfInternetTimeLeft(0)
	  , msInternetFailReason("")
	  , mfMasterRegisterAccum(0)
	  , mbMasterRegistered(false)
	  , mpBodySync(new cBodySync())
	  , mbCoopRespawn(true) /* v12: multiplayer.cfg coop_respawn */
	  , msPlayerName()      /* v13: multiplayer.cfg player_name */
	  , m_mapPlayerNames()
	  , mvPartyEvents()
	  , m_setJoinAnnounced()
	  , mfSinceJoinSeconds(0)
	  , mvSnapChunks()      /* v14: world snapshot */
	  , mlSnapId(0)
	  , mlSnapGen(0)
	  , mbSnapBuffering(false)
	  , mfSnapAge(0)
	  , mlSnapIdOut(0)
	  , msServerPassword()  /* v15: multiplayer.cfg server_password ("" = open) */
	  , msJoinPassword()    /* v15: multiplayer.cfg join_password */
	  , mbAuthSent(false)
	  , mlGuestViolationsLogged(0)
	  , mvFreeGuestIds()
	  , mpVoice(NULL)       /* v16: voice chat (created in Startup) */
	  , mbVoiceEnabled(true)
	  , mbVoiceOpenMic(false)
	  , mfVoiceVolume(1.0f)
{
	/* v9: listen to the engine's script-var writes for replication (the
	   stub build registers too — its NetOnScriptEvent is a no-op) */
	gpNetMgrForScript = this;
	hpl::gpScriptVarNetCallback = ScriptVarNetThunk;
}

cNetworkManager::~cNetworkManager()
{
	hpl::gpScriptVarNetCallback = NULL;
	gpNetMgrForScript = NULL;
	Disconnect(); /* v16: also shuts the voice chat down (AL context still alive: cGame dies later) */
	ClearGhostsInternal();
	if (mpVoice)
		hplDelete(mpVoice);
	mpVoice = NULL;
	delete mpBodySync;
	mpBodySync = NULL;
	delete mpImpl;
	mpImpl = NULL;
}

//-----------------------------------------------------------------------

void cNetworkManager::RegisterInputActions()
{
	if (mbActionsRegistered || !mpInit || !mpInit->mpGame)
		return;
	mbActionsRegistered = true;
	hpl::cInput *inp = mpInit->mpGame->GetInput();
	inp->AddAction(hplNew(cActionKeyboard, ("MultiplayerHost", inp, eKey_F11)));
	inp->AddAction(hplNew(cActionKeyboard, ("MultiplayerJoinLocal", inp, eKey_F10)));
	inp->AddAction(hplNew(cActionKeyboard, ("MultiplayerDiscover", inp, eKey_F9)));
	/* Ghost preview keys (only polled with ghost_preview=1). F1/F4/F5/F12 are
	   the game's, F9-F11 ours; F2/F6/F7/F8 are free. */
	inp->AddAction(hplNew(cActionKeyboard, ("GhostPreviewNext", inp, eKey_F6)));
	inp->AddAction(hplNew(cActionKeyboard, ("GhostPreviewPrev", inp, eKey_F7)));
	inp->AddAction(hplNew(cActionKeyboard, ("GhostPreviewCrouch", inp, eKey_F8)));
	inp->AddAction(hplNew(cActionKeyboard, ("GhostPreviewTreadmill", inp, eKey_F2)));
	/* v16: push-to-talk. V is unbound in the vanilla key map. */
	inp->AddAction(hplNew(cActionKeyboard, ("VoiceTalk", inp, eKey_v)));
	Log(" multiplayer: F11=toggle HOST :%u — F10=JOIN 127.0.0.1:%u — F9=LAN discovery — V=push-to-talk — menu Multiplayer · multiplayer.cfg\n",
		(unsigned)mlDefaultPort, (unsigned)mlDefaultPort);
}

//-----------------------------------------------------------------------
/* multiplayer.cfg example:
   host=1
   port=7777
   join=127.0.0.1:7777
   server_name=Nick's bunker      (spaces allowed; shown in the server browser)
   max_players=4                  (advertised in discovery; 2..31)
   ghost_models=survivor1.dae,survivor2.dae
   ghost_body_y=-1.65
   Optional single mesh replaces the list:
   ghost_model=mine_barrel.dae
   Internet server browser (README 'Public servers'):
   master_server=1.2.3.4:7779    (host:port of a master_server.py; implies public=1)
   public=0                      (1 = register with the master while hosting)
   server_password=secret        (guests must know it; listed with [pw])
   join_password=secret          (presented on join; the browser prompts too)

   Peer PlayerID picks mesh as index (id-1) modulo list length — id 1 => first .dae, id 2 => second.
*/
void cNetworkManager::TryLoadMultiplayerCfg()
{
	FILE *fp = fopen("multiplayer.cfg", "r");
	if (!fp)
		return;

	char buf[384];
	bool wantHost = false;
	while (fgets(buf, sizeof(buf), fp))
	{
		if (buf[0] == '#' || buf[0] == ';' || buf[0] == '\n' || buf[0] == '\r')
			continue;

		/* server_name may contain spaces, which %255s would cut — take the raw
		   remainder of the line instead of going through sscanf. */
		char rawKey[64];
		rawKey[0] = '\0';
		const bool bHaveKey = (sscanf(buf, " %63[^=]", rawKey) == 1);
		const bool bServerName = bHaveKey && strcmp(rawKey, "server_name") == 0;
		const bool bPlayerName = bHaveKey && strcmp(rawKey, "player_name") == 0; /* v13 */
		const bool bServerPw = bHaveKey && strcmp(rawKey, "server_password") == 0; /* v15 */
		const bool bJoinPw = bHaveKey && strcmp(rawKey, "join_password") == 0;     /* v15 */
		if (bServerName || bPlayerName || bServerPw || bJoinPw)
		{
			char *eq = strchr(buf, '=');
			size_t ln = 0;
			char *nm = buf;
			if (eq)
			{
				nm = eq + 1;
				while (*nm == ' ' || *nm == '\t')
					++nm;
				ln = strlen(nm);
				while (ln > 0 && (nm[ln - 1] == '\n' || nm[ln - 1] == '\r' ||
								  nm[ln - 1] == ' ' || nm[ln - 1] == '\t'))
					--ln;
			}
			if (bPlayerName)
				msPlayerName = SanitizePlayerName(hpl::tString(nm, ln)); /* "" = ask in the menu */
			else if (bServerPw)
				SetServerPassword(hpl::tString(nm, ln)); /* v15: "" = open server */
			else if (bJoinPw)
				SetJoinPassword(hpl::tString(nm, ln));   /* v15: sent on JoinGame */
			else
			{
				if (ln > 31)
					ln = 31; /* wire field is char[32] */
				msServerName = hpl::tString(nm, ln); /* "" = "<player_name>'s game" */
			}
			continue;
		}

		char key[64], val[256];
		if (sscanf(buf, " %63[^=]=%255s", key, val) != 2)
			continue;
		if (strcmp(key, "host") == 0 && atoi(val) != 0)
			wantHost = true;
		else if (strcmp(key, "port") == 0)
		{
			int p = atoi(val);
			if (p > 0 && p < 65536)
				mlDefaultPort = (uint16_t)p;
		}
		else if (strcmp(key, "join") == 0)
			msDeferredJoinAddress = val;
		else if (strcmp(key, "ghost_models") == 0)
			ParseCsvGhostModels(val, mvGhostMeshPaths);
		else if (strcmp(key, "ghost_model") == 0)
		{
			mvGhostMeshPaths.clear();
			mvGhostMeshPaths.push_back(val);
		}
		else if (strcmp(key, "ghost_body_y") == 0)
			mfGhostMeshBodyYOffset = static_cast<float>(atof(val));
		else if (strcmp(key, "ghost_body_y_crouch") == 0)
			mfGhostMeshBodyYOffsetCrouch = static_cast<float>(atof(val));
		else if (strcmp(key, "ghost_body_ys") == 0)
			ParseCsvFloats(val, mvGhostBodyYList);
		else if (strcmp(key, "ghost_body_ys_crouch") == 0)
			ParseCsvFloats(val, mvGhostBodyYCrouchList);
		else if (strcmp(key, "ghost_preview") == 0)
			mbGhostPreview = (atoi(val) != 0);
		else if (strcmp(key, "ghost_preview_model") == 0)
		{
			const int m = atoi(val);
			mlGhostPreviewModel = (m < 0) ? 0 : m;
		}
		else if (strcmp(key, "ghost_anim_trace") == 0)
			mGhostTuning.mbAnimTrace = (atoi(val) != 0);
		else if (strcmp(key, "ghost_interp_ms") == 0)
		{
			float ms = static_cast<float>(atof(val));
			if (ms < 0.0f) ms = 0.0f;
			else if (ms > 250.0f) ms = 250.0f;
			mGhostTuning.mfInterpDelaySec = ms / 1000.0f;
		}
		else if (strcmp(key, "ghost_turn_rate") == 0)
		{
			float deg = static_cast<float>(atof(val));
			if (deg < 30.0f) deg = 30.0f;
			else if (deg > 3600.0f) deg = 3600.0f;
			mGhostTuning.mfTurnRateRadPerSec = deg * (kPif / 180.0f);
		}
		else if (strncmp(key, "ghost_gait_", 11) == 0)
		{
			/* ghost_gait_<clip>=m/s at playback speed 1.0. The six slot names
			   (walk, run, crouch_walk, walk_back, strafe_walk, strafe_run) set
			   the clip(s) of that slot; an exact clip name (strafe_walk_l)
			   overrides one clip. */
			const char *name = key + 11;
			const float g = static_cast<float>(atof(val));
			if (name[0] && g > 0.05f && g < 10.0f)
			{
				if (strcmp(name, "strafe_walk") == 0)
				{
					mGhostTuning.m_mapGaitOverrides["strafe_walk_l"] = g;
					mGhostTuning.m_mapGaitOverrides["strafe_walk_r"] = g;
				}
				else if (strcmp(name, "strafe_run") == 0)
				{
					mGhostTuning.m_mapGaitOverrides["strafe_run_l"] = g;
					mGhostTuning.m_mapGaitOverrides["strafe_run_r"] = g;
				}
				else
					mGhostTuning.m_mapGaitOverrides[hpl::tString(name)] = g;
			}
		}
		else if (strcmp(key, "coop_respawn") == 0)
			mbCoopRespawn = (atoi(val) != 0); /* v12: 0 = vanilla death menu online too */
		else if (strcmp(key, "max_players") == 0)
		{
			int mp = atoi(val);
			if (mp < 2)
				mp = 2;
			if (mp > 31) /* enet_host_create is sized for 31 peers */
				mp = 31;
			mlMaxPlayers = (uint8_t)mp;
		}
		/* Internet server browser (master server) — README 'Public servers'. */
		else if (strcmp(key, "master_server") == 0)
		{
			msMasterServer = val;
			if (!mbPublicExplicit)
				mbPublic = true; /* naming a master is opting in, unless public=0 says otherwise */
		}
		else if (strcmp(key, "public") == 0)
		{
			mbPublic = atoi(val) != 0;
			mbPublicExplicit = true;
		}
		/* server_password= / join_password= are raw-line keys handled above (v15) */
		/* v16 voice chat */
		else if (strcmp(key, "voice_enabled") == 0)
			mbVoiceEnabled = (atoi(val) != 0);
		else if (strcmp(key, "voice_open_mic") == 0)
			mbVoiceOpenMic = (atoi(val) != 0);
		else if (strcmp(key, "voice_volume") == 0)
		{
			float v = static_cast<float>(atof(val));
			if (v < 0.0f) v = 0.0f;
			else if (v > 2.0f) v = 2.0f;
			mfVoiceVolume = v;
		}
	}
	fclose(fp);

	if (wantHost)
	{
		msDeferredJoinAddress = "";
		HostGame(mlDefaultPort);
	}
}

void cNetworkManager::Startup()
{
	RegisterInputActions();
	TryLoadMultiplayerCfg();
	/* No cfg (fresh install from the zip): default to the SHIPPED models —
	   otherwise remote players have no mesh at all and render as nothing but
	   their marker light. Offsets stay AUTO: the 2026-07-18 exports have
	   their origin exactly at the feet. */
	if (mvGhostMeshPaths.empty())
	{
		mvGhostMeshPaths.push_back("malik.dae");
		mvGhostMeshPaths.push_back("phillip.dae");
	}
	/* Relative to cwd (folder with overture.exe). Drop survivor1.dae / survivor2.dae here — see multiplayer.cfg.example. */
	if (mpInit && mpInit->mpGame && mpInit->mpGame->GetResources())
		mpInit->mpGame->GetResources()->AddResourceDir("multiplayer/models");
	/* Remote grabs use the same feel constants as the local grab state. */
	if (mpInit && mpInit->mpGameConfig)
		mpBodySync->SetGrabTuning(
			mpInit->mpGameConfig->GetFloat("Interaction_Grab", "MaxPidForce", 80.0f),
			mpInit->mpGameConfig->GetFloat("Interaction_Grab", "MaxThrowImpulse", 13.0f));
	/* v16: the voice object is cheap (no Opus/AL until a session is live —
	   UpdateVoice). voice_enabled=0 or a build without opus: no object. */
	if (mpVoice == NULL && mbVoiceEnabled && cVoiceChat::IsCompiledIn())
	{
		mpVoice = hplNew(cVoiceChat, ());
		mpVoice->SetEnabled(true);
		mpVoice->SetOpenMic(mbVoiceOpenMic);
		mpVoice->SetVolume(mfVoiceVolume);
		Log(" multiplayer: voice chat on (hold V to talk%s, voice_volume=%.2f)\n",
			mbVoiceOpenMic ? " — voice_open_mic=1: level-gated open mic" : "", mfVoiceVolume);
	}
	else if (cVoiceChat::IsCompiledIn())
		Log(" multiplayer: voice chat off (voice_enabled=0)\n");
}

//-----------------------------------------------------------------------
// Rung 3 — forwarded intent, called from the player interaction states.
// Guest + replicated body -> forward and return true (caller goes hands-off);
// host -> bookkeeping only (its sim IS the truth); offline -> plain false.
//-----------------------------------------------------------------------

bool cNetworkManager::NetGrabBegin(hpl::iPhysicsBody *apBody, bool abPickAtPoint,
	const hpl::cVector3f &avRelPick, float afMassMul)
{
	uint32_t lHash;
	if (!apBody || !mpBodySync->GetHashForBody(apBody, &lHash))
		return false;

	if (mbHosting)
	{
		/* The host grabs with its normal local physics; only the holder book
		   changes. LAST GRAB WINS: evict a guest that was holding it. */
		uint8_t lPrev = mpBodySync->SetHolder(lHash, 1);
		if (lPrev >= 2)
		{
			mpBodySync->RemoteGrabEnd(lHash, lPrev, cVector3f(0, 0, 0));
			SendGrabDeny(lPrev, lHash);
			Log(" multiplayer: snatched '%s' from guest %u\n",
				apBody->GetName().c_str(), (unsigned)lPrev);
		}
		return false;
	}

	if (!mbClientConnected || !mbHadJoinPacket || !mpImpl->mpServerPeer)
		return false;

	cNetBodyGrabBegin pkt;
	pkt.mType = eNetPacketType_BodyGrabBegin;
	pkt.mFlags = abPickAtPoint ? kNetGrabFlag_PickAtPoint : 0;
	pkt.mlNameHash = lHash;
	pkt.mfRelX = avRelPick.x; pkt.mfRelY = avRelPick.y; pkt.mfRelZ = avRelPick.z;
	pkt.mfMassMul = afMassMul;
	SendStructToPeer(mpImpl->mpServerPeer, &pkt, sizeof(pkt), true);

	mpImpl->mlHeldHash = lHash;
	mpImpl->mbHaveGrabTarget = false;
	/* high-ping feel: our copy of a free-grab tracks OUR crosshair locally */
	mpBodySync->SetGuestHeld(lHash, abPickAtPoint, avRelPick);
	return true;
}

void cNetworkManager::NetGrabTarget(hpl::iPhysicsBody *apBody, const hpl::cVector3f &avTarget)
{
	uint32_t lHash;
	if (mbHosting || !apBody || !mpBodySync->GetHashForBody(apBody, &lHash))
		return;
	if (mpImpl->mlHeldHash != lHash)
		return;
	mpImpl->mvGrabTarget = avTarget; /* latest wins; flushed at the send tick */
	mpImpl->mbHaveGrabTarget = true;
	mpBodySync->UpdateGuestHeldTarget(avTarget); /* local prediction target */
}

void cNetworkManager::NetGrabEnd(hpl::iPhysicsBody *apBody, const hpl::cVector3f &avImpulse)
{
	uint32_t lHash;
	if (!apBody || !mpBodySync->GetHashForBody(apBody, &lHash))
		return;

	if (mbHosting)
	{
		if (mpBodySync->GetHolder(lHash) == 1)
			mpBodySync->ClearHolder(lHash);
		return;
	}

	if (mpImpl->mlHeldHash != lHash)
		return; /* already ended (throw path ends before LeaveState runs) */
	mpImpl->mlHeldHash = 0;
	mpImpl->mbHaveGrabTarget = false;
	mpBodySync->ClearGuestHeld(); /* host states own the body again */

	if (!mbClientConnected || !mpImpl->mpServerPeer)
		return;
	cNetBodyGrabEnd pkt;
	pkt.mType = eNetPacketType_BodyGrabEnd;
	pkt.mlNameHash = lHash;
	pkt.mfImpX = avImpulse.x; pkt.mfImpY = avImpulse.y; pkt.mfImpZ = avImpulse.z;
	SendStructToPeer(mpImpl->mpServerPeer, &pkt, sizeof(pkt), true);
}

bool cNetworkManager::NetMoveBegin(hpl::iPhysicsBody *apBody)
{
	uint32_t lHash;
	if (mbHosting || !apBody || !mpBodySync->GetHashForBody(apBody, &lHash))
		return false;
	if (!mbClientConnected || !mbHadJoinPacket || !mpImpl->mpServerPeer)
		return false;
	/* Stateless on the wire: nothing to announce, just start accumulating. */
	mpImpl->mlPushHash = lHash;
	mpImpl->mvPushImpulse = cVector3f(0, 0, 0);
	mpImpl->mbPushAny = false;
	mpImpl->mbPushStop = false;
	return true;
}

void cNetworkManager::NetMoveForce(hpl::iPhysicsBody *apBody, const hpl::cVector3f &avForce,
	const hpl::cVector3f &avPoint, float afTimeStep)
{
	uint32_t lHash;
	if (mbHosting || !apBody || !mpBodySync->GetHashForBody(apBody, &lHash))
		return;
	if (mpImpl->mlPushHash != lHash)
		return;
	mpImpl->mvPushImpulse += avForce * afTimeStep; /* force integrated -> impulse */
	mpImpl->mvPushPoint = avPoint;
	mpImpl->mbPushAny = true;
}

void cNetworkManager::NetMoveStop(hpl::iPhysicsBody *apBody)
{
	uint32_t lHash;
	if (mbHosting || !apBody || !mpBodySync->GetHashForBody(apBody, &lHash))
		return;
	if (mpImpl->mlPushHash == lHash)
		mpImpl->mbPushStop = true;
}

void cNetworkManager::NetMoveEnd(hpl::iPhysicsBody *apBody)
{
	uint32_t lHash;
	if (mbHosting || !apBody || !mpBodySync->GetHashForBody(apBody, &lHash))
		return;
	if (mpImpl->mlPushHash != lHash)
		return;
	FlushIntentPackets(); /* don't drop the final shove */
	mpImpl->mlPushHash = 0;
	mpImpl->mbPushAny = false;
	mpImpl->mbPushStop = false;
}

void cNetworkManager::FlushIntentPackets()
{
	if (mbHosting || !mbClientConnected || !mpImpl->mpServerPeer)
		return;

	if (mpImpl->mbHaveGrabTarget && mpImpl->mlHeldHash != 0)
	{
		cNetBodyGrabTarget pkt;
		pkt.mType = eNetPacketType_BodyGrabTarget;
		pkt.mlNameHash = mpImpl->mlHeldHash;
		pkt.mfX = mpImpl->mvGrabTarget.x;
		pkt.mfY = mpImpl->mvGrabTarget.y;
		pkt.mfZ = mpImpl->mvGrabTarget.z;
		SendStructToPeer(mpImpl->mpServerPeer, &pkt, sizeof(pkt), false);
		mpImpl->mbHaveGrabTarget = false;
	}

	if (mpImpl->mlPushHash != 0 && (mpImpl->mbPushAny || mpImpl->mbPushStop))
	{
		cNetBodyPush pkt;
		pkt.mType = eNetPacketType_BodyPush;
		pkt.mFlags = mpImpl->mbPushStop ? kNetPushFlag_Stop : 0;
		pkt.mlNameHash = mpImpl->mlPushHash;
		pkt.mfImpX = mpImpl->mvPushImpulse.x;
		pkt.mfImpY = mpImpl->mvPushImpulse.y;
		pkt.mfImpZ = mpImpl->mvPushImpulse.z;
		pkt.mfPtX = mpImpl->mvPushPoint.x;
		pkt.mfPtY = mpImpl->mvPushPoint.y;
		pkt.mfPtZ = mpImpl->mvPushPoint.z;
		SendStructToPeer(mpImpl->mpServerPeer, &pkt, sizeof(pkt), false);
		mpImpl->mvPushImpulse = cVector3f(0, 0, 0);
		mpImpl->mbPushAny = false;
		mpImpl->mbPushStop = false;
	}
}

void cNetworkManager::SendGrabDeny(uint8_t alPeerId, uint32_t alHash)
{
	if (!mpImpl || !mpImpl->mpHost)
		return;
	cNetBodyGrabDeny pkt;
	pkt.mType = eNetPacketType_BodyGrabDeny;
	pkt.mlNameHash = alHash;
	for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
	{
		ENetPeer *pd = &mpImpl->mpHost->peers[i];
		if (PeerGetId(pd) == alPeerId)
		{
			SendStructToPeer(pd, &pkt, sizeof(pkt), true);
			return;
		}
	}
}

void cNetworkManager::ForceReleaseIfHolding(uint32_t alHash)
{
	if (!mpInit || !mpInit->mpPlayer)
		return;
	cPlayer *pPlayer = mpInit->mpPlayer;
	const ePlayerState state = pPlayer->GetState();
	if (state != ePlayerState_Grab && state != ePlayerState_Move &&
		state != ePlayerState_Push)
		return;
	uint32_t lHeld;
	iPhysicsBody *pHeld = pPlayer->GetPushBody();
	if (!pHeld || !mpBodySync->GetHashForBody(pHeld, &lHeld) || lHeld != alHash)
		return;
	pPlayer->ChangeState(ePlayerState_Normal);
	Log(" multiplayer: '%s' snatched out of our hands\n", pHeld->GetName().c_str());
}

void cNetworkManager::HandleBodyIntent(uint8_t alAuthor, const void *apData, size_t alLen)
{
	if (alAuthor < 2 || !apData || alLen < 1)
		return;
	const uint8_t lType = *(const uint8_t *)apData;

	if (lType == eNetPacketType_BodyGrabBegin && alLen >= sizeof(cNetBodyGrabBegin))
	{
		cNetBodyGrabBegin pkt;
		memcpy(&pkt, apData, sizeof(pkt));

		const uint8_t lPrev = mpBodySync->SetHolder(pkt.mlNameHash, alAuthor);
		if (lPrev == 1)
			ForceReleaseIfHolding(pkt.mlNameHash); /* guest snatches from the host */
		else if (lPrev >= 2 && lPrev != alAuthor)
		{
			mpBodySync->RemoteGrabEnd(pkt.mlNameHash, lPrev, cVector3f(0, 0, 0));
			SendGrabDeny(lPrev, pkt.mlNameHash);
		}
		mpBodySync->RemoteGrabBegin(pkt.mlNameHash, alAuthor,
			(pkt.mFlags & kNetGrabFlag_PickAtPoint) != 0,
			cVector3f(pkt.mfRelX, pkt.mfRelY, pkt.mfRelZ), pkt.mfMassMul);
		return;
	}

	if (lType == eNetPacketType_BodyGrabTarget && alLen >= sizeof(cNetBodyGrabTarget))
	{
		cNetBodyGrabTarget pkt;
		memcpy(&pkt, apData, sizeof(pkt));
		mpBodySync->RemoteGrabTarget(pkt.mlNameHash, alAuthor,
			cVector3f(pkt.mfX, pkt.mfY, pkt.mfZ));
		return;
	}

	if (lType == eNetPacketType_BodyGrabEnd && alLen >= sizeof(cNetBodyGrabEnd))
	{
		cNetBodyGrabEnd pkt;
		memcpy(&pkt, apData, sizeof(pkt));
		mpBodySync->RemoteGrabEnd(pkt.mlNameHash, alAuthor,
			cVector3f(pkt.mfImpX, pkt.mfImpY, pkt.mfImpZ));
		return;
	}

	if (lType == eNetPacketType_BodyPush && alLen >= sizeof(cNetBodyPush))
	{
		cNetBodyPush pkt;
		memcpy(&pkt, apData, sizeof(pkt));
		mpBodySync->RemotePush(pkt.mlNameHash,
			cVector3f(pkt.mfImpX, pkt.mfImpY, pkt.mfImpZ),
			cVector3f(pkt.mfPtX, pkt.mfPtY, pkt.mfPtZ),
			(pkt.mFlags & kNetPushFlag_Stop) != 0);
		return;
	}
}

//-----------------------------------------------------------------------

void cNetworkManager::ClearGhostsInternal()
{
	m_mapGhostSeq.clear(); /* new session, new counters */
	m_mapGhostMoveState.clear();
	m_mapGhostHealth.clear();
	m_mapPlayerNames.clear(); /* v13: names + "joined" bookkeeping are per session */
	m_setJoinAnnounced.clear();
	if (mpVoice)
		mpVoice->DropAllPlayers(); /* v16: every remote decoder + AL source */
	for (std::map<uint8_t, cGhostPlayer *>::iterator it = m_mapGhosts.begin(); it != m_mapGhosts.end(); ++it)
		hplDelete(it->second);
	m_mapGhosts.clear();
	DestroyPreviewGhost(false); /* respawns on the next Update if still enabled */
}

void cNetworkManager::DropRemotePlayer(uint8_t id)
{
	m_mapGhostSeq.erase(id); /* a rejoiner restarts its counter */
	m_mapGhostMoveState.erase(id);
	m_mapGhostHealth.erase(id); /* v12: gone = not alive for the respawn rule */
	ForgetPlayerName(id);       /* v13: "<name> left" + name table entry */
	if (mpVoice)
		mpVoice->DropPlayer(id); /* v16: decoder + AL source freed */
	std::map<uint8_t, cGhostPlayer *>::iterator it = m_mapGhosts.find(id);
	if (it != m_mapGhosts.end())
	{
		hplDelete(it->second);
		m_mapGhosts.erase(it);
	}
}

void cNetworkManager::EnsureGhost(uint8_t id)
{
	if (!mpWorld || id == 0 || id == mlLocalPlayerId)
		return;
	if (m_mapGhosts.find(id) != m_mapGhosts.end())
		return;
	/* PlayerID picks the mesh as index (id-1) modulo the list length —
	   id 1 => first .dae, id 2 => second (same order on every machine). */
	cGhostPlayer *pGhost = CreateGhost(id, (size_t)(id - 1));
	if (pGhost)
		m_mapGhosts[id] = pGhost;
}

void cNetworkManager::DispatchIncoming(const void *data, size_t len)
{
	if (!data || len < sizeof(cNetPlayerJoin))
		return;
	uint8_t t = *(const uint8_t *)data;

	if (t == eNetPacketType_Challenge)
	{
		/* v15, guest side: the host's FIRST packet — answer it (once) */
		if (!mbHosting && len >= sizeof(cNetChallenge))
		{
			cNetChallenge ch;
			memcpy(&ch, data, sizeof(ch));
			SendAuthResponse(ch);
		}
		return;
	}
	if (t == eNetPacketType_Auth)
		return; /* v15: host-only, consumed in Service before any dispatch */

	if (t == eNetPacketType_VersionAck && len >= sizeof(cNetVersionAck))
	{
		cNetVersionAck ack;
		memcpy(&ack, data, sizeof(ack));
		if (ack.mlVersion == kNetProtocolVersion)
			mbGotVersionAck = true;
		else
		{
			msJoinFailReason = "Host runs a DIFFERENT VERSION of the mod - you both need the same zip.";
			Log(" multiplayer: version mismatch: host v%u, we are v%u\n",
				(unsigned)ack.mlVersion, (unsigned)kNetProtocolVersion);
			if (mpImpl && mpImpl->mpServerPeer)
				enet_peer_disconnect(mpImpl->mpServerPeer, 0);
		}
		return;
	}

	if (t == eNetPacketType_PlayerJoin && len >= sizeof(cNetPlayerJoin))
	{
		/* An OLD host (protocol <= 6) never sends VersionAck — its first
		   reliable packet is this join. Refuse: half-working is worse than
		   not connecting (v3 host + v6 guest = garbage physics, no items). */
		if (!mbHosting && !mbGotVersionAck)
		{
			msJoinFailReason = "Host runs an OLDER VERSION of the mod - send them the current zip.";
			Log(" multiplayer: host is an old build (join before version ack) - refusing\n");
			if (mpImpl && mpImpl->mpServerPeer)
				enet_peer_disconnect(mpImpl->mpServerPeer, 0);
			return;
		}
		const cNetPlayerJoin *pj = (const cNetPlayerJoin *)data;
		mlLocalPlayerId = pj->mPlayerID;
		mbHadJoinPacket = true;
		Log(" multiplayer: local PlayerID=%u\n", (unsigned)mlLocalPlayerId);
		mfSinceJoinSeconds = 0; /* v13: the table that follows is the existing party */
		SendLocalName();        /* v13: we know our id now — tell the host who we are */
		return;
	}

	if (t == eNetPacketType_PlayerName)
	{
		/* v13, guest side: one entry of the host's table. (The host takes a
		   guest's name in Service with the PEER's id, never from here.) */
		if (!mbHosting && len >= sizeof(cNetPlayerName))
		{
			const cNetPlayerName *pn = (const cNetPlayerName *)data;
			OnPlayerNameReceived(pn->mPlayerID, pn->msName, sizeof(pn->msName));
		}
		return;
	}

	if (t == eNetPacketType_PlayerLeave && len >= sizeof(cNetPlayerLeave))
	{
		const cNetPlayerLeave *lv = (const cNetPlayerLeave *)data;
		DropRemotePlayer(lv->mPlayerID);
		return;
	}

	if (t == eNetPacketType_Voice)
	{
		/* v16: guest side = the host's own voice (id 1) or a relayed guest
		   (author stamped by the host); host side = arrives via RelayVoice
		   with the peer id stamped. cVoiceChat validates the frame table. */
		if (mpVoice && len >= sizeof(cNetVoice))
		{
			cNetVoice hdr;
			memcpy(&hdr, data, sizeof(hdr));
			mpVoice->OnVoicePacket(hdr.mPlayerID, data, len);
		}
		return;
	}

	if (t == eNetPacketType_ObjectState)
	{
		if (!mbHosting) /* host->guest only; the host's world IS the truth */
			mpBodySync->ApplyStateBatch(data, len);
		return;
	}

	if (t == eNetPacketType_BodyCensus)
	{
		if (!mbHosting && len >= sizeof(cNetBodyCensus))
		{
			cNetBodyCensus census;
			memcpy(&census, data, sizeof(census));
			const bool bNewGen = !mpBodySync->HasRemoteCensus() ||
				mpBodySync->GetRemoteMapGen() != census.mMapGen;
			mpBodySync->OnCensusReceived(census);
			if (bNewGen)
				mbEnemySeqInKnown = false; /* v14 (5c): host reloaded — its enemy seq restarted */
			/* v14 hook 2: we already stand in a world (census taken) and the
			   host just (re)announced its own — reconnect while in-game, or
			   a host save/load on the same map. Ask for the world state. */
			if (mpBodySync->HasCensus())
				SendMapReady();
		}
		return;
	}

	if (t == eNetPacketType_MapReady)
		return; /* v14: host-only, answered in Service (needs the peer) */

	if (t == eNetPacketType_WorldSnapshot)
	{
		if (!mbHosting)
			HandleSnapshotChunk(data, len);
		return;
	}

	if (t == eNetPacketType_BodyGrabDeny)
	{
		if (!mbHosting && len >= sizeof(cNetBodyGrabDeny))
		{
			cNetBodyGrabDeny deny;
			memcpy(&deny, data, sizeof(deny));
			/* Clear the forward FIRST so the state's LeaveState end no-ops. */
			if (mpImpl->mlHeldHash == deny.mlNameHash)
			{
				mpImpl->mlHeldHash = 0;
				mpImpl->mbHaveGrabTarget = false;
				mpBodySync->ClearGuestHeld();
			}
			ForceReleaseIfHolding(deny.mlNameHash);
		}
		return;
	}

	if (t == eNetPacketType_MapChange)
	{
		if (len >= sizeof(cNetMapChange))
		{
			cNetMapChange mc;
			memcpy(&mc, data, sizeof(mc));
			mc.msMap[sizeof(mc.msMap) - 1] = 0; /* wire strings are untrusted */
			mc.msPos[sizeof(mc.msPos) - 1] = 0;
			if (mc.msMap[0] != 0)
			{
				msPendingMap = mc.msMap;
				msPendingPos = mc.msPos;
				msRemoteCurrentMap = mc.msMap;
				mbHavePendingMapChange = true; /* applied at the frame tick */
			}
		}
		return;
	}

	if (t == eNetPacketType_EnemyState)
	{
		if (!mbHosting)
			ApplyEnemyBatch(data, len);
		return;
	}

	if (t == eNetPacketType_EnemyDamage)
	{
		/* host only: a guest's weapon connected with a shared enemy */
		if (mbHosting && len >= sizeof(cNetEnemyDamage) && mpInit && mpInit->mpMapHandler)
		{
			cNetEnemyDamage dmg;
			memcpy(&dmg, data, sizeof(dmg));
			tGameEnemyIterator eit = mpInit->mpMapHandler->GetGameEnemyIterator();
			while (eit.HasNext())
			{
				iGameEnemy *pE = eit.Next();
				if (pE && NetHashName(pE->GetName().c_str()) == dmg.mlNameHash)
				{
					pE->Damage(dmg.mfDamage, (int)dmg.mlStrength);
					break;
				}
			}
		}
		return;
	}

	if (t == eNetPacketType_PlayerDamage)
	{
		if (!mbHosting && len >= sizeof(cNetPlayerDamage) && mpInit && mpInit->mpPlayer)
		{
			cNetPlayerDamage pd;
			memcpy(&pd, data, sizeof(pd));
			if (pd.mPlayerID == mlLocalPlayerId)
				mpInit->mpPlayer->Damage(pd.mfDamage, ePlayerDamageType_BloodSplash);
		}
		return;
	}

	if (t == eNetPacketType_ItemDrop)
	{
		if (len >= sizeof(cNetItemDrop))
		{
			cNetItemDrop drop;
			memcpy(&drop, data, sizeof(drop));
			drop.msName[sizeof(drop.msName) - 1] = 0; /* untrusted wire strings */
			drop.msFile[sizeof(drop.msFile) - 1] = 0;
			ApplyRemoteDrop(drop);
		}
		return;
	}

	if (t == eNetPacketType_ItemPickup)
	{
		if (len >= sizeof(cNetItemPickup))
		{
			cNetItemPickup ip;
			memcpy(&ip, data, sizeof(ip));
			ip.msItemName[sizeof(ip.msItemName) - 1] = 0;
			m_setTakenItems.insert(ip.mlQualHash);
			if (ip.msItemName[0] != 0)
				m_setPartyItems.insert(tString(ip.msItemName)); /* the party has it */
			ApplyTakenItems(); /* now if we are on that map; else on its load */
		}
		return;
	}

	if (t == eNetPacketType_EntityDamage)
	{
		if (len >= sizeof(cNetEntityDamage) && mpInit && mpInit->mpMapHandler)
		{
			cNetEntityDamage ed;
			memcpy(&ed, data, sizeof(ed));
			tGameEntityIterator eit = mpInit->mpMapHandler->GetGameEntityIterator();
			while (eit.HasNext())
			{
				iGameEntity *pEnt = eit.Next();
				if (pEnt == NULL || pEnt->GetType() == eGameEntityType_Enemy)
					continue; /* enemies have their own damage channel */
				if (QualifiedItemHash(pEnt->GetName()) != ed.mlQualHash)
					continue;
				gbNetScriptApplying = true; /* suppress re-broadcast */
				pEnt->Damage(ed.mfDamage, (int)ed.mlStrength);
				gbNetScriptApplying = false;
				break;
			}
		}
		return;
	}

	if (t == eNetPacketType_ScriptEvent)
	{
		if (len >= sizeof(cNetScriptEvent))
		{
			cNetScriptEvent se;
			memcpy(&se, data, sizeof(se));
			se.msName[sizeof(se.msName) - 1] = 0;
			if (se.mOp == eNetScriptOp_RemoveItem)
				m_setPartyItems.erase(tString(se.msName)); /* consumed for everyone */
			NetApplyScriptEvent((int)se.mOp, tString(se.msName), (int)se.mlVal);
			/* v14 (5b): the host is the single authority for Add ops — after
			   applying a guest's delta, broadcast the absolute value so the
			   author and every other guest land on the same number. */
			if (mbHosting && mpInit && mpInit->mpGame &&
				(se.mOp == eNetScriptOp_LocalVarAdd || se.mOp == eNetScriptOp_GlobalVarAdd))
			{
				cScene *pScene = mpInit->mpGame->GetScene();
				cScriptVar *pVar = pScene ? ((se.mOp == eNetScriptOp_LocalVarAdd) ?
					pScene->GetLocalVar(tString(se.msName)) : pScene->GetGlobalVar(tString(se.msName))) : NULL;
				if (pVar)
				{
					cNetScriptEvent setPkt = se;
					setPkt.mOp = (se.mOp == eNetScriptOp_LocalVarAdd) ?
						(uint8_t)eNetScriptOp_LocalVarSet : (uint8_t)eNetScriptOp_GlobalVarSet;
					setPkt.mlVal = (int32_t)pVar->mlVal;
					SendReliableEvent(&setPkt, sizeof(setPkt));
				}
			}
		}
		return;
	}

	if (t != eNetPacketType_PlayerState || len < sizeof(cNetPlayerState))
		return;

	const cNetPlayerState *st = (const cNetPlayerState *)data;
	if (st->mPlayerID == 0 || st->mPlayerID == mlLocalPlayerId)
		return;
	{
		/* v7 reorder guard: only strictly newer states move a ghost. int16
		   difference handles the wrap. */
		std::map<uint8_t, uint16_t>::iterator si = m_mapGhostSeq.find(st->mPlayerID);
		if (si != m_mapGhostSeq.end() && (int16_t)(st->mSeq - si->second) <= 0)
			return;
		m_mapGhostSeq[st->mPlayerID] = st->mSeq;
	}
	/* v12: mirrored health per id (both roles) — kept outside the ghost
	   entity so it survives our own map change (ghosts are rebuilt). */
	{
		const uint8_t lHealth =
			(st->mFlags & eNetPlayerFlag_Dead) ? (uint8_t)0 :
			((st->mHealth > 100) ? (uint8_t)100 : st->mHealth);
		NotePartyHealth(st->mPlayerID, lHealth); /* v13: "died" / "respawned" feed lines */
		m_mapGhostHealth[st->mPlayerID] = lHealth;
	}
	EnsureGhost(st->mPlayerID);
	std::map<uint8_t, cGhostPlayer *>::iterator gi = m_mapGhosts.find(st->mPlayerID);
	if (gi != m_mapGhosts.end())
		gi->second->ApplyState(*st);
	if (mbHosting)
		m_mapGhostMoveState[st->mPlayerID] = st->mMoveState; /* enemy senses: stealth + hearing (GetGhostSense) */

	/* Spawn-at-friend: the first HOST state after the census verifies (same
	   map, same identities) teleports the joining guest to the host's side.
	   One-shot per connection so later map scripts keep their own starts.
	   Ghosts have no collider, so materializing inside the host's ghost is
	   safe — one step and you're apart. */
	bool bSameMapAsHost = mpBodySync->CensusMatched();
	if (!bSameMapAsHost && !msRemoteCurrentMap.empty() && mpInit && mpInit->mpMapHandler)
	{
		/* A save-loaded host can have a census our virgin map can't match
		   (scripts destroyed bodies) — the announced map NAME still proves
		   we are standing in the same level. */
		bSameMapAsHost =
			cString::ToLowerCase(cString::SetFileExt(msRemoteCurrentMap, "")) ==
			cString::ToLowerCase(cString::SetFileExt(mpInit->mpMapHandler->GetCurrentMapName(), ""));
	}
	if (!mbHosting && !mbSpawnedAtHost && st->mPlayerID == 1 &&
		bSameMapAsHost && mpInit && mpInit->mpPlayer &&
		mpInit->mpPlayer->GetCharacterBody())
	{
		mbSpawnedAtHost = true;
		/* v11: wire Y is the host's FEET already (a hair up so the capsule
		   never starts intersecting the floor) */
		cVector3f vFeet(st->mfPosX, st->mfPosY + 0.05f, st->mfPosZ);
		iCharacterBody *pBody = mpInit->mpPlayer->GetCharacterBody();
		const cVector3f vCur = pBody->GetFeetPosition();
		const cVector3f vD = vFeet - vCur;
		if (vD.x * vD.x + vD.y * vD.y + vD.z * vD.z > 2.0f * 2.0f)
		{
			pBody->SetFeetPosition(vFeet);
			Log(" multiplayer: spawned at host's position (%.1f %.1f %.1f)\n",
				vFeet.x, vFeet.y, vFeet.z);
		}
	}
}

void cNetworkManager::EmitLocalSnapshots()
{
	if (!mpImpl || !mpImpl->mpHost)
		return;

	cNetPlayerState st;
	if (!BuildLocalSnapshot(&st))
		return;
	st.mPlayerID = mlLocalPlayerId;
	st.mSeq = ++mlStateSeqOut; /* v7: receivers drop reordered stale states */

	if (mbHosting)
	{
		for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
		{
			ENetPeer *pd = &mpImpl->mpHost->peers[i];
			if (!PeerLive(pd)) /* v15: accepted peers only */
				continue;
			ENetPacket *pkt = enet_packet_create(&st, sizeof(st), ENET_PACKET_FLAG_UNSEQUENCED);
			if (pkt)
				enet_peer_send(pd, 1, pkt);
		}
		return;
	}

	if (!mbClientConnected || !mbHadJoinPacket || !mpImpl->mpServerPeer ||
		mpImpl->mpServerPeer->state != ENET_PEER_STATE_CONNECTED)
		return;

	ENetPacket *pkt = enet_packet_create(&st, sizeof(st), ENET_PACKET_FLAG_UNSEQUENCED);
	if (pkt)
		enet_peer_send(mpImpl->mpServerPeer, 1, pkt);
}

//-----------------------------------------------------------------------
// Phase 5 object sync — the physics side lives in cBodySync (BodySync.cpp);
// here is only the transport: batch -> connected peers, census reliable.
//-----------------------------------------------------------------------

void cNetworkManager::EmitObjectStates()
{
	if (!mbHosting || !mpImpl || !mpImpl->mpHost)
		return; /* HOST-authoritative: only the host's world goes on the wire */

	bool bAnyPeer = false;
	for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
	{
		if (PeerLive(&mpImpl->mpHost->peers[i])) /* v15: accepted peers only */
		{
			bAnyPeer = true;
			break;
		}
	}
	if (!bAnyPeer)
		return;

	unsigned char aMoving[cBodySync::kMaxBatchBytes];
	unsigned char aSleep[cBodySync::kMaxBatchBytes];
	size_t lMovingLen = 0, lSleepLen = 0;
	mpBodySync->BuildStateBatches(aMoving, &lMovingLen, aSleep, &lSleepLen);
	if (lMovingLen == 0 && lSleepLen == 0)
		return;

	for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
	{
		ENetPeer *pd = &mpImpl->mpHost->peers[i];
		if (!PeerLive(pd))
			continue;
		if (lMovingLen > 0)
			SendStructToPeer(pd, aMoving, lMovingLen, false); /* next tick replaces a loss */
		if (lSleepLen > 0)
			SendStructToPeer(pd, aSleep, lSleepLen, true);    /* rest poses may NOT be lost */
	}
}

void cNetworkManager::SendCensus(ENetPeer *apOnlyTo)
{
	if (!mbHosting || !mpImpl || !mpImpl->mpHost || !mpBodySync->HasCensus())
		return;

	cNetBodyCensus census;
	mpBodySync->BuildCensusPacket(&census);

	for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
	{
		ENetPeer *pd = &mpImpl->mpHost->peers[i];
		if (!PeerLive(pd)) /* v15: accepted peers only */
			continue;
		if (apOnlyTo && pd != apOnlyTo)
			continue;
		ENetPacket *pkt = enet_packet_create(&census, sizeof(census), ENET_PACKET_FLAG_RELIABLE);
		if (pkt)
			enet_peer_send(pd, 0, pkt);
	}
}

void cNetworkManager::Service(int timeoutMs)
{
	if (!mpImpl || !mpImpl->mpHost)
		return;

	ENetEvent ev;
	while (enet_host_service(mpImpl->mpHost, &ev, timeoutMs) > 0)
	{
		timeoutMs = 0;
		switch (ev.type)
		{
		case ENET_EVENT_TYPE_CONNECT:
			if (mbHosting)
			{
				char sWho[64];
				FormatPeerAddr(ev.peer, sWho, sizeof(sWho));
				mpImpl->m_mapGuards.erase(ev.peer); /* v15: a reused slot starts clean */
				PeerSetId(ev.peer, 0);
				if (ev.data != kNetConnectData)
				{
					/* old exe (sends 0) or foreign client — refuse BEFORE any
					   state flows; half-compatible is the worst outcome */
					Log(" multiplayer: REFUSED incompatible peer %s (connect data 0x%08X, want 0x%08X)\n",
						sWho, (unsigned)ev.data, (unsigned)kNetConnectData);
					enet_peer_disconnect(ev.peer, kNetDisconnectBadVersion);
					break;
				}
				/* v15: max_players at CONNECT. Every connected slot counts —
				   accepted or still answering the challenge — so a burst of
				   half-open joins cannot exceed the cap either. ev.peer is
				   already CONNECTED here, hence the "- 1". */
				{
					const int lOthers = CountConnectedPeers(false) - 1;
					if (lOthers >= (int)mlMaxPlayers - 1 ||
						(mvFreeGuestIds.empty() && mlNextGuestId >= kPreviewGhostId))
					{
						Log(" multiplayer: REFUSED peer %s - server full (%d/%u guests)\n",
							sWho, lOthers, (unsigned)mlMaxPlayers - 1u);
						enet_peer_disconnect(ev.peer, kNetDisconnectFull);
						break;
					}
				}
				/* v15: challenge FIRST. The peer gets no id, no VersionAck, no
				   PlayerJoin, no census/beacon/name table and nothing from the
				   send loops until its cNetAuth is accepted (HostAcceptPeer);
				   anything else it sends meanwhile is dropped, and silence
				   is a disconnect after kNetAuthTimeoutSeconds. */
				{
					Impl::cPeerGuard &guard = mpImpl->m_mapGuards[ev.peer];
					guard = Impl::cPeerGuard();
					MakeNonce(ev.peer, guard.mNonce);
					cNetChallenge ch;
					ch.mType = eNetPacketType_Challenge;
					memcpy(ch.mNonce, guard.mNonce, sizeof(ch.mNonce));
					SendStructToPeer(ev.peer, &ch, sizeof(ch), true);
				}
				Log(" multiplayer: peer %s connected - challenge sent, waiting for auth%s\n",
					sWho, msServerPassword.empty() ? " (open server)" : " (password)");
			}
			else if (ev.peer == mpImpl->mpServerPeer)
			{
				mbClientConnected = true;
				Log(" multiplayer: host link up (waiting for JOIN packet)\n");
			}
			break;

		case ENET_EVENT_TYPE_DISCONNECT:
			if (!mbHosting && mpImpl && ev.peer == mpImpl->mpServerPeer)
			{
				if (ev.data == kNetDisconnectBadVersion)
				{
					msJoinFailReason = "Host REFUSED: version mismatch - you both need the same zip.";
					Log(" multiplayer: host refused us - version mismatch\n");
				}
				else if (ev.data == kNetDisconnectBadAuth) /* v15 */
				{
					msJoinFailReason = mbAuthSent ?
						"Host REFUSED: wrong password." :
						"Host REFUSED: no password answer in time - check your connection.";
					Log(" multiplayer: host refused us - bad auth (answer sent: %d)\n", mbAuthSent ? 1 : 0);
				}
				else if (ev.data == kNetDisconnectFull) /* v15 */
				{
					msJoinFailReason = "Host REFUSED: the server is full.";
					Log(" multiplayer: host refused us - server full\n");
				}
				else if (ev.data == kNetDisconnectKicked) /* v15 */
				{
					msJoinFailReason = "Host dropped us: too many bad packets (see hpl.log on the host).";
					Log(" multiplayer: host kicked us - protocol violations\n");
				}
				Log(" multiplayer: host disconnected\n");
				mbClientConnected = false;
				mbHadJoinPacket = false;
				mbAuthSent = false; /* v15: a reconnect gets a new challenge */
				mlLocalPlayerId = 0;
				mpImpl->mpServerPeer = NULL;
				ClearGhostsInternal();
			}
			else if (mbHosting)
			{
				uint8_t gone = PeerGetId(ev.peer);
				if (gone)
				{
					BlastLeaves(mpImpl->mpHost, gone, ev.peer);
					DropRemotePlayer(gone);
					SendNameTable(NULL); /* v13: table without the leaver */
					/* rung 3: a vanished guest drops whatever it held */
					int lFreed = mpBodySync->ReleaseAllHeldBy(gone);
					if (lFreed > 0)
						Log(" multiplayer: guest %u left holding %d object(s) — released\n",
							(unsigned)gone, lFreed);
					FreeGuestId(gone); /* v15: the id goes back to the pool */
				}
				else
				{
					char sWho[64];
					FormatPeerAddr(ev.peer, sWho, sizeof(sWho));
					Log(" multiplayer: unaccepted peer %s gone\n", sWho);
				}
				PeerSetId(ev.peer, 0);
				mpImpl->m_mapGuards.erase(ev.peer); /* v15: per-peer state dies with the slot */
			}
			break;

		case ENET_EVENT_TYPE_RECEIVE:
		{
			ENetPacket *pk = ev.packet;
			if (!pk || pk->dataLength == 0)
				break;

			const uint8_t lFirst = *(const uint8_t *)pk->data;
			if (mbHosting)
			{
				/* v15 gate, in this order: a known, not-refused peer ->
				   authenticated (until then only cNetAuth is looked at) ->
				   reliable rate limit -> role table -> payload validation,
				   which clamps IN PLACE so the relays below forward the
				   sanitised bytes. Every failure is a strike (NoteViolation);
				   kNetMaxStrikes = kNetDisconnectKicked. */
				std::map<const ENetPeer *, Impl::cPeerGuard>::iterator gi = mpImpl->m_mapGuards.find(ev.peer);
				Impl::cPeerGuard *pGuard = (gi != mpImpl->m_mapGuards.end()) ? &gi->second : NULL;
				if (pGuard == NULL || pGuard->mbRefused)
				{
					enet_packet_destroy(pk); /* refused at CONNECT / kicked / unknown slot */
					break;
				}
				if (!pGuard->mbAuthed)
				{
					if (lFirst == eNetPacketType_Auth && (size_t)pk->dataLength >= sizeof(cNetAuth))
					{
						cNetAuth auth;
						memcpy(&auth, pk->data, sizeof(auth));
						HostAcceptPeer(ev.peer, auth);
					}
					else
						NoteViolation(ev.peer, lFirst, "packet before authentication");
					enet_packet_destroy(pk);
					break;
				}
				if (ev.channelID == 0)
				{
					++pGuard->mlReliableInWindow;
					if (pGuard->mlReliableInWindow > kNetMaxReliablePerSec)
					{
						if (!pGuard->mbRateStruck)
						{
							pGuard->mbRateStruck = true;
							NoteViolation(ev.peer, lFirst, "reliable packet flood");
						}
						enet_packet_destroy(pk);
						break;
					}
				}
				if (!IsAllowedFrom(lFirst, false))
				{
					NoteViolation(ev.peer, lFirst, "type not allowed from a guest");
					enet_packet_destroy(pk);
					break;
				}
				if (!ValidateGuestPacket(ev.peer, pk->data, (size_t)pk->dataLength))
				{
					NoteViolation(ev.peer, lFirst, "malformed or out-of-bounds payload");
					enet_packet_destroy(pk);
					break;
				}
				const uint8_t author = PeerGetId(ev.peer); /* >= 2: accepted above */
				if (lFirst >= eNetPacketType_BodyGrabBegin && lFirst <= eNetPacketType_BodyPush)
				{
					/* rung 3 intent needs to know WHICH guest sent it */
					HandleBodyIntent(author, pk->data, (size_t)pk->dataLength);
				}
				else if (lFirst == eNetPacketType_PlayerState && author >= 2)
				{
					cNetPlayerState relay;
					memcpy(&relay, pk->data, sizeof(relay));
					relay.mPlayerID = author;
					for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
					{
						ENetPeer *dst = &mpImpl->mpHost->peers[i];
						if (dst == ev.peer || !PeerLive(dst)) /* v15: accepted peers only */
							continue;
						ENetPacket *rp =
							enet_packet_create(&relay, sizeof(relay), ENET_PACKET_FLAG_UNSEQUENCED);
						if (rp)
							enet_peer_send(dst, 1, rp);
					}
					DispatchIncoming(&relay, sizeof(relay));
				}
				else if (author >= 2 && lFirst == eNetPacketType_PlayerName &&
					(size_t)pk->dataLength >= sizeof(cNetPlayerName))
				{
					/* v13: a guest's name — trusted by PEER id, not by the
					   id byte in the packet; then the whole table goes out */
					const cNetPlayerName *pn = (const cNetPlayerName *)pk->data;
					OnPlayerNameReceived(author, pn->msName, sizeof(pn->msName));
					SendNameTable(NULL);
				}
				else if (author >= 2 && lFirst == eNetPacketType_MapReady)
				{
					/* v14: needs the peer to answer with the snapshot */
					HandleMapReady(ev.peer, pk->data, (size_t)pk->dataLength);
				}
				else if (author >= 2 && lFirst == eNetPacketType_Voice)
				{
					/* v16: author stamped from the PEER (never trusted from
					   the packet), relayed unsequenced to the other guests */
					RelayVoice(ev.peer, author, pk->data, (size_t)pk->dataLength);
				}
				else if (author >= 2 && (lFirst == eNetPacketType_MapChange ||
					lFirst == eNetPacketType_ItemPickup ||
					lFirst == eNetPacketType_ItemDrop ||
					lFirst == eNetPacketType_ScriptEvent ||
					lFirst == eNetPacketType_EntityDamage))
				{
					/* v14 (5b): a guest's var ADD is not relayed blindly — the
					   host applies it and broadcasts the resulting absolute
					   Set (DispatchIncoming), so every machine converges. */
					bool bRelay = true;
					if (lFirst == eNetPacketType_ScriptEvent &&
						(size_t)pk->dataLength >= sizeof(cNetScriptEvent))
					{
						cNetScriptEvent peek;
						memcpy(&peek, pk->data, sizeof(peek));
						if (peek.mOp == eNetScriptOp_LocalVarAdd || peek.mOp == eNetScriptOp_GlobalVarAdd)
							bRelay = false;
					}
					for (size_t i = 0; bRelay && i < mpImpl->mpHost->peerCount; ++i)
					{
						ENetPeer *dst = &mpImpl->mpHost->peers[i];
						if (dst == ev.peer || !PeerLive(dst)) /* v15: accepted peers only */
							continue;
						SendStructToPeer(dst, pk->data, (size_t)pk->dataLength, true);
					}
					DispatchIncoming(pk->data, (size_t)pk->dataLength);
				}
				else
					DispatchIncoming(pk->data, (size_t)pk->dataLength);
			}
			else
			{
				/* v15 guest side: the host is the game's authority, but its
				   packets still pass the role table and the shape/bounds
				   checks (ValidateEventPacket clamps in place) — a hostile
				   host cannot make us load "..\\x.dae" or spawn from a path.
				   Log once per type per connection, no strikes: we simply
				   drop what we do not understand. */
				bool bOk = IsAllowedFrom(lFirst, true);
				if (bOk)
					bOk = ValidateEventPacket(pk->data, (size_t)pk->dataLength, true);
				if (bOk)
					DispatchIncoming(pk->data, (size_t)pk->dataLength);
				else
				{
					const unsigned lBit = lFirst < 64 ? lFirst : 63;
					if ((mlGuestViolationsLogged & (1ull << lBit)) == 0)
					{
						mlGuestViolationsLogged |= (1ull << lBit);
						Log(" multiplayer: dropped host packet type %u (%u B) - not allowed or malformed (logged once)\n",
							(unsigned)lFirst, (unsigned)pk->dataLength);
					}
				}
			}

			enet_packet_destroy(pk);
			break;
		}

		default:
			break;
		}
	}
}

//-----------------------------------------------------------------------

void cNetworkManager::HostGame(uint16_t alPort)
{
	Disconnect();
	if (!NetAcquire())
		return;

	ENetAddress addr;
	addr.host = ENET_HOST_ANY;
	addr.port = alPort;

	mpImpl->mpHost = enet_host_create(&addr, 31, 2, 0, 0);
	if (!mpImpl->mpHost)
	{
		Log(" multiplayer: bind UDP %u failed\n", (unsigned)alPort);
		NetRelease();
		return;
	}

	mbHosting = true;
	mlLocalPlayerId = 1;
	mbHavePendingMapChange = false;
	mbLocalMapChangeArmed = false;
	m_setTakenItems.clear(); /* one-of-each bookkeeping is per session */
	m_setPartyItems.clear();
	mlNextGuestId = 2;
	mvFreeGuestIds.clear(); /* v15: fresh id space */
	ResetPeerGuards();
	mbHadJoinPacket = true;
	mlListenPort = alPort;
	mbClientConnected = false;
	Log(" multiplayer: HOST udp/%u\n", (unsigned)alPort);

	OpenHostDiscovery();

	/* Internet listing: resolve the master NOW (blocking DNS is acceptable
	   here — a button click or the cfg auto-host, never a frame) and beacon
	   at once; Update repeats every kNetMasterRegisterSeconds. */
	mbMasterRegistered = false;
	mfMasterRegisterAccum = 0;
	if (mbPublic && ResolveMasterAddress())
		SendMasterRegister();
}

hpl::tString cNetworkManager::GetClipboardTextAscii()
{
#ifdef _WIN32
	tString sOut;
	if (!OpenClipboard(NULL))
		return sOut;
	HANDLE h = GetClipboardData(CF_TEXT);
	if (h)
	{
		const char *pc = (const char *)GlobalLock(h);
		if (pc)
		{
			for (; *pc && sOut.size() < 512; ++pc)
				if ((unsigned char)*pc >= 32 && (unsigned char)*pc < 127)
					sOut += *pc;
			GlobalUnlock(h);
		}
	}
	CloseClipboard();
	return sOut;
#else
	return "";
#endif
}

//-----------------------------------------------------------------------
// v5 shared world: party-follow level transitions + one-of-each items.
//-----------------------------------------------------------------------

void cNetworkManager::SendReliableEvent(const void *apData, size_t alLen)
{
	if (!mpImpl || !mpImpl->mpHost)
		return;
	if (mbHosting)
	{
		for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
		{
			ENetPeer *pd = &mpImpl->mpHost->peers[i];
			if (PeerLive(pd)) /* v15: nothing reaches a peer before its auth */
				SendStructToPeer(pd, apData, alLen, true);
		}
	}
	else if (mbClientConnected && mbHadJoinPacket && mpImpl->mpServerPeer &&
		mpImpl->mpServerPeer->state == ENET_PEER_STATE_CONNECTED)
	{
		SendStructToPeer(mpImpl->mpServerPeer, apData, alLen, true);
	}
}

int cNetworkManager::GetConnectedGuestCount() const
{
	if (!mbHosting || !mpImpl || !mpImpl->mpHost)
		return 0;
	return CountConnectedPeers(true); /* v15: accepted guests only */
}

/** Host: "the party is on THIS map" — sent to everyone on our census frame
    (covers save loads and new games, which never pass through ChangeMap) and
    to each guest the moment it connects. A guest already on the map ignores
    it; a guest elsewhere follows; a guest parked in the MENU launches. */
void cNetworkManager::SendMapBeacon(ENetPeer *apOnlyTo)
{
	if (!mbHosting || !mpInit || !mpInit->mpMapHandler)
		return;
	const tString sMap = mpInit->mpMapHandler->GetCurrentMapName();
	if (sMap.empty())
		return; /* still in the menu ourselves — nothing to announce */
	/* msCurrentMap is stored lowercase WITHOUT extension (MapHandler::Load
	   strips it), but LoadWorld3D needs the real file name — restore it. */
	const tString sMapFile = cString::SetFileExt(sMap, "dae");
	cNetMapChange pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.mType = eNetPacketType_MapChange;
	strncpy(pkt.msMap, sMapFile.c_str(), sizeof(pkt.msMap) - 1);
	if (apOnlyTo)
		SendStructToPeer(apOnlyTo, &pkt, sizeof(pkt), true);
	else
		SendReliableEvent(&pkt, sizeof(pkt));
}

bool cNetworkManager::PartyHasItem(const hpl::tString &asName) const
{
	return m_setPartyItems.count(asName) != 0;
}

void cNetworkManager::NetOnScriptEvent(int alOp, const hpl::tString &asName, int alVal)
{
	/* v14 (5a): an item consumed by a script running HERE leaves the party
	   set too (a friend's key we used) — before any early return, or a
	   HasItem() gate stays open forever. Empty offline, so a no-op there. */
	if (alOp == eNetScriptOp_RemoveItem)
		m_setPartyItems.erase(asName);
	if (gbNetScriptApplying)
		return; /* this mutation IS a replication — do not echo it */
	if (!mpImpl || !mpImpl->mpHost)
		return; /* offline: single-player stays untouched */
	if (asName.empty() || asName.size() >= 48)
		return;
	cNetScriptEvent pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.mType = eNetPacketType_ScriptEvent;
	pkt.mOp = (uint8_t)alOp;
	strncpy(pkt.msName, asName.c_str(), sizeof(pkt.msName) - 1);
	pkt.mlVal = alVal;

	/* v14 (5b): Add ops. Both machines run the same symmetric scripts
	   (OnStart/OnLoad/OnUpdate/timers), so relaying every local +1 doubled
	   the counter. The HOST is the authority: it broadcasts the absolute
	   value (a Set) instead of the delta. A GUEST applied its +1 locally
	   already; it forwards the delta only when the script ran because of
	   something only it did (player context) — for a symmetric script the
	   host runs the same code and its Set arrives shortly. */
	if (alOp == eNetScriptOp_LocalVarAdd || alOp == eNetScriptOp_GlobalVarAdd)
	{
		if (mbHosting)
		{
			cScene *pScene = (mpInit && mpInit->mpGame) ? mpInit->mpGame->GetScene() : NULL;
			cScriptVar *pVar = pScene ? ((alOp == eNetScriptOp_LocalVarAdd) ?
				pScene->GetLocalVar(asName) : pScene->GetGlobalVar(asName)) : NULL;
			if (pVar) /* the engine fires the callback AFTER the add (v14) */
			{
				pkt.mOp = (alOp == eNetScriptOp_LocalVarAdd) ?
					(uint8_t)eNetScriptOp_LocalVarSet : (uint8_t)eNetScriptOp_GlobalVarSet;
				pkt.mlVal = (int32_t)pVar->mlVal;
			}
		}
		else if (!gbNetScriptPlayerContext)
			return; /* symmetric script: the host's Set(abs) is on its way */
	}
	SendReliableEvent(&pkt, sizeof(pkt));
}

void cNetworkManager::NetOnEntityDamaged(const hpl::tString &asName, float afDamage, int alStrength)
{
	if (gbNetScriptApplying)
		return; /* this hit IS a replication */
	if (!mpImpl || !mpImpl->mpHost)
		return;
	cNetEntityDamage pkt;
	pkt.mType = eNetPacketType_EntityDamage;
	pkt.mlQualHash = QualifiedItemHash(asName);
	pkt.mfDamage = afDamage;
	pkt.mlStrength = (int8_t)alStrength;
	SendReliableEvent(&pkt, sizeof(pkt));
}

bool cNetworkManager::IsEnemyPuppetMode() const
{
	return !mbHosting && mbClientConnected && mbHadJoinPacket;
}

void cNetworkManager::GetGhostCamPositions(std::vector<std::pair<uint8_t, hpl::cVector3f> > &avOut)
{
	avOut.clear();
	if (!mbHosting)
		return; /* only the host's AI has any business asking */
	for (tGhostMap::const_iterator it = m_mapGhosts.begin(); it != m_mapGhosts.end(); ++it)
	{
		cVector3f v;
		if (it->second && it->second->GetLastStatePos(&v))
			avOut.push_back(std::make_pair(it->first, v));
	}
}

void cNetworkManager::SendPlayerDamage(uint8_t alPlayerId, float afDamage)
{
	if (!mbHosting || !mpImpl || !mpImpl->mpHost)
		return;
	cNetPlayerDamage pkt;
	pkt.mType = eNetPacketType_PlayerDamage;
	pkt.mPlayerID = alPlayerId;
	pkt.mfDamage = afDamage;
	for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
	{
		ENetPeer *pd = &mpImpl->mpHost->peers[i];
		if (pd->state == ENET_PEER_STATE_CONNECTED && PeerGetId(pd) == alPlayerId)
		{
			SendStructToPeer(pd, &pkt, sizeof(pkt), true);
			return;
		}
	}
}

void cNetworkManager::NetOnEnemyDamaged(const hpl::tString &asName, float afDamage, int alStrength)
{
	if (!mpImpl || !mpImpl->mpHost)
		return;
	cNetEnemyDamage pkt;
	pkt.mType = eNetPacketType_EnemyDamage;
	pkt.mlNameHash = NetHashName(asName.c_str());
	pkt.mfDamage = afDamage;
	pkt.mlStrength = (int8_t)alStrength;
	SendReliableEvent(&pkt, sizeof(pkt));
}

/** Host, at the send tick: every enemy's pose + vitals + commanded clip.
    Whole roster each tick — Penumbra maps carry a handful of enemies, so a
    full batch is ~30 B each and always under MTU. */
/** One enemy's wire state (the roster stream and the v14 snapshot share
    it). False = no character body yet, nothing to say. */
static bool FillEnemyState(iGameEnemy *apEnemy, cNetEnemyState *apOut)
{
	if (apEnemy == NULL || apOut == NULL || apEnemy->GetMover() == NULL)
		return false;
	iCharacterBody *pBody = apEnemy->GetMover()->GetCharBody();
	if (pBody == NULL)
		return false;
	apOut->mlNameHash = NetHashName(apEnemy->GetName().c_str());
	const cVector3f v = pBody->GetFeetPosition();
	apOut->mfPosX = v.x; apOut->mfPosY = v.y; apOut->mfPosZ = v.z;
	apOut->mfYaw = pBody->GetYaw();
	apOut->mfHealth = apEnemy->GetHealth();
	apOut->mlAnimHash = apEnemy->GetNetAnimHash();
	apOut->mFlags = (uint8_t)((apEnemy->GetNetAnimLoop() ? 1 : 0) |
		(apEnemy->IsActive() ? 2 : 0));
	return true;
}

void cNetworkManager::EmitEnemyStates()
{
	if (!mbHosting || !mpImpl || !mpImpl->mpHost || !mpInit || !mpInit->mpMapHandler)
		return;

	/* v14 (5c): the roster is streamed in chunks of kMaxEnemiesPerBatch —
	   the old fixed 8-entry buffer silently left enemy 9+ un-puppeted
	   (running LOCAL AI on the guest). Each chunk carries its own seq; the
	   guest only drops a chunk that is older than the newest it applied. */
	unsigned char aBuf[sizeof(cNetEnemyBatch) + kMaxEnemiesPerBatch * sizeof(cNetEnemyState)];
	int lCount = 0;
	tGameEnemyIterator it = mpInit->mpMapHandler->GetGameEnemyIterator();
	for (;;)
	{
		iGameEnemy *pEnemy = it.HasNext() ? it.Next() : NULL;
		if (pEnemy)
		{
			cNetEnemyState st;
			if (FillEnemyState(pEnemy, &st))
			{
				memcpy(aBuf + sizeof(cNetEnemyBatch) + (size_t)lCount * sizeof(cNetEnemyState),
					&st, sizeof(st));
				++lCount;
			}
		}

		const bool bLast = !it.HasNext();
		if (lCount > 0 && (lCount >= (int)kMaxEnemiesPerBatch || bLast))
		{
			cNetEnemyBatch hdr;
			hdr.mType = eNetPacketType_EnemyState;
			hdr.mCount = (uint8_t)lCount;
			hdr.mMapGen = mpBodySync->GetMapGen(); /* v14: stale-map guard, was 0 */
			hdr.mSeq = ++mlEnemySeqOut;
			memcpy(aBuf, &hdr, sizeof(hdr));
			const size_t lLen = sizeof(hdr) + (size_t)lCount * sizeof(cNetEnemyState);

			for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
			{
				ENetPeer *pd = &mpImpl->mpHost->peers[i];
				if (!PeerLive(pd)) /* v15: accepted peers only */
					continue;
				ENetPacket *pkt = enet_packet_create(aBuf, lLen, ENET_PACKET_FLAG_UNSEQUENCED);
				if (pkt)
					enet_peer_send(pd, 1, pkt);
			}
			lCount = 0;
		}
		if (bLast)
			break;
	}
}

/** Guest: adopt the host's enemy roster. Each entry turns the local enemy
    into a puppet, retargets it, mirrors active/anim, and a health<=0 entry
    hands the enemy BACK to local code for its death (ragdoll wants the
    normal path, and a dead enemy needs no further puppeting). */
void cNetworkManager::ApplyEnemyBatch(const void *apData, size_t alLen)
{
	if (!apData || alLen < sizeof(cNetEnemyBatch))
		return;
	const cNetEnemyBatch *pHdr = (const cNetEnemyBatch *)apData;
	/* v14 (5c): a batch from a generation we did not pair with (in flight
	   across a level change, or the host's previous load) must not touch
	   these enemies — same guard as the body batches. */
	if (!mpBodySync->IsRemoteGen(pHdr->mMapGen))
		return;
	if (mbEnemySeqInKnown && (int16_t)(pHdr->mSeq - mlEnemySeqIn) <= 0)
		return; /* reordered stale batch */
	mlEnemySeqIn = pHdr->mSeq;
	mbEnemySeqInKnown = true;

	if (!mpInit || !mpInit->mpMapHandler)
		return;
	size_t lCount = pHdr->mCount;
	const size_t lWhole = (alLen - sizeof(cNetEnemyBatch)) / sizeof(cNetEnemyState);
	if (lCount > lWhole)
		lCount = lWhole;
	const unsigned char *pRaw = (const unsigned char *)apData;

	/* v14 (5c): one hash -> enemy index per batch instead of a roster scan
	   per entry (40 entries x N enemies at 30 Hz added up). */
	std::map<uint32_t, iGameEnemy *> mapEnemies;
	{
		tGameEnemyIterator eit = mpInit->mpMapHandler->GetGameEnemyIterator();
		while (eit.HasNext())
		{
			iGameEnemy *pE = eit.Next();
			if (pE)
				mapEnemies.insert(std::make_pair(NetHashName(pE->GetName().c_str()), pE));
		}
	}

	for (size_t i = 0; i < lCount; ++i)
	{
		cNetEnemyState st;
		memcpy(&st, pRaw + sizeof(cNetEnemyBatch) + i * sizeof(cNetEnemyState), sizeof(st));

		std::map<uint32_t, iGameEnemy *>::iterator ei = mapEnemies.find(st.mlNameHash);
		iGameEnemy *pEnemy = (ei != mapEnemies.end()) ? ei->second : NULL;
		if (pEnemy == NULL)
			continue; /* different map or a despawned enemy */
		if (pEnemy->GetHealth() <= 0)
			continue; /* already locally dead: the ragdoll owns it */

		if (st.mfHealth <= 0)
		{
			/* host says it died: run the LOCAL death for ragdoll/sounds —
			   but never its death SCRIPT (v14 5b): the host ran that, and
			   its effects arrive as script events; running it here too
			   doubled every AddLocalVar in a kill-counter puzzle. */
			pEnemy->SetOnDeathCallback("");
			pEnemy->SetNetPuppet(false);
			pEnemy->Damage(100000.0f, 100);
			continue;
		}

		pEnemy->SetNetPuppet(true);
		pEnemy->NetSetTarget(cVector3f(st.mfPosX, st.mfPosY, st.mfPosZ), st.mfYaw);

		const bool bActive = (st.mFlags & 2) != 0;
		if (pEnemy->IsActive() != bActive)
			pEnemy->SetActive(bActive);

		if (st.mlAnimHash != 0 && bActive)
		{
			/* reverse-map the clip hash against OUR mesh's animation list */
			cMeshEntity *pMesh = pEnemy->GetMeshEntity();
			if (pMesh)
			{
				const int lNum = pMesh->GetAnimationStateNum();
				for (int a = 0; a < lNum; ++a)
				{
					cAnimationState *pA = pMesh->GetAnimationState(a);
					if (pA && NetHashName(tString(pA->GetName()).c_str()) == st.mlAnimHash)
					{
						/* PlayAnim early-outs if it is already playing */
						pEnemy->PlayAnim(pA->GetName(), (st.mFlags & 1) != 0,
							0.3f, false, 1.0f, false, true);
						break;
					}
				}
			}
		}
	}
}

uint32_t cNetworkManager::QualifiedItemHash(const hpl::tString &asEntityName) const
{
	tString sMap = "";
	if (mpInit && mpInit->mpMapHandler)
		sMap = cString::ToLowerCase(
			cString::SetFileExt(mpInit->mpMapHandler->GetCurrentMapName(), ""));
	const tString sQual = sMap + ":" + asEntityName;
	return NetHashName(sQual.c_str());
}

void cNetworkManager::NetOnLocalMapChange(const hpl::tString &asMap, const hpl::tString &asPos)
{
	if (mbApplyingRemoteMapChange) /* we are FOLLOWING — do not echo it back */
		return;
	if (!mpImpl || !mpImpl->mpHost)
		return;
	mbLocalMapChangeArmed = true; /* our own transition wins until it lands */
	cNetMapChange pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.mType = eNetPacketType_MapChange;
	strncpy(pkt.msMap, asMap.c_str(), sizeof(pkt.msMap) - 1);
	strncpy(pkt.msPos, asPos.c_str(), sizeof(pkt.msPos) - 1);
	SendReliableEvent(&pkt, sizeof(pkt));
	Log(" multiplayer: level transition to '%s' announced to the party\n", asMap.c_str());
}

void cNetworkManager::NetOnItemPicked(const hpl::tString &asEntityName)
{
	if (!mpImpl || !mpImpl->mpHost)
		return;
	cNetItemPickup pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.mType = eNetPacketType_ItemPickup;
	pkt.mlQualHash = QualifiedItemHash(asEntityName);
	strncpy(pkt.msItemName, asEntityName.c_str(), sizeof(pkt.msItemName) - 1);
	SendReliableEvent(&pkt, sizeof(pkt));
	/* v14 (5a): our OWN picks belong in the taken set too — it is what the
	   world snapshot serialises for a joiner (only received pickups were
	   recorded before). The entity is being destroyed here anyway. */
	m_setTakenItems.insert(pkt.mlQualHash);
}

void cNetworkManager::NetOnItemDropped(const hpl::tString &asName, const hpl::tString &asFile,
	const hpl::cVector3f &avPos, const hpl::cVector3f &avImpulse)
{
	if (!mpImpl || !mpImpl->mpHost)
		return;
	cNetItemDrop pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.mType = eNetPacketType_ItemDrop;
	strncpy(pkt.msName, asName.c_str(), sizeof(pkt.msName) - 1);
	strncpy(pkt.msFile, asFile.c_str(), sizeof(pkt.msFile) - 1);
	pkt.mfPosX = avPos.x; pkt.mfPosY = avPos.y; pkt.mfPosZ = avPos.z;
	pkt.mfImpX = avImpulse.x; pkt.mfImpY = avImpulse.y; pkt.mfImpZ = avImpulse.z;
	SendReliableEvent(&pkt, sizeof(pkt));
	/* our own re-dropped item must not be re-hidden by an old pickup event */
	m_setTakenItems.erase(QualifiedItemHash(asName));
}

void cNetworkManager::ApplyRemoteDrop(const cNetItemDrop &aDrop)
{
	if (!mpInit || !mpInit->mpMapHandler || !mpInit->mpGame)
		return;
	cWorld3D *pWorld = mpInit->mpGame->GetScene()->GetWorld3D();
	if (pWorld == NULL)
		return;

	const tString sName = aDrop.msName;
	const tString sFile = aDrop.msFile;
	if (sName.empty() || sFile.empty())
		return;

	/* A pickup event may have deactivated our copy earlier — that pickup is
	   hereby undone, so the sweep must not re-hide the reborn item. */
	m_setTakenItems.erase(QualifiedItemHash(sName));
	m_setPartyItems.erase(sName); /* it is on the floor, nobody HOLDS it */

	const cVector3f vPos(aDrop.mfPosX, aDrop.mfPosY, aDrop.mfPosZ);
	const cVector3f vImp(aDrop.mfImpX, aDrop.mfImpY, aDrop.mfImpZ);

	/* Case 1: WE still have the original entity, deactivated by the pickup
	   (or even still active) — reuse it: reactivate + move to the drop spot.
	   Spawning a second entity under the same name would break the name-hash
	   identity every other sync layer relies on. */
	iGameEntity *pEnt = mpInit->mpMapHandler->GetGameEntity(sName, false);
	if (pEnt)
	{
		pEnt->SetActive(true);
		if (pEnt->GetBody(0))
		{
			iPhysicsBody *pBody = pEnt->GetBody(0);
			cMatrixf mtx = cMatrixf::Identity;
			mtx.SetTranslation(vPos);
			pBody->SetMatrix(mtx);
			pBody->SetLinearVelocity(cVector3f(0, 0, 0));
			pBody->SetAngularVelocity(cVector3f(0, 0, 0));
			pBody->SetEnabled(true);
			pBody->AddImpulse(vImp);
		}
		Log(" multiplayer: friend dropped '%s' - reactivated our copy\n", sName.c_str());
		return;
	}

	/* Case 2: never had it (they carried it in from another map, or picked it
	   up before we ever loaded this one) — spawn an identical twin. */
	cMatrixf mtxItem = cMatrixf::Identity;
	mtxItem.SetTranslation(vPos);
	iEntity3D *pSpawned = pWorld->CreateEntity(sName, mtxItem, sFile, true);
	if (pSpawned)
	{
		cMeshEntity *pMesh = static_cast<cMeshEntity *>(pSpawned);
		if (pMesh->GetBody())
			pMesh->GetBody()->AddImpulse(vImp);
		Log(" multiplayer: friend dropped '%s' - spawned from '%s'\n",
			sName.c_str(), sFile.c_str());
	}
	else
		Log(" multiplayer: friend dropped '%s' but spawn from '%s' FAILED\n",
			sName.c_str(), sFile.c_str());
}

void cNetworkManager::ApplyTakenItems()
{
	if (m_setTakenItems.empty() || !mpInit || !mpInit->mpMapHandler)
		return;
	tGameEntityIterator it = mpInit->mpMapHandler->GetGameEntityIterator();
	while (it.HasNext())
	{
		iGameEntity *pEnt = it.Next();
		if (pEnt == NULL || pEnt->GetType() != eGameEntityType_Item || !pEnt->IsActive())
			continue;
		if (m_setTakenItems.count(QualifiedItemHash(pEnt->GetName())))
		{
			pEnt->SetActive(false);
			Log(" multiplayer: item '%s' pocketed by a friend — removed here\n",
				pEnt->GetName().c_str());
		}
	}
}

void cNetworkManager::ApplyPendingMapChange()
{
	if (!mbHavePendingMapChange || !mpInit || !mpInit->mpMapHandler)
		return;
	mbHavePendingMapChange = false;
	if (mbLocalMapChangeArmed) /* we are mid-transition ourselves; ours wins */
		return;

	/* Parked in the main menu with no world (joined from the lobby): perform
	   the same launch the join screen's manual button does, straight into the
	   host's map. Spawn-at-friend then walks us to their side. */
	if (!mbHosting && mpInit->mpMapHandler->GetCurrentMapName().empty())
	{
		if (msPendingMap[0] == 0)
			return;
		Log(" multiplayer: host launched - entering '%s' from the menu\n",
			msPendingMap.c_str());
		mbApplyingRemoteMapChange = true;
		mpInit->mpGraphicsHelper->DrawLoadingScreen("");
		if (mpInit->mpMainMenu)
			mpInit->mpMainMenu->SetActive(false);
		mpInit->ResetGame(true);
		mpInit->mpGame->GetUpdater()->SetContainer("Default");
		mpInit->mpGame->GetScene()->SetDrawScene(true);
		mpInit->mpMapHandler->Load(msPendingMap, msPendingPos);
		mbApplyingRemoteMapChange = false;
		return;
	}

	const tString sCur = cString::ToLowerCase(
		cString::SetFileExt(mpInit->mpMapHandler->GetCurrentMapName(), ""));
	const tString sWant = cString::ToLowerCase(cString::SetFileExt(msPendingMap, ""));
	if (sCur == sWant)
		return; /* already there (or already heading there) */
	Log(" multiplayer: following the party to '%s' (start '%s')\n",
		msPendingMap.c_str(), msPendingPos.c_str());
	/* The fade + map changer run in the Default container, which is PAUSED
	   while the main menu overlays a running game — close it, or the follow
	   silently waits until the player backs out on their own. */
	if (mpInit->mpMainMenu && mpInit->mpMainMenu->IsActive())
		mpInit->mpMainMenu->SetActive(false);
	mbApplyingRemoteMapChange = true;
	mpInit->mpMapHandler->ChangeMap(msPendingMap, msPendingPos, "", "",
		0.6f, 0.6f, tString(""), tString(""));
	mbApplyingRemoteMapChange = false;
}

void cNetworkManager::JoinGame(const char *aszHostPort)
{
	if (!aszHostPort || !aszHostPort[0])
		return;

	Disconnect();
	if (!NetAcquire())
		return;

	char hbuf[260];
	uint16_t rport = mlDefaultPort;
	if (!SplitHostPort(aszHostPort, hbuf, sizeof(hbuf), rport))
	{
		Log(" multiplayer: bad join string '%s'\n", aszHostPort);
		NetRelease();
		return;
	}

	mpImpl->mpHost = enet_host_create(NULL, 1, 2, 0, 0);
	if (!mpImpl->mpHost)
	{
		Log(" multiplayer: client socket create failed\n");
		NetRelease();
		return;
	}

	ENetAddress remote;
	if (enet_address_set_host(&remote, hbuf) != 0)
	{
		Log(" multiplayer: resolve '%s' failed\n", hbuf);
		enet_host_destroy(mpImpl->mpHost);
		mpImpl->mpHost = NULL;
		mpImpl->mpServerPeer = NULL;
		NetRelease();
		return;
	}
	remote.port = rport;

	mpImpl->mpServerPeer = enet_host_connect(mpImpl->mpHost, &remote, 2, kNetConnectData);
	if (!mpImpl->mpServerPeer)
	{
		enet_host_destroy(mpImpl->mpHost);
		mpImpl->mpHost = NULL;
		NetRelease();
		return;
	}

	mbHosting = false;
	mbClientConnected = false;
	mbHadJoinPacket = false;
	mbSpawnedAtHost = false; /* fresh session: walk to the host once */
	mbGotVersionAck = false;
	mbAuthSent = false;          /* v15: answer the next challenge */
	mlGuestViolationsLogged = 0; /* v15: log-once per connection */
	msJoinFailReason = "";
	mbHavePendingMapChange = false;
	mbLocalMapChangeArmed = false;
	m_setTakenItems.clear(); /* one-of-each bookkeeping is per session */
	m_setPartyItems.clear();
	mlLocalPlayerId = 0;
	Log(" multiplayer: joining %s:%u ...\n", hbuf, (unsigned)rport);
}

void cNetworkManager::Disconnect()
{
	ClearGhostsInternal();
	if (mbHosting)
		SendMasterUnregister(); /* before the discovery socket goes away */
	if (mpVoice)
		mpVoice->Shutdown(); /* v16: mic closed, encoder/decoders/sources freed */
	CloseHostDiscovery();
	StopDiscovery(); /* keeps results; frees the browse socket + its net ref */
	StopInternetRefresh();

	if (mpImpl && mpImpl->mpHost)
	{
		if (!mbHosting && mpImpl->mpServerPeer &&
			mpImpl->mpServerPeer->state == ENET_PEER_STATE_CONNECTED)
			enet_peer_disconnect(mpImpl->mpServerPeer, 0);

		if (mbHosting)
		{
			for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
			{
				ENetPeer *rp = &mpImpl->mpHost->peers[i];
				if (rp->state == ENET_PEER_STATE_CONNECTED)
					enet_peer_disconnect(rp, 0);
			}
		}

		for (int flush = 0; flush < 12; ++flush)
			enet_host_service(mpImpl->mpHost, NULL, 20);

		enet_host_destroy(mpImpl->mpHost);
		mpImpl->mpHost = NULL;
		mpImpl->mpServerPeer = NULL;

		NetRelease();
	}

	mbHosting = false;
	mbClientConnected = false;
	mbHadJoinPacket = false;
	mlLocalPlayerId = 0;
	mlNextGuestId = 2;
	mvFreeGuestIds.clear(); /* v15 */
	mbAuthSent = false;
	if (mpImpl)
	{
		ResetPeerGuards();      /* v15: the peer slots died with the host */
		mpImpl->ResetIntent(); /* a dead session forwards nothing */
	}
	if (mpBodySync)
		mpBodySync->ClearGuestHeld();
}

void cNetworkManager::Update(float afTimeStep)
{
	if (!mpInit || !mpInit->mpGame)
		return;

	UpdatePartyEvents(afTimeStep); /* v13: feed line ages + join grace timer */

	/* Track the CURRENT world only — never a stale one. Keeping the old
	   pointer across a map change/unload meant EnsureGhost built ghosts in a
	   destroyed cWorld3D. The dying world already tore the ghost entities
	   down with it, so orphan (never destroy-through) the ghosts and let the
	   next state packet respawn them in the new world. */
	cWorld3D *w = mpInit->mpGame->GetScene()->GetWorld3D();
	if (w != mpWorld)
	{
		if (!m_mapGhosts.empty())
		{
			Log(" multiplayer: world changed — dropped %u ghost(s), they reappear on the next state packet\n",
				(unsigned)m_mapGhosts.size());
			for (tGhostMap::iterator it = m_mapGhosts.begin(); it != m_mapGhosts.end(); ++it)
			{
				it->second->OrphanWorld();
				hplDelete(it->second);
			}
			m_mapGhosts.clear();
		}
		DestroyPreviewGhost(true); /* same dead world; respawns after a short delay */
		mfPreviewSpawnDelay = 0.75f;
		mpWorld = w;
		/* v14: the new world pairs afresh — accept the next enemy batch
		   regardless of seq, and forget a snapshot meant for the old world
		   (our next MapReady requests a new one). */
		mbEnemySeqInKnown = false;
		ResetSnapshotBuffer();
	}

	/* Phase 5: cBodySync tracks the world itself (per-world state dies with
	   it) and takes the map-load census on the first frame the new world's
	   physics exists — the frame it computes one, the HOST announces it to
	   every connected peer (a guest self-verifies against the host's inside). */
	ApplyPendingMapChange(); /* party follow runs at this safe point */

	if (mpBodySync->Update(mpWorld))
	{
		if (mbHosting)
		{
			SendCensus(NULL);
			SendMapBeacon(NULL); /* save loads/new games never call ChangeMap */
		}
		/* The new map is up: our own transition (if any) has landed, and any
		   items a friend pocketed while we were elsewhere vanish before the
		   fade-in shows them. */
		mbLocalMapChangeArmed = false;
		ApplyTakenItems();
		/* v14 hook 1: our census for a NEW world is in and the host's is
		   already known (menu launch via beacon, a followed MapChange, our
		   own save reload) — ask the host for this world's state. Runs one
		   frame after the load, i.e. after OnStart/OnLoad/PreUpdate, which
		   is exactly what the host's state must overwrite. */
		if (!mbHosting && mpBodySync->HasRemoteCensus())
			SendMapReady();
	}

	/* v14: a snapshot whose End never comes (host died mid-send) must not
	   sit in memory forever, nor apply minutes later. */
	if (mbSnapBuffering)
	{
		mfSnapAge += afTimeStep;
		if (mfSnapAge > 10.0f)
		{
			Log(" multiplayer: world snapshot id=%u timed out (%u chunk(s) buffered) - dropped\n",
				(unsigned)mlSnapId, (unsigned)mvSnapChunks.size());
			ResetSnapshotBuffer();
		}
	}

	/* v15: auth timeouts, rate windows, strike decay, dead-slot sweep. */
	if (mbHosting)
		UpdatePeerGuards(afTimeStep);

	/* Rung 3: drive the guests' grab springs in the authoritative sim. */
	if (mbHosting)
		mpBodySync->UpdateRemoteGrabs(afTimeStep);
	/* Rung 4: a guest eases replicated bodies onto their latest received
	   states every frame — smooth at internet latency instead of 20 Hz
	   stepping — and predicts the body it is holding itself. */
	else
		mpBodySync->UpdateGuestBlend(afTimeStep);

	if (!msDeferredJoinAddress.empty() && !mbHosting)
	{
		hpl::tString dj = msDeferredJoinAddress;
		msDeferredJoinAddress = "";
		JoinGame(dj.c_str());
	}

	/* Non-blocking loops used Service(0); give ENet time to finish CONNECT / deliver reliable JOIN packet. */
	if (!mbHosting && mpImpl && mpImpl->mpHost && mpImpl->mpServerPeer)
	{
		const int st = (int)mpImpl->mpServerPeer->state;
		if (st == ENET_PEER_STATE_CONNECTING)
			Service(50);
		else if (st == ENET_PEER_STATE_CONNECTED && !mbHadJoinPacket && mlLocalPlayerId == 0)
			Service(35);
	}

	hpl::cInput *inp = mpInit->mpGame->GetInput();
	if (inp)
	{
		if (inp->BecameTriggerd("MultiplayerHost"))
		{
			if (mbHosting)
				Disconnect();
			else
				HostGame(mlDefaultPort);
		}
		if (inp->BecameTriggerd("MultiplayerJoinLocal"))
		{
			hpl::tString sj = "127.0.0.1:";
			sj += cString::ToString((int)mlDefaultPort);
			JoinGame(sj.c_str());
		}
		if (inp->BecameTriggerd("MultiplayerDiscover"))
			StartDiscovery();
	}

	PollDiscovery(afTimeStep);

	/* Public host: keep the master's entry alive (it expires after
	   kNetMasterExpirySeconds). No DNS here — the address was resolved in
	   HostGame / SetPublic; an unresolved master simply never beacons. */
	if (mbHosting && mbPublic)
	{
		mfMasterRegisterAccum += afTimeStep;
		if (mfMasterRegisterAccum >= kNetMasterRegisterSeconds)
		{
			mfMasterRegisterAccum = 0;
			SendMasterRegister();
		}
	}

	Service(0);
	const bool ticking = mbHosting || (mbClientConnected && mbHadJoinPacket && !mbHosting);
	mfSendAccum += afTimeStep;
	if (ticking)
	{
		while (mfSendAccum >= kSendPeriodSeconds)
		{
			mfSendAccum -= kSendPeriodSeconds;
			EmitLocalSnapshots();
			EmitObjectStates();   /* Phase 5: host->guests, awake dynamic bodies */
			EmitEnemyStates();    /* Phase 6: host->guests, the shared enemy roster */
			FlushIntentPackets(); /* rung 3: guest->host grab target / pushes */
			Service(0);
		}
	}
	Service(0);

	/* v11: every ghost interpolates/animates once per tick, AFTER the last
	   Service(0) so this tick's states are already in the buffers. */
	UpdateGhosts(afTimeStep);

	/* v16: voice after the ghosts moved (sources follow the drawn heads);
	   its outbox goes out right away, not at the 30 Hz tick — 40 ms of
	   audio per packet already paces it. */
	UpdateVoice(afTimeStep);

	mpBodySync->LogStatsTick(afTimeStep);
}

//-----------------------------------------------------------------------

void cNetworkManager::UpdateGhosts(float afTimeStep)
{
	for (tGhostMap::iterator it = m_mapGhosts.begin(); it != m_mapGhosts.end(); ++it)
	{
		if (it->second)
			it->second->Update(afTimeStep);
	}
	UpdatePreviewGhost(afTimeStep);
}

//-----------------------------------------------------------------------

void cNetworkManager::UpdatePreviewGhost(float afTimeStep)
{
	if (mbGhostPreview == false || !mpInit || !mpInit->mpGame)
		return;
	cPlayer *pPlayer = mpInit->mpPlayer;
	iCharacterBody *pBody = pPlayer ? pPlayer->GetCharacterBody() : NULL;
	cCamera3D *pCam = pPlayer ? pPlayer->GetCamera() : NULL;
	if (!mpWorld || !pBody || !pCam)
		return;

	if (mpPreviewGhost == NULL)
	{
		/* a fresh world: give the map its first frames so the player stands
		   at the real start before we measure "2 m ahead" */
		mfPreviewSpawnDelay -= afTimeStep;
		if (mfPreviewSpawnDelay > 0.0f)
			return;

		const float fYaw = pCam->GetYaw();
		const cVector3f vFwd(-sinf(fYaw), 0.0f, -cosf(fYaw));
		mvPreviewCenter = pBody->GetFeetPosition() + vFwd * 2.0f;
		mfPreviewFacingYaw = fYaw + kPif; /* faces the player */
		mfPreviewCircleAngle = 0.0f;
		mlPreviewSeq = 0;
		mfPreviewSendAccum = 0.0f;

		mpPreviewGhost = CreateGhost(kPreviewGhostId, (size_t)mlGhostPreviewModel);
		if (mpPreviewGhost == NULL)
		{
			mbGhostPreview = false;
			Log(" multiplayer: ghost preview could not create a ghost — disabled\n");
			return;
		}
		Log(" multiplayer: ghost preview spawned 2 m ahead (mesh #%d) — F6/F7 cycle clips, F8 crouch, F2 treadmill off/walk/run; ghost_anim_trace=1 logs the selector\n",
			mlGhostPreviewModel);
		if (mlPreviewClipIdx >= 0)
			mpPreviewGhost->DebugPlayClip(cGhostPlayer::GetClipName(mlPreviewClipIdx));
	}

	/* keys */
	hpl::cInput *inp = mpInit->mpGame->GetInput();
	if (inp)
	{
		const int lClipNum = cGhostPlayer::GetClipCount();
		int lDir = 0;
		if (inp->BecameTriggerd("GhostPreviewNext"))
			lDir = 1;
		else if (inp->BecameTriggerd("GhostPreviewPrev"))
			lDir = -1;
		if (lDir != 0 && lClipNum > 0)
		{
			/* step to the next LOADED clip (a clip whose file failed is skipped) */
			int lIdx = mlPreviewClipIdx;
			bool bSet = false;
			for (int lTry = 0; lTry < lClipNum && bSet == false; ++lTry)
			{
				lIdx = ((lIdx + lDir) % lClipNum + lClipNum) % lClipNum;
				bSet = mpPreviewGhost->DebugPlayClip(cGhostPlayer::GetClipName(lIdx));
			}
			if (bSet)
			{
				mlPreviewClipIdx = lIdx;
				if (mlPreviewTreadmill != 0)
					mlPreviewTreadmill = 0; /* a forced clip stands still */
				Log(" multiplayer: ghost preview clip '%s' (%d/%d)\n",
					cGhostPlayer::GetClipName(lIdx), lIdx + 1, lClipNum);
			}
			else
				Log(" multiplayer: ghost preview: no clips loaded on this mesh\n");
		}
		if (inp->BecameTriggerd("GhostPreviewCrouch"))
		{
			mbPreviewCrouch = !mbPreviewCrouch;
			Log(" multiplayer: ghost preview stance: %s\n", mbPreviewCrouch ? "crouch" : "stand");
		}
		if (inp->BecameTriggerd("GhostPreviewTreadmill"))
		{
			mlPreviewTreadmill = (mlPreviewTreadmill + 1) % 3;
			if (mlPreviewTreadmill != 0)
			{
				mlPreviewClipIdx = -1; /* the real selector picks the clips */
				mpPreviewGhost->DebugPlayClip("");
			}
			Log(" multiplayer: ghost preview treadmill: %s\n",
				mlPreviewTreadmill == 0 ? "off" : (mlPreviewTreadmill == 1 ? "walk (1 m circle)" : "run (1 m circle)"));
		}
	}

	/* Synthetic sender at the real send rate, through the real receive path:
	   seq clock, interpolation delay, selector, gait scaling — all exercised. */
	mfPreviewSendAccum += afTimeStep;
	while (mfPreviewSendAccum >= kSendPeriodSeconds)
	{
		mfPreviewSendAccum -= kSendPeriodSeconds;

		cVector3f vPos = mvPreviewCenter;
		float fYaw = mfPreviewFacingYaw;
		float fSpeed = 0.0f;
		uint8_t lFlags = eNetPlayerFlag_OnGround;
		uint8_t lMoveState = eNetMoveState_Walk;
		if (mbPreviewCrouch)
		{
			lFlags |= eNetPlayerFlag_Crouch;
			lMoveState = eNetMoveState_Crouch;
		}
		if (mlPreviewTreadmill != 0)
		{
			/* true player speeds from game.cfg via the player's own move
			   states (Movement_Walk/Run/Crouch ForwardSpeed) */
			const ePlayerMoveState eState = mbPreviewCrouch ? ePlayerMoveState_Crouch
				: (mlPreviewTreadmill == 2 ? ePlayerMoveState_Run : ePlayerMoveState_Walk);
			iPlayerMoveState *pMove = pPlayer->GetMoveStateData(eState);
			fSpeed = pMove ? pMove->mfForwardSpeed : 0.0f;
			if (fSpeed <= 0.05f)
				fSpeed = mbPreviewCrouch ? 0.9f : (mlPreviewTreadmill == 2 ? 3.0f : 1.5f);
			if (mlPreviewTreadmill == 2 && mbPreviewCrouch == false)
			{
				lFlags |= eNetPlayerFlag_RunKey;
				lMoveState = eNetMoveState_Run;
			}

			const float kRadius = 1.0f;
			mfPreviewCircleAngle += (fSpeed / kRadius) * kSendPeriodSeconds;
			if (mfPreviewCircleAngle > 2.0f * kPif)
				mfPreviewCircleAngle -= 2.0f * kPif;
			const float a = mfPreviewCircleAngle;
			vPos = mvPreviewCenter + cVector3f(cosf(a) * kRadius, 0.0f, sinf(a) * kRadius);
			/* facing = direction of travel (the circle's tangent) in the
			   camera yaw convention forward = (-sin y, 0, -cos y) */
			const float fDx = -sinf(a), fDz = cosf(a);
			fYaw = atan2f(-fDx, -fDz);
		}

		cNetPlayerState st;
		memset(&st, 0, sizeof(st));
		st.mType = eNetPacketType_PlayerState;
		st.mPlayerID = kPreviewGhostId;
		st.mSeq = ++mlPreviewSeq;
		st.mfPosX = vPos.x;
		st.mfPosY = vPos.y;
		st.mfPosZ = vPos.z;
		st.mfPitch = 0.0f;
		st.mfYaw = fYaw;
		st.mbFlashlightOn = 0;
		st.mMoveState = lMoveState;
		st.mVelFwd = EncodeNetVel(fSpeed);
		st.mVelRight = 0;
		st.mFlags = lFlags;
		st.mHealth = 100; /* v12: the preview is never "dead" */
		mpPreviewGhost->ApplyState(st);
	}

	mpPreviewGhost->Update(afTimeStep);
}

/** Same GetAdaptersInfo walk the discovery pinger does, but for HUMANS: the
    host lobby shows these so the host can read off the address to give a
    friend. VPN ranges are tagged by their well-known /8 (Hamachi ships 25.x,
    Radmin 26.x) — those are the ones that work across the internet. */
void cNetworkManager::GetLocalAddressLines(std::vector<hpl::tString> &avOut) const
{
	avOut.clear();

	ULONG sz = 0;
	if (GetAdaptersInfo(NULL, &sz) != ERROR_BUFFER_OVERFLOW || sz == 0)
		return;
	IP_ADAPTER_INFO *pInfo = (IP_ADAPTER_INFO *)malloc(sz);
	if (!pInfo)
		return;
	if (GetAdaptersInfo(pInfo, &sz) == NO_ERROR)
	{
		for (IP_ADAPTER_INFO *ad = pInfo; ad; ad = ad->Next)
		{
			for (IP_ADDR_STRING *ip = &ad->IpAddressList; ip; ip = ip->Next)
			{
				const uint32_t a = inet_addr(ip->IpAddress.String);
				if (a == 0 || a == INADDR_NONE)
					continue;
				const unsigned lFirst = ntohl(a) >> 24;
				if (lFirst == 127 || lFirst == 169) /* loopback / link-local */
					continue;

				const char *pKind = " (LAN)";
				if (lFirst == 25)
					pKind = " (Hamachi)";
				else if (lFirst == 26)
					pKind = " (Radmin)";
				else if (lFirst == 10 || lFirst == 192 || lFirst == 172)
					pKind = " (LAN)";
				else
					pKind = " (VPN/other)";

				hpl::tString sLine = ip->IpAddress.String;
				sLine += pKind;
				avOut.push_back(sLine);
				if (avOut.size() >= 6)
					break; /* the lobby has room for a handful */
			}
			if (avOut.size() >= 6)
				break;
		}
	}
	free(pInfo);
}

//-----------------------------------------------------------------------
// LAN / Hamachi discovery — raw UDP on kNetDiscoveryPort (see NetworkPackets.h
// for why this is a fixed side port and not SO_REUSEADDR on the ENet port).
//-----------------------------------------------------------------------

void cNetworkManager::OpenHostDiscovery()
{
	CloseHostDiscovery();
	if (!mpImpl)
		return;

	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
	{
		Log(" multiplayer: discovery socket create failed (err %d)\n", WSAGetLastError());
		return;
	}

	/* SO_REUSEADDR so two hosts on ONE machine can both answer: Windows hands a
	   *broadcast* datagram to every socket bound to the port with this flag.
	   Their pongs then differ by mlGamePort, so a browser lists both. */
	BOOL yes = TRUE;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&yes, sizeof(yes));

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_ANY);
	addr.sin_port = htons(kNetDiscoveryPort);
	if (bind(s, (const sockaddr *)&addr, sizeof(addr)) != 0)
	{
		Log(" multiplayer: discovery bind udp/%u failed (err %d) — hosting works but this server is not LAN-discoverable\n",
			(unsigned)kNetDiscoveryPort, WSAGetLastError());
		closesocket(s);
		return;
	}

	TameUdpSocket(s);
	mpImpl->mDiscoveryListenSock = s;
	Log(" multiplayer: discovery listening udp/%u\n", (unsigned)kNetDiscoveryPort);
}

void cNetworkManager::CloseHostDiscovery()
{
	if (mpImpl)
		CloseUdpSocket(mpImpl->mDiscoveryListenSock);
}

/** The browser-side socket is shared by the LAN scan and the master List so
    the two can overlap (Internet tab refresh while a LAN scan is running):
    PollDiscovery tells the replies apart by their type byte. It stays open
    while EITHER window is active and holds one ENet/WSA ref meanwhile. */
bool cNetworkManager::OpenBrowseSocket()
{
	if (!mpImpl)
		return false;
	if (mpImpl->mDiscoveryBrowseSock != INVALID_SOCKET)
		return true;

	if (!NetAcquire()) /* WSAStartup may not be up yet — browsing can start from the menu */
		return false;
	mpImpl->mbBrowseHoldsNetRef = true;

	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
	{
		Log(" multiplayer: discovery browse socket failed (err %d)\n", WSAGetLastError());
		CloseBrowseSocketIfIdle();
		return false;
	}

	BOOL yes = TRUE;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));

	/* Explicit ephemeral bind so pongs have a live return address immediately. */
	sockaddr_in local;
	memset(&local, 0, sizeof(local));
	local.sin_family = AF_INET;
	local.sin_addr.s_addr = htonl(INADDR_ANY);
	local.sin_port = 0;
	if (bind(s, (const sockaddr *)&local, sizeof(local)) != 0)
	{
		Log(" multiplayer: discovery browse bind failed (err %d)\n", WSAGetLastError());
		closesocket(s);
		CloseBrowseSocketIfIdle();
		return false;
	}

	TameUdpSocket(s);
	mpImpl->mDiscoveryBrowseSock = s;
	return true;
}

void cNetworkManager::CloseBrowseSocketIfIdle()
{
	if (!mpImpl)
		return;
	if (mbDiscoveryActive || mbInternetActive)
		return; /* the other window still needs it */
	CloseUdpSocket(mpImpl->mDiscoveryBrowseSock);
	if (mpImpl->mbBrowseHoldsNetRef)
	{
		mpImpl->mbBrowseHoldsNetRef = false;
		NetRelease();
	}
}

void cNetworkManager::StartDiscovery()
{
	if (!mpImpl)
		return;

	/* Refresh semantics: drop the previous scan's results and re-ping. */
	StopDiscovery();
	mvDiscovered.clear();

	if (!OpenBrowseSocket())
		return;
	mbDiscoveryActive = true;
	mfDiscoveryTimeLeft = kDiscoveryWindowSeconds;

	SendDiscoveryPings();
}

void cNetworkManager::StopDiscovery()
{
	mbDiscoveryActive = false;
	mfDiscoveryTimeLeft = 0;
	CloseBrowseSocketIfIdle();
}

//-----------------------------------------------------------------------
// Internet browser: master server List / Entries + host Register beacons.
// Wire structs: NetworkPackets.h 'MASTER SERVER protocol'.
//-----------------------------------------------------------------------

/** Turns cfg `master_server=host:port` into mpImpl->mMasterAddr. Blocking
    (DNS) — callers are HostGame, SetPublic and RefreshInternetServers, all
    user actions; the result is cached until the cfg string changes.
    Resolution goes through enet_address_set_host (the same resolver the
    direct-IP join uses; ENet needs to be initialised, which every caller
    guarantees). ENetAddress::host is already in network byte order. */
bool cNetworkManager::ResolveMasterAddress()
{
	if (!mpImpl)
		return false;

	const hpl::tString sMaster = GetMasterServer();
	if (sMaster.empty())
	{
		mpImpl->mbMasterResolved = false;
		mpImpl->msMasterResolvedFor = "";
		return false;
	}
	if (mpImpl->mbMasterResolved && mpImpl->msMasterResolvedFor == sMaster)
		return true;

	mpImpl->mbMasterResolved = false;
	mpImpl->msMasterResolvedFor = sMaster;

	/* "host" alone means kNetMasterDefaultPort; "host:port" must carry a
	   real port (SplitHostPort would silently fall back to the GAME port). */
	char hbuf[260];
	uint16_t port = kNetMasterDefaultPort;
	const char *pColon = strrchr(sMaster.c_str(), ':');
	if (pColon)
	{
		const int pv = atoi(pColon + 1);
		if (pv <= 0 || pv > 65535)
		{
			Log(" multiplayer: bad master_server '%s' (want host:port)\n", sMaster.c_str());
			return false;
		}
		port = (uint16_t)pv;
	}
	if (!SplitHostPort(sMaster.c_str(), hbuf, sizeof(hbuf), port))
	{
		Log(" multiplayer: bad master_server '%s' (want host:port)\n", sMaster.c_str());
		return false;
	}

	ENetAddress ea;
	memset(&ea, 0, sizeof(ea));
	if (enet_address_set_host(&ea, hbuf) != 0 || ea.host == 0)
	{
		if (!mpImpl->mbMasterWarned)
		{
			mpImpl->mbMasterWarned = true;
			Log(" multiplayer: master server '%s' does not resolve — set master_server=host:port in multiplayer.cfg (README 'Public servers')\n",
				sMaster.c_str());
		}
		return false;
	}

	memset(&mpImpl->mMasterAddr, 0, sizeof(mpImpl->mMasterAddr));
	mpImpl->mMasterAddr.sin_family = AF_INET;
	mpImpl->mMasterAddr.sin_addr.s_addr = ea.host; /* network order, as ENet keeps it */
	mpImpl->mMasterAddr.sin_port = htons(port);
	mpImpl->mbMasterResolved = true;
	mpImpl->mbMasterWarned = false;

	char where[64];
	FormatAddrPort(mpImpl->mMasterAddr, port, where, sizeof(where));
	Log(" multiplayer: master server '%s' -> %s\n", sMaster.c_str(), where);
	return true;
}

/** Host -> master beacon, from the host discovery socket (the one bound to
    kNetDiscoveryPort; if that bind failed there is nothing to send from and
    the server is simply not listed — same as not LAN-discoverable). */
void cNetworkManager::SendMasterRegister()
{
	if (!mpImpl || !mbHosting || !mbPublic || !mpImpl->mbMasterResolved)
		return;
	if (mpImpl->mDiscoveryListenSock == INVALID_SOCKET)
		return;

	cNetMasterRegister reg;
	memset(&reg, 0, sizeof(reg));
	reg.mType = eNetMasterPacketType_Register;
	reg.mlMagic = kNetMasterMagic;
	reg.mlMasterVer = kNetMasterProtocolVersion;
	reg.mlGamePort = mlListenPort;
	reg.mlPlayerCount = (uint8_t)(m_mapGhosts.size() + 1); /* guests + me */
	reg.mlMaxPlayers = mlMaxPlayers;
	reg.mFlags = HasServerPassword() ? kNetMasterFlag_Password : 0; /* v15 auth: the real setting */
	reg.mlProtocolVer = kNetProtocolVersion;
	{
		/* same advertised name as the LAN pong: server_name, else "<player_name>'s game" */
		hpl::tString sAdvertised = msServerName;
		if (sAdvertised.empty())
			sAdvertised = msPlayerName.empty() ? hpl::tString("Penumbra Server") : msPlayerName + "'s game";
		CopyPacketString(reg.msServerName, sizeof(reg.msServerName), sAdvertised.c_str());
	}
	const char *mapName = "";
	if (mpInit && mpInit->mpMapHandler)
		mapName = mpInit->mpMapHandler->GetCurrentMapName().c_str();
	CopyPacketString(reg.msMapName, sizeof(reg.msMapName), mapName);

	if (sendto(mpImpl->mDiscoveryListenSock, (const char *)&reg, sizeof(reg), 0,
			   (const sockaddr *)&mpImpl->mMasterAddr, sizeof(mpImpl->mMasterAddr)) == SOCKET_ERROR)
	{
		Log(" multiplayer: master register FAILED (err %d)\n", WSAGetLastError());
		return;
	}
	if (!mbMasterRegistered) /* fire-and-forget: the master never acks a beacon */
		Log(" multiplayer: master register beacon -> %s as '%s' game port %u (forward udp/%u on your router)\n",
			GetMasterServer().c_str(), reg.msServerName, (unsigned)reg.mlGamePort, (unsigned)reg.mlGamePort);
	mbMasterRegistered = true;
}

void cNetworkManager::SendMasterUnregister()
{
	if (!mpImpl || !mbMasterRegistered || !mpImpl->mbMasterResolved)
		return;
	mbMasterRegistered = false;
	if (mpImpl->mDiscoveryListenSock == INVALID_SOCKET)
		return;

	cNetMasterUnregister un;
	memset(&un, 0, sizeof(un));
	un.mType = eNetMasterPacketType_Unregister;
	un.mlMagic = kNetMasterMagic;
	un.mlMasterVer = kNetMasterProtocolVersion;
	un.mlGamePort = mlListenPort;
	sendto(mpImpl->mDiscoveryListenSock, (const char *)&un, sizeof(un), 0,
		   (const sockaddr *)&mpImpl->mMasterAddr, sizeof(mpImpl->mMasterAddr));
	Log(" multiplayer: unregistered from the master\n");
}

void cNetworkManager::SetPublic(bool abPublic)
{
	if (mbPublic == abPublic)
		return;
	mbPublic = abPublic;
	if (!mbHosting)
		return;
	if (mbPublic)
	{
		mfMasterRegisterAccum = 0;
		if (ResolveMasterAddress()) /* button click: blocking DNS is fine */
			SendMasterRegister();
	}
	else
		SendMasterUnregister();
}

void cNetworkManager::RefreshInternetServers()
{
	if (!mpImpl)
		return;

	StopInternetRefresh();
	mvInternet.clear();
	msInternetFailReason = "";

	if (GetMasterServer().empty())
	{
		msInternetFailReason = "No master server configured (multiplayer.cfg master_server=host:port)";
		Log(" multiplayer: internet refresh — %s\n", msInternetFailReason.c_str());
		return;
	}
	if (!OpenBrowseSocket())
	{
		msInternetFailReason = "Could not open a UDP socket";
		return;
	}
	if (!ResolveMasterAddress()) /* enet is up now (OpenBrowseSocket acquired it) */
	{
		msInternetFailReason = "Master server '" + GetMasterServer() + "' does not resolve";
		CloseBrowseSocketIfIdle();
		return;
	}

	cNetMasterList req;
	memset(&req, 0, sizeof(req));
	req.mType = eNetMasterPacketType_List;
	req.mlMagic = kNetMasterMagic;
	req.mlMasterVer = kNetMasterProtocolVersion;
	req.mlProtocolVer = kNetProtocolVersion;
	if (sendto(mpImpl->mDiscoveryBrowseSock, (const char *)&req, sizeof(req), 0,
			   (const sockaddr *)&mpImpl->mMasterAddr, sizeof(mpImpl->mMasterAddr)) == SOCKET_ERROR)
	{
		msInternetFailReason = "Sending to the master failed";
		Log(" multiplayer: master list request FAILED (err %d)\n", WSAGetLastError());
		CloseBrowseSocketIfIdle();
		return;
	}

	mbInternetActive = true;
	mfInternetTimeLeft = kNetMasterListWindowSeconds;
	Log(" multiplayer: master list request -> %s\n", GetMasterServer().c_str());
}

void cNetworkManager::StopInternetRefresh()
{
	mbInternetActive = false;
	mfInternetTimeLeft = 0;
	CloseBrowseSocketIfIdle();
}

/** One Entries datagram from the master (source already verified). Every
    bound is checked against alLen, never against the header's count alone. */
void cNetworkManager::HandleMasterEntries(const char *apBuf, int alLen)
{
	if (!apBuf || alLen < (int)sizeof(cNetMasterEntries))
		return;
	cNetMasterEntries hdr;
	memcpy(&hdr, apBuf, sizeof(hdr));
	if (hdr.mType != eNetMasterPacketType_Entries || hdr.mlMagic != kNetMasterMagic ||
		hdr.mlMasterVer != kNetMasterProtocolVersion)
		return;

	size_t count = hdr.mCount;
	const size_t avail = ((size_t)alLen - sizeof(hdr)) / sizeof(cNetMasterEntry);
	if (count > avail)
		count = avail; /* truncated datagram: take what really arrived */
	if (count > kNetMasterMaxEntriesPerDatagram)
		count = kNetMasterMaxEntriesPerDatagram;

	for (size_t i = 0; i < count; ++i)
	{
		cNetMasterEntry e;
		memcpy(&e, apBuf + sizeof(hdr) + i * sizeof(e), sizeof(e));
		e.msServerName[sizeof(e.msServerName) - 1] = '\0';
		e.msMapName[sizeof(e.msMapName) - 1] = '\0';

		char addr[64];
		_snprintf(addr, sizeof(addr), "%u.%u.%u.%u:%u",
				  (unsigned)e.mIp4[0], (unsigned)e.mIp4[1], (unsigned)e.mIp4[2], (unsigned)e.mIp4[3],
				  (unsigned)e.mlGamePort);
		addr[sizeof(addr) - 1] = '\0';

		bool known = false;
		for (size_t k = 0; k < mvInternet.size(); ++k)
			if (mvInternet[k].msAddress == addr)
			{
				known = true;
				break;
			}
		if (known || mvInternet.size() >= 100)
			continue;

		cDiscoveredServer sv;
		sv.msAddress = addr;
		sv.msName = e.msServerName;
		sv.msMap = e.msMapName;
		sv.mlPlayerCount = e.mlPlayerCount;
		sv.mlMaxPlayers = e.mlMaxPlayers;
		sv.mbVersionMatch = (e.mlProtocolVer == kNetProtocolVersion);
		sv.mbPassword = (e.mFlags & kNetMasterFlag_Password) != 0;
		sv.mlAgeSeconds = e.mlAgeSeconds;
		sv.mbInternet = true;
		mvInternet.push_back(sv);

		Log(" multiplayer: internet server '%s' map='%s' %u/%u at %s age %us%s%s\n",
			sv.msName.c_str(), sv.msMap.c_str(),
			(unsigned)sv.mlPlayerCount, (unsigned)sv.mlMaxPlayers,
			sv.msAddress.c_str(), (unsigned)sv.mlAgeSeconds,
			sv.mbPassword ? " [pw]" : "",
			sv.mbVersionMatch ? "" : " [VERSION MISMATCH]");
	}
}

void cNetworkManager::SendDiscoveryPings()
{
	if (!mpImpl || mpImpl->mDiscoveryBrowseSock == INVALID_SOCKET)
		return;

	cNetDiscoveryPing ping;
	ping.mType = eNetPacketType_DiscoveryPing;
	ping.mlProtocolMagic = kNetProtocolMagic;
	ping.mlProtocolVer = kNetProtocolVersion;

	std::vector<uint32_t> sentTo; /* network-order addrs already pinged */
	sockaddr_in dst;
	memset(&dst, 0, sizeof(dst));
	dst.sin_family = AF_INET;
	dst.sin_port = htons(kNetDiscoveryPort);

	struct SendOnce
	{
		SOCKET s;
		const cNetDiscoveryPing *p;
		std::vector<uint32_t> *seen;
		void operator()(sockaddr_in &d, uint32_t netAddr) const
		{
			for (size_t i = 0; i < seen->size(); ++i)
				if ((*seen)[i] == netAddr)
					return;
			seen->push_back(netAddr);
			d.sin_addr.s_addr = netAddr;
			sendto(s, (const char *)p, sizeof(*p), 0, (const sockaddr *)&d, sizeof(d));
			char where[64];
			FormatAddrPort(d, kNetDiscoveryPort, where, sizeof(where));
			Log(" multiplayer: discovery ping -> %s\n", where);
		}
	};
	SendOnce send1 = { mpImpl->mDiscoveryBrowseSock, &ping, &sentTo };

	/* 1) Global broadcast — covers the common single-NIC LAN case. */
	send1(dst, htonl(INADDR_BROADCAST));

	/* 2) Loopback — Windows does not reliably loop 255.255.255.255 back to the
	      sending machine, and two-instances-on-one-PC is the main test setup. */
	send1(dst, inet_addr("127.0.0.1"));

	/* 3) Per-interface directed broadcast (ip|~mask). THIS is what makes
	      Hamachi/Radmin/ZeroTier work: they are their own interface (e.g.
	      25.x.x.x/8) and the global broadcast usually picks a different NIC. */
	ULONG sz = 0;
	if (GetAdaptersInfo(NULL, &sz) == ERROR_BUFFER_OVERFLOW && sz > 0)
	{
		IP_ADAPTER_INFO *pInfo = (IP_ADAPTER_INFO *)malloc(sz);
		if (pInfo)
		{
			if (GetAdaptersInfo(pInfo, &sz) == NO_ERROR)
			{
				for (IP_ADAPTER_INFO *ad = pInfo; ad; ad = ad->Next)
				{
					for (IP_ADDR_STRING *ip = &ad->IpAddressList; ip; ip = ip->Next)
					{
						uint32_t a = inet_addr(ip->IpAddress.String);
						uint32_t m = inet_addr(ip->IpMask.String);
						if (a == 0 || a == INADDR_NONE || m == 0)
							continue; /* down / DHCP-less adapter */
						if ((ntohl(a) >> 24) == 127)
							continue; /* loopback already pinged */
						send1(dst, (a & m) | ~m);
					}
				}
			}
			free(pInfo);
		}
	}
}

void cNetworkManager::PollDiscovery(float afTimeStep)
{
	if (!mpImpl)
		return;

	/* --- Host: answer pings with live state ------------------------------ */
	if (mbHosting && mpImpl->mDiscoveryListenSock != INVALID_SOCKET)
	{
		/* v15: age the reflector limiter windows (see DiscoveryPongAllowed) */
		mpImpl->mfPongGlobalWindow += afTimeStep;
		if (mpImpl->mfPongGlobalWindow >= 1.0f)
		{
			mpImpl->mfPongGlobalWindow = 0;
			mpImpl->mlPongGlobalCount = 0;
		}
		for (int b = 0; b < Impl::kPongBuckets; ++b)
			if (mpImpl->mPong[b].mfWindowLeft > 0)
				mpImpl->mPong[b].mfWindowLeft -= afTimeStep;

		for (;;)
		{
			char buf[128];
			sockaddr_in from;
			int fromLen = sizeof(from);
			int n = recvfrom(mpImpl->mDiscoveryListenSock, buf, sizeof(buf), 0,
							 (sockaddr *)&from, &fromLen);
			if (n < 0)
			{
				int e = WSAGetLastError();
				if (e == WSAECONNRESET || e == WSAEMSGSIZE)
					continue; /* stray ICMP / oversized noise — keep draining */
				if (e != WSAEWOULDBLOCK)
					Log(" multiplayer: discovery recv error %d\n", e);
				break;        /* WSAEWOULDBLOCK: drained */
			}
			if (n != (int)sizeof(cNetDiscoveryPing))
				continue;
			cNetDiscoveryPing ping;
			memcpy(&ping, buf, sizeof(ping));
			if (ping.mType != eNetPacketType_DiscoveryPing ||
				ping.mlProtocolMagic != kNetProtocolMagic ||
				ping.mlProtocolVer != kNetProtocolVersion)
				continue; /* v15: a reflector on the open internet answers ONLY
				             its own protocol; a mismatched browser no longer
				             gets a (greyed-out) row, it simply does not see us */
			if (!DiscoveryPongAllowed((uint32_t)from.sin_addr.s_addr))
				continue; /* v15: per-source + global pong rate limit */

			cNetDiscoveryPong pong;
			memset(&pong, 0, sizeof(pong));
			pong.mType = eNetPacketType_DiscoveryPong;
			pong.mlProtocolMagic = kNetProtocolMagic;
			pong.mlProtocolVer = kNetProtocolVersion;
			pong.mlGamePort = mlListenPort;
			pong.mlPlayerCount = (uint8_t)(m_mapGhosts.size() + 1); /* guests + me */
			pong.mlMaxPlayers = mlMaxPlayers;
			{
				/* v13: no server_name -> "<player_name>'s game" (or the old default) */
				hpl::tString sAdvertised = msServerName;
				if (sAdvertised.empty())
					sAdvertised = msPlayerName.empty() ? hpl::tString("Penumbra Server") : msPlayerName + "'s game";
				CopyPacketString(pong.msServerName, sizeof(pong.msServerName), sAdvertised.c_str());
			}
			const char *mapName = "";
			if (mpInit && mpInit->mpMapHandler)
				mapName = mpInit->mpMapHandler->GetCurrentMapName().c_str();
			CopyPacketString(pong.msMapName, sizeof(pong.msMapName), mapName);

			char who[64];
			FormatAddrPort(from, (uint16_t)ntohs(from.sin_port), who, sizeof(who));
			if (sendto(mpImpl->mDiscoveryListenSock, (const char *)&pong, sizeof(pong), 0,
					   (const sockaddr *)&from, fromLen) == SOCKET_ERROR)
				Log(" multiplayer: discovery pong to %s FAILED (err %d)\n", who, WSAGetLastError());
			else
				Log(" multiplayer: discovery ping from %s — pong'd '%s' %u/%u\n",
					who, pong.msServerName,
					(unsigned)pong.mlPlayerCount, (unsigned)pong.mlMaxPlayers);
		}
	}

	/* --- Browser: collect pongs / master entries until the windows close -- */
	if (!(mbDiscoveryActive || mbInternetActive) || mpImpl->mDiscoveryBrowseSock == INVALID_SOCKET)
		return;

	for (;;)
	{
		char buf[1024]; /* a full master Entries datagram is 778 bytes */
		sockaddr_in from;
		int fromLen = sizeof(from);
		int n = recvfrom(mpImpl->mDiscoveryBrowseSock, buf, sizeof(buf), 0,
						 (sockaddr *)&from, &fromLen);
		if (n < 0)
		{
			int e = WSAGetLastError();
			if (e == WSAECONNRESET || e == WSAEMSGSIZE)
				continue;
			break;
		}
		if (n < 1)
			continue;

		/* Master reply? Only from the address we asked — anything else on
		   this socket claiming to be the master is dropped unread. */
		if ((uint8_t)buf[0] == eNetMasterPacketType_Entries)
		{
			if (mbInternetActive && mpImpl->mbMasterResolved &&
				from.sin_addr.s_addr == mpImpl->mMasterAddr.sin_addr.s_addr &&
				from.sin_port == mpImpl->mMasterAddr.sin_port)
				HandleMasterEntries(buf, n);
			continue;
		}

		if (n != (int)sizeof(cNetDiscoveryPong))
			continue;
		cNetDiscoveryPong pong;
		memcpy(&pong, buf, sizeof(pong));
		if (pong.mType != eNetPacketType_DiscoveryPong ||
			pong.mlProtocolMagic != kNetProtocolMagic)
			continue;

		char addr[64];
		FormatAddrPort(from, pong.mlGamePort, addr, sizeof(addr));

		bool known = false;
		for (size_t i = 0; i < mvDiscovered.size(); ++i)
			if (mvDiscovered[i].msAddress == addr)
			{
				known = true;
				break;
			}
		if (known || mvDiscovered.size() >= 32)
			continue;

		pong.msServerName[sizeof(pong.msServerName) - 1] = '\0';
		pong.msMapName[sizeof(pong.msMapName) - 1] = '\0';

		cDiscoveredServer sv;
		sv.msAddress = addr;
		sv.msName = pong.msServerName;
		sv.msMap = pong.msMapName;
		sv.mlPlayerCount = pong.mlPlayerCount;
		sv.mlMaxPlayers = pong.mlMaxPlayers;
		sv.mbVersionMatch = (pong.mlProtocolVer == kNetProtocolVersion);
		sv.mbPassword = false; /* LAN pongs predate the flag; the host refuses a wrong password anyway */
		sv.mlAgeSeconds = 0;
		sv.mbInternet = false;
		mvDiscovered.push_back(sv);

		Log(" multiplayer: discovered '%s' map='%s' %u/%u at %s%s\n",
			sv.msName.c_str(), sv.msMap.c_str(),
			(unsigned)sv.mlPlayerCount, (unsigned)sv.mlMaxPlayers,
			sv.msAddress.c_str(), sv.mbVersionMatch ? "" : " [VERSION MISMATCH]");
	}

	if (mbDiscoveryActive)
	{
		mfDiscoveryTimeLeft -= afTimeStep;
		if (mfDiscoveryTimeLeft <= 0)
		{
			Log(" multiplayer: discovery done — %u server(s)\n", (unsigned)mvDiscovered.size());
			StopDiscovery();
		}
	}
	if (mbInternetActive)
	{
		mfInternetTimeLeft -= afTimeStep;
		if (mfInternetTimeLeft <= 0)
		{
			Log(" multiplayer: internet list done — %u server(s)\n", (unsigned)mvInternet.size());
			StopInternetRefresh();
		}
	}
}

//-----------------------------------------------------------------------
// Enemy senses (host): appended accessor — see NetworkManager.h tail.
//-----------------------------------------------------------------------

bool cNetworkManager::GetGhostSense(uint8_t alId, hpl::cVector3f *apCamPos, uint8_t *apMoveState) const
{
	if (!mbHosting)
		return false; /* only the host's AI has any business asking */
	tGhostMap::const_iterator it = m_mapGhosts.find(alId);
	if (it == m_mapGhosts.end() || !it->second)
		return false; /* disconnected (DropRemotePlayer) / never joined */
	cVector3f v;
	if (!it->second->GetLastStatePos(&v))
		return false; /* no state yet */
	if (apCamPos)
		*apCamPos = v;
	if (apMoveState)
	{
		std::map<uint8_t, uint8_t>::const_iterator mi = m_mapGhostMoveState.find(alId);
		*apMoveState = (mi != m_mapGhostMoveState.end()) ? mi->second : (uint8_t)eNetMoveState_Run;
	}
	return true;
}

//-----------------------------------------------------------------------
// v12 party health (both roles): appended accessors — see NetworkManager.h tail.
//-----------------------------------------------------------------------

bool cNetworkManager::GetGhostHealth(uint8_t alId, float *apHealth) const
{
	if (alId == 0 || alId == mlLocalPlayerId)
		return false;
	std::map<uint8_t, uint8_t>::const_iterator it = m_mapGhostHealth.find(alId);
	if (it == m_mapGhostHealth.end())
		return false; /* disconnected (DropRemotePlayer) / no state yet */
	if (apHealth)
		*apHealth = (float)it->second;
	return true;
}

void cNetworkManager::GetPartyStatus(std::vector<cNetPartyMember> &avOut) const
{
	avOut.clear();
	if (!mbHosting && !(mbClientConnected && mbHadJoinPacket))
		return;
	for (std::map<uint8_t, uint8_t>::const_iterator it = m_mapGhostHealth.begin();
		it != m_mapGhostHealth.end(); ++it)
	{
		if (it->first == 0 || it->first == mlLocalPlayerId || it->first == kPreviewGhostId)
			continue;
		cNetPartyMember m;
		m.mlId = it->first;
		m.mfHealth = (float)it->second;
		tGhostMap::const_iterator gi = m_mapGhosts.find(it->first);
		if (gi != m_mapGhosts.end() && gi->second)
		{
			m.mbHasFeetPos = gi->second->GetLastFeetPos(&m.mvFeetPos);
			m.mbHasRenderPos = gi->second->GetRenderFeetPos(&m.mvRenderFeetPos);
		}
		avOut.push_back(m);
	}
}

bool cNetworkManager::IsSessionLive() const
{
	if (mbHosting)
		return GetConnectedGuestCount() > 0;
	return mbClientConnected && mbHadJoinPacket;
}

//-----------------------------------------------------------------------
// v14 world snapshot (late join / reconnect / host save+load): appended —
// see NetworkManager.h tail and multiplayer/README.md "World snapshot".
//-----------------------------------------------------------------------

namespace
{
/** RAII: gbNetScriptApplying for the whole apply, restored on every path
    (the script hooks, NetOnScriptEvent, NetOnEntityDamaged and the engine
    var callback all go silent, so nothing we set echoes back to the host). */
struct cNetApplyScope
{
	bool mbPrev;
	cNetApplyScope() : mbPrev(gbNetScriptApplying) { gbNetScriptApplying = true; }
	~cNetApplyScope() { gbNetScriptApplying = mbPrev; }
private:
	cNetApplyScope(const cNetApplyScope &);
	cNetApplyScope &operator=(const cNetApplyScope &);
};

/** MapReady identity: lowercase, extension-stripped map name (the same
    normalisation QualifiedItemHash uses). */
static uint32_t SnapMapNameHash(const tString &asMap)
{
	return NetHashName(cString::ToLowerCase(cString::SetFileExt(asMap, "")).c_str());
}

/** Host-side chunk writer. Entries of ONE section are appended and split
    into chunks of at most kNetSnapMaxChunkPayload payload bytes; every
    section yields at least one chunk (mCount may be 0) so the receiver can
    tell "the host has none" from "the section never arrived". */
class cSnapWriter
{
public:
	cSnapWriter(uint8_t alGen, uint8_t alId)
		: mlGen(alGen), mlId(alId), mlSection(0), mbOpen(false), mlCount(0) {}

	void Begin(uint8_t alSection)
	{
		Flush();
		mlSection = alSection;
		mbOpen = true;
		mlCount = 0;
		mvCur.clear();
	}

	void Add(const void *apEntry, size_t alSize)
	{
		if (!mbOpen || apEntry == NULL || alSize == 0 || alSize > kNetSnapMaxChunkPayload)
			return;
		if (mlCount > 0 && mvCur.size() + alSize > (size_t)kNetSnapMaxChunkPayload)
		{
			const uint8_t lSection = mlSection;
			Flush();
			mlSection = lSection;
			mbOpen = true;
		}
		const unsigned char *p = (const unsigned char *)apEntry;
		mvCur.insert(mvCur.end(), p, p + alSize);
		++mlCount;
	}

	void Flush()
	{
		if (!mbOpen)
			return;
		cNetSnapshotHdr hdr;
		hdr.mType = eNetPacketType_WorldSnapshot;
		hdr.mSection = mlSection;
		hdr.mMapGen = mlGen;
		hdr.mSnapId = mlId;
		hdr.mCount = (uint16_t)mlCount; /* <= 300 per chunk (4 B entries) */
		std::vector<uint8_t> chunk(sizeof(hdr) + mvCur.size());
		memcpy(&chunk[0], &hdr, sizeof(hdr));
		if (!mvCur.empty())
			memcpy(&chunk[sizeof(hdr)], &mvCur[0], mvCur.size());
		mvChunks.push_back(chunk);
		mvCur.clear();
		mlCount = 0;
		mbOpen = false;
	}

	std::vector<std::vector<uint8_t> > mvChunks;

private:
	uint8_t mlGen, mlId, mlSection;
	bool mbOpen;
	size_t mlCount;
	std::vector<uint8_t> mvCur;
};

/** Guest side: header + whole entries of one buffered chunk. Returns the
    entry count actually present (mCount clamped to what the packet holds). */
static size_t SnapChunkEntries(const std::vector<uint8_t> &avChunk, size_t alEntrySize,
	cNetSnapshotHdr *apHdr, const unsigned char **appFirst)
{
	if (avChunk.size() < sizeof(cNetSnapshotHdr) || alEntrySize == 0)
		return 0;
	memcpy(apHdr, &avChunk[0], sizeof(cNetSnapshotHdr));
	*appFirst = &avChunk[0] + sizeof(cNetSnapshotHdr);
	size_t lCount = apHdr->mCount;
	const size_t lWhole = (avChunk.size() - sizeof(cNetSnapshotHdr)) / alEntrySize;
	if (lCount > lWhole)
		lCount = lWhole; /* truncated / corrupt length: whole entries only */
	return lCount;
}

/** Wire floats are untrusted: NaN/inf must never reach a body or a health. */
static bool SnapFinite(float afX)
{
	return afX == afX && afX <= 3.0e38f && afX >= -3.0e38f;
}

/** Entity types gameplay DESTROYS (picked items, broken objects): absent on
    the host = gone for the party. Areas/links/ladders are never destroyed
    by gameplay, so their absence means a map mismatch, not state. */
static bool SnapTypeIsDestroyable(eGameEntityType aType)
{
	return aType == eGameEntityType_Item || aType == eGameEntityType_Object ||
		aType == eGameEntityType_SwingDoor || aType == eGameEntityType_Door ||
		aType == eGameEntityType_DoorPanel || aType == eGameEntityType_Lamp;
}
} // namespace

//-----------------------------------------------------------------------

void cNetworkManager::ResetSnapshotBuffer()
{
	mvSnapChunks.clear();
	mbSnapBuffering = false;
	mfSnapAge = 0;
	mlSnapId = 0;
	mlSnapGen = 0;
}

//-----------------------------------------------------------------------

void cNetworkManager::SendMapReady()
{
	if (mbHosting || !mpImpl || !mpImpl->mpHost || !mpImpl->mpServerPeer ||
		mpImpl->mpServerPeer->state != ENET_PEER_STATE_CONNECTED)
		return;
	if (!mpBodySync->HasCensus() || !mpBodySync->HasRemoteCensus())
		return; /* paired = both censuses known for OUR current world */
	if (!mpInit || !mpInit->mpMapHandler)
		return;
	const tString sMap = mpInit->mpMapHandler->GetCurrentMapName();
	if (sMap.empty())
		return; /* menu: no world to be ready in */

	cNetBodyCensus mine;
	mpBodySync->BuildCensusPacket(&mine);

	cNetMapReady pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.mType = eNetPacketType_MapReady;
	pkt.mMapGen = mpBodySync->GetRemoteMapGen();
	pkt.mlMapNameHash = SnapMapNameHash(sMap);
	pkt.mlLocalBodyCount = mine.mlBodyCount;
	pkt.mlLocalChecksum = mine.mlChecksum;
	SendStructToPeer(mpImpl->mpServerPeer, &pkt, sizeof(pkt), true);
	Log(" multiplayer: MapReady sent gen=%u map '%s' (census %u/0x%08X)\n",
		(unsigned)pkt.mMapGen, sMap.c_str(), (unsigned)mine.mlBodyCount, mine.mlChecksum);
}

//-----------------------------------------------------------------------

void cNetworkManager::HandleMapReady(ENetPeer *apPeer, const void *apData, size_t alLen)
{
	if (!mbHosting || !apPeer || !apData || alLen < sizeof(cNetMapReady))
		return;
	cNetMapReady mr;
	memcpy(&mr, apData, sizeof(mr));
	const unsigned lGuest = (unsigned)PeerGetId(apPeer);

	if (mr.mMapGen != mpBodySync->GetMapGen())
	{
		/* paired with a world we already left (our reload/transition): the
		   census for the new one is on its way; it will ask again */
		Log(" multiplayer: guest %u ready on STALE gen %u (ours %u) - ignored\n",
			lGuest, (unsigned)mr.mMapGen, (unsigned)mpBodySync->GetMapGen());
		return;
	}
	if (!mpInit || !mpInit->mpMapHandler || !mpInit->mpGame)
		return;
	const tString sMap = mpInit->mpMapHandler->GetCurrentMapName();
	if (sMap.empty() || mr.mlMapNameHash != SnapMapNameHash(sMap))
	{
		/* standing on another map: the beacon already told it where to go;
		   it sends MapReady again once it has followed */
		Log(" multiplayer: guest %u ready on another map (hash 0x%08X, ours '%s') - no snapshot\n",
			lGuest, (unsigned)mr.mlMapNameHash, sMap.c_str());
		return;
	}
	if (mpInit->mpGame->GetScene() == NULL || mpInit->mpGame->GetScene()->GetWorld3D() == NULL ||
		!mpBodySync->HasCensus())
		return;

	cNetBodyCensus mine;
	mpBodySync->BuildCensusPacket(&mine);
	Log(" multiplayer: guest %u ready on gen %u (census %u/0x%08X vs ours %u/0x%08X)\n",
		lGuest, (unsigned)mr.mMapGen, (unsigned)mr.mlLocalBodyCount, mr.mlLocalChecksum,
		(unsigned)mine.mlBodyCount, mine.mlChecksum);
	SendWorldSnapshot(apPeer);
}

//-----------------------------------------------------------------------

int cNetworkManager::SendBodySnapshot(ENetPeer *apPeer, size_t *apBytesOut)
{
	if (!mbHosting || !apPeer || apPeer->state != ENET_PEER_STATE_CONNECTED)
		return 0;
	/* Every replicable body, resting ones flagged Sleeping (the guest pins
	   those and disables the twin); primes m_mapSent so the delta path does
	   not resend what this just carried. Reliable ch0: ordered with the
	   snapshot sections around it. */
	unsigned char aSnapBuf[cBodySync::kMaxBatchBytes];
	uint32_t lCursor = 0;
	int lChunks = 0;
	size_t lLen;
	while ((lLen = mpBodySync->BuildSnapshotChunk(aSnapBuf, &lCursor)) != 0)
	{
		SendStructToPeer(apPeer, aSnapBuf, lLen, true);
		++lChunks;
		if (apBytesOut)
			*apBytesOut += lLen;
	}
	return lChunks;
}

//-----------------------------------------------------------------------

void cNetworkManager::SendWorldSnapshot(ENetPeer *apPeer)
{
	if (!mbHosting || !apPeer || apPeer->state != ENET_PEER_STATE_CONNECTED)
		return;
	if (!mpInit || !mpInit->mpMapHandler || !mpInit->mpGame)
		return;
	cScene *pScene = mpInit->mpGame->GetScene();
	if (pScene == NULL || pScene->GetWorld3D() == NULL)
		return;

	const uint8_t lGen = mpBodySync->GetMapGen();
	const uint8_t lId = ++mlSnapIdOut;
	cSnapWriter w(lGen, lId);
	int lVarsL = 0, lVarsG = 0, lEnts = 0, lTaken = 0, lParty = 0, lEnemies = 0, lTimers = 0, lSkipped = 0;

	/* a. script vars — local, then global. The scene keys them lowercase;
	   msName keeps the script's spelling (what CreateLocalVar wants back). */
	for (int lPass = 0; lPass < 2; ++lPass)
	{
		w.Begin(lPass == 0 ? (uint8_t)eNetSnap_LocalVar : (uint8_t)eNetSnap_GlobalVar);
		tScriptVarMap *pMap = lPass == 0 ? pScene->GetLocalVarMap() : pScene->GetGlobalVarMap();
		if (pMap == NULL)
			continue;
		for (tScriptVarMapIt it = pMap->begin(); it != pMap->end(); ++it)
		{
			const tString &sName = it->second.msName.empty() ? it->first : it->second.msName;
			if (sName.empty())
				continue;
			if (sName.size() >= sizeof(cNetSnapVar().msName))
			{
				Log(" multiplayer: snapshot: var name '%s' too long - skipped\n", sName.c_str());
				++lSkipped;
				continue;
			}
			cNetSnapVar e;
			memset(&e, 0, sizeof(e));
			strncpy(e.msName, sName.c_str(), sizeof(e.msName) - 1);
			e.mlVal = (int32_t)it->second.mlVal;
			w.Add(&e, sizeof(e));
			if (lPass == 0) ++lVarsL; else ++lVarsG;
		}
	}

	/* b/c/g1. every non-enemy game entity: presence, active, lock, lit,
	   health. Unqualified name hash (the generation scopes the map);
	   duplicate names (a multimap) keep the first, like the body index. */
	{
		w.Begin(eNetSnap_Entity);
		std::set<uint32_t> setSeen;
		tGameEntityIterator it = mpInit->mpMapHandler->GetGameEntityIterator();
		while (it.HasNext())
		{
			iGameEntity *pEnt = it.Next();
			if (pEnt == NULL || pEnt->GetType() == eGameEntityType_Enemy || pEnt->GetName().empty())
				continue;
			const uint32_t lHash = NetHashName(pEnt->GetName().c_str());
			if (!setSeen.insert(lHash).second)
			{
				Log(" multiplayer: snapshot: duplicate entity name '%s' - second skipped\n",
					pEnt->GetName().c_str());
				++lSkipped;
				continue;
			}
			cNetSnapEntity e;
			memset(&e, 0, sizeof(e));
			e.mlNameHash = lHash;
			e.mType = (uint8_t)pEnt->GetType();
			e.mFlags = pEnt->IsActive() ? kNetSnapEntityFlag_Active : 0;
			if (pEnt->GetType() == eGameEntityType_SwingDoor &&
				static_cast<cGameSwingDoor *>(pEnt)->IsLocked())
				e.mFlags |= kNetSnapEntityFlag_Locked;
			if (pEnt->GetType() == eGameEntityType_Lamp &&
				static_cast<cGameLamp *>(pEnt)->IsLit())
				e.mFlags |= kNetSnapEntityFlag_Lit;
			e.mfHealth = pEnt->GetHealth();
			w.Add(&e, sizeof(e));
			++lEnts;
		}
	}

	/* d. taken items (whole session, qualified map:name hashes — after 5a
	   our own picks are in here too) and the party inventory (our items +
	   what the other guests told us they hold). */
	{
		w.Begin(eNetSnap_TakenItem);
		for (std::set<uint32_t>::const_iterator it = m_setTakenItems.begin(); it != m_setTakenItems.end(); ++it)
		{
			const uint32_t lHash = *it;
			w.Add(&lHash, sizeof(lHash));
			++lTaken;
		}

		w.Begin(eNetSnap_PartyItem);
		std::set<tString> setParty(m_setPartyItems);
		if (mpInit->mpInventory)
		{
			for (tInventoryItemMapIt it = mpInit->mpInventory->m_mapItems.begin();
				it != mpInit->mpInventory->m_mapItems.end(); ++it)
			{
				if (it->second)
					setParty.insert(it->second->GetName());
			}
		}
		for (std::set<tString>::const_iterator it = setParty.begin(); it != setParty.end(); ++it)
		{
			char aName[32];
			if (it->empty())
				continue;
			if (it->size() >= sizeof(aName))
			{
				Log(" multiplayer: snapshot: item name '%s' too long - skipped\n", it->c_str());
				++lSkipped;
				continue;
			}
			memset(aName, 0, sizeof(aName));
			strncpy(aName, it->c_str(), sizeof(aName) - 1);
			w.Add(aName, sizeof(aName));
			++lParty;
		}
	}

	/* f. enemy roster: the same entries the stream carries. */
	{
		w.Begin(eNetSnap_Enemy);
		tGameEnemyIterator it = mpInit->mpMapHandler->GetGameEnemyIterator();
		while (it.HasNext())
		{
			cNetEnemyState st;
			if (!FillEnemyState(it.Next(), &st))
				continue;
			w.Add(&st, sizeof(st));
			++lEnemies;
		}
	}

	/* g2. LOCAL timers (global ones outlive the map and are not its state). */
	{
		w.Begin(eNetSnap_Timer);
		for (tGameTimerListIt it = mpInit->mpMapHandler->mlstTimers.begin();
			it != mpInit->mpMapHandler->mlstTimers.end(); ++it)
		{
			cGameTimer *pTimer = *it;
			if (pTimer == NULL || pTimer->mbGlobal || pTimer->mbDeleteMe)
				continue;
			if (pTimer->msName.empty() ||
				pTimer->msName.size() >= sizeof(cNetSnapTimer().msName) ||
				pTimer->msCallback.size() >= sizeof(cNetSnapTimer().msCallback))
			{
				Log(" multiplayer: snapshot: timer '%s' name/callback too long - skipped\n",
					pTimer->msName.c_str());
				++lSkipped;
				continue;
			}
			cNetSnapTimer e;
			memset(&e, 0, sizeof(e));
			strncpy(e.msName, pTimer->msName.c_str(), sizeof(e.msName) - 1);
			strncpy(e.msCallback, pTimer->msCallback.c_str(), sizeof(e.msCallback) - 1);
			e.mfTime = pTimer->mfTime;
			e.mbPaused = pTimer->mbPaused ? 1 : 0;
			w.Add(&e, sizeof(e));
			++lTimers;
		}
		w.Flush();
	}

	/* Send: Begin, the section chunks, the body poses (ObjectState chunks,
	   same reliable channel = same order), End. */
	size_t lBytes = 0;
	cNetSnapshotHdr hdr;
	hdr.mType = eNetPacketType_WorldSnapshot;
	hdr.mSection = eNetSnap_Begin;
	hdr.mMapGen = lGen;
	hdr.mSnapId = lId;
	hdr.mCount = (uint16_t)(w.mvChunks.size() > 65535 ? 65535 : w.mvChunks.size());
	SendStructToPeer(apPeer, &hdr, sizeof(hdr), true);
	lBytes += sizeof(hdr);

	for (size_t i = 0; i < w.mvChunks.size(); ++i)
	{
		SendStructToPeer(apPeer, &w.mvChunks[i][0], w.mvChunks[i].size(), true);
		lBytes += w.mvChunks[i].size();
	}

	const int lBodyChunks = SendBodySnapshot(apPeer, &lBytes);

	hdr.mSection = eNetSnap_End;
	hdr.mCount = (uint16_t)(lBodyChunks > 65535 ? 65535 : lBodyChunks);
	SendStructToPeer(apPeer, &hdr, sizeof(hdr), true);
	lBytes += sizeof(hdr);

	Log(" multiplayer: world snapshot -> peer %u: %u chunks, %u bytes (id=%u gen=%u; vars %d/%d, entities %d, taken %d, party %d, enemies %d, timers %d, bodies %d chunks, skipped %d)\n",
		(unsigned)PeerGetId(apPeer), (unsigned)(w.mvChunks.size() + (size_t)lBodyChunks + 2),
		(unsigned)lBytes, (unsigned)lId, (unsigned)lGen, lVarsL, lVarsG, lEnts, lTaken, lParty,
		lEnemies, lTimers, lBodyChunks, lSkipped);
}

//-----------------------------------------------------------------------

void cNetworkManager::HandleSnapshotChunk(const void *apData, size_t alLen)
{
	if (mbHosting || !apData || alLen < sizeof(cNetSnapshotHdr))
		return;
	cNetSnapshotHdr hdr;
	memcpy(&hdr, apData, sizeof(hdr));

	if (!mpBodySync->IsRemoteGen(hdr.mMapGen))
	{
		if (hdr.mSection == eNetSnap_Begin)
			Log(" multiplayer: world snapshot id=%u for gen %u (paired %u) - dropped\n",
				(unsigned)hdr.mSnapId, (unsigned)hdr.mMapGen, (unsigned)mpBodySync->GetRemoteMapGen());
		return;
	}

	if (hdr.mSection == eNetSnap_Begin)
	{
		if (mbSnapBuffering)
			Log(" multiplayer: world snapshot id=%u superseded by id=%u - discarded\n",
				(unsigned)mlSnapId, (unsigned)hdr.mSnapId);
		ResetSnapshotBuffer();
		mlSnapId = hdr.mSnapId;
		mlSnapGen = hdr.mMapGen;
		mbSnapBuffering = true;
		mfSnapAge = 0;
		Log(" multiplayer: world snapshot begin id=%u gen=%u (%u section chunk(s))\n",
			(unsigned)hdr.mSnapId, (unsigned)hdr.mMapGen, (unsigned)hdr.mCount);
		return;
	}
	if (!mbSnapBuffering || hdr.mSnapId != mlSnapId)
		return; /* stray chunk of another snapshot */

	if (hdr.mSection == eNetSnap_End)
	{
		Log(" multiplayer: world snapshot end id=%u: %u section chunk(s), %u body chunk(s) - applying\n",
			(unsigned)hdr.mSnapId, (unsigned)mvSnapChunks.size(), (unsigned)hdr.mCount);
		ApplyWorldSnapshot();
		ResetSnapshotBuffer();
		return;
	}
	if (hdr.mSection < eNetSnap_LocalVar || hdr.mSection > eNetSnap_Timer)
		return; /* unknown section */
	if (mvSnapChunks.size() >= 4096)
	{
		Log(" multiplayer: world snapshot id=%u: too many chunks - dropped\n", (unsigned)mlSnapId);
		ResetSnapshotBuffer();
		return;
	}
	const uint8_t *pRaw = (const uint8_t *)apData;
	mvSnapChunks.push_back(std::vector<uint8_t>(pRaw, pRaw + alLen));
}

//-----------------------------------------------------------------------

void cNetworkManager::ApplyWorldSnapshot()
{
	if (mbHosting || !mpInit || !mpInit->mpGame || !mpInit->mpMapHandler)
		return;
	cScene *pScene = mpInit->mpGame->GetScene();
	if (pScene == NULL || pScene->GetWorld3D() == NULL)
	{
		Log(" multiplayer: world snapshot id=%u: no world - dropped\n", (unsigned)mlSnapId);
		return;
	}
	if (!mpBodySync->IsRemoteGen(mlSnapGen))
	{
		Log(" multiplayer: world snapshot id=%u: gen %u no longer paired - dropped\n",
			(unsigned)mlSnapId, (unsigned)mlSnapGen);
		return;
	}
	if (mbLocalMapChangeArmed)
	{
		Log(" multiplayer: world snapshot id=%u: we are leaving this map - dropped\n",
			(unsigned)mlSnapId);
		return;
	}

	cNetApplyScope applyScope; /* nothing below echoes back to the host */

	int lVarsL = 0, lVarsG = 0, lEnts = 0, lActive = 0, lAbsent = 0, lMissing = 0, lDoors = 0,
		lLamps = 0, lTaken = 0, lParty = 0, lEnemies = 0, lEnemiesDead = 0, lTimers = 0;
	bool bHaveEntities = false, bHaveParty = false, bHaveTimers = false;
	cNetSnapshotHdr hdr;
	memset(&hdr, 0, sizeof(hdr));
	const unsigned char *pFirst = NULL;

	/* 1. vars first: polled by OnUpdate scripts, so they are final before
	   anything visible changes. */
	for (size_t c = 0; c < mvSnapChunks.size(); ++c)
	{
		const size_t lCount = SnapChunkEntries(mvSnapChunks[c], sizeof(cNetSnapVar), &hdr, &pFirst);
		if (hdr.mSection != eNetSnap_LocalVar && hdr.mSection != eNetSnap_GlobalVar)
			continue;
		for (size_t i = 0; i < lCount; ++i)
		{
			cNetSnapVar v;
			memcpy(&v, pFirst + i * sizeof(v), sizeof(v));
			v.msName[sizeof(v.msName) - 1] = 0; /* untrusted wire string */
			if (v.msName[0] == 0)
				continue;
			if (hdr.mSection == eNetSnap_LocalVar)
			{
				pScene->CreateLocalVar(tString(v.msName))->mlVal = (int)v.mlVal;
				++lVarsL;
			}
			else
			{
				pScene->CreateGlobalVar(tString(v.msName))->mlVal = (int)v.mlVal;
				++lVarsG;
			}
		}
	}

	/* 2. entities: one hash -> entity index, then the host's entries, then
	   the "present here, absent on the host" sweep. */
	{
		std::map<uint32_t, iGameEntity *> mapEnts;
		{
			tGameEntityIterator it = mpInit->mpMapHandler->GetGameEntityIterator();
			while (it.HasNext())
			{
				iGameEntity *pEnt = it.Next();
				if (pEnt == NULL || pEnt->GetType() == eGameEntityType_Enemy || pEnt->GetName().empty())
					continue;
				mapEnts.insert(std::make_pair(NetHashName(pEnt->GetName().c_str()), pEnt));
			}
		}
		std::set<uint32_t> setSeen;

		for (size_t c = 0; c < mvSnapChunks.size(); ++c)
		{
			const size_t lCount = SnapChunkEntries(mvSnapChunks[c], sizeof(cNetSnapEntity), &hdr, &pFirst);
			if (hdr.mSection != eNetSnap_Entity)
				continue;
			bHaveEntities = true;
			for (size_t i = 0; i < lCount; ++i)
			{
				cNetSnapEntity e;
				memcpy(&e, pFirst + i * sizeof(e), sizeof(e));
				++lEnts;
				setSeen.insert(e.mlNameHash);

				std::map<uint32_t, iGameEntity *>::iterator ei = mapEnts.find(e.mlNameHash);
				if (ei == mapEnts.end())
				{
					/* host-only entity (ReplaceEntity result, a drop twin that
					   failed to spawn here): never CREATE — log and skip */
					Log(" multiplayer: snapshot: host entity 0x%08X (type %u) does not exist here - skipped\n",
						(unsigned)e.mlNameHash, (unsigned)e.mType);
					++lMissing;
					continue;
				}
				iGameEntity *pEnt = ei->second;
				if ((uint8_t)pEnt->GetType() != e.mType)
					continue; /* same name, different kind: not the same thing */

				const bool bActive = (e.mFlags & kNetSnapEntityFlag_Active) != 0;
				if (pEnt->IsActive() != bActive)
				{
					if (!bActive)
					{
						/* our player may be holding one of its bodies */
						for (int b = 0; b < pEnt->GetBodyNum(); ++b)
						{
							uint32_t lBodyHash;
							if (pEnt->GetBody(b) && mpBodySync->GetHashForBody(pEnt->GetBody(b), &lBodyHash))
								ForceReleaseIfHolding(lBodyHash);
						}
					}
					pEnt->SetActive(bActive);
					++lActive;
				}
				if (pEnt->GetType() == eGameEntityType_SwingDoor)
				{
					/* after the body pose (already pinned on arrival), so the
					   +-1 degree hinge clamp lands on the host's door pose */
					cGameSwingDoor *pDoor = static_cast<cGameSwingDoor *>(pEnt);
					const bool bLocked = (e.mFlags & kNetSnapEntityFlag_Locked) != 0;
					if (pDoor->IsLocked() != bLocked)
					{
						pDoor->SetLocked(bLocked);
						++lDoors;
					}
				}
				else if (pEnt->GetType() == eGameEntityType_Lamp)
				{
					cGameLamp *pLamp = static_cast<cGameLamp *>(pEnt);
					const bool bLit = (e.mFlags & kNetSnapEntityFlag_Lit) != 0;
					if (pLamp->IsLit() != bLit)
					{
						/* silence the lit-change script: the host ran it */
						const tString sCallback = pLamp->GetLitChangeCallback();
						pLamp->SetLitChangeCallback("");
						pLamp->SetLit(bLit, false);
						pLamp->SetLitChangeCallback(sCallback);
						++lLamps;
					}
				}
				/* alive breakables: plain assign (SetHealth's > 0 branch);
				   a host-side death is carried by ABSENCE, not by health */
				if (SnapFinite(e.mfHealth) && e.mfHealth > 0 && pEnt->GetHealth() > 0 &&
					e.mfHealth != pEnt->GetHealth())
					pEnt->SetHealth(e.mfHealth);
			}
		}

		if (bHaveEntities)
		{
			for (std::map<uint32_t, iGameEntity *>::iterator mi = mapEnts.begin(); mi != mapEnts.end(); ++mi)
			{
				if (setSeen.count(mi->first))
					continue;
				iGameEntity *pEnt = mi->second;
				if (!SnapTypeIsDestroyable(pEnt->GetType()))
				{
					Log(" multiplayer: snapshot: '%s' exists here but not on the host (map mismatch?)\n",
						pEnt->GetName().c_str());
					continue;
				}
				if (!pEnt->IsActive())
					continue;
				for (int b = 0; b < pEnt->GetBodyNum(); ++b)
				{
					uint32_t lBodyHash;
					if (pEnt->GetBody(b) && mpBodySync->GetHashForBody(pEnt->GetBody(b), &lBodyHash))
						ForceReleaseIfHolding(lBodyHash);
				}
				pEnt->SetActive(false);
				++lAbsent;
				Log(" multiplayer: snapshot: '%s' is gone on the host (picked up / broken) - deactivated\n",
					pEnt->GetName().c_str());
			}
		}
	}

	/* 3. enemies — after the entity actives so nothing undoes SetActive. */
	{
		std::map<uint32_t, iGameEnemy *> mapEnemies;
		{
			tGameEnemyIterator it = mpInit->mpMapHandler->GetGameEnemyIterator();
			while (it.HasNext())
			{
				iGameEnemy *pE = it.Next();
				if (pE)
					mapEnemies.insert(std::make_pair(NetHashName(pE->GetName().c_str()), pE));
			}
		}
		for (size_t c = 0; c < mvSnapChunks.size(); ++c)
		{
			const size_t lCount = SnapChunkEntries(mvSnapChunks[c], sizeof(cNetEnemyState), &hdr, &pFirst);
			if (hdr.mSection != eNetSnap_Enemy)
				continue;
			for (size_t i = 0; i < lCount; ++i)
			{
				cNetEnemyState st;
				memcpy(&st, pFirst + i * sizeof(st), sizeof(st));
				std::map<uint32_t, iGameEnemy *>::iterator ei = mapEnemies.find(st.mlNameHash);
				if (ei == mapEnemies.end())
					continue;
				iGameEnemy *pEnemy = ei->second;
				if (!SnapFinite(st.mfPosX) || !SnapFinite(st.mfPosY) || !SnapFinite(st.mfPosZ) ||
					!SnapFinite(st.mfYaw) || !SnapFinite(st.mfHealth))
					continue; /* garbage entry */
				const cVector3f vFeet(st.mfPosX, st.mfPosY, st.mfPosZ);

				if (st.mfHealth <= 0)
				{
					if (pEnemy->GetHealth() > 0)
					{
						/* dead on the host: ragdoll at the host's spot, and
						   NEVER its death script (the host ran it) */
						pEnemy->SetOnDeathCallback("");
						if (pEnemy->GetMover() && pEnemy->GetMover()->GetCharBody())
						{
							pEnemy->GetMover()->GetCharBody()->SetFeetPosition(vFeet);
							pEnemy->GetMover()->GetCharBody()->SetYaw(st.mfYaw);
						}
						pEnemy->SetNetPuppet(false);
						pEnemy->Damage(100000.0f, 100);
						++lEnemiesDead;
					}
					continue;
				}
				if (pEnemy->GetHealth() <= 0)
					continue; /* dead here, alive there: the ragdoll owns it */

				pEnemy->SetNetPuppet(true);
				const bool bActive = (st.mFlags & 2) != 0;
				if (pEnemy->IsActive() != bActive)
					pEnemy->SetActive(bActive);
				pEnemy->NetSetTarget(vFeet, st.mfYaw); /* snaps at > 3 m */
				if (st.mfHealth != pEnemy->GetHealth())
					pEnemy->SetHealth(st.mfHealth); /* > 0 here: plain assign */
				++lEnemies;
			}
		}
	}

	/* 4. body poses were pinned on arrival (ObjectState chunks). */

	/* 5. taken items (insert + sweep) and the party inventory (replace —
	   JoinGame cleared it; our OWN inventory is never touched). */
	for (size_t c = 0; c < mvSnapChunks.size(); ++c)
	{
		const size_t lCount = SnapChunkEntries(mvSnapChunks[c], sizeof(uint32_t), &hdr, &pFirst);
		if (hdr.mSection != eNetSnap_TakenItem)
			continue;
		for (size_t i = 0; i < lCount; ++i)
		{
			uint32_t lHash;
			memcpy(&lHash, pFirst + i * sizeof(lHash), sizeof(lHash));
			m_setTakenItems.insert(lHash);
			++lTaken;
		}
	}
	if (lTaken > 0)
		ApplyTakenItems();
	for (size_t c = 0; c < mvSnapChunks.size(); ++c)
	{
		const size_t lCount = SnapChunkEntries(mvSnapChunks[c], 32, &hdr, &pFirst);
		if (hdr.mSection != eNetSnap_PartyItem)
			continue;
		if (!bHaveParty)
		{
			bHaveParty = true;
			m_setPartyItems.clear();
		}
		for (size_t i = 0; i < lCount; ++i)
		{
			char aName[32];
			memcpy(aName, pFirst + i * sizeof(aName), sizeof(aName));
			aName[sizeof(aName) - 1] = 0;
			if (aName[0] == 0)
				continue;
			m_setPartyItems.insert(tString(aName));
			++lParty;
		}
	}

	/* 6. local timers: ours (created by our own OnStart) are replaced by
	   the host's — theirs fire later on both machines alike. */
	for (size_t c = 0; c < mvSnapChunks.size(); ++c)
	{
		const size_t lCount = SnapChunkEntries(mvSnapChunks[c], sizeof(cNetSnapTimer), &hdr, &pFirst);
		if (hdr.mSection != eNetSnap_Timer)
			continue;
		if (!bHaveTimers)
		{
			bHaveTimers = true;
			mpInit->mpMapHandler->RemoveLocalTimers();
		}
		for (size_t i = 0; i < lCount; ++i)
		{
			cNetSnapTimer t;
			memcpy(&t, pFirst + i * sizeof(t), sizeof(t));
			t.msName[sizeof(t.msName) - 1] = 0;
			t.msCallback[sizeof(t.msCallback) - 1] = 0;
			if (t.msName[0] == 0 || !SnapFinite(t.mfTime))
				continue;
			cGameTimer *pTimer = mpInit->mpMapHandler->CreateTimer(tString(t.msName), t.mfTime,
				tString(t.msCallback), false);
			if (pTimer)
				pTimer->mbPaused = t.mbPaused != 0;
			++lTimers;
		}
	}

	/* 7. done: the enemy stream restarts from whatever seq comes next. */
	mbEnemySeqInKnown = false;
	Log(" multiplayer: world snapshot id=%u applied: vars %d/%d, entities %d (%d active changed, %d deactivated: absent on host, %d unknown here), doors %d, lamps %d, taken %d, party %d, enemies %d (+%d dead), timers %d\n",
		(unsigned)mlSnapId, lVarsL, lVarsG, lEnts, lActive, lAbsent, lMissing, lDoors, lLamps,
		lTaken, lParty, lEnemies, lEnemiesDead, lTimers);
}

#endif /* PENUMBRA_MULTIPLAYER */

//======================================================================
// v13: player names + party event feed — APPENDED, shared by the real and
// the stub build (see the NetworkManager.h tail). Only SendNameTable /
// SendLocalName touch ENet and live under PENUMBRA_MULTIPLAYER.
//======================================================================

const float cNetworkManager::kPartyEventLifeSeconds = 6.0f;
const float cNetworkManager::kPartyJoinGraceSeconds = 2.0f;

hpl::tString cNetworkManager::SanitizePlayerName(const hpl::tString &asName)
{
	hpl::tString sOut;
	sOut.reserve(asName.size() < kNetPlayerNameMaxChars ? asName.size() : kNetPlayerNameMaxChars);
	for (size_t i = 0; i < asName.size(); ++i)
	{
		const unsigned char c = (unsigned char)asName[i];
		if (c < 32 || c > 126)
			continue; /* control chars, DEL, anything non-ASCII */
		if (c == ' ' && sOut.empty())
			continue; /* leading blanks */
		if (sOut.size() >= kNetPlayerNameMaxChars)
			break;
		sOut += (char)c;
	}
	while (!sOut.empty() && sOut[sOut.size() - 1] == ' ')
		sOut.erase(sOut.size() - 1); /* trailing blanks (also after truncation) */
	return sOut;
}

/* One key of the plain key=value file, other lines untouched. The file is
   small (a few dozen lines) so it is read whole, patched in memory and
   written back through a temp file + rename. */
bool cNetworkManager::UpdateMultiplayerCfgKey(const char *asKey, const hpl::tString &asValue)
{
	if (!asKey || !asKey[0])
		return false;
	const size_t lKeyLen = strlen(asKey);

	std::vector<hpl::tString> vLines;
	{
		FILE *fp = fopen("multiplayer.cfg", "r");
		if (fp)
		{
			char buf[1024];
			while (fgets(buf, sizeof(buf), fp))
				vLines.push_back(hpl::tString(buf));
			fclose(fp);
		}
	}

	bool bReplaced = false;
	for (size_t i = 0; i < vLines.size() && !bReplaced; ++i)
	{
		const hpl::tString &sLine = vLines[i];
		size_t p = 0;
		while (p < sLine.size() && (sLine[p] == ' ' || sLine[p] == '\t'))
			++p;
		if (sLine.compare(p, lKeyLen, asKey) != 0)
			continue; /* different key, comment ("# key=") or blank line */
		p += lKeyLen;
		while (p < sLine.size() && (sLine[p] == ' ' || sLine[p] == '\t'))
			++p;
		if (p >= sLine.size() || sLine[p] != '=')
			continue; /* "player_name_x=" or no '=' at all: not our key */
		const bool bCrLf = sLine.size() >= 2 && sLine[sLine.size() - 2] == '\r';
		vLines[i] = hpl::tString(asKey) + "=" + asValue + (bCrLf ? "\r\n" : "\n");
		bReplaced = true;
	}
	if (!bReplaced)
	{
		if (!vLines.empty())
		{
			hpl::tString &sLast = vLines[vLines.size() - 1];
			if (sLast.empty() || sLast[sLast.size() - 1] != '\n')
				sLast += "\n"; /* a file that ends mid-line */
		}
		vLines.push_back(hpl::tString(asKey) + "=" + asValue + "\n");
	}

	const char *szTmp = "multiplayer.cfg.tmp";
	FILE *fo = fopen(szTmp, "w");
	if (!fo)
	{
		Log(" multiplayer: cannot write %s\n", szTmp);
		return false;
	}
	bool bOk = true;
	for (size_t i = 0; i < vLines.size() && bOk; ++i)
		bOk = fwrite(vLines[i].data(), 1, vLines[i].size(), fo) == vLines[i].size();
	if (fclose(fo) != 0)
		bOk = false;
	if (!bOk)
	{
		Log(" multiplayer: writing %s failed - multiplayer.cfg left untouched\n", szTmp);
		remove(szTmp);
		return false;
	}
	remove("multiplayer.cfg"); /* Windows rename() refuses to overwrite */
	if (rename(szTmp, "multiplayer.cfg") != 0)
	{
		Log(" multiplayer: rename %s -> multiplayer.cfg failed\n", szTmp);
		return false;
	}
	Log(" multiplayer: multiplayer.cfg %s=%s\n", asKey, asValue.c_str());
	return true;
}

void cNetworkManager::SetLocalPlayerName(const hpl::tString &asName)
{
	msPlayerName = SanitizePlayerName(asName);
	UpdateMultiplayerCfgKey("player_name", msPlayerName);
#ifdef PENUMBRA_MULTIPLAYER
	if (mbHosting)
		SendNameTable(NULL); /* guests see "<old> is now <new>" */
	else if (mbClientConnected && mbHadJoinPacket)
		SendLocalName();
#endif
}

hpl::tString cNetworkManager::GetPlayerName(uint8_t alId) const
{
	if (alId != 0 && alId == mlLocalPlayerId && !msPlayerName.empty())
		return msPlayerName;
	std::map<uint8_t, hpl::tString>::const_iterator it = m_mapPlayerNames.find(alId);
	if (it != m_mapPlayerNames.end() && !it->second.empty())
		return it->second;
	return "Player " + hpl::cString::ToString((int)alId);
}

void cNetworkManager::AddPartyEvent(const hpl::tString &asText)
{
	if (asText.empty())
		return;
	while (mvPartyEvents.size() >= kPartyEventMax)
		mvPartyEvents.erase(mvPartyEvents.begin()); /* oldest first */
	cNetPartyEvent ev;
	ev.msText = asText;
	ev.mfAge = 0.0f;
	mvPartyEvents.push_back(ev);
	Log(" multiplayer: party: %s\n", asText.c_str());
}

void cNetworkManager::UpdatePartyEvents(float afTimeStep)
{
	if (afTimeStep < 0.0f)
		afTimeStep = 0.0f;
	if (mfSinceJoinSeconds < 1000.0f)
		mfSinceJoinSeconds += afTimeStep; /* saturates: only "< grace" matters */
	for (size_t i = 0; i < mvPartyEvents.size(); ++i)
		mvPartyEvents[i].mfAge += afTimeStep;
	/* appended in time order, so the expired ones are always at the front */
	while (!mvPartyEvents.empty() && mvPartyEvents[0].mfAge >= kPartyEventLifeSeconds)
		mvPartyEvents.erase(mvPartyEvents.begin());
}

void cNetworkManager::OnPlayerNameReceived(uint8_t alId, const char *apName, size_t alLen)
{
	if (alId == 0 || alId == mlLocalPlayerId || alId == kPreviewGhostId || !apName)
		return; /* unknown / our own / the preview: ignored */
	/* bounded scan — the wire field need not be NUL-terminated */
	size_t n = 0;
	while (n < alLen && n < kNetPlayerNameMaxChars && apName[n] != '\0')
		++n;
	const hpl::tString sNew = SanitizePlayerName(hpl::tString(apName, n));

	const bool bAnnounced = m_setJoinAnnounced.find(alId) != m_setJoinAnnounced.end();
	const hpl::tString sOld = GetPlayerName(alId);

	if (sNew.empty())
		m_mapPlayerNames.erase(alId); /* shows as "Player <id>" */
	else
		m_mapPlayerNames[alId] = sNew;

	if (!bAnnounced)
	{
		m_setJoinAnnounced.insert(alId);
		/* A guest gets the whole existing party right after its own join:
		   those are listed silently; anything later really joined. The host
		   only ever hears a name from a peer that just connected. */
		const bool bExisting = !mbHosting && mfSinceJoinSeconds < kPartyJoinGraceSeconds;
		if (!bExisting)
			AddPartyEvent(GetPlayerName(alId) + " joined");
	}
	else if (!sNew.empty() && sNew != sOld)
		AddPartyEvent(sOld + " is now " + sNew);
}

void cNetworkManager::NotePartyHealth(uint8_t alId, uint8_t alNewHealth)
{
	if (alId == 0 || alId == mlLocalPlayerId || alId == kPreviewGhostId)
		return;
	std::map<uint8_t, uint8_t>::const_iterator it = m_mapGhostHealth.find(alId);
	if (it == m_mapGhostHealth.end())
		return; /* first value we hear: no transition to report */
	if (it->second > 0 && alNewHealth == 0)
		AddPartyEvent(GetPlayerName(alId) + " died");
	else if (it->second == 0 && alNewHealth > 0)
		AddPartyEvent(GetPlayerName(alId) + " respawned");
}

void cNetworkManager::ForgetPlayerName(uint8_t alId)
{
	const hpl::tString sName = GetPlayerName(alId); /* before the erase */
	m_mapPlayerNames.erase(alId);
	if (m_setJoinAnnounced.erase(alId) > 0)
		AddPartyEvent(sName + " left");
}

#ifdef PENUMBRA_MULTIPLAYER

void cNetworkManager::SendNameTable(ENetPeer *apOnlyTo)
{
	if (!mbHosting || !mpImpl || !mpImpl->mpHost)
		return;
	/* our own entry first (id 1, may be empty = "Player 1"), then every guest */
	std::vector<std::pair<uint8_t, hpl::tString> > vTable;
	vTable.push_back(std::make_pair(mlLocalPlayerId, msPlayerName));
	for (std::map<uint8_t, hpl::tString>::const_iterator it = m_mapPlayerNames.begin();
		it != m_mapPlayerNames.end(); ++it)
		vTable.push_back(*it);

	for (size_t i = 0; i < vTable.size(); ++i)
	{
		cNetPlayerName pkt;
		memset(&pkt, 0, sizeof(pkt)); /* NUL padding; a full name has no NUL */
		pkt.mType = eNetPacketType_PlayerName;
		pkt.mPlayerID = vTable[i].first;
		const hpl::tString &sName = vTable[i].second;
		const size_t n = sName.size() < sizeof(pkt.msName) ? sName.size() : sizeof(pkt.msName);
		if (n > 0)
			memcpy(pkt.msName, sName.data(), n);
		if (apOnlyTo)
			SendStructToPeer(apOnlyTo, &pkt, sizeof(pkt), true);
		else
			SendReliableEvent(&pkt, sizeof(pkt));
	}
}

void cNetworkManager::SendLocalName()
{
	if (mbHosting)
		return;
	/* Sent even when empty: the host announces the join on THIS packet
	   ("Player <id> joined" when we have no name) */
	cNetPlayerName pkt;
	memset(&pkt, 0, sizeof(pkt));
	pkt.mType = eNetPacketType_PlayerName;
	pkt.mPlayerID = mlLocalPlayerId; /* the host uses the peer id anyway */
	const size_t n = msPlayerName.size() < sizeof(pkt.msName) ? msPlayerName.size() : sizeof(pkt.msName);
	if (n > 0)
		memcpy(pkt.msName, msPlayerName.data(), n);
	SendReliableEvent(&pkt, sizeof(pkt)); /* no-op until connected + joined */
}

#endif /* PENUMBRA_MULTIPLAYER */

//======================================================================
// v15: internet hardening — APPENDED. Shared part first (both builds):
// passwords, the role table, string/file-name checks, guest id pool.
// The ENet-touching part (auth state machine, validation with clamps,
// strikes, rate limits, discovery limiter) is under PENUMBRA_MULTIPLAYER.
// README.md "Security" describes the flow.
//======================================================================

hpl::tString cNetworkManager::SanitizePassword(const hpl::tString &asPassword)
{
	hpl::tString sOut;
	for (size_t i = 0; i < asPassword.size(); ++i)
	{
		const unsigned char c = (unsigned char)asPassword[i];
		if (c < 32 || c > 126)
			continue; /* control chars, DEL, non-ASCII */
		if (c == ' ' && sOut.empty())
			continue; /* leading blanks */
		if (sOut.size() >= kNetPasswordMaxChars)
			break;
		sOut += (char)c;
	}
	while (!sOut.empty() && sOut[sOut.size() - 1] == ' ')
		sOut.erase(sOut.size() - 1);
	return sOut;
}

void cNetworkManager::SetJoinPassword(const hpl::tString &asPassword)
{
	msJoinPassword = SanitizePassword(asPassword);
}

void cNetworkManager::SetServerPassword(const hpl::tString &asPassword)
{
	msServerPassword = SanitizePassword(asPassword);
}

/* THE role table. Both directions share the event types the host relays;
   everything else is one-way. Keep in step with eNetPacketType — an
   unlisted type is dropped from EITHER side (the voice type 29 of v16
   must be added here by whoever adds it). */
bool cNetworkManager::IsAllowedFrom(uint8_t alType, bool abFromHost)
{
	switch (alType)
	{
	/* either direction (guest -> host, host relays / host -> guests) */
	case eNetPacketType_PlayerState:
	case eNetPacketType_MapChange:
	case eNetPacketType_ItemPickup:
	case eNetPacketType_ItemDrop:
	case eNetPacketType_ScriptEvent:
	case eNetPacketType_EntityDamage:
	case eNetPacketType_PlayerName:
	case eNetPacketType_Voice: /* v16: guest -> host, host relays / host -> guests */
		return true;
	/* host -> guest only */
	case eNetPacketType_PlayerJoin:
	case eNetPacketType_PlayerLeave:
	case eNetPacketType_VersionAck:
	case eNetPacketType_ObjectState:
	case eNetPacketType_BodyCensus:
	case eNetPacketType_BodyGrabDeny:
	case eNetPacketType_EnemyState:
	case eNetPacketType_EnemyEvent:
	case eNetPacketType_PlayerDamage:
	case eNetPacketType_WorldSnapshot:
	case eNetPacketType_Challenge:
		return abFromHost;
	/* guest -> host only */
	case eNetPacketType_BodyGrabBegin:
	case eNetPacketType_BodyGrabTarget:
	case eNetPacketType_BodyGrabEnd:
	case eNetPacketType_BodyPush:
	case eNetPacketType_EnemyDamage:
	case eNetPacketType_MapReady:
	case eNetPacketType_Auth:
		return !abFromHost;
	/* ChatMessage (4, reserved), discovery 5/6 (never over ENet), unknown */
	default:
		return false;
	}
}

bool cNetworkManager::NetStringOk(const char *apStr, size_t alCap, bool abAllowEmpty)
{
	if (!apStr)
		return false;
	size_t n = 0;
	while (n < alCap && apStr[n] != '\0')
	{
		const unsigned char c = (unsigned char)apStr[n];
		if (c < 32 || c > 126)
			return false;
		++n;
	}
	return n > 0 || abAllowEmpty;
}

bool cNetworkManager::NetBareFileNameOk(const char *apStr, size_t alCap, const char *asExt)
{
	if (!NetStringOk(apStr, alCap, false) || !asExt)
		return false;
	size_t n = 0;
	while (n < alCap && apStr[n] != '\0')
		++n;
	const size_t lExt = strlen(asExt);
	if (n < lExt + 2) /* at least "x." + ext */
		return false;
	if (apStr[0] == '.' || apStr[0] == ' ' || apStr[n - 1] == ' ')
		return false;
	for (size_t i = 0; i < n; ++i)
	{
		const char c = apStr[i];
		if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
			c == '"' || c == '<' || c == '>' || c == '|')
			return false;
		if (c == '.' && i + 1 < n && apStr[i + 1] == '.')
			return false; /* ".." anywhere */
	}
	if (apStr[n - lExt - 1] != '.')
		return false;
	for (size_t i = 0; i < lExt; ++i)
	{
		char a = apStr[n - lExt + i], b = asExt[i];
		if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
		if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
		if (a != b)
			return false;
	}
	return true;
}

uint8_t cNetworkManager::AllocGuestId()
{
	if (!mvFreeGuestIds.empty())
	{
		const uint8_t id = mvFreeGuestIds.back();
		mvFreeGuestIds.pop_back();
		return id;
	}
	if (mlNextGuestId < 2 || mlNextGuestId >= kPreviewGhostId)
		return 0; /* the counter never reaches the preview id / wraps */
	return mlNextGuestId++;
}

void cNetworkManager::FreeGuestId(uint8_t alId)
{
	if (alId < 2 || alId >= kPreviewGhostId)
		return;
	for (size_t i = 0; i < mvFreeGuestIds.size(); ++i)
		if (mvFreeGuestIds[i] == alId)
			return;
	mvFreeGuestIds.push_back(alId);
}

#ifdef PENUMBRA_MULTIPLAYER

namespace
{
/* Validation bounds. Penumbra levels are a few hundred metres across;
   a guest reaches ~3 m, its throws are capped by the grab tuning anyway. */
const float kNetMaxCoord = 20000.0f;     /* any world position component */
const float kNetMaxAngle = 100.0f;       /* radians (pitch/yaw are < 2 pi) */
const float kNetMaxReach = 50.0f;        /* grab target / push point from the guest */
const float kNetMaxRelPick = 10.0f;      /* pick offset from a body's centre */
const float kNetMaxDropImpulse = 15.0f;  /* ItemDrop toss */
const float kNetMaxThrowImpulse = 30.0f; /* GrabEnd throw (BodySync clamps again) */
const float kNetMaxPushImpulse = 50.0f;  /* one tick of move/push force */
const float kNetMaxDamage = 200.0f;      /* enemy / entity / player damage per hit */
const float kNetStrikeDecaySeconds = 5.0f;

static bool NetFinite(float afX)
{
	return afX == afX && afX <= 3.0e38f && afX >= -3.0e38f;
}

static bool NetFiniteBounded(float afX, float afMaxAbs)
{
	return NetFinite(afX) && afX <= afMaxAbs && afX >= -afMaxAbs;
}

/** All three finite; |v| scaled down to afMaxLen when longer. false = NaN/inf. */
static bool NetClampVec(float &x, float &y, float &z, float afMaxLen)
{
	if (!NetFinite(x) || !NetFinite(y) || !NetFinite(z))
		return false;
	const float fSq = x * x + y * y + z * z;
	if (fSq > afMaxLen * afMaxLen && fSq > 0)
	{
		const float fScale = afMaxLen / sqrtf(fSq);
		x *= fScale; y *= fScale; z *= fScale;
	}
	return true;
}

static bool NetClampDamage(float &afDamage)
{
	if (!NetFinite(afDamage))
		return false;
	if (afDamage < 0)
		afDamage = 0;
	else if (afDamage > kNetMaxDamage)
		afDamage = kNetMaxDamage;
	return true;
}

static bool NetPointNear(const hpl::cVector3f &avFrom, float x, float y, float z, float afMax)
{
	const float dx = x - avFrom.x, dy = y - avFrom.y, dz = z - avFrom.z;
	return dx * dx + dy * dy + dz * dz <= afMax * afMax;
}
} // namespace

bool cNetworkManager::PeerLive(const ENetPeer *apPeer)
{
	return apPeer && apPeer->state == ENET_PEER_STATE_CONNECTED && PeerGetId(apPeer) >= 2;
}

int cNetworkManager::CountConnectedPeers(bool abAcceptedOnly) const
{
	if (!mpImpl || !mpImpl->mpHost)
		return 0;
	int n = 0;
	for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
	{
		const ENetPeer *pd = &mpImpl->mpHost->peers[i];
		if (pd->state != ENET_PEER_STATE_CONNECTED)
			continue;
		if (abAcceptedOnly && PeerGetId(pd) < 2)
			continue;
		++n;
	}
	return n;
}

void cNetworkManager::ResetPeerGuards()
{
	if (!mpImpl)
		return;
	mpImpl->m_mapGuards.clear();
	memset(mpImpl->mPong, 0, sizeof(mpImpl->mPong));
	mpImpl->mfPongGlobalWindow = 0;
	mpImpl->mlPongGlobalCount = 0;
}

/* Not a CSPRNG — the nonce only has to differ per connection so a captured
   answer cannot be replayed: clock, ENet clock, the peer's address and
   connect id, a counter and rand(), folded through the digest itself. */
void cNetworkManager::MakeNonce(const ENetPeer *apPeer, uint8_t aOut[16])
{
	uint32_t aSeed[8];
	aSeed[0] = (uint32_t)time(NULL);
	aSeed[1] = enet_time_get();
	aSeed[2] = apPeer ? apPeer->address.host : 0u;
	aSeed[3] = apPeer ? ((uint32_t)apPeer->address.port | ((uint32_t)apPeer->connectID << 16)) : 0u;
	aSeed[4] = ++mpImpl->mlNonceCounter;
	aSeed[5] = (uint32_t)rand();
	aSeed[6] = (uint32_t)(uintptr_t)apPeer;
	aSeed[7] = (uint32_t)(uintptr_t)this ^ 0x5A17E5EDu;
	uint8_t aPrev[16];
	memcpy(aPrev, aOut, sizeof(aPrev)); /* whatever was there (zeros) adds nothing but hurts nothing */
	NetAuthDigest((const char *)aSeed, sizeof(aSeed), aPrev, (uint16_t)(aSeed[4] & 0xFFFFu), aOut);
}

void cNetworkManager::HostAcceptPeer(ENetPeer *apPeer, const cNetAuth &aAuth)
{
	if (!mbHosting || !mpImpl || !mpImpl->mpHost || !apPeer)
		return;
	std::map<const ENetPeer *, Impl::cPeerGuard>::iterator gi = mpImpl->m_mapGuards.find(apPeer);
	if (gi == mpImpl->m_mapGuards.end())
		return;
	Impl::cPeerGuard &guard = gi->second;
	if (guard.mbAuthed || guard.mbRefused)
		return;
	char sWho[64];
	FormatPeerAddr(apPeer, sWho, sizeof(sWho));

	uint8_t aExpect[16];
	NetAuthDigest(msServerPassword.c_str(), msServerPassword.size(), guard.mNonce,
		kNetProtocolVersion, aExpect);
	if (aAuth.mlVersion != kNetProtocolVersion || memcmp(aExpect, aAuth.mDigest, sizeof(aExpect)) != 0)
	{
		guard.mbRefused = true;
		Log(" multiplayer: REFUSED peer %s - bad auth (version %u, %s)\n", sWho,
			(unsigned)aAuth.mlVersion, msServerPassword.empty() ? "open server" : "wrong password");
		enet_peer_disconnect(apPeer, kNetDisconnectBadAuth);
		return;
	}

	const uint8_t aid = AllocGuestId();
	if (aid == 0)
	{
		guard.mbRefused = true;
		Log(" multiplayer: REFUSED peer %s - no guest id left\n", sWho);
		enet_peer_disconnect(apPeer, kNetDisconnectFull);
		return;
	}
	guard.mbAuthed = true;
	guard.mlStrikes = 0;
	PeerSetId(apPeer, aid);
	{
		/* FIRST game packet: proves we speak their protocol (their
		   join-handler refuses old hosts that skip this) */
		cNetVersionAck ack;
		ack.mType = eNetPacketType_VersionAck;
		ack.mlVersion = kNetProtocolVersion;
		SendStructToPeer(apPeer, &ack, sizeof(ack), true);
	}
	SendPlayerJoin(apPeer, aid);
	SendCensus(apPeer);    /* late joiner gets the map-load census now */
	SendMapBeacon(apPeer); /* ...and where the party is, so a guest sitting
	                          in the menu launches straight into our map */
	/* v14: NO world/body snapshot here — the guest has no world yet (or a
	   stale one). It asks with MapReady once its census is paired with
	   ours, and SendWorldSnapshot answers. */
	if (aAuth.msName[0] != '\0')
		OnPlayerNameReceived(aid, aAuth.msName, sizeof(aAuth.msName)); /* sanitised inside */
	SendNameTable(NULL); /* v13: who is here — the newcomer lists it silently */
	Log(" multiplayer: peer %s accepted as id=%u%s\n", sWho, (unsigned)aid,
		msServerPassword.empty() ? "" : " (password ok)");
}

void cNetworkManager::SendAuthResponse(const cNetChallenge &aChallenge)
{
	if (mbHosting || !mpImpl || !mpImpl->mpServerPeer)
		return;
	if (mbAuthSent)
	{
		Log(" multiplayer: second challenge from the host ignored\n");
		return;
	}
	cNetAuth auth;
	memset(&auth, 0, sizeof(auth));
	auth.mType = eNetPacketType_Auth;
	auth.mlVersion = kNetProtocolVersion;
	NetAuthDigest(msJoinPassword.c_str(), msJoinPassword.size(), aChallenge.mNonce,
		kNetProtocolVersion, auth.mDigest);
	const size_t n = msPlayerName.size() < sizeof(auth.msName) ? msPlayerName.size() : sizeof(auth.msName);
	if (n > 0)
		memcpy(auth.msName, msPlayerName.data(), n);
	SendStructToPeer(mpImpl->mpServerPeer, &auth, sizeof(auth), true);
	mbAuthSent = true;
	Log(" multiplayer: challenge answered (%s)\n", msJoinPassword.empty() ? "no password" : "with password");
}

void cNetworkManager::NoteViolation(ENetPeer *apPeer, uint8_t alType, const char *asWhy)
{
	if (!mpImpl || !apPeer)
		return;
	std::map<const ENetPeer *, Impl::cPeerGuard>::iterator gi = mpImpl->m_mapGuards.find(apPeer);
	if (gi == mpImpl->m_mapGuards.end())
		return;
	Impl::cPeerGuard &guard = gi->second;
	const unsigned lBit = alType < 64 ? alType : 63;
	if ((guard.mlLoggedTypes & (1ull << lBit)) == 0)
	{
		guard.mlLoggedTypes |= (1ull << lBit);
		char sWho[64];
		FormatPeerAddr(apPeer, sWho, sizeof(sWho));
		Log(" multiplayer: guest %u (%s): dropped packet type %u - %s (logged once per type)\n",
			(unsigned)PeerGetId(apPeer), sWho, (unsigned)alType, asWhy ? asWhy : "");
	}
	++guard.mlStrikes;
	guard.mfStrikeDecay = 0;
	if (guard.mlStrikes >= kNetMaxStrikes && !guard.mbRefused)
	{
		guard.mbRefused = true; /* everything else from it is dropped until the slot dies */
		char sWho[64];
		FormatPeerAddr(apPeer, sWho, sizeof(sWho));
		Log(" multiplayer: guest %u (%s) KICKED - %u protocol violations\n",
			(unsigned)PeerGetId(apPeer), sWho, guard.mlStrikes);
		enet_peer_disconnect(apPeer, kNetDisconnectKicked);
	}
}

/* Shape + bounds of every packet either role applies, clamping in place.
   Host->guest STREAMS (ObjectState, EnemyState, WorldSnapshot, census,
   join/leave/ack/deny/event) keep their own length/generation checks in
   the handlers; only the event payloads with strings/floats are looked at
   here. A short packet of a known type is malformed: a real build never
   sends one. */
bool cNetworkManager::ValidateEventPacket(unsigned char *apData, size_t alLen, bool abFromHost)
{
	if (!apData || alLen < 1)
		return false;
	const uint8_t t = apData[0];
	switch (t)
	{
	case eNetPacketType_PlayerState:
	{
		if (alLen < sizeof(cNetPlayerState))
			return false;
		cNetPlayerState st;
		memcpy(&st, apData, sizeof(st));
		return NetFiniteBounded(st.mfPosX, kNetMaxCoord) && NetFiniteBounded(st.mfPosY, kNetMaxCoord) &&
			NetFiniteBounded(st.mfPosZ, kNetMaxCoord) && NetFiniteBounded(st.mfPitch, kNetMaxAngle) &&
			NetFiniteBounded(st.mfYaw, kNetMaxAngle); /* velocities are int8, health is clamped on apply */
	}
	case eNetPacketType_MapChange:
	{
		if (alLen < sizeof(cNetMapChange))
			return false;
		const cNetMapChange *mc = (const cNetMapChange *)apData;
		/* the start name may be empty (the beacon sends none) */
		return NetBareFileNameOk(mc->msMap, sizeof(mc->msMap), "dae") &&
			NetStringOk(mc->msPos, sizeof(mc->msPos), true);
	}
	case eNetPacketType_ItemPickup:
	{
		if (alLen < sizeof(cNetItemPickup))
			return false;
		const cNetItemPickup *ip = (const cNetItemPickup *)apData;
		return NetStringOk(ip->msItemName, sizeof(ip->msItemName), true);
	}
	case eNetPacketType_ItemDrop:
	{
		if (alLen < sizeof(cNetItemDrop))
			return false;
		cNetItemDrop drop;
		memcpy(&drop, apData, sizeof(drop));
		if (!NetStringOk(drop.msName, sizeof(drop.msName), false) ||
			!NetBareFileNameOk(drop.msFile, sizeof(drop.msFile), "ent"))
			return false;
		if (!NetFiniteBounded(drop.mfPosX, kNetMaxCoord) || !NetFiniteBounded(drop.mfPosY, kNetMaxCoord) ||
			!NetFiniteBounded(drop.mfPosZ, kNetMaxCoord))
			return false;
		if (!NetClampVec(drop.mfImpX, drop.mfImpY, drop.mfImpZ, kNetMaxDropImpulse))
			return false;
		memcpy(apData, &drop, sizeof(drop));
		return true;
	}
	case eNetPacketType_ScriptEvent:
	{
		if (alLen < sizeof(cNetScriptEvent))
			return false;
		const cNetScriptEvent *se = (const cNetScriptEvent *)apData;
		switch (se->mOp)
		{
		case eNetScriptOp_LocalVarSet: case eNetScriptOp_LocalVarAdd:
		case eNetScriptOp_GlobalVarSet: case eNetScriptOp_GlobalVarAdd:
		case eNetScriptOp_EntityActive: case eNetScriptOp_DoorLocked:
		case eNetScriptOp_RemoveItem: case eNetScriptOp_LampLit:
			break;
		default:
			return false;
		}
		return NetStringOk(se->msName, sizeof(se->msName), false);
	}
	case eNetPacketType_EntityDamage:
	{
		if (alLen < sizeof(cNetEntityDamage))
			return false;
		cNetEntityDamage ed;
		memcpy(&ed, apData, sizeof(ed));
		if (!NetClampDamage(ed.mfDamage))
			return false;
		memcpy(apData, &ed, sizeof(ed));
		return true;
	}
	case eNetPacketType_EnemyDamage:
	{
		if (alLen < sizeof(cNetEnemyDamage))
			return false;
		cNetEnemyDamage dmg;
		memcpy(&dmg, apData, sizeof(dmg));
		if (!NetClampDamage(dmg.mfDamage))
			return false;
		memcpy(apData, &dmg, sizeof(dmg));
		return true;
	}
	case eNetPacketType_PlayerDamage:
	{
		if (alLen < sizeof(cNetPlayerDamage))
			return false;
		cNetPlayerDamage pd;
		memcpy(&pd, apData, sizeof(pd));
		if (!NetClampDamage(pd.mfDamage))
			return false;
		memcpy(apData, &pd, sizeof(pd));
		return true;
	}
	case eNetPacketType_BodyGrabBegin:
	{
		if (alLen < sizeof(cNetBodyGrabBegin))
			return false;
		cNetBodyGrabBegin gb;
		memcpy(&gb, apData, sizeof(gb));
		if (!NetFiniteBounded(gb.mfRelX, kNetMaxRelPick) || !NetFiniteBounded(gb.mfRelY, kNetMaxRelPick) ||
			!NetFiniteBounded(gb.mfRelZ, kNetMaxRelPick) || !NetFinite(gb.mfMassMul))
			return false;
		gb.mfMassMul = gb.mfMassMul < 0.1f ? 0.1f : (gb.mfMassMul > 20.0f ? 20.0f : gb.mfMassMul);
		memcpy(apData, &gb, sizeof(gb));
		return true;
	}
	case eNetPacketType_BodyGrabTarget:
	{
		if (alLen < sizeof(cNetBodyGrabTarget))
			return false;
		const cNetBodyGrabTarget *gt = (const cNetBodyGrabTarget *)apData;
		return NetFiniteBounded(gt->mfX, kNetMaxCoord) && NetFiniteBounded(gt->mfY, kNetMaxCoord) &&
			NetFiniteBounded(gt->mfZ, kNetMaxCoord);
	}
	case eNetPacketType_BodyGrabEnd:
	{
		if (alLen < sizeof(cNetBodyGrabEnd))
			return false;
		cNetBodyGrabEnd ge;
		memcpy(&ge, apData, sizeof(ge));
		if (!NetClampVec(ge.mfImpX, ge.mfImpY, ge.mfImpZ, kNetMaxThrowImpulse))
			return false;
		memcpy(apData, &ge, sizeof(ge));
		return true;
	}
	case eNetPacketType_BodyPush:
	{
		if (alLen < sizeof(cNetBodyPush))
			return false;
		cNetBodyPush push;
		memcpy(&push, apData, sizeof(push));
		if (!NetFiniteBounded(push.mfPtX, kNetMaxCoord) || !NetFiniteBounded(push.mfPtY, kNetMaxCoord) ||
			!NetFiniteBounded(push.mfPtZ, kNetMaxCoord))
			return false;
		if (!NetClampVec(push.mfImpX, push.mfImpY, push.mfImpZ, kNetMaxPushImpulse))
			return false;
		memcpy(apData, &push, sizeof(push));
		return true;
	}
	case eNetPacketType_PlayerName:
		return alLen >= sizeof(cNetPlayerName); /* SanitizePlayerName on apply */
	case eNetPacketType_Voice:
	{
		/* v16: header + 1..2 length-prefixed Opus frames, payload bounded
		   (kNetVoiceMaxPayload); the frame table must add up EXACTLY to the
		   packet — trailing bytes are as malformed as missing ones. The
		   author byte is not checked here: the host restamps it from the
		   peer (RelayVoice), the guest trusts the host's stamp. */
		if (alLen < sizeof(cNetVoice) || alLen > sizeof(cNetVoice) + kNetVoiceMaxPayload)
			return false;
		cNetVoice vh;
		memcpy(&vh, apData, sizeof(vh));
		if (vh.mFrames == 0 || vh.mFrames > kNetVoiceMaxFramesPerPacket)
			return false;
		size_t at = sizeof(cNetVoice);
		for (int f = 0; f < (int)vh.mFrames; ++f)
		{
			if (alLen - at < 2)
				return false;
			uint16_t l = 0;
			memcpy(&l, apData + at, 2);
			at += 2;
			if (alLen - at < (size_t)l)
				return false;
			at += l;
		}
		return at == alLen;
	}
	case eNetPacketType_MapReady:
		return alLen >= sizeof(cNetMapReady);
	case eNetPacketType_Auth:
		return alLen >= sizeof(cNetAuth);
	case eNetPacketType_Challenge:
		return alLen >= sizeof(cNetChallenge);
	case eNetPacketType_PlayerJoin:
	case eNetPacketType_PlayerLeave:
		return alLen >= sizeof(cNetPlayerJoin);
	case eNetPacketType_VersionAck:
		return alLen >= sizeof(cNetVersionAck);
	case eNetPacketType_BodyGrabDeny:
		return alLen >= sizeof(cNetBodyGrabDeny);
	case eNetPacketType_EnemyEvent:
		return alLen >= sizeof(cNetEnemyEvent);
	case eNetPacketType_BodyCensus:
		return alLen >= sizeof(cNetBodyCensus);
	case eNetPacketType_ObjectState:
	case eNetPacketType_EnemyState:
	case eNetPacketType_WorldSnapshot:
		return abFromHost; /* host streams; the handlers check header + count */
	default:
		return false;
	}
}

/* Host extras on top of ValidateEventPacket: file names must resolve
   through the engine's file searcher (the same lookup the loaders use), and
   physics intent must be within reach of where the guest last said it was
   (a guest that never sent a state cannot push anything). */
bool cNetworkManager::ValidateGuestPacket(ENetPeer *apPeer, unsigned char *apData, size_t alLen)
{
	if (!ValidateEventPacket(apData, alLen, false))
		return false;
	if (!mpImpl || !apPeer)
		return false;
	std::map<const ENetPeer *, Impl::cPeerGuard>::iterator gi = mpImpl->m_mapGuards.find(apPeer);
	if (gi == mpImpl->m_mapGuards.end())
		return false;
	Impl::cPeerGuard &guard = gi->second;
	hpl::cFileSearcher *pSearcher = (mpInit && mpInit->mpGame && mpInit->mpGame->GetResources()) ?
		mpInit->mpGame->GetResources()->GetFileSearcher() : NULL;

	const uint8_t t = apData[0];
	switch (t)
	{
	case eNetPacketType_PlayerState:
	{
		const cNetPlayerState *st = (const cNetPlayerState *)apData;
		guard.mvLastPos = cVector3f(st->mfPosX, st->mfPosY, st->mfPosZ);
		guard.mbHavePos = true;
		return true;
	}
	case eNetPacketType_ItemDrop:
	{
		const cNetItemDrop *drop = (const cNetItemDrop *)apData;
		char sFile[sizeof(drop->msFile) + 1];
		memcpy(sFile, drop->msFile, sizeof(drop->msFile));
		sFile[sizeof(drop->msFile)] = '\0';
		if (pSearcher == NULL || pSearcher->GetFilePath(tString(sFile)).empty())
		{
			Log(" multiplayer: guest %u drop of '%s' refused - no such entity file here\n",
				(unsigned)PeerGetId(apPeer), sFile);
			return false;
		}
		return true;
	}
	case eNetPacketType_MapChange:
	{
		const cNetMapChange *mc = (const cNetMapChange *)apData;
		char sMap[sizeof(mc->msMap) + 1];
		memcpy(sMap, mc->msMap, sizeof(mc->msMap));
		sMap[sizeof(mc->msMap)] = '\0';
		if (pSearcher == NULL || pSearcher->GetFilePath(tString(sMap)).empty())
		{
			Log(" multiplayer: guest %u map change to '%s' refused - no such map here\n",
				(unsigned)PeerGetId(apPeer), sMap);
			return false;
		}
		return true;
	}
	case eNetPacketType_BodyGrabTarget:
	{
		const cNetBodyGrabTarget *gt = (const cNetBodyGrabTarget *)apData;
		return guard.mbHavePos && NetPointNear(guard.mvLastPos, gt->mfX, gt->mfY, gt->mfZ, kNetMaxReach);
	}
	case eNetPacketType_BodyPush:
	{
		const cNetBodyPush *push = (const cNetBodyPush *)apData;
		return guard.mbHavePos && NetPointNear(guard.mvLastPos, push->mfPtX, push->mfPtY, push->mfPtZ, kNetMaxReach);
	}
	case eNetPacketType_BodyGrabBegin:
	case eNetPacketType_BodyGrabEnd:
		return guard.mbHavePos; /* a guest that never stood anywhere holds nothing */
	default:
		return true;
	}
}

void cNetworkManager::UpdatePeerGuards(float afTimeStep)
{
	if (!mbHosting || !mpImpl || !mpImpl->mpHost)
		return;
	std::map<const ENetPeer *, Impl::cPeerGuard>::iterator it = mpImpl->m_mapGuards.begin();
	while (it != mpImpl->m_mapGuards.end())
	{
		const ENetPeer *pPeer = it->first;
		Impl::cPeerGuard &guard = it->second;
		/* the slot died without us seeing the event (never for a normal
		   enet_peer_disconnect, which reports one) — drop the record */
		if (pPeer->state == ENET_PEER_STATE_DISCONNECTED || pPeer->state == ENET_PEER_STATE_ZOMBIE)
		{
			std::map<const ENetPeer *, Impl::cPeerGuard>::iterator dead = it++;
			mpImpl->m_mapGuards.erase(dead);
			continue;
		}
		guard.mfAge += afTimeStep;
		if (!guard.mbAuthed && !guard.mbRefused && guard.mfAge > kNetAuthTimeoutSeconds)
		{
			guard.mbRefused = true;
			char sWho[64];
			FormatPeerAddr(pPeer, sWho, sizeof(sWho));
			Log(" multiplayer: peer %s never answered the challenge (%.1f s) - dropped\n",
				sWho, guard.mfAge);
			enet_peer_disconnect(const_cast<ENetPeer *>(pPeer), kNetDisconnectBadAuth);
		}
		guard.mfRateWindow += afTimeStep;
		if (guard.mfRateWindow >= 1.0f)
		{
			guard.mfRateWindow = 0;
			guard.mlReliableInWindow = 0;
			guard.mbRateStruck = false;
		}
		if (guard.mlStrikes > 0)
		{
			guard.mfStrikeDecay += afTimeStep;
			if (guard.mfStrikeDecay >= kNetStrikeDecaySeconds)
			{
				guard.mfStrikeDecay = 0;
				--guard.mlStrikes; /* a flaky-but-honest client never accumulates */
			}
		}
		++it;
	}
}

/* Windows are aged once per frame in PollDiscovery. A source keeps its
   bucket for the 1 s window; a new source takes an expired bucket, else the
   one closest to expiry (a flood from 17+ spoofed sources is then capped by
   the global limit alone, which is the point of having one). */
bool cNetworkManager::DiscoveryPongAllowed(uint32_t alAddr)
{
	if (!mpImpl)
		return false;
	if (mpImpl->mlPongGlobalCount >= (unsigned)Impl::kMaxPongPerSecond)
		return false;
	int lFree = -1;
	float fOldest = 2.0f;
	for (int b = 0; b < Impl::kPongBuckets; ++b)
	{
		Impl::cPongBucket &bk = mpImpl->mPong[b];
		if (bk.mfWindowLeft > 0 && bk.mlAddr == alAddr)
		{
			if (bk.mlCount >= (unsigned)Impl::kMaxPongPerSource)
				return false;
			++bk.mlCount;
			++mpImpl->mlPongGlobalCount;
			return true;
		}
		if (bk.mfWindowLeft < fOldest)
		{
			fOldest = bk.mfWindowLeft;
			lFree = b;
		}
	}
	if (lFree < 0)
		return false;
	Impl::cPongBucket &bk = mpImpl->mPong[lFree];
	bk.mlAddr = alAddr;
	bk.mfWindowLeft = 1.0f;
	bk.mlCount = 1;
	++mpImpl->mlPongGlobalCount;
	return true;
}

#endif /* PENUMBRA_MULTIPLAYER */

//======================================================================
// v16: proximity voice chat — APPENDED (see the NetworkManager.h tail).
// The HUD accessors are shared by both builds; the transport half lives
// under PENUMBRA_MULTIPLAYER. The codec/AL work is all in cVoiceChat.
//======================================================================

bool cNetworkManager::IsPlayerTalking(uint8_t alId) const
{
	if (!mpVoice || alId == 0)
		return false;
	return mpVoice->IsTalking(alId);
}

bool cNetworkManager::IsMicOpen() const
{
	return mpVoice ? mpVoice->IsMicOpen() : false;
}

bool cNetworkManager::IsVoiceAvailable() const
{
	return mpVoice != NULL;
}

#ifdef PENUMBRA_MULTIPLAYER

namespace
{
/** Ghost render feet -> mouth: the shipped meshes stand ~1.75 m, the
    sender's camera sits ~1.6 m up (game.cfg Player Height). */
const float kVoiceHeadHeight = 1.6f;
}

void cNetworkManager::SendUnreliableEvent(const void *apData, size_t alLen)
{
	if (!mpImpl || !mpImpl->mpHost || !apData || alLen == 0)
		return;
	if (mbHosting)
	{
		for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
		{
			ENetPeer *pd = &mpImpl->mpHost->peers[i];
			if (PeerLive(pd)) /* v15: accepted peers only */
				SendStructToPeer(pd, apData, alLen, false);
		}
	}
	else if (mbClientConnected && mbHadJoinPacket && mpImpl->mpServerPeer &&
		mpImpl->mpServerPeer->state == ENET_PEER_STATE_CONNECTED)
	{
		SendStructToPeer(mpImpl->mpServerPeer, apData, alLen, false);
	}
}

void cNetworkManager::RelayVoice(ENetPeer *apFrom, uint8_t alAuthor, const void *apData, size_t alLen)
{
	if (!mpImpl || !mpImpl->mpHost || !apData || alAuthor < 2)
		return;
	if (alLen < sizeof(cNetVoice) || alLen > sizeof(cNetVoice) + kNetVoiceMaxPayload)
		return; /* oversized = not ours; a guest cannot make us forward garbage */
	uint8_t buf[sizeof(cNetVoice) + kNetVoiceMaxPayload];
	memcpy(buf, apData, alLen);
	cNetVoice hdr;
	memcpy(&hdr, buf, sizeof(hdr));
	hdr.mPlayerID = alAuthor; /* the peer is the truth, never the byte it sent */
	memcpy(buf, &hdr, sizeof(hdr));
	for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
	{
		ENetPeer *dst = &mpImpl->mpHost->peers[i];
		if (dst == apFrom || !PeerLive(dst)) /* v15: accepted peers only */
			continue;
		SendStructToPeer(dst, buf, alLen, false);
	}
	DispatchIncoming(buf, alLen); /* and we hear it too */
}

void cNetworkManager::UpdateVoice(float afTimeStep)
{
	if (!mpVoice)
		return;

	/* Opus/AL live exactly as long as a session: nothing in single-player,
	   nothing while hosting an empty lobby is fine too (cheap), torn down
	   by Disconnect. */
	const bool bLive = mbHosting || (mbClientConnected && mbHadJoinPacket);
	if (!bLive)
	{
		if (mpVoice->IsInitialized())
			mpVoice->Shutdown();
		return;
	}
	if (!mpVoice->IsInitialized() && !mpVoice->Init())
		return; /* failed: logged once, retried after the next Disconnect */

	mpVoice->SetLocalPlayerId(mlLocalPlayerId);

	/* every drawn ghost's mouth this frame (interpolated render pose) */
	for (tGhostMap::const_iterator it = m_mapGhosts.begin(); it != m_mapGhosts.end(); ++it)
	{
		if (!it->second)
			continue;
		cVector3f vFeet;
		if (it->second->GetRenderFeetPos(&vFeet))
			mpVoice->SetRemoteHeadPos(it->first, vFeet + cVector3f(0, kVoiceHeadHeight, 0), true);
	}

	/* push-to-talk (ignored by the open-mic gate inside) */
	bool bTalk = false;
	hpl::cInput *inp = (mpInit && mpInit->mpGame) ? mpInit->mpGame->GetInput() : NULL;
	if (inp)
		bTalk = inp->IsTriggerd("VoiceTalk");

	mpVoice->Update(afTimeStep, bTalk);

	/* outbox -> wire (host: every guest; guest: the host, which relays) */
	const size_t n = mpVoice->GetOutgoingCount();
	for (size_t i = 0; i < n; ++i)
	{
		const std::vector<uint8_t> &pkt = mpVoice->GetOutgoing(i);
		if (!pkt.empty())
			SendUnreliableEvent(&pkt[0], pkt.size());
	}
	if (n > 0)
		mpVoice->ClearOutgoing();
}

#endif /* PENUMBRA_MULTIPLAYER */
