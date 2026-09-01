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

/** One instrument: the zones it owns, or bare generators when it has one
    zone and the test only cares what that zone says. */
struct InstrumentSpec {
    std::vector<ZoneSpec> zones;
    std::vector<Zone>     generators;
};

/** One preset zone: which instrument it names, over what part of the keyboard,
    and the generators the preset lays on top of that instrument's own. Several
    of these is a preset that layers. */
struct PresetZoneSpec {
    uint16_t instrument{0};
    uint8_t  low_key{0};
    uint8_t  high_key{127};
    uint8_t  low_velocity{0};
    uint8_t  high_velocity{127};
    std::vector<Zone> generators;
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

    /** More than one instrument, for a preset that layers them. Empty means the
        single instrument described by `zones` / `instrument_generators`. */
    std::vector<InstrumentSpec> instruments;

    /** More than one preset zone, which is how a preset layers. Empty means one
        zone naming instrument 0 over the whole keyboard, which is the shape
        most of these tests want. */
    std::vector<PresetZoneSpec> preset_zones;

    /** The global zone: generators stated once, ahead of the zones they apply
        to, in a bag that names no sample (or no instrument, at the preset
        level). Written for every instrument in the font, which is more than a
        real one would do and is all a test needs. */
    std::vector<Zone> instrument_global;
    std::vector<Zone> preset_global;
};

