#include "SurgeXT.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef SWAPTUBE_USE_SURGE_XT
#include "Effect.h"
#include "ModulationSource.h"
#include "SurgeSynthesizer.h"
#include "SurgeStorage.h"
#include "dsp/SurgeVoice.h"
#endif

using namespace std;

namespace {

float clamp01(const float value) {
    return std::max(0.0f, std::min(1.0f, value));
}

float clamp_bipolar(const float value) {
    return std::max(-1.0f, std::min(1.0f, value));
}

string lowercase_copy(string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

string lookup_key_copy(const string& value) {
    string key;
    key.reserve(value.size());
    for (const unsigned char ch : value) {
        if (std::isalnum(ch)) {
            key.push_back(static_cast<char>(std::tolower(ch)));
        }
    }
    return key;
}

void require_range(const int index, const int count, const char* label) {
    if (index < 0 || index >= count) {
        throw runtime_error(string("Surge XT ") + label + " is out of range.");
    }
}

#ifdef SWAPTUBE_USE_SURGE_XT
double numeric_value_from_parameter(const Parameter& parameter, const pdata& value) {
    switch (parameter.valtype) {
        case vt_float:
            return value.f;
        case vt_int:
            return value.i;
        case vt_bool:
            return value.b ? 1.0 : 0.0;
    }

    return 0.0;
}

modsources require_mod_source_id(const int source_id) {
    require_range(source_id, n_modsources, "modulation source id");
    return static_cast<modsources>(source_id);
}

bool is_supported_effect_type_id(const int effect_type_id) {
    return effect_type_id > fxt_off &&
           effect_type_id < n_fx_types &&
           effect_type_id != fxt_audio_input &&
           effect_type_id != fxt_convolution;
}

int require_supported_effect_type_id(const int effect_type_id) {
    if (!is_supported_effect_type_id(effect_type_id)) {
        throw runtime_error("Unsupported Surge XT effect type id: " + to_string(effect_type_id));
    }
    return effect_type_id;
}

int effect_type_id_for_name_or_throw(const string& effect_name) {
    const string requested_key = lookup_key_copy(effect_name);
    if (requested_key.empty()) {
        throw runtime_error("Surge XT effect name is empty.");
    }

    for (int i = fxt_off + 1; i < n_fx_types; ++i) {
        if (!is_supported_effect_type_id(i)) {
            continue;
        }
        const string candidate = fx_type_names[i] ? fx_type_names[i] : "";
        if (lookup_key_copy(candidate) == requested_key) {
            return i;
        }
    }

    for (int i = fxt_off + 1; i < n_fx_types; ++i) {
        if (!is_supported_effect_type_id(i)) {
            continue;
        }
        const string candidate = fx_type_names[i] ? fx_type_names[i] : "";
        if (lookup_key_copy(candidate).find(requested_key) != string::npos) {
            return i;
        }
    }

    throw runtime_error("Unknown Surge XT effect: " + effect_name);
}

void copy_patch_globaldata(SurgeStorage& storage) {
    storage.getPatch().copy_globaldata(storage.getPatch().globaldata);
}
#endif

#ifdef SWAPTUBE_USE_SURGE_XT
modsources to_surge_mod_source(const SurgeXTModulationSource source) {
    switch (source) {
        case SurgeXTModulationSource::AmpEnvelope:
            return ms_ampeg;
        case SurgeXTModulationSource::FilterEnvelope:
            return ms_filtereg;
        case SurgeXTModulationSource::VoiceLFO1:
            return ms_lfo1;
        case SurgeXTModulationSource::VoiceLFO2:
            return ms_lfo2;
        case SurgeXTModulationSource::VoiceLFO3:
            return ms_lfo3;
        case SurgeXTModulationSource::VoiceLFO4:
            return ms_lfo4;
        case SurgeXTModulationSource::VoiceLFO5:
            return ms_lfo5;
        case SurgeXTModulationSource::VoiceLFO6:
            return ms_lfo6;
        case SurgeXTModulationSource::SceneLFO1:
            return ms_slfo1;
        case SurgeXTModulationSource::SceneLFO2:
            return ms_slfo2;
        case SurgeXTModulationSource::SceneLFO3:
            return ms_slfo3;
        case SurgeXTModulationSource::SceneLFO4:
            return ms_slfo4;
        case SurgeXTModulationSource::SceneLFO5:
            return ms_slfo5;
        case SurgeXTModulationSource::SceneLFO6:
            return ms_slfo6;
    }

    throw runtime_error("Unsupported Surge XT modulation source.");
}
#endif

}

struct SurgeXTEffect::Impl {
#ifdef SWAPTUBE_USE_SURGE_XT
    enum class AutomationUnit {
        Normalized01,
        NativeValue,
    };

    struct Automation {
        int storage_index = -1;
        SurgeXTEffectValueFn value_fn;
        AutomationUnit unit = AutomationUnit::Normalized01;
        bool force_integer = false;
    };

    unique_ptr<SurgeStorage> storage;
    FxStorage* fxstorage = nullptr;
    unique_ptr<Effect> effect;
    int sample_rate_hz = 48000;
    int current_effect_type_id = fxt_off;
    vector<int> parameter_order;
    vector<Automation> automations;

    Impl(const int sample_rate_hz_, const int effect_type_id)
        : sample_rate_hz(sample_rate_hz_) {
        if (sample_rate_hz <= 0) {
            throw runtime_error("Surge XT effect sample rate must be positive.");
        }

        auto config = SurgeStorage::SurgeStorageConfig::fromDataPath(SWAPTUBE_SURGE_XT_DATA_DIR);
        config.createUserDirectory = false;
        config.scanWavetableAndPatches = false;
        storage = make_unique<SurgeStorage>(config);
        storage->setSamplerate(static_cast<float>(sample_rate_hz));
        storage->temposyncratio = 1.0f;
        storage->temposyncratio_inv = 1.0f;
        storage->songpos = 0.0;

        fxstorage = &storage->getPatch().fx[0];
        fxstorage->return_level.id = -1;
        reset_effect(effect_type_id);
    }

    const char* effect_name_c_str() const {
        if (current_effect_type_id > fxt_off && current_effect_type_id < n_fx_types) {
            return fx_type_names[current_effect_type_id];
        }
        return "";
    }

    void reset_effect(const int effect_type_id) {
        current_effect_type_id = require_supported_effect_type_id(effect_type_id);
        fxstorage->type.val.i = current_effect_type_id;
        for (int i = 0; i < n_fx_params; ++i) {
            fxstorage->p[i].set_type(ct_none);
        }

        effect.reset(spawn_effect(current_effect_type_id, storage.get(), fxstorage,
                                  storage->getPatch().globaldata));
        if (!effect) {
            throw runtime_error("Surge XT could not create effect: " + string(effect_name_c_str()));
        }

        effect->init();
        effect->init_ctrltypes();
        effect->init_default_values();
        effect->sampleRateReset();
        rebuild_parameter_order();
        copy_patch_globaldata(*storage);
    }

    void rebuild_parameter_order() {
        vector<pair<int, int>> ordered;
        ordered.reserve(n_fx_params);
        for (int i = 0; i < n_fx_params; ++i) {
            const auto& parameter = fxstorage->p[i];
            if (parameter.ctrltype == ct_none) {
                continue;
            }

            const int sort_key =
                parameter.posy_offset ? i * 2 + parameter.posy_offset : 10000 + i;
            ordered.push_back({i, sort_key});
        }

        sort(ordered.begin(), ordered.end(),
             [](const pair<int, int>& left, const pair<int, int>& right) {
                 return left.second < right.second;
             });

        parameter_order.clear();
        parameter_order.reserve(ordered.size());
        for (const auto& item : ordered) {
            parameter_order.push_back(item.first);
        }
    }

