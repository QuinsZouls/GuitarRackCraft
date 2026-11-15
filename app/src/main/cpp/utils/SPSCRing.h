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

#ifndef GUITARRACKCRAFT_SPSC_RING_H
#define GUITARRACKCRAFT_SPSC_RING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>

/*
 * Bounded single-producer / single-consumer ring for the audio thread.
 *
 * Design constraints that shaped it:
 *
 *  - The storage is flat and preallocated: `Count` slots of `Slot` bytes live
 *    inside the object, so pushing or popping never allocates and never
 *    follows a per-item pointer. A slot must therefore be trivially copyable;
 *    payloads carry their own length instead of owning memory.
 *  - No locks: the two ends only meet through atomics. `head_` is written by
 *    the producer alone, `tail_` by the consumer alone, and each slot has a
 *    sequence number that publishes "the payload of position N is complete"
 *    (seq == N + 1) or "this slot is free for position N" (seq == N). That is
 *    the same publication protocol as a Vyukov bounded queue, which removes
 *    the need for the producer to ever read `tail_` to know whether the
 *    payload it is about to read/write is stable.
 *  - Full means "the oldest entry is replaced" (last-wins), the policy a
 *    control-rate mailbox wants: while the UI drags a knob, the intermediate
 *    positions are worthless and only the newest matters. The alternative
 *    (dropping the newest) would leave the device on a stale value forever.
 *  - Dropping the oldest is only safe after proving the consumer is not
 *    reading that slot. The consumer claims an entry by moving `tail_` with a
 *    compare-exchange *before* it touches the payload, so a producer that wins
 *    the same compare-exchange knows the slot is untouched. When the consumer
 *    won the race the producer retries a bounded number of times (the consumer
 *    releases the slot within a memcpy) and only then drops the new entry,
 *    counting it.
 *  - Cache-line isolation: the producer-owned counters (head + drop counters)
 *    share one cache line and the consumer-owned tail another, so the two
 *    threads do not ping-pong a line between them. Slots are padded to the
 *    cache line as well: an atom/producer writing slot N must not invalidate
 *    the line holding slot N+1 of the other end.
 *  - `SpinOnBusyProducer` is a compile-time policy: a UI or worker thread can
 *    afford yields between retries to avoid losing an event, the audio thread
 *    cannot, so the RT-to-UI ring is instantiated with `false` and only retries
 *    the atomic loads (no syscall) before dropping and counting.
 */

namespace guitarrackcraft {

/** Bytes plus their used length: the byte-payload slot of SPSCRing. */
template <size_t Bytes>
struct SPSCByteSlot {
    static constexpr size_t kCapacity = Bytes;

    uint32_t size;
    uint8_t data[Bytes];
};

/**
 * @tparam Slot  trivially copyable, fixed-size payload (no owning members)
 * @tparam Count number of slots, >= 2
 * @tparam SpinOnBusyProducer whether the producer may yield when the consumer
 *         is still copying the slot it wants. Must be false for a producer
 *         that runs on the audio thread.
 */
template <typename Slot, size_t Count, bool SpinOnBusyProducer = true>
class SPSCRing {
    static_assert(Count >= 2, "a ring needs at least two slots to tell full from empty");
    static_assert(std::is_trivially_copyable<Slot>::value,
                  "slots are stored flat: a slot must not own memory or need a copy constructor");
    static_assert(std::atomic<uint64_t>::is_always_lock_free,
                  "the sequence numbers must not fall back to a lock");

public:
    SPSCRing() { releaseAllSlots(); }

    enum class PushOutcome {
        Pushed,
        /** The ring was full: the oldest entry was replaced by the new one. */
        DroppedOldest,
        /** The consumer was still reading the oldest entry: the new one was discarded. */
        DroppedNewest,
    };

