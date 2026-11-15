/*
 * Copyright (C) 2026 Kamil Lulko <kamil.lulko@gmail.com>
 *
 * This file is part of Guitar RackCraft.
 *
 * Guitar RackCraft is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Guitar RackCraft is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Guitar RackCraft. If not, see <https://www.gnu.org/licenses/>.
 */

#include "LV2PluginFactory.h"
#include "LV2Plugin.h"
#include "LV2Utils.h"
#include <android/log.h>
#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

#define LOG_TAG "LV2PluginFactory"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#if defined(HAVE_LV2) && HAVE_LV2 == 1
#include <lilv/lilv.h>
#endif

namespace guitarrackcraft {

#if defined(HAVE_LV2) && HAVE_LV2 == 1

LV2PluginFactory::LV2PluginFactory(const std::string& lv2Path, const std::string& nativeLibDir,
                                   const std::string& filesDir, const std::string& pluginLibDir)
    : world_(nullptr)
    , lv2Path_(lv2Path)
    , nativeLibDir_(nativeLibDir)
    , filesDir_(filesDir)
    , pluginLibDir_(pluginLibDir)
    , initialized_(false)
{
}

LV2PluginFactory::~LV2PluginFactory() {
    if (world_) {
        lilv_world_free(world_);
    }
}

bool LV2PluginFactory::initialize() {
    if (initialized_) {
        return true;
    }

    world_ = lilv_world_new();
    if (!world_) {
        LOGE("Failed to create LV2 world");
        return false;
    }

    scanRoots_ = buildScanRoots();
    LOGI("LV2 scan roots: %zu", scanRoots_.size());
    for (const std::string& root : scanRoots_) {
        LOGI("LV2 scan path: '%s'", root.c_str());
    }

    scanRoots();
    loadSpecificationsAndClasses();
    rebuildPluginInfos();

    initialized_ = true;
    LOGI("LV2 plugin factory initialized: %zu plugins found", plugins_.size());
    return true;
}

bool LV2PluginFactory::refresh() {
    if (!initialized_) {
        return initialize();
    }
    if (!world_) {
        return false;
    }

    const int loaded = scanRoots();
    if (loaded > 0) {
        // A new bundle can declare new specifications and plugin classes, so the
        // world's tables have to be refreshed before the enumeration is rebuilt.
        // Both calls are idempotent: lilv skips files it already loaded and
        // ignores plugin classes whose URI is already in its tree.
        loadSpecificationsAndClasses();
    }
    rebuildPluginInfos();

    LOGI("LV2 plugin factory refreshed: %d new bundles, %zu plugins total",
         loaded, plugins_.size());
    return true;
}

std::vector<std::string> LV2PluginFactory::buildScanRoots() const {
    std::vector<std::string> roots;
    const auto add = [&roots](const std::string& path) {
        if (path.empty()) {
            return;
        }
        for (const std::string& existing : roots) {
            if (existing == path) {
                return;
            }
        }
        roots.push_back(path);
    };

    // Extracted assets (nativeSetLv2Path) and user-imported bundles, then the
    // legacy fallbacks kept for installs that never set a path.
    add(lv2Path_);
    if (!filesDir_.empty()) {
        add(filesDir_ + "/lv2_user");
    }
    add("/data/data/com.varcain.guitarrackcraft/files/lv2");
    add("/sdcard/Android/data/com.varcain.guitarrackcraft/files/lv2");
    return roots;
}

int LV2PluginFactory::scanRoots() {
    if (!world_) {
        return 0;
    }

    int loaded = 0;
    for (const std::string& root : scanRoots_) {
        // Only bundles this world has never seen need their manifests pointed at
        // an exec-permitted binary. Rewriting the whole root on every refresh
        // re-read and re-parsed every manifest for nothing (the rewrite itself
        // is idempotent, the I/O is not free), so the scanner decides first.
        const std::vector<std::string> newBundles = bundleScanner_.findNewBundles(root);
        if (newBundles.empty()) {
            continue;
        }
        if (!nativeLibDir_.empty()) {
            for (const std::string& bundleDir : newBundles) {
                rewriteBundleManifest(bundleDir);
            }
        }
        loaded += bundleScanner_.loadBundles(world_, newBundles);
    }
    return loaded;
}

void LV2PluginFactory::loadSpecificationsAndClasses() {
    // Parse specifications and plugin classes so lilv_world_get_all_plugins()
    // returns discovered plugins.
    lilv_world_load_specifications(world_);
    lilv_world_load_plugin_classes(world_);
}

void LV2PluginFactory::rebuildPluginInfos() {
    plugins_.clear();
    int x11Count = 0;
    int modguiCount = 0;

    LV2BundleScanner::forEachPlugin(world_, [&](const LilvPlugin* plugin) {
        PluginInfo info;

        const LilvNode* uri = lilv_plugin_get_uri(plugin);
        const LilvNode* name = lilv_plugin_get_name(plugin);

        if (uri) {
            info.id = lilv_node_as_string(uri);
        }
        if (name) {
            info.name = lilv_node_as_string(name);
        }
        info.format = "LV2";

        // Absolute bundle directory, always available (modguiBasePath is not:
        // only modgui bundles have it). The UI uses it to attribute an imported
        // plugin to the directory the user imported it into.
        info.bundlePath = pluginBundlePath(plugin);

        // Get port count for info
        uint32_t numPorts = lilv_plugin_get_num_ports(plugin);
        info.ports.reserve(numPorts);

        // Discover modgui (modgui.ttl + iconTemplate)
        discoverModgui(plugin, info);

        // Discover X11UI (guiext:X11UI in the TTL). Always prefer X11 over modgui when both present.
        discoverX11UI(plugin, info);

        if (info.hasX11Ui) x11Count++;
        if (!info.modguiBasePath.empty()) modguiCount++;

        plugins_.push_back(info);
    });

    LOGI("LV2 plugin factory: %zu plugins (x11=%d, modgui=%d)",
         plugins_.size(), x11Count, modguiCount);
}

std::vector<PluginInfo> LV2PluginFactory::enumeratePlugins() {
    if (!initialized_) {
        return {};
    }
    return plugins_;
}

std::unique_ptr<IPlugin> LV2PluginFactory::createPlugin(const std::string& pluginId) {
    if (!initialized_ || !world_) {
        return nullptr;
    }

    LilvNode* uri = lilv_new_uri(world_, pluginId.c_str());
    if (!uri) {
        LOGE("Invalid plugin URI: %s", pluginId.c_str());
        return nullptr;
    }
    
    const LilvPlugins* plugins = lilv_world_get_all_plugins(world_);
    const LilvPlugin* plugin = lilv_plugins_get_by_uri(plugins, uri);
    lilv_node_free(uri);
    
    if (!plugin) {
        LOGE("LV2 plugin not found: %s", pluginId.c_str());
        return nullptr;
    }
    
    static constexpr float kDefaultSampleRate = 48000.0f;
    auto lv2Plugin = std::make_unique<LV2Plugin>(plugin, world_, kDefaultSampleRate, filesDir_);
    if (!lv2Plugin->hasInstance()) {
        LOGE("LV2 plugin could not be instantiated (binary missing or load failed): %s", pluginId.c_str());
        return nullptr;
    }
    return lv2Plugin;
}

void LV2PluginFactory::discoverModgui(const LilvPlugin* plugin, PluginInfo& info) {
    discoverModguiMetadata(plugin, info);
}

void LV2PluginFactory::discoverX11UI(const LilvPlugin* plugin, PluginInfo& info) {
    LilvNode* x11UiClass = lilv_new_uri(world_, "http://lv2plug.in/ns/extensions/ui#X11UI");
    if (!x11UiClass) return;

    LilvUIs* uis = lilv_plugin_get_uis(plugin);
    if (!uis) {
        lilv_node_free(x11UiClass);
        return;
    }

    LILV_FOREACH(uis, u, uis) {
        const LilvUI* ui = lilv_uis_get(uis, u);
        if (!lilv_ui_is_a(ui, x11UiClass)) continue;

        std::string binaryPath = resolveX11UIBinaryPath(ui, plugin, world_);
        if (binaryPath.empty()) continue;

        info.hasX11Ui = true;
        info.x11UiBinaryPath = binaryPath;
        info.x11UiUri = lilv_node_as_string(lilv_ui_get_uri(ui));
        break;
    }

    lilv_uis_free(uis);
    lilv_node_free(x11UiClass);
}

// Returns number of UI binary paths rewritten in this bundle directory.
int LV2PluginFactory::rewriteGuiextBinaryPaths(const std::string& bundleDir) {
    DIR* dir = opendir(bundleDir.c_str());
    if (!dir) return 0;

    int rewriteCount = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string name = entry->d_name;
        if (name.size() < 4 || name.substr(name.size() - 4) != ".ttl") continue;

        std::string ttlPath = bundleDir + "/" + name;
        FILE* f = fopen(ttlPath.c_str(), "r");
        if (!f) continue;

        std::string content;
        char buf[1024];
        while (fgets(buf, sizeof(buf), f)) {
            content += buf;
        }
        fclose(f);

        // Find all UI binary references and rewrite them to nativeLibDir.
        // Handles both "guiext:binary" (xputty plugins) and "ui:binary" (DPF/AIDA-X).
        bool modified = false;
        std::string newContent = content;
        static const char* kBinaryKeywords[] = { "guiext:binary", "ui:binary", nullptr };
        for (const char** kw = kBinaryKeywords; *kw; ++kw) {
            size_t kwLen = strlen(*kw);
            size_t searchPos = 0;
            while (true) {
                size_t binaryPos = newContent.find(*kw, searchPos);
                if (binaryPos == std::string::npos) break;

                size_t lt = newContent.find('<', binaryPos);
                size_t gt = (lt != std::string::npos) ? newContent.find('>', lt) : std::string::npos;
                if (lt == std::string::npos || gt == std::string::npos) {
                    searchPos = binaryPos + kwLen;
                    continue;
                }

                std::string binaryUri = newContent.substr(lt + 1, gt - lt - 1);
                // Skip if already rewritten (has file:// prefix)
                if (binaryUri.find("file://") == 0) {
                    searchPos = gt + 1;
                    continue;
                }

                size_t lastSlash = binaryUri.rfind('/');
                std::string soName = (lastSlash != std::string::npos)
                    ? binaryUri.substr(lastSlash + 1) : binaryUri;

                if (soName.size() <= 3 || soName.substr(soName.size() - 3) != ".so") {
                    searchPos = gt + 1;
                    continue;
                }

                std::string nativeLibPath = nativeLibDir_ + "/lib" + soName;
                std::string pluginLibPath = pluginLibDir_.empty() ? std::string() : (pluginLibDir_ + "/lib" + soName);
                std::string bundleUiPath = bundleDir + "/" + soName;
                std::string resolvedPath;

                if (access(nativeLibPath.c_str(), F_OK) == 0) {
                    resolvedPath = nativeLibPath;
                } else if (!pluginLibPath.empty() && access(pluginLibPath.c_str(), F_OK) == 0) {
                    resolvedPath = pluginLibPath;
                } else if (access(bundleUiPath.c_str(), F_OK) == 0) {
                    // Imported bundles ship their UI binary inside the bundle;
                    // lilv resolves that relative reference by itself, an
                    // absolute one keeps working when the bundle is moved.
                    resolvedPath = bundleUiPath;
                } else {
                    searchPos = gt + 1;
                    continue;
                }

                std::string newToken = "<file://" + resolvedPath + ">";
                newContent.replace(lt, gt - lt + 1, newToken);
                modified = true;
                rewriteCount++;
                searchPos = lt + newToken.size();
            }
        }

        if (modified) {
            FILE* fw = fopen(ttlPath.c_str(), "w");
            if (fw) {
                fwrite(newContent.c_str(), 1, newContent.size(), fw);
                fclose(fw);
            } else {
                LOGE("Failed to rewrite TTL: %s: %s", ttlPath.c_str(), strerror(errno));
            }
        }
    }
    closedir(dir);
    return rewriteCount;
}