    int require_display_parameter_index(const int parameter_index) const {
        if (parameter_index < 0 || parameter_index >= static_cast<int>(parameter_order.size())) {
            throw runtime_error("Surge XT effect parameter index is out of range.");
        }
        return parameter_order[static_cast<size_t>(parameter_index)];
    }

    optional<int> storage_index_for_name(const string& parameter_name) const {
        const string requested_key = lookup_key_copy(parameter_name);
        if (requested_key.empty()) {
            return nullopt;
        }

        for (const int storage_index : parameter_order) {
            const string name_key = lookup_key_copy(fxstorage->p[storage_index].get_name());
            if (name_key == requested_key) {
                return storage_index;
            }
        }

        for (const int storage_index : parameter_order) {
            const string name_key = lookup_key_copy(fxstorage->p[storage_index].get_name());
            if (name_key.find(requested_key) != string::npos) {
                return storage_index;
            }
        }

        return nullopt;
    }

    int require_storage_index_for_name(const string& parameter_name) const {
        const optional<int> storage_index = storage_index_for_name(parameter_name);
        if (!storage_index) {
            throw runtime_error("Unknown Surge XT effect parameter: " + parameter_name);
        }
        return *storage_index;
    }

    string group_name_for_storage_index(const int storage_index) const {
        if (!effect) {
            return "";
        }

        const auto& parameter = fxstorage->p[storage_index];
        const int fpos = parameter.posy / 10 + parameter.posy_offset;
        string group;
        for (int i = 0; i < n_fx_params && effect->group_label(i); ++i) {
            if (effect->group_label_ypos(i) <= fpos) {
                group = effect->group_label(i);
            }
        }
        return group;
    }

    SurgeXTEffectParameterInfo parameter_info(const int display_index,
                                              const int storage_index) const {
        const auto& parameter = fxstorage->p[storage_index];

        SurgeXTEffectParameterInfo info;
        info.index = display_index;
        info.storage_index = storage_index;
        info.name = parameter.get_name();
        info.group = group_name_for_storage_index(storage_index);
        info.control_type = parameter.ctrltype;
        info.value_type = parameter.valtype;
        info.modulateable = parameter.modulateable;
        info.is_bipolar = parameter.is_bipolar();
        info.is_discrete = parameter.is_discrete_selection();
        info.can_temposync = parameter.can_temposync();
        info.can_extend_range = parameter.can_extend_range();
        info.can_deactivate = parameter.can_deactivate();
        info.enabled = parameter.ctrltype != ct_none;
        info.normalized_value = parameter.get_value_f01();
        info.default_normalized_value = parameter.get_default_value_f01();
        info.value = numeric_value_from_parameter(parameter, parameter.val);
        info.min_value = numeric_value_from_parameter(parameter, parameter.val_min);
        info.max_value = numeric_value_from_parameter(parameter, parameter.val_max);
        info.default_value = numeric_value_from_parameter(parameter, parameter.val_default);
        info.display = parameter.get_display();
        return info;
    }

    vector<SurgeXTEffectParameterInfo> list_parameters() const {
        vector<SurgeXTEffectParameterInfo> parameters;
        parameters.reserve(parameter_order.size());
        for (size_t i = 0; i < parameter_order.size(); ++i) {
            parameters.push_back(parameter_info(static_cast<int>(i), parameter_order[i]));
        }
        return parameters;
    }

    void set_parameter_01_storage(const int storage_index,
                                  const float normalized_01,
                                  const bool force_integer,
                                  const bool copy_globaldata_after = true) {
        require_range(storage_index, n_fx_params, "effect parameter storage index");
        fxstorage->p[storage_index].set_value_f01(clamp01(normalized_01), force_integer);
        if (copy_globaldata_after) {
            copy_patch_globaldata(*storage);
        }
    }

    void set_parameter_value_storage(const int storage_index,
                                     const float value,
                                     const bool force_integer,
                                     const bool copy_globaldata_after = true) {
        require_range(storage_index, n_fx_params, "effect parameter storage index");
        const float normalized_01 = fxstorage->p[storage_index].value_to_normalized(value);
        set_parameter_01_storage(storage_index, normalized_01, force_integer,
                                 copy_globaldata_after);
    }

    void replace_automation(const int storage_index,
                            SurgeXTEffectValueFn value_fn,
                            const AutomationUnit unit,
                            const bool force_integer) {
        if (!value_fn) {
            throw runtime_error("Surge XT effect automation requires a value function.");
        }

        automations.erase(remove_if(automations.begin(), automations.end(),
                                    [storage_index](const Automation& automation) {
                                        return automation.storage_index == storage_index;
                                    }),
                          automations.end());
        automations.push_back({storage_index, std::move(value_fn), unit, force_integer});
    }

    SurgeXTEffectContext context_for(const int64_t sample_index,
                                     const int64_t start_sample,
                                     const int64_t active_sample_count) const {
        SurgeXTEffectContext context;
        context.sample_index = sample_index;
        context.relative_sample_index = max<int64_t>(0, sample_index - start_sample);
        context.time_seconds = static_cast<double>(sample_index) / sample_rate_hz;
        context.relative_time_seconds =
            static_cast<double>(context.relative_sample_index) / sample_rate_hz;
        if (active_sample_count > 1) {
            context.progress_01 =
                static_cast<double>(context.relative_sample_index) /
                static_cast<double>(active_sample_count - 1);
            context.progress_01 = max(0.0, min(1.0, context.progress_01));
        }
        return context;
    }

    void apply_automations(const SurgeXTEffectContext& context) {
        if (automations.empty()) {
            return;
        }

        for (const auto& automation : automations) {
            const float value = automation.value_fn(context);
            if (!std::isfinite(value)) {
                throw runtime_error("Surge XT effect automation returned a non-finite value.");
            }

            if (automation.unit == AutomationUnit::NativeValue) {
                set_parameter_value_storage(automation.storage_index, value,
                                            automation.force_integer, false);
            } else {
                set_parameter_01_storage(automation.storage_index, value,
                                         automation.force_integer, false);
            }
        }
        copy_patch_globaldata(*storage);
    }

