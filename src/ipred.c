/*
 * Copyright © 2018, VideoLAN and dav1d authors
 * Copyright © 2018, Two Orioles, LLC
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "common/attributes.h"
#include "common/intops.h"

#include "src/ipred.h"
#include "src/tables.h"

static NOINLINE void
splat_dc(pixel *dst, const ptrdiff_t stride,
         const int width, const int height, const unsigned dc)
{
    assert(dc <= (1 << BITDEPTH) - 1);
#if BITDEPTH == 8
    if (width > 4) {
        const uint64_t dcN = dc * 0x0101010101010101ULL;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x += sizeof(dcN))
                *((uint64_t *) &dst[x]) = dcN;
            dst += PXSTRIDE(stride);
        }
    } else {
        const unsigned dcN = dc * 0x01010101U;
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x += sizeof(dcN))
                *((unsigned *) &dst[x]) = dcN;
            dst += PXSTRIDE(stride);
        }
    }
#else
    const uint64_t dcN = dc * 0x0001000100010001ULL;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x += sizeof(dcN) >> 1)
            *((uint64_t *) &dst[x]) = dcN;
        dst += PXSTRIDE(stride);
    }
#endif
}

static void ipred_dc_top_c(pixel *dst, const ptrdiff_t stride,
                           const pixel *const topleft,
                           const int width, const int height, const int a)
{
    unsigned dc = width >> 1;
    for (int i = 0; i < width; i++)
       dc += topleft[1 + i];

    splat_dc(dst, stride, width, height, dc >> ctz(width));
}

static void ipred_dc_left_c(pixel *dst, const ptrdiff_t stride,
                            const pixel *const topleft,
                            const int width, const int height, const int a)
{
    unsigned dc = height >> 1;
    for (int i = 0; i < height; i++)
       dc += topleft[-(1 + i)];

    splat_dc(dst, stride, width, height, dc >> ctz(height));
}

#if BITDEPTH == 8
#define MULTIPLIER_1x2 0x5556
#define MULTIPLIER_1x4 0x3334
#define BASE_SHIFT 16
#else
#define MULTIPLIER_1x2 0xAAAB
#define MULTIPLIER_1x4 0x6667
#define BASE_SHIFT 17
#endif

static void ipred_dc_c(pixel *dst, const ptrdiff_t stride,
                       const pixel *const topleft,
                       const int width, const int height, const int a)
{
    unsigned dc = (width + height) >> 1;
    for (int i = 0; i < width; i++)
       dc += topleft[i + 1];
    for (int i = 0; i < height; i++)
       dc += topleft[-(i + 1)];
    dc >>= ctz(width + height);

    if (width != height) {
        dc *= (width > height * 2 || height > width * 2) ? MULTIPLIER_1x4 :
                                                           MULTIPLIER_1x2;
        dc >>= BASE_SHIFT;
    }

    splat_dc(dst, stride, width, height, dc);
}

#undef MULTIPLIER_1x2
#undef MULTIPLIER_1x4
#undef BASE_SHIFT

static void ipred_dc_128_c(pixel *dst, const ptrdiff_t stride,
                           const pixel *const topleft,
                           const int width, const int height, const int a)
{
    splat_dc(dst, stride, width, height, 1 << (BITDEPTH - 1));
}

static void ipred_v_c(pixel *dst, const ptrdiff_t stride,
                      const pixel *const topleft,
                      const int width, const int height, const int a)
{
    for (int y = 0; y < height; y++) {
        pixel_copy(dst, topleft + 1, width);
        dst += PXSTRIDE(stride);
    }
}

static void ipred_h_c(pixel *dst, const ptrdiff_t stride,
                      const pixel *const topleft,
                      const int width, const int height, const int a)
{
    for (int y = 0; y < height; y++) {
        pixel_set(dst, topleft[-(1 + y)], width);
        dst += PXSTRIDE(stride);
    }
}

static void ipred_paeth_c(pixel *dst, const ptrdiff_t stride,
                          const pixel *const tl_ptr,
                          const int width, const int height, const int a)
{
    const int topleft = tl_ptr[0];
    for (int y = 0; y < height; y++) {
        const int left = tl_ptr[-(y + 1)];
        for (int x = 0; x < width; x++) {
            const int top = tl_ptr[1 + x];
            const int base = left + top - topleft;
            const int ldiff = abs(left - base);
            const int tdiff = abs(top - base);
            const int tldiff = abs(topleft - base);

            dst[x] = ldiff <= tdiff && ldiff <= tldiff ? left :
                     tdiff <= tldiff ? top : topleft;
        }
        dst += PXSTRIDE(stride);
    }
}

static void ipred_smooth_c(pixel *dst, const ptrdiff_t stride,
                           const pixel *const topleft,
                           const int width, const int height, const int a)
{
    const uint8_t *const weights_hor = &dav1d_sm_weights[width];
    const uint8_t *const weights_ver = &dav1d_sm_weights[height];
    const int right = topleft[width], bottom = topleft[-height];

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int pred = weights_ver[y]  * topleft[1 + x] +
                      (256 - weights_ver[y]) * bottom +
                             weights_hor[x]  * topleft[-(1 + y)] +
                      (256 - weights_hor[x]) * right;
            dst[x] = (pred + 256) >> 9;
        }
        dst += PXSTRIDE(stride);
    }
}

static void ipred_smooth_v_c(pixel *dst, const ptrdiff_t stride,
                             const pixel *const topleft,
                             const int width, const int height, const int a)
{
    const uint8_t *const weights_ver = &dav1d_sm_weights[height];
    const int bottom = topleft[-height];

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int pred = weights_ver[y]  * topleft[1 + x] +
                      (256 - weights_ver[y]) * bottom;
            dst[x] = (pred + 128) >> 8;
        }
        dst += PXSTRIDE(stride);
    }
}

static void ipred_smooth_h_c(pixel *dst, const ptrdiff_t stride,
                             const pixel *const topleft,
                             const int width, const int height, const int a)
{
    const uint8_t *const weights_hor = &dav1d_sm_weights[width];
    const int right = topleft[width];

    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int pred = weights_hor[x]  * topleft[-(y + 1)] +
                      (256 - weights_hor[x]) * right;
            dst[x] = (pred + 128) >> 8;
        }
        dst += PXSTRIDE(stride);
    }
}

static int get_filter_strength(const unsigned blk_wh, const unsigned d,
                               const int type)
{
    int strength = 0;

    if (type == 0) {
        if (blk_wh <= 8) {
            if (d >= 56) strength = 1;
        } else if (blk_wh <= 12) {
            if (d >= 40) strength = 1;
        } else if (blk_wh <= 16) {
            if (d >= 40) strength = 1;
        } else if (blk_wh <= 24) {
            if (d >= 8) strength = 1;
            if (d >= 16) strength = 2;
            if (d >= 32) strength = 3;
        } else if (blk_wh <= 32) {
            if (d >= 1) strength = 1;
            if (d >= 4) strength = 2;
            if (d >= 32) strength = 3;
        } else {
            if (d >= 1) strength = 3;
        }
    } else {
        if (blk_wh <= 8) {
            if (d >= 40) strength = 1;
            if (d >= 64) strength = 2;
        } else if (blk_wh <= 16) {
            if (d >= 20) strength = 1;
            if (d >= 48) strength = 2;
        } else if (blk_wh <= 24) {
            if (d >= 4) strength = 3;
        } else {
            if (d >= 1) strength = 3;
        }
    }

    return strength;
}

static void filter_edge(pixel *const out, const int sz, const pixel *const in,
                        const int from, const int to, const unsigned strength)
{
    static const uint8_t kernel[3][5] = {
        { 0, 4, 8, 4, 0 },
        { 0, 5, 6, 5, 0 },
        { 2, 4, 4, 4, 2 }
    };

    assert(strength > 0);
    for (int i = 0; i < sz; i++) {
        int s = 0;
        for (int j = 0; j < 5; j++)
            s += in[iclip(i - 2 + j, from, to - 1)] * kernel[strength - 1][j];
        out[i] = (s + 8) >> 4;
    }
}

static int get_upsample(const int blk_wh, const unsigned d, const int type) {
    if (d >= 40) return 0;
    return type ? (blk_wh <= 8) : (blk_wh <= 16);
}

static void upsample_edge(pixel *const out, const int hsz,
                          const pixel *const in, const int from, const int to)
{
    static const int8_t kernel[4] = { -1, 9, 9, -1 };
    int i;
    for (i = 0; i < hsz - 1; i++) {
        out[i * 2] = in[iclip(i, from, to - 1)];

        int s = 0;
        for (int j = 0; j < 4; j++)
            s += in[iclip(i + j - 1, from, to - 1)] * kernel[j];
        out[i * 2 + 1] = iclip_pixel((s + 8) >> 4);
    }
    out[i * 2] = in[iclip(i, from, to - 1)];
}

static void ipred_z1_c(pixel *dst, const ptrdiff_t stride,
                       const pixel *const topleft_in,
                       const int width, const int height, int angle)
{
    const int is_sm = angle >> 9;
    angle &= 511;
    assert(angle < 90);
    const int dx = dav1d_dr_intra_derivative[angle];
    pixel top_out[(64 + 64) * 2];
    const pixel *top;
    int max_base_x;
    const int upsample_above = get_upsample(width + height, 90 - angle, is_sm);
    if (upsample_above) {
        upsample_edge(top_out, width + height,
                      &topleft_in[1], -1, width + imin(width, height));
        top = top_out;
        max_base_x = 2 * (width + height) - 2;
    } else {
        const int filter_strength =
            get_filter_strength(width + height, 90 - angle, is_sm);

        if (filter_strength) {
            filter_edge(top_out, width + height,
                        &topleft_in[1], -1, width + imin(width, height),
                        filter_strength);
            top = top_out;
            max_base_x = width + height - 1;
        } else {
            top = &topleft_in[1];
            max_base_x = width + imin(width, height) - 1;
        }
    }
    const int frac_bits = 6 - upsample_above;
    const int base_inc = 1 << upsample_above;
    for (int y = 0, xpos = dx; y < height;
         y++, dst += PXSTRIDE(stride), xpos += dx)
    {
        int base = xpos >> frac_bits;
        const int frac = ((xpos << upsample_above) & 0x3F) >> 1;

        for (int x = 0; x < width; x++, base += base_inc) {
            if (base < max_base_x) {
                const int v = top[base] * (32 - frac) + top[base + 1] * frac;
                dst[x] = iclip_pixel((v + 16) >> 5);
            } else {
                pixel_set(&dst[x], top[max_base_x], width - x);
                break;
            }
        }
    }
}

static void ipred_z2_c(pixel *dst, const ptrdiff_t stride,
                       const pixel *const topleft_in,
                       const int width, const int height, int angle)
{
    const int is_sm = angle >> 9;
    angle &= 511;
    assert(angle > 90 && angle < 180);
    const int dy = dav1d_dr_intra_derivative[angle - 90];
    const int dx = dav1d_dr_intra_derivative[180 - angle];
    const int upsample_left = get_upsample(width + height, 180 - angle, is_sm);
    const int upsample_above = get_upsample(width + height, angle - 90, is_sm);
    pixel edge[64 * 2 + 64 * 2 + 1];
    pixel *const topleft = &edge[height * 2];

    if (upsample_above) {
        upsample_edge(topleft, width + 1, topleft_in, 0, width + 1);
    } else {
        const int filter_strength =
            get_filter_strength(width + height, angle - 90, is_sm);

        if (filter_strength) {
            filter_edge(&topleft[1], width, &topleft_in[1], -1, width,
                        filter_strength);
        } else {
            pixel_copy(&topleft[1], &topleft_in[1], width);
        }
    }
    if (upsample_left) {
        upsample_edge(edge, height + 1, &topleft_in[-height], 0, height + 1);
    } else {
        const int filter_strength =
            get_filter_strength(width + height, 180 - angle, is_sm);

        if (filter_strength) {
            filter_edge(&topleft[-height], height, &topleft_in[-height],
                        0, height + 1, filter_strength);
        } else {
            pixel_copy(&topleft[-height], &topleft_in[-height], height);
        }
    }
    *topleft = *topleft_in;

    const int min_base_x = -(1 << upsample_above);
    const int frac_bits_y = 6 - upsample_left, frac_bits_x = 6 - upsample_above;
    const int base_inc_x = 1 << upsample_above;
    const pixel *const left = &topleft[-(1 << upsample_left)];
    const pixel *const top = &topleft[1 << upsample_above];
    for (int y = 0, xpos = -dx; y < height;
         y++, xpos -= dx, dst += PXSTRIDE(stride))
    {
        int base_x = xpos >> frac_bits_x;
        const int frac_x = ((xpos * (1 << upsample_above)) & 0x3F) >> 1;

        for (int x = 0, ypos = (y << 6) - dy; x < width;
             x++, base_x += base_inc_x, ypos -= dy)
        {
            int v;

            if (base_x >= min_base_x) {
                v = top[base_x] * (32 - frac_x) + top[base_x + 1] * frac_x;
            } else {
                const int base_y = ypos >> frac_bits_y;
                assert(base_y >= -(1 << upsample_left));
                const int frac_y = ((ypos * (1 << upsample_left)) & 0x3F) >> 1;
                v = left[-base_y] * (32 - frac_y) + left[-(base_y + 1)] * frac_y;
            }
            dst[x] = iclip_pixel((v + 16) >> 5);
        }
    }
}

static void ipred_z3_c(pixel *dst, const ptrdiff_t stride,
                       const pixel *const topleft_in,
                       const int width, const int height, int angle)
{
    const int is_sm = angle >> 9;
    angle &= 511;
    assert(angle > 180);
    const int dy = dav1d_dr_intra_derivative[270 - angle];
    pixel left_out[(64 + 64) * 2];
    const pixel *left;
    int max_base_y;
    const int upsample_left = get_upsample(width + height, angle - 180, is_sm);
    if (upsample_left) {
        upsample_edge(left_out, width + height,
                      &topleft_in[-(width + height)],
                      imax(width - height, 0), width + height + 1);
        left = &left_out[2 * (width + height) - 2];
        max_base_y = 2 * (width + height) - 2;
    } else {
        const int filter_strength =
            get_filter_strength(width + height, angle - 180, is_sm);

        if (filter_strength) {
            filter_edge(left_out, width + height,
                        &topleft_in[-(width + height)],
                        imax(width - height, 0), width + height + 1,
                        filter_strength);
            left = &left_out[width + height - 1];
            max_base_y = width + height - 1;
        } else {
            left = &topleft_in[-1];
            max_base_y = height + imin(width, height) - 1;
        }
    }
    const int frac_bits = 6 - upsample_left;
    const int base_inc = 1 << upsample_left;
    for (int x = 0, ypos = dy; x < width; x++, ypos += dy) {
        int base = ypos >> frac_bits;
        const int frac = ((ypos << upsample_left) & 0x3F) >> 1;

        for (int y = 0; y < height; y++, base += base_inc) {
            if (base < max_base_y) {
                const int v = left[-base] * (32 - frac) +
                              left[-(base + 1)] * frac;
                dst[y * PXSTRIDE(stride) + x] = iclip_pixel((v + 16) >> 5);
            } else {
                do {
                    dst[y * PXSTRIDE(stride) + x] = left[-max_base_y];
                } while (++y < height);
                break;
            }
        }
    }
}

/* Up to 32x32 only */
static void ipred_filter_c(pixel *dst, const ptrdiff_t stride,
                           const pixel *const topleft_in,
                           const int width, const int height, int filt_idx)
{
    filt_idx &= 511;
    assert(filt_idx < 5);

    const int8_t (*const filter)[8] = dav1d_filter_intra_taps[filt_idx];
    int x, y;
    ptrdiff_t left_stride;
    const pixel *left, *topleft, *top;

    top = &topleft_in[1];
    for (y = 0; y < height; y += 2) {
        topleft = &topleft_in[-y];
        left = &topleft[-1];
        left_stride = -1;
        for (x = 0; x < width; x += 4) {
            const int p0 = *topleft;
            const int p1 = top[0], p2 = top[1], p3 = top[2], p4 = top[3];
            const int p5 = left[0 * left_stride], p6 = left[1 * left_stride];
            pixel *ptr = &dst[x];
            const int8_t (*flt_ptr)[8] = filter;

            for (int yy = 0; yy < 2; yy++) {
                for (int xx = 0; xx < 4; xx++, flt_ptr++) {
                    int acc = flt_ptr[0][0] * p0 + flt_ptr[0][1] * p1 +
                              flt_ptr[0][2] * p2 + flt_ptr[0][3] * p3 +
                              flt_ptr[0][4] * p4 + flt_ptr[0][5] * p5 +
                              flt_ptr[0][6] * p6;
                    ptr[xx] = iclip_pixel((acc + 8) >> 4);
                }
                ptr += PXSTRIDE(stride);
            }

            left = &dst[x + 4 - 1];
            left_stride = PXSTRIDE(stride);
            top += 4;
            topleft = &top[-1];
        }
        top = &dst[PXSTRIDE(stride)];
        dst = &dst[PXSTRIDE(stride) * 2];
    }
}

