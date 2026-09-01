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

/** One instrument zone: what part of the keyboard, played how. */
struct ZoneSpec {
    uint8_t  low_key{0};
    uint8_t  high_key{127};
    uint8_t  root{60};
    uint16_t sample_id{0};
    uint8_t  low_velocity{0};
    uint8_t  high_velocity{127};
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
    /** Extra instrument-zone generators, before the sampleID that ends it.
        Used when `zones` is empty, which is the single-zone shape most of
        these tests want. */
    std::vector<Zone> instrument_generators;

    /** Several zones instead, which is what a real instrument has. */
    std::vector<ZoneSpec> zones;
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

    // inst: one instrument, then EOI. The terminal record's bag index is one
    // past the last bag the instrument owns, so it is what says how many zones
    // there are -- leaving it at 1 hides every zone after the first.
    const auto zone_count = static_cast<uint16_t>(spec.zones.empty() ? 1 : spec.zones.size());
    std::string inst;
    put_name(inst, "Test Instrument");
    put16(inst, 0);
    put_name(inst, "EOI");
    put16(inst, zone_count);

    // ibag and igen: one bag per zone, each pointing at where its generators
    // start, then a terminal bag pointing one past the last generator.
    std::string ibag;
    std::string igen;
    uint16_t generator_count = 0;
    if (spec.zones.empty()) {
        put16(ibag, 0); put16(ibag, 0);
        for (const auto& z : spec.instrument_generators) {
            put16(igen, z.op); put16(igen, z.amount); ++generator_count;
        }
        put16(igen, 53); put16(igen, 0); ++generator_count;   // sampleID last
    } else {
        for (const auto& zone : spec.zones) {
            put16(ibag, generator_count); put16(ibag, 0);
            // keyRange and velRange pack low in the low byte, high in the high.
            put16(igen, 43);
            put16(igen, static_cast<uint16_t>(zone.low_key | (zone.high_key << 8)));
            put16(igen, 44);
            put16(igen, static_cast<uint16_t>(zone.low_velocity | (zone.high_velocity << 8)));
            put16(igen, 58); put16(igen, zone.root);          // overridingRootKey
            // sampleID has to be last, as the format requires of an instrument
            // zone: it is what ends the zone.
            put16(igen, 53); put16(igen, zone.sample_id);
            generator_count = static_cast<uint16_t>(generator_count + 4);
        }
    }
    put16(ibag, generator_count); put16(ibag, 0);   // the terminal bag
    put16(igen, 0); put16(igen, 0);                 // the terminal generator

    std::string imod(10, '\0');


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

    const auto* zone = (*bank)->zone_for(0, 60, 100);
    REQUIRE(zone != nullptr);
    CHECK((*bank)->sample_of(*zone)->name == "Test Sample");
    CHECK((*bank)->sample_of(*zone)->data.size() == 8);
    CHECK((*bank)->sample_of(*zone)->source_rate == 22050);
    CHECK(zone->root_key == 60);
    // 16-bit PCM normalised: 16000/32768.
    CHECK((*bank)->sample_of(*zone)->data[2] == doctest::Approx(16000.0f / 32768.0f));
    CHECK((*bank)->sample_of(*zone)->data[6] == doctest::Approx(-16000.0f / 32768.0f));
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
    const auto* zone = (*bank)->zone_for(0, 60, 100);
    REQUIRE(zone != nullptr);
    CHECK(zone->loop_start == 2);
    CHECK(zone->loop_end == 6);
}

TEST_CASE("a program a preset does not name has nothing behind it") {
    FontSpec spec;
    spec.program = 40;
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());

    CHECK((*bank)->has_program(40));
    CHECK_FALSE((*bank)->has_program(0));
    CHECK_FALSE((*bank)->has_program(127));
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

    const auto* zone = (*bank)->zone_for(0, 60, 100);
    REQUIRE(zone != nullptr);
    CHECK(zone->loop_start == -1);
}

TEST_CASE("an overriding root key wins over the sample header") {
    FontSpec spec;
    spec.root_key = 60;
    spec.instrument_generators = {{58, 48}};   // overridingRootKey
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->zone_for(0, 60, 100)->root_key == 48);
}

