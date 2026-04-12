#pragma once

#include <cstdint>
#include <compare>

namespace midi_composer::base {

template<typename Tag>
class StrongId {
public:
    using value_type = std::uint64_t;

    constexpr StrongId() noexcept = default;
    explicit constexpr StrongId(value_type v) noexcept : value_(v) {}

    [[nodiscard]] constexpr value_type value() const noexcept { return value_; }

    auto operator<=>(const StrongId&) const = default;
    bool operator==(const StrongId&) const = default;

private:
    value_type value_{0};
};

struct CompositionIdTag {};
struct TrackIdTag {};
struct NoteIdTag {};
struct EventIdTag {};

using CompositionId = StrongId<CompositionIdTag>;
using TrackId = StrongId<TrackIdTag>;
using NoteId = StrongId<NoteIdTag>;
using EventId = StrongId<EventIdTag>;

} // namespace midi_composer::base

namespace std {
    template<typename Tag>
    struct hash<midi_composer::base::StrongId<Tag>> {
        size_t operator()(const midi_composer::base::StrongId<Tag>& id) const noexcept {
            return std::hash<uint64_t>{}(id.value());
        }
    };
}
