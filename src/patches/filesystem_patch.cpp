/**
 * Alice Senki 2 - Filesystem Shift-JIS Path Patch
 *
 * On non-Japanese Windows, kernel32's CreateFileA converts narrow paths using
 * the system's NLS ANSI codepage (not GetACP). Shift-JIS filenames in the game
 * get garbled. We intercept A-suffix APIs, convert through CP932, and call W-suffix.
 *
 * Extracted from as2_rollback.cpp (non-rollback patch).
 */

#include "filesystem_patch.h"
#include "core/as2_constants.h"
#include "locale_patch.h"
#include "ui/log_window.h"
#include "MinHook.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <unordered_set>
#include <vector>

// Original function pointers — populated by MinHook.
CreateFileA_t g_origCreateFileA = nullptr;
DeleteFileA_t g_origDeleteFileA = nullptr;
FindFirstFileA_t g_origFindFirstFileA = nullptr;
GetFileAttributesA_t g_origGetFileAttributesA = nullptr;

namespace {

typedef void (__cdecl *LoadedModInit_t)(HMODULE gameModule);
typedef void (__cdecl *LoadedModShutdown_t)();
typedef void (__cdecl *LoadedModSetLogDir_t)(const char* dir);

struct LoadedModEntry {
    char name[64];
    char root_path[MAX_PATH];
    char dll_path[MAX_PATH];
    HMODULE module;
    LoadedModShutdown_t shutdown;
    bool init_called;
};

static HMODULE s_gameModule = nullptr;
static bool s_initialized = false;
static bool s_hooksInstalled = false;
static bool s_modDllsLoaded = false;
static char s_gameRoot[MAX_PATH] = {};
static char s_modsRoot[MAX_PATH] = {};
static char s_configPath[MAX_PATH] = {};
static std::vector<LoadedModEntry> s_loadedMods;
static std::unordered_set<std::string> s_loggedOverrides;

static const char kDefaultModsConfigTemplate[] =
    "; Alice Senki 2 mod loader configuration\r\n"
    ";\r\n"
    "; Top-to-bottom priority: earlier entries win file conflicts.\r\n"
    "; Set =1 to enable a mod folder under mods\\<ModName>.\r\n"
    "; Set =0 or leave the line commented out to keep it disabled.\r\n"
    "; Optional DLL mods load from mods\\<ModName>\\<ModName>.dll.\r\n"
    "; File overrides mirror the game path, for example:\r\n"
    ";   data\\tit.bin -> mods\\ExampleMod\\data\\tit.bin\r\n"
    "; Folder names must not contain spaces or path separators.\r\n"
    "\r\n"
    "[Mods]\r\n"
    ";ExampleMod=1\r\n";

static constexpr uintptr_t kAddrLegacyConfigPath = GAME_BASE + 0x3445E4; // byte_7445E4
static constexpr uintptr_t kAddrLegacyKeymapPath = GAME_BASE + 0x3445D0; // byte_7445D0

static const char kPatchedConfigPath[] = "as2_system.ini";
static const char kPatchedKeymapPath[] = "as2_system.rec";

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

static bool PathContainsNonASCII(const char* path) {
    if (!path) {
        return false;
    }

    for (const unsigned char* p = (const unsigned char*)path; *p; ++p) {
        if (*p > 0x7F) {
            return true;
        }
    }

    return false;
}

static bool ConvertAnsiPathToWide(const char* path, WCHAR* widePath, int wideCap) {
    if (!path || !widePath || wideCap <= 0) {
        return false;
    }

    return MultiByteToWideChar(CP_JAPANESE_SHIFTJIS, 0, path, -1, widePath, wideCap) > 0;
}

static bool OpenAnsiPathFile(FILE** outFile, const char* path, const wchar_t* mode) {
    if (!outFile) {
        return false;
    }

    *outFile = nullptr;
    if (!path || !path[0] || !mode) {
        return false;
    }

    WCHAR widePath[MAX_PATH] = {};
    if (!ConvertAnsiPathToWide(path, widePath, MAX_PATH)) {
        return false;
    }

    return _wfopen_s(outFile, widePath, mode) == 0 && *outFile;
}

static bool QueryFileAttributesAnsi(const char* path, DWORD* outAttributes) {
    if (!path) {
        return false;
    }

    WCHAR widePath[MAX_PATH] = {};
    if (!ConvertAnsiPathToWide(path, widePath, MAX_PATH)) {
        return false;
    }

    const DWORD attributes = GetFileAttributesW(widePath);
    if (outAttributes) {
        *outAttributes = attributes;
    }

    return attributes != INVALID_FILE_ATTRIBUTES;
}

static bool FileExistsAnsi(const char* path) {
    DWORD attributes = INVALID_FILE_ATTRIBUTES;
    return QueryFileAttributesAnsi(path, &attributes) && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

static bool DirectoryExistsAnsi(const char* path) {
    DWORD attributes = INVALID_FILE_ATTRIBUTES;
    return QueryFileAttributesAnsi(path, &attributes) && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

static bool EnsureDirectoryExistsAnsi(const char* path, bool* outCreated) {
    if (outCreated) {
        *outCreated = false;
    }

    if (!path || !path[0]) {
        return false;
    }

    if (DirectoryExistsAnsi(path)) {
        return true;
    }

    WCHAR widePath[MAX_PATH] = {};
    if (!ConvertAnsiPathToWide(path, widePath, MAX_PATH)) {
        return false;
    }

    if (CreateDirectoryW(widePath, nullptr)) {
        if (outCreated) {
            *outCreated = true;
        }
        return true;
    }

    return GetLastError() == ERROR_ALREADY_EXISTS;
}

static bool WriteTextFileAnsi(const char* path, const char* content) {
    if (!path || !path[0] || !content) {
        return false;
    }

    FILE* file = nullptr;
    if (!OpenAnsiPathFile(&file, path, L"wb")) {
        return false;
    }

    const size_t length = strlen(content);
    const size_t written = fwrite(content, 1, length, file);
    fclose(file);
    return written == length;
}

static bool CopyFileAnsi(const char* sourcePath, const char* destPath, bool failIfExists) {
    if (!sourcePath || !sourcePath[0] || !destPath || !destPath[0]) {
        return false;
    }

    WCHAR wideSource[MAX_PATH] = {};
    WCHAR wideDest[MAX_PATH] = {};
    if (!ConvertAnsiPathToWide(sourcePath, wideSource, MAX_PATH) ||
        !ConvertAnsiPathToWide(destPath, wideDest, MAX_PATH)) {
        return false;
    }

    return CopyFileW(wideSource, wideDest, failIfExists ? TRUE : FALSE) == TRUE;
}

static void NormalizeSlashes(char* path) {
    if (!path) {
        return;
    }

    for (char* cursor = path; *cursor; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\\';
        }
    }
}

static char* TrimStringInPlace(char* text) {
    if (!text) {
        return text;
    }

    while (*text && isspace((unsigned char)*text)) {
        ++text;
    }

    char* end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

static bool IsAbsoluteWindowsPath(const char* path) {
    if (!path || !path[0]) {
        return false;
    }

    if ((path[0] == '\\' && path[1] == '\\') ||
        (isalpha((unsigned char)path[0]) && path[1] == ':')) {
        return true;
    }

    return false;
}

static bool ContainsParentTraversal(const char* path) {
    if (!path || !path[0]) {
        return false;
    }

    char scratch[MAX_PATH] = {};
    CopyText(scratch, sizeof(scratch), path);
    NormalizeSlashes(scratch);

    char* context = nullptr;
    for (char* token = strtok_s(scratch, "\\", &context);
         token;
         token = strtok_s(nullptr, "\\", &context)) {
        if (strcmp(token, "..") == 0) {
            return true;
        }
    }

    return false;
}

static void JoinPath(char* outPath, size_t outCap, const char* left, const char* right) {
    if (!outPath || outCap == 0) {
        return;
    }

    outPath[0] = '\0';
    if (!left || !left[0]) {
        CopyText(outPath, outCap, right);
        return;
    }
    if (!right || !right[0]) {
        CopyText(outPath, outCap, left);
        return;
    }

    snprintf(outPath, outCap, "%s\\%s", left, right);
}

static bool ReadStaticAnsiString(uintptr_t address, char* outText, size_t outCap) {
    if (!address || !outText || outCap == 0) {
        return false;
    }

    const char* source = reinterpret_cast<const char*>(address);
    const size_t sourceLen = strnlen_s(source, MAX_PATH);
    if (sourceLen == 0 || sourceLen >= MAX_PATH || sourceLen >= outCap) {
        return false;
    }

    memcpy(outText, source, sourceLen);
    outText[sourceLen] = '\0';
    return true;
}

static bool PatchStaticAnsiString(uintptr_t address, const char* replacement) {
    if (!address || !replacement || !replacement[0]) {
        return false;
    }

    char* target = reinterpret_cast<char*>(address);
    const size_t originalLen = strnlen_s(target, MAX_PATH);
    const size_t replacementLen = strlen(replacement);
    if (originalLen == 0 || originalLen >= MAX_PATH || replacementLen > originalLen) {
        return false;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, originalLen + 1, PAGE_READWRITE, &oldProtect)) {
        return false;
    }

    memset(target, 0, originalLen + 1);
    memcpy(target, replacement, replacementLen);

    DWORD restoredProtect = 0;
    VirtualProtect(target, originalLen + 1, oldProtect, &restoredProtect);
    return true;
}

static void MaybeMigrateLegacySettingsFile(const char* legacyName,
                                           const char* patchedName,
                                           const char* label) {
    if (!legacyName || !legacyName[0] || !patchedName || !patchedName[0] || !s_gameRoot[0]) {
        return;
    }

    char legacyPath[MAX_PATH] = {};
    char patchedPath[MAX_PATH] = {};
    JoinPath(legacyPath, sizeof(legacyPath), s_gameRoot, legacyName);
    JoinPath(patchedPath, sizeof(patchedPath), s_gameRoot, patchedName);

    if (FileExistsAnsi(patchedPath) || !FileExistsAnsi(legacyPath)) {
        return;
    }

    if (CopyFileAnsi(legacyPath, patchedPath, true)) {
        LOG_INFO("[ModLoader] Migrated legacy %s file to %s", label, patchedPath);
    } else {
        LOG_WARN("[ModLoader] Failed to migrate legacy %s file to %s", label, patchedPath);
    }
}

static void PatchLegacySettingsPaths() {
    char legacyConfigName[MAX_PATH] = {};
    char legacyKeymapName[MAX_PATH] = {};

    if (!ReadStaticAnsiString(kAddrLegacyConfigPath, legacyConfigName, sizeof(legacyConfigName))) {
        LOG_WARN("[ModLoader] Failed to read legacy config filename at 0x%08X", (unsigned int)kAddrLegacyConfigPath);
        return;
    }

    if (!ReadStaticAnsiString(kAddrLegacyKeymapPath, legacyKeymapName, sizeof(legacyKeymapName))) {
        LOG_WARN("[ModLoader] Failed to read legacy keymap filename at 0x%08X", (unsigned int)kAddrLegacyKeymapPath);
        return;
    }

    MaybeMigrateLegacySettingsFile(legacyConfigName, kPatchedConfigPath, "config");
    MaybeMigrateLegacySettingsFile(legacyKeymapName, kPatchedKeymapPath, "keymap");

    if (!PatchStaticAnsiString(kAddrLegacyConfigPath, kPatchedConfigPath)) {
        LOG_WARN("[ModLoader] Failed to patch legacy config filename at 0x%08X", (unsigned int)kAddrLegacyConfigPath);
    }

    if (!PatchStaticAnsiString(kAddrLegacyKeymapPath, kPatchedKeymapPath)) {
        LOG_WARN("[ModLoader] Failed to patch legacy keymap filename at 0x%08X", (unsigned int)kAddrLegacyKeymapPath);
    }
}

static bool IsValidModFolderName(const char* name) {
    if (!name || !name[0]) {
        return false;
    }

    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return false;
    }

    for (const unsigned char* cursor = (const unsigned char*)name; *cursor; ++cursor) {
        if (isspace(*cursor) || *cursor == '\\' || *cursor == '/' || *cursor == ':' ||
            *cursor == '*' || *cursor == '?' || *cursor == '"' || *cursor == '<' ||
            *cursor == '>' || *cursor == '|') {
            return false;
        }
    }

    return true;
}

static std::string LowercaseCopy(const char* text) {
    std::string lower = text ? text : "";
    for (char& ch : lower) {
        ch = (char)tolower((unsigned char)ch);
    }
    return lower;
}

static bool TryMakeGameRelativePath(const char* requestPath, char* outRelativePath, size_t outCap) {
    if (!requestPath || !requestPath[0] || !outRelativePath || outCap == 0) {
        return false;
    }

    char scratch[MAX_PATH] = {};
    CopyText(scratch, sizeof(scratch), requestPath);
    NormalizeSlashes(scratch);

    if (strchr(scratch, '*') || strchr(scratch, '?')) {
        return false;
    }

    const char* relative = scratch;
    if (IsAbsoluteWindowsPath(scratch)) {
        if (!s_gameRoot[0]) {
            return false;
        }

        const size_t rootLen = strlen(s_gameRoot);
        if (_strnicmp(scratch, s_gameRoot, rootLen) != 0) {
            return false;
        }
        if (scratch[rootLen] != '\0' && scratch[rootLen] != '\\') {
            return false;
        }

        relative = scratch + rootLen;
        while (*relative == '\\') {
            ++relative;
        }
    } else {
        while (relative[0] == '.' && relative[1] == '\\') {
            relative += 2;
        }
        while (*relative == '\\') {
            ++relative;
        }
    }

    if (!relative[0]) {
        return false;
    }

    if (_strnicmp(relative, "mods\\", 5) == 0) {
        return false;
    }

    if (ContainsParentTraversal(relative)) {
        return false;
    }

    CopyText(outRelativePath, outCap, relative);
    NormalizeSlashes(outRelativePath);
    return outRelativePath[0] != '\0';
}

static void MaybeLogOverride(const char* requestedPath, const char* overridePath, const char* modName) {
    if (!requestedPath || !overridePath || !modName) {
        return;
    }

    std::string key = std::string(requestedPath) + "|" + overridePath;
    if (s_loggedOverrides.insert(key).second) {
        LOG_INFO("[ModLoader] Override: %s -> %s (mod=%s)",
                 requestedPath,
                 overridePath,
                 modName);
    }
}

static bool TryResolveModOverridePath(const char* requestedPath,
                                      char* outOverridePath,
                                      size_t outOverrideCap,
                                      const LoadedModEntry** outMod) {
    if (!requestedPath || !requestedPath[0] || s_loadedMods.empty()) {
        return false;
    }

    char relativePath[MAX_PATH] = {};
    if (!TryMakeGameRelativePath(requestedPath, relativePath, sizeof(relativePath))) {
        return false;
    }

    for (const LoadedModEntry& mod : s_loadedMods) {
        char candidatePath[MAX_PATH] = {};
        JoinPath(candidatePath, sizeof(candidatePath), mod.root_path, relativePath);
        if (!FileExistsAnsi(candidatePath)) {
            continue;
        }

        CopyText(outOverridePath, outOverrideCap, candidatePath);
        if (outMod) {
            *outMod = &mod;
        }
        return true;
    }

    return false;
}

static HANDLE CreateFileViaWidePath(const char* path,
                                    DWORD desiredAccess,
                                    DWORD shareMode,
                                    LPSECURITY_ATTRIBUTES securityAttributes,
                                    DWORD creationDisposition,
                                    DWORD flagsAndAttributes,
                                    HANDLE templateFile) {
    WCHAR widePath[MAX_PATH] = {};
    if (!ConvertAnsiPathToWide(path, widePath, MAX_PATH)) {
        return INVALID_HANDLE_VALUE;
    }

    return CreateFileW(widePath,
                       desiredAccess,
                       shareMode,
                       securityAttributes,
                       creationDisposition,
                       flagsAndAttributes,
                       templateFile);
}

static BOOL DeleteFileViaWidePath(const char* path) {
    WCHAR widePath[MAX_PATH] = {};
    if (!ConvertAnsiPathToWide(path, widePath, MAX_PATH)) {
        return FALSE;
    }

    return DeleteFileW(widePath);
}

static HANDLE FindFirstFileViaWidePath(const char* path, LPWIN32_FIND_DATAA outFindData) {
    WCHAR widePath[MAX_PATH] = {};
    if (!ConvertAnsiPathToWide(path, widePath, MAX_PATH)) {
        return INVALID_HANDLE_VALUE;
    }

    WIN32_FIND_DATAW wideData = {};
    HANDLE handle = FindFirstFileW(widePath, &wideData);
    if (handle == INVALID_HANDLE_VALUE || !outFindData) {
        return handle;
    }

    outFindData->dwFileAttributes = wideData.dwFileAttributes;
    outFindData->ftCreationTime = wideData.ftCreationTime;
    outFindData->ftLastAccessTime = wideData.ftLastAccessTime;
    outFindData->ftLastWriteTime = wideData.ftLastWriteTime;
    outFindData->nFileSizeHigh = wideData.nFileSizeHigh;
    outFindData->nFileSizeLow = wideData.nFileSizeLow;
    outFindData->dwReserved0 = wideData.dwReserved0;
    outFindData->dwReserved1 = wideData.dwReserved1;
    WideCharToMultiByte(CP_JAPANESE_SHIFTJIS, 0, wideData.cFileName, -1,
        outFindData->cFileName, MAX_PATH, nullptr, nullptr);
    WideCharToMultiByte(CP_JAPANESE_SHIFTJIS, 0, wideData.cAlternateFileName, -1,
        outFindData->cAlternateFileName, 14, nullptr, nullptr);
    return handle;
}

static DWORD GetFileAttributesViaWidePath(const char* path) {
    WCHAR widePath[MAX_PATH] = {};
    if (!ConvertAnsiPathToWide(path, widePath, MAX_PATH)) {
        return INVALID_FILE_ATTRIBUTES;
    }

    return GetFileAttributesW(widePath);
}

static HMODULE LoadLibraryViaWidePath(const char* path) {
    WCHAR widePath[MAX_PATH] = {};
    if (!ConvertAnsiPathToWide(path, widePath, MAX_PATH)) {
        return nullptr;
    }

    return LoadLibraryW(widePath);
}

static bool ResolveGameRootPath(HMODULE gameModule, char* outRoot, size_t outRootCap) {
    if (!outRoot || outRootCap == 0) {
        return false;
    }

    outRoot[0] = '\0';

    char modulePath[MAX_PATH] = {};
    const DWORD modulePathLen = gameModule
        ? GetModuleFileNameA(gameModule, modulePath, MAX_PATH)
        : 0;
    if (modulePathLen > 0 && modulePathLen < MAX_PATH) {
        char moduleDir[MAX_PATH] = {};
        CopyText(moduleDir, sizeof(moduleDir), modulePath);

        char* lastSlash = strrchr(moduleDir, '\\');
        if (!lastSlash) {
            lastSlash = strrchr(moduleDir, '/');
        }

        if (lastSlash) {
            *lastSlash = '\0';
            NormalizeSlashes(moduleDir);
            if (moduleDir[0] &&
                strcmp(moduleDir, ".") != 0 &&
                strcmp(moduleDir, "..") != 0 &&
                DirectoryExistsAnsi(moduleDir)) {
                CopyText(outRoot, outRootCap, moduleDir);
                return true;
            }
        }

        LOG_WARN("[ModLoader] Ignoring unusable module-derived game root '%s'; falling back to current directory", modulePath);
    } else if (modulePathLen >= MAX_PATH) {
        LOG_WARN("[ModLoader] Module path was truncated while resolving game root; falling back to current directory");
    } else {
        LOG_WARN("[ModLoader] GetModuleFileNameA failed while resolving game root; falling back to current directory");
    }

    char cwd[MAX_PATH] = {};
    if (GetCurrentDirectoryA(MAX_PATH, cwd) == 0 || !cwd[0]) {
        return false;
    }

    NormalizeSlashes(cwd);
    CopyText(outRoot, outRootCap, cwd);
    return true;
}

static bool ShouldRedirectReadOpen(DWORD desiredAccess, DWORD creationDisposition, DWORD flagsAndAttributes) {
    if (creationDisposition != OPEN_EXISTING) {
        return false;
    }

    const DWORD writeLikeMask = GENERIC_WRITE | DELETE | WRITE_DAC | WRITE_OWNER |
        FILE_APPEND_DATA | FILE_WRITE_DATA | FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA;
    if ((desiredAccess & writeLikeMask) != 0) {
        return false;
    }

    if ((flagsAndAttributes & FILE_FLAG_DELETE_ON_CLOSE) != 0) {
        return false;
    }

    return true;
}

static void ParseModsConfig() {
    s_loadedMods.clear();
    s_loggedOverrides.clear();

    FILE* file = nullptr;
    if (!OpenAnsiPathFile(&file, s_configPath, L"rb")) {
        LOG_INFO("[ModLoader] No config found at %s", s_configPath);
        return;
    }

    std::unordered_set<std::string> seenMods;
    bool inModsSection = false;
    char line[512] = {};
    while (fgets(line, sizeof(line), file)) {
        char* cursor = line;
        if ((unsigned char)cursor[0] == 0xEF &&
            (unsigned char)cursor[1] == 0xBB &&
            (unsigned char)cursor[2] == 0xBF) {
            cursor += 3;
        }

        char* comment = strchr(cursor, ';');
        if (comment) {
            *comment = '\0';
        }

        char* trimmed = TrimStringInPlace(cursor);
        if (!trimmed[0]) {
            continue;
        }

        if (trimmed[0] == '[') {
            char* closing = strchr(trimmed, ']');
            if (!closing) {
                inModsSection = false;
                continue;
            }

            *closing = '\0';
            inModsSection = _stricmp(trimmed + 1, "Mods") == 0;
            continue;
        }

        if (!inModsSection) {
            continue;
        }

        char* equals = strchr(trimmed, '=');
        if (!equals) {
            continue;
        }

        *equals = '\0';
        char* modName = TrimStringInPlace(trimmed);
        char* enabledValue = TrimStringInPlace(equals + 1);
        if (!modName[0] || enabledValue[0] != '1') {
            continue;
        }

        if (!IsValidModFolderName(modName)) {
            LOG_WARN("[ModLoader] Ignoring invalid mod folder name '%s'", modName);
            continue;
        }

        const std::string dedupeKey = LowercaseCopy(modName);
        if (!seenMods.insert(dedupeKey).second) {
            LOG_WARN("[ModLoader] Ignoring duplicate mod entry '%s'", modName);
            continue;
        }

        LoadedModEntry entry = {};
        CopyText(entry.name, sizeof(entry.name), modName);
        JoinPath(entry.root_path, sizeof(entry.root_path), s_modsRoot, modName);
        JoinPath(entry.dll_path, sizeof(entry.dll_path), entry.root_path, "");
        snprintf(entry.dll_path, sizeof(entry.dll_path), "%s\\%s.dll", entry.root_path, modName);

        if (!DirectoryExistsAnsi(entry.root_path)) {
            LOG_WARN("[ModLoader] Enabled mod folder missing: %s", entry.root_path);
            continue;
        }

        s_loadedMods.push_back(entry);
    }

    fclose(file);

    if (s_loadedMods.empty()) {
        LOG_INFO("[ModLoader] Config loaded from %s (no enabled mods)", s_configPath);
        return;
    }

    LOG_INFO("[ModLoader] Config loaded from %s (%u enabled mod%s)",
             s_configPath,
             (unsigned)s_loadedMods.size(),
             s_loadedMods.size() == 1 ? "" : "s");
    for (size_t index = 0; index < s_loadedMods.size(); ++index) {
        LOG_INFO("[ModLoader] Priority %u: %s", (unsigned)(index + 1), s_loadedMods[index].name);
    }
}

static void EnsureModsDirectoryAndConfig() {
    bool createdDirectory = false;
    if (!EnsureDirectoryExistsAnsi(s_modsRoot, &createdDirectory)) {
        LOG_WARN("[ModLoader] Failed to create mods directory: %s", s_modsRoot);
        return;
    }

    if (createdDirectory) {
        LOG_INFO("[ModLoader] Created mods directory: %s", s_modsRoot);
    }

    if (FileExistsAnsi(s_configPath)) {
        return;
    }

    if (WriteTextFileAnsi(s_configPath, kDefaultModsConfigTemplate)) {
        LOG_INFO("[ModLoader] Created sample mod loader config: %s", s_configPath);
    } else {
        LOG_WARN("[ModLoader] Failed to create sample mod loader config: %s", s_configPath);
    }
}

} // namespace

