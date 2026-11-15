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

#include "PluginRegistry.h"
#include "lv2/LV2WorldLock.h"
#include <mutex>

namespace guitarrackcraft {

namespace {

/** Fill [cache] from [factory]'s current enumeration. Keys are
 *  "FORMAT:id" using the PluginInfo's own format (not the factory's primary
 *  one) so multi-format factories — VstFactory serving both VST2 and VST3 —
 *  produce correct keys. */
void cacheFactoryPlugins(IPluginFactory& factory,
                         std::unordered_map<std::string, PluginInfo>& cache) {
    const auto plugins = factory.enumeratePlugins();
    for (const auto& plugin : plugins) {
        const std::string& fmt = plugin.format.empty() ? factory.getFormat() : plugin.format;
        cache[fmt + ":" + plugin.id] = plugin;
    }
}

} // namespace

void PluginRegistry::registerFactory(std::unique_ptr<IPluginFactory> factory) {
    if (!factory) {
        return;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    factories_.push_back(std::move(factory));
}

bool PluginRegistry::initializeAll() {
    // World first, cache second (see plugin/lv2/LV2WorldLock.h). initialize()
    // creates the LV2 world and loads its bundles, which mutates it.
    lv2world::Guard worldLock = lv2world::guard();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    pluginCache_.clear();
    bool allSucceeded = true;

    for (auto& factory : factories_) {
        if (!factory->initialize()) {
            allSucceeded = false;
            continue;
        }
        cacheFactoryPlugins(*factory, pluginCache_);
    }

    return allSucceeded;
}

bool PluginRegistry::refreshAll() {
    // World first, cache second. The LV2 factory loads new bundles into the
    // live world here and re-enumerates it, while JNI entry points read the same
    // world under that same lock (plugin creation, getInfo, activation): without
    // this, importing a bundle while the rack runs corrupts lilv's model.
    lv2world::Guard worldLock = lv2world::guard();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    pluginCache_.clear();
    bool allSucceeded = true;

    for (auto& factory : factories_) {
        if (!factory->refresh()) {
            allSucceeded = false;
            continue;
        }
        cacheFactoryPlugins(*factory, pluginCache_);
    }

    return allSucceeded;
}

std::vector<PluginInfo> PluginRegistry::getAllPlugins() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<PluginInfo> allPlugins;
    allPlugins.reserve(pluginCache_.size());

    for (const auto& pair : pluginCache_) {
        allPlugins.push_back(pair.second);
    }

    return allPlugins;
}

std::unique_ptr<IPlugin> PluginRegistry::createPlugin(const std::string& pluginId) const {
    // Parse format:plugin_id
    size_t colonPos = pluginId.find(':');
    if (colonPos == std::string::npos) {
        return nullptr;
    }

    std::string format = pluginId.substr(0, colonPos);
    std::string id = pluginId.substr(colonPos + 1);

    // Shared: creating a plugin only reads the factories' state. That keeps a
    // slow factory instantiation (VST: wine fork) from blocking getAvailable
    // Plugins while still excluding a concurrent rescan. The world lock the
    // caller holds (see the header) is what keeps a rescan out of the factory.
    std::shared_lock<std::shared_mutex> lock(mutex_);

    // Find factory for this format. Multi-format factories (e.g. VstFactory
    // serving both VST2 and VST3) override acceptsFormat to claim more than
    // one format string.
    for (const auto& factory : factories_) {
        if (factory->acceptsFormat(format)) {
            return factory->createPlugin(id);
        }
    }

    return nullptr;
}

PluginInfo PluginRegistry::getPluginInfo(const std::string& pluginId) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    auto it = pluginCache_.find(pluginId);
    if (it != pluginCache_.end()) {
        return it->second;
    }
    return PluginInfo{}; // Return empty info if not found
}

} // namespace guitarrackcraft
