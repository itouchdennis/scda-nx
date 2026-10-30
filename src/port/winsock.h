#pragma once
// The part of Winsock xdk/xnet.cpp runs system link on, over BSD sockets (Linux, libnx). The guest sees Winsock's
// error numbers, so WSAGetLastError turns errno into them.
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <thread>

using SOCKET = int;
using u_long = unsigned long;
using DWORD = uint32_t;
using BOOL = int;
using WORD = uint16_t;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
#ifndef FALSE
#define FALSE 0
#define TRUE 1
#endif

// Winsock's error numbers, which the guest also uses
enum : int {
    WSA_IO_PENDING = 997,
    WSA_IO_INCOMPLETE = 996,
    WSA_OPERATION_ABORTED = 995,
    WSAEINVAL = 10022,
    WSAEWOULDBLOCK = 10035,
    WSAENOTSOCK = 10038,
    WSAESOCKTNOSUPPORT = 10044,
    WSAEAFNOSUPPORT = 10047,
    WSAECONNRESET = 10054,
    WSAETIMEDOUT = 10060,
    WSANOTINITIALISED = 10093,
    WSAHOST_NOT_FOUND = 11001,
};

struct WSADATA {
    WORD wVersion;
};
inline WORD MAKEWORD(int lo, int hi) { return WORD(uint8_t(lo) | uint8_t(hi) << 8); }
inline int WSAStartup(WORD, WSADATA*) { return 0; }

inline int WSAGetLastError() {
    switch (errno) {
        case EAGAIN:
#if EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:
#endif
        case EINPROGRESS: return WSAEWOULDBLOCK;
        case ECONNRESET:
        case ECONNREFUSED: return WSAECONNRESET;
        case ETIMEDOUT: return WSAETIMEDOUT;
        case EBADF:
        case ENOTSOCK: return WSAENOTSOCK;
        case EAFNOSUPPORT: return WSAEAFNOSUPPORT;
        default: return WSAEINVAL;
    }
}

inline int closesocket(SOCKET s) { return close(s); }
inline int ioctlsocket(SOCKET s, long cmd, u_long* arg) {
    if (cmd != long(FIONBIO)) return -1;
    int flags = fcntl(s, F_GETFL, 0);
    return fcntl(s, F_SETFL, *arg ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
}
// SIO_UDP_CONNRESET: BSD sockets never fail a receive over an earlier send's port unreachable
inline int WSAIoctl(SOCKET, DWORD, void*, DWORD, void*, DWORD, DWORD*, void*, void*) { return 0; }

// the two kernel32 calls xnet.cpp paces itself with
inline uint64_t GetTickCount64() {
    using namespace std::chrono;
    return uint64_t(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}
#ifndef SCDA_SLEEP_DEFINED
#define SCDA_SLEEP_DEFINED
inline void Sleep(DWORD ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }
#endif
