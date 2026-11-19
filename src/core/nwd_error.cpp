#include "nwd_error.h"

namespace HNwd::detail {

const char* error_message(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok:               return "ok";
        case ErrorCode::FileNotFound:     return "file not found";
        case ErrorCode::FileOpenFailed:   return "could not open file";
        case ErrorCode::MmapFailed:       return "could not map file into memory";
        case ErrorCode::NotLcUStream:     return "not an LcUStream file";
        case ErrorCode::CorruptHeader:    return "corrupt section header";
        case ErrorCode::NoDirectory:      return "no package directory found";
        case ErrorCode::CorruptDirectory: return "corrupt package directory";
        case ErrorCode::CorruptStream:    return "corrupt compressed stream";
        case ErrorCode::StreamTooLarge:   return "decompressed stream exceeds the limit";
        case ErrorCode::OutOfRange:       return "entry points outside the file";
        case ErrorCode::CorruptObject:    return "corrupt object stream";
        case ErrorCode::CorruptPackage:   return "corrupt package";
        case ErrorCode::CorruptGeometry:  return "corrupt geometry record";
        case ErrorCode::CorruptLink:      return "packages do not agree with each other";
        case ErrorCode::Unsupported:      return "not supported";
        case ErrorCode::LimitExceeded:    return "limit exceeded";
        case ErrorCode::PackageMissing:   return "package is missing";
        case ErrorCode::NotLoaded:        return "package is not loaded";
        case ErrorCode::Cancelled:        return "cancelled by the caller";
        case ErrorCode::kCount:           break;
    }
    return "unknown error";
}

} // namespace HNwd::detail
