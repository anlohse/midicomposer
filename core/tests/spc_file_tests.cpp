#include <doctest/doctest.h>

#include "io/brr.hpp"
#include "io/spc_file.hpp"
#include "playback/spc700_output.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace midi_composer;

namespace {

/** One BRR block: a shift, a predictor, the two flags, and sixteen nibbles. */
std::vector<uint8_t> brr_block(int range, int filter, bool loop, bool end,
                               const std::vector<int>& nibbles) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>((range << 4) | (filter << 2) |
                                       (loop ? 2 : 0) | (end ? 1 : 0)));
    for (size_t i = 0; i < 8; ++i) {
        const int hi = i * 2 < nibbles.size() ? nibbles[i * 2] : 0;
        const int lo = i * 2 + 1 < nibbles.size() ? nibbles[i * 2 + 1] : 0;
        out.push_back(static_cast<uint8_t>(((hi & 0x0F) << 4) | (lo & 0x0F)));
    }
    return out;
}

void append(std::vector<uint8_t>& into, const std::vector<uint8_t>& what) {
    into.insert(into.end(), what.begin(), what.end());
}

/**
 * An .spc written here, so the ripper can be exercised without one on disk.
 *
 * `samples` is a list of (address, blocks) -- the ripper is meant to find them
 * through the directory the DSP names, so the test puts them at addresses the
 * directory points at rather than anywhere convenient.
 */
struct SpcSpec {
    // Away from where the tests put samples: the directory lives in the same
    // 64KB as everything else, and a sample written over it is a bug in the
    // test rather than in the ripper.
    uint8_t directory_page{0x50};
    // address in ARAM, BRR bytes, and the loop address the entry declares.
    struct Entry { uint16_t start; std::vector<uint8_t> brr; uint16_t loop; bool present{true}; };
    std::vector<Entry> entries;
};

std::string build_spc(const SpcSpec& spec) {
    std::string file(0x10180, '\0');
    // The header the format begins with.
    const std::string tag = "SNES-SPC700 Sound File Data v0.30";
    std::copy(tag.begin(), tag.end(), file.begin());

    auto aram_at = [&file](size_t address) { return file.begin() + 0x100 + address; };

    const size_t directory = static_cast<size_t>(spec.directory_page) << 8;
    for (size_t i = 0; i < spec.entries.size(); ++i) {
        const auto& entry = spec.entries[i];
        const size_t at = directory + i * 4;
        if (entry.present) {
            *aram_at(at)     = static_cast<char>(entry.start & 0xFF);
            *aram_at(at + 1) = static_cast<char>((entry.start >> 8) & 0xFF);
            *aram_at(at + 2) = static_cast<char>(entry.loop & 0xFF);
            *aram_at(at + 3) = static_cast<char>((entry.loop >> 8) & 0xFF);
        }
        std::copy(entry.brr.begin(), entry.brr.end(), aram_at(entry.start));
    }

    // DSP register $5D: where the directory lives. Without this the ripper has
    // nowhere to look, which is the point of the register.
    file[0x10100 + 0x5D] = static_cast<char>(spec.directory_page);
    return file;
}

/** Four blocks of a constant-ish sample, ending properly. */
std::vector<uint8_t> simple_sample(int blocks = 4) {
    std::vector<uint8_t> out;
    for (int b = 0; b < blocks; ++b) {
        const bool last = (b == blocks - 1);
        // Filter 0, so each nibble is the sample and nothing is predicted.
        append(out, brr_block(8, 0, last, last, {4, -4, 4, -4, 4, -4, 4, -4,
                                                 4, -4, 4, -4, 4, -4, 4, -4}));
    }
    return out;
}

} // namespace

// ── BRR ──────────────────────────────────────────────────────────────────────

TEST_CASE("filter 0 decodes each nibble on its own") {
    const auto block = brr_block(1, 0, false, true, {1, 2, 3, -1, -2, -3, 7, -8,
                                                     0, 0, 0, 0, 0, 0, 0, 0});
    const auto out = io::decode_brr(block, 0);

    REQUIRE(out.samples.size() == 16);
    CHECK(out.ended);
    // range 1 means the nibble is shifted left one and then right one -- back
    // to itself -- and the result is presented over 16384.
    CHECK(out.samples[0] == doctest::Approx(1.0f / 16384.0f));
    CHECK(out.samples[2] == doctest::Approx(3.0f / 16384.0f));
    CHECK(out.samples[3] == doctest::Approx(-1.0f / 16384.0f));
    CHECK(out.samples[7] == doctest::Approx(-8.0f / 16384.0f));
}

TEST_CASE("the range shifts a nibble up") {
    const auto quiet = io::decode_brr(brr_block(1, 0, false, true, {4}), 0);
    const auto loud  = io::decode_brr(brr_block(5, 0, false, true, {4}), 0);

    // Four more bits of shift is sixteen times the amplitude.
    CHECK(loud.samples[0] == doctest::Approx(quiet.samples[0] * 16.0f));
}

TEST_CASE("filter 1 predicts from the previous sample") {
    // One nibble to set a value, then zeroes: what comes out is the predictor
    // decaying on its own, which is what makes filter 1 identifiable.
    const auto block = brr_block(8, 1, false, true, {7, 0, 0, 0, 0, 0, 0, 0,
                                                     0, 0, 0, 0, 0, 0, 0, 0});
    const auto out = io::decode_brr(block, 0);
    REQUIRE(out.samples.size() == 16);

    // p1 * 15/16 each step, so it falls but stays the same sign.
    CHECK(out.samples[0] > 0.0f);
    for (size_t i = 1; i < 8; ++i) {
        CHECK(out.samples[i] > 0.0f);
        CHECK(out.samples[i] < out.samples[i - 1]);
    }
    CHECK(out.samples[1] == doctest::Approx(out.samples[0] * 15.0f / 16.0f).epsilon(0.02));
}

TEST_CASE("decoding stops at the end flag") {
    std::vector<uint8_t> data;
    append(data, brr_block(8, 0, false, false, {1}));
    append(data, brr_block(8, 0, false, true, {1}));    // end here
    append(data, brr_block(8, 0, false, false, {1}));   // must not be read

    const auto out = io::decode_brr(data, 0);
    CHECK(out.samples.size() == 32);
    CHECK(out.ended);
}

TEST_CASE("data with no end flag stops at the buffer rather than running off it") {
    std::vector<uint8_t> data;
    for (int i = 0; i < 3; ++i) append(data, brr_block(8, 0, false, false, {1}));

    const auto out = io::decode_brr(data, 0);
    CHECK(out.samples.size() == 48);
    CHECK_FALSE(out.ended);      // and the caller can tell
}

TEST_CASE("a partial trailing block is not decoded") {
    auto data = brr_block(8, 0, false, false, {1});
    data.resize(data.size() + 5);     // half a block
    const auto out = io::decode_brr(data, 0);
    CHECK(out.samples.size() == 16);
}

TEST_CASE("the loop address becomes a sample index") {
    std::vector<uint8_t> data;
    append(data, brr_block(8, 0, false, false, {1}));
    append(data, brr_block(8, 0, true, false, {1}));
    append(data, brr_block(8, 0, false, true, {1}));

    // The second block starts at byte 9 and at sample 16.
    const auto out = io::decode_brr(data, 0, /*loop_offset*/ 9);
    CHECK(out.loop_start == 16);
}

TEST_CASE("a loop pointing past the data is no loop") {
    const auto out = io::decode_brr(brr_block(8, 0, false, true, {1}), 0, /*loop*/ 900);
    CHECK(out.loop_start == -1);
}

TEST_CASE("starting past the end decodes nothing") {
    const auto out = io::decode_brr(brr_block(8, 0, false, true, {1}), 500);
    CHECK(out.samples.empty());
    CHECK_FALSE(out.ended);
}

// ── The .spc itself ──────────────────────────────────────────────────────────

TEST_CASE("samples are found through the directory the DSP names") {
    SpcSpec spec;
    spec.directory_page = 0x50;
    spec.entries.push_back({0x1000, simple_sample(), 0x1000});
    spec.entries.push_back({0x1200, simple_sample(), 0x1200});

    const auto bank = io::parse_spc(build_spc(spec), "Rip");
    REQUIRE(bank.has_value());
    CHECK((*bank)->samples.size() == 2);
    CHECK((*bank)->for_program(0) != nullptr);
    CHECK((*bank)->for_program(1) != nullptr);
    CHECK((*bank)->for_program(2) == nullptr);
}

TEST_CASE("the directory is read from wherever the register points") {
    SpcSpec spec;
    spec.directory_page = 0x6C;      // somewhere else entirely
    spec.entries.push_back({0x3000, simple_sample(), 0x3000});

    const auto bank = io::parse_spc(build_spc(spec), "Rip");
    REQUIRE(bank.has_value());
    CHECK((*bank)->samples.size() == 1);
}

TEST_CASE("a rip carries the chip's rate, not the host's") {
    SpcSpec spec;
    spec.entries.push_back({0x1000, simple_sample(), 0x1000});

    const auto bank = io::parse_spc(build_spc(spec), "Rip");
    REQUIRE(bank.has_value());
    // 32kHz is what the samples were recorded for; playing them at the host's
    // rate without saying so would transpose the whole rip.
    CHECK((*bank)->samples[0].source_rate == 32000);
}

