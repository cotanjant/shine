#pragma once

#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace shine::core {

enum class ErrorCode {
    Ok = 0,
    InvalidArgument,
    NotSupported,
    AlreadyRunning,
    NotRunning,
    DeviceLost,
    CaptureFailed,
    EncoderInitFailed,
    EncodeFailed,
    IoError,
    IpcError,
    InternalError
};

struct Error {
    ErrorCode code{ErrorCode::InternalError};
    std::string message;
};

template <typename T>
class Result {
public:
    static_assert(!std::is_void_v<T>, "Result<void> has its own specialization");

    [[nodiscard]] static Result make_ok(T value) {
        return Result(std::move(value));
    }

    [[nodiscard]] static Result make_err(ErrorCode code, std::string message) {
        return Result(Error{code, std::move(message)});
    }

    [[nodiscard]] static Result make_err(Error err) {
        return Result(std::move(err));
    }

    [[nodiscard]] bool ok() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ok();
    }

    [[nodiscard]] T& value() & {
        return std::get<T>(storage_);
    }

    [[nodiscard]] const T& value() const& {
        return std::get<T>(storage_);
    }

    [[nodiscard]] T take() {
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] const Error& error() const {
        return std::get<Error>(storage_);
    }

private:
    explicit Result(T value)
        : storage_(std::move(value)) {}

    explicit Result(Error err)
        : storage_(std::move(err)) {}

    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    [[nodiscard]] static Result make_ok() {
        return Result(true, {});
    }

    [[nodiscard]] static Result make_err(ErrorCode code, std::string message) {
        return Result(false, Error{code, std::move(message)});
    }

    [[nodiscard]] static Result make_err(Error err) {
        return Result(false, std::move(err));
    }

    [[nodiscard]] bool ok() const noexcept {
        return ok_;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return ok_;
    }

    [[nodiscard]] const Error& error() const noexcept {
        return error_;
    }

private:
    Result(bool ok, Error err)
        : ok_(ok), error_(std::move(err)) {}

    bool ok_{false};
    Error error_{};
};

} // namespace shine::core
