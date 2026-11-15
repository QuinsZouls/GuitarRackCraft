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

package com.varcain.guitarrackcraft.ui.settings

import androidx.activity.compose.BackHandler
import androidx.compose.foundation.layout.*
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.verticalScroll
import androidx.compose.ui.Alignment
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontWeight
import com.varcain.guitarrackcraft.engine.AudioDeviceOption
import com.varcain.guitarrackcraft.engine.AudioEngine
import com.varcain.guitarrackcraft.engine.AudioSettingsManager
import com.varcain.guitarrackcraft.ui.orientation.OrientationManager
import com.varcain.guitarrackcraft.ui.orientation.OrientationMode
import com.varcain.guitarrackcraft.ui.orientation.UiSettingsManager
import com.varcain.guitarrackcraft.ui.rack.RackViewModel
import kotlin.math.roundToInt

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun AudioSettingsScreen(
    viewModel: RackViewModel,
    onNavigateBack: () -> Unit
) {
    val context = LocalContext.current

    val inputDevices = remember { AudioSettingsManager.getInputDevices(context) }
    val outputDevices = remember { AudioSettingsManager.getOutputDevices(context) }

    var selectedInputId by remember { mutableIntStateOf(AudioSettingsManager.getInputDeviceId(context)) }
    var selectedOutputId by remember { mutableIntStateOf(AudioSettingsManager.getOutputDeviceId(context)) }
    var selectedBufferSize by remember { mutableIntStateOf(AudioSettingsManager.getBufferSize(context)) }
    var refreshKey by remember { mutableIntStateOf(0) }

    var inputGainDb by remember { mutableFloatStateOf(AudioSettingsManager.getInputGainDb(context)) }
    var outputGainDb by remember { mutableFloatStateOf(AudioSettingsManager.getOutputGainDb(context)) }
    var orientationMode by remember { mutableStateOf(UiSettingsManager.getOrientationMode(context)) }

    BackHandler { onNavigateBack() }

    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text("Audio Settings") },
                navigationIcon = {
                    IconButton(onClick = { onNavigateBack() }) {
                        Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                    }
                }
            )
        }
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .padding(horizontal = 16.dp, vertical = 8.dp)
                .verticalScroll(rememberScrollState()),
            verticalArrangement = Arrangement.spacedBy(20.dp)
        ) {
            // Input device selector
            DeviceDropdown(
                label = "Input Device",
                devices = inputDevices,
                selectedId = selectedInputId,
                onSelected = { id ->
                    selectedInputId = id
                    AudioSettingsManager.setInputDeviceId(context, id)
                    viewModel.restartEngine(context)
                    refreshKey++
                }
            )

            // Output device selector
            DeviceDropdown(
                label = "Output Device",
                devices = outputDevices,
                selectedId = selectedOutputId,
                onSelected = { id ->
                    selectedOutputId = id
                    AudioSettingsManager.setOutputDeviceId(context, id)
                    viewModel.restartEngine(context)
                    refreshKey++
                }
            )

            // Buffer size selector
            BufferSizeDropdown(
                selectedSize = selectedBufferSize,
                onSelected = { size ->
                    selectedBufferSize = size
                    AudioSettingsManager.setBufferSize(context, size)
                    viewModel.restartEngine(context)
                    refreshKey++
                }
            )

            Text(
                text = "Lower buffer sizes reduce latency but increase CPU usage. Use Auto unless you experience issues.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            // Gain changes are applied live by the view model; never restart the engine here
            Divider()
            Text(
                text = "Global Gains",
                style = MaterialTheme.typography.labelLarge,
                modifier = Modifier.padding(bottom = 4.dp)
            )
            Text(
                text = "Trims applied to the whole chain. Input Gain sits right after the input device " +
                    "and before the rack, so it also trims WAV playback; Output Gain right after the rack " +
                    "and before the output device. " +
                    "Input VU/clip respond to the input trim, output VU/clip and recordings to both. " +
                    "0 dB = unity (no change). Adjustments are applied live, so the engine keeps running.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            GlobalGainSetting(
                label = "Input Gain",
                valueDb = inputGainDb,
                onValueChange = { db ->
                    inputGainDb = db
                    viewModel.setGlobalInputGain(context, db)
                }
            )

            GlobalGainSetting(
                label = "Output Gain",
                valueDb = outputGainDb,
                onValueChange = { db ->
                    outputGainDb = db
                    viewModel.setGlobalOutputGain(context, db)
                }
            )

            // Orientation is applied by the orientation manager itself; never restart the engine here
            Divider()
            Text(
                text = "Display",
                style = MaterialTheme.typography.labelLarge,
                modifier = Modifier.padding(bottom = 4.dp)
            )
            LabeledDropdown(
                label = "Screen Orientation",
                selectedLabel = orientationMode.label,
                options = OrientationMode.entries.map { it.label to it },
                onSelected = { mode ->
                    orientationMode = mode
                    OrientationManager.setMode(context, mode)
                }
            )
            Text(
                text = "Auto follows the device sensor or the system rotation lock. Portrait and " +
                    "Landscape lock the app to that orientation. The rack, plugin editors and meters " +
                    "adapt to both orientations; a fullscreen plugin editor still takes over while it " +
                    "is open and restores your choice on exit.",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )

            // Current engine info + low-latency checklist
            val isRunning by viewModel.isEngineRunning.collectAsState()
            if (isRunning) {
                val sampleRate = remember(refreshKey) { AudioEngine.getSampleRate() }
                val bufferFrames = remember(refreshKey) { AudioEngine.getBufferFrameCount() }
                val streamInfo = remember(refreshKey) { AudioEngine.getStreamInfo() }
                val latency = remember(refreshKey) { AudioEngine.getLatencyDetails() }

                Divider()
                Text(
                    text = "Current Session",
                    style = MaterialTheme.typography.labelLarge,
                    modifier = Modifier.padding(bottom = 4.dp)
                )
                InfoRow("Sample Rate", "%.0f Hz".format(sampleRate))
                InfoRow("Callback Size", "$bufferFrames frames")
                InfoRow(
                    "Buffer In Use",
                    "${streamInfo.outputBufferSizeInFrames} of ${streamInfo.outputBufferCapacity} frames"
                )
                InfoRow(
                    "Burst Size",
                    "in ${streamInfo.inputFramesPerBurst} / out ${streamInfo.framesPerBurst} frames"
                )
                InfoRow(
                    "Input Format",
                    "${streamInfo.inputFormatLabel} · ${streamInfo.inputChannels} ch · " +
                        streamInfo.inputPresetLabel
                )
                InfoRow(
                    "Output Format",
                    "${streamInfo.outputFormatLabel} · ${streamInfo.outputChannels} ch"
                )
                InfoRow("Latency input", "%.1f ms".format(latency.inputMs))
                InfoRow("Latency output", "%.1f ms".format(latency.outputMs))
                InfoRow("Round-trip", "%.1f ms".format(latency.roundTripMs))

                Spacer(modifier = Modifier.height(4.dp))
                Divider()
                Text(
                    text = "Low-Latency Checklist",
                    style = MaterialTheme.typography.labelLarge,
                    modifier = Modifier.padding(bottom = 4.dp)
                )
                Text(
                    text = "Based on developer.android.com/games/sdk/oboe/low-latency-audio",
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                    modifier = Modifier.padding(bottom = 8.dp)
                )

                ChecklistItem("Oboe API", true)
                ChecklistItem("AAudio backend", streamInfo.isAAudio, "OpenSL ES")
                ChecklistItem("Performance: Low Latency", streamInfo.outputLowLatency)
                ChecklistItem("Sharing: Exclusive (output)", streamInfo.outputExclusive, "Shared")
                ChecklistItem("Sharing: Exclusive (input)", streamInfo.inputExclusive, "Shared")
                ChecklistItem("Sample rate: 48000 Hz", sampleRate.toInt() == 48000, "%.0f Hz".format(sampleRate))
                ChecklistItem("Data callback", streamInfo.outputCallback)
                ChecklistItem("MMAP buffer (output)", streamInfo.outputMMap)
                ChecklistItem("MMAP buffer (input)", streamInfo.inputMMap)
                ChecklistItem(
                    "Timestamp latency (output)",
                    streamInfo.outputTimestampSupported,
                    "queued-frames estimate"
                )

                if (streamInfo.usesLegacyPath) {
                    Spacer(modifier = Modifier.height(8.dp))
                    Text(
                        text = legacyPathHint,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }
            }
        }
    }
}

