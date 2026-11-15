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

package com.varcain.guitarrackcraft.engine

import android.content.Context
import java.io.File

/**
 * Configuration actually granted by the audio HAL, as reported by
 * `nativeGetStreamInfo`. Every field has a safe default so an engine that is
 * not running (or an older native library) still produces a usable object.
 */
data class AudioStreamInfo(
    val isAAudio: Boolean = false,
    val inputExclusive: Boolean = false,
    val outputExclusive: Boolean = false,
    val inputLowLatency: Boolean = false,
    val outputLowLatency: Boolean = false,
    val outputMMap: Boolean = false,
    val outputCallback: Boolean = false,
    val framesPerBurst: Int = 0,
    val inputMMap: Boolean = false,
    val inputChannels: Int = 0,
    val outputChannels: Int = 0,
    val inputFormat: Int = FORMAT_FLOAT,
    val outputFormat: Int = FORMAT_FLOAT,
    val inputPreset: Int = PRESET_OTHER,
    val outputBufferSizeInFrames: Int = 0,
    val outputBufferCapacity: Int = 0,
    val outputFramesPerCallback: Int = 0,
    val inputFramesPerBurst: Int = 0,
    val inputBufferSizeInFrames: Int = 0,
    val outputTimestampSupported: Boolean = false
) {
    val inputFormatLabel: String
        get() = formatLabel(inputFormat)

    val outputFormatLabel: String
        get() = formatLabel(outputFormat)

    val inputPresetLabel: String
        get() = when (inputPreset) {
            PRESET_VOICE_RECOGNITION -> "Voice Recognition"
            PRESET_VOICE_PERFORMANCE -> "Voice Performance"
            else -> "Other"
        }

    /** True when neither stream reached the lowest-latency path; the UI shows a hint then. */
    val usesLegacyPath: Boolean
        get() = !outputExclusive || !outputMMap || !inputExclusive || !inputMMap

    companion object {
        const val FORMAT_FLOAT = 0
        const val FORMAT_I16 = 1

        const val PRESET_VOICE_RECOGNITION = 0
        const val PRESET_VOICE_PERFORMANCE = 1
        const val PRESET_OTHER = 2

        /** Field count of the native `nativeGetStreamInfo` array. */
        const val FIELD_COUNT = 20

        fun formatLabel(format: Int): String = when (format) {
            FORMAT_FLOAT -> "32-bit Float"
            FORMAT_I16 -> "16-bit PCM"
            else -> "Unknown"
        }

        /**
         * Maps the native array. A short array is tolerated (missing fields keep
         * their defaults) so a version mismatch between the APK and the native
         * library can never crash the settings screen.
         */
        fun fromArray(values: IntArray?): AudioStreamInfo {
            if (values == null) return AudioStreamInfo()
            fun field(index: Int) = if (index < values.size) values[index] else 0
            return AudioStreamInfo(
                isAAudio = field(0) != 0,
                inputExclusive = field(1) != 0,
                outputExclusive = field(2) != 0,
                inputLowLatency = field(3) != 0,
                outputLowLatency = field(4) != 0,
                outputMMap = field(5) != 0,
                outputCallback = field(6) != 0,
                framesPerBurst = field(7),
                inputMMap = field(8) != 0,
                inputChannels = field(9),
                outputChannels = field(10),
                inputFormat = field(11),
                outputFormat = field(12),
                inputPreset = field(13),
                outputBufferSizeInFrames = field(14),
                outputBufferCapacity = field(15),
                outputFramesPerCallback = field(16),
                inputFramesPerBurst = field(17),
                inputBufferSizeInFrames = field(18),
                outputTimestampSupported = field(19) != 0
            )
        }
    }
}

/**
 * Round-trip latency from the stream timestamps, in milliseconds.
 * `roundTripMs` is the number the top bar shows: what the player actually feels.
 */
data class LatencyDetails(
    val inputMs: Double = 0.0,
    val outputMs: Double = 0.0,
    val roundTripMs: Double = 0.0
) {
    companion object {
        const val FIELD_COUNT = 3

        fun fromArray(values: DoubleArray?): LatencyDetails {
            if (values == null) return LatencyDetails()
            fun field(index: Int) = if (index < values.size) values[index] else 0.0
            return LatencyDetails(
                inputMs = field(0),
                outputMs = field(1),
                roundTripMs = field(2)
            )
        }
    }
}

