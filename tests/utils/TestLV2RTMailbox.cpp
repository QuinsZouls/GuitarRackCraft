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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "plugin/lv2/LV2RTMailbox.h"

using guitarrackcraft::lv2rt::AtomRing;
using guitarrackcraft::lv2rt::FilePathMessage;
using guitarrackcraft::lv2rt::FilePathRing;
using guitarrackcraft::lv2rt::OutputAtom;
using guitarrackcraft::lv2rt::OutputAtomRing;

namespace {

std::string textOf(std::initializer_list<uint8_t> bytes) {
    return std::string(bytes.begin(), bytes.end());
}

/** The path capacity of a mailbox slot, hard-coded on purpose: the boundary
 *  tests below must fail if the constant in the header moves silently. */
constexpr size_t kPathCapacity = 1016;
/** Longest path a slot can hold: the capacity minus the terminator. */
constexpr size_t kLongestPath = kPathCapacity - 1;

/** Waits for `flag`, so a broken producer fails the test instead of hanging it. */
bool waitFor(const std::atomic<bool>& flag, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (flag.load()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load();
}

/** Marks every byte of `message`, so a rejected assign that writes shows up. */
void markAllBytes(FilePathMessage& message, unsigned char value) {
    auto* raw = reinterpret_cast<unsigned char*>(&message);
    std::fill(raw, raw + sizeof(message), value);
}

/** Index of the first byte of `message` that is not `value`, or sizeof(message). */
size_t firstByteNotEqualTo(const FilePathMessage& message, unsigned char value) {
    const auto* raw = reinterpret_cast<const unsigned char*>(&message);
    for (size_t index = 0; index < sizeof(message); ++index) {
        if (raw[index] != value) {
            return index;
        }
    }
    return sizeof(message);
}

} // namespace

TEST(LV2RTMailbox, FilePathMessageIsOneKilobyte) {
    EXPECT_EQ(1024u, sizeof(FilePathMessage));
    EXPECT_EQ(1016u, sizeof(FilePathMessage::path));
}

TEST(LV2RTMailbox, FilePathMessageCopiesUridAndPath) {
    FilePathMessage message;
    const std::string path = "/data/user/0/app/files/models/amp.nam";

    EXPECT_TRUE(message.assign(42, path.c_str(), path.size()));

    EXPECT_EQ(42u, message.propertyUrid);
    EXPECT_EQ(path.size(), message.length);
    EXPECT_STREQ(path.c_str(), message.path);
}

TEST(LV2RTMailbox, FilePathMessageAcceptsALongPathIncludingTheLastByteThatFits) {
    FilePathMessage message;
    const std::string fits(kLongestPath, 'p');

    EXPECT_TRUE(message.assign(1, fits.c_str(), fits.size()));

    EXPECT_EQ(kLongestPath, message.length);
    EXPECT_EQ(kLongestPath, std::strlen(message.path));
    EXPECT_STREQ(fits.c_str(), message.path);
    EXPECT_EQ('\0', message.path[kLongestPath]);
}

TEST(LV2RTMailbox, FilePathMessageRejectsTheFirstPathThatDoesNotFitWithoutWriting) {
    FilePathMessage message;
    markAllBytes(message, 0x5a);
    const std::string oneByteTooLong(kPathCapacity, 'y');

    EXPECT_FALSE(message.assign(9, oneByteTooLong.c_str(), oneByteTooLong.size()));

    EXPECT_EQ(sizeof(message), firstByteNotEqualTo(message, 0x5a))
        << "a rejected assign wrote bytes into the message";
}

TEST(LV2RTMailbox, FilePathMessageRejectsAMissingPath) {
    FilePathMessage message;
    markAllBytes(message, 0x5a);

    EXPECT_FALSE(message.assign(9, nullptr, 0));
    EXPECT_FALSE(message.assign(9, nullptr, 7));

    EXPECT_EQ(sizeof(message), firstByteNotEqualTo(message, 0x5a))
        << "a rejected assign wrote bytes into the message";
}

TEST(LV2RTMailbox, FilePathRingKeepsTheNewestOfThreePushes) {
    FilePathRing ring;
    for (uint32_t urid = 1; urid <= 3; ++urid) {
        const std::string path = "/models/" + std::to_string(urid);
        ring.try_push(urid, path.c_str(), path.size());
    }

    EXPECT_EQ(1u, ring.dropped_oldest());

    uint32_t seen = 0;
    int count = 0;
    while (ring.try_pop([&](const FilePathMessage& message) {
        ++count;
        seen = message.propertyUrid;
    })) {
    }
    EXPECT_EQ(2, count);
    EXPECT_EQ(3u, seen);
}

TEST(LV2RTMailbox, FilePathRingDropsAndCountsAPathThatDoesNotFit) {
    FilePathRing ring;
    const std::string tooLong(kPathCapacity, 'z');

    EXPECT_EQ(FilePathRing::PushOutcome::TooLong,
              ring.try_push(5, tooLong.c_str(), tooLong.size()));

    EXPECT_EQ(1u, ring.too_long());
    EXPECT_EQ(1u, ring.total_dropped());
    EXPECT_TRUE(ring.empty());

    const std::string fits(kLongestPath, 'z');
    EXPECT_EQ(FilePathRing::PushOutcome::Pushed, ring.try_push(5, fits.c_str(), fits.size()));

    uint32_t length = 0;
    ASSERT_TRUE(ring.try_pop([&](const FilePathMessage& message) { length = message.length; }));
    EXPECT_EQ(kLongestPath, length);
}

TEST(LV2RTMailbox, OutputAtomFitsItsSlot) {
    EXPECT_EQ(512u, sizeof(OutputAtom));
    EXPECT_EQ(504u, sizeof(OutputAtom::data));

    OutputAtomRing ring;
    OutputAtom atom;
    atom.portIndex = 3;
    atom.size = 504;
    std::memset(atom.data, 0xa5, sizeof(atom.data));

    EXPECT_EQ(OutputAtomRing::PushOutcome::Pushed, ring.try_push(atom));

    uint32_t port = 0;
    uint32_t size = 0;
    uint8_t first = 0;
    ASSERT_TRUE(ring.try_pop([&](const OutputAtom& pushed) {
        port = pushed.portIndex;
        size = pushed.size;
        first = pushed.data[0];
    }));
    EXPECT_EQ(3u, port);
    EXPECT_EQ(504u, size);
    EXPECT_EQ(0xa5, first);
}

TEST(LV2RTMailbox, OutputAtomRingDropsTheOldestOfThirtyThree) {
    OutputAtomRing ring;
    for (uint32_t index = 0; index < 33; ++index) {
        OutputAtom atom;
        atom.portIndex = index;
        ring.try_push(atom);
    }

    EXPECT_EQ(1u, ring.dropped_oldest());

    std::vector<uint32_t> ports;
    while (ring.try_pop([&](const OutputAtom& atom) { ports.push_back(atom.portIndex); })) {
    }
    ASSERT_EQ(32u, ports.size());
    EXPECT_EQ(1u, ports.front());
    EXPECT_EQ(32u, ports.back());
}

TEST(LV2RTMailbox, OutputAtomRingProducerDoesNotSpinWhileTheConsumerHoldsASlot) {
    OutputAtomRing ring;
    for (uint32_t index = 0; index < 32; ++index) {
        OutputAtom atom;
        atom.portIndex = index;
        ring.try_push(atom);
    }

    // A consumer that claims the oldest slot and sits in the sink: the slot stays
    // unreleased, so the RT producer finds the position it needs still busy.
    std::atomic<bool> holdsSlot{false};
    std::thread consumer([&ring, &holdsSlot] {
        ring.try_pop([&holdsSlot](const OutputAtom&) {
            holdsSlot.store(true);
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        });
    });
    ASSERT_TRUE(waitFor(holdsSlot, std::chrono::milliseconds(2000)))
        << "the consumer never claimed a slot, the test cannot measure anything";

    OutputAtom atom;
    atom.portIndex = 99;
    const auto start = std::chrono::steady_clock::now();
    const auto outcome = ring.try_push(atom);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    // The RT policy is drop + count, never wait: the push has to return long
    // before the consumer releases the slot (a spin on the busy slot would need
    // the full 500 ms and an unbounded one would never return).
    EXPECT_EQ(OutputAtomRing::PushOutcome::DroppedNewest, outcome);
    EXPECT_EQ(1u, ring.dropped_newest());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100)
        << "the RT producer waited for the consumer";

    consumer.join();
}

TEST(LV2RTMailbox, AtomRingHoldsEightEightKilobyteAtoms) {
    AtomRing ring;
    const std::string atom = textOf({0x01, 0x02, 0x03, 0x04});

    for (int index = 0; index < 8; ++index) {
        EXPECT_EQ(AtomRing::PushOutcome::Pushed, ring.try_push(atom.data(), atom.size()));
    }
    EXPECT_EQ(0u, ring.total_dropped());

    EXPECT_EQ(AtomRing::PushOutcome::DroppedOldest, ring.try_push(atom.data(), atom.size()));
    EXPECT_EQ(1u, ring.dropped_oldest());

    const std::vector<uint8_t> oversized(8193, 0x5a);
    EXPECT_EQ(AtomRing::PushOutcome::TooLarge,
              ring.try_push(oversized.data(), oversized.size()));
    EXPECT_EQ(1u, ring.too_large());
}
