/**
 * NSTU Exam Studio - Zero-Dependency Offline ZIP & SHA-256 Utility
 * 
 * Provides:
 * 1. NstuZipWriter: Packages files into standard .zip/.nstuexam format (Store or Deflate).
 * 2. NstuZipReader: Unpacks .zip/.nstuexam files natively using DecompressionStream.
 * 3. computePackageDigests: Calculates both archive SHA-256 and the canonical
 *    content digest exactly as implemented in NSTU's C++ ExamHost (exam_host.cpp).
 */

(function (global) {
  "use strict";

  // Precomputed CRC-32 table
  const CRC_TABLE = new Uint32Array(256);
  for (let i = 0; i < 256; i++) {
    let c = i;
    for (let k = 0; k < 8; k++) {
      c = (c & 1) ? (0xedb88320 ^ (c >>> 1)) : (c >>> 1);
    }
    CRC_TABLE[i] = c >>> 0;
  }

  function crc32(bytes) {
    let crc = 0xffffffff;
    for (let i = 0; i < bytes.length; i++) {
      crc = CRC_TABLE[(crc ^ bytes[i]) & 0xff] ^ (crc >>> 8);
    }
    return (crc ^ 0xffffffff) >>> 0;
  }

  function textToBytes(text) {
    return new TextEncoder().encode(text);
  }

  function bytesToText(bytes) {
    return new TextDecoder().decode(bytes);
  }

  function dosDateTime(date) {
    const d = date || new Date();
    const dosTime = ((d.getHours() << 11) | (d.getMinutes() << 5) | (d.getSeconds() >> 1)) & 0xffff;
    const dosDate = (((d.getFullYear() - 1980) << 9) | ((d.getMonth() + 1) << 5) | d.getDate()) & 0xffff;
    return { dosTime, dosDate };
  }

  function hexString(buffer) {
    const bytes = new Uint8Array(buffer);
    let hex = "";
    for (let i = 0; i < bytes.length; i++) {
      hex += bytes[i].toString(16).padStart(2, "0");
    }
    return hex;
  }

  /**
   * Computes the exact canonical content digest as verified by ExamHost (exam_host.cpp):
   * 1. Files sorted ascending by relative path string.
   * 2. For each file:
   *    - uint64 little-endian: path length
   *    - path bytes (UTF-8)
   *    - uint64 little-endian: content length
   *    - content bytes
   * 3. SHA-256 of the combined canonical byte stream.
   */
  async function computeContentDigest(files) {
    const sorted = [...files].sort((a, b) => a.path.localeCompare(b.path));
    
    let totalSize = 0;
    const items = [];
    for (const file of sorted) {
      const pathBytes = textToBytes(file.path.replace(/\\/g, "/"));
      const contentBytes = file.bytes instanceof Uint8Array ? file.bytes : new Uint8Array(file.bytes);
      items.push({ pathBytes, contentBytes });
      totalSize += 8 + pathBytes.length + 8 + contentBytes.length;
    }

    const canonical = new Uint8Array(totalSize);
    const view = new DataView(canonical.buffer);
    let offset = 0;

    for (const item of items) {
      // 8 bytes little-endian path length
      view.setBigUint64(offset, BigInt(item.pathBytes.length), true);
      offset += 8;
      canonical.set(item.pathBytes, offset);
      offset += item.pathBytes.length;

      // 8 bytes little-endian content length
      view.setBigUint64(offset, BigInt(item.contentBytes.length), true);
      offset += 8;
      canonical.set(item.contentBytes, offset);
      offset += item.contentBytes.length;
    }

    const hashBuffer = await crypto.subtle.digest("SHA-256", canonical);
    return hexString(hashBuffer);
  }

  /**
   * Native Browser ZIP Writer
   */
  class NstuZipWriter {
    constructor() {
      this.entries = [];
    }

    /**
     * Add a file to the archive.
     * @param {string} path - Package-relative path (e.g. "manifest.json", "exam/web/app.js")
     * @param {Uint8Array|ArrayBuffer|string} data - Content
     */
    add(path, data) {
      const normalizedPath = path.replace(/\\/g, "/").replace(/^\/+/, "");
      const bytes = (typeof data === "string") ? textToBytes(data) :
                    (data instanceof Uint8Array) ? data : new Uint8Array(data);
      this.entries.push({
        path: normalizedPath,
        bytes,
        crc: crc32(bytes),
        date: new Date()
      });
      return this;
    }

    /**
     * Generate the ZIP Blob.
     * Uses Store mode (method 0) which is universally compatible and instant.
     * @returns {Promise<{blob: Blob, archiveSha256: string, contentSha256: string, fileCount: number, totalBytes: number}>}
     */
    async build() {
      const localHeaders = [];
      const centralHeaders = [];
      let currentOffset = 0;

      for (const entry of this.entries) {
        const pathBytes = textToBytes(entry.path);
        const { dosTime, dosDate } = dosDateTime(entry.date);
        const uncompressedSize = entry.bytes.length;
        const compressedSize = uncompressedSize; // Store mode
        const method = 0; // Store

        // --- Local File Header (30 bytes + name) ---
        const localHeader = new Uint8Array(30 + pathBytes.length);
        const lView = new DataView(localHeader.buffer);
        lView.setUint32(0, 0x04034b50, true); // Local header signature
        lView.setUint16(4, 20, true);         // Version needed to extract (2.0)
        lView.setUint16(6, 0x0800, true);     // Flags (Bit 11 = UTF-8 filenames)
        lView.setUint16(8, method, true);     // Compression method (0 = Store)
        lView.setUint16(10, dosTime, true);
        lView.setUint16(12, dosDate, true);
        lView.setUint32(14, entry.crc, true); // CRC-32
        lView.setUint32(18, compressedSize, true);
        lView.setUint32(22, uncompressedSize, true);
        lView.setUint16(26, pathBytes.length, true);
        lView.setUint16(28, 0, true);         // Extra field length
        localHeader.set(pathBytes, 30);

        localHeaders.push({
          header: localHeader,
          data: entry.bytes,
          offset: currentOffset
        });

        currentOffset += localHeader.length + entry.bytes.length;

        // --- Central Directory File Header (46 bytes + name) ---
        const centralHeader = new Uint8Array(46 + pathBytes.length);
        const cView = new DataView(centralHeader.buffer);
        cView.setUint32(0, 0x02014b50, true); // Central header signature
        cView.setUint16(4, 20, true);         // Version made by
        cView.setUint16(6, 20, true);         // Version needed
        cView.setUint16(8, 0x0800, true);     // UTF-8 flag
        cView.setUint16(10, method, true);    // Method
        cView.setUint16(12, dosTime, true);
        cView.setUint16(14, dosDate, true);
        cView.setUint32(16, entry.crc, true);
        cView.setUint32(20, compressedSize, true);
        cView.setUint32(24, uncompressedSize, true);
        cView.setUint16(28, pathBytes.length, true);
        cView.setUint16(30, 0, true);         // Extra field length
        cView.setUint16(32, 0, true);         // Comment length
        cView.setUint16(34, 0, true);         // Disk number start
        cView.setUint16(36, 0, true);         // Internal attributes
        cView.setUint32(38, 0, true);         // External attributes
        cView.setUint32(42, localHeaders[localHeaders.length - 1].offset, true); // Local header offset
        centralHeader.set(pathBytes, 46);

        centralHeaders.push(centralHeader);
      }

      const centralDirOffset = currentOffset;
      let centralDirSize = 0;
      for (const h of centralHeaders) centralDirSize += h.length;

      // --- End of Central Directory Record (22 bytes) ---
      const eocd = new Uint8Array(22);
      const eView = new DataView(eocd.buffer);
      eView.setUint32(0, 0x06054b50, true); // EOCD signature
      eView.setUint16(4, 0, true);          // Disk number
      eView.setUint16(6, 0, true);          // Start disk
      eView.setUint16(8, this.entries.length, true);  // Entries on this disk
      eView.setUint16(10, this.entries.length, true); // Total entries
      eView.setUint32(12, centralDirSize, true);      // Central directory size
      eView.setUint32(16, centralDirOffset, true);    // Central directory offset
      eView.setUint16(20, 0, true);         // Comment length

      // Assemble all pieces into a single Blob
      const parts = [];
      for (const lh of localHeaders) {
        parts.push(lh.header);
        parts.push(lh.data);
      }
      for (const ch of centralHeaders) {
        parts.push(ch);
      }
      parts.push(eocd);

      const blob = new Blob(parts, { type: "application/x-nstuexam" });
      const arrayBuffer = await blob.arrayBuffer();

      // Compute Archive SHA-256
      const archiveHash = await crypto.subtle.digest("SHA-256", arrayBuffer);
      const archiveSha256 = hexString(archiveHash);

      // Compute Content SHA-256 matching ExamHost
      const contentSha256 = await computeContentDigest(this.entries);

      return {
        blob,
        archiveSha256,
        contentSha256,
        fileCount: this.entries.length,
        totalBytes: blob.size
      };
    }
  }

  /**
   * Native Browser ZIP Reader (for importing existing .nstuexam packages)
   */
  class NstuZipReader {
    constructor(arrayBuffer) {
      this.buffer = arrayBuffer;
      this.view = new DataView(arrayBuffer);
    }

    _findEOCD() {
      // Search backwards from the end of the file for 0x06054b50
      const maxSearch = Math.min(this.buffer.byteLength, 65557);
      for (let i = this.buffer.byteLength - 22; i >= this.buffer.byteLength - maxSearch; i--) {
        if (this.view.getUint32(i, true) === 0x06054b50) {
          return i;
        }
      }
      return -1;
    }

    async readEntries() {
      const eocdOffset = this._findEOCD();
      if (eocdOffset === -1) {
        throw new Error("Invalid .nstuexam package: End of Central Directory record not found.");
      }

      const totalEntries = this.view.getUint16(eocdOffset + 10, true);
      const centralDirOffset = this.view.getUint32(eocdOffset + 16, true);

      let offset = centralDirOffset;
      const entries = [];

      for (let i = 0; i < totalEntries; i++) {
        if (this.view.getUint32(offset, true) !== 0x02014b50) {
          throw new Error("Invalid Central Directory header at offset " + offset);
        }

        const method = this.view.getUint16(offset + 10, true);
        const compressedSize = this.view.getUint32(offset + 20, true);
        const uncompressedSize = this.view.getUint32(offset + 24, true);
        const nameLength = this.view.getUint16(offset + 28, true);
        const extraLength = this.view.getUint16(offset + 30, true);
        const commentLength = this.view.getUint16(offset + 32, true);
        const localOffset = this.view.getUint32(offset + 42, true);

        const nameBytes = new Uint8Array(this.buffer, offset + 46, nameLength);
        const filename = bytesToText(nameBytes);

        // Advance to next central entry
        offset += 46 + nameLength + extraLength + commentLength;

        // Skip directory records
        if (filename.endsWith("/")) continue;

        // Read Local Header to find exact data start
        if (this.view.getUint32(localOffset, true) !== 0x04034b50) {
          throw new Error("Invalid Local File header for: " + filename);
        }
        const localNameLength = this.view.getUint16(localOffset + 26, true);
        const localExtraLength = this.view.getUint16(localOffset + 28, true);
        const dataOffset = localOffset + 30 + localNameLength + localExtraLength;

        const rawData = new Uint8Array(this.buffer, dataOffset, compressedSize);
        let uncompressedData;

        if (method === 0) {
          // Store
          uncompressedData = rawData.slice();
        } else if (method === 8) {
          // Deflate (inflate with browser DecompressionStream)
          if (typeof DecompressionStream === "function") {
            const stream = new Response(rawData).body.pipeThrough(new DecompressionStream("deflate-raw"));
            const res = await new Response(stream).arrayBuffer();
            uncompressedData = new Uint8Array(res);
          } else {
            throw new Error("DecompressionStream is not supported in this browser.");
          }
        } else {
          throw new Error(`Unsupported compression method ${method} for ${filename}`);
        }

        entries.push({
          path: filename,
          bytes: uncompressedData,
          size: uncompressedSize
        });
      }

      return entries;
    }
  }

  // Export to global scope
  global.NstuZip = {
    Writer: NstuZipWriter,
    Reader: NstuZipReader,
    computeContentDigest
  };

})(window);
