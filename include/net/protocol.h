/**
 * Alice Senki 2 - Network Protocol Definitions
 *
 * Packet types, wire header, version constants, and channel assignments
 * for the mod-owned ENet transport layer.
 *
 * All multi-byte fields are little-endian (x86 native).
 * ENet packets carry: [PacketType (2 bytes) | payload]
 */

#pragma once

#include <stdint.h>

namespace Net {

// ============================================================================
// Protocol Constants
// ============================================================================

constexpr uint16_t PROTOCOL_VERSION = 18;
constexpr int      MAX_PACKET_SIZE  = 1200;     // Stay under typical MTU
constexpr int      MAX_PAYLOAD_SIZE = MAX_PACKET_SIZE - 2;  // minus PacketType
constexpr int      NETPLAY_PALETTE_BANK_COUNT = 12;
constexpr int      NETPLAY_PALETTE_BANK_SIZE = 1024;
constexpr int      NETPLAY_PALETTE_MAX_PAYLOAD = NETPLAY_PALETTE_BANK_SIZE;

// ============================================================================
// ENet Channel Assignments
// ============================================================================

constexpr uint8_t CHANNEL_CONTROL   = 0;  // Reliable ordered: handshake, session control
constexpr uint8_t CHANNEL_GAMEPLAY  = 1;  // Unreliable sequenced: gameplay input (future)
constexpr uint8_t CHANNEL_DEBUG     = 2;  // Unreliable: diagnostics, state digests
constexpr uint8_t NUM_CHANNELS      = 3;

// ============================================================================
// Packet Types
// ============================================================================

enum class PacketType : uint16_t {
    // Session control (reliable, channel 0)
    Hello           = 1,    // Initiator sends after ENet connect
    HelloAck        = 2,    // Responder replies with own info
    Ready           = 3,    // Peer is ready for next phase
    Disconnect      = 4,    // Graceful disconnect with reason
    SessionMeta     = 5,    // Arbitrary session metadata exchange

    // Initial session sync (reliable, channel 0)
    SyncAnnounce    = 6,    // Session metadata announce for pre-game agreement
    SyncConfirm     = 7,    // Session sync confirmation / side assignment
    FrontendPhaseBarrier = 8,  // Frontend phase barrier / transition ack
    FrontendBoundaryDigest = 9, // Frontend boundary digest / watchdog state

    // NAT traversal coordination (reliable, channel 0)
    NatInfo         = 10,   // Exchange NAT/UPnP status
    NatTraversalSignal = 34, // Exchange ICE description/candidates over session control

    // Pre-game sync (reliable, channel 0)
    CharSelInput    = 11,   // CharSel custom-palette availability catalog exchange
    CharSelLock     = 12,   // Scoped character-selection lock notification
    StageSync       = 13,   // Stage selection exchange/lock
    ConfigExchange  = 14,   // LockedMatchConfig proposed by host
    ConfigAck       = 15,   // Join acknowledges config (includes config hash)
    LoadBarrier     = 16,   // Peer finished loading assets
    BaselineReady   = 17,   // Peer captured baseline savestate
    BaselineDigest  = 18,   // Normalized bootstrap agreement digest
    GameplayStart   = 19,   // Both peers ready — begin gameplay
    BaselineBreakdown = 25, // Detailed per-region baseline CRCs for mismatch diagnosis

    // CharSel lockstep (unreliable, channel 1)
    CharSelFrameInput = 40, // Per-frame charsel input with redundancy

    // Win screen / pause
    WinScreenConfirm    = 41,  // Legacy win screen one-shot confirm signal
    PauseQuit           = 42,  // Pause menu quit signal
    WinScreenFrameInput = 43,  // Mode 9 lockstep frame input with redundancy

    // Palette metadata control-plane (reliable, channel 0)
    PaletteConfig      = 50,
    PaletteData        = 51,
    PaletteAck         = 52,

    // Gameplay (unreliable, channel 1)
    GameplayInput   = 20,   // Legacy mod-owned rollback input sync (DEAD — no send site)
    GekkoData       = 23,   // Raw GekkoNet internal protocol data

    // Startup gameplay-entry barrier (reliable, channel 0)
    // Sent when first post-intro interactive boundary is reached while held;
    // release requires mutual ready+ack.
    GekkoReady      = 24,   // Startup barrier control (ready/ack)

