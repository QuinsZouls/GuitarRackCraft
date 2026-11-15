/*
 * Host stand-in for the Android logging header.
 *
 * The LV2 factory and LV2Plugin are the code under test for the bundle-path
 * contract, and they are ordinary C++ plus lilv: the only Android dependency
 * they have is `__android_log_print` behind their LOGI/LOGE macros. This header
 * keeps those out of the log so the host build can compile the real translation
 * units instead of a copy of them (see tests/CMakeLists.txt,
 * lv2_factory_bundle_path_tests).
 */

#ifndef GRC_TESTS_ANDROID_LOG_STUB_H
#define GRC_TESTS_ANDROID_LOG_STUB_H

#define ANDROID_LOG_UNKNOWN 0
#define ANDROID_LOG_DEFAULT 1
#define ANDROID_LOG_VERBOSE 2
#define ANDROID_LOG_DEBUG 3
#define ANDROID_LOG_INFO 4
#define ANDROID_LOG_WARN 5
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_FATAL 7
#define ANDROID_LOG_SILENT 8

#ifdef __cplusplus
extern "C" {
#endif

static inline int __android_log_print(int priority, const char* tag, const char* format, ...) {
    (void)priority;
    (void)tag;
    (void)format;
    return 0;
}

static inline int __android_log_write(int priority, const char* tag, const char* text) {
    (void)priority;
    (void)tag;
    (void)text;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif
