cmake_minimum_required(VERSION 3.22.1)
project(x11_server_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fetch Google Test
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ThreadSanitizer for the host tests. Off by default: the graded build uses a
# plain Debug configuration, the race detectors are opt-in runs.
#   cmake -S tests -B build/host-tests-tsan -DCMAKE_BUILD_TYPE=Debug -DGRC_HOST_TSAN=ON
#   cmake --build build/host-tests-tsan -j8 && ctest --test-dir build/host-tests-tsan
option(GRC_HOST_TSAN "Build the host tests with ThreadSanitizer" OFF)
if(GRC_HOST_TSAN)
    add_compile_options(-fsanitize=thread -g -O1)
    add_link_options(-fsanitize=thread)
    message(STATUS "Host tests: ThreadSanitizer ENABLED")
endif()

# Source directory for X11 modules
set(X11_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/x11)
set(CPP_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp)

# Platform-independent X11 core modules (no Android deps)
add_library(x11_core STATIC
    ${X11_SRC_DIR}/X11AtomStore.cpp
    ${X11_SRC_DIR}/X11WindowManager.cpp
    ${X11_SRC_DIR}/X11PixmapStore.cpp
    ${X11_SRC_DIR}/X11ConnectionHandler.cpp
    ${X11_SRC_DIR}/X11Framebuffer.cpp
    ${X11_SRC_DIR}/X11PropertyStore.cpp
)
target_include_directories(x11_core PUBLIC
    ${X11_SRC_DIR}
    ${CPP_SRC_DIR}
)

# Unit test executable
add_executable(x11_unit_tests
    x11/TestByteOrder.cpp
    x11/TestAtomStore.cpp
    x11/TestEventBuilder.cpp
    x11/TestWindowManager.cpp
    x11/TestPixmapStore.cpp
    x11/TestConnectionHandshake.cpp
    x11/TestConnectionReply.cpp
    x11/TestFramebuffer.cpp
    x11/TestPropertyStore.cpp
)
target_link_libraries(x11_unit_tests PRIVATE x11_core gtest_main)

# Wire-level integration tests (uses POSIX sockets)
add_executable(x11_wire_tests
    x11/TestWireProtocol.cpp
    x11/TestWireImageOps.cpp
    x11/TestWireGLX.cpp
)
target_link_libraries(x11_wire_tests PRIVATE x11_core gtest_main pthread)
target_include_directories(x11_wire_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/x11)

# Optional: XCB client-level tests (requires libxcb-dev)
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(XCB QUIET xcb)
    if(XCB_FOUND)
        add_executable(x11_xcb_tests
            x11/TestXCBClient.cpp
        )
        target_link_libraries(x11_xcb_tests PRIVATE
            x11_core gtest_main pthread ${XCB_LIBRARIES})
        target_include_directories(x11_xcb_tests PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/x11 ${XCB_INCLUDE_DIRS})
        message(STATUS "XCB tests: ENABLED")
    else()
        message(STATUS "XCB tests: DISABLED (libxcb-dev not found)")
    endif()
endif()

enable_testing()

# DSP host tests. GlobalGain.h is platform-independent and has no dependency on
# the NAM submodule, so its coverage must not disappear when the NAM patches are
# not applied.
add_executable(gain_unit_tests
    dsp/TestGlobalGain.cpp
)
target_link_libraries(gain_unit_tests PRIVATE gtest_main)
target_include_directories(gain_unit_tests PRIVATE ${CPP_SRC_DIR})

# The NAM amp-control DSP lives in the neural-amp-modeler-lv2 submodule and only
# exists after
# 3rd_party/patches/neural-amp-modeler-lv2/0002-amp-controls-dsp.patch has been
# applied, so this target is skipped (not failed) on a bare checkout.
set(NAM_LV2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/neural-amp-modeler-lv2)
if(EXISTS ${NAM_LV2_DIR}/src/amp_controls.h)
    add_executable(dsp_unit_tests
        dsp/TestAmpControls.cpp
    )
    target_link_libraries(dsp_unit_tests PRIVATE gtest_main)
    target_include_directories(dsp_unit_tests PRIVATE ${NAM_LV2_DIR}/src ${CPP_SRC_DIR})

    # The port-order invariant is asserted against the real Ports struct, which
    # pulls in the LV2 and NeuralAudio headers of the same submodule (its own
    # nested submodules: `git submodule update --init --recursive <path>`).
    set(NAM_LV2_HAS_DEPS OFF)
    if(EXISTS ${NAM_LV2_DIR}/deps/lv2/include/lv2/core/lv2.h AND
       EXISTS ${NAM_LV2_DIR}/deps/NeuralAudio/NeuralAudio/NeuralModel.h)
        set(NAM_LV2_HAS_DEPS ON)
    endif()

    if(EXISTS ${NAM_LV2_DIR}/src/nam_plugin.h AND NAM_LV2_HAS_DEPS)
        target_compile_definitions(dsp_unit_tests PRIVATE GRC_HAVE_NAM_PLUGIN_HEADER=1)
        target_include_directories(dsp_unit_tests PRIVATE
            ${NAM_LV2_DIR}/deps/lv2/include
            ${NAM_LV2_DIR}/deps/NeuralAudio
        )
    else()
        message(WARNING "DSP port-order test: DISABLED (NAM submodule deps/lv2 and deps/NeuralAudio are not checked out)")
    endif()

    # The plugin integration tests drive the built LV2 plugin through its
    # exported descriptor, which needs the shared object from a host plugin
    # build:
    #   cmake -S 3rd_party/neural-amp-modeler-lv2 -B build/nam-host \
    #         -DBUILD_UTILS=OFF -DUSE_NATIVE_ARCH=OFF -DCMAKE_BUILD_TYPE=Release
    #   cmake --build build/nam-host
    # Without it the integration tests are not compiled at all (they are behind
    # GRC_NAM_PLUGIN_LIBRARY). With it, a plugin that cannot be loaded is a hard
    # FAILURE, never a skip: ctest exits 0 on a skipped test, which would leave
    # the CI test step green on a broken plugin.
    set(NAM_PLUGIN_LIBRARY "" CACHE FILEPATH
        "Built neural_amp_modeler.so, enables the NAM plugin integration tests")
    if(NOT NAM_PLUGIN_LIBRARY)
        foreach(candidate
            ${CMAKE_CURRENT_BINARY_DIR}/../nam-host/neural_amp_modeler.lv2/neural_amp_modeler.so
            ${NAM_LV2_DIR}/build/neural_amp_modeler.lv2/neural_amp_modeler.so
            ${NAM_LV2_DIR}/build/nam-host/neural_amp_modeler.lv2/neural_amp_modeler.so)
            if(NOT NAM_PLUGIN_LIBRARY AND EXISTS ${candidate})
                set(NAM_PLUGIN_LIBRARY ${candidate})
            endif()
        endforeach()
    endif()

    if(NAM_PLUGIN_LIBRARY AND NAM_LV2_HAS_DEPS)
        target_compile_definitions(dsp_unit_tests PRIVATE
            "GRC_NAM_PLUGIN_LIBRARY=\"${NAM_PLUGIN_LIBRARY}\"")
        target_include_directories(dsp_unit_tests PRIVATE ${NAM_LV2_DIR}/deps/lv2/include)
        target_link_libraries(dsp_unit_tests PRIVATE ${CMAKE_DL_LIBS})
        message(STATUS "DSP plugin integration tests: ENABLED (${NAM_PLUGIN_LIBRARY})")
    else()
        message(WARNING "DSP plugin integration tests: DISABLED (build the NAM plugin "
                        "into build/nam-host or set NAM_PLUGIN_LIBRARY)")
    endif()

    message(STATUS "DSP tests: ENABLED")
else()
    message(WARNING "DSP amp-control tests: DISABLED (apply "
                    "3rd_party/patches/neural-amp-modeler-lv2/0002-amp-controls-dsp.patch)")
endif()

# Audio engine host tests. AudioEngine.cpp itself cannot be built here (it needs
# Oboe and liblog), so only the platform-independent helpers it uses are
# covered: the latency arithmetic and the sample-format conversions. Both are
# header-only and live under engine/, which is on the include path.
add_executable(engine_unit_tests
    engine/TestLatencyMath.cpp
    engine/TestAudioFormatUtils.cpp
)
target_link_libraries(engine_unit_tests PRIVATE gtest_main)
target_include_directories(engine_unit_tests PRIVATE ${CPP_SRC_DIR})

# Lock-free utilities and the LV2 real-time mailboxes. SPSCRing.h is the only
# shared-state primitive left in the LV2 audio path (the four mutexes it
# replaces are gone), so it is covered on its own, including two-thread stress
# tests and the drop-oldest accounting the audio thread relies on.
# LV2RTMailbox.h has no lilv dependency, so it is testable even on a host
# without the LV2 stack.
add_executable(utils_unit_tests
    utils/TestSPSCRing.cpp
    utils/TestLV2RTMailbox.cpp
)
target_link_libraries(utils_unit_tests PRIVATE gtest_main pthread)
target_include_directories(utils_unit_tests PRIVATE ${CPP_SRC_DIR})

# The atom-input helper of the LV2 wrapper needs only the inline LV2 atom
# headers (no lilv, no liblilv link), so it is host-testable even though
# LV2Plugin.cpp itself is not: the repo ships liblilv only as an aarch64
# archive, and building lilv for the host would need meson plus serd/sord/
# sratom/zix, which this environment does not have. The wrapping of process()
# around this helper is covered by the arm64 build of lv2_backend.
set(LV2_HEADERS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party/lv2/include)
if(EXISTS ${LV2_HEADERS_DIR}/lv2/atom/util.h)
    add_executable(lv2_atom_unit_tests
        utils/TestLV2AtomInput.cpp
    )
    target_link_libraries(lv2_atom_unit_tests PRIVATE gtest_main)
    target_include_directories(lv2_atom_unit_tests PRIVATE ${CPP_SRC_DIR} ${LV2_HEADERS_DIR})
    message(STATUS "LV2 atom input tests: ENABLED")
else()
    message(WARNING "LV2 atom input tests: DISABLED (3rd_party/lv2 is not checked out)")
endif()

# CTest integration
include(GoogleTest)
gtest_discover_tests(x11_unit_tests)
gtest_discover_tests(x11_wire_tests)
if(TARGET x11_xcb_tests)
    gtest_discover_tests(x11_xcb_tests)
endif()
gtest_discover_tests(gain_unit_tests)
gtest_discover_tests(engine_unit_tests)
gtest_discover_tests(utils_unit_tests)
if(TARGET lv2_atom_unit_tests)
    gtest_discover_tests(lv2_atom_unit_tests)
endif()
if(TARGET dsp_unit_tests)
    gtest_discover_tests(dsp_unit_tests)
endif()
# ---------------------------------------------------------------------------
# Plugin registry refresh (no Android, no lilv): pins the contract behind
# nativeRefreshPluginRegistry — refreshAll() must reach IPluginFactory::refresh()
# and rebuild the "FORMAT:id" cache.
# ---------------------------------------------------------------------------
add_executable(plugin_registry_unit_tests
    plugin/TestPluginRegistryRefresh.cpp
    ${CPP_SRC_DIR}/plugin/PluginRegistry.cpp
)
target_link_libraries(plugin_registry_unit_tests PRIVATE gtest_main)
target_include_directories(plugin_registry_unit_tests PRIVATE ${CPP_SRC_DIR})
gtest_discover_tests(plugin_registry_unit_tests)

# ---------------------------------------------------------------------------
# Lock protocol of the rescan path (no Android, no lilv). refreshAll() and
# initializeAll() mutate the shared LilvWorld, so they must hold the process-wide
# world lock for their whole duration, or an import corrupts the world under the
# JNI entry points that instantiate plugins and read their info.
# ---------------------------------------------------------------------------
add_executable(plugin_world_lock_tests
    plugin/TestPluginRegistryWorldLock.cpp
    ${CPP_SRC_DIR}/plugin/PluginRegistry.cpp
)
target_link_libraries(plugin_world_lock_tests PRIVATE gtest_main pthread)
target_include_directories(plugin_world_lock_tests PRIVATE ${CPP_SRC_DIR})
gtest_discover_tests(plugin_world_lock_tests)

# ---------------------------------------------------------------------------
# LV2 incremental rescan against the real lilv stack.
#
# The repo's LV2 archives are aarch64 only, so the host build compiles the
# checked-out sources of serd/sord/sratom/zix/lilv directly: plain C, no meson,
# no generated file beyond lilv_config.h (tests/lv2/lilv_config.h) and the
# abstract_io layer lilv's Android fork redirects through
# (tests/lv2/abstract_io_posix.c). Skipped, never failed, on a bare checkout.
# ---------------------------------------------------------------------------
enable_language(C)

set(LV2_3P_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../3rd_party)
set(LV2_HOST_CONFIG_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lv2)
set(LV2_HOST_READY ON)
foreach(required
    serd/src/env.c
    sord/src/sord.c
    sratom/src/sratom.c
    zix/src/btree.c
    lilv/src/world.c
    lv2/include/lv2/core/lv2.h)
    if(NOT EXISTS ${LV2_3P_DIR}/${required})
        set(LV2_HOST_READY OFF)
    endif()
endforeach()

if(LV2_HOST_READY)
    add_library(lv2host_serd STATIC
        ${LV2_3P_DIR}/serd/src/base64.c
        ${LV2_3P_DIR}/serd/src/byte_source.c
        ${LV2_3P_DIR}/serd/src/env.c
        ${LV2_3P_DIR}/serd/src/n3.c
        ${LV2_3P_DIR}/serd/src/node.c
        ${LV2_3P_DIR}/serd/src/read_utf8.c
        ${LV2_3P_DIR}/serd/src/reader.c
        ${LV2_3P_DIR}/serd/src/string.c
        ${LV2_3P_DIR}/serd/src/system.c
        ${LV2_3P_DIR}/serd/src/uri.c
        ${LV2_3P_DIR}/serd/src/writer.c
    )
    target_include_directories(lv2host_serd PUBLIC
        ${LV2_3P_DIR}/serd/include
        ${LV2_3P_DIR}/serd/src
    )

    add_library(lv2host_zix STATIC
        ${LV2_3P_DIR}/zix/src/allocator.c
        ${LV2_3P_DIR}/zix/src/btree.c
        ${LV2_3P_DIR}/zix/src/bump_allocator.c
        ${LV2_3P_DIR}/zix/src/digest.c
        ${LV2_3P_DIR}/zix/src/errno_status.c
        ${LV2_3P_DIR}/zix/src/hash.c
        ${LV2_3P_DIR}/zix/src/status.c
        ${LV2_3P_DIR}/zix/src/string_view.c
    )
    target_include_directories(lv2host_zix PUBLIC ${LV2_3P_DIR}/zix/include)

    add_library(lv2host_sord STATIC
        ${LV2_3P_DIR}/sord/src/sord.c
        ${LV2_3P_DIR}/sord/src/syntax.c
    )
    target_include_directories(lv2host_sord PUBLIC
        ${LV2_3P_DIR}/sord/include
        ${LV2_3P_DIR}/sord/src
    )
    target_link_libraries(lv2host_sord PUBLIC lv2host_serd lv2host_zix)

    add_library(lv2host_sratom STATIC
        ${LV2_3P_DIR}/sratom/src/sratom.c
    )
    target_include_directories(lv2host_sratom PUBLIC
        ${LV2_3P_DIR}/sratom/include
        ${LV2_3P_DIR}/lv2/include
    )
    target_link_libraries(lv2host_sratom PUBLIC lv2host_sord lv2host_serd)

    add_library(lv2host_lilv STATIC
        ${LV2_3P_DIR}/lilv/src/collections.c
        ${LV2_3P_DIR}/lilv/src/filesystem.c
        ${LV2_3P_DIR}/lilv/src/instance.c
        ${LV2_3P_DIR}/lilv/src/lib.c
        ${LV2_3P_DIR}/lilv/src/node.c
        ${LV2_3P_DIR}/lilv/src/plugin.c
        ${LV2_3P_DIR}/lilv/src/pluginclass.c
        ${LV2_3P_DIR}/lilv/src/port.c
        ${LV2_3P_DIR}/lilv/src/query.c
        ${LV2_3P_DIR}/lilv/src/scalepoint.c
        ${LV2_3P_DIR}/lilv/src/state.c
        ${LV2_3P_DIR}/lilv/src/ui.c
        ${LV2_3P_DIR}/lilv/src/util.c
        ${LV2_3P_DIR}/lilv/src/world.c
        ${LV2_3P_DIR}/lilv/src/zix/tree.c
        ${CMAKE_CURRENT_SOURCE_DIR}/lv2/abstract_io_posix.c
    )
    # The vendored lilv sources are compiled below lilv's own I/O redirection
    # macros, so they need the abstract layer of the fork, provided by
    # abstract_io_posix.c above; LILV_INTERNAL is what its static build uses.
    target_compile_definitions(lv2host_lilv PRIVATE LILV_INTERNAL=1)
    target_include_directories(lv2host_lilv PUBLIC
        ${LV2_HOST_CONFIG_DIR}
        ${LV2_3P_DIR}/lilv
        ${LV2_3P_DIR}/lilv/src
        ${LV2_3P_DIR}/lv2/include
    )
    target_link_libraries(lv2host_lilv PUBLIC lv2host_sord lv2host_sratom lv2host_serd)
    set_target_properties(lv2host_lilv PROPERTIES C_STANDARD 99)

    add_library(lv2_test_plugin MODULE lv2/TestPluginBinary.c)
    target_include_directories(lv2_test_plugin PRIVATE ${LV2_3P_DIR}/lv2/include)
    set_target_properties(lv2_test_plugin PROPERTIES PREFIX "" OUTPUT_NAME "lv2testplugin")

    add_executable(lv2_rescan_tests
        lv2/TestLV2BundleScanner.cpp
        ${CPP_SRC_DIR}/plugin/lv2/LV2BundleScanner.cpp
    )
    target_compile_definitions(lv2_rescan_tests PRIVATE
        HAVE_LV2=1
        "GRC_LV2_TEST_PLUGIN=\"$<TARGET_FILE:lv2_test_plugin>\""
    )
    target_include_directories(lv2_rescan_tests PRIVATE ${CPP_SRC_DIR})
    target_link_libraries(lv2_rescan_tests PRIVATE gtest_main lv2host_lilv ${CMAKE_DL_LIBS})
    add_dependencies(lv2_rescan_tests lv2_test_plugin)
    gtest_discover_tests(lv2_rescan_tests)

    # The plugin -> bundle mapping the importer relies on: `PluginInfo.bundlePath`
    # is what `Lv2BundleAttribution` uses to decide which imported bundle a plugin
    # came from, and nothing asserted that the factory fills it in. This target
    # compiles the REAL factory against the vendored lilv, plus the two
    # translation units it references (LV2Plugin, and PluginUIGuard which
    # LV2Plugin's destructor calls); the only Android headers those files include
    # are logging and asset-manager declarations that the host paths never reach,
    # so lv2/stubs/ stands in for them.
    add_executable(lv2_factory_bundle_path_tests
        lv2/TestLV2PluginFactoryBundlePath.cpp
        ${CPP_SRC_DIR}/plugin/lv2/LV2PluginFactory.cpp
        ${CPP_SRC_DIR}/plugin/lv2/LV2Plugin.cpp
        ${CPP_SRC_DIR}/plugin/lv2/LV2BundleScanner.cpp
        ${CPP_SRC_DIR}/plugin/lv2/LV2Utils.cpp
        ${CPP_SRC_DIR}/plugin/PluginUIGuard.cpp
    )
    target_compile_definitions(lv2_factory_bundle_path_tests PRIVATE
        HAVE_LV2=1
        "GRC_LV2_TEST_PLUGIN=\"$<TARGET_FILE:lv2_test_plugin>\""
    )
    target_include_directories(lv2_factory_bundle_path_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/lv2/stubs
        ${CPP_SRC_DIR}
    )
    target_link_libraries(lv2_factory_bundle_path_tests PRIVATE
        gtest_main lv2host_lilv ${CMAKE_DL_LIBS} pthread)
    add_dependencies(lv2_factory_bundle_path_tests lv2_test_plugin)
    gtest_discover_tests(lv2_factory_bundle_path_tests)

    # The race the world lock closes, against the real lilv/sord: an import
    # (lilv_world_load_bundle + specifications + classes) while the rack
    # instantiates plugins and reads their info from the same world. Run under
    # -DGRC_HOST_TSAN=ON it is the race detector for lilv's model itself.
    add_executable(lv2_world_lock_tests
        lv2/TestLV2WorldLock.cpp
    )
    target_compile_definitions(lv2_world_lock_tests PRIVATE
        "GRC_LV2_TEST_PLUGIN=\"$<TARGET_FILE:lv2_test_plugin>\""
    )
    target_include_directories(lv2_world_lock_tests PRIVATE ${CPP_SRC_DIR})
    target_link_libraries(lv2_world_lock_tests PRIVATE
        gtest_main lv2host_lilv ${CMAKE_DL_LIBS} pthread)
    add_dependencies(lv2_world_lock_tests lv2_test_plugin)
    gtest_discover_tests(lv2_world_lock_tests)

    message(STATUS "LV2 rescan tests: ENABLED (host lilv built from 3rd_party)")
else()
    message(WARNING "LV2 rescan tests: DISABLED (3rd_party/{serd,sord,sratom,zix,lilv,lv2} "
                    "are not checked out)")
endif()
