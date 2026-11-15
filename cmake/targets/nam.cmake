# Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
#
# This file is part of Guitar RackCraft.
#
# Guitar RackCraft is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# Guitar RackCraft is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.

# =============================================================================
# cmake/targets/nam.cmake — Build Neural Amp Modeler LV2 plugin
# =============================================================================

set(_nam_src     "${THIRD_PARTY}/neural-amp-modeler-lv2")
set(_nam_build   "${PROJECT_ROOT}/build/nam")
set(_nam_assets  "${ASSETS_DIR}/neural_amp_modeler.lv2")
set(_nam_lv2_id  "http://github.com/mikeoliphant/neural-amp-modeler-lv2")

# 3rd_party/patches/neural-amp-modeler-lv2 in name order (also applied by
# build.sh): Android platform compat, then the amp control ports + DSP and their
# TTL/modgui declaration.
set(_nam_patches
    "${THIRD_PARTY}/patches/neural-amp-modeler-lv2/0001-android-platform-compat.patch"
    "${THIRD_PARTY}/patches/neural-amp-modeler-lv2/0002-amp-controls-dsp.patch"
    "${THIRD_PARTY}/patches/neural-amp-modeler-lv2/0003-amp-controls-modgui.patch"
)

# ─── Phase 0: Apply the patches (configure time) ─────────────────────────────
# Phase 1 reads and copies the resources while CMake configures, but the
# ExternalProject below applies 3rd_party/patches only while it builds. On a
# pristine checkout - ./build.sh native never runs do_patch, and neither does a
# bare `cmake --preset android-arm64` - that order would stage the unpatched
# resources: a TTL declaring 3 control ports and a 2-knob modgui next to a
# 12-port .so, repaired only by the next configure. Apply them here first.
# scripts/apply-patches.sh is idempotent: "applied" when the patch is already in
# the tree, "patched" when it applies now, an error when it fits neither.
if(IS_DIRECTORY "${_nam_src}")
    foreach(_nam_patch IN LISTS _nam_patches)
        execute_process(
            COMMAND bash "${PROJECT_ROOT}/scripts/apply-patches.sh" "${_nam_patch}" "${_nam_src}"
            RESULT_VARIABLE _nam_patch_status
            OUTPUT_VARIABLE _nam_patch_stdout
            ERROR_VARIABLE  _nam_patch_stderr
        )
        if(NOT _nam_patch_status EQUAL 0)
            message(FATAL_ERROR "nam.cmake: cannot apply ${_nam_patch} to ${_nam_src}:\n"
                                "${_nam_patch_stdout}${_nam_patch_stderr}")
        endif()
        message(STATUS "NAM ${_nam_patch_stdout}")
    endforeach()
endif()

# ─── Phase 1: Process TTL templates ──────────────────────────────────────────
file(MAKE_DIRECTORY "${_nam_assets}")

# Parse the upstream version from the submodule's project() declaration so the
# TTL never goes stale on a version bump (e.g. v0.1.9 -> v0.2.0). LV2 maps the
# project minor/patch to lv2:minorVersion/lv2:microVersion.
set(_nam_ver_minor 0)
set(_nam_ver_patch 0)
grc_configure_inputs("${_nam_src}/CMakeLists.txt")
file(STRINGS "${_nam_src}/CMakeLists.txt" _nam_proj_line REGEX "project\\(.*VERSION[ \t]+[0-9]+\\.[0-9]+\\.[0-9]+")
if(_nam_proj_line MATCHES "VERSION[ \t]+([0-9]+)\\.([0-9]+)\\.([0-9]+)")
    set(_nam_ver_minor "${CMAKE_MATCH_2}")
    set(_nam_ver_patch "${CMAKE_MATCH_3}")
else()
    message(FATAL_ERROR "nam.cmake: could not parse version from ${_nam_src}/CMakeLists.txt")
endif()

# manifest.ttl
if(EXISTS "${_nam_src}/resources/manifest.ttl.in")
    grc_configure_inputs("${_nam_src}/resources/manifest.ttl.in")
    file(READ "${_nam_src}/resources/manifest.ttl.in" _manifest)
    string(REPLACE "@NAM_LV2_ID@" "${_nam_lv2_id}" _manifest "${_manifest}")
    string(REPLACE "@CMAKE_SHARED_MODULE_SUFFIX@" ".so" _manifest "${_manifest}")
    grc_write_if_changed("${_nam_assets}/manifest.ttl" "${_manifest}")
