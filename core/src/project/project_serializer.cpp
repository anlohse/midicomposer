#include "project_serializer.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace midi_composer::project {

namespace {

std::filesystem::path utf8_path(const std::string& path) {
    return std::filesystem::path(reinterpret_cast<const char8_t*>(path.c_str()));
}

} // namespace

nlohmann::json ProjectSerializer::to_json(const music::Composition& comp) {
    nlohmann::json j;
    j["formatVersion"] = kFormatVersion;
    j["title"] = comp.title();
    j["ppqn"] = comp.ppqn();
    j["masterVolume"] = comp.master_volume();

    auto tempo_map = nlohmann::json::array();
    for (const auto& ev : comp.tempo_map().events()) {
        tempo_map.push_back({
            {"id", ev.id.value()},
            {"tick", ev.tick.value()},
            {"microsecondsPerQuarter", ev.microseconds_per_quarter},
        });
    }
    j["tempoMap"] = tempo_map;

    auto ts_map = nlohmann::json::array();
    for (const auto& ev : comp.time_signature_map().events()) {
        ts_map.push_back({
            {"id", ev.id.value()},
            {"tick", ev.tick.value()},
            {"numerator", ev.numerator},
            {"denominator", ev.denominator},
        });
    }
    j["timeSignatureMap"] = ts_map;

    auto key_map = nlohmann::json::array();
    for (const auto& ev : comp.key_signature_map().events()) {
        key_map.push_back({
            {"id", ev.id.value()},
            {"tick", ev.tick.value()},
            {"fifths", ev.fifths},
            {"minor", ev.minor},
        });
    }
    j["keySignatureMap"] = key_map;

    auto tracks = nlohmann::json::array();
    for (const auto& track : comp.tracks()) {
        nlohmann::json t;
        t["id"] = track.id().value();
        t["name"] = track.name();
        t["midiChannel"] = track.midi_channel();
        t["clef"] = music::clef_to_string(track.clef());
        t["outputId"] = std::string(track.output_id());
        t["volume"] = track.volume();
        t["pan"] = track.pan();
        t["muted"] = track.is_muted();
        t["solo"] = track.is_solo();
        t["armed"] = track.is_armed();

        auto notes = nlohmann::json::array();
        for (const auto& n : track.notes()) {
            notes.push_back({
                {"id", n.id.value()},
                {"startTick", n.start.value()},
                {"durationTicks", n.duration.value()},
                {"pitch", n.pitch},
                {"velocity", n.velocity},
            });
        }
        t["notes"] = notes;

        auto ccs = nlohmann::json::array();
        for (const auto& e : track.controller_events()) {
            ccs.push_back({{"id", e.id.value()}, {"tick", e.tick.value()},
                           {"controller", e.controller}, {"value", e.value}});
        }
        t["controllerEvents"] = ccs;

        auto pbs = nlohmann::json::array();
        for (const auto& e : track.pitch_bends()) {
            pbs.push_back({{"id", e.id.value()}, {"tick", e.tick.value()}, {"value", e.value}});
        }
        t["pitchBends"] = pbs;

        auto pcs = nlohmann::json::array();
        for (const auto& e : track.program_changes()) {
            pcs.push_back({{"id", e.id.value()}, {"tick", e.tick.value()}, {"program", e.program}});
        }
        t["programChanges"] = pcs;

        tracks.push_back(t);
    }
    j["tracks"] = tracks;
    return j;
}

