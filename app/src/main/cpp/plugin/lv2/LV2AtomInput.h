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

#ifndef GUITARRACKCRAFT_LV2_ATOM_INPUT_H
#define GUITARRACKCRAFT_LV2_ATOM_INPUT_H

#include <cstdint>
#include <cstring>

#include <lv2/atom/atom.h>

/*
 * Appending UI atoms to the atom input sequence of an LV2 plugin.
 *
 * This is the only part of the atom mailbox that runs on the audio thread, so
 * it lives here on its own: pure pointer arithmetic over two preallocated
 * buffers, no allocation, no LV2 host dependency (the atom headers are inline).
 * That also makes it testable on the host without lilv.
 */
namespace guitarrackcraft {
namespace lv2rt {

enum class AtomAppend {
    Appended,
    /**
     * No room left in this callback's sequence. The atom was already popped out
     * of the mailbox, so it is *discarded*: process() stops draining the queue
     * for this callback (the entries still in the mailbox wait for the next one)
     * and the plugin never sees this atom. Nothing retries it.
     */
    NoRoom,
    /** The atom cannot fit even in an empty sequence, or is not a valid atom. */
    Unrepresentable,
};

/**
 * Copies the atom `atom` (an LV2_Atom header plus its body) into `sequence` as
 * one event at frame time 0, padding the event to 8 bytes as the atom
 * specification requires.
 *
 * @param capacity total size of the buffer holding `sequence`, in bytes
 */
inline AtomAppend appendAtomToSequence(LV2_Atom_Sequence* sequence, uint32_t capacity,
                                       const uint8_t* atom, uint32_t atomSize) {
    if (sequence == nullptr || atom == nullptr || atomSize < sizeof(LV2_Atom)) {
        return AtomAppend::Unrepresentable;
    }
    const auto* header = reinterpret_cast<const LV2_Atom*>(atom);
    const uint32_t bodySize = header->size;
    if (bodySize > atomSize - sizeof(LV2_Atom)) {
        return AtomAppend::Unrepresentable;
    }

    const uint32_t eventBytes = sizeof(LV2_Atom_Event) + bodySize;
    const uint32_t paddedEventBytes = (eventBytes + 7u) & ~7u;
    const uint32_t emptySequenceEnd = sizeof(LV2_Atom) + sizeof(LV2_Atom_Sequence_Body);
    if (emptySequenceEnd + paddedEventBytes > capacity) {
        return AtomAppend::Unrepresentable;
    }
    const uint32_t used = sizeof(LV2_Atom) + sequence->atom.size;
    if (used + paddedEventBytes > capacity) {
        return AtomAppend::NoRoom;
    }

    auto* writePos = reinterpret_cast<uint8_t*>(&sequence->body) + sequence->atom.size;
    auto* event = reinterpret_cast<LV2_Atom_Event*>(writePos);
    event->time.frames = 0;
    event->body.size = bodySize;
    event->body.type = header->type;
    std::memcpy(reinterpret_cast<uint8_t*>(event) + sizeof(LV2_Atom_Event),
                atom + sizeof(LV2_Atom), bodySize);
    sequence->atom.size += paddedEventBytes;
    return AtomAppend::Appended;
}

} // namespace lv2rt
} // namespace guitarrackcraft

#endif // GUITARRACKCRAFT_LV2_ATOM_INPUT_H