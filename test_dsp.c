// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 deepivin
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>

// Include the C file directly to access static functions and structs
#include "babydsp.c"

#pragma pack(push, 1)
typedef struct {
    char riff[4];
    uint32_t overall_size;
    char wave[4];
    char fmt_chunk_marker[4];
    uint32_t length_of_fmt;
    uint16_t format_type;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byterate;
    uint16_t block_align;
    uint16_t bits_per_sample;
    char data_chunk_header[4];
    uint32_t data_size;
} wav_header_t;
#pragma pack(pop)

int main(int argc, char **argv) {
    if (argc < 3) {
        printf("Usage: %s <input.wav> <output.wav>\n", argv[0]);
        return 1;
    }

    FILE *fin = fopen(argv[1], "rb");
    if (!fin) { perror("Input error"); return 1; }

    wav_header_t header;
    fread(&header, sizeof(wav_header_t), 1, fin);

    if (header.channels != 2 || header.bits_per_sample != 16) {
        printf("Error: Input must be 16-bit stereo WAV.\n");
        return 1;
    }

    int num_samples = header.data_size / 2; // total 16-bit samples
    int num_frames = num_samples / 2;       // total stereo frames

    int16_t *in_buf = malloc(header.data_size);
    int16_t *out_buf = malloc(header.data_size);
    fread(in_buf, 2, num_samples, fin);
    fclose(fin);

    // Initialize our DSP
    babydsp_info_t *dsp = calloc(1, sizeof(*dsp));
    dsp->ext.private_data = dsp;
    babydsp_init(&dsp->ext);

    // Set up dummy ALSA channel areas for interleaved 16-bit stereo
    snd_pcm_channel_area_t src_areas[2];
    src_areas[0].addr = in_buf;
    src_areas[0].first = 0;       // Left starts at bit 0
    src_areas[0].step = 32;       // 32 bits to next Left sample
    
    src_areas[1].addr = in_buf;
    src_areas[1].first = 16;      // Right starts at bit 16
    src_areas[1].step = 32;       // 32 bits to next Right sample

    snd_pcm_channel_area_t dst_areas[2];
    dst_areas[0].addr = out_buf;
    dst_areas[0].first = 0;
    dst_areas[0].step = 32;
    
    dst_areas[1].addr = out_buf;
    dst_areas[1].first = 16;
    dst_areas[1].step = 32;

    printf("Processing %d frames...\n", num_frames);
    
    // Run the transfer function!
    babydsp_transfer(&dsp->ext, dst_areas, 0, src_areas, 0, num_frames);

    FILE *fout = fopen(argv[2], "wb");
    fwrite(&header, sizeof(wav_header_t), 1, fout);
    fwrite(out_buf, 2, num_samples, fout);
    fclose(fout);

    babydsp_close(&dsp->ext);
    free(dsp);
    free(in_buf);
    free(out_buf);

    printf("Done! Saved to %s\n", argv[2]);
    printf("Left channel contains Tweeter output (High-pass).\n");
    printf("Right channel contains Woofer output (Low-pass).\n");

    return 0;
}

// Dummy to resolve ALSA plugin symbol linker error in executable
struct snd_dlsym_link *snd_dlsym_start;
