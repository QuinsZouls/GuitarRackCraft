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
import java.io.IOException
import java.io.InputStream
import java.nio.file.Files
import java.util.zip.ZipInputStream

/** One file of an [Lv2ImportSource], path relative to the source root. */
class Lv2ImportEntry(
    val relativePath: String,
    /** Uncompressed size, or -1 when the source does not know it up front. */
    val sizeBytes: Long,
    /** Opens the content. The stream is only valid inside the `walk` call that
     *  produced this entry: ZIP sources stream straight out of the archive. */
    val openStream: () -> InputStream,
)

/**
 * Something an LV2 bundle can be imported from: a ZIP archive or a picked
 * folder.
 *
 * The importer only needs this much from the platform, which keeps copying,
 * validation and the zip-slip/zip-bomb limits testable on the JVM while the SAF
 * adapter ([Lv2SafSources]) handles the Android side.
 */
interface Lv2ImportSource {
    /** Name the imported bundle is labelled with (the file or folder name). */
    val displayName: String

    /** Visit every file in the source. The visitor returns false to stop early. */
    fun walk(visit: (Lv2ImportEntry) -> Boolean)
}

/** ZIP archive source: streams entries, never extracts on its own. */
class ZipImportSource(
    private val input: InputStream,
    override val displayName: String,
) : Lv2ImportSource {

    override fun walk(visit: (Lv2ImportEntry) -> Boolean) {
        ZipInputStream(input.buffered()).use { zip ->
            while (true) {
                val entry = zip.nextEntry ?: break
                if (!entry.isDirectory) {
                    val accepted = visit(
                        Lv2ImportEntry(entry.name, entry.size) { zip.nonClosingStream() }
                    )
                    if (!accepted) return
                }
                zip.closeEntry()
            }
        }
    }

    /** The importer copies straight out of the archive stream, so closing that
     *  copy must not close the ZIP itself. */
    private fun ZipInputStream.nonClosingStream(): InputStream = object : InputStream() {
        override fun read(): Int = this@nonClosingStream.read()
        override fun read(b: ByteArray, off: Int, len: Int): Int =
            this@nonClosingStream.read(b, off, len)
        override fun close() = Unit
    }
}

/** Directory source, used by the SAF adapter (after staging a folder tree) and
 *  by the tests. Symbolic links are rejected: they would smuggle files from
 *  outside the picked folder into the bundle. */
class DirectoryImportSource(
    private val root: File,
    override val displayName: String = root.name,
) : Lv2ImportSource {

    override fun walk(visit: (Lv2ImportEntry) -> Boolean) {
        val files = mutableListOf<File>()
        collectFiles(root, files)
        val rootPath = root.absoluteFile.toPath()

        for (file in files) {
            val relative = rootPath
                .relativize(file.absoluteFile.toPath())
                .toString()
                .replace(File.separatorChar, '/')
            if (!visit(Lv2ImportEntry(relative, file.length()) { file.inputStream() })) return
        }
    }

    private fun collectFiles(directory: File, out: MutableList<File>) {
        val children = directory.listFiles() ?: return
        for (child in children.sortedBy { it.name }) {
            if (Files.isSymbolicLink(child.toPath())) {
                throw IOException("symbolic links are not supported: ${child.name}")
            }
            if (child.isDirectory) {
                collectFiles(child, out)
            } else if (child.isFile) {
                out.add(child)
            }
        }
    }
}

/**
 * Copies an LV2 bundle into `<filesDir>/lv2_user/<id>` so the native scanner
 * picks it up on the next refresh, refusing anything the engine could not load
 * (no manifest, no binary, binary built for another architecture).
 *
 * Plain java.io on purpose: the Android side only supplies an [Lv2ImportSource],
 * so the whole validation path runs in JVM unit tests.
 */
object Lv2BundleImporter {
    /** Refuse archives that unpack into more than this many files. */
    const val MAX_ENTRIES = 4096

    /** Refuse archives that unpack into more than 512 MB. */
    const val MAX_TOTAL_BYTES = 512L * 1024L * 1024L

    const val ELF_MACHINE_AARCH64 = 183
    const val BUNDLE_SUFFIX = ".lv2"

    private const val ELF_HEADER_BYTES = 20
    private const val ELF_CLASS_64 = 2
    private const val ELF_DATA_LSB = 1
    private const val STAGING_PREFIX = ".staging-"
    private const val COPY_BUFFER_BYTES = 64 * 1024

