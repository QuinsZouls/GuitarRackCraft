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

package com.varcain.guitarrackcraft.engine

import java.io.File

/**
 * What belongs to an imported bundle, decided from where the engine loaded each
 * plugin instead of from what appeared between two enumerations.
 *
 * The engine reports `bundlePath` for every LV2 plugin (the directory lilv
 * loaded it from), so a plugin is part of bundle `id` when its bundle lives at
 * `userDir/id` or below — that covers both a plain `<id>/Foo.lv2/` bundle and a
 * bare bundle wrapped at import time. This works no matter what the engine was
 * doing when the import happened, which is exactly what the old
 * "URIs that appeared during the rescan" diff could not do.
 *
 * Pure functions only: no filesystem access beyond path canonicalisation, so the
 * rules are covered by JVM tests.
 */
object Lv2BundleAttribution {

    /**
     * Bundle id -> plugin URIs. Only plugins whose [PluginInfo.bundlePath] is
     * inside `userDir` are attributed; everything else (shipped bundles, VSTs)
     * is not ours to claim.
     */
    fun urisByBundle(plugins: List<PluginInfo>, userDir: File): Map<String, List<String>> {
        val root = canonicalPrefix(userDir)
        if (root.isEmpty()) return emptyMap()

        val byBundle = LinkedHashMap<String, MutableList<String>>()
        for (plugin in plugins) {
            if (plugin.id.isEmpty()) continue
            val bundle = canonicalPrefix(File(pluginPath(plugin)))
            if (bundle.isEmpty() || !bundle.startsWith(root)) continue

            val remainder = bundle.removePrefix(root).trim(File.separatorChar)
            val id = remainder.substringBefore(File.separatorChar)
            if (id.isEmpty()) continue
            byBundle.getOrPut(id) { mutableListOf() }.add(plugin.id)
        }
        return byBundle
    }

    /**
     * The plugins the browser shows: plugins of removed bundles are hidden, and
     * the metadata's "show only listed" filter never drops a plugin of an
     * imported bundle (they cannot be in the shipped metadata, so filtering them
     * by name would hide every imported plugin).
     */
    fun visiblePlugins(
        plugins: List<PluginInfo>,
        availableNamesLowercase: Set<String>,
        importedUris: Set<String>,
        removedUris: Set<String>,
    ): List<PluginInfo> = plugins.filter { plugin ->
        if (removedUris.contains(plugin.id)) {
            false
        } else if (availableNamesLowercase.isEmpty()) {
            true
        } else {
            importedUris.contains(plugin.id) ||
                availableNamesLowercase.contains(plugin.name.lowercase())
        }
    }

    /** The bundle directory the engine reported, falling back to the modgui base
     *  path for plugins enumerated before `bundlePath` existed. */
    private fun pluginPath(plugin: PluginInfo): String =
        plugin.bundlePath.ifEmpty { plugin.modguiBasePath }

    /** Canonical form of [dir] with a trailing separator, so `startsWith` is a
     *  directory test and not a string-prefix test (`lv2_user/abc` must not
     *  match `lv2_user/abcdef`). */
    private fun canonicalPrefix(dir: File): String {
        if (dir.path.isEmpty()) return ""
        val canonical = runCatching { dir.canonicalFile }.getOrDefault(dir.absoluteFile)
        val path = canonical.path
        return if (path.endsWith(File.separator)) path else path + File.separator
    }
}
