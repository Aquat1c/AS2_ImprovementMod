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
#   - Dear ImGui: https://github.com/ocornut/imgui (static lib)
#   - ENet: https://github.com/lsalzman/enet (static lib)
#   - libjuice: https://github.com/paullouisageneau/libjuice (optional NAT traversal)
#   - miniupnpc: https://github.com/miniupnp/miniupnp (optional UPnP)
#   - libpcpnatpmp: https://github.com/libpcp/pcp (optional PCP/NAT-PMP)
#   - autopunch: https://github.com/delthas/autopunch (protocol-compatible local client behavior)

cmake_minimum_required(VERSION 3.15)
project(AS2_Rollback VERSION 0.7.0 LANGUAGES C CXX)
set(AS2_MOD_VERSION "0.7-beta")

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
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHa /FS")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /EHa /FS")

# ============================================================================
# Options
# ============================================================================

option(USE_VCPKG "Use vcpkg for dependencies" OFF)
option(AS2_ENABLE_LEGACY_CUMULATIVE_PACKETS "Build the legacy cumulative packet experiment" OFF)
# re0.7 post-M8: engine2 is the ONLY rollback backend. The legacy fallback
# option and its vendored library were unwired after the M8 acceptance
# sign-off (see docs/re0.7/M8_ACCEPTANCE_RUNBOOK.md §6); the vendored tree
# stays on disk for history but is not part of the build.
option(AS2_ENABLE_NAT_TRAVERSAL "Enable NAT traversal backend integration" ON)
option(AS2_ENABLE_UPNP_FALLBACK "Enable miniupnpc UPnP fallback mapping" ON)
option(AS2_ENABLE_PCPNATPMP_FALLBACK "Enable libpcpnatpmp PCP/NAT-PMP fallback mapping" ON)
set(MINHOOK_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/minhook_src" CACHE PATH "MinHook source directory")
set(SDL3_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/SDL3" CACHE PATH "SDL3 directory")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/imgui" CACHE PATH "ImGui directory")
set(MINIUPNPC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniupnp_suite/miniupnpc" CACHE PATH "miniupnpc directory")
set(LIBJUICE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libjuice" CACHE PATH "libjuice directory")
set(PCPNATPMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libpcpnatpmp/lib" CACHE PATH "libpcpnatpmp directory")
set(ENET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/lib/enet" CACHE PATH "ENet directory")
set(GAME_DIR "D:/Alice in wonderland 2" CACHE PATH "Game installation directory")
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
# D3D9 Proxy DLL
# Dependencies: ImGui (static), MinHook (static), d3d9.lib (system)
# Now uses MinHook for reliable EndScene hooking
# ============================================================================

add_library(d3d9 SHARED
    d3d9_proxy/d3d9_proxy.cpp
    d3d9_proxy/overlay_resources.rc
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
    include/patches/frame_scheduler.h
    include/patches/frame_scheduler_core.h
    include/patches/memory_utils.h
    include/patches/unlock_patch.h
    include/patches/locale_patch.h
    include/patches/filesystem_patch.h
    include/patches/palette_asset_hook.h
    include/patches/charsel_palette_select.h
    include/patches/charsel_select_actions.h
    include/patches/render_guard.h
    include/patches/session_pump_hook.h
    include/patches/netplay_background_run.h
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
    include/input/gamepad_worker.h
)

set(DIAGNOSTICS_HEADERS
    include/diagnostics/async_log.h
)

set(ROLLBACK_HEADERS
    include/rollback/block_digest.h
    include/rollback/determinism_verify.h
    include/rollback/engine2.h
    include/rollback/game_snapshot.h
    include/rollback/lifecycle_window.h
    include/rollback/savestate.h
    include/rollback/input_timeline.h
    include/rollback/prediction.h
    include/rollback/resimulation.h
    include/rollback/rollback_session.h
    include/rollback/rollback_telemetry.h
    include/rollback/run_state.h
    include/rollback/rollback_debug.h
    include/rollback/desync_diag.h
    include/rollback/desync_dump.h
    include/rollback/desync_fine_diag.h
    include/rollback/netplay_log.h
    include/rollback/rollback_audio.h
    include/rollback/rollback_combo_fx.h
    include/rollback/owner_diagnostics.h
    include/rollback/rollback_status_fx.h
    include/rollback/stress_hooks.h
    include/rollback/online_wiring.h
    include/rollback/rematch_cleanup.h
)

set(NET_HEADERS
    include/net/protocol.h
    include/net/session_types.h
    include/net/baseline_sync.h
    include/net/enet_transport.h
    include/net/frame_arithmetic.h
    include/net/frontend_input_sync.h
    include/net/packet_router.h
    include/net/transport2.h
    include/net/session2.h
    include/net/session_manager.h
    include/net/spectator_protocol.h
    include/net/spectator_manager.h
    include/net/spectator_runtime.h
    include/net/spectator_client.h
    include/net/spectator_playback.h
    include/net/spectator_playback_policy.h
    include/net/netplay_palette_runtime.h
    include/net/netplay_palette_storage.h
    include/net/time_probe.h
    include/net/netplay_phase_runtime.h
    include/net/game_settings_sync.h
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
    include/net/match_lifecycle.h
    include/net/sync_policy.h
    include/net/delay_policy.h
    include/net/churn_pause.h
    include/net/player_side_mapping.h
    include/net/barrier_protocol.h
    include/net/sync_trace.h
)

set(TESTING_HEADERS
    include/testing/scripted_input_runner.h
    include/testing/test_scenarios.h
    include/testing/autoconnect_harness.h
    include/testing/harness_shared_memory.h
    include/testing/rematch_soak.h
)

set(TRAINING_HEADERS
    include/training/action_state_classifier.h
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
    src/patches/charsel_select_actions.cpp
    src/patches/input_override.cpp
    src/patches/shell_hotkey_patch.cpp
    src/patches/input_sync_hooks.cpp
    src/patches/render_guard.cpp
    src/patches/session_pump_hook.cpp
    src/patches/netplay_background_run.cpp
    src/patches/frame_scheduler.cpp
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
    src/ui/game_settings_menu.cpp
    src/ui/netplay_hud_style.cpp
    src/ui/netplay_hud_vanilla.cpp
)

set(INPUT_SOURCES
    src/input/input_system.cpp
    src/input/gamepad_worker.cpp
)

set(DIAGNOSTICS_SOURCES
    src/diagnostics/async_log.cpp
)

set(ROLLBACK_SOURCES
    src/rollback/determinism_verify.cpp
    src/rollback/engine2.cpp
    src/rollback/game_snapshot.cpp
    src/rollback/savestate.cpp
    src/rollback/input_timeline.cpp
    src/rollback/prediction.cpp
    src/rollback/resimulation.cpp
    src/rollback/rollback_debug.cpp
    src/rollback/desync_diag.cpp
    src/rollback/desync_dump.cpp
    src/rollback/desync_fine_diag.cpp
    src/rollback/netplay_log.cpp
    src/rollback/rollback_audio.cpp
    src/rollback/rollback_combo_fx.cpp
    src/rollback/rollback_status_fx.cpp
    src/rollback/stress_hooks.cpp
    src/rollback/match_director.cpp
    src/rollback/rematch_cleanup.cpp
)

# The RollbackSession_* facade (unchanged header, plan §6 M4 "facade
# adapter") has exactly one provider since the post-M8 legacy-backend
# removal: the engine2 adapter.
list(APPEND ROLLBACK_SOURCES src/rollback/rollback_session_engine2.cpp)

set(NET_SOURCES
    src/net/baseline_sync.cpp
    src/net/connection_supervisor.cpp
    src/net/continue_flow.cpp
    src/net/transition_barrier.cpp
    src/net/enet_transport.cpp
    src/net/frontend_input_sync.cpp
    src/net/packet_router.cpp
    src/net/transport2.cpp
    src/net/link_emulator.cpp
    src/net/session2.cpp
    src/net/spectator_protocol.cpp
    src/net/spectator_manager.cpp
    src/net/spectator_runtime.cpp
    src/net/spectator_client.cpp
    src/net/spectator_playback.cpp
    src/net/netplay_palette_runtime.cpp
    src/net/netplay_palette_storage.cpp
    src/net/time_probe.cpp
    src/net/churn_pause.cpp
    src/net/netplay_phase_runtime.cpp
    src/net/game_settings_sync.cpp
    src/net/nat_traversal.cpp
    src/net/mode_ownership.cpp
    src/net/netplay_menu_controller.cpp
    src/net/netplay_menu_ui.cpp
    src/net/locked_match_config.cpp
    src/net/match_setup.cpp
    src/net/charsel_sync.cpp
    src/net/stagesel_sync.cpp
    src/net/stage_watchdog_tracker.cpp
    src/net/match_lifecycle.cpp
    src/net/winscreen_sync.cpp
    src/net/pause_handler.cpp
    src/net/session_exit.cpp
    src/net/sync_policy.cpp
    src/net/delay_policy.cpp
    src/net/player_side_mapping.cpp
    src/net/barrier_protocol.cpp
    src/net/sync_trace.cpp
    src/net/set_tracker.cpp
)

set(TESTING_SOURCES
    src/testing/scripted_input_runner.cpp
    src/testing/test_scenarios.cpp
    src/testing/autoconnect_harness.cpp
    src/testing/rematch_soak.cpp
)

set(TRAINING_SOURCES
    src/training/action_state_classifier.cpp
    src/training/frame_advantage.cpp
    src/training/hotkey_config.cpp
    src/training/input_macro.cpp
    src/training/practice_tools.cpp
    src/training/practice_runtime.cpp
)

set(REPLAY_SOURCES
    src/replay/replay_runtime.cpp
)

add_executable(frontend_sync_tests
    tests/frontend_sync_tests.cpp
    src/net/frontend_input_sync.cpp
    src/net/winscreen_sync.cpp
    src/net/continue_flow.cpp
    src/net/delay_policy.cpp
    src/net/time_probe.cpp
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

add_executable(frame_arithmetic_tests
    tests/frame_arithmetic_tests.cpp
)

target_include_directories(frame_arithmetic_tests PRIVATE
    include
)

target_compile_definitions(frame_arithmetic_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(frame_arithmetic_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

# ---------------------------------------------------------------------------
# link_emulator_tests — the emulator that makes high-RTT claims testable must
# itself be trustworthy (ordering, honest latency, reliable-safe loss).
# ---------------------------------------------------------------------------
add_executable(link_emulator_tests
    tests/link_emulator_tests.cpp
    src/net/link_emulator.cpp
)

target_include_directories(link_emulator_tests PRIVATE
    include
)

target_compile_definitions(link_emulator_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    AS2_LINK_EMULATOR_TESTING
)

set_target_properties(link_emulator_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME link_emulator_tests COMMAND link_emulator_tests)

add_test(NAME frame_arithmetic_tests COMMAND frame_arithmetic_tests)

add_executable(frame_scheduler_tests
    tests/frame_scheduler_tests.cpp
)

target_include_directories(frame_scheduler_tests PRIVATE
    include
)

target_compile_definitions(frame_scheduler_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(frame_scheduler_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME frame_scheduler_tests COMMAND frame_scheduler_tests)

add_executable(async_log_tests
    tests/async_log_tests.cpp
    src/diagnostics/async_log.cpp
)

target_include_directories(async_log_tests PRIVATE
    include
)

target_compile_definitions(async_log_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(async_log_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME async_log_tests COMMAND async_log_tests)

# re0.7 M6: TransitionBarrier unit target (EpochAlign commit rule, Clear
# semantics, echo-verbatim acks, ladder-kind arrival orders) — closes the M5
# gate deviation. Links the real module with stubbed transport/logging.
add_executable(transition_barrier_tests
    tests/transition_barrier_tests.cpp
    src/net/transition_barrier.cpp
)

target_include_directories(transition_barrier_tests PRIVATE
    include
)

target_compile_definitions(transition_barrier_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(transition_barrier_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME transition_barrier_tests COMMAND transition_barrier_tests)

# re0.7 M4: engine2 core unit tests + deterministic socket-free soak harness
# (plan §7.1 T-ENG-1..12, §7.2 model) + savestate-cost microbench (§7.5 #4).
# Pure target: engine core + headers only, no Win32-linked mod TUs.
add_executable(engine2_tests
    tests/engine2_tests.cpp
    src/rollback/engine2.cpp
    src/rollback/desync_diag.cpp
)

target_include_directories(engine2_tests PRIVATE
    include
    include/core
)

target_compile_definitions(engine2_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(engine2_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME engine2_tests COMMAND engine2_tests)

# re0.7 post-M8: two-peer determinism suite (runbook R-DET class, offline).
# Full network-profile matrix over the simulated-link harness: stable/jittery
# RTT, loss, loss+jitter, reorder+duplication; multi-match epochs with
# per-epoch state payloads; forced-rollback-every-frame cells; full-speed
# (hold-budget) invariants. Pure target like engine2_tests: engine core only.
add_executable(determinism_tests
    tests/determinism_tests.cpp
    src/rollback/engine2.cpp
)

target_include_directories(determinism_tests PRIVATE
    include
    include/core
)

target_compile_definitions(determinism_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(determinism_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME determinism_tests COMMAND determinism_tests)

add_executable(training_logic_tests
    tests/frame_advantage_classifier_tests.cpp
    src/training/action_state_classifier.cpp
)

target_include_directories(training_logic_tests PRIVATE
    include
)

target_compile_definitions(training_logic_tests PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

set_target_properties(training_logic_tests PROPERTIES
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
)

add_test(NAME training_logic_tests COMMAND training_logic_tests)

add_library(as2_rollback SHARED
    ${CORE_HEADERS}
    ${PATCH_HEADERS}
    ${UI_HEADERS}
    ${INPUT_HEADERS}
    ${DIAGNOSTICS_HEADERS}
    ${ROLLBACK_HEADERS}
    ${NET_HEADERS}
    ${TESTING_HEADERS}
    ${TRAINING_HEADERS}
    ${REPLAY_HEADERS}
    ${CORE_SOURCES}
    ${PATCH_SOURCES}
    ${UI_SOURCES}
    ${INPUT_SOURCES}
    ${DIAGNOSTICS_SOURCES}
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
    include/diagnostics
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
)

target_compile_definitions(as2_rollback PRIVATE
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
    AS2_HAVE_LIBJUICE=${AS2_HAVE_LIBJUICE}
    AS2_HAVE_MINIUPNPC=${AS2_HAVE_MINIUPNPC}
    AS2_HAVE_PCPNATPMP=${AS2_HAVE_PCPNATPMP}
)

# Link libraries (MinHook built from source - true static, SDL3 requires DLL)
target_link_libraries(as2_rollback PRIVATE
    imgui
    minhook
    enet
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

    # wsock32.dll carries the shell-hotkey/middle-click EARLYPATCH layer
    # (phantom keybd_event(VK7) drop, cursor-hide bypass, SC_TASKLIST /
    # SC_SCREENSAVE swallow bypasses — see docs/SHELL_HOTKEY_POLICY.md).
    # It was never part of the post-build deploy, so game dirs silently ran
    # stale pre-fix copies. Keep it in lockstep with the other proxies.
    add_custom_command(TARGET wsock32 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:wsock32>
            ${GAME_DIR}/wsock32.dll
        COMMENT "Copying wsock32.dll to game directory"
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

    add_custom_command(TARGET wsock32 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:wsock32>
            ${TEST_GAME_DIR}/wsock32.dll
        COMMENT "Copying wsock32.dll to test game directory"
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
message(STATUS "Version: ${AS2_MOD_VERSION}")
message(STATUS "Static Runtime: YES (/MT)")
message(STATUS "MinHook: ${MINHOOK_DIR} (static)")
message(STATUS "SDL3: ${SDL3_DIR} (DLL required)")
message(STATUS "ImGui: ${IMGUI_DIR} (static)")
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
