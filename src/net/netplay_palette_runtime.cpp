#include "net/netplay_palette_runtime.h"

#include "core/game_state.h"
#include "net/netplay_palette_storage.h"
#include "net/player_side_mapping.h"
#include "net/gameplay_bridge.h"
#include "net/match_lifecycle.h"
#include "patches/charsel_palette_select.h"
#include "net/session_manager.h"
#include "net/session_types.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"
#include "ui/mod_menu.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace {

using namespace Net;

constexpr DWORD kPaletteResendIntervalMs = 500;
constexpr const char* kCharacterArchiveStemById[] = {
    "ran",
    "hat",
    "pat",
    "see",
    "ray",
    "ari",
    "mar",
    "shi",
    "fan",
    "mik",
    "men",
    "han",
    "sat",
    "tig",
    "esc",
    "mak",
    "ali",
    "nal",
    "dem",
    "lit",
    "tad",
    "fna",
};

struct PlayerRuntime {
    bool               valid;
    uint8_t            game_slot;
    uint8_t            character_id;
    uint8_t            base_palette;
    uint8_t            remote_flags;
    uint32_t           remote_epoch;
    uint32_t           remote_config_hash;
    bool               asset_loaded;
    uint16_t           asset_count;
    char               archive_path[MAX_PATH];
    char               patch_path[MAX_PATH];
    NetplayPaletteBank vanilla_bank;
    NetplayPaletteBank live_bank;
    bool               live_bank_loaded;
    NetplayPaletteBank local_custom_bank;
    bool               local_custom_loaded;
    NetplayPaletteBank saved_custom_bank;
    bool               saved_custom_loaded;
    bool               local_visual_override_enabled;
    NetplayPaletteBank remote_custom_bank;
    bool               remote_custom_loaded;
};

static bool          s_initialized = false;
static bool          s_enabled = true;
static bool          s_remotePreviewEnabled = false;
static bool          s_remoteMatchCustomEnabled = true;
static bool          s_matchActive = false;
static bool          s_winscreenSuppressOverrides = false;
static bool          s_gameplayReapplyIssued = false;
static bool          s_tabWasDown = false;
static uint32_t      s_localEpoch = 0;
static uint32_t      s_stateRevision = 0;
static uint32_t      s_configHash = 0;
static bool          s_localDirty = false;
static bool          s_localSent = false;
static bool          s_remoteAcknowledged = false;
static DWORD         s_lastSendAt = 0;
static int           s_localGameSlot = -1;
static int           s_offlineEditorGameSlot = -1;
static PlayerRuntime s_player[2] = {};
static bool          s_liveReloadRequested[2] = {};
static char          s_status[128] = "Palette sync idle.";

