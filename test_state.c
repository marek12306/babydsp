// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 deepivin
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_MODE
#include "babydsp.c"

#define FRAMES 128

static void init_instance(babydsp_info_t *dsp)
{
    memset(dsp, 0, sizeof(*dsp));
    dsp->ext.private_data = dsp;
    babydsp_init(&dsp->ext);
}

static void process(babydsp_info_t *dsp, const int16_t input[FRAMES][2],
                    int16_t output[FRAMES][2])
{
    snd_pcm_channel_area_t src[2] = {
        { .addr = (void *)input, .first = 0, .step = 32 },
        { .addr = (void *)input, .first = 16, .step = 32 },
    };
    snd_pcm_channel_area_t dst[2] = {
        { .addr = output, .first = 0, .step = 32 },
        { .addr = output, .first = 16, .step = 32 },
    };

    memset(output, 0, sizeof(int16_t) * FRAMES * 2);
    if (babydsp_transfer(&dsp->ext, dst, 0, src, 0, FRAMES) != FRAMES) {
        fprintf(stderr, "transfer failed\n");
        exit(2);
    }
}

static int outputs_equal(const int16_t a[FRAMES][2],
                         const int16_t b[FRAMES][2])
{
    return memcmp(a, b, sizeof(int16_t) * FRAMES * 2) == 0;
}

int main(void)
{
    babydsp_info_t reference;
    babydsp_info_t first;
    babydsp_info_t second;
    int16_t silence[FRAMES][2] = {{0}};
    int16_t impulse[FRAMES][2] = {{0}};
    int16_t expected[FRAMES][2];
    int16_t scratch[FRAMES][2];
    int16_t actual[FRAMES][2];
    int failures = 0;

    impulse[0][0] = 12000;
    impulse[0][1] = 12000;

    init_instance(&reference);
    process(&reference, silence, expected);

    init_instance(&first);
    init_instance(&second);
    process(&first, impulse, scratch);
    process(&second, silence, actual);
    if (!outputs_equal(expected, actual)) {
        fprintf(stderr,
                "FAIL: one PCM instance changes another instance's filter history\n");
        failures++;
    }

    process(&first, impulse, scratch);
    babydsp_init(&first.ext);
    process(&first, silence, actual);
    if (!outputs_equal(expected, actual)) {
        fprintf(stderr,
                "FAIL: init does not reset the instance's filter history\n");
        failures++;
    }

    if (failures == 0)
        puts("PASS: filter state is per-instance and reset by init");
    return failures != 0;
}
