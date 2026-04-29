#include <assert.h>
#include <math.h>
#include <string.h>

#include "nnet.h"
#include "os_support.h"
#include "rade_constants.h"

#define RADE_NNET_MAX_ACTIVATIONS 4096
#define RADE_NNET_MAX_INPUTS 2048
#define SPARSE_BLOCK_SIZE 32

static float rade_sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

void compute_activation_c(float *output, const float *input, int N, int activation) {
    assert(output != NULL);
    assert(input != NULL);
    assert(N >= 0);
    assert(N <= RADE_NNET_MAX_ACTIVATIONS);

    if (activation == ACTIVATION_LINEAR) {
        if (output != input) {
            OPUS_COPY(output, input, N);
        }
        return;
    }

    if (activation == ACTIVATION_SOFTMAX) {
        float max_val = input[0];
        float sum = 0.0f;
        for (int i = 1; i < N; i++) {
            if (input[i] > max_val) {
                max_val = input[i];
            }
        }
        for (int i = 0; i < N; i++) {
            output[i] = expf(input[i] - max_val);
            sum += output[i];
        }
        if (sum > 0.0f) {
            float inv = 1.0f / sum;
            for (int i = 0; i < N; i++) {
                output[i] *= inv;
            }
        }
        return;
    }

    for (int i = 0; i < N; i++) {
        float x = input[i];
        switch (activation) {
        case ACTIVATION_SIGMOID:
            output[i] = rade_sigmoid(x);
            break;
        case ACTIVATION_TANH:
            output[i] = tanhf(x);
            break;
        case ACTIVATION_RELU:
            output[i] = x < 0.0f ? 0.0f : x;
            break;
        case ACTIVATION_SWISH:
            output[i] = x * rade_sigmoid(x);
            break;
        case ACTIVATION_EXP:
            output[i] = expf(x);
            break;
        default:
            assert(!"unsupported activation");
        }
    }
}

static void compute_linear_dense_float(const LinearLayer *linear, float *out, const float *in) {
    int rows = linear->nb_outputs;
    int cols = linear->nb_inputs;
    const float *weights = linear->float_weights;

    for (int i = 0; i < rows; i++) {
        out[i] = 0.0f;
    }
    for (int j = 0; j < cols; j++) {
        const float *col = &weights[j * rows];
        float in_j = in[j];
        for (int i = 0; i < rows; i++) {
            out[i] += col[i] * in_j;
        }
    }
}

static void compute_linear_sparse_float(const LinearLayer *linear, float *out, const float *in) {
    int rows = linear->nb_outputs;
    const float *weights = linear->float_weights;
    const int *idx = linear->weights_idx;

    assert((rows % 8) == 0);
    for (int i = 0; i < rows; i++) {
        out[i] = 0.0f;
    }

    for (int row = 0; row < rows; row += 8) {
        int col_blocks = *idx++;
        for (int block = 0; block < col_blocks; block++) {
            int pos = *idx++;
            for (int lane = 0; lane < 4; lane++) {
                float x = in[pos + lane];
                const float *w_lane = &weights[lane * 8];
                for (int out_lane = 0; out_lane < 8; out_lane++) {
                    out[row + out_lane] += w_lane[out_lane] * x;
                }
            }
            weights += SPARSE_BLOCK_SIZE;
        }
    }
}

void compute_linear_c(const LinearLayer *linear, float *out, const float *in) {
    int M;
    int N;

    assert(linear != NULL);
    assert(out != NULL);
    assert(in != NULL);
    assert(out != in);

    M = linear->nb_inputs;
    N = linear->nb_outputs;

    if (linear->float_weights != NULL) {
        if (linear->weights_idx != NULL) {
            compute_linear_sparse_float(linear, out, in);
        } else {
            compute_linear_dense_float(linear, out, in);
        }
    } else {
        OPUS_CLEAR(out, N);
    }

    if (linear->bias != NULL) {
        for (int i = 0; i < N; i++) {
            out[i] += linear->bias[i];
        }
    }
    if (linear->diag != NULL) {
        assert(3 * M == N);
        for (int i = 0; i < M; i++) {
            out[i] += linear->diag[i] * in[i];
            out[i + M] += linear->diag[i + M] * in[i];
            out[i + 2 * M] += linear->diag[i + 2 * M] * in[i];
        }
    }
}

void compute_generic_dense(const LinearLayer *layer, float *output, const float *input, int activation, int arch) {
    (void)arch;
    compute_linear_c(layer, output, input);
    compute_activation_c(output, output, layer->nb_outputs, activation);
}

