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

#ifndef GUITARRACKCRAFT_LV2_RT_MAILBOX_H
#define GUITARRACKCRAFT_LV2_RT_MAILBOX_H

#include <atomic>
#include <cstddef>
#include <cstdint>

#include "utils/SPSCRing.h"

/*
 * Lock-free mailboxes of the LV2 wrapper, one per direction that used to be a
 * mutex plus a std::vector/std::string/std::queue.
 *
 * Sizes, and why:
 *
 *  - kAtomBytes is the atom port buffer size the host already gives every atom
 *    port (8 KiB). A bigger UI atom could never be delivered anyway, because
 *    process() has to copy it into that buffer, so the ring rejects it with
 *    TooLarge and counts it. What the audio thread can really deliver is a bit
 *    less: appendAtomToSequence() fits the whole event into that same 8 KiB
 *    sequence (8 B atom header + 8 B sequence body header + 16 B event header =
 *    32 B), so the largest body that travels is 8160 B. Atoms with a body of
 *    8161..8184 B do enter the ring (a slot accepts up to 8192 B of header +
 *    body) and are then discarded as Unrepresentable by the audio thread, which
 *    counts them.
 *  - kAtomSlots is 8: the audio thread drains the whole queue once per callback
 *    (~2.7 ms at 48 kHz/128 frames), so 8 entries only overflow if the audio
 *    thread stalls for 8 callbacks; at that point the oldest knob positions are
 *    worthless and last-wins is the right answer.
 *  - kResponseBytes is also 8 KiB: the largest response in the plugins we ship
 *    is NAM's LV2SwitchModelMsg (type + char[1024] + model pointer ≈ 1040 B);
 *    AIDA-X/DPF ignores the payload entirely (its lv2_work_response drops it)
 *    and the messages it does receive are state key/value pairs of a few
 *    hundred bytes. 8 KiB leaves room for a plugin that answers with a whole
 *    state chunk instead of a fixed struct.
 *  - kResponseSlots is 8 for the same reason as kAtomSlots, and because losing
 *    a response is worse than losing a knob update (NAM's response carries the
 *    freshly loaded model, so dropping it means the model never switches).
 *  - kPathSlots is 2: a path change is a user action that replaces the previous
 *    one (last-wins), a second slot only keeps the RT thread from having to
 *    overwrite the entry it is reading.
 *  - kPathCapacity is the path a slot can hold, terminator included: 1016 bytes
 *    of member leave 1015 bytes of path. A longer path is a programming error
 *    (Android's files dir is ~90 bytes) and is dropped and counted instead of
 *    being truncated, because a cut path names no file and the plugin would
 *    answer with an error and fall back to the previously loaded model.
 *  - kOutputAtomBytes/kOutputAtomSlots: what plugins publish on atom output
 *    ports (patch:Get answers, MIDI, meter/spectrum blobs) is small compared to
 *    the 8 KiB port buffer, and the UI drains the queue on every frame
 *    (32 entries cover ~0.5 s of 60 Hz frames). Larger events are dropped and
 *    counted rather than silently truncated.
 */

namespace guitarrackcraft {
namespace lv2rt {

constexpr size_t kAtomBytes = 8192;
constexpr size_t kAtomSlots = 8;
constexpr size_t kResponseBytes = 8192;
constexpr size_t kResponseSlots = 8;
constexpr size_t kPathSlots = 2;
constexpr size_t kOutputAtomBytes = 512;
constexpr size_t kOutputAtomSlots = 32;

/**
 * A patch:Set the UI wants delivered: the property URID is already resolved on
 * the UI thread (mapping a URI takes a mutex, which the audio thread must not
 * do) and the path is copied in, so the reader never touches a std::string.
 */
struct FilePathMessage {
    static constexpr size_t kPathCapacity = 1016;

    uint32_t propertyUrid = 0;
    uint32_t length = 0;
    char path[kPathCapacity] = {};