    sealed class Result {
        /** [bundleDir] is the final directory under `lv2_user`, [pluginBinaries]
         *  the number of `.so` files it carries. */
        data class Success(
            val bundleDir: File,
            val displayName: String,
            val pluginBinaries: Int,
        ) : Result()

        data class Failure(val reason: String) : Result()
    }

    /** Copy [source] into `userDir/<id>/`. [id] becomes a directory name, so it
     *  must already be a safe token (the caller passes a UUID). */
    fun import(source: Lv2ImportSource, userDir: File, id: String): Result {
        if (!isSafeToken(id)) {
            return Result.Failure("internal error: unsafe bundle id")
        }
        if (!userDir.isDirectory && !userDir.mkdirs()) {
            return Result.Failure("could not create ${userDir.absolutePath}")
        }

        val staging = File(userDir, STAGING_PREFIX + id)
        staging.deleteRecursively()
        if (!staging.mkdirs()) {
            return Result.Failure("could not create a staging directory")
        }

        val copyFailure = try {
            copyEntries(source, staging)
        } catch (e: IOException) {
            Result.Failure("could not read the bundle: ${e.message}")
        }
        if (copyFailure != null) {
            staging.deleteRecursively()
            return copyFailure
        }

        val bundleRoot = try {
            unwrapBareBundle(staging, source.displayName, userDir)
        } catch (e: IOException) {
            staging.deleteRecursively()
            return Result.Failure("could not unpack the bundle: ${e.message}")
        }

        val invalid = validate(bundleRoot)
        if (invalid != null) {
            bundleRoot.deleteRecursively()
            staging.deleteRecursively()
            return invalid
        }

        val target = File(userDir, id)
        target.deleteRecursively()
        if (!bundleRoot.renameTo(target)) {
            bundleRoot.deleteRecursively()
            return Result.Failure("could not finalise the bundle directory")
        }

        val binaries = markBinariesReadOnly(target)
        return Result.Success(
            bundleDir = target,
            displayName = source.displayName.ifBlank { id },
            pluginBinaries = binaries,
        )
    }

    /** True when [file] is an ELF64 little-endian AArch64 object, i.e. something
     *  this build can dlopen. */
    fun isAarch64SharedObject(file: File): Boolean {
        val header = readHeader(file) ?: return false
        val isElf = header[0] == 0x7F.toByte() &&
            header[1] == 'E'.code.toByte() &&
            header[2] == 'L'.code.toByte() &&
            header[3] == 'F'.code.toByte()
        if (!isElf) return false

        val elfClass = header[4].toInt() and 0xFF
        val elfData = header[5].toInt() and 0xFF
        val machine = (header[18].toInt() and 0xFF) or ((header[19].toInt() and 0xFF) shl 8)
        return elfClass == ELF_CLASS_64 && elfData == ELF_DATA_LSB && machine == ELF_MACHINE_AARCH64
    }

    /** Bundle directory name derived from the picked name: `Foo.zip` ->
     *  `Foo.lv2`, everything that is not a plain filename character replaced. */
    fun bundleDirectoryName(displayName: String): String {
        val stem = displayName.substringBeforeLast('.', displayName)
        val sanitized = stem
            .map { if (it.isLetterOrDigit() || it in "._-") it else '_' }
            .joinToString("")
            .trim('.', '_', '-')
        val base = sanitized.ifEmpty { "bundle" }
        return if (base.endsWith(BUNDLE_SUFFIX)) base else base + BUNDLE_SUFFIX
    }

    private fun copyEntries(source: Lv2ImportSource, staging: File): Result.Failure? {
        var entries = 0
        var written = 0L
        var failure: Result.Failure? = null

        source.walk { entry ->
            entries++
            if (entries > MAX_ENTRIES) {
                failure = Result.Failure("bundle has more than $MAX_ENTRIES files")
                return@walk false
            }

            val relative = normalizeRelativePath(entry.relativePath)
            val target = relative?.let { File(staging, it) }
            if (relative == null || target == null || !isInside(staging, target)) {
                failure = Result.Failure("bundle contains an unsafe path: ${entry.relativePath}")
                return@walk false
            }

            target.parentFile?.mkdirs()
            var copied = 0L
            entry.openStream().use { input ->
                target.outputStream().use { output ->
                    val buffer = ByteArray(COPY_BUFFER_BYTES)
                    while (true) {
                        val read = input.read(buffer)
                        if (read < 0) break
                        copied += read
                        written += read
                        if (written > MAX_TOTAL_BYTES) {
                            failure = Result.Failure(tooLargeMessage())
                            return@walk false
                        }
                        output.write(buffer, 0, read)
                    }
                }
            }

            if (entry.sizeBytes >= 0 && copied != entry.sizeBytes) {
                failure = Result.Failure("bundle is truncated: ${entry.relativePath}")
                return@walk false
            }
            true
        }

        return failure
    }

