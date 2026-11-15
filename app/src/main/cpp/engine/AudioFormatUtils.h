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

#ifndef GUITARRACKCRAFT_AUDIO_FORMAT_UTILS_H
#define GUITARRACKCRAFT_AUDIO_FORMAT_UTILS_H

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

/*
 * Sample-format conversions used by the audio callback.
 *
 * The engine negotiates the stream configuration at runtime (Oboe can grant
 * 16-bit PCM instead of float, or a stereo input instead of mono), so the
 * callback has to convert between all granted layouts and its internal
 * mono-float / stereo-float buffers. Everything here is allocation free,
 * branch-light and safe to call from the real-time thread.
 *
 * The header is intentionally free of Oboe and Android dependencies so the
 * arithmetic can be unit tested on the host.
 */
namespace guitarrackcraft {
namespace audioformat {

constexpr float kI16ToFloatScale = 1.0f / 32768.0f;
constexpr float kFloatToI16Scale = 32767.0f;

inline float clampUnit(float sample) {
    return std::max(-1.0f, std::min(1.0f, sample));
}

inline float i16ToFloat(int16_t sample) {
    return static_cast<float>(sample) * kI16ToFloatScale;
}

/*
 * Rounds half away from zero (std::lround) instead of truncating: truncation
 * biases every sample towards zero and Audacity-style analysis of a recorded
 * file shows it as a DC offset at low levels. +/-1.0 maps to +/-32767, so the
 * most negative representable value (-32768) is not reproduced by a round
 * trip; that matches the usual convention and keeps full scale symmetric.
 */
inline int16_t floatToI16(float sample) {
    const float scaled = clampUnit(sample) * kFloatToI16Scale;
    const long rounded = std::lround(scaled);
    if (rounded > 32767L) return 32767;
    if (rounded < -32767L) return -32767;
    return static_cast<int16_t>(rounded);
}

inline void i16ToFloatBuffer(const int16_t* source, float* destination, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        destination[i] = i16ToFloat(source[i]);
    }
}

inline void floatToI16Buffer(const float* source, int16_t* destination, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        destination[i] = floatToI16(source[i]);
    }
}

/**
 * Averages `channels` interleaved float samples per frame into a mono buffer.
 * The HAL occasionally grants more channels than requested, so the stride is a
 * parameter instead of being hard-coded to two.
 */
inline void interleavedToMono(const float* interleaved, float* destination, size_t frames,
                              int32_t channels) {
    if (channels <= 1) {
        for (size_t i = 0; i < frames; ++i) {
            destination[i] = interleaved[i];
        }
        return;
    }
    const float scale = 1.0f / static_cast<float>(channels);
    for (size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (int32_t channel = 0; channel < channels; ++channel) {
            sum += interleaved[i * static_cast<size_t>(channels) + channel];
        }
        destination[i] = sum * scale;
    }
}

/** Averages interleaved 16-bit samples per frame into a mono float buffer. */
inline void interleavedI16ToMonoFloat(const int16_t* interleaved, float* destination, size_t frames,
                                      int32_t channels) {
    if (channels <= 1) {
        i16ToFloatBuffer(interleaved, destination, frames);
        return;
    }
    const float scale = 1.0f / static_cast<float>(channels);
    for (size_t i = 0; i < frames; ++i) {
        float sum = 0.0f;
        for (int32_t channel = 0; channel < channels; ++channel) {
            sum += i16ToFloat(interleaved[i * static_cast<size_t>(channels) + channel]);
        }
        destination[i] = sum * scale;
    }
}

/** Averages interleaved L/R float pairs into a mono buffer. */
inline void stereoToMono(const float* interleaved, float* destination, size_t frames) {
    interleavedToMono(interleaved, destination, frames, 2);
}

/** Converts interleaved 16-bit L/R pairs into a mono float buffer (average). */
inline void i16StereoToMonoFloat(const int16_t* interleaved, float* destination, size_t frames) {
    interleavedI16ToMonoFloat(interleaved, destination, frames, 2);
}

} // namespace audioformat
} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_AUDIO_FORMAT_UTILS_H