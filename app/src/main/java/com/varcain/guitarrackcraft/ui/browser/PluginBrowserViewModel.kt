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

import android.app.Application
import android.content.Context
import android.net.Uri
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import com.varcain.guitarrackcraft.BuildConfig
import com.varcain.guitarrackcraft.engine.FavoritesManager
import com.varcain.guitarrackcraft.engine.Lv2BundleAttribution
import com.varcain.guitarrackcraft.engine.Lv2BundleEntry
import com.varcain.guitarrackcraft.engine.Lv2BundleImporter
import com.varcain.guitarrackcraft.engine.Lv2BundleRegistry
import com.varcain.guitarrackcraft.engine.Lv2SafSources
import com.varcain.guitarrackcraft.engine.NativeEngine
import com.varcain.guitarrackcraft.engine.PluginInfo
import com.varcain.guitarrackcraft.engine.RackManager
import com.varcain.guitarrackcraft.engine.ZipImportSource
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONObject
import java.io.File
import java.util.UUID

/**
 * Category information for a plugin.
 */
data class PluginCategory(
    val name: String,
    val plugins: List<PluginInfo>
)

/**
 * Author group containing categories of plugins.
 */
data class AuthorGroup(
    val name: String,
    val categories: List<PluginCategory>
)

/**
 * Mapping of plugin names to their categories based on GxPlugins.lv2 README.
 * Plugins are matched by checking if the plugin name contains the key (case-insensitive).
 */
/**
 * Plugin metadata loaded from assets.
 */
data class PluginMetadata(
    val descriptions: Map<String, String>,
    val thumbnails: Map<String, String>,
    val authors: Map<String, String>,
    val categories: Map<String, String>,
    /** displayName (lowercased) -> is64Bit, for imported Windows VSTs from the registry. */
    val archByName: Map<String, Boolean> = emptyMap()
)

object PluginCategoryMapping {

    const val AUTHOR_GXPLUGINS = "GxPlugins"
    const val AUTHOR_GUITARIX = "Guitarix"
    const val AUTHOR_NEURAL_AMP = "Neural Amp Modeler"
    const val AUTHOR_AIDA_DSP = "Aida DSP"
    const val AUTHOR_BRUMMER10 = "brummer10"
    const val AUTHOR_UNKNOWN = "Unknown"
    const val AUTHOR_WINDOWS_VST = "Windows VST"
    const val AUTHOR_IMPORTED_LV2 = "Imported LV2"

    /** Maps LV2 plugin class names to display categories. */
    private val LV2_CLASS_TO_CATEGORY = mapOf(
        "DistortionPlugin" to "Distortion",
        "AmplifierPlugin" to "Amplifier",
        "SimulatorPlugin" to "Simulator",
        "DelayPlugin" to "Delay",
        "ModulatorPlugin" to "Modulator",
        "FilterPlugin" to "Filter",
        "ReverbPlugin" to "Reverb",
        "EQPlugin" to "EQ",
        "CompressorPlugin" to "Compressor",
        "PitchPlugin" to "Pitch",
        "ChorusPlugin" to "Modulator",
        "FlangerPlugin" to "Modulator",
        "PhaserPlugin" to "Modulator",
        "EnvelopePlugin" to "Dynamics",
        "GatePlugin" to "Dynamics",
        "ExpanderPlugin" to "Dynamics",
        "UtilityPlugin" to "Utility",
        "AnalyserPlugin" to "Utility"
    )

    /**
     * Determines the category for a plugin using metadata LV2 class.
     * Falls back to "Other" if no class is found.
     */
    fun getCategory(pluginName: String, metadataCategories: Map<String, String> = emptyMap()): String {
        val normalizedName = pluginName.lowercase()
        val lv2Class = metadataCategories.entries
            .find { it.key.lowercase() == normalizedName }
            ?.value
        if (lv2Class != null) {
            return LV2_CLASS_TO_CATEGORY[lv2Class] ?: "Other"
        }
        return "Other"
    }
    