/**
 * JNI bridge to the native audio engine.
 * Provides a Kotlin interface to the C++ audio processing engine.
 */
class NativeEngine private constructor() {
    
    companion object {
        @Volatile
        private var INSTANCE: NativeEngine? = null

        fun getInstance(): NativeEngine {
            return INSTANCE ?: synchronized(this) {
                INSTANCE ?: NativeEngine().also { INSTANCE = it }
            }
        }
    }

    init {
        // Preload libc++_shared.so so that dlopen() of LV2 plugin .so files
        // (which depend on it) can resolve it from the app's lib/ directory
        try {
            System.loadLibrary("c++_shared")
        } catch (_: UnsatisfiedLinkError) {}

        System.loadLibrary("guitarrackcraft")
    }

    /**
     * Set the path where LV2 bundles (e.g. Guitarix) are extracted.
     * Must be called before nativeInit() so plugins are discovered.
     */
    external fun nativeSetLv2Path(path: String)

    /**
     * Set the path to the app's native library directory (for symlink-based plugin loading).
     * Must be called before nativeInit().
     */
    external fun nativeSetNativeLibDir(path: String)

    /**
     * Set the path to the app's files directory. Call before nativeInit() if needed.
     */
    external fun nativeSetFilesDir(path: String)

    /**
     * Set the path to extracted X11 SONAME libs (libX11.so.6 etc.) so plugin UI load can copy them into the bundle dir.
     * Call after extracting assets to e.g. context.filesDir/x11_libs.
     */
    external fun nativeSetX11LibsDir(path: String)

    /**
     * Set the path to extracted PAD plugin .so files (playstore flavor).
     * Must be called before nativeInit() so plugins are discovered.
     */
    external fun nativeSetPluginLibDir(path: String)

    /**
     * Initialize the native engine.
     * Must be called before any other methods.
     * Call nativeSetLv2Path() first with the extracted assets path to enable LV2 plugins.
     */
    external fun nativeInit(): Boolean

    /** Re-runs each PluginFactory's initialize() and rebuilds the registry's
     *  plugin cache. Used by the Manage VST UI (full flavor) so an imported
     *  VST appears in the browser without restarting the audio engine. */
    external fun nativeRefreshPluginRegistry(): Boolean

    /** X11 display slot the plugin at the given rack position renders to,
     *  or -1 if the position is invalid or the plugin doesn't use X11.
     *  VST plugins (full flavor only) return their wine display number; LV2
     *  plugins return -1. */
    external fun nativeGetRackPluginX11Display(position: Int): Int

    /** Editor size of the plugin at the given rack position, encoded as
     *  one Long: high 32 bits = width, low 32 bits = height. 0 if no
     *  editor or size unknown yet. */
    external fun nativeGetRackPluginEditorSize(position: Int): Long

    /**
     * Start the audio engine.
     * @param sampleRate Desired sample rate (will use device default if not supported)
     * @return true if started successfully
     */
    external fun nativeStartEngine(sampleRate: Float = 48000f, inputDeviceId: Int = 0, outputDeviceId: Int = 0, bufferFrames: Int = 0): Boolean

    /**
     * Stop the audio engine.
     */
    external fun nativeStopEngine()

    /**
     * Check if the engine is currently running.
     */
    external fun nativeIsEngineRunning(): Boolean

    /**
     * Get current sample rate in Hz.
     */
    external fun nativeGetSampleRate(): Float

    /**
     * Get actual buffer frame count used by the audio callback.
     */
    external fun nativeGetBufferFrameCount(): Int

    /**
     * Get stream info for low-latency checklist.
     * Returns 20 fields, see [AudioStreamInfo.fromArray].
     */
    external fun nativeGetStreamInfo(): IntArray

    /**
     * Get the current round-trip latency in milliseconds (input + output).
     */
    external fun nativeGetLatencyMs(): Double

    /**
     * Get the latency breakdown: [input ms, output ms, round-trip ms].
     */
    external fun nativeGetLatencyDetails(): DoubleArray

    /**
     * Get input peak level (0.0–1.0).
     */
    external fun nativeGetInputLevel(): Float

    /**
     * Get output peak level (0.0–1.0).
     */
    external fun nativeGetOutputLevel(): Float

    /**
     * Get CPU load (0.0–1.0).
     */
    external fun nativeGetCpuLoad(): Float

