#include "clap_instance.hpp"

#include "base/logger.hpp"

#include <algorithm>
#include <cstring>

namespace midi_composer::playback {

namespace {

// Room for a burst without allocating on the audio thread. Same trade as the
// internal synth's queue: a drop is counted rather than swallowed.
constexpr size_t kMaxPending = 2048;

clap_event_header_t make_header(uint32_t size, uint16_t type) {
    clap_event_header_t h{};
    h.size = size;
    h.time = 0;                     // filled in per block, in sample offsets
    h.space_id = CLAP_CORE_EVENT_SPACE_ID;
    h.type = type;
    h.flags = 0;
    return h;
}

} // namespace

ClapInstance::ClapInstance(const clap_plugin_t* plugin, std::string id, std::string name)
    : m_plugin(plugin), m_id(std::move(id)), m_name(std::move(name)) {
    m_host.clap_version = CLAP_VERSION;
    m_host.host_data = this;
    m_host.name = "MIDI Composer";
    m_host.vendor = "MIDI Composer";
    m_host.url = "";
    m_host.version = MIDI_COMPOSER_VERSION;
    // No extensions offered yet. A plugin asking for one it needs will find
    // nothing and is entitled to refuse to load, which is better than claiming
    // support for something unimplemented.
    m_host.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
    m_host.request_restart = [](const clap_host_t*) {};
    m_host.request_process = [](const clap_host_t*) {};
    m_host.request_callback = [](const clap_host_t*) {};

    m_in_events.ctx = this;
    m_in_events.size = &ClapInstance::input_size;
    m_in_events.get = &ClapInstance::input_get;
    m_out_events.ctx = this;
    m_out_events.try_push = &ClapInstance::output_try_push;

    m_pending.reserve(kMaxPending);
    m_block.reserve(kMaxPending);
    m_block_headers.reserve(kMaxPending);
}

ClapInstance::~ClapInstance() {
    stop();
    if (m_active && m_plugin) {
        m_plugin->deactivate(m_plugin);
        m_active = false;
    }
    if (m_initialised && m_plugin) {
        m_plugin->destroy(m_plugin);
    }
    m_plugin = nullptr;
    // Only now: the library holds the code the destructor above just ran.
    m_owner.reset();
}

void ClapInstance::adopt(const clap_plugin_t* plugin, std::shared_ptr<void> owner) {
    m_plugin = plugin;
    m_owner = std::move(owner);
}

base::Result<void> ClapInstance::initialise() {
    if (!m_plugin) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument, "No plugin"});
    }
    if (!m_plugin->init(m_plugin)) {
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure,
                                           m_name + " failed to initialise"});
    }
    m_initialised = true;

    // Which dialect the plugin speaks decides what can be sent at all. Asked
    // once, here, rather than per event.
    const auto* ports = static_cast<const clap_plugin_note_ports_t*>(
        m_plugin->get_extension(m_plugin, CLAP_EXT_NOTE_PORTS));
    if (ports && ports->count(m_plugin, true) > 0) {
        clap_note_port_info_t info{};
        if (ports->get(m_plugin, 0, true, &info)) {
            m_accepts_midi = (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
            m_accepts_clap_notes = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
        }
    }
    if (!m_accepts_midi && !m_accepts_clap_notes) {
        return std::unexpected(base::Error{
            base::ErrorCode::UnsupportedFormat,
            m_name + " accepts no note input, so it cannot play a composition"});
    }
    return {};
}

base::Result<void> ClapInstance::start() {
    if (!m_initialised) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidState,
                                           m_name + " was not initialised"});
    }
    if (m_processing) return {};

    if (!m_active) {
        // The host owns the rate: several plugins share one device, so the
        // device decides and everything else is told (§9a.5).
        if (!m_plugin->activate(m_plugin, static_cast<double>(m_sample_rate), 1, kMaxBlock)) {
            return std::unexpected(base::Error{base::ErrorCode::DeviceFailure,
                                               m_name + " refused to activate"});
        }
        m_active = true;
    }
    if (!m_plugin->start_processing(m_plugin)) {
        return std::unexpected(base::Error{base::ErrorCode::DeviceFailure,
                                           m_name + " refused to start processing"});
    }
    m_processing = true;
    return {};
}

