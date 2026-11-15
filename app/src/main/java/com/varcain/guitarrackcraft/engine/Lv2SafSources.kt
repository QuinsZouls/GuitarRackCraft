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

import android.content.ContentResolver
import android.net.Uri
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import java.io.FileNotFoundException

/**
 * Storage Access Framework side of the LV2 import: turns a picked document (ZIP)
 * or a picked tree (folder) into an [Lv2ImportSource] the testable
 * [Lv2BundleImporter] can consume.
 *
 * Only plain [ContentResolver] + [DocumentsContract] are used, so the app does
 * not pull in androidx.documentfile for two call sites.
 */
object Lv2SafSources {

    private val ZIP_MAGIC = byteArrayOf(0x50, 0x4B, 0x03, 0x04)

    fun displayName(resolver: ContentResolver, uri: Uri): String {
        val queried = runCatching {
            resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
                val index = cursor.getColumnIndex(OpenableColumns.DISPLAY_NAME)
                if (index >= 0 && cursor.moveToFirst()) cursor.getString(index) else null
            }
        }.getOrNull()

        return queried?.takeIf { it.isNotBlank() }
            ?: uri.lastPathSegment?.substringAfterLast('/')?.takeIf { it.isNotBlank() }
            ?: "bundle.zip"
    }

    /** A ZIP is recognised by content (PK\x03\x04), not by extension: pickers
     *  happily hand back documents named `.lv2` or without a suffix at all. */
    fun looksLikeZip(resolver: ContentResolver, uri: Uri): Boolean {
        val header = ByteArray(ZIP_MAGIC.size)
        val read = runCatching {
            resolver.openInputStream(uri)?.use { stream ->
                var offset = 0
                while (offset < header.size) {
                    val count = stream.read(header, offset, header.size - offset)
                    if (count <= 0) return@use offset
                    offset += count
                }
                offset
            } ?: 0
        }.getOrElse { 0 }

        return read == ZIP_MAGIC.size && header.contentEquals(ZIP_MAGIC)
    }

    /** Opens the picked ZIP. The caller owns the returned stream. */
    fun openZip(resolver: ContentResolver, uri: Uri): java.io.InputStream =
        resolver.openInputStream(uri) ?: throw FileNotFoundException(uri.toString())

    fun treeSource(resolver: ContentResolver, treeUri: Uri): Lv2ImportSource =
        SafTreeImportSource(resolver, treeUri, displayName(resolver, treeUri))
}

/**
 * Walks a picked tree through the DocumentsContract, without materialising it.
 * Every file becomes an [Lv2ImportEntry] whose stream is opened lazily while the
 * importer copies it.
 */
private class SafTreeImportSource(
    private val resolver: ContentResolver,
    private val treeUri: Uri,
    override val displayName: String,
) : Lv2ImportSource {

    override fun walk(visit: (Lv2ImportEntry) -> Boolean) {
        val rootId = DocumentsContract.getTreeDocumentId(treeUri)
        walkDocument(rootId, "", visit)
    }

    private fun walkDocument(
        documentId: String,
        prefix: String,
        visit: (Lv2ImportEntry) -> Boolean,
    ): Boolean {
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(treeUri, documentId)
        val projection = arrayOf(
            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
            DocumentsContract.Document.COLUMN_DISPLAY_NAME,
            DocumentsContract.Document.COLUMN_MIME_TYPE,
            DocumentsContract.Document.COLUMN_SIZE,
        )

        resolver.query(children, projection, null, null, null)?.use { cursor ->
            val idIndex = cursor.getColumnIndex(projection[0])
            val nameIndex = cursor.getColumnIndex(projection[1])
            val mimeIndex = cursor.getColumnIndex(projection[2])
            val sizeIndex = cursor.getColumnIndex(projection[3])
            if (idIndex < 0 || nameIndex < 0 || mimeIndex < 0) return true

            while (cursor.moveToNext()) {
                val childId = cursor.getString(idIndex) ?: continue
                val name = cursor.getString(nameIndex) ?: continue
                val mimeType = cursor.getString(mimeIndex)
                val size = if (sizeIndex >= 0 && !cursor.isNull(sizeIndex)) cursor.getLong(sizeIndex) else -1L
                val path = if (prefix.isEmpty()) name else "$prefix/$name"

                val accepted = if (mimeType == DocumentsContract.Document.MIME_TYPE_DIR) {
                    walkDocument(childId, path, visit)
                } else {
                    val childUri = DocumentsContract.buildDocumentUriUsingTree(treeUri, childId)
                    visit(Lv2ImportEntry(path, size) { open(childUri) })
                }
                if (!accepted) return false
            }
        }
        return true
    }

    private fun open(childUri: Uri): java.io.InputStream =
        resolver.openInputStream(childUri) ?: throw FileNotFoundException(childUri.toString())
}
