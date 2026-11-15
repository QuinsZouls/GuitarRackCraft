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

#include "AudioEngine.h"
#include "AudioFormatUtils.h"
#include "utils/WavIO.h"
#include "utils/ThreadUtils.h"
#include <oboe/OboeExtensions.h>
#include <android/api-level.h>
#include <android/log.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <algorithm>
#include <thread>

#define LOG_TAG "AudioEngine"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

namespace {

/** u/voice-performance only exists from Android 10 on. */
constexpr int32_t kVoicePerformanceMinApiLevel = 29;
/** How long the monitor thread of out-of-process plugins waits between samples. */
constexpr int kPluginMonitorIntervalMs = 500;
/** LatencyTuner wants a whole number of bursts; two is the documented minimum. */
constexpr int32_t kMinimumBursts = 2;

/**
 * One rung of the stream negotiation ladder. The engine asks for the cheapest
 * configuration first (mono float, the one that MMAPs on every device we have
 * seen) and degrades the request until the HAL grants Exclusive + MMAP.
 */
struct StreamAttempt {
    oboe::AudioFormat format;
    int32_t channels;
    oboe::InputPreset preset;  // input only
    const char* label;
};

bool isPowerOfTwo(int32_t value) {
    return value > 0 && (value & (value - 1)) == 0;
}

uint32_t nextPowerOfTwo(uint32_t value) {
    uint32_t power = 1;
    while (power < value) {
        power <<= 1;
    }
    return power;
}

const char* sharingModeText(oboe::SharingMode mode) {
    return mode == oboe::SharingMode::Exclusive ? "Exclusive" : "Shared";
}

/**
 * The whole point of the ladder: on a shared (legacy) stream the HAL adds one
 * extra buffer stage and the round trip roughly doubles, so a configuration is
 * only considered good when both Exclusive and MMAP were granted.
 */
bool grantsExclusiveMmap(oboe::AudioStream* stream) {
    return stream != nullptr &&
           stream->getSharingMode() == oboe::SharingMode::Exclusive &&
           oboe::OboeExtensions::isMMapUsed(stream);
}

oboe::AudioStreamBuilder makeInputBuilder(const StreamAttempt& attempt, oboe::AudioApi api,
                                          int32_t sampleRate, int32_t deviceId) {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Input)
           ->setAudioApi(api)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(attempt.format)
           ->setChannelCount(attempt.channels)
           ->setSampleRate(sampleRate)
           ->setInputPreset(attempt.preset);
    if (deviceId != 0) {
        builder.setDeviceId(deviceId);
    }
    return builder;
}

oboe::AudioStreamBuilder makeOutputBuilder(const StreamAttempt& attempt, oboe::AudioApi api,
                                           int32_t sampleRate, int32_t deviceId,
                                           int32_t framesPerDataCallback,
                                           oboe::AudioStreamCallback* callback) {
    oboe::AudioStreamBuilder builder;
    builder.setDirection(oboe::Direction::Output)
           ->setAudioApi(api)
           ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
           ->setSharingMode(oboe::SharingMode::Exclusive)
           ->setFormat(attempt.format)
           ->setChannelCount(attempt.channels)
           ->setSampleRate(sampleRate)
           ->setUsage(oboe::Usage::Game)
           ->setCallback(callback);
    if (deviceId != 0) {
        builder.setDeviceId(deviceId);
    }
    if (framesPerDataCallback > 0) {
        builder.setFramesPerDataCallback(framesPerDataCallback);
    }
    return builder;
}

int32_t classifyInputPreset(oboe::InputPreset preset) {
    switch (preset) {
        case oboe::InputPreset::VoiceRecognition:
            return 0;
        case oboe::InputPreset::VoicePerformance:
            return 1;
        default:
            return 2;
    }
}

int64_t monotonicNowNs() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1'000'000'000LL + static_cast<int64_t>(ts.tv_nsec);
}

/**
 * Walks the ladder and returns the first stream that grants Exclusive + MMAP.
 * When none does, the last configuration that opened at all is returned so the
 * engine still starts (legacy path) instead of leaving the user without audio.
 * `makeBuilder` builds one fully configured builder per rung.
 */
template <typename MakeBuilder>
oboe::AudioStream* negotiateStream(const std::vector<StreamAttempt>& ladder, const char* direction,
                                   const MakeBuilder& makeBuilder, StreamAttempt& grantedAttempt) {
    oboe::AudioStream* accepted = nullptr;
    oboe::AudioStream* openedFallback = nullptr;
    for (const auto& attempt : ladder) {
        oboe::AudioStreamBuilder builder = makeBuilder(attempt);
        oboe::AudioStream* stream = nullptr;
        const oboe::Result openResult = builder.openStream(&stream);
        if (openResult != oboe::Result::OK || stream == nullptr) {
            LOGI("%s [%s] open failed: %s", direction, attempt.label,
                 oboe::convertToText(openResult));
            continue;
        }
        const bool lowLatency = grantsExclusiveMmap(stream);
        LOGI("%s [%s] granted api=%d sharing=%s mmap=%d format=%d channels=%d preset=%d burst=%d rate=%d -> %s",
             direction, attempt.label, static_cast<int>(stream->getAudioApi()),
             sharingModeText(stream->getSharingMode()),
             oboe::OboeExtensions::isMMapUsed(stream) ? 1 : 0,
             static_cast<int>(stream->getFormat()), stream->getChannelCount(),
             static_cast<int>(stream->getInputPreset()), stream->getFramesPerBurst(),
             stream->getSampleRate(), lowLatency ? "ACCEPTED" : "kept as legacy fallback");
        if (lowLatency) {
            if (openedFallback != nullptr) {
                openedFallback->close();
                delete openedFallback;
                openedFallback = nullptr;
            }
            accepted = stream;
            grantedAttempt = attempt;
            break;
        }
        if (openedFallback != nullptr) {
            openedFallback->close();
            delete openedFallback;
        }
        openedFallback = stream;
        grantedAttempt = attempt;
    }
    return accepted != nullptr ? accepted : openedFallback;
}

/**
 * Reopens the output stream with an explicit frames-per-callback. Needed when
 * the hardware burst is not a power of two (convolvers partition on power of
 * two blocks). On failure the original configuration is retried so a quirk of
 * one device cannot leave the engine without output.
 */
