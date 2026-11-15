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
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Contract of [AudioSettingsManager.clampGainDb], the only Kotlin-side guard on the
 * global gain values.
 *
 * The range and the NaN policy are asserted through [AudioSettingsManager.clampGainDb]
 * rather than by reading `MIN_GAIN_DB` / `MAX_GAIN_DB` / `DEFAULT_GAIN_DB` directly:
 * they are `const val`, so the compiler inlines them into this class and a literal
 * comparison would keep passing after somebody edited them. Going through the function
 * pins the constants by their behaviour, which is what the audio path actually sees.
 *
 * The SharedPreferences getters/setters are deliberately not covered: they need a real
 * `Context`, and the project has no Robolectric (or any other Android test runtime) on
 * the JVM classpath, so a test for them here would only exercise the framework. The
 * clamping they rely on is exactly what is verified below.
 */
class AudioSettingsManagerTest {
    private val minDb = -24f
    private val maxDb = 24f
    private val unityDb = 0f

    private fun assertClamped(input: Float, expected: Float) {
        assertEquals("clampGainDb($input)", expected, AudioSettingsManager.clampGainDb(input), 0f)
    }

    @Test
    fun clampGainDbPinsTheDocumentedRange() {
        assertClamped(1e9f, maxDb)
        assertClamped(-1e9f, minDb)
        assertClamped(Float.MAX_VALUE, maxDb)
        assertClamped(-Float.MAX_VALUE, minDb)
        assertEquals(unityDb, AudioSettingsManager.clampGainDb(unityDb), 0f)
    }

    @Test
    fun clampGainDbMapsNaNDefaultToUnity() {
        assertClamped(Float.NaN, unityDb)
        assertFalse(AudioSettingsManager.clampGainDb(Float.NaN).isNaN())
    }

    @Test
    fun clampGainDbMapsInfinitiesToTheEndpoints() {
        assertClamped(Float.POSITIVE_INFINITY, maxDb)
        assertClamped(Float.NEGATIVE_INFINITY, minDb)
    }

    @Test
    fun clampGainDbKeepsEveryInRangeValueUntouched() {
        for (db in listOf(-24f, -23.9f, -12f, -6f, -0.5f, 0f, 0.5f, 6f, 12f, 23.9f, 24f)) {
            assertClamped(db, db)
        }
    }

    @Test
    fun clampGainDbPreservesTinyValuesAndTheirSign() {
        for (db in listOf(1e-9f, -1e-9f, Float.MIN_VALUE, -Float.MIN_VALUE, 1e-20f, -1e-20f)) {
            assertClamped(db, db)
        }
    }

    @Test
    fun clampGainDbAcceptsBothZeros() {
        assertClamped(0f, unityDb)
        assertClamped(-0f, unityDb)
    }

    @Test
    fun clampGainDbCoercesJustOutsideTheRange() {
        assertClamped(24.0001f, maxDb)
        assertClamped(-24.0001f, minDb)
        assertClamped(24.5f, maxDb)
        assertClamped(-24.5f, minDb)
    }

    @Test
    fun clampGainDbIsIdempotentAndAlwaysFinite() {
        val inputs = listOf(
            Float.NaN,
            Float.POSITIVE_INFINITY,
            Float.NEGATIVE_INFINITY,
            1e9f,
            -1e9f,
            -24f,
            0f,
            24f,
            -3.7f
        )

        for (db in inputs) {
            val once = AudioSettingsManager.clampGainDb(db)
            val twice = AudioSettingsManager.clampGainDb(once)

            assertTrue("clampGainDb($db) is not finite", once.isFinite())
            assertTrue("clampGainDb($db) left the range", once in minDb..maxDb)
            assertEquals("clampGainDb(clampGainDb($db))", once, twice, 0f)
        }
    }
}