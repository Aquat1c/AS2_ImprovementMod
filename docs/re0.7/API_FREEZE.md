# re0.7 API Freeze Checklist (M1)

Date: 2026-08-17. Branch: `re0.7`.

This is the M1 "facade freeze" commitment from `RE07_MASTER_REBUILD_PLAN.md` §6:
the contracts below are **preserved verbatim** through the whole rebuild. Any
change to a listed signature, struct shape, or enum value is a rejected review
until this document is amended first. Source of truth for the lists:
`FRONTEND_BACKEND_INVENTORY.md` §2.1–2.5.

Compile-time pins added at M1:

| Pin | Location |
|---|---|
| `sizeof(LockedMatchConfig) == 20` | `include/net/locked_match_config.h` (pre-existing) |
| `sizeof(SessionSnapshot) == 472`, `sizeof(PeerInfo) == 96`, `sizeof(ConnectionStats) == 40` | `include/net/session_manager.h` |
| `sizeof(PaletteConfigPayload) == 20`, `sizeof(PaletteDataPayload) == 1044`, `sizeof(PaletteAckPayload) == 16` | `include/net/protocol.h` |
| Frontend frame/barrier/digest payload sizes (56/56/20/36) | `include/net/protocol.h` |
| All new v2 payload sizes (§3.2) | `include/net/protocol.h` |

## 1. `Net::Session_*` (include/net/session_manager.h) — preserved verbatim

All 17 functions, byte/shape-stable types (`SessionState`, `SessionRole`,
`SessionConfig`, `PeerInfo`, `ConnectionStats`, `NatTraversalConfig`,
`ConnectPreference`, `SessionSnapshot`):

- [ ] `Session_Init` / `Session_Shutdown`
- [ ] `Session_StartHost(const SessionConfig*)` / `Session_StartJoin(const SessionConfig*)`
- [ ] `Session_Cancel` / `Session_NotifyGameExit`
- [ ] `Session_GetMsSinceLastInbound` (re-implemented over `protocol_silence_ms` at M3; header contract unchanged)
- [ ] `Session_SignalReady` / `Session_Update`
- [ ] `Session_SendPacket(channel, type, payload, len, reliable)`
- [ ] `Session_SetPacketCallback(PacketCallback)` incl. deferred-flush semantics
- [ ] `Session_GetSnapshot` / `Session_GetState` / `Session_GetRole`
- [ ] `Session_IsConnected` / `Session_GetRemotePeer` / `Session_GetStats`

## 2. `Net::PregameSync_*` (include/net/pregame_sync.h) — preserved

12 functions; `PregamePhase` enum values kept as the *reporting vocabulary*
(internal states of match_setup may differ); `PregameSnapshot` shape kept:

- [ ] `PregameSync_Init` / `Shutdown` / `FrameUpdate`
- [ ] `PregameSync_Begin` / `PregameSync_BeginRematch(const LockedMatchConfig*)`
- [ ] `PregameSync_Abort(const char*)`
- [ ] `PregameSync_GetPhase` / `IsActive` / `GetSnapshot` / `IsComplete`
- [ ] `PregameSync_GetLockedConfig`
- [ ] `PregameSync_HandleCrossPhaseSessionPacket`

`MatchBootstrap_*` is NOT frozen — internal to the doomed set, retired at M5.

## 3. `Rollback::OnlineWiring_*` (include/rollback/online_wiring.h) — preserved

All 13 functions + `OnlineWiringSnapshot`:

- [ ] `OnlineWiring_Init` / `Shutdown` / `FrameUpdate`
- [ ] `OnlineWiring_IsGameplayActive` / `IsStartupReleased` / `IsGameplayEntryAdvanceBlocked`
- [ ] `OnlineWiring_OnGameplayStart` / `OnGameplayPause` / `OnMatchEnd`
- [ ] `OnlineWiring_OnDisconnect` / `OnRematch` / `OnReturnToSession`
- [ ] `OnlineWiring_GetSnapshot`

M0 additions (packet sinks used by `gameplay_packet_router`, replaced at M5 by
packet_router→engine ingest — additions are allowed, removals are not):
`OnlineWiring_HandleEngineDataPacket`, `OnlineWiring_HandleStartupBarrierPacket`.

## 4. `Rollback::RollbackSession_*` (include/rollback/rollback_session.h) — preserved facade