    /**
     * Determines the author for a plugin.
     * Currently identifies GxPlugins based on name prefix.
     */
    fun getAuthor(pluginName: String, format: String = "", metadataAuthors: Map<String, String> = emptyMap()): String {
        // Windows VST plugins (VST2/VST3) group under their own "Windows VST" header,
        // regardless of vendor metadata, instead of scattering into Unknown / per-vendor groups.
        if (format == "VST2" || format == "VST3") return AUTHOR_WINDOWS_VST
        // Check metadata authors map first (case-insensitive)
        val normalizedName = pluginName.lowercase()
        metadataAuthors.entries.find { it.key.lowercase() == normalizedName }?.let {
            return it.value
        }
        return if (pluginName.startsWith("Gx", ignoreCase = true)) {
            AUTHOR_GXPLUGINS
        } else {
            AUTHOR_UNKNOWN
        }
    }
}

/**
 * Progress/outcome of an LV2 bundle import, shown as Snackbar + disabled menu in
 * the browser.
 */
sealed interface Lv2ImportState {
    data object Idle : Lv2ImportState
    data class Running(val label: String) : Lv2ImportState
    data class Success(val message: String) : Lv2ImportState
    data class Error(val message: String) : Lv2ImportState
}

class PluginBrowserViewModel(application: Application) : AndroidViewModel(application) {

    companion object {
        const val FAVORITES_GROUP = "Favorites"
    }

    private val appContext: Context = application.applicationContext
    
    private val _plugins = MutableStateFlow<List<PluginInfo>>(emptyList())
    val plugins: StateFlow<List<PluginInfo>> = _plugins.asStateFlow()
    
    private val _groupedPlugins = MutableStateFlow<List<AuthorGroup>>(emptyList())
    val groupedPlugins: StateFlow<List<AuthorGroup>> = _groupedPlugins.asStateFlow()
    
    private val _isLoading = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading.asStateFlow()
    
    private val _errorMessage = MutableStateFlow<String?>(null)
    val errorMessage: StateFlow<String?> = _errorMessage.asStateFlow()

    /** Shown as Snackbar when add-to-rack fails (e.g. plugin binary missing). */
    private val _addFailureMessage = MutableStateFlow<String?>(null)
    val addFailureMessage: StateFlow<String?> = _addFailureMessage.asStateFlow()

    private val _blockingOperation = MutableStateFlow<String?>(null)
    val blockingOperation: StateFlow<String?> = _blockingOperation.asStateFlow()
    
    private val _favorites = MutableStateFlow<Set<String>>(emptySet())
    val favorites: StateFlow<Set<String>> = _favorites.asStateFlow()

    /** Tracks which authors are expanded. Initially only Favorites is expanded. */
    private val _expandedAuthors = MutableStateFlow<Set<String>>(setOf(FAVORITES_GROUP))
    val expandedAuthors: StateFlow<Set<String>> = _expandedAuthors.asStateFlow()
    
    /** Tracks which categories are expanded (key is "Author|Category") */
    private val _expandedCategories = MutableStateFlow<Set<String>>(emptySet())
    val expandedCategories: StateFlow<Set<String>> = _expandedCategories.asStateFlow()
    
    /** Plugin metadata loaded from assets */
    private var pluginMetadata: PluginMetadata? = null
    
    /** Set of plugin names that have working binaries */
    private var availablePlugins: Set<String> = emptySet()

    /** Plugin URIs contributed by user-imported bundles; grouped under
     *  [PluginCategoryMapping.AUTHOR_IMPORTED_LV2]. */
    private var importedUris: Set<String> = emptySet()

    /** Plugin URIs whose imported bundle was deleted. The native world keeps
     *  reporting them until the process restarts, so they are filtered here. */
    private var removedUris: Set<String> = emptySet()

    private val _importState = MutableStateFlow<Lv2ImportState>(Lv2ImportState.Idle)
    val importState: StateFlow<Lv2ImportState> = _importState.asStateFlow()

    private val _importedBundles = MutableStateFlow<List<Lv2BundleEntry>>(emptyList())
    val importedBundles: StateFlow<List<Lv2BundleEntry>> = _importedBundles.asStateFlow()

    private val filesDir: File get() = appContext.filesDir

    private val importEnabled: Boolean get() = BuildConfig.LV2_IMPORT_ENABLED