    // Frontend shared-delay coordination
    DelayChangeReq  = 21,
    DelayChangeAck  = 22,

    // Debug / diagnostics (unreliable, channel 2)
    Ping            = 30,   // Application-level ping (supplements ENet RTT)
    Pong            = 31,   // Application-level pong
    StateDigest     = 32,   // CRC32 state digest for desync detection
    FrameSyncStatus = 33,   // Lightweight frame-progress telemetry
    SyncTrace       = 35,   // Synchronized diagnostics trace, debug channel
};

enum class FrameTimingMode : uint8_t {
    Vanilla58_8 = 0,  // Native 17ms limiter, about 58.8235 fps
    Proper60    = 1,  // 17ms limiter corrected to 1000/60 cadence
};

inline bool FrameTimingMode_IsValid(uint8_t value) {
    return value == (uint8_t)FrameTimingMode::Vanilla58_8 ||
           value == (uint8_t)FrameTimingMode::Proper60;
}

inline const char* FrameTimingModeName(FrameTimingMode mode) {
    switch (mode) {
        case FrameTimingMode::Vanilla58_8: return "vanilla_58_8";
        case FrameTimingMode::Proper60:    return "proper_60";
        default:                           return "unknown";
    }
}

inline const char* FrameTimingModeDisplayName(FrameTimingMode mode) {
    switch (mode) {
        case FrameTimingMode::Vanilla58_8: return "58.8 FPS";
        case FrameTimingMode::Proper60:    return "60.0 FPS";
        default:                           return "Unknown";
    }
}

// ============================================================================
// Wire Payloads
// ============================================================================

#pragma pack(push, 1)

struct HelloPayload {
    uint16_t protocol_version;   // Must match PROTOCOL_VERSION
    uint32_t build_hash;         // Exact local mod build fingerprint
    char     nickname[64];       // Null-terminated UTF-8 nickname
    uint16_t listen_port;        // Port this peer is listening on
    uint8_t  round_count;        // Sender's current vanilla round option 0..2
    uint8_t  frame_timing_mode;  // Net::FrameTimingMode
    uint8_t  hud_trail_r;
    uint8_t  hud_trail_g;
    uint8_t  hud_trail_b;
    uint8_t  hud_text_r;
    uint8_t  hud_text_g;
    uint8_t  hud_text_b;
    uint8_t  hud_trail_length;   // Encoded bar extend (64-320 px)
    uint8_t  hud_score_r;
    uint8_t  hud_score_g;
    uint8_t  hud_score_b;
    uint8_t  hud_font_size;      // NetplayHudStyle::HudFontSize
    uint8_t  hud_vertical_position; // NetplayHudStyle::HudVerticalPosition
};

struct HelloAckPayload {
    uint16_t protocol_version;
    uint32_t build_hash;         // Exact local mod build fingerprint
    char     nickname[64];
    uint16_t listen_port;
    uint8_t  round_count;        // Sender's current vanilla round option 0..2
    uint8_t  frame_timing_mode;  // Host-authoritative Net::FrameTimingMode
    uint8_t  hud_trail_r;
    uint8_t  hud_trail_g;
    uint8_t  hud_trail_b;
    uint8_t  hud_text_r;
    uint8_t  hud_text_g;
    uint8_t  hud_text_b;
    uint8_t  hud_trail_length;
    uint8_t  hud_score_r;
    uint8_t  hud_score_g;
    uint8_t  hud_score_b;
    uint8_t  hud_font_size;
    uint8_t  hud_vertical_position;
};

struct DisconnectPayload {
    uint16_t reason_code;        // 0 = normal, 1 = timeout, 2 = version mismatch, 3 = error
    char     message[64];        // Human-readable reason
};

struct PingPayload {
    uint32_t ping_id;            // Echoed back in Pong
    uint32_t send_time_ms;       // Local timestamp (GetTickCount)
};

struct PongPayload {
    uint32_t ping_id;
    uint32_t original_send_time_ms;
    uint32_t responder_time_ms;
};

struct StateDigestPayload {
    uint32_t frame_number;
    uint32_t crc32;
};

struct FrameSyncStatusPayload {
    int32_t  rb_frame_current;              // Sender's current rollback-session-relative frame
    int32_t  game_abs_frame_current;        // Sender's current absolute engine frame
    int32_t  frame_origin_abs;              // Sender's absolute rollback origin
    int32_t  rb_frame_last_remote_received; // Sender's latest remote progress in rb_frame domain
    int32_t  rb_frame_confirmed;            // Sender's fully confirmed rollback frame
    int32_t  predicted_frames;              // Sender's outstanding predicted frames
    uint32_t checksum;                      // Sender's current state checksum
};

enum class SyncTraceDomain : uint8_t {
    None             = 0,
    FrontendLockstep = 1,
    GameplayRollback = 2,
    LifecycleEvent   = 3,
};

constexpr uint16_t SYNC_TRACE_FLAG_VALID_STATE_CRC  = 1 << 0;
constexpr uint16_t SYNC_TRACE_FLAG_VALID_PLAYER_CRC = 1 << 1;
constexpr uint16_t SYNC_TRACE_FLAG_ROLLBACK_SETTLED = 1 << 2;
constexpr uint16_t SYNC_TRACE_FLAG_ROLLING_BACK     = 1 << 3;
constexpr uint16_t SYNC_TRACE_FLAG_PASSIVE_PHASE    = 1 << 4;

struct SyncTracePayload {
    uint16_t schema;             // start at 1
    uint16_t flags;              // SYNC_TRACE_FLAG_*