/**
 * Shown when the HAL did not grant Exclusive + MMAP on both streams. Those are
 * the two properties that make the difference between a ~10 ms and a ~40 ms
 * round trip, and they are lost to other apps holding the device or to
 * system-wide audio effects.
 */
private const val legacyPathHint =
    "The HAL did not grant the lowest-latency path on both streams. Close other audio " +
        "apps, disable touch sounds / Dolby Atmos, then restart the engine."

@Composable
private fun GlobalGainSetting(
    label: String,
    valueDb: Float,
    onValueChange: (Float) -> Unit
) {
    Column(modifier = Modifier.fillMaxWidth()) {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically
        ) {
            Text(
                text = label,
                style = MaterialTheme.typography.bodyMedium,
                modifier = Modifier.weight(1f)
            )
            Text(
                text = formatGainDb(valueDb),
                style = MaterialTheme.typography.bodyMedium,
                fontWeight = FontWeight.SemiBold
            )
            TextButton(
                onClick = { onValueChange(AudioSettingsManager.DEFAULT_GAIN_DB) },
                enabled = valueDb != AudioSettingsManager.DEFAULT_GAIN_DB,
                contentPadding = PaddingValues(horizontal = 8.dp, vertical = 4.dp)
            ) {
                Text(text = "Reset", style = MaterialTheme.typography.bodySmall)
            }
        }
        Slider(
            value = valueDb,
            onValueChange = { onValueChange(stepGainDb(it)) },
            valueRange = AudioSettingsManager.MIN_GAIN_DB..AudioSettingsManager.MAX_GAIN_DB,
            modifier = Modifier.fillMaxWidth()
        )
        Row(
            modifier = Modifier.fillMaxWidth(),
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            GainScaleLabel("%.0f dB".format(AudioSettingsManager.MIN_GAIN_DB))
            GainScaleLabel(formatGainDb(AudioSettingsManager.DEFAULT_GAIN_DB))
            GainScaleLabel("%.0f dB".format(AudioSettingsManager.MAX_GAIN_DB))
        }
    }
}