static void CopyText(char* dst, size_t dstSize, const char* src) {
    if (!dst || dstSize == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy_s(dst, dstSize, src, _TRUNCATE);
}

static void SetStatus(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(s_status, sizeof(s_status), _TRUNCATE, fmt, ap);
    va_end(ap);
}

static void ZeroBank(NetplayPaletteBank* bank) {
    if (!bank) {
        return;
    }
    memset(bank, 0, sizeof(*bank));
    bank->valid = false;
}

static void ClearPlayerRuntime(PlayerRuntime* player, uint8_t gameSlot) {
    if (!player) {
        return;
    }
    memset(player, 0, sizeof(*player));
    player->game_slot = gameSlot;
}

static void AdvanceLocalEpoch() {
    ++s_localEpoch;
    if (s_localEpoch == 0) {
        s_localEpoch = 1;
    }
}

static void AdvanceStateRevision() {
    ++s_stateRevision;
    if (s_stateRevision == 0) {
        s_stateRevision = 1;
    }
}

static void ResetMatchState(const char* reason) {
    s_remoteMatchCustomEnabled = true;
    s_matchActive = false;
    s_winscreenSuppressOverrides = false;
    s_gameplayReapplyIssued = false;
    s_tabWasDown = false;
    s_localEpoch = 0;
    s_stateRevision = 0;
    s_configHash = 0;
    s_localDirty = false;
    s_localSent = false;
    s_remoteAcknowledged = false;
    s_lastSendAt = 0;
    s_localGameSlot = -1;
    s_offlineEditorGameSlot = -1;
    memset(s_liveReloadRequested, 0, sizeof(s_liveReloadRequested));
    ClearPlayerRuntime(&s_player[0], 0);
    ClearPlayerRuntime(&s_player[1], 1);
    SetStatus("Palette sync idle%s%s",
        reason ? ": " : "",
        reason ? reason : "");
    LOG_INFO("[Palette] Runtime reset: reason=%s", reason ? reason : "none");
}

static bool IsValidGameSlot(int gameSlot) {
    return gameSlot >= 0 && gameSlot < 2;
}

static bool IsModNetplayFrontendContext() {
    return GetGameMode() == MODE_CHARSEL && Session_IsConnected();
}

static int GetFrontendPreviewLocalSlot() {
    if (s_matchActive ||
        !IsModNetplayFrontendContext()) {
        return -1;
    }

    return Session_GetRole() == SessionRole::Host ? 0 : 1;
}

static bool HasOfflineLocalContext() {
    return !s_matchActive &&
           (s_player[0].valid || s_player[1].valid);
}

static bool IsOfflinePaletteEditingGameType(uint32_t gameType) {
    switch (gameType) {
        case GAMETYPE_ARCADE:
        case GAMETYPE_VS_CPU:
        case GAMETYPE_VS_HUMAN:
        case GAMETYPE_TRAINING:
            return true;
        default:
            return false;
    }
}

static bool IsOfflinePaletteContextRetentionMode(uint32_t gameMode) {
    switch (gameMode) {
        case MODE_CHARSEL:
        case MODE_PREMATCH_INTRO:
        case MODE_MATCH:
            return true;
        default:
            return false;
    }
}

static bool IsIgnoredOfflinePaletteGameType(uint32_t gameType) {
    return gameType == GAMETYPE_NETPLAY ||
           gameType == GAMETYPE_REPLAY ||
           gameType == GAMETYPE_DEMO;
}

static bool IsSupportedFrontendPaletteContext(uint32_t gameMode, uint32_t gameType) {
    if (gameMode == MODE_CHARSEL && Session_IsConnected()) {
        return true;
    }

    return !IsIgnoredOfflinePaletteGameType(gameType) &&
           IsOfflinePaletteEditingGameType(gameType);
}

static bool ShouldRetainFrontendPaletteContext() {
    const uint32_t gameType = GetGameType();
    const uint32_t gameMode = GetGameMode();

    if (Session_IsConnected()) {
        return gameMode == MODE_CHARSEL;
    }

    if (IsIgnoredOfflinePaletteGameType(gameType)) {
        return false;
    }

    return IsOfflinePaletteContextRetentionMode(gameMode);
}

static int FindFirstValidOfflineGameSlot() {
    for (int slot = 0; slot < 2; slot++) {
        if (s_player[slot].valid) {
            return slot;
        }
    }
    return -1;
}

static int GetEditableGameSlot() {
    if (s_matchActive) {
        return IsValidGameSlot(s_localGameSlot) ? s_localGameSlot : -1;
    }

    if (GetGameMode() != MODE_MATCH || !HasOfflineLocalContext()) {
        return -1;
    }

    if (IsValidGameSlot(s_offlineEditorGameSlot) && s_player[s_offlineEditorGameSlot].valid) {
        return s_offlineEditorGameSlot;
    }

    return FindFirstValidOfflineGameSlot();
}

static int ResolveCharacterIdFromArchivePath(const char* archivePath) {
    if (!archivePath || !archivePath[0]) {
        return -1;
    }

    const char* stem = archivePath;
    for (const char* scan = archivePath; *scan; ++scan) {
        if (*scan == '\\' || *scan == '/') {
            stem = scan + 1;
        }
    }

    char fileStem[16] = {};
    size_t stemLen = 0;
    while (stem[stemLen] && stem[stemLen] != '.' && stemLen + 1 < sizeof(fileStem)) {
        fileStem[stemLen] = stem[stemLen];
        ++stemLen;
    }
    fileStem[stemLen] = '\0';

    if (stemLen == 0) {
        return -1;
    }

    for (int i = 0; i < (int)(sizeof(kCharacterArchiveStemById) / sizeof(kCharacterArchiveStemById[0])); ++i) {
        if (_stricmp(fileStem, kCharacterArchiveStemById[i]) == 0) {
            return i;
        }
    }

    return -1;
}

static bool HasCurrentLocalCustomBank(const PlayerRuntime& player) {
    return player.local_custom_loaded &&
           player.local_custom_bank.valid &&
           player.local_custom_bank.character_id == player.character_id &&
           player.local_custom_bank.base_palette == player.base_palette;
}

static bool HasCurrentSavedCustomBank(const PlayerRuntime& player) {
    return player.saved_custom_loaded &&
           player.saved_custom_bank.valid &&
           player.saved_custom_bank.character_id == player.character_id &&
           player.saved_custom_bank.base_palette == player.base_palette;
}

static bool CopyBank(const NetplayPaletteBank& bank, NetplayPaletteBank* out);

static bool ShouldUseSelectedCustomBank(int gameSlot) {
    return IsValidGameSlot(gameSlot) &&
           s_player[gameSlot].valid &&
           CharSelPaletteSelect_ShouldUseCustomBank(
               (uint8_t)gameSlot,
               s_player[gameSlot].character_id,
               s_player[gameSlot].base_palette);
}

static bool RemoteSelectionClaimsCustomBank(int gameSlot) {
    return s_matchActive &&
           IsValidGameSlot(gameSlot) &&
           gameSlot != s_localGameSlot &&
           s_player[gameSlot].valid &&
           (s_player[gameSlot].remote_flags & NETPLAY_PALETTE_FLAG_HAS_CUSTOM_DATA) != 0;
}

static bool HasRemoteMatchNetplayBank(int gameSlot) {
    return s_matchActive &&
           IsValidGameSlot(gameSlot) &&
           gameSlot != s_localGameSlot &&
           s_player[gameSlot].valid &&
           s_player[gameSlot].remote_custom_loaded &&
           RemoteSelectionClaimsCustomBank(gameSlot);
}

static bool HasRemoteMatchVanillaBank(int gameSlot) {
    return s_matchActive &&
           IsValidGameSlot(gameSlot) &&
           gameSlot != s_localGameSlot &&
           s_player[gameSlot].valid &&
           s_player[gameSlot].vanilla_bank.valid &&
           RemoteSelectionClaimsCustomBank(gameSlot);
}

static bool HasRemoteMatchPaletteChoice(int gameSlot) {
    return HasRemoteMatchNetplayBank(gameSlot) &&
           HasRemoteMatchVanillaBank(gameSlot);
}

static const char* RemoteMatchPaletteViewName() {
    return s_remoteMatchCustomEnabled ? "netplay" : "vanilla";
}

static bool HasRemoteMatchDisplayBank(int gameSlot) {
    return s_remoteMatchCustomEnabled
        ? HasRemoteMatchNetplayBank(gameSlot)
        : HasRemoteMatchVanillaBank(gameSlot);
}

static bool CopyRemoteMatchDisplayBank(int gameSlot, NetplayPaletteBank* out) {
    const bool netplayReady = HasRemoteMatchNetplayBank(gameSlot);
    const bool vanillaReady = HasRemoteMatchVanillaBank(gameSlot);
    const bool choiceReady = HasRemoteMatchPaletteChoice(gameSlot);
    const bool displayReady = HasRemoteMatchDisplayBank(gameSlot);

    if (!out || !displayReady) {
        if (IsValidGameSlot(gameSlot)) {
            const PlayerRuntime& player = s_player[gameSlot];
            LOG_INFO("[Palette] Remote match palette read unavailable slot=P%d view=%s choice=%d vanilla_ready=%d netplay_ready=%d valid=%d char=%u base=%u vanilla_crc=0x%08X netplay_crc=0x%08X",
                gameSlot + 1,
                RemoteMatchPaletteViewName(),
                choiceReady ? 1 : 0,
                vanillaReady ? 1 : 0,
                netplayReady ? 1 : 0,
                player.valid ? 1 : 0,
                player.character_id,
                player.base_palette,
                player.vanilla_bank.crc32,
                player.remote_custom_bank.crc32);
        }
        return false;
    }

    const PlayerRuntime& player = s_player[gameSlot];
    const NetplayPaletteBank& selected = s_remoteMatchCustomEnabled
        ? player.remote_custom_bank
        : player.vanilla_bank;
    LOG_INFO("[Palette] Remote match palette read slot=P%d view=%s char=%u base=%u selected_crc=0x%08X vanilla_crc=0x%08X netplay_crc=0x%08X choice=%d",
        gameSlot + 1,
        RemoteMatchPaletteViewName(),
        player.character_id,
        player.base_palette,
        selected.crc32,
        player.vanilla_bank.crc32,
        player.remote_custom_bank.crc32,
        choiceReady ? 1 : 0);

    return s_remoteMatchCustomEnabled
        ? CopyBank(s_player[gameSlot].remote_custom_bank, out)
        : CopyBank(s_player[gameSlot].vanilla_bank, out);
}

static bool AreRemotePaletteHotkeyModifiersDown() {
    return (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
           (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
}

static bool HasForcedLocalVisualCustomBank(int gameSlot) {
    if (!IsValidGameSlot(gameSlot)) {
        return false;
    }

    const PlayerRuntime& player = s_player[gameSlot];
    if (!player.valid ||
        !player.local_visual_override_enabled ||
        !HasCurrentLocalCustomBank(player)) {
        return false;
    }

    if (!s_matchActive) {
        return true;
    }

    return gameSlot == s_localGameSlot;
}

static bool ShouldUsePreviewCustomBank(int gameSlot) {
    if (!IsValidGameSlot(gameSlot) || !s_player[gameSlot].valid) {
        return false;
    }

    if (GetGameMode() != MODE_CHARSEL) {
        return false;
    }

    const int localPreviewSlot = GetFrontendPreviewLocalSlot();
    if (IsValidGameSlot(localPreviewSlot) && gameSlot != localPreviewSlot) {
        return false;
    }

    return CharSelPaletteSelect_ShouldPreviewCustomBank(
        (uint8_t)gameSlot,
        s_player[gameSlot].character_id,
        s_player[gameSlot].base_palette);
}

static bool ShouldUseVisualCustomBank(int gameSlot) {
    if (!IsValidGameSlot(gameSlot) || !s_player[gameSlot].valid) {
        return false;
    }

    if (HasForcedLocalVisualCustomBank(gameSlot)) {
        return true;
    }

    if (!s_matchActive && GetGameMode() == MODE_CHARSEL) {
        return ShouldUsePreviewCustomBank(gameSlot);
    }

    if (s_matchActive && gameSlot != s_localGameSlot) {
        return RemoteSelectionClaimsCustomBank(gameSlot);
    }

    return ShouldUseSelectedCustomBank(gameSlot);
}

static bool LocalTransportHasCustomBank() {
    return IsValidGameSlot(s_localGameSlot) &&
           s_enabled &&
           s_player[s_localGameSlot].local_custom_loaded &&
           ShouldUseSelectedCustomBank(s_localGameSlot);
}

static bool HasVisualOverrideForGameSlot(int gameSlot) {
    if (!IsValidGameSlot(gameSlot)) {
        return false;
    }

    const PlayerRuntime& player = s_player[gameSlot];
    if (!player.valid) {
        return false;
    }

    if (!ShouldUseVisualCustomBank(gameSlot)) {
        return false;
    }

    if (!s_matchActive) {
        return player.local_custom_loaded;
    }

    if (gameSlot == s_localGameSlot) {
        return player.local_custom_loaded;
    }

    return HasRemoteMatchDisplayBank(gameSlot);
}

static uint8_t BuildLocalFlags() {
    uint8_t flags = 0;
    if (s_enabled) {
        flags |= NETPLAY_PALETTE_FLAG_TRANSPORT_ENABLED;
    }
    if (s_remotePreviewEnabled) {
        flags |= NETPLAY_PALETTE_FLAG_REMOTE_PREVIEW_ENABLED;
    }
    if (LocalTransportHasCustomBank()) {
        flags |= NETPLAY_PALETTE_FLAG_HAS_CUSTOM_DATA;
    }
    if (LocalTransportHasCustomBank()) {
        flags |= NETPLAY_PALETTE_FLAG_SPECTATOR_PROPAGATE;
    }
    return flags;
}

static void MarkLocalDirty(bool visualStateChanged, const char* reason) {
    AdvanceLocalEpoch();
    if (visualStateChanged) {
        AdvanceStateRevision();
    }
    s_localDirty = true;
    s_localSent = false;
    s_remoteAcknowledged = false;
    s_lastSendAt = 0;
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Local palette dirty: visual=%d reason=%s epoch=%u state=%u",
        visualStateChanged ? 1 : 0,
        reason ? reason : "unspecified",
        s_localEpoch,
        s_stateRevision);
}

static void RequestLiveReload(uint8_t gameSlot, const char* reason) {
    if (!IsValidGameSlot(gameSlot) || !s_player[gameSlot].asset_loaded) {
        return;
    }
    s_liveReloadRequested[gameSlot] = true;
    LOG_INFO("[Palette] Live reload requested slot=P%d reason=%s",
        gameSlot + 1,
        reason ? reason : "unspecified");
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Live reload requested: slot=P%d reason=%s",
        gameSlot + 1,
        reason ? reason : "unspecified");
}

static bool RequestGameplayPaletteReapply(const char* reason) {
    if (!s_matchActive) {
        return true;
    }

    int reloadCount = 0;
    bool waitingForAssets = false;
    for (int slot = 0; slot < 2; ++slot) {
        if (!HasVisualOverrideForGameSlot(slot)) {
            continue;
        }

        if (!s_player[slot].asset_loaded) {
            waitingForAssets = true;
            continue;
        }

        RequestLiveReload((uint8_t)slot, reason);
        ++reloadCount;
    }

    if (reloadCount > 0) {
        SetStatus("Refreshing match palettes before the round starts");
        Rollback::NetplayLog_Write("PALETTE", -1,
            "Intro palette reapply: reload_count=%d local_slot=%d remote_palette=%s",
            reloadCount,
            s_localGameSlot,
            s_remoteMatchCustomEnabled ? "netplay" : "vanilla");
    }

    return !waitingForAssets;
}

static void MaybeReapplyGameplayOverrides() {
    if (!s_matchActive) {
        s_gameplayReapplyIssued = false;
        return;
    }

    const bool introWindowActive =
        !GameplayBridge_IsSessionActive() &&
        GetGameMode() == MODE_MATCH &&
        MatchLifecycle_GetPhase() == MatchLifecyclePhase::IntroActive;
    if (!introWindowActive) {
        return;
    }

    if (s_gameplayReapplyIssued) {
        return;
    }

    s_gameplayReapplyIssued = RequestGameplayPaletteReapply("intro entry reapply");
}

static void ProcessMatchPaletteHotkeys() {
    const bool tabDown = (GetAsyncKeyState(VK_TAB) & 0x8000) != 0;
    const bool modifiersDown = AreRemotePaletteHotkeyModifiersDown();

    const bool allowHotkey = s_matchActive &&
        GameplayBridge_IsSessionActive() &&
        GetGameMode() == MODE_MATCH &&
        !ModMenu_IsOpen();

    if (tabDown && !s_tabWasDown) {
        const int remoteSlot = IsValidGameSlot(s_localGameSlot)
            ? (s_localGameSlot == 0 ? 1 : 0)
            : -1;
        const bool choiceAvailable = HasRemoteMatchPaletteChoice(remoteSlot);
        const bool vanillaReady = HasRemoteMatchVanillaBank(remoteSlot);
        const bool netplayReady = HasRemoteMatchNetplayBank(remoteSlot);

        LOG_INFO("[Palette] Tab press detected allow=%d modifiers=%d remote_slot=%d current_view=%s choice=%d vanilla_ready=%d netplay_ready=%d menu_open=%d gameplay_active=%d mode=%u",
            allowHotkey ? 1 : 0,
            modifiersDown ? 1 : 0,
            IsValidGameSlot(remoteSlot) ? (remoteSlot + 1) : 0,
            RemoteMatchPaletteViewName(),
            choiceAvailable ? 1 : 0,
            vanillaReady ? 1 : 0,
            netplayReady ? 1 : 0,
            ModMenu_IsOpen() ? 1 : 0,
            GameplayBridge_IsSessionActive() ? 1 : 0,
            (unsigned)GetGameMode());

        if (allowHotkey && !modifiersDown && choiceAvailable) {
            s_remoteMatchCustomEnabled = !s_remoteMatchCustomEnabled;
            RequestLiveReload((uint8_t)remoteSlot,
                s_remoteMatchCustomEnabled
                    ? "remote match switched to netplay palette"
                    : "remote match switched to captured vanilla palette");

            SetStatus("Opponent palette view: %s",
                s_remoteMatchCustomEnabled ? "Synced" : "Original");
            LOG_INFO("[Palette] Remote match palette switched to %s via Tab hotkey",
                s_remoteMatchCustomEnabled ? "NETPLAY" : "VANILLA");
            Rollback::NetplayLog_Write("PALETTE", -1,
                "Remote match palette switched: view=%s",
                s_remoteMatchCustomEnabled ? "netplay" : "vanilla");
        } else {
            const char* reason = !allowHotkey
                ? "hotkey not allowed"
                : modifiersDown
                    ? "modifier held"
                    : "missing vanilla/netplay palette choice";
            LOG_INFO("[Palette] Tab press ignored: %s", reason);
        }
    }

    s_tabWasDown = tabDown;
}

static void RefreshLocalStoredBank() {
    if (!IsValidGameSlot(s_localGameSlot)) {
        return;
    }

    PlayerRuntime& local = s_player[s_localGameSlot];
    local.local_visual_override_enabled = false;
    ZeroBank(&local.saved_custom_bank);
    local.saved_custom_loaded = NetplayPaletteStorage_GetBank(
        local.character_id,
        local.base_palette,
        &local.saved_custom_bank);

    ZeroBank(&local.local_custom_bank);
    local.local_custom_loaded = false;
    if (local.saved_custom_loaded) {
        local.local_custom_bank = local.saved_custom_bank;
        local.local_custom_loaded = true;
    }
}

static void RefreshStoredBankForSlot(uint8_t gameSlot) {
    if (!IsValidGameSlot(gameSlot)) {
        return;
    }

    PlayerRuntime& player = s_player[gameSlot];
    player.local_visual_override_enabled = false;
    ZeroBank(&player.saved_custom_bank);
    player.saved_custom_loaded = NetplayPaletteStorage_GetBank(
        player.character_id,
        player.base_palette,
        &player.saved_custom_bank);

    ZeroBank(&player.local_custom_bank);
    player.local_custom_loaded = false;
    if (player.saved_custom_loaded) {
        player.local_custom_bank = player.saved_custom_bank;
        player.local_custom_loaded = true;
    }
}

static void MaybeArmOfflineLocalContext(uint8_t gameSlot,
                                        uint8_t characterId,
                                        uint8_t basePalette) {
    const uint32_t gameMode = GetGameMode();
    const uint32_t gameType = GetGameType();
    if (s_matchActive) {
        LOG_INFO("[Palette] Offline context skip: slot=P%d char=%u base=%u reason=match-active mode=%u sub=%u type=%u",
            gameSlot + 1,
            characterId,
            basePalette,
            (unsigned)gameMode,
            (unsigned)GetSubstate(),
            (unsigned)gameType);
        return;
    }

    if (!IsSupportedFrontendPaletteContext(gameMode, gameType)) {
        LOG_INFO("[Palette] Offline context skip: slot=P%d char=%u base=%u reason=game-type mode=%u sub=%u type=%u",
            gameSlot + 1,
            characterId,
            basePalette,
            (unsigned)gameMode,
            (unsigned)GetSubstate(),
            (unsigned)gameType);
        return;
    }

    PlayerRuntime& player = s_player[gameSlot];
    const bool sameContext = player.valid &&
        player.character_id == characterId &&
        player.base_palette == basePalette;

    player.valid = true;
    player.game_slot = gameSlot;
    player.character_id = characterId;
    player.base_palette = basePalette;

    if (!IsValidGameSlot(s_offlineEditorGameSlot) || !s_player[s_offlineEditorGameSlot].valid) {
        s_offlineEditorGameSlot = gameSlot;
    }

    if (sameContext) {
        return;
    }

    RefreshStoredBankForSlot(gameSlot);
    AdvanceStateRevision();
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Offline palette context armed: slot=P%d char=%u base=%u stored_custom=%d",
        gameSlot + 1,
        player.character_id,
        player.base_palette,
        player.local_custom_loaded ? 1 : 0);
    LOG_INFO("[Palette] Offline context armed slot=P%d char=%u base=%u stored=%d mode=%u sub=%u type=%u",
        gameSlot + 1,
        player.character_id,
        player.base_palette,
        player.local_custom_loaded ? 1 : 0,
        (unsigned)GetGameMode(),
        (unsigned)GetSubstate(),
        (unsigned)GetGameType());
}

