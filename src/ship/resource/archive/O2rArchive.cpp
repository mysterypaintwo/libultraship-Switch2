#include "ship/resource/archive/O2rArchive.h"

#include "ship/Context.h"
#include "ship/window/Window.h"
#include "spdlog/spdlog.h"
#include <algorithm>
#include <unordered_map>

#ifdef __SWITCH__
#include "ship/port/switch/SwitchLoading.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <mutex>

namespace {
// The game archives hold tens of thousands of very small files, and reading each
// one from the SD card costs about a millisecond, which stalls map changes. Keep
// the whole archive in memory instead. If that fails, it is read from the SD card.
struct ArchiveImage {
    void* data = nullptr;
    size_t size = 0;
};

constexpr long kMaxArchiveInMemory = 512L * 1024 * 1024;
constexpr size_t kArchiveReadChunk = 1024 * 1024;

const ArchiveImage* GetArchiveImage(const std::string& path) {
    const std::string name = std::filesystem::path(path).filename().string();
    if (name != "mm.o2r" && name != "2ship.o2r") {
        return nullptr;
    }

    static std::mutex sMutex;
    static std::map<std::string, ArchiveImage> sImages;
    std::lock_guard<std::mutex> lock(sMutex);
    auto found = sImages.find(path);
    if (found != sImages.end()) {
        return &found->second;
    }

    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return nullptr;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    void* data = (size > 0 && size <= kMaxArchiveInMemory) ? malloc(size) : nullptr;
    if (data == nullptr) {
        fclose(file);
        return nullptr;
    }
    size_t done = 0;
    while (done < static_cast<size_t>(size)) {
        const size_t chunk = std::min(kArchiveReadChunk, static_cast<size_t>(size) - done);
        if (fread(static_cast<char*>(data) + done, 1, chunk, file) != chunk) {
            break;
        }
        done += chunk;
        SwitchLoading_Update(static_cast<int>(done / 1024), static_cast<int>(size / 1024));
    }
    fclose(file);
    if (done != static_cast<size_t>(size)) {
        free(data);
        return nullptr;
    }
    SPDLOG_INFO("Keeping {} in memory ({} KB)", name, size / 1024);
    return &(sImages[path] = ArchiveImage{ data, static_cast<size_t>(size) });
}

zip_t* OpenArchiveImage(const ArchiveImage& image) {
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* source = zip_source_buffer_create(image.data, image.size, 0, &error);
    zip_t* archive = nullptr;
    if (source != nullptr) {
        archive = zip_open_from_source(source, ZIP_RDONLY, &error);
        if (archive == nullptr) {
            zip_source_free(source);
        }
    }
    zip_error_fini(&error);
    return archive;
}
} // namespace
#endif