    void process(vector<sample_t>& left,
                 vector<sample_t>& right,
                 const SurgeXTEffectRenderOptions& options) {
        if (left.size() != right.size()) {
            throw runtime_error("Surge XT effect processing requires equal left/right lengths.");
        }
        if (!effect) {
            throw runtime_error("Surge XT effect is not initialized.");
        }

        const int64_t total_samples = static_cast<int64_t>(left.size());
        if (total_samples == 0) {
            return;
        }

        const int64_t start_sample = max<int64_t>(0, options.start_sample);
        if (start_sample >= total_samples) {
            return;
        }

        const int64_t requested_count =
            options.num_samples < 0 ? total_samples - start_sample : options.num_samples;
        if (requested_count <= 0) {
            return;
        }

        const int64_t active_sample_count =
            min<int64_t>(requested_count, total_samples - start_sample);
        const int64_t active_end = start_sample + active_sample_count;
        const int64_t tail_blocks =
            options.add_tail && options.tail_seconds > 0.0
                ? static_cast<int64_t>(ceil(options.tail_seconds * sample_rate_hz / BLOCK_SIZE))
                : 0;

        array<float, BLOCK_SIZE> block_left{};
        array<float, BLOCK_SIZE> block_right{};
        int64_t cursor = start_sample;

        while (cursor < active_end) {
            fill(block_left.begin(), block_left.end(), 0.0f);
            fill(block_right.begin(), block_right.end(), 0.0f);

            for (int i = 0; i < BLOCK_SIZE; ++i) {
                const int64_t sample_index = cursor + i;
                if (sample_index >= active_end || sample_index >= total_samples) {
                    break;
                }
                block_left[i] = sample_to_float(left[static_cast<size_t>(sample_index)]);
                block_right[i] = sample_to_float(right[static_cast<size_t>(sample_index)]);
            }

            const SurgeXTEffectContext context =
                context_for(cursor, start_sample, active_sample_count);
            storage->songpos = context.time_seconds;
            apply_automations(context);
            effect->process_ringout(block_left.data(), block_right.data(), true);

            for (int i = 0; i < BLOCK_SIZE; ++i) {
                const int64_t sample_index = cursor + i;
                if (sample_index >= total_samples) {
                    break;
                }

                if (sample_index < active_end) {
                    left[static_cast<size_t>(sample_index)] = float_to_sample(block_left[i]);
                    right[static_cast<size_t>(sample_index)] = float_to_sample(block_right[i]);
                } else if (options.add_tail) {
                    const float mixed_left =
                        sample_to_float(left[static_cast<size_t>(sample_index)]) + block_left[i];
                    const float mixed_right =
                        sample_to_float(right[static_cast<size_t>(sample_index)]) + block_right[i];
                    left[static_cast<size_t>(sample_index)] = float_to_sample(mixed_left);
                    right[static_cast<size_t>(sample_index)] = float_to_sample(mixed_right);
                }
            }

            cursor += BLOCK_SIZE;
        }

        for (int64_t tail_block = 0;
             tail_block < tail_blocks && cursor < total_samples;
             ++tail_block, cursor += BLOCK_SIZE) {
            fill(block_left.begin(), block_left.end(), 0.0f);
            fill(block_right.begin(), block_right.end(), 0.0f);

            storage->songpos = static_cast<double>(cursor) / sample_rate_hz;
            const bool has_output =
                effect->process_ringout(block_left.data(), block_right.data(), false);
            if (!has_output) {
                break;
            }

            for (int i = 0; i < BLOCK_SIZE; ++i) {
                const int64_t sample_index = cursor + i;
                if (sample_index >= total_samples) {
                    break;
                }
                const float mixed_left =
                    sample_to_float(left[static_cast<size_t>(sample_index)]) + block_left[i];
                const float mixed_right =
                    sample_to_float(right[static_cast<size_t>(sample_index)]) + block_right[i];
                left[static_cast<size_t>(sample_index)] = float_to_sample(mixed_left);
                right[static_cast<size_t>(sample_index)] = float_to_sample(mixed_right);
            }
        }
    }
#else
    explicit Impl(const int, const int) {
        throw runtime_error(SurgeXTEffect::availability_message());
    }
#endif
};

SurgeXTEffect::SurgeXTEffect(const int sample_rate_hz, const string& effect_name)
    : SurgeXTEffect(sample_rate_hz,
#ifdef SWAPTUBE_USE_SURGE_XT
                    effect_type_id_for_name_or_throw(effect_name)
#else
                    -1
#endif
                    ) {
#ifndef SWAPTUBE_USE_SURGE_XT
    (void)effect_name;
#endif
}

SurgeXTEffect::SurgeXTEffect(const int sample_rate_hz, const int effect_type_id)
    : impl(make_unique<Impl>(sample_rate_hz, effect_type_id)) {}
SurgeXTEffect::~SurgeXTEffect() = default;
SurgeXTEffect::SurgeXTEffect(SurgeXTEffect&&) noexcept = default;
SurgeXTEffect& SurgeXTEffect::operator=(SurgeXTEffect&&) noexcept = default;

bool SurgeXTEffect::available() {
    return SurgeXT::available();
}

string SurgeXTEffect::availability_message() {
    return SurgeXT::availability_message();
}

vector<SurgeXTEffectTypeInfo> SurgeXTEffect::list_effect_types() {
    vector<SurgeXTEffectTypeInfo> effects;
#ifdef SWAPTUBE_USE_SURGE_XT
    effects.reserve(n_fx_types - 1);
    for (int i = fxt_off + 1; i < n_fx_types; ++i) {
        effects.push_back({
            i,
            fx_type_names[i] ? fx_type_names[i] : "",
            is_supported_effect_type_id(i),
        });
    }
#endif
    return effects;
}

optional<int> SurgeXTEffect::effect_type_id_for_name(const string& effect_name) {
#ifdef SWAPTUBE_USE_SURGE_XT
    try {
        return effect_type_id_for_name_or_throw(effect_name);
    } catch (const runtime_error&) {
        return nullopt;
    }
#else
    (void)effect_name;
    return nullopt;
#endif
}

int SurgeXTEffect::effect_type_id() const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->current_effect_type_id;
#else
    throw runtime_error(availability_message());
#endif
}

string SurgeXTEffect::effect_name() const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->effect_name_c_str();
#else
    throw runtime_error(availability_message());
#endif
}

vector<SurgeXTEffectParameterInfo> SurgeXTEffect::list_parameters() const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->list_parameters();
#else
    return {};
#endif
}

optional<SurgeXTEffectParameterInfo> SurgeXTEffect::get_parameter_info(
    const int parameter_index) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    if (parameter_index < 0 || parameter_index >= static_cast<int>(impl->parameter_order.size())) {
        return nullopt;
    }
    return impl->parameter_info(parameter_index, impl->parameter_order[parameter_index]);
#else
    (void)parameter_index;
    return nullopt;
#endif
}

optional<SurgeXTEffectParameterInfo> SurgeXTEffect::get_parameter_info(
    const string& parameter_name) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    const optional<int> storage_index = impl->storage_index_for_name(parameter_name);
    if (!storage_index) {
        return nullopt;
    }
    for (size_t i = 0; i < impl->parameter_order.size(); ++i) {
        if (impl->parameter_order[i] == *storage_index) {
            return impl->parameter_info(static_cast<int>(i), *storage_index);
        }
    }
    return nullopt;
#else
    (void)parameter_name;
    return nullopt;
#endif
}

int SurgeXTEffect::parameter_index(const string& parameter_name) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    const optional<int> storage_index = impl->storage_index_for_name(parameter_name);
    if (!storage_index) {
        return -1;
    }
    for (size_t i = 0; i < impl->parameter_order.size(); ++i) {
        if (impl->parameter_order[i] == *storage_index) {
            return static_cast<int>(i);
        }
    }
    return -1;
#else
    (void)parameter_name;
    return -1;
#endif
}

SurgeXTEffect& SurgeXTEffect::set_parameter_01(const int parameter_index,
                                               const float normalized_01,
                                               const bool force_integer) {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->set_parameter_01_storage(impl->require_display_parameter_index(parameter_index),
                                   normalized_01, force_integer);
    return *this;
#else
    (void)parameter_index;
    (void)normalized_01;
    (void)force_integer;
    throw runtime_error(availability_message());
#endif
}