void FilesystemPatch_Init(HMODULE gameModule) {
    if (s_initialized) {
        return;
    }

    s_gameModule = gameModule ? gameModule : GetModuleHandleA(nullptr);

    if (!ResolveGameRootPath(s_gameModule, s_gameRoot, sizeof(s_gameRoot))) {
        CopyText(s_gameRoot, sizeof(s_gameRoot), ".");
        LOG_WARN("[ModLoader] Failed to resolve game root; using relative current directory fallback");
    }

    JoinPath(s_modsRoot, sizeof(s_modsRoot), s_gameRoot, "mods");
    JoinPath(s_configPath, sizeof(s_configPath), s_modsRoot, "mods.ini");

    LOG_INFO("[ModLoader] Game root resolved to %s", s_gameRoot);

    PatchLegacySettingsPaths();

    EnsureModsDirectoryAndConfig();
    ParseModsConfig();
    s_initialized = true;
}

bool FilesystemPatch_InstallHooks() {
    MH_STATUS status = MH_Initialize();
    if (status != MH_OK && status != MH_ERROR_ALREADY_INITIALIZED) {
        LOG_ERROR("[ModLoader] MH_Initialize failed for filesystem hooks! Status: %d", status);
        return false;
    }

    if (s_hooksInstalled) {
        return true;
    }

    status = MH_CreateHook(
            reinterpret_cast<void*>(&CreateFileA),
            reinterpret_cast<void*>(&Hook_CreateFileA),
            reinterpret_cast<void**>(&g_origCreateFileA));
    if (status != MH_OK) {
        LOG_ERROR("[ModLoader] Failed to hook CreateFileA! Status: %d", status);
        return false;
    }

    status = MH_CreateHook(
            reinterpret_cast<void*>(&DeleteFileA),
            reinterpret_cast<void*>(&Hook_DeleteFileA),
            reinterpret_cast<void**>(&g_origDeleteFileA));
    if (status != MH_OK) {
        LOG_WARN("[ModLoader] Failed to hook DeleteFileA! Status: %d (continuing anyway)", status);
    }

    status = MH_CreateHook(
            reinterpret_cast<void*>(&FindFirstFileA),
            reinterpret_cast<void*>(&Hook_FindFirstFileA),
            reinterpret_cast<void**>(&g_origFindFirstFileA));
    if (status != MH_OK) {
        LOG_WARN("[ModLoader] Failed to hook FindFirstFileA! Status: %d (continuing anyway)", status);
    }

    status = MH_CreateHook(
            reinterpret_cast<void*>(&GetFileAttributesA),
            reinterpret_cast<void*>(&Hook_GetFileAttributesA),
            reinterpret_cast<void**>(&g_origGetFileAttributesA));
    if (status != MH_OK) {
        LOG_WARN("[ModLoader] Failed to hook GetFileAttributesA! Status: %d (continuing anyway)", status);
    }

    MH_EnableHook(reinterpret_cast<void*>(&CreateFileA));
    MH_EnableHook(reinterpret_cast<void*>(&DeleteFileA));
    MH_EnableHook(reinterpret_cast<void*>(&FindFirstFileA));
    MH_EnableHook(reinterpret_cast<void*>(&GetFileAttributesA));

    s_hooksInstalled = true;
    LOG_INFO("[ModLoader] Filesystem hooks installed (Shift-JIS paths + mod file overrides)");
    return true;
}

