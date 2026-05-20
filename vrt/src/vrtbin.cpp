/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * @file vrtbin.cpp
 * @brief Vrtbin archive extraction and metadata discovery.
 */

#include <vrt/vrtbin.hpp>

#include <tar.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <system_error>

namespace vrt {

namespace {

constexpr std::size_t TAR_BLOCK_SIZE = 512;
constexpr char TAR_LONGNAME_TYPE = 'L';

struct TarHeader {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char chksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
};
static_assert(sizeof(TarHeader) == TAR_BLOCK_SIZE, "Invalid tar header size");

bool isZeroBlock(const std::array<char, TAR_BLOCK_SIZE>& block) {
    return std::all_of(block.begin(), block.end(), [](char c) { return c == '\0'; });
}

bool hasGzipMagic(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    std::array<unsigned char, 2> magic{};
    file.read(reinterpret_cast<char*>(magic.data()), static_cast<std::streamsize>(magic.size()));
    return file.gcount() == static_cast<std::streamsize>(magic.size()) && magic[0] == 0x1Fu &&
           magic[1] == 0x8Bu;
}

uint64_t parseOctal(const char* field, std::size_t len) {
    uint64_t value = 0;
    bool seenDigit = false;
    for (std::size_t i = 0; i < len; ++i) {
        const unsigned char c = static_cast<unsigned char>(field[i]);
        if (c == '\0' || c == ' ') {
            if (seenDigit) {
                break;
            }
            continue;
        }
        if (c < '0' || c > '7') {
            break;
        }
        seenDigit = true;
        value = (value << 3) | static_cast<uint64_t>(c - '0');
    }
    return value;
}

std::string readField(const char* field, std::size_t len) {
    std::size_t n = 0;
    while (n < len && field[n] != '\0') {
        ++n;
    }
    return std::string(field, field + n);
}

void streamSkip(std::istream& stream, uint64_t size) {
    static constexpr std::streamsize CHUNK = 1 << 20;
    while (size > 0) {
        const std::streamsize chunk =
            static_cast<std::streamsize>(std::min<uint64_t>(size, static_cast<uint64_t>(CHUNK)));
        stream.ignore(chunk);
        if (stream.gcount() != chunk) {
            throw std::runtime_error("Unexpected EOF while skipping tar payload");
        }
        size -= static_cast<uint64_t>(chunk);
    }
}

void streamCopy(std::istream& src, std::ostream& dst, uint64_t size) {
    std::array<char, 1 << 16> buffer{};
    while (size > 0) {
        const std::size_t chunk = static_cast<std::size_t>(
            std::min<uint64_t>(size, static_cast<uint64_t>(buffer.size())));
        src.read(buffer.data(), static_cast<std::streamsize>(chunk));
        if (src.gcount() != static_cast<std::streamsize>(chunk)) {
            throw std::runtime_error("Unexpected EOF while reading tar entry");
        }
        dst.write(buffer.data(), static_cast<std::streamsize>(chunk));
        if (!dst) {
            throw std::runtime_error("Failed writing extracted tar entry");
        }
        size -= static_cast<uint64_t>(chunk);
    }
}

void decompressGzipToTar(const std::string& gzipPath, const std::filesystem::path& tarPath) {
    gzFile gz = gzopen(gzipPath.c_str(), "rb");
    if (gz == nullptr) {
        throw std::runtime_error("Cannot open gzip archive: " + gzipPath);
    }

    std::ofstream out(tarPath, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        gzclose(gz);
        throw std::runtime_error("Failed to create temporary tar stream: " + tarPath.string());
    }

    std::array<char, 1 << 16> buffer{};
    while (true) {
        const int bytesRead = gzread(gz, buffer.data(), static_cast<unsigned int>(buffer.size()));
        if (bytesRead > 0) {
            out.write(buffer.data(), static_cast<std::streamsize>(bytesRead));
            if (!out) {
                gzclose(gz);
                throw std::runtime_error("Failed writing temporary tar stream: " +
                                         tarPath.string());
            }
            continue;
        }
        if (bytesRead == 0) {
            break;
        }

        int zerr = Z_OK;
        const char* zmsg = gzerror(gz, &zerr);
        const std::string err = zmsg == nullptr ? "unknown gzip error" : zmsg;
        gzclose(gz);
        throw std::runtime_error("Failed to decompress gzip archive: " + err);
    }

    const int closeRc = gzclose(gz);
    if (closeRc != Z_OK) {
        throw std::runtime_error("Failed to finalize gzip decompression");
    }
}

bool hasValidTarChecksum(const std::array<char, TAR_BLOCK_SIZE>& raw) {
    TarHeader header{};
    std::memcpy(&header, raw.data(), sizeof(header));
    const uint64_t expected = parseOctal(header.chksum, sizeof(header.chksum));

    uint64_t actual = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (i >= 148 && i < 156) {
            actual += static_cast<unsigned char>(' ');
        } else {
            actual += static_cast<unsigned char>(raw[i]);
        }
    }
    return expected == actual;
}

std::filesystem::path sanitizeArchivePath(const std::string& entryName) {
    std::filesystem::path path(entryName);
    path = path.lexically_normal();
    if (path.empty() || path == ".") {
        return {};
    }
    if (path.is_absolute()) {
        throw std::runtime_error("Tar archive contains absolute path entry: " + entryName);
    }
    for (const auto& part : path) {
        if (part == "..") {
            throw std::runtime_error("Tar archive contains parent path traversal: " + entryName);
        }
    }
    return path;
}

bool isRegularTarType(char typeflag) {
    return typeflag == REGTYPE || typeflag == AREGTYPE || typeflag == '\0';
}

std::filesystem::perms tarModeToPerms(uint64_t mode) {
    std::filesystem::perms perms = std::filesystem::perms::none;
    if ((mode & 0400u) != 0u) perms |= std::filesystem::perms::owner_read;
    if ((mode & 0200u) != 0u) perms |= std::filesystem::perms::owner_write;
    if ((mode & 0100u) != 0u) perms |= std::filesystem::perms::owner_exec;
    if ((mode & 0040u) != 0u) perms |= std::filesystem::perms::group_read;
    if ((mode & 0020u) != 0u) perms |= std::filesystem::perms::group_write;
    if ((mode & 0010u) != 0u) perms |= std::filesystem::perms::group_exec;
    if ((mode & 0004u) != 0u) perms |= std::filesystem::perms::others_read;
    if ((mode & 0002u) != 0u) perms |= std::filesystem::perms::others_write;
    if ((mode & 0001u) != 0u) perms |= std::filesystem::perms::others_exec;
    if ((mode & 04000u) != 0u) perms |= std::filesystem::perms::set_uid;
    if ((mode & 02000u) != 0u) perms |= std::filesystem::perms::set_gid;
    if ((mode & 01000u) != 0u) perms |= std::filesystem::perms::sticky_bit;
    return perms;
}

}  // namespace

