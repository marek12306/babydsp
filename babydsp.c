// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 deepivin
/*
 * babydsp: ALSA external PCM plugin of the Logitech Squeezebox Radio.
 *
 * Reconstruction of SqueezeOS's libasound_module_pcm_babydsp.so: the
 * stereo input is summed to mono, equalised and split by a ~2 kHz
 * crossover (at 44.1 kHz) into a tweeter signal on the left output and a
 * woofer signal on the right output.
 * Six Q30 Direct Form I biquads: BEQ 1, BEQ 2, bass 1, bass 2, then
 * tweeter and woofer. An optional ALSA control (1 = on) bypasses the
 * filters when it is 0.
 *
 * Unlike the original, each PCM instance has its own filter history and
 * the extplug init callback resets it.
 *
 * Build with -DBABYDSP_DEBUG_STATS to publish per-instance peak levels in
 * /tmp/babydsp-stats.<pid>.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <alsa/asoundlib.h>
#include <alsa/pcm_external.h>
#ifdef BABYDSP_DEBUG_STATS
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

typedef int32_t s32;
typedef int64_t s64;

#define BABYDSP_EXTPLUG_VERSION_1_0_1 ((1U << 16) | 1U)

typedef struct {
    s32 b[3];
    s32 a[2];
    s32 x[2];
    s32 y[2];
} bq_1_t;

typedef struct {
    bq_1_t tweeter;
    bq_1_t woofer;
    bq_1_t bass_1;
    bq_1_t bass_2;
    bq_1_t beq_1;
    bq_1_t beq_2;
} babydsp_filters_t;

#ifdef BABYDSP_DEBUG_STATS
typedef struct {
    uint32_t sequence;
    uint32_t instance;
    uint32_t frames;
    s32 input_peak;
    s32 tweeter_peak;
    s32 woofer_peak;
} babydsp_debug_stats_t;

#endif

typedef struct babydsp_info {
    snd_pcm_extplug_t ext;
    snd_ctl_t *ctl;
    snd_ctl_elem_value_t *enable_value;
    babydsp_filters_t filters;
#ifdef BABYDSP_DEBUG_STATS
    babydsp_debug_stats_t *debug_stats;
#endif
#ifdef BABYDSP_DEBUG_STATS
    uint32_t debug_frames;
    s32 debug_input_peak;
    s32 debug_tweeter_peak;
    s32 debug_woofer_peak;
#endif
} babydsp_info_t;

static const babydsp_filters_t babydsp_initial_filters = {
    .tweeter = {
        { -821234486, 1642468972, -821234486 },
        { 1608671439, -602524681 }, { 0, 0 }, { 0, 0 }
    },
    .woofer = {
        { 16898766, 33797533, 16898766 },
        { 1608671439, -602524681 }, { 0, 0 }, { 0, 0 }
    },
    .bass_1 = {
        { 1075078144, -2130062720, 1055350016 },
        { 2130176256, -1056572800 }, { 0, 0 }, { 0, 0 }
    },
    .bass_2 = {
        { 1065122560, -2130245248, 1065122560 },
        { 2130176000, -1056572544 }, { 0, 0 }, { 0, 0 }
    },
    .beq_1 = {
        { 1009767855, -1424545963, 791013492 },
        { 1424545963, -727039523 }, { 0, 0 }, { 0, 0 }
    },
    .beq_2 = {
        { 1175744322, -387744644, 622949156 },
        { 387744644, -724951654 }, { 0, 0 }, { 0, 0 }
    },
};

static void babydsp_reset_filters(babydsp_info_t *dsp)
{
    dsp->filters = babydsp_initial_filters;
}

void bq_1_asm(const s32 *src, s32 *dst, int count, bq_1_t *biquad)
{
    s32 x1 = biquad->x[0];
    s32 x2 = biquad->x[1];
    s32 y1 = biquad->y[0];
    s32 y2 = biquad->y[1];
    const s32 b0 = biquad->b[0];
    const s32 b1 = biquad->b[1];
    const s32 b2 = biquad->b[2];
    const s32 a1 = biquad->a[0];
    const s32 a2 = biquad->a[1];
    int i;

    for (i = 0; i < count; ++i) {
        const s32 x0 = src[i];
        s64 acc = 0x20000000LL;
        s32 out;

        acc += (s64)x0 * b0;
        acc += (s64)x1 * b1;
        acc += (s64)x2 * b2;
        acc += (s64)y1 * a1;
        acc += (s64)y2 * a2;
        out = (s32)(acc >> 30);
        dst[i] = out;

        x2 = x1;
        x1 = x0;
        y2 = y1;
        y1 = out;
    }

    biquad->x[0] = x1;
    biquad->x[1] = x2;
    biquad->y[0] = y1;
    biquad->y[1] = y2;
}

static inline int16_t *area_addr(const snd_pcm_channel_area_t *area,
                                 snd_pcm_uframes_t offset)
{
    unsigned int bit_offset = area->first + area->step * offset;
    return (int16_t *)((char *)area->addr + bit_offset / 8);
}

static inline unsigned int area_step(const snd_pcm_channel_area_t *area)
{
    return area->step / 16;
}

static int babydsp_is_crossover_enabled(babydsp_info_t *dsp)
{
    int err;

    if (dsp->ctl == NULL)
        return 1;
    err = snd_ctl_elem_read(dsp->ctl, dsp->enable_value);
    if (err < 0)
        return 1;
    return snd_ctl_elem_value_get_boolean(dsp->enable_value, 0) != 0;
}

#ifdef BABYDSP_DEBUG_STATS
static inline s32 babydsp_abs(s32 value)
{
    return value < 0 ? -value : value;
}

static void babydsp_debug_open(babydsp_info_t *dsp)
{
    char path[64];
    int fd;

    snprintf(path, sizeof(path), "/tmp/babydsp-stats.%ld", (long)getpid());
    fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return;
    if (ftruncate(fd, sizeof(*dsp->debug_stats)) < 0) {
        close(fd);
        return;
    }
    dsp->debug_stats = mmap(NULL, sizeof(*dsp->debug_stats),
                            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (dsp->debug_stats == MAP_FAILED)
        dsp->debug_stats = NULL;
}

static void babydsp_publish_debug(babydsp_info_t *dsp)
{
    if (dsp->debug_frames < 4096 || dsp->debug_stats == NULL)
        return;

    dsp->debug_stats->sequence++;
    __asm__ volatile("" ::: "memory");
    dsp->debug_stats->instance = (uint32_t)(uintptr_t)dsp;
    dsp->debug_stats->frames = dsp->debug_frames;
    dsp->debug_stats->input_peak = dsp->debug_input_peak;
    dsp->debug_stats->tweeter_peak = dsp->debug_tweeter_peak;
    dsp->debug_stats->woofer_peak = dsp->debug_woofer_peak;
    __asm__ volatile("" ::: "memory");
    dsp->debug_stats->sequence++;

    dsp->debug_frames = 0;
    dsp->debug_input_peak = 0;
    dsp->debug_tweeter_peak = 0;
    dsp->debug_woofer_peak = 0;
}
#endif

static snd_pcm_sframes_t babydsp_transfer(
    snd_pcm_extplug_t *ext,
    const snd_pcm_channel_area_t *dst_areas,
    snd_pcm_uframes_t dst_offset,
    const snd_pcm_channel_area_t *src_areas,
    snd_pcm_uframes_t src_offset,
    snd_pcm_uframes_t size)
{
    babydsp_info_t *dsp = ext->private_data;
    int16_t *src[2];
    int16_t *dst[2];
    unsigned int src_step[2];
    unsigned int dst_step[2];
    snd_pcm_uframes_t i;
    s32 l_out[size];
    s32 r_out[size];
    s32 mono[size];

    if (!babydsp_is_crossover_enabled(dsp)) {
        snd_pcm_areas_copy(dst_areas, dst_offset, src_areas, src_offset,
                           2, size, SND_PCM_FORMAT_S16);
        return size;
    }

    src[0] = area_addr(&src_areas[0], src_offset);
    src[1] = area_addr(&src_areas[1], src_offset);
    src_step[0] = area_step(&src_areas[0]);
    src_step[1] = area_step(&src_areas[1]);
    dst[0] = area_addr(&dst_areas[0], dst_offset);
    dst[1] = area_addr(&dst_areas[1], dst_offset);
    dst_step[0] = area_step(&dst_areas[0]);
    dst_step[1] = area_step(&dst_areas[1]);

    for (i = 0; i < size; ++i) {
        int in_l = src[0][i * src_step[0]];
        int in_r = src[1][i * src_step[1]];
        mono[i] = in_l * 256 + in_r * 256 + 64;
#ifdef BABYDSP_DEBUG_STATS
        if (babydsp_abs(in_l) > dsp->debug_input_peak)
            dsp->debug_input_peak = babydsp_abs(in_l);
        if (babydsp_abs(in_r) > dsp->debug_input_peak)
            dsp->debug_input_peak = babydsp_abs(in_r);
#endif
    }

    bq_1_asm(mono, l_out, size, &dsp->filters.beq_1);
    bq_1_asm(l_out, mono, size, &dsp->filters.beq_2);
    bq_1_asm(mono, l_out, size, &dsp->filters.bass_1);
    bq_1_asm(l_out, mono, size, &dsp->filters.bass_2);
    bq_1_asm(mono, l_out, size, &dsp->filters.tweeter);
    bq_1_asm(mono, r_out, size, &dsp->filters.woofer);

    for (i = 0; i < size; ++i) {
        int l = l_out[i] >> 8;
        int r = r_out[i] >> 8;

        if (l < -18500)
            l = -18500;
        if (r < -18500)
            r = -18500;
        if (l >= 18500)
            l = 18500;
        if (r >= 18500)
            r = 18500;

        dst[0][i * dst_step[0]] = (int16_t)l;
        dst[1][i * dst_step[1]] = (int16_t)r;
#ifdef BABYDSP_DEBUG_STATS
        if (babydsp_abs(l) > dsp->debug_tweeter_peak)
            dsp->debug_tweeter_peak = babydsp_abs(l);
        if (babydsp_abs(r) > dsp->debug_woofer_peak)
            dsp->debug_woofer_peak = babydsp_abs(r);
#endif
    }
#ifdef BABYDSP_DEBUG_STATS
    dsp->debug_frames += size;
    babydsp_publish_debug(dsp);
#endif

    return size;
}

static int babydsp_hwparams(snd_pcm_extplug_t *ext,
                            snd_pcm_hw_params_t *params)
{
    (void)ext;
    (void)params;
    return 0;
}

static int babydsp_init(snd_pcm_extplug_t *ext)
{
    babydsp_info_t *dsp = ext->private_data;

    babydsp_reset_filters(dsp);
    return 0;
}

static int babydsp_close(snd_pcm_extplug_t *ext)
{
    babydsp_info_t *dsp = ext->private_data;

    if (dsp->enable_value != NULL)
        snd_ctl_elem_value_free(dsp->enable_value);
    if (dsp->ctl != NULL)
        snd_ctl_close(dsp->ctl);
#ifdef BABYDSP_DEBUG_STATS
    if (dsp->debug_stats != NULL)
        munmap(dsp->debug_stats, sizeof(*dsp->debug_stats));
#endif
    return 0;
}

static int babydsp_set_hw_constraint(babydsp_info_t *dsp)
{
    unsigned int in_formats[] = { SND_PCM_FORMAT_S16 };
    unsigned int out_formats[] = { SND_PCM_FORMAT_S16 };
    int err;

    err = snd_pcm_extplug_set_param_list(&dsp->ext,
                                          SND_PCM_EXTPLUG_HW_FORMAT,
                                          1, in_formats);
    if (err < 0)
        return err;
    err = snd_pcm_extplug_set_param_minmax(&dsp->ext,
                                            SND_PCM_EXTPLUG_HW_CHANNELS,
                                            2, 2);
    if (err < 0)
        return err;
    err = snd_pcm_extplug_set_slave_param_list(&dsp->ext,
                                                SND_PCM_EXTPLUG_HW_FORMAT,
                                                1, out_formats);
    if (err < 0)
        return err;
    return snd_pcm_extplug_set_slave_param_minmax(&dsp->ext,
                                                   SND_PCM_EXTPLUG_HW_CHANNELS,
                                                   2, 2);
}

static int babydsp_load_control(snd_pcm_t *pcm, babydsp_info_t *dsp,
                                int card, snd_ctl_elem_id_t *ctl_id)
{
    char tmp_name[32];
    snd_ctl_elem_info_t *cinfo;
    snd_ctl_elem_value_t *value;
    int err;

    (void)pcm;
    snprintf(tmp_name, sizeof(tmp_name), "hw:%d", card);
    err = snd_ctl_open(&dsp->ctl, tmp_name, 0);
    if (err < 0)
        return err;

    snd_ctl_elem_value_malloc(&dsp->enable_value);
    snd_ctl_elem_value_set_id(dsp->enable_value, ctl_id);
    snd_ctl_elem_info_alloca(&cinfo);
    snd_ctl_elem_info_set_id(cinfo, ctl_id);
    err = snd_ctl_elem_info(dsp->ctl, cinfo);
    if (err >= 0)
        return 0;
    if (err != -ENOENT)
        return err;

    err = snd_ctl_elem_add_boolean(dsp->ctl, ctl_id, 1);
    if (err < 0)
        return err;
    snd_ctl_elem_value_alloca(&value);
    snd_ctl_elem_value_set_id(value, ctl_id);
    snd_ctl_elem_value_set_boolean(value, 0, 1);
    err = snd_ctl_elem_write(dsp->ctl, value);
    if (err < 0)
        return err;
    err = snd_ctl_close(dsp->ctl);
    if (err < 0)
        return err;
    dsp->ctl = NULL;
    return snd_ctl_open(&dsp->ctl, tmp_name, 0);
}

static const snd_pcm_extplug_callback_t babydsp_callbacks = {
    .transfer = babydsp_transfer,
    .close = babydsp_close,
    .hw_params = babydsp_hwparams,
    .init = babydsp_init,
};

SND_PCM_PLUGIN_DEFINE_FUNC(babydsp)
{
    snd_config_iterator_t i, next;
    snd_config_t *slave = NULL;
    snd_config_t *control = NULL;
    babydsp_info_t *dsp;
    int err;

    snd_config_for_each(i, next, conf) {
        snd_config_t *entry = snd_config_iterator_entry(i);
        const char *id;

        if (snd_config_get_id(entry, &id) < 0)
            continue;
        if (strcmp(id, "comment") == 0 || strcmp(id, "type") == 0)
            continue;
        if (strcmp(id, "slave") == 0) {
            slave = entry;
            continue;
        }
        if (strcmp(id, "control") == 0) {
            control = entry;
            continue;
        }
        SNDERR("Unknown field %s", id);
        return -EINVAL;
    }
    if (slave == NULL) {
        SNDERR("No slave defined for babydsp");
        return -EINVAL;
    }

    dsp = calloc(1, sizeof(*dsp));
    if (dsp == NULL)
        return -ENOMEM;
    dsp->ext.version = BABYDSP_EXTPLUG_VERSION_1_0_1;
    dsp->ext.name = "Squeezebox Radio Plugin";
    dsp->ext.callback = &babydsp_callbacks;
    dsp->ext.private_data = dsp;
    babydsp_reset_filters(dsp);

    err = snd_pcm_extplug_create(&dsp->ext, name, root, slave, stream, mode);
    if (err < 0)
        goto error;
    err = babydsp_set_hw_constraint(dsp);
    if (err < 0)
        goto delete_extplug;

    if (control != NULL) {
        snd_ctl_elem_id_t *ctl_id;
        int card;

        snd_ctl_elem_id_alloca(&ctl_id);
        err = snd_pcm_parse_control_id(control, ctl_id, &card, NULL, NULL);
        if (err < 0)
            goto delete_extplug;
        err = babydsp_load_control(*pcmp, dsp, card, ctl_id);
        if (err < 0)
            goto delete_extplug;
    }
#ifdef BABYDSP_DEBUG_STATS
    babydsp_debug_open(dsp);
#endif

    *pcmp = dsp->ext.pcm;
    return 0;

delete_extplug:
    snd_pcm_extplug_delete(&dsp->ext);
error:
    free(dsp);
    return err;
}

#ifndef TEST_MODE
SND_PCM_PLUGIN_SYMBOL(babydsp);
#endif
