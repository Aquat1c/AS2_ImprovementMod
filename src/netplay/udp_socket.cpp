/**
 * Alice Senki 2 - Non-blocking UDP Socket Implementation
 */

#include "udp_socket.h"
#include "log_window.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace UdpSocket {

static bool s_wsaInitialized = false;

// ============================================================================
// Network Simulation State
// ============================================================================

struct SimConfig {
    int   latencyMs;
    int   jitterMs;
    float lossPercent;
    float dupPercent;
    bool  active;
};

struct DelayedPacket {
    SOCKET      sock;
    sockaddr_in to;
    uint8_t     data[1500];
    int         len;
    DWORD       sendAtMs;  // GetTickCount() when to actually send
    bool        used;
};

static constexpr int kMaxDelayedPackets = 512;
static SimConfig      s_simConfig = {};
static DelayedPacket  s_delayQueue[kMaxDelayedPackets] = {};
static int            s_delayQueueCount = 0;
static uint32_t       s_simRandState = 12345;
static uint32_t       s_simDropCount = 0;
static uint32_t       s_simDelayCount = 0;
static uint32_t       s_simDupCount = 0;

// Simple xorshift for deterministic simulation (no game rand() dependency)
static uint32_t SimRand() {
    s_simRandState ^= s_simRandState << 13;
    s_simRandState ^= s_simRandState >> 17;
    s_simRandState ^= s_simRandState << 5;
    return s_simRandState;
}

static float SimRandFloat() {
    return (float)(SimRand() & 0xFFFF) / 65535.0f;
}

static int SimRandRange(int minVal, int maxVal) {
    if (minVal >= maxVal) return minVal;
    return minVal + (int)(SimRand() % (uint32_t)(maxVal - minVal + 1));
}

bool Init() {
    if (s_wsaInitialized) return true;

    WSADATA wsa;
    int result = WSAStartup(MAKEWORD(2, 2), &wsa);
    if (result != 0) {
        LOG_NET_ERROR("[UdpSocket] WSAStartup failed: %d", result);
        return false;
    }
    s_wsaInitialized = true;
    LOG_NET_INFO("[UdpSocket] Winsock initialized (version %d.%d)", LOBYTE(wsa.wVersion), HIBYTE(wsa.wVersion));
    return true;
}

SOCKET Create(uint16_t bindPort) {
    if (!Init()) return INVALID_SOCKET;

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        LOG_NET_ERROR("[UdpSocket] socket() failed: %d", WSAGetLastError());
        return INVALID_SOCKET;
    }

    // Set non-blocking
    u_long nonBlocking = 1;
    if (ioctlsocket(sock, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
        LOG_NET_ERROR("[UdpSocket] ioctlsocket(FIONBIO) failed: %d", WSAGetLastError());
        closesocket(sock);
        return INVALID_SOCKET;
    }

    // Allow address reuse so we can rebind quickly after restart
    int reuseAddr = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&reuseAddr, sizeof(reuseAddr));

    // Bind
    sockaddr_in bindAddr = {};
    bindAddr.sin_family = AF_INET;
    bindAddr.sin_addr.s_addr = INADDR_ANY;
    bindAddr.sin_port = htons(bindPort);

    if (bind(sock, (sockaddr*)&bindAddr, sizeof(bindAddr)) == SOCKET_ERROR) {
        LOG_NET_ERROR("[UdpSocket] bind(port=%u) failed: %d", bindPort, WSAGetLastError());
        closesocket(sock);
        return INVALID_SOCKET;
    }

    uint16_t actualPort = GetBoundPort(sock);
    LOG_NET_INFO("[UdpSocket] Socket created and bound on port %u (requested %u)", actualPort, bindPort);
    return sock;
}

int SendTo(SOCKET sock, const void* data, int len, const sockaddr_in* to) {
    if (sock == INVALID_SOCKET || !data || len <= 0 || !to) return -1;

    int sent = sendto(sock, (const char*)data, len, 0, (const sockaddr*)to, sizeof(sockaddr_in));
    if (sent == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) return -2;  // Would block, caller must retry
        LOG_NET_ERROR("[UdpSocket] sendto failed: %d", err);
        return -1;
    }
    return sent;
}

int RecvFrom(SOCKET sock, void* buf, int bufLen, sockaddr_in* from) {
    if (sock == INVALID_SOCKET || !buf || bufLen <= 0) return -1;

    sockaddr_in fromAddr = {};
    int fromLen = sizeof(fromAddr);

    int received = recvfrom(sock, (char*)buf, bufLen, 0, (sockaddr*)&fromAddr, &fromLen);
    if (received == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK || err == WSAECONNRESET) return 0;
        LOG_NET_ERROR("[UdpSocket] recvfrom failed: %d", err);
        return -1;
    }

    if (from) *from = fromAddr;
    return received;
}

void Close(SOCKET sock) {
    if (sock != INVALID_SOCKET) {
        closesocket(sock);
        LOG_NET_INFO("[UdpSocket] Socket closed");
    }
}

sockaddr_in MakeAddr(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port) {
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.S_un.S_un_b.s_b1 = a;
    addr.sin_addr.S_un.S_un_b.s_b2 = b;
    addr.sin_addr.S_un.S_un_b.s_b3 = c;
    addr.sin_addr.S_un.S_un_b.s_b4 = d;
    return addr;
}

