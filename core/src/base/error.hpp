#pragma once

#include <string>
#include <system_error>
#include <expected>

namespace midi_composer::base {

enum class ErrorCode {
    Success = 0,
    InvalidArgument,
    InvalidState,
    NotFound,
    IoFailure,
    ParseFailure,
    DeviceFailure,
    UnsupportedFormat,
    Conflict,
    InternalError
};

struct Error {
    ErrorCode code{ErrorCode::InternalError};
    std::string message;
};

template<typename T>
using Result = std::expected<T, Error>;

} // namespace midi_composer::base
