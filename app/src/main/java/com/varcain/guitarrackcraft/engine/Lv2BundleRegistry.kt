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

import java.io.File

/**
 * One user-imported LV2 bundle.
 *
 * Every imported bundle lives in its own directory under [Lv2BundleRegistry.userDir],
 * named after [id]; [bundleDir] is that directory relative to the user dir, so the
 * registry keeps working if `filesDir` moves (it does not on Android, but the
 * relative form is what the native scanner sees anyway).
 */
data class Lv2BundleEntry(
    val id: String,
    val displayName: String,
    val bundleDir: String,
    /** LV2 plugin URIs this bundle contributed, as reported by the engine right
     *  after the import. Empty when the bundle turned out to be a duplicate. */
    val uris: List<String> = emptyList(),
    val importedAt: Long = 0L,
    /**
     * Tombstone: the files are gone, but lilv can only drop a plugin from its
     * world by reloading it, so the entry stays here to keep the browser from
     * listing URIs whose bundle no longer exists.
     */
    val removed: Boolean = false,
)

/**
 * Filesystem + JSON I/O for the imported-LV2 registry, mirroring `VstRegistry`
 * (the C++ LV2 factory only needs the directories on disk — it re-scans
 * `filesDir/lv2_user` — the registry is for the UI: names, plugin counts,
 * tombstones).
 *
 * Corrupt or truncated files are treated as "nothing imported" instead of
 * failing the browser: losing the bookkeeping hides the Manage list, it does not
 * break the plugins, which are discovered from the filesystem.
 *
 * Writes go to a staging file and are renamed over `registry.json`, so a process
 * death during an import cannot leave a half-written registry behind (that path
 * would read as "nothing imported" and orphan every bundle already on disk).
 */
object Lv2BundleRegistry {
    const val DIR_NAME = "lv2_user"
    const val REGISTRY_FILE = "registry.json"
    internal const val STAGING_FILE = "$REGISTRY_FILE.tmp"
    private const val ARRAY_KEY = "bundles"

    fun userDir(filesDir: File): File = File(filesDir, DIR_NAME)

    fun registryFile(filesDir: File): File = File(userDir(filesDir), REGISTRY_FILE)

    fun read(filesDir: File): List<Lv2BundleEntry> {
        val file = registryFile(filesDir)
        if (!file.isFile) return emptyList()
        return runCatching { decode(file.readText()) }.getOrElse { emptyList() }
    }

    fun write(filesDir: File, entries: List<Lv2BundleEntry>) {
        val target = registryFile(filesDir)
        val dir = target.parentFile ?: return
        dir.mkdirs()

        val staging = File(dir, STAGING_FILE)
        staging.writeText(encode(entries))
        if (!staging.renameTo(target)) {
            target.writeText(staging.readText())
            staging.delete()
        }
    }

    fun activeBundles(filesDir: File): List<Lv2BundleEntry> = activeBundles(read(filesDir))

    fun activeBundles(entries: List<Lv2BundleEntry>): List<Lv2BundleEntry> = entries.filter { !it.removed }

    /** Plugin URIs the user imported and has not removed. */
    fun activeUris(filesDir: File): Set<String> = activeUris(read(filesDir))

    fun activeUris(entries: List<Lv2BundleEntry>): Set<String> =
        entries.filter { !it.removed }.flatMap { it.uris }.toSet()

    /** Plugin URIs whose bundle was removed; the native world still reports them
     *  until the process restarts, so the browser filters them out. */
    fun removedUris(filesDir: File): Set<String> = removedUris(read(filesDir))

    fun removedUris(entries: List<Lv2BundleEntry>): Set<String> =
        entries.filter { it.removed }.flatMap { it.uris }.toSet()

    /**
     * Fill in the URIs of the active entries from [attributed] (bundle id -> the
     * plugin URIs whose bundle directory the engine reports inside that id's
     * directory), and rewrite `registry.json` only when something changed.
     *
     * Attribution has to be reconciled on every enumeration, not once at import
     * time: an import that happens while the engine is not initialized gets no
     * URIs from the rescan, and without this pass its plugins would never be
     * grouped under "Imported LV2" nor hidden when the bundle is deleted.
     *
     * An entry with no attributed URIs keeps what it already has — an empty
     * attribution means "the engine has not enumerated this bundle", never "the
     * bundle is empty".
     */
    fun reconcileUris(filesDir: File, attributed: Map<String, List<String>>): List<Lv2BundleEntry> {
        val entries = read(filesDir)
        var changed = false
        val updated = entries.map { entry ->
            if (entry.removed) return@map entry
            val found = attributed[entry.id].orEmpty().distinct().sorted()
            if (found.isEmpty() || found == entry.uris.sorted()) {
                entry
            } else {
                changed = true
                entry.copy(uris = found)
            }
        }
        if (changed) write(filesDir, updated)
        return updated
    }

    fun add(filesDir: File, entry: Lv2BundleEntry) {
        write(filesDir, read(filesDir) + entry)
    }

    fun setUris(filesDir: File, id: String, uris: List<String>) {
        write(filesDir, read(filesDir).map { if (it.id == id) it.copy(uris = uris) else it })
    }

    fun markRemoved(filesDir: File, id: String) {
        write(filesDir, read(filesDir).map { if (it.id == id) it.copy(removed = true) else it })
    }

