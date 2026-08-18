/**
 * Alice Senki 2 - Inbound link emulator (test instrumentation)
 *
 * Applies a configurable one-way latency, jitter and loss to EVERY inbound
 * transport event, inside the transport2 worker, before the game thread ever
 * sees it. Sitting at the transport boundary is what makes it honest:
 *
 *   - Every packet class goes through it — session handshake, pregame,
 *     charsel/stage/winscreen frontend lockstep, gameplay InputStream,
 *     SyncHash, TimeProbe. A shim that only delayed gameplay input would
 *     "prove" the frontend works at 150 ms while never actually testing it.
 *   - TimeProbe (77/78) rides the same path, and TimeProbe is what
 *     delay_policy measures RTT from. So the emulated latency shows up in the
 *     same `RTT=` telemetry a real WAN produces, and the delay policy reacts
 *     to it exactly as it would to a real link. The number cannot be faked
 *     without also faking the thing under test.
 *
 * Both peers emulate their own INBOUND direction, so configuring N ms on each
 * side yields a round trip of ~2N.
 *
 * ORDERING. Release times are monotonic per queue: jitter only ever adds
 * delay, it never reorders. We sit ABOVE ENet's reliability layer, where
 * reordering a reliable channel would hand the session layer a sequence it is
 * entitled to assume cannot happen.
 *
 * LOSS. Only unreliable packets may be dropped, for the same reason: ENet has
 * already acknowledged a reliable packet to the sender by the time we see it,
 * so dropping it here is permanent — no retransmit will ever come. Gameplay
 * input is unreliable and carries a redundant window, which is exactly what
 * loss testing should exercise.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

namespace Net {

struct LinkEmulatorConfig {
    // Not a link property. Starting value forced into the game's image-handle
    // serial (dword_91EA74) so a same-machine pair can reproduce a divergent
    // ALLOCATION HISTORY -- the one cross-machine difference this harness
    // cannot otherwise produce. 0 = off.
    uint32_t handle_serial_skew;

    uint32_t one_way_latency_ms = 0;  // 0 disables the emulator entirely
    uint32_t jitter_ms = 0;           // uniform [0, jitter_ms], added only
    uint32_t loss_percent = 0;        // unreliable packets only
    uint32_t seed = 0x9E3779B9u;
};

/// Read `as2_stress.cfg` (same resolution as StressHooks: exe dir, then this
/// DLL's dir, then CWD) and apply link_latency_ms / link_jitter_ms /
/// link_loss_pct. Safe to call more than once; logs what it armed, and what
/// it searched when it finds nothing.
void LinkEmulator_LoadConfig();

/// Apply handle_serial_skew (no-op when 0). Call once, before a match loads.
void LinkEmulator_ApplyHandleSerialSkew();

void LinkEmulator_Configure(const LinkEmulatorConfig& cfg);
void LinkEmulator_GetConfig(LinkEmulatorConfig* out);

/// True when a nonzero one-way latency (or loss) is armed. Callers use this to
/// skip the queue entirely in production.
bool LinkEmulator_IsActive();

/// Hand an inbound event to the emulator. Returns false if the packet was
/// dropped (loss); otherwise it is queued for release at `now + latency`.
/// `reliable` suppresses loss. `now_ms` is the caller's clock (GetTickCount).
bool LinkEmulator_Submit(const void* event, size_t event_size,
                         bool reliable, uint32_t now_ms);

/// Pop the next event whose release time has arrived. Returns false when
/// nothing is due. Call in a loop each worker iteration.
bool LinkEmulator_PopDue(void* out_event, size_t event_size, uint32_t now_ms);

/// Drop everything queued (session teardown / reset).
void LinkEmulator_Reset();

struct LinkEmulatorStats {
    uint32_t queued;          // currently held
    uint32_t total_delayed;
    uint32_t total_dropped;
    uint32_t total_released;
    uint32_t overflow_forced; // released early because the queue was full
    uint32_t peak_queued;
};
void LinkEmulator_GetStats(LinkEmulatorStats* out);

} // namespace Net
