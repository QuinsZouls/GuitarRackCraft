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

package com.varcain.guitarrackcraft.ui.rack

import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Orientation contract of the rack's top bar and of the inline X11 viewport.
 *
 * The cutout inset is the only input that differs between the portrait (notch in the status
 * bar) and landscape (notch on a side edge) layouts, so these helpers pin both cases:
 * portrait keeps today's rendering, and a missing cutout never hides the meters.
 */
class RackOrientationLayoutTest {

    @Test
    fun `portrait notched band keeps the cutout height`() {
        assertEquals(30.dp, meterBandHeight(30.dp))
        assertEquals(44.dp, meterBandHeight(44.dp))
        assertEquals(24.dp, meterBandHeight(24.dp))
    }

    @Test
    fun `portrait notch splits the meter row`() {
        assertTrue(notchSplitsMeterRow(30.dp))
        assertTrue(notchSplitsMeterRow(24.dp))
    }

    @Test
    fun `band never collapses when the cutout is missing`() {
        assertEquals(24.dp, meterBandHeight(0.dp))
        assertFalse(notchSplitsMeterRow(0.dp))
    }

    @Test
    fun `band stays readable on a cutout thinner than the minimum`() {
        assertEquals(24.dp, meterBandHeight(8.dp))
    }

    @Test
    fun `unknown X11 viewport uses the fixed height when the screen is tall`() {
        assertEquals(360.dp, unknownX11ViewportHeight(Dp.Infinity))
        assertEquals(360.dp, unknownX11ViewportHeight(800.dp))
        assertEquals(360.dp, unknownX11ViewportHeight(720.dp))
    }

    @Test
    fun `unknown X11 viewport shrinks with a short viewport`() {
        assertEquals(240.dp, unknownX11ViewportHeight(480.dp))
        assertEquals(180.dp, unknownX11ViewportHeight(360.dp))
        assertEquals(160.dp, unknownX11ViewportHeight(320.dp))
    }
}