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

#ifndef GUITARRACKCRAFT_OFFLINE_PROCESSOR_H
#define GUITARRACKCRAFT_OFFLINE_PROCESSOR_H

#include <string>
#include <functional>
#include <cstdint>
#include "dsp/GlobalGain.h"
#include "plugin/PluginChain.h"

namespace guitarrackcraft {

/**
 * Processes audio files offline through the plugin chain.
 * Not real-time, so can use blocking operations.
 */
class OfflineProcessor {
public:
    using ProgressCallback = std::function<void(float progress)>;

    OfflineProcessor(PluginChain& chain);
    ~OfflineProcessor() = default;

    /**
     * Process an audio file through the plugin chain.
     * @param inputPath Path to input WAV file
     * @param outputPath Path to output WAV file
     * @param progressCallback Optional callback for progress updates (0.0-1.0)
     * @param inputGainDb Global input gain in dB, applied to the input block
     *        before the chain (default 0 dB = unity)
     * @param outputGainDb Global output gain in dB, applied to the output block
     *        after the chain (default 0 dB = unity)
     * @return true if processing succeeded
     */
    bool processFile(
        const std::string& inputPath,
        const std::string& outputPath,
        ProgressCallback progressCallback = nullptr,
        float inputGainDb = GlobalGain::kUnityDb,
        float outputGainDb = GlobalGain::kUnityDb);

    /**
     * Apply a global gain to one stereo block with a constant factor.
     *
     * Offline processing is not real-time, so the zipper noise a live ramp
     * exists to hide cannot happen here; more importantly the exported file must
     * carry exactly the steady-state gain the live engine converges to, so the
     * helper snaps to the target for every block instead of ramping across it.
     * Unity (0 dB) leaves the samples bit-identical, and the dB value is clamped
     * by GlobalGain exactly like the live path (NaN maps to GlobalGain::kMinDb).
     *
     * @param left Left channel block, scaled in place
     * @param right Right channel block, scaled in place
     * @param numFrames Frames in each block
     * @param gainDb Gain in dB
     */
    static void applyConstantGainDb(float* left, float* right, int32_t numFrames, float gainDb) {
        GlobalGain gain;
        gain.snapTo(gainDb);
        gain.process(left, right, numFrames, gainDb);
    }

private:
    PluginChain& chain_;
    static constexpr size_t BUFFER_SIZE = 4096;
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_OFFLINE_PROCESSOR_H
