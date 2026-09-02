#include "bridge_dispatcher.hpp"
#include "base/logger.hpp"
#include "shell/file_dialogs.hpp"
#include "shell/unsaved_changes.hpp"
#include "playback/output_plugin.hpp"
#include "app/preferences.hpp"

#include <variant>

namespace {

const char* parameter_type_name(midi_composer::playback::ParameterType type) {
    using T = midi_composer::playback::ParameterType;
    switch (type) {
        case T::Enum:   return "enum";
        case T::Int:    return "int";
        case T::Bool:   return "bool";
        case T::String: return "string";
        case T::File:   return "file";
    }
    return "string";
}

nlohmann::json parameter_value_to_json(const midi_composer::playback::ParameterValue& value) {
    // monostate is "not set", which is null rather than an empty string: a
    // parameter with no value and one set to "" are different things.
    if (const auto* s = std::get_if<std::string>(&value)) return *s;
    if (const auto* i = std::get_if<int>(&value)) return *i;
    if (const auto* b = std::get_if<bool>(&value)) return *b;
    return nullptr;
}

midi_composer::playback::ParameterValue parameter_value_from_json(const nlohmann::json& j) {
    if (j.is_string()) return j.get<std::string>();
    if (j.is_boolean()) return j.get<bool>();
    if (j.is_number()) return j.get<int>();
    return {};
}

} // namespace

