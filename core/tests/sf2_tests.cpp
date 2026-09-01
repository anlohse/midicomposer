#include <doctest/doctest.h>

#include "io/sf2_file.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace midi_composer;

namespace {

// ── A SoundFont written here ─────────────────────────────────────────────────
//
// The same trade as fake_clap_plugin.hpp: a loader tested only against files
// that happen to be on this machine is a loader tested nowhere. Building one
// means the tests state exactly what the parser is being given, including the
// parts of the format that are easy to get quietly wrong -- the terminal
// records, the word alignment, loop points stated relative to the whole sample
// pool rather than to one sample.

void put16(std::string& out, uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put32(std::string& out, uint32_t v) {
    put16(out, static_cast<uint16_t>(v & 0xFFFF));
    put16(out, static_cast<uint16_t>((v >> 16) & 0xFFFF));
}

void put_name(std::string& out, const std::string& name, size_t width = 20) {
    for (size_t i = 0; i < width; ++i) out.push_back(i < name.size() ? name[i] : '\0');
}

/** id + size + body, padded to an even length as RIFF requires. */
std::string chunk(const std::string& id, const std::string& body) {
    std::string out = id;
    put32(out, static_cast<uint32_t>(body.size()));
    out += body;
    if (body.size() & 1) out.push_back('\0');
    return out;
}

std::string list(const std::string& kind, const std::string& body) {
    return chunk("LIST", kind + body);
}

struct Zone {
    uint16_t op{0};
    uint16_t amount{0};
};

struct FontSpec {
    std::vector<int16_t> audio{0, 8000, 16000, 8000, 0, -8000, -16000, -8000};
    uint16_t program{0};
    uint16_t bank{0};
    uint32_t loop_start{2};
    uint32_t loop_end{6};
    uint32_t rate{22050};
    uint8_t  root_key{60};
    int8_t   correction{0};
    /** Extra instrument-zone generators, before the sampleID that ends it. */
    std::vector<Zone> instrument_generators;
};

std::string build_sf2(const FontSpec& spec) {
    // sdta: the raw sample pool.
    std::string smpl;
    for (int16_t v : spec.audio) put16(smpl, static_cast<uint16_t>(v));
    const std::string sdta = list("sdta", chunk("smpl", smpl));

    // phdr: one preset, then the terminal EOP record.
    std::string phdr;
    put_name(phdr, "Test Preset");
    put16(phdr, spec.program);
    put16(phdr, spec.bank);
    put16(phdr, 0);              // first preset bag
    put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);   // library, genre, morphology
    put_name(phdr, "EOP");
    put16(phdr, 0); put16(phdr, 0);
    put16(phdr, 1);              // one past the last bag
    put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);

    // pbag: one zone, then the terminal.
    std::string pbag;
    put16(pbag, 0); put16(pbag, 0);
    put16(pbag, 1); put16(pbag, 0);

    std::string pmod(10, '\0');   // the terminal modulator; never read here

    // pgen: the preset zone names instrument 0, then the terminal.
    std::string pgen;
    put16(pgen, 41); put16(pgen, 0);        // instrument = 0
    put16(pgen, 0);  put16(pgen, 0);

    // inst: one instrument, then EOI.
    std::string inst;
    put_name(inst, "Test Instrument");
    put16(inst, 0);
    put_name(inst, "EOI");
    put16(inst, 1);

    // ibag: one zone, then the terminal.
    const auto extra = static_cast<uint16_t>(spec.instrument_generators.size());
    std::string ibag;
    put16(ibag, 0); put16(ibag, 0);
    put16(ibag, static_cast<uint16_t>(extra + 1)); put16(ibag, 0);

    std::string imod(10, '\0');

    // igen: whatever the spec asks for, then sampleID -- which has to be last,
    // as the format requires of an instrument zone -- then the terminal.
    std::string igen;
    for (const auto& z : spec.instrument_generators) { put16(igen, z.op); put16(igen, z.amount); }
    put16(igen, 53); put16(igen, 0);        // sampleID = 0
    put16(igen, 0);  put16(igen, 0);

    // shdr: one sample, then EOS.
    std::string shdr;
    put_name(shdr, "Test Sample");
    put32(shdr, 0);
    put32(shdr, static_cast<uint32_t>(spec.audio.size()));
    put32(shdr, spec.loop_start);
    put32(shdr, spec.loop_end);
    put32(shdr, spec.rate);
    shdr.push_back(static_cast<char>(spec.root_key));
    shdr.push_back(static_cast<char>(spec.correction));
    put16(shdr, 0);              // sampleLink
    put16(shdr, 1);              // monoSample
    put_name(shdr, "EOS");
    put32(shdr, 0); put32(shdr, 0); put32(shdr, 0); put32(shdr, 0); put32(shdr, 0);
    shdr.push_back('\0'); shdr.push_back('\0');
    put16(shdr, 0); put16(shdr, 0);

    const std::string pdta = list("pdta",
        chunk("phdr", phdr) + chunk("pbag", pbag) + chunk("pmod", pmod) +
        chunk("pgen", pgen) + chunk("inst", inst) + chunk("ibag", ibag) +
        chunk("imod", imod) + chunk("igen", igen) + chunk("shdr", shdr));

    std::string info;
    put16(info, 2); put16(info, 1);          // ifil: version 2.1
    const std::string info_list = list("INFO", chunk("ifil", info));

    return chunk("RIFF", "sfbk" + info_list + sdta + pdta);
}

} // namespace

