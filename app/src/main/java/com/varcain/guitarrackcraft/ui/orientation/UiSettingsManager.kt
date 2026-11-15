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

import android.content.Context

/**
 * Persisted UI preferences that are not engine settings. Read on the calling
 * thread; every write is applied to memory immediately and flushed by
 * [android.content.SharedPreferences.Editor.apply].
 */
object UiSettingsManager {
    private const val PREFS_NAME = "ui_settings"
    private const val KEY_ORIENTATION_MODE = "orientationMode"

    private fun prefs(context: Context) =
        context.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE)

    /**
     * Base orientation preference. [OrientationMode.AUTO] when unset, when the
     * stored value is unknown, or when it was written by a newer/older build.
     */
    fun getOrientationMode(context: Context): OrientationMode =
        OrientationMode.fromPrefValue(
            prefs(context).getString(KEY_ORIENTATION_MODE, OrientationMode.AUTO.prefValue)
        )

    fun setOrientationMode(context: Context, mode: OrientationMode) {
        prefs(context).edit().putString(KEY_ORIENTATION_MODE, mode.prefValue).apply()
    }
}