#include <doctest/doctest.h>

#include "io/sf2_file.hpp"
#include "io/sample_pitch.hpp"
#include "io/spc_file.hpp"
#include "playback/spc700_output.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

using namespace midi_composer;

// ── Against real files, when there are any ───────────────────────────────────
//
// Everything else about these loaders is checked against files the tests build,
// which proves the parsing matches what the formats say and nothing about what
// a real one contains. A game's rip is written by a driver nobody documented,
// with a directory that runs into other data and samples that break rules the
// specification never made.
//
// So: point MC_BANK_DIR at a folder of `.spc` and `.sf2` files and these run
// for real. Skipped otherwise, because the suite has to pass on a machine that
// has none -- and because the files that would make it pass are not ours to
// commit.

namespace {

std::string env(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || !value) return {};
    std::string out(value);
    std::free(value);
    return out;
#else
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string{};
#endif
}

std::string from_path(const std::filesystem::path& path) {
    const auto text = path.u8string();
    return std::string(reinterpret_cast<const char*>(text.c_str()), text.size());
}

std::vector<std::string> files_with(const std::string& extension) {
    const auto dir = env("MC_BANK_DIR");
    std::vector<std::string> out;
    if (dir.empty()) return out;

    std::error_code ec;
    const std::filesystem::path root(reinterpret_cast<const char8_t*>(dir.c_str()));
    for (auto it = std::filesystem::directory_iterator(root, ec);
         it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (ec) break;
        if (!it->is_regular_file(ec)) continue;
        auto ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == extension) out.push_back(from_path(it->path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

/** Plays a bank hard enough that a bad index or loop would be found. */
void hammer(const std::shared_ptr<const playback::SampleBank>& bank) {
    playback::Spc700Output out;
    out.set_sample_rate(48000);
    out.set_bank(bank);
    REQUIRE(out.start().has_value());

    std::vector<float> buffer(256 * 2, 0.0f);
    float loudest = 0.0f;
    for (int program = 0; program < 128; ++program) {
        if (!bank->for_program(program)) continue;
        out.program_change(0, static_cast<uint8_t>(program), 0);
        // Across the range, because a sample is read fastest at the top and
        // a loop is exercised hardest at the bottom.
        for (int pitch = 24; pitch <= 108; pitch += 12) {
            out.note_on(0, static_cast<uint8_t>(pitch), 100, 0);
            for (int block = 0; block < 4; ++block) {
                out.begin_block(static_cast<int64_t>(block) * 5000);
                out.render(buffer.data(), 256);
                for (float f : buffer) loudest = std::max(loudest, std::abs(f));
            }
            out.note_off(0, static_cast<uint8_t>(pitch), 0);
        }
    }
    CHECK(out.dropped_events() == 0);
    // Every one of these banks has audio in it; a silent result would mean the
    // samples decoded to nothing.
    CHECK(loudest > 0.0f);
}

} // namespace

TEST_CASE("every real .spc rips") {
    const auto files = files_with(".spc");
    if (files.empty()) {
        MESSAGE("No .spc found; set MC_BANK_DIR to run this against real rips");
        return;
    }

    size_t ripped = 0;
    size_t samples = 0;
    size_t tuned = 0;
    int lowest = 127;
    int highest = 0;
    for (const auto& file : files) {
        const auto bank = io::load_spc(file);
        if (!bank) {
            // Reported rather than failed: a rip this cannot read is worth
            // knowing about by name, and one bad file should not hide the
            // results for the other ninety.
            MESSAGE("Could not rip ", file, ": ", bank.error().message);
            continue;
        }
        ++ripped;
        samples += (*bank)->samples.size();
        CHECK_FALSE((*bank)->empty());

        for (const auto& sample : (*bank)->samples) {
            // 60 with no correction is the default the pitch detector leaves
            // behind when it found nothing to measure.
            if (sample.root_key != 60 || sample.fine_tune_cents != 0.0) {
                ++tuned;
                lowest = std::min(lowest, sample.root_key);
                highest = std::max(highest, sample.root_key);
            }
            // A root key outside the keyboard means the detector locked onto
            // something absurd, and the instrument would play at a rate no
            // interpolation can help.
            CHECK(sample.root_key >= 0);
            CHECK(sample.root_key <= 127);
            CHECK(std::abs(sample.fine_tune_cents) <= 50.0);
        }
    }
    MESSAGE("Ripped ", ripped, " of ", files.size(), " files, ", samples, " samples in total");
    MESSAGE("Pitch measured for ", tuned, " of ", samples, " samples; root keys ",
            lowest, " to ", highest);
    CHECK(ripped == files.size());
}

TEST_CASE("the echo a real rip declares is a usable echo") {
    const auto files = files_with(".spc");
    if (files.empty()) {
        MESSAGE("No .spc found; set MC_BANK_DIR to run this against real rips");
        return;
    }

    // The register map for the echo is believed rather than demonstrated -- only
    // $5D is confirmed by this project's own results. If the map were wrong the
    // values would be noise, so this asks whether ninety-two independent files
    // agree on something a chip could mean.
    size_t with_echo = 0;
    int shortest = 10000;
    int longest = 0;
    double feedback_sum = 0.0;
    for (const auto& file : files) {
        const auto bank = io::load_spc(file);
        if (!bank) continue;
        const auto& echo = (*bank)->echo;
        if (!echo.enabled) continue;
        ++with_echo;
        shortest = std::min(shortest, echo.delay_ms);
        longest = std::max(longest, echo.delay_ms);
        feedback_sum += echo.feedback;

        // Everything the chip can mean, and nothing it cannot.
        CHECK(echo.delay_ms > 0);
        CHECK(echo.delay_ms <= 240);
        CHECK(std::abs(echo.feedback) <= 1.0f);
        CHECK(std::abs(echo.volume_left) <= 1.0f);
        CHECK(std::abs(echo.volume_right) <= 1.0f);
    }
    MESSAGE(with_echo, " of ", files.size(), " rips declare an echo; delays ",
            shortest, " to ", longest, "ms, mean feedback ",
            with_echo ? feedback_sum / static_cast<double>(with_echo) : 0.0);

    // A soundtrack where nothing used the echo would mean the map is wrong:
    // reverb is most of what a SNES score sounds like.
    CHECK(with_echo > files.size() / 2);
}

TEST_CASE("a real rip plays across the whole keyboard") {
    const auto files = files_with(".spc");
    if (files.empty()) {
        MESSAGE("No .spc found; set MC_BANK_DIR to run this against real rips");
        return;
    }

    // Every one of them, not a sample of them: a loop point that walks off the
    // end of its data is exactly the kind of thing one file in ninety has.
    for (const auto& file : files) {
        const auto bank = io::load_spc(file);
        if (!bank) continue;
        CAPTURE(file);
        hammer(*bank);
    }
}

TEST_CASE("every real .sf2 loads") {
    const auto files = files_with(".sf2");
    if (files.empty()) {
        MESSAGE("No .sf2 found; set MC_BANK_DIR to run this against real banks");
        return;
    }

    for (const auto& file : files) {
        CAPTURE(file);
        const auto bank = io::load_sf2(file);
        REQUIRE(bank.has_value());

        int programs = 0;
        for (int p = 0; p < 128; ++p) if ((*bank)->for_program(p)) ++programs;
        MESSAGE(file, ": ", (*bank)->samples.size(), " samples across ", programs, " programs");
        CHECK(programs > 0);

        hammer(*bank);
    }
}

TEST_CASE("a sample long enough to hold a pitch gets one") {
    const auto files = files_with(".spc");
    if (files.empty()) {
        MESSAGE("No .spc found; set MC_BANK_DIR to run this against real rips");
        return;
    }

    // The short ones are percussion and effects: two milliseconds cannot
    // contain a period, and they have no pitch to be wrong about. The long ones
    // are the instruments, and every one of them has to be placed on the
    // keyboard or it plays at whatever interval C4 happens to imply.
    constexpr size_t kLongEnough = 2000;   // 62ms at the chip's rate
    size_t longs = 0;
    size_t placed = 0;
    for (const auto& file : files) {
        const auto bank = io::load_spc(file);
        if (!bank) continue;
        for (const auto& sample : (*bank)->samples) {
            if (sample.data.size() < kLongEnough) continue;
            ++longs;
            if (sample.root_key != 60 || sample.fine_tune_cents != 0.0) ++placed;
        }
    }
    MESSAGE("Placed ", placed, " of ", longs, " samples over ", kLongEnough, " frames");
    REQUIRE(longs > 0);
    // 85 rather than a rounder number because that is where the material sits,
    // not where it was aimed: about one long sample in ten in this set has no
    // fundamental to find -- choirs, wind, surf, the sound effects a game keeps
    // in the same bank. Raising the bar further would mean inventing pitches
    // for them, which is the thing this is meant to stop doing.
    CHECK(placed * 100 / longs >= 85);
}
