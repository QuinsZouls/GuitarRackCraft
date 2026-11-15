/*
 * Harness configuration for the lilv sources under 3rd_party/lilv.
 *
 * lilv's waf build generates this header; the host test build compiles the
 * vendored sources directly (the repo only ships aarch64 archives), so the
 * handful of symbols lilv's sources expect are provided here instead. Values
 * that only matter on a desktop: default LV2 path and path separators.
 */

#ifndef LILV_CONFIG_H
#define LILV_CONFIG_H

#define LILV_VERSION "0.24.11"
#define HAVE_LV2 1
#define HAVE_SERD 1
#define HAVE_SORD 1
#define HAVE_SRATOM 1
#define HAVE_LSTAT 1
#define HAVE_FLOCK 1
#define HAVE_FILENO 1
#define HAVE_LIBDL 1
#define LILV_PATH_SEP ":"
#define LILV_DIR_SEP "/"
#define LILV_DEFAULT_LV2_PATH ""

#endif // LILV_CONFIG_H