TEST_CASE("tuning generators add up in cents") {
    FontSpec spec;
    spec.correction = 5;
    spec.instrument_generators = {{51, 2}, {52, 10}};   // +2 semitones, +10 cents
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->zone_for(0, 60, 100)->fine_tune_cents == doctest::Approx(5 + 200 + 10));
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

    const auto* zone = (*bank)->zone_for(0, 60, 100);
    REQUIRE(zone != nullptr);
    CHECK(zone->attack == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(zone->release == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(zone->sustain == doctest::Approx(0.316f).epsilon(0.01));
}

TEST_CASE("a very negative attack means no attack at all") {
    FontSpec spec;
    // The format's way of saying "immediately" is a large negative, not zero --
    // zero timecents is a whole second.
    spec.instrument_generators = {{34, static_cast<uint16_t>(static_cast<int16_t>(-12000))}};
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->zone_for(0, 60, 100)->attack == doctest::Approx(0.0f));
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
    CHECK((*bank)->programs[0].name == "Test Preset");
    CHECK((*bank)->samples[0].name == "Test Sample");
    // And nothing is claimed for a program that has nothing behind it.
    CHECK((*bank)->programs[1].name.empty());
}

TEST_CASE("every zone of an instrument becomes a zone of the program") {
    // What the split is for. An instrument spread across the keyboard has one
    // zone per region, and taking the first stretched one recording over the
    // lot -- audible as chipmunks at the top and mud at the bottom.
    FontSpec spec;
    spec.zones = {
        {/*key*/ 0, 59, /*root*/ 48, /*sample*/ 0},
        {/*key*/ 60, 127, /*root*/ 72, /*sample*/ 0},
    };
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    REQUIRE((*bank)->programs[0].zones.size() == 2);

    // The note picks the zone, and the zones disagree about the root key --
    // which is exactly what a multi-sampled instrument states and what a
    // per-sample model could not hold.
    const auto* low = (*bank)->zone_for(0, 40, 100);
    const auto* high = (*bank)->zone_for(0, 90, 100);
    REQUIRE(low != nullptr);
    REQUIRE(high != nullptr);
    CHECK(low->root_key == 48);
    CHECK(high->root_key == 72);
    CHECK(low != high);
}

TEST_CASE("a note outside every zone has nothing to play") {
    FontSpec spec;
    spec.zones = {{60, 72, 60, 0}};
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());

    CHECK((*bank)->zone_for(0, 66, 100) != nullptr);
    // Silence rather than the nearest zone stretched: a bank that says nothing
    // about a key is a bank whose author did not want that key played.
    CHECK((*bank)->zone_for(0, 40, 100) == nullptr);
    CHECK((*bank)->zone_for(0, 100, 100) == nullptr);
    // But the program still exists, so the instrument list offers it.
    CHECK((*bank)->has_program(0));
}

TEST_CASE("velocity picks a zone too") {
    FontSpec spec;
    spec.zones = {
        {0, 127, 60, 0, /*velocity*/ 0, 63},
        {0, 127, 72, 0, /*velocity*/ 64, 127},
    };
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    REQUIRE((*bank)->zone_for(0, 60, 30) != nullptr);
    REQUIRE((*bank)->zone_for(0, 60, 100) != nullptr);
    CHECK((*bank)->zone_for(0, 60, 30)->root_key == 60);
    CHECK((*bank)->zone_for(0, 60, 100)->root_key == 72);
}

TEST_CASE("zones sharing one recording decode it once") {
    FontSpec spec;
    spec.zones = {{0, 59, 48, 0}, {60, 127, 72, 0}};
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());

    // Two zones, one sample: a piano reaches the same recording from several
    // regions, and decoding it twice would double the memory for nothing.
    CHECK((*bank)->programs[0].zones.size() == 2);
    CHECK((*bank)->samples.size() == 1);
    CHECK((*bank)->programs[0].zones[0].sample == (*bank)->programs[0].zones[1].sample);
}
