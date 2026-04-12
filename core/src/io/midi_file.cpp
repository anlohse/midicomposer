#include "midi_file.hpp"
#include "base/logger.hpp"
#include <libremidi/reader.hpp>
#include <libremidi/writer.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

namespace midi_composer::io {

namespace {

std::filesystem::path utf8_path(const std::string& path) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str()));
}

// MIDI files carry no clef, so pick one from where the track actually sits.
// Without this, bass parts import into treble clef and render as a wall of
// ledger lines. Thresholds are the middle of each clef's comfortable range.
music::Clef derive_clef(const music::Track& track) {
    if (track.notes().empty()) return music::Clef::Treble;
    std::int64_t sum = 0;
    for (const auto& note : track.notes()) sum += note.pitch;
    const auto mean = static_cast<int>(sum / static_cast<std::int64_t>(track.notes().size()));

    if (mean < 36) return music::Clef::Bass8vb;   // below C2
    if (mean < 55) return music::Clef::Bass;      // below G3
    if (mean > 84) return music::Clef::Treble8va; // above C6
    return music::Clef::Treble;
}

// Same-tick ordering policy (core spec §14.3): meta first, then program
// changes, controllers, note-offs, note-ons, pitch bends.
enum EventOrder : int {
    kMeta = 0,
    kProgramChange = 1,
    kController = 2,
    kNoteOff = 3,
    kNoteOn = 4,
    kPitchBend = 5,
};

struct AbsEvent {
    int64_t tick;
    int order;
    libremidi::message msg;
};

// libremidi's writer emits event.tick verbatim as the delta field, so events
// must be sorted by absolute tick here and converted to deltas.
void write_track(libremidi::writer& w, int track_index, std::vector<AbsEvent>& events) {
    std::stable_sort(events.begin(), events.end(), [](const AbsEvent& a, const AbsEvent& b) {
        if (a.tick != b.tick) return a.tick < b.tick;
        return a.order < b.order;
    });
    int64_t prev = 0;
    for (auto& ev : events) {
        const auto delta = static_cast<int>(ev.tick - prev);
        prev = ev.tick;
        w.add_event(delta, track_index, ev.msg);
    }
    if (events.empty()) {
        // Force the track chunk to exist (writer creates tracks lazily).
        w.add_event(0, track_index, libremidi::meta_events::end_of_track());
    }
}

libremidi::message track_name_meta(std::string_view name) {
    libremidi::message m;
    const auto len = std::min<std::size_t>(name.size(), 127);
    m.bytes.reserve(3 + len);
    m.bytes.push_back(0xFF);
    m.bytes.push_back(0x03);
    m.bytes.push_back(static_cast<unsigned char>(len));
    for (std::size_t i = 0; i < len; ++i) {
        m.bytes.push_back(static_cast<unsigned char>(name[i]));
    }
    return m;
}

} // namespace

base::Result<void> MidiFile::export_file(const music::Composition& comp, const std::string& path) {
    libremidi::writer w;
    w.ticksPerQuarterNote = static_cast<int>(comp.ppqn());

    // Track 0: conductor (tempo + time signature).
    std::vector<AbsEvent> conductor;
    for (const auto& ev : comp.tempo_map().events()) {
        conductor.push_back({ev.tick.value(), kMeta,
            libremidi::meta_events::tempo(static_cast<int>(ev.microseconds_per_quarter))});
    }
    for (const auto& ev : comp.time_signature_map().events()) {
        conductor.push_back({ev.tick.value(), kMeta,
            libremidi::meta_events::time_signature(ev.numerator, ev.denominator)});
    }
    for (const auto& ev : comp.key_signature_map().events()) {
        // FF 59 02 sf mi — sf is a signed byte (-7..7), mi is 0 major / 1 minor.
        conductor.push_back({ev.tick.value(), kMeta,
            {0xFF, 0x59, 0x02,
             static_cast<unsigned char>(static_cast<std::uint8_t>(ev.fifths)),
             static_cast<unsigned char>(ev.minor ? 1 : 0)}});
    }
    write_track(w, 0, conductor);

    int track_index = 1;
    for (const auto& track : comp.tracks()) {
        const uint8_t ch = track.midi_channel() & 0x0F;
        std::vector<AbsEvent> events;
        events.push_back({0, kMeta, track_name_meta(track.name())});

        for (const auto& e : track.program_changes()) {
            events.push_back({e.tick.value(), kProgramChange,
                {static_cast<unsigned char>(0xC0 | ch), static_cast<unsigned char>(e.program & 0x7F)}});
        }
        for (const auto& e : track.controller_events()) {
            events.push_back({e.tick.value(), kController,
                {static_cast<unsigned char>(0xB0 | ch),
                 static_cast<unsigned char>(e.controller & 0x7F),
                 static_cast<unsigned char>(e.value & 0x7F)}});
        }
        for (const auto& e : track.pitch_bends()) {
            const int raw = std::clamp(e.value + 8192, 0, 16383);
            events.push_back({e.tick.value(), kPitchBend,
                {static_cast<unsigned char>(0xE0 | ch),
                 static_cast<unsigned char>(raw & 0x7F),
                 static_cast<unsigned char>((raw >> 7) & 0x7F)}});
        }
        for (const auto& n : track.notes()) {
            events.push_back({n.start.value(), kNoteOn,
                {static_cast<unsigned char>(0x90 | ch),
                 static_cast<unsigned char>(n.pitch & 0x7F),
                 static_cast<unsigned char>(std::max<uint8_t>(1, n.velocity) & 0x7F)}});
            events.push_back({n.end().value(), kNoteOff,
                {static_cast<unsigned char>(0x80 | ch),
                 static_cast<unsigned char>(n.pitch & 0x7F),
                 static_cast<unsigned char>(0)}});
        }
        write_track(w, track_index++, events);
    }

    std::ofstream out(utf8_path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(base::Error{base::ErrorCode::IoFailure, "Cannot open file for writing: " + path});
    }
    w.write(out);
    if (!out.good()) {
        return std::unexpected(base::Error{base::ErrorCode::IoFailure, "Failed writing file: " + path});
    }
    return {};
}

