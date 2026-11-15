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

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include "utils/SPSCRing.h"

using guitarrackcraft::SPSCByteRing;
using guitarrackcraft::SPSCRing;

namespace {

constexpr size_t kByteSlot = 32;

using ByteRing = SPSCByteRing<kByteSlot, 4>;
using ByteRingPush = ByteRing::PushOutcome;

/** Payload used by the stress tests: a value plus its complement, so a torn
 *  read (writer half-way through a slot) is detected instead of silently
 *  producing a plausible value. */
struct StressPayload {
    uint64_t value;
    uint64_t complement;
};

StressPayload makePayload(uint64_t value) {
    return StressPayload{value, ~value};
}

bool payloadIsIntact(const StressPayload& payload) {
    return payload.complement == ~payload.value;
}

std::vector<uint8_t> bytesOf(uint8_t seed, size_t size) {
    std::vector<uint8_t> buffer(size);
    for (size_t i = 0; i < size; ++i) {
        buffer[i] = static_cast<uint8_t>(seed + i);
    }
    return buffer;
}

} // namespace

TEST(SPSCRing, PopFromAnEmptyRingFails) {
    ByteRing ring;
    bool popped = false;
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.try_pop([&](size_t, const uint8_t*) { popped = true; }));
    EXPECT_FALSE(popped);
    EXPECT_TRUE(ring.empty());
}

TEST(SPSCRing, PushThenPopReturnsTheSameBytes) {
    ByteRing ring;
    const auto payload = bytesOf(7, 13);

    ASSERT_EQ(ByteRingPush::Pushed, ring.try_push(payload.data(), payload.size()));

    size_t size = 0;
    std::vector<uint8_t> received;
    ASSERT_TRUE(ring.try_pop([&](size_t len, const uint8_t* data) {
        size = len;
        received.assign(data, data + len);
    }));
    EXPECT_EQ(payload, received);
    EXPECT_EQ(payload.size(), size);
    EXPECT_TRUE(ring.empty());
}

TEST(SPSCRing, EntriesComeOutInPushOrder) {
    ByteRing ring;
    for (uint8_t value = 1; value <= 4; ++value) {
        const auto payload = bytesOf(value, 4);
        ASSERT_EQ(ByteRingPush::Pushed, ring.try_push(payload.data(), payload.size()));
    }
    for (uint8_t expected = 1; expected <= 4; ++expected) {
        uint8_t first = 0;
        ASSERT_TRUE(ring.try_pop([&](size_t, const uint8_t* data) { first = data[0]; }));
        EXPECT_EQ(expected, first);
    }
    EXPECT_TRUE(ring.empty());
}

TEST(SPSCRing, FullRingDropsTheOldestEntryAndKeepsTheNewest) {
    ByteRing ring;
    constexpr uint8_t kValues[] = {10, 20, 30, 40, 50};
    for (uint8_t value : kValues) {
        const auto payload = bytesOf(value, 2);
        ring.try_push(payload.data(), payload.size());
    }

    EXPECT_EQ(1u, ring.dropped_oldest());
    EXPECT_EQ(0u, ring.dropped_newest());

    for (uint8_t expected : {20, 30, 40, 50}) {
        uint8_t first = 0;
        ASSERT_TRUE(ring.try_pop([&](size_t, const uint8_t* data) { first = data[0]; }));
        EXPECT_EQ(expected, first);
    }
    EXPECT_TRUE(ring.empty());
}

TEST(SPSCRing, DropOldestKeepsCountingWhileTheRingStaysFull) {
    ByteRing ring;
    for (uint8_t value = 1; value <= 10; ++value) {
        const auto payload = bytesOf(value, 1);
        ring.try_push(payload.data(), payload.size());
    }

    EXPECT_EQ(6u, ring.dropped_oldest());
    EXPECT_EQ(6u, ring.total_dropped());

    for (uint8_t expected = 7; expected <= 10; ++expected) {
        uint8_t first = 0;
        ASSERT_TRUE(ring.try_pop([&](size_t, const uint8_t* data) { first = data[0]; }));
        EXPECT_EQ(expected, first);
    }
    EXPECT_TRUE(ring.empty());
}

TEST(SPSCRing, WrapAroundKeepsTheOrdering) {
    ByteRing ring;
    for (uint8_t round = 0; round < 12; ++round) {
        const auto payload = bytesOf(round, 3);
        ASSERT_EQ(ByteRingPush::Pushed, ring.try_push(payload.data(), payload.size()));

        uint8_t first = 0;
        ASSERT_TRUE(ring.try_pop([&](size_t, const uint8_t* data) { first = data[0]; }));
        EXPECT_EQ(round, first);
    }
    EXPECT_EQ(0u, ring.total_dropped());
    EXPECT_TRUE(ring.empty());
}

