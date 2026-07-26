#include "whisper.h"
#include "ggml-backend.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

void disable_whisper_log(enum ggml_log_level, const char *, void *) {}

bool read_exact(char * destination, std::streamsize bytes) {
    std::cin.read(destination, bytes);
    return std::cin.gcount() == bytes;
}

} // namespace

int main(int argc, char ** argv) {
    if (argc != 2) {
        std::cerr << "usage: clap-vad <ggml-silero-vad-model>\n";
        return 2;
    }

    whisper_log_set(disable_whisper_log, nullptr);
    ggml_backend_load_all();
    whisper_vad_context_params params = whisper_vad_default_context_params();
    params.n_threads = 2;
    params.use_gpu = false;

    whisper_vad_context * context = whisper_vad_init_from_file_with_params(argv[1], params);
    if (context == nullptr) {
        std::cerr << "failed to initialize Silero VAD\n";
        return 3;
    }

    std::cout << "READY\n" << std::flush;

    while (true) {
        uint32_t sample_count = 0;
        if (!read_exact(reinterpret_cast<char *>(&sample_count), sizeof(sample_count))) break;
        if (sample_count == 0 || sample_count > 16000 * 2) {
            std::cout << "ERROR invalid-sample-count\n" << std::flush;
            continue;
        }

        std::vector<float> samples(sample_count);
        if (!read_exact(reinterpret_cast<char *>(samples.data()),
                        static_cast<std::streamsize>(sample_count * sizeof(float)))) {
            break;
        }

        if (!whisper_vad_detect_speech(context, samples.data(), static_cast<int>(samples.size()))) {
            std::cout << "ERROR vad-failed\n" << std::flush;
            continue;
        }

        const int probability_count = whisper_vad_n_probs(context);
        const float * probabilities = whisper_vad_probs(context);
        float maximum = 0.0f;
        float sum = 0.0f;
        for (int i = 0; i < probability_count; ++i) {
            maximum = std::max(maximum, probabilities[i]);
            sum += probabilities[i];
        }
        const float average = probability_count > 0 ? sum / probability_count : 0.0f;
        std::cout << "VAD " << maximum << " " << average << "\n" << std::flush;
    }

    whisper_vad_free(context);
    return 0;
}