bool reopenOutputStream(oboe::AudioStream** streamSlot, const StreamAttempt& attempt,
                        oboe::AudioApi api, int32_t sampleRate, int32_t deviceId,
                        int32_t framesPerDataCallback, oboe::AudioStreamCallback* callback,
                        uint32_t& callbackFrameCount) {
    LOGI("output: reopening [%s] with framesPerDataCallback=%d", attempt.label, framesPerDataCallback);
    if (*streamSlot != nullptr) {
        (*streamSlot)->close();
        delete *streamSlot;
        *streamSlot = nullptr;
    }
    for (int32_t requested : {framesPerDataCallback, 0}) {
        oboe::AudioStreamBuilder builder = makeOutputBuilder(attempt, api, sampleRate, deviceId,
                                                             requested, callback);
        oboe::AudioStream* stream = nullptr;
        const oboe::Result result = builder.openStream(&stream);
        if (result == oboe::Result::OK && stream != nullptr) {
            *streamSlot = stream;
            if (requested == 0) {
                // The HAL refused the pinned block size: follow the hardware burst
                // instead. Convolvers were activated for the old size, so this is
                // logged as a warning rather than silently accepted.
                callbackFrameCount = nextPowerOfTwo(
                    static_cast<uint32_t>(std::max(1, stream->getFramesPerBurst())));
                LOGE("output: framesPerDataCallback=%d rejected, following the burst (%u frames)",
                     framesPerDataCallback, callbackFrameCount);
            }
            return true;
        }
        LOGE("output: reopen with framesPerDataCallback=%d failed: %s", requested,
             oboe::convertToText(result));
    }
    return false;
}

} // namespace

AudioEngine::AudioEngine()
    : sampleRate_(48000.0f)
    , isRunning_(false)
{
    inputPtrs_[0] = nullptr;
    inputPtrs_[1] = nullptr;
    outputPtrs_[0] = nullptr;
    outputPtrs_[1] = nullptr;
}

AudioEngine::~AudioEngine() {
    stop();
}

bool AudioEngine::start(float sampleRate, int32_t inputDeviceId,
                        int32_t outputDeviceId, int32_t bufferFrames) {
    LOGI("start() ENTER tid=%ld sampleRate=%.0f inputDev=%d outputDev=%d bufFrames=%d isRunning_=%d",
         getTid(), sampleRate, inputDeviceId, outputDeviceId, bufferFrames, isRunning_ ? 1 : 0);
    if (isRunning_) {
        LOGI("start() early return (already running)");
        return true;
    }

    sampleRate_ = sampleRate;
    inputDeviceId_ = inputDeviceId;
    outputDeviceId_ = outputDeviceId;
    requestedBufferFrames_ = bufferFrames;

    if (!createAudioStreams(sampleRate)) {
        LOGE("Failed to create audio streams");
        return false;
    }
    startVstMonitor();

    // Activate plugin chain with the power-of-2 callback frame count
    // so convolver plugins configure their partition size correctly
    LOGI("Using callback frame count: %u (power-of-2)", callbackFrameCount_);
    chain_.setSampleRate(sampleRate_, callbackFrameCount_);
    chain_.activate();

    // Start the gain ramps at their current targets so the very first callback
    // plays at the requested gain instead of sliding in from the previous run.
    inputGain_.snapTo(globalInputGainDb_.load());
    outputGain_.snapTo(globalOutputGainDb_.load());

    isRunning_ = true;
    LOGI("start() EXIT tid=%ld Audio engine started at %.0f Hz", getTid(), sampleRate_);
    return true;
}

void AudioEngine::stop() {
    LOGI("stop() entered tid=%ld isRunning_=%d", getTid(), isRunning_ ? 1 : 0);
    if (!isRunning_) {
        // Stream may have been closed by onErrorAfterClose (e.g. system closed stream when opening X11 UI).
        // We must still call closeStreams() so streams are torn down with the 250ms wait before the
        // destructor runs. Otherwise ~AudioEngine destroys outputStream_/inputStream_ while the
        // AudioTrack callback thread is still in getStream() -> pthread_mutex_lock on destroyed mutex (SIGABRT).
        LOGI("stop() isRunning_=0; calling closeStreams() anyway so streams tear down safely");
        closeStreams();
        return;
    }
    // Stop the /proc monitor first: it walks the plugin chain and must not
    // outlive the streams or the chain it reads.
    stopVstMonitor();

    // Stop recording before tearing down the audio path
    if (recorder_.isRecording()) {
        recorder_.stopRecording();
    }

    // Signal callback to exit immediately so it does not touch chain_ or stream
    // during teardown (avoids use-after-free / destroyed mutex in plugin chain or Oboe).
    isRunning_ = false;
    LOGI("stop() isRunning_=false set, calling chain_.deactivate()");

    chain_.deactivate();
    LOGI("stop() chain_.deactivate() done, calling closeStreams()");

    closeStreams();
    LOGI("stop() done");
}

bool AudioEngine::isRunning() const {
    return isRunning_;
}

AudioEngine::StreamInfo AudioEngine::getStreamInfo() {
    StreamInfo info;
    if (outputStream_) {
        info.isAAudio = outputStream_->getAudioApi() == oboe::AudioApi::AAudio;
        info.outputExclusive = outputStream_->getSharingMode() == oboe::SharingMode::Exclusive;
        info.outputLowLatency = outputStream_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;
        info.outputMMap = oboe::OboeExtensions::isMMapUsed(outputStream_.get());
        info.outputCallback = true; // always using callback
        info.framesPerBurst = outputStream_->getFramesPerBurst();
        info.outputChannels = outputStream_->getChannelCount();
        info.outputFormat = outputFormatIsFloat_ ? 0 : 1;
        info.outputBufferSizeInFrames = outputStream_->getBufferSizeInFrames();
        info.outputBufferCapacity = outputStream_->getBufferCapacityInFrames();
        // The value the stream itself reports, which is 0 when the HAL burst
        // drives the callback size. callbackFrameCount_ is only the request the
        // chain was activated with, so it must not be reported as granted.
        info.outputFramesPerCallback = outputStream_->getFramesPerCallback();
        info.outputTimestampSupported =
            outputStream_->getTimestamp(CLOCK_MONOTONIC) == oboe::Result::OK ? 1 : 0;
    }
    if (inputStream_) {
        info.inputExclusive = inputStream_->getSharingMode() == oboe::SharingMode::Exclusive;
        info.inputLowLatency = inputStream_->getPerformanceMode() == oboe::PerformanceMode::LowLatency;
        info.inputMMap = oboe::OboeExtensions::isMMapUsed(inputStream_.get());
        info.inputChannels = inputStream_->getChannelCount();
        info.inputFormat = inputFormatIsFloat_ ? 0 : 1;
        info.inputPreset = static_cast<int32_t>(inputPreset_);
        info.inputFramesPerBurst = inputStream_->getFramesPerBurst();
        info.inputBufferSizeInFrames = inputStream_->getBufferSizeInFrames();
    }
    return info;
}

