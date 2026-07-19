#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "StdAfx.h"
#include "NetworkManager.h"
#include "BodySync.h"
#include "system/String.h"
#include "Init.h"
#include "MapHandler.h"
#include "GameEntity.h" /* iGameEntity for the one-of-each item sweep */
#include "GameEnemy.h"   /* Phase 6: shared-enemy streaming */
#include "CharacterMove.h"
#include "GameScripts.h" /* v9: NetApplyScriptEvent */

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

//======================================================================

#ifndef PENUMBRA_MULTIPLAYER

struct cNetworkManager::Impl {};

const float cNetworkManager::kSendPeriodSeconds = 1.0f / 30.0f; /* v10:
    20 -> 30 Hz — noticeably smoother object/enemy motion; ~18 KB/s peak
    is still nothing for any internet link */
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
	  , mvDiscovered()
	  , mbDiscoveryActive(false)
	  , mfDiscoveryTimeLeft(0)
	  , msServerName("Penumbra Server")
	  , mlMaxPlayers(4)
	  , mpBodySync(new cBodySync())
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
	Disconnect();
	ClearGhostsInternal();
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

void cNetworkManager::Disconnect()
{
	mbHosting = false;
	mbClientConnected = false;
	mbHadJoinPacket = false;
	mlLocalPlayerId = 0;
	mlNextGuestId = 2;
	msDeferredJoinAddress = "";
}

void cNetworkManager::Update(float /*afTimeStep*/)
{
}

void cNetworkManager::ClearGhostsInternal()
{
	m_mapGhostSeq.clear(); /* new session, new counters */
	for (std::map<uint8_t, cGhostPlayer *>::iterator it = m_mapGhosts.begin(); it != m_mapGhosts.end(); ++it)
		hplDelete(it->second);
	m_mapGhosts.clear();
}