static void MarkPaletteChanged(uint8_t gameSlot, bool visualStateChanged, const char* reason) {
    if (s_matchActive && gameSlot == (uint8_t)s_localGameSlot) {
        MarkLocalDirty(visualStateChanged, reason);
        return;
    }

    if (visualStateChanged) {
        AdvanceStateRevision();
    }

    Rollback::NetplayLog_Write("PALETTE", -1,
        "Offline palette changed: slot=P%d visual=%d reason=%s state=%u",
        gameSlot + 1,
        visualStateChanged ? 1 : 0,
        reason ? reason : "unspecified",
        s_stateRevision);
}

static void SendAck(uint8_t gameSlot,
                    uint32_t epoch,
                    uint32_t configHash,
                    uint8_t accepted,
                    uint8_t receivedData,
                    uint32_t payloadCrc) {
    PaletteAckPayload ack{};
    ack.epoch = epoch;
    ack.config_hash = configHash;
    ack.game_slot = gameSlot;
    ack.accepted = accepted;
    ack.received_data = receivedData;
    ack.payload_crc = payloadCrc;
    Session_SendPacket(
        CHANNEL_CONTROL,
        PacketType::PaletteAck,
        &ack,
        sizeof(ack),
        true);
}

static bool SendLocalConfig() {
    if (!s_matchActive || !Session_IsConnected() || !IsValidGameSlot(s_localGameSlot)) {
        return false;
    }

    PlayerRuntime& local = s_player[s_localGameSlot];
    const bool hasCustomData = LocalTransportHasCustomBank();

    PaletteConfigPayload config{};
    config.epoch = s_localEpoch;
    config.config_hash = s_configHash;
    config.game_slot = (uint8_t)s_localGameSlot;
    config.character_id = local.character_id;
    config.base_palette = local.base_palette;
    config.flags = BuildLocalFlags();
    config.payload_size = hasCustomData ? NETPLAY_PALETTE_BANK_SIZE : 0;
    config.payload_crc = hasCustomData ? local.local_custom_bank.crc32 : 0;

    const bool configSent = Session_SendPacket(
        CHANNEL_CONTROL,
        PacketType::PaletteConfig,
        &config,
        sizeof(config),
        true);

    bool dataPacketSent = false;
    bool dataOk = !hasCustomData;
    if (configSent && hasCustomData) {
        PaletteDataPayload data{};
        data.epoch = s_localEpoch;
        data.config_hash = s_configHash;
        data.game_slot = (uint8_t)s_localGameSlot;
        data.character_id = local.character_id;
        data.base_palette = local.base_palette;
        data.payload_crc = local.local_custom_bank.crc32;
        data.payload_size = NETPLAY_PALETTE_BANK_SIZE;
        memcpy(data.payload, local.local_custom_bank.data, NETPLAY_PALETTE_BANK_SIZE);
        dataPacketSent = Session_SendPacket(
            CHANNEL_CONTROL,
            PacketType::PaletteData,
            &data,
            sizeof(data),
            true);
        dataOk = dataPacketSent;
    }

    if (configSent) {
        s_localSent = true;
        s_localDirty = false;
        s_lastSendAt = GetTickCount();
        SetStatus("Sent a palette update for P%d", s_localGameSlot + 1);
        Rollback::NetplayLog_Write("PALETTE", -1,
            "Local palette send: epoch=%u config=0x%08X slot=P%d char=%u base=%u flags=0x%02X size=%u crc=0x%08X data_sent=%d",
            s_localEpoch,
            s_configHash,
            s_localGameSlot + 1,
            local.character_id,
            local.base_palette,
            config.flags,
            (unsigned)config.payload_size,
            config.payload_crc,
            dataPacketSent ? 1 : 0);
    }

    return configSent && dataOk;
}

