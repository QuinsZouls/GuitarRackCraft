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

# CTest integration
include(GoogleTest)
gtest_discover_tests(x11_unit_tests)
gtest_discover_tests(x11_wire_tests)
if(TARGET x11_xcb_tests)
    gtest_discover_tests(x11_xcb_tests)
endif()
gtest_discover_tests(gain_unit_tests)
if(TARGET dsp_unit_tests)
    gtest_discover_tests(dsp_unit_tests)
endif()