void ClapInstance::stop() {
    if (m_processing && m_plugin) {
        m_plugin->stop_processing(m_plugin);
        m_processing = false;
    }
    std::lock_guard lock(m_producer);
    m_pending.clear();
}

// ── Events in ────────────────────────────────────────────────────────────────

void ClapInstance::push(const Queued& q) {
    std::lock_guard lock(m_producer);
    if (m_pending.size() >= kMaxPending) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    m_pending.push_back(q);
}

void ClapInstance::note_on(uint8_t channel, uint8_t pitch, uint8_t velocity, int64_t when_us) {
    Queued q;
    q.when_us = when_us;
    if (m_accepts_clap_notes) {
        q.event.note.header = make_header(sizeof(clap_event_note_t), CLAP_EVENT_NOTE_ON);
        q.event.note.note_id = -1;
        q.event.note.port_index = 0;
        q.event.note.channel = channel & 0x0F;
        q.event.note.key = pitch & 0x7F;
        q.event.note.velocity = static_cast<double>(velocity) / 127.0;
    } else {
        q.event.midi.header = make_header(sizeof(clap_event_midi_t), CLAP_EVENT_MIDI);
        q.event.midi.port_index = 0;
        q.event.midi.data[0] = static_cast<uint8_t>(0x90 | (channel & 0x0F));
        q.event.midi.data[1] = pitch & 0x7F;
        q.event.midi.data[2] = velocity & 0x7F;
    }
    push(q);
}

void ClapInstance::note_off(uint8_t channel, uint8_t pitch, int64_t when_us) {
    Queued q;
    q.when_us = when_us;
    if (m_accepts_clap_notes) {
        q.event.note.header = make_header(sizeof(clap_event_note_t), CLAP_EVENT_NOTE_OFF);
        q.event.note.note_id = -1;
        q.event.note.port_index = 0;
        q.event.note.channel = channel & 0x0F;
        q.event.note.key = pitch & 0x7F;
        q.event.note.velocity = 0.0;
    } else {
        q.event.midi.header = make_header(sizeof(clap_event_midi_t), CLAP_EVENT_MIDI);
        q.event.midi.port_index = 0;
        q.event.midi.data[0] = static_cast<uint8_t>(0x80 | (channel & 0x0F));
        q.event.midi.data[1] = pitch & 0x7F;
        q.event.midi.data[2] = 0;
    }
    push(q);
}