namespace midi_composer::ui_bridge {

namespace {

constexpr const wchar_t* kProjectFilter =
    L"MIDI Composer Project (*.mcproj)\0*.mcproj\0All Files (*.*)\0*.*\0";
constexpr const wchar_t* kMidiFilter =
    L"MIDI Files (*.mid;*.midi)\0*.mid;*.midi\0All Files (*.*)\0*.*\0";
constexpr const wchar_t* kAudioFilter =
    L"WAV Audio (*.wav)\0*.wav\0All Files (*.*)\0*.*\0";

} // namespace

BridgeDispatcher::BridgeDispatcher(app::CoreFacade& core) : m_core(core) {}

void BridgeDispatcher::register_with(saucer::smartview<saucer::serializers::glaze>& view) {
    m_view = &view;

    view.expose("ping_native", []() -> std::string {
        return "pong-native";
    });

    view.expose("send_to_core", [this](const std::string& json_str) -> std::string {
        MC_LOG_DEBUG("UI message: {}", json_str);
        try {
            auto msg = nlohmann::json::parse(json_str);
            auto type = msg.at("type").get<std::string>();
            auto payload = msg.value("payload", nlohmann::json::object());
            
            auto response = handle_command(type, payload);
            return response.dump();
        } catch (const std::exception& e) {
            MC_LOG_ERROR("Bridge parsing error: {}", e.what());
            return nlohmann::json{{"error", e.what()}}.dump();
        }
    });
}

namespace {

// nlohmann's get<uint8_t>() silently truncates out-of-range values; read as
// int and validate at the bridge boundary instead.
// Pitch bend is the one signed data field on the wire, and out of range it would
// wrap rather than clip.
int16_t checked_bend(const nlohmann::json& payload, const char* field) {
    const int value = payload.at(field).get<int>();
    if (value < -8192 || value > 8191) {
        throw std::invalid_argument(std::string(field) + " must be between -8192 and 8191");
    }
    return static_cast<int16_t>(value);
}

/**
 * The document a command is about, under either of its two names.
 *
 * Two names for one field grew up in this file: the document-lifecycle commands
 * took `id` and the editing commands took `documentId` -- and `export_midi` and
 * `export_audio`, which are siblings, ended up on opposite sides of that
 * accident. Reading both costs nothing and stops the next person losing an hour
 * to a key name, which is how this was found.
 */
base::CompositionId document_id(const nlohmann::json& payload) {
    if (payload.contains("documentId")) {
        return base::CompositionId{payload.at("documentId").get<uint64_t>()};
    }
    return base::CompositionId{payload.at("id").get<uint64_t>()};
}

uint8_t checked_u8(const nlohmann::json& payload, const char* field, int min, int max) {
    const int value = payload.at(field).get<int>();
    if (value < min || value > max) {
        throw std::invalid_argument(std::string(field) + " must be between " +
                                    std::to_string(min) + " and " + std::to_string(max));
    }
    return static_cast<uint8_t>(value);
}

} // namespace

nlohmann::json BridgeDispatcher::handle_command(const std::string& type, const nlohmann::json& payload) {
    nlohmann::json response;
    response["success"] = true;

    try {
        if (type == "ping") {
            m_core.ping();
            response["result"] = "pong";
        } else if (type == "get_version") {
            response["result"] = m_core.get_version();
        } else if (type == "exit_application") {
            // Closes the window rather than ending the process, so leaving by
            // the menu goes through the same door as clicking the X: the
            // unsaved-work question, then the window size, then shutdown.
            //
            // It used to call std::exit straight out, which meant File > Exit
            // discarded unsaved work in silence and forgot the window -- the
            // protection added for the X was simply not on this path. Two ways
            // out is one too many.
            if (m_view) m_view->close();
            else m_core.exit_application();   // no window to close: end it
        } else if (type == "new_project") {
            auto id = m_core.new_project();
            response["result"] = {{"id", id.value()}};
        } else if (type == "close_project") {
            const auto id = document_id(payload);
            // Asked before anything is dropped. Cancelling comes back the same
            // way a cancelled file dialog does, so the UI just refreshes and
            // finds the document still there.
            if (!shell::resolve_unsaved(m_core, id)) {
                response["result"] = {{"cancelled", true}};
                return response;
            }
            response["result"] = m_core.close_project(id);
        } else if (type == "get_document_snapshot") {
            const auto id = document_id(payload);
            response["result"] = m_core.get_document_snapshot(id);
        } else if (type == "get_open_documents") {
            auto docs = m_core.get_open_documents();
            auto arr = nlohmann::json::array();
            for (auto id : docs) arr.push_back(id.value());
            response["result"] = arr;
        } else if (type == "set_active_document") {
            const auto id = document_id(payload);
            m_core.set_active_document(id);
        } else if (type == "create_note") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.create_note(doc_id, track_id,
                payload.at("tick").get<int64_t>(),
                payload.at("duration").get<int64_t>(),
                checked_u8(payload, "pitch", 0, 127),
                checked_u8(payload, "velocity", 1, 127));
            if (res) response["result"] = {{"id", res->value()}};
            else { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "delete_note") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            base::NoteId note_id{payload.at("noteId").get<uint64_t>()};
            auto res = m_core.delete_note(doc_id, track_id, note_id);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "move_note") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            base::NoteId note_id{payload.at("noteId").get<uint64_t>()};
            auto res = m_core.move_note(doc_id, track_id, note_id, payload.at("tick").get<int64_t>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "resize_note") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            base::NoteId note_id{payload.at("noteId").get<uint64_t>()};
            auto res = m_core.resize_note(doc_id, track_id, note_id, payload.at("duration").get<int64_t>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "update_note") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            base::NoteId note_id{payload.at("noteId").get<uint64_t>()};
            std::optional<uint8_t> pitch, velocity;
            if (payload.contains("pitch")) pitch = checked_u8(payload, "pitch", 0, 127);
            if (payload.contains("velocity")) velocity = checked_u8(payload, "velocity", 1, 127);
            auto res = m_core.update_note(doc_id, track_id, note_id, pitch, velocity);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "create_controller_event") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.create_controller_event(
                doc_id, track_id, payload.at("tick").get<int64_t>(),
                checked_u8(payload, "controller", 0, 127), checked_u8(payload, "value", 0, 127));
            if (res) response["result"] = {{"id", res->value()}};
            else { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "update_controller_event") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            base::EventId event_id{payload.at("eventId").get<uint64_t>()};
            std::optional<int64_t> tick;
            std::optional<uint8_t> controller, value;
            if (payload.contains("tick")) tick = payload.at("tick").get<int64_t>();
            if (payload.contains("controller")) controller = checked_u8(payload, "controller", 0, 127);
            if (payload.contains("value")) value = checked_u8(payload, "value", 0, 127);
            auto res = m_core.update_controller_event(doc_id, track_id, event_id, tick, controller, value);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "delete_controller_event") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            base::EventId event_id{payload.at("eventId").get<uint64_t>()};
            auto res = m_core.delete_controller_event(doc_id, track_id, event_id);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "create_pitch_bend") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.create_pitch_bend(doc_id, track_id, payload.at("tick").get<int64_t>(),
                                                checked_bend(payload, "value"));
            if (res) response["result"] = {{"id", res->value()}};
            else { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "update_pitch_bend") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            base::EventId event_id{payload.at("eventId").get<uint64_t>()};
            std::optional<int64_t> tick;
            std::optional<int16_t> value;
            if (payload.contains("tick")) tick = payload.at("tick").get<int64_t>();
            if (payload.contains("value")) value = checked_bend(payload, "value");
            auto res = m_core.update_pitch_bend(doc_id, track_id, event_id, tick, value);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "delete_pitch_bend") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            base::EventId event_id{payload.at("eventId").get<uint64_t>()};
            auto res = m_core.delete_pitch_bend(doc_id, track_id, event_id);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "batch_edit") {
            const auto doc_id = document_id(payload);
            std::vector<edit::BatchOperation> ops;
            for (const auto& j : payload.at("operations")) {
                edit::BatchOperation op;
                const auto op_type = j.at("type").get<std::string>();
                op.track_id = base::TrackId{j.at("trackId").get<uint64_t>()};
                if (op_type == "CreateNote") {
                    op.type = edit::BatchOperation::Type::CreateNote;
                    op.start = timeline::Tick{j.at("startTick").get<int64_t>()};
                    op.duration = timeline::TickDuration{j.at("durationTicks").get<int64_t>()};
                    op.pitch = checked_u8(j, "pitch", 0, 127);
                    op.velocity = checked_u8(j, "velocity", 1, 127);
                } else if (op_type == "MoveNote") {
                    op.type = edit::BatchOperation::Type::MoveNote;
                    op.note_id = base::NoteId{j.at("noteId").get<uint64_t>()};
                    op.start = timeline::Tick{j.at("newStartTick").get<int64_t>()};
                } else if (op_type == "ResizeNote") {
                    op.type = edit::BatchOperation::Type::ResizeNote;
                    op.note_id = base::NoteId{j.at("noteId").get<uint64_t>()};
                    op.duration = timeline::TickDuration{j.at("newDurationTicks").get<int64_t>()};
                } else if (op_type == "DeleteNote") {
                    op.type = edit::BatchOperation::Type::DeleteNote;
                    op.note_id = base::NoteId{j.at("noteId").get<uint64_t>()};
                } else if (op_type == "UpdateNote") {
                    op.type = edit::BatchOperation::Type::UpdateNote;
                    op.note_id = base::NoteId{j.at("noteId").get<uint64_t>()};
                    if (j.contains("pitch")) op.pitch = checked_u8(j, "pitch", 0, 127);
                    if (j.contains("velocity")) op.velocity = checked_u8(j, "velocity", 1, 127);
                } else {
                    throw std::invalid_argument("Unknown batch operation type: " + op_type);
                }
                ops.push_back(op);
            }
            auto res = m_core.batch_edit(doc_id, std::move(ops));
            if (res) {
                auto ids = nlohmann::json::array();
                for (auto id : *res) ids.push_back(id.value());
                response["result"] = {{"createdNoteIds", ids}};
            } else { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "undo") {
            const auto doc_id = document_id(payload);
            auto res = m_core.undo(doc_id);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "redo") {
            const auto doc_id = document_id(payload);
            auto res = m_core.redo(doc_id);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "create_track") {
            const auto doc_id = document_id(payload);
            auto res = m_core.create_track(doc_id, payload.value("name", std::string{}));
            if (res) response["result"] = {{"id", res->value()}};
            else { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "rename_track") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.rename_track(doc_id, track_id, payload.at("name").get<std::string>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "delete_track") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.delete_track(doc_id, track_id);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_tempo") {
            const auto doc_id = document_id(payload);
            auto res = m_core.set_tempo(doc_id, payload.at("bpm").get<double>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "save_project" || type == "save_project_as") {
            const auto id = document_id(payload);
            // Explicit path (automation) > remembered path (plain save) > dialog.
            std::string path = payload.value("path", std::string{});
            if (path.empty() && type == "save_project") path = m_core.get_project_path(id);
            if (path.empty()) {
                auto chosen = shell::save_file_dialog(kProjectFilter, L"mcproj");
                if (!chosen) { response["result"] = {{"cancelled", true}}; return response; }
                path = *chosen;
            }
            auto res = m_core.save_project(id, path);
            if (res) response["result"] = {{"path", path}};
            else { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "open_project") {
            std::string path = payload.value("path", std::string{});
            if (path.empty()) {
                auto chosen = shell::open_file_dialog(kProjectFilter);
                if (!chosen) { response["result"] = {{"cancelled", true}}; return response; }
                path = *chosen;
            }
            auto res = m_core.open_project(path);
            if (res) response["result"] = {{"id", res->value()}};
            else { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "export_midi") {
            const auto id = document_id(payload);
            std::string path = payload.value("path", std::string{});
            if (path.empty()) {
                auto chosen = shell::save_file_dialog(kMidiFilter, L"mid");
                if (!chosen) { response["result"] = {{"cancelled", true}}; return response; }
                path = *chosen;
            }
            auto res = m_core.export_midi(id, path);
            if (res) response["result"] = {{"path", path}};
            else { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "import_midi") {
            std::string path = payload.value("path", std::string{});
            if (path.empty()) {
                auto chosen = shell::open_file_dialog(kMidiFilter);
                if (!chosen) { response["result"] = {{"cancelled", true}}; return response; }
                path = *chosen;
            }
            auto res = m_core.import_midi(path);
            if (res) response["result"] = {{"id", res->value()}};
            else { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "get_transport_state") {
            response["result"] = {
                {"state", transport_state_name(m_core.playback_engine().state())},
                {"tick", m_core.playback_engine().current_tick().value()},
            };
        } else if (type == "set_track_volume") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.set_track_volume(doc_id, track_id, checked_u8(payload, "volume", 0, 127));
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_master_volume") {
            const auto doc_id = document_id(payload);
            auto res = m_core.set_master_volume(doc_id, checked_u8(payload, "volume", 0, 127));
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_track_pan") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.set_track_pan(doc_id, track_id, checked_u8(payload, "pan", 0, 127));
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_track_mute") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.set_track_mute(doc_id, track_id, payload.at("muted").get<bool>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_track_solo") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.set_track_solo(doc_id, track_id, payload.at("solo").get<bool>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_track_arm") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.set_track_arm(doc_id, track_id, payload.at("armed").get<bool>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_track_channel") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.set_track_channel(doc_id, track_id, payload.at("channel").get<uint8_t>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_time_signature") {
            const auto doc_id = document_id(payload);
            auto res = m_core.set_time_signature(doc_id, payload.value("tick", int64_t{0}),
                                                 checked_u8(payload, "numerator", 1, 32),
                                                 checked_u8(payload, "denominator", 1, 32));
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_key_signature") {
            const auto doc_id = document_id(payload);
            const auto tick = payload.value("tick", int64_t{0});
            const auto fifths = static_cast<int8_t>(payload.at("fifths").get<int>());
            auto res = m_core.set_key_signature(doc_id, tick, fifths, payload.value("minor", false));
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_track_clef") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            const auto clef = music::clef_from_string(payload.at("clef").get<std::string>());
            auto res = m_core.set_track_clef(doc_id, track_id, clef);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_track_program") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.set_track_program(doc_id, track_id, payload.at("program").get<uint8_t>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "audition_program") {
            // Hearing an instrument without committing to it. The document id
            // is asked for even though nothing is edited: it is how the facade
            // finds a channel no track is using.
            const auto doc_id = document_id(payload);
            const auto value = [&payload](const char* key, uint8_t fallback) {
                return payload.contains(key) ? payload.at(key).get<uint8_t>() : fallback;
            };
            auto res = m_core.audition_program(doc_id, payload.at("program").get<uint8_t>(),
                                               value("key", 60), value("velocity", 100),
                                               payload.contains("milliseconds")
                                                   ? payload.at("milliseconds").get<int>()
                                                   : 700);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "get_routing_info") {
            // Which plugin each channel actually reaches. Per-track routing
            // (§9a) decides this from the document, and a track that sounds
            // wrong because its channel goes somewhere unexpected is otherwise
            // invisible: the score, the mixer and the instrument list all look
            // right while the audio comes from another plugin.
            nlohmann::json channels = nlohmann::json::array();
            for (uint8_t ch = 0; ch < 16; ++ch) {
                auto* target = m_core.routing().target_for(ch);
                channels.push_back({{"channel", ch},
                                    {"output", target ? std::string(target->id()) : ""}});
            }
            nlohmann::json result;
            result["channels"] = channels;
            result["selected"] = std::string(m_core.output().id());
            response["result"] = result;
        } else if (type == "get_output_info") {
            // The selected output, its declared parameters and their current
            // values, in one round trip. Values come back with the schema
            // because a plugin may change another parameter's choices, so the
            // two are never read separately.
            auto& out = m_core.output();
            nlohmann::json info;
            info["id"] = std::string(out.id());
            info["name"] = std::string(out.name());
            // The capability query, so the UI knows whether rendering to a file
            // is something this output could even do.
            info["producesAudio"] = out.audio() != nullptr;
            // Whether it is actually being heard, and the evidence that the
            // callback is running at all.
            const auto& audio = m_core.audio_device();
            info["audioDevice"] = {{"running", audio.is_running()},
                                   {"name", audio.device_name()},
                                   {"framesRendered", audio.frames_rendered()},
                                   {"latencyMs", audio.latency_ms()}};
            auto available = nlohmann::json::array();
            for (auto* candidate : m_core.outputs()) {
                available.push_back({{"id", std::string(candidate->id())},
                                     {"name", std::string(candidate->name())}});
            }
            info["available"] = available;
            auto params = nlohmann::json::array();
            for (const auto& p : out.parameters()) {
                nlohmann::json jp;
                jp["name"] = p.name;
                jp["label"] = p.label;
                jp["type"] = parameter_type_name(p.type);
                jp["headline"] = p.headline;
                jp["value"] = parameter_value_to_json(out.get_parameter(p.name));
                switch (p.type) {
                    case playback::ParameterType::Enum: {
                        auto choices = nlohmann::json::array();
                        for (const auto& c : p.choices) {
                            choices.push_back({{"value", c.value}, {"label", c.label}});
                        }
                        jp["choices"] = choices;
                        break;
                    }
                    case playback::ParameterType::Int:
                        jp["min"] = p.min;
                        jp["max"] = p.max;
                        jp["step"] = p.step;
                        jp["unit"] = p.unit;
                        break;
                    case playback::ParameterType::File:
                        jp["filter"] = p.filter;
                        break;
                    default:
                        break;
                }
                params.push_back(jp);
            }
            info["parameters"] = params;
            response["result"] = info;
        } else if (type == "set_track_output") {
            const auto doc_id = document_id(payload);
            base::TrackId track_id{payload.at("trackId").get<uint64_t>()};
            auto res = m_core.set_track_output(doc_id, track_id,
                                               payload.value("outputId", std::string{}));
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "select_output") {
            auto res = m_core.select_output(payload.at("id").get<std::string>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "export_audio") {
            const auto doc_id = document_id(payload);
            std::string path = payload.value("path", std::string{});
            if (path.empty()) {
                auto chosen = shell::save_file_dialog(kAudioFilter, L"wav");
                if (!chosen) { response["result"] = {{"cancelled", true}}; return response; }
                path = *chosen;
            }
            auto res = m_core.export_audio(doc_id, path);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
            else { response["result"] = {{"path", path}}; }
        } else if (type == "set_output_parameter") {
            const auto name = payload.at("name").get<std::string>();
            auto res = m_core.set_output_parameter(name,
                                                   parameter_value_from_json(payload.at("value")));
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "get_program_names") {
            // Empty means General MIDI, which is what the UI already has. Only
            // an output that knows its own instruments answers.
            auto result = nlohmann::json::array();
            for (const auto& program : m_core.output().programs()) {
                result.push_back({{"program", program.program}, {"name", program.name}});
            }
            response["result"] = result;
        } else if (type == "get_preferences") {
            const auto& prefs = m_core.preferences();
            nlohmann::json result;
            result["selectedOutput"] = prefs.selected_output();
            // Resolved, not raw: the UI shows what the click actually goes
            // through, and "empty means the default" is the facade's business.
            result["metronomeOutput"] = m_core.metronome_output_id();
            result["clapSearchPaths"] = prefs.clap_search_paths();
            // Reported separately from the list above because it is not one of
            // them: it is always scanned and cannot be removed.
            const auto own = midi_composer::app::Preferences::plugin_folder().u8string();
            result["pluginFolder"] =
                std::string(reinterpret_cast<const char*>(own.c_str()), own.size());
            // Whatever the interface asked to have remembered. Opaque to the
            // core, so a new panel needs no change down here.
            result["uiLayout"] = prefs.ui_layout();
            response["result"] = result;
        } else if (type == "set_ui_layout") {
            // Saved immediately rather than at exit: a panel someone collapsed
            // should still be collapsed after a crash, and this file is small.
            auto layout = payload.value("layout", nlohmann::json::object());
            if (!layout.is_object()) layout = nlohmann::json::object();
            auto res = m_core.set_ui_layout(std::move(layout));
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "set_clap_search_paths") {
            std::vector<std::string> paths;
            for (const auto& entry : payload.at("paths")) {
                if (entry.is_string()) paths.push_back(entry.get<std::string>());
            }
            auto res = m_core.set_clap_search_paths(std::move(paths));
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "choose_file") {
            // The pattern comes from what the plugin declared for that
            // parameter: the host draws a picker for a File without ever
            // learning what the file is for.
            const auto filter = shell::make_file_filter(
                payload.value("filter", std::string{}));
            auto chosen = shell::open_file_dialog(filter.c_str());
            if (!chosen) { response["result"] = {{"cancelled", true}}; }
            else { response["result"] = {{"path", *chosen}}; }
        } else if (type == "set_metronome_output") {
            auto res = m_core.set_metronome_output(payload.at("id").get<std::string>());
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "reveal_folder") {
            shell::reveal_folder(payload.at("path").get<std::string>());
        } else if (type == "choose_folder") {
            // The core opens it, like every other path the core is given: a
            // browser has no folder picker, and one typed by hand is a typo
            // waiting to be reported as "the plugin was not found".
            auto chosen = shell::open_folder_dialog();
            if (!chosen) { response["result"] = {{"cancelled", true}}; }
            else { response["result"] = {{"path", *chosen}}; }
        } else if (type == "get_midi_input_devices") {
            auto devices = m_core.midi_service().get_input_devices();
            auto arr = nlohmann::json::array();
            for (const auto& dev : devices) {
                arr.push_back({{"index", dev.index}, {"name", dev.name}});
            }
            response["result"] = arr;
        } else if (type == "open_midi_input") {
            int index = payload.at("index").get<int>();
            auto res = m_core.midi_service().open_input_port(index);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "close_midi_input") {
            m_core.midi_service().close_input_port();
        } else if (type == "is_midi_input_open") {
            response["result"] = m_core.midi_service().is_input_open();
        } else if (type == "play") {
            const auto doc_id = document_id(payload);
            auto res = m_core.play(doc_id);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "record") {
            const auto doc_id = document_id(payload);
            auto res = m_core.record(doc_id);
            if (!res) { response["success"] = false; response["error"] = res.error().message; }
        } else if (type == "stop") {
            m_core.stop();
        } else if (type == "pause") {
            m_core.pause();
        } else if (type == "seek") {
            m_core.seek(payload.at("tick").get<int64_t>());
        } else if (type == "set_metronome_enabled") {
            m_core.set_metronome_enabled(payload.at("enabled").get<bool>());
        } else if (type == "is_metronome_enabled") {
            response["result"] = m_core.is_metronome_enabled();
        } else if (type == "ui_log") {
            auto level = payload.value("level", "info");
            auto message = payload.value("message", "");
            if (level == "debug") MC_LOG_DEBUG("[UI] {}", message);
            else if (level == "warn") MC_LOG_WARN("[UI] {}", message);
            else if (level == "error") MC_LOG_ERROR("[UI] {}", message);
            else MC_LOG_INFO("[UI] {}", message);
        } else {
            response["success"] = false;
            response["error"] = "Unknown command type: " + type;
        }
    } catch (const std::exception& e) {
        response["success"] = false;
        response["error"] = e.what();
    }

    return response;
}

void BridgeDispatcher::send_notification(const std::string& type, const nlohmann::json& payload) {
    if (!m_view) return;
    //  MC_LOG_INFO("UI send_notification: ('{}', {})", type, payload.dump());
    // In Saucer 2.0, evaluate is thread-safe but returns a std::future which is [[nodiscard]].
    // We ignore it here because we don't need the result of the notification.
    [[maybe_unused]] auto future = m_view->evaluate<void>("window.dispatchNativeEvent({}, {})", type, payload.dump());
}

} // namespace midi_composer::ui_bridge