static NOINLINE void
cfl_ac_c(int16_t *ac, const pixel *ypx, const ptrdiff_t stride,
         const int w_pad, const int h_pad, const int width, const int height,
         const int ss_hor, const int ss_ver, const int log2sz)
{
    int y, x;
    int16_t *const ac_orig = ac;

    assert(w_pad >= 0 && w_pad * 4 < width);
    assert(h_pad >= 0 && h_pad * 4 < height);

    for (y = 0; y < height - 4 * h_pad; y++) {
        for (x = 0; x < width - 4 * w_pad; x++) {
            int ac_sum = ypx[x << ss_hor];
            if (ss_hor) ac_sum += ypx[x * 2 + 1];
            if (ss_ver) {
                ac_sum += ypx[(x << ss_hor) + PXSTRIDE(stride)];
                if (ss_hor) ac_sum += ypx[x * 2 + 1 + PXSTRIDE(stride)];
            }
            ac[x] = ac_sum << (1 + !ss_ver + !ss_hor);
        }
        for (; x < width; x++)
            ac[x] = ac[x - 1];
        ac += width;
        ypx += PXSTRIDE(stride) << ss_ver;
    }
    for (; y < height; y++) {
        memcpy(ac, &ac[-width], width * sizeof(*ac));
        ac += width;
    }

    int sum = (1 << log2sz) >> 1;
    for (ac = ac_orig, y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            sum += ac[x];
        ac += width;
    }
    sum >>= log2sz;

    // subtract DC
    for (ac = ac_orig, y = 0; y < height; y++) {
        for (x = 0; x < width; x++)
            ac[x] -= sum;
        ac += width;
    }
}