TEST(SPSCRing, PayloadUpToTheSlotSizeFits) {
    ByteRing ring;
    const auto payload = bytesOf(3, kByteSlot);

    ASSERT_EQ(ByteRingPush::Pushed, ring.try_push(payload.data(), payload.size()));

    std::vector<uint8_t> received;
    ASSERT_TRUE(ring.try_pop([&](size_t len, const uint8_t* data) {
        received.assign(data, data + len);
    }));
    EXPECT_EQ(payload, received);
}

TEST(SPSCRing, PayloadLargerThanASlotIsRejectedWithoutTouchingTheQueue) {
    ByteRing ring;
    const auto accepted = bytesOf(5, 4);
    ASSERT_EQ(ByteRingPush::Pushed, ring.try_push(accepted.data(), accepted.size()));

    const auto oversized = bytesOf(9, kByteSlot + 1);
    EXPECT_EQ(ByteRingPush::TooLarge, ring.try_push(oversized.data(), oversized.size()));
    EXPECT_EQ(1u, ring.too_large());
    EXPECT_EQ(1u, ring.total_dropped());

    std::vector<uint8_t> received;
    ASSERT_TRUE(ring.try_pop([&](size_t len, const uint8_t* data) {
        received.assign(data, data + len);
    }));
    EXPECT_EQ(accepted, received);
    EXPECT_TRUE(ring.empty());
}

TEST(SPSCRing, EmptyPayloadIsAValidEntry) {
    ByteRing ring;
    ASSERT_EQ(ByteRingPush::Pushed, ring.try_push(nullptr, 0));

    size_t size = 99;
    ASSERT_TRUE(ring.try_pop([&](size_t len, const uint8_t*) { size = len; }));
    EXPECT_EQ(0u, size);
}

TEST(SPSCRing, ClearResetsTheQueueAndTheCounters) {
    ByteRing ring;
    for (uint8_t value = 1; value <= 6; ++value) {
        const auto payload = bytesOf(value, 1);
        ring.try_push(payload.data(), payload.size());
        ring.try_push(bytesOf(value, kByteSlot + 1).data(), kByteSlot + 1);
    }
    ASSERT_NE(0u, ring.total_dropped());

    ring.clear();

    EXPECT_TRUE(ring.empty());
    EXPECT_EQ(0u, ring.total_dropped());
    EXPECT_EQ(0u, ring.too_large());
    EXPECT_FALSE(ring.try_pop([](size_t, const uint8_t*) {}));

    const auto payload = bytesOf(42, 2);
    ASSERT_EQ(ByteRingPush::Pushed, ring.try_push(payload.data(), payload.size()));
    uint8_t first = 0;
    ASSERT_TRUE(ring.try_pop([&](size_t, const uint8_t* data) { first = data[0]; }));
    EXPECT_EQ(42, first);
}

TEST(SPSCRing, PayloadIsWrittenInPlaceThroughTryPushWith) {
    SPSCRing<StressPayload, 2> ring;
    ASSERT_EQ(decltype(ring)::PushOutcome::Pushed, ring.try_push_with([](StressPayload& slot) {
        slot = makePayload(1234);
    }));

    StressPayload received{0, 0};
    ASSERT_TRUE(ring.try_pop([&](const StressPayload& payload) { received = payload; }));
    EXPECT_EQ(1234u, received.value);
    EXPECT_TRUE(payloadIsIntact(received));
}

TEST(SPSCRing, RealTimeVariantDoesNotSpinButStillDelivers) {
    SPSCByteRing<8, 2, false> ring;
    const auto payload = bytesOf(1, 8);
    for (int i = 0; i < 5; ++i) {
        ring.try_push(payload.data(), payload.size());
    }
    EXPECT_EQ(3u, ring.dropped_oldest());

    size_t received = 0;
    while (ring.try_pop([&](size_t, const uint8_t*) { ++received; })) {
    }
    EXPECT_EQ(2u, received);
}

