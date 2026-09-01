#include "spc_file.hpp"

#include "base/logger.hpp"
#include "io/brr.hpp"
#include "io/sample_pitch.hpp"
#include "io/spc_adsr.hpp"

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>

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

// The echo, which unlike the interpolation kernel and the ADSR rates is *data
// in this file* rather than hardware we would have to know. A game set these,
// and the piece was written to sound through them.
//
// The register numbers come from the DSP's documented map. Only $5D is
// confirmed by this project's own results -- it finds sample directories at
// sensible addresses across ninety-two files -- so the ones below are believed
// rather than demonstrated, and the values they yield are range-checked instead
// of trusted.
// Per-voice registers, at $x0 through $x9 for voice x. Only three are read:
// which sample the voice is set to play, and its two ADSR bytes.
constexpr size_t kVoiceCount   = 8;
constexpr size_t kVoiceStride  = 0x10;
constexpr size_t kVoiceSrcn    = 0x04;
constexpr size_t kVoiceAdsr1   = 0x05;
constexpr size_t kVoiceAdsr2   = 0x06;

constexpr size_t kDspEchoVolumeLeft  = 0x2C;
constexpr size_t kDspEchoVolumeRight = 0x3C;
constexpr size_t kDspFlags           = 0x6C;   // bit 5 disables the echo
constexpr size_t kDspEchoFeedback    = 0x0D;
constexpr size_t kDspEchoDelay       = 0x7D;   // steps of 16ms, 0 to 15
constexpr size_t kDspFirBase         = 0x0F;   // then $1F, $2F ... $7F

constexpr uint8_t kFlagEchoDisabled = 0x20;

// The chip's echo delay register counts in sixteenths of a second's sixteenth.
constexpr int kEchoDelayStepMs = 16;

// Signed eighth-bit registers are read as a fraction of 128, which is the
// chip's own scaling for volumes, feedback and filter taps alike.
float to_signed_fraction(uint8_t raw) {
    return static_cast<float>(static_cast<int8_t>(raw)) / 128.0f;
}

// A directory entry is four bytes: start address, then loop address, both
// little-endian pointers into ARAM.
constexpr size_t kDirEntryBytes = 4;

// How many entries to walk. The directory has no length -- it is a page of
// memory the driver decided the size of -- so this is where a limit has to be
// chosen rather than derived. 256 entries is the whole page.
constexpr size_t kMaxEntries = 256;

// Where a sample sits when its pitch could not be measured -- percussion and
// noise, mostly, which have no pitch to be wrong about.
constexpr int kAssumedRootKey = 60;
constexpr int kChipSampleRate = 32000;

// Anything the detector is willing to answer is taken, because the two mistakes
// are not the same size. A pitch measured loosely puts an instrument a semitone
// or so out; *no* measurement leaves it at C4, and a sample actually recorded at
// note 83 then plays two octaves low. Percussion, which has no right answer,
// costs nothing either way -- so the detector's own floor is the only gate.
constexpr double kPitchConfidence = 0.0;

// A loop shorter than this cannot hold enough cycles to measure, so the whole
// sample is used instead. The loop is the better window when there is enough of
// it; a 32-frame loop is not a window at all.
constexpr int kUsableLoopFrames = 1024;

// One full block. Below this there is nothing to have decoded.
constexpr size_t kMinimumSamples = 16;