std::string build_sf2(const FontSpec& spec) {
    // sdta: the raw sample pool.
    std::string smpl;
    for (int16_t v : spec.audio) put16(smpl, static_cast<uint16_t>(v));
    const std::string sdta = list("sdta", chunk("smpl", smpl));

    // Both levels are normalised to vectors so the writer below has one shape
    // to deal with; the defaults are the single preset zone and single
    // instrument that most of these tests want.
    std::vector<PresetZoneSpec> preset_zones = spec.preset_zones;
    if (preset_zones.empty()) preset_zones.push_back({});

    std::vector<InstrumentSpec> instruments = spec.instruments;
    if (instruments.empty()) {
        instruments.push_back({spec.zones, spec.instrument_generators});
    }

    // phdr: one preset, then the terminal EOP record.
    std::string phdr;
    put_name(phdr, "Test Preset");
    put16(phdr, spec.program);
    put16(phdr, spec.bank);
    put16(phdr, 0);              // first preset bag
    put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);   // library, genre, morphology
    put_name(phdr, "EOP");
    put16(phdr, 0); put16(phdr, 0);
    put16(phdr, static_cast<uint16_t>(
        preset_zones.size() + (spec.preset_global.empty() ? 0 : 1)));   // one past the last bag
    put32(phdr, 0); put32(phdr, 0); put32(phdr, 0);

    // pbag and pgen: one bag per preset zone, then the terminal. Ranges are
    // written only when they say something, which is what a real font does --
    // and it keeps the single-zone default byte for byte what it was.
    std::string pbag;
    std::string pgen;
    uint16_t preset_generator_count = 0;
    uint16_t preset_bag_count = 0;
    if (!spec.preset_global.empty()) {
        // A bag with generators and no instrument: the format knows it is the
        // global zone by what it does not say.
        put16(pbag, preset_generator_count); put16(pbag, 0);
        ++preset_bag_count;
        for (const auto& generator : spec.preset_global) {
            put16(pgen, generator.op); put16(pgen, generator.amount);
            ++preset_generator_count;
        }
    }
    for (const auto& zone : preset_zones) {
        ++preset_bag_count;
        put16(pbag, preset_generator_count); put16(pbag, 0);
        if (zone.low_key != 0 || zone.high_key != 127) {
            put16(pgen, 43);
            put16(pgen, static_cast<uint16_t>(zone.low_key | (zone.high_key << 8)));
            ++preset_generator_count;
        }
        if (zone.low_velocity != 0 || zone.high_velocity != 127) {
            put16(pgen, 44);
            put16(pgen, static_cast<uint16_t>(zone.low_velocity | (zone.high_velocity << 8)));
            ++preset_generator_count;
        }
        for (const auto& generator : zone.generators) {
            put16(pgen, generator.op); put16(pgen, generator.amount);
            ++preset_generator_count;
        }
        // instrument has to be last, as the format requires of a preset zone:
        // it is what ends the zone.
        put16(pgen, 41); put16(pgen, zone.instrument);
        ++preset_generator_count;
    }
    put16(pbag, preset_generator_count); put16(pbag, 0);   // the terminal bag
    put16(pgen, 0); put16(pgen, 0);                        // the terminal generator

    std::string pmod(10, '\0');   // the terminal modulator; never read here

    // inst: one record per instrument, then EOI. The terminal record's bag index
    // is one past the last bag any instrument owns, so it is what says how many
    // zones there are -- leaving it at 1 hides every zone after the first.
    //
    // ibag and igen: one bag per zone, each pointing at where its generators
    // start, then a terminal bag pointing one past the last generator.
    std::string inst;
    std::string ibag;
    std::string igen;
    uint16_t generator_count = 0;
    uint16_t bag_count = 0;
    for (size_t i = 0; i < instruments.size(); ++i) {
        put_name(inst, "Test Instrument " + std::to_string(i));
        put16(inst, bag_count);

        const auto& instrument = instruments[i];
        if (!spec.instrument_global.empty()) {
            put16(ibag, generator_count); put16(ibag, 0);
            ++bag_count;
            for (const auto& generator : spec.instrument_global) {
                put16(igen, generator.op); put16(igen, generator.amount);
                ++generator_count;
            }
        }
        if (instrument.zones.empty()) {
            put16(ibag, generator_count); put16(ibag, 0);
            ++bag_count;
            for (const auto& generator : instrument.generators) {
                put16(igen, generator.op); put16(igen, generator.amount);
                ++generator_count;
            }
            put16(igen, 53); put16(igen, 0); ++generator_count;   // sampleID last
        } else {
            for (const auto& zone : instrument.zones) {
                put16(ibag, generator_count); put16(ibag, 0);
                ++bag_count;
                // keyRange and velRange pack low in the low byte, high in the high.
                put16(igen, 43);
                put16(igen, static_cast<uint16_t>(zone.low_key | (zone.high_key << 8)));
                put16(igen, 44);
                put16(igen, static_cast<uint16_t>(zone.low_velocity | (zone.high_velocity << 8)));
                put16(igen, 58); put16(igen, zone.root);          // overridingRootKey
                // sampleID has to be last, as the format requires of an
                // instrument zone: it is what ends the zone.
                put16(igen, 53); put16(igen, zone.sample_id);
                generator_count = static_cast<uint16_t>(generator_count + 4);
            }
        }
    }
    put_name(inst, "EOI");
    put16(inst, bag_count);

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
    spec.instrument_generators = {{54, 1}};   // sampleModes: loop while held
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

// ── Looping is opt-in ────────────────────────────────────────────────────────
//
// Almost every sample in a SoundFont carries loop points in its header whether
// or not it is meant to loop. Reading them was what made a timpani strike into
// a roll and a snare into a machine gun under a held note.

TEST_CASE("a zone that says nothing about looping does not loop") {
    FontSpec spec;
    spec.loop_start = 2;
    spec.loop_end = 6;                        // header loop points, and no mode
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());

    // The format's default for sampleModes is 0, which is play once. A percussion
    // hit is exactly the sample that states nothing and carries loop points.
    const auto* zone = (*bank)->zone_for(0, 60, 100);
    REQUIRE(zone != nullptr);
    CHECK(zone->loop_start == -1);
}

TEST_CASE("sampleModes 3 loops, having a tail this engine cannot stage") {
    FontSpec spec;
    spec.instrument_generators = {{54, 3}};   // loop until release, then the tail
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->zone_for(0, 60, 100)->loop_start == 2);
}

TEST_CASE("sampleModes 2 is unused and means play once") {
    FontSpec spec;
    spec.instrument_generators = {{54, 2}};
    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->zone_for(0, 60, 100)->loop_start == -1);
}