#define cfl_ac_fn(lw, lh, cw, ch, ss_hor, ss_ver, log2sz) \
static void cfl_ac_##lw##x##lh##_to_##cw##x##ch##_c(int16_t *const ac, \
                                                    const pixel *const ypx, \
                                                    const ptrdiff_t stride, \
                                                    const int w_pad, \
                                                    const int h_pad) \
{ \
    cfl_ac_c(ac, ypx, stride, w_pad, h_pad, cw, ch, ss_hor, ss_ver, log2sz); \
}

cfl_ac_fn( 8,  8,  4,  4, 1, 1, 4)
cfl_ac_fn( 8, 16,  4,  8, 1, 1, 5)
cfl_ac_fn( 8, 32,  4, 16, 1, 1, 6)
cfl_ac_fn(16,  8,  8,  4, 1, 1, 5)
cfl_ac_fn(16, 16,  8,  8, 1, 1, 6)
cfl_ac_fn(16, 32,  8, 16, 1, 1, 7)
cfl_ac_fn(32,  8, 16,  4, 1, 1, 6)
cfl_ac_fn(32, 16, 16,  8, 1, 1, 7)
cfl_ac_fn(32, 32, 16, 16, 1, 1, 8)

cfl_ac_fn( 8,  4,  4,  4, 1, 0, 4)
cfl_ac_fn( 8,  8,  4,  8, 1, 0, 5)
cfl_ac_fn(16,  4,  8,  4, 1, 0, 5)
cfl_ac_fn(16,  8,  8,  8, 1, 0, 6)
cfl_ac_fn(16, 16,  8, 16, 1, 0, 7)
cfl_ac_fn(32,  8, 16,  8, 1, 0, 7)
cfl_ac_fn(32, 16, 16, 16, 1, 0, 8)
cfl_ac_fn(32, 32, 16, 32, 1, 0, 9)