static bool CopyBank(const NetplayPaletteBank& bank, NetplayPaletteBank* out) {
    if (!out || !bank.valid) {
        return false;
    }
    *out = bank;
    return true;
}

static void PopulateBank(NetplayPaletteBank* bank,
                         uint8_t characterId,
                         uint8_t basePalette,
                         const void* data,
                         size_t len) {
    if (!bank || !data || len < NETPLAY_PALETTE_BANK_SIZE) {
        return;
    }

    bank->valid = true;
    bank->character_id = characterId;
    bank->base_palette = basePalette;
    memcpy(bank->data, data, NETPLAY_PALETTE_BANK_SIZE);
    bank->crc32 = CalcCRC32(bank->data, NETPLAY_PALETTE_BANK_SIZE);
}

static void UpdateObservedBank(PlayerRuntime* player,
                               NetplayPaletteBank* bank,
                               bool* loadedFlag,
                               uint8_t gameSlot,
                               uint8_t characterId,
                               uint8_t basePalette,
                               const void* data,
                               size_t len,
                               uint16_t assetCount,
                               const char* archivePath,
                               const char* patchPath,
                               const char* logLabel,
                               const char* statusLabel) {
    if (!player || !bank || !loadedFlag || !data || len < NETPLAY_PALETTE_BANK_SIZE) {
        return;
    }

    player->valid = true;
    player->game_slot = gameSlot;
    player->character_id = characterId;
    player->base_palette = basePalette;
    player->asset_loaded = true;
    player->asset_count = assetCount;
    CopyText(player->archive_path, sizeof(player->archive_path), archivePath);
    CopyText(player->patch_path, sizeof(player->patch_path), patchPath);

    PopulateBank(bank, characterId, basePalette, data, len);
    *loadedFlag = true;

    SetStatus("%s for P%d", statusLabel, gameSlot + 1);
    Rollback::NetplayLog_Write("PALETTE", -1,
        "%s: slot=P%d char=%u base=%u asset_count=%u crc=0x%08X",
        logLabel,
        gameSlot + 1,
        characterId,
        basePalette,
        (unsigned)assetCount,
        bank->crc32);
}

} // namespace

