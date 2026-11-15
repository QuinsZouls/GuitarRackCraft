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

#include <cstdint>
#include <cstring>
#include <vector>

#include "plugin/lv2/LV2AtomInput.h"

using guitarrackcraft::lv2rt::appendAtomToSequence;
using guitarrackcraft::lv2rt::AtomAppend;

namespace {

// The engine's atom port budget, hard-coded on purpose: the test must fail if
// the constant in the header changes silently.
constexpr uint32_t kCapacity = 8192;
// An event carries its frame time (8 bytes) plus the atom header (8 bytes).
constexpr uint32_t kEventHeader = 16;
// An empty sequence already holds the body header (unit + pad), and events are
// appended after it, so the sequence size starts at 8 and not at 0.
constexpr uint32_t kEmptySequenceSize = 8;

/** First event of a sequence that was filled from empty. */
const uint8_t* firstEvent(const LV2_Atom_Sequence* sequence) {
    return reinterpret_cast<const uint8_t*>(sequence) + sizeof(LV2_Atom) + kEmptySequenceSize;
}

/** A buffer laid out as an empty LV2 atom sequence, like process() resets it. */
class SequenceBuffer {
public:
    explicit SequenceBuffer(uint32_t capacity = kCapacity) : storage_(capacity, 0) {
        auto* sequence = reinterpret_cast<LV2_Atom_Sequence*>(storage_.data());
        sequence->atom.type = 1;
        sequence->atom.size = sizeof(LV2_Atom_Sequence_Body);
        sequence->body.unit = 0;
        sequence->body.pad = 0;
    }

    LV2_Atom_Sequence* sequence() {
        return reinterpret_cast<LV2_Atom_Sequence*>(storage_.data());
    }

    uint32_t used() {
        return reinterpret_cast<LV2_Atom_Sequence*>(storage_.data())->atom.size;
    }

private:
    std::vector<uint8_t> storage_;
};

/** Builds an LV2 atom with `bodySize` bytes of 0x5a, preceded by its header. */
std::vector<uint8_t> makeAtom(uint32_t type, uint32_t bodySize) {
    std::vector<uint8_t> atom(sizeof(LV2_Atom) + bodySize, 0);
    auto* header = reinterpret_cast<LV2_Atom*>(atom.data());
    header->size = bodySize;
    header->type = type;
    std::memset(atom.data() + sizeof(LV2_Atom), 0x5a, bodySize);
    return atom;
}

} // namespace

TEST(LV2AtomInput, AppendsOneAtomAsAnEventAtFrameZero) {
    SequenceBuffer buffer;
    const auto atom = makeAtom(7, 4);

    ASSERT_EQ(AtomAppend::Appended,
              appendAtomToSequence(buffer.sequence(), kCapacity, atom.data(),
                                   static_cast<uint32_t>(atom.size())));

    // The body header (8) plus a 16 byte event header and a 4 byte body,
    // padded to 24.
    EXPECT_EQ(8u + 24u, buffer.used());

    const auto* event = reinterpret_cast<const LV2_Atom_Event*>(firstEvent(buffer.sequence()));
    EXPECT_EQ(0, event->time.frames);
    EXPECT_EQ(7u, event->body.type);
    EXPECT_EQ(4u, event->body.size);
    const auto* payload = reinterpret_cast<const uint8_t*>(event) + kEventHeader;
    EXPECT_EQ(0x5a, payload[0]);
    EXPECT_EQ(0x5a, payload[3]);
}

TEST(LV2AtomInput, PadsTheEventToEightBytes) {
    SequenceBuffer buffer;
    const auto atom = makeAtom(3, 3);

    ASSERT_EQ(AtomAppend::Appended,
              appendAtomToSequence(buffer.sequence(), kCapacity, atom.data(),
                                   static_cast<uint32_t>(atom.size())));

    // The body header plus 16 + 3 = 19 bytes of event, rounded up to 24.
    EXPECT_EQ(32u, buffer.used());
}