cfl_ac_fn( 4,  4,  4,  4, 0, 0, 4)
cfl_ac_fn( 4,  8,  4,  8, 0, 0, 5)
cfl_ac_fn( 4, 16,  4, 16, 0, 0, 6)
cfl_ac_fn( 8,  4,  8,  4, 0, 0, 5)
cfl_ac_fn( 8,  8,  8,  8, 0, 0, 6)
cfl_ac_fn( 8, 16,  8, 16, 0, 0, 7)
cfl_ac_fn( 8, 32,  8, 32, 0, 0, 8)
cfl_ac_fn(16,  4, 16,  4, 0, 0, 6)
cfl_ac_fn(16,  8, 16,  8, 0, 0, 7)
cfl_ac_fn(16, 16, 16, 16, 0, 0, 8)
cfl_ac_fn(16, 32, 16, 32, 0, 0, 9)
cfl_ac_fn(32,  8, 32,  8, 0, 0, 8)
cfl_ac_fn(32, 16, 32, 16, 0, 0, 9)
cfl_ac_fn(32, 32, 32, 32, 0, 0, 10)

static NOINLINE void
cfl_pred_1_c(pixel *dst, const ptrdiff_t stride, const int16_t *ac,
             const int8_t alpha, const int width, const int height)
{
    const pixel dc = *dst;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int diff = alpha * ac[x];
            dst[x] = iclip_pixel(dc + apply_sign((abs(diff) + 32) >> 6, diff));
        }
        ac += width;
        dst += PXSTRIDE(stride);
    }
}

