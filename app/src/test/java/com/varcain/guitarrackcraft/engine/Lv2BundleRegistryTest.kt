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

import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.File
import java.nio.file.Files

/**
 * Bookkeeping contract of the imported-LV2 registry: what the browser needs to
 * list and delete bundles (`displayName`, plugin count) and the tombstone that
 * hides URIs whose files are gone while the native world still reports them.
 *
 * The file format itself is not asserted textually; the tests go through
 * read/write so the writer and the reader have to agree, and the URIs used here
 * are the LV2 URIs the engine really reports.
 */
class Lv2BundleRegistryTest {

    @get:Rule
    val temporaryFolder = TemporaryFolder()

    private val filesDir: File by lazy { temporaryFolder.newFolder("files") }

    @Test
    fun readsWhatItWrote() {
        val entries = listOf(
            Lv2BundleEntry(
                id = "id-1",
                displayName = "Cool Amp",
                bundleDir = "id-1",
                uris = listOf("http://example.org/amp", "http://example.org/cab"),
                importedAt = 1_700_000_000_000L,
            ),
            Lv2BundleEntry(
                id = "id-2",
                displayName = "Quoted \"name\" \\ backslash",
                bundleDir = "id-2",
                uris = emptyList(),
                importedAt = 1_700_000_001_000L,
                removed = true,
            ),
        )

        Lv2BundleRegistry.write(filesDir, entries)

        assertEquals(entries, Lv2BundleRegistry.read(filesDir))
    }

    @Test
    fun missingRegistryReadsAsEmpty() {
        assertEquals(emptyList<Lv2BundleEntry>(), Lv2BundleRegistry.read(filesDir))
        assertEquals(emptySet<String>(), Lv2BundleRegistry.activeUris(filesDir))
        assertEquals(emptySet<String>(), Lv2BundleRegistry.removedUris(filesDir))
    }

    @Test
    fun corruptRegistryReadsAsEmptyInsteadOfThrowing() {
        val file = Lv2BundleRegistry.registryFile(filesDir)
        file.parentFile?.mkdirs()
        file.writeText("{\"bundles\": [ {\"id\": \"id-1\",, ]")

        assertEquals(emptyList<Lv2BundleEntry>(), Lv2BundleRegistry.read(filesDir))
    }

    @Test
    fun truncatedJsonIsIgnored() {
        val file = Lv2BundleRegistry.registryFile(filesDir)
        file.parentFile?.mkdirs()
        file.writeText("{\"bundles\": [{\"id\": \"id-1\", \"uris\": [\"http://exam")

        assertEquals(emptyList<Lv2BundleEntry>(), Lv2BundleRegistry.read(filesDir))
    }

    @Test
    fun entriesWithoutAnIdAreSkipped() {
        val file = Lv2BundleRegistry.registryFile(filesDir)
        file.parentFile?.mkdirs()
        file.writeText(
            """
            {"bundles": [
              {"displayName": "no id"},
              {"id": "kept", "displayName": "Kept", "bundleDir": "kept"}
            ]}
            """.trimIndent()
        )

        val entries = Lv2BundleRegistry.read(filesDir)
        assertEquals(listOf("kept"), entries.map { it.id })
        assertEquals(emptyList<String>(), entries.single().uris)
        assertEquals(false, entries.single().removed)
    }

    @Test
    fun activeAndRemovedUrisAreSplit() {
        Lv2BundleRegistry.write(
            filesDir,
            listOf(
                Lv2BundleEntry("a", "A", "a", listOf("uri:a")),
                Lv2BundleEntry("b", "B", "b", listOf("uri:b"), removed = true),
            ),
        )

        assertEquals(setOf("uri:a"), Lv2BundleRegistry.activeUris(filesDir))
        assertEquals(setOf("uri:b"), Lv2BundleRegistry.removedUris(filesDir))
        assertEquals(listOf("a"), Lv2BundleRegistry.activeBundles(filesDir).map { it.id })
    }

    @Test
    fun markRemovedKeepsTheEntryAsTombstone() {
        Lv2BundleRegistry.add(filesDir, Lv2BundleEntry("a", "A", "a", listOf("uri:a")))

        Lv2BundleRegistry.markRemoved(filesDir, "a")

        val entries = Lv2BundleRegistry.read(filesDir)
        assertEquals(1, entries.size)
        assertTrue(entries.single().removed)
        assertEquals(listOf("uri:a"), entries.single().uris)
        assertEquals(emptySet<String>(), Lv2BundleRegistry.activeUris(filesDir))
        assertEquals(setOf("uri:a"), Lv2BundleRegistry.removedUris(filesDir))
    }

    @Test
    fun setUrisReplacesOnlyTheTargetEntry() {
        Lv2BundleRegistry.write(
            filesDir,
            listOf(
                Lv2BundleEntry("a", "A", "a", listOf("uri:a")),
                Lv2BundleEntry("b", "B", "b"),
            ),
        )

        Lv2BundleRegistry.setUris(filesDir, "b", listOf("uri:b1", "uri:b2"))

        val entries = Lv2BundleRegistry.read(filesDir).associateBy { it.id }
        assertEquals(listOf("uri:a"), entries.getValue("a").uris)
        assertEquals(listOf("uri:b1", "uri:b2"), entries.getValue("b").uris)
    }

