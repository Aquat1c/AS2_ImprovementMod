/**
 * Alice Senki 2 - Non-blocking UDP Socket
 *
 * Thin wrapper over ws2_32. The mod owns its own socket directly —
 * no winsock proxy DLL needed.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdint.h>

#pragma comment(lib, "ws2_32.lib")

namespace UdpSocket {

// Initialize winsock (safe to call multiple times)
bool Init();

// Create and bind a non-blocking UDP socket on the given port.
// Pass 0 to let the OS pick a port.
// Returns INVALID_SOCKET on failure.
SOCKET Create(uint16_t bindPort);

// Send data to a specific address. Non-blocking.
// Returns bytes sent, or -1 on error.
int SendTo(SOCKET sock, const void* data, int len, const sockaddr_in* to);

// Receive data from any address. Non-blocking.
// Returns bytes received, 0 if nothing available, or -1 on error.
// On success, fills 'from' with the sender's address.
int RecvFrom(SOCKET sock, void* buf, int bufLen, sockaddr_in* from);

// Close a socket.
void Close(SOCKET sock);

// Build a sockaddr_in from IP octets and port.
sockaddr_in MakeAddr(uint8_t a, uint8_t b, uint8_t c, uint8_t d, uint16_t port);
sockaddr_in MakeAddr(uint32_t ip[4], uint32_t port);

// Compare two addresses (IP + port).
bool AddrEqual(const sockaddr_in* a, const sockaddr_in* b);

// Format address to string (e.g. "192.168.1.5:7500").
void FormatAddr(const sockaddr_in* addr, char* out, int outLen);

// Get the local port a socket is bound to.
uint16_t GetBoundPort(SOCKET sock);

// ============================================================================
// Network Condition Simulation
// ============================================================================

// Set simulation parameters. Pass 0 to disable.
void SetSimulation(int latencyMs, int jitterMs, float lossPercent, float dupPercent);

// Must be called once per frame to flush delayed packets.
void TickSimulation();

// Simulated send — queues or drops based on simulation config.
// Delegates to real SendTo after delay.
int SimSendTo(SOCKET sock, const void* data, int len, const sockaddr_in* to);

} // namespace UdpSocket