#define cfl_pred_1_fn(width) \
static void cfl_pred_1_##width##xN_c(pixel *const dst, \
                                     const ptrdiff_t stride, \
                                     const int16_t *const ac, \
                                     const int8_t alpha, \
                                     const int height) \
{ \
    cfl_pred_1_c(dst, stride, ac, alpha, width, height); \
}

cfl_pred_1_fn( 4)
cfl_pred_1_fn( 8)
cfl_pred_1_fn(16)
cfl_pred_1_fn(32)

static NOINLINE void
cfl_pred_c(pixel *dstU, pixel *dstV, const ptrdiff_t stride, const int16_t *ac,
           const int8_t *const alphas, const int width, const int height)
{
    const pixel dcU = *dstU, dcV = *dstV;
    for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
            const int diff1 = alphas[0] * ac[x];
            dstU[x] = iclip_pixel(dcU + apply_sign((abs(diff1) + 32) >> 6, diff1));
            const int diff2 = alphas[1] * ac[x];
            dstV[x] = iclip_pixel(dcV + apply_sign((abs(diff2) + 32) >> 6, diff2));
        }
        ac += width;
        dstU += PXSTRIDE(stride);
        dstV += PXSTRIDE(stride);
    }
}

#define cfl_pred_fn(width) \
static void cfl_pred_##width##xN_c(pixel *const dstU, \
                                   pixel *const dstV, \
                                   const ptrdiff_t stride, \
                                   const int16_t *const ac, \
                                   const int8_t *const alphas, \
                                   const int height) \
{ \
    cfl_pred_c(dstU, dstV, stride, ac, alphas, width, height); \
}

