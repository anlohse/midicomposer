#pragma once

#include <cstdint>
#include <compare>

namespace midi_composer::timeline {

class Tick {
public:
    constexpr Tick() noexcept = default;
    explicit constexpr Tick(std::int64_t v) noexcept : value_(v) {}

    [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }

    auto operator<=>(const Tick&) const = default;
    bool operator==(const Tick&) const = default;

private:
    std::int64_t value_{0};
};

class TickDuration {
public:
    constexpr TickDuration() noexcept = default;
    explicit constexpr TickDuration(std::int64_t v) noexcept : value_(v) {}

    [[nodiscard]] constexpr std::int64_t value() const noexcept { return value_; }

    auto operator<=>(const TickDuration&) const = default;
    bool operator==(const TickDuration&) const = default;

private:
    std::int64_t value_{0};
};

} // namespace midi_composer::timeline
