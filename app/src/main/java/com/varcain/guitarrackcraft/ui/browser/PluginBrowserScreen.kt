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

package com.varcain.guitarrackcraft.ui.browser

import android.graphics.BitmapFactory
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.expandVertically
import androidx.compose.animation.shrinkVertically
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.ArrowBack
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.KeyboardArrowDown
import androidx.compose.material.icons.filled.KeyboardArrowRight
import androidx.compose.material.icons.filled.MoreVert
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Star
import androidx.compose.material.icons.outlined.Star
import androidx.compose.material3.*
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.asImageBitmap
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.zIndex
import androidx.activity.compose.BackHandler
import androidx.lifecycle.viewmodel.compose.viewModel
import com.varcain.guitarrackcraft.BuildConfig
import com.varcain.guitarrackcraft.engine.Lv2BundleEntry
import com.varcain.guitarrackcraft.engine.PluginInfo
import com.varcain.guitarrackcraft.engine.UiType
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@OptIn(ExperimentalMaterial3Api::class)
@Composable
fun PluginBrowserScreen(
    replaceIndex: Int = -1,
    onNavigateBack: () -> Unit,
    viewModel: PluginBrowserViewModel = viewModel()
) {
    val groupedPlugins by viewModel.groupedPlugins.collectAsState()
    val expandedAuthors by viewModel.expandedAuthors.collectAsState()
    val expandedCategories by viewModel.expandedCategories.collectAsState()
    val isLoading by viewModel.isLoading.collectAsState()
    val errorMessage by viewModel.errorMessage.collectAsState()
    val addFailureMessage by viewModel.addFailureMessage.collectAsState()
    val favorites by viewModel.favorites.collectAsState()
    val blockingOperation by viewModel.blockingOperation.collectAsState()
    val importState by viewModel.importState.collectAsState()
    val importedBundles by viewModel.importedBundles.collectAsState()

    val importEnabled = BuildConfig.LV2_IMPORT_ENABLED
    val importRunning = importState is Lv2ImportState.Running
    var importMenuOpen by remember { mutableStateOf(false) }
    var showManageImported by remember { mutableStateOf(false) }

    val zipPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? ->
        if (uri != null) viewModel.importZip(uri)
    }
    val folderPicker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocumentTree()) { uri: Uri? ->
        if (uri != null) viewModel.importTree(uri)
    }

    // Scope for kicking off add/replace plugin operations off the main thread.
    // VST activate (wine fork + guest_ready wait) blocks for several seconds.
    val addPluginScope = rememberCoroutineScope()

    val snackbarHostState = remember { SnackbarHostState() }

    // Reload plugins every time the screen is displayed (e.g. engine may have
    // been initialized after permission grant, or user returned to this screen)
    LaunchedEffect(Unit) {
        viewModel.refresh()
    }

    // Show Snackbar when add-to-rack fails (plugin binary missing, etc.)
    LaunchedEffect(addFailureMessage) {
        addFailureMessage?.let { msg ->
            snackbarHostState.showSnackbar(
                message = msg,
                duration = SnackbarDuration.Long
            )
            viewModel.clearAddFailureMessage()
        }
    }

    // Import results are reported where the user is looking: the menu closes and
    // the outcome arrives as a Snackbar.
    LaunchedEffect(importState) {
        when (val state = importState) {
            is Lv2ImportState.Success -> {
                snackbarHostState.showSnackbar(message = state.message, duration = SnackbarDuration.Long)
                viewModel.consumeImportResult()
            }
            is Lv2ImportState.Error -> {
                snackbarHostState.showSnackbar(message = state.message, duration = SnackbarDuration.Long)
                viewModel.consumeImportResult()
            }
            is Lv2ImportState.Running, Lv2ImportState.Idle -> Unit
        }
    }

    BackHandler(enabled = blockingOperation != null) { }

    Box(modifier = Modifier.fillMaxSize()) {
        Scaffold(
            snackbarHost = { SnackbarHost(snackbarHostState) },
            topBar = {
                TopAppBar(
                    title = { Text("Plugin Browser") },
                    navigationIcon = {
                        IconButton(onClick = onNavigateBack) {
                            Icon(Icons.Default.ArrowBack, contentDescription = "Back")
                        }
                    },
                    actions = {
                        IconButton(onClick = { viewModel.refresh() }, enabled = !importRunning) {
                            Icon(Icons.Default.Refresh, contentDescription = "Refresh")
                        }
                        if (importEnabled) {
                            Box {
                                IconButton(
                                    onClick = { importMenuOpen = true },
                                    enabled = !importRunning
                                ) {
                                    Icon(Icons.Default.MoreVert, contentDescription = "More options")
                                }
                                DropdownMenu(
                                    expanded = importMenuOpen,
                                    onDismissRequest = { importMenuOpen = false }
                                ) {
                                    DropdownMenuItem(
                                        text = { Text("Import LV2 (ZIP)") },
                                        onClick = {
                                            importMenuOpen = false
                                            zipPicker.launch(arrayOf("*/*"))
                                        }
                                    )
                                    DropdownMenuItem(
                                        text = { Text("Import LV2 (folder)") },
                                        onClick = {
                                            importMenuOpen = false
                                            folderPicker.launch(null)
                                        }
                                    )
                                    DropdownMenuItem(
                                        text = { Text("Manage imported (${importedBundles.size})") },
                                        onClick = {
                                            importMenuOpen = false
                                            showManageImported = true
                                        }
                                    )
                                }
                            }
                        }
                    }
                )
            }
        ) { padding ->
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(padding)
            ) {
                when {
                    isLoading -> {
                        CircularProgressIndicator(
                            modifier = Modifier.align(Alignment.Center)
                        )
                    }
                    errorMessage != null -> {
                        Column(
                            modifier = Modifier
                                .fillMaxSize()
                                .padding(16.dp),
                            horizontalAlignment = Alignment.CenterHorizontally,
                            verticalArrangement = Arrangement.Center
                        ) {
                            Text(
                                text = errorMessage ?: "Unknown error",
                                color = MaterialTheme.colorScheme.error
                            )
                            Spacer(modifier = Modifier.height(16.dp))
                            Button(onClick = { viewModel.refresh() }) {
                                Text("Retry")
                            }
                        }
                    }
                    groupedPlugins.isEmpty() -> {
                        Column(
                            modifier = Modifier
                                .fillMaxSize()
                                .padding(16.dp),
                            horizontalAlignment = Alignment.CenterHorizontally,
                            verticalArrangement = Arrangement.Center
                        ) {
                            Text("No plugins available")
                            if (importEnabled) {
                                Spacer(modifier = Modifier.height(8.dp))
                                Text(
                                    text = "You can import your own LV2 bundles with the menu in the top bar.",
                                    style = MaterialTheme.typography.bodySmall
                                )
                            }
                            Spacer(modifier = Modifier.height(8.dp))
                            Text(
                                text = "LV2 plugins are loaded from the app's extracted assets (assets/lv2). This app ships with GxPlugins in assets—if you see nothing here, the native build may be using the LV2 stub (no lilv/serd/sord). Build the LV2 libraries and place them in app/src/main/cpp/libs/lv2/, then rebuild the app. See LV2_INTEGRATION.md.",
                                style = MaterialTheme.typography.bodySmall
                            )
                        }
                    }
                    else -> {
                        LazyColumn(
                            modifier = Modifier.fillMaxSize(),
                            contentPadding = PaddingValues(16.dp),
                            verticalArrangement = Arrangement.spacedBy(8.dp)
                        ) {
                            groupedPlugins.forEach { authorGroup ->
                                // Author header
                                item(key = "author_${authorGroup.name}") {
                                    AuthorHeader(
                                        authorName = authorGroup.name,
                                        pluginCount = authorGroup.categories.sumOf { it.plugins.size },
                                        isExpanded = expandedAuthors.contains(authorGroup.name),
                                        onToggle = { viewModel.toggleAuthor(authorGroup.name) }
                                    )
                                }

                                // Categories (only show if author is expanded)
                                if (expandedAuthors.contains(authorGroup.name)) {
                                    authorGroup.categories.forEach { category ->
                                        val categoryKey = "${authorGroup.name}|${category.name}"
                                        val isCategoryExpanded = expandedCategories.contains(categoryKey)

                                        // Category header
                                        item(key = "category_${categoryKey}") {
                                            CategoryHeader(
                                                categoryName = category.name,
                                                pluginCount = category.plugins.size,
                                                isExpanded = isCategoryExpanded,
                                                onToggle = {
                                                    viewModel.toggleCategory(authorGroup.name, category.name)
                                                }
                                            )
                                        }

                                        // Plugins in category (only show if category is expanded)
                                        if (isCategoryExpanded) {
                                            items(
                                                items = category.plugins,
                                                key = { "${authorGroup.name}_${category.name}_${it.fullId}" }
                                            ) { plugin ->
                                                PluginItem(
                                                    plugin = plugin,
                                                    isFavorite = favorites.contains(plugin.fullId),
                                                    onToggleFavorite = { viewModel.toggleFavorite(plugin.fullId) },
                                                    onClick = {
                                                        if (blockingOperation == null) {
                                                            // VST activate can take ~5s (wine fork + FEX startup);
                                                            // doing it on the main thread triggers Android's
                                                            // input-dispatch ANR watchdog. Launch on the screen's
                                                            // coroutine scope; the view model dispatches to IO.
                                                            addPluginScope.launch {
                                                                val success = if (replaceIndex >= 0) {
                                                                    viewModel.replacePluginInRack(replaceIndex, plugin)
                                                                } else {
                                                                    viewModel.addPluginToRack(plugin)
                                                                }
                                                                if (success) onNavigateBack()
                                                            }
                                                        }
                                                    }
                                                )
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
        blockingOperation?.let { label ->
            BrowserBlockingOperationOverlay(label = label)
        }
        val runningImport = importState as? Lv2ImportState.Running
        if (blockingOperation == null && runningImport != null) {
            BrowserBlockingOperationOverlay(label = runningImport.label)
        }
    }

    if (importEnabled && showManageImported) {
        ManageImportedDialog(
            entries = importedBundles,
            onDelete = { viewModel.removeImported(it) },
            onDismiss = { showManageImported = false }
        )
    }
}

/**
 * "Manage imported" — the user-imported LV2 bundles currently in `filesDir/lv2_user`.
 * Deleting only removes the files and tombstones the entry; the native world keeps
 * the plugin until the engine restarts (lilv cannot unload a bundle in place).
 */
@Composable
private fun ManageImportedDialog(
    entries: List<Lv2BundleEntry>,
    onDelete: (String) -> Unit,
    onDismiss: () -> Unit
) {
    var pendingDelete by remember { mutableStateOf<Lv2BundleEntry?>(null) }

    AlertDialog(
        onDismissRequest = onDismiss,
        title = { Text("Imported LV2 bundles") },
        text = {
            if (entries.isEmpty()) {
                Text("Nothing imported yet. Use Import LV2 (ZIP) or Import LV2 (folder).")
            } else {
                LazyColumn(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    items(entries, key = { it.id }) { entry ->
                        Row(
                            modifier = Modifier.fillMaxWidth(),
                            verticalAlignment = Alignment.CenterVertically
                        ) {
                            Column(modifier = Modifier.weight(1f)) {
                                Text(
                                    text = entry.displayName.ifEmpty { entry.id },
                                    style = MaterialTheme.typography.bodyLarge
                                )
                                Text(
                                    text = "${entry.uris.size} plugin(s)",
                                    style = MaterialTheme.typography.bodySmall,
                                    color = MaterialTheme.colorScheme.onSurfaceVariant
                                )
                            }
                            IconButton(onClick = { pendingDelete = entry }) {
                                Icon(Icons.Default.Delete, contentDescription = "Delete ${entry.displayName}")
                            }
                        }
                    }
                }
            }
        },
        confirmButton = {
            TextButton(onClick = onDismiss) { Text("Close") }
        }
    )

    pendingDelete?.let { entry ->
        AlertDialog(
            onDismissRequest = { pendingDelete = null },
            title = { Text("Delete ${entry.displayName.ifEmpty { entry.id }}?") },
            text = {
                Text(
                    "The bundle's files are deleted now and its plugins disappear from this " +
                        "list. The native plugin world only drops them when the engine restarts."
                )
            },
            confirmButton = {
                TextButton(onClick = {
                    pendingDelete = null
                    onDelete(entry.id)
                }) { Text("Delete") }
            },
            dismissButton = {
                TextButton(onClick = { pendingDelete = null }) { Text("Cancel") }
            }
        )
    }
}

@Composable
private fun BrowserBlockingOperationOverlay(label: String) {
    Box(
        modifier = Modifier
            .fillMaxSize()
            .zIndex(100f)
            .background(MaterialTheme.colorScheme.scrim.copy(alpha = 0.45f))
            .pointerInput(Unit) {
                awaitPointerEventScope {
                    while (true) {
                        val event = awaitPointerEvent()
                        event.changes.forEach { it.consume() }
                    }
                }
            },
        contentAlignment = Alignment.Center
    ) {
        Surface(
            tonalElevation = 8.dp,
            shadowElevation = 8.dp,
            shape = RoundedCornerShape(8.dp),
            color = MaterialTheme.colorScheme.surface
        ) {
            Column(
                modifier = Modifier.padding(horizontal = 28.dp, vertical = 22.dp),
                horizontalAlignment = Alignment.CenterHorizontally,
                verticalArrangement = Arrangement.spacedBy(12.dp)
            ) {
                CircularProgressIndicator(strokeWidth = 3.dp)
                Text(
                    text = "$label...",
                    style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurface
                )
            }
        }
    }
}

@Composable
fun AuthorHeader(
    authorName: String,
    pluginCount: Int,
    isExpanded: Boolean,
    onToggle: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .clickable(onClick = onToggle),
        colors = CardDefaults.cardColors(
            containerColor = MaterialTheme.colorScheme.primaryContainer
        ),
        elevation = CardDefaults.cardElevation(defaultElevation = 4.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 16.dp, vertical = 12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(8.dp)
            ) {
                Icon(
                    imageVector = if (isExpanded) Icons.Default.KeyboardArrowDown else Icons.Default.KeyboardArrowRight,
                    contentDescription = if (isExpanded) "Collapse" else "Expand",
                    tint = MaterialTheme.colorScheme.onPrimaryContainer
                )
                Text(
                    text = authorName,
                    style = MaterialTheme.typography.titleLarge,
                    fontWeight = FontWeight.Bold,
                    color = MaterialTheme.colorScheme.onPrimaryContainer
                )
            }
            Text(
                text = "$pluginCount plugins",
                style = MaterialTheme.typography.bodyMedium,
                color = MaterialTheme.colorScheme.onPrimaryContainer.copy(alpha = 0.7f)
            )
        }
    }
}

@Composable
fun CategoryHeader(
    categoryName: String,
    pluginCount: Int,
    isExpanded: Boolean,
    onToggle: () -> Unit
) {
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 16.dp)
            .clickable(onClick = onToggle),
        color = MaterialTheme.colorScheme.secondaryContainer.copy(alpha = 0.5f),
        shape = MaterialTheme.shapes.small
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(horizontal = 12.dp, vertical = 8.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.SpaceBetween
        ) {
            Row(
                verticalAlignment = Alignment.CenterVertically,
                horizontalArrangement = Arrangement.spacedBy(4.dp)
            ) {
                Icon(
                    imageVector = if (isExpanded) Icons.Default.KeyboardArrowDown else Icons.Default.KeyboardArrowRight,
                    contentDescription = if (isExpanded) "Collapse" else "Expand",
                    tint = MaterialTheme.colorScheme.onSecondaryContainer,
                    modifier = Modifier.size(20.dp)
                )
                Text(
                    text = categoryName,
                    style = MaterialTheme.typography.titleSmall,
                    fontWeight = FontWeight.SemiBold,
                    color = MaterialTheme.colorScheme.onSecondaryContainer
                )
            }
            Text(
                text = "$pluginCount",
                style = MaterialTheme.typography.bodySmall,
                color = MaterialTheme.colorScheme.onSecondaryContainer.copy(alpha = 0.7f)
            )
        }
    }
}

/**
 * Decodes a plugin thumbnail. Shipped plugins keep theirs in `assets/lv2/`;
 * an imported bundle is a directory on disk, so the same relative name is tried
 * as an absolute path and under the plugin's modgui base path before giving up.
 */
private fun decodeThumbnail(
    context: android.content.Context,
    thumbnailPath: String,
    modguiBasePath: String
): androidx.compose.ui.graphics.ImageBitmap? {
    if (thumbnailPath.isEmpty()) return null

    runCatching {
        context.assets.open("lv2/$thumbnailPath").use { inputStream ->
            BitmapFactory.decodeStream(inputStream)?.asImageBitmap()
        }
    }.getOrNull()?.let { return it }

    val candidates = buildList {
        if (thumbnailPath.startsWith("/")) add(java.io.File(thumbnailPath))
        if (modguiBasePath.isNotEmpty()) add(java.io.File(modguiBasePath, thumbnailPath))
    }
    for (candidate in candidates) {
        if (!candidate.isFile) continue
        runCatching { BitmapFactory.decodeFile(candidate.absolutePath)?.asImageBitmap() }
            .getOrNull()?.let { return it }
    }
    return null
}

@Composable
fun PluginThumbnail(
    thumbnailPath: String,
    modifier: Modifier = Modifier,
    modguiBasePath: String = ""
) {
    val context = LocalContext.current
    
    val bitmap = produceState<androidx.compose.ui.graphics.ImageBitmap?>(
        initialValue = null,
        thumbnailPath,
        modguiBasePath
    ) {
        value = withContext(Dispatchers.IO) {
            decodeThumbnail(context, thumbnailPath, modguiBasePath)
        }
    }
    
    Box(
        modifier = modifier,
        contentAlignment = Alignment.Center
    ) {
        bitmap.value?.let { imageBitmap ->
            Image(
                bitmap = imageBitmap,
                contentDescription = "Plugin thumbnail",
                modifier = Modifier.fillMaxSize()
            )
        } ?: run {
            // Placeholder when no thumbnail
            Box(
                modifier = Modifier
                    .fillMaxSize()
                    .padding(4.dp),
                contentAlignment = Alignment.Center
            ) {
                Text(
                    text = "🎸",
                    style = MaterialTheme.typography.titleMedium
                )
            }
        }
    }
}

@Composable
fun PluginItem(
    plugin: PluginInfo,
    isFavorite: Boolean = false,
    onToggleFavorite: () -> Unit = {},
    onClick: () -> Unit
) {
    Card(
        modifier = Modifier
            .fillMaxWidth()
            .padding(start = 32.dp)
            .clickable(onClick = onClick)
            .testTag("browser_plugin_item"),
        elevation = CardDefaults.cardElevation(defaultElevation = 2.dp)
    ) {
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
            horizontalArrangement = Arrangement.spacedBy(12.dp)
        ) {
            // Thumbnail
            PluginThumbnail(
                thumbnailPath = plugin.thumbnailPath,
                modguiBasePath = plugin.modguiBasePath,
                modifier = Modifier
                    .size(80.dp)
            )

            // Plugin info
            Column(
                modifier = Modifier.weight(1f)
            ) {
                Text(
                    text = plugin.name.ifEmpty { plugin.id },
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.Bold
                )

                // Description (if available)
                if (plugin.description.isNotEmpty()) {
                    Spacer(modifier = Modifier.height(4.dp))
                    Text(
                        text = plugin.description,
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant
                    )
                }

                Spacer(modifier = Modifier.height(4.dp))
                Row(
                    verticalAlignment = Alignment.CenterVertically,
                    horizontalArrangement = Arrangement.spacedBy(6.dp),
                ) {
                    // Format badge: makes VST2 vs VST3 (or LV2) immediately
                    // distinguishable when two plugins share the same display
                    // name (e.g. AmpCraft.dll + AmpCraft.vst3 imported into
                    // the same library).
                    // Per-format badge colors so VST3 / VST2 / LV2 are tellable at a glance.
                    val (badgeBg, badgeFg) = when (plugin.format) {
                        "VST3" -> Color(0xFF1976D2) to Color.White  // blue
                        "VST2" -> Color(0xFFE64A19) to Color.White  // deep orange
                        "LV2"  -> Color(0xFF388E3C) to Color.White  // green
                        else   -> MaterialTheme.colorScheme.secondaryContainer to MaterialTheme.colorScheme.onSecondaryContainer
                    }
                    Surface(
                        shape = androidx.compose.foundation.shape.RoundedCornerShape(4.dp),
                        color = badgeBg,
                    ) {
                        Text(
                            text = plugin.format,
                            style = MaterialTheme.typography.labelSmall,
                            color = badgeFg,
                            modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                        )
                    }
                    // Architecture badge (x86 / x64 / native) — colors distinct from the format badge.
                    if (plugin.arch.isNotEmpty()) {
                        val (archBg, archFg) = when (plugin.arch) {
                            "x64"    -> Color(0xFF7B1FA2) to Color.White  // purple
                            "x86"    -> Color(0xFF00838F) to Color.White  // teal
                            "native" -> Color(0xFF546E7A) to Color.White  // blue-grey
                            else     -> MaterialTheme.colorScheme.secondaryContainer to MaterialTheme.colorScheme.onSecondaryContainer
                        }
                        Surface(
                            shape = androidx.compose.foundation.shape.RoundedCornerShape(4.dp),
                            color = archBg,
                        ) {
                            Text(
                                text = plugin.arch,
                                style = MaterialTheme.typography.labelSmall,
                                color = archFg,
                                modifier = Modifier.padding(horizontal = 6.dp, vertical = 2.dp),
                            )
                        }
                    }
                    Text(
                        text = plugin.guiTypes.joinToString(", ") { it.displayName },
                        style = MaterialTheme.typography.bodySmall,
                        color = MaterialTheme.colorScheme.onSurfaceVariant.copy(alpha = 0.7f)
                    )
                }
            }

            // Favorite star
            IconButton(onClick = onToggleFavorite) {
                Icon(
                    imageVector = if (isFavorite) Icons.Filled.Star else Icons.Outlined.Star,
                    contentDescription = if (isFavorite) "Remove from favorites" else "Add to favorites",
                    tint = if (isFavorite) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.onSurfaceVariant
                )
            }
        }
    }
}