double AudioEngine::getLatencyMs() {
    return getLatencyDetails().roundTripMs;
}

AudioEngine::LatencyDetails AudioEngine::getLatencyDetails() {
    LatencyDetails details;
    const double rate = sampleRate_ > 0.0f ? static_cast<double>(sampleRate_) : 48000.0;
    const int64_t nowNs = monotonicNowNs();

    if (inputStream_) {
        const int64_t framesRead = inputStream_->getFramesRead();
        int64_t latencyFrames = 0;
        auto timestamp = inputStream_->getTimestamp(CLOCK_MONOTONIC);
        if (timestamp == oboe::Result::OK) {
            latencyFrames = latency::inputLatencyFrames(timestamp.value().position,
                                                       timestamp.value().timestamp, nowNs, rate,
                                                       framesRead);
            inputLatencyHold_.store(latencyFrames, nowNs);
        } else if (!inputLatencyHold_.get(nowNs, latencyFrames)) {
            latencyFrames = latency::fillLatencyFrames(inputStream_->getFramesWritten(), framesRead);
        }
        details.inputMs = latency::framesToMs(latencyFrames, rate);
    }

    if (outputStream_) {
        const int64_t framesWritten = outputStream_->getFramesWritten();
        int64_t latencyFrames = 0;
        auto timestamp = outputStream_->getTimestamp(CLOCK_MONOTONIC);
        if (timestamp == oboe::Result::OK) {
            latencyFrames = latency::outputLatencyFrames(timestamp.value().position,
                                                        timestamp.value().timestamp, nowNs, rate,
                                                        framesWritten);
            outputLatencyHold_.store(latencyFrames, nowNs);
        } else if (!outputLatencyHold_.get(nowNs, latencyFrames)) {
            latencyFrames = latency::fillLatencyFrames(framesWritten, outputStream_->getFramesRead());
        }
        details.outputMs = latency::framesToMs(latencyFrames, rate);
    }

    details.roundTripMs = details.inputMs + details.outputMs;
    return details;
}

float AudioEngine::getInputLevel() const {
    return inputPeakLevel_.load();
}

float AudioEngine::getOutputLevel() const {
    return outputPeakLevel_.load();
}

float AudioEngine::getCpuLoad() const {
    /* Audio-thread load (cpuLoad_) measures chain.process() wall time vs
     * buffer duration. For LV2 plugins (in-process), that's the real
     * processing cost. For VST plugins (out-of-process via wine), it
     * measures only the shm push/pull — the wine subprocess does the
     * actual work on different threads in another address space. Reporting
     * the max of the two captures the bottleneck either way. */
    return std::max(cpuLoad_.load(), vstCpuLoad_.load());
}

int32_t AudioEngine::getXRunCount() const {
    int32_t oboeXruns = 0;
    if (outputStream_) {
        auto result = outputStream_->getXRunCount();
        oboeXruns = result ? result.value() : 0;
    }
    /* Oboe-side xruns only happen when the audio thread itself misses a
     * deadline. VST plugins under wine zero-fill on their own (the audio
     * thread sees full buffers, just silent) — so Oboe never knows. Add
     * the per-plugin underrun counter so the UI's "xruns" reflects both. */
    return oboeXruns + vstUnderruns_.load();
}

bool AudioEngine::isInputClipping() const {
    return inputClipping_.load();
}

bool AudioEngine::isOutputClipping() const {
    return outputClipping_.load();
}

void AudioEngine::resetClipping() {
    inputClipping_.store(false);
    outputClipping_.store(false);
}

// --- WAV playback ---

bool AudioEngine::loadWav(const std::string& path) {
    if (!isRunning_) {
        LOGE("loadWav: engine not running");
        return false;
    }
    wavPause();
    std::vector<float> samples;
    uint32_t fileRate = 44100;
    uint32_t numChannels = 1;
    if (!guitarrackcraft::readWavFile(path, samples, fileRate, numChannels)) {
        return false;
    }
    // Convert to mono if stereo (samples are interleaved L,R,L,R,...)
    std::vector<float> mono;
    if (numChannels == 2) {
        size_t numFrames = samples.size() / 2;
        mono.resize(numFrames);
        for (size_t i = 0; i < numFrames; ++i) {
            mono[i] = (samples[i * 2] + samples[i * 2 + 1]) * 0.5f;
        }
    } else {
        mono = std::move(samples);
    }
    wavBuffer_.clear();
    resampleToEngineRate(mono, fileRate, wavBuffer_);
    wavLengthFrames_ = wavBuffer_.size();
    wavPositionFrames_.store(0);
    wavPlaying_.store(false);
    LOGI("WAV loaded: %zu frames at %.0f Hz (from %u Hz)", wavLengthFrames_, sampleRate_, fileRate);
    return true;
}

void AudioEngine::unloadWav() {
    wavPlaying_.store(false);
    wavBuffer_.clear();
    wavLengthFrames_ = 0;
    wavPositionFrames_.store(0);
}

void AudioEngine::wavPlay() {
    if (!wavBuffer_.empty()) {
        wavPlaying_.store(true);
    }
}

void AudioEngine::wavPause() {
    wavPlaying_.store(false);
}

void AudioEngine::wavSeekToFrame(size_t frame) {
    size_t end = wavLengthFrames_;
    if (end > 0 && frame > end) {
        frame = end;
    }
    wavPositionFrames_.store(frame);
}

double AudioEngine::getWavDurationSec() const {
    if (sampleRate_ <= 0.0f || wavLengthFrames_ == 0) return 0.0;
    return static_cast<double>(wavLengthFrames_) / sampleRate_;
}

double AudioEngine::getWavPositionSec() const {
    if (sampleRate_ <= 0.0f) return 0.0;
    return static_cast<double>(wavPositionFrames_.load()) / sampleRate_;
}

bool AudioEngine::isWavPlaying() const {
    return wavPlaying_.load();
}

bool AudioEngine::isWavLoaded() const {
    return !wavBuffer_.empty();
}


void AudioEngine::resampleToEngineRate(const std::vector<float>& src,
                                       uint32_t srcRate,
                                       std::vector<float>& dst) {
    if (src.empty() || sampleRate_ <= 0.0f) {
        dst.clear();
        return;
    }
    if (static_cast<float>(srcRate) == sampleRate_) {
        dst = src;
        return;
    }
    double ratio = sampleRate_ / static_cast<double>(srcRate);
    size_t outFrames = static_cast<size_t>(std::round(src.size() * ratio));
    if (outFrames == 0) {
        dst.clear();
        return;
    }
    dst.resize(outFrames);
    for (size_t i = 0; i < outFrames; ++i) {
        double srcIdx = i / ratio;
        size_t j = static_cast<size_t>(srcIdx);
        float frac = static_cast<float>(srcIdx - j);
        if (j + 1 >= src.size()) {
            dst[i] = src[src.size() - 1];
        } else {
            dst[i] = src[j] * (1.0f - frac) + src[j + 1] * frac;
        }
    }
}

