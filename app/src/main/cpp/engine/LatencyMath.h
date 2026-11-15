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

#ifndef GUITARRACKCRAFT_LATENCY_MATH_H
#define GUITARRACKCRAFT_LATENCY_MATH_H

#include <algorithm>
#include <cmath>
#include <cstdint>

/*
 * Latency arithmetic shared by the engine and its host tests.
 *
 * The old estimate was `bufferSizeInFrames + (framesWritten - framesRead)`,
 * which double counts: bufferSizeInFrames is the *capacity currently in use*
 * (an upper bound of the queue) while framesWritten - framesRead already
 * measures how much audio is queued right now. The result was a latency that
 * never went below one buffer and grew with the buffer size even when the HAL
 * was running empty.
 *
 * The honest estimate uses AAudio timestamps: a timestamp tells which frame
 * position was entering (output) or leaving (input) the audio pipeline at a
 * given CLOCK_MONOTONIC instant. Extrapolating that position to "now" and
 * subtracting it from the frame counter gives the number of frames still in
 * flight, i.e. exactly the delay between the app and the hardware.
 *
 * No Oboe, no Android: only int64 nanoseconds and a sample rate.
 */
namespace guitarrackcraft {
namespace latency {

/** Frames represented by an elapsed interval of nanoseconds. */
inline double framesFromElapsedNs(int64_t elapsedNs, double sampleRate) {
    if (elapsedNs <= 0 || sampleRate <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(elapsedNs) * sampleRate / 1.0e9;
}

/**
 * Frame position the hardware is at "now", extrapolated from the timestamp.
 * A timestamp from the future (clock skew) is treated as "now".
 */
inline int64_t extrapolatedFramePosition(int64_t framePosition, int64_t timestampNs,
                                         int64_t nowNs, double sampleRate) {
    const int64_t elapsedNs = nowNs > timestampNs ? nowNs - timestampNs : 0;
    const double frames = framesFromElapsedNs(elapsedNs, sampleRate);
    return framePosition + static_cast<int64_t>(std::llround(frames));
}

/**
 * Output latency in frames: audio we already handed to the stream but that the
 * device has not played yet.
 */
inline int64_t outputLatencyFrames(int64_t framePosition, int64_t timestampNs, int64_t nowNs,
                                   double sampleRate, int64_t framesWritten) {
    const int64_t hardwarePosition =
        extrapolatedFramePosition(framePosition, timestampNs, nowNs, sampleRate);
    return std::max<int64_t>(0, framesWritten - hardwarePosition);
}

/**
 * Input latency in frames: audio the device captured but we have not read yet.
 */
inline int64_t inputLatencyFrames(int64_t framePosition, int64_t timestampNs, int64_t nowNs,
                                  double sampleRate, int64_t framesRead) {
    const int64_t hardwarePosition =
        extrapolatedFramePosition(framePosition, timestampNs, nowNs, sampleRate);
    return std::max<int64_t>(0, hardwarePosition - framesRead);
}

/**
 * Fallback when the device cannot report a timestamp (OpenSL ES, some HALs):
 * the amount of audio currently queued. Deliberately does NOT add the buffer
 * size — the queue is the latency.
 */
inline int64_t fillLatencyFrames(int64_t producedFrames, int64_t consumedFrames) {
    return std::max<int64_t>(0, producedFrames - consumedFrames);
}

inline double framesToMs(int64_t frames, double sampleRate) {
    if (sampleRate <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(frames) * 1000.0 / sampleRate;
}

/**
 * Holds the last latency computed from a valid timestamp so a device whose
 * clock does not advance on every query (or that fails one query in a burst)
 * does not make the UI jitter. The value is only reused inside a short window,
 * after which the caller falls back to the fill estimate.
 */
struct LatencyHold {
    static constexpr uint64_t kDefaultWindowNs = 100'000'000ULL; // 100 ms

    int64_t frames = 0;
    int64_t timestampNs = 0;

    void store(int64_t value, int64_t nowNs) {
        frames = value;
        timestampNs = nowNs;
    }

    bool valid(int64_t nowNs, uint64_t windowNs = kDefaultWindowNs) const {
        if (timestampNs <= 0 || nowNs < timestampNs) {
            return false;
        }
        return static_cast<uint64_t>(nowNs - timestampNs) <= windowNs;
    }

    bool get(int64_t nowNs, int64_t& outFrames, uint64_t windowNs = kDefaultWindowNs) const {
        if (!valid(nowNs, windowNs)) {
            return false;
        }
        outFrames = frames;
        return true;
    }

    void clear() {
        frames = 0;
        timestampNs = 0;
    }
};

} // namespace latency
} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_LATENCY_MATH_H