TEST(LV2AtomInput, AppendsSeveralAtomsInOrder) {
    SequenceBuffer buffer;
    for (uint32_t index = 1; index <= 3; ++index) {
        const auto atom = makeAtom(index, 8);
        ASSERT_EQ(AtomAppend::Appended,
                  appendAtomToSequence(buffer.sequence(), kCapacity, atom.data(),
                                       static_cast<uint32_t>(atom.size())));
    }
    EXPECT_EQ(8u + 3u * 24u, buffer.used());

    const auto* first = reinterpret_cast<const LV2_Atom_Event*>(firstEvent(buffer.sequence()));
    const auto* second = reinterpret_cast<const LV2_Atom_Event*>(
        reinterpret_cast<const uint8_t*>(first) + 24);
    EXPECT_EQ(1u, first->body.type);
    EXPECT_EQ(2u, second->body.type);
}

TEST(LV2AtomInput, ReportsNoRoomWhenTheSequenceIsFull) {
    SequenceBuffer buffer;
    const auto atom = makeAtom(1, 8);
    uint32_t appended = 0;
    while (appendAtomToSequence(buffer.sequence(), kCapacity, atom.data(),
                                static_cast<uint32_t>(atom.size())) == AtomAppend::Appended) {
        ++appended;
    }

    // 8192 bytes in total: 8 for the sequence atom header, 8 for the body
    // header and 24 per event, so 340 events fit and 8 bytes are left over.
    EXPECT_EQ(340u, appended);
    EXPECT_EQ(8168u, buffer.used());
    EXPECT_EQ(AtomAppend::NoRoom,
              appendAtomToSequence(buffer.sequence(), kCapacity, atom.data(),
                                   static_cast<uint32_t>(atom.size())));
}

TEST(LV2AtomInput, AnAtomThatCannotFitAnEmptySequenceIsUnrepresentable) {
    SequenceBuffer buffer;
    // 8192 - 16 (atom header + body header) = 8176 bytes available for the whole
    // event, of which 16 are the event header: a 8160 byte body is the largest
    // atom that can ever be delivered.
    const auto tooBig = makeAtom(1, 8161);
    EXPECT_EQ(AtomAppend::Unrepresentable,
              appendAtomToSequence(buffer.sequence(), kCapacity, tooBig.data(),
                                   static_cast<uint32_t>(tooBig.size())));
    EXPECT_EQ(kEmptySequenceSize, buffer.used());

    const auto largestThatFits = makeAtom(1, 8160);
    EXPECT_EQ(AtomAppend::Appended,
              appendAtomToSequence(buffer.sequence(), kCapacity, largestThatFits.data(),
                                   static_cast<uint32_t>(largestThatFits.size())));
    EXPECT_EQ(8184u, buffer.used());
}

TEST(LV2AtomInput, RejectsTruncatedAndEmptyPayloads) {
    SequenceBuffer buffer;
    const auto atom = makeAtom(1, 4);

    EXPECT_EQ(AtomAppend::Unrepresentable,
              appendAtomToSequence(buffer.sequence(), kCapacity, atom.data(),
                                   kEventHeader - sizeof(LV2_Atom) - 1));
    EXPECT_EQ(AtomAppend::Unrepresentable,
              appendAtomToSequence(buffer.sequence(), kCapacity, nullptr, 16));
    EXPECT_EQ(AtomAppend::Unrepresentable,
              appendAtomToSequence(nullptr, kCapacity, atom.data(),
                                   static_cast<uint32_t>(atom.size())));
    EXPECT_EQ(kEmptySequenceSize, buffer.used());
}

TEST(LV2AtomInput, RejectsABodyLargerThanTheOfferedBytes) {
    SequenceBuffer buffer;
    auto atom = makeAtom(1, 64);
    reinterpret_cast<LV2_Atom*>(atom.data())->size = 4096;

    EXPECT_EQ(AtomAppend::Unrepresentable,
              appendAtomToSequence(buffer.sequence(), kCapacity, atom.data(),
                                   static_cast<uint32_t>(atom.size())));
    EXPECT_EQ(kEmptySequenceSize, buffer.used());
}