    /**
     * Get cumulative audio xrun (underrun/overrun) count.
     */
    external fun nativeGetXRunCount(): Int

    /**
     * True if input has clipped.
     */
    external fun nativeIsInputClipping(): Boolean

    /**
     * True if output has clipped.
     */
    external fun nativeIsOutputClipping(): Boolean

    /**
     * Clear clipping indicators.
     */
    external fun nativeResetClipping()

    /**
     * Get list of all available plugins.
     */
    external fun nativeGetAvailablePlugins(): Array<PluginInfo>

    /**
     * Add a plugin to the rack.
     * @param pluginId Full plugin ID (format:plugin_id)
     * @param position Position in chain (-1 for end)
     * @return Index of inserted plugin, or -1 on failure
     */
    external fun nativeAddPluginToRack(pluginId: String, position: Int = -1): Int

    /**
     * Remove a plugin from the rack.
     * @param position Index of plugin to remove
     * @return true if removed successfully
     */
    external fun nativeRemovePluginFromRack(position: Int): Boolean

    /**
     * Reorder plugins in the rack.
     * @param fromPos Source position
     * @param toPos Destination position
     * @return true if reordered successfully
     */
    external fun nativeReorderRack(fromPos: Int, toPos: Int): Boolean

    /**
     * Send a file path to a plugin via LV2 patch:Set.
     * @param pluginIndex Index of plugin in chain
     * @param propertyUri LV2 property URI (e.g. model file parameter)
     * @param filePath Absolute file path on device
     */
    external fun nativeSetPluginFilePath(pluginIndex: Int, propertyUri: String, filePath: String)

    /**
     * Set a parameter value for a plugin in the rack.
     * @param pluginIndex Index of plugin in chain
     * @param portIndex Index of control port
     * @param value Parameter value
     */
    external fun nativeSetParameter(pluginIndex: Int, portIndex: Int, value: Float)

    /**
     * Get a parameter value for a plugin in the rack.
     * @param pluginIndex Index of plugin in chain
     * @param portIndex Index of control port
     * @return Current parameter value
     */
    external fun nativeGetParameter(pluginIndex: Int, portIndex: Int): Float

    /**
     * Process an audio file offline through the plugin chain.
     * Offline processing is not real-time, so the global gains are applied as a
     * constant factor per block instead of the live ramp; the result is the
     * steady-state gain the live engine converges to.
     * @param inputPath Path to input WAV file
     * @param outputPath Path to output WAV file
     * @param inputGainDb Global input gain in dB applied before the chain (0 = unity)
     * @param outputGainDb Global output gain in dB applied after the chain (0 = unity)
     * @return true if processing succeeded
     */
    external fun nativeProcessFile(
        inputPath: String,
        outputPath: String,
        inputGainDb: Float,
        outputGainDb: Float
    ): Boolean

    /**
     * Get the number of plugins in the rack.
     */
    external fun nativeGetRackSize(): Int

    /**
     * Get plugin info for a plugin in the rack.
     * @param index Index of plugin in rack
     * @return PluginInfo or null if index is invalid
     */
    external fun nativeGetRackPluginInfo(index: Int): PluginInfo?

    // --- X11 UI management (EGL + ANativeWindow, native X server) ---

    /**
     * Attach an Android Surface to the given X11 display number.
     * Starts the native X server and EGL rendering. Returns root window ID for createPluginUI.
     */
    external fun nativeAttachSurfaceToDisplay(displayNumber: Int, surface: android.view.Surface, width: Int, height: Int): Long

    /** Detach surface and stop X server for the display. */
    /** Signal X11 threads to exit without blocking (call from view destroy callback). */
    /** @return true if detach was deferred (createPluginUI still running); false if connection was closed now */
    external fun nativeSignalDetachSurfaceFromDisplay(displayNumber: Int): Boolean
    external fun nativeStopX11RenderThreadOnly(displayNumber: Int)
    external fun nativeDetachSurfaceFromDisplay(displayNumber: Int)

    /** Release display resources (call after detach when releasing display number). */
    external fun nativeDestroyX11Display(displayNumber: Int)
    /** Detach and destroy display only if it exists (for delayed cleanup from view destroy). */
    external fun nativeDetachAndDestroyX11DisplayIfExists(displayNumber: Int)