sockaddr_in MakeAddr(uint32_t ip[4], uint32_t port) {
    return MakeAddr(
        (uint8_t)ip[0], (uint8_t)ip[1], (uint8_t)ip[2], (uint8_t)ip[3],
        (uint16_t)port
    );
}

bool AddrEqual(const sockaddr_in* a, const sockaddr_in* b) {
    if (!a || !b) return false;
    return a->sin_addr.s_addr == b->sin_addr.s_addr &&
           a->sin_port == b->sin_port;
}

void FormatAddr(const sockaddr_in* addr, char* out, int outLen) {
    if (!addr || !out || outLen <= 0) return;
    uint8_t* ip = (uint8_t*)&addr->sin_addr.s_addr;
    _snprintf_s(out, outLen, _TRUNCATE, "%u.%u.%u.%u:%u",
        ip[0], ip[1], ip[2], ip[3], ntohs(addr->sin_port));
}

uint16_t GetBoundPort(SOCKET sock) {
    if (sock == INVALID_SOCKET) return 0;
    sockaddr_in addr = {};
    int len = sizeof(addr);
    if (getsockname(sock, (sockaddr*)&addr, &len) == SOCKET_ERROR) return 0;
    return ntohs(addr.sin_port);
}

// ============================================================================
// Network Condition Simulation
// ============================================================================

void SetSimulation(int latencyMs, int jitterMs, float lossPercent, float dupPercent) {
    s_simConfig.latencyMs   = latencyMs > 0 ? latencyMs : 0;
    s_simConfig.jitterMs    = jitterMs > 0 ? jitterMs : 0;
    s_simConfig.lossPercent = (lossPercent > 0.0f && lossPercent <= 100.0f) ? lossPercent : 0.0f;
    s_simConfig.dupPercent  = (dupPercent > 0.0f && dupPercent <= 100.0f) ? dupPercent : 0.0f;
    s_simConfig.active      = (s_simConfig.latencyMs > 0 || s_simConfig.lossPercent > 0.0f ||
                               s_simConfig.dupPercent > 0.0f);

    s_simDropCount = 0;
    s_simDelayCount = 0;
    s_simDupCount = 0;
    s_delayQueueCount = 0;
    memset(s_delayQueue, 0, sizeof(s_delayQueue));

    if (s_simConfig.active) {
        LOG_NET_INFO("[UdpSocket] Network simulation ACTIVE: latency=%dms jitter=%dms loss=%.1f%% dup=%.1f%%",
                     s_simConfig.latencyMs, s_simConfig.jitterMs,
                     s_simConfig.lossPercent, s_simConfig.dupPercent);
    } else {
        LOG_NET_INFO("[UdpSocket] Network simulation disabled");
    }
}

void TickSimulation() {
    if (!s_simConfig.active) return;

    DWORD now = GetTickCount();
    for (int i = 0; i < kMaxDelayedPackets; i++) {
        DelayedPacket* pkt = &s_delayQueue[i];
        if (!pkt->used) continue;

        if ((int)(now - pkt->sendAtMs) >= 0) {
            // Time to send
            SendTo(pkt->sock, pkt->data, pkt->len, &pkt->to);
            pkt->used = false;
            s_delayQueueCount--;
        }
    }
}

int SimSendTo(SOCKET sock, const void* data, int len, const sockaddr_in* to) {
    if (!s_simConfig.active) {
        return SendTo(sock, data, len, to);
    }

    // Simulate packet loss
    if (s_simConfig.lossPercent > 0.0f) {
        if (SimRandFloat() * 100.0f < s_simConfig.lossPercent) {
            s_simDropCount++;
            return len;  // Pretend we sent it
        }
    }

    // Compute delay
    int delayMs = s_simConfig.latencyMs;
    if (s_simConfig.jitterMs > 0) {
        delayMs += SimRandRange(-s_simConfig.jitterMs, s_simConfig.jitterMs);
        if (delayMs < 0) delayMs = 0;
    }

    // Duplicate check
    bool duplicate = false;
    if (s_simConfig.dupPercent > 0.0f) {
        if (SimRandFloat() * 100.0f < s_simConfig.dupPercent) {
            duplicate = true;
            s_simDupCount++;
        }
    }

    auto enqueue = [&](int extraDelayMs) -> int {
        if (delayMs + extraDelayMs <= 0) {
            return SendTo(sock, data, len, to);
        }

        // Find a free slot
        for (int i = 0; i < kMaxDelayedPackets; i++) {
            DelayedPacket* pkt = &s_delayQueue[i];
            if (pkt->used) continue;

            pkt->sock = sock;
            pkt->to = *to;
            int copyLen = len > 1500 ? 1500 : len;
            memcpy(pkt->data, data, copyLen);
            pkt->len = copyLen;
            pkt->sendAtMs = GetTickCount() + (DWORD)(delayMs + extraDelayMs);
            pkt->used = true;
            s_delayQueueCount++;
            s_simDelayCount++;
            return len;
        }

        // Queue full — send immediately
        return SendTo(sock, data, len, to);
    };

    int result = enqueue(0);

    if (duplicate) {
        enqueue(SimRandRange(1, s_simConfig.jitterMs > 0 ? s_simConfig.jitterMs : 5));
    }

    return result;
}

} // namespace UdpSocket