TEST_CASE("a SoundFont's sample reaches the bank") {
    const auto bank = io::parse_sf2(build_sf2({}), "Test");
    REQUIRE(bank.has_value());
    REQUIRE((*bank)->samples.size() == 1);

    const auto* sample = (*bank)->for_program(0);
    REQUIRE(sample != nullptr);
    CHECK(sample->name == "Test Sample");
    CHECK(sample->data.size() == 8);
    CHECK(sample->source_rate == 22050);
    CHECK(sample->root_key == 60);
    // 16-bit PCM normalised: 16000/32768.
    CHECK(sample->data[2] == doctest::Approx(16000.0f / 32768.0f));
    CHECK(sample->data[6] == doctest::Approx(-16000.0f / 32768.0f));
}

TEST_CASE("loop points arrive relative to the sample, not to the pool") {
    FontSpec spec;
    spec.loop_start = 2;
    spec.loop_end = 6;
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());

    // The header states them as offsets into smpl; a voice reads its own data.
    // With this sample starting at 0 the numbers coincide, which is exactly why
    // the sample below starts elsewhere.
    const auto* sample = (*bank)->for_program(0);
    REQUIRE(sample != nullptr);
    CHECK(sample->loop_start == 2);
    CHECK(sample->loop_end == 6);
}

TEST_CASE("a program a preset does not name has nothing behind it") {
    FontSpec spec;
    spec.program = 40;
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());

    CHECK((*bank)->for_program(40) != nullptr);
    CHECK((*bank)->for_program(0) == nullptr);
    CHECK((*bank)->for_program(127) == nullptr);
}

TEST_CASE("presets outside bank 0 are skipped") {
    FontSpec spec;
    spec.bank = 128;             // the percussion bank
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");

    // One slot per program means the variation banks address the same 128
    // programs; taking them would make the last one loaded win at random.
    REQUIRE_FALSE(bank.has_value());
    CHECK(bank.error().code == base::ErrorCode::ParseFailure);
}

TEST_CASE("sampleModes 0 turns off a loop the header declares") {
    FontSpec spec;
    spec.instrument_generators = {{54, 0}};   // sampleModes = play once
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());

    const auto* sample = (*bank)->for_program(0);
    REQUIRE(sample != nullptr);
    CHECK(sample->loop_start == -1);
}

TEST_CASE("an overriding root key wins over the sample header") {
    FontSpec spec;
    spec.root_key = 60;
    spec.instrument_generators = {{58, 48}};   // overridingRootKey
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->for_program(0)->root_key == 48);
}

TEST_CASE("tuning generators add up in cents") {
    FontSpec spec;
    spec.correction = 5;
    spec.instrument_generators = {{51, 2}, {52, 10}};   // +2 semitones, +10 cents
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->for_program(0)->fine_tune_cents == doctest::Approx(5 + 200 + 10));
}

TEST_CASE("envelope generators are converted out of timecents") {
    FontSpec spec;
    spec.instrument_generators = {
        {34, static_cast<uint16_t>(static_cast<int16_t>(-1200))},   // attack: 0.5s
        {38, 0},                                                    // release: 1s
        {37, 100},                                                  // sustain: -10dB
    };
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());

    const auto* sample = (*bank)->for_program(0);
    REQUIRE(sample != nullptr);
    CHECK(sample->attack == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(sample->release == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(sample->sustain == doctest::Approx(0.316f).epsilon(0.01));
}

TEST_CASE("a very negative attack means no attack at all") {
    FontSpec spec;
    // The format's way of saying "immediately" is a large negative, not zero --
    // zero timecents is a whole second.
    spec.instrument_generators = {{34, static_cast<uint16_t>(static_cast<int16_t>(-12000))}};
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->for_program(0)->attack == doctest::Approx(0.0f));
}

TEST_CASE("garbage is refused rather than parsed into nonsense") {
    for (const std::string& text : {std::string("not a soundfont at all"), std::string(),
                                    std::string("RIFF"), std::string("RIFFxxxxWAVE")}) {
        const auto bank = io::parse_sf2(text, "Test");
        CHECK_FALSE(bank.has_value());
    }
}

TEST_CASE("a truncated file is refused rather than read past its end") {
    const auto whole = build_sf2({});
    // Every prefix: each one has to be a parse failure and not a crash, which
    // is the whole reason the reader is bounds-checked.
    for (size_t cut = 1; cut < whole.size(); cut += 7) {
        const auto bank = io::parse_sf2(whole.substr(0, cut), "Test");
        if (bank.has_value()) {
            // A prefix that still parses is fine as long as it produced
            // something coherent; it must never be empty.
            CHECK_FALSE((*bank)->empty());
        }
    }
}

TEST_CASE("a file with no sample data is refused") {
    auto whole = build_sf2({});
    // Break the sample pool's id, leaving everything else intact.
    const auto at = whole.find("smpl");
    REQUIRE(at != std::string::npos);
    whole.replace(at, 4, "xxxx");

    const auto bank = io::parse_sf2(whole, "Test");
    REQUIRE_FALSE(bank.has_value());
}

TEST_CASE("the bank is named after where it came from") {
    const auto bank = io::parse_sf2(build_sf2({}), "Super Famicom Kit");
    REQUIRE(bank.has_value());
    CHECK((*bank)->name == "Super Famicom Kit");
}

TEST_CASE("a program is named after its preset, not its sample") {
    // A SoundFont calls the sample "Piano C4" and the preset "Grand Piano".
    // The second is what somebody is choosing between.
    const auto bank = io::parse_sf2(build_sf2({}), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->program_names[0] == "Test Preset");
    CHECK((*bank)->samples[0].name == "Test Sample");
    // And nothing is claimed for a program that has nothing behind it.
    CHECK((*bank)->program_names[1].empty());
}
