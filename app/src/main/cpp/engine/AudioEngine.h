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

#ifndef GUITARRACKCRAFT_AUDIO_ENGINE_H
#define GUITARRACKCRAFT_AUDIO_ENGINE_H

#include <oboe/Oboe.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include "plugin/PluginChain.h"
#include "AudioRecorder.h"
#include "LatencyMath.h"
#include "dsp/GlobalGain.h"

namespace guitarrackcraft {

/**
 * Audio engine using Oboe for low-latency audio I/O.
 * Processes audio through the plugin chain in real-time.
 */
class AudioEngine : public oboe::AudioStreamCallback {
public:
    AudioEngine();
    ~AudioEngine();

    /**
     * Start audio processing.
     * @param sampleRate Desired sample rate (will use device default if not supported)
     * @return true if started successfully
     */
    bool start(float sampleRate = 48000.0f, int32_t inputDeviceId = 0,
               int32_t outputDeviceId = 0, int32_t bufferFrames = 0);

    /**
     * Stop audio processing.
     */
    void stop();

    /**
     * Check if engine is running.
     */
    bool isRunning() const;

    /**
     * Get the plugin chain (for adding/removing plugins).
     */
    PluginChain& getChain() { return chain_; }
    const PluginChain& getChain() const { return chain_; }

    /**
     * Get current sample rate.
     */
    float getSampleRate() const { return sampleRate_; }

    /**
     * Get actual callback frame count (buffer size used by the audio callback).
     */
    uint32_t getCallbackFrameCount() const { return callbackFrameCount_; }

    struct StreamInfo {
        bool isAAudio = false;         // AAudio vs OpenSL ES
        bool inputExclusive = false;   // Exclusive sharing mode granted
        bool outputExclusive = false;
        bool inputLowLatency = false;  // LowLatency performance mode granted
        bool outputLowLatency = false;
        bool outputMMap = false;       // MMAP used (lowest path)
        bool outputCallback = true;    // Using data callback
        int32_t framesPerBurst = 0;    // Hardware burst size (output)
        bool inputMMap = false;        // MMAP used by the input stream
        int32_t inputChannels = 0;     // Granted input channel count (1 or 2)
        int32_t outputChannels = 0;
        int32_t inputFormat = 0;       // 0 = float, 1 = 16-bit PCM
        int32_t outputFormat = 0;
        int32_t inputPreset = 0;       // 0 = VoiceRecognition, 1 = VoicePerformance, 2 = other
        int32_t outputBufferSizeInFrames = 0;   // Real buffer the HAL is using now
        int32_t outputBufferCapacity = 0;       // Maximum the stream can use
        int32_t outputFramesPerCallback = 0;
        int32_t inputFramesPerBurst = 0;
        int32_t inputBufferSizeInFrames = 0;
        int32_t outputTimestampSupported = 0;   // 1 when getTimestamp() answered
    };

    /**
     * Get stream configuration info for the low-latency checklist.
     */
    StreamInfo getStreamInfo();

    /** Round-trip latency broken down by direction, all values in milliseconds. */
    struct LatencyDetails {
        double inputMs = 0.0;
        double outputMs = 0.0;
        double roundTripMs = 0.0;
    };

    /**
     * Latency measured from the stream timestamps (input + output), with the
     * queued-frames estimate as fallback when the device cannot report one.
     */
    LatencyDetails getLatencyDetails();

    /**
     * Get the current round-trip latency in milliseconds (input + output).
     */
    double getLatencyMs();

    /**
     * Get input peak level (0.0–1.0).
     */
    float getInputLevel() const;

    /**
     * Get output peak level (0.0–1.0).
     */
    float getOutputLevel() const;

    /**
     * Get CPU load (0.0–1.0) from processing time vs buffer duration.
     */
    float getCpuLoad() const;

    /**
     * Get cumulative audio xrun (underrun/overrun) count from the output stream.
     */
    int32_t getXRunCount() const;

    /**
     * True if input has clipped (peak >= 0.99).
     */
    bool isInputClipping() const;

    /**
     * True if output has clipped (peak >= 0.99).
     */
    bool isOutputClipping() const;

    /**
     * Clear clipping indicators (call when user taps to reset).
     */
    void resetClipping();

