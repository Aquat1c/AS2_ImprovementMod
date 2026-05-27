#pragma once

#include "core/as2_constants.h"
#include "core/game_state.h"
#include "rollback/netplay_log.h"

#include <windows.h>
#include <stdint.h>

namespace Rollback {

struct OwnerDiagSnapshot {
    uint8_t p1_owner;
    uint8_t p2_owner;
    int8_t p1_facing;
    int8_t p2_facing;
    uint32_t p1_entity_char;
    uint32_t p2_entity_char;
    uint32_t p1_sel_char;
    uint32_t p2_sel_char;
    uint8_t p1_sel_palette;
    uint8_t p2_sel_palette;
    uint16_t p1_sel_variant;
    uint16_t p2_sel_variant;
    uint8_t p1_sel_variant_extra;
    uint8_t p2_sel_variant_extra;
};

inline uint8_t OwnerDiag_ReadU8(uintptr_t addr, uint8_t fallback = 0xFF) {
    __try {
        return *reinterpret_cast<volatile uint8_t*>(addr);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

inline int8_t OwnerDiag_ReadI8(uintptr_t addr, int8_t fallback = 0x7F) {
    return static_cast<int8_t>(OwnerDiag_ReadU8(addr, static_cast<uint8_t>(fallback)));
}

inline uint16_t OwnerDiag_ReadU16(uintptr_t addr, uint16_t fallback = 0xFFFF) {
    __try {
        return *reinterpret_cast<volatile uint16_t*>(addr);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

inline uint32_t OwnerDiag_ReadU32(uintptr_t addr, uint32_t fallback = 0xFFFFFFFFu) {
    __try {
        return *reinterpret_cast<volatile uint32_t*>(addr);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        return fallback;
    }
}

inline uint32_t OwnerDiag_ReadEntityChar(uintptr_t entityBase) {
    const uint32_t charData = OwnerDiag_ReadU32(entityBase, 0);
    if (!charData) {
        return 0xFFFFFFFFu;
    }
    return OwnerDiag_ReadU32(static_cast<uintptr_t>(charData) + ENTITY_OFF_CHAR_ID, 0xFFFFFFFFu);
}

inline OwnerDiagSnapshot OwnerDiag_Capture() {
    OwnerDiagSnapshot s{};
    s.p1_owner = OwnerDiag_ReadU8(ADDR_P1_ENTITY_BASE, 0xFF);
    s.p2_owner = OwnerDiag_ReadU8(ADDR_P2_ENTITY_BASE, 0xFF);
    s.p1_facing = OwnerDiag_ReadI8(ADDR_P1_ENTITY_BASE + ENTITY_OFF_FACING, 0x7F);
    s.p2_facing = OwnerDiag_ReadI8(ADDR_P2_ENTITY_BASE + ENTITY_OFF_FACING, 0x7F);
    s.p1_entity_char = OwnerDiag_ReadEntityChar(ADDR_P1_ENTITY_BASE);
    s.p2_entity_char = OwnerDiag_ReadEntityChar(ADDR_P2_ENTITY_BASE);
    s.p1_sel_char = OwnerDiag_ReadU32(ADDR_CHARSEL_P1_CHAR_ID, 0xFFFFFFFFu);
    s.p2_sel_char = OwnerDiag_ReadU32(ADDR_CHARSEL_P2_CHAR_ID, 0xFFFFFFFFu);
    s.p1_sel_palette = OwnerDiag_ReadU8(ADDR_CHARSEL_P1_PALETTE, 0xFF);
    s.p2_sel_palette = OwnerDiag_ReadU8(ADDR_CHARSEL_P2_PALETTE, 0xFF);
    s.p1_sel_variant = OwnerDiag_ReadU16(ADDR_CHARSEL_P1_VARIANT, 0xFFFF);
    s.p2_sel_variant = OwnerDiag_ReadU16(ADDR_CHARSEL_P2_VARIANT, 0xFFFF);
    s.p1_sel_variant_extra = OwnerDiag_ReadU8(ADDR_CHARSEL_P1_VARIANT_EXTRA, 0xFF);
    s.p2_sel_variant_extra = OwnerDiag_ReadU8(ADDR_CHARSEL_P2_VARIANT_EXTRA, 0xFF);
    return s;
}

inline void OwnerDiag_LogSnapshot(const char* label,
                                  const OwnerDiagSnapshot& s,
                                  bool includeTailCrc = false,
                                  uint32_t p1TailCrc = 0,
                                  uint32_t p2TailCrc = 0,
                                  bool p1TailReadable = false,
                                  bool p2TailReadable = false) {
    if (includeTailCrc) {
        NetplayLog_Write(
            "OWNERCHK", -1,
            "%s mode=%u sub=%u p1_owner=%u p2_owner=%u p1_facing=%d p2_facing=%d "
            "p1_char=%u p2_char=%u p1_sel=%u/%u/%u/%u p2_sel=%u/%u/%u/%u "
            "p1_tail_crc=0x%08X p2_tail_crc=0x%08X tail_read=%d/%d",
            label ? label : "owner-check",
            GetGameMode(),
            GetSubstate(),
            s.p1_owner,
            s.p2_owner,
            (int)s.p1_facing,
            (int)s.p2_facing,
            s.p1_entity_char,
            s.p2_entity_char,
            s.p1_sel_char,
            s.p1_sel_palette,
            s.p1_sel_variant,
            s.p1_sel_variant_extra,
            s.p2_sel_char,
            s.p2_sel_palette,
            s.p2_sel_variant,
            s.p2_sel_variant_extra,
            p1TailCrc,
            p2TailCrc,
            p1TailReadable ? 1 : 0,
            p2TailReadable ? 1 : 0);
    } else {
        NetplayLog_Write(
            "OWNERCHK", -1,
            "%s mode=%u sub=%u p1_owner=%u p2_owner=%u p1_facing=%d p2_facing=%d "
            "p1_char=%u p2_char=%u p1_sel=%u/%u/%u/%u p2_sel=%u/%u/%u/%u",
            label ? label : "owner-check",
            GetGameMode(),
            GetSubstate(),
            s.p1_owner,
            s.p2_owner,
            (int)s.p1_facing,
            (int)s.p2_facing,
            s.p1_entity_char,
            s.p2_entity_char,
            s.p1_sel_char,
            s.p1_sel_palette,
            s.p1_sel_variant,
            s.p1_sel_variant_extra,
            s.p2_sel_char,
            s.p2_sel_palette,
            s.p2_sel_variant,
            s.p2_sel_variant_extra);
    }

    if (s.p1_owner == s.p2_owner || s.p1_owner > 1 || s.p2_owner > 1) {
        NetplayLog_Write(
            "OWNERCHK", -1,
            "ERROR suspicious player owner bytes at %s: p1_owner=%u p2_owner=%u",
            label ? label : "owner-check",
            s.p1_owner,
            s.p2_owner);
    }
}

inline void OwnerDiag_Log(const char* label) {
    OwnerDiag_LogSnapshot(label, OwnerDiag_Capture());
}

} // namespace Rollback
