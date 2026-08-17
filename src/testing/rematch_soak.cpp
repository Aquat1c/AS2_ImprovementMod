/**
 * Alice Senki 2 - Rematch Soak Monitor (Implementation)
 *
 * See include/testing/rematch_soak.h for the contract and
 * docs/RESILIENCE_TESTING.md for usage.
 *
 * Design notes:
 *  - Purely observational: no inputs are injected and no net/ state is
 *    mutated from here. The rematch loop itself is driven by the autoconnect
 *    state machine (match_count) and the fighting AI.
 *  - Iteration boundaries are edge-detected on Net::PregameSync_GetPhase():
 *      * first entry into GameplayHandoff  = baseline match (not counted)
 *      * leaving GameplayHandoff afterwards = a rematch iteration opens
 *        (rematch_cleanup aborts pregame to Idle, then PregameSync_Begin
 *        restarts the full announce/frontend/config/bootstrap ladder)
 *      * next entry into GameplayHandoff    = iteration PASS
 *  - Any disconnect-shaped event while the soak is unfinished is a FAIL and
 *    ends the soak (a dead session cannot continue rematching).
 */

#include "testing/rematch_soak.h"

#include "net/pregame_sync.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "net/netplay_menu_controller.h"
#include "net/netplay_menu_state.h"
#include "net/enet_transport.h"
#include "rollback/netplay_log.h"
#include "rollback/rollback_session.h"
#include "ui/log_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// State
// ============================================================================

namespace {

constexpr const char* kAutoConnectFile = "as2_autoconnect.cfg";

struct SoakState {
    bool     enabled;
    bool     finished;        // verdict frozen (target reached or fatal FAIL)
    bool     summaryDone;     // final summary emitted
    bool     isHost;
    int      target;          // N rematch iterations to verify
    int      cfgMatchCount;   // match_count parsed for the sanity warning
    int      passCount;
    int      failCount;
    int      handoffCount;    // GameplayHandoff entry edges observed
    bool     iterationOpen;   // a rematch cycle is in flight
    uint32_t iterationStartMs;
    bool     prevPhaseValid;
    Net::PregamePhase prevPhase;
    bool     sessionWasLive;  // saw Connected/Ready at least once

