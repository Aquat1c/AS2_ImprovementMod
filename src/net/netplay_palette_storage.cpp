#include "net/netplay_palette_storage.h"

#include "patches/memory_utils.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <string.h>

namespace {

using namespace Net;

constexpr int kMaxCharacters = 256;
constexpr int kMaxBasePalettes = NETPLAY_PALETTE_BANK_COUNT;
constexpr const char* kPaletteDir = "custom_palettes";

struct StoredPaletteEntry {
    bool               loaded;
    bool               present;
    NetplayPaletteBank bank;
};

static StoredPaletteEntry s_entries[kMaxCharacters][kMaxBasePalettes] = {};

static bool IsValidBasePalette(uint8_t basePalette) {
    return basePalette < kMaxBasePalettes;
}

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

static bool EnsurePaletteDir() {
    return CreateDirectoryA(kPaletteDir, nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void BuildPalettePath(char* out, size_t outSize, uint8_t characterId, uint8_t basePalette) {
    if (!out || outSize == 0) {
        return;
    }
    _snprintf_s(out,
        outSize,
        _TRUNCATE,
        "%s\\char_%03u_base_%u.bin",
        kPaletteDir,
        (unsigned)characterId,
        (unsigned)basePalette);
}

static void BuildPresetDirPath(char* out, size_t outSize, uint8_t characterId, uint8_t basePalette) {
    if (!out || outSize == 0) {
        return;
    }
    _snprintf_s(out,
        outSize,
        _TRUNCATE,
        "%s\\char_%03u\\base_%u",
        kPaletteDir,
        (unsigned)characterId,
        (unsigned)basePalette);
}

static bool SanitizePresetName(const char* presetName, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return false;
    }

    out[0] = '\0';
    if (!presetName) {
        return false;
    }

    size_t writeIndex = 0;
    while (*presetName && writeIndex + 1 < outSize) {
        const char ch = *presetName++;
        if (ch <= 31 || ch == '<' || ch == '>' || ch == ':' || ch == '"' || ch == '/' ||
            ch == '\\' || ch == '|' || ch == '?' || ch == '*') {
            out[writeIndex++] = '_';
        } else {
            out[writeIndex++] = ch;
        }
    }

    while (writeIndex > 0 && (out[writeIndex - 1] == ' ' || out[writeIndex - 1] == '.')) {
        --writeIndex;
    }
    out[writeIndex] = '\0';
    return writeIndex > 0;
}

static void BuildPresetPath(char* out,
                            size_t outSize,
                            uint8_t characterId,
                            uint8_t basePalette,
                            const char* presetName) {
    if (!out || outSize == 0) {
        return;
    }

    char sanitized[64] = {};
    if (!SanitizePresetName(presetName, sanitized, sizeof(sanitized))) {
        out[0] = '\0';
        return;
    }

    char presetDir[MAX_PATH] = {};
    BuildPresetDirPath(presetDir, sizeof(presetDir), characterId, basePalette);
    _snprintf_s(out,
        outSize,
        _TRUNCATE,
        "%s\\%s.bin",
        presetDir,
        sanitized);
}

static bool EnsurePresetDir(uint8_t characterId, uint8_t basePalette) {
    if (!EnsurePaletteDir()) {
        return false;
    }

    char charDir[MAX_PATH] = {};
    _snprintf_s(charDir,
        sizeof(charDir),
        _TRUNCATE,
        "%s\\char_%03u",
        kPaletteDir,
        (unsigned)characterId);
    if (CreateDirectoryA(charDir, nullptr) == FALSE && GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }

    char presetDir[MAX_PATH] = {};
    BuildPresetDirPath(presetDir, sizeof(presetDir), characterId, basePalette);
    return CreateDirectoryA(presetDir, nullptr) != FALSE || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void FillBank(NetplayPaletteBank* bank,
                     uint8_t characterId,
                     uint8_t basePalette,
                     const uint8_t* data) {
    if (!bank || !data) {
        return;
    }
    memset(bank, 0, sizeof(*bank));
    bank->valid = true;
    bank->character_id = characterId;
    bank->base_palette = basePalette;
    memcpy(bank->data, data, NETPLAY_PALETTE_BANK_SIZE);
    bank->crc32 = CalcCRC32(bank->data, NETPLAY_PALETTE_BANK_SIZE);
}

static bool LoadBankFromPath(const char* path,
                             uint8_t characterId,
                             uint8_t basePalette,
                             NetplayPaletteBank* out) {
    if (!path || !out) {
        return false;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, path, "rb") != 0 || !file) {
        return false;
    }

    uint8_t buffer[NETPLAY_PALETTE_BANK_SIZE] = {};
    const size_t read = fread(buffer, 1, sizeof(buffer), file);
    const int extra = fgetc(file);
    fclose(file);

    if (read != sizeof(buffer) || extra != EOF) {
        return false;
    }

    FillBank(out, characterId, basePalette, buffer);
    return true;
}

static bool LoadEntry(uint8_t characterId, uint8_t basePalette) {
    if (!IsValidBasePalette(basePalette)) {
        return false;
    }

    StoredPaletteEntry& entry = s_entries[characterId][basePalette];
    if (entry.loaded) {
        return entry.present;
    }

    entry.loaded = true;
    entry.present = false;
    memset(&entry.bank, 0, sizeof(entry.bank));

    char path[MAX_PATH] = {};
    BuildPalettePath(path, sizeof(path), characterId, basePalette);
    if (!LoadBankFromPath(path, characterId, basePalette, &entry.bank)) {
        return false;
    }

    entry.present = true;
    return true;
}

} // namespace

namespace Net {

void NetplayPaletteStorage_Init() {
    memset(s_entries, 0, sizeof(s_entries));
    EnsurePaletteDir();
}

void NetplayPaletteStorage_Shutdown() {
    memset(s_entries, 0, sizeof(s_entries));
}

bool NetplayPaletteStorage_GetBank(uint8_t characterId,
                                   uint8_t basePalette,
                                   NetplayPaletteBank* out) {
    if (!out || !IsValidBasePalette(basePalette) || !LoadEntry(characterId, basePalette)) {
        return false;
    }

    *out = s_entries[characterId][basePalette].bank;
    return true;
}

bool NetplayPaletteStorage_SaveBank(const NetplayPaletteBank* bank) {
    if (!bank || !bank->valid || !IsValidBasePalette(bank->base_palette)) {
        return false;
    }

    EnsurePaletteDir();

    char path[MAX_PATH] = {};
    BuildPalettePath(path, sizeof(path), bank->character_id, bank->base_palette);

    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) {
        return false;
    }