    init {
        _favorites.value = FavoritesManager.getFavorites(appContext)
        viewModelScope.launch {
            withContext(Dispatchers.IO) { loadMetadata() }
            loadPluginsInternal()
        }
    }
    
    /**
     * Load plugin metadata (descriptions and thumbnails) from JSON file.
     */
    private fun loadMetadata() {
        try {
            appContext.assets.open("plugin_metadata.json").use { inputStream ->
                val jsonString = inputStream.bufferedReader().use { it.readText() }
                val json = JSONObject(jsonString)
                
                val descriptions = mutableMapOf<String, String>()
                val thumbnails = mutableMapOf<String, String>()
                val available = mutableSetOf<String>()
                
                json.optJSONObject("descriptions")?.let { descObj ->
                    descObj.keys().forEach { key ->
                        descriptions[key] = descObj.optString(key, "")
                    }
                }
                
                json.optJSONObject("thumbnails")?.let { thumbObj ->
                    thumbObj.keys().forEach { key ->
                        thumbnails[key] = thumbObj.optString(key, "")
                    }
                }
                
                json.optJSONArray("availablePlugins")?.let { availableArr ->
                    for (i in 0 until availableArr.length()) {
                        available.add(availableArr.optString(i, ""))
                    }
                }
                
                val authors = mutableMapOf<String, String>()
                json.optJSONObject("authors")?.let { authObj ->
                    authObj.keys().forEach { key ->
                        authors[key] = authObj.optString(key, "")
                    }
                }

                val categories = mutableMapOf<String, String>()
                json.optJSONObject("categories")?.let { catObj ->
                    catObj.keys().forEach { key ->
                        categories[key] = catObj.optString(key, "")
                    }
                }

                // Runtime overlay: imported VSTs (full flavor) live in
                // filesDir/vst_plugins/registry.json and aren't in the static
                // plugin_metadata.json. Extract each one's architecture (is64Bit ->
                // x86/x64) for the arch badge; the author group is handled by
                // getAuthor() returning "Windows VST" for VST formats. DO NOT append to
                // `available` — the native engine already enumerates VSTs via the
                // registered VstFactory, and adding to `available` flips the filter from
                // "show all" to "show only listed", which hides every LV2 plugin.
                val vstArch = mutableMapOf<String, Boolean>()
                runCatching {
                    val regFile = java.io.File(appContext.filesDir, "vst_plugins/registry.json")
                    if (regFile.exists()) {
                        val arr = JSONObject(regFile.readText()).optJSONArray("plugins")
                        if (arr != null) {
                            for (i in 0 until arr.length()) {
                                val obj = arr.optJSONObject(i) ?: continue
                                val name = obj.optString("displayName").orEmpty()
                                if (name.isNotEmpty()) vstArch[name.lowercase()] = obj.optBoolean("is64Bit", true)
                            }
                            android.util.Log.i("PluginBrowser", "Loaded arch for ${arr.length()} imported VST(s)")
                        }
                    }
                }

                pluginMetadata = PluginMetadata(descriptions, thumbnails, authors, categories, vstArch)
                availablePlugins = available

                // Runtime overlay: user-imported LV2 bundles (full flavor) are not
                // in the static metadata either. Same rule as for VSTs above — never
                // append them to `available`, that would flip the native list into
                // "show only listed" and hide every shipped LV2 plugin. Their URIs
                // are only used to group them under "Imported LV2" and to hide
                // plugins whose bundle was deleted.
                importedUris = Lv2BundleRegistry.activeUris(filesDir)
                removedUris = Lv2BundleRegistry.removedUris(filesDir)
                _importedBundles.value = Lv2BundleRegistry.activeBundles(filesDir)

                android.util.Log.i(
                    "PluginBrowser",
                    "Loaded ${available.size} available plugins with binaries, " +
                        "${importedUris.size} imported LV2 URI(s)"
                )
            }
        } catch (e: Exception) {
            android.util.Log.w("PluginBrowser", "Failed to load plugin metadata: ${e.message}")
            pluginMetadata = PluginMetadata(emptyMap(), emptyMap(), emptyMap(), emptyMap())
            availablePlugins = emptySet()
            importedUris = emptySet()
            removedUris = emptySet()
        }
    }
    
