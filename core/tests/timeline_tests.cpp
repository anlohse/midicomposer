#include <doctest/doctest.h>

#include "edit/edit_service.hpp"
#include "music/composition.hpp"
#include "project/project_document.hpp"

using namespace midi_composer;

namespace {

constexpr std::int64_t kPpqn = 480;

project::ProjectDocument make_document() {
    music::Composition comp{base::CompositionId{1}};
    comp.tracks().push_back(music::Track{base::TrackId{1}, "Track"});
    return project::ProjectDocument{std::move(comp)};
}

// (tick, numerator, denominator) of every meter event, for legible assertions.
std::vector<std::tuple<std::int64_t, int, int>> meters(const project::ProjectDocument& doc) {
    std::vector<std::tuple<std::int64_t, int, int>> out;
    for (const auto& e : doc.composition().time_signature_map().events()) {
        out.emplace_back(e.tick.value(), e.numerator, e.denominator);
    }
    return out;
}

std::vector<std::tuple<std::int64_t, int, bool>> keys(const project::ProjectDocument& doc) {
    std::vector<std::tuple<std::int64_t, int, bool>> out;
    for (const auto& e : doc.composition().key_signature_map().events()) {
        out.emplace_back(e.tick.value(), e.fifths, e.minor);
    }
    return out;
}

} // namespace

TEST_CASE("a new composition has an effective meter and key at tick 0") {
    auto doc = make_document();
    CHECK(meters(doc) == std::vector<std::tuple<std::int64_t, int, int>>{{0, 4, 4}});
    CHECK(keys(doc) == std::vector<std::tuple<std::int64_t, int, bool>>{{0, 0, false}});
}

TEST_CASE("setting the meter at tick 0 replaces the opening event") {
    auto doc = make_document();
    edit::EditService svc;

    REQUIRE(svc.set_time_signature(doc, timeline::Tick{0}, 6, 8).has_value());
    CHECK(meters(doc) == std::vector<std::tuple<std::int64_t, int, int>>{{0, 6, 8}});

    REQUIRE(doc.history().undo());
    CHECK(meters(doc) == std::vector<std::tuple<std::int64_t, int, int>>{{0, 4, 4}});
}

TEST_CASE("a meter change snaps to the start of the bar containing the tick") {
    auto doc = make_document();
    edit::EditService svc;
    // 6/8 bars are 1440 ticks, so bar 3 spans 2880..4320.
    REQUIRE(svc.set_time_signature(doc, timeline::Tick{0}, 6, 8).has_value());

    for (std::int64_t tick : {std::int64_t{2880}, std::int64_t{3000}, std::int64_t{4319}}) {
        auto fresh = make_document();
        edit::EditService s2;
        REQUIRE(s2.set_time_signature(fresh, timeline::Tick{0}, 6, 8).has_value());
        REQUIRE(s2.set_time_signature(fresh, timeline::Tick{tick}, 3, 4).has_value());
        // Landing mid-bar would put a bar line inside a bar.
        CHECK(meters(fresh) == std::vector<std::tuple<std::int64_t, int, int>>{{0, 6, 8}, {2880, 3, 4}});
    }
}

TEST_CASE("snapping follows earlier meter changes rather than assuming 4/4") {
    auto doc = make_document();
    edit::EditService svc;
    // 2/4 bars are 960 ticks: bars start at 0, 960, 1920, 2880 …
    REQUIRE(svc.set_time_signature(doc, timeline::Tick{0}, 2, 4).has_value());
    REQUIRE(svc.set_time_signature(doc, timeline::Tick{1000}, 7, 8).has_value());
    CHECK(meters(doc) == std::vector<std::tuple<std::int64_t, int, int>>{{0, 2, 4}, {960, 7, 8}});

    // 7/8 bars are 1680 ticks, so the next bar after 960 starts at 2640.
    REQUIRE(svc.set_time_signature(doc, timeline::Tick{2700}, 4, 4).has_value());
    CHECK(meters(doc) == std::vector<std::tuple<std::int64_t, int, int>>{
        {0, 2, 4}, {960, 7, 8}, {2640, 4, 4}});
}

