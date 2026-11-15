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

package com.varcain.guitarrackcraft.ui.modgui

import com.varcain.guitarrackcraft.engine.PluginInfo
import com.varcain.guitarrackcraft.engine.PortInfo
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Assume.assumeTrue
import org.junit.Test
import java.io.File
import java.nio.file.Files

/**
 * Parsing/rendering contract of the NAM modgui panel (7 knobs: Input Lvl, Gain, Bass, Mid,
 * Treble, Presence, Output Lvl) and of the LV2 port table the DSP side appends to.
 *
 * The real submodule artifacts are used when the submodule is checked out; without them the
 * artifact-based tests are skipped so CI jobs that do not init submodules stay green.
 */
class ModguiTemplateRendererTest {

    private val namResources: File? by lazy { findNamResources() }

    private fun namResourcesOrSkip(): File {
        assumeTrue("NAM submodule resources not checked out — skipping", namResources != null)
        return namResources!!
    }

    @Test
    fun modguiTtlDeclaresSevenKnobsInSignalOrder() {
        val data = parseModguiTtlAndPluginInfo(namResourcesOrSkip().absolutePath, namPluginInfo())

        assertEquals(
            listOf("input_level", "gain", "bass", "mid", "treble", "presence", "output_level"),
            data.controls.map { it.symbol }
        )
        assertEquals(
            listOf("Input Lvl", "Gain", "Bass", "Mid", "Treble", "Presence", "Output Lvl"),
            data.controls.map { it.name }
        )
    }

    @Test
    fun everyModguiKnobBindsToAControlPortAndOnlyQualityStaysOut() {
        val pluginInfo = namPluginInfo()
        val controls = parseModguiTtlAndPluginInfo(namResourcesOrSkip().absolutePath, pluginInfo).controls
        val controlPortSymbols = pluginInfo.controlPorts.map { it.symbol }

        controls.forEach { control ->
            assertTrue("modgui knob '${control.symbol}' has no matching control port", control.symbol in controlPortSymbols)
        }
        assertEquals(
            listOf("quality_scale"),
            controlPortSymbols.filter { symbol -> controls.none { it.symbol == symbol } }
        )
    }