endif()

# neural_amp_modeler.ttl
if(EXISTS "${_nam_src}/resources/neural_amp_modeler.ttl.in")
    grc_configure_inputs("${_nam_src}/resources/neural_amp_modeler.ttl.in")
    file(READ "${_nam_src}/resources/neural_amp_modeler.ttl.in" _nam_ttl)
    string(REPLACE "@NAM_LV2_ID@" "${_nam_lv2_id}" _nam_ttl "${_nam_ttl}")
    string(REPLACE "@PROJECT_VERSION_MINOR@" "${_nam_ver_minor}" _nam_ttl "${_nam_ttl}")
    string(REPLACE "@PROJECT_VERSION_PATCH@" "${_nam_ver_patch}" _nam_ttl "${_nam_ttl}")
    grc_write_if_changed("${_nam_assets}/neural_amp_modeler.ttl" "${_nam_ttl}")
endif()

# Copy modgui
if(EXISTS "${_nam_src}/resources/modgui.ttl")
    configure_file("${_nam_src}/resources/modgui.ttl" "${_nam_assets}/modgui.ttl" COPYONLY)
endif()
if(IS_DIRECTORY "${_nam_src}/resources/modgui")
    grc_configure_inputs("${_nam_src}/resources/modgui")
    file(COPY "${_nam_src}/resources/modgui/" DESTINATION "${_nam_assets}/modgui/")
endif()

# ─── Phase 2: CMake cross-compile ────────────────────────────────────────────
set(_nam_so_output "${_nam_build}/neural_amp_modeler.lv2/neural_amp_modeler.so")

# The build-time pass over the same patches: ExternalProject may re-clone or
# refresh <SOURCE_DIR>, and build.sh's do_patch leaves them applied anyway.
# PATCH_COMMAND introduces the first command only, every later one needs its
# own COMMAND separator; build the whole sequence and drop the trailing one.
set(_nam_patch_commands PATCH_COMMAND)
foreach(_nam_patch IN LISTS _nam_patches)
    list(APPEND _nam_patch_commands bash "${PROJECT_ROOT}/scripts/apply-patches.sh" "${_nam_patch}" "<SOURCE_DIR>")
    list(APPEND _nam_patch_commands COMMAND)
endforeach()
list(POP_BACK _nam_patch_commands)

ExternalProject_Add(nam_build
    SOURCE_DIR      "${_nam_src}"
    BINARY_DIR      "${_nam_build}"
    INSTALL_DIR     "${_nam_assets}"
    ${_nam_patch_commands}
    CMAKE_ARGS
        -DCMAKE_POLICY_VERSION_MINIMUM=3.5
        -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
        -DANDROID_ABI=${ANDROID_ABI}
        -DANDROID_PLATFORM=${ANDROID_PLATFORM}
        -DCMAKE_BUILD_TYPE=Release
        -DCMAKE_SHARED_LINKER_FLAGS_RELEASE=-Wl,--as-needed\ -Wl,--strip-all
        -DUSE_NATIVE_ARCH=OFF
        -DSMART_BYPASS_ENABLED=ON
        -DBUILD_UTILS=OFF
        ${NDK_CCACHE_CMAKE_ARGS}
    BUILD_COMMAND   ${CMAKE_COMMAND} --build <BINARY_DIR> -j${NJOBS}
    INSTALL_COMMAND ""
    DEPENDS         lv2_libs
    BUILD_BYPRODUCTS "${_nam_so_output}"
    LOG_CONFIGURE TRUE
    LOG_BUILD TRUE
)

watch_external_sources(nam_build DIRECTORIES "${_nam_src}/src" "${_nam_src}/deps")

# ─── Phase 3: Sync to assets + jniLibs ──────────────────────────────────────
lv2_sync_to_jnilibs(nam_sync "${_nam_build}/neural_amp_modeler.lv2" "${_nam_so_output}")

add_custom_target(nam_done DEPENDS nam_sync)