void ClapInstance::controller(uint8_t channel, uint8_t controller, uint8_t value, int64_t when_us) {
    if (!m_accepts_midi) {
        // A controller has nowhere else to go: reaching it through parameter
        // automation would mean guessing which parameter, and guessing wrong is
        // worse than not sending. Counted so the silence has a reason.
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Queued q;
    q.when_us = when_us;
    q.event.midi.header = make_header(sizeof(clap_event_midi_t), CLAP_EVENT_MIDI);
    q.event.midi.port_index = 0;
    q.event.midi.data[0] = static_cast<uint8_t>(0xB0 | (channel & 0x0F));
    q.event.midi.data[1] = controller & 0x7F;
    q.event.midi.data[2] = value & 0x7F;
    push(q);
}

void ClapInstance::program_change(uint8_t channel, uint8_t program, int64_t when_us) {
    if (!m_accepts_midi) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Queued q;
    q.when_us = when_us;
    q.event.midi.header = make_header(sizeof(clap_event_midi_t), CLAP_EVENT_MIDI);
    q.event.midi.port_index = 0;
    q.event.midi.data[0] = static_cast<uint8_t>(0xC0 | (channel & 0x0F));
    q.event.midi.data[1] = program & 0x7F;
    q.event.midi.data[2] = 0;
    push(q);
}

void ClapInstance::pitch_bend(uint8_t channel, int16_t value, int64_t when_us) {
    if (!m_accepts_midi) {
        m_dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const int wire = std::clamp(static_cast<int>(value), -8192, 8191) + 8192;
    Queued q;
    q.when_us = when_us;
    q.event.midi.header = make_header(sizeof(clap_event_midi_t), CLAP_EVENT_MIDI);
    q.event.midi.port_index = 0;
    q.event.midi.data[0] = static_cast<uint8_t>(0xE0 | (channel & 0x0F));
    q.event.midi.data[1] = static_cast<uint8_t>(wire & 0x7F);
    q.event.midi.data[2] = static_cast<uint8_t>((wire >> 7) & 0x7F);
    push(q);
}

std::optional<base::Error> ClapInstance::failure() const {
    std::lock_guard lock(m_failure_mutex);
    return m_failure;
}

// ── The event list the plugin reads ──────────────────────────────────────────

uint32_t ClapInstance::input_size(const clap_input_events_t* list) {
    auto* self = static_cast<ClapInstance*>(list->ctx);
    return static_cast<uint32_t>(self->m_block_headers.size());
}

const clap_event_header_t* ClapInstance::input_get(const clap_input_events_t* list,
                                                   uint32_t index) {
    auto* self = static_cast<ClapInstance*>(list->ctx);
    if (index >= self->m_block_headers.size()) return nullptr;
    return self->m_block_headers[index];
}

bool ClapInstance::output_try_push(const clap_output_events_t*, const clap_event_header_t*) {
    // Nothing consumes what a plugin sends back yet. Accepted rather than
    // refused: a plugin is entitled to emit and should not have to care.
    return true;
}

// ── AudioSource ──────────────────────────────────────────────────────────────

void ClapInstance::begin_block(int64_t start_us) {
    m_block_start_us = start_us;
}

void ClapInstance::flush_expired(int64_t block_start_us, int frames) {
    m_block.clear();
    m_block_headers.clear();

    const double us_per_frame = 1'000'000.0 / m_sample_rate;
    {
        std::lock_guard lock(m_producer);
        for (auto& q : m_pending) {
            // The sample offset CLAP wants is exactly what the timestamp was
            // approximating. Already-overdue events land on frame 0 rather than
            // being dropped: live, they routinely arrive late.
            const double offset = static_cast<double>(q.when_us - block_start_us) / us_per_frame;
            const int frame = std::clamp(static_cast<int>(offset), 0, frames - 1);
            q.event.note.header.time = static_cast<uint32_t>(frame);
            m_block.push_back(q);
        }
        m_pending.clear();
    }

    // Sorted by sample offset, which the specification requires of the host.
    std::sort(m_block.begin(), m_block.end(), [](const Queued& a, const Queued& b) {
        return a.event.note.header.time < b.event.note.header.time;
    });
    for (auto& q : m_block) {
        m_block_headers.push_back(reinterpret_cast<const clap_event_header_t*>(&q.event));
    }
}

void ClapInstance::render(float* interleaved, int frames) {
    if (!m_processing || frames <= 0) {
        std::fill(interleaved, interleaved + static_cast<size_t>(frames) * 2, 0.0f);
        return;
    }

    flush_expired(m_block_start_us, frames);

    m_left.assign(static_cast<size_t>(frames), 0.0f);
    m_right.assign(static_cast<size_t>(frames), 0.0f);
    float* channels[2] = {m_left.data(), m_right.data()};

    clap_audio_buffer_t out{};
    out.data32 = channels;
    out.data64 = nullptr;
    out.channel_count = 2;
    out.latency = 0;
    out.constant_mask = 0;

    clap_process_t process{};
    process.steady_time = m_steady_time;
    process.frames_count = static_cast<uint32_t>(frames);
    process.transport = nullptr;          // free running; no transport events
    process.audio_inputs = nullptr;
    process.audio_inputs_count = 0;
    process.audio_outputs = &out;
    process.audio_outputs_count = 1;
    process.in_events = &m_in_events;
    process.out_events = &m_out_events;

    const auto status = m_plugin->process(m_plugin, &process);
    m_steady_time += frames;

    if (status == CLAP_PROCESS_ERROR) {
        std::lock_guard lock(m_failure_mutex);
        if (!m_failure) {
            m_failure = base::Error{base::ErrorCode::DeviceFailure,
                                    m_name + " stopped processing"};
        }
        std::fill(interleaved, interleaved + static_cast<size_t>(frames) * 2, 0.0f);
        return;
    }

    // CLAP renders channels apart; everything downstream here is interleaved.
    for (int i = 0; i < frames; ++i) {
        interleaved[i * 2]     = m_left[static_cast<size_t>(i)];
        interleaved[i * 2 + 1] = m_right[static_cast<size_t>(i)];
    }
}

} // namespace midi_composer::playback
