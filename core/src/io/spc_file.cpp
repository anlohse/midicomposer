#include "spc_file.hpp"

#include "base/logger.hpp"
#include "io/brr.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace midi_composer::io {

namespace {

using playback::Sample;
using playback::SampleBank;

// The layout of the format, all of it fixed.
constexpr size_t kHeaderSize   = 0x100;      // tags and registers
constexpr size_t kAramOffset   = 0x100;
constexpr size_t kAramSize     = 0x10000;    // the whole 64KB
constexpr size_t kDspOffset    = 0x10100;
constexpr size_t kDspSize      = 128;
constexpr size_t kMinimumSize  = kDspOffset + kDspSize;

// DSP register $5D: the high byte of the sample directory's address. The
// directory is what makes ripping deterministic rather than a search.
constexpr size_t kDspDir = 0x5D;

// A directory entry is four bytes: start address, then loop address, both
// little-endian pointers into ARAM.
constexpr size_t kDirEntryBytes = 4;

// How many entries to walk. The directory has no length -- it is a page of
// memory the driver decided the size of -- so this is where a limit has to be
// chosen rather than derived. 256 entries is the whole page.
constexpr size_t kMaxEntries = 256;

// A rip has no root key. The chip plays a sample at its recorded rate when the
// pitch register reads 0x1000, and 32000Hz is that rate; everything is treated
// as recorded at this note so intervals between notes stay right even though
// the absolute pitch is a convention.
constexpr int kAssumedRootKey = 60;
constexpr int kChipSampleRate = 32000;

// Below this a "sample" is a directory entry pointing at something that is not
// a sample -- uninitialised memory, or the driver's own data.
constexpr size_t kMinimumSamples = 32;

base::Error bad(const std::string& what) {
    return base::Error{base::ErrorCode::ParseFailure, "Not a usable SPC file: " + what};
}

uint16_t read_u16(const std::vector<uint8_t>& aram, size_t at) {
    if (at + 1 >= aram.size()) return 0;
    return static_cast<uint16_t>(aram[at] | (static_cast<uint16_t>(aram[at + 1]) << 8));
}

} // namespace

base::Result<std::shared_ptr<SampleBank>> parse_spc(const std::string& bytes,
                                                    const std::string& name) {
    if (bytes.size() < kMinimumSize) {
        return std::unexpected(bad("it is too short to hold a 64KB snapshot"));
    }
    // The header identifies itself. Checked loosely: the string has been
    // written with more than one spelling of the version over the years, and
    // refusing a file over its punctuation would be refusing real rips.
    if (bytes.compare(0, 8, "SNES-SPC") != 0) {
        return std::unexpected(bad("it does not begin with SNES-SPC"));
    }

    const std::vector<uint8_t> aram(
        reinterpret_cast<const uint8_t*>(bytes.data()) + kAramOffset,
        reinterpret_cast<const uint8_t*>(bytes.data()) + kAramOffset + kAramSize);
    const auto* dsp = reinterpret_cast<const uint8_t*>(bytes.data()) + kDspOffset;

    const size_t directory = static_cast<size_t>(dsp[kDspDir]) << 8;

    auto bank = std::make_shared<SampleBank>();
    bank->name = name;

    int program = 0;
    for (size_t entry = 0; entry < kMaxEntries && program < 128; ++entry) {
        const size_t at = directory + entry * kDirEntryBytes;
        if (at + kDirEntryBytes > aram.size()) break;

        const uint16_t start = read_u16(aram, at);
        const uint16_t loop  = read_u16(aram, at + 2);

        // An entry the driver never filled in. Not an error and not the end of
        // the directory either -- a game may use entry 0 and entry 12 and
        // nothing between.
        if (start == 0) continue;

        auto decoded = decode_brr(aram, start, loop >= start ? static_cast<int>(loop) : -1);
        if (decoded.samples.size() < kMinimumSamples) continue;
        if (!decoded.ended) {
            // Ran to the end of ARAM without an end flag: the entry pointed at
            // something that is not a sample.
            continue;
        }

        Sample sample;
        sample.name = "Sample " + std::to_string(entry);
        sample.data = std::move(decoded.samples);
        sample.source_rate = kChipSampleRate;
        sample.root_key = kAssumedRootKey;
        if (decoded.loop_start >= 0) {
            sample.loop_start = decoded.loop_start;
            sample.loop_end = static_cast<int>(sample.data.size());
        }
        // The envelope is the one thing a snapshot cannot give: the DSP's ADSR
        // registers describe the eight voices at one instant, not the
        // instruments. A short attack and a short release let every sample be
        // auditioned; shaping them is the user's, once there is anywhere to.
        sample.attack = 0.001f;
        sample.decay = 0.0f;
        sample.sustain = 1.0f;
        sample.release = 0.05f;

        bank->samples.push_back(std::move(sample));
        bank->program_to_sample[static_cast<size_t>(program)] =
            static_cast<int>(bank->samples.size()) - 1;
        ++program;
    }

    if (bank->empty()) {
        return std::unexpected(bad("no samples were found at the directory the DSP names"));
    }
    MC_LOG_INFO("Ripped {}: {} samples from the directory at ${:04X}", name,
                bank->samples.size(), directory);
    return bank;
}

base::Result<std::shared_ptr<SampleBank>> load_spc(const std::string& utf8_path) {
    const std::filesystem::path path(reinterpret_cast<const char8_t*>(utf8_path.c_str()));

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected(base::Error{base::ErrorCode::NotFound,
                                           "Cannot read " + utf8_path});
    }
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());

    const auto stem = path.stem().u8string();
    return parse_spc(bytes, std::string(reinterpret_cast<const char*>(stem.c_str()),
                                        stem.size()));
}

} // namespace midi_composer::io
