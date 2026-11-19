#pragma once
#include "nwd_error.h"
#include <cstddef>
#include <cstdint>

namespace HNwd::detail {

// En fil minnesmappad för läsning. Ingenting läses in förrän en sida rörs, så
// en fil på flera GB kostar inget minne förrän dess paket packas upp.
//
// Filen ska ligga kvar oförändrad så länge den är mappad. Kortas den av någon
// annan, eller försvinner disken den ligger på, avbryter systemet processen
// vid nästa läsning i det som saknas.
class MappedFile {
public:
    MappedFile() = default;
    ~MappedFile();

    MappedFile(const MappedFile&) = delete;
    MappedFile& operator=(const MappedFile&) = delete;

    MappedFile(MappedFile&& other) noexcept;
    MappedFile& operator=(MappedFile&& other) noexcept;

    // Sökvägen är UTF-8 på alla plattformar. En tom fil, och det som inte är
    // en vanlig fil, ger FileOpenFailed.
    ErrorCode open(const char* path);
    void close();

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    bool is_open() const { return data_ != nullptr; }

private:
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
#ifdef _WIN32
    void* file_handle_ = nullptr;
    void* mapping_handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace HNwd::detail