base::Result<music::Composition> ProjectSerializer::from_json(const nlohmann::json& j) {
    try {
        const int version = j.at("formatVersion").get<int>();
        if (version > kFormatVersion) {
            return std::unexpected(base::Error{base::ErrorCode::UnsupportedFormat,
                "Project was saved by a newer version (format " + std::to_string(version) + ")"});
        }

        music::Composition comp;
        comp.set_title(j.value("title", std::string{"Untitled"}));
        // Absent in projects written before there was a master fader: those were
        // mixed with the track faders alone, so unity is the only reading that
        // leaves them sounding as they did.
        comp.set_master_volume(j.value("masterVolume", std::uint8_t{127}));

        comp.tempo_map().events().clear();
        for (const auto& e : j.value("tempoMap", nlohmann::json::array())) {
            music::TempoEvent ev;
            ev.id = base::EventId{e.value("id", uint64_t{0})};
            ev.tick = timeline::Tick{e.at("tick").get<int64_t>()};
            ev.microseconds_per_quarter = e.at("microsecondsPerQuarter").get<uint32_t>();
            if (ev.microseconds_per_quarter == 0) {
                return std::unexpected(base::Error{base::ErrorCode::ParseFailure, "Zero tempo value"});
            }
            comp.tempo_map().events().push_back(ev);
        }
        if (comp.tempo_map().events().empty()) {
            comp.tempo_map().events().push_back(music::TempoEvent{});
        }

        comp.time_signature_map().events().clear();
        for (const auto& e : j.value("timeSignatureMap", nlohmann::json::array())) {
            music::TimeSignatureEvent ev;
            ev.id = base::EventId{e.value("id", uint64_t{0})};
            ev.tick = timeline::Tick{e.at("tick").get<int64_t>()};
            ev.numerator = e.at("numerator").get<uint8_t>();
            ev.denominator = e.at("denominator").get<uint8_t>();
            if (ev.numerator == 0 || ev.denominator == 0) {
                return std::unexpected(base::Error{base::ErrorCode::ParseFailure, "Invalid time signature"});
            }
            comp.time_signature_map().events().push_back(ev);
        }
        if (comp.time_signature_map().events().empty()) {
            comp.time_signature_map().events().push_back(music::TimeSignatureEvent{});
        }

        comp.key_signature_map().events().clear();
        for (const auto& e : j.value("keySignatureMap", nlohmann::json::array())) {
            music::KeySignatureEvent ev;
            ev.id = base::EventId{e.value("id", uint64_t{0})};
            ev.tick = timeline::Tick{e.at("tick").get<int64_t>()};
            ev.fifths = std::clamp<int8_t>(e.value("fifths", int8_t{0}), -7, 7);
            ev.minor = e.value("minor", false);
            comp.key_signature_map().events().push_back(ev);
        }
        // Projects written before key signatures existed have no map: fall back
        // to C major from tick 0 rather than leaving the notation without a key.
        if (comp.key_signature_map().events().empty()) {
            comp.key_signature_map().events().push_back(music::KeySignatureEvent{});
        }

        for (const auto& t : j.value("tracks", nlohmann::json::array())) {
            music::Track track(base::TrackId{t.at("id").get<uint64_t>()},
                               t.value("name", std::string{"Track"}));
            track.set_midi_channel(t.value("midiChannel", uint8_t{0}) & 0x0F);
            track.set_clef(music::clef_from_string(t.value("clef", std::string{"treble"})));
            // Absent means "the project's output", which is what every project
            // written before this had.
            track.set_output_id(t.value("outputId", std::string{}));
            track.set_volume(std::min<uint8_t>(127, t.value("volume", uint8_t{100})));
            track.set_pan(std::min<uint8_t>(127, t.value("pan", uint8_t{64})));
            track.set_muted(t.value("muted", false));
            track.set_solo(t.value("solo", false));
            track.set_armed(t.value("armed", false));

            for (const auto& n : t.value("notes", nlohmann::json::array())) {
                music::Note note;
                note.id = base::NoteId{n.at("id").get<uint64_t>()};
                note.start = timeline::Tick{n.at("startTick").get<int64_t>()};
                note.duration = timeline::TickDuration{n.at("durationTicks").get<int64_t>()};
                note.pitch = std::min<uint8_t>(127, n.at("pitch").get<uint8_t>());
                note.velocity = std::clamp<uint8_t>(n.at("velocity").get<uint8_t>(), 1, 127);
                if (note.start.value() < 0 || note.duration.value() <= 0) {
                    return std::unexpected(base::Error{base::ErrorCode::ParseFailure, "Invalid note range"});
                }
                track.notes().push_back(note);
            }
            std::sort(track.notes().begin(), track.notes().end(),
                      [](const auto& a, const auto& b) { return a.start < b.start; });

            for (const auto& e : t.value("controllerEvents", nlohmann::json::array())) {
                track.controller_events().push_back({
                    base::EventId{e.value("id", uint64_t{0})},
                    timeline::Tick{e.at("tick").get<int64_t>()},
                    e.at("controller").get<uint8_t>(),
                    e.at("value").get<uint8_t>(),
                });
            }
            for (const auto& e : t.value("pitchBends", nlohmann::json::array())) {
                track.pitch_bends().push_back({
                    base::EventId{e.value("id", uint64_t{0})},
                    timeline::Tick{e.at("tick").get<int64_t>()},
                    e.at("value").get<int16_t>(),
                });
            }
            for (const auto& e : t.value("programChanges", nlohmann::json::array())) {
                track.program_changes().push_back({
                    base::EventId{e.value("id", uint64_t{0})},
                    timeline::Tick{e.at("tick").get<int64_t>()},
                    e.at("program").get<uint8_t>(),
                });
            }

            comp.tracks().push_back(std::move(track));
        }

        return comp;
    } catch (const std::exception& e) {
        return std::unexpected(base::Error{base::ErrorCode::ParseFailure,
                                           std::string{"Malformed project file: "} + e.what()});
    }
}

base::Result<void> ProjectSerializer::save_file(const music::Composition& comp, const std::string& path) {
    std::ofstream out(utf8_path(path), std::ios::binary | std::ios::trunc);
    if (!out) {
        return std::unexpected(base::Error{base::ErrorCode::IoFailure, "Cannot open file for writing: " + path});
    }
    out << to_json(comp).dump(2);
    if (!out.good()) {
        return std::unexpected(base::Error{base::ErrorCode::IoFailure, "Failed writing file: " + path});
    }
    return {};
}

base::Result<music::Composition> ProjectSerializer::load_file(const std::string& path) {
    std::ifstream in(utf8_path(path), std::ios::binary);
    if (!in) {
        return std::unexpected(base::Error{base::ErrorCode::IoFailure, "Cannot open file: " + path});
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded()) {
        return std::unexpected(base::Error{base::ErrorCode::ParseFailure, "File is not valid JSON: " + path});
    }
    return from_json(j);
}

} // namespace midi_composer::project
