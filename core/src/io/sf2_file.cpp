#include "sf2_file.hpp"

#include "base/logger.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace midi_composer::io {

namespace {

using playback::Sample;
using playback::SampleBank;

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
 * The zone of `bags` belonging to record `index`, and the generator span of the
 * first zone that names `wanted`.
 *
 * SF2 stores a global zone first when one is present, and it is the zone that
 * does *not* name an instrument or a sample. Looking for the first zone that
 * does is what skips it without needing to know whether it was there.
 */
std::optional<std::map<uint16_t, uint16_t>> first_zone_naming(
    const std::vector<Bag>& bags, const std::vector<Generator>& generators,
    size_t bag_first, size_t bag_last, uint16_t wanted) {
    for (size_t b = bag_first; b < bag_last && b + 1 < bags.size() + 1; ++b) {
        if (b >= bags.size()) break;
        const size_t gen_first = bags[b].gen_index;
        const size_t gen_last =
            (b + 1 < bags.size()) ? bags[b + 1].gen_index : generators.size();
        auto zone = zone_generators(generators, gen_first, gen_last);
        if (zone.count(wanted)) return zone;
    }
    return std::nullopt;
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
        if (bank->program_to_sample[preset.program] >= 0) continue;   // first wins

        const size_t bag_first = preset.bag_index;
        const size_t bag_last = presets[p + 1].bag_index;
        auto preset_zone = first_zone_naming(preset_bags, preset_gens, bag_first, bag_last,
                                             kGenInstrument);
        if (!preset_zone) continue;
        const auto instrument_id = find_generator(*preset_zone, kGenInstrument);
        if (!instrument_id || *instrument_id + 1u >= instruments.size()) continue;

        const auto& instrument = instruments[*instrument_id];
        auto inst_zone = first_zone_naming(inst_bags, inst_gens, instrument.bag_index,
                                           instruments[*instrument_id + 1].bag_index,
                                           kGenSampleId);
        if (!inst_zone) continue;
        const auto sample_id = find_generator(*inst_zone, kGenSampleId);
        if (!sample_id || *sample_id + 1u > sample_headers.size()) continue;

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
            sample.source_rate = header.rate > 0 ? static_cast<int>(header.rate) : 44100;
            sample.root_key = header.original_key <= 127 ? header.original_key : 60;
            sample.fine_tune_cents = header.correction;

            // Loop points are absolute in the smpl chunk; a voice wants them
            // relative to the sample it is reading.
            if (header.loop_end > header.loop_start && header.loop_start >= header.start &&
                header.loop_end <= header.end) {
                sample.loop_start = static_cast<int>(header.loop_start - header.start);
                sample.loop_end = static_cast<int>(header.loop_end - header.start);
            }

            bank->samples.push_back(std::move(sample));
            index = static_cast<int>(bank->samples.size()) - 1;
            sample_index_for_header.emplace(*sample_id, index);
        }

        auto& sample = bank->samples[static_cast<size_t>(index)];

        // Zone generators that shape the note. Applied per preset rather than
        // per sample would be more faithful; with one sample per program the
        // distinction has nowhere to show.
        if (const auto modes = find_generator(*inst_zone, kGenSampleModes)) {
            // 0 means play once, whatever the header's loop points say.
            if ((*modes & 3) == 0) sample.loop_start = -1;
        }
        if (const auto root = find_generator(*inst_zone, kGenOverridingRootKey);
            root && *root <= 127) {
            sample.root_key = static_cast<int>(*root);
        }
        if (const auto coarse = find_generator(*inst_zone, kGenCoarseTune)) {
            sample.fine_tune_cents += static_cast<int16_t>(*coarse) * 100.0;
        }
        if (const auto fine = find_generator(*inst_zone, kGenFineTune)) {
            sample.fine_tune_cents += static_cast<int16_t>(*fine);
        }
        if (const auto attack = find_generator(*inst_zone, kGenAttackVolEnv)) {
            sample.attack = timecents_to_seconds(static_cast<int16_t>(*attack));
        }
        if (const auto decay = find_generator(*inst_zone, kGenDecayVolEnv)) {
            sample.decay = timecents_to_seconds(static_cast<int16_t>(*decay));
        }
        if (const auto sustain = find_generator(*inst_zone, kGenSustainVolEnv)) {
            sample.sustain = centibels_to_level(static_cast<int16_t>(*sustain));
        }
        if (const auto release = find_generator(*inst_zone, kGenReleaseVolEnv)) {
            sample.release = timecents_to_seconds(static_cast<int16_t>(*release));
        }

        bank->program_to_sample[preset.program] = index;
        // The preset's name, not the sample's: a SoundFont calls the sample
        // "Piano C4" and the preset "Grand Piano", and the second is the one a
        // player is choosing between.
        bank->program_names[preset.program] =
            preset.name.empty() ? sample.name : preset.name;
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