void FilesystemPatch_LoadEnabledModDLLs() {
    if (!s_initialized || s_modDllsLoaded) {
        return;
    }

    for (LoadedModEntry& mod : s_loadedMods) {
        if (!FileExistsAnsi(mod.dll_path)) {
            LOG_INFO("[ModLoader] %s: file overrides only (no DLL at %s)", mod.name, mod.dll_path);
            continue;
        }

        mod.module = LoadLibraryViaWidePath(mod.dll_path);
        if (!mod.module) {
            mod.module = LoadLibraryA(mod.dll_path);
        }
        if (!mod.module) {
            LOG_WARN("[ModLoader] Failed to load %s (err %lu)", mod.dll_path, GetLastError());
            continue;
        }

        LoadedModSetLogDir_t setLogDir = (LoadedModSetLogDir_t)GetProcAddress(mod.module, "ModSetLogDir");
        if (setLogDir && LogWindow_GetLogDir()[0]) {
            setLogDir(LogWindow_GetLogDir());
        }

        LoadedModInit_t initFn = (LoadedModInit_t)GetProcAddress(mod.module, "ModInit");
        mod.shutdown = (LoadedModShutdown_t)GetProcAddress(mod.module, "ModShutdown");

        if (initFn) {
            __try {
                initFn(s_gameModule);
                mod.init_called = true;
            }
            __except(EXCEPTION_EXECUTE_HANDLER) {
                LOG_ERROR("[ModLoader] %s threw during ModInit", mod.name);
            }
        }

        LOG_INFO("[ModLoader] Loaded mod DLL: %s", mod.dll_path);
    }

    s_modDllsLoaded = true;
}

