#pragma once

#include <filesystem>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace catalyst {

/**
 * @brief cross-platform read-only memory mapped file wrapper.
 *
 * Provides access to file contents by mapping them into memory.
 * Handles resource cleanup via RAII.
 * Throws std::runtime_error on failure.
 */
class MappedFile {
public:
    /**
     * @brief Opens and maps the specified file.
     * @param path The path to the file.
     * @throws std::runtime_error If opening, stating, or mapping fails.
     */
    explicit MappedFile(const std::filesystem::path &path, bool populate = true) {
#ifdef _WIN32
        file_handle_ = CreateFileW(
            path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (file_handle_ == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Failed to open file: " + path.string());
        }

        LARGE_INTEGER size_li;
        if (!GetFileSizeEx(file_handle_, &size_li)) {
            CloseHandle(file_handle_);
            throw std::runtime_error("Failed to stat file: " + path.string());
        }
        file_size = static_cast<size_t>(size_li.QuadPart);

        if (file_size == 0) {
            file_data = nullptr;
            CloseHandle(file_handle_);
            file_handle_ = INVALID_HANDLE_VALUE;
            return;
        }

        mapping_handle_ = CreateFileMappingW(file_handle_, nullptr, PAGE_READONLY, 0, 0, nullptr);
        if (!mapping_handle_) {
            CloseHandle(file_handle_);
            throw std::runtime_error("Failed to create file mapping: " + path.string());
        }

        void *addr = MapViewOfFile(mapping_handle_, FILE_MAP_READ, 0, 0, 0);
        if (!addr) {
            CloseHandle(mapping_handle_);
            CloseHandle(file_handle_);
            throw std::runtime_error("Failed to map view of file: " + path.string());
        }
        file_data = static_cast<char *>(addr);
#else
        file_descriptor = open(path.c_str(), O_RDONLY);
        if (file_descriptor == -1) {
            throw std::runtime_error("Failed to open file: " + path.string());
        }

        struct stat sb{};
        if (fstat(file_descriptor, &sb) == -1) {
            close(file_descriptor);
            throw std::runtime_error("Failed to stat file: " + path.string());
        }
        file_size = static_cast<size_t>(sb.st_size);

        if (file_size == 0) {
            file_data = nullptr;
            return;
        }

        posix_fadvise(file_descriptor, 0, 0, POSIX_FADV_SEQUENTIAL);

#ifdef __linux__
        int flags = populate ? (MAP_POPULATE | MAP_PRIVATE) : MAP_PRIVATE;
        void *addr = mmap(nullptr, file_size, PROT_READ, flags, file_descriptor, 0);
#else
        void *addr = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, file_descriptor, 0);
#endif

        if (addr == MAP_FAILED) {
            close(file_descriptor);
            throw std::runtime_error("Failed to mmap file: " + path.string());
        }
        file_data = static_cast<char *>(addr);
#endif
    }

    ~MappedFile() {
#ifdef _WIN32
        if (file_data) {
            UnmapViewOfFile(file_data);
        }
        if (mapping_handle_) {
            CloseHandle(mapping_handle_);
        }
        if (file_handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_handle_);
        }
#else
        if (file_data) {
            munmap(file_data, file_size);
        }
        if (file_descriptor != -1) {
            close(file_descriptor);
        }
#endif
    }

    [[nodiscard]] std::string_view content() const {
        if (!file_data)
            return {};
        return {file_data, file_size};
    }

    MappedFile(MappedFile &&other) noexcept = delete;
    MappedFile &operator=(MappedFile &&other) noexcept = delete;
    MappedFile(const MappedFile &) = delete;
    MappedFile &operator=(const MappedFile &) = delete;

private:
#ifdef _WIN32
    HANDLE file_handle_ = INVALID_HANDLE_VALUE;
    HANDLE mapping_handle_ = nullptr;
#else
    int file_descriptor = -1;
#endif
    char *file_data = nullptr;
    size_t file_size = 0;
};

class MappedUnfaultedFile : public MappedFile {
public:
    explicit MappedUnfaultedFile(const std::filesystem::path &path) : MappedFile(path, false) {
    }
};
} // namespace catalyst