void compute_generic_gru(const LinearLayer *input_weights, const LinearLayer *recurrent_weights, float *state, const float *in, int arch) {
    int N;
    float zrh[3 * RADE_MAX_RNN_NEURONS];
    float recur[3 * RADE_MAX_RNN_NEURONS];
    float *z;
    float *r;
    float *h;

    (void)arch;
    assert(input_weights != NULL);
    assert(recurrent_weights != NULL);
    assert(state != NULL);
    assert(in != NULL);
    assert(3 * recurrent_weights->nb_inputs == recurrent_weights->nb_outputs);
    assert(input_weights->nb_outputs == recurrent_weights->nb_outputs);
    assert(in != state);

    N = recurrent_weights->nb_inputs;
    assert(N <= RADE_MAX_RNN_NEURONS);

    z = zrh;
    r = &zrh[N];
    h = &zrh[2 * N];

    compute_linear_c(input_weights, zrh, in);
    compute_linear_c(recurrent_weights, recur, state);
    for (int i = 0; i < 2 * N; i++) {
        zrh[i] += recur[i];
    }
    compute_activation_c(zrh, zrh, 2 * N, ACTIVATION_SIGMOID);
    for (int i = 0; i < N; i++) {
        h[i] += recur[2 * N + i] * r[i];
    }
    compute_activation_c(h, h, N, ACTIVATION_TANH);
    for (int i = 0; i < N; i++) {
        h[i] = z[i] * state[i] + (1.0f - z[i]) * h[i];
        state[i] = h[i];
    }
}

void compute_glu(const LinearLayer *layer, float *output, const float *input, int arch) {
    float act[RADE_NNET_MAX_INPUTS];

    (void)arch;
    assert(layer != NULL);
    assert(output != NULL);
    assert(input != NULL);
    assert(layer->nb_inputs == layer->nb_outputs);
    assert(layer->nb_outputs <= RADE_NNET_MAX_INPUTS);

    compute_linear_c(layer, act, input);
    compute_activation_c(act, act, layer->nb_outputs, ACTIVATION_SIGMOID);
    if (output == input) {
        for (int i = 0; i < layer->nb_outputs; i++) {
            output[i] *= act[i];
        }
    } else {
        for (int i = 0; i < layer->nb_outputs; i++) {
            output[i] = input[i] * act[i];
        }
    }
}

void compute_gated_activation(const LinearLayer *layer, float *output, const float *input, int activation, int arch) {
    float gate[RADE_NNET_MAX_INPUTS];

    (void)arch;
    assert(layer != NULL);
    assert(output != NULL);
    assert(input != NULL);
    assert(layer->nb_outputs <= RADE_NNET_MAX_INPUTS);

    compute_linear_c(layer, gate, input);
    compute_activation_c(gate, gate, layer->nb_outputs, activation);
    if (output == input) {
        for (int i = 0; i < layer->nb_outputs; i++) {
            output[i] *= gate[i];
        }
    } else {
        for (int i = 0; i < layer->nb_outputs; i++) {
            output[i] = input[i] * gate[i];
        }
    }
}

void compute_generic_conv1d(const LinearLayer *layer, float *output, float *mem, const float *input, int input_size, int activation, int arch) {
    float tmp[RADE_MAX_CONV_INPUTS];

    (void)arch;
    assert(layer != NULL);
    assert(output != NULL);
    assert(mem != NULL);
    assert(input != NULL);
    assert(input != output);
    assert(layer->nb_inputs <= RADE_MAX_CONV_INPUTS);

    if (layer->nb_inputs != input_size) {
        OPUS_COPY(tmp, mem, layer->nb_inputs - input_size);
    }
    OPUS_COPY(&tmp[layer->nb_inputs - input_size], input, input_size);
    compute_linear_c(layer, output, tmp);
    compute_activation_c(output, output, layer->nb_outputs, activation);
    if (layer->nb_inputs != input_size) {
        OPUS_COPY(mem, &tmp[input_size], layer->nb_inputs - input_size);
    }
}

void compute_generic_conv1d_dilation(const LinearLayer *layer, float *output, float *mem, const float *input, int input_size, int dilation, int activation, int arch) {
    float tmp[RADE_MAX_CONV_INPUTS];
    int ksize;

    (void)arch;
    assert(layer != NULL);
    assert(output != NULL);
    assert(mem != NULL);
    assert(input != NULL);
    assert(input != output);
    assert(layer->nb_inputs <= RADE_MAX_CONV_INPUTS);

    ksize = layer->nb_inputs / input_size;
    if (dilation == 1) {
        OPUS_COPY(tmp, mem, layer->nb_inputs - input_size);
    } else {
        for (int i = 0; i < ksize - 1; i++) {
            OPUS_COPY(&tmp[i * input_size], &mem[i * input_size * dilation], input_size);
        }
    }
    OPUS_COPY(&tmp[layer->nb_inputs - input_size], input, input_size);
    compute_linear_c(layer, output, tmp);
    compute_activation_c(output, output, layer->nb_outputs, activation);
    if (dilation == 1) {
        OPUS_COPY(mem, &tmp[input_size], layer->nb_inputs - input_size);
    } else {
        OPUS_COPY(mem, &mem[input_size], input_size * dilation * (ksize - 1) - input_size);
        OPUS_COPY(&mem[input_size * dilation * (ksize - 1) - input_size], input, input_size);
    }
}