void FilesystemPatch_Shutdown() {
    for (size_t i = s_loadedMods.size(); i > 0; --i) {
        LoadedModEntry& mod = s_loadedMods[i - 1];
        if (!mod.module) {
            continue;
        }

        if (mod.shutdown) {
            __try {
                mod.shutdown();
            }
            __except(EXCEPTION_EXECUTE_HANDLER) {
                LOG_ERROR("[ModLoader] %s threw during ModShutdown", mod.name);
            }
        }

        FreeLibrary(mod.module);
        mod.module = nullptr;
    }

    s_loadedMods.clear();
    s_loggedOverrides.clear();
    s_modDllsLoaded = false;
    s_initialized = false;
    s_gameModule = nullptr;
    s_gameRoot[0] = '\0';
    s_modsRoot[0] = '\0';
    s_configPath[0] = '\0';
}

HANDLE WINAPI Hook_CreateFileA(LPCSTR lpFileName,
                               DWORD dwDesiredAccess,
                               DWORD dwShareMode,
                               LPSECURITY_ATTRIBUTES lpSecAttr,
                               DWORD dwCreationDisposition,
                               DWORD dwFlagsAndAttributes,
                               HANDLE hTemplateFile) {
    if (!lpFileName) {
        return g_origCreateFileA
            ? g_origCreateFileA(lpFileName, dwDesiredAccess, dwShareMode, lpSecAttr,
                                dwCreationDisposition, dwFlagsAndAttributes, hTemplateFile)
            : INVALID_HANDLE_VALUE;
    }

    const char* pathToOpen = lpFileName;
    char overridePath[MAX_PATH] = {};
    const LoadedModEntry* overrideMod = nullptr;
    if (ShouldRedirectReadOpen(dwDesiredAccess, dwCreationDisposition, dwFlagsAndAttributes) &&
        TryResolveModOverridePath(lpFileName, overridePath, sizeof(overridePath), &overrideMod)) {
        pathToOpen = overridePath;
        MaybeLogOverride(lpFileName, overridePath, overrideMod->name);
    }

    HANDLE handle = CreateFileViaWidePath(pathToOpen,
                                          dwDesiredAccess,
                                          dwShareMode,
                                          lpSecAttr,
                                          dwCreationDisposition,
                                          dwFlagsAndAttributes,
                                          hTemplateFile);
    if (handle != INVALID_HANDLE_VALUE || !g_origCreateFileA) {
        return handle;
    }

    return g_origCreateFileA(lpFileName,
                             dwDesiredAccess,
                             dwShareMode,
                             lpSecAttr,
                             dwCreationDisposition,
                             dwFlagsAndAttributes,
                             hTemplateFile);
}

