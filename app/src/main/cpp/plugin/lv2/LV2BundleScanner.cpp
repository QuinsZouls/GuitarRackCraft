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

#include "LV2BundleScanner.h"

#if defined(HAVE_LV2) && HAVE_LV2 == 1

#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

namespace guitarrackcraft {

namespace {

std::string withoutTrailingSlash(const std::string& path) {
    std::string out = path;
    while (out.size() > 1 && out.back() == '/') {
        out.pop_back();
    }
    return out;
}

bool isDirectory(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool isBundleDir(const std::string& path) {
    return access((path + "/manifest.ttl").c_str(), F_OK) == 0;
}

} // namespace

void LV2BundleScanner::collectBundles(const std::string& root, std::vector<std::string>& out) {
    DIR* dir = opendir(root.c_str());
    if (!dir) {
        return;
    }

    std::vector<std::string> children;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const std::string fullPath = root + "/" + entry->d_name;
        if (isDirectory(fullPath)) {
            children.push_back(fullPath);
        }
    }
    closedir(dir);

    std::sort(children.begin(), children.end());
    for (const std::string& child : children) {
        if (isBundleDir(child)) {
            out.push_back(child);
        } else {
            collectBundles(child, out);
        }
    }
}

std::vector<std::string> LV2BundleScanner::findNewBundles(const std::string& root) const {
    std::vector<std::string> newBundles;
    if (root.empty()) {
        return newBundles;
    }

    std::vector<std::string> found;
    collectBundles(root, found);

    for (const std::string& bundle : found) {
        if (loadedBundles_.find(bundle) == loadedBundles_.end()) {
            newBundles.push_back(bundle);
        }
    }
    return newBundles;
}

int LV2BundleScanner::loadBundles(LilvWorld* world, const std::vector<std::string>& bundleDirs) {
    if (!world) {
        return 0;
    }

    int loaded = 0;
    for (const std::string& bundleDir : bundleDirs) {
        if (bundleDir.empty() || loadedBundles_.count(bundleDir) != 0) {
            continue;
        }
        const std::string dir = withoutTrailingSlash(bundleDir);
        const std::string pathWithSlash = dir + "/";

        LilvNode* bundleUri = lilv_new_file_uri(world, nullptr, pathWithSlash.c_str());
        if (!bundleUri) {
            continue;
        }
        lilv_world_load_bundle(world, bundleUri);
        lilv_node_free(bundleUri);
        loadedBundles_.insert(dir);
        ++loaded;
    }
    return loaded;
}

int LV2BundleScanner::scanRoot(LilvWorld* world, const std::string& root) {
    return loadBundles(world, findNewBundles(root));
}

void LV2BundleScanner::forEachPlugin(LilvWorld* world, const PluginVisitor& visit) {
    if (!world || !visit) {
        return;
    }

    const LilvPlugins* plugins = lilv_world_get_all_plugins(world);
    if (!plugins) {
        return;
    }

    std::set<std::string> seen;
    LILV_FOREACH(plugins, i, plugins) {
        const LilvPlugin* plugin = lilv_plugins_get(plugins, i);
        if (!plugin) {
            continue;
        }
        const LilvNode* uri = lilv_plugin_get_uri(plugin);
        if (!uri) {
            continue;
        }
        const char* uriString = lilv_node_as_string(uri);
        if (!uriString) {
            continue;
        }
        if (!seen.insert(uriString).second) {
            continue;
        }
        visit(plugin);
    }
}

std::vector<std::string> LV2BundleScanner::pluginUris(LilvWorld* world) {
    std::vector<std::string> uris;
    forEachPlugin(world, [&uris](const LilvPlugin* plugin) {
        const LilvNode* uri = lilv_plugin_get_uri(plugin);
        if (uri) {
            uris.emplace_back(lilv_node_as_string(uri));
        }
    });
    return uris;
}

} // namespace guitarrackcraft

#endif // HAVE_LV2 == 1