    /**
     * Hide the X11 display without destroying it.
     * Stops the render thread, but keeps the X server and surface alive.
     * This allows the display to be reused when switching back to X11 UI mode.
     * Use this when switching to another UI mode (MODGUI/Sliders) to avoid driver mutex crashes.
     */
    external fun nativeHideX11Display(displayNumber: Int)

    /**
     * Resume the X11 display after hiding.
     * Restarts the render thread to resume rendering.
     * Use this when switching back to X11 UI mode.
     */
    external fun nativeResumeX11Display(displayNumber: Int)

    /** Update surface size (e.g. on surfaceChanged). */
    external fun nativeSetSurfaceSize(displayNumber: Int, width: Int, height: Int)

    /** Inject touch: action 0=down, 1=up, 2=move; x, y in view coordinates. */
    external fun nativeInjectTouch(displayNumber: Int, action: Int, x: Int, y: Int)

    /** Hit-test: returns true if (x, y) in surface coords hits an X11 widget (knob, slider, etc.). */
    external fun nativeIsWidgetAtPoint(displayNumber: Int, x: Int, y: Int): Boolean

    /** Request a frame (swap buffers). */
    external fun nativeRequestX11Frame(displayNumber: Int)

    /** Get plugin natural window size [width, height]. Returns [0, 0] if not yet known. */
    external fun nativeGetX11PluginSize(displayNumber: Int): IntArray

    /** Get the UI scale factor for the given display (1.0 = no scaling). */
    external fun nativeGetX11UIScale(displayNumber: Int): Float

    /**
     * Mark that we are about to create a plugin UI for this display.
     * Call from the UI thread before posting createPluginUI to the executor, so that if surfaceDestroyed
     * runs before the executor runs, signalDetach will defer (avoid closing X11 connection and killing the process).
     */
    external fun nativeBeginCreatePluginUI(displayNumber: Int, pluginIndex: Int)

    /**
     * Create an X11 UI for the plugin at the given rack index.
     * The UI binary is loaded via dlopen and connects to the native X server; parentWindowId is the root window ID from nativeAttachSurfaceToDisplay.
     */
    external fun nativeCreatePluginUI(pluginIndex: Int, displayNumber: Int, parentWindowId: Long): Boolean

    /** Tear down the X11 UI for the given rack index. */
    external fun nativeDestroyPluginUI(pluginIndex: Int)

    /** Pump idle on every active X11 UI (event processing).  Returns true if any UI reported work (dirty). */
    external fun nativeIdlePluginUIs(): Boolean

    /** Poll for a pending file request from any X11 plugin UI (ui:requestValue).
     *  Returns [pluginIndex, propertyUri] or null if no request pending. */
    external fun nativePollFileRequest(): Array<String>?

    /** Deliver a file path to a plugin's X11 UI via patch:Set atom. */
    external fun nativeDeliverFileToPluginUI(pluginIndex: Int, propertyUri: String, filePath: String)

    /** Poll for a pending Wine common-dialog file request from a hosted VST. */
    external fun nativePollVstFilePickerRequest(pluginIndex: Int): Array<String>?

    /** Deliver a Windows path, or cancellation, to a hosted VST file dialog. */
    external fun nativeRespondVstFilePicker(
        pluginIndex: Int,
        sequence: Int,
        cancelled: Boolean,
        windowsPath: String
    )

    /** Save the complete chain state (all plugins) as a JSON string. */
    external fun nativeSaveChainState(): String?

    /** Restore a single plugin's state from decomposed arrays. */
    external fun nativeRestorePluginState(
        pluginIndex: Int,
        portValues: FloatArray, portIndices: IntArray,
        propertyKeys: Array<String>, propertyTypes: Array<String>,
        propertyValues: Array<ByteArray>, propertyFlags: IntArray
    ): Boolean

    // Kotlin wrappers
    fun attachSurfaceToDisplay(displayNumber: Int, surface: android.view.Surface, width: Int, height: Int): Long =
        nativeAttachSurfaceToDisplay(displayNumber, surface, width, height)

