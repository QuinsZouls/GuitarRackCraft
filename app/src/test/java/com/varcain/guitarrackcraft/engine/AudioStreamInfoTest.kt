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
 * Contract of the JNI payload decoders.
 *
 * The native side writes a flat array and the mapping (index -> field) is the
 * only thing keeping the checklist honest: a swapped index would report a
 * granted MMAP buffer that is not there. These tests pin the order and the
 * defensive behaviour for a shorter array (native library and APK out of sync).
 */
class AudioStreamInfoTest {

    private fun fullArray(): IntArray = intArrayOf(
        1, // 0 isAAudio
        1, // 1 inputExclusive
        0, // 2 outputExclusive
        0, // 3 inputLowLatency
        1, // 4 outputLowLatency
        1, // 5 outputMMap
        1, // 6 outputCallback
        192, // 7 framesPerBurst
        0, // 8 inputMMap
        2, // 9 inputChannels
        2, // 10 outputChannels
        1, // 11 inputFormat (i16)
        0, // 12 outputFormat (float)
        1, // 13 inputPreset (voice performance)
        256, // 14 outputBufferSizeInFrames
        1024, // 15 outputBufferCapacity
        64, // 16 outputFramesPerCallback
        96, // 17 inputFramesPerBurst
        128, // 18 inputBufferSizeInFrames
        1 // 19 outputTimestampSupported
    )

    @Test
    fun `maps every field in order`() {
        val info = AudioStreamInfo.fromArray(fullArray())

        assertTrue(info.isAAudio)
        assertTrue(info.inputExclusive)
        assertFalse(info.outputExclusive)
        assertFalse(info.inputLowLatency)
        assertTrue(info.outputLowLatency)
        assertTrue(info.outputMMap)
        assertTrue(info.outputCallback)
        assertEquals(192, info.framesPerBurst)
        assertFalse(info.inputMMap)
        assertEquals(2, info.inputChannels)
        assertEquals(2, info.outputChannels)
        assertEquals(AudioStreamInfo.FORMAT_I16, info.inputFormat)
        assertEquals(AudioStreamInfo.FORMAT_FLOAT, info.outputFormat)
        assertEquals(AudioStreamInfo.PRESET_VOICE_PERFORMANCE, info.inputPreset)
        assertEquals(256, info.outputBufferSizeInFrames)
        assertEquals(1024, info.outputBufferCapacity)
        assertEquals(64, info.outputFramesPerCallback)
        assertEquals(96, info.inputFramesPerBurst)
        assertEquals(128, info.inputBufferSizeInFrames)
        assertTrue(info.outputTimestampSupported)
    }

    @Test
    fun `decodes three states per boolean field`() {
        val info = AudioStreamInfo.fromArray(fullArray())
        assertTrue(info.outputMMap)
        assertFalse(info.inputMMap)
        // Any non-zero value counts as granted, not only 1.
        val granted = fullArray().also { it[2] = 7 }
        assertTrue(AudioStreamInfo.fromArray(granted).outputExclusive)
    }

    @Test
    fun `tolerates a null or short array`() {
        assertEquals(AudioStreamInfo(), AudioStreamInfo.fromArray(null))

        val short = AudioStreamInfo.fromArray(intArrayOf(1, 1, 1))
        assertTrue(short.isAAudio)
        assertTrue(short.inputExclusive)
        assertTrue(short.outputExclusive)
        assertEquals(0, short.framesPerBurst)
        assertEquals(0, short.inputChannels)
        assertEquals(AudioStreamInfo.FORMAT_FLOAT, short.outputFormat)
        assertFalse(short.outputTimestampSupported)
    }

    @Test
    fun `tolerates a longer array`() {
        val longer = fullArray() + intArrayOf(99, 99)
        assertEquals(AudioStreamInfo.fromArray(fullArray()), AudioStreamInfo.fromArray(longer))
    }

    @Test
    fun `usesLegacyPath requires exclusive plus mmap on both streams`() {
        val best = AudioStreamInfo(
            inputExclusive = true,
            outputExclusive = true,
            inputMMap = true,
            outputMMap = true
        )
        assertFalse(best.usesLegacyPath)

        assertTrue(best.copy(inputExclusive = false).usesLegacyPath)
        assertTrue(best.copy(outputExclusive = false).usesLegacyPath)
        assertTrue(best.copy(inputMMap = false).usesLegacyPath)
        assertTrue(best.copy(outputMMap = false).usesLegacyPath)
    }

    @Test
    fun `labels describe the granted configuration`() {
        val info = AudioStreamInfo.fromArray(fullArray())
        assertEquals("16-bit PCM", info.inputFormatLabel)
        assertEquals("32-bit Float", info.outputFormatLabel)
        assertEquals("Voice Performance", info.inputPresetLabel)

        assertEquals("Unknown", AudioStreamInfo.formatLabel(42))
        assertEquals("Voice Recognition", AudioStreamInfo(inputPreset = 0).inputPresetLabel)
        assertEquals("Other", AudioStreamInfo(inputPreset = 2).inputPresetLabel)
    }

    @Test
    fun `field count matches the native array`() {
        assertEquals(AudioStreamInfo.FIELD_COUNT, fullArray().size)
    }
}

class LatencyDetailsTest {

    @Test
    fun `maps input output and round trip in order`() {
        val details = LatencyDetails.fromArray(doubleArrayOf(3.5, 8.25, 11.75))
        assertEquals(3.5, details.inputMs, 1e-9)
        assertEquals(8.25, details.outputMs, 1e-9)
        assertEquals(11.75, details.roundTripMs, 1e-9)
    }

    @Test
    fun `tolerates a null or short array`() {
        assertEquals(LatencyDetails(), LatencyDetails.fromArray(null))

        val short = LatencyDetails.fromArray(doubleArrayOf(2.0))
        assertEquals(2.0, short.inputMs, 1e-9)
        assertEquals(0.0, short.outputMs, 1e-9)
        assertEquals(0.0, short.roundTripMs, 1e-9)
    }

    @Test
    fun `field count matches the native array`() {
        assertEquals(LatencyDetails.FIELD_COUNT, 3)
    }
}