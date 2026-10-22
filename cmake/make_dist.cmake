# Assembles the distributable game folder and zip.
# Invoked by the `dist` target: cmake -DDIST_CONFIG=<generated config> -P make_dist.cmake
# The config file carries every path (including non-ASCII ones) so nothing
# non-ASCII travels through a shell command line.
cmake_minimum_required(VERSION 3.21)

if(NOT DIST_CONFIG OR NOT EXISTS "${DIST_CONFIG}")
    message(FATAL_ERROR "make_dist: DIST_CONFIG not given or missing (${DIST_CONFIG})")
endif()
include("${DIST_CONFIG}")

foreach(var DIST_BASE DIST_DIR PKG_NAME MOD_VERSION FILE_D3D9 FILE_CORE FILE_WSOCK FILE_SDL3 FILE_LAUNCHER)
    if(NOT DEFINED ${var} OR "${${var}}" STREQUAL "")
        message(FATAL_ERROR "make_dist: ${var} is not set in ${DIST_CONFIG}")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${DIST_BASE}")
    message(FATAL_ERROR
        "make_dist: base game folder not found:\n  ${DIST_BASE}\n"
        "Point AS2_DIST_BASE at a clean copy of the game folder "
        "(the one containing the game executable, DATA and WAVE).")
endif()

set(pkg "${DIST_DIR}/${PKG_NAME}")
# Start clean so files dropped from the base or the exclusion list do not linger.
file(REMOVE_RECURSE "${pkg}")
file(MAKE_DIRECTORY "${pkg}")

# file(WRITE) uses text mode on Windows; force LF for files Linux users read.
# (configure_file() inside a function writes an empty file in script mode.)
function(dist_write_lf path content)
    file(CONFIGURE OUTPUT "${path}" CONTENT "${content}" @ONLY NEWLINE_STYLE LF)
endfunction()
message(STATUS "dist: base    ${DIST_BASE}")
message(STATUS "dist: package ${pkg}")

# ---------------------------------------------------------------------------
# 1. Base game files. Skip archives, logs, per-user state and anything the
#    build overlays below so a played-in folder can serve as the base too.
# ---------------------------------------------------------------------------
set(overlay_names
    d3d9.dll as2_rollback.dll wsock32.dll SDL3.dll
    run_as2_linux.sh README.txt README_LINUX.md README_LINUX.txt MANIFEST.sha256 BUILD_INFO.txt)

set(exclude_regexes
    "^(logs|synclogs|REPLAY|custom_palettes|dist)/"
    "^マニュアル/"
    "\\.(zip|rar|7z|log|rec|i64|pdb)$"
    "^(crash_log\\.txt|as2_imgui\\.ini|imgui\\.ini|as2_autoconnect.*\\.cfg)$"
    "^as2_[A-Za-z0-9_]+\\.(cfg|ini)$"
    "(^|/)(as2_test_harness|standalone_rollback_tests|[A-Za-z0-9_]+_tests)\\.exe$")

file(GLOB_RECURSE base_files RELATIVE "${DIST_BASE}" LIST_DIRECTORIES false "${DIST_BASE}/*")
set(copied 0)
set(skipped 0)
foreach(rel IN LISTS base_files)
    set(skip FALSE)
    if(rel IN_LIST overlay_names)
        set(skip TRUE)
    else()
        foreach(re IN LISTS exclude_regexes)
            if(rel MATCHES "${re}")
                set(skip TRUE)
                break()
            endif()
        endforeach()
    endif()
    if(skip)
        math(EXPR skipped "${skipped} + 1")
        continue()
    endif()
    get_filename_component(dst_dir "${pkg}/${rel}" DIRECTORY)
    file(MAKE_DIRECTORY "${dst_dir}")
    file(COPY_FILE "${DIST_BASE}/${rel}" "${pkg}/${rel}" ONLY_IF_DIFFERENT)
    math(EXPR copied "${copied} + 1")
endforeach()
message(STATUS "dist: base files copied=${copied} skipped=${skipped}")

