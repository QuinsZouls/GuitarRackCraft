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
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * Validation contract of the LV2 bundle importer: what may be installed under
 * `filesDir/lv2_user` and what has to be refused before the native scanner sees
 * it.
 *
 * Everything here goes through [Lv2BundleImporter.import] with a value the test
 * builds itself (ZIP entries, fake ELF headers, directory trees). Nothing reads
 * the limits back from the importer's constants: the cases below fail either by
 * exceeding a limit or by being explicitly invalid, so shrinking a limit or
 * dropping a check breaks a test instead of silently agreeing with it.
 */
class Lv2BundleImporterTest {

    @get:Rule
    val temporaryFolder = TemporaryFolder()

    private val userDir: File by lazy { temporaryFolder.newFolder("lv2_user") }

    // ---------------------------------------------------------------- ZIP input

    @Test
    fun importsZipWithNestedBundleDirectory() {
        val zip = zipOf(
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
            "CoolAmp.lv2/plugin.ttl" to pluginTtl("CoolAmp"),
            "CoolAmp.lv2/coolamp.so" to aarch64Elf(),
        )

        val result = importZip(zip, displayName = "CoolAmp.zip")

        val success = assertSuccess(result)
        assertEquals("CoolAmp.zip", success.displayName)
        assertEquals(1, success.pluginBinaries)

        val bundle = File(success.bundleDir, "CoolAmp.lv2")
        assertTrue(bundle.isDirectory)
        assertTrue(File(bundle, "manifest.ttl").isFile)
        assertTrue(File(bundle, "coolamp.so").isFile)
    }

    @Test
    fun wrapsBareBundleWhoseManifestIsAtTheArchiveRoot() {
        val zip = zipOf(
            "manifest.ttl" to manifestFor("http://example.org/bare"),
            "plugin.ttl" to pluginTtl("Bare"),
            "bare.so" to aarch64Elf(),
        )

        val result = importZip(zip, displayName = "Bare Tone.zip")

        val success = assertSuccess(result)
        // The wrapper directory is derived from the picked name, sanitised to a
        // plain filename ("Bare Tone.zip" -> "Bare_Tone.lv2").
        val bundles = success.bundleDir.listFiles().orEmpty().filter { it.isDirectory }
        assertEquals(listOf("Bare_Tone.lv2"), bundles.map { it.name })
        assertTrue(File(bundles.single(), "manifest.ttl").isFile)
        assertTrue(File(bundles.single(), "bare.so").isFile)
    }

    @Test
    fun importsFolderSource() {
        val source = temporaryFolder.newFolder("picked")
        File(source, "FolderAmp.lv2").mkdirs()
        File(source, "FolderAmp.lv2/manifest.ttl").writeBytes(manifestFor("http://example.org/folder"))
        File(source, "FolderAmp.lv2/folderamp.so").writeBytes(aarch64Elf())

        val result = Lv2BundleImporter.import(
            DirectoryImportSource(source, "FolderAmp"),
            userDir,
            "bundle-1",
        )

        val success = assertSuccess(result)
        assertTrue(File(success.bundleDir, "FolderAmp.lv2/manifest.ttl").isFile)
    }

    @Test
    fun rejectsZipSlipEntryOutsideTheBundleDirectory() {
        val zip = zipOf(
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
            "CoolAmp.lv2/coolamp.so" to aarch64Elf(),
            "../../escaped.so" to aarch64Elf(),
        )

        val result = importZip(zip, displayName = "Escape.zip")

        val failure = assertFailure(result)
        assertTrue(failure.reason, failure.reason.contains("unsafe path"))
        assertFalse("nothing may be left behind", File(userDir.parentFile, "escaped.so").exists())
        assertEquals(emptyList<File>(), userDir.listFiles().orEmpty().toList())
    }

    @Test
    fun rejectsAbsolutePathEntry() {
        val zip = zipOf(
            "/data/local/tmp/evil.so" to aarch64Elf(),
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
        )

        val failure = assertFailure(importZip(zip, displayName = "Absolute.zip"))
        assertTrue(failure.reason, failure.reason.contains("unsafe path"))
        assertFalse(File("/data/local/tmp/evil.so").exists())
    }

    @Test
    fun rejectsArchiveWithTooManyEntries() {
        val entries = mutableListOf<Pair<String, ByteArray>>()
        entries.add("CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"))
        entries.add("CoolAmp.lv2/coolamp.so" to aarch64Elf())
        repeat(4200) { index ->
            entries.add("CoolAmp.lv2/pad-$index.bin" to ByteArray(1))
        }

