/*
 * Stateful complex band-pass filter used by the RADE V2 streaming paths.
 *
 * Ports the Python complex_bpf class in radae/dsp.py:
 *   - complex mix to baseband
 *   - real low-pass FIR
 *   - mix back up to centre frequency
 *
 * Copyright (c) 2026 Rhizomatica, BSD-3-Clause
 */

#ifndef COMPLEX_BPF_H
#define COMPLEX_BPF_H

#ifdef __cplusplus
extern "C" {
#endif

#include "comp.h"

struct complex_bpf {
    int ntap;
    int max_len;
    int mem_len;
    float alpha;

    COMP phase;
    COMP *h;
    COMP *mem;
    COMP *x_mem;
    COMP *x_filt;
    COMP *phase_vec_exp;
    COMP *phase_vec;
};

/*
 * Initialise a streaming complex BPF.
 *
 * max_len is the maximum chunk length that will be passed to
 * complex_bpf_process().
 *
 * Returns 0 on success, -1 on allocation/parameter failure.
 */
int complex_bpf_init(struct complex_bpf *bpf, int ntap, float Fs_Hz,
                     float bandwidth_Hz, float centre_freq_Hz, int max_len);

/* Reset streaming state back to the Python constructor state. */
void complex_bpf_reset(struct complex_bpf *bpf);

/* Release all dynamic storage held by the filter. Safe on zeroed structs. */
void complex_bpf_destroy(struct complex_bpf *bpf);

/*
 * Filter n complex samples from x[] into y[].
 *
 * Returns 0 on success, -1 if n exceeds max_len or arguments are invalid.
 */
int complex_bpf_process(struct complex_bpf *bpf, const COMP x[], int n, COMP y[]);

#ifdef __cplusplus
}
#endif

#endif /* COMPLEX_BPF_H */
