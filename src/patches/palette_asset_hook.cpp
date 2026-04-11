#include "patches/palette_asset_hook.h"

#include "core/as2_constants.h"
#include "net/netplay_palette_runtime.h"
#include "patches/memory_utils.h"
#include "rollback/netplay_log.h"
#include "ui/log_window.h"
#include "MinHook.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <stdio.h>
#include <stdarg.h>
#include <string.h>

namespace {

using namespace Net;

constexpr size_t kPatchBufferSize = 0x3200;
constexpr const char* kTempDir = "custom_palettes";
constexpr const char* kPaletteDumpDirName = "palette_dumps";
constexpr uint8_t kMaxCharacterPaletteBanks = 12;
constexpr size_t kDecodedBmpPaletteOffset = 53;
constexpr size_t kRuntimeFormatPaletteOffset = 32;
constexpr size_t kRuntimeFormatWidthOffset = 1064;
constexpr size_t kRuntimeFormatHeightOffset = 1068;
constexpr size_t kHandleWrapperObjectOffset = 20;
constexpr size_t kImageObjectIndexedFlagOffset = 32;
constexpr size_t kImageObjectPalettePrimaryOffset = 56;
constexpr size_t kImageObjectPaletteSecondaryOffset = 60;
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

using AssetLoadAllFromArchive_t = void (__cdecl *)(int* pOutHandles,
                                                   char* archiveFileName,
                                                   const char* patchFileName,
                                                   int patchIndex);
using ImageCreateFromDecodedBmp_t = int (__cdecl *)(int decodedBmp,
                                                    int decodedBmpBytes,
                                                    int a3,
                                                    int a4,
                                                    int a5,
                                                    int a6);
using ImageRegisterHandle_t = int (__cdecl *)(int primaryFormat,
                                              int secondaryFormat,
                                              int usageKind);
using HandleRenderBind_t = int (__cdecl *)(uint16_t* a1,
                                           void* a2,
                                           void* a3,
                                           uint8_t* a4,
                                           int a5,
                                           unsigned __int16* a6,
                                           uint32_t* a7,
                                           int a8,
                                           int a9,
                                           int a10,
                                           int a11);
using ImageUploadToHandle_t = int* (__cdecl *)(int format,
                                               int* clipRect,
                                               int handlePtr,
                                               int offsetX,
                                               int offsetY,
                                               int* srcRect,
                                               int* dstRect,
                                               int drawX,
                                               int drawY,
                                               int flags);
using HandleFree_t = int (__cdecl *)(int handle);

struct SlotLoadState {
    bool     valid;
    int*     handle_table;
    uint16_t asset_count;
    uint8_t  base_palette;
    char     archive_path[MAX_PATH];
    char     patch_path[MAX_PATH];
    char     temp_patch_path[MAX_PATH];
};

struct ActiveLoadState {
    bool     valid;
    bool     live_bank_observed;
    bool     decoded_bmp_stage_seen;
    bool     handle_create_stage_seen;
    bool     handle_render_bind_stage_seen;
    bool     late_upload_stage_seen;
    uint8_t  game_slot;
    uint8_t  base_palette;
    uint16_t asset_count;
    int      decoded_bmp_ptr;
    int      decoded_bmp_bytes;
    int      created_handle;
    int      created_handle_usage_kind;
    int      render_bind_handle;
    int      render_bind_result;
    int      late_upload_format;
    int      late_upload_result;
    const void* created_handle_entry;
    const void* created_handle_resource;
    const void* late_upload_handle_ptr;
    const void* late_upload_object_ptr;
    const void* late_upload_palette_ptr;
    char     archive_path[MAX_PATH];
    char     patch_path[MAX_PATH];
};

static AssetLoadAllFromArchive_t s_originalAssetLoad = nullptr;
static ImageCreateFromDecodedBmp_t s_originalImageCreateFromDecodedBmp = nullptr;
static ImageRegisterHandle_t    s_originalImageRegisterHandle = nullptr;
static HandleRenderBind_t       s_originalHandleRenderBind = nullptr;
static ImageUploadToHandle_t     s_originalImageUploadToHandle = nullptr;
static HandleFree_t              s_handleFree = reinterpret_cast<HandleFree_t>(ADDR_HANDLE_FREE);
static SlotLoadState             s_slotState[2] = {};
static ActiveLoadState           s_activeLoad = {};
static bool                      s_insideAssetLoad = false;
static int                       s_unmatchedAssetLoadLogs = 0;
static bool                      s_loggedResearchAddresses = false;

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

static bool ExtractFileStem(const char* path, char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return false;
    }

    out[0] = '\0';
    if (!path || !path[0]) {
        return false;
    }

    const char* stem = path;
    for (const char* scan = path; *scan; ++scan) {
        if (*scan == '\\' || *scan == '/') {
            stem = scan + 1;
        }
    }

    size_t stemLen = 0;
    while (stem[stemLen] && stem[stemLen] != '.' && stemLen + 1 < outSize) {
        out[stemLen] = stem[stemLen];
        ++stemLen;
    }
    out[stemLen] = '\0';
    return stemLen > 0;
}

static void LogResearchEvent(const char* stage, const char* fmt, ...) {
    if (!stage || !fmt) {
        return;
    }

    char buffer[1024] = {};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf_s(buffer, sizeof(buffer), _TRUNCATE, fmt, ap);
    va_end(ap);

    LOG_INFO("[Palette][Research] %s: %s", stage, buffer);
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Research %s: %s",
        stage,
        buffer);
}

static const int* ResolveHandleEntry(int handle) {
    if (handle < 0 || (handle & 0x78000000) != 0x08000000) {
        return nullptr;
    }

    const int* const* handleTable = reinterpret_cast<const int* const*>(ADDR_HANDLE_TABLE);
    const int* entry = handleTable[handle & 0xFFFF];
    if (!entry || (entry[0] << 16) != (handle & 0x07FF0000)) {
        return nullptr;
    }
    return entry;
}

static bool TryGetRuntimePaletteFromFormat(int imageFormat, const void** outPalette, uint32_t* outCrc) {
    if (outPalette) {
        *outPalette = nullptr;
    }
    if (outCrc) {
        *outCrc = 0;
    }
    if (imageFormat == 0 || *reinterpret_cast<const uint16_t*>(imageFormat + 2) != 1) {
        return false;
    }

    const void* palette = reinterpret_cast<const void*>(imageFormat + kRuntimeFormatPaletteOffset);
    if (outPalette) {
        *outPalette = palette;
    }
    if (outCrc) {
        *outCrc = CalcCRC32(palette, NETPLAY_PALETTE_BANK_SIZE);
    }
    return true;
}

static void CaptureCreatedHandleInfo(int handle) {
    s_activeLoad.created_handle_entry = nullptr;
    s_activeLoad.created_handle_resource = nullptr;

    const int* entry = ResolveHandleEntry(handle);
    if (!entry) {
        return;
    }

    s_activeLoad.created_handle_entry = entry;
    s_activeLoad.created_handle_resource = reinterpret_cast<const void*>(entry[3]);
}

