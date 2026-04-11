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
constexpr int kMaxBasePalettes = 8;
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

    FillBank(&entry.bank, characterId, basePalette, buffer);
    entry.present = true;
    return true;
}

} // namespace

namespace Net {

void NetplayPaletteStorage_Init() {
    memset(s_entries, 0, sizeof(s_entries));
    CreateDirectoryA(kPaletteDir, nullptr);
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

    CreateDirectoryA(kPaletteDir, nullptr);

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

} // namespace Net