        val failure = assertFailure(importZip(zipOf(*entries.toTypedArray()), "Bomb.zip"))
        assertTrue(failure.reason, failure.reason.contains("more than"))
    }

    @Test
    fun rejectsArchiveThatUnpacksBeyondTheSizeLimit() {
        // Written straight to disk: 600 MB of zeroes compresses to a few hundred
        // kB, so the archive stays small while the unpacked size is over the limit.
        val archive = temporaryFolder.newFile("Huge.zip")
        val padding = ByteArray(1024 * 1024)
        archive.outputStream().use { fileOut ->
            ZipOutputStream(fileOut).use { zip ->
                zip.putNextEntry(ZipEntry("CoolAmp.lv2/manifest.ttl"))
                zip.write(manifestFor("http://example.org/amp"))
                zip.closeEntry()
                zip.putNextEntry(ZipEntry("CoolAmp.lv2/coolamp.so"))
                zip.write(aarch64Elf())
                zip.closeEntry()
                repeat(600) { index ->
                    zip.putNextEntry(ZipEntry("CoolAmp.lv2/blob-$index.bin"))
                    zip.write(padding)
                    zip.closeEntry()
                }
            }
        }

        val result = archive.inputStream().use { stream ->
            Lv2BundleImporter.import(ZipImportSource(stream, "Huge.zip"), userDir, "huge-id")
        }

        val failure = assertFailure(result)
        assertTrue(failure.reason, failure.reason.contains("larger than"))
        assertEquals(emptyList<File>(), userDir.listFiles().orEmpty().toList())
    }

    // ------------------------------------------------------------- binary checks

    @Test
    fun rejectsPluginBuiltForX8664() {
        val zip = zipOf(
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
            "CoolAmp.lv2/coolamp.so" to elf(machine = 62),
        )

        val failure = assertFailure(importZip(zip, displayName = "X86.zip"))
        assertTrue(failure.reason, failure.reason.contains("ARM64 required"))
        assertTrue(failure.reason, failure.reason.contains("coolamp.so"))
    }

    @Test
    fun rejectsPluginBuiltForArm32() {
        val zip = zipOf(
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
            "CoolAmp.lv2/coolamp.so" to elf(machine = 40, elfClass = 1),
        )

        assertFailure(importZip(zip, displayName = "Arm32.zip"))
    }

    @Test
    fun rejectsBundleWhenAnyBinaryIsForAnotherArchitecture() {
        val zip = zipOf(
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
            "CoolAmp.lv2/coolamp.so" to aarch64Elf(),
            "CoolAmp.lv2/helper.so" to elf(machine = 62),
        )

        val failure = assertFailure(importZip(zip, displayName = "Mixed.zip"))
        assertTrue(failure.reason, failure.reason.contains("helper.so"))
    }

    @Test
    fun rejectsElf32BuiltForTheAarch64Machine() {
        // 32-bit object, right machine: only the ELF class check can reject it.
        val zip = zipOf(
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
            "CoolAmp.lv2/coolamp.so" to elf(machine = Lv2BundleImporter.ELF_MACHINE_AARCH64, elfClass = 1),
        )

        val failure = assertFailure(importZip(zip, displayName = "Elf32.zip"))
        assertTrue(failure.reason, failure.reason.contains("ARM64 required"))
    }

    @Test
    fun rejectsBundleWithoutManifest() {
        val zip = zipOf("CoolAmp.lv2/coolamp.so" to aarch64Elf())

        val failure = assertFailure(importZip(zip, displayName = "NoManifest.zip"))
        assertTrue(failure.reason, failure.reason.contains("manifest.ttl"))
    }

    @Test
    fun rejectsBundleThatHasDescriptionsAndBinaryButNoManifest() {
        // What the user actually picks when they zip the insides of a bundle
        // instead of its directory: everything lilv needs is there except the
        // file that makes lilv look at it.
        val zip = zipOf(
            "CoolAmp.lv2/plugin.ttl" to pluginTtl("CoolAmp"),
            "CoolAmp.lv2/modgui.ttl" to "@prefix modgui: <http://moddevices.com/ns/modgui#> .".toByteArray(),
            "CoolAmp.lv2/coolamp.so" to aarch64Elf(),
        )

        val failure = assertFailure(importZip(zip, displayName = "NoManifest.zip"))
        assertTrue(failure.reason, failure.reason.contains("manifest.ttl"))
        assertEquals(emptyList<String>(), userDir.listFiles().orEmpty().map { it.name })
    }

    @Test
    fun rejectsBundleWithoutBinary() {
        val zip = zipOf(
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
            "CoolAmp.lv2/plugin.ttl" to pluginTtl("CoolAmp"),
        )

        val failure = assertFailure(importZip(zip, displayName = "NoBinary.zip"))
        assertTrue(failure.reason, failure.reason.contains(".so"))
    }

    @Test
    fun rejectsFileThatIsNotAnElf() {
        val zip = zipOf(
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
            "CoolAmp.lv2/coolamp.so" to "not an executable".toByteArray(),
        )

        assertFailure(importZip(zip, displayName = "Text.zip"))
    }

    // ------------------------------------------------------------------ results

    @Test
    fun writesBinariesReadOnly() {
        val zip = zipOf(
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
            "CoolAmp.lv2/coolamp.so" to aarch64Elf(),
        )

        val success = assertSuccess(importZip(zip, displayName = "CoolAmp.zip"))
        val binary = File(File(success.bundleDir, "CoolAmp.lv2"), "coolamp.so")

        assertFalse("the plugin binary must not stay writable", binary.canWrite())
        assertTrue(binary.canRead())
    }

    @Test
    fun leavesNoStagingDirectoryBehind() {
        val zip = zipOf(
            "CoolAmp.lv2/manifest.ttl" to manifestFor("http://example.org/amp"),
            "CoolAmp.lv2/coolamp.so" to aarch64Elf(),
        )
        assertSuccess(importZip(zip, displayName = "CoolAmp.zip", id = "fixed-id"))

        assertEquals(listOf("fixed-id"), userDir.listFiles().orEmpty().map { it.name })
    }

    @Test
    fun failedImportLeavesNothingBehind() {
        val zip = zipOf("CoolAmp.lv2/coolamp.so" to aarch64Elf())
        assertFailure(importZip(zip, displayName = "NoManifest.zip", id = "fixed-id"))

        assertEquals(emptyList<String>(), userDir.listFiles().orEmpty().map { it.name })
    }

    @Test
    fun cleansTheWrapperDirectoryWhenUnwrappingFails() {
        // A bare bundle (manifest.ttl at the archive root) is moved into a
        // wrapper directory first. A picked name long enough to overflow a file
        // name makes that move fail halfway: the half-built wrapper must not stay
        // behind as a stray .staging directory.
        val zip = zipOf(
            "manifest.ttl" to manifestFor("http://example.org/amp"),
            "coolamp.so" to aarch64Elf(),
        )

        val failure = assertFailure(importZip(zip, displayName = "a".repeat(300), id = "fixed-id"))

        assertTrue(failure.reason, failure.reason.contains("could not unpack"))
        assertEquals(emptyList<String>(), userDir.listFiles().orEmpty().map { it.name })
    }

    @Test
    fun rejectsUnsafeBundleId() {
        val source = DirectoryImportSource(temporaryFolder.newFolder("empty"), "Empty")

        val failure = assertFailure(Lv2BundleImporter.import(source, userDir, "../escape"))

        assertTrue(failure.reason, failure.reason.contains("unsafe bundle id"))
    }

    @Test
    fun rejectsBundleIdsMadeOnlyOfDots() {
        // Each of these resolves outside its own directory once turned into a
        // File: ".." is lv2_user's parent, which the import deletes recursively
        // before copying.
        val source = DirectoryImportSource(temporaryFolder.newFolder("empty-dots"), "Empty")

        for (unsafe in listOf(".", "..", "...")) {
            val failure = assertFailure(Lv2BundleImporter.import(source, userDir, unsafe))
            assertTrue(
                "id '$unsafe' must be rejected: ${failure.reason}",
                failure.reason.contains("unsafe bundle id"),
            )
        }
        assertEquals(emptyList<String>(), userDir.listFiles().orEmpty().map { it.name })
    }

    @Test
    fun derivesBundleDirectoryNameFromPickedName() {
        assertEquals("CoolAmp.lv2", Lv2BundleImporter.bundleDirectoryName("CoolAmp.zip"))
        assertEquals("CoolAmp.lv2", Lv2BundleImporter.bundleDirectoryName("CoolAmp.lv2"))
        assertEquals("Cool_Amp_v2.lv2", Lv2BundleImporter.bundleDirectoryName("Cool Amp v2"))
        assertEquals("bundle.lv2", Lv2BundleImporter.bundleDirectoryName(".zip"))
    }

    @Test
    fun recognisesOnlyAarch64ElfHeaders() {
        val root = temporaryFolder.newFolder("headers")

        val good = File(root, "good.so").apply { writeBytes(aarch64Elf()) }
        val x86 = File(root, "x86.so").apply { writeBytes(elf(machine = 62)) }
        val arm32 = File(root, "arm32.so").apply { writeBytes(elf(machine = 40, elfClass = 1)) }
        val elf32Aarch64 = File(root, "elf32-aarch64.so").apply {
            writeBytes(elf(machine = 183, elfClass = 1))
        }
        val bigEndian = File(root, "be.so").apply { writeBytes(elf(machine = 183, bigEndian = true)) }
        val truncated = File(root, "cut.so").apply { writeBytes(aarch64Elf().copyOf(8)) }

        assertTrue(Lv2BundleImporter.isAarch64SharedObject(good))
        assertFalse(Lv2BundleImporter.isAarch64SharedObject(x86))
        assertFalse(Lv2BundleImporter.isAarch64SharedObject(arm32))
        assertFalse(Lv2BundleImporter.isAarch64SharedObject(elf32Aarch64))
        assertFalse(Lv2BundleImporter.isAarch64SharedObject(bigEndian))
        assertFalse(Lv2BundleImporter.isAarch64SharedObject(truncated))
        assertFalse(Lv2BundleImporter.isAarch64SharedObject(File(root, "missing.so")))
    }

    // ------------------------------------------------------------------ helpers

    private fun importZip(
        zip: ByteArray,
        displayName: String,
        id: String = "bundle-id",
    ): Lv2BundleImporter.Result =
        Lv2BundleImporter.import(
            ZipImportSource(ByteArrayInputStream(zip), displayName),
            userDir,
            id,
        )

    private fun assertSuccess(result: Lv2BundleImporter.Result): Lv2BundleImporter.Result.Success {
        assertTrue("expected success, got $result", result is Lv2BundleImporter.Result.Success)
        val success = result as Lv2BundleImporter.Result.Success
        assertNotNull(success.bundleDir)
        assertTrue("bundle directory must exist", success.bundleDir.isDirectory)
        return success
    }

    private fun assertFailure(result: Lv2BundleImporter.Result): Lv2BundleImporter.Result.Failure {
        assertTrue("expected failure, got $result", result is Lv2BundleImporter.Result.Failure)
        return result as Lv2BundleImporter.Result.Failure
    }

    private fun zipOf(vararg entries: Pair<String, ByteArray>): ByteArray {
        val out = ByteArrayOutputStream()
        ZipOutputStream(out).use { zip ->
            for ((name, content) in entries) {
                zip.putNextEntry(ZipEntry(name))
                zip.write(content)
                zip.closeEntry()
            }
        }
        return out.toByteArray()
    }

    private fun manifestFor(uri: String): ByteArray = """
        @prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
        @prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .

        <$uri>
            a lv2:Plugin ;
            lv2:binary <plugin.so> ;
            rdfs:seeAlso <plugin.ttl> .
    """.trimIndent().toByteArray()

    private fun pluginTtl(name: String): ByteArray = """
        @prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
        @prefix doap: <http://usefulinc.com/ns/doap#> .

        <http://example.org/$name>
            doap:name "$name" .
    """.trimIndent().toByteArray()

    private fun aarch64Elf(): ByteArray = elf(machine = Lv2BundleImporter.ELF_MACHINE_AARCH64)

    /** Minimal ELF header plus padding: the importer only reads the first 20
     *  bytes (magic, class, data encoding, machine). */
    private fun elf(machine: Int, elfClass: Int = 2, bigEndian: Boolean = false): ByteArray {
        val header = ByteArray(64)
        header[0] = 0x7F
        header[1] = 'E'.code.toByte()
        header[2] = 'L'.code.toByte()
        header[3] = 'F'.code.toByte()
        header[4] = elfClass.toByte()
        header[5] = if (bigEndian) 2 else 1
        header[6] = 1
        header[16] = 3
        if (bigEndian) {
            header[18] = ((machine shr 8) and 0xFF).toByte()
            header[19] = (machine and 0xFF).toByte()
        } else {
            header[18] = (machine and 0xFF).toByte()
            header[19] = ((machine shr 8) and 0xFF).toByte()
        }
        return header
    }
}
