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

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File

/**
 * Rules that decide which imported bundle a plugin belongs to, and which plugins
 * the browser may show.
 *
 * The fixtures are the shapes the importer really produces: `<id>/Foo.lv2/` for
 * a bundle that came in a folder or ZIP, `<id>/Foo.lv2/` under a wrapper for a
 * bare bundle, several bundles in one directory, and plugins that come from the
 * shipped assets rather than from an import.
 */
class Lv2BundleAttributionTest {

    @get:Rule
    val temporaryFolder = TemporaryFolder()

    private val filesDir: File by lazy { temporaryFolder.newFolder("files") }
    private val userDir: File by lazy { Lv2BundleRegistry.userDir(filesDir).apply { mkdirs() } }

    /** `userDir/<relative...>`, created on demand: the shapes below are the ones
     *  the importer produces (`<id>/Foo.lv2/`, `<id>/` for a wrapped bundle). */
    private fun bundleDir(vararg relative: String): File =
        File(userDir, relative.joinToString(File.separator)).apply { mkdirs() }

    @Test
    fun attributesPluginToTheBundleDirectoryItWasLoadedFrom() {
        val bundle = bundleDir("id-1", "CoolAmp.lv2")

        val attributed = Lv2BundleAttribution.urisByBundle(
            listOf(lv2("http://example.org/amp", bundle.path)),
            userDir,
        )

        assertEquals(mapOf("id-1" to listOf("http://example.org/amp")), attributed)
    }

    @Test
    fun attributesEveryPluginOfTheSameBundle() {
        val bundle = bundleDir("id-2", "Multi.lv2")

        val attributed = Lv2BundleAttribution.urisByBundle(
            listOf(
                lv2("http://example.org/a", bundle.path),
                lv2("http://example.org/b", bundle.path),
            ),
            userDir,
        )

        assertEquals(listOf("http://example.org/a", "http://example.org/b"), attributed["id-2"])
    }

    @Test
    fun attributesPluginsSittingDirectlyInTheImportedDirectory() {
        val bundle = bundleDir("id-3")

        val attributed = Lv2BundleAttribution.urisByBundle(
            listOf(lv2("http://example.org/bare", bundle.path)),
            userDir,
        )

        assertEquals(mapOf("id-3" to listOf("http://example.org/bare")), attributed)
    }

    @Test
    fun ignoresPluginsOutsideTheUserDirectory() {
        val assets = temporaryFolder.newFolder("assets", "GxPlugins.lv2", "GxAmp.lv2")

        val attributed = Lv2BundleAttribution.urisByBundle(
            listOf(lv2("http://example.org/shipped", assets.path)),
            userDir,
        )

        assertTrue(attributed.toString(), attributed.isEmpty())
    }

    @Test
    fun doesNotMatchADirectoryWhoseNameOnlyStartsWithTheUserDirectory() {
        // `files/lv2_user_old` is not inside `files/lv2_user`: without the
        // trailing separator in the prefix, a string test would claim it.
        val lookalike = File(filesDir, "lv2_user_old/Other.lv2").apply { mkdirs() }

        val attributed = Lv2BundleAttribution.urisByBundle(
            listOf(lv2("http://example.org/lookalike", lookalike.path)),
            userDir,
        )

        assertTrue(attributed.toString(), attributed.isEmpty())
    }

    @Test
    fun ignoresPluginsWithoutABundlePath() {
        val vst = PluginInfo(id = "uuid", name = "Wine Amp", format = "VST2")

        val attributed = Lv2BundleAttribution.urisByBundle(listOf(vst), userDir)

        assertTrue(attributed.toString(), attributed.isEmpty())
    }

    @Test
    fun fallsBackToTheModguiBasePathWhenTheEngineDoesNotReportABundlePath() {
        val bundle = bundleDir("id-4", "Gui.lv2")

        val attributed = Lv2BundleAttribution.urisByBundle(
            listOf(PluginInfo(id = "http://example.org/gui", format = "LV2", modguiBasePath = bundle.path)),
            userDir,
        )

        assertEquals(mapOf("id-4" to listOf("http://example.org/gui")), attributed)
    }

    @Test
    fun hidesPluginsOfRemovedBundles() {
        val plugins = listOf(
            lv2("http://example.org/kept", "/nowhere"),
            lv2("http://example.org/gone", "/nowhere"),
        )

        val visible = Lv2BundleAttribution.visiblePlugins(
            plugins,
            availableNamesLowercase = emptySet(),
            importedUris = setOf("http://example.org/kept"),
            removedUris = setOf("http://example.org/gone"),
        )

        assertEquals(listOf("http://example.org/kept"), visible.map { it.id })
    }

    @Test
    fun keepsImportedPluginsWhenTheMetadataListsSpecificPlugins() {
        // A non-empty "availablePlugins" list switches the browser to "show only
        // listed"; imported plugins can never be in that shipped list, so they
        // would all disappear.
        val plugins = listOf(
            PluginInfo(id = "http://example.org/shipped", name = "GxAmp", format = "LV2"),
            PluginInfo(id = "http://example.org/imported", name = "CoolAmp", format = "LV2"),
            PluginInfo(id = "http://example.org/unrelated", name = "Whatever", format = "LV2"),
        )

        val visible = Lv2BundleAttribution.visiblePlugins(
            plugins,
            availableNamesLowercase = setOf("gxamp"),
            importedUris = setOf("http://example.org/imported"),
            removedUris = emptySet(),
        )

        assertEquals(
            listOf("http://example.org/shipped", "http://example.org/imported"),
            visible.map { it.id },
        )
    }

    private fun lv2(uri: String, bundlePath: String): PluginInfo =
        PluginInfo(id = uri, name = uri.substringAfterLast('/'), format = "LV2", bundlePath = bundlePath)
}