    /**
     * Fills the next free slot through `writer(Slot&)` and publishes it, so the
     * payload is written in place instead of being copied through a temporary.
     */
    template <typename Writer>
    PushOutcome try_push_with(Writer&& writer) {
        const uint64_t position = head_.load(std::memory_order_relaxed);
        bool replacedOldest = false;

        for (int attempt = 0;; ++attempt) {
            const uint64_t tail = tail_.load(std::memory_order_acquire);
            if (position - tail >= Count) {
                // Full: claim the oldest entry before overwriting its slot. The
                // consumer claims the entry it reads the same way, so winning the
                // compare-exchange proves it is not copying that slot right now.
                uint64_t expected = tail;
                if (tail_.compare_exchange_strong(expected, tail + 1, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
                    replacedOldest = true;
                    break;
                }
            } else if (slots_[position % Count].sequence.load(std::memory_order_acquire) ==
                       position) {
                break;
            }
            if (attempt >= kBusyRetries) {
                droppedNewest_.fetch_add(1, std::memory_order_relaxed);
                return PushOutcome::DroppedNewest;
            }
            if (SpinOnBusyProducer) {
                std::this_thread::yield();
            }
        }

        SlotState& slot = slots_[position % Count];
        writer(slot.value);
        slot.sequence.store(position + 1, std::memory_order_release);
        head_.store(position + 1, std::memory_order_release);

        if (replacedOldest) {
            droppedOldest_.fetch_add(1, std::memory_order_relaxed);
            return PushOutcome::DroppedOldest;
        }
        return PushOutcome::Pushed;
    }

    PushOutcome try_push(const Slot& item) {
        return try_push_with([&item](Slot& slot) { slot = item; });
    }

    /**
     * Moves one entry to `sink(const Slot&)` and releases the slot once the sink
     * returns. The reference is only valid inside that call, which lets the
     * audio thread read a payload in place without copying it out first.
     */
    template <typename Sink>
    bool try_pop(Sink&& sink) {
        const uint64_t position = tail_.load(std::memory_order_relaxed);
        SlotState& slot = slots_[position % Count];
        if (slot.sequence.load(std::memory_order_acquire) != position + 1) {
            return false;
        }
        uint64_t expected = position;
        if (!tail_.compare_exchange_strong(expected, position + 1, std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
            return false;
        }
        const Slot& payload = slot.value;
        sink(payload);
        slot.sequence.store(position + Count, std::memory_order_release);
        return true;
    }

    /** Entries replaced by a newer one because the ring was full. */
    uint64_t dropped_oldest() const { return droppedOldest_.load(std::memory_order_relaxed); }

    /** Entries discarded because the consumer was copying the slot they needed. */
    uint64_t dropped_newest() const { return droppedNewest_.load(std::memory_order_relaxed); }

    uint64_t total_dropped() const { return dropped_oldest() + dropped_newest(); }

    /** Approximate: true when no published entry is waiting. */
    bool empty() const {
        const uint64_t position = tail_.load(std::memory_order_acquire);
        return slots_[position % Count].sequence.load(std::memory_order_acquire) != position + 1;
    }

    /** Only valid when neither end is running (start/stop, teardown, tests). */
    void clear() {
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
        droppedOldest_.store(0, std::memory_order_relaxed);
        droppedNewest_.store(0, std::memory_order_relaxed);
        releaseAllSlots();
    }

private:
    struct alignas(64) SlotState {
        std::atomic<uint64_t> sequence;
        Slot value;
    };

    /**
     * Slot i starts out free for position i (its first position): "sequence N"
     * means "free for position N", which is also what the consumer stores when
     * it releases an entry (N = position + Count).
     */
    void releaseAllSlots() {
        for (size_t index = 0; index < Count; ++index) {
            slots_[index].sequence.store(index, std::memory_order_relaxed);
        }
    }

    /**
     * Retries before the producer gives up on an entry the consumer is still
     * copying. A UI/worker producer yields between attempts (its slot frees up
     * within a memcpy); the audio-thread producer only retries the atomic loads,
     * so the worst case stays a handful of instructions instead of a syscall.
     */
    static constexpr int kBusyRetries = SpinOnBusyProducer ? 64 : 8;

    // Producer-owned counters (head + drop counters) share one cache line.
    std::atomic<uint64_t> head_{0};
    std::atomic<uint64_t> droppedOldest_{0};
    std::atomic<uint64_t> droppedNewest_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};
    alignas(64) SlotState slots_[Count];
};

/** Byte payloads with an explicit length, the shape every message here uses. */
template <size_t Bytes, size_t Count, bool SpinOnBusyProducer = true>
class SPSCByteRing {
public:
    using Slot = SPSCByteSlot<Bytes>;
    using Ring = SPSCRing<Slot, Count, SpinOnBusyProducer>;

    enum class PushOutcome {
        Pushed,
        DroppedOldest,
        DroppedNewest,
        /** The payload does not fit in a slot and was never offered to the ring. */
        TooLarge,
    };

    PushOutcome try_push(const void* data, size_t size) {
        if (size > Bytes) {
            tooLarge_.fetch_add(1, std::memory_order_relaxed);
            return PushOutcome::TooLarge;
        }
        const auto outcome = ring_.try_push_with([data, size](Slot& slot) {
            slot.size = static_cast<uint32_t>(size);
            if (size > 0 && data != nullptr) {
                std::memcpy(slot.data, data, size);
            }
        });
        switch (outcome) {
            case Ring::PushOutcome::DroppedOldest:
                return PushOutcome::DroppedOldest;
            case Ring::PushOutcome::DroppedNewest:
                return PushOutcome::DroppedNewest;
            case Ring::PushOutcome::Pushed:
                break;
        }
        return PushOutcome::Pushed;
    }

    /** `sink(size, data)` reads the payload in place; the pointer dies with the call. */
    template <typename Sink>
    bool try_pop(Sink&& sink) {
        return ring_.try_pop([&sink](const Slot& slot) { sink(slot.size, slot.data); });
    }

    uint64_t too_large() const { return tooLarge_.load(std::memory_order_relaxed); }
    uint64_t dropped_oldest() const { return ring_.dropped_oldest(); }
    uint64_t dropped_newest() const { return ring_.dropped_newest(); }
    uint64_t total_dropped() const { return ring_.total_dropped() + too_large(); }
    bool empty() const { return ring_.empty(); }

    void clear() {
        ring_.clear();
        tooLarge_.store(0, std::memory_order_relaxed);
    }

private:
    Ring ring_;
    std::atomic<uint64_t> tooLarge_{0};
};

} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_SPSC_RING_H