// Praia archive plugin — tar.gz and zip support
// Requires zlib (-lz)

#include "praia_plugin.h"
#include <zlib.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

PRAIA_DECLARE_ABI();
PRAIA_PLUGIN_METADATA("archive", "0.2.0",
                     "tar.gz and zip archive support");

// Bail out of a long pack/unpack loop if the surrounding withCancel
// scope's token has been cancelled. Returns true if cancelled; the
// caller throws to surface the bail-out to user code. Polled once
// per archive entry (typical archives have hundreds-to-thousands of
// entries, not millions, so an unconditional check beats amortising).
static bool archiveCancelled() {
    auto c = praia::shouldCancel();
    return c && *c;
}

// ── Tar format helpers ──

struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char padding[12];
};

static_assert(sizeof(TarHeader) == 512, "TarHeader must be 512 bytes");

static void octal(char* dst, size_t len, uint64_t val) {
    snprintf(dst, len, "%0*llo", static_cast<int>(len - 1), static_cast<unsigned long long>(val));
}

static uint64_t parseOctal(const char* s, size_t len) {
    uint64_t val = 0;
    for (size_t i = 0; i < len && s[i] >= '0' && s[i] <= '7'; i++)
        val = val * 8 + (s[i] - '0');
    return val;
}

static uint32_t tarChecksum(const TarHeader& h) {
    uint32_t sum = 0;
    auto* p = reinterpret_cast<const unsigned char*>(&h);
    for (int i = 0; i < 512; i++) sum += p[i];
    return sum;
}

static std::string readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw RuntimeError("Cannot read file: " + path, 0);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static void writeFile(const std::string& path, const std::string& data) {
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream f(path, std::ios::binary);
    if (!f) throw RuntimeError("Cannot write file: " + path, 0);
    f.write(data.data(), data.size());
}

// Validate that an extracted filename doesn't escape the output directory
static std::string safePath(const std::string& outDir, const std::string& name) {
    // Reject absolute paths and parent traversal
    if (name.empty() || name[0] == '/' || name.find("..") != std::string::npos)
        throw RuntimeError("archive: unsafe path in archive: " + name, 0);
    auto full = fs::path(outDir) / name;
    // Normalize and verify it's still under outDir
    auto canonical_out = fs::weakly_canonical(outDir).string();
    auto canonical_full = fs::weakly_canonical(full).string();
    if (canonical_full.rfind(canonical_out, 0) != 0)
        throw RuntimeError("archive: path escapes output directory: " + name, 0);
    return canonical_full;
}

// ── Gzip compress/decompress ──

static std::string gzipCompress(const std::string& data) {
    z_stream zs = {};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw RuntimeError("gzip init failed", 0);

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::string out;
    char buf[32768];
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        deflate(&zs, Z_FINISH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (zs.avail_out == 0);

    deflateEnd(&zs);
    return out;
}

static std::string gzipDecompress(const std::string& data) {
    z_stream zs = {};
    if (inflateInit2(&zs, 15 + 16) != Z_OK)
        throw RuntimeError("gzip decompress init failed", 0);

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::string out;
    char buf[32768];
    int ret;
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        ret = inflate(&zs, Z_NO_FLUSH);
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&zs);
            throw RuntimeError("gzip decompress failed", 0);
        }
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (ret != Z_STREAM_END);

    inflateEnd(&zs);
    return out;
}

// ── Tar create ──