TEST(SPSCRing, StressSingleProducerSingleConsumerKeepsFifoAndAccountsForEveryPush) {
    constexpr uint64_t kItems = 200000;
    SPSCRing<StressPayload, 64> ring;

    std::atomic<bool> producerDone{false};
    std::atomic<uint64_t> pushed{0};
    std::atomic<uint64_t> popped{0};
    std::atomic<uint64_t> corrupted{0};
    std::atomic<uint64_t> outOfOrder{0};

    std::thread consumer([&] {
        uint64_t previous = 0;
        bool havePrevious = false;
        for (;;) {
            const bool finished = producerDone.load(std::memory_order_acquire);
            bool poppedSomething = false;
            while (ring.try_pop([&](const StressPayload& payload) {
                poppedSomething = true;
                popped.fetch_add(1, std::memory_order_relaxed);
                if (!payloadIsIntact(payload)) {
                    corrupted.fetch_add(1, std::memory_order_relaxed);
                }
                if (havePrevious && payload.value <= previous) {
                    outOfOrder.fetch_add(1, std::memory_order_relaxed);
                }
                previous = payload.value;
                havePrevious = true;
            })) {
            }
            if (finished && !poppedSomething) {
                break;
            }
        }
    });

    for (uint64_t value = 0; value < kItems; ++value) {
        ring.try_push(makePayload(value));
        pushed.fetch_add(1, std::memory_order_relaxed);
    }
    producerDone.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(0u, corrupted.load());
    EXPECT_EQ(0u, outOfOrder.load());
    EXPECT_EQ(kItems, pushed.load());
    EXPECT_EQ(pushed.load(), popped.load() + ring.total_dropped());
}

TEST(SPSCRing, StressWithASlowConsumerDropsOnlyCountedEntries) {
    constexpr uint64_t kItems = 100000;
    SPSCRing<StressPayload, 8> ring;

    std::atomic<bool> producerDone{false};
    std::atomic<uint64_t> popped{0};
    std::atomic<uint64_t> corrupted{0};
    std::atomic<uint64_t> outOfOrder{0};

    std::thread consumer([&] {
        uint64_t previous = 0;
        bool havePrevious = false;
        for (;;) {
            const bool finished = producerDone.load(std::memory_order_acquire);
            bool poppedSomething = false;
            while (ring.try_pop([&](const StressPayload& payload) {
                poppedSomething = true;
                popped.fetch_add(1, std::memory_order_relaxed);
                if (!payloadIsIntact(payload)) {
                    corrupted.fetch_add(1, std::memory_order_relaxed);
                }
                if (havePrevious && payload.value <= previous) {
                    outOfOrder.fetch_add(1, std::memory_order_relaxed);
                }
                previous = payload.value;
                havePrevious = true;
            })) {
            }
            if (finished && !poppedSomething) {
                break;
            }
            std::this_thread::yield();
        }
    });

    for (uint64_t value = 0; value < kItems; ++value) {
        ring.try_push(makePayload(value));
        if (value % 512 == 0) {
            std::this_thread::yield();
        }
    }
    producerDone.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(0u, corrupted.load());
    EXPECT_EQ(0u, outOfOrder.load());
    EXPECT_NE(0u, ring.dropped_oldest());
    EXPECT_EQ(kItems, popped.load() + ring.total_dropped());
}

TEST(SPSCRing, StressByteRingCopiesEveryPayloadIntact) {
    constexpr uint64_t kItems = 100000;
    SPSCByteRing<64, 16, false> ring;

    std::atomic<bool> producerDone{false};
    std::atomic<uint64_t> popped{0};
    std::atomic<uint64_t> corrupted{0};
    std::atomic<uint64_t> outOfOrder{0};

    std::thread consumer([&] {
        uint64_t previous = 0;
        bool havePrevious = false;
        for (;;) {
            const bool finished = producerDone.load(std::memory_order_acquire);
            bool poppedSomething = false;
            while (ring.try_pop([&](size_t size, const uint8_t* data) {
                poppedSomething = true;
                popped.fetch_add(1, std::memory_order_relaxed);
                if (size != sizeof(uint64_t)) {
                    corrupted.fetch_add(1, std::memory_order_relaxed);
                    return;
                }
                uint64_t value = 0;
                std::memcpy(&value, data, sizeof(value));
                if (havePrevious && value <= previous) {
                    outOfOrder.fetch_add(1, std::memory_order_relaxed);
                }
                previous = value;
                havePrevious = true;
            })) {
            }
            if (finished && !poppedSomething) {
                break;
            }
        }
    });

    for (uint64_t value = 0; value < kItems; ++value) {
        ring.try_push(&value, sizeof(value));
    }
    producerDone.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(0u, corrupted.load());
    EXPECT_EQ(0u, outOfOrder.load());
    EXPECT_EQ(kItems, popped.load() + ring.total_dropped());
}