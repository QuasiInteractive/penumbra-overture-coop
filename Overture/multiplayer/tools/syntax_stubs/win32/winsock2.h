// syntax-check stub: winsock2 surface used by NetworkManager.cpp / enet, --win32 mode only
#pragma once
#include <windows.h>
typedef unsigned long u_long; typedef unsigned int u_int; typedef unsigned short u_short; typedef unsigned char u_char;
typedef size_t SOCKET;
#define INVALID_SOCKET ((SOCKET)(~0))
#define SOCKET_ERROR (-1)
typedef int socklen_t;
struct in_addr { union { struct { u_char s_b1, s_b2, s_b3, s_b4; } S_un_b; u_long S_addr; } S_un; };
#define s_addr S_un.S_addr
struct sockaddr { u_short sa_family; char sa_data[14]; };
struct sockaddr_in { short sin_family; u_short sin_port; struct in_addr sin_addr; char sin_zero[8]; };
// timeval / fd_set / select come from glibc's <sys/select.h> (pulled in by <cstdlib>).
typedef struct WSAData { WORD wVersion; WORD wHighVersion; char szDescription[257]; char szSystemStatus[129]; unsigned short iMaxSockets; unsigned short iMaxUdpDg; char* lpVendorInfo; } WSADATA, *LPWSADATA;
typedef struct _WSAOVERLAPPED { DWORD Internal, InternalHigh, Offset, OffsetHigh; HANDLE hEvent; } WSAOVERLAPPED, *LPWSAOVERLAPPED;
typedef void (*LPWSAOVERLAPPED_COMPLETION_ROUTINE)(DWORD, DWORD, LPWSAOVERLAPPED, DWORD);
#define MAKEWORD(a, b) ((WORD)(((BYTE)(a)) | ((WORD)((BYTE)(b))) << 8))
#define AF_INET 2
#define PF_INET 2
#define SOCK_STREAM 1
#define SOCK_DGRAM 2
#define IPPROTO_TCP 6
#define IPPROTO_UDP 17
#define INADDR_ANY 0x00000000UL
#define INADDR_BROADCAST 0xFFFFFFFFUL
#define INADDR_NONE 0xFFFFFFFFUL
#define INADDR_LOOPBACK 0x7F000001UL
#define SOL_SOCKET 0xFFFF
#define SO_REUSEADDR 0x0004
#define SO_BROADCAST 0x0020
#define SO_RCVBUF 0x1002
#define SO_SNDBUF 0x1001
#define SO_RCVTIMEO 0x1006
#define SO_SNDTIMEO 0x1005
#define SO_ERROR 0x1007
#define TCP_NODELAY 0x0001
#define FIONBIO 0x8004667EUL
#define IOC_VENDOR 0x18000000
#define IOC_IN 0x80000000
#define _WSAIOW(x, y) (IOC_IN | (x) | (y))
#define WSAEWOULDBLOCK 10035
#define WSAEMSGSIZE 10040
#define WSAECONNRESET 10054
#define WSAEINTR 10004
#define WSAEINPROGRESS 10036
#define WSAENOTCONN 10057
#define WSAECONNREFUSED 10061
#define WSAEADDRINUSE 10048
#define WSAETIMEDOUT 10060
extern "C" {
int WSAStartup(WORD, LPWSADATA); int WSACleanup(void); int WSAGetLastError(void); void WSASetLastError(int);
int WSAIoctl(SOCKET, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPWSAOVERLAPPED, LPWSAOVERLAPPED_COMPLETION_ROUTINE);
SOCKET socket(int, int, int); int bind(SOCKET, const struct sockaddr*, int); int closesocket(SOCKET); int ioctlsocket(SOCKET, long, u_long*); int setsockopt(SOCKET, int, int, const char*, int); int getsockopt(SOCKET, int, int, char*, int*);
int sendto(SOCKET, const char*, int, int, const struct sockaddr*, int); int recvfrom(SOCKET, char*, int, int, struct sockaddr*, int*); int send(SOCKET, const char*, int, int); int recv(SOCKET, char*, int, int);
int connect(SOCKET, const struct sockaddr*, int); int listen(SOCKET, int); SOCKET accept(SOCKET, struct sockaddr*, int*); int getsockname(SOCKET, struct sockaddr*, int*); int shutdown(SOCKET, int);
unsigned long inet_addr(const char*); char* inet_ntoa(struct in_addr); u_short htons(u_short); u_short ntohs(u_short); u_long htonl(u_long); u_long ntohl(u_long); int gethostname(char*, int);
struct hostent { char* h_name; char** h_aliases; short h_addrtype; short h_length; char** h_addr_list; }; struct hostent* gethostbyname(const char*); struct hostent* gethostbyaddr(const char*, int, int);
}