Vrtbin::Vrtbin(std::string vrtbinPath, const std::string& bdf) {
    this->vrtbinPath = vrtbinPath;
    if (!std::filesystem::exists(vrtbinPath)) {
        throw std::runtime_error(vrtbinPath + " does not exist");
    }

    const std::filesystem::path metadataPath =
        FilesystemCache::getCachePath() / ("metadata_" + sanitizeForPath(bdf));
    std::error_code metadataEc;
    std::filesystem::create_directories(metadataPath, metadataEc);
    if (metadataEc) {
        throw std::runtime_error("Failed to initialize metadata path: " + metadataPath.string());
    }

    this->tempExtractPath =
        (FilesystemCache::getCachePath() / ("vrtbin_" + sanitizeForPath(bdf))).string();
    this->systemMapPath = (metadataPath / "system_map.xml").string();

    extract();
    discoverPdiFiles();

    const std::filesystem::path tempSystemMapPath = findExtractedFile("system_map.xml");
    if (tempSystemMapPath.empty()) {
        throw std::runtime_error("system_map.xml not found in tar archive: " + vrtbinPath);
    }
    XMLParser parser(tempSystemMapPath.string());
    parser.parseXML();
    this->platform = parser.getPlatform();
    copy(tempSystemMapPath.string(), systemMapPath);

    const std::filesystem::path reportPath =
        findExtractedFileByPrefix("report_utilization", ".xml");
    if (!reportPath.empty()) {
        utilizationReportPath = (metadataPath / "report_utilization.xml").string();
        copy(reportPath.string(), utilizationReportPath);
    }

    if (this->platform == Platform::HARDWARE) {
        if (pdiPaths.empty()) {
            throw std::runtime_error("No .pdi files found in tar archive: " + vrtbinPath);
        }
    } else if (this->platform == Platform::EMULATION) {
        const std::filesystem::path emuPath = findExtractedFile("vpp_emu");
        emulationExecPath = emuPath.empty() ? std::string() : emuPath.string();
        const std::filesystem::path emuManifestPath = findExtractedFile("emu_manifest.json");
        if (!emuManifestPath.empty()) {
            emulationManifestPath = (metadataPath / "emu_manifest.json").string();
            copy(emuManifestPath.string(), emulationManifestPath);
        }

    } else {
        const std::filesystem::path simPath = findExtractedFile("vpp_sim");
        simulationExecPath = simPath.empty() ? std::string() : simPath.string();
    }
}

