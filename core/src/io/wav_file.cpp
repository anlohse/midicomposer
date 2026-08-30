#include "wav_file.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace midi_composer::io {

namespace {

void put_u32(std::vector<unsigned char>& out, uint32_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 16) & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 24) & 0xFF));
}

void put_u16(std::vector<unsigned char>& out, uint16_t v) {
    out.push_back(static_cast<unsigned char>(v & 0xFF));
    out.push_back(static_cast<unsigned char>((v >> 8) & 0xFF));
}

void put_tag(std::vector<unsigned char>& out, const char* tag) {
    out.insert(out.end(), tag, tag + 4);
}

} // namespace

base::Result<void> write_wav(const std::string& utf8_path,
                             const std::vector<float>& interleaved_stereo,
                             int sample_rate) {
    if (sample_rate <= 0) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument,
                                           "Sample rate must be positive"});
    }
    if (interleaved_stereo.size() % 2 != 0) {
        return std::unexpected(base::Error{base::ErrorCode::InvalidArgument,
                                           "Stereo audio needs an even number of samples"});
    }

    constexpr uint16_t kChannels = 2;
    constexpr uint16_t kBits = 16;
    const uint32_t data_bytes = static_cast<uint32_t>(interleaved_stereo.size() * 2);
    const uint32_t byte_rate =
        static_cast<uint32_t>(sample_rate) * kChannels * (kBits / 8);

    std::vector<unsigned char> header;
    header.reserve(44);
    put_tag(header, "RIFF");
    put_u32(header, 36 + data_bytes);
    put_tag(header, "WAVE");
    put_tag(header, "fmt ");
    put_u32(header, 16);            // PCM chunk size
    put_u16(header, 1);             // PCM
    put_u16(header, kChannels);
    put_u32(header, static_cast<uint32_t>(sample_rate));
    put_u32(header, byte_rate);
    put_u16(header, kChannels * (kBits / 8));   // block align
    put_u16(header, kBits);
    put_tag(header, "data");
    put_u32(header, data_bytes);

    std::vector<unsigned char> samples;
    samples.reserve(data_bytes);
    for (float f : interleaved_stereo) {
        // Clamped before scaling: a sample above 1.0 would wrap to full negative
        // and put a click exactly where the music was loudest.
        const float clamped = std::clamp(f, -1.0f, 1.0f);
        const int16_t v = static_cast<int16_t>(std::lround(clamped * 32767.0f));
        put_u16(samples, static_cast<uint16_t>(v));
    }

    const std::filesystem::path path(reinterpret_cast<const char8_t*>(utf8_path.c_str()));
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    if (!file) {
        return std::unexpected(base::Error{base::ErrorCode::IoFailure,
                                           "Could not open '" + utf8_path + "' for writing"});
    }
    file.write(reinterpret_cast<const char*>(header.data()),
               static_cast<std::streamsize>(header.size()));
    file.write(reinterpret_cast<const char*>(samples.data()),
               static_cast<std::streamsize>(samples.size()));
    if (!file) {
        return std::unexpected(base::Error{base::ErrorCode::IoFailure,
                                           "Failed while writing '" + utf8_path + "'"});
    }
    return {};
}

} // namespace midi_composer::io