SurgeXTEffect& SurgeXTEffect::set_parameter_01(const string& parameter_name,
                                               const float normalized_01,
                                               const bool force_integer) {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->set_parameter_01_storage(impl->require_storage_index_for_name(parameter_name),
                                   normalized_01, force_integer);
    return *this;
#else
    (void)parameter_name;
    (void)normalized_01;
    (void)force_integer;
    throw runtime_error(availability_message());
#endif
}

SurgeXTEffect& SurgeXTEffect::set_parameter_value(const int parameter_index,
                                                  const float value,
                                                  const bool force_integer) {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->set_parameter_value_storage(impl->require_display_parameter_index(parameter_index),
                                      value, force_integer);
    return *this;
#else
    (void)parameter_index;
    (void)value;
    (void)force_integer;
    throw runtime_error(availability_message());
#endif
}

SurgeXTEffect& SurgeXTEffect::set_parameter_value(const string& parameter_name,
                                                  const float value,
                                                  const bool force_integer) {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->set_parameter_value_storage(impl->require_storage_index_for_name(parameter_name),
                                      value, force_integer);
    return *this;
#else
    (void)parameter_name;
    (void)value;
    (void)force_integer;
    throw runtime_error(availability_message());
#endif
}

bool SurgeXTEffect::set_parameter_from_string(const string& parameter_name,
                                              const string& value,
                                              string& error_message) {
#ifdef SWAPTUBE_USE_SURGE_XT
    const int storage_index = impl->require_storage_index_for_name(parameter_name);
    if (!impl->fxstorage->p[storage_index].set_value_from_string(value, error_message)) {
        return false;
    }
    copy_patch_globaldata(*impl->storage);
    return true;
#else
    (void)parameter_name;
    (void)value;
    error_message = availability_message();
    throw runtime_error(availability_message());
#endif
}

SurgeXTEffect& SurgeXTEffect::automate_parameter_01(const string& parameter_name,
                                                    SurgeXTEffectValueFn value_fn,
                                                    const bool force_integer) {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->replace_automation(impl->require_storage_index_for_name(parameter_name),
                             std::move(value_fn), Impl::AutomationUnit::Normalized01,
                             force_integer);
    return *this;
#else
    (void)parameter_name;
    (void)value_fn;
    (void)force_integer;
    throw runtime_error(availability_message());
#endif
}

SurgeXTEffect& SurgeXTEffect::automate_parameter_value(const string& parameter_name,
                                                       SurgeXTEffectValueFn value_fn,
                                                       const bool force_integer) {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->replace_automation(impl->require_storage_index_for_name(parameter_name),
                             std::move(value_fn), Impl::AutomationUnit::NativeValue,
                             force_integer);
    return *this;
#else
    (void)parameter_name;
    (void)value_fn;
    (void)force_integer;
    throw runtime_error(availability_message());
#endif
}

SurgeXTEffect& SurgeXTEffect::clear_automations() {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->automations.clear();
    return *this;
#else
    throw runtime_error(availability_message());
#endif
}

SurgeXTEffect& SurgeXTEffect::clear_automation(const string& parameter_name) {
#ifdef SWAPTUBE_USE_SURGE_XT
    const int storage_index = impl->require_storage_index_for_name(parameter_name);
    impl->automations.erase(remove_if(impl->automations.begin(), impl->automations.end(),
                                      [storage_index](const Impl::Automation& automation) {
                                          return automation.storage_index == storage_index;
                                      }),
                            impl->automations.end());
    return *this;
#else
    (void)parameter_name;
    throw runtime_error(availability_message());
#endif
}

float SurgeXTEffect::get_parameter_01(const string& parameter_name) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->fxstorage->p[impl->require_storage_index_for_name(parameter_name)].get_value_f01();
#else
    (void)parameter_name;
    throw runtime_error(availability_message());
#endif
}

float SurgeXTEffect::normalized_to_value(const string& parameter_name,
                                         const float normalized_01) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->fxstorage->p[impl->require_storage_index_for_name(parameter_name)]
        .normalized_to_value(clamp01(normalized_01));
#else
    (void)parameter_name;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

float SurgeXTEffect::value_to_normalized(const string& parameter_name, const float value) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->fxstorage->p[impl->require_storage_index_for_name(parameter_name)]
        .value_to_normalized(value);
#else
    (void)parameter_name;
    (void)value;
    throw runtime_error(availability_message());
#endif
}

string SurgeXTEffect::get_parameter_display(const string& parameter_name) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->fxstorage->p[impl->require_storage_index_for_name(parameter_name)].get_display();
#else
    (void)parameter_name;
    throw runtime_error(availability_message());
#endif
}

string SurgeXTEffect::get_parameter_display_for_normalized(const string& parameter_name,
                                                           const float normalized_01) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    auto& parameter = impl->fxstorage->p[impl->require_storage_index_for_name(parameter_name)];
    return parameter.get_display(true, parameter.normalized_to_value(clamp01(normalized_01)));
#else
    (void)parameter_name;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXTEffect::process(vector<sample_t>& left,
                            vector<sample_t>& right,
                            const SurgeXTEffectRenderOptions& options) {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->process(left, right, options);
#else
    (void)left;
    (void)right;
    (void)options;
    throw runtime_error(availability_message());
#endif
}

void SurgeXTEffect::process_from_sample(vector<sample_t>& left,
                                        vector<sample_t>& right,
                                        const int64_t start_sample) {
    SurgeXTEffectRenderOptions options;
    options.start_sample = start_sample;
    process(left, right, options);
}

void SurgeXTEffect::process_from_seconds(vector<sample_t>& left,
                                         vector<sample_t>& right,
                                         const double start_seconds) {
    if (start_seconds < 0.0) {
        throw runtime_error("Surge XT effect start time must be non-negative.");
    }
#ifdef SWAPTUBE_USE_SURGE_XT
    SurgeXTEffectRenderOptions options;
    options.start_sample = static_cast<int64_t>(llround(start_seconds * impl->sample_rate_hz));
    process(left, right, options);
#else
    (void)left;
    (void)right;
    (void)start_seconds;
    throw runtime_error(availability_message());
#endif
}

void SurgeXTEffect::process_from_frame(vector<sample_t>& left,
                                       vector<sample_t>& right,
                                       const int64_t start_frame,
                                       const int video_framerate_fps) {
    if (start_frame < 0) {
        throw runtime_error("Surge XT effect start frame must be non-negative.");
    }
    if (video_framerate_fps <= 0) {
        throw runtime_error("Surge XT effect video framerate must be positive.");
    }
#ifdef SWAPTUBE_USE_SURGE_XT
    SurgeXTEffectRenderOptions options;
    options.start_sample =
        static_cast<int64_t>(llround(static_cast<double>(start_frame) *
                                     impl->sample_rate_hz / video_framerate_fps));
    process(left, right, options);
#else
    (void)left;
    (void)right;
    (void)start_frame;
    (void)video_framerate_fps;
    throw runtime_error(availability_message());
#endif
}

struct SurgeXT::Impl {
#ifdef SWAPTUBE_USE_SURGE_XT
    struct PluginLayerProxy : public SurgeSynthesizer::PluginLayer {
        void surgeParameterUpdated(const SurgeSynthesizer::ID&, float) override {}
        void surgeMacroUpdated(long, float) override {}
    };

    unique_ptr<PluginLayerProxy> plugin_layer;
    shared_ptr<SurgeSynthesizer> synth;