oboe::DataCallbackResult AudioEngine::onAudioReady(
    oboe::AudioStream* audioStream,
    void* audioData,
    int32_t numFrames) {
    // Debug: log callback thread still active (rate-limited) to correlate with closeStreams() tid
    {
        static std::atomic<int> enterCount{0};
        static auto lastEnterLog = std::chrono::steady_clock::now();
        int c = enterCount++;
        auto now = std::chrono::steady_clock::now();
        if (c < 3 || std::chrono::duration<double>(now - lastEnterLog).count() >= 5.0) {
            if (c >= 3) lastEnterLog = now;
            LOGI("onAudioReady ENTER tid=%ld (callback thread, count=%d)", getTid(), c);
        }
    }
    // Debug: log when callback bails due to shutdown (rate-limited).
    // Deliberate exception to "every exit path ends in finishOutputCallback()":
    // tune() may call setBufferSizeInFrames(), and during teardown the stream is
    // being stopped/closed underneath us, so an extra buffer resize would either
    // fail loudly or touch a stream that is already gone.
    if (!isRunning_ || numFrames <= 0) {
        static std::atomic<int> bailCount{0};
        int c = bailCount++;
        if (c < 5 || (c % 50 == 0)) {
            LOGI("onAudioReady: bail tid=%ld !isRunning_=%d numFrames=%d (bail #%d)",
                 getTid(), isRunning_ ? 0 : 1, numFrames, c);
        }
        return oboe::DataCallbackResult::Continue;
    }
    // Only process when the output stream needs data (we do not set callback on
    // input). Like the bail above, this return skips tune() on purpose: it is a
    // callback for a different stream (the input one), and tuning the output
    // buffer from an input callback would size it against the wrong deadline.
    if (audioStream != outputStream_.get()) {
        return oboe::DataCallbackResult::Continue;
    }

    // A callback larger than the preallocated buffers would need a resize, which
    // is forbidden on the audio thread. It cannot happen for a sane configuration
    // (frames per callback <= stream capacity), so play silence and keep running.
    if (static_cast<size_t>(numFrames) > bufferCapacityFrames_) {
        static auto lastCapacityLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastCapacityLog).count() >= 1.0) {
            lastCapacityLog = now;
            LOGE("onAudioReady: numFrames=%d exceeds the preallocated %zu frames, muting",
                 numFrames, bufferCapacityFrames_);
        }
        const size_t sampleBytes = outputFormatIsFloat_ ? sizeof(float) : sizeof(int16_t);
        const int32_t channels = std::max(1, audioStream->getChannelCount());
        std::memset(audioData, 0, static_cast<size_t>(numFrames) * channels * sampleBytes);
        return finishOutputCallback();
    }

    // Input source: WAV playback or microphone
    const bool useWav = wavPlaying_.load() && !wavBuffer_.empty();
    if (useWav) {
        size_t pos = wavPositionFrames_.load();
        size_t len = wavLengthFrames_;
        size_t toCopy = std::min(static_cast<size_t>(numFrames), len > pos ? len - pos : 0);
        if (toCopy > 0) {
            std::memcpy(inputBuffer_.data(), wavBuffer_.data() + pos, toCopy * sizeof(float));
            wavPositionFrames_.store(pos + toCopy);
        }
        if (toCopy < static_cast<size_t>(numFrames)) {
            std::memset(inputBuffer_.data() + toCopy, 0, (numFrames - toCopy) * sizeof(float));
            wavPlaying_.store(false);
        }
    } else {
        readInputIntoBuffer(numFrames);
    }

    // Global input gain (post-gain gain staging: meters, recorder raw tap and
    // the chain all see the gained signal). Applies to both mic and WAV input.
    inputGain_.process(inputBuffer_.data(), numFrames, globalInputGainDb_.load());

    // Input peak metering and clipping
    float inputPeak = 0.0f;
    bool inputClip = false;
    for (int32_t i = 0; i < numFrames; ++i) {
        float s = std::fabs(inputBuffer_[i]);
        if (s > inputPeak) inputPeak = s;
        if (s >= kClippingThreshold) inputClip = true;
    }
    inputPeakHold_ = std::max(inputPeak, inputPeakHold_ * kPeakDecay);
    inputPeakLevel_.store(inputPeakHold_);
    if (inputClip) inputClipping_.store(true);

    // Rate-limited debug: input source and level (once per second)
    {
        static auto lastLog = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLog).count() >= 1.0) {
            lastLog = now;
            LOGI("onAudioReady: source=%s numFrames=%d inputPeak=%.4f",
                 useWav ? "WAV" : "mic", numFrames, inputPeak);
        }
    }

    // Set up input pointers (mono guitar input -> stereo)
    inputPtrs_[0] = inputBuffer_.data();
    inputPtrs_[1] = inputBuffer_.data();  // Duplicate mono to stereo

    // Set up output pointers (always process into our buffers for metering)
    const int32_t numChannels = audioStream->getChannelCount();
    outputPtrs_[0] = outputBufferLeft_.data();
    outputPtrs_[1] = outputBufferRight_.data();

    // Skip chain processing when bypassed (during preset load) or WAV bypass active
    if (chainBypass_.load() || (useWav && wavBypassChain_.load())) {
        for (int32_t ch = 0; ch < 2; ++ch) {
            std::memcpy(outputPtrs_[ch], inputPtrs_[ch],
                        numFrames * sizeof(float));
        }
    } else {
        // Process through plugin chain (measure CPU time)
        double bufferDurationMs = (numFrames / static_cast<double>(sampleRate_)) * 1000.0;
        auto t0 = std::chrono::high_resolution_clock::now();
        chain_.process(inputPtrs_, outputPtrs_, numFrames);
        auto t1 = std::chrono::high_resolution_clock::now();
        double processMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
        float load = static_cast<float>(processMs / bufferDurationMs);
        if (load > 1.0f) load = 1.0f;

        /* The out-of-process (wine VST) load is sampled by vstMonitorLoop();
         * reading /proc from this thread would mean fopen/fread on the audio
         * thread. Here only the in-process measurement is published. */
        cpuLoad_.store(load);
    }

    // Global output gain, after the chain/bypass branch and before output
    // metering: bypassed (chainBypass_ / wavBypassChain_) audio is gained too.
    outputGain_.process(outputBufferLeft_.data(), outputBufferRight_.data(), numFrames,
                        globalOutputGainDb_.load());

    // Output peak metering (from buffers we wrote to)
    float outputPeak = 0.0f;
    bool outputClip = false;
    for (int32_t i = 0; i < numFrames; ++i) {
        float s = std::max(std::fabs(outputBufferLeft_[i]), std::fabs(outputBufferRight_[i]));
        if (s > outputPeak) outputPeak = s;
        if (s >= kClippingThreshold) outputClip = true;
    }
    outputPeakHold_ = std::max(outputPeak, outputPeakHold_ * kPeakDecay);
    outputPeakLevel_.store(outputPeakHold_);
    if (outputClip) outputClipping_.store(true);

    // Feed recorder (lock-free ring buffer write)
    if (recorder_.isRecording()) {
        recorder_.feedAudio(inputBuffer_.data(),
                            outputBufferLeft_.data(),
                            outputBufferRight_.data(),
                            numFrames);
    }

    // Rate-limited debug: output level (once per second)
    {
        static auto lastLogOut = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration<double>(now - lastLogOut).count() >= 1.0) {
            lastLogOut = now;
            LOGI("onAudioReady: outputPeak=%.4f", outputPeak);
        }
    }

    writeOutputBuffer(audioData, numFrames, numChannels);
    return finishOutputCallback();
}