    /**
     * Copies `urid` and `path` into the message.
     *
     * A path that does not fit with its terminator is rejected instead of
     * truncated: a cut path names no file, so the caller drops it (and counts
     * it) instead of delivering something the plugin cannot open.
     *
     * @param size path length in bytes, terminator excluded
     * @return false when nothing was written (the message keeps whatever it had,
     *         so a fresh one stays empty); true when the urid and the whole path
     *         are in the message
     */
    bool assign(uint32_t urid, const char* value, size_t size) {
        if (value == nullptr || size >= kPathCapacity) {
            return false;
        }
        for (size_t i = 0; i < size; ++i) {
            path[i] = value[i];
        }
        path[size] = '\0';
        propertyUrid = urid;
        length = static_cast<uint32_t>(size);
        return true;
    }
};

static_assert(sizeof(FilePathMessage) == 1024,
              "the file path mailbox slot is 1024 bytes, see kPathSlots");

/** One atom read from an output port, queued for the UI thread. */
struct OutputAtom {
    static constexpr size_t kPayloadCapacity = kOutputAtomBytes - 2 * sizeof(uint32_t);

    uint32_t portIndex = 0;
    uint32_t size = 0;
    uint8_t data[kPayloadCapacity] = {};
};

static_assert(sizeof(OutputAtom) == kOutputAtomBytes,
              "output atom slots are 512 bytes, see kOutputAtomBytes");

/** UI thread -> audio thread: raw atoms (DPF state sync, knob feedback). */
using AtomRing = SPSCByteRing<kAtomBytes, kAtomSlots>;

/**
 * UI thread -> audio thread: the patch:Set path to forge on the next run().
 *
 * The ring itself is single-producer, but this producer is not a single thread:
 * setFilePath() arrives from Dispatchers.Main (RackViewModel) and from
 * Dispatchers.IO (Tone3000 and ToneDetail), so LV2Plugin wraps the producer side
 * in a mutex. That lock deliberately lives there and not here: this header stays
 * host-testable and the contract it documents is "push() from one thread at a
 * time", which the plugin then guarantees.
 *
 * A path that does not fit a slot is counted here, next to the entries the ring
 * replaces or discards, so a truncated file name is never mistaken for a load
 * that the plugin refused.
 */
class FilePathRing {
    using Ring = SPSCRing<FilePathMessage, kPathSlots>;

public:
    enum class PushOutcome {
        Pushed,
        /** The ring was full: the oldest entry was replaced by the new one. */
        DroppedOldest,
        /** The consumer was still forging the oldest entry: the new one was discarded. */
        DroppedNewest,
        /** The path does not fit a slot with its terminator: never offered to the ring. */
        TooLong,
    };

    PushOutcome try_push(uint32_t urid, const char* path, size_t size) {
        FilePathMessage message;
        if (!message.assign(urid, path, size)) {
            tooLong_.fetch_add(1, std::memory_order_relaxed);
            return PushOutcome::TooLong;
        }
        switch (ring_.try_push(message)) {
            case Ring::PushOutcome::DroppedOldest:
                return PushOutcome::DroppedOldest;
            case Ring::PushOutcome::DroppedNewest:
                return PushOutcome::DroppedNewest;
            case Ring::PushOutcome::Pushed:
                break;
        }
        return PushOutcome::Pushed;
    }

    /** `sink(const FilePathMessage&)` reads the entry in place; the reference dies with the call. */
    template <typename Sink>
    bool try_pop(Sink&& sink) {
        return ring_.try_pop(sink);
    }

    /** Paths rejected because they do not fit a slot, terminator included. */
    uint64_t too_long() const { return tooLong_.load(std::memory_order_relaxed); }
    uint64_t dropped_oldest() const { return ring_.dropped_oldest(); }
    uint64_t dropped_newest() const { return ring_.dropped_newest(); }
    uint64_t total_dropped() const { return ring_.total_dropped() + too_long(); }
    bool empty() const { return ring_.empty(); }

    /** Only valid when neither end is running (start/stop, teardown, tests). */
    void clear() {
        ring_.clear();
        tooLong_.store(0, std::memory_order_relaxed);
    }

private:
    Ring ring_;
    std::atomic<uint64_t> tooLong_{0};
};

/** Worker thread -> audio thread: LV2 work responses, delivered after run(). */
using ResponseRing = SPSCByteRing<kResponseBytes, kResponseSlots>;
/**
 * Audio thread -> UI thread: output atoms. The producer runs on the audio
 * thread, so it must not yield when a slot is still being read: it drops and
 * counts instead.
 */
using OutputAtomRing = SPSCRing<OutputAtom, kOutputAtomSlots, false>;

} // namespace lv2rt
} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_LV2_RT_MAILBOX_H