    uint32_t epoch_id;           // frontend epoch, match epoch, or session token
    uint32_t seq;                // local monotonic trace sequence

    uint8_t  domain;             // SyncTraceDomain
    uint8_t  sync_mode;          // Net::SyncMode
    uint8_t  lockstep_context;   // Net::LockstepContext
    uint8_t  frontend_phase;     // Net::FrontendSyncPhase

    uint8_t  match_lifecycle_phase; // Net::MatchLifecyclePhase
    uint8_t  rollback_phase;         // Net::MatchRollbackPhase
    uint8_t  native_mode;            // MODE_*
    uint8_t  native_substate;        // *_SUB_*

    uint8_t  game_type;
    uint8_t  local_player;       // 0=P1, 1=P2, 0xFF unknown
    uint8_t  remote_player;      // 0=P1, 1=P2, 0xFF unknown
    uint8_t  _pad0;

    int32_t  game_abs_frame;     // absolute engine/sim frame if known, else -1
    int32_t  rb_frame;           // rollback frame for gameplay, else -1
    uint32_t frontend_frame;     // lockstep consume frame, else 0

    uint16_t local_input;        // normalized local input for compared frame
    uint16_t remote_input;       // normalized remote input for compared frame

    uint32_t local_input_frame;  // frontend local head or rb input head
    uint32_t remote_latest_frame;// frontend remote latest or rb remote received
    uint32_t consume_frame;      // frontend consume frame or rb current frame
    uint32_t remote_ack_frame;   // frontend ack or rb confirmed frame

    uint8_t  local_visible_delay;
    uint8_t  remote_visible_delay;
    uint8_t  local_effective_delay;
    uint8_t  remote_effective_delay;

    uint8_t  rollback_budget;
    uint8_t  predicted_frames;
    uint8_t  last_rollback_depth;
    uint8_t  max_rollback_depth;

    uint32_t state_crc;          // normalized phase-specific CRC
    uint32_t p1_crc;             // optional; 0 if not captured
    uint32_t p2_crc;             // optional; 0 if not captured
    uint32_t rng_seed;           // 0 if unavailable
};

// Initial session sync payloads

struct SyncAnnouncePayload {
    uint32_t session_id;         // Random session identifier for this match
    uint8_t  capability_flags;   // Bit 0: savestate baseline, Bit 1: desync diagnostics
    uint8_t  frontend_delay_proposal; // Local frontend delay recommendation for this epoch
    uint8_t  round_count;        // Host-owned vanilla option 0..2 when sent by host
    uint8_t  _pad;
};

struct SyncConfirmPayload {
    uint32_t session_id;         // Agreed session ID (host's ID is authoritative)
    uint8_t  confirmed;          // 1 = all checks passed
    uint8_t  assigned_side;      // Host decides: 0 = host is P1, 1 = host is P2
    uint8_t  frontend_delay_proposal; // Echo of sender's local recommendation
    uint8_t  frontend_shared_delay;   // Shared frontend delay agreed for this epoch
    uint8_t  round_count;        // Host-owned vanilla option 0..2 when sent by host
    uint8_t  _pad[3];
};

struct FrontendPhaseBarrierPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase (current phase)
    uint16_t next_phase;         // Net::FrontendSyncPhase (next phase)
    uint32_t last_completed_frame; // Sender's final frame index for the completed phase
    uint8_t  reason_code;        // Barrier reason / transition category
    uint8_t  _pad[3];
};

struct FrontendBoundaryDigestPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase
    uint8_t  digest_kind;        // Net::FrontendDigestKind
    uint8_t  substate;           // Native substate for diagnostics
    uint32_t frame;              // Local phase frame when digest was captured
    uint32_t digest;             // CRC32 over the phase boundary state
    uint8_t  p1_character;
    uint8_t  p1_palette;
    uint8_t  p2_character;
    uint8_t  p2_palette;
    uint8_t  p1_palette_custom;
    uint8_t  p2_palette_custom;
    uint8_t  _palette_pad[2];
    uint8_t  stage_cursor;
    uint8_t  stage_confirmed;
    uint8_t  stage_counter;
    uint8_t  stage_aux;
    uint8_t  stage_cancel;
    uint8_t  confirm_menu_cursor;
    uint8_t  confirm_menu_action;
    uint8_t  committed_stage_id;
};

struct NatInfoPayload {
    uint8_t  flags;              // NAT_INFO_FLAG_*
    uint8_t  upnp_status;        // Net::NatStatus
    uint8_t  stun_status;        // Net::StunStatus
    uint8_t  extra_flags;        // NAT_INFO_EX_FLAG_*
    uint16_t listen_port;        // Local listen port used by ENet host
    uint16_t external_port;      // External/stun-discovered UDP port
    char     external_ip[48];    // UPnP/STUN external IP string (v4/v6 text)
};

struct NatTraversalSignalPayload {
    uint8_t  signal_type;        // Net::NatSignalType
    uint8_t  _pad;
    uint16_t text_len;           // bytes used in text[]
    char     text[1024];         // ICE SDP/candidate payload (trickle)
};

// Pre-game sync payloads

struct CharSelInputPayload {
    uint8_t  game_slot;          // 0 = P1, 1 = P2
    uint8_t  _pad;
    uint16_t custom_masks[256];  // Bit N set => stored custom bank exists for base palette N
};

struct CharSelLockPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase (must be CharSel)
    uint8_t  character_id;       // Resolved character ID from grid table
    uint8_t  palette;            // Final palette
    uint8_t  flags;              // CHARSEL_LOCK_FLAG_*
    uint8_t  _pad[3];
};

struct StageSyncPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase (must be StageSel)
    uint16_t frame;              // Sender's current phase frame
    uint8_t  stage_id;           // Sender's currently resolved stage ID
    uint8_t  confirmed;          // 0 = browsing, 1 = locked
    uint8_t  stage_cursor;       // Shared cursor/grid index
    uint8_t  stage_counter;      // Stage confirm/roulette counter byte
    uint8_t  stage_aux;          // Adjacent stage state byte (watchdog only)
    uint8_t  stage_cancel;       // Cancel flag
    uint8_t  confirm_menu_cursor; // Confirm-menu cursor index
    uint8_t  confirm_menu_action; // Confirm-menu action/armed flag
    uint8_t  committed_stage_id; // Final committed stage ID if available
    uint8_t  substate;           // Native charsel substate for diagnostics
    uint8_t  round_count;        // Host-owned vanilla option 0..2 (wins required = value + 1)
    uint8_t  _pad;
};

struct ConfigExchangePayload {
    uint8_t  p1_character;
    uint8_t  p1_palette;
    uint8_t  p2_character;
    uint8_t  p2_palette;
    uint8_t  stage_id;
    uint8_t  host_side;          // 0 = host is P1, 1 = host is P2
    uint8_t  round_count;        // Vanilla option 0..2 (wins required = value + 1)
    uint8_t  time_limit;
    uint32_t rng_seed;
    uint32_t session_seed;
    // Local rollback configuration announcement
    uint8_t  my_input_delay;     // Sender's local gameplay input delay
    uint8_t  my_max_rollback;    // Sender's input prediction window / max rollback
    uint8_t  gameplay_delay_mode; // Net::GameplayDelayMode
    uint8_t  frame_timing_mode;  // Host-authoritative Net::FrameTimingMode
};