namespace Ship {
O2rArchive::O2rArchive(const std::string& archivePath) : Archive(archivePath) {
    mZipArchive = nullptr;
}

O2rArchive::~O2rArchive() {
    SPDLOG_TRACE("destruct o2rarchive: {}", GetPath());
    Close();
}

zip_t* O2rArchive::GetZipHandle() {
    std::lock_guard<std::mutex> lock(mPoolMutex);
    if (!mZipArchivePool.empty()) {
        zip_t* handle = mZipArchivePool.back();
        mZipArchivePool.pop_back();
        return handle;
    }
#ifdef __SWITCH__
    if (const ArchiveImage* image = GetArchiveImage(GetPath())) {
        if (zip_t* handle = OpenArchiveImage(*image)) {
            return handle;
        }
    }
#endif
    return zip_open(GetPath().c_str(), ZIP_RDONLY, nullptr);
}

void O2rArchive::ReleaseZipHandle(zip_t* handle) {
    if (handle == nullptr) {
        return;
    }

    std::lock_guard<std::mutex> lock(mPoolMutex);
    mZipArchivePool.push_back(handle);
}

std::shared_ptr<File> O2rArchive::LoadFile(uint64_t hash) {
    const std::string& filePath =
        *Context::GetRawInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    return LoadFile(filePath);
}

std::shared_ptr<File> O2rArchive::LoadFile(const std::string& filePath) {
    zip_t* zipArchive = GetZipHandle();
    if (zipArchive == nullptr) {
        SPDLOG_TRACE("Failed to open file {} from zip archive {}. Archive not open.", filePath, GetPath());
        return nullptr;
    }

    auto zipEntryIndex = zip_name_locate(zipArchive, filePath.c_str(), 0);
    if (zipEntryIndex < 0) {
        SPDLOG_TRACE("Failed to find file {} in zip archive  {}.", filePath, GetPath());
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }

    struct zip_stat zipEntryStat;
    zip_stat_init(&zipEntryStat);
    if (zip_stat_index(zipArchive, zipEntryIndex, 0, &zipEntryStat) != 0) {
        SPDLOG_TRACE("Failed to get entry information for file {} in zip archive  {}.", filePath, GetPath());
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }

    // Filesize 0, no logging needed
    if (zipEntryStat.size == 0) {
        SPDLOG_TRACE("Failed to load file {}; filesize 0", filePath, GetPath());
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }

    struct zip_file* zipEntryFile = zip_fopen_index(zipArchive, zipEntryIndex, 0);
    if (!zipEntryFile) {
        SPDLOG_TRACE("Failed to open file {} in zip archive  {}.", filePath, GetPath());
        ReleaseZipHandle(zipArchive);
        return nullptr;
    }

    auto fileToLoad = std::make_shared<File>();
    fileToLoad->Buffer = std::make_shared<std::vector<char>>(zipEntryStat.size);

    if (zip_fread(zipEntryFile, fileToLoad->Buffer->data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }

    if (zip_fclose(zipEntryFile) != 0) {
        SPDLOG_TRACE("Error closing file {} in zip archive  {}.", filePath, GetPath());
    }

    ReleaseZipHandle(zipArchive);

    fileToLoad->IsLoaded = true;

    return fileToLoad;
}

bool O2rArchive::Open() {
#ifdef __SWITCH__
    if (const ArchiveImage* image = GetArchiveImage(GetPath())) {
        mZipArchive = OpenArchiveImage(*image);
    }
    if (mZipArchive == nullptr) {
        mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    }
#else
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
#endif
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }

    auto zipNumEntries = zip_get_num_entries(mZipArchive, 0);
    for (auto i = 0; i < zipNumEntries; i++) {
        auto zipEntryName = zip_get_name(mZipArchive, i, 0);

        // It is possible for directories to have entries in a zip
        // file, we don't want those indexed as files in the archive
        if (zipEntryName[strlen(zipEntryName) - 1] == '/') {
            continue;
        }

        IndexFile(zipEntryName);
    }

    return true;
}

bool O2rArchive::Close() {
    bool success = true;

    if (mZipArchive != nullptr) {
        if (zip_close(mZipArchive) == -1) {
            SPDLOG_ERROR("Failed to close zip file \"{}\"", GetPath());
            success = false;
        }
        mZipArchive = nullptr;
    }

    std::lock_guard<std::mutex> lock(mPoolMutex);
    for (auto* handle : mZipArchivePool) {
        if (zip_close(handle) == -1) {
            SPDLOG_ERROR("Failed to close pooled zip file \"{}\"", GetPath());
            success = false;
        }
    }
    mZipArchivePool.clear();

    return success;
}

bool O2rArchive::WriteFile(const std::string& filePath, const std::vector<uint8_t>& data) {
    if (!mZipArchive) {
        SPDLOG_ERROR("Cannot write to zip: Archive is not open.");
        return false;
    }

    // Create a new zip source from the data buffer
    zip_source_t* source = zip_source_buffer(mZipArchive, data.data(), data.size(), 0);
    if (!source) {
        SPDLOG_ERROR("Failed to create zip source for file \"{}\"", filePath);
        return false;
    }

    // Add or replace the file in the zip archive
    if (zip_file_add(mZipArchive, filePath.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
        SPDLOG_ERROR("Failed to add file \"{}\" to ZIP", filePath);
        zip_source_free(source);
        return false;
    }

    // Save changes to disk
    if (zip_close(mZipArchive) < 0) {
        zip_error_t* error = zip_get_error(mZipArchive);
        SPDLOG_ERROR("Failed to save changes to zip archive: {} ({})", zip_error_strerror(error),
                     zip_error_code_zip(error));
        zip_discard(mZipArchive); // Close zip and discard changes
        return false;
    }

    // Clear the pool as the file on disk has likely changed
    {
        std::lock_guard<std::mutex> lock(mPoolMutex);
        for (auto* handle : mZipArchivePool) {
            zip_close(handle);
        }
        mZipArchivePool.clear();
    }

    SPDLOG_INFO("Successfully wrote file: {}", filePath);

    // Reopen the zip file so that it may continued to be used by libultraship
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to reopen zip file after writing.");
        return false;
    }

    IndexFile(filePath);

    // Success
    return true;
}

} // namespace Ship