    internal fun encode(entries: List<Lv2BundleEntry>): String = buildString {
        append("{\n  \"").append(ARRAY_KEY).append("\": [\n")
        entries.forEachIndexed { index, entry ->
            append("    {\n")
            append("      \"id\": ").append(quote(entry.id)).append(",\n")
            append("      \"displayName\": ").append(quote(entry.displayName)).append(",\n")
            append("      \"bundleDir\": ").append(quote(entry.bundleDir)).append(",\n")
            append("      \"importedAt\": ").append(entry.importedAt).append(",\n")
            append("      \"removed\": ").append(entry.removed).append(",\n")
            append("      \"uris\": [")
            entry.uris.forEachIndexed { uriIndex, uri ->
                if (uriIndex > 0) append(", ")
                append(quote(uri))
            }
            append("]\n")
            append("    }")
            if (index < entries.lastIndex) append(",")
            append("\n")
        }
        append("  ]\n}\n")
    }

    internal fun decode(body: String): List<Lv2BundleEntry> {
        val root = Lv2RegistryJson(body).parseDocument() as? Map<*, *>
            ?: throw IllegalArgumentException("registry root is not an object")
        val bundles = root[ARRAY_KEY] as? List<*> ?: return emptyList()

        return bundles.mapNotNull { item ->
            val bundle = item as? Map<*, *> ?: return@mapNotNull null
            val id = bundle.string("id")
            if (id.isEmpty()) return@mapNotNull null
            Lv2BundleEntry(
                id = id,
                displayName = bundle.string("displayName"),
                bundleDir = bundle.string("bundleDir"),
                uris = (bundle["uris"] as? List<*>).orEmpty().mapNotNull { it as? String },
                importedAt = (bundle["importedAt"] as? Double)?.toLong() ?: 0L,
                removed = bundle["removed"] as? Boolean ?: false,
            )
        }
    }

    private fun Map<*, *>.string(key: String): String = this[key] as? String ?: ""

    private fun quote(value: String): String {
        val out = StringBuilder("\"")
        for (c in value) {
            when (c) {
                '"' -> out.append("\\\"")
                '\\' -> out.append("\\\\")
                '\n' -> out.append("\\n")
                '\r' -> out.append("\\r")
                '\t' -> out.append("\\t")
                else -> if (c < ' ') out.append("\\u%04x".format(c.code)) else out.append(c)
            }
        }
        return out.append('"').toString()
    }
}

/**
 * Minimal JSON reader for [Lv2BundleRegistry]: objects, arrays, strings, numbers,
 * booleans and null. `org.json` is an Android framework class and this registry
 * is covered by JVM unit tests, so the parsing lives here.
 */
internal class Lv2RegistryJson(private val text: String) {
    private var pos = 0

    fun parseDocument(): Any? {
        val value = parseValue()
        skipWhitespace()
        require(pos == text.length) { "trailing content at $pos" }
        return value
    }

    private fun parseValue(): Any? {
        skipWhitespace()
        require(pos < text.length) { "unexpected end of input" }
        return when (val c = text[pos]) {
            '{' -> parseObject()
            '[' -> parseArray()
            '"' -> parseString()
            't' -> parseLiteral("true", true)
            'f' -> parseLiteral("false", false)
            'n' -> parseLiteral("null", null)
            else -> if (c == '-' || c.isDigit()) parseNumber() else fail("unexpected '$c'")
        }
    }

    private fun parseObject(): Map<String, Any?> {
        val result = LinkedHashMap<String, Any?>()
        pos++
        skipWhitespace()
        if (peek() == '}') {
            pos++
            return result
        }
        while (true) {
            skipWhitespace()
            val key = parseString()
            skipWhitespace()
            expect(':')
            result[key] = parseValue()
            skipWhitespace()
            when (peek()) {
                ',' -> pos++
                '}' -> { pos++; return result }
                else -> fail("expected ',' or '}'")
            }
        }
    }

    private fun parseArray(): List<Any?> {
        val result = ArrayList<Any?>()
        pos++
        skipWhitespace()
        if (peek() == ']') {
            pos++
            return result
        }
        while (true) {
            result.add(parseValue())
            skipWhitespace()
            when (peek()) {
                ',' -> pos++
                ']' -> { pos++; return result }
                else -> fail("expected ',' or ']'")
            }
        }
    }

    private fun parseString(): String {
        expect('"')
        val out = StringBuilder()
        while (true) {
            require(pos < text.length) { "unterminated string" }
            when (val c = text[pos++]) {
                '"' -> return out.toString()
                '\\' -> {
                    require(pos < text.length) { "unterminated escape" }
                    when (val escape = text[pos++]) {
                        '"' -> out.append('"')
                        '\\' -> out.append('\\')
                        '/' -> out.append('/')
                        'b' -> out.append('\b')
                        'f' -> out.append('\u000C')
                        'n' -> out.append('\n')
                        'r' -> out.append('\r')
                        't' -> out.append('\t')
                        'u' -> {
                            require(pos + 4 <= text.length) { "truncated \\u escape" }
                            out.append(text.substring(pos, pos + 4).toInt(16).toChar())
                            pos += 4
                        }
                        else -> fail("bad escape '\\$escape'")
                    }
                }
                else -> out.append(c)
            }
        }
    }

    private fun parseNumber(): Double {
        val start = pos
        if (peek() == '-') pos++
        while (pos < text.length && (text[pos].isDigit() || text[pos] in ".eE+-")) pos++
        return text.substring(start, pos).toDoubleOrNull() ?: fail("bad number")
    }

    private fun parseLiteral(literal: String, value: Any?): Any? {
        require(text.startsWith(literal, pos)) { "expected $literal" }
        pos += literal.length
        return value
    }

    private fun skipWhitespace() {
        while (pos < text.length && text[pos].isWhitespace()) pos++
    }

    private fun peek(): Char? = if (pos < text.length) text[pos] else null

    private fun expect(c: Char) {
        require(peek() == c) { "expected '$c' at $pos" }
        pos++
    }

    private fun fail(message: String): Nothing = throw IllegalArgumentException("$message at $pos")
}