    explicit Impl(const int sample_rate_hz) {
        plugin_layer = make_unique<PluginLayerProxy>();

        const string data_path = SWAPTUBE_SURGE_XT_DATA_DIR;
        const string supplied_path =
            data_path.empty() ? SurgeStorage::skipPatchLoadDataPathSentinel : data_path;

        synth = shared_ptr<SurgeSynthesizer>(new SurgeSynthesizer(plugin_layer.get(), supplied_path));
        synth->setSamplerate(static_cast<float>(sample_rate_hz));
        synth->time_data.tempo = 120;
        synth->time_data.ppqPos = 0;

        for (int i = 0; i < 4; ++i) {
            synth->process();
        }
    }

    bool has_parameter(const long parameter_id) const {
        return parameter_id >= 0 &&
               parameter_id < static_cast<long>(synth->storage.getPatch().param_ptr.size());
    }

    SurgeSynthesizer::ID require_id(const long parameter_id) const {
        SurgeSynthesizer::ID id;
        if (!synth->fromSynthSideId(static_cast<int>(parameter_id), id)) {
            throw runtime_error("Surge XT parameter id is out of range.");
        }
        return id;
    }

    Parameter* require_parameter(const long parameter_id) const {
        return synth->storage.getPatch().param_ptr.at(static_cast<size_t>(parameter_id));
    }

    string parameter_name(const long parameter_id) const {
        char buffer[512]{};
        synth->getParameterName(require_id(parameter_id), buffer);
        return buffer;
    }

    string parameter_display(const long parameter_id) const {
        return require_parameter(parameter_id)->get_display();
    }

    string parameter_display_for_normalized(const long parameter_id, const float normalized_01) const {
        auto* parameter = require_parameter(parameter_id);
        return parameter->get_display(true, parameter->normalized_to_value(clamp01(normalized_01)));
    }

    long parameter_id_for(const Parameter* parameter) const {
        return parameter->id;
    }

    void set_parameter_01(Parameter* parameter, const float normalized_01) {
        synth->setParameter01(synth->idForParameter(parameter), clamp01(normalized_01), true, false);
        synth->processAudioThreadOpsWhenAudioEngineUnavailable(true);
    }

    bool set_parameter_01(const long parameter_id, const float normalized_01, const bool force_integer) {
        const bool success =
            synth->setParameter01(require_id(parameter_id), clamp01(normalized_01), true, force_integer);
        synth->processAudioThreadOpsWhenAudioEngineUnavailable(true);
        return success;
    }
#else
    explicit Impl(const int) {
        throw runtime_error(SurgeXT::availability_message());
    }
#endif
};

SurgeXT::SurgeXT(const int sample_rate_hz) : impl(make_unique<Impl>(sample_rate_hz)) {}
SurgeXT::~SurgeXT() = default;
SurgeXT::SurgeXT(SurgeXT&&) noexcept = default;
SurgeXT& SurgeXT::operator=(SurgeXT&&) noexcept = default;

bool SurgeXT::available() {
#ifdef SWAPTUBE_USE_SURGE_XT
    return true;
#else
    return false;
#endif
}

string SurgeXT::availability_message() {
#ifdef SWAPTUBE_USE_SURGE_XT
    return "Surge XT is available.";
#else
    return "Surge XT support is disabled. Reconfigure with -DSWAPTUBE_ENABLE_SURGE_XT=ON "
           "-DSURGE_XT_SOURCE_DIR=/abs/path/to/surge";
#endif
}

vector<SurgeXTPatchInfo> SurgeXT::list_patches() const {
    vector<SurgeXTPatchInfo> patches;
#ifdef SWAPTUBE_USE_SURGE_XT
    if (!impl || !impl->synth) {
        return patches;
    }

    const auto& storage = impl->synth->storage;
    patches.reserve(storage.patchOrdering.empty() ? storage.patch_list.size() : storage.patchOrdering.size());

    if (storage.patchOrdering.empty()) {
        for (size_t i = 0; i < storage.patch_list.size(); ++i) {
            const auto& patch = storage.patch_list[i];
            const string category =
                patch.category >= 0 && patch.category < static_cast<int>(storage.patch_category.size())
                    ? storage.patch_category[patch.category].name
                    : "";
            patches.push_back({static_cast<int>(i), patch.name, category, patch.path.string()});
        }
        return patches;
    }

    for (size_t ordered_index = 0; ordered_index < storage.patchOrdering.size(); ++ordered_index) {
        const int patch_index = storage.patchOrdering[ordered_index];
        if (patch_index < 0 || patch_index >= static_cast<int>(storage.patch_list.size())) {
            continue;
        }
        const auto& patch = storage.patch_list[patch_index];
        const string category =
            patch.category >= 0 && patch.category < static_cast<int>(storage.patch_category.size())
                ? storage.patch_category[patch.category].name
                : "";
        patches.push_back({static_cast<int>(ordered_index), patch.name, category, patch.path.string()});
    }
#endif
    return patches;
}

vector<SurgeXTParameterInfo> SurgeXT::list_parameters() const {
    vector<SurgeXTParameterInfo> parameters;
#ifdef SWAPTUBE_USE_SURGE_XT
    if (!impl || !impl->synth) {
        return parameters;
    }

    const auto& param_ptrs = impl->synth->storage.getPatch().param_ptr;
    parameters.reserve(param_ptrs.size());
    for (const auto* parameter : param_ptrs) {
        parametermeta metadata{};
        impl->synth->getParameterMeta(impl->synth->idForParameter(parameter), metadata);

        SurgeXTParameterInfo info;
        info.id = parameter->id;
        info.name = parameter->get_name();
        info.full_name = parameter->get_full_name();
        info.storage_name = parameter->get_storage_name();
        info.ui_identifier = parameter->ui_identifier;
        info.osc_name = parameter->oscName;
        info.control_group =
            (parameter->ctrlgroup >= 0 && parameter->ctrlgroup < endCG)
                ? ControlGroupDisplay[parameter->ctrlgroup]
                : "";
        info.control_group_id = parameter->ctrlgroup;
        info.control_group_entry = parameter->ctrlgroup_entry;
        info.scene = parameter->scene;
        info.param_id_in_scene = parameter->param_id_in_scene;
        info.control_type = parameter->ctrltype;
        info.value_type = parameter->valtype;
        info.modulateable = parameter->modulateable;
        info.is_bipolar = parameter->is_bipolar();
        info.is_discrete = parameter->is_discrete_selection();
        info.can_temposync = parameter->can_temposync();
        info.can_extend_range = parameter->can_extend_range();
        info.can_deactivate = parameter->can_deactivate();
        info.can_be_absolute = parameter->can_be_absolute();
        info.can_be_nondestructively_modulated = parameter->can_be_nondestructively_modulated();
        info.hidden = metadata.hide;
        info.expert = metadata.expert;
        info.meta = metadata.meta;
        info.normalized_value = parameter->get_value_f01();
        info.default_normalized_value = parameter->get_default_value_f01();
        info.value = numeric_value_from_parameter(*parameter, parameter->val);
        info.min_value = numeric_value_from_parameter(*parameter, parameter->val_min);
        info.max_value = numeric_value_from_parameter(*parameter, parameter->val_max);
        info.default_value = numeric_value_from_parameter(*parameter, parameter->val_default);
        parameters.push_back(std::move(info));
    }
#endif
    return parameters;
}