    @Test
    fun reconcileFillsTheUrisOfAnEntryThatWasImportedWithoutTheEngine() {
        // The bug this covers: a bundle imported while the engine was not
        // enumerating stored no URIs, so its plugins were never grouped under
        // "Imported LV2" and its tombstone could not hide them.
        Lv2BundleRegistry.add(filesDir, Lv2BundleEntry("a", "A", "a"))

        val reconciled = Lv2BundleRegistry.reconcileUris(
            filesDir,
            mapOf("a" to listOf("http://example.org/b", "http://example.org/a")),
        )

        assertEquals(listOf("http://example.org/a", "http://example.org/b"), reconciled.single().uris)
        assertEquals(reconciled, Lv2BundleRegistry.read(filesDir))
        assertEquals(
            setOf("http://example.org/a", "http://example.org/b"),
            Lv2BundleRegistry.activeUris(filesDir),
        )
    }

    @Test
    fun reconcileKeepsWhatAnEntryAlreadyHasWhenNothingIsAttributed() {
        Lv2BundleRegistry.add(filesDir, Lv2BundleEntry("a", "A", "a", listOf("http://example.org/a")))

        val reconciled = Lv2BundleRegistry.reconcileUris(filesDir, emptyMap())

        assertEquals(listOf("http://example.org/a"), reconciled.single().uris)
    }

    @Test
    fun reconcileLeavesTombstonesAlone() {
        Lv2BundleRegistry.write(
            filesDir,
            listOf(Lv2BundleEntry("gone", "Gone", "gone", listOf("uri:gone"), removed = true)),
        )

        val reconciled = Lv2BundleRegistry.reconcileUris(filesDir, mapOf("gone" to listOf("uri:other")))

        assertEquals(listOf("uri:gone"), reconciled.single().uris)
        assertEquals(setOf("uri:gone"), Lv2BundleRegistry.removedUris(filesDir))
    }

    @Test
    fun reconcileDoesNotRewriteAnUnchangedRegistry() {
        Lv2BundleRegistry.add(filesDir, Lv2BundleEntry("a", "A", "a", listOf("uri:a")))
        val file = Lv2BundleRegistry.registryFile(filesDir)
        // A rewrite would stamp "now" on it; 1970 is only reachable if nothing
        // touched the file.
        assertTrue(file.setLastModified(1_000_000L))

        Lv2BundleRegistry.reconcileUris(filesDir, mapOf("a" to listOf("uri:a")))

        assertEquals(1_000_000L, file.lastModified())
    }

    @Test
    fun writeLeavesNoStagingFileBehind() {
        Lv2BundleRegistry.write(filesDir, listOf(Lv2BundleEntry("a", "A", "a", listOf("uri:a"))))

        val names = Lv2BundleRegistry.userDir(filesDir).list()?.sorted().orEmpty()

        assertEquals(listOf(Lv2BundleRegistry.REGISTRY_FILE), names)
    }

    @Test
    fun aTruncatedStagingFileFromAnInterruptedWriteIsIgnored() {
        Lv2BundleRegistry.write(filesDir, listOf(Lv2BundleEntry("a", "A", "a")))
        File(Lv2BundleRegistry.userDir(filesDir), Lv2BundleRegistry.STAGING_FILE)
            .writeText("{\"bundles\": [{\"id\": \"half")

        val entries = Lv2BundleRegistry.read(filesDir)

        assertEquals(listOf("a"), entries.map { it.id })
    }

    @Test
    fun writeReplacesTheRegistryInsteadOfWritingThroughIt() {
        // Pins the atomic write: a staged file renamed over registry.json swaps
        // the file, while an in-place write would follow a pre-existing symlink
        // and leave the registry being written through it.
        val dir = Lv2BundleRegistry.userDir(filesDir)
        dir.mkdirs()
        val elsewhere = temporaryFolder.newFile("elsewhere.json").apply { writeText("untouched") }
        val registry = Lv2BundleRegistry.registryFile(filesDir).toPath()
        Files.createSymbolicLink(registry, elsewhere.toPath())

        Lv2BundleRegistry.write(filesDir, listOf(Lv2BundleEntry("a", "A", "a")))

        assertEquals("untouched", elsewhere.readText())
        assertEquals(false, Files.isSymbolicLink(registry))
        assertEquals(listOf("a"), Lv2BundleRegistry.read(filesDir).map { it.id })
    }

    @Test
    fun writeCreatesTheUserDirectory() {
        assertEquals(false, Lv2BundleRegistry.userDir(filesDir).exists())

        Lv2BundleRegistry.write(filesDir, listOf(Lv2BundleEntry("a", "A", "a")))

        assertEquals(true, Lv2BundleRegistry.registryFile(filesDir).isFile)
        assertEquals("lv2_user", Lv2BundleRegistry.userDir(filesDir).name)
        assertEquals("registry.json", Lv2BundleRegistry.registryFile(filesDir).name)
    }
}