    /**
     * Apply metadata (description and thumbnail) to a plugin.
     * Uses case-insensitive matching since plugin names can have different casing
     * (e.g., "GxVMK2" vs "GxVmk2").
     */
    private fun applyMetadata(plugin: PluginInfo): PluginInfo {
        val metadata = pluginMetadata ?: return plugin
        
        // Find matching metadata by plugin name (case-insensitive)
        val normalizedPluginName = plugin.name.lowercase()
        
        val description = metadata.descriptions.entries
            .find { it.key.lowercase() == normalizedPluginName }
            ?.value ?: ""
        
        val thumbnailPath = metadata.thumbnails.entries
            .find { it.key.lowercase() == normalizedPluginName }
            ?.value ?: ""
        
        // Architecture for the badge: Windows VSTs are x86/x64 (from the registry's
        // is64Bit), LV2 plugins are native (ARM).
        val arch = when {
            plugin.format == "VST2" || plugin.format == "VST3" ->
                metadata.archByName[normalizedPluginName]?.let { if (it) "x64" else "x86" } ?: ""
            plugin.format == "LV2" -> "native"
            else -> ""
        }

        return plugin.copy(
            description = description,
            thumbnailPath = thumbnailPath,
            arch = arch
        )
    }

    private suspend fun loadPluginsInternal() {
        _isLoading.value = true
        _errorMessage.value = null

        try {
            val allPlugins = RackManager.getAvailablePlugins()

            // Attribute imported plugins to their bundle directory before the
            // list is grouped: an import that happened while the engine was not
            // enumerating leaves its entry without URIs, and those plugins would
            // otherwise never be grouped under "Imported LV2" (nor hidden when
            // the bundle is deleted).
            reconcileImportedUris(allPlugins)

            val availablePluginNamesLower = availablePlugins.map { it.lowercase() }.toSet()

            val filteredPlugins = Lv2BundleAttribution
                .visiblePlugins(allPlugins, availablePluginNamesLower, importedUris, removedUris)
                .map { applyMetadata(it) }

            android.util.Log.i("PluginBrowser", "Showing ${filteredPlugins.size} out of ${allPlugins.size} discovered plugins (metadata filter=${availablePluginNamesLower.size})")

            _plugins.value = filteredPlugins
            _groupedPlugins.value = groupPluginsByAuthorAndCategory(filteredPlugins)
        } catch (e: Exception) {
            _errorMessage.value = "Failed to load plugins: ${e.message}"
        } finally {
            _isLoading.value = false
        }
    }

    /** Re-derive which bundle each imported plugin belongs to and persist it.
     *  Cheap and idempotent: [Lv2BundleRegistry.reconcileUris] only touches the
     *  registry file when the attribution actually changed. */
    private suspend fun reconcileImportedUris(plugins: List<PluginInfo>) {
        val entries = withContext(Dispatchers.IO) {
            Lv2BundleRegistry.reconcileUris(
                filesDir,
                Lv2BundleAttribution.urisByBundle(plugins, userDir()),
            )
        }
        importedUris = Lv2BundleRegistry.activeUris(entries)
        removedUris = Lv2BundleRegistry.removedUris(entries)
        _importedBundles.value = Lv2BundleRegistry.activeBundles(entries)
    }
    
