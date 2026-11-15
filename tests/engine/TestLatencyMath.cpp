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

#include <cmath>

#include "engine/LatencyMath.h"

using namespace guitarrackcraft::latency;

namespace {

constexpr double kRate48 = 48000.0;
constexpr double kRate44 = 44100.0;

// One frame at 48 kHz is ~20833.33 ns; 100 frames are 2.0833 ms.
int64_t nsForFrames(int64_t frames, double sampleRate) {
    return static_cast<int64_t>(std::llround(static_cast<double>(frames) * 1.0e9 / sampleRate));
}

} // namespace

TEST(LatencyMath, FramesFromElapsedNsUsesTheSampleRate) {
    EXPECT_NEAR(480.0, framesFromElapsedNs(10'000'000, kRate48), 0.001);
    EXPECT_NEAR(441.0, framesFromElapsedNs(10'000'000, kRate44), 0.001);
    EXPECT_DOUBLE_EQ(0.0, framesFromElapsedNs(0, kRate48));
    EXPECT_DOUBLE_EQ(0.0, framesFromElapsedNs(-5, kRate48));
    EXPECT_DOUBLE_EQ(0.0, framesFromElapsedNs(1'000'000, 0.0));
}

TEST(LatencyMath, ExtrapolatedPositionAdvancesWithTheClock) {
    const int64_t timestampNs = 1'000'000'000;
    EXPECT_EQ(1000, extrapolatedFramePosition(1000, timestampNs, timestampNs, kRate48));
    EXPECT_EQ(1480, extrapolatedFramePosition(1000, timestampNs, timestampNs + 10'000'000, kRate48));
    EXPECT_EQ(1000, extrapolatedFramePosition(1000, timestampNs, timestampNs - 10'000'000, kRate48));
}

TEST(LatencyMath, OutputLatencyIsTheFramesStillInFlight) {
    const int64_t timestampNs = 5'000'000'000;
    // The HAL is presenting frame 1000 right now and we have written 1256.
    const int64_t latency =
        outputLatencyFrames(1000, timestampNs, timestampNs, kRate48, 1256);
    EXPECT_EQ(256, latency);
    EXPECT_NEAR(5.333, framesToMs(latency, kRate48), 0.001);
}

TEST(LatencyMath, OutputLatencyExtrapolatesForwardFromTheTimestamp) {
    const int64_t timestampNs = 5'000'000'000;
    // 10 ms later the hardware has played 480 more frames: 1256 - 1480 < 0.
    const int64_t latency =
        outputLatencyFrames(1000, timestampNs, timestampNs + 10'000'000, kRate48, 1256);
    EXPECT_EQ(0, latency);
}

