#include "sf2_file.hpp"

#include "base/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>
#include <vector>

namespace midi_composer::io {

namespace {

using playback::Sample;
using playback::SampleBank;
using playback::Zone;

base::Error bad(const std::string& what) {
    return base::Error{base::ErrorCode::ParseFailure, "Not a usable SoundFont: " + what};
}

// ── Little-endian reads, bounds-checked ──────────────────────────────────────
//
// Every read goes through these. A SoundFont is a file someone downloaded, so
// a truncated or hostile one has to be a parse failure rather than a read past
// the end of a buffer.

class Reader {
public:
    Reader(const char* data, size_t size) : m_data(data), m_size(size) {}

    [[nodiscard]] bool has(size_t n) const { return m_pos + n <= m_size; }
    [[nodiscard]] size_t remaining() const { return m_size - m_pos; }
    [[nodiscard]] size_t position() const { return m_pos; }
    void seek(size_t pos) { m_pos = std::min(pos, m_size); }
    void skip(size_t n) { m_pos = std::min(m_pos + n, m_size); }

    uint8_t u8() {
        if (!has(1)) return 0;
        return static_cast<uint8_t>(m_data[m_pos++]);
    }
    uint16_t u16() {
        const uint16_t lo = u8();
        return static_cast<uint16_t>(lo | (static_cast<uint16_t>(u8()) << 8));
    }
    int16_t i16() { return static_cast<int16_t>(u16()); }
    uint32_t u32() {
        const uint32_t lo = u16();
        return lo | (static_cast<uint32_t>(u16()) << 16);
    }
    std::string fourcc() {
        if (!has(4)) return {};
        std::string out(m_data + m_pos, 4);
        m_pos += 4;
        return out;
    }
    /** A fixed-width name field; SF2 pads with NULs and does not always
        terminate. */
    std::string name(size_t width) {
        if (!has(width)) return {};
        const char* start = m_data + m_pos;
        const size_t len = static_cast<size_t>(
            std::find(start, start + width, '\0') - start);
        m_pos += width;
        return std::string(start, len);
    }

private:
    const char* m_data;
    size_t m_size;
    size_t m_pos{0};
};

// ── The records this loader needs ────────────────────────────────────────────

struct PresetHeader {
    std::string name;
    uint16_t program{0};
    uint16_t bank{0};
    uint16_t bag_index{0};
};

struct Bag {
    uint16_t gen_index{0};
};

struct Generator {
    uint16_t op{0};
    uint16_t amount{0};
};

struct Instrument {
    std::string name;
    uint16_t bag_index{0};
};

struct SampleHeader {
    std::string name;
    uint32_t start{0};
    uint32_t end{0};
    uint32_t loop_start{0};
    uint32_t loop_end{0};
    uint32_t rate{0};
    uint8_t  original_key{60};
    int8_t   correction{0};
    uint16_t type{1};
};

// The generator operators used here. The specification numbers many more; these
// are the ones that change what a note sounds like in this simplified model.
constexpr uint16_t kGenInstrument       = 41;
constexpr uint16_t kGenSampleId         = 53;
constexpr uint16_t kGenSampleModes      = 54;
constexpr uint16_t kGenOverridingRootKey = 58;
constexpr uint16_t kGenCoarseTune       = 51;
constexpr uint16_t kGenFineTune         = 52;
constexpr uint16_t kGenAttackVolEnv     = 34;
constexpr uint16_t kGenDecayVolEnv      = 36;
constexpr uint16_t kGenSustainVolEnv    = 37;
constexpr uint16_t kGenReleaseVolEnv    = 38;
constexpr uint16_t kGenInitialAttenuation = 48;
constexpr uint16_t kGenKeyRange         = 43;
constexpr uint16_t kGenVelocityRange    = 44;

/** What the format says an envelope time means when nobody states one: a
    timecent value of -12000, which is a millisecond. Only reached when a preset
    offsets a generator its instrument left out, and an offset has to be an
    offset from something. */
constexpr int16_t kAbsentEnvelopeTimecents = -12000;

/**
 * The band a sample rate has to fall in to be believed.
 *
 * The format says a rate below 400 is not supported and names no ceiling. A
 * ceiling is needed anyway: the field is 32 bits, and a damaged one holding
 * four billion becomes a *negative* int on the way into the bank, which makes
 * a voice read backwards from where it started. Found by fuzzing the loader,
 * and reachable from any partly corrupt file.
 */
constexpr uint32_t kLowestRate = 400;
constexpr uint32_t kHighestRate = 192000;

/** Timecents to seconds. SF2 states envelope times logarithmically, and its
    "no time at all" value is a very large negative rather than zero. */
float timecents_to_seconds(int16_t timecents) {
    if (timecents <= -12000) return 0.0f;
    return static_cast<float>(std::pow(2.0, static_cast<double>(timecents) / 1200.0));
}

/** Centibels of attenuation to a linear level. 0 is full, 1000 is silence. */
float centibels_to_level(int16_t centibels) {
    if (centibels <= 0) return 1.0f;
    if (centibels >= 1000) return 0.0f;
    return static_cast<float>(std::pow(10.0, -static_cast<double>(centibels) / 200.0));
}

/** Every chunk directly inside `list`, by id. A chunk appearing twice keeps the
    first, which is what the specification says a reader should do. */
struct Chunk {
    size_t offset{0};
    size_t size{0};
};

std::map<std::string, Chunk> read_chunks(Reader& reader, size_t end) {
    std::map<std::string, Chunk> found;
    while (reader.position() + 8 <= end) {
        const auto id = reader.fourcc();
        const auto size = static_cast<size_t>(reader.u32());
        const size_t body = reader.position();
        if (body + size > end) break;          // truncated
        found.emplace(id, Chunk{body, size});
        // Chunks are word aligned, with the pad byte outside the stated size.
        reader.seek(body + size + (size & 1));
    }
    return found;
}

template <typename T, typename Fn>
std::vector<T> read_records(const std::string& bytes, const Chunk& chunk, size_t record_size,
                            Fn&& read_one) {
    std::vector<T> out;
    if (record_size == 0) return out;
    const size_t count = chunk.size / record_size;
    out.reserve(count);
    Reader reader(bytes.data(), bytes.size());
    for (size_t i = 0; i < count; ++i) {
        reader.seek(chunk.offset + i * record_size);
        out.push_back(read_one(reader));
    }
    return out;
}

/** The generators of one zone, flattened. Later ones win, as the format says. */
std::map<uint16_t, uint16_t> zone_generators(const std::vector<Generator>& generators,
                                             size_t first, size_t last) {
    std::map<uint16_t, uint16_t> out;
    for (size_t i = first; i < last && i < generators.size(); ++i) {
        out[generators[i].op] = generators[i].amount;
    }
    return out;
}

std::optional<uint16_t> find_generator(const std::map<uint16_t, uint16_t>& generators,
                                       uint16_t op) {
    const auto it = generators.find(op);
    if (it == generators.end()) return std::nullopt;
    return it->second;
}

/**
 * Every zone of `bags` between `bag_first` and `bag_last` that names `wanted`,
 * with the global zone folded into each of them.
 *
 * SF2 puts a global zone first when there is one, and it is recognised by what
 * it lacks: a zone that does not name a sample (or, at the preset level, an
 * instrument) is not a zone that plays nothing, it is the defaults for every
 * zone after it. Skipping it -- which this used to do, on the grounds that it
 * named nothing -- silently drops whatever the font chose to say once instead
 * of on every zone, and fonts say a great deal that way: in ExpressiveSNES.sf2
 * all 136 instruments have one, carrying the release for 135 of them, the decay
 * for 116, the sustain for 114 and the loop mode for 122. That bank was being
 * played with default envelopes throughout.
 *
 * Folded in rather than returned separately so that everything downstream keeps
 * reading one map per zone. A generator stated in the zone wins over the same
 * generator in the global zone -- it is an override at this level, unlike the
 * preset-over-instrument case, which is an offset.
 *
 * All the zones, not the first: an instrument's zones are how a piano covers
 * the keyboard, and taking one stretches a single recording over the lot.
 */
std::vector<std::map<uint16_t, uint16_t>> zones_naming(
    const std::vector<Bag>& bags, const std::vector<Generator>& generators,
    size_t bag_first, size_t bag_last, uint16_t wanted) {
    std::vector<std::map<uint16_t, uint16_t>> out;
    std::map<uint16_t, uint16_t> shared;
    bool first = true;

    for (size_t b = bag_first; b < bag_last && b < bags.size(); ++b) {
        const size_t gen_first = bags[b].gen_index;
        const size_t gen_last =
            (b + 1 < bags.size()) ? bags[b + 1].gen_index : generators.size();
        auto zone = zone_generators(generators, gen_first, gen_last);

        if (first) {
            first = false;
            // Only the first zone can be global, and only if it names nothing.
            // A later zone without a sample is a malformed one, and dropping it
            // is what the format asks for.
            if (!zone.count(wanted)) {
                shared = std::move(zone);
                continue;
            }
        }
        if (!zone.count(wanted)) continue;

        for (const auto& [op, amount] : shared) zone.emplace(op, amount);
        out.push_back(std::move(zone));
    }
    return out;
}

/** A keyRange or velRange generator packs low in the low byte and high in the
    high one. Absent means the whole span. */
std::pair<uint8_t, uint8_t> range_of(const std::map<uint16_t, uint16_t>& generators,
                                     uint16_t op) {
    const auto found = find_generator(generators, op);
    if (!found) return {0, 127};
    const auto low = static_cast<uint8_t>(*found & 0xFF);
    const auto high = static_cast<uint8_t>((*found >> 8) & 0xFF);
    return low <= high ? std::pair{low, high} : std::pair<uint8_t, uint8_t>{0, 127};
}

} // namespace

base::Result<std::shared_ptr<SampleBank>> parse_sf2(const std::string& bytes,
                                                    const std::string& name) {
    Reader reader(bytes.data(), bytes.size());
    if (reader.fourcc() != "RIFF") return std::unexpected(bad("no RIFF header"));
    const auto riff_size = static_cast<size_t>(reader.u32());
    const size_t riff_end = std::min(bytes.size(), reader.position() + riff_size);
    if (reader.fourcc() != "sfbk") return std::unexpected(bad("not an sfbk file"));

    // The three top-level lists. Only two are needed: INFO carries names and
    // the version, neither of which changes what is played.
    std::map<std::string, Chunk> sdta;
    std::map<std::string, Chunk> pdta;
    while (reader.position() + 8 <= riff_end) {
        const auto id = reader.fourcc();
        const auto size = static_cast<size_t>(reader.u32());
        const size_t body = reader.position();
        if (body + size > riff_end) break;
        if (id == "LIST") {
            Reader inner(bytes.data(), bytes.size());
            inner.seek(body);
            const auto kind = inner.fourcc();
            if (kind == "sdta")      sdta = read_chunks(inner, body + size);
            else if (kind == "pdta") pdta = read_chunks(inner, body + size);
        }
        reader.seek(body + size + (size & 1));
    }

    if (!pdta.count("phdr") || !pdta.count("pbag") || !pdta.count("pgen") ||
        !pdta.count("inst") || !pdta.count("ibag") || !pdta.count("igen") ||
        !pdta.count("shdr")) {
        return std::unexpected(bad("the pdta list is missing a required chunk"));
    }
    if (!sdta.count("smpl")) return std::unexpected(bad("there is no sample data"));

    const auto presets = read_records<PresetHeader>(bytes, pdta.at("phdr"), 38, [](Reader& r) {
        PresetHeader p;
        p.name = r.name(20);
        p.program = r.u16();
        p.bank = r.u16();
        p.bag_index = r.u16();
        r.skip(12);                 // library, genre, morphology
        return p;
    });
    const auto preset_bags = read_records<Bag>(bytes, pdta.at("pbag"), 4, [](Reader& r) {
        Bag b;
        b.gen_index = r.u16();
        r.skip(2);                  // modulator index
        return b;
    });
    const auto preset_gens = read_records<Generator>(bytes, pdta.at("pgen"), 4, [](Reader& r) {
        Generator g;
        g.op = r.u16();
        g.amount = r.u16();
        return g;
    });
    const auto instruments = read_records<Instrument>(bytes, pdta.at("inst"), 22, [](Reader& r) {
        Instrument i;
        i.name = r.name(20);
        i.bag_index = r.u16();
        return i;
    });
    const auto inst_bags = read_records<Bag>(bytes, pdta.at("ibag"), 4, [](Reader& r) {
        Bag b;
        b.gen_index = r.u16();
        r.skip(2);
        return b;
    });
    const auto inst_gens = read_records<Generator>(bytes, pdta.at("igen"), 4, [](Reader& r) {
        Generator g;
        g.op = r.u16();
        g.amount = r.u16();
        return g;
    });
    const auto sample_headers = read_records<SampleHeader>(bytes, pdta.at("shdr"), 46,
                                                           [](Reader& r) {
        SampleHeader s;
        s.name = r.name(20);
        s.start = r.u32();
        s.end = r.u32();
        s.loop_start = r.u32();
        s.loop_end = r.u32();
        s.rate = r.u32();
        s.original_key = r.u8();
        s.correction = static_cast<int8_t>(r.u8());
        r.skip(2);                  // sampleLink
        s.type = r.u16();
        return s;
    });

    // Every list ends with a terminal record that is a sentinel rather than an
    // entry -- EOP, EOI, EOS. Reading them as real ones would offer the user an
    // instrument called EOP that plays nothing.
    if (presets.size() < 2 || instruments.size() < 2 || sample_headers.size() < 2) {
        return std::unexpected(bad("it declares no instruments"));
    }

    const auto& smpl = sdta.at("smpl");
    const size_t total_frames = smpl.size / 2;

    auto bank = std::make_shared<SampleBank>();
    bank->name = name;

    // A sample is decoded once and shared by every preset that names it.
    std::map<uint16_t, int> sample_index_for_header;

    for (size_t p = 0; p + 1 < presets.size(); ++p) {
        const auto& preset = presets[p];
        // Bank 0 only: the percussion bank and the variation banks address the
        // same 128 programs, and this model has one slot per program.
        if (preset.bank != 0 || preset.program > 127) continue;
        auto& program = bank->programs[preset.program];
        if (!program.zones.empty()) continue;   // first preset for a slot wins

        const auto preset_zones = zones_naming(preset_bags, preset_gens, preset.bag_index,
                                               presets[p + 1].bag_index, kGenInstrument);
        if (preset_zones.empty()) continue;

        // Every zone of the preset, not just the first.
        //
        // A preset zone names an instrument and the part of the keyboard it
        // answers for; a preset with more than one is layering them. Seven of
        // the 128 programs in a General MIDI bank ripped from SNES games do,
        // and in every case both halves cover the whole keyboard at every
        // velocity -- genuine stacking rather than a split. Honky-tonk is two
        // pianos a few cents apart, and taking one of the two leaves an
        // ordinary piano.
        for (const auto& preset_zone : preset_zones) {
            const auto instrument_id = find_generator(preset_zone, kGenInstrument);
            if (!instrument_id || *instrument_id + 1u >= instruments.size()) continue;

            const auto& instrument = instruments[*instrument_id];
            const auto inst_zones = zones_naming(inst_bags, inst_gens, instrument.bag_index,
                                                 instruments[*instrument_id + 1].bag_index,
                                                 kGenSampleId);

            const auto [preset_low_key, preset_high_key] = range_of(preset_zone, kGenKeyRange);
            const auto [preset_low_velocity, preset_high_velocity] =
                range_of(preset_zone, kGenVelocityRange);

            for (const auto& inst_zone : inst_zones) {
                const auto sample_id = find_generator(inst_zone, kGenSampleId);
                if (!sample_id || *sample_id >= sample_headers.size()) continue;

                // Decoded once and shared: a multi-sampled instrument reaches
                // the same recording from several zones, and a piano would
                // otherwise arrive several times over.
                int index;
                if (const auto known = sample_index_for_header.find(*sample_id);
                    known != sample_index_for_header.end()) {
                    index = known->second;
                } else {
                    const auto& header = sample_headers[*sample_id];
                    if (header.end <= header.start || header.end > total_frames) continue;

                    Sample sample;
                    sample.name = header.name;
                    const size_t frames = header.end - header.start;
                    sample.data.resize(frames);
                    Reader audio(bytes.data(), bytes.size());
                    audio.seek(smpl.offset + static_cast<size_t>(header.start) * 2);
                    for (size_t i = 0; i < frames; ++i) {
                        sample.data[i] = static_cast<float>(audio.i16()) / 32768.0f;
                    }
                    // Outside the band, the header is not to be trusted about
                    // this sample at all -- but the audio usually is, so it is
                    // played at a plausible rate rather than thrown away.
                    sample.source_rate =
                        (header.rate >= kLowestRate && header.rate <= kHighestRate)
                            ? static_cast<int>(header.rate)
                            : 44100;

                    bank->samples.push_back(std::move(sample));
                    index = static_cast<int>(bank->samples.size()) - 1;
                    sample_index_for_header.emplace(*sample_id, index);
                }

                const auto& header = sample_headers[*sample_id];
                Zone zone;
                zone.sample = index;

                // Ranges narrow rather than add: the preset says where its
                // instrument applies, the instrument says which of its samples
                // covers what, and a zone the preset excludes is not a quiet
                // zone but an absent one.
                const auto [key_low, key_high] = range_of(inst_zone, kGenKeyRange);
                const auto [vel_low, vel_high] = range_of(inst_zone, kGenVelocityRange);
                zone.low_key       = std::max(preset_low_key, key_low);
                zone.high_key      = std::min(preset_high_key, key_high);
                zone.low_velocity  = std::max(preset_low_velocity, vel_low);
                zone.high_velocity = std::min(preset_high_velocity, vel_high);
                if (zone.low_key > zone.high_key) continue;
                if (zone.low_velocity > zone.high_velocity) continue;

                zone.root_key = header.original_key <= 127 ? header.original_key : 60;
                zone.fine_tune_cents = header.correction;

                // Looping is off unless the zone asks for it.
                //
                // Nearly every sample in a SoundFont carries loop points in its
                // header whether or not it is meant to loop: a timpani hit has
                // them, spanning almost the whole recording, and honouring them
                // turns one strike into a drum roll that never ends. What
                // decides is the sampleModes generator, whose default when
                // absent is 0 -- play once -- and reading the header instead
                // was what made a snare repeat under a held note.
                //
                // Mode 1 loops for as long as the note is held. Mode 3 loops
                // until the key is released and then plays out the tail, which
                // this engine has no separate stage for; looping it is the
                // nearer of the two answers available. Mode 2 is unused and
                // means the same as 0.
                const auto modes = find_generator(inst_zone, kGenSampleModes);
                const int loop_mode = modes ? (*modes & 3) : 0;
                const bool wants_loop = loop_mode == 1 || loop_mode == 3;

                // Loop points are absolute in the smpl chunk; a voice wants
                // them relative to the sample it is reading.
                if (wants_loop && header.loop_end > header.loop_start &&
                    header.loop_start >= header.start && header.loop_end <= header.end) {
                    zone.loop_start = static_cast<int>(header.loop_start - header.start);
                    zone.loop_end = static_cast<int>(header.loop_end - header.start);
                }

                // A generator stated at both levels is not a choice between the
                // two: the format says the preset's value is an offset applied
                // to the instrument's. They are added before being converted,
                // because timecents and centibels are logarithmic -- adding
                // seconds or amplitudes afterwards would mean something else
                // entirely, and quietly.
                const auto layered = [&](uint16_t op, int16_t absent) -> std::optional<int> {
                    const auto from_instrument = find_generator(inst_zone, op);
                    const auto from_preset = find_generator(preset_zone, op);
                    if (!from_instrument && !from_preset) return std::nullopt;
                    const int base =
                        from_instrument ? static_cast<int16_t>(*from_instrument) : absent;
                    const int offset = from_preset ? static_cast<int16_t>(*from_preset) : 0;
                    return std::clamp(base + offset, -32768, 32767);
                };

                // Generators shape the note, and they belong to the zone rather
                // than to the recording -- which is the whole reason the two are
                // separate. Two zones can share a piano sample and give it
                // different root keys.
                if (const auto root = find_generator(inst_zone, kGenOverridingRootKey);
                    root && *root <= 127) {
                    zone.root_key = static_cast<int>(*root);
                }
                if (const auto coarse = layered(kGenCoarseTune, 0)) {
                    zone.fine_tune_cents += *coarse * 100.0;
                }
                if (const auto fine = layered(kGenFineTune, 0)) {
                    zone.fine_tune_cents += *fine;
                }
                if (const auto attack = layered(kGenAttackVolEnv, kAbsentEnvelopeTimecents)) {
                    zone.attack = timecents_to_seconds(static_cast<int16_t>(*attack));
                }
                if (const auto decay = layered(kGenDecayVolEnv, kAbsentEnvelopeTimecents)) {
                    zone.decay = timecents_to_seconds(static_cast<int16_t>(*decay));
                }
                if (const auto sustain = layered(kGenSustainVolEnv, 0)) {
                    zone.sustain = centibels_to_level(static_cast<int16_t>(*sustain));
                }
                if (const auto release = layered(kGenReleaseVolEnv, kAbsentEnvelopeTimecents)) {
                    zone.release = timecents_to_seconds(static_cast<int16_t>(*release));
                }
                // Attenuation is what pays for layering: two instruments
                // sounding together arrive at twice the level of one, and a
                // bank that stacks on purpose says how much to take back.
                if (const auto attenuation = layered(kGenInitialAttenuation, 0)) {
                    zone.gain = centibels_to_level(static_cast<int16_t>(*attenuation));
                }

                program.zones.push_back(zone);
            }
        }

        if (program.zones.empty()) continue;
        // The preset's name, not the sample's: a SoundFont calls the sample
        // "Piano C4" and the preset "Grand Piano", and the second is what a
        // player is choosing between.
        program.name = preset.name.empty()
                           ? bank->samples[static_cast<size_t>(program.zones.front().sample)].name
                           : preset.name;
    }

    if (bank->empty()) {
        return std::unexpected(bad("none of its presets resolved to a sample"));
    }
    MC_LOG_INFO("Loaded {}: {} samples", name, bank->samples.size());
    return bank;
}

base::Result<std::shared_ptr<SampleBank>> load_sf2(const std::string& utf8_path) {
    const std::filesystem::path path(reinterpret_cast<const char8_t*>(utf8_path.c_str()));

    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound,
                                           "Cannot read " + utf8_path});
    }
    // A SoundFont is read whole because a bank has to be resident to be played
    // from an audio thread. Worth a limit: a gigabyte-scale orchestral font
    // would be loaded into memory here without one.
    constexpr uintmax_t kMaxBytes = 512ull * 1024 * 1024;
    if (size > kMaxBytes) {
        return std::unexpected(base::Error{base::ErrorCode::UnsupportedFormat,
                                           "That SoundFont is larger than 512 MB"});
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(base::Error{base::ErrorCode::IoFailure,
                                           "Cannot open " + utf8_path});
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

    const auto stem = path.stem().u8string();
    return parse_sf2(bytes, std::string(reinterpret_cast<const char*>(stem.c_str()),
                                        stem.size()));
}

} // namespace midi_composer::io