// The block size, needed here as well as in the decoder: a loop address that is
// not a multiple of it away from the start cannot be a block boundary, and so
// cannot be a loop point.
constexpr size_t kBrrBlockBytes = 9;

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

    // ── The envelopes the game wrote ─────────────────────────────────────────
    //
    // A snapshot describes eight voices at one instant, which is why this was
    // once dismissed as unusable. What that missed is SRCN: each voice names
    // the directory entry it is set to play, so the eight voices are eight
    // (sample, envelope) pairs the game itself configured. Around five distinct
    // samples a file get a real envelope this way, and they are overwhelmingly
    // the long ones -- the instruments rather than the percussion.
    //
    // Several voices often name the same sample with *different* envelopes --
    // Wind Scene plays sample 33 on four voices, one of them instant-and-held
    // and three with a 260ms attack fading over 24 seconds. That is a real
    // thing a driver does: one sample, several articulations. So a rip gives
    // *an* envelope the game used with a sample, never *the* envelope, and the
    // most common one among the voices is the least arbitrary choice available.
    // Taking whichever voice came first would let voice order decide.
    std::map<uint8_t, std::vector<std::pair<uint16_t, AdsrRegisters>>> candidates;
    for (size_t voice = 0; voice < kVoiceCount; ++voice) {
        const size_t at = voice * kVoiceStride;
        const uint8_t adsr1 = dsp[at + kVoiceAdsr1];
        const uint8_t adsr2 = dsp[at + kVoiceAdsr2];
        const auto adsr = decode_adsr(adsr1, adsr2);
        // A voice using GAIN instead is running a custom envelope in driver
        // code, which is not something a snapshot can hand over.
        if (!adsr.enabled) continue;
        const auto shape = static_cast<uint16_t>((adsr1 << 8) | adsr2);
        candidates[dsp[at + kVoiceSrcn]].emplace_back(shape, adsr);
    }

    std::map<uint8_t, AdsrRegisters> envelope_for_entry;
    for (const auto& [srcn, voices] : candidates) {
        std::map<uint16_t, int> votes;
        for (const auto& [shape, _] : voices) ++votes[shape];
        uint16_t winner = voices.front().first;
        int best = 0;
        for (const auto& [shape, count] : votes) {
            if (count > best) { best = count; winner = shape; }
        }
        for (const auto& [shape, adsr] : voices) {
            if (shape == winner) { envelope_for_entry.emplace(srcn, adsr); break; }
        }
    }

    int program = 0;
    size_t envelopes_applied = 0;
    std::set<uint16_t> seen_starts;
    for (size_t entry = 0; entry < kMaxEntries && program < 128; ++entry) {
        const size_t at = directory + entry * kDirEntryBytes;
        if (at + kDirEntryBytes > aram.size()) break;

        const uint16_t start = read_u16(aram, at);
        const uint16_t loop  = read_u16(aram, at + 2);

        // An entry the driver never filled in. Not an error and not the end of
        // the directory either -- a game may use entry 0 and entry 12 and
        // nothing between.
        if (start == 0) continue;

        // The same sample reached twice. Real directories alias entries, and
        // offering the identical instrument under two programs is noise.
        if (!seen_starts.insert(start).second) continue;

        auto decoded = decode_brr(aram, start, static_cast<int>(loop));
        if (decoded.samples.size() < kMinimumSamples) continue;
        if (!decoded.ended) {
            // Ran to the end of ARAM without an end flag: the entry pointed at
            // something that is not a sample.
            continue;
        }

        // ── What separates an instrument from a coincidence ──────────────────
        //
        // A directory page is 256 entries whether or not the game filled them,
        // and the rest is whatever memory was there. Around 90 of those stale
        // entries per file happen to point at bytes with an end flag somewhere
        // near, so "it decodes" is not a test at all -- it produced four times
        // too many instruments, most of them a millisecond of noise.
        //
        // The loop address is the test. On real hardware it has to point at a
        // BRR block *inside the sample it belongs to*, which means within the
        // bytes just decoded and a whole number of blocks from the start. Stale
        // entries fail this immediately: their loop points tens of kilobytes
        // away, at no boundary in particular. It takes Chrono Trigger from
        // about a hundred entries a file to about twenty-six, which is what a
        // game of that size actually loads.
        if (loop < start || loop >= start + decoded.bytes_used) continue;
        if ((loop - start) % kBrrBlockBytes != 0) continue;

        Sample sample;
        sample.name = "Sample " + std::to_string(entry);
        sample.data = std::move(decoded.samples);
        sample.source_rate = kChipSampleRate;
        sample.root_key = kAssumedRootKey;
        if (decoded.loop_start >= 0) {
            sample.loop_start = decoded.loop_start;
            sample.loop_end = static_cast<int>(sample.data.size());
        }

        // ── The pitch, measured rather than assumed ──────────────────────────
        //
        // Nothing in an .spc says what note a sample is. Treating them all as
        // C4 leaves every instrument transposed by its own interval, which is
        // the loudest thing wrong with a ripped bank. The audio answers instead:
        // a pitched instrument's loop is periodic by construction, so the
        // period is measurable and the period is the pitch.
        //
        // Measured over the loop when there is one -- the steady state a looped
        // sample exists to reach -- and over the whole thing when there is not.
        const bool loop_is_usable = sample.loop_start >= 0 &&
                                    sample.loop_end - sample.loop_start >= kUsableLoopFrames;
        const auto pitch = estimate_pitch(sample.data, sample.source_rate,
                                          loop_is_usable ? sample.loop_start : 0,
                                          loop_is_usable ? sample.loop_end : -1);
        bool pitched = false;
        if (pitch.frequency > 0.0 && pitch.confidence >= kPitchConfidence) {
            pitched = true;
            // Rounded to a key, with the remainder kept as cents. Dropping the
            // remainder is the difference between an instrument that is in tune
            // and one that is a third of a semitone sharp -- and a bank where
            // every instrument is differently sharp is a bank that cannot be
            // played together.
            sample.root_key = static_cast<int>(std::lround(pitch.midi_note));
            sample.fine_tune_cents = (sample.root_key - pitch.midi_note) * 100.0;
        }
        // Defaults for a sample no voice was pointed at: audible immediately
        // and held, which is the least wrong thing to do with an unknown.
        sample.attack = 0.001f;
        sample.decay = 0.0f;
        sample.sustain = 1.0f;
        sample.sustain_rate = 0.0f;
        sample.release = 0.05f;

        if (const auto found = envelope_for_entry.find(static_cast<uint8_t>(entry));
            found != envelope_for_entry.end()) {
            sample.attack = found->second.attack;
            sample.decay = found->second.decay;
            sample.sustain = found->second.sustain;
            sample.sustain_rate = found->second.sustain_rate;
            // Release is deliberately not taken: the chip has no release rate
            // in ADSR -- key-off runs a fixed linear fade -- and §11a.8 chose a
            // short curve over that, so a note does not end on a click.
            ++envelopes_applied;
        }

        // ── Naming what has no name ──────────────────────────────────────────
        //
        // A rip carries no instrument names: the driver knew and the driver is
        // not in the file. "Sample 32" is honest and useless. What helps
        // someone auditioning two dozen unknowns is what was already measured
        // about each -- length tells a percussion hit from a held instrument at
        // a glance, the note says where it sits, and looped says whether it can
        // be held at all.
        const double seconds = static_cast<double>(sample.data.size()) /
                               static_cast<double>(sample.source_rate);
        std::string label = "Sample " + std::to_string(entry) + " (";
        if (pitched) {
            static constexpr const char* kNoteNames[12] =
                {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            label += kNoteNames[sample.root_key % 12];
            label += std::to_string(sample.root_key / 12 - 1);
            label += ", ";
        }
        if (seconds >= 1.0) {
            label += std::to_string(static_cast<int>(seconds)) + "." +
                     std::to_string(static_cast<int>(seconds * 10) % 10) + "s";
        } else {
            label += std::to_string(static_cast<int>(seconds * 1000 + 0.5)) + "ms";
        }
        if (sample.loop_start >= 0) label += ", looped";
        label += ")";
        sample.name = label;

        bank->samples.push_back(std::move(sample));
        bank->program_to_sample[static_cast<size_t>(program)] =
            static_cast<int>(bank->samples.size()) - 1;
        bank->program_names[static_cast<size_t>(program)] = label;
        ++program;
    }

    // ── The echo the game was playing through ────────────────────────────────
    //
    // Range-checked rather than trusted: these registers are read from a
    // snapshot of running hardware, and a value outside what the chip can mean
    // says the register map is wrong here rather than that the game did
    // something clever.
    const int delay_steps = dsp[kDspEchoDelay] & 0x0F;
    auto& echo = bank->echo;
    echo.enabled = (dsp[kDspFlags] & kFlagEchoDisabled) == 0 && delay_steps > 0;
    echo.delay_ms = delay_steps * kEchoDelayStepMs;
    echo.feedback = to_signed_fraction(dsp[kDspEchoFeedback]);
    echo.volume_left = to_signed_fraction(dsp[kDspEchoVolumeLeft]);
    echo.volume_right = to_signed_fraction(dsp[kDspEchoVolumeRight]);
    for (size_t tap = 0; tap < echo.fir.size(); ++tap) {
        echo.fir[tap] = to_signed_fraction(dsp[kDspFirBase + tap * 0x10]);
    }

    // A filter with gain far above one inside a feedback path runs away, and
    // the chip's own wrapping is not something to reproduce into a wav file.
    float fir_gain = 0.0f;
    for (float tap : echo.fir) fir_gain += std::abs(tap);
    if (fir_gain * std::abs(echo.feedback) >= 1.0f) {
        MC_LOG_WARN("{}: the echo would not decay (filter gain {:.2f}, feedback {:.2f});"
                    " leaving it off", name, fir_gain, echo.feedback);
        echo.enabled = false;
    }

    if (bank->empty()) {
        return std::unexpected(bad("no samples were found at the directory the DSP names"));
    }
    if (echo.enabled) {
        MC_LOG_INFO("{}: echo {}ms, feedback {:.2f}, volume {:.2f}/{:.2f}", name,
                    echo.delay_ms, echo.feedback, echo.volume_left, echo.volume_right);
    }
    MC_LOG_INFO("Ripped {}: {} samples from the directory at ${:04X}, {} with the "
                "game's own envelope", name, bank->samples.size(), directory,
                envelopes_applied);
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