optional<SurgeXTParameterInfo> SurgeXT::get_parameter_info(const long parameter_id) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    if (!impl || !impl->has_parameter(parameter_id)) {
        return nullopt;
    }

    const auto all_parameters = list_parameters();
    for (const auto& parameter : all_parameters) {
        if (parameter.id == parameter_id) {
            return parameter;
        }
    }
    return nullopt;
#else
    (void)parameter_id;
    return nullopt;
#endif
}

vector<long> SurgeXT::find_parameter_ids(const string& query) const {
    vector<long> ids;
#ifdef SWAPTUBE_USE_SURGE_XT
    const string lowered_query = lowercase_copy(query);
    if (lowered_query.empty()) {
        return ids;
    }

    const auto& param_ptrs = impl->synth->storage.getPatch().param_ptr;
    ids.reserve(param_ptrs.size());
    for (const auto* parameter : param_ptrs) {
        const string haystack =
            lowercase_copy(string(parameter->get_name()) + "\n" + parameter->get_full_name() + "\n" +
                           parameter->get_storage_name() + "\n" + parameter->ui_identifier + "\n" +
                           parameter->oscName);
        if (haystack.find(lowered_query) != string::npos) {
            ids.push_back(parameter->id);
        }
    }
#else
    (void)query;
#endif
    return ids;
}

vector<SurgeXTModulationSourceInfo> SurgeXT::list_modulation_sources() const {
    vector<SurgeXTModulationSourceInfo> sources;
#ifdef SWAPTUBE_USE_SURGE_XT
    sources.reserve(n_modsources);
    for (int source_id = 0; source_id < n_modsources; ++source_id) {
        const modsources source = static_cast<modsources>(source_id);
        sources.push_back({
            source_id,
            modsource_names_tag[source_id],
            modsource_names[source_id],
            modsource_names_button[source_id],
            isScenelevel(source),
            isEnvelope(source),
            isLFO(source),
            isCustomController(source),
            isVoiceModulator(source),
        });
    }
#endif
    return sources;
}

bool SurgeXT::has_parameter(const long parameter_id) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl && impl->has_parameter(parameter_id);
#else
    (void)parameter_id;
    return false;
#endif
}

void SurgeXT::load_patch(const int ordered_patch_index) {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->synth->loadPatch(ordered_patch_index);
    impl->synth->processAudioThreadOpsWhenAudioEngineUnavailable(true);
    for (int i = 0; i < 4; ++i) {
        impl->synth->process();
    }
#else
    (void)ordered_patch_index;
    throw runtime_error(availability_message());
#endif
}