void AudioEngine::onErrorBeforeClose(oboe::AudioStream* oboeStream, oboe::Result error) {
    LOGE("onErrorBeforeClose tid=%ld stream=%p error=%s (set isRunning_=false so callback bails)",
         getTid(), static_cast<void*>(oboeStream), oboe::convertToText(error));
    // Signal callback to exit immediately; Oboe will close the stream after we return.
    isRunning_ = false;
}

void AudioEngine::onErrorAfterClose(oboe::AudioStream* oboeStream, oboe::Result error) {
    LOGE("onErrorAfterClose tid=%ld stream=%p error=%s", getTid(), static_cast<void*>(oboeStream), oboe::convertToText(error));
    isRunning_ = false;
    // Do NOT reset() the stream here. The underlying AAudio stream is already closed by
    // Oboe/the system (e.g. AudioBoost cancelling boost can trigger this). If we run
    // outputStream_.reset() on this thread, the destructor (~AAudioLoader) runs while
    // the audio callback thread may still be inside Oboe -> pthread_mutex_lock on
    // destroyed mutex (SIGABRT). Leave the stream object alive; stop() -> closeStreams()
    // will run later (from lifecycle or user) and destroy it on the main thread after
    // the 250ms sleep, when the callback thread is guaranteed idle.
}

bool AudioEngine::createAudioStreams(float sampleRate) {
    // Enable MMAP data path for lowest latency (must be set before opening streams).
    // Without this, AAudio uses the legacy non-MMAP path and cannot grant exclusive mode.
    oboe::OboeExtensions::setMMapEnabled(true);
    LOGI("MMAP supported=%d enabled=%d", oboe::OboeExtensions::isMMapSupported(),
         oboe::OboeExtensions::isMMapEnabled());

    sampleRate_ = sampleRate;

    if (!openInputStream()) {
        LOGE("Failed to open the input stream");
        return false;
    }
    if (!openOutputStream()) {
        LOGE("Failed to open the output stream");
        closeStreams();
        return false;
    }
    if (!startStreams()) {
        closeStreams();
        return false;
    }

    prepareBuffers();

    LOGI("Audio streams created: %d Hz, request callback %u frames, output buffer %d/%d frames",
         static_cast<int>(sampleRate_), callbackFrameCount_,
         outputStream_->getBufferSizeInFrames(), outputStream_->getBufferCapacityInFrames());
    return true;
}

bool AudioEngine::openInputStream() {
    const int32_t apiLevel = android_get_device_api_level();
    std::vector<StreamAttempt> ladder = {
        {oboe::AudioFormat::Float, 1, oboe::InputPreset::VoiceRecognition,
         "mono/float/voice-recognition"},
        {oboe::AudioFormat::Float, 2, oboe::InputPreset::VoiceRecognition,
         "stereo/float/voice-recognition"},
        {oboe::AudioFormat::I16, 1, oboe::InputPreset::VoiceRecognition,
         "mono/i16/voice-recognition"},
        {oboe::AudioFormat::I16, 2, oboe::InputPreset::VoiceRecognition,
         "stereo/i16/voice-recognition"},
    };
    if (apiLevel >= kVoicePerformanceMinApiLevel) {
        ladder.insert(ladder.begin() + 2,
                      {oboe::AudioFormat::Float, 2, oboe::InputPreset::VoicePerformance,
                       "stereo/float/voice-performance"});
    } else {
        LOGI("input: skipping the voice-performance rung (API level %d < %d)", apiLevel,
             kVoicePerformanceMinApiLevel);
    }
    LOGI("input: negotiating over %zu configurations (AAudio first)", ladder.size());

    for (oboe::AudioApi api : {oboe::AudioApi::AAudio, oboe::AudioApi::Unspecified}) {
        StreamAttempt granted = ladder.front();
        const int32_t rate = static_cast<int32_t>(sampleRate_);
        oboe::AudioStream* stream = negotiateStream(
            ladder, "input",
            [&](const StreamAttempt& attempt) {
                return makeInputBuilder(attempt, api, rate, inputDeviceId_);
            },
            granted);
        if (stream == nullptr) {
            // AAudio cannot open anything at all: fall back to whatever Oboe picks.
            LOGI("input: no configuration could be opened with audioApi=%d", static_cast<int>(api));
            continue;
        }

        inputStream_.reset(stream);
        sampleRate_ = static_cast<float>(inputStream_->getSampleRate());
        inputChannels_ = std::max(1, inputStream_->getChannelCount());
        inputFormatIsFloat_ = inputStream_->getFormat() == oboe::AudioFormat::Float;
        if (inputStream_->getFormat() != oboe::AudioFormat::Float &&
            inputStream_->getFormat() != oboe::AudioFormat::I16) {
            LOGE("input: unexpected granted format %d, treating it as 16-bit PCM",
                 static_cast<int>(inputStream_->getFormat()));
        }
        inputPreset_ = static_cast<InputPresetClass>(classifyInputPreset(inputStream_->getInputPreset()));

        LOGI("Input stream opened: label=[%s] api=%d sharing=%d perf=%d mmap=%d format=%d channels=%d preset=%d rate=%d burst=%d exclusive+mmap=%d",
             granted.label, static_cast<int>(inputStream_->getAudioApi()),
             static_cast<int>(inputStream_->getSharingMode()),
             static_cast<int>(inputStream_->getPerformanceMode()),
             oboe::OboeExtensions::isMMapUsed(inputStream_.get()) ? 1 : 0,
             static_cast<int>(inputStream_->getFormat()), inputChannels_,
             static_cast<int>(inputStream_->getInputPreset()), inputStream_->getSampleRate(),
             inputStream_->getFramesPerBurst(),
             grantsExclusiveMmap(inputStream_.get()) ? 1 : 0);
        if (!grantsExclusiveMmap(inputStream_.get())) {
            LOGI("input: no configuration was granted Exclusive+MMAP, running on the legacy "
                 "shared path (higher round-trip latency)");
        }
        return true;
    }
    return false;
}