namespace Net {

void NetplayPaletteRuntime_Init() {
    if (s_initialized) {
        return;
    }
    NetplayPaletteStorage_Init();
    ResetMatchState(nullptr);
    s_initialized = true;
}

void NetplayPaletteRuntime_Shutdown() {
    if (!s_initialized) {
        return;
    }
    ResetMatchState("shutdown");
    NetplayPaletteStorage_Shutdown();
    s_initialized = false;
}

void NetplayPaletteRuntime_FrameUpdate() {
    if (!s_initialized) {
        return;
    }

    if (!s_matchActive) {
        s_tabWasDown = false;
        s_gameplayReapplyIssued = false;
        if (HasOfflineLocalContext() && !ShouldRetainFrontendPaletteContext()) {
            LOG_INFO("[Palette] Clearing offline context: mode=%u sub=%u type=%u has_p1=%d has_p2=%d",
                (unsigned)GetGameMode(),
                (unsigned)GetSubstate(),
                (unsigned)GetGameType(),
                s_player[0].valid ? 1 : 0,
                s_player[1].valid ? 1 : 0);
            ResetMatchState("left offline palette flow");
        } else if (!IsValidGameSlot(s_offlineEditorGameSlot) ||
                   (IsValidGameSlot(s_offlineEditorGameSlot) && !s_player[s_offlineEditorGameSlot].valid)) {
            s_offlineEditorGameSlot = FindFirstValidOfflineGameSlot();
        }
        return;
    }

    MaybeReapplyGameplayOverrides();
    ProcessMatchPaletteHotkeys();

    if (s_localDirty) {
        SendLocalConfig();
        return;
    }

    if (!s_remoteAcknowledged && s_localSent) {
        const DWORD now = GetTickCount();
        if ((now - s_lastSendAt) >= kPaletteResendIntervalMs) {
            SendLocalConfig();
        }
    }
}

void NetplayPaletteRuntime_SetSyncEnabled(bool enabled) {
    if (s_enabled == enabled) {
        return;
    }
    s_enabled = enabled;
    MarkLocalDirty(false, enabled ? "sync enabled" : "sync disabled");
}

void NetplayPaletteRuntime_SetRemotePreviewEnabled(bool enabled) {
    if (s_remotePreviewEnabled == enabled) {
        return;
    }

    s_remotePreviewEnabled = enabled;
    MarkLocalDirty(false, enabled ? "remote preview enabled" : "remote preview disabled");

    const int remoteSlot = IsValidGameSlot(s_localGameSlot)
        ? (s_localGameSlot == 0 ? 1 : 0)
        : -1;
    if (IsValidGameSlot(remoteSlot) && s_player[remoteSlot].remote_custom_loaded) {
        RequestLiveReload((uint8_t)remoteSlot,
            enabled ? "remote preview enabled" : "remote preview disabled");
    }
}

bool NetplayPaletteRuntime_GetSyncEnabled() {
    return s_enabled;
}

bool NetplayPaletteRuntime_GetRemotePreviewEnabled() {
    return s_remotePreviewEnabled;
}

void NetplayPaletteRuntime_OnLockedMatchConfig(const LockedMatchConfig* config) {
    if (!s_initialized || !config) {
        return;
    }

    ResetMatchState(nullptr);
    s_matchActive = true;
    s_configHash = LockedMatchConfig_Hash(config);

    s_player[0].valid = true;
    s_player[0].game_slot = 0;
    s_player[0].character_id = config->p1_character;
    s_player[0].base_palette = config->p1_palette;

    s_player[1].valid = true;
    s_player[1].game_slot = 1;
    s_player[1].character_id = config->p2_character;
    s_player[1].base_palette = config->p2_palette;

    const bool isHost = Session_GetRole() == SessionRole::Host;
    s_localGameSlot = PlayerMapping_DeriveFromRole(config->host_side, isHost);
    if (!IsValidGameSlot(s_localGameSlot)) {
        s_localGameSlot = isHost ? 0 : 1;
    }

    RefreshLocalStoredBank();
    s_remoteMatchCustomEnabled = true;
    s_gameplayReapplyIssued = false;
    AdvanceLocalEpoch();
    AdvanceStateRevision();
    s_localDirty = true;

    const bool localSelectedCustom = ShouldUseSelectedCustomBank(s_localGameSlot);
    const bool localTransportCustom = LocalTransportHasCustomBank();
    SetStatus("Palette sync ready for P%d", s_localGameSlot + 1);
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Palette runtime armed: epoch=%u config=0x%08X local=P%d remote=P%d local_custom=%d selected_custom=%d",
        s_localEpoch,
        s_configHash,
        s_localGameSlot + 1,
        s_localGameSlot == 0 ? 2 : 1,
        localTransportCustom ? 1 : 0,
        localSelectedCustom ? 1 : 0);
}

void NetplayPaletteRuntime_OnRoundRestart() {
    if (!s_initialized || !s_matchActive) {
        return;
    }

    s_winscreenSuppressOverrides = false;

    int reloadCount = 0;
    for (int slot = 0; slot < 2; ++slot) {
        if (!HasVisualOverrideForGameSlot(slot) || !s_player[slot].asset_loaded) {
            continue;
        }

        RequestLiveReload((uint8_t)slot, "round restart reapply");
        ++reloadCount;
    }

    SetStatus(reloadCount > 0
        ? "Refreshing match palettes for the new round"
        : "No palette refresh was needed for the new round");
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Round restart palette refresh: reload_count=%d local_slot=%d suppress=%d",
        reloadCount,
        s_localGameSlot,
        s_winscreenSuppressOverrides ? 1 : 0);
}

void NetplayPaletteRuntime_OnWinScreenEnter() {
    if (!s_initialized || !s_matchActive || s_winscreenSuppressOverrides) {
        return;
    }

    s_winscreenSuppressOverrides = true;

    int reloadCount = 0;
    for (int slot = 0; slot < 2; ++slot) {
        if (!HasVisualOverrideForGameSlot(slot) || !s_player[slot].asset_loaded) {
            continue;
        }

        RequestLiveReload((uint8_t)slot, "winscreen clear");
        ++reloadCount;
    }

    SetStatus(reloadCount > 0
        ? "Clearing match palette previews for the win screen"
        : "Palette previews are disabled on the win screen");
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Win screen palette clear: reload_count=%d local_slot=%d remote_preview=%d",
        reloadCount,
        s_localGameSlot,
        s_remotePreviewEnabled ? 1 : 0);
}

void NetplayPaletteRuntime_OnMatchEnd(const char* reason) {
    ResetMatchState(reason ? reason : "match ended");
}

void NetplayPaletteRuntime_OnDisconnect(const char* reason) {
    int remoteReloadSlot = -1;
    if (s_matchActive && IsValidGameSlot(s_localGameSlot)) {
        const int remoteSlot = s_localGameSlot == 0 ? 1 : 0;
        if (IsValidGameSlot(remoteSlot) &&
            s_player[remoteSlot].remote_custom_loaded &&
            s_player[remoteSlot].asset_loaded &&
            HasVisualOverrideForGameSlot(remoteSlot)) {
            remoteReloadSlot = remoteSlot;
        }
    }

    ResetMatchState(reason ? reason : "disconnect");

    if (IsValidGameSlot(remoteReloadSlot)) {
        s_liveReloadRequested[remoteReloadSlot] = true;
        LOG_INFO("[Palette] Queued remote palette cleanup slot=P%d reason=%s",
            remoteReloadSlot + 1,
            reason ? reason : "disconnect");
        Rollback::NetplayLog_Write("PALETTE", -1,
            "Queued remote palette cleanup: slot=P%d reason=%s",
            remoteReloadSlot + 1,
            reason ? reason : "disconnect");
    }
}