// ── The global zone ──────────────────────────────────────────────────────────
//
// An instrument's first bag may name no sample. That is not a zone that plays
// nothing; it is the defaults for every zone after it, and skipping it drops
// whatever the font chose to state once instead of on every zone. All 136
// instruments of ExpressiveSNES.sf2 have one, and between them they carry the
// release for 135, the decay for 116 and the loop mode for 122.

TEST_CASE("an instrument's global zone reaches the zones under it") {
    FontSpec spec;
    spec.instruments = {{{ZoneSpec{.root = 60}}, {}}};
    // A global zone is the one that names no sample, so it is written as a
    // preset zone's instrument would never be: generators and no sampleID.
    spec.instrument_global = {{54, 1}, {58, 48}};   // sampleModes, overridingRootKey

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    const auto* zone = (*bank)->zone_for(0, 60, 100);
    REQUIRE(zone != nullptr);
    CHECK(zone->loop_start == 2);      // the loop mode came from the global zone
}

TEST_CASE("a zone overrides its instrument's global zone") {
    FontSpec spec;
    spec.instruments = {{{ZoneSpec{.root = 72}}, {}}};
    spec.instrument_global = {{58, 48}};   // the global zone says root 48

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    // At this level the zone's own value wins outright -- an override, unlike
    // preset over instrument, which adds.
    CHECK((*bank)->zone_for(0, 60, 100)->root_key == 72);
}

TEST_CASE("a preset's global zone offsets every instrument it names") {
    FontSpec spec;
    spec.instruments = {{{ZoneSpec{}}, {}}, {{ZoneSpec{}}, {}}};
    spec.preset_zones = {{.instrument = 0}, {.instrument = 1}};
    spec.preset_global = {{52, 25}};   // fineTune, stated once for the preset

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    REQUIRE((*bank)->programs[0].zones.size() == 2);
    for (const auto& zone : (*bank)->programs[0].zones) {
        CHECK(zone.fine_tune_cents == doctest::Approx(25));
    }
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

// ── Layering ─────────────────────────────────────────────────────────────────
//
// A preset may name several instruments to sound together, and until these
// tests existed only the first was taken. Seven of the 128 programs in
// ExpressiveSNES.sf2 layer that way, all of them covering the whole keyboard
// twice over -- Honky-tonk is two pianos a few cents apart, and one of the two
// is just a piano.

TEST_CASE("a preset naming two instruments keeps both") {
    FontSpec spec;
    spec.instruments = {
        {{ZoneSpec{.root = 60}}, {}},
        {{ZoneSpec{.root = 72}}, {}},
    };
    spec.preset_zones = {{.instrument = 0}, {.instrument = 1}};

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    REQUIRE((*bank)->programs[0].zones.size() == 2);

    // Both answer the same note, which is what layering means.
    const playback::Zone* matched[8] = {};
    CHECK((*bank)->zones_for(0, 60, 100, matched, 8) == 2);
    CHECK(matched[0]->root_key == 60);
    CHECK(matched[1]->root_key == 72);
}

TEST_CASE("a preset zone narrows the instrument it names") {
    FontSpec spec;
    spec.instruments = {{{ZoneSpec{.low_key = 0, .high_key = 127}}, {}}};
    spec.preset_zones = {{.instrument = 0, .low_key = 60, .high_key = 72}};

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    REQUIRE((*bank)->programs[0].zones.size() == 1);

    // The narrower of the two wins on each side: the preset says where its
    // instrument applies, the instrument says which sample covers what.
    CHECK((*bank)->programs[0].zones[0].low_key == 60);
    CHECK((*bank)->programs[0].zones[0].high_key == 72);
    CHECK((*bank)->zone_for(0, 59, 100) == nullptr);
    CHECK((*bank)->zone_for(0, 66, 100) != nullptr);
    CHECK((*bank)->zone_for(0, 73, 100) == nullptr);
}

TEST_CASE("a zone the preset excludes is absent, not silent") {
    FontSpec spec;
    // Two zones splitting the keyboard, and a preset that only reaches the low
    // one. The high zone must not arrive at all -- a zone with a backwards
    // range would match nothing, but it would still be counted and named.
    spec.instruments = {{{ZoneSpec{.low_key = 0, .high_key = 59},
                         ZoneSpec{.low_key = 60, .high_key = 127}}, {}}};
    spec.preset_zones = {{.instrument = 0, .low_key = 0, .high_key = 50}};

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->programs[0].zones.size() == 1);
    CHECK((*bank)->programs[0].zones[0].high_key == 50);
}

