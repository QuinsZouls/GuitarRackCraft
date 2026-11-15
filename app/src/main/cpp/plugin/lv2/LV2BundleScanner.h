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

#ifndef GUITARRACKCRAFT_LV2_BUNDLE_SCANNER_H
#define GUITARRACKCRAFT_LV2_BUNDLE_SCANNER_H

#include <functional>
#include <set>
#include <string>
#include <vector>

#if defined(HAVE_LV2) && HAVE_LV2 == 1

#include <lilv/lilv.h>

namespace guitarrackcraft {

/**
 * Incremental LV2 bundle discovery for a long-lived LilvWorld.
 *
 * A LilvWorld cannot be rebuilt while plugins instantiated from it are alive
 * (an LV2Plugin keeps pointers into the world for its whole lifetime), so
 * "rescan" means: find bundles under the scan roots that have never been
 * loaded into this world and load only those. Already loaded bundles are
 * remembered in [loadedBundles] and never re-read, which keeps the operation
 * cheap and idempotent.
 *
 * Not thread safe: the caller serializes access (PluginRegistry holds the lock
 * that covers rescans and plugin creation).
 */
class LV2BundleScanner {
public:
    using PluginVisitor = std::function<void(const LilvPlugin*)>;

    /** Bundle directories already loaded into the world, absolute and without a
     *  trailing separator. */
    const std::set<std::string>& loadedBundles() const { return loadedBundles_; }

    /** Directories under [root] that contain a manifest.ttl and were never
     *  loaded, sorted by path so the scan order is reproducible. Recurses into
     *  plain subdirectories (GxPlugins.lv2 style groupings) but stops at the
     *  first manifest.ttl, which marks a bundle. */
    std::vector<std::string> findNewBundles(const std::string& root) const;

    /** Load [bundleDirs] into [world] and remember them.
     *  @return number of bundles actually loaded (a bundle whose manifest
     *          cannot be turned into a URI is left unloaded and retried on the
     *          next call). */
    int loadBundles(LilvWorld* world, const std::vector<std::string>& bundleDirs);

    /** [findNewBundles] + [loadBundles] for one root.
     *  @return number of newly loaded bundles. */
    int scanRoot(LilvWorld* world, const std::string& root);

    /** Visit every plugin in [world] exactly once, in lilv's enumeration order
     *  (which is the order bundles were loaded). Plugins that two bundles
     *  declare with the same URI are visited once: the first one wins. */
    static void forEachPlugin(LilvWorld* world, const PluginVisitor& visit);

    /** Plugin URIs in [world], deduplicated, in [forEachPlugin] order. */
    static std::vector<std::string> pluginUris(LilvWorld* world);

private:
    static void collectBundles(const std::string& root, std::vector<std::string>& out);

    std::set<std::string> loadedBundles_;
};

} // namespace guitarrackcraft

#endif // HAVE_LV2 == 1

#endif // GUITARRACKCRAFT_LV2_BUNDLE_SCANNER_H