void NetplayPaletteRuntime_OnRemoteConfig(const PaletteConfigPayload* payload) {
    if (!payload || payload->game_slot > 1) {
        return;
    }

    if (!s_matchActive || payload->config_hash != s_configHash) {
        SendAck(payload->game_slot,
            payload->epoch,
            payload->config_hash,
            0,
            0,
            0);
        Rollback::NetplayLog_Write("PALETTE", -1,
            "Rejected remote palette config: epoch=%u config=0x%08X local_config=0x%08X slot=P%d active=%u flags=0x%02X",
            payload->epoch,
            payload->config_hash,
            s_configHash,
            payload->game_slot + 1,
            s_matchActive ? 1 : 0,
            payload->flags);
        return;
    }

    PlayerRuntime& remote = s_player[payload->game_slot];
    remote.remote_flags = payload->flags;
    remote.valid = true;
    remote.game_slot = payload->game_slot;
    remote.character_id = payload->character_id;
    remote.base_palette = payload->base_palette;
    remote.remote_epoch = payload->epoch;
    remote.remote_config_hash = payload->config_hash;

    const bool expectsData =
        (payload->flags & NETPLAY_PALETTE_FLAG_HAS_CUSTOM_DATA) != 0 &&
        payload->payload_size == NETPLAY_PALETTE_BANK_SIZE;

    if (!expectsData) {
        const bool hadCustomBank = remote.remote_custom_loaded;
        ZeroBank(&remote.remote_custom_bank);
        remote.remote_custom_loaded = false;
        SendAck(payload->game_slot,
            payload->epoch,
            payload->config_hash,
            1,
            0,
            0);

        if (hadCustomBank) {
            AdvanceStateRevision();
            if (payload->game_slot != s_localGameSlot) {
                RequestLiveReload(payload->game_slot, "remote palette cleared");
            }
        }
    }

    SetStatus(expectsData
            ? "Waiting for palette data from P%d"
            : "Received palette settings for P%d",
        payload->game_slot + 1);
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Remote palette config: epoch=%u config=0x%08X slot=P%d char=%u base=%u flags=0x%02X size=%u crc=0x%08X expects_data=%d remote_custom=%d",
        payload->epoch,
        payload->config_hash,
        payload->game_slot + 1,
        payload->character_id,
        payload->base_palette,
        payload->flags,
        (unsigned)payload->payload_size,
        payload->payload_crc,
        expectsData ? 1 : 0,
        RemoteSelectionClaimsCustomBank(payload->game_slot) ? 1 : 0);
}

void NetplayPaletteRuntime_OnRemoteData(const PaletteDataPayload* payload) {
    if (!payload || payload->game_slot > 1) {
        return;
    }

    PlayerRuntime& remote = s_player[payload->game_slot];
    const bool validPacket =
        s_matchActive &&
        payload->config_hash == s_configHash &&
        payload->epoch == remote.remote_epoch &&
        payload->config_hash == remote.remote_config_hash &&
        payload->payload_size == NETPLAY_PALETTE_BANK_SIZE &&
        payload->character_id == remote.character_id &&
        payload->base_palette == remote.base_palette &&
        CalcCRC32(payload->payload, NETPLAY_PALETTE_BANK_SIZE) == payload->payload_crc;

    if (!validPacket) {
        SendAck(payload->game_slot,
            payload->epoch,
            payload->config_hash,
            0,
            0,
            0);
        Rollback::NetplayLog_Write("PALETTE", -1,
            "Rejected remote palette data: epoch=%u config=0x%08X slot=P%d size=%u crc=0x%08X",
            payload->epoch,
            payload->config_hash,
            payload->game_slot + 1,
            (unsigned)payload->payload_size,
            payload->payload_crc);
        return;
    }

    remote.remote_custom_bank.valid = true;
    remote.remote_custom_bank.character_id = payload->character_id;
    remote.remote_custom_bank.base_palette = payload->base_palette;
    remote.remote_custom_bank.crc32 = payload->payload_crc;
    memcpy(remote.remote_custom_bank.data, payload->payload, NETPLAY_PALETTE_BANK_SIZE);
    remote.remote_custom_loaded = true;

    LOG_INFO("[Palette] Remote netplay palette bank captured slot=P%d char=%u base=%u crc=0x%08X local_slot=P%d",
        payload->game_slot + 1,
        payload->character_id,
        payload->base_palette,
        payload->payload_crc,
        IsValidGameSlot(s_localGameSlot) ? (s_localGameSlot + 1) : 0);

    AdvanceStateRevision();
    SendAck(payload->game_slot,
        payload->epoch,
        payload->config_hash,
        1,
        1,
        payload->payload_crc);

    if (payload->game_slot != s_localGameSlot) {
        RequestLiveReload(payload->game_slot, "remote palette updated");
    }

    SetStatus("Custom palette ready for P%d", payload->game_slot + 1);
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Remote palette data accepted: epoch=%u config=0x%08X slot=P%d crc=0x%08X",
        payload->epoch,
        payload->config_hash,
        payload->game_slot + 1,
        payload->payload_crc);
}

void NetplayPaletteRuntime_OnRemoteAck(const PaletteAckPayload* payload) {
    if (!payload || !IsValidGameSlot(s_localGameSlot)) {
        return;
    }

    if (payload->config_hash != s_configHash ||
        payload->epoch != s_localEpoch ||
        payload->game_slot != (uint8_t)s_localGameSlot) {
        return;
    }

    const uint32_t expectedCrc = LocalTransportHasCustomBank()
        ? s_player[s_localGameSlot].local_custom_bank.crc32
        : 0;
    const bool expectedData = LocalTransportHasCustomBank();

    if (payload->accepted != 0 &&
        ((!expectedData && payload->received_data == 0 && payload->payload_crc == 0) ||
         (expectedData && payload->received_data != 0 && payload->payload_crc == expectedCrc))) {
        s_remoteAcknowledged = true;
        SetStatus("The other player confirmed the palette update");
        Rollback::NetplayLog_Write("PALETTE", -1,
            "Remote palette ack accepted: epoch=%u slot=P%d crc=0x%08X",
            payload->epoch,
            payload->game_slot + 1,
            payload->payload_crc);
    }
}

bool NetplayPaletteRuntime_CopyEditableLocalBank(NetplayPaletteBank* out) {
    const int editableSlot = GetEditableGameSlot();
    if (!out || !IsValidGameSlot(editableSlot)) {
        return false;
    }

    const PlayerRuntime& player = s_player[editableSlot];
    if (player.live_bank_loaded) {
        return CopyBank(player.live_bank, out);
    }
    if (player.local_custom_loaded) {
        return CopyBank(player.local_custom_bank, out);
    }
    return CopyBank(player.vanilla_bank, out);
}

bool NetplayPaletteRuntime_CopyLocalBankForSource(NetplayPaletteBankSource source, NetplayPaletteBank* out) {
    const int editableSlot = GetEditableGameSlot();
    if (!out || !IsValidGameSlot(editableSlot)) {
        return false;
    }

    const PlayerRuntime& player = s_player[editableSlot];
    switch (source) {
        case NetplayPaletteBankSource::LiveMemory:
            return CopyBank(player.live_bank, out);
        case NetplayPaletteBankSource::VanillaSource:
            return CopyBank(player.vanilla_bank, out);
        case NetplayPaletteBankSource::AppliedCustom:
            return CopyBank(player.local_custom_bank, out);
        case NetplayPaletteBankSource::SavedCustom:
            return CopyBank(player.saved_custom_bank, out);
        default:
            return false;
    }
}

bool NetplayPaletteRuntime_SetLocalCustomBank(const NetplayPaletteBank* bank, bool persistToDisk) {
    const int editableSlot = GetEditableGameSlot();
    if (!bank || !IsValidGameSlot(editableSlot)) {
        return false;
    }

    PlayerRuntime& player = s_player[editableSlot];
    const bool hadVisualOverride = HasVisualOverrideForGameSlot(editableSlot);
    const bool hadCatalogEntry = NetplayPaletteRuntime_HasLocalCustomBankFor(
        player.character_id,
        player.base_palette);
    NetplayPaletteBank updatedBank = *bank;
    updatedBank.valid = true;
    updatedBank.character_id = player.character_id;
    updatedBank.base_palette = player.base_palette;
    updatedBank.crc32 = CalcCRC32(updatedBank.data, NETPLAY_PALETTE_BANK_SIZE);

    const bool unchanged = player.local_custom_loaded &&
        player.local_custom_bank.character_id == updatedBank.character_id &&
        player.local_custom_bank.base_palette == updatedBank.base_palette &&
        player.local_custom_bank.crc32 == updatedBank.crc32 &&
        memcmp(player.local_custom_bank.data, updatedBank.data, NETPLAY_PALETTE_BANK_SIZE) == 0;

    if (!unchanged) {
        player.local_custom_bank = updatedBank;
        player.local_custom_loaded = true;
    }

    player.local_visual_override_enabled = true;

    bool saved = true;
    if (persistToDisk) {
        saved = NetplayPaletteStorage_SaveBank(&player.local_custom_bank);
        if (saved) {
            player.saved_custom_bank = player.local_custom_bank;
            player.saved_custom_loaded = true;
        }
    }

    const bool hasVisualOverride = HasVisualOverrideForGameSlot(editableSlot);
    const bool hasCatalogEntry = NetplayPaletteRuntime_HasLocalCustomBankFor(
        player.character_id,
        player.base_palette);
    const bool visualStateChanged = !unchanged || hadVisualOverride != hasVisualOverride;

    if (visualStateChanged) {
        MarkPaletteChanged((uint8_t)editableSlot,
            true,
            persistToDisk ? "local custom saved" : "local custom updated");
        RequestLiveReload((uint8_t)editableSlot, "local custom updated");
    }

    if (hadCatalogEntry != hasCatalogEntry) {
        CharSelPaletteSelect_OnLocalCatalogChanged();
    }

    if (persistToDisk) {
        SetStatus(saved
                ? "Saved custom palette for P%d"
                : "Failed to save custom palette for P%d",
            editableSlot + 1);
    } else {
        SetStatus("Updated live palette for P%d", editableSlot + 1);
    }

    return saved;
}