TEST_CASE("velocity narrows the same way keys do") {
    FontSpec spec;
    spec.instruments = {{{ZoneSpec{.low_velocity = 0, .high_velocity = 127}}, {}}};
    spec.preset_zones = {{.instrument = 0, .low_velocity = 64, .high_velocity = 127}};

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->zone_for(0, 60, 30) == nullptr);
    CHECK((*bank)->zone_for(0, 60, 100) != nullptr);
}

TEST_CASE("a preset's tuning is an offset, not a replacement") {
    FontSpec spec;
    spec.instruments = {{{}, {Zone{52, static_cast<uint16_t>(10)}}}};   // fineTune +10
    spec.preset_zones = {{.instrument = 0,
                          .generators = {Zone{51, 1},                    // coarseTune +1
                                         Zone{52, static_cast<uint16_t>(20)}}}};

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    const auto* zone = (*bank)->zone_for(0, 60, 100);
    REQUIRE(zone != nullptr);
    // 10 from the instrument plus 20 from the preset, plus a semitone the
    // preset asked for on top.
    CHECK(zone->fine_tune_cents == doctest::Approx(100 + 30));
}

TEST_CASE("attenuation reaches the zone as a level") {
    FontSpec spec;
    // 100 centibels is 10 dB down, which is a tenth of the power and about
    // 0.316 of the amplitude.
    spec.preset_zones = {{.instrument = 0, .generators = {Zone{48, 100}}}};

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    const auto* zone = (*bank)->zone_for(0, 60, 100);
    REQUIRE(zone != nullptr);
    CHECK(zone->gain == doctest::Approx(0.3162f).epsilon(0.01));
}

TEST_CASE("a bank that says nothing about level says one") {
    const auto bank = io::parse_sf2(build_sf2({}), "Test");
    REQUIRE(bank.has_value());
    CHECK((*bank)->zone_for(0, 60, 100)->gain == doctest::Approx(1.0f));
}

TEST_CASE("an envelope offset from a preset adds in timecents, not in seconds") {
    FontSpec spec;
    // -7973 timecents is about 10ms; adding 1200 timecents doubles it, which
    // adding seconds could never do.
    spec.instruments = {{{}, {Zone{34, static_cast<uint16_t>(-7973)}}}};
    spec.preset_zones = {{.instrument = 0, .generators = {Zone{34, 1200}}}};

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    const auto* zone = (*bank)->zone_for(0, 60, 100);
    REQUIRE(zone != nullptr);
    CHECK(zone->attack == doctest::Approx(0.02f).epsilon(0.02));
}

TEST_CASE("zones_for stops where the caller runs out of room") {
    FontSpec spec;
    spec.instruments = {{{ZoneSpec{}}, {}}, {{ZoneSpec{}}, {}}, {{ZoneSpec{}}, {}}};
    spec.preset_zones = {{.instrument = 0}, {.instrument = 1}, {.instrument = 2}};

    const auto bank = io::parse_sf2(build_sf2(spec), "Test");
    REQUIRE(bank.has_value());
    REQUIRE((*bank)->programs[0].zones.size() == 3);

    const playback::Zone* matched[2] = {};
    // Two, not three and not a write past the end: a note cannot cost more
    // voices than the chip has, and the ceiling belongs to the caller.
    CHECK((*bank)->zones_for(0, 60, 100, matched, 2) == 2);
}