    @Test
    fun pluginTtlAppendsAmpPortsAfterQualityScale() {
        val ttl = normalized(File(namResourcesOrSkip(), "neural_amp_modeler.ttl.in").readText())

        assertEquals(
            listOf(
                "control", "notify", "input", "output",
                "input_level", "output_level", "quality_scale",
                "gain", "bass", "mid", "treble", "presence"
            ),
            Regex("""lv2:index (\d+); lv2:symbol "([^"]*)"""").findAll(ttl)
                .map { it.groupValues[2] }
                .toList()
        )
        assertTrue(ttl, ttl.contains("""lv2:index 7; lv2:symbol "gain"; lv2:name "Gain"; lv2:default 0.0; lv2:minimum 0.0; lv2:maximum 24.0; units:unit units:db"""))
        assertTrue(ttl, ttl.contains("""lv2:index 8; lv2:symbol "bass"; lv2:name "Bass"; lv2:default 0.0; lv2:minimum -12.0; lv2:maximum 12.0; units:unit units:db"""))
        assertTrue(ttl, ttl.contains("""lv2:index 9; lv2:symbol "mid"; lv2:name "Mid"; lv2:default 0.0; lv2:minimum -12.0; lv2:maximum 12.0; units:unit units:db"""))
        assertTrue(ttl, ttl.contains("""lv2:index 10; lv2:symbol "treble"; lv2:name "Treble"; lv2:default 0.0; lv2:minimum -12.0; lv2:maximum 12.0; units:unit units:db"""))
        assertTrue(ttl, ttl.contains("""lv2:index 11; lv2:symbol "presence"; lv2:name "Presence"; lv2:default 0.0; lv2:minimum -12.0; lv2:maximum 12.0; units:unit units:db"""))
    }

    @Test
    fun iconTemplateRendersOneKnobPerControlInDisplayOrder() {
        val resources = namResourcesOrSkip()
        val data = parseModguiTtlAndPluginInfo(resources.absolutePath, namPluginInfo())
        val rendered = renderModguiTemplate(File(resources, "modgui/icon-nam.html").readText(), data)

        assertEquals(
            listOf("input_level", "gain", "bass", "mid", "treble", "presence", "output_level"),
            Regex("""mod-role="input-control-port" mod-port-symbol="([^"]*)"""")
                .findAll(rendered).map { it.groupValues[1] }.toList()
        )
        assertEquals(
            listOf("Input Lvl", "Gain", "Bass", "Mid", "Treble", "Presence", "Output Lvl"),
            Regex("""<div class="mod-knob-title">([^<]*)</div>""")
                .findAll(rendered).map { it.groupValues[1] }.toList()
        )
        assertFalse(rendered, rendered.contains("{{#controls}}"))
    }

    @Test
    fun displayOrderComesFromLv2IndexNotFromFileOrder() {
        val baseDir = Files.createTempDirectory("modgui-index-order").toFile()
        try {
            File(baseDir, "modgui.ttl").writeText(
                """
                @prefix modgui: <http://moddevices.com/ns/modgui#>.
                @prefix lv2:    <http://lv2plug.in/ns/lv2core#>.

                <http://example.com/plugin> modgui:gui [
                    modgui:port [
                        lv2:index 2;
                        lv2:symbol "bass";
                        lv2:name "Bass";
                    ], [
                        lv2:index 0;
                        lv2:symbol "input_level";
                        lv2:name "Input Lvl";
                    ], [
                        lv2:index 1;
                        lv2:symbol "gain";
                        lv2:name "Gain";
                    ];
                ].
                """.trimIndent()
            )
            val pluginInfo = PluginInfo(
                id = "http://example.com/plugin",
                name = "Example",
                format = "LV2",
                modguiBasePath = baseDir.absolutePath,
                modguiIconTemplate = "modgui/icon-example.html",
                ports = listOf(
                    controlPort(index = 4, symbol = "input_level", name = "Input Lvl"),
                    controlPort(index = 5, symbol = "gain", name = "Gain"),
                    controlPort(index = 6, symbol = "bass", name = "Bass")
                )
            )

            val controls = parseModguiTtlAndPluginInfo(baseDir.absolutePath, pluginInfo).controls

            assertEquals(listOf("input_level", "gain", "bass"), controls.map { it.symbol })
        } finally {
            baseDir.deleteRecursively()
        }
    }

    @Test
    fun pluginWithoutModguiFallsBackToControlPortsInPortOrderSkippingToggles() {
        val baseDir = Files.createTempDirectory("modgui-absent").toFile()
        try {
            val pluginInfo = PluginInfo(
                id = namId,
                name = "Neural Amp Modeler",
                format = "LV2",
                modguiBasePath = baseDir.absolutePath,
                modguiIconTemplate = "modgui/icon-nam.html",
                ports = listOf(
                    PortInfo(index = 2, name = "Input", symbol = "input", isInput = true, isAudio = true),
                    controlPort(index = 4, symbol = "input_level", name = "Input Lvl"),
                    controlPort(index = 5, symbol = "output_level", name = "Output Lvl"),
                    controlPort(index = 6, symbol = "quality_scale", name = "Quality"),
                    PortInfo(
                        index = 12,
                        name = "Bypass",
                        symbol = "bypass",
                        isInput = true,
                        isControl = true,
                        isToggle = true
                    ),
                    controlPort(index = 7, symbol = "gain", name = "Gain")
                )
            )

            val data = parseModguiTtlAndPluginInfo(baseDir.absolutePath, pluginInfo)

            assertEquals(
                listOf("input_level", "output_level", "quality_scale", "gain"),
                data.controls.map { it.symbol }
            )
            assertEquals(listOf("Input Lvl", "Output Lvl", "Quality", "Gain"), data.controls.map { it.name })
            assertEquals(listOf("input"), data.effect.ports.audio.input.map { it.symbol })
            assertEquals(1, data.pathParameters.size)
        } finally {
            baseDir.deleteRecursively()
        }
    }

    private fun namPluginInfo(): PluginInfo = PluginInfo(
        id = namId,
        name = "Neural Amp Modeler",
        format = "LV2",
        modguiBasePath = namResourcesOrSkip().absolutePath,
        modguiIconTemplate = "modgui/icon-nam.html",
        ports = listOf(
            PortInfo(index = 2, name = "Input", symbol = "input", isInput = true, isAudio = true),
            PortInfo(index = 3, name = "Output", symbol = "output", isInput = false, isAudio = true),
            controlPort(index = 4, symbol = "input_level", name = "Input Lvl", min = -20f, max = 20f),
            controlPort(index = 5, symbol = "output_level", name = "Output Lvl", min = -20f, max = 20f),
            controlPort(index = 6, symbol = "quality_scale", name = "Quality", min = 0f, max = 1f, default = 1f),
            controlPort(index = 7, symbol = "gain", name = "Gain", max = 24f),
            controlPort(index = 8, symbol = "bass", name = "Bass", min = -12f, max = 12f),
            controlPort(index = 9, symbol = "mid", name = "Mid", min = -12f, max = 12f),
            controlPort(index = 10, symbol = "treble", name = "Treble", min = -12f, max = 12f),
            controlPort(index = 11, symbol = "presence", name = "Presence", min = -12f, max = 12f)
        )
    )

    private fun controlPort(
        index: Int,
        symbol: String,
        name: String,
        min: Float = 0f,
        max: Float = 1f,
        default: Float = 0f
    ): PortInfo = PortInfo(
        index = index,
        name = name,
        symbol = symbol,
        isInput = true,
        isAudio = false,
        isControl = true,
        isToggle = false,
        defaultValue = default,
        minValue = min,
        maxValue = max
    )

    private fun normalized(text: String): String = text.replace(Regex("\\s+"), " ").trim()

    private fun findNamResources(): File? {
        var dir: File? = File(System.getProperty("user.dir") ?: ".").absoluteFile
        while (dir != null) {
            val modguiTtl = File(dir, "$NAM_RESOURCES_RELATIVE_PATH/modgui.ttl")
            if (modguiTtl.isFile) return modguiTtl.parentFile
            dir = dir.parentFile
        }
        return null
    }

    private companion object {
        const val namId = "http://github.com/mikeoliphant/neural-amp-modeler-lv2"
        const val NAM_RESOURCES_RELATIVE_PATH = "3rd_party/neural-amp-modeler-lv2/resources"
    }
}