/* Lightweight RNNoise-compatible fallback.
 *
 * This file exists only as an offline fallback when the real RNNoise library
 * is disabled in CMake. The preferred path is the official RNNoise release.
 */

#include "rnnoise_wrapper.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define FRAME_SIZE 480

struct DenoiseState {
    float previous_sample;
    int initialized;
};

struct RNNModel {
    int unused;
};

static void simple_noise_gate(float* out, const float* in, int frame_size, float* previous_sample) {
    float energy = 0.0f;
    for (int i = 0; i < frame_size; ++i) {
        float const sample = in[i] / 32768.0f;
        energy += sample * sample;
    }

    energy = sqrtf(energy / frame_size);
    float const gate = (energy > 0.015f) ? 1.0f : 0.1f;

    for (int i = 0; i < frame_size; ++i) {
        float processed = in[i] * gate;
        float const current = processed;
        processed = 0.9f * processed + 0.1f * (current - *previous_sample);
        *previous_sample = current;
        out[i] = processed;
    }
}

int rnnoise_get_frame_size(void) {
    return FRAME_SIZE;
}

DenoiseState* rnnoise_create(RNNModel* model) {
    (void)model;
    DenoiseState* st = (DenoiseState*)malloc(sizeof(struct DenoiseState));
    if (!st) {
        return NULL;
    }

    memset(st, 0, sizeof(struct DenoiseState));
    st->initialized = 1;
    return st;
}

void rnnoise_destroy(DenoiseState* st) {
    if (st) {
        free(st);
    }
}

float rnnoise_process_frame(DenoiseState* st, float* out, const float* in) {
    if (!st || !st->initialized || !out || !in) {
        return 0.0f;
    }

    simple_noise_gate(out, in, FRAME_SIZE, &st->previous_sample);
    return 1.0f;
}