base::Result<music::Composition> MidiFile::import_file(const std::string& path) {
    std::ifstream in(utf8_path(path), std::ios::binary);
    if (!in) {
        return std::unexpected(base::Error{base::ErrorCode::IoFailure, "Cannot open file: " + path});
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    libremidi::reader reader(/*useAbsolute=*/true);
    const auto result = reader.parse(bytes);
    if (result == libremidi::reader::invalid || result == libremidi::reader::incomplete) {
        return std::unexpected(base::Error{base::ErrorCode::ParseFailure, "Not a valid MIDI file: " + path});
    }

    music::Composition comp;
    if (reader.ticksPerBeat > 0) {
        comp.set_ppqn(static_cast<uint16_t>(reader.ticksPerBeat));
    }
    comp.tempo_map().events().clear();
    comp.time_signature_map().events().clear();
    comp.key_signature_map().events().clear();

    uint64_t next_track_id = 1;
    uint64_t next_note_id = 1;
    uint64_t next_event_id = 1;

    for (const auto& midi_track : reader.tracks) {
        music::Track track(base::TrackId{next_track_id}, "Track " + std::to_string(next_track_id));
        bool has_channel = false;
        int64_t last_tick = 0;

        // Overlapping same-pitch notes: FIFO per (channel, pitch).
        std::map<uint16_t, std::vector<std::pair<int64_t, uint8_t>>> pending; // key -> [(start, velocity)]

        for (const auto& event : midi_track) {
            const auto& m = event.m.bytes;
            if (m.empty()) continue;
            const int64_t tick = event.tick;
            last_tick = std::max(last_tick, tick);

            if (m[0] == 0xFF) {
                if (m.size() >= 6 && m[1] == 0x51) {                    // tempo
                    const uint32_t uspq = (uint32_t(m[3]) << 16) | (uint32_t(m[4]) << 8) | m[5];
                    if (uspq > 0) {
                        comp.tempo_map().events().push_back(
                            {base::EventId{next_event_id++}, timeline::Tick{tick}, uspq});
                    }
                } else if (m.size() >= 5 && m[1] == 0x59) {             // key signature
                    comp.key_signature_map().events().push_back(
                        music::KeySignatureEvent{base::EventId{next_event_id++}, timeline::Tick{tick},
                                                 std::clamp<int8_t>(static_cast<int8_t>(m[3]), -7, 7),
                                                 m[4] != 0});
                } else if (m.size() >= 5 && m[1] == 0x58) {             // time signature
                    const uint8_t num = m[3];
                    const uint8_t den = static_cast<uint8_t>(1u << m[4]);
                    if (num > 0 && den > 0) {
                        comp.time_signature_map().events().push_back(
                            {base::EventId{next_event_id++}, timeline::Tick{tick}, num, den});
                    }
                } else if (m.size() >= 3 && m[1] == 0x03 && m.size() >= 3u + m[2]) { // track name
                    track.set_name(std::string(reinterpret_cast<const char*>(m.data() + 3), m[2]));
                }
                continue;
            }

            const uint8_t status = m[0] & 0xF0;
            const uint8_t channel = m[0] & 0x0F;
            if (status < 0x80 || status > 0xE0) continue;
            if (!has_channel) { track.set_midi_channel(channel); has_channel = true; }

            if (status == 0x90 && m.size() >= 3 && m[2] > 0) {          // note on
                const uint16_t key = (uint16_t(channel) << 8) | (m[1] & 0x7F);
                pending[key].push_back({tick, m[2] & 0x7F});
            } else if ((status == 0x80 || (status == 0x90 && m.size() >= 3 && m[2] == 0)) && m.size() >= 3) { // note off
                const uint16_t key = (uint16_t(channel) << 8) | (m[1] & 0x7F);
                auto it = pending.find(key);
                if (it != pending.end() && !it->second.empty()) {
                    auto [start, velocity] = it->second.front();
                    it->second.erase(it->second.begin());
                    music::Note note;
                    note.id = base::NoteId{next_note_id++};
                    note.start = timeline::Tick{start};
                    note.duration = timeline::TickDuration{std::max<int64_t>(1, tick - start)};
                    note.pitch = m[1] & 0x7F;
                    note.velocity = std::max<uint8_t>(1, velocity);
                    track.notes().push_back(note);
                }
            } else if (status == 0xB0 && m.size() >= 3) {               // controller
                track.controller_events().push_back(
                    {base::EventId{next_event_id++}, timeline::Tick{tick},
                     static_cast<uint8_t>(m[1] & 0x7F), static_cast<uint8_t>(m[2] & 0x7F)});
            } else if (status == 0xC0 && m.size() >= 2) {               // program change
                track.program_changes().push_back(
                    {base::EventId{next_event_id++}, timeline::Tick{tick},
                     static_cast<uint8_t>(m[1] & 0x7F)});
            } else if (status == 0xE0 && m.size() >= 3) {               // pitch bend
                const int raw = ((m[2] & 0x7F) << 7) | (m[1] & 0x7F);
                track.pitch_bends().push_back(
                    {base::EventId{next_event_id++}, timeline::Tick{tick},
                     static_cast<int16_t>(raw - 8192)});
            }
        }

        // Close hanging note-ons at end of track (malformed files).
        for (auto& [key, starts] : pending) {
            for (auto [start, velocity] : starts) {
                music::Note note;
                note.id = base::NoteId{next_note_id++};
                note.start = timeline::Tick{start};
                note.duration = timeline::TickDuration{std::max<int64_t>(1, last_tick - start)};
                note.pitch = static_cast<uint8_t>(key & 0x7F);
                note.velocity = std::max<uint8_t>(1, velocity);
                track.notes().push_back(note);
                MC_LOG_WARN("MIDI import: closed hanging note at end of track (pitch {})", note.pitch);
            }
        }

        std::sort(track.notes().begin(), track.notes().end(),
                  [](const auto& a, const auto& b) { return a.start < b.start; });

        // Skip conductor/empty tracks (no notes and no channel events).
        if (track.notes().empty() && track.controller_events().empty() &&
            track.pitch_bends().empty() && track.program_changes().empty()) {
            continue;
        }
        track.set_armed(false);
        track.set_clef(derive_clef(track));
        comp.tracks().push_back(std::move(track));
        ++next_track_id;
    }

    // Guarantee effective events at tick 0.
    auto& tempo = comp.tempo_map().events();
    std::sort(tempo.begin(), tempo.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    if (tempo.empty() || tempo.front().tick.value() > 0) {
        tempo.insert(tempo.begin(), music::TempoEvent{base::EventId{next_event_id++}, timeline::Tick{0}, 500000});
    }
    auto& ts = comp.time_signature_map().events();
    std::sort(ts.begin(), ts.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    if (ts.empty() || ts.front().tick.value() > 0) {
        ts.insert(ts.begin(), music::TimeSignatureEvent{base::EventId{next_event_id++}, timeline::Tick{0}, 4, 4});
    }

    auto& keys = comp.key_signature_map().events();
    std::sort(keys.begin(), keys.end(), [](const auto& a, const auto& b) { return a.tick < b.tick; });
    if (keys.empty() || keys.front().tick.value() > 0) {
        keys.insert(keys.begin(), music::KeySignatureEvent{base::EventId{next_event_id++}, timeline::Tick{0}, 0, false});
    }

    if (comp.tracks().empty()) {
        return std::unexpected(base::Error{base::ErrorCode::ParseFailure, "MIDI file contains no note data"});
    }
    return comp;
}

} // namespace midi_composer::io
