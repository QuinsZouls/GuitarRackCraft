/*
 * Host stand-in for Android's asset-manager headers.
 *
 * LV2PluginFactory includes them for the asset-extraction paths, which the host
 * tests never reach (the factory is constructed with an explicit lv2Path, so no
 * AAssetManager is involved). Keeping the header empty is enough to compile the
 * real translation unit; no symbol from it is referenced.
 */

#ifndef GRC_TESTS_ANDROID_ASSET_MANAGER_STUB_H
#define GRC_TESTS_ANDROID_ASSET_MANAGER_STUB_H

struct AAssetManager;
struct AAsset;
struct AAssetDir;

#endif
