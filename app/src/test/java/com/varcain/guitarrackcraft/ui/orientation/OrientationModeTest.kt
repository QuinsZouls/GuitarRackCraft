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

package com.varcain.guitarrackcraft.ui.orientation

import android.content.pm.ActivityInfo
import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Persistence and window-value contract of the orientation preference.
 *
 * `ActivityInfo.SCREEN_ORIENTATION_*` are compile-time int constants, so Kotlin
 * inlines them: this test runs on a plain JVM without touching the framework.
 * The stored preference strings are pinned too, because changing one silently
 * resets every user's saved choice.
 */
class OrientationModeTest {

    @Test
    fun `every mode round-trips through its preference value`() {
        OrientationMode.entries.forEach { mode ->
            assertEquals(mode, OrientationMode.fromPrefValue(mode.prefValue))
        }
    }

    @Test
    fun `landscape is not confused with auto`() {
        assertEquals(OrientationMode.LANDSCAPE, OrientationMode.fromPrefValue("landscape"))
        assertEquals(OrientationMode.PORTRAIT, OrientationMode.fromPrefValue("portrait"))
        assertEquals(OrientationMode.AUTO, OrientationMode.fromPrefValue("auto"))
    }

    @Test
    fun `missing or unknown preference values fall back to auto`() {
        assertEquals(OrientationMode.AUTO, OrientationMode.fromPrefValue(null))
        assertEquals(OrientationMode.AUTO, OrientationMode.fromPrefValue(""))
        assertEquals(OrientationMode.AUTO, OrientationMode.fromPrefValue("Landscape"))
        assertEquals(OrientationMode.AUTO, OrientationMode.fromPrefValue(" landscape"))
        assertEquals(OrientationMode.AUTO, OrientationMode.fromPrefValue("sensor"))
    }

    @Test
    fun `each mode maps to its requestedOrientation value`() {
        assertEquals(
            ActivityInfo.SCREEN_ORIENTATION_FULL_USER,
            OrientationMode.AUTO.screenOrientation,
        )
        assertEquals(
            ActivityInfo.SCREEN_ORIENTATION_PORTRAIT,
            OrientationMode.PORTRAIT.screenOrientation,
        )
        assertEquals(
            ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE,
            OrientationMode.LANDSCAPE.screenOrientation,
        )
        assertEquals(3, OrientationMode.entries.map { it.screenOrientation }.toSet().size)
    }

    @Test
    fun `preference values stay stable for already-persisted settings`() {
        assertEquals("auto", OrientationMode.AUTO.prefValue)
        assertEquals("portrait", OrientationMode.PORTRAIT.prefValue)
        assertEquals("landscape", OrientationMode.LANDSCAPE.prefValue)
        assertEquals(3, OrientationMode.entries.map { it.prefValue }.toSet().size)
    }
}