    /**
     * Bypass chain processing (audio passthrough). Use during preset loading
     * to prevent the audio thread from processing a partially-built chain.
     */
    void setChainBypass(bool bypass) { chainBypass_.store(bypass); }
    void setWavBypassChain(bool bypass) { wavBypassChain_.store(bypass); }

    /**
     * Global input gain in dB, applied to the mic/WAV input before any metering,
     * recording or plugin processing. Clamped to [-24, +24]; default 0 dB.
     * Takes effect on the next audio callback — no engine restart.
     */
    void setGlobalInputGainDb(float db) { globalInputGainDb_.store(GlobalGain::clampDb(db)); }
    float getGlobalInputGainDb() const { return globalInputGainDb_.load(); }

    /**
     * Global output gain in dB, applied to the processed (or bypassed) output
     * before output metering, recording and the final interleave.
     * Clamped to [-24, +24]; default 0 dB. Takes effect on the next callback.
     */
    void setGlobalOutputGainDb(float db) { globalOutputGainDb_.store(GlobalGain::clampDb(db)); }
    float getGlobalOutputGainDb() const { return globalOutputGainDb_.load(); }

    /**
     * Get the audio recorder for real-time recording of raw input and processed output.
     */
    AudioRecorder& getRecorder() { return recorder_; }

    // --- WAV real-time playback ---

    /**
     * Load a WAV file for playback. Engine must be running (sample rate known).
     * Converts to mono and resamples to engine rate.
     * @return true on success
     */
    bool loadWav(const std::string& path);

    /**
     * Unload the current WAV and stop playback.
     */
    void unloadWav();

    void wavPlay();
    void wavPause();
    void wavSeekToFrame(size_t frame);

    double getWavDurationSec() const;
    double getWavPositionSec() const;
    bool isWavPlaying() const;
    bool isWavLoaded() const;

    // Oboe AudioStreamCallback implementation
    oboe::DataCallbackResult onAudioReady(
        oboe::AudioStream* audioStream,
        void* audioData,
        int32_t numFrames) override;

    void onErrorBeforeClose(oboe::AudioStream* oboeStream, oboe::Result error) override;
    void onErrorAfterClose(oboe::AudioStream* oboeStream, oboe::Result error) override;

private:
    std::unique_ptr<oboe::AudioStream> inputStream_;
    std::unique_ptr<oboe::AudioStream> outputStream_;
    PluginChain chain_;
    float sampleRate_;
    int32_t inputDeviceId_ = 0;
    int32_t outputDeviceId_ = 0;
    int32_t requestedBufferFrames_ = 0;
    uint32_t callbackFrameCount_ = 0;  // Power-of-2 frames per audio callback
    std::atomic<bool> isRunning_;
    std::atomic<bool> chainBypass_{false};  // skip chain processing (passthrough)

    // Configuration actually granted by the HAL. The negotiation ladder asks for
    // mono float, but a device may only grant stereo or 16-bit, so the callback
    // converts from whatever was granted instead of assuming the request.
    int32_t inputChannels_ = 1;
    int32_t outputChannels_ = 2;
    bool inputFormatIsFloat_ = true;
    bool outputFormatIsFloat_ = true;
    /** Coarse classification of the granted input preset, for the diagnostics UI. */
    enum class InputPresetClass : int32_t {
        VoiceRecognition = 0,
        VoicePerformance = 1,
        Other = 2,
    };
    InputPresetClass inputPreset_ = InputPresetClass::VoiceRecognition;

    // Upper bound of numFrames per callback, so nothing in the RT path allocates.
    size_t bufferCapacityFrames_ = 0;

    // Latency tuner for the output stream: shrinks the HAL buffer to the minimum
    // and grows it back only when xruns appear. It references the stream, so
    // closeStreams() resets it right after outputStream_ (that is, after the
    // 500 ms wait that drains the callback thread): the member itself outlives
    // closeStreams(), the stream it points at does not.
    std::unique_ptr<oboe::LatencyTuner> latencyTuner_;

    // Last latency computed from a valid timestamp, per direction. Only touched
    // from the JNI/UI thread.
    latency::LatencyHold inputLatencyHold_;
    latency::LatencyHold outputLatencyHold_;