- [ ] `RollbackSession_IsActive` / `IsSessionRunning` / `IsPeerInterrupted`
- [ ] `RollbackSession_GetErrorReason` / `TakeErrorReason` / `GetSnapshot`
- [ ] `RollbackSession_IsRollingBack` / `ShouldSuppressSideEffects`
- [ ] Two-phase dispatcher contract: `BeginFrame(uint16_t)` → loop
      `ProcessNextEvent()` (`Advance | Done | Error`) → `GetAdvanceInputs(p1,p2)`;
      `HasPendingFrame` / `PollSession` / `DrainPendingNonAdvanceEvents` / `End`
- [ ] `RollbackSession_FramesAhead` (telemetry only in engine2 — no decision consumes it)
- [ ] `RollbackSession_GetCurrentFrame` / `GetFrameOriginAbs` / `GetCurrentGameAbsFrame` / `RbFrameToGameAbs`
- [ ] `RollbackSession_GetActiveDelay` / `SetLocalDelay` / `GetRollbackBudget`
- [ ] `RollbackSession_GetTimesyncTelemetry(RollbackTimesyncTelemetry*)`
      (struct extracted to `rollback/rollback_telemetry.h` at M0;
      `gekko_avg_ping`/`gekko_jitter` → `link_avg_ping`/`link_jitter`)
- [ ] `RollbackSession_ComputeLiveStateChecksum` / `InjectLocalInput`

Deleted at the M5 cutover (already planned, not part of the freeze):
`RollbackSession_BufferGekkoPacket`. `RollbackSessionSnapshot.gekko_*` field
names are renamed `link_*` at M5 together with the engine cutover.

## 5. Policy/lifecycle contracts consumed by the frontend — preserved

- [ ] `DelayPolicy_*` per inventory §2.5 (incl. `OnRollbackApplied`/`IsRollbackSynced` fed by engine2). `DelayPolicy_GetStallThreshold` is retired WITH the rewritten input_override branch at M5 (its only consumer).
- [ ] `SyncPolicy_*`, `MatchLifecycle_*` (full header), `NetplayPhaseRuntime_*`
- [ ] `InputSyncHooks_SetLoadBarrierFreeze` / `SetTimesyncFreeze` / `IsModOwnedSync`
- [ ] `TransitionBarrier_*` full surface; `PhaseTransitionProposal/Ack` routed by every dispatch owner
- [ ] `ConnectionSupervisor_*` (verdict consumed instead of private timers)
- [ ] Spectator: `Transport_*ForHost` helper set + `SpectatorRuntime_On*` push API (inventory §6)
- [ ] `StateHistory_*` stays public (replay depends on it — do NOT privatize in engine2)
- [ ] `BarrierProtocol_IsReliable` / `GetChannel` / `SendPacket`
- [ ] Wire payloads byte-stable: `LockedMatchConfig` (20 B), palette 50–52, NAT signaling, ChurnPause, Ping, SyncTrace

## 6. Wire symbols — v20 state

Deleted at M1 (were dead): `SessionMeta`, `GameplayInput`, `WinScreenConfirm`.
Renamed at M1: `GekkoData` → `InputStream` (same id 23; carries raw Gekko bytes
until M5, then the v2 `InputStreamPayload` schema).
Deleted at M3 (session2 cutover): `Hello` (1), `HelloAck` (2) — superseded by
the live 5-step nonce handshake; their side data (nickname/round/timing/HUD
style) moved to `PeerIdentity` (79, new at M3, sent post-handshake).
Marked LEGACY, still live until the M5 cutover: `DelayChangeReq/Ack`,
`GekkoReady`, `SyncAnnounce`/`SyncConfirm` delay-negotiation fields.
Live from M3: `SessionHello/Offer/Ack/Confirm/ConfirmAck` (the handshake),
`PeerIdentity`; `DisconnectReason::Busy` (5) as ENet disconnect data.
Defined, still unsent (M4+): `InputStreamPayload` + `PressureReport`,
`SyncHash/SyncHashAck`, `TimeProbe/TimeProbeAck`, `ResyncReply`
(+ `ResyncRequest` payload), `FrontendPhaseId`,
`NetTransitionKind::EpochAlign` + payload fields, `phase_id` in the four
frontend payloads.
Changed at M4 (§3.2 terminal shape, INV-20): `DisconnectPayload` is now
`{code u8, reason_id u32 (fnv1a32 of the typed reason name), human[96]}`
(102 B pin); session2 fault terminals resend it at 100 ms across the bounded
goodbye window. Wire break is legal (v20 is dev-only until M6).
