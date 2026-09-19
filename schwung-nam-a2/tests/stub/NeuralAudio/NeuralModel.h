/* A stub NeuralAudio, so the plugin's PARAM PLUMBING can be compiled and run
 * on the build host.
 *
 * The strings this module serves - chain_params and ui_hierarchy - are built
 * by C code at runtime, and a malformed one is not a build error: chain_host
 * silently rejects it and falls back to module.json, which drops `access`
 * and `live` on the way through. That failure mode cost four builds on
 * hardware before the trailing comma that caused it was found. It is a
 * five-minute check on the host and nothing else catches it.
 *
 * Only the members nam_a2c_plugin.cpp actually calls are here. No audio. */
#ifndef A2_TEST_STUB_NEURALMODEL_H
#define A2_TEST_STUB_NEURALMODEL_H

#include <cstddef>

namespace NeuralAudio {

class NeuralModel {
public:
    virtual ~NeuralModel() {}
    void Process(float *in, float *out, size_t n) {
        for (size_t i = 0; i < n; i++) out[i] = in[i];
    }
    bool IsQualityChangeRealtimeSafe(float) { return true; }
    void SetQualityScaleFactor(float) {}
    float GetRecommendedInputDBAdjustment() { return 0.0f; }
    float GetRecommendedOutputDBAdjustment() { return 0.0f; }
};

class NeuralModelLoader {
public:
    NeuralModel *CreateFromFile(const char *) { return new NeuralModel(); }
};

}  /* namespace NeuralAudio */

#endif
