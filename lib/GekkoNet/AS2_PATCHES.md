# AS2 local patches to vendored GekkoNet

> **RETIRED (2026-08-17, re0.7 post-M8):** GekkoNet is no longer part of the
> build. The `AS2_WITH_GEKKO` CMake option, the Gekko adapter
> (`src/rollback/rollback_session.cpp`), and the `gekko_input_tests` target
> were all deleted in the one-commit removal recorded in
> `docs/re0.7/M8_ACCEPTANCE_RUNBOOK.md` §6; the engine2 backend is the sole
> rollback implementation. This `lib/GekkoNet` directory is retained on disk
> for history only and is not referenced by CMake.

Local modifications to the vendored GekkoNet source (`GekkoLib/`) made for the
Alice Senki 2 netplay mod. Review this file when syncing with upstream — every
change below must be re-applied (or upstreamed) after a vendor drop. All hunks
are marked in-source with `AS2 patch` comments.

## M3: Interrupted peer state (2026-08-17)

Goal: GekkoNet must not unilaterally kill a live session after 5 s of silence.
New model: Connected → **Interrupted** (freeze, keep resending, emit event) →
resume on first packet; only a much later configurable timeout produces
Disconnected. Session-death policy belongs to the mod's `Net::ConnectionSupervisor`.

### `GekkoLib/include/gekkonet.h`
- `GekkoConfig`: two fields APPENDED (struct is memcpy'd whole in
  `GameSession::Init`; callers must zero-init the struct):
  - `unsigned long long disconnect_timeout_ms;` — 0 = default 5000 ms
  - `unsigned long long interrupt_timeout_ms;` — 0 = default 3000 ms
- `GekkoSessionEventType`: two values APPENDED (existing numbering unchanged):
  - `GekkoPlayerInterrupted = 7`
  - `GekkoPlayerResumed = 8`
- `GekkoSessionEvent` union: added `interrupted { int handle; }` and
  `resumed { int handle; }` members (mirrors `disconnected`).

### `GekkoLib/include/net.h`
- `NetStats`: `DISCONNECT_TIMEOUT = 5000` is now only the DEFAULT; added
  `INTERRUPT_TIMEOUT = 3000` default and instance fields
  `u64 disconnect_timeout` / `u64 interrupt_timeout` (initialized to the
  defaults). Effective values live on `MessageSystem` (see backend.h).

### `GekkoLib/include/backend.h`
- `Player`: added public `bool interrupted = false;` — parallel liveness flag;
  status stays `Connected` while interrupted so address/magic/sync state and
  the send path survive.
- `MessageSystem::Init` signature extended:
  `Init(u8 num_players, u32 input_size, u64 disconnect_timeout_ms = 0, u64 interrupt_timeout_ms = 0)`
  (0 = library defaults).
- Added private members `_disconnect_timeout` / `_interrupt_timeout`.
- `MAX_INPUT_QUEUE_SIZE`: **128 → 1800** (~30 s at 60 fps). This caps a
  `std::deque<std::unique_ptr<u8[]>>` (per-frame heap blobs of `input_size`
  bytes), not a fixed array — worst case ≈ 1800 × (pointer + tiny alloc)
  ≈ tens of KB per queue. The old 128-frame cap force-dropped unacked inputs
  after ~2.1 s of silence, permanently corrupting the input stream on resume.

### `GekkoLib/src/backend.cpp`
- `MessageSystem::Init`: stores configured timeouts (0 → defaults), logs them.
- `HandleTooFarBehindActors`: three-state liveness.
  - silence ≥ `_interrupt_timeout` and peer `Connected` and not yet flagged →
    set `actor->interrupted`, emit `PlayerInterrupted` once. Status untouched.
  - silence ≥ `_disconnect_timeout` → existing path (event + `Disconnected` +
    `sync_num = 0`), also clears `interrupted`. Comparison changed `>` → `>=`.
- `ParsePacket` (receive-timer refresh site): any packet from a flagged peer
  clears `interrupted` and emits `PlayerResumed` once, before refreshing
  `last_received_message`.
- Send gate note: `SendInputsToPeer` early-returns only for
  `Disconnected`/no-address peers; interrupted peers remain `Connected`, so
  input resends (50 ms retry cadence) and health packets keep flowing with no
  code change needed there.

### `GekkoLib/include/event.h`, `GekkoLib/src/event.cpp`
- `SessionEventSystem::AddPlayerInterruptedEvent(Handle)` and
  `AddPlayerResumedEvent(Handle)` — exact mirrors of
  `AddPlayerDisconnectedEvent`.

### `GekkoLib/src/game_session.cpp`
- `GameSession::Init`: passes `_config.disconnect_timeout_ms` /
  `_config.interrupt_timeout_ms` into `_msg.Init`.
- `HandleReceivedInputs`: debug assert bound raised 128 → 1800 to match
  `MAX_INPUT_QUEUE_SIZE` (a resume burst after a long interruption can
  legitimately deliver more than 128 frames in one poll).

### `GekkoLib/src/spectator_session.cpp`
- `SpectatorSession::Init`: same timeout plumbing into `_msg.Init`.

### Build
- No files added/removed; `GekkoLib/CMakeLists.txt` globs `src/*.cpp` and is
  unchanged.

### Mod-side contract (for context, not part of the vendor tree)
- `mod/src/rollback/rollback_session.cpp` passes
  `disconnect_timeout_ms = 20000` (matches `ConnectionSupervisor` Dead
  default) and `interrupt_timeout_ms = 3000` (matches supervisor Interrupted
  threshold), and consumes the two new events without breaking the session.