struct ConfigAckPayload {
    uint32_t config_hash;        // CRC32 of the LockedMatchConfig peer built
    uint8_t  accepted;           // 1 = matches, 0 = mismatch
    // Local rollback configuration announcement
    uint8_t  my_input_delay;     // Sender's local gameplay input delay
    uint8_t  my_max_rollback;    // Sender's input prediction window / max rollback
    uint8_t  gameplay_delay_mode; // Net::GameplayDelayMode
    uint8_t  frame_timing_mode;  // Sender's effective Net::FrameTimingMode
};

struct LoadBarrierPayload {
    uint8_t  loaded;             // 1 = assets loaded
    uint8_t  mode;               // Local game mode when barrier was sent
    uint8_t  substate;           // Local substate when barrier was sent
    uint8_t  _pad;
    uint32_t sim_frame;          // Native sim frame at load completion
};

struct BaselineReadyPayload {
    uint8_t  captured;           // 1 = baseline savestate captured
    uint8_t  mode;               // Local game mode when baseline was captured
    uint8_t  substate;           // Local substate when baseline was captured
    uint8_t  _pad;
    uint32_t sim_frame;          // Native sim frame when baseline was captured
};

struct BaselineDigestPayload {
    uint32_t crc32;              // Normalized bootstrap agreement digest
};

struct BaselineBreakdownPayload {
    // Baseline CRC components
    uint32_t main_crc;
    uint32_t header_crc;
    uint32_t context_crc;
    uint32_t effect_crc;
    uint32_t summon_crc;
    uint32_t p1_entity_crc;
    uint32_t p2_entity_crc;

    // Normalized bootstrap-agreement CRCs. These intentionally exclude
    // volatile loader/handle regions that are useful for diagnostics but
    // not authoritative for baseline agreement.
    uint32_t header_agreement_crc;
    uint32_t p1_agreement_crc;
    uint32_t p2_agreement_crc;

    // Adjacent/non-hash diagnostics
    uint32_t pre_match_gap_crc;
    uint32_t p1_input_crc;
    uint32_t p2_input_crc;
    uint32_t per_frame_temp_crc;

    // Contextual counters
    uint32_t rng_seed;
    uint32_t sim_frame;
    uint32_t display_frame;
    uint32_t game_mode;
    uint32_t substate;
    uint32_t game_type;
    uint32_t match_phase_timer;
    uint32_t frame_simulation;
    uint32_t frame_display;
    uint32_t frame_write_idx;
    uint32_t frame_net_idx;
    uint32_t remote_frame_idx;

    // Raw critical bytes
    uint8_t  match_header_bytes[16];
    uint8_t  pre_match_gap_bytes[12];
};

struct GameplayStartPayload {
    uint32_t bootstrap_frame_abs;   // Absolute engine frame of the restored bootstrap baseline
    uint32_t host_game_abs_frame;   // Host absolute engine frame when GameplayStart was sent
};

struct GekkoReadyPayload {
    uint8_t  flags;              // GEKKO_READY_FLAG_*
    uint8_t  phase;              // Sender MatchLifecyclePhase at send time
    uint16_t _pad;
    int32_t  game_abs_frame;     // Sender absolute engine frame at startup barrier send time
};

struct CharSelFrameInputPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase (CharSel or StageSel)
    uint16_t _phase_pad;
    uint32_t frame;              // Lockstep frame number
    uint32_t ack_frame;          // Sender's consumeFrame (frame they need from us)
    uint16_t inputs[16];         // Redundant history: [frame, frame-1, ..., frame-15]
    uint16_t input_count;        // Number of valid entries in inputs[] (1-16)
    uint16_t _pad;
};

struct WinScreenFrameInputPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase (WinScreen)
    uint16_t _phase_pad;
    uint32_t frame;              // Lockstep frame number
    uint32_t ack_frame;          // Sender's consumeFrame (frame they need from us)
    uint16_t inputs[16];         // Redundant history: [frame, frame-1, ..., frame-15]
    uint16_t input_count;        // Number of valid entries in inputs[] (1-16)
    uint16_t _pad;
};

struct PaletteConfigPayload {
    uint32_t epoch;
    uint32_t config_hash;
    uint8_t  game_slot;          // 0 = P1, 1 = P2
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  flags;
    uint16_t payload_size;
    uint16_t _pad;
    uint32_t payload_crc;
};