void LV2PluginFactory::rewriteBundleManifest(const std::string& bundleDir) {
    const std::string manifestPath = bundleDir + "/manifest.ttl";
    FILE* f = fopen(manifestPath.c_str(), "r");
    if (!f) return;

    std::string content;
    char buf[1024];
    while (fgets(buf, sizeof(buf), f)) {
        content += buf;
    }
    fclose(f);

    // Find lv2:binary <soname.so> and replace it with an absolute path
    size_t binaryPos = content.find("lv2:binary");
    if (binaryPos == std::string::npos) return;

    size_t lt = content.find('<', binaryPos);
    size_t gt = content.find('>', lt != std::string::npos ? lt : 0);
    if (lt == std::string::npos || gt == std::string::npos) return;

    std::string binaryUri = content.substr(lt + 1, gt - lt - 1);
    size_t lastSlash = binaryUri.rfind('/');
    std::string soName = (lastSlash != std::string::npos)
        ? binaryUri.substr(lastSlash + 1) : binaryUri;

    if (soName.size() <= 3 || soName.substr(soName.size() - 3) != ".so") return;

    std::string nativeLibPath = nativeLibDir_ + "/lib" + soName;
    std::string pluginLibPath = pluginLibDir_.empty() ? std::string() : (pluginLibDir_ + "/lib" + soName);
    std::string bundleSoPath = bundleDir + "/" + soName;

    // Resolve which path to use for lv2:binary.
    // Search order: nativeLibDir (full flavor) → pluginLibDir (PAD extraction) → bundle fallback
    std::string binaryUriValue;
    bool haveInLibDir = (access(nativeLibPath.c_str(), F_OK) == 0);
    bool haveInPluginDir = (!pluginLibPath.empty() && access(pluginLibPath.c_str(), F_OK) == 0);
    bool haveInBundle = (access(bundleSoPath.c_str(), F_OK) == 0);

    if (haveInLibDir) {
        binaryUriValue = "file://" + nativeLibPath;
    } else if (haveInPluginDir) {
        binaryUriValue = "file://" + pluginLibPath;
    } else if (haveInBundle) {
        binaryUriValue = "file://" + bundleSoPath;
    } else {
        LOGI("rewriteBundleManifest: no binary for %s in %s", soName.c_str(), bundleDir.c_str());
        return;
    }

    // Replace ALL occurrences of <soname.so> (multi-plugin bundles share the same binary)
    std::string oldToken = "<" + binaryUri + ">";
    std::string newToken = "<" + binaryUriValue + ">";
    std::string newContent = content;
    size_t pos = 0;
    while ((pos = newContent.find(oldToken, pos)) != std::string::npos) {
        newContent.replace(pos, oldToken.size(), newToken);
        pos += newToken.size();
    }

    if (newContent == content) {
        return;
    }

    FILE* fw = fopen(manifestPath.c_str(), "w");
    if (!fw) {
        LOGE("Failed to rewrite manifest: %s: %s", manifestPath.c_str(), strerror(errno));
        return;
    }
    fwrite(newContent.c_str(), 1, newContent.size(), fw);
    fclose(fw);

    // Also rewrite guiext:binary in all .ttl files in this bundle
    const int uiRewriteCount = rewriteGuiextBinaryPaths(bundleDir);
    LOGI("rewriteBundleManifest: manifest rewritten, %d UI binaries rewritten (%s)",
         uiRewriteCount, bundleDir.c_str());
}

