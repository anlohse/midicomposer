#include <doctest/doctest.h>

#include "app/core_facade.hpp"

#include <cstdint>

using namespace midi_composer;

namespace {

/**
 * A composition with tracks on the given channels.
 *
 * Auditioning needs a channel to send its program change on, and the only
 * decision it makes is which. Everything else about it is three calls to a
 * plugin, so this is the part worth building a fixture for.
 */
music::Composition with_channels(std::initializer_list<uint8_t> channels) {
    music::Composition composition;
    uint64_t id = 1;
    for (uint8_t channel : channels) {
        music::Track track(base::TrackId{id++}, "Track");
        track.set_midi_channel(channel);
        composition.tracks().push_back(std::move(track));
    }
    return composition;
}

} // namespace

TEST_CASE("an audition takes the highest channel nobody is on") {
    const auto chosen = app::CoreFacade::choose_audition_channel(with_channels({0, 1, 2}));
    CHECK(chosen.channel == 15);
    CHECK_FALSE(chosen.borrowed);
}

TEST_CASE("an audition avoids a channel a track is using") {
    // A program change is a channel's instrument, so sending one where a track
    // lives would quietly change what that track sounds like.
    const auto chosen = app::CoreFacade::choose_audition_channel(with_channels({15, 14}));
    CHECK(chosen.channel == 13);
    CHECK_FALSE(chosen.borrowed);
}

TEST_CASE("an audition with an empty project still has a channel") {
    const auto chosen = app::CoreFacade::choose_audition_channel(music::Composition{});
    CHECK(chosen.channel == 15);
    CHECK_FALSE(chosen.borrowed);
}

TEST_CASE("with every channel taken the audition borrows one and puts it back") {
    music::Composition composition;
    uint64_t id = 1;
    for (uint8_t channel = 0; channel < 16; ++channel) {
        music::Track track(base::TrackId{id++}, "Track");
        track.set_midi_channel(channel);
        // The instrument of a track is its program change at tick 0.
        track.program_changes().push_back(
            {base::EventId{id}, timeline::Tick{0}, static_cast<uint8_t>(40 + channel)});
        composition.tracks().push_back(std::move(track));
    }

    const auto chosen = app::CoreFacade::choose_audition_channel(composition);
    CHECK(chosen.borrowed);
    CHECK(chosen.channel == 15);
    // Which is what gets sent again once the note has ended, so the track it
    // was borrowed from is left as it was.
    CHECK(chosen.restore_program == 55);
}

TEST_CASE("a borrowed channel whose track has no instrument restores to program one") {
    music::Composition composition;
    for (uint8_t channel = 0; channel < 16; ++channel) {
        music::Track track(base::TrackId{static_cast<uint64_t>(channel + 1)}, "Track");
        track.set_midi_channel(channel);
        composition.tracks().push_back(std::move(track));
    }

    // A track that never had its instrument set has no program change to put
    // back, and zero is what a channel starts on anyway.
    const auto chosen = app::CoreFacade::choose_audition_channel(composition);
    CHECK(chosen.borrowed);
    CHECK(chosen.restore_program == 0);
}