static std::string createTar(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string tar;

    for (auto& [name, content] : files) {
        TarHeader h = {};
        std::memset(&h, 0, sizeof(h));

        // Handle long names with prefix
        if (name.size() <= 100) {
            std::strncpy(h.name, name.c_str(), 100);
        } else if (name.size() <= 255) {
            auto slash = name.rfind('/', 154);
            if (slash != std::string::npos) {
                std::strncpy(h.prefix, name.substr(0, slash).c_str(), 155);
                std::strncpy(h.name, name.substr(slash + 1).c_str(), 100);
            } else {
                std::strncpy(h.name, name.c_str(), 100);
            }
        } else {
            std::strncpy(h.name, name.substr(0, 100).c_str(), 100);
        }

        octal(h.mode, sizeof(h.mode), 0644);
        octal(h.uid, sizeof(h.uid), 0);
        octal(h.gid, sizeof(h.gid), 0);
        octal(h.size, sizeof(h.size), content.size());
        octal(h.mtime, sizeof(h.mtime), static_cast<uint64_t>(std::time(nullptr)));
        h.typeflag = '0'; // regular file
        std::memcpy(h.magic, "ustar", 5);
        h.magic[5] = '\0';
        h.version[0] = '0';
        h.version[1] = '0';

        // Compute checksum (fill with spaces first)
        std::memset(h.checksum, ' ', 8);
        uint32_t cksum = tarChecksum(h);
        snprintf(h.checksum, 7, "%06o", cksum);
        h.checksum[6] = '\0';

        tar.append(reinterpret_cast<const char*>(&h), 512);
        tar.append(content);

        // Pad to 512 boundary
        size_t pad = (512 - (content.size() % 512)) % 512;
        tar.append(pad, '\0');
    }

    // Two zero blocks to end
    tar.append(1024, '\0');
    return tar;
}

// ── Tar extract ──

struct TarEntry {
    std::string name;
    std::string content;
};

static std::vector<TarEntry> extractTar(const std::string& data) {
    std::vector<TarEntry> entries;
    size_t pos = 0;

    while (pos + 512 <= data.size()) {
        auto* h = reinterpret_cast<const TarHeader*>(data.data() + pos);

        // Check for end-of-archive (all zeros)
        bool allZero = true;
        for (int i = 0; i < 512; i++) {
            if (data[pos + i] != '\0') { allZero = false; break; }
        }
        if (allZero) break;

        std::string name;
        if (h->prefix[0] != '\0') {
            name = std::string(h->prefix, strnlen(h->prefix, 155)) + "/" +
                   std::string(h->name, strnlen(h->name, 100));
        } else {
            name = std::string(h->name, strnlen(h->name, 100));
        }

        uint64_t size = parseOctal(h->size, 12);
        pos += 512;

        if (h->typeflag == '0' || h->typeflag == '\0') {
            std::string content;
            if (size > 0 && pos + size <= data.size()) {
                content = data.substr(pos, size);
            }
            entries.push_back({name, content});
        }

        pos += size;
        pos += (512 - (size % 512)) % 512; // skip padding
    }

    return entries;
}

// ── Zip format helpers ──

static void writeLE16(std::string& out, uint16_t val) {
    out += static_cast<char>(val & 0xFF);
    out += static_cast<char>((val >> 8) & 0xFF);
}

static void writeLE32(std::string& out, uint32_t val) {
    out += static_cast<char>(val & 0xFF);
    out += static_cast<char>((val >> 8) & 0xFF);
    out += static_cast<char>((val >> 16) & 0xFF);
    out += static_cast<char>((val >> 24) & 0xFF);
}

static uint16_t readLE16(const char* p) {
    return static_cast<uint16_t>(
        static_cast<unsigned char>(p[0]) |
        (static_cast<unsigned char>(p[1]) << 8));
}

static uint32_t readLE32(const char* p) {
    return static_cast<uint32_t>(
        static_cast<unsigned char>(p[0]) |
        (static_cast<unsigned char>(p[1]) << 8) |
        (static_cast<unsigned char>(p[2]) << 16) |
        (static_cast<unsigned char>(p[3]) << 24));
}