static void LogReloadResearchAddresses() {
    if (s_loggedResearchAddresses) {
        return;
    }

    s_loggedResearchAddresses = true;
    LogResearchEvent("address_map", "asset_load_all_from_archive=0x%08X role=archive decode loop + palette patch injection", ADDR_ASSET_LOAD_ALL_FROM_ARCHIVE);
    LogResearchEvent("address_map", "image_parse_from_buffer=0x%08X role=buffer parser front door (sub_63E730)", ADDR_IMAGE_PARSE_FROM_BUFFER);
    LogResearchEvent("address_map", "image_create_from_decoded_bmp=0x%08X role=decoded BMP -> image/handle bridge (sub_63A9B0)", ADDR_IMAGE_CREATE_FROM_DECODED_BMP);
    LogResearchEvent("address_map", "image_create_from_format=0x%08X role=pixelformat -> live handle creation (sub_63AB70)", ADDR_IMAGE_CREATE_FROM_FORMAT);
    LogResearchEvent("address_map", "image_register_handle=0x%08X role=allocates live handle from parsed image (sub_620930)", ADDR_IMAGE_REGISTER_HANDLE);
    LogResearchEvent("address_map", "handle_render_bind=0x%08X role=uploads/binds image into render handle (sub_6132E0)", ADDR_HANDLE_RENDER_BIND);
    LogResearchEvent("address_map", "handle_alloc=0x%08X role=raw handle allocator (sub_612DF0)", ADDR_HANDLE_ALLOC);
    LogResearchEvent("address_map", "handle_set_source=0x%08X role=names/copies handle source linkage (sub_6207A0)", ADDR_HANDLE_SET_SOURCE);
    LogResearchEvent("address_map", "image_create_surface=0x%08X role=image object allocation with palette slots (Image_Create)", ADDR_IMAGE_CREATE_SURFACE);
    LogResearchEvent("address_map", "image_create_subrect=0x%08X role=subhandle construction for asset pieces (Image_CreateSubRect)", ADDR_IMAGE_CREATE_SUBRECT);
    LogResearchEvent("address_map", "pixel_format_build=0x%08X role=constructs format descriptors for runtime image objects (sub_646770)", ADDR_PIXELFORMAT_BUILD);
    LogResearchEvent("address_map", "image_upload_to_handle=0x%08X role=late handle blit/upload helper (sub_6460C0)", ADDR_IMAGE_UPLOAD_TO_HANDLE);
    LogResearchEvent("address_map", "handle_free=0x%08X role=tears down live handle for reload replacement (Handle_Free)", ADDR_HANDLE_FREE);
    LogResearchEvent("address_map", "handle_table=0x%08X role=global live handle table base", ADDR_HANDLE_TABLE);
    LogResearchEvent("address_map", "handle_system_active=0x%08X role=global handle system gate", ADDR_HANDLE_SYSTEM_ACTIVE);
    LogResearchEvent("offsets", "decoded_bmp_palette=%u runtime_format_palette=%u handle_wrapper_object=%u image_object_indexed_flag=%u image_object_palette_a=%u image_object_palette_b=%u",
        (unsigned)kDecodedBmpPaletteOffset,
        (unsigned)kRuntimeFormatPaletteOffset,
        (unsigned)kHandleWrapperObjectOffset,
        (unsigned)kImageObjectIndexedFlagOffset,
        (unsigned)kImageObjectPalettePrimaryOffset,
        (unsigned)kImageObjectPaletteSecondaryOffset);
}