TEST_CASE("a declared loop reaches the sample") {
    SpcSpec spec;
    // Loop at the second block: 9 bytes in, 16 samples in.
    spec.entries.push_back({0x1000, simple_sample(), static_cast<uint16_t>(0x1000 + 9)});

    const auto bank = io::parse_spc(build_spc(spec), "Rip");
    REQUIRE(bank.has_value());
    const auto& sample = (*bank)->samples[0];
    CHECK(sample.loop_start == 16);
    CHECK(sample.loop_end == static_cast<int>(sample.data.size()));
}

TEST_CASE("empty directory entries are skipped, not treated as the end") {
    SpcSpec spec;
    spec.entries.push_back({0x1000, simple_sample(), 0x1000});
    spec.entries.push_back({0, {}, 0, /*present*/ false});      // a hole
    spec.entries.push_back({0x1200, simple_sample(), 0x1200});

    // A game may use entry 0 and entry 2 and nothing between; stopping at the
    // hole would lose half the rip.
    const auto bank = io::parse_spc(build_spc(spec), "Rip");
    REQUIRE(bank.has_value());
    CHECK((*bank)->samples.size() == 2);
}

TEST_CASE("an entry pointing at something that is not a sample is skipped") {
    SpcSpec spec;
    spec.entries.push_back({0x1000, simple_sample(), 0x1000});
    // Points into memory with no end flag anywhere: it decodes to the end of
    // ARAM, which is how you can tell it was never a sample.
    spec.entries.push_back({0xF000, {}, 0xF000});

    const auto bank = io::parse_spc(build_spc(spec), "Rip");
    REQUIRE(bank.has_value());
    CHECK((*bank)->samples.size() == 1);
}

TEST_CASE("a file that is not an SPC is refused") {
    CHECK_FALSE(io::parse_spc("", "x").has_value());
    CHECK_FALSE(io::parse_spc(std::string(0x20000, '\0'), "x").has_value());

    std::string wrong = build_spc({});
    wrong.replace(0, 8, "NOT-SPC!");
    CHECK_FALSE(io::parse_spc(wrong, "x").has_value());
}

TEST_CASE("a truncated SPC is refused rather than read past its end") {
    SpcSpec spec;
    spec.entries.push_back({0x1000, simple_sample(), 0x1000});
    const auto whole = build_spc(spec);

    for (size_t cut = 1; cut < whole.size(); cut += 4093) {
        const auto bank = io::parse_spc(whole.substr(0, cut), "x");
        CHECK_FALSE(bank.has_value());       // nothing short of the whole snapshot
    }
}

TEST_CASE("an SPC with no samples anywhere is refused rather than returning nothing") {
    // A rip that yields no instruments is a failure the user has to be told
    // about; an empty bank would be silence with no explanation.
    const auto bank = io::parse_spc(build_spc({}), "Rip");
    REQUIRE_FALSE(bank.has_value());
    CHECK(bank.error().code == base::ErrorCode::ParseFailure);
}

TEST_CASE("no more than 128 samples are mapped") {
    SpcSpec spec;
    for (int i = 0; i < 200; ++i) {
        // Spread far enough apart that the samples do not overlap.
        spec.entries.push_back({static_cast<uint16_t>(0x1000 + i * 64), simple_sample(), 0});
    }
    const auto bank = io::parse_spc(build_spc(spec), "Rip");
    REQUIRE(bank.has_value());
    CHECK((*bank)->samples.size() <= 128);
}

// ── The rip, played ──────────────────────────────────────────────────────────

TEST_CASE("a ripped bank renders without falling over") {
    // The rip is the first bank whose loop points come from decoded audio
    // rather than from a file that stated them, so this is where an index the
    // voice trusts could be one the sample does not have.
    SpcSpec spec;
    spec.entries.push_back({0x1000, simple_sample(8), static_cast<uint16_t>(0x1000 + 9)});
    spec.entries.push_back({0x1400, simple_sample(4), 0x1400});

    const auto bank = io::parse_spc(build_spc(spec), "Rip");
    REQUIRE(bank.has_value());

    playback::Spc700Output out;
    out.set_sample_rate(48000);
    out.set_bank(*bank);
    REQUIRE(out.start().has_value());

    std::vector<float> buffer(512 * 2, 0.0f);
    for (int program = 0; program < 2; ++program) {
        out.program_change(0, static_cast<uint8_t>(program), 0);
        for (int pitch = 24; pitch <= 108; pitch += 6) {
            out.note_on(0, static_cast<uint8_t>(pitch), 100, 0);
            for (int block = 0; block < 8; ++block) {
                out.begin_block(static_cast<int64_t>(block) * 10000);
                out.render(buffer.data(), 512);
            }
            out.note_off(0, static_cast<uint8_t>(pitch), 0);
        }
    }
    // Reaching here at all is the assertion; every sample read has to have
    // stayed inside the data the rip produced.
    CHECK(out.dropped_events() == 0);
}