    // re0.7 M8 assertions (M6 obligation: epoch strictly increasing AND
    // canonical frame counter monotonic across the whole session).
    uint32_t epochMaxSeen;        // highest PregameSync epoch observed (0 = none)
    uint32_t epochAtLastHandoff;  // epoch when GameplayHandoff was last entered
    bool     epochHandoffValid;   // epochAtLastHandoff is meaningful
    bool     sawFrontendPhase;    // charsel/stagesel seen inside current iteration
    int      fastPathPasses;      // PASS iterations with no frontend phase (YES,YES)
    int      charselPasses;       // PASS iterations that routed through charsel (any NO)
    bool     canonValid;          // canonMax holds a real sample
    bool     canonWasActive;      // rollback session active on the previous frame
    int32_t  canonMax;            // highest canonical rb frame observed
};

SoakState s_soak = {};

// Static to keep the ~2.5KB snapshot off the per-frame stack. Single-threaded:
// only touched from the frame-update path.
NetMenu::MenuSnapshot s_menuSnap;

void SoakLog(const char* fmt, ...) {
    char buf[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    buf[sizeof(buf) - 1] = '\0';

    LOG_INFO("[Soak] %s", buf);
    Rollback::NetplayLog_Write("SOAK", -1, "%s", buf);
}

void TrimWs(char* s) {
    if (!s) return;
    char* start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r') start++;
    if (start != s) memmove(s, start, strlen(start) + 1);
    size_t len = strlen(s);
    while (len > 0 && (s[len - 1] == ' ' || s[len - 1] == '\t' ||
                       s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }
}

// Minimal re-parse of as2_autoconnect.cfg for soak-only keys. The menu
// controller's parser ignores unknown keys, so `soak_rematches` can live in
// the same [autoconnect] section without touching net/ code.
void ParseSoakConfig(int* outSoakRematches, int* outMatchCount) {
    *outSoakRematches = 0;
    *outMatchCount = 0;

    FILE* f = nullptr;
    if (fopen_s(&f, kAutoConnectFile, "r") != 0 || !f) {
        return;
    }

    char buf[4096] = {};
    const size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    bool inAutoConnect = false;
    char* ctx = nullptr;
    char* linePtr = strtok_s(buf, "\n", &ctx);
    while (linePtr) {
        char line[256];
        strncpy_s(line, sizeof(line), linePtr, _TRUNCATE);
        linePtr = strtok_s(nullptr, "\n", &ctx);
        TrimWs(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') continue;

        if (line[0] == '[') {
            inAutoConnect = (_stricmp(line, "[autoconnect]") == 0);
            continue;
        }
        if (!inAutoConnect) continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        char* key = line;
        char* val = eq + 1;
        TrimWs(key);
        TrimWs(val);

        if (_stricmp(key, "soak_rematches") == 0) {
            *outSoakRematches = atoi(val);
        } else if (_stricmp(key, "match_count") == 0) {
            *outMatchCount = atoi(val);
        }
    }
}

void FailSoak(const char* reasonFmt, ...) {
    if (s_soak.finished) return;

    char reason[192];
    va_list args;
    va_start(args, reasonFmt);
    vsnprintf(reason, sizeof(reason), reasonFmt, args);
    va_end(args);
    reason[sizeof(reason) - 1] = '\0';

    s_soak.failCount++;
    const int iter = s_soak.handoffCount;  // iteration that was in flight/next
    SoakLog("FAIL iteration=%d/%d: %s", iter, s_soak.target, reason);

    s_soak.iterationOpen = false;
    s_soak.finished = true;  // a dead session cannot continue the soak
}

void EmitSummary(const char* reason) {
    if (s_soak.summaryDone) return;
    s_soak.summaryDone = true;

    const int notRun = s_soak.target - s_soak.passCount - s_soak.failCount;
    const bool pass = (s_soak.passCount >= s_soak.target && s_soak.failCount == 0);
    SoakLog("SUMMARY (%s): verdict=%s target=%d passed=%d failed=%d not_run=%d baseline_matches=%d "
            "fastpath=%d charsel=%d epoch_max=%u canonical_max=%d",
            reason ? reason : "?",
            pass ? "SOAK-PASS" : "SOAK-FAIL",
            s_soak.target,
            s_soak.passCount,
            s_soak.failCount,
            notRun > 0 ? notRun : 0,
            s_soak.handoffCount > 0 ? 1 : 0,
            s_soak.fastPathPasses,
            s_soak.charselPasses,
            s_soak.epochMaxSeen,
            s_soak.canonValid ? s_soak.canonMax : -1);
    Rollback::NetplayLog_Flush();
}

}  // namespace

// ============================================================================
// Public API
// ============================================================================

void RematchSoak_Init(bool isHost) {
    memset(&s_soak, 0, sizeof(s_soak));
    s_soak.isHost = isHost;
    s_soak.prevPhase = Net::PregamePhase::Idle;

    int cfgSoak = 0;
    ParseSoakConfig(&cfgSoak, &s_soak.cfgMatchCount);

    // Env override (used by launchers that don't want to rewrite the cfg).
    char envBuf[16] = {};
    const DWORD n = GetEnvironmentVariableA("AS2_SOAK_REMATCHES", envBuf, sizeof(envBuf));
    if (n > 0 && n < sizeof(envBuf)) {
        cfgSoak = atoi(envBuf);
    }

    if (cfgSoak <= 0) {
        return;  // soak monitoring not requested
    }

    s_soak.target = cfgSoak;
    s_soak.enabled = true;

    SoakLog("Armed: role=%s target_rematches=%d match_count=%d fault_injection=%s",
            isHost ? "Host" : "Client",
            s_soak.target,
            s_soak.cfgMatchCount,
            Net::Transport_FaultInjectionActive() ? "ACTIVE" : "off");

    // The autoconnect driver stops after match_count matches; the soak needs
    // the first match plus N rematches.
    if (s_soak.cfgMatchCount < s_soak.target + 1) {
        SoakLog("WARNING: match_count=%d < soak_rematches+1=%d -- the autoconnect driver "
                "will stop early; set match_count >= %d",
                s_soak.cfgMatchCount, s_soak.target + 1, s_soak.target + 1);
    }
}

void RematchSoak_FrameUpdate(const char* autoconnectStateName, uint32_t frameCounter) {
    if (!s_soak.enabled || s_soak.finished) return;

    // ---- 0. re0.7 M8 continuous assertions ---------------------------------
    // (a) Epoch is a host-minted, strictly increasing session generation
    //     (§2.5, INV-15 companion). Any observed regression is structural.
    const uint32_t epoch = Net::PregameSync_GetCurrentEpoch();
    if (epoch != 0) {
        if (s_soak.epochMaxSeen != 0 && epoch < s_soak.epochMaxSeen) {
            FailSoak("epoch regressed: %u -> %u (must be strictly increasing, plan §2.5)",
                     s_soak.epochMaxSeen, epoch);
            EmitSummary("epoch regression");
            return;
        }
        if (epoch > s_soak.epochMaxSeen) s_soak.epochMaxSeen = epoch;
    }

    // (b) Canonical frame counter never goes backward (INV-15). The engine2
    //     backend keeps the engine armed across matches (suspend + rotate),
    //     so the counter must be monotonic for the WHOLE session.
    {
        const bool rbActive = Rollback::RollbackSession_IsActive();
        if (rbActive) {
            const int32_t rb = Rollback::RollbackSession_GetCurrentFrame();
            const bool comparable = s_soak.canonValid;
            if (comparable && rb < s_soak.canonMax) {
                FailSoak("canonical frame counter regressed: %d -> %d (INV-15)",
                         s_soak.canonMax, rb);
                EmitSummary("canonical counter regression");
                return;
            }
            if (!s_soak.canonValid || rb > s_soak.canonMax) {
                s_soak.canonMax = rb;
                s_soak.canonValid = true;
            }
        }
        s_soak.canonWasActive = rbActive;
    }

    // ---- 1. PASS edges: pregame phase reaching GameplayHandoff -------------
    const Net::PregamePhase phase = Net::PregameSync_GetPhase();

    // Track which route the in-flight iteration took: the YES,YES fast path
    // never enters a frontend phase (EpochAlign(None) -> ConfigExchange);
    // any-NO routes through charsel (§2.5 deterministic phase schedule).
    if (s_soak.iterationOpen &&
        (phase == Net::PregamePhase::FrontendCharSel ||
         phase == Net::PregamePhase::FrontendStageSel)) {
        s_soak.sawFrontendPhase = true;
    }

    if (s_soak.prevPhaseValid &&
        phase == Net::PregamePhase::GameplayHandoff &&
        s_soak.prevPhase != Net::PregamePhase::GameplayHandoff) {
        s_soak.handoffCount++;
        if (s_soak.handoffCount == 1) {
            SoakLog("Baseline match reached GameplayHandoff (frame=%u epoch=%u) -- soak of %d rematches begins after it",
                    frameCounter, epoch, s_soak.target);
        } else {
            const int iter = s_soak.handoffCount - 1;
            const uint32_t durMs = s_soak.iterationOpen
                ? (GetTickCount() - s_soak.iterationStartMs) : 0;

            // M8 assertion: every rematch handoff must carry a HIGHER epoch
            // than the previous handoff (rotation per cycle, §2.5/§4.4).
            if (s_soak.epochHandoffValid && epoch != 0 &&
                epoch <= s_soak.epochAtLastHandoff) {
                FailSoak("epoch did not increase across rematch: handoff epoch %u after %u",
                         epoch, s_soak.epochAtLastHandoff);
                EmitSummary("epoch not rotated");
                return;
            }

            const char* path = s_soak.sawFrontendPhase ? "charsel" : "fastpath";
            if (s_soak.sawFrontendPhase) s_soak.charselPasses++;
            else                          s_soak.fastPathPasses++;

            s_soak.passCount++;
            s_soak.iterationOpen = false;
            SoakLog("PASS iteration=%d/%d: new pregame sync completed (GameplayHandoff) in %ums epoch=%u path=%s canonical=%d",
                    iter, s_soak.target, durMs, epoch, path,
                    s_soak.canonValid ? s_soak.canonMax : -1);
            if (s_soak.passCount >= s_soak.target) {
                s_soak.finished = true;
                EmitSummary("target reached");
                return;
            }
        }
        if (epoch != 0) {
            s_soak.epochAtLastHandoff = epoch;
            s_soak.epochHandoffValid = true;
        }
    }

    // Leaving GameplayHandoff after a completed match opens the next rematch
    // iteration (rematch cleanup aborts pregame, then the driver relaunches).
    if (s_soak.prevPhaseValid &&
        s_soak.prevPhase == Net::PregamePhase::GameplayHandoff &&
        phase != Net::PregamePhase::GameplayHandoff &&
        s_soak.handoffCount > 0 &&
        !s_soak.iterationOpen) {
        s_soak.iterationOpen = true;
        s_soak.iterationStartMs = GetTickCount();
        s_soak.sawFrontendPhase = false;
        SoakLog("Iteration %d/%d begin (pregame left GameplayHandoff -> %s, frame=%u epoch=%u)",
                s_soak.handoffCount, s_soak.target,
                Net::PregamePhaseName(phase), frameCounter, epoch);
    }

    s_soak.prevPhase = phase;
    s_soak.prevPhaseValid = true;

    // ---- 2. FAIL detection --------------------------------------------------
    if (phase == Net::PregamePhase::Error) {
        Net::PregameSnapshot pgSnap{};
        Net::PregameSync_GetSnapshot(&pgSnap);
        FailSoak("pregame sync Error phase: %s",
                 pgSnap.error_text[0] ? pgSnap.error_text : "(no error text)");
        EmitSummary("pregame error");
        return;
    }

    NetMenu::GetSnapshot(&s_menuSnap);
    if (s_menuSnap.state == NetMenu::MenuState::DisconnectError) {
        FailSoak("DisconnectError: %s",
                 s_menuSnap.last_error[0] ? s_menuSnap.last_error : "(no reason)");
        EmitSummary("disconnect error");
        return;
    }

    Net::SessionSnapshot snap{};
    Net::Session_GetSnapshot(&snap);
    const bool live = (snap.state == Net::SessionState::Connected ||
                       snap.state == Net::SessionState::Ready);
    if (live) {
        s_soak.sessionWasLive = true;
    } else if (s_soak.sessionWasLive) {
        if (snap.state == Net::SessionState::Failed) {
            FailSoak("session Failed: %s",
                     snap.error_text[0] ? snap.error_text : "(no error text)");
            EmitSummary("session failed");
            return;
        }
        // Connected/Ready -> Idle/Disconnecting before the target was hit:
        // the session ended underneath the soak.
        FailSoak("session ended mid-soak (state=%s)", Net::SessionStateName(snap.state));
        EmitSummary("session ended");
        return;
    }

    // Autoconnect driver aborted (its own timeouts, launch failures, ...).
    if (autoconnectStateName && strcmp(autoconnectStateName, "Failed") == 0) {
        FailSoak("autoconnect driver reached Failed state");
        EmitSummary("autoconnect failed");
        return;
    }
}

void RematchSoak_Finish(const char* reason) {
    if (!s_soak.enabled || s_soak.summaryDone) return;

    if (!s_soak.finished) {
        if (s_soak.iterationOpen) {
            FailSoak("aborted with iteration in flight: %s", reason ? reason : "?");
        } else {
            s_soak.finished = true;
        }
    }
    EmitSummary(reason ? reason : "finish");
}

bool RematchSoak_IsEnabled() {
    return s_soak.enabled;
}
