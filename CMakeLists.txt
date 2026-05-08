# Alice Senki 2 Improvement Mod - CMake Build
# 
# Build instructions:
#   mkdir build && cd build
#   cmake .. -A Win32
#   cmake --build . --config Release
#
# Dependencies:
#   - MinHook: https://github.com/TsudaKageyu/minhook (static lib)
#   - SDL3: https://github.com/libsdl-org/SDL (DLL - no static prebuilt available)
#   - ImGui: https://github.com/ocornut/imgui (compiled into DLL)

cmake_minimum_required(VERSION 3.15)
project(AS2_Rollback VERSION 0.4.0 LANGUAGES C CXX)

enable_testing()

# Force 32-bit build (the game is 32-bit)
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(WARNING "This project targets 32-bit. Use -A Win32 with Visual Studio generator.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ============================================================================
# Static Runtime Linking (/MT instead of /MD)
# This eliminates the need for VC runtime DLLs
# ============================================================================

set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# Enable asynchronous exception handling for SEH (__try/__except)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHa")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /EHa")

# ============================================================================
# Options
# ============================================================================

option(USE_VCPKG "Use vcpkg for dependencies" OFF)
option(AS2_ENABLE_LEGACY_CUMULATIVE_PACKETS "Build the legacy cumulative packet experiment" OFF)
option(AS2_ENABLE_NAT_TRAVERSAL "Enable NAT traversal backend integration" ON)
option(AS2_ENABLE_UPNP_FALLBACK "Enable miniupnpc UPnP fallback mapping" ON)
option(AS2_ENABLE_PCPNATPMP_FALLBACK "Enable libpcpnatpmp PCP/NAT-PMP fallback mapping" ON)
set(MINHOOK_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/minhook_src" CACHE PATH "MinHook source directory")
set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/SDL3" CACHE PATH "SDL3 directory")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui" CACHE PATH "ImGui directory")
set(GEKKONET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/GekkoNet" CACHE PATH "GekkoNet directory")
set(MINIUPNPC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniupnp_suite/miniupnpc" CACHE PATH "miniupnpc directory")
set(LIBJUICE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjuice" CACHE PATH "libjuice directory")
set(PCPNATPMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libpcpnatpmp/lib" CACHE PATH "libpcpnatpmp directory")
set(ENET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/enet" CACHE PATH "ENet directory")
set(GAME_DIR "D:/Alice in wonderland ２" CACHE PATH "Game installation directory")
set(TEST_GAME_DIR "" CACHE PATH "Secondary game directory for test harness (optional)")

# ============================================================================
# MinHook Library (built from source as TRUE static lib)
# ============================================================================

add_library(minhook STATIC
    ${MINHOOK_SRC_DIR}/src/buffer.c
    ${MINHOOK_SRC_DIR}/src/hook.c
    ${MINHOOK_SRC_DIR}/src/trampoline.c
    ${MINHOOK_SRC_DIR}/src/hde/hde32.c
)

target_include_directories(minhook PUBLIC
    ${MINHOOK_SRC_DIR}/include
)

target_compile_definitions(minhook PRIVATE
    WIN32_LEAN_AND_MEAN
)

# ============================================================================
# ImGui Library (built as static lib, linked into our DLLs)
# ============================================================================

add_library(imgui STATIC
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp
)

target_include_directories(imgui PUBLIC
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(imgui PRIVATE
    WIN32_LEAN_AND_MEAN
)

# ============================================================================
# miniupnpc Library (static)
# UPnP IGD client for automatic port forwarding.
# ============================================================================

set(AS2_HAVE_MINIUPNPC 0)
if(AS2_ENABLE_NAT_TRAVERSAL AND AS2_ENABLE_UPNP_FALLBACK AND EXISTS "${MINIUPNPC_DIR}/CMakeLists.txt")
    set(UPNPC_BUILD_STATIC ON CACHE BOOL "Build miniupnpc static" FORCE)
    set(UPNPC_BUILD_SHARED OFF CACHE BOOL "Build miniupnpc shared" FORCE)
    set(UPNPC_BUILD_TESTS OFF CACHE BOOL "Build miniupnpc tests" FORCE)
    set(UPNPC_BUILD_SAMPLE OFF CACHE BOOL "Build miniupnpc samples" FORCE)
    set(UPNPC_NO_INSTALL ON CACHE BOOL "Skip miniupnpc install" FORCE)
    add_subdirectory("${MINIUPNPC_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/miniupnpc" EXCLUDE_FROM_ALL)
    if(TARGET libminiupnpc-static)
        set(AS2_HAVE_MINIUPNPC 1)
    endif()
else()
    message(WARNING "miniupnpc not found or disabled; UPnP port forwarding fallback is disabled.")
endif()

# ============================================================================
# libjuice Library (static)
# Primary ICE/STUN/TURN candidate gathering and traversal signaling backend.
# ============================================================================

set(AS2_HAVE_LIBJUICE 0)
if(AS2_ENABLE_NAT_TRAVERSAL AND EXISTS "${LIBJUICE_DIR}/CMakeLists.txt")
    set(BUILD_SHARED_LIBS_SAVED "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF)
    set(NO_TESTS ON CACHE BOOL "Disable libjuice tests" FORCE)
    add_subdirectory("${LIBJUICE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/libjuice" EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVED}")
    if(TARGET juice-static)
        set(AS2_HAVE_LIBJUICE 1)
    endif()
else()
    message(WARNING "libjuice not found or disabled; ICE/STUN/TURN traversal backend is disabled.")
endif()

# ============================================================================
# libpcpnatpmp Library (static)
# Optional PCP/NAT-PMP router mapping fallback.
# ============================================================================

set(AS2_HAVE_PCPNATPMP 0)
if(AS2_ENABLE_NAT_TRAVERSAL AND AS2_ENABLE_PCPNATPMP_FALLBACK AND EXISTS "${PCPNATPMP_DIR}/CMakeLists.txt")
    add_subdirectory("${PCPNATPMP_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/pcpnatpmp" EXCLUDE_FROM_ALL)
    if(TARGET pcpnatpmp)
        set(AS2_HAVE_PCPNATPMP 1)
    endif()
else()
    message(STATUS "libpcpnatpmp not found or disabled; PCP/NAT-PMP fallback is disabled.")
endif()

# ============================================================================
# ENet Library (static)
# Reliable UDP networking library replacing hand-rolled transport.
# ============================================================================

if(EXISTS "${ENET_DIR}/CMakeLists.txt")
    set(BUILD_SHARED_LIBS_SAVED "${BUILD_SHARED_LIBS}")
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory("${ENET_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/enet" EXCLUDE_FROM_ALL)
    set(BUILD_SHARED_LIBS "${BUILD_SHARED_LIBS_SAVED}")
else()
    message(FATAL_ERROR "ENet not found at ${ENET_DIR}; networking will not work.")
endif()

# ============================================================================
# GekkoNet Library (static)
# Pinned workspace dependency used for the gameplay rollback bridge.
# ============================================================================ 

if(EXISTS "${GEKKONET_DIR}/GekkoLib/CMakeLists.txt")
    set(BUILD_DOCS OFF CACHE BOOL "Build GekkoNet docs" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build GekkoNet shared libraries" FORCE)
    set(NO_ASIO_BUILD OFF CACHE BOOL "Build GekkoNet without ASIO" FORCE)
    add_subdirectory("${GEKKONET_DIR}/GekkoLib" "${CMAKE_CURRENT_BINARY_DIR}/GekkoNet" EXCLUDE_FROM_ALL)
    if(TARGET GekkoNet)
        if(MSVC)
            # Force runtime parity with this project for all configs.
            set_property(TARGET GekkoNet PROPERTY
                MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
        target_include_directories(GekkoNet PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/include
        )
    endif()
else()
    message(WARNING "GekkoNet not found at ${GEKKONET_DIR}; gameplay bridge build will fail until it is added.")
endif()

# ============================================================================
# D3D9 Proxy DLL
# Dependencies: ImGui (static), MinHook (static), d3d9.lib (system)
# Now uses MinHook for reliable EndScene hooking
# ============================================================================

add_library(d3d9 SHARED
    d3d9_proxy/d3d9_proxy.cpp
    d3d9_proxy/letterbox_scaler.cpp
)

target_include_directories(d3d9 PRIVATE
    ${MINHOOK_SRC_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(d3d9 PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(d3d9 PRIVATE
    imgui
    minhook
    d3d9.lib
)

set_target_properties(d3d9 PROPERTIES
    OUTPUT_NAME "d3d9"
    PREFIX ""
    LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/d3d9_proxy/d3d9_proxy.def"
)

# ============================================================================
# wsock32 Proxy DLL
# Loaded at process start (game imports WSOCK32.dll statically).
# Sole purpose: IAT-hook FindWindowA so DXLib allows duplicate instances.
# All winsock functions forwarded to real wsock32.dll.
# ============================================================================

add_library(wsock32 SHARED
    winsock_proxy/wsock32_proxy.cpp
)

target_compile_definitions(wsock32 PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(wsock32 PRIVATE
    ws2_32
)

set_target_properties(wsock32 PROPERTIES
    OUTPUT_NAME "wsock32"
    PREFIX ""
    LINK_FLAGS "/DEF:${CMAKE_CURRENT_SOURCE_DIR}/winsock_proxy/wsock32_proxy.def"
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)

# ============================================================================
# Main Mod DLL (as2_rollback)
# Dependencies: ImGui (static), MinHook (static), SDL3 (DLL)
# ============================================================================

# Header files (for IDE visibility)
set(CORE_HEADERS
    include/core/as2_constants.h
    include/core/game_state.h
)

set(PATCH_HEADERS
    include/patches/memory_utils.h
    include/patches/unlock_patch.h
    include/patches/locale_patch.h
    include/patches/filesystem_patch.h
    include/patches/palette_asset_hook.h
    include/patches/charsel_palette_select.h
)

# Dead NETPLAY_HEADERS removed — old src/netplay/ code was moved to old_files/

set(UI_HEADERS
    include/ui/log_window.h
    include/ui/mod_menu.h
    include/ui/hitbox_viewer.h
    include/ui/palette_editor.h
)

set(INPUT_HEADERS
    include/input/input_system.h
)

set(ROLLBACK_HEADERS
    include/rollback/determinism_verify.h
    include/rollback/frame_lineage.h
    include/rollback/game_snapshot.h
    include/rollback/savestate.h
    include/rollback/input_timeline.h
    include/rollback/prediction.h
    include/rollback/resimulation.h
    include/rollback/rollback_session.h
    include/rollback/rollback_debug.h
    include/rollback/desync_dump.h
    include/rollback/netplay_log.h
    include/rollback/stress_hooks.h
    include/rollback/online_wiring.h
    include/rollback/rematch_cleanup.h
)

set(NET_HEADERS
    include/net/protocol.h
    include/net/session_types.h
    include/net/baseline_sync.h
    include/net/enet_transport.h
    include/net/frontend_input_sync.h
    include/net/network_thread.h
    include/net/session_manager.h
    include/net/spectator_protocol.h
    include/net/spectator_manager.h
    include/net/spectator_runtime.h
    include/net/spectator_client.h
    include/net/spectator_playback.h
    include/net/netplay_palette_runtime.h
    include/net/netplay_palette_storage.h
    include/net/netplay_pacing.h
    include/net/netplay_phase_runtime.h
    include/net/nat_traversal.h
    include/net/netplay_menu_state.h
    include/net/mode_ownership.h
    include/net/netplay_menu_controller.h
    include/net/netplay_menu_ui.h
    include/net/locked_match_config.h
    include/net/pregame_sync.h
    include/net/charsel_sync.h
    include/net/stagesel_sync.h
    include/net/stage_watchdog_tracker.h
    include/net/match_bootstrap.h
    include/net/match_lifecycle.h
    include/net/sync_policy.h
    include/net/delay_policy.h
    include/net/player_side_mapping.h
    include/net/barrier_protocol.h
    include/net/gameplay_bridge.h
)

set(TESTING_HEADERS
    include/testing/scripted_input_runner.h
    include/testing/test_scenarios.h
    include/testing/autoconnect_harness.h
    include/testing/harness_shared_memory.h
)

set(TRAINING_HEADERS
    include/training/frame_advantage.h
    include/training/hotkey_config.h
    include/training/input_macro.h
    include/training/practice_tools.h
)

set(REPLAY_HEADERS
    include/replay/replay_runtime.h
)

# Source files organized by component
set(CORE_SOURCES
    src/core/mod_main.cpp
    src/core/game_console.cpp
)

set(PATCH_SOURCES
    src/patches/memory_utils.cpp
    src/patches/unlock_patch.cpp
    src/patches/locale_patch.cpp
    src/patches/filesystem_patch.cpp
    src/patches/palette_asset_hook.cpp
    src/patches/charsel_palette_select.cpp
    src/patches/input_override.cpp
    src/patches/input_sync_hooks.cpp
    src/patches/tick_hooks.cpp
    src/patches/hook_installer.cpp
)

# Dead NETPLAY_SOURCES removed — old src/netplay/ code was moved to old_files/

set(UI_SOURCES
    src/ui/log_window.cpp
    src/ui/mod_menu.cpp
    src/ui/palette_editor.cpp
    src/ui/menu_utils.cpp
    src/ui/hitbox_viewer.cpp
    src/ui/netplay_hud.cpp
)

set(INPUT_SOURCES
    src/input/input_system.cpp
)

set(ROLLBACK_SOURCES
    src/rollback/determinism_verify.cpp
    src/rollback/frame_lineage.cpp
    src/rollback/game_snapshot.cpp
    src/rollback/savestate.cpp
    src/rollback/input_timeline.cpp
    src/rollback/prediction.cpp
    src/rollback/resimulation.cpp
    src/rollback/rollback_session.cpp
    src/rollback/rollback_debug.cpp
    src/rollback/desync_dump.cpp
    src/rollback/netplay_log.cpp
    src/rollback/stress_hooks.cpp
    src/rollback/online_wiring.cpp
    src/rollback/rematch_cleanup.cpp
)

set(NET_SOURCES
    src/net/baseline_sync.cpp
    src/net/enet_transport.cpp
    src/net/frontend_input_sync.cpp
    src/net/network_thread.cpp
    src/net/session_manager.cpp
    src/net/spectator_protocol.cpp
    src/net/spectator_manager.cpp
    src/net/spectator_runtime.cpp
    src/net/spectator_client.cpp
    src/net/spectator_playback.cpp
    src/net/netplay_palette_runtime.cpp
    src/net/netplay_palette_storage.cpp
    src/net/netplay_pacing.cpp
    src/net/netplay_phase_runtime.cpp
    src/net/nat_traversal.cpp
    src/net/mode_ownership.cpp
    src/net/netplay_menu_controller.cpp
    src/net/netplay_menu_ui.cpp
    src/net/locked_match_config.cpp
    src/net/pregame_sync.cpp
    src/net/charsel_sync.cpp
    src/net/stagesel_sync.cpp
    src/net/stage_watchdog_tracker.cpp
    src/net/match_bootstrap.cpp
    src/net/match_lifecycle.cpp
    src/net/winscreen_sync.cpp
    src/net/pause_handler.cpp
    src/net/sync_policy.cpp
    src/net/delay_policy.cpp
    src/net/player_side_mapping.cpp
    src/net/barrier_protocol.cpp
    src/net/gameplay_bridge.cpp
    src/net/set_tracker.cpp
)

set(TESTING_SOURCES
    src/testing/scripted_input_runner.cpp
    src/testing/test_scenarios.cpp
    src/testing/autoconnect_harness.cpp
)

set(TRAINING_SOURCES
    src/training/frame_advantage.cpp
    src/training/hotkey_config.cpp
    src/training/input_macro.cpp
    src/training/practice_tools.cpp
    src/training/practice_runtime.cpp
)

set(REPLAY_SOURCES
    src/replay/replay_runtime.cpp
)

# standalone_rollback_tests disabled — legacy source files moved to old_files/
# add_executable(standalone_rollback_tests tests/test_runner_main.cpp tests/standalone_rollback_tests.cpp tests/test_packet_codec.cpp src/netplay/packet_codec.cpp)
# target_include_directories(standalone_rollback_tests PRIVATE include/netplay include/ui)
# target_link_libraries(standalone_rollback_tests PRIVATE ws2_32)

add_executable(frontend_sync_tests
    tests/frontend_sync_tests.cpp
    src/net/frontend_input_sync.cpp
    src/net/delay_policy.cpp
    src/net/stagesel_sync.cpp
    src/net/stage_watchdog_tracker.cpp
)

target_include_directories(frontend_sync_tests PRIVATE
    include
)

target_compile_definitions(frontend_sync_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    AS2_FRONTEND_SYNC_TESTING
)

set_target_properties(frontend_sync_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME frontend_sync_tests COMMAND frontend_sync_tests)

add_library(as2_rollback SHARED
    ${CORE_HEADERS}
    ${PATCH_HEADERS}
    ${UI_HEADERS}
    ${INPUT_HEADERS}
    ${ROLLBACK_HEADERS}
    ${NET_HEADERS}
    ${TESTING_HEADERS}
    ${TRAINING_HEADERS}
    ${REPLAY_HEADERS}
    ${CORE_SOURCES}
    ${PATCH_SOURCES}
    ${UI_SOURCES}
    ${INPUT_SOURCES}
    ${ROLLBACK_SOURCES}
    ${NET_SOURCES}
    ${TESTING_SOURCES}
    ${TRAINING_SOURCES}
    ${REPLAY_SOURCES}
)

set_target_properties(as2_rollback PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

target_include_directories(as2_rollback PRIVATE
    include
    include/core
    include/patches
    include/ui
    include/input
    include/rollback
    include/testing
    include/training
    ${MINHOOK_SRC_DIR}/include
    ${SDL3_DIR}/include
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    ${ENET_DIR}/include
    ${MINIUPNPC_DIR}/include
    ${LIBJUICE_DIR}/include
    ${PCPNATPMP_DIR}/include
    ${GEKKONET_DIR}/GekkoLib/include
)

target_compile_definitions(as2_rollback PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    GEKKONET_STATIC
    AS2_HAVE_LIBJUICE=${AS2_HAVE_LIBJUICE}
    AS2_HAVE_MINIUPNPC=${AS2_HAVE_MINIUPNPC}
    AS2_HAVE_PCPNATPMP=${AS2_HAVE_PCPNATPMP}
)

# Link libraries (MinHook built from source - true static, SDL3 requires DLL)
target_link_libraries(as2_rollback PRIVATE
    imgui
    minhook
    enet
    GekkoNet
    ${SDL3_DIR}/lib/x86/SDL3.lib
    ws2_32.lib
    winhttp.lib
    # Windows system libs for static linking
    winmm.lib
    imm32.lib
    version.lib
    setupapi.lib
    iphlpapi.lib
)

if(AS2_HAVE_MINIUPNPC AND TARGET libminiupnpc-static)
    target_link_libraries(as2_rollback PRIVATE libminiupnpc-static)
endif()

if(AS2_HAVE_LIBJUICE AND TARGET juice-static)
    target_link_libraries(as2_rollback PRIVATE juice-static)
endif()

if(AS2_HAVE_PCPNATPMP AND TARGET pcpnatpmp)
    target_link_libraries(as2_rollback PRIVATE pcpnatpmp)
endif()
set_target_properties(as2_rollback PROPERTIES
    OUTPUT_NAME "as2_rollback"
    PREFIX ""
)

# Generate PDB and MAP for Release to aid crash symbolization (MSVC)
if(MSVC)
    # Emit full debug info and a .map with exports in Release builds
    # Using FASTLINK to avoid PDB size/lock issues during incremental builds
    target_compile_options(as2_rollback PRIVATE $<$<CONFIG:Release>:/Zi>)
    target_link_options(as2_rollback PRIVATE
        $<$<CONFIG:Release>:/DEBUG:FASTLINK>
        $<$<CONFIG:Release>:/INCREMENTAL:NO>
        $<$<CONFIG:Release>:/MAP:${CMAKE_CURRENT_BINARY_DIR}/as2_rollback.map>
        $<$<CONFIG:Release>:/MAPINFO:EXPORTS>
    )
endif()

# ============================================================================
# Post-build: Copy to game directory
# Only SDL3.dll needs to be copied (our DLLs are self-contained otherwise)
# ============================================================================

if(EXISTS ${GAME_DIR})
    add_custom_command(TARGET d3d9 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:d3d9>
            ${GAME_DIR}/d3d9.dll
        COMMENT "Copying d3d9.dll to game directory"
    )
    
    add_custom_command(TARGET as2_rollback POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:as2_rollback>
            ${GAME_DIR}/as2_rollback.dll
        COMMENT "Copying as2_rollback.dll to game directory"
    )
    
    # SDL3.dll is required (no static prebuilt available)
    if(EXISTS ${SDL3_DIR}/lib/x86/SDL3.dll)
        add_custom_command(TARGET as2_rollback POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SDL3_DIR}/lib/x86/SDL3.dll
                ${GAME_DIR}/SDL3.dll
            COMMENT "Copying SDL3.dll to game directory"
        )
    endif()
endif()

if(TEST_GAME_DIR AND EXISTS ${TEST_GAME_DIR})
    add_custom_command(TARGET d3d9 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:d3d9>
            ${TEST_GAME_DIR}/d3d9.dll
        COMMENT "Copying d3d9.dll to test game directory"
    )

    add_custom_command(TARGET as2_rollback POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:as2_rollback>
            ${TEST_GAME_DIR}/as2_rollback.dll
        COMMENT "Copying as2_rollback.dll to test game directory"
    )

    if(EXISTS ${SDL3_DIR}/lib/x86/SDL3.dll)
        add_custom_command(TARGET as2_rollback POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${SDL3_DIR}/lib/x86/SDL3.dll
                ${TEST_GAME_DIR}/SDL3.dll
            COMMENT "Copying SDL3.dll to test game directory"
        )
    endif()
endif()

# ============================================================================
# Installation
# ============================================================================

install(TARGETS d3d9 as2_rollback wsock32
    RUNTIME DESTINATION ${GAME_DIR}
    LIBRARY DESTINATION ${GAME_DIR}
)

install(FILES
    ${SDL3_DIR}/lib/x86/SDL3.dll
    DESTINATION ${GAME_DIR}
    OPTIONAL
)

# Secondary test game directory (optional)
if(TEST_GAME_DIR)
    install(TARGETS d3d9 as2_rollback wsock32
        RUNTIME DESTINATION ${TEST_GAME_DIR}
        LIBRARY DESTINATION ${TEST_GAME_DIR}
    )
    install(FILES
        ${SDL3_DIR}/lib/x86/SDL3.dll
        DESTINATION ${TEST_GAME_DIR}
        OPTIONAL
    )
endif()

# ============================================================================
# Test Harness Launcher (ImGui + D3D9 dashboard application)
# ============================================================================

add_executable(as2_test_harness
    tools/test_harness_launcher.cpp
)

target_include_directories(as2_test_harness PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include/testing
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

target_compile_definitions(as2_test_harness PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

target_link_libraries(as2_test_harness PRIVATE
    imgui
)

set_target_properties(as2_test_harness PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# Copy launcher to game directory on build
if(EXISTS ${GAME_DIR})
    add_custom_command(TARGET as2_test_harness POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:as2_test_harness>
            ${GAME_DIR}/as2_test_harness.exe
        COMMENT "Copying as2_test_harness.exe to game directory"
    )
endif()

if(TEST_GAME_DIR AND EXISTS ${TEST_GAME_DIR})
    add_custom_command(TARGET as2_test_harness POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:as2_test_harness>
            ${TEST_GAME_DIR}/as2_test_harness.exe
        COMMENT "Copying as2_test_harness.exe to test game directory"
    )
endif()

install(TARGETS as2_test_harness
    RUNTIME DESTINATION ${GAME_DIR}
)

if(TEST_GAME_DIR)
    install(TARGETS as2_test_harness
        RUNTIME DESTINATION ${TEST_GAME_DIR}
    )
endif()

# ============================================================================
# Package Info
# ============================================================================

message(STATUS "")
message(STATUS "=== AS2 Improvement Mod Build Configuration ===")
message(STATUS "Version: ${PROJECT_VERSION}")
message(STATUS "Static Runtime: YES (/MT)")
message(STATUS "MinHook: ${MINHOOK_DIR} (static)")
message(STATUS "SDL3: ${SDL3_DIR} (DLL required)")
message(STATUS "ImGui: ${IMGUI_DIR} (static)")
message(STATUS "GekkoNet: ${GEKKONET_DIR}")
message(STATUS "NAT traversal enabled: ${AS2_ENABLE_NAT_TRAVERSAL}")
message(STATUS "libjuice: ${LIBJUICE_DIR} (linked=${AS2_HAVE_LIBJUICE})")
message(STATUS "miniupnpc: ${MINIUPNPC_DIR} (linked=${AS2_HAVE_MINIUPNPC})")
message(STATUS "libpcpnatpmp: ${PCPNATPMP_DIR} (linked=${AS2_HAVE_PCPNATPMP})")
message(STATUS "Legacy cumulative packets: ${AS2_ENABLE_LEGACY_CUMULATIVE_PACKETS}")
message(STATUS "Game Dir: ${GAME_DIR}")
message(STATUS "")
message(STATUS "Files needed in game folder:")
message(STATUS "  - d3d9.dll (our proxy)")
message(STATUS "  - as2_rollback.dll (our mod)")
message(STATUS "  - SDL3.dll (from SDL3 release)")
message(STATUS "  - wsock32.dll (duplicate instance bypass proxy)")
message(STATUS "")