bool AudioEngine::openOutputStream() {
    std::vector<StreamAttempt> ladder = {
        {oboe::AudioFormat::Float, 2, oboe::InputPreset::Unprocessed, "stereo/float"},
        {oboe::AudioFormat::I16, 2, oboe::InputPreset::Unprocessed, "stereo/i16"},
    };

    // When the user pinned a buffer size we know the callback block up front, so
    // every rung can carry it and no second open is needed.
    const int32_t wantedFramesPerCallback = requestedBufferFrames_ > 0 ? requestedBufferFrames_ : 0;
    LOGI("output: negotiating over %zu configurations, framesPerDataCallback=%d", ladder.size(),
         wantedFramesPerCallback);

    for (oboe::AudioApi api : {oboe::AudioApi::AAudio, oboe::AudioApi::Unspecified}) {
        StreamAttempt granted = ladder.front();
        const int32_t rate = static_cast<int32_t>(sampleRate_);
        oboe::AudioStream* stream = negotiateStream(
            ladder, "output",
            [&](const StreamAttempt& attempt) {
                return makeOutputBuilder(attempt, api, rate, outputDeviceId_,
                                         wantedFramesPerCallback, this);
            },
            granted);
        if (stream == nullptr) {
            LOGI("output: no configuration could be opened with audioApi=%d", static_cast<int>(api));
            continue;
        }
        // The callback block size is only pinned on the builder here: the stream
        // is not owned yet so the power-of-two reopen can swap it safely.
        if (requestedBufferFrames_ > 0) {
            callbackFrameCount_ = static_cast<uint32_t>(requestedBufferFrames_);
            LOGI("output: using user-requested callback frames: %u", callbackFrameCount_);
        } else {
            const int32_t framesPerBurst = stream->getFramesPerBurst();
            callbackFrameCount_ = nextPowerOfTwo(static_cast<uint32_t>(std::max(1, framesPerBurst)));
            // Convolvers partition on power-of-two blocks, so a burst like 240 has
            // to be rounded up even though AAudio would happily use it as is.
            if (!isPowerOfTwo(framesPerBurst)) {
                if (!reopenOutputStream(&stream, granted, api, rate, outputDeviceId_,
                                        static_cast<int32_t>(callbackFrameCount_), this,
                                        callbackFrameCount_)) {
                    // reopenOutputStream already released the stream; the caller
                    // tears down the input stream via closeStreams().
                    LOGE("output: could not reopen with a power-of-two callback size");
                    return false;
                }
            } else {
                LOGI("output: framesPerBurst=%d is already a power of two", framesPerBurst);
            }
        }

        outputStream_.reset(stream);
        outputChannels_ = std::max(1, outputStream_->getChannelCount());
        outputFormatIsFloat_ = outputStream_->getFormat() == oboe::AudioFormat::Float;
        if (outputStream_->getFormat() != oboe::AudioFormat::Float &&
            outputStream_->getFormat() != oboe::AudioFormat::I16) {
            LOGE("output: unexpected granted format %d, treating it as 16-bit PCM",
                 static_cast<int>(outputStream_->getFormat()));
        }
        const int32_t grantedRate = outputStream_->getSampleRate();
        if (grantedRate != static_cast<int32_t>(sampleRate_)) {
            // Diagnostics only: the chain keeps running at sampleRate_ (the rate
            // the input stream settled on), so a HAL that resamples or refuses
            // the rate shows up here instead of as unexplained pitch drift.
            LOGE("output: stream runs at %d Hz but the chain is at %d Hz (no resampling)",
                 grantedRate, static_cast<int32_t>(sampleRate_));
        }
        if (outputChannels_ > 2) {
            LOGE("output: %d channels granted, only the first two carry audio "
                 "(the rest are muted)", outputChannels_);
        }

        LOGI("Output stream opened: label=[%s] api=%d sharing=%d perf=%d mmap=%d format=%d channels=%d rate=%d burst=%d exclusive+mmap=%d",
             granted.label, static_cast<int>(outputStream_->getAudioApi()),
             static_cast<int>(outputStream_->getSharingMode()),
             static_cast<int>(outputStream_->getPerformanceMode()),
             oboe::OboeExtensions::isMMapUsed(outputStream_.get()) ? 1 : 0,
             static_cast<int>(outputStream_->getFormat()), outputChannels_,
             outputStream_->getSampleRate(), outputStream_->getFramesPerBurst(),
             grantsExclusiveMmap(outputStream_.get()) ? 1 : 0);
        if (!grantsExclusiveMmap(outputStream_.get())) {
            LOGI("output: no configuration was granted Exclusive+MMAP, running on the legacy "
                 "shared path (higher round-trip latency)");
        }
        return true;
    }
    return false;
}

bool AudioEngine::startStreams() {
    oboe::Result result = inputStream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start input stream: %s", oboe::convertToText(result));
        return false;
    }

    result = outputStream_->requestStart();
    if (result != oboe::Result::OK) {
        LOGE("Failed to start output stream: %s", oboe::convertToText(result));
        return false;
    }

    // Ask for the smallest buffer the HAL will honour before the tuner takes
    // over: two bursts is the AAudio minimum and what LatencyTuner resets to.
    requestMinimumBufferSize(outputStream_.get(), "output");
    requestMinimumBufferSize(inputStream_.get(), "input");

    // The tuner only grows the output buffer back when xruns appear, so a clean
    // session keeps the minimum. It is created after requestStart() so the
    // buffer size it sets is actually applied by AAudio.
    latencyTuner_ = std::make_unique<oboe::LatencyTuner>(*outputStream_);
    LOGI("output: latency tuner created, minimum=%d frames (%d bursts), capacity=%d frames",
         latencyTuner_->getMinimumBufferSize(), kMinimumBursts,
         outputStream_->getBufferCapacityInFrames());
    return true;
}