    const size_t written = fwrite(bank->data, 1, NETPLAY_PALETTE_BANK_SIZE, file);
    fclose(file);
    if (written != NETPLAY_PALETTE_BANK_SIZE) {
        return false;
    }

    StoredPaletteEntry& entry = s_entries[bank->character_id][bank->base_palette];
    entry.loaded = true;
    entry.present = true;
    entry.bank = *bank;
    return true;
}

bool NetplayPaletteStorage_DeleteBank(uint8_t characterId, uint8_t basePalette) {
    if (!IsValidBasePalette(basePalette)) {
        return false;
    }

    char path[MAX_PATH] = {};
    BuildPalettePath(path, sizeof(path), characterId, basePalette);
    const BOOL deleted = DeleteFileA(path);
    const DWORD err = GetLastError();

    StoredPaletteEntry& entry = s_entries[characterId][basePalette];
    entry.loaded = true;
    entry.present = false;
    memset(&entry.bank, 0, sizeof(entry.bank));

    return deleted != FALSE || err == ERROR_FILE_NOT_FOUND;
}

int NetplayPaletteStorage_ListPresets(uint8_t characterId,
                                      uint8_t basePalette,
                                      NetplayPalettePresetInfo* out,
                                      int capacity) {
    if (!out || capacity <= 0 || !IsValidBasePalette(basePalette)) {
        return 0;
    }

    char presetDir[MAX_PATH] = {};
    BuildPresetDirPath(presetDir, sizeof(presetDir), characterId, basePalette);

    char searchPath[MAX_PATH] = {};
    _snprintf_s(searchPath, sizeof(searchPath), _TRUNCATE, "%s\\*.bin", presetDir);

    WIN32_FIND_DATAA findData = {};
    HANDLE findHandle = FindFirstFileA(searchPath, &findData);
    if (findHandle == INVALID_HANDLE_VALUE) {
        return 0;
    }

    int count = 0;
    do {
        if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            continue;
        }

        char displayName[64] = {};
        CopyText(displayName, sizeof(displayName), findData.cFileName);
        char* extension = strrchr(displayName, '.');
        if (extension) {
            *extension = '\0';
        }
        if (!displayName[0]) {
            continue;
        }

        CopyText(out[count].name, sizeof(out[count].name), displayName);
        ++count;
    } while (count < capacity && FindNextFileA(findHandle, &findData));

    FindClose(findHandle);

    for (int i = 0; i + 1 < count; ++i) {
        for (int j = i + 1; j < count; ++j) {
            if (_stricmp(out[i].name, out[j].name) > 0) {
                NetplayPalettePresetInfo temp = out[i];
                out[i] = out[j];
                out[j] = temp;
            }
        }
    }

    return count;
}

bool NetplayPaletteStorage_HasPreset(uint8_t characterId,
                                     uint8_t basePalette,
                                     const char* presetName) {
    char path[MAX_PATH] = {};
    BuildPresetPath(path, sizeof(path), characterId, basePalette, presetName);
    if (!path[0]) {
        return false;
    }

    const DWORD attrs = GetFileAttributesA(path);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

bool NetplayPaletteStorage_LoadPreset(uint8_t characterId,
                                      uint8_t basePalette,
                                      const char* presetName,
                                      NetplayPaletteBank* out) {
    if (!out || !IsValidBasePalette(basePalette)) {
        return false;
    }

    char path[MAX_PATH] = {};
    BuildPresetPath(path, sizeof(path), characterId, basePalette, presetName);
    if (!path[0]) {
        return false;
    }

    return LoadBankFromPath(path, characterId, basePalette, out);
}

bool NetplayPaletteStorage_SavePreset(const NetplayPaletteBank* bank, const char* presetName) {
    if (!bank || !bank->valid || !IsValidBasePalette(bank->base_palette) || !presetName) {
        return false;
    }

    if (!EnsurePresetDir(bank->character_id, bank->base_palette)) {
        return false;
    }

    char path[MAX_PATH] = {};
    BuildPresetPath(path, sizeof(path), bank->character_id, bank->base_palette, presetName);
    if (!path[0]) {
        return false;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) {
        return false;
    }

    const size_t written = fwrite(bank->data, 1, NETPLAY_PALETTE_BANK_SIZE, file);
    fclose(file);
    return written == NETPLAY_PALETTE_BANK_SIZE;
}

bool NetplayPaletteStorage_DeletePreset(uint8_t characterId,
                                        uint8_t basePalette,
                                        const char* presetName) {
    if (!IsValidBasePalette(basePalette)) {
        return false;
    }

    char path[MAX_PATH] = {};
    BuildPresetPath(path, sizeof(path), characterId, basePalette, presetName);
    if (!path[0]) {
        return false;
    }

    const BOOL deleted = DeleteFileA(path);
    const DWORD err = GetLastError();
    return deleted != FALSE || err == ERROR_FILE_NOT_FOUND;
}

} // namespace Net