    /**
     * Groups plugins first by author, then by category within each author.
     */
    private fun groupPluginsByAuthorAndCategory(plugins: List<PluginInfo>): List<AuthorGroup> {
        val metadataAuthors = pluginMetadata?.authors ?: emptyMap()
        val metadataCategories = pluginMetadata?.categories ?: emptyMap()
        val byAuthor = plugins.groupBy { plugin ->
            if (isImported(plugin)) {
                PluginCategoryMapping.AUTHOR_IMPORTED_LV2
            } else {
                PluginCategoryMapping.getAuthor(plugin.name, plugin.format, metadataAuthors)
            }
        }

        val authorGroups = byAuthor.map { (author, authorPlugins) ->
            val byCategory = authorPlugins.groupBy { plugin ->
                if (isImported(plugin)) {
                    PluginCategoryMapping.AUTHOR_IMPORTED_LV2
                } else {
                    PluginCategoryMapping.getCategory(plugin.name, metadataCategories)
                }
            }

            val sortedCategories = byCategory.toList()
                .sortedBy { (category, _) ->
                    if (category == "Other") "\uFFFF" else category
                }
                .map { (category, catPlugins) ->
                    PluginCategory(
                        name = category,
                        plugins = catPlugins.sortedBy { it.name }
                    )
                }

            AuthorGroup(
                name = author,
                categories = sortedCategories
            )
        }.sortedBy {
            when (it.name) {
                PluginCategoryMapping.AUTHOR_WINDOWS_VST -> 0
                PluginCategoryMapping.AUTHOR_IMPORTED_LV2 -> 1
                PluginCategoryMapping.AUTHOR_NEURAL_AMP -> 2
                PluginCategoryMapping.AUTHOR_AIDA_DSP -> 3
                PluginCategoryMapping.AUTHOR_GUITARIX -> 4
                PluginCategoryMapping.AUTHOR_BRUMMER10 -> 5
                PluginCategoryMapping.AUTHOR_GXPLUGINS -> 6
                else -> 7
            }
        }

        // Prepend Favorites group if any favorites exist
        val favIds = _favorites.value
        if (favIds.isEmpty()) return authorGroups

        val favPlugins = plugins.filter { favIds.contains(it.fullId) }
        if (favPlugins.isEmpty()) return authorGroups

        val byCategory = favPlugins.groupBy { PluginCategoryMapping.getCategory(it.name, metadataCategories) }
        val favCategories = byCategory.toList()
            .sortedBy { (category, _) ->
                if (category == "Other") "\uFFFF" else category
            }
            .map { (category, catPlugins) ->
                PluginCategory(
                    name = category,
                    plugins = catPlugins.sortedBy { it.name }
                )
            }
        val favGroup = AuthorGroup(name = FAVORITES_GROUP, categories = favCategories)
        return listOf(favGroup) + authorGroups
    }
    
    /**
     * Toggle author expansion state.
     */
    fun toggleAuthor(authorName: String) {
        _expandedAuthors.value = _expandedAuthors.value.toMutableSet().apply {
            if (contains(authorName)) {
                remove(authorName)
            } else {
                add(authorName)
            }
        }
    }
    
    /**
     * Toggle category expansion state.
     */
    fun toggleCategory(authorName: String, categoryName: String) {
        val key = "$authorName|$categoryName"
        _expandedCategories.value = _expandedCategories.value.toMutableSet().apply {
            if (contains(key)) {
                remove(key)
            } else {
                add(key)
            }
        }
    }

    private suspend fun <T> withBlockingOperation(label: String, block: suspend () -> T): T {
        _blockingOperation.value = label
        return try {
            block()
        } finally {
            _blockingOperation.value = null
        }
    }