static std::string deflateCompress(const std::string& data) {
    z_stream zs = {};
    if (deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -15, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        throw RuntimeError("deflate init failed", 0);

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::string out;
    char buf[32768];
    do {
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = sizeof(buf);
        deflate(&zs, Z_FINISH);
        out.append(buf, sizeof(buf) - zs.avail_out);
    } while (zs.avail_out == 0);

    deflateEnd(&zs);
    return out;
}

static std::string deflateDecompress(const std::string& data, size_t uncompSize) {
    z_stream zs = {};
    if (inflateInit2(&zs, -15) != Z_OK)
        throw RuntimeError("inflate init failed", 0);

    zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    zs.avail_in = static_cast<uInt>(data.size());

    std::string out(uncompSize, '\0');
    zs.next_out = reinterpret_cast<Bytef*>(&out[0]);
    zs.avail_out = static_cast<uInt>(uncompSize);

    int ret = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    if (ret != Z_STREAM_END)
        throw RuntimeError("inflate failed", 0);

    return out;
}

// ── Zip builder (shared by zipCreate and zipPack) ──

struct ZipFileEntry {
    std::string name, content, compressed;
    uint32_t crc, offset;
};

static std::string buildZip(std::vector<ZipFileEntry>& entries) {
    std::string zip;

    for (auto& e : entries) {
        e.offset = static_cast<uint32_t>(zip.size());
        writeLE32(zip, 0x04034b50);
        writeLE16(zip, 20);        // version needed
        writeLE16(zip, 0);         // flags
        writeLE16(zip, 8);         // compression: deflate
        writeLE16(zip, 0);         // mod time
        writeLE16(zip, 0);         // mod date
        writeLE32(zip, e.crc);
        writeLE32(zip, static_cast<uint32_t>(e.compressed.size()));
        writeLE32(zip, static_cast<uint32_t>(e.content.size()));
        writeLE16(zip, static_cast<uint16_t>(e.name.size()));
        writeLE16(zip, 0);         // extra field length
        zip.append(e.name);
        zip.append(e.compressed);
    }

    uint32_t cdOffset = static_cast<uint32_t>(zip.size());
    for (auto& e : entries) {
        writeLE32(zip, 0x02014b50);
        writeLE16(zip, 20);         // version made by
        writeLE16(zip, 20);         // version needed
        writeLE16(zip, 0);          // flags
        writeLE16(zip, 8);          // compression
        writeLE16(zip, 0);          // mod time
        writeLE16(zip, 0);          // mod date
        writeLE32(zip, e.crc);
        writeLE32(zip, static_cast<uint32_t>(e.compressed.size()));
        writeLE32(zip, static_cast<uint32_t>(e.content.size()));
        writeLE16(zip, static_cast<uint16_t>(e.name.size()));
        writeLE16(zip, 0);          // extra field length
        writeLE16(zip, 0);          // comment length
        writeLE16(zip, 0);          // disk number
        writeLE16(zip, 0);          // internal attrs
        writeLE32(zip, 0);          // external attrs
        writeLE32(zip, e.offset);
        zip.append(e.name);
    }
    uint32_t cdSize = static_cast<uint32_t>(zip.size()) - cdOffset;

    writeLE32(zip, 0x06054b50);
    writeLE16(zip, 0);              // disk number
    writeLE16(zip, 0);              // cd disk
    writeLE16(zip, static_cast<uint16_t>(entries.size()));
    writeLE16(zip, static_cast<uint16_t>(entries.size()));
    writeLE32(zip, cdSize);
    writeLE32(zip, cdOffset);
    writeLE16(zip, 0);              // comment length

    return zip;
}

static std::vector<ZipFileEntry> collectZipEntries(const std::shared_ptr<PraiaArray>& arr) {
    std::vector<ZipFileEntry> entries;
    for (auto& item : arr->elements) {
        if (!item.isMap())
            throw RuntimeError("archive: each item must be a map", 0);
        auto& m = item.asMap()->entries;
        auto nameIt = m.find("name");
        if (nameIt == m.end() || !nameIt->second.isString())
            throw RuntimeError("archive: each item must have a 'name'", 0);

        ZipFileEntry ze;
        ze.name = nameIt->second.asString();

        auto contentIt = m.find("content");
        auto pathIt = m.find("path");
        if (contentIt != m.end() && contentIt->second.isString()) {
            ze.content = contentIt->second.asString();
        } else if (pathIt != m.end() && pathIt->second.isString()) {
            ze.content = readFile(pathIt->second.asString());
        } else {
            throw RuntimeError("archive: each item needs 'content' or 'path'", 0);
        }

        ze.crc = crc32(0L, reinterpret_cast<const Bytef*>(ze.content.data()),
                       static_cast<uInt>(ze.content.size()));
        ze.compressed = deflateCompress(ze.content);
        entries.push_back(std::move(ze));
    }
    return entries;
}

// ── Plugin registration ──

extern "C" void praia_register(PraiaMap* module) {

    // archive.tarCreate(files) — create a .tar.gz from array of {name, content} or {name, path}
    module->entries["tarCreate"] = Value(makeNative("archive.tarCreate", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("archive.tarCreate() requires an array of {name, content} or {name, path}", 0);

            std::vector<std::pair<std::string, std::string>> files;
            for (auto& item : args[0].asArray()->elements) {
                if (!item.isMap())
                    throw RuntimeError("archive.tarCreate(): each item must be a map with 'name'", 0);
                auto& m = item.asMap()->entries;
                auto nameIt = m.find("name");
                if (nameIt == m.end() || !nameIt->second.isString())
                    throw RuntimeError("archive.tarCreate(): each item must have a 'name' string", 0);

                std::string name = nameIt->second.asString();
                std::string content;

                auto contentIt = m.find("content");
                auto pathIt = m.find("path");
                if (contentIt != m.end() && contentIt->second.isString()) {
                    content = contentIt->second.asString();
                } else if (pathIt != m.end() && pathIt->second.isString()) {
                    content = readFile(pathIt->second.asString());
                } else {
                    throw RuntimeError("archive.tarCreate(): each item needs 'content' or 'path'", 0);
                }
                files.push_back({name, content});
            }

            std::string tar = createTar(files);
            return Value(gzipCompress(tar));
        }));

    // archive.tarExtract(data) — extract a .tar.gz, returns array of {name, content}
    module->entries["tarExtract"] = Value(makeNative("archive.tarExtract", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("archive.tarExtract() requires a string (tar.gz data)", 0);

            std::string tar = gzipDecompress(args[0].asString());
            auto entries = extractTar(tar);

            auto result = gcNew<PraiaArray>();
            for (auto& e : entries) {
                auto entry = gcNew<PraiaMap>();
                entry->entries["name"] = Value(e.name);
                entry->entries["content"] = Value(e.content);
                result->elements.push_back(Value(entry));
            }
            return Value(result);
        }));

    // archive.tarPack(dir, outPath) — pack a directory into a .tar.gz file
    module->entries["tarPack"] = Value(makeNative("archive.tarPack", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("archive.tarPack() requires (directory, outputPath)", 0);

            auto& dir = args[0].asString();
            auto& outPath = args[1].asString();

            if (!fs::is_directory(dir))
                throw RuntimeError("archive.tarPack(): not a directory: " + dir, 0);

            std::vector<std::pair<std::string, std::string>> files;
            for (auto& entry : fs::recursive_directory_iterator(dir)) {
                if (archiveCancelled())
                    throw RuntimeError("archive.tarPack: cancelled", 0);
                if (!entry.is_regular_file()) continue;
                auto rel = fs::relative(entry.path(), dir).string();
                files.push_back({rel, readFile(entry.path().string())});
            }

            std::string tar = createTar(files);
            std::string gz = gzipCompress(tar);
            writeFile(outPath, gz);
            return Value(static_cast<int64_t>(files.size()));
        }));

    // archive.tarUnpack(tarPath, outDir) — extract a .tar.gz file to a directory
    module->entries["tarUnpack"] = Value(makeNative("archive.tarUnpack", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("archive.tarUnpack() requires (tarPath, outputDir)", 0);

            std::string gz = readFile(args[0].asString());
            std::string tar = gzipDecompress(gz);
            auto entries = extractTar(tar);

            auto& outDir = args[1].asString();
            for (auto& e : entries) {
                if (archiveCancelled())
                    throw RuntimeError("archive.tarUnpack: cancelled", 0);
                writeFile(safePath(outDir, e.name), e.content);
            }
            return Value(static_cast<int64_t>(entries.size()));
        }));

    // archive.zipCreate(files) — create a .zip from array of {name, content} or {name, path}
    module->entries["zipCreate"] = Value(makeNative("archive.zipCreate", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isArray())
                throw RuntimeError("archive.zipCreate() requires an array", 0);
            auto entries = collectZipEntries(args[0].asArray());
            return Value(buildZip(entries));
        }));

    // archive.zipExtract(data) — extract a .zip, returns array of {name, content}
    module->entries["zipExtract"] = Value(makeNative("archive.zipExtract", 1,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString())
                throw RuntimeError("archive.zipExtract() requires a string (zip data)", 0);

            auto& data = args[0].asString();
            auto result = gcNew<PraiaArray>();

            size_t pos = 0;
            while (pos + 30 <= data.size()) {
                if (archiveCancelled())
                    throw RuntimeError("archive.zipExtract: cancelled", 0);
                uint32_t sig = readLE32(data.data() + pos);
                if (sig != 0x04034b50) break; // not a local file header

                uint16_t method = readLE16(data.data() + pos + 8);
                uint32_t compSize = readLE32(data.data() + pos + 18);
                uint32_t uncompSize = readLE32(data.data() + pos + 22);
                uint16_t nameLen = readLE16(data.data() + pos + 26);
                uint16_t extraLen = readLE16(data.data() + pos + 28);

                std::string name(data.data() + pos + 30, nameLen);
                pos += 30 + nameLen + extraLen;

                std::string content;
                if (method == 0) {
                    content = data.substr(pos, uncompSize);
                } else if (method == 8) {
                    content = deflateDecompress(data.substr(pos, compSize), uncompSize);
                }
                pos += compSize;

                if (!name.empty() && name.back() != '/') {
                    auto entry = gcNew<PraiaMap>();
                    entry->entries["name"] = Value(name);
                    entry->entries["content"] = Value(content);
                    result->elements.push_back(Value(entry));
                }
            }
            return Value(result);
        }));

    // archive.zipPack(dir, outPath) — pack a directory into a .zip file
    module->entries["zipPack"] = Value(makeNative("archive.zipPack", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("archive.zipPack() requires (directory, outputPath)", 0);

            auto& dir = args[0].asString();
            auto& outPath = args[1].asString();

            if (!fs::is_directory(dir))
                throw RuntimeError("archive.zipPack(): not a directory: " + dir, 0);

            std::vector<ZipFileEntry> entries;
            for (auto& entry : fs::recursive_directory_iterator(dir)) {
                if (archiveCancelled())
                    throw RuntimeError("archive.zipPack: cancelled", 0);
                if (!entry.is_regular_file()) continue;
                ZipFileEntry ze;
                ze.name = fs::relative(entry.path(), dir).string();
                ze.content = readFile(entry.path().string());
                ze.crc = crc32(0L, reinterpret_cast<const Bytef*>(ze.content.data()),
                               static_cast<uInt>(ze.content.size()));
                ze.compressed = deflateCompress(ze.content);
                entries.push_back(std::move(ze));
            }

            writeFile(outPath, buildZip(entries));
            return Value(static_cast<int64_t>(entries.size()));
        }));

    // archive.zipUnpack(zipPath, outDir) — extract a .zip file to a directory
    module->entries["zipUnpack"] = Value(makeNative("archive.zipUnpack", 2,
        [](const std::vector<Value>& args) -> Value {
            if (!args[0].isString() || !args[1].isString())
                throw RuntimeError("archive.zipUnpack() requires (zipPath, outputDir)", 0);

            std::string data = readFile(args[0].asString());
            auto& outDir = args[1].asString();
            int count = 0;

            size_t pos = 0;
            while (pos + 30 <= data.size()) {
                if (archiveCancelled())
                    throw RuntimeError("archive.zipUnpack: cancelled", 0);
                uint32_t sig = readLE32(data.data() + pos);
                if (sig != 0x04034b50) break;

                uint16_t method = readLE16(data.data() + pos + 8);
                uint32_t compSize = readLE32(data.data() + pos + 18);
                uint32_t uncompSize = readLE32(data.data() + pos + 22);
                uint16_t nameLen = readLE16(data.data() + pos + 26);
                uint16_t extraLen = readLE16(data.data() + pos + 28);

                std::string name(data.data() + pos + 30, nameLen);
                pos += 30 + nameLen + extraLen;

                if (!name.empty() && name.back() != '/') {
                    std::string content;
                    if (method == 0) {
                        content = data.substr(pos, uncompSize);
                    } else if (method == 8) {
                        content = deflateDecompress(data.substr(pos, compSize), uncompSize);
                    }
                    writeFile(safePath(outDir, name), content);
                    count++;
                }
                pos += compSize;
            }
            return Value(static_cast<int64_t>(count));
        }));
}
