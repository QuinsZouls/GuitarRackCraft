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

#include "AudioRecorder.h"
#include <android/log.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#define LOG_TAG "AudioRecorder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace guitarrackcraft {

AudioRecorder::AudioRecorder() = default;

AudioRecorder::~AudioRecorder() {
    if (recording_.load()) {
        stopRecording();
    }
}

bool AudioRecorder::startRecording(const std::string& rawPath, const std::string& processedPath, float sampleRate) {
    if (recording_.load()) {
        LOGE("Already recording");
        return false;
    }

    sampleRate_ = sampleRate;
    totalRawFrames_.store(0);

    // Size ring buffers: 2 seconds of audio
    size_t rawCapacity = static_cast<size_t>(sampleRate * 2);           // mono
    size_t processedCapacity = static_cast<size_t>(sampleRate * 4);     // stereo interleaved
    rawRing_.resize(rawCapacity);
    processedRing_.resize(processedCapacity);

    // Open files
    rawFile_.open(rawPath, std::ios::binary | std::ios::trunc);
    if (!rawFile_.is_open()) {
        LOGE("Failed to open raw file: %s", rawPath.c_str());
        return false;
    }

    processedFile_.open(processedPath, std::ios::binary | std::ios::trunc);
    if (!processedFile_.is_open()) {
        LOGE("Failed to open processed file: %s", processedPath.c_str());
        rawFile_.close();
        return false;
    }

    // Preallocate the interleave scratch so feedAudio() never allocates on the
    // audio thread: kMaxFeedFrames stereo frames is far above any real HAL block.
    interleaveBuffer_.assign(static_cast<size_t>(kMaxFeedFrames) * 2, 0.0f);

    // Write placeholder WAV headers (will be finalized on stop)
    writeWavHeader(rawFile_, 1, static_cast<uint32_t>(sampleRate));
    writeWavHeader(processedFile_, 2, static_cast<uint32_t>(sampleRate));

    // Start writer thread. The release store publishes the interleave scratch
    // that was just (re)sized above: feedAudio() acquires it before writing, so
    // the audio thread cannot observe a recording_=true with a stale buffer.
    writerRunning_.store(true);
    recording_.store(true, std::memory_order_release);
    writerThread_ = std::thread(&AudioRecorder::writerLoop, this);

    LOGI("Recording started: raw=%s processed=%s sr=%.0f", rawPath.c_str(), processedPath.c_str(), sampleRate);
    return true;
}

void AudioRecorder::stopRecording() {
    if (!recording_.load()) return;

    recording_.store(false, std::memory_order_release);
    writerRunning_.store(false);

    if (writerThread_.joinable()) {
        writerThread_.join();
    }

    // Final drain
    size_t rawTotalSamples = 0;
    size_t processedTotalSamples = 0;
    drainRing(rawRing_, rawFile_, rawTotalSamples);
    drainRing(processedRing_, processedFile_, processedTotalSamples);

    // Finalize WAV headers with actual sizes
    size_t totalFrames = totalRawFrames_.load();
    finalizeWavFile(rawFile_, totalFrames, 1);
    finalizeWavFile(processedFile_, totalFrames, 2);

    rawFile_.close();
    processedFile_.close();

    rawRing_.reset();
    processedRing_.reset();

    LOGI("Recording stopped: %zu frames (%.1f sec)", totalFrames, totalFrames / static_cast<double>(sampleRate_));
}

double AudioRecorder::getDurationSec() const {
    if (sampleRate_ <= 0.0f) return 0.0;
    return static_cast<double>(totalRawFrames_.load()) / sampleRate_;
}