bool SurgeXT::load_patch_by_path(const string& fxp_path, const string& patch_name) {
#ifdef SWAPTUBE_USE_SURGE_XT
    const bool loaded = impl->synth->loadPatchByPath(fxp_path.c_str(), -1, patch_name.c_str(), true);
    impl->synth->processAudioThreadOpsWhenAudioEngineUnavailable(true);
    for (int i = 0; i < 4; ++i) {
        impl->synth->process();
    }
    return loaded;
#else
    (void)fxp_path;
    (void)patch_name;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::note_on(const int midi_note, const int velocity, const int channel, const int note_id) {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->synth->playNote(static_cast<char>(channel), static_cast<char>(midi_note),
                          static_cast<char>(velocity), 0, note_id);
#else
    (void)midi_note;
    (void)velocity;
    (void)channel;
    (void)note_id;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::note_off(const int midi_note, const int velocity, const int channel, const int note_id) {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->synth->releaseNote(static_cast<char>(channel), static_cast<char>(midi_note),
                             static_cast<char>(velocity), note_id);
#else
    (void)midi_note;
    (void)velocity;
    (void)channel;
    (void)note_id;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::all_notes_off() {
#ifdef SWAPTUBE_USE_SURGE_XT
    impl->synth->allNotesOff();
#else
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_pitch_bend(const float normalized_bipolar, const int channel) {
#ifdef SWAPTUBE_USE_SURGE_XT
    const int value = static_cast<int>(std::lround((clamp_bipolar(normalized_bipolar) + 1.0f) * 8192.0f));
    impl->synth->pitchBend(static_cast<char>(channel), std::max(0, std::min(16383, value)));
#else
    (void)normalized_bipolar;
    (void)channel;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_channel_cc(const int cc, const float normalized_01, const int channel) {
#ifdef SWAPTUBE_USE_SURGE_XT
    const int value = static_cast<int>(std::lround(clamp01(normalized_01) * 127.0f));
    impl->synth->channelController(static_cast<char>(channel), cc, value);
#else
    (void)cc;
    (void)normalized_01;
    (void)channel;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_mod_wheel(const float normalized_01, const int channel) {
    set_channel_cc(1, normalized_01, channel);
}

void SurgeXT::set_aftertouch(const float normalized_01, const int channel) {
#ifdef SWAPTUBE_USE_SURGE_XT
    const int value = static_cast<int>(std::lround(clamp01(normalized_01) * 127.0f));
    impl->synth->channelAftertouch(static_cast<char>(channel), value);
#else
    (void)normalized_01;
    (void)channel;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_macro(const int macro_index_1_based, const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    if (macro_index_1_based < 1 || macro_index_1_based > 8) {
        throw runtime_error("Surge XT macro index must be in the range 1..8.");
    }
    impl->synth->setMacroParameter01(macro_index_1_based - 1, clamp01(normalized_01));
#else
    (void)macro_index_1_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_note_pitch(const int note_id,
                             const int held_midi_note,
                             const float midi_note,
                             const int channel) {
#ifdef SWAPTUBE_USE_SURGE_XT
    const float clamped_midi_note = std::max(0.0f, std::min(127.0f, midi_note));
    const float detune_semitones = clamped_midi_note - static_cast<float>(held_midi_note);
    impl->synth->setNoteExpression(SurgeVoice::PITCH, note_id, -1,
                                   static_cast<int16_t>(channel), detune_semitones);
#else
    (void)note_id;
    (void)held_midi_note;
    (void)midi_note;
    (void)channel;
    throw runtime_error(availability_message());
#endif
}

bool SurgeXT::set_parameter_01(const long parameter_id,
                               const float normalized_01,
                               const bool force_integer) {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->set_parameter_01(parameter_id, normalized_01, force_integer);
#else
    (void)parameter_id;
    (void)normalized_01;
    (void)force_integer;
    throw runtime_error(availability_message());
#endif
}

bool SurgeXT::set_parameter_value(const long parameter_id,
                                  const float value,
                                  const bool force_integer) {
#ifdef SWAPTUBE_USE_SURGE_XT
    const float normalized_01 = value_to_normalized(parameter_id, value);
    return impl->set_parameter_01(parameter_id, normalized_01, force_integer);
#else
    (void)parameter_id;
    (void)value;
    (void)force_integer;
    throw runtime_error(availability_message());
#endif
}

bool SurgeXT::set_parameter_from_string(const long parameter_id,
                                        const string& value,
                                        string& error_message) {
#ifdef SWAPTUBE_USE_SURGE_XT
    const auto parameter_id_obj = impl->require_id(parameter_id);
    float normalized_01 = 0.0f;
    if (!impl->synth->stringToNormalizedValue(parameter_id_obj, value, normalized_01)) {
        error_message = "Surge XT could not parse that parameter value.";
        return false;
    }

    error_message.clear();
    return impl->set_parameter_01(parameter_id, normalized_01, false);
#else
    (void)parameter_id;
    (void)value;
    error_message = availability_message();
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_scene_volume(const int scene_index_0_based, const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    impl->set_parameter_01(&impl->synth->storage.getPatch().scene[scene_index_0_based].volume,
                           normalized_01);
#else
    (void)scene_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_scene_pan(const int scene_index_0_based, const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    impl->set_parameter_01(&impl->synth->storage.getPatch().scene[scene_index_0_based].pan,
                           normalized_01);
#else
    (void)scene_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_scene_width(const int scene_index_0_based, const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    impl->set_parameter_01(&impl->synth->storage.getPatch().scene[scene_index_0_based].width,
                           normalized_01);
#else
    (void)scene_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_waveshaper_drive(const int scene_index_0_based, const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    impl->set_parameter_01(&impl->synth->storage.getPatch().scene[scene_index_0_based].wsunit.drive,
                           normalized_01);
#else
    (void)scene_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_scene_send_level(const int scene_index_0_based,
                                   const int send_slot_0_based,
                                   const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(send_slot_0_based, n_send_slots, "send slot");
    impl->set_parameter_01(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].send_level[send_slot_0_based],
        normalized_01);
#else
    (void)scene_index_0_based;
    (void)send_slot_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_fx_return_level(const int fx_slot_0_based, const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(fx_slot_0_based, n_fx_slots, "FX slot");
    impl->set_parameter_01(&impl->synth->storage.getPatch().fx[fx_slot_0_based].return_level,
                           normalized_01);
#else
    (void)fx_slot_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_filter_cutoff(const int scene_index_0_based,
                                const int filter_index_0_based,
                                const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(filter_index_0_based, n_filterunits_per_scene, "filter index");
    impl->set_parameter_01(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].filterunit[filter_index_0_based]
             .cutoff,
        normalized_01);
#else
    (void)scene_index_0_based;
    (void)filter_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_filter_resonance(const int scene_index_0_based,
                                   const int filter_index_0_based,
                                   const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(filter_index_0_based, n_filterunits_per_scene, "filter index");
    impl->set_parameter_01(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].filterunit[filter_index_0_based]
             .resonance,
        normalized_01);
#else
    (void)scene_index_0_based;
    (void)filter_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_filter_env_amount(const int scene_index_0_based,
                                    const int filter_index_0_based,
                                    const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(filter_index_0_based, n_filterunits_per_scene, "filter index");
    impl->set_parameter_01(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].filterunit[filter_index_0_based]
             .envmod,
        normalized_01);
#else
    (void)scene_index_0_based;
    (void)filter_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_envelope_attack(const int scene_index_0_based,
                                  const int envelope_index_0_based,
                                  const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(envelope_index_0_based, 2, "envelope index");
    impl->set_parameter_01(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].adsr[envelope_index_0_based].a,
        normalized_01);
#else
    (void)scene_index_0_based;
    (void)envelope_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_envelope_release(const int scene_index_0_based,
                                   const int envelope_index_0_based,
                                   const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(envelope_index_0_based, 2, "envelope index");
    impl->set_parameter_01(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].adsr[envelope_index_0_based].r,
        normalized_01);
#else
    (void)scene_index_0_based;
    (void)envelope_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_lfo_rate(const int scene_index_0_based,
                           const int lfo_index_0_based,
                           const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(lfo_index_0_based, n_lfos, "LFO index");
    impl->set_parameter_01(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].lfo[lfo_index_0_based].rate,
        normalized_01);
#else
    (void)scene_index_0_based;
    (void)lfo_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_lfo_depth(const int scene_index_0_based,
                            const int lfo_index_0_based,
                            const float normalized_01) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(lfo_index_0_based, n_lfos, "LFO index");
    impl->set_parameter_01(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].lfo[lfo_index_0_based].magnitude,
        normalized_01);
#else
    (void)scene_index_0_based;
    (void)lfo_index_0_based;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::set_scene_filter_cutoff(const int scene_index_0_based,
                                      const int filter_index_0_based,
                                      const float normalized_01) {
    set_filter_cutoff(scene_index_0_based, filter_index_0_based, normalized_01);
}

float SurgeXT::get_parameter_01(const long parameter_id) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->require_parameter(parameter_id)->get_value_f01();
#else
    (void)parameter_id;
    throw runtime_error(availability_message());
#endif
}

float SurgeXT::normalized_to_value(const long parameter_id, const float normalized_01) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->require_parameter(parameter_id)->normalized_to_value(clamp01(normalized_01));
#else
    (void)parameter_id;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

float SurgeXT::value_to_normalized(const long parameter_id, const float value) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->require_parameter(parameter_id)->value_to_normalized(value);
#else
    (void)parameter_id;
    (void)value;
    throw runtime_error(availability_message());
#endif
}

string SurgeXT::get_parameter_name(const long parameter_id) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->parameter_name(parameter_id);
#else
    (void)parameter_id;
    throw runtime_error(availability_message());
#endif
}

string SurgeXT::get_parameter_display(const long parameter_id) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->parameter_display(parameter_id);
#else
    (void)parameter_id;
    throw runtime_error(availability_message());
#endif
}

string SurgeXT::get_parameter_display_for_normalized(const long parameter_id,
                                                     const float normalized_01) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    return impl->parameter_display_for_normalized(parameter_id, normalized_01);
#else
    (void)parameter_id;
    (void)normalized_01;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::scene_volume_parameter_id(const int scene_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    return impl->parameter_id_for(&impl->synth->storage.getPatch().scene[scene_index_0_based].volume);
#else
    (void)scene_index_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::scene_pan_parameter_id(const int scene_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    return impl->parameter_id_for(&impl->synth->storage.getPatch().scene[scene_index_0_based].pan);
#else
    (void)scene_index_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::scene_width_parameter_id(const int scene_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    return impl->parameter_id_for(&impl->synth->storage.getPatch().scene[scene_index_0_based].width);
#else
    (void)scene_index_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::waveshaper_drive_parameter_id(const int scene_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    return impl->parameter_id_for(&impl->synth->storage.getPatch().scene[scene_index_0_based].wsunit.drive);
#else
    (void)scene_index_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::scene_send_level_parameter_id(const int scene_index_0_based,
                                            const int send_slot_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(send_slot_0_based, n_send_slots, "send slot");
    return impl->parameter_id_for(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].send_level[send_slot_0_based]);
#else
    (void)scene_index_0_based;
    (void)send_slot_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::fx_return_level_parameter_id(const int fx_slot_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(fx_slot_0_based, n_fx_slots, "FX slot");
    return impl->parameter_id_for(&impl->synth->storage.getPatch().fx[fx_slot_0_based].return_level);
#else
    (void)fx_slot_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::filter_cutoff_parameter_id(const int scene_index_0_based,
                                         const int filter_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(filter_index_0_based, n_filterunits_per_scene, "filter index");
    return impl->parameter_id_for(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].filterunit[filter_index_0_based]
             .cutoff);
#else
    (void)scene_index_0_based;
    (void)filter_index_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::filter_resonance_parameter_id(const int scene_index_0_based,
                                            const int filter_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(filter_index_0_based, n_filterunits_per_scene, "filter index");
    return impl->parameter_id_for(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].filterunit[filter_index_0_based]
             .resonance);
#else
    (void)scene_index_0_based;
    (void)filter_index_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::filter_env_amount_parameter_id(const int scene_index_0_based,
                                             const int filter_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(filter_index_0_based, n_filterunits_per_scene, "filter index");
    return impl->parameter_id_for(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].filterunit[filter_index_0_based]
             .envmod);
#else
    (void)scene_index_0_based;
    (void)filter_index_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::envelope_attack_parameter_id(const int scene_index_0_based,
                                           const int envelope_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(envelope_index_0_based, 2, "envelope index");
    return impl->parameter_id_for(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].adsr[envelope_index_0_based].a);
#else
    (void)scene_index_0_based;
    (void)envelope_index_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::envelope_release_parameter_id(const int scene_index_0_based,
                                            const int envelope_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(envelope_index_0_based, 2, "envelope index");
    return impl->parameter_id_for(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].adsr[envelope_index_0_based].r);
#else
    (void)scene_index_0_based;
    (void)envelope_index_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::lfo_rate_parameter_id(const int scene_index_0_based,
                                    const int lfo_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(lfo_index_0_based, n_lfos, "LFO index");
    return impl->parameter_id_for(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].lfo[lfo_index_0_based].rate);
#else
    (void)scene_index_0_based;
    (void)lfo_index_0_based;
    throw runtime_error(availability_message());
#endif
}

long SurgeXT::lfo_depth_parameter_id(const int scene_index_0_based,
                                     const int lfo_index_0_based) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(scene_index_0_based, n_scenes, "scene index");
    require_range(lfo_index_0_based, n_lfos, "LFO index");
    return impl->parameter_id_for(
        &impl->synth->storage.getPatch().scene[scene_index_0_based].lfo[lfo_index_0_based].magnitude);
#else
    (void)scene_index_0_based;
    (void)lfo_index_0_based;
    throw runtime_error(availability_message());
#endif
}

bool SurgeXT::set_modulation_depth_01(const long destination_parameter_id,
                                      const SurgeXTModulationSource mod_source,
                                      const float normalized_01,
                                      const int source_scene_0_based,
                                      const int source_index) {
#ifdef SWAPTUBE_USE_SURGE_XT
    return set_modulation_depth_01(destination_parameter_id,
                                   static_cast<int>(to_surge_mod_source(mod_source)),
                                   normalized_01, source_scene_0_based, source_index);
#else
    (void)destination_parameter_id;
    (void)mod_source;
    (void)normalized_01;
    (void)source_scene_0_based;
    (void)source_index;
    throw runtime_error(availability_message());
#endif
}

bool SurgeXT::set_modulation_depth_01(const long destination_parameter_id,
                                      const int surge_mod_source_id,
                                      const float normalized_01,
                                      const int source_scene_0_based,
                                      const int source_index) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(source_scene_0_based, n_scenes, "source scene index");
    const bool success = impl->synth->setModDepth01(destination_parameter_id,
                                                    require_mod_source_id(surge_mod_source_id),
                                                    source_scene_0_based, source_index,
                                                    clamp01(normalized_01));
    impl->synth->processAudioThreadOpsWhenAudioEngineUnavailable(true);
    return success;
#else
    (void)destination_parameter_id;
    (void)surge_mod_source_id;
    (void)normalized_01;
    (void)source_scene_0_based;
    (void)source_index;
    throw runtime_error(availability_message());
#endif
}

bool SurgeXT::set_modulation_depth_bipolar(const long destination_parameter_id,
                                           const SurgeXTModulationSource mod_source,
                                           const float normalized_bipolar,
                                           const int source_scene_0_based,
                                           const int source_index) {
#ifdef SWAPTUBE_USE_SURGE_XT
    return set_modulation_depth_bipolar(destination_parameter_id,
                                        static_cast<int>(to_surge_mod_source(mod_source)),
                                        normalized_bipolar, source_scene_0_based, source_index);
#else
    (void)destination_parameter_id;
    (void)mod_source;
    (void)normalized_bipolar;
    (void)source_scene_0_based;
    (void)source_index;
    throw runtime_error(availability_message());
#endif
}

bool SurgeXT::set_modulation_depth_bipolar(const long destination_parameter_id,
                                           const int surge_mod_source_id,
                                           const float normalized_bipolar,
                                           const int source_scene_0_based,
                                           const int source_index) {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(source_scene_0_based, n_scenes, "source scene index");
    const modsources surge_mod_source = require_mod_source_id(surge_mod_source_id);
    const float clamped_depth = clamp_bipolar(normalized_bipolar);
    const float magnitude = std::abs(clamped_depth);
    const bool success = impl->synth->setModDepth01(destination_parameter_id, surge_mod_source,
                                                    source_scene_0_based, source_index, magnitude);
    if (success && magnitude > 0.0f) {
        auto* routing = impl->synth->getModRouting(destination_parameter_id, surge_mod_source,
                                                   source_scene_0_based, source_index);
        if (routing) {
            const Parameter* parameter =
                impl->synth->storage.getPatch().param_ptr[destination_parameter_id];
            routing->depth = std::copysign(parameter->set_modulation_f01(magnitude), clamped_depth);
            impl->synth->storage.getPatch().isDirty = true;
        }
    }
    impl->synth->processAudioThreadOpsWhenAudioEngineUnavailable(true);
    return success;
#else
    (void)destination_parameter_id;
    (void)surge_mod_source_id;
    (void)normalized_bipolar;
    (void)source_scene_0_based;
    (void)source_index;
    throw runtime_error(availability_message());
#endif
}

float SurgeXT::get_modulation_depth_01(const long destination_parameter_id,
                                       const int surge_mod_source_id,
                                       const int source_scene_0_based,
                                       const int source_index) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(source_scene_0_based, n_scenes, "source scene index");
    return impl->synth->getModDepth01(destination_parameter_id,
                                      require_mod_source_id(surge_mod_source_id),
                                      source_scene_0_based, source_index);
#else
    (void)destination_parameter_id;
    (void)surge_mod_source_id;
    (void)source_scene_0_based;
    (void)source_index;
    throw runtime_error(availability_message());
#endif
}

float SurgeXT::get_modulation_depth_bipolar(const long destination_parameter_id,
                                            const int surge_mod_source_id,
                                            const int source_scene_0_based,
                                            const int source_index) const {
#ifdef SWAPTUBE_USE_SURGE_XT
    require_range(source_scene_0_based, n_scenes, "source scene index");
    return impl->require_parameter(destination_parameter_id)->get_modulation_f01(
        impl->synth->getModDepth(destination_parameter_id, require_mod_source_id(surge_mod_source_id),
                                 source_scene_0_based, source_index));
#else
    (void)destination_parameter_id;
    (void)surge_mod_source_id;
    (void)source_scene_0_based;
    (void)source_index;
    throw runtime_error(availability_message());
#endif
}

void SurgeXT::render_samples(const int num_samples, vector<sample_t>& left, vector<sample_t>& right) {
    if (num_samples < 0) {
        throw runtime_error("render_samples requires a non-negative sample count.");
    }

    left.clear();
    right.clear();
    left.reserve(num_samples);
    right.reserve(num_samples);

#ifdef SWAPTUBE_USE_SURGE_XT
    const int block_size = impl->synth->getBlockSize();
    int remaining = num_samples;
    while (remaining > 0) {
        impl->synth->process();
        const int write_count = std::min(block_size, remaining);
        for (int i = 0; i < write_count; ++i) {
            left.push_back(float_to_sample(impl->synth->output[0][i]));
            right.push_back(float_to_sample(impl->synth->output[1][i]));
        }
        remaining -= write_count;
    }
#else
    (void)num_samples;
    throw runtime_error(availability_message());
#endif
}