bool NetplayPaletteRuntime_ClearLocalCustomBank(bool deleteFromDisk) {
    const int editableSlot = GetEditableGameSlot();
    if (!IsValidGameSlot(editableSlot)) {
        return false;
    }

    PlayerRuntime& player = s_player[editableSlot];
    const bool hadVisualOverride = HasVisualOverrideForGameSlot(editableSlot);
    const bool hadCatalogEntry = NetplayPaletteRuntime_HasLocalCustomBankFor(
        player.character_id,
        player.base_palette);
    if (!player.local_custom_loaded) {
        if (!deleteFromDisk) {
            return true;
        }

        const bool deleted = !deleteFromDisk ||
            NetplayPaletteStorage_DeleteBank(player.character_id, player.base_palette);
        ZeroBank(&player.saved_custom_bank);
        player.saved_custom_loaded = false;
        const bool hasCatalogEntry = NetplayPaletteRuntime_HasLocalCustomBankFor(
            player.character_id,
            player.base_palette);
        if (hadCatalogEntry != hasCatalogEntry) {
            CharSelPaletteSelect_OnLocalCatalogChanged();
        }
        return deleted;
    }

    ZeroBank(&player.local_custom_bank);
    player.local_custom_loaded = false;
    player.local_visual_override_enabled = false;

    const bool deleted = !deleteFromDisk ||
        NetplayPaletteStorage_DeleteBank(player.character_id, player.base_palette);
    if (deleteFromDisk) {
        ZeroBank(&player.saved_custom_bank);
        player.saved_custom_loaded = false;
    }

    const bool hasVisualOverride = HasVisualOverrideForGameSlot(editableSlot);
    const bool hasCatalogEntry = NetplayPaletteRuntime_HasLocalCustomBankFor(
        player.character_id,
        player.base_palette);
    if (hadVisualOverride != hasVisualOverride) {
        MarkPaletteChanged((uint8_t)editableSlot,
            true,
            deleteFromDisk ? "local custom deleted" : "local custom cleared");
        RequestLiveReload((uint8_t)editableSlot, "local custom cleared");
    }
    if (hadCatalogEntry != hasCatalogEntry) {
        CharSelPaletteSelect_OnLocalCatalogChanged();
    }
    SetStatus("Cleared custom palette for P%d", editableSlot + 1);
    return deleted;
}

bool NetplayPaletteRuntime_SetOfflineEditorGameSlot(uint8_t gameSlot) {
    if (s_matchActive || !IsValidGameSlot(gameSlot) || !s_player[gameSlot].valid) {
        return false;
    }

    s_offlineEditorGameSlot = gameSlot;
    SetStatus("Offline palette editor switched to P%d", gameSlot + 1);
    return true;
}

void NetplayPaletteRuntime_GetLocalContext(NetplayPaletteLocalContext* out) {
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->match_active = s_matchActive;
    out->transport_enabled = s_enabled;
    out->remote_preview_enabled = s_remotePreviewEnabled;

    const int editableSlot = GetEditableGameSlot();
    if (IsValidGameSlot(editableSlot)) {
        const PlayerRuntime& player = s_player[editableSlot];
        out->available = player.valid;
        out->has_applied_custom_bank = HasCurrentLocalCustomBank(player);
        out->has_saved_custom_bank = HasCurrentSavedCustomBank(player);
        out->has_vanilla_bank = player.vanilla_bank.valid;
        out->has_live_bank = player.live_bank_loaded;
        out->asset_loaded = player.asset_loaded;
        out->game_slot = (uint8_t)editableSlot;
        out->character_id = player.character_id;
        out->base_palette = player.base_palette;
    }

    CopyText(out->status, sizeof(out->status), s_status);
}

bool NetplayPaletteRuntime_HasLocalCustomBankFor(uint8_t characterId, uint8_t basePalette) {
    if (NetplayPaletteStorage_HasBank(characterId, basePalette)) {
        return true;
    }

    for (int gameSlot = 0; gameSlot < 2; ++gameSlot) {
        const PlayerRuntime& player = s_player[gameSlot];
        if (!player.valid || !HasCurrentLocalCustomBank(player)) {
            continue;
        }

        if (player.local_custom_bank.character_id == characterId &&
            player.local_custom_bank.base_palette == basePalette) {
            return true;
        }
    }

    return false;
}

bool NetplayPaletteRuntime_CopyAssetOverrideBank(uint8_t gameSlot, NetplayPaletteBank* out) {
    if (!IsValidGameSlot(gameSlot) || !out) {
        return false;
    }

    if (!s_matchActive) {
        if (!ShouldUseVisualCustomBank(gameSlot)) {
            return false;
        }
        return CopyBank(s_player[gameSlot].local_custom_bank, out);
    }

    if (s_winscreenSuppressOverrides) {
        return false;
    }

    if (gameSlot == (uint8_t)s_localGameSlot) {
        if (!ShouldUseVisualCustomBank(gameSlot)) {
            return false;
        }
        return CopyBank(s_player[gameSlot].local_custom_bank, out);
    }

    if (!CopyRemoteMatchDisplayBank(gameSlot, out)) {
        return false;
    }

    return true;
}

bool NetplayPaletteRuntime_CopySpectatorBank(uint8_t gameSlot, NetplayPaletteBank* out) {
    if (!IsValidGameSlot(gameSlot) || !out) {
        return false;
    }

    if (!s_matchActive) {
        if (!ShouldUseVisualCustomBank(gameSlot)) {
            return false;
        }
        return CopyBank(s_player[gameSlot].local_custom_bank, out);
    }

    if (gameSlot == (uint8_t)s_localGameSlot) {
        if (!ShouldUseVisualCustomBank(gameSlot)) {
            return false;
        }
        return CopyBank(s_player[gameSlot].local_custom_bank, out);
    }

    if (!RemoteSelectionClaimsCustomBank(gameSlot)) {
        return false;
    }

    return CopyBank(s_player[gameSlot].remote_custom_bank, out);
}