void AudioRecorder::feedAudio(const float* rawMono, const float* processedL, const float* processedR, int32_t numFrames) {
    // Acquire pairs with startRecording()'s release store: it guarantees that the
    // preallocated interleaveBuffer_ written there is visible on the audio thread.
    if (!recording_.load(std::memory_order_acquire)) return;
    if (rawMono == nullptr || processedL == nullptr || processedR == nullptr || numFrames <= 0) return;

    // Write raw mono
    rawRing_.write(rawMono, static_cast<size_t>(numFrames));

    // Interleave stereo into the stack buffer for the common small blocks and
    // into the preallocated member buffer for larger ones. Never resizes: on the
    // audio thread a resize is a malloc, and an oversized block is truncated
    // (the ring would drop it anyway) rather than allocating.
    constexpr int32_t kStackInterleaveFrames = 512;
    float stackBuf[kStackInterleaveFrames * 2];
    float* buf;
    int32_t framesToWrite = numFrames;
    if (numFrames <= kStackInterleaveFrames) {
        buf = stackBuf;
    } else {
        if (static_cast<size_t>(numFrames) * 2 > interleaveBuffer_.size()) {
            static auto lastTruncateLog = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration<double>(now - lastTruncateLog).count() >= 1.0) {
                lastTruncateLog = now;
                LOGE("feedAudio: %d frames exceed the preallocated %zu interleave samples, "
                     "recording truncated", numFrames, interleaveBuffer_.size());
            }
            framesToWrite = static_cast<int32_t>(interleaveBuffer_.size() / 2);
        }
        buf = interleaveBuffer_.data();
    }

    for (int32_t i = 0; i < framesToWrite; ++i) {
        buf[i * 2] = processedL[i];
        buf[i * 2 + 1] = processedR[i];
    }
    processedRing_.write(buf, static_cast<size_t>(framesToWrite) * 2);

    totalRawFrames_.fetch_add(static_cast<size_t>(numFrames), std::memory_order_relaxed);
}

void AudioRecorder::writerLoop() {
    LOGI("Writer thread started");

    size_t rawTotalSamples = 0;
    size_t processedTotalSamples = 0;

    while (writerRunning_.load()) {
        drainRing(rawRing_, rawFile_, rawTotalSamples);
        drainRing(processedRing_, processedFile_, processedTotalSamples);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    // One final drain after stopping
    drainRing(rawRing_, rawFile_, rawTotalSamples);
    drainRing(processedRing_, processedFile_, processedTotalSamples);

    LOGI("Writer thread exiting: rawSamples=%zu processedSamples=%zu", rawTotalSamples, processedTotalSamples);
}

void AudioRecorder::drainRing(RingBuffer& ring, std::ofstream& file, size_t& totalSamples) {
    float readBuf[4096];
    while (ring.available() > 0) {
        size_t n = ring.read(readBuf, 4096);
        if (n == 0) break;

        // Convert float to int16_t
        int16_t pcmBuf[4096];
        for (size_t i = 0; i < n; ++i) {
            float clamped = std::max(-1.0f, std::min(1.0f, readBuf[i]));
            pcmBuf[i] = static_cast<int16_t>(clamped * 32767.0f);
        }
        file.write(reinterpret_cast<const char*>(pcmBuf), n * sizeof(int16_t));
        totalSamples += n;
    }
}

void AudioRecorder::writeWavHeader(std::ofstream& file, uint16_t numChannels, uint32_t sampleRate) {
    // Write a placeholder WAV header (44 bytes). Sizes will be patched in finalizeWavFile().
    uint16_t bitsPerSample = 16;
    uint32_t byteRate = sampleRate * numChannels * (bitsPerSample / 8);
    uint16_t blockAlign = numChannels * (bitsPerSample / 8);

    file.write("RIFF", 4);
    uint32_t chunkSize = 0; // placeholder
    file.write(reinterpret_cast<const char*>(&chunkSize), 4);
    file.write("WAVE", 4);

    // fmt sub-chunk
    file.write("fmt ", 4);
    uint32_t fmtSize = 16;
    file.write(reinterpret_cast<const char*>(&fmtSize), 4);
    uint16_t audioFormat = 1; // PCM
    file.write(reinterpret_cast<const char*>(&audioFormat), 2);
    file.write(reinterpret_cast<const char*>(&numChannels), 2);
    file.write(reinterpret_cast<const char*>(&sampleRate), 4);
    file.write(reinterpret_cast<const char*>(&byteRate), 4);
    file.write(reinterpret_cast<const char*>(&blockAlign), 2);
    file.write(reinterpret_cast<const char*>(&bitsPerSample), 2);

    // data sub-chunk
    file.write("data", 4);
    uint32_t dataSize = 0; // placeholder
    file.write(reinterpret_cast<const char*>(&dataSize), 4);
}

void AudioRecorder::finalizeWavFile(std::ofstream& file, size_t totalFrames, uint16_t numChannels) {
    if (!file.is_open()) return;

    uint32_t dataSize = static_cast<uint32_t>(totalFrames * numChannels * sizeof(int16_t));
    uint32_t chunkSize = 36 + dataSize;

    // Patch RIFF chunk size at offset 4
    file.seekp(4);
    file.write(reinterpret_cast<const char*>(&chunkSize), 4);

    // Patch data sub-chunk size at offset 40
    file.seekp(40);
    file.write(reinterpret_cast<const char*>(&dataSize), 4);

    file.flush();
}

} // namespace guitarrackcraft
