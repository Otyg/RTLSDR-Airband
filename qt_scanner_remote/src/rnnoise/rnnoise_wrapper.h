#ifndef RNNOISE_WRAPPER_H
#define RNNOISE_WRAPPER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef QT_SCANNER_REMOTE_USE_REAL_RNNOISE

#include <rnnoise.h>

#else

typedef struct DenoiseState DenoiseState;
typedef struct RNNModel RNNModel;

int rnnoise_get_frame_size(void);

DenoiseState* rnnoise_create(RNNModel* model);

void rnnoise_destroy(DenoiseState* st);

float rnnoise_process_frame(DenoiseState* st, float* out, const float* in);

#endif

#ifdef __cplusplus
}
#endif

#endif
