/**
 * Alice Senki 2 - Test Harness Launcher
 *
 * Standalone Win32 application that:
 *   1. Writes two as2_autoconnect.cfg files (host + client)
 *   2. Launches two game instances
 *   3. Opens a D3D9+ImGui dashboard reading shared memory from both
 *   4. Shows real-time profiling, entity state, rollback stats, anomalies
 *   5. Collects and summarizes file-based logs on exit
 *
 * Build: Part of AS2 CMake project (as2_test_harness target)
 * Deploy: Copy to game directory and run.
 *
 * Usage:
 *   as2_test_harness.exe [options]
 *     --game-dir <path>     Game directory (default: current dir)
 *     --host-port <n>       Host listen port (default: 7500)
 *     --client-port <n>     Client listen port (default: 7501)
 *     --host-char <n>       Host character grid index 0-20 (default: 0)
 *     --client-char <n>     Client character grid index 0-20 (default: 1)
 *     --delay <n>           Input delay frames (default: 2)
 *     --duration <sec>      Match auto-exit duration (default: 120)
 *     --latency <ms>        Simulated one-way latency (default: 0)
 *     --jitter <ms>         Jitter +/- (default: 0)
 *     --loss <pct>          Packet loss % (default: 0)
 *     --dup <pct>           Packet duplicate % (default: 0)
 *     --no-launch           Write config files but don't launch games
 *     --no-gui              Console-only mode (no ImGui window)
 *     --help                Show help
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellscalingapi.h>  // SetProcessDpiAwareness
#include <d3d9.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <direct.h>

#pragma comment(lib, "shcore.lib")

// ImGui
#include "imgui.h"
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"

// Shared memory layout (same header used by the mod DLL writer)
#include "harness_shared_memory.h"

// NOTE: We do NOT link d3d9.lib statically.  The harness exe lives in the game
// directory next to our d3d9 proxy DLL.  If we let the loader resolve d3d9.dll
// normally it picks up our proxy, which calls ModInit and crashes.  Instead we
// load the real system d3d9.dll from System32 at runtime.
// #pragma comment(lib, "d3d9.lib")   // intentionally removed

typedef IDirect3D9* (WINAPI *PFN_Direct3DCreate9)(UINT SDKVersion);
static HMODULE             g_hRealD3D9 = nullptr;
static PFN_Direct3DCreate9 g_pfnDirect3DCreate9 = nullptr;

static bool LoadSystemD3D9() {
    char sys32[MAX_PATH];
    GetSystemDirectoryA(sys32, MAX_PATH);
    char path[MAX_PATH];
    _snprintf_s(path, sizeof(path), _TRUNCATE, "%s\\d3d9.dll", sys32);
    g_hRealD3D9 = LoadLibraryA(path);
    if (!g_hRealD3D9) {
        printf("ERROR: Failed to load system d3d9.dll from %s (err %lu)\n", path, GetLastError());
        return false;
    }
    g_pfnDirect3DCreate9 = (PFN_Direct3DCreate9)GetProcAddress(g_hRealD3D9, "Direct3DCreate9");
    if (!g_pfnDirect3DCreate9) {
        printf("ERROR: Direct3DCreate9 not found in system d3d9.dll\n");
        FreeLibrary(g_hRealD3D9); g_hRealD3D9 = nullptr;
        return false;
    }
    return true;
}

// ============================================================================
// Forward-declare Win32 ImGui handler
// ============================================================================

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// ============================================================================
// Configuration
// ============================================================================

struct LauncherConfig {
    char  gameDir[MAX_PATH];
    char  gameExe[MAX_PATH];
    int   hostPort;
    int   clientPort;
    int   hostChar;
    int   clientChar;
    int   hostPalette;
    int   clientPalette;
    int   delayFrames;
    int   durationSec;
    int   latencyMs;
    int   jitterMs;
    float lossPct;
    float dupPct;
    float timesyncThreshold;
    bool  stageMashTest;
    int   stageMashFrames;
    bool  noLaunch;
    bool  noGui;
};

static void SetDefaults(LauncherConfig* cfg) {
    memset(cfg, 0, sizeof(*cfg));
    GetCurrentDirectoryA(MAX_PATH, cfg->gameDir);
    cfg->hostPort     = 7500;
    cfg->clientPort   = 7501;
    cfg->hostChar     = 0;
    cfg->clientChar   = 1;
    cfg->delayFrames  = 0;
    cfg->durationSec  = 600;
    cfg->stageMashTest = false;
    cfg->stageMashFrames = 180;
}

static void PrintUsage() {
    printf("AS2 Test Harness Launcher\n");
    printf("=========================\n\n");
    printf("Launches two game instances with auto-connect configuration.\n\n");
    printf("Options:\n");
    printf("  --game-dir <path>    Game directory (default: current dir)\n");
    printf("  --host-port <n>      Host port (default: 7500)\n");
    printf("  --client-port <n>    Client port (default: 7501)\n");
    printf("  --host-char <n>      Host character 0-20 (default: 0)\n");
    printf("  --client-char <n>    Client character 0-20 (default: 1)\n");
    printf("  --delay <n>          Input delay frames 0-6 (default: 0)\n");
    printf("  --duration <sec>     Match duration (default: 120)\n");
    printf("  --latency <ms>       One-way latency (default: 0)\n");
    printf("  --jitter <ms>        Jitter +/- (default: 0)\n");
    printf("  --loss <pct>         Packet loss %% (default: 0)\n");
    printf("  --dup <pct>          Duplicate %% (default: 0)\n");
    printf("  --timesync <float>   Timesync threshold (default: 0.5)\n");
    printf("  --stage-mash         Mash opposite directionals during stage preview\n");
    printf("  --stage-mash-frames <n>  Preview frames to mash before confirming (default: 180)\n");
    printf("  --no-launch          Write configs only\n");
    printf("  --no-gui             Console-only mode (skip ImGui window)\n");
    printf("  --help               Show this message\n");
}

static bool ParseArgs(int argc, char** argv, LauncherConfig* cfg) {
    for (int i = 1; i < argc; i++) {
        const char* arg = argv[i];
        if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) { PrintUsage(); return false; }
        else if (!strcmp(arg, "--game-dir") && i+1<argc)    strncpy_s(cfg->gameDir, MAX_PATH, argv[++i], _TRUNCATE);
        else if (!strcmp(arg, "--host-port") && i+1<argc)   cfg->hostPort = atoi(argv[++i]);
        else if (!strcmp(arg, "--client-port") && i+1<argc) cfg->clientPort = atoi(argv[++i]);
        else if (!strcmp(arg, "--host-char") && i+1<argc)   cfg->hostChar = atoi(argv[++i]);
        else if (!strcmp(arg, "--client-char") && i+1<argc) cfg->clientChar = atoi(argv[++i]);
        else if (!strcmp(arg, "--delay") && i+1<argc)       cfg->delayFrames = atoi(argv[++i]);
        else if (!strcmp(arg, "--duration") && i+1<argc)    cfg->durationSec = atoi(argv[++i]);
        else if (!strcmp(arg, "--latency") && i+1<argc)     cfg->latencyMs = atoi(argv[++i]);
        else if (!strcmp(arg, "--jitter") && i+1<argc)      cfg->jitterMs = atoi(argv[++i]);
        else if (!strcmp(arg, "--loss") && i+1<argc)        cfg->lossPct = (float)atof(argv[++i]);
        else if (!strcmp(arg, "--dup") && i+1<argc)         cfg->dupPct = (float)atof(argv[++i]);
        else if (!strcmp(arg, "--timesync") && i+1<argc)    cfg->timesyncThreshold = (float)atof(argv[++i]);
        else if (!strcmp(arg, "--stage-mash"))               cfg->stageMashTest = true;
        else if (!strcmp(arg, "--stage-mash-frames") && i+1<argc) cfg->stageMashFrames = atoi(argv[++i]);
        else if (!strcmp(arg, "--no-launch"))                cfg->noLaunch = true;
        else if (!strcmp(arg, "--no-gui"))                   cfg->noGui = true;
        else { printf("Unknown option: %s\n", arg); return false; }
    }
    return true;
}

// ============================================================================
// Config file writer
// ============================================================================

static bool WriteConfigFile(const char* path, bool isHost, const LauncherConfig* cfg) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "w") != 0 || !f) {
        printf("ERROR: Failed to write %s\n", path);
        return false;
    }
    const char* role = isHost ? "host" : "client";
    const char* nick = isHost ? "TestHost" : "TestClient";
    int localPort  = isHost ? cfg->hostPort  : cfg->clientPort;
    int remotePort = isHost ? cfg->clientPort : cfg->hostPort;
    int charId     = isHost ? cfg->hostChar   : cfg->clientChar;
    int palette    = isHost ? cfg->hostPalette : cfg->clientPalette;
    const char* logFile = isHost ? "harness_host.log" : "harness_client.log";

    fprintf(f, "# AS2 Test Harness Auto-Connect Config\n\n");
    fprintf(f, "[autoconnect]\n");
    fprintf(f, "enabled=1\nrole=%s\nnickname=%s\nport=%d\n", role, nick, localPort);
    fprintf(f, "target_ip=127.0.0.1\ntarget_port=%d\n", remotePort);
    fprintf(f, "character_id=%d\npalette=%d\ndelay_frames=%d\n", charId, palette, cfg->delayFrames);
    fprintf(f, "match_duration_sec=%d\n\n", cfg->durationSec);
        fprintf(f, "stage_mash_test=%d\nstage_mash_frames=%d\n\n",
            cfg->stageMashTest ? 1 : 0,
            cfg->stageMashFrames);
    fprintf(f, "[network_sim]\n");
    fprintf(f, "latency_ms=%d\njitter_ms=%d\n", cfg->latencyMs, cfg->jitterMs);
    fprintf(f, "packet_loss_pct=%.1f\nduplicate_pct=%.1f\n", cfg->lossPct, cfg->dupPct);
    if (cfg->timesyncThreshold > 0.0f)
        fprintf(f, "timesync_threshold=%.1f\n", cfg->timesyncThreshold);
    fprintf(f, "\n");
    fprintf(f, "[profiling]\n");
    fprintf(f, "enabled=1\nlog_file=%s\n", logFile);
    fprintf(f, "log_inputs=1\nlog_checksums=1\nlog_rollbacks=1\n");
    fprintf(f, "log_frame_timing=1\nlog_rtt=1\n");
        fprintf(f, "log_game_state=1\nlog_entity_detail=1\nlog_rng=1\nlog_vanilla_flags=1\nlog_stage_debug=%d\n",
            cfg->stageMashTest ? 1 : 0);
    fclose(f);
    printf("  Wrote: %s (%s)\n", path, role);
    return true;
}

// ============================================================================
// Process launcher
// ============================================================================

static HANDLE LaunchGame(const char* workDir, const char* exePath, const char* label) {
    STARTUPINFOA si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_SHOW;
    PROCESS_INFORMATION pi = {};
    char cmdLine[MAX_PATH + 64];
    _snprintf_s(cmdLine, sizeof(cmdLine), _TRUNCATE, "\"%s\"", exePath);
    if (!CreateProcessA(nullptr, cmdLine, nullptr, nullptr, FALSE, 0, nullptr, workDir, &si, &pi)) {
        printf("  ERROR: Failed to launch %s (error %lu)\n", label, GetLastError());
        return INVALID_HANDLE_VALUE;
    }
    printf("  Launched %s (PID %lu)\n", label, pi.dwProcessId);
    CloseHandle(pi.hThread);
    return pi.hProcess;
}

// ============================================================================
// Find game executable
// ============================================================================

static bool FindGameExe(const char* dir, char* outPath, int outLen) {
    const char* candidates[] = { "as2.exe", "AliceSenki2.exe", "alice_senki_2.exe" };
    for (const char* name : candidates) {
        char p[MAX_PATH];
        _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", dir, name);
        if (GetFileAttributesA(p) != INVALID_FILE_ATTRIBUTES) {
            strncpy_s(outPath, outLen, p, _TRUNCATE);
            printf("  Found game exe: %s\n", name);
            return true;
        }
    }
    // Fallback: search for any .exe
    char searchPath[MAX_PATH];
    _snprintf_s(searchPath, sizeof(searchPath), _TRUNCATE, "%s\\*.exe", dir);
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA(searchPath, &fd);
    if (hFind == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (_stricmp(fd.cFileName, "as2_test_harness.exe") == 0) continue;
        _snprintf_s(outPath, outLen, _TRUNCATE, "%s\\%s", dir, fd.cFileName);
        printf("  Found game exe: %s\n", fd.cFileName);
        found = true; break;
    } while (FindNextFileA(hFind, &fd));
    FindClose(hFind);
    return found;
}

// ============================================================================
// Log file analysis (console output)
// ============================================================================

static void AnalyzeLog(const char* logPath, const char* label) {
    FILE* f = nullptr;
    if (fopen_s(&f, logPath, "r") != 0 || !f) {
        printf("  (%s log not found: %s)\n", label, logPath);
        return;
    }
    int total = 0, rbCount = 0, maxRbDepth = 0, desyncs = 0, timings = 0, stageLogs = 0, rbInputs = 0, rbChecks = 0;
    float maxRtt = 0, sumRtt = 0; int rttN = 0;
    long sumSv = 0, sumLd = 0, sumAdv = 0;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        if (line[0] == '#') continue;
        if (!strncmp(line, "INPUT", 5))    total++;
        else if (!strncmp(line, "RBINPUT", 7)) rbInputs++;
        else if (!strncmp(line, "RBCHECK", 7)) rbChecks++;
        else if (!strncmp(line, "STAGE", 5)) stageLogs++;
        else if (!strncmp(line, "ROLLBACK", 8)) {
            rbCount++;
            const char* dp = strstr(line, "depth="); if (dp) { int d = atoi(dp+6); if (d > maxRbDepth) maxRbDepth = d; }
        } else if (!strncmp(line, "RTT", 3)) {
            const char* rp = strrchr(line, '|'); if (rp) { float r = (float)atof(rp+2); sumRtt += r; rttN++; if (r > maxRtt) maxRtt = r; }
        } else if (!strncmp(line, "TIMING", 6)) {
            timings++;
            const char *sp=strstr(line,"save="), *lp=strstr(line,"load="), *ap=strstr(line,"advance=");
            if (sp) sumSv  += atoi(sp+5);
            if (lp) sumLd  += atoi(lp+5);
            if (ap) sumAdv += atoi(ap+8);
        } else if (!strncmp(line, "DESYNC", 6)) desyncs++;
    }
    fclose(f);
    printf("\n--- %s Results ---\n", label);
        printf("  Frames: %d  Rollbacks: %d (max depth %d)  Desyncs: %d  Stage logs: %d\n",
            total, rbCount, maxRbDepth, desyncs, stageLogs);
        printf("  Rollback inputs: %d  Rollback checksums: %d\n", rbInputs, rbChecks);
    if (rttN) printf("  RTT avg/max: %.1f / %.1f ms\n", sumRtt/rttN, maxRtt);
    if (timings) printf("  Avg save/load/adv: %ld/%ld/%ld us\n", sumSv/timings, sumLd/timings, sumAdv/timings);
}

static bool FindNewestHarnessLog(const char* logsRoot, const char* fileName,
                                 char* outPath, size_t outPathLen) {
    WIN32_FIND_DATAA dirFd;
    char searchPath[MAX_PATH];
    _snprintf_s(searchPath, sizeof(searchPath), _TRUNCATE, "%s\\*", logsRoot);

    HANDLE hFind = FindFirstFileA(searchPath, &dirFd);
    if (hFind == INVALID_HANDLE_VALUE) return false;

    bool found = false;
    FILETIME newestTime = {};

    do {
        if (!(dirFd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || dirFd.cFileName[0] == '.')
            continue;

        char candidate[MAX_PATH];
        _snprintf_s(candidate, sizeof(candidate), _TRUNCATE,
                    "%s\\%s\\%s", logsRoot, dirFd.cFileName, fileName);

        WIN32_FIND_DATAA logFd;
        HANDLE hLog = FindFirstFileA(candidate, &logFd);
        if (hLog == INVALID_HANDLE_VALUE)
            continue;

        if (!found || CompareFileTime(&logFd.ftLastWriteTime, &newestTime) > 0) {
            strncpy_s(outPath, outPathLen, candidate, _TRUNCATE);
            newestTime = logFd.ftLastWriteTime;
            found = true;
        }
        FindClose(hLog);
    } while (FindNextFileA(hFind, &dirFd));

    FindClose(hFind);
    return found;
}

// ============================================================================
// Shared Memory Reader
// ============================================================================

struct ShmSlot {
    HANDLE              handle;
    HarnessSharedData*  data;
    bool                valid;
    uint32_t            lastSeq;
    uint32_t            lastLogHead;
};

struct HarnessRunLogs {
    char hostLog[MAX_PATH];
    char clientLog[MAX_PATH];
};

static void CaptureHarnessLogPath(HarnessRunLogs* runLogs, const HarnessSharedData* data) {
    if (!runLogs || !data || !data->session_log_dir[0]) return;

    char* outPath = data->is_host ? runLogs->hostLog : runLogs->clientLog;
    if (outPath[0]) return;

    const char* fileName = data->is_host ? "harness_host.log" : "harness_client.log";
    _snprintf_s(outPath, MAX_PATH, _TRUNCATE, "%s\\%s", data->session_log_dir, fileName);
}

static bool OpenShmSlot(ShmSlot* slot, const char* name) {
    slot->handle = OpenFileMappingA(FILE_MAP_READ, FALSE, name);
    if (!slot->handle) { slot->valid = false; return false; }
    slot->data = (HarnessSharedData*)MapViewOfFile(slot->handle, FILE_MAP_READ, 0, 0, sizeof(HarnessSharedData));
    if (!slot->data) { CloseHandle(slot->handle); slot->handle = nullptr; slot->valid = false; return false; }
    slot->valid = (slot->data->magic == HARNESS_SHM_MAGIC && slot->data->version == HARNESS_SHM_VERSION);
    slot->lastSeq = 0;
    slot->lastLogHead = 0;
    return slot->valid;
}

static void CloseShmSlot(ShmSlot* slot) {
    if (slot->data) { UnmapViewOfFile(slot->data); slot->data = nullptr; }
    if (slot->handle) { CloseHandle(slot->handle); slot->handle = nullptr; }
    slot->valid = false;
}

// ============================================================================
// D3D9 Device
// ============================================================================

static LPDIRECT3D9        g_pD3D = nullptr;
static LPDIRECT3DDEVICE9  g_pd3dDevice = nullptr;
static D3DPRESENT_PARAMETERS g_d3dpp = {};
static bool               g_deviceLost = false;

// Live net-sim controls (editable via ImGui Config tab)
static int   g_liveLatencyMs = 0;
static int   g_liveJitterMs  = 0;
static float g_liveLossPct   = 0.0f;
static float g_liveDupPct    = 0.0f;

static bool CreateDeviceD3D(HWND hWnd) {
    if (!g_pfnDirect3DCreate9) return false;
    g_pD3D = g_pfnDirect3DCreate9(D3D_SDK_VERSION);
    if (!g_pD3D) return false;
    memset(&g_d3dpp, 0, sizeof(g_d3dpp));
    g_d3dpp.Windowed               = TRUE;
    g_d3dpp.SwapEffect             = D3DSWAPEFFECT_DISCARD;
    g_d3dpp.BackBufferFormat       = D3DFMT_UNKNOWN;
    g_d3dpp.EnableAutoDepthStencil = TRUE;
    g_d3dpp.AutoDepthStencilFormat = D3DFMT_D16;
    g_d3dpp.PresentationInterval   = D3DPRESENT_INTERVAL_ONE; // vsync
    if (g_pD3D->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
            D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_d3dpp, &g_pd3dDevice) < 0)
        return false;
    return true;
}

static void CleanupDeviceD3D() {
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }
    if (g_pD3D)       { g_pD3D->Release();       g_pD3D = nullptr;       }
    if (g_hRealD3D9)  { FreeLibrary(g_hRealD3D9); g_hRealD3D9 = nullptr; }
}

static void ResetDevice() {
    ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_pd3dDevice->Reset(&g_d3dpp);
    if (hr == D3DERR_INVALIDCALL) return;
    ImGui_ImplDX9_CreateDeviceObjects();
}

// ============================================================================
// Win32 Window
// ============================================================================

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam == SIZE_MINIMIZED) return 0;
        g_d3dpp.BackBufferWidth  = (UINT)LOWORD(lParam);
        g_d3dpp.BackBufferHeight = (UINT)HIWORD(lParam);
        g_deviceLost = true;
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

// ============================================================================
// ImGui Rendering Helpers
// ============================================================================

static const char* GameModeName(uint32_t m) {
    switch (m) {
    case 0: return "Boot"; case 2: return "Title"; case 3: return "Menu";
    case 4: return "Lobby"; case 5: return "VsSel"; case 6: return "CharSel";
    case 7: return "StageSel"; case 8: return "Match"; case 9: return "Story";
    default: return "?";
    }
}

static void DrawHpBar(const char* label, int16_t hp, int16_t maxHp,
                       float r, float g, float b) {
    float frac = maxHp > 0 ? (float)hp / (float)maxHp : 0;
    if (frac < 0) frac = 0; if (frac > 1) frac = 1;
    char overlay[64];
    _snprintf_s(overlay, sizeof(overlay), _TRUNCATE, "%s: %d/%d", label, hp, maxHp);
    ImGui::PushStyleColor(ImGuiCol_PlotHistogram, ImVec4(r, g, b, 0.8f));
    ImGui::ProgressBar(frac, ImVec2(-1, 0), overlay);
    ImGui::PopStyleColor();
}

static void DrawSlotPanel(const char* title, ShmSlot* slot,
                          char logBuf[][HARNESS_LOG_LINE_LEN], int* logCount) {
    ImGui::BeginChild(title, ImVec2(0, 0), ImGuiChildFlags_Borders);

    if (!slot->valid || !slot->data || !slot->data->active) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.2f, 1), "%s: Waiting for game...", title);
        ImGui::EndChild();
        return;
    }

    const HarnessSharedData* d = slot->data;

    // Header
    ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.4f, 1.0f), "%s", title);
    ImGui::SameLine();
    ImGui::Text("[%s]  Phase: %s  Frame: %u",
                d->nickname, d->phase_name, d->frame_counter);

    ImGui::Separator();

    // Connection
    if (ImGui::CollapsingHeader("Connection##conn", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Peer: %s  RTT: %.1f ms (avg %.1f / peak %.1f)",
                    d->peer_nickname, d->conn_rtt_ms, d->avg_rtt_ms, d->peak_rtt_ms);
        ImGui::Text("Packets: sent=%u recv=%u  Desyncs: %u",
                    d->packets_sent, d->packets_received, d->desync_count);
        if (d->status_text[0])
            ImGui::TextWrapped("Status: %s", d->status_text);
        if (d->error_text[0])
            ImGui::TextColored(ImVec4(1,0.3f,0.3f,1), "Error: %s", d->error_text);
    }

    // Game State
    if (ImGui::CollapsingHeader("Game State##gs", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Mode: %u (%s)  Sub: %u  Stage: %u  Type: %u",
                    d->game_mode, GameModeName(d->game_mode), d->sub_state, d->stage_id, d->game_type);
        ImGui::Text("Sim: %u  Disp: %u  Write: %u  Net: %u",
                    d->sim_frame, d->display_frame, d->write_frame, d->net_frame);
        ImGui::Text("RNG: 0x%08X  Checksum: 0x%04X", d->rng_seed, d->checksum);

        if (d->vanilla_role || d->vanilla_connected)
            ImGui::TextColored(ImVec4(1,0,0,1), "!! VANILLA FLAGS: role=%u connected=%u !!",
                              d->vanilla_role, d->vanilla_connected);
        else
            ImGui::TextColored(ImVec4(0.3f,0.8f,0.3f,1), "Vanilla flags: clean");
    }

    // Entity
    if (ImGui::CollapsingHeader("Entity State##ent", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawHpBar("P1", d->p1_hp, 10000, 0.27f, 0.53f, 1.0f);
        ImGui::Text("  Meter: %u  Pos: (%d,%d)  Act: %u  Input: 0x%04X",
                    d->p1_meter, d->p1_x, d->p1_y, d->p1_action, d->p1_input);
        DrawHpBar("P2", d->p2_hp, 10000, 1.0f, 0.27f, 0.27f);
        ImGui::Text("  Meter: %u  Pos: (%d,%d)  Act: %u  Input: 0x%04X",
                    d->p2_meter, d->p2_x, d->p2_y, d->p2_action, d->p2_input);
    }

    // Profiling
    if (ImGui::CollapsingHeader("Profiling##prof", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Frames: %u  Rollbacks: %u (max %u)",
                    d->total_frames, d->total_rollbacks, d->max_rollback_depth);
        ImGui::Text("Saves: %u  Loads: %u", d->total_saves, d->total_loads);
        ImGui::Text("Adapter: tx=%u rx=%u  Timesync skips: %u",
                    d->rb_adapter_send, d->rb_adapter_recv, d->rb_timesync_skips);
        ImGui::Separator();
        ImGui::Text("Save:    avg %.0f us  peak %.0f us", d->avg_save_us, d->peak_save_us);
        ImGui::Text("Load:    avg %.0f us  peak %.0f us", d->avg_load_us, d->peak_load_us);
        ImGui::Text("Advance: avg %.0f us  peak %.0f us", d->avg_advance_us, d->peak_advance_us);
        ImGui::Separator();
        ImGui::TextColored(
            d->desync_warnings ? ImVec4(1,0.3f,0.3f,1) : ImVec4(0.3f,0.8f,0.3f,1),
            "Desyncs: %u", d->desync_warnings);
        ImGui::TextColored(
            d->vanilla_flag_violations ? ImVec4(1,0.3f,0.3f,1) : ImVec4(0.3f,0.8f,0.3f,1),
            "Vanilla violations: %u", d->vanilla_flag_violations);
        ImGui::TextColored(
            d->frame_counter_anomalies ? ImVec4(1,0.6f,0.1f,1) : ImVec4(0.3f,0.8f,0.3f,1),
            "Frame anomalies: %u", d->frame_counter_anomalies);
        ImGui::Text("RNG jumps: %u", d->rng_jumps);
    }

    // Network Sim
    if (ImGui::CollapsingHeader("Net Sim##ns")) {
        ImGui::Text("Latency: %d ms  Jitter: %d ms  Loss: %.1f%%  Dup: %.1f%%",
                    d->sim_latency_ms, d->sim_jitter_ms, d->sim_loss_pct, d->sim_dup_pct);
    }

    if (ImGui::CollapsingHeader("Rollback Trace##trace", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::TextWrapped("Trace log: %s", d->rollback_trace_log[0] ? d->rollback_trace_log : "<not available>");
        ImGui::TextWrapped("Trace bin: %s", d->rollback_trace_bin[0] ? d->rollback_trace_bin : "<not available>");
        ImGui::TextWrapped("Session dir: %s", d->session_log_dir[0] ? d->session_log_dir : "<not available>");
    }

    // Log tail
    if (ImGui::CollapsingHeader("Log##log")) {
        ImGui::BeginChild("LogScroll##ls", ImVec2(0, 150), ImGuiChildFlags_Borders);
        // Show last N lines from ring buffer
        uint32_t head = d->log_head;
        uint32_t count = d->log_count;
        int show = (count < HARNESS_LOG_LINES) ? (int)count : HARNESS_LOG_LINES;
        for (int i = show - 1; i >= 0; i--) {
            uint32_t idx = (head - 1 - i) % HARNESS_LOG_LINES;
            if (d->log_lines[idx][0])
                ImGui::TextUnformatted(d->log_lines[idx]);
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY())
            ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }

    ImGui::EndChild();
}

// Comparison panel: detect cross-instance desyncs
static void DrawComparisonPanel(ShmSlot* host, ShmSlot* client) {
    if (!host->valid || !host->data || !host->data->active ||
        !client->valid || !client->data || !client->data->active) {
        ImGui::TextDisabled("Comparison requires both instances active");
        return;
    }
    const HarnessSharedData* h = host->data;
    const HarnessSharedData* c = client->data;

    ImGui::Text("Frame delta: Host=%u Client=%u (diff=%d)",
                h->frame_counter, c->frame_counter,
                (int)h->frame_counter - (int)c->frame_counter);

    // Checksum comparison (only meaningful when sim frames match)
    if (h->sim_frame == c->sim_frame && h->sim_frame > 0) {
        if (h->checksum == c->checksum) {
            ImGui::TextColored(ImVec4(0.3f,0.8f,0.3f,1),
                "Sim frame %u: checksums MATCH (0x%04X)", h->sim_frame, h->checksum);
        } else {
            ImGui::TextColored(ImVec4(1,0,0,1),
                "!! DESYNC at sim frame %u: Host=0x%04X Client=0x%04X !!",
                h->sim_frame, h->checksum, c->checksum);
        }
    } else {
        ImGui::TextDisabled("Sim frames differ: Host=%u Client=%u", h->sim_frame, c->sim_frame);
    }

    // RNG comparison
    if (h->rng_seed == c->rng_seed)
        ImGui::TextColored(ImVec4(0.3f,0.8f,0.3f,1), "RNG: synced (0x%08X)", h->rng_seed);
    else
        ImGui::TextColored(ImVec4(1,0.5f,0,1), "RNG: Host=0x%08X Client=0x%08X", h->rng_seed, c->rng_seed);

    // HP comparison
    bool hpMatch = (h->p1_hp == c->p1_hp && h->p2_hp == c->p2_hp);
    ImGui::TextColored(hpMatch ? ImVec4(0.3f,0.8f,0.3f,1) : ImVec4(1,0,0,1),
        "HP: P1=%d/%d P2=%d/%d %s",
        h->p1_hp, c->p1_hp, h->p2_hp, c->p2_hp, hpMatch ? "MATCH" : "MISMATCH");

    const bool stageMatch = (h->stage_id == c->stage_id);
    ImGui::TextColored(stageMatch ? ImVec4(0.3f,0.8f,0.3f,1) : ImVec4(1,0.5f,0,1),
        "Stage ID: Host=%u Client=%u %s",
        h->stage_id, c->stage_id, stageMatch ? "MATCH" : "MISMATCH");

    // Rollback comparison
    ImGui::Text("Rollbacks: Host=%u Client=%u  MaxDepth: %u/%u",
                h->total_rollbacks, c->total_rollbacks,
                h->max_rollback_depth, c->max_rollback_depth);
    ImGui::Text("Timesync skips: Host=%u Client=%u  Ahead: H=%.1f C=%.1f",
                h->rb_timesync_skips, c->rb_timesync_skips,
                h->rb_frames_ahead, c->rb_frames_ahead);
}

// ============================================================================
// GUI Main Loop
// ============================================================================

static bool RunGui(const LauncherConfig* cfg, HANDLE hHost, HANDLE hClient, HarnessRunLogs* runLogs) {
    // Initialize live net-sim controls from command-line config
    g_liveLatencyMs = cfg->latencyMs;
    g_liveJitterMs  = cfg->jitterMs;
    g_liveLossPct   = cfg->lossPct;
    g_liveDupPct    = cfg->dupPct;

    // Register window class
    WNDCLASSEXA wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_CLASSDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.lpszClassName = "AS2HarnessWnd";
    RegisterClassExA(&wc);

    HWND hwnd = CreateWindowExA(
        0, wc.lpszClassName, "AS2 Test Harness Dashboard",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 1600, 950,
        nullptr, nullptr, wc.hInstance, nullptr);

    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassA(wc.lpszClassName, wc.hInstance);
        return false;
    }

    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    // Make fonts larger for readability on high-DPI displays
    ImFontConfig fontCfg;
    fontCfg.SizePixels = 18.0f;
    io.Fonts->AddFontDefault(&fontCfg);

    // Scale style for better readability
    ImGui::GetStyle().FramePadding  = ImVec2(6, 4);
    ImGui::GetStyle().ItemSpacing   = ImVec2(8, 5);
    ImGui::GetStyle().ScrollbarSize = 16;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_pd3dDevice);

    // Open shared memory slots (will retry in loop)
    ShmSlot hostSlot = {}, clientSlot = {};
    char hostLogBuf[HARNESS_LOG_LINES][HARNESS_LOG_LINE_LEN] = {};
    char clientLogBuf[HARNESS_LOG_LINES][HARNESS_LOG_LINE_LEN] = {};
    int hostLogCount = 0, clientLogCount = 0;
    DWORD lastShmRetry = 0;

    ImVec4 clearColor = ImVec4(0.06f, 0.06f, 0.08f, 1.00f);
    bool running = true;
    DWORD completeSince = 0;

    while (running) {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) running = false;
        }
        if (!running) break;

        // Handle device lost
        if (g_deviceLost) {
            HRESULT hr = g_pd3dDevice->TestCooperativeLevel();
            if (hr == D3DERR_DEVICELOST) continue;
            if (hr == D3DERR_DEVICENOTRESET) ResetDevice();
            g_deviceLost = false;
        }

        // Retry shared memory connections periodically
        DWORD now = GetTickCount();
        if (now - lastShmRetry > 1000) {
            lastShmRetry = now;
            if (!hostSlot.valid)   OpenShmSlot(&hostSlot,   HARNESS_SHM_NAME_HOST);
            if (!clientSlot.valid) OpenShmSlot(&clientSlot, HARNESS_SHM_NAME_CLIENT);

            // Check if game processes have exited
            if (hHost != INVALID_HANDLE_VALUE) {
                DWORD exitCode;
                if (GetExitCodeProcess(hHost, &exitCode) && exitCode != STILL_ACTIVE) {
                    printf("  Host process exited (code %lu)\n", exitCode);
                    CloseHandle(hHost); hHost = INVALID_HANDLE_VALUE;
                }
            }
            if (hClient != INVALID_HANDLE_VALUE) {
                DWORD exitCode;
                if (GetExitCodeProcess(hClient, &exitCode) && exitCode != STILL_ACTIVE) {
                    printf("  Client process exited (code %lu)\n", exitCode);
                    CloseHandle(hClient); hClient = INVALID_HANDLE_VALUE;
                }
            }
        }

        const bool hostDone = hostSlot.valid && hostSlot.data && hostSlot.data->phase == 9;
        const bool clientDone = clientSlot.valid && clientSlot.data && clientSlot.data->phase == 9;
        if (hostSlot.valid && hostSlot.data) CaptureHarnessLogPath(runLogs, hostSlot.data);
        if (clientSlot.valid && clientSlot.data) CaptureHarnessLogPath(runLogs, clientSlot.data);
        const bool bothDone = hostDone && clientDone;
        const bool bothExited = (hHost == INVALID_HANDLE_VALUE && hClient == INVALID_HANDLE_VALUE);
        if (bothDone || bothExited) {
            if (completeSince == 0) {
                completeSince = now;
            } else if ((now - completeSince) > 3000) {
                running = false;
            }
        } else {
            completeSince = 0;
        }

        // Start ImGui frame
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        // Main window: fill entire client area
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("Dashboard", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);

        ImGui::TextColored(ImVec4(0.4f, 0.8f, 1.0f, 1.0f),
                          "AS2 Test Harness Dashboard");
        ImGui::SameLine(ImGui::GetWindowWidth() - 300);
        ImGui::Text("Host: %s  Client: %s",
                    hostSlot.valid ? "Connected" : "Waiting...",
                    clientSlot.valid ? "Connected" : "Waiting...");
        if (completeSince != 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                               "Auto-closing after completion in %.1f s",
                               (3000.0f - (double)(now - completeSince)) / 1000.0f);
        }

        if (ImGui::BeginTabBar("MainTabs")) {
            // Dual panel view
            if (ImGui::BeginTabItem("Side by Side")) {
                float halfW = ImGui::GetContentRegionAvail().x * 0.5f - 4;
                float h = ImGui::GetContentRegionAvail().y;

                ImGui::BeginChild("LeftCol", ImVec2(halfW, h));
                DrawSlotPanel("HOST", &hostSlot, hostLogBuf, &hostLogCount);
                ImGui::EndChild();

                ImGui::SameLine();

                ImGui::BeginChild("RightCol", ImVec2(halfW, h));
                DrawSlotPanel("CLIENT", &clientSlot, clientLogBuf, &clientLogCount);
                ImGui::EndChild();

                ImGui::EndTabItem();
            }

            // Comparison / desync detection
            if (ImGui::BeginTabItem("Comparison")) {
                DrawComparisonPanel(&hostSlot, &clientSlot);
                ImGui::EndTabItem();
            }

            // Host only
            if (ImGui::BeginTabItem("Host Detail")) {
                DrawSlotPanel("HOST (Detail)", &hostSlot, hostLogBuf, &hostLogCount);
                ImGui::EndTabItem();
            }

            // Client only
            if (ImGui::BeginTabItem("Client Detail")) {
                DrawSlotPanel("CLIENT (Detail)", &clientSlot, clientLogBuf, &clientLogCount);
                ImGui::EndTabItem();
            }

            // Config overview + controls
            if (ImGui::BeginTabItem("Config")) {
                ImGui::SeparatorText("Game");
                ImGui::Text("Game dir: %s", cfg->gameDir);
                ImGui::Text("Game exe: %s", cfg->gameExe);

                ImGui::SeparatorText("Connection");
                ImGui::Text("Host port: %d  Client port: %d", cfg->hostPort, cfg->clientPort);
                ImGui::Text("Host char: %d  Client char: %d", cfg->hostChar, cfg->clientChar);
                ImGui::Text("Delay: %d  Duration: %d sec", cfg->delayFrames, cfg->durationSec);
                ImGui::Text("Stage mash: %s  Preview frames: %d",
                            cfg->stageMashTest ? "on" : "off",
                            cfg->stageMashFrames);

                ImGui::SeparatorText("Network Simulation (live)");
                ImGui::SetNextItemWidth(200);
                ImGui::SliderInt("Latency (ms)##lat", &g_liveLatencyMs, 0, 500);
                ImGui::SetNextItemWidth(200);
                ImGui::SliderInt("Jitter (ms)##jit", &g_liveJitterMs, 0, 200);
                ImGui::SetNextItemWidth(200);
                ImGui::SliderFloat("Loss %%##loss", &g_liveLossPct, 0.0f, 50.0f, "%.1f");
                ImGui::SetNextItemWidth(200);
                ImGui::SliderFloat("Dup %%##dup", &g_liveDupPct, 0.0f, 50.0f, "%.1f");

                // Push live net-sim values to shared memory (if connected)
                // Note: This writes into the read-only mapped data; the mod can
                // check sim_* fields each frame.  If the mapping is FILE_MAP_READ
                // the writes silently fail, which is harmless.

                ImGui::SeparatorText("Instance Control");
                float bw = 180;
                if (ImGui::Button("Terminate Host", ImVec2(bw, 0)) && hHost != INVALID_HANDLE_VALUE) {
                    TerminateProcess(hHost, 0); CloseHandle(hHost); hHost = INVALID_HANDLE_VALUE;
                }
                ImGui::SameLine();
                if (ImGui::Button("Terminate Client", ImVec2(bw, 0)) && hClient != INVALID_HANDLE_VALUE) {
                    TerminateProcess(hClient, 0); CloseHandle(hClient); hClient = INVALID_HANDLE_VALUE;
                }
                ImGui::SameLine();
                if (ImGui::Button("Terminate Both", ImVec2(bw, 0))) {
                    if (hHost   != INVALID_HANDLE_VALUE) { TerminateProcess(hHost, 0);   CloseHandle(hHost);   hHost   = INVALID_HANDLE_VALUE; }
                    if (hClient != INVALID_HANDLE_VALUE) { TerminateProcess(hClient, 0); CloseHandle(hClient); hClient = INVALID_HANDLE_VALUE; }
                }

                // Process liveness
                ImGui::Spacing();
                ImGui::Text("Host process: %s",
                    hHost != INVALID_HANDLE_VALUE ? "running" : "not running");
                ImGui::Text("Client process: %s",
                    hClient != INVALID_HANDLE_VALUE ? "running" : "not running");

                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();

        // Render
        ImGui::EndFrame();
        g_pd3dDevice->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        g_pd3dDevice->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        D3DCOLOR cc = D3DCOLOR_RGBA(
            (int)(clearColor.x*255), (int)(clearColor.y*255),
            (int)(clearColor.z*255), (int)(clearColor.w*255));
        g_pd3dDevice->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, cc, 1.0f, 0);
        if (g_pd3dDevice->BeginScene() >= 0) {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_pd3dDevice->EndScene();
        }
        HRESULT result = g_pd3dDevice->Present(nullptr, nullptr, nullptr, nullptr);
        if (result == D3DERR_DEVICELOST) g_deviceLost = true;
    }

    // Cleanup
    CloseShmSlot(&hostSlot);
    CloseShmSlot(&clientSlot);

    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassA(wc.lpszClassName, wc.hInstance);

    return true;
}

// ============================================================================
// Console-only monitoring (fallback / --no-gui mode)
// ============================================================================

static const char* PhaseStr(uint32_t p) {
    static const char* names[] = {
        "Idle","WaitForGame","StartSession","WaitConnect",
        "LaunchCharSel","WaitCharSel","AutoSelect","WaitMatch","InMatch","Done"
    };
    return p <= 9 ? names[p] : "???";
}

static const char* ModeStr(uint32_t m) {
    switch (m) {
        case 0: return "Title"; case 2: return "Title2"; case 3: return "Menu";
        case 4: return "Lobby"; case 5: return "VsSel"; case 6: return "CharSel";
        case 7: return "StageSel"; case 8: return "Match"; case 9: return "Story"; default: return "???";
    }
}

static void PrintSlotLine(const char* tag, const HarnessSharedData* d) {
    if (!d) { printf("  %-6s [not connected]\n", tag); return; }
    printf("  %-6s phase=%-12s mode=%-7s sub=%u stage=%u f=%u gekko[L=%d R=%d ahead=%.1f adv=%u] "
            "rb=%u/%u ld=%u tsync=%u p1=%d p2=%d rtt=%.0f pkt=%u/%u dsync=%u warn=%u\n",
        tag,
        PhaseStr(d->phase),
        ModeStr(d->game_mode),
        d->sub_state,
        d->stage_id,
        d->total_frames,
        d->rb_local_frame, d->rb_remote_frame, d->rb_frames_ahead, d->rb_advance_count,
        d->total_rollbacks, d->max_rollback_depth,
        d->total_loads,
        d->rb_timesync_skips,
        (int)d->p1_hp, (int)d->p2_hp,
        d->conn_rtt_ms,
        d->packets_sent, d->packets_received,
        d->desync_count,
        d->desync_warnings);
}

static void PrintSlotNewLogs(const char* tag, ShmSlot* slot) {
    if (!slot->valid || !slot->data) return;
    uint32_t head = slot->data->log_head;
    uint32_t count = slot->data->log_count;
    if (count <= slot->lastLogHead) return;  // no new lines

    // How many new lines?
    uint32_t newLines = count - slot->lastLogHead;
    if (newLines > HARNESS_LOG_LINES) newLines = HARNESS_LOG_LINES;

    // Read from oldest new to newest
    uint32_t startIdx = head >= newLines ? head - newLines : HARNESS_LOG_LINES - (newLines - head);
    for (uint32_t i = 0; i < newLines; i++) {
        uint32_t idx = (startIdx + i) % HARNESS_LOG_LINES;
        const char* line = slot->data->log_lines[idx];
        if (line[0] != '\0') {
            printf("    [%s] %s\n", tag, line);
        }
    }
    slot->lastLogHead = count;
}

static void RunConsoleMonitor(const LauncherConfig* cfg, HANDLE hHost, HANDLE hClient, HarnessRunLogs* runLogs) {
    HANDLE handles[2] = { hHost, hClient };
    int handleCount = (hClient != INVALID_HANDLE_VALUE) ? 2 : 1;
    DWORD startTime = GetTickCount();
    int remaining = handleCount;

    // Try to open shared memory slots
    ShmSlot hostSlot = {}, clientSlot = {};
    bool shmOpened = false;
    int retryCount = 0;
    const int kMaxRetries = 30;  // Client needs more time to start up

    // Phase tracking for transition logging
    uint32_t prevHostPhase = UINT32_MAX, prevClientPhase = UINT32_MAX;
    uint32_t prevHostMode = UINT32_MAX, prevClientMode = UINT32_MAX;
    uint32_t prevHostSub = UINT32_MAX, prevClientSub = UINT32_MAX;
    uint32_t prevHostStage = UINT32_MAX, prevClientStage = UINT32_MAX;

    printf("\n--- Live Console Monitor ---\n\n");

    while (remaining > 0) {
        // Try to open SHM if not yet done
        if (!shmOpened && retryCount < kMaxRetries) {
            if (OpenShmSlot(&hostSlot, HARNESS_SHM_NAME_HOST)) {
                printf("  [SHM] Host shared memory connected\n");
            }
            if (OpenShmSlot(&clientSlot, HARNESS_SHM_NAME_CLIENT)) {
                printf("  [SHM] Client shared memory connected\n");
            }
            shmOpened = hostSlot.valid && clientSlot.valid;
            retryCount++;
        }

        DWORD result = WaitForMultipleObjects(handleCount, handles, FALSE, 1000);

        if (result >= WAIT_OBJECT_0 && result < WAIT_OBJECT_0 + (DWORD)handleCount) {
            int idx = result - WAIT_OBJECT_0;
            DWORD exitCode = 0;
            GetExitCodeProcess(handles[idx], &exitCode);
            printf("\n  >>> %s exited (code %lu) <<<\n", idx==0 ? "HOST" : "CLIENT", exitCode);
            remaining--;
            if (remaining > 0) {
                printf("  Waiting for other instance...\n");
                DWORD other = WaitForMultipleObjects(handleCount, handles, TRUE, 10000);
                if (other == WAIT_TIMEOUT) {
                    printf("  Timeout — terminating remaining instance\n");
                    for (int i = 0; i < handleCount; i++)
                        if (handles[i] != INVALID_HANDLE_VALUE) TerminateProcess(handles[i], 0);
                }
                remaining = 0;
            }
        } else if (result == WAIT_TIMEOUT) {
            DWORD elapsed = (GetTickCount() - startTime) / 1000;

            if (hostSlot.valid && hostSlot.data) CaptureHarnessLogPath(runLogs, hostSlot.data);
            if (clientSlot.valid && clientSlot.data) CaptureHarnessLogPath(runLogs, clientSlot.data);

            // Print phase transitions
            if (hostSlot.valid && hostSlot.data) {
                if (hostSlot.data->phase != prevHostPhase) {
                    printf("  [HOST]   Phase: %s -> %s\n",
                        prevHostPhase != UINT32_MAX ? PhaseStr(prevHostPhase) : "---",
                        PhaseStr(hostSlot.data->phase));
                    prevHostPhase = hostSlot.data->phase;
                }
                if (prevHostMode == UINT32_MAX ||
                    hostSlot.data->game_mode != prevHostMode ||
                    hostSlot.data->sub_state != prevHostSub ||
                    hostSlot.data->stage_id != prevHostStage) {
                    printf("  [HOST]   Game: mode=%u(%s) sub=%u stage=%u\n",
                           hostSlot.data->game_mode,
                           ModeStr(hostSlot.data->game_mode),
                           hostSlot.data->sub_state,
                           hostSlot.data->stage_id);
                    prevHostMode = hostSlot.data->game_mode;
                    prevHostSub = hostSlot.data->sub_state;
                    prevHostStage = hostSlot.data->stage_id;
                }
            }
            if (clientSlot.valid && clientSlot.data) {
                if (clientSlot.data->phase != prevClientPhase) {
                    printf("  [CLIENT] Phase: %s -> %s\n",
                        prevClientPhase != UINT32_MAX ? PhaseStr(prevClientPhase) : "---",
                        PhaseStr(clientSlot.data->phase));
                    prevClientPhase = clientSlot.data->phase;
                }
                if (prevClientMode == UINT32_MAX ||
                    clientSlot.data->game_mode != prevClientMode ||
                    clientSlot.data->sub_state != prevClientSub ||
                    clientSlot.data->stage_id != prevClientStage) {
                    printf("  [CLIENT] Game: mode=%u(%s) sub=%u stage=%u\n",
                           clientSlot.data->game_mode,
                           ModeStr(clientSlot.data->game_mode),
                           clientSlot.data->sub_state,
                           clientSlot.data->stage_id);
                    prevClientMode = clientSlot.data->game_mode;
                    prevClientSub = clientSlot.data->sub_state;
                    prevClientStage = clientSlot.data->stage_id;
                }
            }

            // Print log lines from shared memory ring buffers
            PrintSlotNewLogs("HOST", &hostSlot);
            PrintSlotNewLogs("CLIENT", &clientSlot);

            // Print status line every second
            printf("  [%3lus] ", elapsed);
            PrintSlotLine("HOST", hostSlot.valid ? hostSlot.data : nullptr);
            printf("         ");
            PrintSlotLine("CLIENT", clientSlot.valid ? clientSlot.data : nullptr);

            // Safety timeout
            if (elapsed > (DWORD)(cfg->durationSec + 60)) {
                printf("\n  >>> SAFETY TIMEOUT (%lus) — terminating <<<\n", elapsed);
                for (int i = 0; i < handleCount; i++)
                    if (handles[i] != INVALID_HANDLE_VALUE) TerminateProcess(handles[i], 0);
                remaining = 0;
            }

            // Check if both instances reached "Done" phase (9)
            bool hostDone = hostSlot.valid && hostSlot.data && hostSlot.data->phase == 9;
            bool clientDone = clientSlot.valid && clientSlot.data && clientSlot.data->phase == 9;
            if (hostDone && clientDone) {
                printf("\n  >>> Both instances reached Done phase — match complete <<<\n");
                // Give them a moment to flush logs, then terminate
                Sleep(3000);
                for (int i = 0; i < handleCount; i++)
                    if (handles[i] != INVALID_HANDLE_VALUE) TerminateProcess(handles[i], 0);
                remaining = 0;
            }
        } else {
            break;
        }
    }

    // Print final summary from shared memory
    printf("\n--- Final State ---\n");
    if (hostSlot.valid && hostSlot.data) {
        printf("  HOST:   frames=%u gekko[L=%d R=%d ahead=%.1f adv=%u] rb=%u(max %u) saves=%u loads=%u desyncs=%u "
                    "rtt=%.1f/%.1fms anomalies=%u trace=%s\n",
            hostSlot.data->total_frames,
            hostSlot.data->rb_local_frame, hostSlot.data->rb_remote_frame,
            hostSlot.data->rb_frames_ahead, hostSlot.data->rb_advance_count,
            hostSlot.data->total_rollbacks, hostSlot.data->max_rollback_depth,
            hostSlot.data->total_saves, hostSlot.data->total_loads,
            hostSlot.data->desync_count,
            hostSlot.data->avg_rtt_ms, hostSlot.data->peak_rtt_ms,
                hostSlot.data->frame_counter_anomalies,
                hostSlot.data->rollback_trace_log[0] ? hostSlot.data->rollback_trace_log : "<none>");
    }
    if (clientSlot.valid && clientSlot.data) {
        printf("  CLIENT: frames=%u gekko[L=%d R=%d ahead=%.1f adv=%u] rb=%u(max %u) saves=%u loads=%u desyncs=%u "
                    "rtt=%.1f/%.1fms anomalies=%u trace=%s\n",
            clientSlot.data->total_frames,
            clientSlot.data->rb_local_frame, clientSlot.data->rb_remote_frame,
            clientSlot.data->rb_frames_ahead, clientSlot.data->rb_advance_count,
            clientSlot.data->total_rollbacks, clientSlot.data->max_rollback_depth,
            clientSlot.data->total_saves, clientSlot.data->total_loads,
            clientSlot.data->desync_count,
            clientSlot.data->avg_rtt_ms, clientSlot.data->peak_rtt_ms,
                clientSlot.data->frame_counter_anomalies,
                clientSlot.data->rollback_trace_log[0] ? clientSlot.data->rollback_trace_log : "<none>");
    }

    CloseShmSlot(&hostSlot);
    CloseShmSlot(&clientSlot);

    for (int i = 0; i < handleCount; i++)
        if (handles[i] != INVALID_HANDLE_VALUE) CloseHandle(handles[i]);
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    // Enable DPI awareness so the window uses real pixel sizes
    SetProcessDpiAwareness(PROCESS_PER_MONITOR_DPI_AWARE);

    // Load real system d3d9.dll BEFORE anything else — avoids our proxy in the
    // game directory which would trigger mod initialization and crash.
    if (!LoadSystemD3D9()) {
        printf("FATAL: Could not load system d3d9.dll\n");
        return 1;
    }

    // Allocate console for printf output even in GUI mode
    printf("============================================\n");
    printf("  Alice Senki 2 - Test Harness Launcher\n");
    printf("============================================\n\n");

    HarnessRunLogs runLogs = {};

    LauncherConfig cfg;
    SetDefaults(&cfg);
    if (!ParseArgs(argc, argv, &cfg)) return 1;

    // Find game exe
    if (!FindGameExe(cfg.gameDir, cfg.gameExe, sizeof(cfg.gameExe))) {
        printf("ERROR: No game exe found in: %s\n", cfg.gameDir);
        return 1;
    }

    // Check mod DLLs
    const char* requiredDlls[] = { "d3d9.dll", "as2_rollback.dll", "SDL3.dll" };
    for (const char* dll : requiredDlls) {
        char p[MAX_PATH];
        _snprintf_s(p, sizeof(p), _TRUNCATE, "%s\\%s", cfg.gameDir, dll);
        if (GetFileAttributesA(p) == INVALID_FILE_ATTRIBUTES)
            printf("WARNING: %s not found in game directory\n", dll);
    }

    printf("Configuration:\n");
    printf("  Game dir:  %s\n", cfg.gameDir);
    printf("  Host:      port %d, char %d\n", cfg.hostPort, cfg.hostChar);
    printf("  Client:    port %d, char %d\n", cfg.clientPort, cfg.clientChar);
    printf("  Delay:     %d frames  Duration: %d sec\n", cfg.delayFrames, cfg.durationSec);
        printf("  Stage test:%s  Preview mash frames: %d\n",
            cfg.stageMashTest ? " on" : " off",
            cfg.stageMashFrames);
    if (cfg.latencyMs > 0 || cfg.lossPct > 0)
        printf("  Net sim:   lat=%dms jit=%dms loss=%.1f%% dup=%.1f%%\n",
               cfg.latencyMs, cfg.jitterMs, cfg.lossPct, cfg.dupPct);
    printf("\n");

    // Write config files
    char hostCfgPath[MAX_PATH];
    _snprintf_s(hostCfgPath, sizeof(hostCfgPath), _TRUNCATE,
                "%s\\as2_autoconnect.cfg", cfg.gameDir);

    printf("Writing config files...\n");
    if (!WriteConfigFile(hostCfgPath, true, &cfg)) return 1;

    if (cfg.noLaunch) {
        char clientCfgPath[MAX_PATH];
        _snprintf_s(clientCfgPath, sizeof(clientCfgPath), _TRUNCATE,
                    "%s\\as2_autoconnect_client.cfg", cfg.gameDir);
        WriteConfigFile(clientCfgPath, false, &cfg);
        printf("\nConfigs written. --no-launch specified.\n");
        return 0;
    }

    // Launch host
    printf("\nLaunching HOST instance...\n");
    HANDLE hHost = LaunchGame(cfg.gameDir, cfg.gameExe, "Host");
    if (hHost == INVALID_HANDLE_VALUE) return 1;

    // Wait then swap config for client
    printf("Waiting 3 seconds before launching client...\n");
    Sleep(3000);
    if (!WriteConfigFile(hostCfgPath, false, &cfg)) {
        TerminateProcess(hHost, 1); CloseHandle(hHost);
        return 1;
    }

    printf("Launching CLIENT instance...\n");
    HANDLE hClient = LaunchGame(cfg.gameDir, cfg.gameExe, "Client");

    printf("\n--- Dashboard starting ---\n\n");

    if (!cfg.noGui) {
        RunGui(&cfg, hHost, hClient, &runLogs);
    } else {
        RunConsoleMonitor(&cfg, hHost, hClient, &runLogs);
    }

    // Wait for log flush
    Sleep(1000);

    // Analyze logs — harness writes to the session log directory (logs/<timestamp>/)
    // rather than the game directory. Search for the most recent session directories.
    printf("\n============================================\n");
    printf("  Log Analysis\n");
    printf("============================================\n");
    char hostLog[MAX_PATH] = {}, clientLog[MAX_PATH] = {};

    // Prefer exact paths captured from shared memory for this run. If shared
    // memory never came up, fall back to scanning log directories.
    if (runLogs.hostLog[0]) {
        strncpy_s(hostLog, sizeof(hostLog), runLogs.hostLog, _TRUNCATE);
    }
    if (runLogs.clientLog[0]) {
        strncpy_s(clientLog, sizeof(clientLog), runLogs.clientLog, _TRUNCATE);
    }

    if (!hostLog[0] || !clientLog[0]) {
        char logsRoot[MAX_PATH];
        _snprintf_s(logsRoot, sizeof(logsRoot), _TRUNCATE, "%s\\logs", cfg.gameDir);

        if (!hostLog[0])
            FindNewestHarnessLog(logsRoot, "harness_host.log", hostLog, sizeof(hostLog));
        if (!clientLog[0])
            FindNewestHarnessLog(logsRoot, "harness_client.log", clientLog, sizeof(clientLog));

        // Fallback: try gameDir directly (legacy behavior)
        if (!hostLog[0])
            _snprintf_s(hostLog, sizeof(hostLog), _TRUNCATE, "%s\\harness_host.log", cfg.gameDir);
        if (!clientLog[0])
            _snprintf_s(clientLog, sizeof(clientLog), _TRUNCATE, "%s\\harness_client.log", cfg.gameDir);
    }

    printf("  Using host log:   %s\n", hostLog);
    printf("  Using client log: %s\n", clientLog);

    AnalyzeLog(hostLog, "Host");
    AnalyzeLog(clientLog, "Client");

    printf("\n============================================\n");
    printf("  Test complete.\n");
    printf("============================================\n");

    // Cleanup autoconnect config
    DeleteFileA(hostCfgPath);
    printf("\nCleaned up %s\n", hostCfgPath);

    return 0;
}