    suspend fun addPluginToRack(plugin: PluginInfo, position: Int = -1): Boolean {
        if (_blockingOperation.value != null) return false
        return withBlockingOperation("Adding plugin") {
            kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                // Off the main thread because for VST plugins, RackManager.addPlugin
                // chains through WineVstPlugin::activate() which can block for ~5s
                // waiting on guest_ready (wine + FEX + plugin DLL load is slow).
                // Running this on the main thread triggers Android's input-dispatch
                // ANR watchdog and the system SIGKILLs the app.
                android.util.Log.i("PluginBrowser", "[LIFECYCLE] addPluginToRack called: ${plugin.name} (${plugin.fullId})")
                try {
                    val index = RackManager.addPlugin(plugin.fullId, position)
                    android.util.Log.i("PluginBrowser", "[LIFECYCLE] addPluginToRack result: ${plugin.name} -> index=$index")
                    if (index >= 0) {
                        true
                    } else {
                        _addFailureMessage.value = "Could not add plugin. Plugin binaries (.so) are not included in this build—only metadata is available."
                        false
                    }
                } catch (e: Exception) {
                    android.util.Log.e("PluginBrowser", "[LIFECYCLE] addPluginToRack failed: ${plugin.name}", e)
                    _addFailureMessage.value = "Failed to add plugin: ${e.message}"
                    false
                }
            }
        }
    }

    suspend fun replacePluginInRack(position: Int, plugin: PluginInfo): Boolean {
        if (_blockingOperation.value != null) return false
        return withBlockingOperation("Replacing plugin") {
            kotlinx.coroutines.withContext(kotlinx.coroutines.Dispatchers.IO) {
                // Same off-main-thread reason as addPluginToRack — VST replace
                // triggers activate() which blocks on guest_ready.
                android.util.Log.i("PluginBrowser", "[LIFECYCLE] replacePluginInRack called: position=$position, ${plugin.name} (${plugin.fullId})")
                try {
                    RackManager.removePlugin(position)
                    val index = RackManager.addPlugin(plugin.fullId, position)
                    android.util.Log.i("PluginBrowser", "[LIFECYCLE] replacePluginInRack result: ${plugin.name} -> index=$index")
                    if (index >= 0) {
                        true
                    } else {
                        _addFailureMessage.value = "Could not add plugin. Plugin binaries (.so) are not included in this build—only metadata is available."
                        false
                    }
                } catch (e: Exception) {
                    android.util.Log.e("PluginBrowser", "[LIFECYCLE] replacePluginInRack failed: ${plugin.name}", e)
                    _addFailureMessage.value = "Failed to replace plugin: ${e.message}"
                    false
                }
            }
        }
    }

    fun clearAddFailureMessage() {
        _addFailureMessage.value = null
    }

    fun toggleFavorite(pluginId: String) {
        FavoritesManager.toggleFavorite(appContext, pluginId)
        _favorites.value = FavoritesManager.getFavorites(appContext)
        // Regroup to add/remove from Favorites section
        _groupedPlugins.value = groupPluginsByAuthorAndCategory(_plugins.value)
    }

    /** True when this plugin came from a user-imported bundle. */
    private fun isImported(plugin: PluginInfo): Boolean =
        plugin.format == "LV2" && importedUris.contains(plugin.id)

    /**
     * Re-scan the native plugin sources, then reload the browser list.
     *
     * The native refresh is what makes a bundle imported by another screen (or a
     * deleted one) show up here; it is cheap when nothing changed, because the
     * LV2 factory only loads bundles it has never seen.
     */
    fun refresh() {
        viewModelScope.launch {
            withContext(Dispatchers.IO) {
                runCatching { NativeEngine.getInstance().nativeRefreshPluginRegistry() }
                loadMetadata()
            }
            loadPluginsInternal()
        }
    }

    /** Import an LV2 bundle from a ZIP archive picked with OpenDocument. */
    fun importZip(uri: Uri) {
        if (!importEnabled) return
        runImport("Importing LV2 bundle") { id ->
            val resolver = appContext.contentResolver
            val name = Lv2SafSources.displayName(resolver, uri)
            if (!Lv2SafSources.looksLikeZip(resolver, uri)) {
                return@runImport Lv2BundleImporter.Result.Failure(
                    "Selected file is not a ZIP archive"
                )
            }
            Lv2SafSources.openZip(resolver, uri).use { stream ->
                Lv2BundleImporter.import(ZipImportSource(stream, name), userDir(), id)
            }
        }
    }

    /** Import an LV2 bundle from a folder picked with OpenDocumentTree. */
    fun importTree(uri: Uri) {
        if (!importEnabled) return
        runImport("Importing LV2 folder") { id ->
            Lv2BundleImporter.import(
                Lv2SafSources.treeSource(appContext.contentResolver, uri),
                userDir(),
                id,
            )
        }
    }

    /** Delete an imported bundle's files and tombstone its entry. */
    fun removeImported(id: String) {
        if (!importEnabled) return
        if (_importState.value is Lv2ImportState.Running) return

        _importState.value = Lv2ImportState.Running("Removing bundle")
        viewModelScope.launch {
            val state = withContext(Dispatchers.IO) {
                runCatching {
                    val entry = Lv2BundleRegistry.read(filesDir).firstOrNull { it.id == id }
                    File(userDir(), id).deleteRecursively()
                    Lv2BundleRegistry.markRemoved(filesDir, id)
                    runCatching { NativeEngine.getInstance().nativeRefreshPluginRegistry() }
                    loadMetadata()
                    loadPluginsInternal()
                    Lv2ImportState.Success(
                        "Removed ${entry?.displayName ?: id} — hidden from the list now, " +
                            "and gone from the engine on the next start"
                    )
                }.getOrElse {
                    Lv2ImportState.Error("Could not remove the bundle: ${it.message}")
                }
            }
            _importState.value = state
        }
    }

    /** Clears a Success/Error banner after the UI has shown it. */
    fun consumeImportResult() {
        if (_importState.value is Lv2ImportState.Success || _importState.value is Lv2ImportState.Error) {
            _importState.value = Lv2ImportState.Idle
        }
    }

    private fun userDir(): File = Lv2BundleRegistry.userDir(filesDir)

    private fun runImport(label: String, block: (id: String) -> Lv2BundleImporter.Result) {
        if (_importState.value is Lv2ImportState.Running) return

        _importState.value = Lv2ImportState.Running(label)
        viewModelScope.launch {
            // Everything, including the failure paths, has to end in a state:
            // leaving Running set would keep the browser's menu disabled.
            val state = runCatching {
                withContext(Dispatchers.IO) {
                    val id = UUID.randomUUID().toString()
                    val before = nativeLv2Uris()
                    val result = runCatching { block(id) }.getOrElse {
                        Lv2BundleImporter.Result.Failure(it.message ?: "Import failed")
                    }
                    when (result) {
                        is Lv2BundleImporter.Result.Failure -> Lv2ImportState.Error(result.reason)
                        is Lv2BundleImporter.Result.Success -> registerImport(id, result, before)
                    }
                }
            }.getOrElse { Lv2ImportState.Error(it.message ?: "Import failed") }

            _importState.value = state
        }
    }

    /** Record the imported bundle, ask the engine to rescan and store the URIs
     *  the rescan attributed to it. */
    private suspend fun registerImport(
        id: String,
        result: Lv2BundleImporter.Result.Success,
        previousUris: Set<String>?,
    ): Lv2ImportState {
        Lv2BundleRegistry.add(
            filesDir,
            Lv2BundleEntry(
                id = id,
                displayName = result.displayName,
                bundleDir = id,
                uris = emptyList(),
                importedAt = System.currentTimeMillis(),
            ),
        )

        runCatching { NativeEngine.getInstance().nativeRefreshPluginRegistry() }
        val after = nativeLv2Uris()
        val enumerated = runCatching { RackManager.getAvailablePlugins() }.getOrNull().orEmpty()

        // Where the engine says the plugin came from beats "what appeared in the
        // rescan": the diff is empty when the engine was not enumerating, and it
        // also picks up bundles another import added in between. The diff stays
        // as a fallback for an engine that cannot report bundle paths.
        val attributed = Lv2BundleAttribution.urisByBundle(enumerated, userDir())[id].orEmpty()
        val uris = when {
            attributed.isNotEmpty() -> attributed.sorted()
            after == null -> emptyList()
            previousUris == null -> after.sorted()
            else -> (after - previousUris).sorted()
        }
        Lv2BundleRegistry.setUris(filesDir, id, uris)

        loadMetadata()
        loadPluginsInternal()

        return when {
            after == null -> Lv2ImportState.Success(
                "Imported ${result.displayName}. Start the audio engine to load it."
            )
            uris.isEmpty() -> Lv2ImportState.Success(
                "Imported ${result.displayName}, but no new plugins were detected — " +
                    "duplicate URI or incompatible bundle"
            )
            else -> Lv2ImportState.Success(
                "Imported ${result.displayName}: ${uris.size} plugin(s) added"
            )
        }
    }

    /** LV2 plugin URIs the engine currently knows, or null when the engine is not
     *  running yet (the JNI call throws before nativeInit). */
    private fun nativeLv2Uris(): Set<String>? = runCatching {
        RackManager.getAvailablePlugins()
            .filter { it.format == "LV2" }
            .map { it.id }
            .toSet()
    }.getOrNull()
}
