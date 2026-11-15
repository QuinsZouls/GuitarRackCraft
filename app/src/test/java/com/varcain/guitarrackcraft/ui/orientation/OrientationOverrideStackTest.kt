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

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Ordering contract of the override stack. Every test is a plain JVM test: the
 * class under test carries the orientation decision, so no Activity, Looper or
 * Robolectric is involved.
 */
class OrientationOverrideStackTest {

    private val auto = OrientationMode.AUTO.screenOrientation
    private val portrait = OrientationMode.PORTRAIT.screenOrientation
    private val landscape = OrientationMode.LANDSCAPE.screenOrientation

    @Test
    fun `empty stack has no top and falls back to the base orientation`() {
        val stack = OrientationOverrideStack(portrait)

        assertTrue(stack.isEmpty)
        assertEquals(0, stack.size)
        assertNull(stack.top())
        assertEquals(portrait, stack.effectiveOrientation())
    }

    @Test
    fun `pushing an existing token replaces it and moves it to the top`() {
        val stack = OrientationOverrideStack(auto)
        stack.push("rack", portrait)
        stack.push("modgui", landscape)
        assertEquals(landscape, stack.top())

        stack.push("rack", auto)

        assertEquals(2, stack.size)
        assertEquals(listOf("modgui", "rack"), stack.tokens())
        assertEquals(auto, stack.top())
        assertEquals(auto, stack.effectiveOrientation())
    }

    @Test
    fun `pushing the same token twice keeps a single entry`() {
        val stack = OrientationOverrideStack(auto)
        stack.push("rack", portrait)
        stack.push("rack", landscape)

        assertEquals(1, stack.size)
        assertEquals(listOf("rack"), stack.tokens())
        assertEquals(landscape, stack.effectiveOrientation())
    }

    @Test
    fun `popping the top token falls back to the override below it`() {
        val stack = OrientationOverrideStack(portrait)
        stack.push("rack", landscape)
        stack.push("modgui", auto)

        stack.pop("modgui")

        assertEquals(landscape, stack.top())
        assertEquals(landscape, stack.effectiveOrientation())
        assertFalse(stack.isEmpty)
    }

    @Test
    fun `popping a non-top token removes it from the stack`() {
        val stack = OrientationOverrideStack(auto)
        stack.push("rack", portrait)
        stack.push("modgui", landscape)
        stack.push("vst", auto)

        stack.pop("modgui")

        assertEquals(listOf("rack", "vst"), stack.tokens())
        assertEquals(2, stack.size)
        assertEquals(auto, stack.top())
    }

    @Test
    fun `popping a non-top token leaves the later top reachable in turn`() {
        val stack = OrientationOverrideStack(portrait)
        stack.push("rack", landscape)
        stack.push("modgui", auto)

        stack.pop("rack")
        stack.pop("modgui")

        assertTrue(stack.isEmpty)
        assertNull(stack.top())
        assertEquals(portrait, stack.effectiveOrientation())
    }

    @Test
    fun `popping an unknown token is safe and keeps the top`() {
        val stack = OrientationOverrideStack(portrait)
        stack.push("rack", landscape)

        stack.pop("never-pushed")

        assertEquals(listOf("rack"), stack.tokens())
        assertEquals(landscape, stack.effectiveOrientation())

        OrientationOverrideStack(auto).pop("never-pushed")
    }

    @Test
    fun `clearing restores the base orientation`() {
        val stack = OrientationOverrideStack(portrait)
        stack.push("rack", landscape)
        stack.push("modgui", auto)

        stack.clear()

        assertTrue(stack.isEmpty)
        assertTrue(stack.tokens().isEmpty())
        assertEquals(portrait, stack.effectiveOrientation())
    }

    @Test
    fun `the top override wins over the base orientation`() {
        val stack = OrientationOverrideStack(auto)
        stack.push("rack", landscape)

        assertEquals(landscape, stack.effectiveOrientation())

        stack.push("modgui", portrait)

        assertEquals(portrait, stack.effectiveOrientation())
    }

    @Test
    fun `changing the base orientation keeps an active override on top`() {
        val stack = OrientationOverrideStack(auto)
        stack.push("rack", landscape)

        stack.setBaseOrientation(portrait)

        assertEquals(landscape, stack.effectiveOrientation())

        stack.pop("rack")

        assertEquals(portrait, stack.effectiveOrientation())
    }
}