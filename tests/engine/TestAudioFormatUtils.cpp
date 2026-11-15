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

#include <gtest/gtest.h>

#include "engine/AudioFormatUtils.h"

using namespace guitarrackcraft::audioformat;

namespace {

constexpr float kTolerance = 1.0f / 32768.0f;

} // namespace

TEST(AudioFormatUtils, I16ToFloatMapsFullScaleNegativeToMinusOne) {
    EXPECT_FLOAT_EQ(-1.0f, i16ToFloat(static_cast<int16_t>(-32768)));
    EXPECT_FLOAT_EQ(0.0f, i16ToFloat(static_cast<int16_t>(0)));
    EXPECT_NEAR(32767.0f / 32768.0f, i16ToFloat(static_cast<int16_t>(32767)), kTolerance);
}

TEST(AudioFormatUtils, I16ToFloatKeepsLinearityAroundZero) {
    EXPECT_FLOAT_EQ(-0.5f, i16ToFloat(static_cast<int16_t>(-16384)));
    EXPECT_FLOAT_EQ(0.25f, i16ToFloat(static_cast<int16_t>(8192)));
    EXPECT_NEAR(1.0f / 32768.0f, i16ToFloat(static_cast<int16_t>(1)), kTolerance);
}

TEST(AudioFormatUtils, FloatToI16SaturatesBeyondFullScale) {
    EXPECT_EQ(32767, floatToI16(1.0f));
    EXPECT_EQ(32767, floatToI16(12.5f));
    EXPECT_EQ(-32767, floatToI16(-1.0f));
    EXPECT_EQ(-32767, floatToI16(-8.0f));
}

TEST(AudioFormatUtils, FloatToI16RoundsHalfAwayFromZero) {
    // 0.5 * 32767 = 16383.5 -> 16384 (truncation would give 16383)
    EXPECT_EQ(16384, floatToI16(0.5f));
    EXPECT_EQ(-16384, floatToI16(-0.5f));
    // 0.25 * 32767 = 8191.75 -> 8192
    EXPECT_EQ(8192, floatToI16(0.25f));
    EXPECT_EQ(0, floatToI16(0.0f));
}

TEST(AudioFormatUtils, FloatToI16NeverProducesTheUnrepresentableMinimum) {
    for (float sample = -1.0f; sample <= 1.0f; sample += 0.01f) {
        const int16_t encoded = floatToI16(sample);
        EXPECT_GE(encoded, -32767);
        EXPECT_LE(encoded, 32767);
    }
}

TEST(AudioFormatUtils, FloatToI16ScalesByThePositiveFullScale) {
    // 0.9999 * 32767 = 32763.72 -> 32764. A scale of 32768 would give 32765 and
    // an unscaled truncation 32763, so this pins the constant and the rounding.
    EXPECT_EQ(32764, floatToI16(0.9999f));
    EXPECT_EQ(-16384, floatToI16(-0.5f));
    EXPECT_EQ(3277, floatToI16(0.1f));
    EXPECT_EQ(-8192, floatToI16(-0.25f));
}

TEST(AudioFormatUtils, ClampUnitIsSymmetric) {
    EXPECT_FLOAT_EQ(1.0f, clampUnit(1.0f));
    EXPECT_FLOAT_EQ(1.0f, clampUnit(1.0001f));
    EXPECT_FLOAT_EQ(-1.0f, clampUnit(-1.0f));
    EXPECT_FLOAT_EQ(-1.0f, clampUnit(-1.0001f));
    EXPECT_FLOAT_EQ(0.0f, clampUnit(0.0f));
}

TEST(AudioFormatUtils, BuffersConvertEverySample) {
    const int16_t source[] = {-32768, -1, 0, 1, 32767};
    float converted[5] = {};
    i16ToFloatBuffer(source, converted, 5);
    EXPECT_FLOAT_EQ(-1.0f, converted[0]);
    EXPECT_FLOAT_EQ(0.0f, converted[2]);
    EXPECT_NEAR(32767.0f / 32768.0f, converted[4], kTolerance);

    // The round trip is not bit exact: 32767/32768 scaled back by 32767 is
    // 32766.0, i.e. one LSB below full scale. That is the expected asymmetry of
    // symmetric full-scale mapping, so only closeness is asserted.
    int16_t roundTrip[5] = {};
    floatToI16Buffer(converted, roundTrip, 5);
    EXPECT_EQ(-32767, roundTrip[0]);
    EXPECT_EQ(0, roundTrip[2]);
    EXPECT_NEAR(32767.0, static_cast<double>(roundTrip[4]), 1.0);
    EXPECT_LE(roundTrip[4], 32767);
}

