#include "../Scenes/Common/CompositeScene.h"
#include "../IO/SurgeXT.h"
#include "../IO/Writer.h"

#include <cmath>
#include <iostream>
#include <vector>

using namespace std;

namespace {

constexpr double kPi = 3.14159265358979323846;

void generate_dry_audio(const double duration_seconds,
                        vector<sample_t>& left,
                        vector<sample_t>& right) {
    const int sample_rate = get_audio_samplerate_hz();
    const int total_samples = static_cast<int>(duration_seconds * sample_rate);
    left.reserve(total_samples);
    right.reserve(total_samples);

    for (int i = 0; i < total_samples; ++i) {
        const double t = static_cast<double>(i) / sample_rate;
        const double envelope = min(1.0, t / 0.05) * min(1.0, (duration_seconds - t) / 0.8);
        const double chord =
            0.040 * sin(2.0 * kPi * 220.0 * t) +
            0.025 * sin(2.0 * kPi * 277.18 * t) +
            0.020 * sin(2.0 * kPi * 329.63 * t);
        const float sample = static_cast<float>(envelope * chord);
        left.push_back(float_to_sample(sample));
        right.push_back(float_to_sample(sample));
    }
}

double circle_radius_at(const SurgeXTEffectContext& context) {
    return 0.15 + 0.85 * context.progress_01;
}

}

void render_video() {
    CompositeScene scene;

    vector<sample_t> left;
    vector<sample_t> right;
    generate_dry_audio(7.0, left, right);

    if (SurgeXTEffect::available()) {
        SurgeXTEffect reverb(get_audio_samplerate_hz(), "Reverb 2");

        cout << "SurgeEffectAutomationDemo: " << reverb.effect_name() << " parameters" << endl;
        for (const auto& parameter : reverb.list_parameters()) {
            cout << "  [" << parameter.index << "] "
                 << parameter.group << " / " << parameter.name
                 << " = " << parameter.display << endl;
        }

        reverb
            .set_parameter_01("Mix", 0.45f)
            .set_parameter_01("Decay Time", 0.42f)
            .automate_parameter_value("Room Size", [](const SurgeXTEffectContext& context) {
                return static_cast<float>(cos(2.0 * kPi * circle_radius_at(context)));
            })
            .automate_parameter_01("Modulation", [](const SurgeXTEffectContext& context) {
                return static_cast<float>(0.25 + 0.60 * context.progress_01);
            });

        SurgeXTEffectRenderOptions options;
        options.start_sample = get_audio_samplerate_hz();
        options.num_samples = 4 * get_audio_samplerate_hz();
        options.tail_seconds = 2.0;
        reverb.process(left, right, options);
    }

    stage_macroblock(GeneratedBlock(left, right), 1);
    scene.render_microblock();
}