TEST_CASE("a repeated meter change at the same bar updates in place") {
    auto doc = make_document();
    edit::EditService svc;
    REQUIRE(svc.set_time_signature(doc, timeline::Tick{1920}, 3, 4).has_value());
    REQUIRE(svc.set_time_signature(doc, timeline::Tick{1920}, 5, 4).has_value());
    CHECK(meters(doc) == std::vector<std::tuple<std::int64_t, int, int>>{{0, 4, 4}, {1920, 5, 4}});
}

TEST_CASE("invalid meters are rejected") {
    auto doc = make_document();
    edit::EditService svc;

    CHECK_FALSE(svc.set_time_signature(doc, timeline::Tick{0}, 0, 4));
    CHECK_FALSE(svc.set_time_signature(doc, timeline::Tick{0}, 33, 4));
    CHECK_FALSE(svc.set_time_signature(doc, timeline::Tick{-1}, 4, 4));
    // Only powers of two: standard notation admits no others and MIDI stores log2.
    for (int den : {0, 3, 5, 6, 7, 9, 12, 64}) {
        CHECK_FALSE(svc.set_time_signature(doc, timeline::Tick{0}, 4, static_cast<std::uint8_t>(den)));
    }
    CHECK(meters(doc) == std::vector<std::tuple<std::int64_t, int, int>>{{0, 4, 4}});
    CHECK(doc.revision() == 0);
}

TEST_CASE("the key signature inserts, updates and undoes as a whole map") {
    auto doc = make_document();
    edit::EditService svc;

    REQUIRE(svc.set_key_signature(doc, timeline::Tick{0}, -3, true).has_value());
    CHECK(keys(doc) == std::vector<std::tuple<std::int64_t, int, bool>>{{0, -3, true}});

    REQUIRE(svc.set_key_signature(doc, timeline::Tick{3840}, 2, false).has_value());
    CHECK(keys(doc) == std::vector<std::tuple<std::int64_t, int, bool>>{{0, -3, true}, {3840, 2, false}});

    // Undo restores the previous map exactly, wherever the event landed.
    REQUIRE(doc.history().undo());
    CHECK(keys(doc) == std::vector<std::tuple<std::int64_t, int, bool>>{{0, -3, true}});
    REQUIRE(doc.history().redo());
    CHECK(keys(doc) == std::vector<std::tuple<std::int64_t, int, bool>>{{0, -3, true}, {3840, 2, false}});
}

TEST_CASE("a key signature outside the circle of fifths is rejected") {
    auto doc = make_document();
    edit::EditService svc;
    CHECK_FALSE(svc.set_key_signature(doc, timeline::Tick{0}, 8, false));
    CHECK_FALSE(svc.set_key_signature(doc, timeline::Tick{0}, -8, false));
    CHECK_FALSE(svc.set_key_signature(doc, timeline::Tick{-1}, 0, false));
    CHECK(keys(doc) == std::vector<std::tuple<std::int64_t, int, bool>>{{0, 0, false}});
}

TEST_CASE("key signature events stay sorted by tick") {
    auto doc = make_document();
    edit::EditService svc;
    REQUIRE(svc.set_key_signature(doc, timeline::Tick{7680}, 3, false).has_value());
    REQUIRE(svc.set_key_signature(doc, timeline::Tick{1920}, -2, false).has_value());
    CHECK(keys(doc) == std::vector<std::tuple<std::int64_t, int, bool>>{
        {0, 0, false}, {1920, -2, false}, {7680, 3, false}});
}

TEST_CASE("setting the tempo clamps to a usable range") {
    auto doc = make_document();
    edit::EditService svc;

    REQUIRE(svc.set_tempo(doc, 96.0).has_value());
    CHECK(doc.composition().tempo_map().events().front().bpm() == doctest::Approx(96.0).epsilon(0.01));
    CHECK_FALSE(svc.set_tempo(doc, 10.0));
    CHECK_FALSE(svc.set_tempo(doc, 1000.0));

    REQUIRE(doc.history().undo());
    CHECK(doc.composition().tempo_map().events().front().bpm() == doctest::Approx(120.0).epsilon(0.01));
}
