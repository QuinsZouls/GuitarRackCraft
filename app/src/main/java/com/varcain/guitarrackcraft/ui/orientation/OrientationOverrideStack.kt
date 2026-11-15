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

package com.varcain.guitarrackcraft.ui.orientation

/**
 * Orientation decision state: the persisted base orientation plus the stack of
 * temporary overrides requested by visible screens.
 *
 * Overrides are keyed by an opaque token and ordered by push; the last entry is
 * the effective override. Pushing a token that is already on the stack replaces
 * its orientation AND moves it to the top, so a screen can re-push whenever its
 * content changes. Popping removes the token wherever it sits, so screens never
 * have to remember what they replaced.
 *
 * Deliberately free of Android and Compose types: [OrientationManager] owns the
 * Activity, the persistence and the main-thread confinement, while the ordering
 * rules that decide the final orientation live here and are unit-testable on a
 * plain JVM. Not thread-safe — the caller confines it to one thread.
 *
 * [effectiveOrientation] is the only way to read an orientation out of the
 * stack: the base value is write-only, so a caller cannot skip the override
 * stack (and silently apply the base mode) by accident.
 */
class OrientationOverrideStack(baseOrientation: Int) {

    private val entries = LinkedHashMap<String, Int>()
    private var base = baseOrientation

    val isEmpty: Boolean get() = entries.isEmpty()

    val size: Int get() = entries.size

    /** Changes the orientation applied while no override is on the stack. */
    fun setBaseOrientation(orientation: Int) {
        base = orientation
    }

    /** Adds [token] on top, replacing any previous entry for the same token. */
    fun push(token: String, orientation: Int) {
        entries.remove(token)
        entries[token] = orientation
    }

    /** Removes [token] wherever it sits; unknown tokens are ignored. */
    fun pop(token: String) {
        entries.remove(token)
    }

    fun clear() {
        entries.clear()
    }

    /** Orientation of the topmost override, or `null` when none is pushed. */
    fun top(): Int? = entries.values.lastOrNull()

    /** Tokens from the bottom to the top of the stack. */
    fun tokens(): List<String> = entries.keys.toList()

    /** Topmost override, else the base orientation. */
    fun effectiveOrientation(): Int = top() ?: base
}