BOOL WINAPI Hook_DeleteFileA(LPCSTR lpFileName) {
    if (!lpFileName) {
        return g_origDeleteFileA ? g_origDeleteFileA(lpFileName) : FALSE;
    }

    const BOOL result = DeleteFileViaWidePath(lpFileName);
    if (result || !g_origDeleteFileA) {
        return result;
    }

    return g_origDeleteFileA(lpFileName);
}

HANDLE WINAPI Hook_FindFirstFileA(LPCSTR lpFileName, LPWIN32_FIND_DATAA lpFindFileData) {
    if (!lpFileName) {
        return g_origFindFirstFileA ? g_origFindFirstFileA(lpFileName, lpFindFileData) : INVALID_HANDLE_VALUE;
    }

    const char* pathToFind = lpFileName;
    char overridePath[MAX_PATH] = {};
    const LoadedModEntry* overrideMod = nullptr;
    if (TryResolveModOverridePath(lpFileName, overridePath, sizeof(overridePath), &overrideMod)) {
        pathToFind = overridePath;
        MaybeLogOverride(lpFileName, overridePath, overrideMod->name);
    }

    HANDLE handle = FindFirstFileViaWidePath(pathToFind, lpFindFileData);
    if (handle != INVALID_HANDLE_VALUE || !g_origFindFirstFileA) {
        return handle;
    }

    return g_origFindFirstFileA(lpFileName, lpFindFileData);
}

DWORD WINAPI Hook_GetFileAttributesA(LPCSTR lpFileName) {
    if (!lpFileName) {
        return g_origGetFileAttributesA ? g_origGetFileAttributesA(lpFileName) : INVALID_FILE_ATTRIBUTES;
    }

    const char* pathToQuery = lpFileName;
    char overridePath[MAX_PATH] = {};
    const LoadedModEntry* overrideMod = nullptr;
    if (TryResolveModOverridePath(lpFileName, overridePath, sizeof(overridePath), &overrideMod)) {
        pathToQuery = overridePath;
        MaybeLogOverride(lpFileName, overridePath, overrideMod->name);
    }

    const DWORD attributes = GetFileAttributesViaWidePath(pathToQuery);
    if (attributes != INVALID_FILE_ATTRIBUTES || !g_origGetFileAttributesA) {
        return attributes;
    }

    return g_origGetFileAttributesA(lpFileName);
}