cfl_pred_fn( 4)
cfl_pred_fn( 8)
cfl_pred_fn(16)
cfl_pred_fn(32)

static void pal_pred_c(pixel *dst, const ptrdiff_t stride,
                       const uint16_t *const pal, const uint8_t *idx,
                       const int w, const int h)
{
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++)
            dst[x] = pal[idx[x]];
        idx += w;
        dst += PXSTRIDE(stride);
    }
}

void bitfn(dav1d_intra_pred_dsp_init)(Dav1dIntraPredDSPContext *const c) {
    c->intra_pred[DC_PRED      ] = ipred_dc_c;
    c->intra_pred[DC_128_PRED  ] = ipred_dc_128_c;
    c->intra_pred[TOP_DC_PRED  ] = ipred_dc_top_c;
    c->intra_pred[LEFT_DC_PRED ] = ipred_dc_left_c;
    c->intra_pred[HOR_PRED     ] = ipred_h_c;
    c->intra_pred[VERT_PRED    ] = ipred_v_c;
    c->intra_pred[PAETH_PRED   ] = ipred_paeth_c;
    c->intra_pred[SMOOTH_PRED  ] = ipred_smooth_c;
    c->intra_pred[SMOOTH_V_PRED] = ipred_smooth_v_c;
    c->intra_pred[SMOOTH_H_PRED] = ipred_smooth_h_c;
    c->intra_pred[Z1_PRED      ] = ipred_z1_c;
    c->intra_pred[Z2_PRED      ] = ipred_z2_c;
    c->intra_pred[Z3_PRED      ] = ipred_z3_c;
    c->intra_pred[FILTER_PRED  ] = ipred_filter_c;

    // cfl functions are split per chroma subsampling type
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I420 - 1][ TX_4X4  ] = cfl_ac_8x8_to_4x4_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I420 - 1][RTX_4X8  ] = cfl_ac_8x16_to_4x8_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I420 - 1][RTX_4X16 ] = cfl_ac_8x32_to_4x16_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I420 - 1][RTX_8X4  ] = cfl_ac_16x8_to_8x4_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I420 - 1][ TX_8X8  ] = cfl_ac_16x16_to_8x8_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I420 - 1][RTX_8X16 ] = cfl_ac_16x32_to_8x16_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I420 - 1][RTX_16X4 ] = cfl_ac_32x8_to_16x4_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I420 - 1][RTX_16X8 ] = cfl_ac_32x16_to_16x8_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I420 - 1][ TX_16X16] = cfl_ac_32x32_to_16x16_c;

    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I422 - 1][ TX_4X4  ] = cfl_ac_8x4_to_4x4_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I422 - 1][RTX_4X8  ] = cfl_ac_8x8_to_4x8_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I422 - 1][RTX_8X4  ] = cfl_ac_16x4_to_8x4_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I422 - 1][ TX_8X8  ] = cfl_ac_16x8_to_8x8_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I422 - 1][RTX_8X16 ] = cfl_ac_16x16_to_8x16_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I422 - 1][RTX_16X8 ] = cfl_ac_32x8_to_16x8_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I422 - 1][ TX_16X16] = cfl_ac_32x16_to_16x16_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I422 - 1][RTX_16X32] = cfl_ac_32x32_to_16x32_c;

    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][ TX_4X4  ] = cfl_ac_4x4_to_4x4_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][RTX_4X8  ] = cfl_ac_4x8_to_4x8_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][RTX_4X16 ] = cfl_ac_4x16_to_4x16_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][RTX_8X4  ] = cfl_ac_8x4_to_8x4_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][ TX_8X8  ] = cfl_ac_8x8_to_8x8_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][RTX_8X16 ] = cfl_ac_8x16_to_8x16_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][RTX_8X32 ] = cfl_ac_8x32_to_8x32_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][RTX_16X4 ] = cfl_ac_16x4_to_16x4_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][RTX_16X8 ] = cfl_ac_16x8_to_16x8_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][ TX_16X16] = cfl_ac_16x16_to_16x16_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][RTX_16X32] = cfl_ac_16x32_to_16x32_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][RTX_32X8 ] = cfl_ac_32x8_to_32x8_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][RTX_32X16] = cfl_ac_32x16_to_32x16_c;
    c->cfl_ac[DAV1D_PIXEL_LAYOUT_I444 - 1][ TX_32X32] = cfl_ac_32x32_to_32x32_c;

    c->cfl_pred_1[0] = cfl_pred_1_4xN_c;
    c->cfl_pred_1[1] = cfl_pred_1_8xN_c;
    c->cfl_pred_1[2] = cfl_pred_1_16xN_c;
    c->cfl_pred_1[3] = cfl_pred_1_32xN_c;

    c->cfl_pred[0] = cfl_pred_4xN_c;
    c->cfl_pred[1] = cfl_pred_8xN_c;
    c->cfl_pred[2] = cfl_pred_16xN_c;
    c->cfl_pred[3] = cfl_pred_32xN_c;

    c->pal_pred = pal_pred_c;

#if HAVE_ASM && ARCH_X86
    bitfn(dav1d_intra_pred_dsp_init_x86)(c);
#endif
}