#else // HAVE_LV2 not defined or == 0 - stub implementation

LV2PluginFactory::LV2PluginFactory(const std::string& /*lv2Path*/, const std::string& /*nativeLibDir*/,
                                   const std::string& /*filesDir*/, const std::string& /*pluginLibDir*/)
    : world_(nullptr)
    , initialized_(false)
{
    LOGE("LV2PluginFactory: LV2 libraries not available (stub mode)");
}

LV2PluginFactory::~LV2PluginFactory() {
}

bool LV2PluginFactory::initialize() {
    initialized_ = true;
    LOGI("LV2PluginFactory: Stub mode - no plugins available");
    return true;
}

std::vector<PluginInfo> LV2PluginFactory::enumeratePlugins() {
    return {};
}

std::unique_ptr<IPlugin> LV2PluginFactory::createPlugin(const std::string& pluginId) {
    LOGE("LV2PluginFactory: Cannot create plugin - LV2 libraries not available");
    return nullptr;
}

bool LV2PluginFactory::refresh() {
    // Stub: nothing can be loaded, but the symbol has to exist — the declaration
    // lives outside the HAVE_LV2 block (IPluginFactory::refresh is part of the
    // interface), so without this definition every stub link fails with an
    // undefined reference.
    return initialize();
}

#endif // HAVE_LV2 == 1

} // namespace guitarrackcraft
