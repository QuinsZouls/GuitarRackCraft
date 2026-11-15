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

#ifndef GUITARRACKCRAFT_DSP_GLOBAL_GAIN_H
#define GUITARRACKCRAFT_DSP_GLOBAL_GAIN_H

#include <cmath>
#include <cstdint>

namespace guitarrackcraft {

/**
 * Global input/output gain stage: a dB target in [-24, +24] around unity,
 * converted to a linear factor and applied as a per-block linear-domain ramp.
 *
 * The ramp completes inside the block that carries the new target, so a block
 * of 64 frames at 48 kHz reaches the new value in ~1.3 ms and a block of 480
 * frames in 10 ms — a live UI drag never produces zipper noise and never
 * overshoots. A change smaller than kSnapEpsilon in linear domain is snapped
 * instead of ramped, so the gain cannot creep forever towards its target.
 *
 * Threading: the ramp state lives in this object, which belongs to the audio
 * thread only. The owner publishes the target elsewhere (typically a
 * std::atomic<float> dB value) and passes it to process() once per block;
 * reading an atomic is the caller's job, not this helper's.
 *
 * Real-time safety: no allocation, no locks, no logging, no exceptions, no
 * virtual calls — only fixed-size arithmetic in the callback path.
 */
class GlobalGain {
public:
    static constexpr float kMinDb = -24.0f;
    static constexpr float kMaxDb = 24.0f;
    static constexpr float kUnityDb = 0.0f;
    /** Linear-domain difference below which the ramp snaps to the target. */
    static constexpr float kSnapEpsilon = 1e-6f;

    /** Clamp a user-facing dB value to the supported range (NaN maps to kMinDb). */
    static float clampDb(float db) {
        return std::fmin(std::fmax(db, kMinDb), kMaxDb);
    }

    /** Convert a dB value to a linear gain factor (0 dB -> exactly 1.0f). */
    static float dbToLinear(float db) {
        return std::pow(10.0f, clampDb(db) * 0.05f);
    }

    /**
     * Apply the gain in place to one buffer (mono tap, or one channel).
     * Per-frame gain is start + step*(i+1) and the last frame lands exactly on
     * the target, so the ramp is monotonic and never overshoots.
     */
    void process(float* buffer, int32_t numFrames, float targetDb) {
        float* const buffers[1] = {buffer};
        processBuffers(buffers, 1, numFrames, targetDb);
    }

    /** Apply the same gain, with one shared ramp, to a pair of buffers. */
    void process(float* left, float* right, int32_t numFrames, float targetDb) {
        float* const buffers[2] = {left, right};
        processBuffers(buffers, 2, numFrames, targetDb);
    }

    /** Jump straight to the target without ramping (engine start, not the callback). */
    void snapTo(float targetDb) { current_ = dbToLinear(targetDb); }

    /** Return to unity, bypassing any ramp (tests, or a host that wants silence-free reset). */
    void reset() { current_ = 1.0f; }

    /** Linear gain currently reached, i.e. where the next ramp starts. */
    float currentGain() const { return current_; }

private:
    void processBuffers(float* const* buffers, int numChannels, int32_t numFrames,
                        float targetDb) {
        if (numFrames <= 0 || numChannels <= 0) return;

        const float target = dbToLinear(targetDb);
        const float start = current_;
        const float delta = target - start;
        current_ = target;

        if (std::fabs(delta) <= kSnapEpsilon) {
            if (target == 1.0f) return;
            for (int c = 0; c < numChannels; ++c) {
                float* buffer = buffers[c];
                if (!buffer) continue;
                for (int32_t i = 0; i < numFrames; ++i) buffer[i] *= target;
            }
            return;
        }

        const float step = delta / static_cast<float>(numFrames);
        for (int c = 0; c < numChannels; ++c) {
            float* buffer = buffers[c];
            if (!buffer) continue;
            for (int32_t i = 0; i < numFrames - 1; ++i) {
                buffer[i] *= start + step * static_cast<float>(i + 1);
            }
            buffer[numFrames - 1] *= target;
        }
    }

    float current_ = 1.0f;
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_DSP_GLOBAL_GAIN_H