@Composable
private fun GainScaleLabel(text: String) {
    Text(
        text = text,
        style = MaterialTheme.typography.bodySmall,
        color = MaterialTheme.colorScheme.onSurfaceVariant
    )
}

private fun formatGainDb(db: Float): String = "%.1f dB".format(db)

private fun stepGainDb(db: Float): Float =
    (AudioSettingsManager.clampGainDb(db) * 10f).roundToInt() / 10f

@Composable
private fun InfoRow(label: String, value: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        horizontalArrangement = Arrangement.SpaceBetween
    ) {
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant
        )
        Text(
            text = value,
            style = MaterialTheme.typography.bodyMedium
        )
    }
}

@Composable
private fun ChecklistItem(label: String, enabled: Boolean, disabledDetail: String? = null) {
    val green = Color(0xFF4CAF50)
    val red = Color(0xFFF44336)
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 3.dp),
        verticalAlignment = Alignment.CenterVertically
    ) {
        Text(
            text = if (enabled) "\u2713" else "\u2717",
            color = if (enabled) green else red,
            fontWeight = FontWeight.Bold,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.width(24.dp)
        )
        Text(
            text = label,
            style = MaterialTheme.typography.bodyMedium,
            modifier = Modifier.weight(1f)
        )
        if (!enabled && disabledDetail != null) {
            Text(
                text = disabledDetail,
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant
            )
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun DeviceDropdown(
    label: String,
    devices: List<AudioDeviceOption>,
    selectedId: Int,
    onSelected: (Int) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    val selectedDevice = devices.find { it.id == selectedId } ?: devices.firstOrNull()

    Column {
        Text(
            text = label,
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.padding(bottom = 4.dp)
        )
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { expanded = it }
        ) {
            OutlinedTextField(
                value = selectedDevice?.name ?: "Default",
                onValueChange = {},
                readOnly = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .menuAnchor(),
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) }
            )
            ExposedDropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                devices.forEach { device ->
                    DropdownMenuItem(
                        text = { Text(device.name) },
                        onClick = {
                            onSelected(device.id)
                            expanded = false
                        }
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun <T> LabeledDropdown(
    label: String,
    selectedLabel: String,
    options: List<Pair<String, T>>,
    onSelected: (T) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }

    Column {
        Text(
            text = label,
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.padding(bottom = 4.dp)
        )
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { expanded = it }
        ) {
            OutlinedTextField(
                value = selectedLabel,
                onValueChange = {},
                readOnly = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .menuAnchor(),
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) }
            )
            ExposedDropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                options.forEach { (optionLabel, value) ->
                    DropdownMenuItem(
                        text = { Text(optionLabel) },
                        onClick = {
                            onSelected(value)
                            expanded = false
                        }
                    )
                }
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun BufferSizeDropdown(
    selectedSize: Int,
    onSelected: (Int) -> Unit
) {
    var expanded by remember { mutableStateOf(false) }
    val options = AudioSettingsManager.BUFFER_SIZE_OPTIONS
    val selectedLabel = options.find { it.first == selectedSize }?.second ?: "Auto"

    Column {
        Text(
            text = "Buffer Size",
            style = MaterialTheme.typography.labelLarge,
            modifier = Modifier.padding(bottom = 4.dp)
        )
        ExposedDropdownMenuBox(
            expanded = expanded,
            onExpandedChange = { expanded = it }
        ) {
            OutlinedTextField(
                value = selectedLabel,
                onValueChange = {},
                readOnly = true,
                modifier = Modifier
                    .fillMaxWidth()
                    .menuAnchor(),
                trailingIcon = { ExposedDropdownMenuDefaults.TrailingIcon(expanded = expanded) }
            )
            ExposedDropdownMenu(
                expanded = expanded,
                onDismissRequest = { expanded = false }
            ) {
                options.forEach { (size, label) ->
                    DropdownMenuItem(
                        text = { Text(label) },
                        onClick = {
                            onSelected(size)
                            expanded = false
                        }
                    )
                }
            }
        }
    }
}