TEST(AudioFormatUtils, StereoToMonoAveragesBothChannels) {
    const float interleaved[] = {1.0f, 0.0f, -1.0f, 1.0f, 0.25f, -0.25f, 0.5f, 0.5f};
    float mono[4] = {};
    stereoToMono(interleaved, mono, 4);
    EXPECT_FLOAT_EQ(0.5f, mono[0]);
    EXPECT_FLOAT_EQ(0.0f, mono[1]);
    EXPECT_FLOAT_EQ(0.0f, mono[2]);
    EXPECT_FLOAT_EQ(0.5f, mono[3]);
}

TEST(AudioFormatUtils, StereoToMonoOfSilenceIsSilence) {
    const float interleaved[] = {0.0f, 0.0f, 0.0f, 0.0f};
    float mono[2] = {1.0f, 1.0f};
    stereoToMono(interleaved, mono, 2);
    EXPECT_FLOAT_EQ(0.0f, mono[0]);
    EXPECT_FLOAT_EQ(0.0f, mono[1]);
}

TEST(AudioFormatUtils, I16StereoToMonoFloatAveragesAndScales) {
    const int16_t interleaved[] = {32767, -32768, 16384, 16384, -32768, -32768};
    float mono[3] = {};
    i16StereoToMonoFloat(interleaved, mono, 3);
    EXPECT_NEAR((32767.0f / 32768.0f - 1.0f) * 0.5f, mono[0], kTolerance);
    EXPECT_NEAR(0.5f, mono[1], kTolerance);
    EXPECT_FLOAT_EQ(-1.0f, mono[2]);
}

TEST(AudioFormatUtils, MonoI16ConversionMatchesScalarHelper) {
    const int16_t source[] = {-32768, -1000, 0, 1000, 32767};
    float buffer[5] = {};
    i16ToFloatBuffer(source, buffer, 5);
    for (size_t i = 0; i < 5; ++i) {
        EXPECT_FLOAT_EQ(i16ToFloat(source[i]), buffer[i]);
    }
}
TEST(AudioFormatUtils, InterleavedToMonoAveragesEveryChannel) {
    // A HAL that grants more channels than requested must still be downmixed,
    // otherwise the extra channels would be read as if they were the next frame.
    const float interleaved[] = {1.0f, 0.0f, 0.0f, 0.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    float mono[2] = {};
    interleavedToMono(interleaved, mono, 2, 4);
    EXPECT_FLOAT_EQ(0.25f, mono[0]);
    EXPECT_FLOAT_EQ(-1.0f, mono[1]);
}

TEST(AudioFormatUtils, InterleavedToMonoCopiesAMonoSource) {
    const float interleaved[] = {0.5f, -0.5f, 0.0f};
    float mono[3] = {};
    interleavedToMono(interleaved, mono, 3, 1);
    EXPECT_FLOAT_EQ(0.5f, mono[0]);
    EXPECT_FLOAT_EQ(-0.5f, mono[1]);
    EXPECT_FLOAT_EQ(0.0f, mono[2]);
}

TEST(AudioFormatUtils, InterleavedI16ToMonoScalesEveryChannel) {
    const int16_t interleaved[] = {16384, 16384, 16384, 16384, -32768, -32768, -32768, -32768};
    float mono[2] = {};
    interleavedI16ToMonoFloat(interleaved, mono, 2, 4);
    EXPECT_NEAR(0.5f, mono[0], kTolerance);
    EXPECT_FLOAT_EQ(-1.0f, mono[1]);
}

TEST(AudioFormatUtils, InterleavedI16ToMonoMatchesTheStereoHelper) {
    const int16_t interleaved[] = {32767, -32768, 16000, -16000};
    float viaStride[2] = {};
    float viaStereo[2] = {};
    interleavedI16ToMonoFloat(interleaved, viaStride, 2, 2);
    i16StereoToMonoFloat(interleaved, viaStereo, 2);
    EXPECT_FLOAT_EQ(viaStereo[0], viaStride[0]);
    EXPECT_FLOAT_EQ(viaStereo[1], viaStride[1]);
}
