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

#ifndef GUITARRACKCRAFT_PLUGIN_REGISTRY_H
#define GUITARRACKCRAFT_PLUGIN_REGISTRY_H

#include <memory>
#include <shared_mutex>
#include <vector>
#include <unordered_map>
#include "IPluginFactory.h"
#include "IPlugin.h"

namespace guitarrackcraft {

/**
 * Central registry for all plugin factories.
 * Provides unified access to plugins across all formats.
 */
class PluginRegistry {
public:
    PluginRegistry() = default;
    ~PluginRegistry() = default;

    /**
     * Register a plugin factory.
     */
    void registerFactory(std::unique_ptr<IPluginFactory> factory);

    /**
     * Initialize all registered factories.
     *
     * Same lock order as refreshAll(): LV2 world (write) first, then the cache.
     */
    bool initializeAll();

    /**
     * Re-scan every factory through IPluginFactory::refresh() and rebuild the
     * plugin cache, so plugins added to or removed from disk since
     * initializeAll() are visible without recreating the registry. Factories
     * that can scan incrementally keep their existing state (LV2 keeps its
     * LilvWorld because live plugins point into it); the rest simply re-run
     * initialize().
     *
     * Takes the LV2 world lock for writing before touching the cache: the LV2
     * factory mutates the world here, and lilv/sord are not thread safe. See
     * plugin/lv2/LV2WorldLock.h for the lock order.
     */
    bool refreshAll();

    /**
     * Get all available plugins from all factories.
     *
     * Cache only: no factory (and therefore no LV2 world) is touched, so this
     * does not need the world lock.
     */
    std::vector<PluginInfo> getAllPlugins() const;

    /**
     * Create a plugin instance by ID.
     * @param pluginId Unique plugin identifier (format: "format:plugin_id")
     * @return Plugin instance, or nullptr if not found
     *
     * Reads the factory state (LV2: the world — instantiating a plugin walks
     * the world's model), so callers must hold the LV2 world lock for the whole
     * call. Taking it here instead would nest an acquisition under the one the
     * caller already holds, and the mutex is not recursive: it deadlocks.
     */
    std::unique_ptr<IPlugin> createPlugin(const std::string& pluginId) const;

    /**
     * Get plugin info by ID.
     */
    PluginInfo getPluginInfo(const std::string& pluginId) const;

private:
    /** Serializes cache rebuilds (exclusive) against plugin lookups (shared).
     *  Neither path is used by the audio thread, which only touches IPlugin
     *  instances already in the chain, so no lock enters the real-time path.
     *
     *  This is the INNER lock: every path that can reach the LV2 world takes
     *  lv2world::lock() before this one. See plugin/lv2/LV2WorldLock.h. */
    mutable std::shared_mutex mutex_;
    std::vector<std::unique_ptr<IPluginFactory>> factories_;
    std::unordered_map<std::string, PluginInfo> pluginCache_;
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_PLUGIN_REGISTRY_H