void Vrtbin::extract() {
    utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__, "Extracting vrtbin: {}",
                       vrtbinPath);
    std::error_code ec;
    std::filesystem::remove_all(tempExtractPath, ec);
    std::filesystem::create_directories(tempExtractPath, ec);
    if (ec) {
        throw std::runtime_error("Failed to initialize extraction path: " + tempExtractPath);
    }

    std::filesystem::path archivePath = vrtbinPath;
    bool cleanupArchivePath = false;
    if (hasGzipMagic(vrtbinPath)) {
        archivePath = std::filesystem::path(tempExtractPath).parent_path() /
                      (std::filesystem::path(tempExtractPath).filename().string() + ".tmp.tar");
        cleanupArchivePath = true;
        try {
            decompressGzipToTar(vrtbinPath, archivePath);
        } catch (...) {
            std::error_code cleanupEc;
            std::filesystem::remove(archivePath, cleanupEc);
            throw;
        }
    }
    auto cleanupArchive = [&]() {
        if (!cleanupArchivePath) {
            return;
        }
        std::error_code cleanupEc;
        std::filesystem::remove(archivePath, cleanupEc);
    };

    std::ifstream archive(archivePath, std::ios::binary);
    if (!archive.is_open()) {
        cleanupArchive();
        throw std::runtime_error("Cannot open tar archive: " + archivePath.string());
    }

    try {
        std::string pendingLongName;
        while (true) {
            std::array<char, TAR_BLOCK_SIZE> raw{};
            archive.read(raw.data(), static_cast<std::streamsize>(raw.size()));
            if (archive.gcount() == 0) {
                break;
            }
            if (archive.gcount() != static_cast<std::streamsize>(raw.size())) {
                throw std::runtime_error("Invalid tar archive: truncated header");
            }
            if (isZeroBlock(raw)) {
                break;
            }
            if (!hasValidTarChecksum(raw)) {
                throw std::runtime_error("Invalid tar archive: header checksum mismatch");
            }

            TarHeader header{};
            std::memcpy(&header, raw.data(), sizeof(header));

            uint64_t payloadSize = parseOctal(header.size, sizeof(header.size));
            const uint64_t headerSize = payloadSize;
            char typeflag = header.typeflag;
            std::string entryName;
            if (!pendingLongName.empty()) {
                entryName = pendingLongName;
                pendingLongName.clear();
            } else {
                std::string name = readField(header.name, sizeof(header.name));
                std::string prefix = readField(header.prefix, sizeof(header.prefix));
                entryName = prefix.empty() ? name : (prefix + "/" + name);
            }

            if (typeflag == TAR_LONGNAME_TYPE) {
                std::string longName(payloadSize, '\0');
                if (payloadSize > 0) {
                    archive.read(longName.data(), static_cast<std::streamsize>(payloadSize));
                    if (archive.gcount() != static_cast<std::streamsize>(payloadSize)) {
                        throw std::runtime_error("Invalid tar archive: truncated long name");
                    }
                }
                std::size_t nul = longName.find('\0');
                if (nul != std::string::npos) {
                    longName.resize(nul);
                }
                pendingLongName = longName;
                payloadSize = 0;
            } else {
                const std::filesystem::path relPath = sanitizeArchivePath(entryName);
                if (!relPath.empty()) {
                    const std::filesystem::path outPath =
                        std::filesystem::path(tempExtractPath) / relPath;
                    if (typeflag == DIRTYPE) {
                        std::filesystem::create_directories(outPath);
                    } else if (isRegularTarType(typeflag)) {
                        const auto parent = outPath.parent_path();
                        if (!parent.empty()) {
                            std::filesystem::create_directories(parent);
                        }
                        std::ofstream out(outPath, std::ios::binary | std::ios::trunc);
                        if (!out.is_open()) {
                            throw std::runtime_error("Failed to create extracted file: " +
                                                     outPath.string());
                        }
                        streamCopy(archive, out, payloadSize);
                        out.flush();
                        if (!out) {
                            throw std::runtime_error("Failed writing extracted file: " +
                                                     outPath.string());
                        }
                        out.close();
                        if (!out) {
                            throw std::runtime_error("Failed closing extracted file: " +
                                                     outPath.string());
                        }
                        std::error_code permEc;
                        const uint64_t mode = parseOctal(header.mode, sizeof(header.mode));
                        std::filesystem::permissions(
                            outPath, tarModeToPerms(mode),
                            std::filesystem::perm_options::replace, permEc);
                        if (permEc) {
                            throw std::runtime_error(
                                "Failed setting permissions on extracted file " + outPath.string() +
                                ": " + permEc.message());
                        }
                        payloadSize = 0;
                    }
                }
            }

            if (payloadSize > 0) {
                streamSkip(archive, payloadSize);
            }
            const uint64_t padding =
                (TAR_BLOCK_SIZE - (headerSize % TAR_BLOCK_SIZE)) % TAR_BLOCK_SIZE;
            if (padding > 0) {
                streamSkip(archive, padding);
            }
        }
    } catch (...) {
        cleanupArchive();
        throw;
    }
    cleanupArchive();
}

