// syntax-check stub
#pragma once
#include <cstddef>
#include <cstdint>
#ifdef _WIN32
#include <winsock2.h>
#endif
typedef uint8_t enet_uint8; typedef uint16_t enet_uint16; typedef uint32_t enet_uint32;
typedef struct _ENetAddress { enet_uint32 host; enet_uint16 port; } ENetAddress;
typedef struct _ENetPacket { size_t referenceCount; enet_uint32 flags; enet_uint8* data; size_t dataLength; void (*freeCallback)(struct _ENetPacket*); void* userData; } ENetPacket;
typedef struct _ENetHost ENetHost;
typedef struct _ENetPeer { void* pad0; ENetHost* host; enet_uint16 outgoingPeerID; enet_uint16 incomingPeerID; enet_uint32 connectID; enet_uint8 outgoingSessionID, incomingSessionID; ENetAddress address; void* data; int state; void* channels; size_t channelCount; enet_uint32 incomingBandwidth, outgoingBandwidth, incomingBandwidthThrottleEpoch, outgoingBandwidthThrottleEpoch, incomingDataTotal, outgoingDataTotal, lastSendTime, lastReceiveTime, nextTimeout, earliestTimeout, packetLossEpoch, packetsSent, packetsLost, packetLoss, packetLossVariance, packetThrottle, packetThrottleLimit, packetThrottleCounter, packetThrottleEpoch, packetThrottleAcceleration, packetThrottleDeceleration, packetThrottleInterval, pingInterval, timeoutLimit, timeoutMinimum, timeoutMaximum, lastRoundTripTime, lowestRoundTripTime, lastRoundTripTimeVariance, highestRoundTripTimeVariance, roundTripTime, roundTripTimeVariance, mtu, windowSize, reliableDataInTransit; enet_uint16 outgoingReliableSequenceNumber; enet_uint32 eventData; size_t totalWaitingData; } ENetPeer;
struct _ENetHost { int socket; ENetAddress address; enet_uint32 incomingBandwidth, outgoingBandwidth, bandwidthThrottleEpoch, mtu, randomSeed; int recalculateBandwidthLimits; ENetPeer* peers; size_t peerCount; size_t channelLimit; enet_uint32 serviceTime; size_t totalSentData, totalSentPackets, totalReceivedData, totalReceivedPackets; size_t connectedPeers, bandwidthLimitedPeers, duplicatePeers, maximumPacketSize, maximumWaitingData; ENetAddress receivedAddress; enet_uint8* receivedData; size_t receivedDataLength; };
typedef enum _ENetEventType { ENET_EVENT_TYPE_NONE = 0, ENET_EVENT_TYPE_CONNECT = 1, ENET_EVENT_TYPE_DISCONNECT = 2, ENET_EVENT_TYPE_RECEIVE = 3 } ENetEventType;
typedef enum _ENetPeerState { ENET_PEER_STATE_DISCONNECTED = 0, ENET_PEER_STATE_CONNECTING, ENET_PEER_STATE_ACKNOWLEDGING_CONNECT, ENET_PEER_STATE_CONNECTION_PENDING, ENET_PEER_STATE_CONNECTION_SUCCEEDED, ENET_PEER_STATE_CONNECTED, ENET_PEER_STATE_DISCONNECT_LATER, ENET_PEER_STATE_DISCONNECTING, ENET_PEER_STATE_ACKNOWLEDGING_DISCONNECT, ENET_PEER_STATE_ZOMBIE } ENetPeerState;
typedef struct _ENetEvent { ENetEventType type; ENetPeer* peer; enet_uint8 channelID; enet_uint32 data; ENetPacket* packet; } ENetEvent;
typedef int ENetSocket;
typedef struct _ENetBuffer { void* data; size_t dataLength; } ENetBuffer;
enum { ENET_PACKET_FLAG_RELIABLE = 1, ENET_PACKET_FLAG_UNSEQUENCED = 2, ENET_PACKET_FLAG_NO_ALLOCATE = 4, ENET_PACKET_FLAG_UNRELIABLE_FRAGMENT = 8 };
enum { ENET_HOST_ANY = 0, ENET_HOST_BROADCAST = 0xFFFFFFFFU, ENET_PORT_ANY = 0 };
enum { ENET_SOCKET_TYPE_STREAM = 1, ENET_SOCKET_TYPE_DATAGRAM = 2 };
enum { ENET_SOCKOPT_NONBLOCK = 1, ENET_SOCKOPT_BROADCAST = 2, ENET_SOCKOPT_RCVBUF = 3, ENET_SOCKOPT_SNDBUF = 4, ENET_SOCKOPT_REUSEADDR = 5, ENET_SOCKOPT_RCVTIMEO = 6, ENET_SOCKOPT_SNDTIMEO = 7, ENET_SOCKOPT_ERROR = 8, ENET_SOCKOPT_NODELAY = 9 };
enum { ENET_PEER_PACKET_THROTTLE_SCALE = 32, ENET_PEER_PACKET_THROTTLE_ACCELERATION = 2, ENET_PEER_PACKET_THROTTLE_DECELERATION = 2, ENET_PEER_PACKET_THROTTLE_INTERVAL = 5000, ENET_PEER_TIMEOUT_LIMIT = 32, ENET_PEER_TIMEOUT_MINIMUM = 5000, ENET_PEER_TIMEOUT_MAXIMUM = 30000, ENET_PEER_PING_INTERVAL = 500 };
#define ENET_SOCKET_NULL -1
#define ENET_VERSION_MAJOR 1
#define ENET_VERSION_MINOR 3
#define ENET_VERSION_PATCH 17
extern "C" {
int enet_initialize(void); void enet_deinitialize(void); enet_uint32 enet_time_get(void); void enet_time_set(enet_uint32);
int enet_address_set_host(ENetAddress*, const char*); int enet_address_set_host_ip(ENetAddress*, const char*); int enet_address_get_host_ip(const ENetAddress*, char*, size_t); int enet_address_get_host(const ENetAddress*, char*, size_t);
ENetHost* enet_host_create(const ENetAddress*, size_t, size_t, enet_uint32, enet_uint32); void enet_host_destroy(ENetHost*); ENetPeer* enet_host_connect(ENetHost*, const ENetAddress*, size_t, enet_uint32); int enet_host_service(ENetHost*, ENetEvent*, enet_uint32); int enet_host_check_events(ENetHost*, ENetEvent*); void enet_host_flush(ENetHost*); void enet_host_broadcast(ENetHost*, enet_uint8, ENetPacket*); void enet_host_channel_limit(ENetHost*, size_t); void enet_host_bandwidth_limit(ENetHost*, enet_uint32, enet_uint32); void enet_host_compress(ENetHost*, const void*); int enet_host_compress_with_range_coder(ENetHost*);
ENetPacket* enet_packet_create(const void*, size_t, enet_uint32); void enet_packet_destroy(ENetPacket*); int enet_packet_resize(ENetPacket*, size_t);
int enet_peer_send(ENetPeer*, enet_uint8, ENetPacket*); ENetPacket* enet_peer_receive(ENetPeer*, enet_uint8*); void enet_peer_ping(ENetPeer*); void enet_peer_ping_interval(ENetPeer*, enet_uint32); void enet_peer_timeout(ENetPeer*, enet_uint32, enet_uint32, enet_uint32); void enet_peer_reset(ENetPeer*); void enet_peer_disconnect(ENetPeer*, enet_uint32); void enet_peer_disconnect_now(ENetPeer*, enet_uint32); void enet_peer_disconnect_later(ENetPeer*, enet_uint32); void enet_peer_throttle_configure(ENetPeer*, enet_uint32, enet_uint32, enet_uint32);
ENetSocket enet_socket_create(int); int enet_socket_bind(ENetSocket, const ENetAddress*); int enet_socket_get_address(ENetSocket, ENetAddress*); int enet_socket_listen(ENetSocket, int); ENetSocket enet_socket_accept(ENetSocket, ENetAddress*); int enet_socket_connect(ENetSocket, const ENetAddress*); int enet_socket_send(ENetSocket, const ENetAddress*, const ENetBuffer*, size_t); int enet_socket_receive(ENetSocket, ENetAddress*, ENetBuffer*, size_t); int enet_socket_wait(ENetSocket, enet_uint32*, enet_uint32); int enet_socket_set_option(ENetSocket, int, int); int enet_socket_get_option(ENetSocket, int, int*); int enet_socket_shutdown(ENetSocket, int); void enet_socket_destroy(ENetSocket); int enet_socket_select(ENetSocket, void*, void*, enet_uint32);
}