TEST(LatencyMath, OutputLatencyClampsNegativeToZero) {
    const int64_t timestampNs = 5'000'000'000;
    EXPECT_EQ(0, outputLatencyFrames(10'000, timestampNs, timestampNs, kRate48, 100));
}

TEST(LatencyMath, InputLatencyIsTheFramesNotReadYet) {
    const int64_t timestampNs = 7'000'000'000;
    // The device has captured up to frame 2000 and we have read 1872.
    const int64_t latency =
        inputLatencyFrames(2000, timestampNs, timestampNs, kRate48, 1872);
    EXPECT_EQ(128, latency);
    EXPECT_NEAR(2.667, framesToMs(latency, kRate48), 0.001);
}

TEST(LatencyMath, InputLatencyClampsNegativeToZero) {
    const int64_t timestampNs = 7'000'000'000;
    EXPECT_EQ(0, inputLatencyFrames(2000, timestampNs, timestampNs, kRate48, 2000));
    EXPECT_EQ(0, inputLatencyFrames(2000, timestampNs, timestampNs, kRate48, 4000));
}

TEST(LatencyMath, ZeroElapsedTimeDoesNotMoveThePosition) {
    const int64_t timestampNs = 42;
    EXPECT_EQ(0, outputLatencyFrames(500, timestampNs, timestampNs, kRate48, 500));
    EXPECT_EQ(0, inputLatencyFrames(500, timestampNs, timestampNs, kRate48, 500));
    EXPECT_EQ(64, outputLatencyFrames(500, timestampNs, timestampNs, kRate48, 564));
    EXPECT_EQ(64, inputLatencyFrames(564, timestampNs, timestampNs, kRate48, 500));
}

TEST(LatencyMath, BothSampleRatesAreHandled) {
    const int64_t timestampNs = 3'000'000'000;
    EXPECT_EQ(480, outputLatencyFrames(0, timestampNs, timestampNs, kRate48, 480));
    EXPECT_EQ(441, outputLatencyFrames(0, timestampNs, timestampNs, kRate44, 441));
    EXPECT_NEAR(10.0, framesToMs(480, kRate48), 1e-9);
    EXPECT_NEAR(10.0, framesToMs(441, kRate44), 1e-9);
}

TEST(LatencyMath, FillFallbackDoesNotAddTheBufferSize) {
    // 256 frames queued: that is the latency, the buffer capacity must not be added.
    EXPECT_EQ(256, fillLatencyFrames(1256, 1000));
    EXPECT_EQ(0, fillLatencyFrames(200, 1000));
    EXPECT_EQ(0, fillLatencyFrames(0, 0));
}

TEST(LatencyMath, FramesToMsIsZeroForAnInvalidRate) {
    EXPECT_DOUBLE_EQ(0.0, framesToMs(480, 0.0));
    EXPECT_DOUBLE_EQ(0.0, framesToMs(480, -48000.0));
    EXPECT_DOUBLE_EQ(0.0, framesToMs(0, kRate48));
}

TEST(LatencyMath, HoldReusesAValueInsideTheWindow) {
    LatencyHold hold;
    hold.store(320, 1'000'000'000);
    int64_t frames = 0;
    EXPECT_TRUE(hold.get(1'050'000'000, frames));
    EXPECT_EQ(320, frames);
    // Exactly at the window edge is still valid.
    EXPECT_TRUE(hold.get(1'100'000'000, frames));
    // Past it, the caller must fall back.
    EXPECT_FALSE(hold.get(1'100'000'001, frames));
}

TEST(LatencyMath, HoldRejectsAGarbageTimestamp) {
    LatencyHold hold;
    EXPECT_FALSE(hold.valid(1'000'000'000));
    hold.store(100, 0);
    EXPECT_FALSE(hold.valid(1'000'000'000));
    hold.store(100, 2'000'000'000);
    EXPECT_FALSE(hold.valid(1'000'000'000));
}

TEST(LatencyMath, HoldClearDropsTheValue) {
    LatencyHold hold;
    hold.store(64, 1'000'000'000);
    int64_t frames = 0;
    EXPECT_TRUE(hold.get(1'000'000'000, frames));
    hold.clear();
    EXPECT_FALSE(hold.get(1'000'000'000, frames));
}

TEST(LatencyMath, RoundTripIsTheSumOfBothDirections) {
    const int64_t timestampNs = 9'000'000'000;
    const int64_t input = inputLatencyFrames(2048, timestampNs, timestampNs, kRate48, 1920);
    const int64_t output = outputLatencyFrames(4096, timestampNs, timestampNs, kRate48, 4096 + 128);
    EXPECT_EQ(128, input);
    EXPECT_EQ(128, output);
    EXPECT_NEAR(5.333, framesToMs(input + output, kRate48), 0.001);
}

TEST(LatencyMath, ExtrapolationIsStableWithJitteringTimestamps) {
    // 1 ms of clock skew must not produce a negative or wildly different value.
    const int64_t base = 4'000'000'000;
    for (int64_t jitter = -1'000'000; jitter <= 1'000'000; jitter += 250'000) {
        const int64_t latency = outputLatencyFrames(1000, base, base + jitter, kRate48, 1200);
        EXPECT_GE(latency, 0) << "jitter=" << jitter;
        EXPECT_LE(latency, 248) << "jitter=" << jitter;
    }
    EXPECT_EQ(200, outputLatencyFrames(1000, base, base, kRate48, 1200));
    EXPECT_EQ(152, outputLatencyFrames(1000, base, base + 1'000'000, kRate48, 1200));
}

TEST(LatencyMath, FramesFromElapsedNsRejectsANegativeRate) {
    // A stream that reports a negative rate must not turn elapsed time into
    // negative frames (which would then be clamped into a fake zero latency).
    EXPECT_DOUBLE_EQ(0.0, framesFromElapsedNs(1'000'000, -48000.0));
    EXPECT_DOUBLE_EQ(0.0, framesFromElapsedNs(-1'000'000, -48000.0));
}

TEST(LatencyMath, ExtrapolationRoundsHalfAFrameUpInsteadOfTruncating) {
    // 31250 ns at 48 kHz is exactly 1.5 frames: rounding gives two frames of
    // advance, truncation would report one and understate the latency.
    const int64_t timestampNs = 1'000'000'000;
    EXPECT_NEAR(1.5, framesFromElapsedNs(31'250, kRate48), 1e-9);
    EXPECT_EQ(1002, extrapolatedFramePosition(1000, timestampNs, timestampNs + 31'250, kRate48));
    EXPECT_EQ(254, outputLatencyFrames(1000, timestampNs, timestampNs + 31'250, kRate48, 1256));

    // Just under a frame and a half still rounds to one frame of advance.
    EXPECT_EQ(1001, extrapolatedFramePosition(1000, timestampNs, timestampNs + 20'833, kRate48));
}

TEST(LatencyMath, FrameIntervalsAcrossOneSecondMatchTheRate) {
    EXPECT_EQ(48000, static_cast<int64_t>(std::llround(framesFromElapsedNs(1'000'000'000, kRate48))));
    EXPECT_EQ(44100, static_cast<int64_t>(std::llround(framesFromElapsedNs(1'000'000'000, kRate44))));
    EXPECT_EQ(nsForFrames(100, kRate48), 2'083'333);
}