    fun signalDetachSurfaceFromDisplay(displayNumber: Int): Boolean = nativeSignalDetachSurfaceFromDisplay(displayNumber)
    fun stopX11RenderThreadOnly(displayNumber: Int) = nativeStopX11RenderThreadOnly(displayNumber)
    fun detachSurfaceFromDisplay(displayNumber: Int) = nativeDetachSurfaceFromDisplay(displayNumber)
    fun destroyX11Display(displayNumber: Int) = nativeDestroyX11Display(displayNumber)
    fun detachAndDestroyX11DisplayIfExists(displayNumber: Int) = nativeDetachAndDestroyX11DisplayIfExists(displayNumber)
    fun hideX11Display(displayNumber: Int) = nativeHideX11Display(displayNumber)
    fun resumeX11Display(displayNumber: Int) = nativeResumeX11Display(displayNumber)
    fun setSurfaceSize(displayNumber: Int, width: Int, height: Int) = nativeSetSurfaceSize(displayNumber, width, height)
    fun injectTouch(displayNumber: Int, action: Int, x: Int, y: Int) = nativeInjectTouch(displayNumber, action, x, y)
    fun isWidgetAtPoint(displayNumber: Int, x: Int, y: Int): Boolean = nativeIsWidgetAtPoint(displayNumber, x, y)
    fun requestX11Frame(displayNumber: Int) = nativeRequestX11Frame(displayNumber)
    fun getX11PluginSize(displayNumber: Int): IntArray = nativeGetX11PluginSize(displayNumber)
    fun getX11UIScale(displayNumber: Int): Float = nativeGetX11UIScale(displayNumber)

    /**
     * Ensure the X11 scratch directory exists and set on native side.
     * X11 SONAME libs (libX11.so.6 etc.) are loaded from nativeLibDir at runtime.
     * This directory is used as a writable scratch dir for plugin UI .so copies.
     */
    fun ensureX11LibsDir(context: Context): File {
        val dir = File(context.filesDir, "x11_libs/arm64-v8a")
        dir.mkdirs()
        nativeSetX11LibsDir(dir.absolutePath)
        return dir
    }

    fun createPluginUI(pluginIndex: Int, displayNumber: Int, parentWindowId: Long): Boolean =
        nativeCreatePluginUI(pluginIndex, displayNumber, parentWindowId)

    fun destroyPluginUI(pluginIndex: Int) = nativeDestroyPluginUI(pluginIndex)

    fun idlePluginUIs(): Boolean = nativeIdlePluginUIs()

    // --- Real-time recording ---

    external fun nativeStartRecording(rawPath: String, processedPath: String): Boolean
    external fun nativeStopRecording()
    external fun nativeIsRecording(): Boolean
    external fun nativeGetRecordingDurationSec(): Double

    // --- WAV real-time playback ---

    external fun nativeLoadWav(path: String): Boolean
    external fun nativeUnloadWav()
    external fun nativeWavPlay()
    external fun nativeWavPause()
    external fun nativeWavSeek(positionSec: Double)
    external fun nativeGetWavDurationSec(): Double
    external fun nativeGetWavPositionSec(): Double
    external fun nativeIsWavPlaying(): Boolean
    external fun nativeIsWavLoaded(): Boolean
    external fun nativeSetChainBypass(bypass: Boolean)
    external fun nativeSetWavBypassChain(bypass: Boolean)

    // Kotlin-friendly wrapper methods
    fun setChainBypass(bypass: Boolean) = nativeSetChainBypass(bypass)
    fun setWavBypassChain(bypass: Boolean) = nativeSetWavBypassChain(bypass)

    // --- Global gain stage (dB, clamped natively to [-24, +24], 0 dB = unity) ---

    /** Set the global input gain in dB. Natively clamped to [-24, +24] and applied
     *  on the next audio callback, so it never needs an engine restart. */
    external fun nativeSetGlobalInputGain(gainDb: Float)

    /** Set the global output gain in dB. Natively clamped to [-24, +24] and applied
     *  on the next audio callback, so it never needs an engine restart. */
    external fun nativeSetGlobalOutputGain(gainDb: Float)

    fun setGlobalInputGain(gainDb: Float) = nativeSetGlobalInputGain(gainDb)
    fun setGlobalOutputGain(gainDb: Float) = nativeSetGlobalOutputGain(gainDb)

    fun startEngine(sampleRate: Float = 48000f, inputDeviceId: Int = 0, outputDeviceId: Int = 0, bufferFrames: Int = 0): Boolean {
        return nativeStartEngine(sampleRate, inputDeviceId, outputDeviceId, bufferFrames)
    }

    fun stopEngine() {
        nativeStopEngine()
    }

    fun isEngineRunning(): Boolean {
        return nativeIsEngineRunning()
    }

