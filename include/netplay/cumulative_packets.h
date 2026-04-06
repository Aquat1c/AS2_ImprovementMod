/**
 * Cumulative Input Packets - Inspired by giuroll-hagb
 * 
 * Key concept: Every packet contains ALL unconfirmed inputs since the last
 * frame the opponent confirmed receiving. This provides automatic packet
 * loss recovery without needing ACKs or retransmission logic.
 * 
 * If a packet is lost, the next packet contains all the missing inputs,
 * so no explicit retransmission is needed.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>

// ============================================================================
// UNIFIED PACKET FORMAT
// ============================================================================

// Single magic header for all mod packets: "AS2U" (Alice Senki 2 Unified)
#define AS2_UNIFIED_MAGIC 0x55325341  // "AS2U" in little endian

// Packet phases (replaces 3 separate magic numbers)
enum AS2PacketPhase : uint8_t {
    PHASE_HANDSHAKE = 1,    // Connection negotiation (version check, build sig)
    PHASE_READY_SYNC = 2,   // Match synchronization (both sides ready)
    PHASE_GAMEPLAY = 3,     // During match (cumulative inputs + rollback data)
};

// Handshake sub-types
enum HandshakeSubType : uint8_t {
    HS_HELLO = 1,           // Client -> Host: "I want to connect"
    HS_WELCOME = 2,         // Host -> Client: "You can join"
    HS_REJECT = 3,          // Host -> Client: "Rejected"
    HS_ACK = 4,             // Client -> Host: "Thanks, confirmed"
};

// ReadySync sub-types
enum ReadySyncSubType : uint8_t {
    RS_ANNOUNCE = 1,        // "I'm in match mode and ready"
    RS_CONFIRM = 2,         // "I got your ready, let's start"
};

// Rejection reasons
enum AS2RejectReason : uint8_t {
    REJECT_NONE = 0,
    REJECT_VERSION = 1,     // Mod version mismatch
    REJECT_BUILD = 2,       // Build signature mismatch
    REJECT_BUSY = 3,        // Already in a match
};

// ============================================================================
// UNIFIED PACKET HEADER (common to all phases)
// ============================================================================

#pragma pack(push, 1)

// Base header - present in ALL packets
struct AS2PacketHeader {
    uint32_t magic;         // AS2_UNIFIED_MAGIC
    uint8_t phase;          // AS2PacketPhase
    uint8_t subtype;        // Phase-specific subtype
    uint16_t session_id;    // Session nonce (changes each connection)
};
static_assert(sizeof(AS2PacketHeader) == 8, "Header must be 8 bytes");

// ============================================================================
// HANDSHAKE PACKET
// ============================================================================

struct AS2HandshakePacket {
    AS2PacketHeader hdr;    // phase=PHASE_HANDSHAKE (8 bytes)
    uint8_t version_major;  // Mod version (1 byte)
    uint8_t version_minor;  // (1 byte)
    uint8_t reject_reason;  // AS2RejectReason (for REJECT subtype) (1 byte)
    uint8_t reserved1;      // (1 byte)
    uint32_t build_sig;     // Build signature for strict binary match (4 bytes)
    char nickname[20];      // Player nickname (20 bytes)
    uint8_t padding[4];     // Pad to 40 bytes (4 bytes)
};
static_assert(sizeof(AS2HandshakePacket) == 40, "Handshake must be 40 bytes for game compat");

// ============================================================================
// READY SYNC PACKET
// ============================================================================

struct AS2ReadySyncPacket {
    AS2PacketHeader hdr;    // phase=PHASE_READY_SYNC
    uint32_t rng_seed;      // Shared RNG seed for synchronization
    uint8_t reserved[28];   // Pad to 40 bytes
};
static_assert(sizeof(AS2ReadySyncPacket) == 40, "ReadySync must be 40 bytes for game compat");

// ============================================================================
// GAMEPLAY PACKET (Cumulative Inputs)
// ============================================================================

// Fixed header for gameplay packets (before variable-length inputs)
struct AS2GameplayHeader {
    AS2PacketHeader hdr;        // phase=PHASE_GAMEPLAY
    uint32_t frame_id;          // Current frame number (end of input range)
    uint32_t last_confirmed;    // Last frame we confirmed receiving from opponent
    uint8_t input_count;        // Number of inputs in this packet
    uint8_t delay;              // Input delay
    uint8_t max_rollback;       // Max rollback frames
    uint8_t desync_check;       // For desync detection (weather byte, etc.)
    int32_t timing_sync;        // Timing synchronization data (microseconds)
    // Followed by: uint16_t inputs[input_count] - variable length
};
static_assert(sizeof(AS2GameplayHeader) == 24, "Gameplay header size check");

// Maximum inputs per packet (prevents oversized packets)
#define MAX_CUMULATIVE_INPUTS 60  // ~1 second of inputs at 60fps

// Maximum packet size (stay under MTU to avoid fragmentation)
#define MAX_AS2_PACKET_SIZE 200  // 24 + 60*2 = 144 max, plus margin

// ============================================================================
// INPUT HISTORY BUFFER
// ============================================================================

// Circular buffer to store recent local inputs for cumulative packets
#define INPUT_HISTORY_SIZE 128  // Must be power of 2

struct InputHistoryBuffer {
    uint16_t inputs[INPUT_HISTORY_SIZE];
    uint32_t frame_ids[INPUT_HISTORY_SIZE];  // Frame number for each input
    uint32_t write_idx;                       // Next write position
    uint32_t oldest_frame;                    // Oldest frame in buffer
    uint32_t newest_frame;                    // Newest frame in buffer
};

// ============================================================================
// FRAME CONFIRMATION STATE
// ============================================================================

struct FrameConfirmState {
    uint32_t last_local_input_frame;      // Last frame we generated input for
    uint32_t last_sent_frame;             // Last frame we sent to opponent
    uint32_t last_opponent_confirm;       // Last frame opponent confirmed receiving
    uint32_t last_received_frame;         // Last frame we received from opponent
    uint32_t last_we_confirmed;           // Last frame we confirmed to opponent
};

#pragma pack(pop)

// ============================================================================
// API FUNCTIONS
// ============================================================================

#ifdef __cplusplus
extern "C" {
#endif

// Initialize cumulative packet system
void CumulativePackets_Init(void);

// Reset state (call when starting new match)
void CumulativePackets_Reset(void);

// Record a local input for the given frame
void CumulativePackets_RecordInput(uint32_t frame, uint16_t input);

// Build a cumulative gameplay packet
// Returns packet size, or 0 on error
// Packet is written to outBuf (must be at least MAX_AS2_PACKET_SIZE bytes)
int CumulativePackets_BuildGameplayPacket(
    uint8_t* outBuf,
    int bufSize,
    uint32_t currentFrame,
    uint16_t currentInput,
    uint8_t delay,
    uint8_t maxRollback,
    uint8_t desyncCheck,
    int32_t timingSync
);

// Process a received gameplay packet
// Extracts inputs and updates confirmation state
// Returns true if packet was valid, false if corrupted/invalid
bool CumulativePackets_ProcessGameplayPacket(
    const uint8_t* data,
    int dataLen,
    uint16_t* outInputs,      // Buffer to receive extracted inputs
    int* outInputCount,       // Number of inputs extracted
    uint32_t* outFrameStart,  // Starting frame of inputs
    uint32_t* outLastConfirm  // Opponent's confirmation frame
);

// Get current confirmation state
const FrameConfirmState* CumulativePackets_GetState(void);

// Check if a frame's input has been confirmed by opponent
bool CumulativePackets_IsFrameConfirmed(uint32_t frame);

#ifdef __cplusplus
}
#endif
