#include "mapped_file.h"

#ifdef _WIN32
#include "path_utf8.h"   // drar in windows.h, med NOMINMAX satt
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <utility>

namespace HNwd::detail {

MappedFile::~MappedFile() {
    close();
}

MappedFile::MappedFile(MappedFile&& other) noexcept {
    *this = std::move(other);
}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
    if (this != &other) {
        close();
        data_ = std::exchange(other.data_, nullptr);
        size_ = std::exchange(other.size_, 0);
#ifdef _WIN32
        file_handle_ = std::exchange(other.file_handle_, nullptr);
        mapping_handle_ = std::exchange(other.mapping_handle_, nullptr);
#else
        fd_ = std::exchange(other.fd_, -1);
#endif
    }
    return *this;
}

#ifdef _WIN32

ErrorCode MappedFile::open(const char* path) {
    close();

    const std::wstring wpath = utf8_to_wide(path);
    if (wpath.empty())
        return ErrorCode::FileNotFound;   // tom sökväg eller ogiltig UTF-8

    // FILE_SHARE_DELETE: utan den går filen inte att radera eller byta namn på
    // så länge den är mappad. POSIX tillåter det, och med flaggan beter sig
    // plattformarna lika — mappningen fortsätter läsa det den redan pekar på.
    HANDLE file = CreateFileW(wpath.c_str(), GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND)
            return ErrorCode::FileNotFound;
        return ErrorCode::FileOpenFailed;
    }
    file_handle_ = file;

    LARGE_INTEGER file_size;
    if (!GetFileSizeEx(file, &file_size) || file_size.QuadPart <= 0) {
        close();
        return ErrorCode::FileOpenFailed;
    }
    // En fil som inte ryms i adressrymden går inte att mappa hel.
    if (static_cast<unsigned long long>(file_size.QuadPart) > SIZE_MAX) {
        close();
        return ErrorCode::MmapFailed;
    }

    mapping_handle_ = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping_handle_) {
        close();
        return ErrorCode::MmapFailed;
    }

    data_ = static_cast<const uint8_t*>(
        MapViewOfFile(mapping_handle_, FILE_MAP_READ, 0, 0, 0));
    if (!data_) {
        close();
        return ErrorCode::MmapFailed;
    }
    size_ = static_cast<size_t>(file_size.QuadPart);
    return ErrorCode::Ok;
}

void MappedFile::close() {
    if (data_) {
        UnmapViewOfFile(data_);
        data_ = nullptr;
    }
    if (mapping_handle_) {
        CloseHandle(mapping_handle_);
        mapping_handle_ = nullptr;
    }
    if (file_handle_) {
        CloseHandle(file_handle_);
        file_handle_ = nullptr;
    }
    size_ = 0;
}

#else // Linux, macOS

ErrorCode MappedFile::open(const char* path) {
    close();

    if (!path || !*path)
        return ErrorCode::FileNotFound;

    // O_NONBLOCK: en namngiven pipe ska ge ett fel nedan, inte en öppning som
    // väntar på en skrivare. En vanlig fil bryr sig inte om flaggan.
    fd_ = ::open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd_ < 0)
        return errno == ENOENT ? ErrorCode::FileNotFound : ErrorCode::FileOpenFailed;

    struct stat st;
    if (fstat(fd_, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
        close();
        return ErrorCode::FileOpenFailed;
    }
    if (static_cast<unsigned long long>(st.st_size) > SIZE_MAX) {
        close();
        return ErrorCode::MmapFailed;
    }
    const size_t size = static_cast<size_t>(st.st_size);

    void* ptr = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd_, 0);
    if (ptr == MAP_FAILED) {
        close();
        return ErrorCode::MmapFailed;
    }
    data_ = static_cast<const uint8_t*>(ptr);
    size_ = size;
    return ErrorCode::Ok;
}

void MappedFile::close() {
    if (data_) {
        munmap(const_cast<uint8_t*>(data_), size_);
        data_ = nullptr;
    }
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    size_ = 0;
}

#endif

} // namespace HNwd::detail
