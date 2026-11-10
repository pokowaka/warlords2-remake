#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "absl/flags/flag.h"
#include "platform/soundfont.hpp"
#include "util/util.hpp"
#include "warlords/xmi.hpp"

ABSL_FLAG(bool, mt32, false, "Enable MT-32 translation mode.");
ABSL_FLAG(int, seconds, 0, "Max duration in seconds (0 = full song).");

int main(int argc, char** argv) {
    auto args = w2::ParseCliFlags(argc, argv, "w2sfrender <XMI_FILE> <SF2_FILE> <OUT_WAV> [--mt32] [--seconds N]");
    if (args.size() < 3) {
        fprintf(stderr, "Usage: w2sfrender <XMI_FILE> <SF2_FILE> <OUT_WAV> [--mt32] [--seconds N]\n");
        return 2;
    }
    std::filesystem::path xmi_path = args[0];
    std::filesystem::path sf2_path = args[1];
    std::filesystem::path wav_path = args[2];
    bool is_mt32 = absl::GetFlag(FLAGS_mt32);
    int max_seconds = absl::GetFlag(FLAGS_seconds);

    auto xmi_bytes = w2::ReadBytes(xmi_path);
    if (!xmi_bytes.ok()) {
        fprintf(stderr, "Failed to read XMI: %s\n", std::string(xmi_bytes.status().message()).c_str());
        return 1;
    }
    auto sequence = w2::xmi::Parse(*xmi_bytes);
    if (!sequence) {
        fprintf(stderr, "Failed to parse XMI\n");
        return 1;
    }

    constexpr int kSampleRate = 44100;
    sound::SoundFontSynth synth;
    if (!synth.Load(sf2_path, kSampleRate)) {
        fprintf(stderr, "Failed to load SoundFont: %s\n", sf2_path.string().c_str());
        return 1;
    }

    synth.Play(&*sequence, is_mt32, /*loop=*/false);

    int total_seconds = max_seconds > 0 ? max_seconds : (sequence->length / w2::xmi::kTickRate + 3);
    std::vector<int16_t> pcm_out;
    pcm_out.reserve(static_cast<size_t>(total_seconds) * kSampleRate * 2);

    constexpr int kChunkFrames = 1024;
    std::vector<float> float_chunk(kChunkFrames * 2, 0.0f);
    int frames_rendered = 0;
    int max_frames = total_seconds * kSampleRate;

    while (frames_rendered < max_frames && (synth.Playing() || !synth.Idle())) {
        int to_render = std::min(kChunkFrames, max_frames - frames_rendered);
        std::fill(float_chunk.begin(), float_chunk.end(), 0.0f);
        synth.Render(float_chunk.data(), to_render);
        for (int i = 0; i < to_render * 2; ++i) {
            float s = std::clamp(float_chunk[static_cast<size_t>(i)], -1.0f, 1.0f);
            pcm_out.push_back(static_cast<int16_t>(s * 32767.0f));
        }
        frames_rendered += to_render;
        if (!synth.Playing() && synth.Idle()) break;
    }

    // Write WAV header
    std::ofstream out(wav_path, std::ios::binary);
    if (!out.is_open()) {
        fprintf(stderr, "Failed to open output WAV: %s\n", wav_path.string().c_str());
        return 1;
    }
    uint32_t data_size = static_cast<uint32_t>(pcm_out.size() * sizeof(int16_t));
    uint32_t riff_size = 36 + data_size;
    uint32_t sample_rate = kSampleRate;
    uint16_t channels = 2;
    uint16_t bits_per_sample = 16;
    uint32_t byte_rate = sample_rate * channels * (bits_per_sample / 8);
    uint16_t block_align = channels * (bits_per_sample / 8);

    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riff_size), 4);
    out.write("WAVEfmt ", 8);
    uint32_t fmt_len = 16;
    uint16_t fmt_type = 1; // PCM
    out.write(reinterpret_cast<const char*>(&fmt_len), 4);
    out.write(reinterpret_cast<const char*>(&fmt_type), 2);
    out.write(reinterpret_cast<const char*>(&channels), 2);
    out.write(reinterpret_cast<const char*>(&sample_rate), 4);
    out.write(reinterpret_cast<const char*>(&byte_rate), 4);
    out.write(reinterpret_cast<const char*>(&block_align), 2);
    out.write(reinterpret_cast<const char*>(&bits_per_sample), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&data_size), 4);
    out.write(reinterpret_cast<const char*>(pcm_out.data()), data_size);

    printf("Rendered %d frames (%.2f s) to %s\n", frames_rendered, static_cast<double>(frames_rendered) / kSampleRate, wav_path.string().c_str());
    return 0;
}