static int ResolveCharacterArchiveId(const char* archivePath) {
    char stem[16] = {};
    if (!ExtractFileStem(archivePath, stem, sizeof(stem))) {
        return -1;
    }

    for (int i = 0; i < (int)(sizeof(kCharacterArchiveStemById) / sizeof(kCharacterArchiveStemById[0])); ++i) {
        if (_stricmp(stem, kCharacterArchiveStemById[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static bool IsMatchingCharacterPatchPath(const char* patchPath, int characterId) {
    if (characterId < 0 ||
        characterId >= (int)(sizeof(kCharacterArchiveStemById) / sizeof(kCharacterArchiveStemById[0]))) {
        return false;
    }

    char stem[16] = {};
    return ExtractFileStem(patchPath, stem, sizeof(stem)) &&
        _stricmp(stem, kCharacterArchiveStemById[characterId]) == 0;
}

static int* ResolveHandleTable(uint8_t gameSlot) {
    switch (gameSlot) {
        case 0:
            return reinterpret_cast<int*>(ADDR_P1_ENTITY_BASE + ENTITY_OFF_ASSET_HANDLE_TABLE);
        case 1:
            return reinterpret_cast<int*>(ADDR_P2_ENTITY_BASE + ENTITY_OFF_ASSET_HANDLE_TABLE);
        default:
            return nullptr;
    }
}

static int ResolveGameSlot(int* pOutHandles) {
    for (int slot = 0; slot < 2; slot++) {
        if (pOutHandles == ResolveHandleTable((uint8_t)slot)) {
            return slot;
        }
    }
    return -1;
}

static void LogResolvedHandleTables() {
    LOG_INFO("[Palette] Handle tables: P1=%p P2=%p",
        ResolveHandleTable(0),
        ResolveHandleTable(1));
}

static void EnsureTempDir() {
    CreateDirectoryA(kTempDir, nullptr);
}

static bool EnsurePaletteDumpDir(char* out, size_t outSize) {
    if (!out || outSize == 0) {
        return false;
    }

    out[0] = '\0';
    const char* logDir = LogWindow_GetLogDir();
    if (!logDir || !logDir[0]) {
        return false;
    }

    _snprintf_s(out,
        outSize,
        _TRUNCATE,
        "%s\\%s",
        logDir,
        kPaletteDumpDirName);
    CreateDirectoryA(out, nullptr);
    return true;
}

static bool BuildPaletteDumpBasePath(char* out,
                                     size_t outSize,
                                     const char* label,
                                     uint8_t gameSlot,
                                     int characterId,
                                     uint8_t basePalette,
                                     const char* archivePath) {
    if (!out || outSize == 0) {
        return false;
    }

    char dumpDir[MAX_PATH] = {};
    if (!EnsurePaletteDumpDir(dumpDir, sizeof(dumpDir))) {
        return false;
    }

    char stem[16] = {};
    if (!ExtractFileStem(archivePath, stem, sizeof(stem))) {
        strncpy_s(stem, sizeof(stem), "unknown", _TRUNCATE);
    }

    _snprintf_s(out,
        outSize,
        _TRUNCATE,
        "%s\\%s_p%d_%s_char%02u_base%u",
        dumpDir,
        label ? label : "palette",
        gameSlot + 1,
        stem,
        (unsigned)(characterId >= 0 ? characterId : 255),
        (unsigned)basePalette);
    return true;
}

static void BuildDumpPath(char* out, size_t outSize, const char* basePath, const char* suffix) {
    if (!out || outSize == 0) {
        return;
    }
    _snprintf_s(out,
        outSize,
        _TRUNCATE,
        "%s%s",
        basePath ? basePath : "",
        suffix ? suffix : "");
}

static bool WriteBinaryDump(const char* path, const void* data, size_t size) {
    if (!path || !path[0] || !data || size == 0) {
        return false;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || !file) {
        return false;
    }

    const size_t written = fwrite(data, 1, size, file);
    fclose(file);
    return written == size;
}

static bool WriteTextDump(const char* path, const char* fmt, ...) {
    if (!path || !path[0] || !fmt) {
        return false;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, path, "w") != 0 || !file) {
        return false;
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(file, fmt, ap);
    va_end(ap);
    fclose(file);
    return true;
}

static void DumpPatchArtifacts(const char* label,
                               uint8_t gameSlot,
                               const char* archivePath,
                               const char* patchPath,
                               uint8_t patchIndex,
                               uint16_t assetCount,
                               const uint8_t* patchBuffer,
                               size_t patchBytes,
                               const uint8_t* runtimeBank) {
    if (!patchBuffer || patchBytes == 0 || !runtimeBank) {
        return;
    }

    const size_t bankOffset = (size_t)patchIndex * NETPLAY_PALETTE_BANK_SIZE;
    if (bankOffset + NETPLAY_PALETTE_BANK_SIZE > patchBytes) {
        return;
    }

    const int characterId = ResolveCharacterArchiveId(archivePath);
    char basePath[MAX_PATH] = {};
    if (!BuildPaletteDumpBasePath(basePath,
            sizeof(basePath),
            label,
            gameSlot,
            characterId,
            patchIndex,
            archivePath)) {
        return;
    }

    const uint8_t* rawBank = patchBuffer + bankOffset;
    const uint32_t fileCrc = CalcCRC32(patchBuffer, patchBytes);
    const uint32_t rawBankCrc = CalcCRC32(rawBank, NETPLAY_PALETTE_BANK_SIZE);
    const uint32_t runtimeBankCrc = CalcCRC32(runtimeBank, NETPLAY_PALETTE_BANK_SIZE);

    char filePath[MAX_PATH] = {};
    char rawBankPath[MAX_PATH] = {};
    char runtimeBankPath[MAX_PATH] = {};
    char summaryPath[MAX_PATH] = {};
    BuildDumpPath(filePath, sizeof(filePath), basePath, "_file.bin");
    BuildDumpPath(rawBankPath, sizeof(rawBankPath), basePath, "_bank_raw.bin");
    BuildDumpPath(runtimeBankPath, sizeof(runtimeBankPath), basePath, "_bank_runtime.bin");
    BuildDumpPath(summaryPath, sizeof(summaryPath), basePath, "_summary.txt");

    const bool wroteFile = WriteBinaryDump(filePath, patchBuffer, patchBytes);
    const bool wroteRawBank = WriteBinaryDump(rawBankPath, rawBank, NETPLAY_PALETTE_BANK_SIZE);
    const bool wroteRuntimeBank = WriteBinaryDump(runtimeBankPath, runtimeBank, NETPLAY_PALETTE_BANK_SIZE);
    const bool wroteSummary = WriteTextDump(summaryPath,
        "PALETTE PATCH DUMP\n"
        "label=%s\n"
        "slot=P%d\n"
        "character_id=%d\n"
        "base_palette=%u\n"
        "asset_count=%u\n"
        "archive=%s\n"
        "patch=%s\n"
        "file_bytes=%zu\n"
        "file_crc32=0x%08X\n"
        "raw_bank_crc32=0x%08X\n"
        "runtime_bank_crc32=0x%08X\n"
        "file_dump=%s\n"
        "raw_bank_dump=%s\n"
        "runtime_bank_dump=%s\n",
        label ? label : "patch",
        gameSlot + 1,
        characterId,
        (unsigned)patchIndex,
        (unsigned)assetCount,
        archivePath ? archivePath : "(null)",
        patchPath ? patchPath : "(null)",
        patchBytes,
        fileCrc,
        rawBankCrc,
        runtimeBankCrc,
        wroteFile ? filePath : "write failed",
        wroteRawBank ? rawBankPath : "write failed",
        wroteRuntimeBank ? runtimeBankPath : "write failed");

    LOG_INFO("[Palette] Dumped %s patch artifacts slot=P%d char=%d base=%u summary=%s ok=[%d/%d/%d/%d]",
        label ? label : "patch",
        gameSlot + 1,
        characterId,
        (unsigned)patchIndex,
        summaryPath,
        wroteFile ? 1 : 0,
        wroteRawBank ? 1 : 0,
        wroteRuntimeBank ? 1 : 0,
        wroteSummary ? 1 : 0);
    Rollback::NetplayLog_Write("PALETTE", -1,
        "%s patch dump: slot=P%d char=%d base=%u file_crc=0x%08X runtime_crc=0x%08X summary=%s",
        label ? label : "patch",
        gameSlot + 1,
        characterId,
        (unsigned)patchIndex,
        fileCrc,
        runtimeBankCrc,
        summaryPath);
}

static void DumpObservedLivePalette(uint8_t gameSlot,
                                    uint8_t basePalette,
                                    uint16_t assetCount,
                                    const char* archivePath,
                                    const char* patchPath,
                                    const void* runtimeBank,
                                    const char* observationSource,
                                    const void* observedPtr) {
    if (!runtimeBank) {
        return;
    }

    const int characterId = ResolveCharacterArchiveId(archivePath);
    char basePath[MAX_PATH] = {};
    if (!BuildPaletteDumpBasePath(basePath,
            sizeof(basePath),
            "live",
            gameSlot,
            characterId,
            basePalette,
            archivePath)) {
        return;
    }

    char bankPath[MAX_PATH] = {};
    char summaryPath[MAX_PATH] = {};
    BuildDumpPath(bankPath, sizeof(bankPath), basePath, "_bank_runtime.bin");
    BuildDumpPath(summaryPath, sizeof(summaryPath), basePath, "_summary.txt");

    const uint32_t runtimeBankCrc = CalcCRC32(runtimeBank, NETPLAY_PALETTE_BANK_SIZE);
    const bool wroteBank = WriteBinaryDump(bankPath, runtimeBank, NETPLAY_PALETTE_BANK_SIZE);
    const bool wroteSummary = WriteTextDump(summaryPath,
        "PALETTE LIVE MEMORY DUMP\n"
        "slot=P%d\n"
        "character_id=%d\n"
        "base_palette=%u\n"
        "asset_count=%u\n"
        "archive=%s\n"
        "patch=%s\n"
        "observation_source=%s\n"
        "observed_ptr=%p\n"
        "runtime_bank_crc32=0x%08X\n"
        "bank_dump=%s\n",
        gameSlot + 1,
        characterId,
        (unsigned)basePalette,
        (unsigned)assetCount,
        archivePath ? archivePath : "(null)",
        patchPath ? patchPath : "(null)",
        observationSource ? observationSource : "unknown",
        observedPtr,
        runtimeBankCrc,
        wroteBank ? bankPath : "write failed");

    LOG_INFO("[Palette] Dumped live memory palette slot=P%d char=%d base=%u summary=%s ok=[%d/%d]",
        gameSlot + 1,
        characterId,
        (unsigned)basePalette,
        summaryPath,
        wroteBank ? 1 : 0,
        wroteSummary ? 1 : 0);
    Rollback::NetplayLog_Write("PALETTE", -1,
        "Live memory dump: slot=P%d char=%d base=%u crc=0x%08X summary=%s",
        gameSlot + 1,
        characterId,
        (unsigned)basePalette,
        runtimeBankCrc,
        summaryPath);
}

static void DumpMissingLiveObservation(uint8_t gameSlot,
                                       uint8_t basePalette,
                                       uint16_t assetCount,
                                       const char* archivePath,
                                       const char* patchPath,
                                       const void* handles) {
    const int characterId = ResolveCharacterArchiveId(archivePath);
    char basePath[MAX_PATH] = {};
    if (!BuildPaletteDumpBasePath(basePath,
            sizeof(basePath),
            "live_missing",
            gameSlot,
            characterId,
            basePalette,
            archivePath)) {
        return;
    }

    char summaryPath[MAX_PATH] = {};
    BuildDumpPath(summaryPath, sizeof(summaryPath), basePath, "_summary.txt");
    const bool wroteSummary = WriteTextDump(summaryPath,
        "PALETTE LIVE MEMORY STATUS\n"
        "slot=P%d\n"
        "character_id=%d\n"
        "base_palette=%u\n"
        "asset_count=%u\n"
        "archive=%s\n"
        "patch=%s\n"
        "handle_table=%p\n"
        "observation_sources=decoded_bmp_palette,handle_object_palette\n"
        "observed_live_palette=0\n",
        gameSlot + 1,
        characterId,
        (unsigned)basePalette,
        (unsigned)assetCount,
        archivePath ? archivePath : "(null)",
        patchPath ? patchPath : "(null)",
        handles);
    if (wroteSummary) {
        LOG_INFO("[Palette] Wrote missing live observation note slot=P%d char=%d base=%u summary=%s",
            gameSlot + 1,
            characterId,
            (unsigned)basePalette,
            summaryPath);
    }
}

static void DumpReloadResearchSummary(uint8_t gameSlot,
                                      uint8_t basePalette,
                                      uint16_t assetCount,
                                      const char* archivePath,
                                      const char* patchPath,
                                      const void* handles) {
    const int characterId = ResolveCharacterArchiveId(archivePath);
    char basePath[MAX_PATH] = {};
    if (!BuildPaletteDumpBasePath(basePath,
            sizeof(basePath),
            "research",
            gameSlot,
            characterId,
            basePalette,
            archivePath)) {
        return;
    }

    char summaryPath[MAX_PATH] = {};
    BuildDumpPath(summaryPath, sizeof(summaryPath), basePath, "_summary.txt");

    const bool wroteSummary = WriteTextDump(summaryPath,
        "PALETTE RELOAD RESEARCH\n"
        "slot=P%d\n"
        "character_id=%d\n"
        "base_palette=%u\n"
        "asset_count=%u\n"
        "archive=%s\n"
        "patch=%s\n"
        "handle_table=%p\n"
        "observed_live_palette=%d\n"
        "decoded_bmp_stage_seen=%d\n"
        "decoded_bmp_ptr=%p\n"
        "decoded_bmp_bytes=%d\n"
        "handle_create_stage_seen=%d\n"
        "created_handle=0x%08X\n"
        "created_handle_usage_kind=%d\n"
        "created_handle_entry=%p\n"
        "created_handle_resource=%p\n"
        "handle_render_bind_stage_seen=%d\n"
        "render_bind_handle=0x%08X\n"
        "render_bind_result=%d\n"
        "late_upload_stage_seen=%d\n"
        "late_upload_format=%d\n"
        "late_upload_result=%d\n"
        "late_upload_handle_ptr=%p\n"
        "late_upload_object_ptr=%p\n"
        "late_upload_palette_ptr=%p\n"
        "candidate_asset_load_all_from_archive=0x%08X\n"
        "candidate_image_parse_from_buffer=0x%08X\n"
        "candidate_image_create_from_decoded_bmp=0x%08X\n"
        "candidate_image_create_from_format=0x%08X\n"
        "candidate_image_register_handle=0x%08X\n"
        "candidate_handle_render_bind=0x%08X\n"
        "candidate_handle_alloc=0x%08X\n"
        "candidate_handle_set_source=0x%08X\n"
        "candidate_image_create_surface=0x%08X\n"
        "candidate_image_create_subrect=0x%08X\n"
        "candidate_pixel_format_build=0x%08X\n"
        "candidate_image_upload_to_handle=0x%08X\n"
        "candidate_handle_free=0x%08X\n"
        "candidate_handle_table=0x%08X\n"
        "candidate_handle_system_active=0x%08X\n"
        "offset_decoded_bmp_palette=%u\n"
        "offset_runtime_format_palette=%u\n"
        "offset_handle_wrapper_object=%u\n"
        "offset_image_object_indexed_flag=%u\n"
        "offset_image_object_palette_a=%u\n"
        "offset_image_object_palette_b=%u\n",
        gameSlot + 1,
        characterId,
        (unsigned)basePalette,
        (unsigned)assetCount,
        archivePath ? archivePath : "(null)",
        patchPath ? patchPath : "(null)",
        handles,
        s_activeLoad.live_bank_observed ? 1 : 0,
        s_activeLoad.decoded_bmp_stage_seen ? 1 : 0,
        reinterpret_cast<const void*>(s_activeLoad.decoded_bmp_ptr),
        s_activeLoad.decoded_bmp_bytes,
        s_activeLoad.handle_create_stage_seen ? 1 : 0,
        s_activeLoad.created_handle,
        s_activeLoad.created_handle_usage_kind,
        s_activeLoad.created_handle_entry,
        s_activeLoad.created_handle_resource,
        s_activeLoad.handle_render_bind_stage_seen ? 1 : 0,
        s_activeLoad.render_bind_handle,
        s_activeLoad.render_bind_result,
        s_activeLoad.late_upload_stage_seen ? 1 : 0,
        s_activeLoad.late_upload_format,
        s_activeLoad.late_upload_result,
        s_activeLoad.late_upload_handle_ptr,
        s_activeLoad.late_upload_object_ptr,
        s_activeLoad.late_upload_palette_ptr,
        ADDR_ASSET_LOAD_ALL_FROM_ARCHIVE,
        ADDR_IMAGE_PARSE_FROM_BUFFER,
        ADDR_IMAGE_CREATE_FROM_DECODED_BMP,
        ADDR_IMAGE_CREATE_FROM_FORMAT,
        ADDR_IMAGE_REGISTER_HANDLE,
        ADDR_HANDLE_RENDER_BIND,
        ADDR_HANDLE_ALLOC,
        ADDR_HANDLE_SET_SOURCE,
        ADDR_IMAGE_CREATE_SURFACE,
        ADDR_IMAGE_CREATE_SUBRECT,
        ADDR_PIXELFORMAT_BUILD,
        ADDR_IMAGE_UPLOAD_TO_HANDLE,
        ADDR_HANDLE_FREE,
        ADDR_HANDLE_TABLE,
        ADDR_HANDLE_SYSTEM_ACTIVE,
        (unsigned)kDecodedBmpPaletteOffset,
        (unsigned)kRuntimeFormatPaletteOffset,
        (unsigned)kHandleWrapperObjectOffset,
        (unsigned)kImageObjectIndexedFlagOffset,
        (unsigned)kImageObjectPalettePrimaryOffset,
        (unsigned)kImageObjectPaletteSecondaryOffset);

    if (wroteSummary) {
        LOG_INFO("[Palette] Wrote reload research summary slot=P%d char=%d base=%u summary=%s",
            gameSlot + 1,
            characterId,
            (unsigned)basePalette,
            summaryPath);
        Rollback::NetplayLog_Write("PALETTE", -1,
            "Reload research summary: slot=P%d char=%d base=%u summary=%s",
            gameSlot + 1,
            characterId,
            (unsigned)basePalette,
            summaryPath);
    }
}

static void BuildTempPatchPath(char* out, size_t outSize, uint8_t gameSlot) {
    if (!out || outSize == 0) {
        return;
    }
    _snprintf_s(out,
        outSize,
        _TRUNCATE,
        "%s\\runtime_slot_%u.pal",
        kTempDir,
        (unsigned)gameSlot);
}

static bool ReadArchiveAssetCount(const char* archivePath, uint16_t* outCount) {
    if (!archivePath || !outCount) {
        return false;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, archivePath, "rb") != 0 || !file) {
        return false;
    }

    uint32_t encodedCount = 0;
    const bool ok = fread(&encodedCount, sizeof(encodedCount), 1, file) == 1;
    fclose(file);
    if (!ok) {
        return false;
    }

    encodedCount ^= 0x30810400u;
    if (encodedCount > 0xFFFFu) {
        encodedCount = 0xFFFFu;
    }
    *outCount = (uint16_t)encodedCount;
    return true;
}

static bool ReadPatchFile(const char* patchPath,
                          uint8_t* buffer,
                          size_t bufferSize,
                          size_t* outBytesRead) {
    if (!patchPath || !buffer || bufferSize != kPatchBufferSize) {
        return false;
    }

    memset(buffer, 0, bufferSize);
    if (outBytesRead) {
        *outBytesRead = 0;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, patchPath, "rb") != 0 || !file) {
        return false;
    }

    const size_t read = fread(buffer, 1, bufferSize, file);
    const bool ok = ferror(file) == 0;
    fclose(file);
    if (outBytesRead) {
        *outBytesRead = read;
    }
    return ok && read > 0;
}

static bool WritePatchFile(const char* patchPath, const uint8_t* buffer, size_t bufferSize) {
    if (!patchPath || !buffer || bufferSize == 0 || bufferSize > kPatchBufferSize) {
        return false;
    }

    FILE* file = nullptr;
    if (fopen_s(&file, patchPath, "wb") != 0 || !file) {
        return false;
    }

    const size_t written = fwrite(buffer, 1, bufferSize, file);
    fclose(file);
    return written == bufferSize;
}

static void ConvertPatchBankToRuntime(const uint8_t* patchBank, uint8_t* runtimeBank) {
    if (!patchBank || !runtimeBank) {
        return;
    }

    // Character .pal banks are stored on disk as [A, B, G, R] quads.
    // Normalize to the runtime/editor BGRA layout used by the game image objects.
    for (size_t offset = 0; offset < NETPLAY_PALETTE_BANK_SIZE; offset += 4) {
        runtimeBank[offset + 0] = patchBank[offset + 1];
        runtimeBank[offset + 1] = patchBank[offset + 2];
        runtimeBank[offset + 2] = patchBank[offset + 3];
        runtimeBank[offset + 3] = patchBank[offset + 0];
    }
}

static void ConvertRuntimeBankToPatch(const uint8_t* runtimeBank, uint8_t* patchBank) {
    if (!runtimeBank || !patchBank) {
        return;
    }

    for (size_t offset = 0; offset < NETPLAY_PALETTE_BANK_SIZE; offset += 4) {
        patchBank[offset + 0] = runtimeBank[offset + 3];
        patchBank[offset + 1] = runtimeBank[offset + 0];
        patchBank[offset + 2] = runtimeBank[offset + 1];
        patchBank[offset + 3] = runtimeBank[offset + 2];
    }
}

static void ObserveLivePaletteFromHandle(int handlePtr) {
    if (!s_activeLoad.valid || handlePtr == 0) {
        return;
    }

    const int object = *reinterpret_cast<const int*>(handlePtr + kHandleWrapperObjectOffset);
    const int indexedFlag = object != 0
        ? *reinterpret_cast<const int*>(object + kImageObjectIndexedFlagOffset)
        : -1;
    const void* palette = (object != 0 && indexedFlag == 1)
        ? *reinterpret_cast<void* const*>(object + kImageObjectPalettePrimaryOffset)
        : nullptr;

    if (!s_activeLoad.late_upload_stage_seen) {
        s_activeLoad.late_upload_stage_seen = true;
        s_activeLoad.late_upload_handle_ptr = reinterpret_cast<const void*>(handlePtr);
        s_activeLoad.late_upload_object_ptr = reinterpret_cast<const void*>(object);
        s_activeLoad.late_upload_palette_ptr = palette;
        LogResearchEvent("late_upload",
            "addr=0x%08X handle_ptr=%p object=%p indexed_flag=%d palette_a=%p palette_b=%p",
            ADDR_IMAGE_UPLOAD_TO_HANDLE,
            reinterpret_cast<const void*>(handlePtr),
            reinterpret_cast<const void*>(object),
            indexedFlag,
            palette,
            object != 0 ? *reinterpret_cast<void* const*>(object + kImageObjectPaletteSecondaryOffset) : nullptr);
    }

    if (s_activeLoad.live_bank_observed || !palette) {
        return;
    }

    s_activeLoad.live_bank_observed = true;
    DumpObservedLivePalette(
        s_activeLoad.game_slot,
        s_activeLoad.base_palette,
        s_activeLoad.asset_count,
        s_activeLoad.archive_path,
        s_activeLoad.patch_path,
        palette,
        "handle_object_palette",
        palette);
    LOG_INFO("[Palette] Handle palette observed slot=P%d base=%u asset_count=%u archive=%s handle=%p object=%p",
        s_activeLoad.game_slot + 1,
        (unsigned)s_activeLoad.base_palette,
        (unsigned)s_activeLoad.asset_count,
        s_activeLoad.archive_path,
        reinterpret_cast<const void*>(handlePtr),
        reinterpret_cast<const void*>(object));
    NetplayPaletteRuntime_OnLiveBankObserved(
        s_activeLoad.game_slot,
        s_activeLoad.base_palette,
        palette,
        NETPLAY_PALETTE_BANK_SIZE,
        s_activeLoad.asset_count,
        s_activeLoad.archive_path,
        s_activeLoad.patch_path);
}

static void ObserveLivePaletteFromDecodedBmp(int decodedBmp, int decodedBmpBytes) {
    if (!s_activeLoad.valid || decodedBmp == 0) {
        return;
    }

    const size_t requiredBytes = kDecodedBmpPaletteOffset + NETPLAY_PALETTE_BANK_SIZE;
    if (decodedBmpBytes < 0 || (size_t)decodedBmpBytes < requiredBytes) {
        return;
    }

    const uint8_t* bmp = reinterpret_cast<const uint8_t*>(decodedBmp);
    if (!s_activeLoad.decoded_bmp_stage_seen) {
        s_activeLoad.decoded_bmp_stage_seen = true;
        s_activeLoad.decoded_bmp_ptr = decodedBmp;
        s_activeLoad.decoded_bmp_bytes = decodedBmpBytes;
        const uint8_t* rawPalette = (size_t)decodedBmpBytes >= requiredBytes
            ? bmp + kDecodedBmpPaletteOffset
            : nullptr;
        LogResearchEvent("decoded_bmp",
            "addr=0x%08X bmp=%p bytes=%d bitcount=%u raw_palette=%p raw_crc=0x%08X",
            ADDR_IMAGE_CREATE_FROM_DECODED_BMP,
            reinterpret_cast<const void*>(decodedBmp),
            decodedBmpBytes,
            (unsigned)bmp[28],
            rawPalette,
            rawPalette ? CalcCRC32(rawPalette, NETPLAY_PALETTE_BANK_SIZE) : 0u);
    }

    if (s_activeLoad.live_bank_observed) {
        return;
    }

    if (bmp[28] != 8) {
        return;
    }

    const uint8_t* rawPalette = bmp + kDecodedBmpPaletteOffset;
    uint8_t runtimeBank[NETPLAY_PALETTE_BANK_SIZE] = {};
    ConvertPatchBankToRuntime(rawPalette, runtimeBank);

    s_activeLoad.live_bank_observed = true;
    DumpObservedLivePalette(
        s_activeLoad.game_slot,
        s_activeLoad.base_palette,
        s_activeLoad.asset_count,
        s_activeLoad.archive_path,
        s_activeLoad.patch_path,
        runtimeBank,
        "decoded_bmp_palette",
        rawPalette);
    LOG_INFO("[Palette] Decoded BMP palette observed slot=P%d base=%u asset_count=%u archive=%s bmp=%p bytes=%d",
        s_activeLoad.game_slot + 1,
        (unsigned)s_activeLoad.base_palette,
        (unsigned)s_activeLoad.asset_count,
        s_activeLoad.archive_path,
        reinterpret_cast<const void*>(decodedBmp),
        decodedBmpBytes);
    NetplayPaletteRuntime_OnLiveBankObserved(
        s_activeLoad.game_slot,
        s_activeLoad.base_palette,
        runtimeBank,
        sizeof(runtimeBank),
        s_activeLoad.asset_count,
        s_activeLoad.archive_path,
        s_activeLoad.patch_path);
}

static bool ApplyOverridePatch(uint8_t gameSlot,
                               const char* archivePath,
                               const char* patchPath,
                               uint8_t patchIndex,
                               const char** outPatchPath,
                               uint16_t* outAssetCount) {
    if (!patchPath || !outPatchPath) {
        return false;
    }

    *outPatchPath = patchPath;

    uint16_t assetCount = 0;
    ReadArchiveAssetCount(archivePath, &assetCount);
    if (outAssetCount) {
        *outAssetCount = assetCount;
    }

    if (patchIndex >= kMaxCharacterPaletteBanks) {
        return false;
    }

    uint8_t patchBuffer[kPatchBufferSize] = {};
    size_t patchBytesRead = 0;
    if (!ReadPatchFile(patchPath, patchBuffer, sizeof(patchBuffer), &patchBytesRead)) {
        return false;
    }

    const size_t bankOffset = (size_t)patchIndex * NETPLAY_PALETTE_BANK_SIZE;
    const size_t requiredBytes = bankOffset + NETPLAY_PALETTE_BANK_SIZE;
    if (requiredBytes > patchBytesRead || requiredBytes > sizeof(patchBuffer)) {
        return false;
    }

    uint8_t sourceBank[NETPLAY_PALETTE_BANK_SIZE] = {};
    ConvertPatchBankToRuntime(patchBuffer + bankOffset, sourceBank);
    DumpPatchArtifacts("source",
        gameSlot,
        archivePath,
        patchPath,
        patchIndex,
        assetCount,
        patchBuffer,
        patchBytesRead,
        sourceBank);

    NetplayPaletteRuntime_OnAssetBankCaptured(gameSlot,
        patchIndex,
        sourceBank,
        NETPLAY_PALETTE_BANK_SIZE,
        assetCount,
        archivePath,
        patchPath);

    NetplayPaletteBank overrideBank{};
    if (!NetplayPaletteRuntime_CopyAssetOverrideBank(gameSlot, &overrideBank) ||
        !overrideBank.valid ||
        overrideBank.base_palette != patchIndex) {
        return true;
    }

    uint8_t encodedOverride[NETPLAY_PALETTE_BANK_SIZE] = {};
    ConvertRuntimeBankToPatch(overrideBank.data, encodedOverride);
    memcpy(patchBuffer + bankOffset, encodedOverride, sizeof(encodedOverride));
    EnsureTempDir();

    char tempPath[MAX_PATH] = {};
    BuildTempPatchPath(tempPath, sizeof(tempPath), gameSlot);
    const size_t patchBytesToWrite = patchBytesRead > requiredBytes
        ? patchBytesRead
        : requiredBytes;
    DumpPatchArtifacts("effective",
        gameSlot,
        archivePath,
        tempPath,
        patchIndex,
        assetCount,
        patchBuffer,
        patchBytesToWrite,
        overrideBank.data);
    if (!WritePatchFile(tempPath, patchBuffer, patchBytesToWrite)) {
        return false;
    }

    CopyText(s_slotState[gameSlot].temp_patch_path,
        sizeof(s_slotState[gameSlot].temp_patch_path),
        tempPath);
    *outPatchPath = s_slotState[gameSlot].temp_patch_path;
    return true;
}

static bool LoadCharacterAssets(uint8_t gameSlot,
                                int* pOutHandles,
                                char* archiveFileName,
                                const char* patchFileName,
                                uint8_t patchIndex) {
    if (!s_originalAssetLoad || !pOutHandles || !archiveFileName || !patchFileName) {
        return false;
    }

    const char* patchPathToUse = patchFileName;
    uint16_t assetCount = 0;
    ApplyOverridePatch(gameSlot,
        archiveFileName,
        patchFileName,
        patchIndex,
        &patchPathToUse,
        &assetCount);

    memset(&s_activeLoad, 0, sizeof(s_activeLoad));
    s_activeLoad.valid = true;
    s_activeLoad.game_slot = gameSlot;
    s_activeLoad.base_palette = patchIndex;
    s_activeLoad.asset_count = assetCount;
    CopyText(s_activeLoad.archive_path, sizeof(s_activeLoad.archive_path), archiveFileName);
    CopyText(s_activeLoad.patch_path, sizeof(s_activeLoad.patch_path), patchPathToUse);

    LOG_INFO("[Palette] Character asset load begin slot=P%d handles=%p base=%u archive=%s patch=%s override=%s assets=%u",
        gameSlot + 1,
        pOutHandles,
        (unsigned)patchIndex,
        archiveFileName,
        patchFileName,
        patchPathToUse != patchFileName ? patchPathToUse : "none",
        (unsigned)assetCount);

    s_originalAssetLoad(pOutHandles, archiveFileName, patchPathToUse, patchIndex);

    if (!s_activeLoad.live_bank_observed) {
        DumpMissingLiveObservation(gameSlot,
            patchIndex,
            assetCount,
            archiveFileName,
            patchPathToUse,
            pOutHandles);
        LOG_WARN("[Palette] No decoded/handle live palette observed for slot=P%d base=%u archive=%s handles=%p",
            gameSlot + 1,
            (unsigned)patchIndex,
            archiveFileName,
            pOutHandles);
    }

    DumpReloadResearchSummary(gameSlot,
        patchIndex,
        assetCount,
        archiveFileName,
        patchPathToUse,
        pOutHandles);

    memset(&s_activeLoad, 0, sizeof(s_activeLoad));

    SlotLoadState& slot = s_slotState[gameSlot];
    slot.valid = true;
    slot.handle_table = pOutHandles;
    slot.asset_count = assetCount;
    slot.base_palette = patchIndex;
    CopyText(slot.archive_path, sizeof(slot.archive_path), archiveFileName);
    CopyText(slot.patch_path, sizeof(slot.patch_path), patchFileName);

    Rollback::NetplayLog_Write("PALETTE", -1,
        "Asset load: slot=P%d patch=%u asset_count=%u archive=%s patch=%s override=%s",
        gameSlot + 1,
        patchIndex,
        (unsigned)assetCount,
        slot.archive_path,
        slot.patch_path,
        patchPathToUse != patchFileName ? patchPathToUse : "none");
    LOG_INFO("[Palette] Asset load slot=P%d base=%u archive=%s override=%s",
        gameSlot + 1,
        (unsigned)patchIndex,
        slot.archive_path,
        patchPathToUse != patchFileName ? patchPathToUse : "none");
    return true;
}

static int* __cdecl Hook_ImageUploadToHandle(int format,
                                             int* clipRect,
                                             int handlePtr,
                                             int offsetX,
                                             int offsetY,
                                             int* srcRect,
                                             int* dstRect,
                                             int drawX,
                                             int drawY,
                                             int flags) {
    int* result = s_originalImageUploadToHandle
        ? s_originalImageUploadToHandle(
            format,
            clipRect,
            handlePtr,
            offsetX,
            offsetY,
            srcRect,
            dstRect,
            drawX,
            drawY,
            flags)
        : nullptr;
    if (s_activeLoad.valid && !s_activeLoad.late_upload_stage_seen) {
        s_activeLoad.late_upload_format = format;
        s_activeLoad.late_upload_result = result ? 0 : -1;
    }
    ObserveLivePaletteFromHandle(handlePtr);
    return result;
}

static int __cdecl Hook_ImageCreateFromDecodedBmp(int decodedBmp,
                                                  int decodedBmpBytes,
                                                  int a3,
                                                  int a4,
                                                  int a5,
                                                  int a6) {
    ObserveLivePaletteFromDecodedBmp(decodedBmp, decodedBmpBytes);
    return s_originalImageCreateFromDecodedBmp
        ? s_originalImageCreateFromDecodedBmp(decodedBmp, decodedBmpBytes, a3, a4, a5, a6)
        : -1;
}

static int __cdecl Hook_ImageRegisterHandle(int primaryFormat,
                                            int secondaryFormat,
                                            int usageKind) {
    const int result = s_originalImageRegisterHandle
        ? s_originalImageRegisterHandle(primaryFormat, secondaryFormat, usageKind)
        : -1;

    if (s_activeLoad.valid && !s_activeLoad.handle_create_stage_seen) {
        s_activeLoad.handle_create_stage_seen = true;
        s_activeLoad.created_handle = result;
        s_activeLoad.created_handle_usage_kind = usageKind;
        CaptureCreatedHandleInfo(result);

        const void* palette = nullptr;
        uint32_t paletteCrc = 0;
        TryGetRuntimePaletteFromFormat(primaryFormat, &palette, &paletteCrc);
        LogResearchEvent("handle_create",
            "addr=0x%08X result=0x%08X usage=%d primary=%p secondary=%p dims=%dx%d palette=%p palette_crc=0x%08X handle_entry=%p resource=%p",
            ADDR_IMAGE_REGISTER_HANDLE,
            result,
            usageKind,
            reinterpret_cast<const void*>(primaryFormat),
            reinterpret_cast<const void*>(secondaryFormat),
            primaryFormat != 0 ? *reinterpret_cast<const int*>(primaryFormat + kRuntimeFormatWidthOffset) : 0,
            primaryFormat != 0 ? *reinterpret_cast<const int*>(primaryFormat + kRuntimeFormatHeightOffset) : 0,
            palette,
            paletteCrc,
            s_activeLoad.created_handle_entry,
            s_activeLoad.created_handle_resource);
    }

    return result;
}

static int __cdecl Hook_HandleRenderBind(uint16_t* a1,
                                         void* a2,
                                         void* a3,
                                         uint8_t* a4,
                                         int a5,
                                         unsigned __int16* a6,
                                         uint32_t* a7,
                                         int a8,
                                         int a9,
                                         int a10,
                                         int a11) {
    const int result = s_originalHandleRenderBind
        ? s_originalHandleRenderBind(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11)
        : -1;

    if (s_activeLoad.valid && !s_activeLoad.handle_render_bind_stage_seen) {
        s_activeLoad.handle_render_bind_stage_seen = true;
        s_activeLoad.render_bind_handle = a10;
        s_activeLoad.render_bind_result = result;
        CaptureCreatedHandleInfo(a10);
        LogResearchEvent("handle_render_bind",
            "addr=0x%08X result=%d handle=0x%08X dst=(%d,%d) handle_entry=%p resource=%p format=%p secondary=%p",
            ADDR_HANDLE_RENDER_BIND,
            result,
            a10,
            a8,
            a9,
            s_activeLoad.created_handle_entry,
            s_activeLoad.created_handle_resource,
            a6,
            a7);
    }

    return result;
}

static void __cdecl Hook_AssetLoadAllFromArchive(int* pOutHandles,
                                                 char* archiveFileName,
                                                 const char* patchFileName,
                                                 int patchIndex) {
    if (!s_originalAssetLoad || s_insideAssetLoad) {
        if (s_originalAssetLoad) {
            s_originalAssetLoad(pOutHandles, archiveFileName, patchFileName, patchIndex);
        }
        return;
    }

    const int gameSlot = ResolveGameSlot(pOutHandles);
    if (gameSlot < 0 || patchIndex < 0 || patchIndex > 255) {
        if (s_unmatchedAssetLoadLogs < 32) {
            ++s_unmatchedAssetLoadLogs;
            LOG_WARN("[Palette] Unmatched Asset_LoadAllFromArchive call handles=%p patchIndex=%d archive=%s patch=%s expected_p1=%p expected_p2=%p",
                pOutHandles,
                patchIndex,
                archiveFileName ? archiveFileName : "(null)",
                patchFileName ? patchFileName : "(null)",
                ResolveHandleTable(0),
                ResolveHandleTable(1));
        }
        s_originalAssetLoad(pOutHandles, archiveFileName, patchFileName, patchIndex);
        return;
    }

    const int characterId = ResolveCharacterArchiveId(archiveFileName);
    if (characterId < 0 || !IsMatchingCharacterPatchPath(patchFileName, characterId)) {
        if (s_unmatchedAssetLoadLogs < 32) {
            ++s_unmatchedAssetLoadLogs;
            LOG_INFO("[Palette] Ignored non-character asset load slot=P%d patchIndex=%d archive=%s patch=%s",
                gameSlot + 1,
                patchIndex,
                archiveFileName ? archiveFileName : "(null)",
                patchFileName ? patchFileName : "(null)");
        }
        s_originalAssetLoad(pOutHandles, archiveFileName, patchFileName, patchIndex);
        return;
    }

    s_insideAssetLoad = true;
    LoadCharacterAssets((uint8_t)gameSlot,
        pOutHandles,
        archiveFileName,
        patchFileName,
        (uint8_t)patchIndex);
    s_insideAssetLoad = false;
}

static bool ReloadGameSlot(uint8_t gameSlot) {
    if (gameSlot > 1 || !s_originalAssetLoad) {
        return false;
    }

    SlotLoadState& slot = s_slotState[gameSlot];
    if (!slot.valid || !slot.handle_table || !slot.archive_path[0] || !slot.patch_path[0]) {
        return false;
    }

    for (uint16_t index = 0; index < slot.asset_count; index++) {
        const int handle = slot.handle_table[index];
        if (handle > 0) {
            s_handleFree(handle);
            slot.handle_table[index] = 0;
        }
    }

    s_insideAssetLoad = true;
    const bool ok = LoadCharacterAssets(gameSlot,
        slot.handle_table,
        slot.archive_path,
        slot.patch_path,
        slot.base_palette);
    s_insideAssetLoad = false;
    return ok;
}

} // namespace

bool PaletteAssetHook_Install() {
    if (s_originalAssetLoad &&
        s_originalImageCreateFromDecodedBmp &&
        s_originalImageRegisterHandle &&
        s_originalHandleRenderBind &&
        s_originalImageUploadToHandle) {
        return true;
    }

    LogReloadResearchAddresses();

    MH_STATUS status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_ASSET_LOAD_ALL_FROM_ARCHIVE),
        reinterpret_cast<void*>(&Hook_AssetLoadAllFromArchive),
        reinterpret_cast<void**>(&s_originalAssetLoad));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook Asset_LoadAllFromArchive! Status: %d", status);
        return false;
    }

    status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_IMAGE_CREATE_FROM_DECODED_BMP),
        reinterpret_cast<void*>(&Hook_ImageCreateFromDecodedBmp),
        reinterpret_cast<void**>(&s_originalImageCreateFromDecodedBmp));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook sub_63A9B0 (image create from decoded BMP)! Status: %d", status);
        return false;
    }

    status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_IMAGE_REGISTER_HANDLE),
        reinterpret_cast<void*>(&Hook_ImageRegisterHandle),
        reinterpret_cast<void**>(&s_originalImageRegisterHandle));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook sub_620930 (image register handle)! Status: %d", status);
        return false;
    }

    status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_HANDLE_RENDER_BIND),
        reinterpret_cast<void*>(&Hook_HandleRenderBind),
        reinterpret_cast<void**>(&s_originalHandleRenderBind));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook sub_6132E0 (handle render bind)! Status: %d", status);
        return false;
    }

    status = MH_CreateHook(
        reinterpret_cast<void*>(ADDR_IMAGE_UPLOAD_TO_HANDLE),
        reinterpret_cast<void*>(&Hook_ImageUploadToHandle),
        reinterpret_cast<void**>(&s_originalImageUploadToHandle));
    if (status != MH_OK) {
        LOG_WARN("Failed to hook sub_6460C0 (image upload to handle)! Status: %d", status);
        return false;
    }

    memset(s_slotState, 0, sizeof(s_slotState));
    memset(&s_activeLoad, 0, sizeof(s_activeLoad));
    s_unmatchedAssetLoadLogs = 0;
    LogResolvedHandleTables();
    LOG_INFO("Hooked Asset_LoadAllFromArchive, sub_63A9B0, sub_620930, sub_6132E0, and sub_6460C0 (character palette asset hooks)");
    return true;
}

void PaletteAssetHook_FrameUpdate() {
    Net::NetplayPaletteReloadRequest request{};
    while (Net::NetplayPaletteRuntime_ConsumeLiveReloadRequest(&request)) {
        const bool ok = ReloadGameSlot(request.game_slot);
        Net::NetplayPaletteRuntime_OnLiveReloadComplete(
            request.game_slot,
            ok,
            ok ? "asset reload complete" : "asset reload unavailable");
    }
}

void PaletteAssetHook_Shutdown() {
    for (int slot = 0; slot < 2; slot++) {
        if (s_slotState[slot].temp_patch_path[0]) {
            DeleteFileA(s_slotState[slot].temp_patch_path);
        }
    }
    memset(s_slotState, 0, sizeof(s_slotState));
    memset(&s_activeLoad, 0, sizeof(s_activeLoad));
    s_insideAssetLoad = false;
    s_unmatchedAssetLoadLogs = 0;
}