static const WeightArray *find_array_entry(const WeightArray *arrays, const char *name) {
    while (arrays->name && strcmp(arrays->name, name) != 0) {
        arrays++;
    }
    return arrays;
}

static const void *find_array_check(const WeightArray *arrays, const char *name, int size) {
    const WeightArray *a = find_array_entry(arrays, name);
    if (a->name && a->size == size) {
        return a->data;
    }
    return NULL;
}

static const void *opt_array_check(const WeightArray *arrays, const char *name, int size, int *error) {
    const WeightArray *a = find_array_entry(arrays, name);
    *error = (a->name != NULL && a->size != size);
    if (a->name && a->size == size) {
        return a->data;
    }
    return NULL;
}

static const void *find_idx_check(const WeightArray *arrays, const char *name, int nb_in, int nb_out, int *total_blocks) {
    int remain;
    const int *idx;
    const WeightArray *a = find_array_entry(arrays, name);

    *total_blocks = 0;
    if (a == NULL) {
        return NULL;
    }
    idx = (const int *)a->data;
    remain = a->size / (int)sizeof(int);
    while (remain > 0) {
        int nb_blocks = *idx++;
        if (remain < nb_blocks + 1) {
            return NULL;
        }
        for (int i = 0; i < nb_blocks; i++) {
            int pos = *idx++;
            if (pos + 3 >= nb_in || (pos & 0x3)) {
                return NULL;
            }
        }
        nb_out -= 8;
        remain -= nb_blocks + 1;
        *total_blocks += nb_blocks;
    }
    if (nb_out != 0) {
        return NULL;
    }
    return a->data;
}

int linear_init(LinearLayer *layer, const WeightArray *arrays,
  const char *bias,
  const char *subias,
  const char *weights,
  const char *float_weights,
  const char *weights_idx,
  const char *diag,
  const char *scale,
  int nb_inputs,
  int nb_outputs) {
    int err;

    layer->bias = NULL;
    layer->subias = NULL;
    layer->weights = NULL;
    layer->float_weights = NULL;
    layer->weights_idx = NULL;
    layer->diag = NULL;
    layer->scale = NULL;
    if (bias != NULL) {
        if ((layer->bias = find_array_check(arrays, bias, nb_outputs * (int)sizeof(layer->bias[0]))) == NULL) return 1;
    }
    if (subias != NULL) {
        if ((layer->subias = find_array_check(arrays, subias, nb_outputs * (int)sizeof(layer->subias[0]))) == NULL) return 1;
    }
    if (weights_idx != NULL) {
        int total_blocks;
        if ((layer->weights_idx = find_idx_check(arrays, weights_idx, nb_inputs, nb_outputs, &total_blocks)) == NULL) return 1;
        if (weights != NULL) {
            if ((layer->weights = find_array_check(arrays, weights, SPARSE_BLOCK_SIZE * total_blocks * (int)sizeof(layer->weights[0]))) == NULL) return 1;
        }
        if (float_weights != NULL) {
            layer->float_weights = opt_array_check(arrays, float_weights, SPARSE_BLOCK_SIZE * total_blocks * (int)sizeof(layer->float_weights[0]), &err);
            if (err) return 1;
        }
    } else {
        if (weights != NULL) {
            if ((layer->weights = find_array_check(arrays, weights, nb_inputs * nb_outputs * (int)sizeof(layer->weights[0]))) == NULL) return 1;
        }
        if (float_weights != NULL) {
            layer->float_weights = opt_array_check(arrays, float_weights, nb_inputs * nb_outputs * (int)sizeof(layer->float_weights[0]), &err);
            if (err) return 1;
        }
    }
    if (diag != NULL) {
        if ((layer->diag = find_array_check(arrays, diag, nb_outputs * (int)sizeof(layer->diag[0]))) == NULL) return 1;
    }
    if (weights != NULL) {
        if ((layer->scale = find_array_check(arrays, scale, nb_outputs * (int)sizeof(layer->scale[0]))) == NULL) return 1;
    }
    layer->nb_inputs = nb_inputs;
    layer->nb_outputs = nb_outputs;
    return 0;
}