# ---------------------------------------------------------------------------
# 2. Freshly built mod files and the Linux launcher.
# ---------------------------------------------------------------------------
function(dist_overlay src dst_name required)
    if(NOT EXISTS "${src}")
        if(required)
            message(FATAL_ERROR "make_dist: required file missing: ${src}")
        endif()
        message(STATUS "dist: optional file missing, skipped: ${src}")
        return()
    endif()
    file(COPY_FILE "${src}" "${pkg}/${dst_name}" ONLY_IF_DIFFERENT)
    file(SHA256 "${src}" hash)
    file(SIZE "${src}" size)
    message(STATUS "dist: ${dst_name}  ${hash}  (${size} bytes)")
    set(BUILD_INFO_LINES "${BUILD_INFO_LINES}${hash} *${dst_name} (${size} bytes)\n" PARENT_SCOPE)
endfunction()

set(BUILD_INFO_LINES "")
dist_overlay("${FILE_D3D9}"     "d3d9.dll"          TRUE)
dist_overlay("${FILE_CORE}"     "as2_rollback.dll"  TRUE)
dist_overlay("${FILE_WSOCK}"    "wsock32.dll"       TRUE)
dist_overlay("${FILE_SDL3}"     "SDL3.dll"          TRUE)
dist_overlay("${FILE_LAUNCHER}" "run_as2_linux.sh"  TRUE)
if(DEFINED FILE_PLAYER_README)
    dist_overlay("${FILE_PLAYER_README}" "README.txt" TRUE)
endif()

# ---------------------------------------------------------------------------
# 3. Identity: build info and a sha256sum-compatible manifest.
# ---------------------------------------------------------------------------
string(TIMESTAMP build_time "%Y-%m-%d %H:%M:%S" UTC)
dist_write_lf("${pkg}/BUILD_INFO.txt"
    "Alice Senki 2 Improvement Mod ${MOD_VERSION}\npackaged: ${build_time} UTC\nbase folder: ${DIST_BASE}\n\nmod files (sha256):\n${BUILD_INFO_LINES}")

file(GLOB_RECURSE pkg_files RELATIVE "${pkg}" LIST_DIRECTORIES false "${pkg}/*")
list(SORT pkg_files)
set(manifest "")
set(total_bytes 0)
foreach(rel IN LISTS pkg_files)
    if(rel STREQUAL "MANIFEST.sha256")
        continue()
    endif()
    file(SHA256 "${pkg}/${rel}" hash)
    file(SIZE "${pkg}/${rel}" size)
    math(EXPR total_bytes "${total_bytes} + ${size}")
    string(APPEND manifest "${hash} *${rel}\n")
endforeach()
dist_write_lf("${pkg}/MANIFEST.sha256" "${manifest}")
list(LENGTH pkg_files file_count)
math(EXPR total_mb "${total_bytes} / 1048576")
message(STATUS "dist: ${file_count} files, ${total_mb} MB, manifest written")

# ---------------------------------------------------------------------------
# 4. Zip with the package contents at the archive root (as previous releases).
#    CMake's archiver drops non-ASCII names on Windows, so .NET does the zip.
# ---------------------------------------------------------------------------
if(MAKE_ZIP)
    find_program(AS2_POWERSHELL NAMES pwsh powershell)
    if(NOT AS2_POWERSHELL)
        message(FATAL_ERROR "make_dist: PowerShell not found; set AS2_DIST_ZIP=OFF or install PowerShell")
    endif()
    set(zip "${DIST_DIR}/as2im_${MOD_VERSION}.zip")
    execute_process(
        COMMAND "${AS2_POWERSHELL}" -NoProfile -NonInteractive -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_LIST_DIR}/make_zip.ps1"
                -DistDir "${DIST_DIR}" -Zip "${zip}"
        RESULT_VARIABLE zip_result
        OUTPUT_VARIABLE zip_output
        ERROR_VARIABLE zip_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT zip_result EQUAL 0)
        message(FATAL_ERROR "make_dist: zip failed (${zip_result}): ${zip_output} ${zip_error}")
    endif()
    file(SIZE "${zip}" zip_size)
    math(EXPR zip_mb "${zip_size} / 1048576")
    message(STATUS "dist: ${zip_output} (${zip_mb} MB)")
endif()
