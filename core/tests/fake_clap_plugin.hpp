#pragma once

#include <clap/clap.h>

#include <cstring>
#include <string>
#include <vector>

namespace midi_composer::testing {

/**
 * A CLAP plugin written here, so the host half can be exercised without one
 * being installed.
 *
 * It is a real plugin as far as the ABI is concerned: a `clap_plugin_t` with a
 * filled vtable that the host drives through init, activate, process and
 * destroy exactly as it would drive a downloaded one. What it records is what
 * arrived and in what order, which is the only way to assert that the events a
 * composition produces reach a plugin correctly.
 *
 * The alternative was to ship the host unverified and hope.
 */
class FakeClapPlugin {
public:
    struct Seen {
        uint16_t type;
        uint32_t time;      // sample offset within the block
        int      channel;
        int      key;       // note key, or the controller number
        int      value;     // velocity 0..127, or the controller value
        uint8_t  status;    // for MIDI events, the raw status byte
    };

    explicit FakeClapPlugin(uint32_t dialects = CLAP_NOTE_DIALECT_MIDI) : m_dialects(dialects) {
        m_note_ports.count = [](const clap_plugin_t*, bool is_input) -> uint32_t {
            return is_input ? 1u : 0u;
        };
        m_note_ports.get = [](const clap_plugin_t* p, uint32_t, bool,
                              clap_note_port_info_t* info) -> bool {
            auto* self = static_cast<FakeClapPlugin*>(p->plugin_data);
            info->id = 0;
            info->supported_dialects = self->m_dialects;
            info->preferred_dialect = self->m_dialects;
            std::strncpy(info->name, "in", sizeof(info->name) - 1);
            return true;
        };

        m_plugin.desc = nullptr;
        m_plugin.plugin_data = this;
        m_plugin.init = [](const clap_plugin_t* p) {
            static_cast<FakeClapPlugin*>(p->plugin_data)->initialised = true;
            return true;
        };
        m_plugin.destroy = [](const clap_plugin_t* p) {
            static_cast<FakeClapPlugin*>(p->plugin_data)->destroyed = true;
        };
        m_plugin.activate = [](const clap_plugin_t* p, double rate, uint32_t, uint32_t max) {
            auto* self = static_cast<FakeClapPlugin*>(p->plugin_data);
            if (self->refuse_activate) return false;
            self->activated_rate = rate;
            self->max_block = max;
            return true;
        };
        m_plugin.deactivate = [](const clap_plugin_t* p) {
            static_cast<FakeClapPlugin*>(p->plugin_data)->activated_rate = 0.0;
        };
        m_plugin.start_processing = [](const clap_plugin_t* p) {
            auto* self = static_cast<FakeClapPlugin*>(p->plugin_data);
            self->processing = !self->refuse_start;
            return !self->refuse_start;
        };
        m_plugin.stop_processing = [](const clap_plugin_t* p) {
            static_cast<FakeClapPlugin*>(p->plugin_data)->processing = false;
        };
        m_plugin.reset = [](const clap_plugin_t*) {};
        m_plugin.process = [](const clap_plugin_t* p,
                              const clap_process_t* process) -> clap_process_status {
            auto* self = static_cast<FakeClapPlugin*>(p->plugin_data);
            self->blocks++;
            self->last_frames = process->frames_count;

            const uint32_t count = process->in_events->size(process->in_events);
            for (uint32_t i = 0; i < count; ++i) {
                const auto* h = process->in_events->get(process->in_events, i);
                Seen seen{h->type, h->time, -1, -1, -1, 0};
                if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF) {
                    const auto* n = reinterpret_cast<const clap_event_note_t*>(h);
                    seen.channel = n->channel;
                    seen.key = n->key;
                    seen.value = static_cast<int>(n->velocity * 127.0 + 0.5);
                } else if (h->type == CLAP_EVENT_MIDI) {
                    const auto* m = reinterpret_cast<const clap_event_midi_t*>(h);
                    seen.status = m->data[0];
                    seen.channel = m->data[0] & 0x0F;
                    seen.key = m->data[1];
                    seen.value = m->data[2];
                }
                self->seen.push_back(seen);
            }

            // A constant, so a test can tell rendered audio from silence.
            if (process->audio_outputs_count > 0 && process->audio_outputs[0].data32) {
                for (uint32_t ch = 0; ch < process->audio_outputs[0].channel_count; ++ch) {
                    for (uint32_t i = 0; i < process->frames_count; ++i) {
                        process->audio_outputs[0].data32[ch][i] = self->output_level;
                    }
                }
            }
            return self->fail_process ? CLAP_PROCESS_ERROR : CLAP_PROCESS_CONTINUE;
        };
        m_plugin.get_extension = [](const clap_plugin_t* p, const char* id) -> const void* {
            auto* self = static_cast<FakeClapPlugin*>(p->plugin_data);
            if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0) return &self->m_note_ports;
            return nullptr;
        };
        m_plugin.on_main_thread = [](const clap_plugin_t*) {};
    }

    [[nodiscard]] const clap_plugin_t* plugin() const { return &m_plugin; }

    std::vector<Seen> seen;
    bool  initialised{false};
    bool  destroyed{false};
    bool  processing{false};
    bool  refuse_activate{false};
    bool  refuse_start{false};
    bool  fail_process{false};
    double activated_rate{0.0};
    uint32_t max_block{0};
    uint32_t blocks{0};
    uint32_t last_frames{0};
    float output_level{0.25f};

private:
    clap_plugin_t m_plugin{};
    clap_plugin_note_ports_t m_note_ports{};
    uint32_t m_dialects;
};

} // namespace midi_composer::testing