    // Global gain targets in dB (written from the UI thread, read per callback).
    // The ramp state that actually reaches the samples lives in the GlobalGain
    // instances below, which are audio-thread-only.
    std::atomic<float> globalInputGainDb_{GlobalGain::kUnityDb};
    std::atomic<float> globalOutputGainDb_{GlobalGain::kUnityDb};
    GlobalGain inputGain_;
    GlobalGain outputGain_;

    // Audio buffers for processing
    std::vector<float> inputBuffer_;
    std::vector<float> outputBufferLeft_;
    std::vector<float> outputBufferRight_;

    // Preallocated scratch for the granted-but-not-preferred stream layouts:
    // stereo float input before the downmix, and 16-bit input (mono or stereo)
    // before the conversion. Both are sized to bufferCapacityFrames_.
    std::vector<float> inputStereoScratch_;
    std::vector<int16_t> inputI16Scratch_;

    // Temporary buffers for plugin chain
    const float* inputPtrs_[2];
    float* outputPtrs_[2];

    // Level metering and CPU (written from audio thread, read from UI)
    std::atomic<float> inputPeakLevel_{0.0f};
    std::atomic<float> outputPeakLevel_{0.0f};
    std::atomic<float> cpuLoad_{0.0f};
    std::atomic<bool> inputClipping_{false};
    std::atomic<bool> outputClipping_{false};
    float inputPeakHold_{0.0f};
    float outputPeakHold_{0.0f};

    // Aggregated subprocess (wine VST) load + xruns. Sampled every ~500ms by the
    // dedicated monitor thread (see startVstMonitor) so the UI's getCpuLoad /
    // xrun metrics include VSTs that run out-of-process. Without this, a Helix
    // Native that's saturating the wine subprocess shows 1% CPU and 0
    // xruns because the audio thread itself is idle — the wine subprocess
    // is the bottleneck and it isn't measured here.
    std::atomic<float>   vstCpuLoad_{0.0f};
    std::atomic<int32_t> vstUnderruns_{0};
    /** Per-plugin last-sampled jiffies counter, keyed by subprocess pid.
     *  Owned exclusively by the monitor thread. */
    std::unordered_map<int, uint64_t> vstLastJiffies_;
    /** Wall-time when vstLastJiffies_ was last updated (clock_gettime ns).
     *  Owned exclusively by the monitor thread. */
    uint64_t vstLastSampleNs_{0};
    /** Reads /proc/<pid>/stat for the out-of-process plugins. Reading files
     *  cannot happen on the audio thread, so this lives on its own thread. */
    std::thread vstMonitorThread_;
    std::atomic<bool> vstMonitorRunning_{false};

    static constexpr float kClippingThreshold = 0.99f;
    static constexpr float kPeakDecay = 0.95f;
    /** Cap for the plugin chain's intermediate buffers (the engine's own buffers
     *  follow the real stream capacity so the callback never has to grow one). */
    static constexpr size_t kMaxBufferedFrames = 8192;

    // WAV playback state (read in callback; written from load/seek/play/pause)
    std::vector<float> wavBuffer_;
    std::atomic<size_t> wavPositionFrames_{0};
    std::atomic<bool> wavPlaying_{false};
    std::atomic<bool> wavBypassChain_{true};  // true = WAV plays raw (backing track), false = through effects
    size_t wavLengthFrames_{0};

    AudioRecorder recorder_;

    bool createAudioStreams(float sampleRate);
    void closeStreams();
    void resampleToEngineRate(const std::vector<float>& src, uint32_t srcRate,
                              std::vector<float>& dst);

    bool openInputStream();
    bool openOutputStream();
    bool startStreams();
    void prepareBuffers();
    void requestMinimumBufferSize(oboe::AudioStream* stream, const char* label);
    int32_t readInputIntoBuffer(int32_t numFrames);
    void writeOutputBuffer(void* audioData, int32_t numFrames, int32_t numChannels);
    oboe::DataCallbackResult finishOutputCallback();

    void startVstMonitor();
    void stopVstMonitor();
    void vstMonitorLoop();
    void sampleVstLoad();
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_AUDIO_ENGINE_H