    fun getSampleRate(): Float = nativeGetSampleRate()
    fun getBufferFrameCount(): Int = nativeGetBufferFrameCount()
    fun getStreamInfo(): AudioStreamInfo = AudioStreamInfo.fromArray(nativeGetStreamInfo())

    fun getLatencyMs(): Double {
        return nativeGetLatencyMs()
    }

    fun getLatencyDetails(): LatencyDetails = LatencyDetails.fromArray(nativeGetLatencyDetails())

    fun getInputLevel(): Float = nativeGetInputLevel()
    fun getOutputLevel(): Float = nativeGetOutputLevel()
    fun getCpuLoad(): Float = nativeGetCpuLoad()
    fun getXRunCount(): Int = nativeGetXRunCount()
    fun isInputClipping(): Boolean = nativeIsInputClipping()
    fun isOutputClipping(): Boolean = nativeIsOutputClipping()
    fun resetClipping() = nativeResetClipping()

    fun getAvailablePlugins(): List<PluginInfo> {
        return nativeGetAvailablePlugins().toList()
    }

    fun addPluginToRack(pluginId: String, position: Int = -1): Int {
        return nativeAddPluginToRack(pluginId, position)
    }

    fun removePluginFromRack(position: Int): Boolean {
        return nativeRemovePluginFromRack(position)
    }

    fun reorderRack(fromPos: Int, toPos: Int): Boolean {
        return nativeReorderRack(fromPos, toPos)
    }

    fun setPluginFilePath(pluginIndex: Int, propertyUri: String, filePath: String) {
        nativeSetPluginFilePath(pluginIndex, propertyUri, filePath)
    }

    fun setParameter(pluginIndex: Int, portIndex: Int, value: Float) {
        nativeSetParameter(pluginIndex, portIndex, value)
    }

    fun getParameter(pluginIndex: Int, portIndex: Int): Float {
        return nativeGetParameter(pluginIndex, portIndex)
    }

    fun processFile(
        inputFile: File,
        outputFile: File,
        inputGainDb: Float = AudioSettingsManager.DEFAULT_GAIN_DB,
        outputGainDb: Float = AudioSettingsManager.DEFAULT_GAIN_DB
    ): Boolean {
        return nativeProcessFile(
            inputFile.absolutePath,
            outputFile.absolutePath,
            inputGainDb,
            outputGainDb
        )
    }

    fun getRackSize(): Int {
        return nativeGetRackSize()
    }

    fun getRackPluginInfo(index: Int): PluginInfo? {
        return nativeGetRackPluginInfo(index)
    }

    fun getRackPlugins(): List<PluginInfo> {
        val size = getRackSize()
        return (0 until size).mapNotNull { index ->
            getRackPluginInfo(index)?.apply {
                // Store the index for reference
            }
        }
    }

    // Recording wrappers
    fun startRecording(rawPath: String, processedPath: String): Boolean = nativeStartRecording(rawPath, processedPath)
    fun stopRecording() = nativeStopRecording()
    fun isRecording(): Boolean = nativeIsRecording()
    fun getRecordingDurationSec(): Double = nativeGetRecordingDurationSec()

    // WAV playback wrappers
    fun loadWav(file: File): Boolean = nativeLoadWav(file.absolutePath)
    fun loadWav(path: String): Boolean = nativeLoadWav(path)
    fun unloadWav() = nativeUnloadWav()
    fun wavPlay() = nativeWavPlay()
    fun wavPause() = nativeWavPause()
    fun wavSeek(positionSec: Double) = nativeWavSeek(positionSec)
    fun getWavDurationSec(): Double = nativeGetWavDurationSec()
    fun getWavPositionSec(): Double = nativeGetWavPositionSec()
    fun isWavPlaying(): Boolean = nativeIsWavPlaying()
    fun isWavLoaded(): Boolean = nativeIsWavLoaded()

    // State save/restore wrappers
    fun saveChainState(): String? = nativeSaveChainState()

    fun restorePluginState(
        pluginIndex: Int,
        portValues: FloatArray, portIndices: IntArray,
        propertyKeys: Array<String>, propertyTypes: Array<String>,
        propertyValues: Array<ByteArray>, propertyFlags: IntArray
    ): Boolean = nativeRestorePluginState(
        pluginIndex, portValues, portIndices,
        propertyKeys, propertyTypes, propertyValues, propertyFlags
    )
}