struct PaletteDataPayload {
    uint32_t epoch;
    uint32_t config_hash;
    uint8_t  game_slot;
    uint8_t  character_id;
    uint8_t  base_palette;
    uint8_t  _pad0;
    uint32_t payload_crc;
    uint16_t payload_size;
    uint16_t _pad1;
    uint8_t  payload[NETPLAY_PALETTE_BANK_SIZE];
};

struct PaletteAckPayload {
    uint32_t epoch;
    uint32_t config_hash;
    uint8_t  game_slot;
    uint8_t  accepted;
    uint8_t  received_data;
    uint8_t  _pad;
    uint32_t payload_crc;
};

struct DelayChangeReqPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase
    uint16_t new_delay;          // Requested shared frontend delay
    uint32_t apply_from_frame;   // Apply point inside the phase timeline
    uint8_t  reason_code;        // Net::FrontendDelayBumpReason
    uint8_t  _pad[3];
};

struct DelayChangeAckPayload {
    uint32_t epoch_id;           // Frontend epoch/session scope
    uint32_t phase_serial;       // Monotonic phase instance inside the epoch
    uint16_t phase;              // Net::FrontendSyncPhase
    uint16_t acked_delay;        // Accepted shared frontend delay
    uint32_t apply_from_frame;   // Apply point inside the phase timeline
    uint8_t  accepted;           // 1 = accepted, 0 = rejected
    uint8_t  reason_code;        // Net::FrontendDelayBumpReason
    uint8_t  _pad[2];
};

#pragma pack(pop)

// GekkoReadyPayload flags
constexpr uint8_t GEKKO_READY_FLAG_READY = 1 << 0;  // Local reached post-intro interactive boundary
constexpr uint8_t GEKKO_READY_FLAG_ACK   = 1 << 1;  // Local has observed peer READY

constexpr uint8_t CHARSEL_LOCK_FLAG_CUSTOM_PALETTE = 1 << 0;

constexpr uint8_t NETPLAY_PALETTE_FLAG_TRANSPORT_ENABLED = 1 << 0;
constexpr uint8_t NETPLAY_PALETTE_FLAG_REMOTE_PREVIEW_ENABLED = 1 << 1;
constexpr uint8_t NETPLAY_PALETTE_FLAG_HAS_CUSTOM_DATA = 1 << 2;
constexpr uint8_t NETPLAY_PALETTE_FLAG_SPECTATOR_PROPAGATE = 1 << 3;

static_assert(sizeof(PacketType) + sizeof(PaletteDataPayload) <= MAX_PACKET_SIZE,
    "PaletteDataPayload must fit inside one transport packet");
static_assert(sizeof(PacketType) + sizeof(CharSelInputPayload) <= MAX_PACKET_SIZE,
    "CharSelInputPayload must fit inside one transport packet");
static_assert(sizeof(HelloPayload) == 86,
    "HelloPayload wire size must remain stable");
static_assert(sizeof(HelloAckPayload) == 86,
    "HelloAckPayload wire size must remain stable");
static_assert(sizeof(SyncAnnouncePayload) == 8,
    "SyncAnnouncePayload wire size must remain stable");
static_assert(sizeof(SyncConfirmPayload) == 12,
    "SyncConfirmPayload wire size must remain stable");
static_assert(sizeof(StageSyncPayload) == 24,
    "StageSyncPayload wire size must remain stable");
static_assert(sizeof(PacketType) + sizeof(SyncTracePayload) <= MAX_PACKET_SIZE,
    "SyncTracePayload must fit inside one transport packet");

// NatInfoPayload flags
constexpr uint8_t NAT_INFO_FLAG_UPNP_ENABLED      = 1 << 0;
constexpr uint8_t NAT_INFO_FLAG_UPNP_MAPPED       = 1 << 1;
constexpr uint8_t NAT_INFO_FLAG_STUN_ENABLED      = 1 << 2;
constexpr uint8_t NAT_INFO_FLAG_STUN_OK           = 1 << 3;
constexpr uint8_t NAT_INFO_FLAG_HOLE_PUNCH        = 1 << 4;
constexpr uint8_t NAT_INFO_FLAG_RELAY_FALLBACK    = 1 << 5;
constexpr uint8_t NAT_INFO_FLAG_PREFER_DIRECT     = 1 << 6;
constexpr uint8_t NAT_INFO_FLAG_IPV6_ENDPOINTS    = 1 << 7;
constexpr uint8_t NAT_INFO_EX_FLAG_TURN_ENABLED   = 1 << 0;
constexpr uint8_t NAT_INFO_EX_FLAG_PCP_ENABLED    = 1 << 1;