void AudioEngine::requestMinimumBufferSize(oboe::AudioStream* stream, const char* label) {
    if (stream == nullptr) {
        return;
    }
    const int32_t burst = stream->getFramesPerBurst();
    const int32_t capacity = stream->getBufferCapacityInFrames();
    if (burst <= 0 || capacity <= 0) {
        LOGI("%s: cannot size the buffer (burst=%d capacity=%d)", label, burst, capacity);
        return;
    }
    int32_t requested = kMinimumBursts * burst;
    requested = std::max(burst, std::min(requested, capacity));

    const int32_t previous = stream->getBufferSizeInFrames();
    auto setResult = stream->setBufferSizeInFrames(requested);
    if (!setResult) {
        LOGI("%s: setBufferSizeInFrames(%d) not supported (%s), keeping %d frames", label, requested,
             oboe::convertToText(setResult.error()), previous);
        return;
    }
    LOGI("%s: buffer requested=%d granted=%d (was %d) burst=%d capacity=%d", label, requested,
         setResult.value(), previous, burst, capacity);
}

void AudioEngine::prepareBuffers() {
    size_t capacityFrames = 0;
    if (outputStream_) {
        const int32_t streamCapacity = outputStream_->getBufferCapacityInFrames();
        if (streamCapacity > 0) {
            capacityFrames = static_cast<size_t>(streamCapacity);
        }
    }
    // The callback block size always has to fit as well: on the shared (legacy)
    // path the HAL may hand out whole buffers larger than the burst.
    capacityFrames = std::max(capacityFrames, static_cast<size_t>(callbackFrameCount_));
    capacityFrames = std::max<size_t>(capacityFrames, 1);
    bufferCapacityFrames_ = capacityFrames;

    inputBuffer_.assign(capacityFrames, 0.0f);
    outputBufferLeft_.assign(capacityFrames, 0.0f);
    outputBufferRight_.assign(capacityFrames, 0.0f);

    // The input scratch has to hold every granted channel: read() fills
    // numFrames * channelCount samples.
    const size_t scratchSamples = capacityFrames * static_cast<size_t>(std::max(2, inputChannels_));
    inputStereoScratch_.assign(scratchSamples, 0.0f);
    inputI16Scratch_.assign(scratchSamples, 0);

    // The chain only ever sees one block per callback, so the capacity cap keeps
    // its intermediate buffers bounded whatever the HAL reports.
    chain_.prepareBuffers(
        static_cast<uint32_t>(std::min(capacityFrames, kMaxBufferedFrames)));

    LOGI("prepareBuffers: %zu frames preallocated (callback %u, input %d ch, output %d ch, "
         "input format %s, output format %s)",
         capacityFrames, callbackFrameCount_, inputChannels_, outputChannels_,
         inputFormatIsFloat_ ? "float" : "i16", outputFormatIsFloat_ ? "float" : "i16");
}

int32_t AudioEngine::readInputIntoBuffer(int32_t numFrames) {
    if (inputStream_ == nullptr) {
        std::memset(inputBuffer_.data(), 0, static_cast<size_t>(numFrames) * sizeof(float));
        return 0;
    }

    // Only the mono-float layout lands straight in inputBuffer_. The other
    // granted layouts are read into preallocated scratch first, because read()
    // fills numFrames * channelCount samples in the stream's own format.
    const bool readsDirectlyToMono = inputFormatIsFloat_ && inputChannels_ == 1;
    void* destination = inputBuffer_.data();
    if (!readsDirectlyToMono) {
        destination = inputFormatIsFloat_ ? static_cast<void*>(inputStereoScratch_.data())
                                          : static_cast<void*>(inputI16Scratch_.data());
    }

    int32_t framesRead = 0;
    auto result = inputStream_->read(destination, numFrames, 0);
    if (result == oboe::Result::OK) {
        framesRead = result.value();
    }
    framesRead = std::max(0, std::min(framesRead, numFrames));

    if (readsDirectlyToMono) {
        if (framesRead < numFrames) {
            std::memset(inputBuffer_.data() + framesRead, 0,
                        static_cast<size_t>(numFrames - framesRead) * sizeof(float));
        }
        return framesRead;
    }

    if (inputFormatIsFloat_) {
        audioformat::interleavedToMono(inputStereoScratch_.data(), inputBuffer_.data(),
                                       static_cast<size_t>(framesRead), inputChannels_);
    } else if (inputChannels_ == 1) {
        audioformat::i16ToFloatBuffer(inputI16Scratch_.data(), inputBuffer_.data(),
                                      static_cast<size_t>(framesRead));
    } else {
        audioformat::interleavedI16ToMonoFloat(inputI16Scratch_.data(), inputBuffer_.data(),
                                               static_cast<size_t>(framesRead), inputChannels_);
    }
    if (framesRead < numFrames) {
        std::memset(inputBuffer_.data() + framesRead, 0,
                    static_cast<size_t>(numFrames - framesRead) * sizeof(float));
    }
    return framesRead;
}

void AudioEngine::writeOutputBuffer(void* audioData, int32_t numFrames, int32_t numChannels) {
    if (audioData == nullptr || numChannels <= 0) {
        return;
    }
    // The HAL decides the stride: it may grant more channels than the two we
    // render, so the frame walk uses the granted count instead of a hard-coded
    // two. Left/right land in the first two lanes and any extra lane is muted
    // (openOutputStream logs the layout once, off the audio thread).
    const size_t stride = static_cast<size_t>(numChannels);
    const bool mono = stride == 1;

    if (outputFormatIsFloat_) {
        float* output = static_cast<float*>(audioData);
        for (int32_t i = 0; i < numFrames; ++i) {
            float* frame = output + static_cast<size_t>(i) * stride;
            if (mono) {
                frame[0] = (outputBufferLeft_[i] + outputBufferRight_[i]) * 0.5f;
                continue;
            }
            frame[0] = outputBufferLeft_[i];
            frame[1] = outputBufferRight_[i];
            for (size_t channel = 2; channel < stride; ++channel) {
                frame[channel] = 0.0f;
            }
        }
        return;
    }

    int16_t* output = static_cast<int16_t*>(audioData);
    for (int32_t i = 0; i < numFrames; ++i) {
        int16_t* frame = output + static_cast<size_t>(i) * stride;
        if (mono) {
            frame[0] = audioformat::floatToI16(
                (outputBufferLeft_[i] + outputBufferRight_[i]) * 0.5f);
            continue;
        }
        frame[0] = audioformat::floatToI16(outputBufferLeft_[i]);
        frame[1] = audioformat::floatToI16(outputBufferRight_[i]);
        for (size_t channel = 2; channel < stride; ++channel) {
            frame[channel] = 0;
        }
    }
}

oboe::DataCallbackResult AudioEngine::finishOutputCallback() {
    // Must be the last thing the callback does: tune() may call
    // setBufferSizeInFrames(), which AAudio applies to the next callback.
    // OpenSL ES (and a HAL without buffer control) reports ErrorUnimplemented,
    // which LatencyTuner remembers and reports on every following call: ignore
    // it, tuning is simply unavailable on that path.
    if (latencyTuner_) {
        latencyTuner_->tune();
    }
    return oboe::DataCallbackResult::Continue;
}