void NetplayPaletteRuntime_OnAssetBankCaptured(uint8_t gameSlot,
                                               uint8_t basePalette,
                                               const void* data,
                                               size_t len,
                                               uint16_t assetCount,
                                               const char* archivePath,
                                               const char* patchPath) {
    if (!IsValidGameSlot(gameSlot) || !data || len < NETPLAY_PALETTE_BANK_SIZE) {
        return;
    }

    PlayerRuntime& player = s_player[gameSlot];
    const int derivedCharacterId = ResolveCharacterIdFromArchivePath(archivePath);
    const bool hadPlayerContext = player.valid;

    if (derivedCharacterId >= 0) {
        if (!s_matchActive) {
            MaybeArmOfflineLocalContext(gameSlot, (uint8_t)derivedCharacterId, basePalette);
        } else if (hadPlayerContext && player.character_id != (uint8_t)derivedCharacterId) {
            Rollback::NetplayLog_Write("PALETTE", -1,
                "Palette character mismatch: slot=P%d config_char=%u archive_char=%u archive=%s",
                gameSlot + 1,
                player.character_id,
                (unsigned)derivedCharacterId,
                archivePath ? archivePath : "(null)");
        }
    }

    player.valid = true;
    player.game_slot = gameSlot;
    if (derivedCharacterId >= 0 && (!s_matchActive || !hadPlayerContext)) {
        player.character_id = (uint8_t)derivedCharacterId;
    }
    player.asset_loaded = true;
    player.asset_count = assetCount;
    player.base_palette = basePalette;
    CopyText(player.archive_path, sizeof(player.archive_path), archivePath);
    CopyText(player.patch_path, sizeof(player.patch_path), patchPath);

    UpdateObservedBank(&player,
        &player.vanilla_bank,
        &player.asset_loaded,
        gameSlot,
        player.character_id,
        basePalette,
        data,
        len,
        assetCount,
        archivePath,
        patchPath,
        "Captured vanilla source palette",
        "Captured the original palette");
    LOG_INFO("[Palette] Vanilla palette bank captured slot=P%d char=%u base=%u crc=0x%08X assets=%u",
        gameSlot + 1,
        player.character_id,
        basePalette,
        player.vanilla_bank.crc32,
        (unsigned)assetCount);
    LOG_INFO("[Palette] Captured source bank slot=P%d char=%u base=%u archive=%s patch=%s assets=%u",
        gameSlot + 1,
        player.character_id,
        basePalette,
        archivePath ? archivePath : "(null)",
        patchPath ? patchPath : "(null)",
        (unsigned)assetCount);
}

void NetplayPaletteRuntime_OnLiveBankObserved(uint8_t gameSlot,
                                             uint8_t basePalette,
                                             const void* data,
                                             size_t len,
                                             uint16_t assetCount,
                                             const char* archivePath,
                                             const char* patchPath) {
    if (!IsValidGameSlot(gameSlot) || !data || len < NETPLAY_PALETTE_BANK_SIZE) {
        return;
    }

    PlayerRuntime& player = s_player[gameSlot];
    const int derivedCharacterId = ResolveCharacterIdFromArchivePath(archivePath);
    const bool hadPlayerContext = player.valid;

    if (derivedCharacterId >= 0) {
        if (!s_matchActive) {
            MaybeArmOfflineLocalContext(gameSlot, (uint8_t)derivedCharacterId, basePalette);
        } else if (hadPlayerContext && player.character_id != (uint8_t)derivedCharacterId) {
            Rollback::NetplayLog_Write("PALETTE", -1,
                "Live palette character mismatch: slot=P%d config_char=%u archive_char=%u archive=%s",
                gameSlot + 1,
                player.character_id,
                (unsigned)derivedCharacterId,
                archivePath ? archivePath : "(null)");
        }
    }

    if (derivedCharacterId >= 0 && (!s_matchActive || !hadPlayerContext)) {
        player.character_id = (uint8_t)derivedCharacterId;
    }

    UpdateObservedBank(&player,
        &player.live_bank,
        &player.live_bank_loaded,
        gameSlot,
        player.character_id,
        basePalette,
        data,
        len,
        assetCount,
        archivePath,
        patchPath,
        "Observed live decoded palette",
        "Read the live palette");
    LOG_INFO("[Palette] Observed live bank slot=P%d char=%u base=%u archive=%s patch=%s assets=%u",
        gameSlot + 1,
        player.character_id,
        basePalette,
        archivePath ? archivePath : "(null)",
        patchPath ? patchPath : "(null)",
        (unsigned)assetCount);
}

void NetplayPaletteRuntime_RequestFrontendReload(uint8_t gameSlot) {
    if (!s_initialized || s_matchActive || !IsValidGameSlot(gameSlot)) {
        return;
    }

    if (GetGameMode() != MODE_CHARSEL) {
        return;
    }

    const int localPreviewSlot = GetFrontendPreviewLocalSlot();
    if (IsValidGameSlot(localPreviewSlot) && gameSlot != localPreviewSlot) {
        return;
    }

    if (!s_player[gameSlot].valid || !s_player[gameSlot].asset_loaded) {
        return;
    }

    RequestLiveReload(gameSlot, "charsel preview change");
}

bool NetplayPaletteRuntime_ConsumeLiveReloadRequest(NetplayPaletteReloadRequest* out) {
    if (!out) {
        return false;
    }

    for (int slot = 0; slot < 2; slot++) {
        if (!s_liveReloadRequested[slot]) {
            continue;
        }
        s_liveReloadRequested[slot] = false;
        out->game_slot = (uint8_t)slot;
        return true;
    }
    return false;
}

void NetplayPaletteRuntime_OnLiveReloadComplete(uint8_t gameSlot, bool success, const char* reason) {
    if (!IsValidGameSlot(gameSlot)) {
        return;
    }

    LOG_INFO("[Palette] Live reload %s slot=P%d reason=%s",
        success ? "complete" : "failed",
        gameSlot + 1,
        reason ? reason : "unspecified");
    SetStatus(success
            ? "Updated the match palette for P%d"
            : "Couldn't update the match palette for P%d",
        gameSlot + 1);
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Live reload %s: slot=P%d reason=%s",
        success ? "complete" : "failed",
        gameSlot + 1,
        reason ? reason : "unspecified");
}

void NetplayPaletteRuntime_GetSnapshot(NetplayPaletteRuntimeSnapshot* out) {
    if (!out) {
        return;
    }

    memset(out, 0, sizeof(*out));
    out->initialized = s_initialized;
    out->enabled = s_enabled;
    out->remote_preview_enabled = s_remotePreviewEnabled;
    out->match_active = s_matchActive;
    out->epoch = s_stateRevision;
    out->config_hash = s_configHash;
    out->local_sent = s_localSent;
    out->remote_acknowledged = s_remoteAcknowledged;

    for (int slot = 0; slot < 2; slot++) {
        NetplayPalettePlayerState& dst = out->player[slot];
        const PlayerRuntime& src = s_player[slot];
        dst.valid = src.valid;
        dst.game_slot = (uint8_t)slot;
        dst.character_id = src.character_id;
        dst.base_palette = src.base_palette;
        dst.flags = s_matchActive
            ? ((slot == s_localGameSlot) ? BuildLocalFlags() : src.remote_flags)
            : 0;
        if (!s_matchActive) {
            dst.has_custom_bank = src.local_custom_loaded;
            dst.custom_bank_ready = src.local_custom_loaded;
            dst.payload_crc = src.local_custom_loaded ? src.local_custom_bank.crc32 : 0;
            dst.payload_size = src.local_custom_loaded ? NETPLAY_PALETTE_BANK_SIZE : 0;
        } else if (slot == s_localGameSlot) {
            dst.has_custom_bank = src.local_custom_loaded;
            dst.custom_bank_ready = src.local_custom_loaded;
            dst.payload_crc = src.local_custom_loaded ? src.local_custom_bank.crc32 : 0;
            dst.payload_size = src.local_custom_loaded ? NETPLAY_PALETTE_BANK_SIZE : 0;
        } else {
            dst.has_custom_bank = src.remote_custom_loaded;
            dst.custom_bank_ready = src.remote_custom_loaded;
            dst.payload_crc = src.remote_custom_loaded ? src.remote_custom_bank.crc32 : 0;
            dst.payload_size = src.remote_custom_loaded ? NETPLAY_PALETTE_BANK_SIZE : 0;
        }
        dst.vanilla_bank_ready = src.vanilla_bank.valid;
        dst.live_bank_ready = src.live_bank_loaded;
        dst.asset_loaded = src.asset_loaded;
        dst.asset_count = src.asset_count;
    }

    CopyText(out->status, sizeof(out->status), s_status);
}

} // namespace Net