    /**
     * A ZIP whose manifest.ttl sits at the top level is a bare bundle, not a
     * folder of bundles. lilv only finds bundles below a scan root, so wrap it
     * in `<name>.lv2/`.
     */
    private fun unwrapBareBundle(staging: File, displayName: String, userDir: File): File {
        if (!File(staging, "manifest.ttl").isFile) {
            return staging
        }

        val wrapped = File(userDir, STAGING_PREFIX + "wrapped-" + staging.name.removePrefix(STAGING_PREFIX))
        wrapped.deleteRecursively()
        try {
            val inner = File(wrapped, bundleDirectoryName(displayName))
            if (!inner.mkdirs()) {
                throw IOException("could not create ${inner.absolutePath}")
            }
            for (child in staging.listFiles().orEmpty()) {
                if (!child.renameTo(File(inner, child.name))) {
                    throw IOException("could not move ${child.name}")
                }
            }
        } catch (e: IOException) {
            // Half-moved trees are useless and would be counted as a leftover
            // staging directory: drop the wrapper before reporting the failure.
            wrapped.deleteRecursively()
            throw e
        }
        staging.deleteRecursively()
        return wrapped
    }

    private fun validate(bundleRoot: File): Result.Failure? {
        val files = bundleRoot.walkTopDown().filter { it.isFile }.toList()
        if (files.none { it.name == "manifest.ttl" }) {
            return Result.Failure("no manifest.ttl found — this is not an LV2 bundle")
        }

        val binaries = files.filter { it.name.endsWith(".so") }
        if (binaries.isEmpty()) {
            return Result.Failure("no plugin binary (.so) found in the bundle")
        }

        for (binary in binaries) {
            if (!isAarch64SharedObject(binary)) {
                return Result.Failure(
                    "plugin compiled for another architecture; ARM64 required (${binary.name})"
                )
            }
        }
        return null
    }

    private fun markBinariesReadOnly(bundleRoot: File): Int {
        val binaries = bundleRoot.walkTopDown()
            .filter { it.isFile && it.name.endsWith(".so") }
            .toList()
        for (binary in binaries) {
            binary.setReadOnly()
        }
        return binaries.size
    }

    /** Reject absolute paths, `..` segments and anything that is not a plain
     *  relative path; returns it with the platform separator. */
    private fun normalizeRelativePath(path: String): String? {
        if (path.isEmpty()) return null
        val unified = path.replace('\\', '/')
        if (unified.startsWith("/") || unified.contains(':')) return null
        val segments = unified.split('/').filter { it.isNotEmpty() && it != "." }
        if (segments.isEmpty() || segments.any { it == ".." }) return null
        return segments.joinToString(File.separator)
    }

    private fun isInside(root: File, target: File): Boolean {
        val rootPath = root.canonicalFile.toPath()
        val targetPath = target.canonicalFile.toPath()
        return targetPath.startsWith(rootPath) && targetPath != rootPath
    }

    /** A bundle id becomes a directory name under `lv2_user`, and the import
     *  starts by deleting that directory recursively: a token made only of dots
     *  ("." / ".." / "...") resolves to lv2_user itself or its parent, so it
     *  must be refused here and not just at the file-name level. */
    private fun isSafeToken(token: String): Boolean =
        token.isNotEmpty() &&
            token.any { it != '.' } &&
            token.all { it.isLetterOrDigit() || it in "._-" }

    private fun tooLargeMessage(): String =
        "bundle is larger than ${MAX_TOTAL_BYTES / (1024L * 1024L)} MB once unpacked"

    private fun readHeader(file: File): ByteArray? {
        if (!file.isFile) return null
        val header = ByteArray(ELF_HEADER_BYTES)
        val complete = runCatching {
            file.inputStream().use { stream ->
                var offset = 0
                while (offset < header.size) {
                    val count = stream.read(header, offset, header.size - offset)
                    if (count <= 0) return null
                    offset += count
                }
                true
            }
        }.getOrNull() ?: false
        return if (complete) header else null
    }
}