// ============================================================================
// Helpers
// ============================================================================

inline const char* SyncTraceDomainName(SyncTraceDomain domain) {
    switch (domain) {
        case SyncTraceDomain::None:             return "None";
        case SyncTraceDomain::FrontendLockstep: return "FrontendLockstep";
        case SyncTraceDomain::GameplayRollback: return "GameplayRollback";
        case SyncTraceDomain::LifecycleEvent:   return "LifecycleEvent";
        default:                                return "Unknown";
    }
}

inline const char* PacketTypeName(PacketType type) {
    switch (type) {
        case PacketType::Hello:          return "Hello";
        case PacketType::HelloAck:       return "HelloAck";
        case PacketType::Ready:          return "Ready";
        case PacketType::Disconnect:     return "Disconnect";
        case PacketType::SessionMeta:    return "SessionMeta";
        case PacketType::SyncAnnounce:   return "SyncAnnounce";
        case PacketType::SyncConfirm:    return "SyncConfirm";
        case PacketType::FrontendPhaseBarrier: return "FrontendPhaseBarrier";
        case PacketType::FrontendBoundaryDigest: return "FrontendBoundaryDigest";
        case PacketType::NatInfo:        return "NatInfo";
        case PacketType::NatTraversalSignal: return "NatTraversalSignal";
        case PacketType::CharSelInput:   return "CharSelInput";
        case PacketType::CharSelLock:    return "CharSelLock";
        case PacketType::StageSync:      return "StageSync";
        case PacketType::ConfigExchange: return "ConfigExchange";
        case PacketType::ConfigAck:      return "ConfigAck";
        case PacketType::LoadBarrier:    return "LoadBarrier";
        case PacketType::BaselineReady:  return "BaselineReady";
        case PacketType::BaselineDigest: return "BaselineDigest";
        case PacketType::BaselineBreakdown: return "BaselineBreakdown";
        case PacketType::GameplayStart:  return "GameplayStart";
        case PacketType::GekkoReady:     return "GekkoReady";
        case PacketType::GameplayInput:   return "GameplayInput";
        case PacketType::DelayChangeReq:  return "DelayChangeReq";
        case PacketType::DelayChangeAck:  return "DelayChangeAck";
        case PacketType::GekkoData:       return "GekkoData";
        case PacketType::CharSelFrameInput:   return "CharSelFrameInput";
        case PacketType::WinScreenConfirm:    return "WinScreenConfirm";
        case PacketType::PauseQuit:           return "PauseQuit";
        case PacketType::WinScreenFrameInput: return "WinScreenFrameInput";
        case PacketType::PaletteConfig:       return "PaletteConfig";
        case PacketType::PaletteData:         return "PaletteData";
        case PacketType::PaletteAck:          return "PaletteAck";
        case PacketType::Ping:           return "Ping";
        case PacketType::Pong:           return "Pong";
        case PacketType::StateDigest:    return "StateDigest";
        case PacketType::FrameSyncStatus:return "FrameSyncStatus";
        case PacketType::SyncTrace:      return "SyncTrace";
        default:                         return "Unknown";
    }
}

/// Validate that a received buffer is large enough for [PacketType + payload].
/// Returns false if the buffer is too small for the packet type header.
inline bool ValidatePacketSize(const void* data, size_t length) {
    return data != nullptr && length >= sizeof(PacketType);
}

/// Extract PacketType from the front of a raw buffer.
inline PacketType ReadPacketType(const void* data) {
    return *static_cast<const PacketType*>(data);
}

/// Get a pointer to the payload after the PacketType header.
inline const void* GetPayloadPtr(const void* data) {
    return static_cast<const uint8_t*>(data) + sizeof(PacketType);
}

/// Get payload size from total packet length.
inline size_t GetPayloadSize(size_t totalLength) {
    return totalLength > sizeof(PacketType) ? totalLength - sizeof(PacketType) : 0;
}

} // namespace Net