void AudioEngine::startVstMonitor() {
    if (vstMonitorRunning_.exchange(true)) {
        return;
    }
    vstLastJiffies_.clear();
    vstLastSampleNs_ = 0;
    vstMonitorThread_ = std::thread([this] { vstMonitorLoop(); });
}

void AudioEngine::stopVstMonitor() {
    vstMonitorRunning_.store(false);
    if (vstMonitorThread_.joinable()) {
        vstMonitorThread_.join();
    }
}

void AudioEngine::vstMonitorLoop() {
    LOGI("plugin monitor thread started tid=%ld (interval %d ms)", getTid(),
         kPluginMonitorIntervalMs);
    constexpr int kSlicesPerInterval = 10;
    constexpr int kSliceMs = kPluginMonitorIntervalMs / kSlicesPerInterval;
    while (vstMonitorRunning_.load()) {
        for (int i = 0; i < kSlicesPerInterval && vstMonitorRunning_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kSliceMs));
        }
        if (!vstMonitorRunning_.load()) {
            break;
        }
        sampleVstLoad();
    }
    LOGI("plugin monitor thread exited tid=%ld", getTid());
}

void AudioEngine::sampleVstLoad() {
    const int64_t nowNs = monotonicNowNs();
    uint64_t totalDeltaJiffies = 0;
    int32_t totalUnderruns = 0;

    // Holding a shared lock while reading /proc keeps add/remove out of the
    // way, and try_lock makes this sample yield instead of blocking them.
    const bool sampled = chain_.forEachPluginTryShared([&](IPlugin& plugin) {
        totalUnderruns += plugin.getUnderrunCount();
        const int pid = plugin.getSubprocessPid();
        if (pid <= 0) {
            return;
        }
        /* /proc/<pid>/stat fields utime (14) + stime (15) are cumulative
         * jiffies (typically 100Hz on Android) since subprocess start. Read
         * both, sum, diff against the last sample to derive jiffies per
         * wall-second. */
        char path[64];
        std::snprintf(path, sizeof(path), "/proc/%d/stat", pid);
        FILE* file = std::fopen(path, "r");
        if (file == nullptr) {
            return;
        }
        char buffer[512];
        const size_t got = std::fread(buffer, 1, sizeof(buffer) - 1, file);
        std::fclose(file);
        if (got == 0) {
            return;
        }
        buffer[got] = 0;
        /* Skip past "comm" (in parens — the name can contain spaces), then we
         * are at field 3 (state). utime is field 14, stime is field 15. */
        char* endOfComm = std::strrchr(buffer, ')');
        if (endOfComm == nullptr) {
            return;
        }
        unsigned long long fields[16] = {0};
        const int parsed = std::sscanf(endOfComm + 1,
                                       " %*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu",
                                       &fields[13], &fields[14]);
        if (parsed != 2) {
            return;
        }
        const uint64_t current = fields[13] + fields[14];
        auto entry = vstLastJiffies_.find(pid);
        if (entry != vstLastJiffies_.end()) {
            totalDeltaJiffies += current - entry->second;
            entry->second = current;
        } else {
            vstLastJiffies_[pid] = current;
        }
    });

    if (!sampled) {
        return;
    }

    if (vstLastSampleNs_ > 0 && nowNs > static_cast<int64_t>(vstLastSampleNs_)) {
        const uint64_t elapsedNs = static_cast<uint64_t>(nowNs) - vstLastSampleNs_;
        /* sysconf(_SC_CLK_TCK) is the jiffies-per-second on this system; on
         * Android it is 100. Compute the fraction of one CPU's wall time the
         * subprocesses used in aggregate. */
        long clockTicks = sysconf(_SC_CLK_TCK);
        if (clockTicks <= 0) {
            clockTicks = 100;
        }
        const double seconds = static_cast<double>(elapsedNs) / 1.0e9;
        double fraction =
            (static_cast<double>(totalDeltaJiffies) / static_cast<double>(clockTicks)) / seconds;
        if (fraction > 1.0) {
            fraction = 1.0;
        }
        vstCpuLoad_.store(static_cast<float>(fraction));
    }
    vstLastSampleNs_ = static_cast<uint64_t>(nowNs);
    vstUnderruns_.store(totalUnderruns);
}

void AudioEngine::closeStreams() {
    LOGI("closeStreams() ENTER tid=%ld (caller thread; AudioTrack callback is different tid)", getTid());

    // The monitor thread walks the plugin chain and must be gone before the
    // streams (and later the chain) are torn down.
    stopVstMonitor();
    
    // First, signal the callback to stop immediately to prevent new callbacks
    // from starting while we're tearing down.
    isRunning_.store(false);
    
    if (inputStream_) {
        LOGI("closeStreams() input stream stop+close+reset");
        inputStream_->stop();
        inputStream_->close();
        inputStream_.reset();
    }

    if (outputStream_) {
        LOGI("closeStreams() output stream stop");
        outputStream_->stop();
        LOGI("closeStreams() output stream close");
        outputStream_->close();
        // Wait for any in-flight AAudio callback to finish before destroying the
        // stream object. close() already removed us from AAudioStreamCollection,
        // so late callbacks will see !isStreamAlive and return Stop. If we reset()
        // too soon, ~AudioStreamAAudio / ~AAudioLoader run while the AudioTrack
        // thread is still in getStream() -> destroyed mutex (SIGABRT).
        // 
        // INCREASED from 250ms to 500ms: The 250ms delay was not sufficient on some
        // devices (e.g., OnePlus) where the audio callback thread takes longer to
        // fully exit, especially when the app is being force-closed or when there
        // are concurrent EGL/GL operations that may interfere with the audio subsystem.
        static constexpr int kStreamCloseSleepMs = 500;
        LOGI("closeStreams() sleep %dms before outputStream_.reset() [tid=%ld]", kStreamCloseSleepMs, getTid());
        std::this_thread::sleep_for(std::chrono::milliseconds(kStreamCloseSleepMs));
        LOGI("closeStreams() outputStream_.reset() NOW tid=%ld", getTid());
        outputStream_.reset();
        // The tuner holds a reference to the stream and calls into it from
        // tune(); it is destroyed here, once the callback thread is drained,
        // so ~LatencyTuner cannot reach a freed AAudio stream.
        latencyTuner_.reset();
        LOGI("closeStreams() output stream destroyed tid=%ld", getTid());
    }
    LOGI("closeStreams() done");
}

} // namespace guitarrackcraft