void Vrtbin::copy(const std::string& source, const std::string& destination) {
    utils::Logger::log(utils::LogLevel::DEBUG, __PRETTY_FUNCTION__, "Copying file {} to {}", source,
                       destination);
    std::ifstream src(source, std::ios::binary);
    if (!src) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "Error opening source file: {}", source);
        throw std::runtime_error("Error opening source file");
    }

    std::ofstream dest(destination, std::ios::binary);
    if (!dest) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "Error opening destination file: {}", destination);
        throw std::runtime_error("Error opening destination file");
    }

    dest << src.rdbuf();

    if (!src) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "Error reading from source file: {}", source);
        throw std::runtime_error("Error reading from source file");
    }

    if (!dest) {
        utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                           "Error writing to destination file: {}", destination);
        throw std::runtime_error("Error writing to destination file");
    }
}

std::string Vrtbin::getSystemMapPath() { return systemMapPath; }
std::string Vrtbin::getPdiPath() { return pdiPath; }
std::vector<std::string> Vrtbin::getPdiPaths() { return pdiPaths; }

std::string Vrtbin::getEmulationExec() { return emulationExecPath; }

std::string Vrtbin::getEmulationManifest() { return emulationManifestPath; }

std::string Vrtbin::getSimulationExec() { return simulationExecPath; }

Platform Vrtbin::getPlatform() const { return platform; }

void Vrtbin::discoverPdiFiles() {
    pdiPaths.clear();

    if (!std::filesystem::exists(tempExtractPath)) {
        return;
    }

    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(tempExtractPath)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".pdi") {
            pdiPaths.push_back(entry.path().string());
        }
    }

    std::sort(pdiPaths.begin(), pdiPaths.end());
    if (pdiPaths.empty()) {
        pdiPath.clear();
        return;
    }

    auto preferred = std::find_if(pdiPaths.begin(), pdiPaths.end(), [](const std::string& p) {
        return std::filesystem::path(p).filename() == "design.pdi";
    });
    if (preferred != pdiPaths.end() && preferred != pdiPaths.begin()) {
        std::iter_swap(pdiPaths.begin(), preferred);
    }
    pdiPath = pdiPaths.front();
}

std::filesystem::path Vrtbin::findExtractedFile(const std::string& filename) const {
    const std::filesystem::path direct = std::filesystem::path(tempExtractPath) / filename;
    if (std::filesystem::exists(direct)) {
        return direct;
    }
    if (!std::filesystem::exists(tempExtractPath)) {
        return {};
    }
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(tempExtractPath)) {
        if (entry.is_regular_file() && entry.path().filename() == filename) {
            return entry.path();
        }
    }
    return {};
}

std::string Vrtbin::sanitizeForPath(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (char c : input) {
        if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out.empty() ? std::string("default") : out;
}

std::string Vrtbin::getUtilizationReportPath() const { return utilizationReportPath; }

std::filesystem::path Vrtbin::findExtractedFileByPrefix(const std::string& prefix,
                                                        const std::string& extension) const {
    if (!std::filesystem::exists(tempExtractPath)) {
        return {};
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(tempExtractPath)) {
        if (!entry.is_regular_file()) {
            continue;
        }
        const std::string fname = entry.path().filename().string();
        if (fname.size() >= prefix.size() && fname.rfind(prefix, 0) == 0 &&
            entry.path().extension().string() == extension) {
            return entry.path();
        }
    }
    return {};
}

std::string Vrtbin::getSystemMapPathFromBdf(const std::string& bdf) {
    return (FilesystemCache::getCachePath() / ("metadata_" + sanitizeForPath(bdf)) / "system_map.xml").string();
}

std::string Vrtbin::getUtilizationReportPathFromBdf(const std::string& bdf) {
    return (FilesystemCache::getCachePath() / ("metadata_" + sanitizeForPath(bdf)) /
            "report_utilization.xml")
        .string();
}

}  // namespace vrt
