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

import android.app.Activity
import android.content.Context
import android.content.pm.ActivityInfo
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.lang.ref.WeakReference

/**
 * User-facing orientation preference. [prefValue] is the value persisted in
 * [UiSettingsManager]; [label] is the text shown in the settings dropdown.
 */
enum class OrientationMode(val prefValue: String, val label: String) {
    AUTO("auto", "Auto"),
    PORTRAIT("portrait", "Portrait"),
    LANDSCAPE("landscape", "Landscape");

    /** `requestedOrientation` value this mode maps to. */
    val screenOrientation: Int
        get() = when (this) {
            AUTO -> ActivityInfo.SCREEN_ORIENTATION_FULL_USER
            PORTRAIT -> ActivityInfo.SCREEN_ORIENTATION_PORTRAIT
            LANDSCAPE -> ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE
        }

    companion object {
        /** Unknown or missing preference values fall back to [AUTO]. */
        fun fromPrefValue(value: String?): OrientationMode =
            values().firstOrNull { it.prefValue == value } ?: AUTO
    }
}

/**
 * Single owner of [Activity.requestedOrientation].
 *
 * The effective orientation is the top of the temporary override stack, or the
 * persisted base mode ([OrientationMode]) when the stack is empty. Screens that
 * only need a specific orientation while they are visible push an override and
 * pop it when they go away, so they never have to know what the previous value
 * was.
 *
 * Thread confinement is enforced, not just documented: every entry point may be
 * called from any thread, and all of them marshal the mutation onto the main
 * thread before touching the state machine ([OrientationOverrideStack], the
 * base mode, the Activity reference) and the window. Nothing here ever blocks,
 * so calling from the main thread (the normal case — Composables' effects,
 * `onCreate`) runs inline and a call from a worker thread is posted, never a
 * deadlock. [effectiveOrientation] is a lock-free read of the last published
 * value and is therefore safe from any thread as well.
 *
 * Every mutation is applied immediately; if no Activity is attached (or the
 * attached one is finishing/destroyed) the state is still recorded and the next
 * [init] applies it.
 */
object OrientationManager {
    private const val TAG = "OrientationManager"

    private val mainHandler = Handler(Looper.getMainLooper())
    private val stack = OrientationOverrideStack(OrientationMode.AUTO.screenOrientation)
    private var activityRef = WeakReference<Activity>(null)

    /** Last value handed to the window, refreshed on the main thread only. */
    @Volatile
    private var publishedOrientation = stack.effectiveOrientation()

    /**
     * Attaches the Activity that owns the window, loads the persisted base mode
     * and applies it. Call once from `MainActivity.onCreate` before
     * `setContent` so the first frame already has the right orientation. The
     * override stack is reset: a new Activity means a new UI tree.
     */
    fun init(activity: Activity) = runOnMain {
        activityRef = WeakReference(activity)
        stack.clear()
        stack.setBaseOrientation(UiSettingsManager.getOrientationMode(activity).screenOrientation)
        applyEffectiveOrientation()
    }

    /** Persists [mode] as the new base mode and applies it (overrides still win). */
    fun setMode(context: Context, mode: OrientationMode) {
        val appContext = context.applicationContext ?: context
        runOnMain {
            UiSettingsManager.setOrientationMode(appContext, mode)
            stack.setBaseOrientation(mode.screenOrientation)
            applyEffectiveOrientation()
        }
    }

    /**
     * Requests [orientation] while [token] is on top of the override stack.
     * Pushing the same token again replaces its orientation and moves it to the
     * top, so a screen can re-push when its content changes.
     */
    fun pushOverride(token: String, orientation: Int) = runOnMain {
        stack.push(token, orientation)
        applyEffectiveOrientation()
    }

    /**
     * Removes [token] wherever it sits in the stack and re-applies the new top.
     * Popping an unknown token is a no-op apart from re-applying the current
     * effective orientation.
     */
    fun popOverride(token: String) = runOnMain {
        stack.pop(token)
        applyEffectiveOrientation()
    }

    /** Drops every override, restoring the persisted base mode. */
    fun clearOverrides() = runOnMain {
        stack.clear()
        applyEffectiveOrientation()
    }

    /**
     * Override on top of the stack, else the base mode, as an `ActivityInfo`
     * value. Returns the value last applied on the main thread, so it is a safe
     * read from any thread and never blocks.
     */
    fun effectiveOrientation(): Int = publishedOrientation

    private fun runOnMain(block: () -> Unit) {
        if (Looper.myLooper() == Looper.getMainLooper()) block() else mainHandler.post(block)
    }

    private fun applyEffectiveOrientation() {
        val orientation = stack.effectiveOrientation()
        publishedOrientation = orientation
        val activity = activityRef.get() ?: return
        if (activity.isFinishing || activity.isDestroyed) return
        if (activity.requestedOrientation == orientation) return
        try {
            activity.requestedOrientation = orientation
        } catch (e: RuntimeException) {
            Log.w(TAG, "requestedOrientation=$orientation rejected by $activity", e)
        }
    }
}