bool cNetworkManager::BuildLocalSnapshot(cNetPlayerState *apOut) const
{
	if (!apOut || !mpInit || !mpInit->mpPlayer)
		return false;
	cCamera3D *cam = mpInit->mpPlayer->GetCamera();
	if (!cam)
		return false;

	cVector3f p = cam->GetPosition();
	apOut->mType = eNetPacketType_PlayerState;
	apOut->mPlayerID = mlLocalPlayerId;
	apOut->mfPosX = p.x;
	apOut->mfPosY = p.y;
	apOut->mfPosZ = p.z;
	apOut->mfPitch = cam->GetPitch();
	apOut->mfYaw = cam->GetYaw();
	cPlayerFlashLight *fl = mpInit->mpPlayer->GetFlashLight();
	apOut->mbFlashlightOn = (uint8_t)(fl && fl->IsActive() && !fl->IsDisabled());
	/* Engine -> wire stance mapping, explicit per state so an engine enum
	   reorder cannot silently change the protocol (the wire values are frozen
	   — see eNetMoveState). Unknown states go out as Run. */
	switch (mpInit->mpPlayer->GetMoveState())
	{
	case ePlayerMoveState_Walk:   apOut->mMoveState = eNetMoveState_Walk;   break;
	case ePlayerMoveState_Run:    apOut->mMoveState = eNetMoveState_Run;    break;
	case ePlayerMoveState_Still:  apOut->mMoveState = eNetMoveState_Still;  break;
	case ePlayerMoveState_Jump:   apOut->mMoveState = eNetMoveState_Jump;   break;
	case ePlayerMoveState_Crouch: apOut->mMoveState = eNetMoveState_Crouch; break;
	default:                      apOut->mMoveState = eNetMoveState_Run;    break;
	}
	return mlLocalPlayerId != 0;
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
void cNetworkManager::SendPlayerDamage(uint8_t, float) {}
void cNetworkManager::NetOnEnemyDamaged(const hpl::tString &, float, int) {}
bool cNetworkManager::PartyHasItem(const hpl::tString &) const { return false; }
void cNetworkManager::NetOnScriptEvent(int, const hpl::tString &, int) {}
void cNetworkManager::NetOnEntityDamaged(const hpl::tString &, float, int) {}

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

	Impl()
		: mpHost(NULL)
		  , mpServerPeer(NULL)
		  , mDiscoveryListenSock(INVALID_SOCKET)
		  , mDiscoveryBrowseSock(INVALID_SOCKET)
		  , mbBrowseHoldsNetRef(false)
		  , mlHeldHash(0)
		  , mvGrabTarget(0, 0, 0)
		  , mbHaveGrabTarget(false)
		  , mlPushHash(0)
		  , mvPushImpulse(0, 0, 0)
		  , mvPushPoint(0, 0, 0)
		  , mbPushStop(false)
		  , mbPushAny(false)
	{
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

const float cNetworkManager::kSendPeriodSeconds = 1.0f / 30.0f; /* v10:
    20 -> 30 Hz — noticeably smoother object/enemy motion; ~18 KB/s peak
    is still nothing for any internet link */
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

static const hpl::tString &GhostMeshPathForId(const std::vector<hpl::tString> &paths,
											  uint8_t id)
{
	static const hpl::tString sEmptyGhostMesh;
	if (paths.empty() || id == 0)
		return sEmptyGhostMesh;
	return paths[(size_t)(id - 1) % paths.size()];
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
		if (rp == skip || rp->state != ENET_PEER_STATE_CONNECTED)
			continue;
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
	  , mvDiscovered()
	  , mbDiscoveryActive(false)
	  , mfDiscoveryTimeLeft(0)
	  , msServerName("Penumbra Server")
	  , mlMaxPlayers(4)
	  , mpBodySync(new cBodySync())
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
	Disconnect();
	ClearGhostsInternal();
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
	Log(" multiplayer: F11=toggle HOST :%u — F10=JOIN 127.0.0.1:%u — F9=LAN discovery — menu Multiplayer · multiplayer.cfg\n",
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
		if (sscanf(buf, " %63[^=]", rawKey) == 1 && strcmp(rawKey, "server_name") == 0)
		{
			char *eq = strchr(buf, '=');
			if (eq && eq[1])
			{
				char *nm = eq + 1;
				while (*nm == ' ' || *nm == '\t')
					++nm;
				size_t ln = strlen(nm);
				while (ln > 0 && (nm[ln - 1] == '\n' || nm[ln - 1] == '\r' ||
								  nm[ln - 1] == ' ' || nm[ln - 1] == '\t'))
					--ln;
				if (ln > 31)
					ln = 31; /* wire field is char[32] */
				if (ln > 0)
					msServerName = hpl::tString(nm, ln);
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
		else if (strcmp(key, "max_players") == 0)
		{
			int mp = atoi(val);
			if (mp < 2)
				mp = 2;
			if (mp > 31) /* enet_host_create is sized for 31 peers */
				mp = 31;
			mlMaxPlayers = (uint8_t)mp;
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
	for (std::map<uint8_t, cGhostPlayer *>::iterator it = m_mapGhosts.begin(); it != m_mapGhosts.end(); ++it)
		hplDelete(it->second);
	m_mapGhosts.clear();
}

void cNetworkManager::DropRemotePlayer(uint8_t id)
{
	m_mapGhostSeq.erase(id); /* a rejoiner restarts its counter */
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
	const hpl::tString &mesh = GhostMeshPathForId(mvGhostMeshPaths, id);
	/* Feet-on-ground by construction: the player camera sits at
	 *   feetY + bodySize.y/2 + CameraHeightAdd            (Player.cpp:376,968)
	 * with bodySize.y = Player/Height standing, Player/CrouchHeight crouched
	 * (Player.cpp:71-80,989). The ghost mesh origin is at its feet, so
	 * cameraY - (H/2 + add) grounds it exactly, using the SAME game.cfg
	 * values that position the player. multiplayer.cfg keys stay as
	 * explicit overrides (any value < 9000 wins). */
	const float fCamAdd  = mpInit->mpGameConfig->GetFloat("Player", "CameraHeightAdd", 0);
	const float fStandH  = mpInit->mpGameConfig->GetFloat("Player", "Height", 1.9f);
	const float fCrouchH = mpInit->mpGameConfig->GetFloat("Player", "CrouchHeight", 1.0f);
	/* EMPIRICAL +0.71: the camera rides ~0.71 m HIGHER above the feet than
	   the H/2+add guess. Measured by four rounds of user eyeball-tuning
	   ("auto" -0.74 -> -1.45 grounded) against clip files that verify as
	   perfectly grounded (FK toes at 0.000 m) — so the discrepancy is in
	   this formula, not the models. Crouch agrees: -(0.475-0.085)-0.71 =
	   -1.10, the user-approved crouch value. (A further -5 cm was tried and
	   REVERTED: in-game the mesh visibly sank shin-deep — 0.71 is the value
	   that matches the verified-grounded clips.) */
	const float kCamFeetCorrection = 0.71f;
	float fStandOffset = (mfGhostMeshBodyYOffset > 9000.0f)
		? -(fStandH * 0.5f + fCamAdd + kCamFeetCorrection)
		: mfGhostMeshBodyYOffset;
	float fCrouchOffset = (mfGhostMeshBodyYOffsetCrouch > 9000.0f)
		? -(fCrouchH * 0.5f + fCamAdd + kCamFeetCorrection)
		: mfGhostMeshBodyYOffsetCrouch;
	/* Per-mesh overrides: different rigs ground at different heights. Indexed
	   like the mesh list; a stand list alone keeps the stand->crouch delta. */
	if (mvGhostMeshPaths.empty() == false && mvGhostBodyYList.empty() == false)
	{
		const size_t lMeshIdx = (size_t)(id - 1) % mvGhostMeshPaths.size();
		const float fPerMesh = mvGhostBodyYList[lMeshIdx % mvGhostBodyYList.size()];
		fCrouchOffset += fPerMesh - fStandOffset;
		fStandOffset = fPerMesh;
		if (mvGhostBodyYCrouchList.empty() == false)
			fCrouchOffset = mvGhostBodyYCrouchList[lMeshIdx % mvGhostBodyYCrouchList.size()];
	}
	m_mapGhosts[id] =
		hplNew(cGhostPlayer, (mpWorld, id, mesh, fStandOffset, fCrouchOffset));
}

bool cNetworkManager::BuildLocalSnapshot(cNetPlayerState *apOut) const
{
	if (!apOut || !mpInit || !mpInit->mpPlayer)
		return false;
	cCamera3D *cam = mpInit->mpPlayer->GetCamera();
	if (!cam)
		return false;

	cVector3f p = cam->GetPosition();
	apOut->mType = eNetPacketType_PlayerState;
	apOut->mPlayerID = mlLocalPlayerId;
	apOut->mfPosX = p.x;
	apOut->mfPosY = p.y;
	apOut->mfPosZ = p.z;
	apOut->mfPitch = cam->GetPitch();
	apOut->mfYaw = cam->GetYaw();
	cPlayerFlashLight *fl = mpInit->mpPlayer->GetFlashLight();
	apOut->mbFlashlightOn = (uint8_t)(fl && fl->IsActive() && !fl->IsDisabled());
	/* Engine -> wire stance mapping, explicit per state so an engine enum
	   reorder cannot silently change the protocol (the wire values are frozen
	   — see eNetMoveState). Unknown states go out as Run. */
	switch (mpInit->mpPlayer->GetMoveState())
	{
	case ePlayerMoveState_Walk:   apOut->mMoveState = eNetMoveState_Walk;   break;
	case ePlayerMoveState_Run:    apOut->mMoveState = eNetMoveState_Run;    break;
	case ePlayerMoveState_Still:  apOut->mMoveState = eNetMoveState_Still;  break;
	case ePlayerMoveState_Jump:   apOut->mMoveState = eNetMoveState_Jump;   break;
	case ePlayerMoveState_Crouch: apOut->mMoveState = eNetMoveState_Crouch; break;
	default:                      apOut->mMoveState = eNetMoveState_Run;    break;
	}
	return mlLocalPlayerId != 0;
}

void cNetworkManager::DispatchIncoming(const void *data, size_t len)
{
	if (!data || len < sizeof(cNetPlayerJoin))
		return;
	uint8_t t = *(const uint8_t *)data;

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
		return;
	}

	if (t == eNetPacketType_PlayerLeave && len >= sizeof(cNetPlayerLeave))
	{
		const cNetPlayerLeave *lv = (const cNetPlayerLeave *)data;
		DropRemotePlayer(lv->mPlayerID);
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
			mpBodySync->OnCensusReceived(census);
		}
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
	EnsureGhost(st->mPlayerID);
	std::map<uint8_t, cGhostPlayer *>::iterator gi = m_mapGhosts.find(st->mPlayerID);
	if (gi != m_mapGhosts.end())
		gi->second->ApplyState(*st);

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
		/* wire pos is the host CAMERA; feet = cam - stand eye height */
		cVector3f vFeet(st->mfPosX, st->mfPosY - 1.50f + 0.25f, st->mfPosZ);
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
			if (pd->state != ENET_PEER_STATE_CONNECTED)
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
		if (mpImpl->mpHost->peers[i].state == ENET_PEER_STATE_CONNECTED)
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
		if (pd->state != ENET_PEER_STATE_CONNECTED)
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
		if (pd->state != ENET_PEER_STATE_CONNECTED)
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
				if (ev.data != kNetConnectData)
				{
					/* old exe (sends 0) or foreign client — refuse BEFORE any
					   state flows; half-compatible is the worst outcome */
					Log(" multiplayer: REFUSED incompatible peer (connect data 0x%08X, want 0x%08X)\n",
						(unsigned)ev.data, (unsigned)kNetConnectData);
					enet_peer_disconnect(ev.peer, kNetDisconnectBadVersion);
					break;
				}
				if (mlNextGuestId == 0)
				{
					Log(" multiplayer: guest id overflow\n");
					break;
				}
				uint8_t aid = mlNextGuestId++;
				PeerSetId(ev.peer, aid);
				{
					/* FIRST reliable packet: proves we speak their protocol
					   (their join-handler refuses old hosts that skip this) */
					cNetVersionAck ack;
					ack.mType = eNetPacketType_VersionAck;
					ack.mlVersion = kNetProtocolVersion;
					SendStructToPeer(ev.peer, &ack, sizeof(ack), true);
				}
				SendPlayerJoin(ev.peer, aid);
				SendCensus(ev.peer); /* late joiner gets the map-load census now */
				SendMapBeacon(ev.peer); /* ...and where the party is, so a guest
				    sitting in the menu launches straight into our map */
				/* ...and one reliable full snapshot, so a mid-game joiner
				   starts from the host's exact current poses instead of the
				   map defaults (rung 2). Chunked to stay under MTU. */
				{
					unsigned char aSnapBuf[cBodySync::kMaxBatchBytes];
					uint32_t lCursor = 0;
					int lChunks = 0;
					size_t lLen;
					while ((lLen = mpBodySync->BuildSnapshotChunk(aSnapBuf, &lCursor)) != 0)
					{
						ENetPacket *sp = enet_packet_create(aSnapBuf, lLen, ENET_PACKET_FLAG_RELIABLE);
						if (sp)
							enet_peer_send(ev.peer, 0, sp);
						++lChunks;
					}
					if (lChunks > 0)
						Log(" multiplayer: full body snapshot -> peer id=%u (%d chunk(s))\n",
							(unsigned)aid, lChunks);
				}
				Log(" multiplayer: peer connected id=%u\n", (unsigned)aid);
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
				Log(" multiplayer: host disconnected\n");
				mbClientConnected = false;
				mbHadJoinPacket = false;
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
					/* rung 3: a vanished guest drops whatever it held */
					int lFreed = mpBodySync->ReleaseAllHeldBy(gone);
					if (lFreed > 0)
						Log(" multiplayer: guest %u left holding %d object(s) — released\n",
							(unsigned)gone, lFreed);
				}
				PeerSetId(ev.peer, 0);
			}
			break;

		case ENET_EVENT_TYPE_RECEIVE:
		{
			ENetPacket *pk = ev.packet;
			if (!pk || pk->dataLength == 0)
				break;

			if (mbHosting)
			{
				uint8_t author = PeerGetId(ev.peer);
				const uint8_t lFirst = *(const uint8_t *)pk->data;
				if (lFirst >= eNetPacketType_BodyGrabBegin && lFirst <= eNetPacketType_BodyPush)
				{
					/* rung 3 intent needs to know WHICH guest sent it */
					HandleBodyIntent(author, pk->data, (size_t)pk->dataLength);
				}
				else if ((size_t)pk->dataLength >= sizeof(cNetPlayerState) &&
					lFirst == eNetPacketType_PlayerState && author >= 2)
				{
					cNetPlayerState relay;
					memcpy(&relay, pk->data, sizeof(relay));
					relay.mPlayerID = author;
					for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
					{
						ENetPeer *dst = &mpImpl->mpHost->peers[i];
						if (dst == ev.peer || dst->state != ENET_PEER_STATE_CONNECTED)
							continue;
						ENetPacket *rp =
							enet_packet_create(&relay, sizeof(relay), ENET_PACKET_FLAG_UNSEQUENCED);
						if (rp)
							enet_peer_send(dst, 1, rp);
					}
					DispatchIncoming(&relay, sizeof(relay));
				}
				else if (author >= 2 && (lFirst == eNetPacketType_MapChange ||
					lFirst == eNetPacketType_ItemPickup ||
					lFirst == eNetPacketType_ItemDrop ||
					lFirst == eNetPacketType_ScriptEvent ||
					lFirst == eNetPacketType_EntityDamage))
				{
					for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
					{
						ENetPeer *dst = &mpImpl->mpHost->peers[i];
						if (dst == ev.peer || dst->state != ENET_PEER_STATE_CONNECTED)
							continue;
						SendStructToPeer(dst, pk->data, (size_t)pk->dataLength, true);
					}
					DispatchIncoming(pk->data, (size_t)pk->dataLength);
				}
				else
					DispatchIncoming(pk->data, (size_t)pk->dataLength);
			}
			else
				DispatchIncoming(pk->data, (size_t)pk->dataLength);

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
	mbHadJoinPacket = true;
	mlListenPort = alPort;
	mbClientConnected = false;
	Log(" multiplayer: HOST udp/%u\n", (unsigned)alPort);

	OpenHostDiscovery();
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
			if (pd->state == ENET_PEER_STATE_CONNECTED)
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
	int n = 0;
	for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
		if (mpImpl->mpHost->peers[i].state == ENET_PEER_STATE_CONNECTED)
			++n;
	return n;
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
void cNetworkManager::EmitEnemyStates()
{
	if (!mbHosting || !mpImpl || !mpImpl->mpHost || !mpInit || !mpInit->mpMapHandler)
		return;

	unsigned char aBuf[sizeof(cNetEnemyBatch) + 8 * sizeof(cNetEnemyState)];
	int lCount = 0;
	tGameEnemyIterator it = mpInit->mpMapHandler->GetGameEnemyIterator();
	while (it.HasNext() && lCount < 8)
	{
		iGameEnemy *pEnemy = it.Next();
		if (pEnemy == NULL || pEnemy->GetMover() == NULL)
			continue;
		iCharacterBody *pBody = pEnemy->GetMover()->GetCharBody();
		if (pBody == NULL)
			continue;

		cNetEnemyState st;
		st.mlNameHash = NetHashName(pEnemy->GetName().c_str());
		const cVector3f v = pBody->GetFeetPosition();
		st.mfPosX = v.x; st.mfPosY = v.y; st.mfPosZ = v.z;
		st.mfYaw = pBody->GetYaw();
		st.mfHealth = pEnemy->GetHealth();
		st.mlAnimHash = pEnemy->GetNetAnimHash();
		st.mFlags = (uint8_t)((pEnemy->GetNetAnimLoop() ? 1 : 0) |
			(pEnemy->IsActive() ? 2 : 0));
		memcpy(aBuf + sizeof(cNetEnemyBatch) + (size_t)lCount * sizeof(cNetEnemyState),
			&st, sizeof(st));
		++lCount;
	}
	if (lCount == 0)
		return;

	cNetEnemyBatch hdr;
	hdr.mType = eNetPacketType_EnemyState;
	hdr.mCount = (uint8_t)lCount;
	hdr.mMapGen = 0; /* reserved — unknown hashes are already inert */
	hdr.mSeq = ++mlEnemySeqOut;
	memcpy(aBuf, &hdr, sizeof(hdr));
	const size_t lLen = sizeof(hdr) + (size_t)lCount * sizeof(cNetEnemyState);

	for (size_t i = 0; i < mpImpl->mpHost->peerCount; ++i)
	{
		ENetPeer *pd = &mpImpl->mpHost->peers[i];
		if (pd->state != ENET_PEER_STATE_CONNECTED)
			continue;
		ENetPacket *pkt = enet_packet_create(aBuf, lLen, ENET_PACKET_FLAG_UNSEQUENCED);
		if (pkt)
			enet_peer_send(pd, 1, pkt);
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

	for (size_t i = 0; i < lCount; ++i)
	{
		cNetEnemyState st;
		memcpy(&st, pRaw + sizeof(cNetEnemyBatch) + i * sizeof(cNetEnemyState), sizeof(st));

		iGameEnemy *pEnemy = NULL;
		tGameEnemyIterator eit = mpInit->mpMapHandler->GetGameEnemyIterator();
		while (eit.HasNext())
		{
			iGameEnemy *pE = eit.Next();
			if (pE && NetHashName(pE->GetName().c_str()) == st.mlNameHash)
			{
				pEnemy = pE;
				break;
			}
		}
		if (pEnemy == NULL)
			continue; /* different map or a despawned enemy */
		if (pEnemy->GetHealth() <= 0)
			continue; /* already locally dead: the ragdoll owns it */

		if (st.mfHealth <= 0)
		{
			/* host says it died: run the LOCAL death for ragdoll/sounds */
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
	CloseHostDiscovery();
	StopDiscovery(); /* keeps results; frees the browse socket + its net ref */

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
	msDeferredJoinAddress = "";
	if (mpImpl)
		mpImpl->ResetIntent(); /* a dead session forwards nothing */
	if (mpBodySync)
		mpBodySync->ClearGuestHeld();
}

void cNetworkManager::Update(float afTimeStep)
{
	if (!mpInit || !mpInit->mpGame)
		return;

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
		mpWorld = w;
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
	}

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

	mpBodySync->LogStatsTick(afTimeStep);
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

void cNetworkManager::StartDiscovery()
{
	if (!mpImpl)
		return;

	/* Refresh semantics: drop the previous scan (socket + results) and re-ping. */
	StopDiscovery();
	mvDiscovered.clear();

	if (!NetAcquire()) /* WSAStartup may not be up yet — browsing can start from the menu */
		return;
	mpImpl->mbBrowseHoldsNetRef = true;

	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET)
	{
		Log(" multiplayer: discovery browse socket failed (err %d)\n", WSAGetLastError());
		StopDiscovery();
		return;
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
		StopDiscovery();
		return;
	}

	TameUdpSocket(s);
	mpImpl->mDiscoveryBrowseSock = s;
	mbDiscoveryActive = true;
	mfDiscoveryTimeLeft = kDiscoveryWindowSeconds;

	SendDiscoveryPings();
}

void cNetworkManager::StopDiscovery()
{
	mbDiscoveryActive = false;
	mfDiscoveryTimeLeft = 0;
	if (!mpImpl)
		return;
	CloseUdpSocket(mpImpl->mDiscoveryBrowseSock);
	if (mpImpl->mbBrowseHoldsNetRef)
	{
		mpImpl->mbBrowseHoldsNetRef = false;
		NetRelease();
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
				ping.mlProtocolMagic != kNetProtocolMagic)
				continue;

			/* Reply even on version mismatch — the browser shows WHY the join
			   would fail instead of the server just not existing. */
			cNetDiscoveryPong pong;
			memset(&pong, 0, sizeof(pong));
			pong.mType = eNetPacketType_DiscoveryPong;
			pong.mlProtocolMagic = kNetProtocolMagic;
			pong.mlProtocolVer = kNetProtocolVersion;
			pong.mlGamePort = mlListenPort;
			pong.mlPlayerCount = (uint8_t)(m_mapGhosts.size() + 1); /* guests + me */
			pong.mlMaxPlayers = mlMaxPlayers;
			CopyPacketString(pong.msServerName, sizeof(pong.msServerName), msServerName.c_str());
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

	/* --- Browser: collect pongs until the window closes ------------------- */
	if (!mbDiscoveryActive || mpImpl->mDiscoveryBrowseSock == INVALID_SOCKET)
		return;

	for (;;)
	{
		char buf[128];
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
		mvDiscovered.push_back(sv);

		Log(" multiplayer: discovered '%s' map='%s' %u/%u at %s%s\n",
			sv.msName.c_str(), sv.msMap.c_str(),
			(unsigned)sv.mlPlayerCount, (unsigned)sv.mlMaxPlayers,
			sv.msAddress.c_str(), sv.mbVersionMatch ? "" : " [VERSION MISMATCH]");
	}

	mfDiscoveryTimeLeft -= afTimeStep;
	if (mfDiscoveryTimeLeft <= 0)
	{
		Log(" multiplayer: discovery done — %u server(s)\n", (unsigned)mvDiscovered.size());
		StopDiscovery();
	}
}

#endif /* PENUMBRA_MULTIPLAYER */
