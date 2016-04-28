/*
 *  Copyright (c) 2016 The WebM project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include <assert.h>
#include <smmintrin.h> /* SSE4.1 */

#include "./vp10_rtcd.h"
#include "./vpx_config.h"
#include "vp10/common/vp10_fwd_txfm2d_cfg.h"
#include "vp10/common/vp10_txfm.h"
#include "vpx_dsp/txfm_common.h"
#include "vpx_dsp/x86/txfm_common_sse2.h"
#include "vpx_ports/mem.h"

static INLINE void load_buffer_4x4(const int16_t *input, __m128i *in,
                                   int stride, int flipud, int fliplr,
                                   int shift) {
  if (!flipud) {
    in[0] = _mm_loadl_epi64((const __m128i *)(input + 0 * stride));
    in[1] = _mm_loadl_epi64((const __m128i *)(input + 1 * stride));
    in[2] = _mm_loadl_epi64((const __m128i *)(input + 2 * stride));
    in[3] = _mm_loadl_epi64((const __m128i *)(input + 3 * stride));
  } else {
    in[0] = _mm_loadl_epi64((const __m128i *)(input + 3 * stride));
    in[1] = _mm_loadl_epi64((const __m128i *)(input + 2 * stride));
    in[2] = _mm_loadl_epi64((const __m128i *)(input + 1 * stride));
    in[3] = _mm_loadl_epi64((const __m128i *)(input + 0 * stride));
  }

  if (fliplr) {
    in[0] = _mm_shufflelo_epi16(in[0], 0x1b);
    in[1] = _mm_shufflelo_epi16(in[1], 0x1b);
    in[2] = _mm_shufflelo_epi16(in[2], 0x1b);
    in[3] = _mm_shufflelo_epi16(in[3], 0x1b);
  }

  in[0] = _mm_cvtepi16_epi32(in[0]);
  in[1] = _mm_cvtepi16_epi32(in[1]);
  in[2] = _mm_cvtepi16_epi32(in[2]);
  in[3] = _mm_cvtepi16_epi32(in[3]);

  in[0] = _mm_slli_epi32(in[0], shift);
  in[1] = _mm_slli_epi32(in[1], shift);
  in[2] = _mm_slli_epi32(in[2], shift);
  in[3] = _mm_slli_epi32(in[3], shift);
}

// We only use stage-2 bit;
// shift[0] is used in load_buffer_4x4()
// shift[1] is used in txfm_func_col()
// shift[2] is used in txfm_func_row()
static void fdct4x4_sse4_1(__m128i *in, int bit) {
  const int32_t *cospi = cospi_arr[bit - cos_bit_min];
  const __m128i cospi32 = _mm_set1_epi32(cospi[32]);
  const __m128i cospi48 = _mm_set1_epi32(cospi[48]);
  const __m128i cospi16 = _mm_set1_epi32(cospi[16]);
  const __m128i rnding = _mm_set1_epi32(1 << (bit - 1));
  __m128i s0, s1, s2, s3;
  __m128i u0, u1, u2, u3;
  __m128i v0, v1, v2, v3;

  s0 = _mm_add_epi32(in[0], in[3]);
  s1 = _mm_add_epi32(in[1], in[2]);
  s2 = _mm_sub_epi32(in[1], in[2]);
  s3 = _mm_sub_epi32(in[0], in[3]);

  // btf_32_sse4_1_type0(cospi32, cospi32, s[01], u[02], bit);
  u0 = _mm_mullo_epi32(s0, cospi32);
  u1 = _mm_mullo_epi32(s1, cospi32);
  u2 = _mm_add_epi32(u0, u1);
  v0 = _mm_sub_epi32(u0, u1);

  u3 = _mm_add_epi32(u2, rnding);
  v1 = _mm_add_epi32(v0, rnding);

  u0 = _mm_srai_epi32(u3, bit);
  u2 = _mm_srai_epi32(v1, bit);

  // btf_32_sse4_1_type1(cospi48, cospi16, s[23], u[13], bit);
  v0 = _mm_mullo_epi32(s2, cospi48);
  v1 = _mm_mullo_epi32(s3, cospi16);
  v2 = _mm_add_epi32(v0, v1);

  v3 = _mm_add_epi32(v2, rnding);
  u1 = _mm_srai_epi32(v3, bit);

  v0 = _mm_mullo_epi32(s2, cospi16);
  v1 = _mm_mullo_epi32(s3, cospi48);
  v2 = _mm_sub_epi32(v1, v0);

  v3 = _mm_add_epi32(v2, rnding);
  u3 = _mm_srai_epi32(v3, bit);

  // Note: shift[1] and shift[2] are zeros

  // Transpose 4x4 32-bit
  v0 = _mm_unpacklo_epi32(u0, u1);
  v1 = _mm_unpackhi_epi32(u0, u1);
  v2 = _mm_unpacklo_epi32(u2, u3);
  v3 = _mm_unpackhi_epi32(u2, u3);

  in[0] = _mm_unpacklo_epi64(v0, v2);
  in[1] = _mm_unpackhi_epi64(v0, v2);
  in[2] = _mm_unpacklo_epi64(v1, v3);
  in[3] = _mm_unpackhi_epi64(v1, v3);
}

static INLINE void write_buffer_4x4(__m128i *res, tran_low_t *output) {
  _mm_store_si128((__m128i *)(output + 0 * 4), res[0]);
  _mm_store_si128((__m128i *)(output + 1 * 4), res[1]);
  _mm_store_si128((__m128i *)(output + 2 * 4), res[2]);
  _mm_store_si128((__m128i *)(output + 3 * 4), res[3]);
}

// Note:
//  We implement vp10_fwd_txfm2d_4x4(). This function is kept here since
//  vp10_highbd_fht4x4_c() is not removed yet
void vp10_highbd_fht4x4_sse4_1(const int16_t *input, tran_low_t *output,
                               int stride, int tx_type) {
  (void)input;
  (void)output;
  (void)stride;
  (void)tx_type;
  assert(0);
}

static void fadst4x4_sse4_1(__m128i *in, int bit) {
  const int32_t *cospi = cospi_arr[bit - cos_bit_min];
  const __m128i cospi8 = _mm_set1_epi32(cospi[8]);
  const __m128i cospi56 = _mm_set1_epi32(cospi[56]);
  const __m128i cospi40 = _mm_set1_epi32(cospi[40]);
  const __m128i cospi24 = _mm_set1_epi32(cospi[24]);
  const __m128i cospi32 = _mm_set1_epi32(cospi[32]);
  const __m128i rnding = _mm_set1_epi32(1 << (bit - 1));
  const __m128i kZero = _mm_setzero_si128();
  __m128i s0, s1, s2, s3;
  __m128i u0, u1, u2, u3;
  __m128i v0, v1, v2, v3;

  // stage 0
  // stage 1
  // stage 2
  u0 = _mm_mullo_epi32(in[3], cospi8);
  u1 = _mm_mullo_epi32(in[0], cospi56);
  u2 = _mm_add_epi32(u0, u1);
  s0 = _mm_add_epi32(u2, rnding);
  s0 = _mm_srai_epi32(s0, bit);

  v0 = _mm_mullo_epi32(in[3], cospi56);
  v1 = _mm_mullo_epi32(in[0], cospi8);
  v2 = _mm_sub_epi32(v0, v1);
  s1 = _mm_add_epi32(v2, rnding);
  s1 = _mm_srai_epi32(s1, bit);

  u0 = _mm_mullo_epi32(in[1], cospi40);
  u1 = _mm_mullo_epi32(in[2], cospi24);
  u2 = _mm_add_epi32(u0, u1);
  s2 = _mm_add_epi32(u2, rnding);
  s2 = _mm_srai_epi32(s2, bit);

  v0 = _mm_mullo_epi32(in[1], cospi24);
  v1 = _mm_mullo_epi32(in[2], cospi40);
  v2 = _mm_sub_epi32(v0, v1);
  s3 = _mm_add_epi32(v2, rnding);
  s3 = _mm_srai_epi32(s3, bit);

  // stage 3
  u0 = _mm_add_epi32(s0, s2);
  u2 = _mm_sub_epi32(s0, s2);
  u1 = _mm_add_epi32(s1, s3);
  u3 = _mm_sub_epi32(s1, s3);

  // stage 4
  v0 = _mm_mullo_epi32(u2, cospi32);
  v1 = _mm_mullo_epi32(u3, cospi32);
  v2 = _mm_add_epi32(v0, v1);
  s2 = _mm_add_epi32(v2, rnding);
  u2 = _mm_srai_epi32(s2, bit);

  v2 = _mm_sub_epi32(v0, v1);
  s3 = _mm_add_epi32(v2, rnding);
  u3 = _mm_srai_epi32(s3, bit);

  // u0, u1, u2, u3
  u2 = _mm_sub_epi32(kZero, u2);
  u1 = _mm_sub_epi32(kZero, u1);

  // u0, u2, u3, u1
  // Transpose 4x4 32-bit
  v0 = _mm_unpacklo_epi32(u0, u2);
  v1 = _mm_unpackhi_epi32(u0, u2);
  v2 = _mm_unpacklo_epi32(u3, u1);
  v3 = _mm_unpackhi_epi32(u3, u1);

  in[0] = _mm_unpacklo_epi64(v0, v2);
  in[1] = _mm_unpackhi_epi64(v0, v2);
  in[2] = _mm_unpacklo_epi64(v1, v3);
  in[3] = _mm_unpackhi_epi64(v1, v3);
}

void vp10_fwd_txfm2d_4x4_sse4_1(const int16_t *input, tran_low_t *coeff,
                                int input_stride, int tx_type,
                                const int bd) {
  __m128i in[4];
  const TXFM_2D_CFG *cfg = NULL;

  switch (tx_type) {
    case DCT_DCT:
      cfg = &fwd_txfm_2d_cfg_dct_dct_4;
      load_buffer_4x4(input, in, input_stride, 0, 0, cfg->shift[0]);
      fdct4x4_sse4_1(in, cfg->cos_bit_col[2]);
      fdct4x4_sse4_1(in, cfg->cos_bit_row[2]);
      write_buffer_4x4(in, coeff);
      break;
    case ADST_DCT:
      cfg = &fwd_txfm_2d_cfg_adst_dct_4;
      load_buffer_4x4(input, in, input_stride, 0, 0, cfg->shift[0]);
      fadst4x4_sse4_1(in, cfg->cos_bit_col[2]);
      fdct4x4_sse4_1(in, cfg->cos_bit_row[2]);
      write_buffer_4x4(in, coeff);
      break;
    case DCT_ADST:
      cfg = &fwd_txfm_2d_cfg_dct_adst_4;
      load_buffer_4x4(input, in, input_stride, 0, 0, cfg->shift[0]);
      fdct4x4_sse4_1(in, cfg->cos_bit_col[2]);
      fadst4x4_sse4_1(in, cfg->cos_bit_row[2]);
      write_buffer_4x4(in, coeff);
      break;
    case ADST_ADST:
      cfg = &fwd_txfm_2d_cfg_adst_adst_4;
      load_buffer_4x4(input, in, input_stride, 0, 0, cfg->shift[0]);
      fadst4x4_sse4_1(in, cfg->cos_bit_col[2]);
      fadst4x4_sse4_1(in, cfg->cos_bit_row[2]);
      write_buffer_4x4(in, coeff);
      break;
    default:
      assert(0);
  }
  (void)bd;
}

static INLINE void load_buffer_8x8(const int16_t *input, __m128i *in,
                                   int stride, int flipud, int fliplr,
                                   int shift) {
  __m128i u;
  if (!flipud) {
    in[0]  = _mm_load_si128((const __m128i *)(input + 0 * stride));
    in[1]  = _mm_load_si128((const __m128i *)(input + 1 * stride));
    in[2]  = _mm_load_si128((const __m128i *)(input + 2 * stride));
    in[3]  = _mm_load_si128((const __m128i *)(input + 3 * stride));
    in[4]  = _mm_load_si128((const __m128i *)(input + 4 * stride));
    in[5]  = _mm_load_si128((const __m128i *)(input + 5 * stride));
    in[6]  = _mm_load_si128((const __m128i *)(input + 6 * stride));
    in[7]  = _mm_load_si128((const __m128i *)(input + 7 * stride));
  } else {
    in[0]  = _mm_load_si128((const __m128i *)(input + 7 * stride));
    in[1]  = _mm_load_si128((const __m128i *)(input + 6 * stride));
    in[2]  = _mm_load_si128((const __m128i *)(input + 5 * stride));
    in[3]  = _mm_load_si128((const __m128i *)(input + 4 * stride));
    in[4]  = _mm_load_si128((const __m128i *)(input + 3 * stride));
    in[5]  = _mm_load_si128((const __m128i *)(input + 2 * stride));
    in[6]  = _mm_load_si128((const __m128i *)(input + 1 * stride));
    in[7]  = _mm_load_si128((const __m128i *)(input + 0 * stride));
  }

  if (fliplr) {
    in[0] = mm_reverse_epi16(in[0]);
    in[1] = mm_reverse_epi16(in[1]);
    in[2] = mm_reverse_epi16(in[2]);
    in[3] = mm_reverse_epi16(in[3]);
    in[4] = mm_reverse_epi16(in[4]);
    in[5] = mm_reverse_epi16(in[5]);
    in[6] = mm_reverse_epi16(in[6]);
    in[7] = mm_reverse_epi16(in[7]);
  }

  u = _mm_unpackhi_epi64(in[4], in[4]);
  in[8] = _mm_cvtepi16_epi32(in[4]);
  in[9] = _mm_cvtepi16_epi32(u);

  u = _mm_unpackhi_epi64(in[5], in[5]);
  in[10] = _mm_cvtepi16_epi32(in[5]);
  in[11] = _mm_cvtepi16_epi32(u);

  u = _mm_unpackhi_epi64(in[6], in[6]);
  in[12] = _mm_cvtepi16_epi32(in[6]);
  in[13] = _mm_cvtepi16_epi32(u);

  u = _mm_unpackhi_epi64(in[7], in[7]);
  in[14] = _mm_cvtepi16_epi32(in[7]);
  in[15] = _mm_cvtepi16_epi32(u);

  u = _mm_unpackhi_epi64(in[3], in[3]);
  in[6] = _mm_cvtepi16_epi32(in[3]);
  in[7] = _mm_cvtepi16_epi32(u);

  u = _mm_unpackhi_epi64(in[2], in[2]);
  in[4] = _mm_cvtepi16_epi32(in[2]);
  in[5] = _mm_cvtepi16_epi32(u);

  u = _mm_unpackhi_epi64(in[1], in[1]);
  in[2] = _mm_cvtepi16_epi32(in[1]);
  in[3] = _mm_cvtepi16_epi32(u);

  u = _mm_unpackhi_epi64(in[0], in[0]);
  in[0] = _mm_cvtepi16_epi32(in[0]);
  in[1] = _mm_cvtepi16_epi32(u);

  in[0] = _mm_slli_epi32(in[0], shift);
  in[1] = _mm_slli_epi32(in[1], shift);
  in[2] = _mm_slli_epi32(in[2], shift);
  in[3] = _mm_slli_epi32(in[3], shift);
  in[4] = _mm_slli_epi32(in[4], shift);
  in[5] = _mm_slli_epi32(in[5], shift);
  in[6] = _mm_slli_epi32(in[6], shift);
  in[7] = _mm_slli_epi32(in[7], shift);

  in[8] = _mm_slli_epi32(in[8], shift);
  in[9] = _mm_slli_epi32(in[9], shift);
  in[10] = _mm_slli_epi32(in[10], shift);
  in[11] = _mm_slli_epi32(in[11], shift);
  in[12] = _mm_slli_epi32(in[12], shift);
  in[13] = _mm_slli_epi32(in[13], shift);
  in[14] = _mm_slli_epi32(in[14], shift);
  in[15] = _mm_slli_epi32(in[15], shift);
}

static INLINE void col_txfm_8x8_rounding(__m128i *in, int shift) {
  const __m128i rounding = _mm_set1_epi32(1 << (shift - 1));

  in[0] = _mm_add_epi32(in[0], rounding);
  in[1] = _mm_add_epi32(in[1], rounding);
  in[2] = _mm_add_epi32(in[2], rounding);
  in[3] = _mm_add_epi32(in[3], rounding);
  in[4] = _mm_add_epi32(in[4], rounding);
  in[5] = _mm_add_epi32(in[5], rounding);
  in[6] = _mm_add_epi32(in[6], rounding);
  in[7] = _mm_add_epi32(in[7], rounding);
  in[8] = _mm_add_epi32(in[8], rounding);
  in[9] = _mm_add_epi32(in[9], rounding);
  in[10] = _mm_add_epi32(in[10], rounding);
  in[11] = _mm_add_epi32(in[11], rounding);
  in[12] = _mm_add_epi32(in[12], rounding);
  in[13] = _mm_add_epi32(in[13], rounding);
  in[14] = _mm_add_epi32(in[14], rounding);
  in[15] = _mm_add_epi32(in[15], rounding);

  in[0] = _mm_srai_epi32(in[0], shift);
  in[1] = _mm_srai_epi32(in[1], shift);
  in[2] = _mm_srai_epi32(in[2], shift);
  in[3] = _mm_srai_epi32(in[3], shift);
  in[4] = _mm_srai_epi32(in[4], shift);
  in[5] = _mm_srai_epi32(in[5], shift);
  in[6] = _mm_srai_epi32(in[6], shift);
  in[7] = _mm_srai_epi32(in[7], shift);
  in[8] = _mm_srai_epi32(in[8], shift);
  in[9] = _mm_srai_epi32(in[9], shift);
  in[10] = _mm_srai_epi32(in[10], shift);
  in[11] = _mm_srai_epi32(in[11], shift);
  in[12] = _mm_srai_epi32(in[12], shift);
  in[13] = _mm_srai_epi32(in[13], shift);
  in[14] = _mm_srai_epi32(in[14], shift);
  in[15] = _mm_srai_epi32(in[15], shift);
}

#define TRANSPOSE_4X4(x0, x1, x2, x3, y0, y1, y2, y3) \
  do {                                \
    __m128i u0, u1, u2, u3;           \
    u0 = _mm_unpacklo_epi32(x0, x1);  \
    u1 = _mm_unpackhi_epi32(x0, x1);  \
    u2 = _mm_unpacklo_epi32(x2, x3);  \
    u3 = _mm_unpackhi_epi32(x2, x3);  \
    y0 = _mm_unpacklo_epi64(u0, u2);  \
    y1 = _mm_unpackhi_epi64(u0, u2);  \
    y2 = _mm_unpacklo_epi64(u1, u3);  \
    y3 = _mm_unpackhi_epi64(u1, u3);  \
  } while (0)

static INLINE void transpose_8x8(__m128i *in) {
  __m128i t[4];

  TRANSPOSE_4X4(in[0], in[2], in[4], in[6], in[0], in[2], in[4], in[6]);
  TRANSPOSE_4X4(in[1], in[3], in[5], in[7], t[0], t[1], t[2], t[3]);
  TRANSPOSE_4X4(in[8], in[10], in[12], in[14], in[1], in[3], in[5], in[7]);
  in[8] = t[0];
  in[10] = t[1];
  in[12] = t[2];
  in[14] = t[3];
  TRANSPOSE_4X4(in[9], in[11], in[13], in[15], in[9], in[11], in[13], in[15]);
}

static INLINE void write_buffer_8x8(__m128i *res, tran_low_t *output) {
  _mm_store_si128((__m128i *)(output + 0 * 4), res[0]);
  _mm_store_si128((__m128i *)(output + 1 * 4), res[1]);
  _mm_store_si128((__m128i *)(output + 2 * 4), res[2]);
  _mm_store_si128((__m128i *)(output + 3 * 4), res[3]);

  _mm_store_si128((__m128i *)(output + 4 * 4), res[4]);
  _mm_store_si128((__m128i *)(output + 5 * 4), res[5]);
  _mm_store_si128((__m128i *)(output + 6 * 4), res[6]);
  _mm_store_si128((__m128i *)(output + 7 * 4), res[7]);

  _mm_store_si128((__m128i *)(output + 8 * 4), res[8]);
  _mm_store_si128((__m128i *)(output + 9 * 4), res[9]);
  _mm_store_si128((__m128i *)(output + 10 * 4), res[10]);
  _mm_store_si128((__m128i *)(output + 11 * 4), res[11]);

  _mm_store_si128((__m128i *)(output + 12 * 4), res[12]);
  _mm_store_si128((__m128i *)(output + 13 * 4), res[13]);
  _mm_store_si128((__m128i *)(output + 14 * 4), res[14]);
  _mm_store_si128((__m128i *)(output + 15 * 4), res[15]);
}

static void fdct8x8_sse4_1(__m128i *in, __m128i *out, int bit) {
  const int32_t *cospi = cospi_arr[bit - cos_bit_min];
  const __m128i cospi32 = _mm_set1_epi32(cospi[32]);
  const __m128i cospim32 = _mm_set1_epi32(-cospi[32]);
  const __m128i cospi48 = _mm_set1_epi32(cospi[48]);
  const __m128i cospi16 = _mm_set1_epi32(cospi[16]);
  const __m128i cospi56 = _mm_set1_epi32(cospi[56]);
  const __m128i cospi8 = _mm_set1_epi32(cospi[8]);
  const __m128i cospi24 = _mm_set1_epi32(cospi[24]);
  const __m128i cospi40 = _mm_set1_epi32(cospi[40]);
  const __m128i rnding = _mm_set1_epi32(1 << (bit - 1));
  __m128i u[8], v[8];

  // Even 8 points 0, 2, ..., 14
  // stage 0
  // stage 1
  u[0] = _mm_add_epi32(in[0], in[14]);
  v[7] = _mm_sub_epi32(in[0], in[14]);  // v[7]
  u[1] = _mm_add_epi32(in[2], in[12]);
  u[6] = _mm_sub_epi32(in[2], in[12]);
  u[2] = _mm_add_epi32(in[4], in[10]);
  u[5] = _mm_sub_epi32(in[4], in[10]);
  u[3] = _mm_add_epi32(in[6], in[8]);
  v[4] = _mm_sub_epi32(in[6], in[8]);   // v[4]

  // stage 2
  v[0] = _mm_add_epi32(u[0], u[3]);
  v[3] = _mm_sub_epi32(u[0], u[3]);
  v[1] = _mm_add_epi32(u[1], u[2]);
  v[2] = _mm_sub_epi32(u[1], u[2]);

  v[5] = _mm_mullo_epi32(u[5], cospim32);
  v[6] = _mm_mullo_epi32(u[6], cospi32);
  v[5] = _mm_add_epi32(v[5], v[6]);
  v[5] = _mm_add_epi32(v[5], rnding);
  v[5] = _mm_srai_epi32(v[5], bit);

  u[0] = _mm_mullo_epi32(u[5], cospi32);
  v[6] = _mm_mullo_epi32(u[6], cospim32);
  v[6] = _mm_sub_epi32(u[0], v[6]);
  v[6] = _mm_add_epi32(v[6], rnding);
  v[6] = _mm_srai_epi32(v[6], bit);

  // stage 3
  // type 0
  v[0] = _mm_mullo_epi32(v[0], cospi32);
  v[1] = _mm_mullo_epi32(v[1], cospi32);
  u[0] = _mm_add_epi32(v[0], v[1]);
  u[0] = _mm_add_epi32(u[0], rnding);
  u[0] = _mm_srai_epi32(u[0], bit);

  u[1] = _mm_sub_epi32(v[0], v[1]);
  u[1] = _mm_add_epi32(u[1], rnding);
  u[1] = _mm_srai_epi32(u[1], bit);

  // type 1
  v[0] = _mm_mullo_epi32(v[2], cospi48);
  v[1] = _mm_mullo_epi32(v[3], cospi16);
  u[2] = _mm_add_epi32(v[0], v[1]);
  u[2] = _mm_add_epi32(u[2], rnding);
  u[2] = _mm_srai_epi32(u[2], bit);

  v[0] = _mm_mullo_epi32(v[2], cospi16);
  v[1] = _mm_mullo_epi32(v[3], cospi48);
  u[3] = _mm_sub_epi32(v[1], v[0]);
  u[3] = _mm_add_epi32(u[3], rnding);
  u[3] = _mm_srai_epi32(u[3], bit);

  u[4] = _mm_add_epi32(v[4], v[5]);
  u[5] = _mm_sub_epi32(v[4], v[5]);
  u[6] = _mm_sub_epi32(v[7], v[6]);
  u[7] = _mm_add_epi32(v[7], v[6]);

  // stage 4
  // stage 5
  v[0] = _mm_mullo_epi32(u[4], cospi56);
  v[1] = _mm_mullo_epi32(u[7], cospi8);
  v[0] = _mm_add_epi32(v[0], v[1]);
  v[0] = _mm_add_epi32(v[0], rnding);
  out[2] = _mm_srai_epi32(v[0], bit);   // buf0[4]

  v[0] = _mm_mullo_epi32(u[4], cospi8);
  v[1] = _mm_mullo_epi32(u[7], cospi56);
  v[0] = _mm_sub_epi32(v[1], v[0]);
  v[0] = _mm_add_epi32(v[0], rnding);
  out[14] = _mm_srai_epi32(v[0], bit);  // buf0[7]

  v[0] = _mm_mullo_epi32(u[5], cospi24);
  v[1] = _mm_mullo_epi32(u[6], cospi40);
  v[0] = _mm_add_epi32(v[0], v[1]);
  v[0] = _mm_add_epi32(v[0], rnding);
  out[10] = _mm_srai_epi32(v[0], bit);  // buf0[5]

  v[0] = _mm_mullo_epi32(u[5], cospi40);
  v[1] = _mm_mullo_epi32(u[6], cospi24);
  v[0] = _mm_sub_epi32(v[1], v[0]);
  v[0] = _mm_add_epi32(v[0], rnding);
  out[6] = _mm_srai_epi32(v[0], bit);   // buf0[6]

  out[0] = u[0];   // buf0[0]
  out[8] = u[1];   // buf0[1]
  out[4] = u[2];   // buf0[2]
  out[12] = u[3];  // buf0[3]

  // Odd 8 points: 1, 3, ..., 15
  // stage 0
  // stage 1
  u[0] = _mm_add_epi32(in[1], in[15]);
  v[7] = _mm_sub_epi32(in[1], in[15]);  // v[7]
  u[1] = _mm_add_epi32(in[3], in[13]);
  u[6] = _mm_sub_epi32(in[3], in[13]);
  u[2] = _mm_add_epi32(in[5], in[11]);
  u[5] = _mm_sub_epi32(in[5], in[11]);
  u[3] = _mm_add_epi32(in[7], in[9]);
  v[4] = _mm_sub_epi32(in[7], in[9]);   // v[4]

  // stage 2
  v[0] = _mm_add_epi32(u[0], u[3]);
  v[3] = _mm_sub_epi32(u[0], u[3]);
  v[1] = _mm_add_epi32(u[1], u[2]);
  v[2] = _mm_sub_epi32(u[1], u[2]);

  v[5] = _mm_mullo_epi32(u[5], cospim32);
  v[6] = _mm_mullo_epi32(u[6], cospi32);
  v[5] = _mm_add_epi32(v[5], v[6]);
  v[5] = _mm_add_epi32(v[5], rnding);
  v[5] = _mm_srai_epi32(v[5], bit);

  u[0] = _mm_mullo_epi32(u[5], cospi32);
  v[6] = _mm_mullo_epi32(u[6], cospim32);
  v[6] = _mm_sub_epi32(u[0], v[6]);
  v[6] = _mm_add_epi32(v[6], rnding);
  v[6] = _mm_srai_epi32(v[6], bit);

  // stage 3
  // type 0
  v[0] = _mm_mullo_epi32(v[0], cospi32);
  v[1] = _mm_mullo_epi32(v[1], cospi32);
  u[0] = _mm_add_epi32(v[0], v[1]);
  u[0] = _mm_add_epi32(u[0], rnding);
  u[0] = _mm_srai_epi32(u[0], bit);

  u[1] = _mm_sub_epi32(v[0], v[1]);
  u[1] = _mm_add_epi32(u[1], rnding);
  u[1] = _mm_srai_epi32(u[1], bit);

  // type 1
  v[0] = _mm_mullo_epi32(v[2], cospi48);
  v[1] = _mm_mullo_epi32(v[3], cospi16);
  u[2] = _mm_add_epi32(v[0], v[1]);
  u[2] = _mm_add_epi32(u[2], rnding);
  u[2] = _mm_srai_epi32(u[2], bit);

  v[0] = _mm_mullo_epi32(v[2], cospi16);
  v[1] = _mm_mullo_epi32(v[3], cospi48);
  u[3] = _mm_sub_epi32(v[1], v[0]);
  u[3] = _mm_add_epi32(u[3], rnding);
  u[3] = _mm_srai_epi32(u[3], bit);

  u[4] = _mm_add_epi32(v[4], v[5]);
  u[5] = _mm_sub_epi32(v[4], v[5]);
  u[6] = _mm_sub_epi32(v[7], v[6]);
  u[7] = _mm_add_epi32(v[7], v[6]);

  // stage 4
  // stage 5
  v[0] = _mm_mullo_epi32(u[4], cospi56);
  v[1] = _mm_mullo_epi32(u[7], cospi8);
  v[0] = _mm_add_epi32(v[0], v[1]);
  v[0] = _mm_add_epi32(v[0], rnding);
  out[3] = _mm_srai_epi32(v[0], bit);   // buf0[4]

  v[0] = _mm_mullo_epi32(u[4], cospi8);
  v[1] = _mm_mullo_epi32(u[7], cospi56);
  v[0] = _mm_sub_epi32(v[1], v[0]);
  v[0] = _mm_add_epi32(v[0], rnding);
  out[15] = _mm_srai_epi32(v[0], bit);  // buf0[7]

  v[0] = _mm_mullo_epi32(u[5], cospi24);
  v[1] = _mm_mullo_epi32(u[6], cospi40);
  v[0] = _mm_add_epi32(v[0], v[1]);
  v[0] = _mm_add_epi32(v[0], rnding);
  out[11] = _mm_srai_epi32(v[0], bit);  // buf0[5]

  v[0] = _mm_mullo_epi32(u[5], cospi40);
  v[1] = _mm_mullo_epi32(u[6], cospi24);
  v[0] = _mm_sub_epi32(v[1], v[0]);
  v[0] = _mm_add_epi32(v[0], rnding);
  out[7] = _mm_srai_epi32(v[0], bit);   // buf0[6]

  out[1] = u[0];   // buf0[0]
  out[9] = u[1];   // buf0[1]
  out[5] = u[2];   // buf0[2]
  out[13] = u[3];  // buf0[3]
}

static void fadst8x8_sse4_1(__m128i *in, __m128i *out, int bit) {
  const int32_t *cospi = cospi_arr[bit - cos_bit_min];
  const __m128i cospi4 = _mm_set1_epi32(cospi[4]);
  const __m128i cospi60 = _mm_set1_epi32(cospi[60]);
  const __m128i cospi20 = _mm_set1_epi32(cospi[20]);
  const __m128i cospi44 = _mm_set1_epi32(cospi[44]);
  const __m128i cospi36 = _mm_set1_epi32(cospi[36]);
  const __m128i cospi28 = _mm_set1_epi32(cospi[28]);
  const __m128i cospi52 = _mm_set1_epi32(cospi[52]);
  const __m128i cospi12 = _mm_set1_epi32(cospi[12]);
  const __m128i cospi16 = _mm_set1_epi32(cospi[16]);
  const __m128i cospi48 = _mm_set1_epi32(cospi[48]);
  const __m128i cospim48 = _mm_set1_epi32(-cospi[48]);
  const __m128i cospi32 = _mm_set1_epi32(cospi[32]);
  const __m128i rnding = _mm_set1_epi32(1 << (bit - 1));
  const __m128i kZero = _mm_setzero_si128();
  __m128i u[8], v[8], x;

  // Even 8 points: 0, 2, ..., 14
  // stage 0
  // stage 1
  // stage 2
  // (1)
  u[0] = _mm_mullo_epi32(in[14], cospi4);
  x = _mm_mullo_epi32(in[0], cospi60);
  u[0] = _mm_add_epi32(u[0], x);
  u[0] = _mm_add_epi32(u[0], rnding);
  u[0] = _mm_srai_epi32(u[0], bit);

  u[1] = _mm_mullo_epi32(in[14], cospi60);
  x = _mm_mullo_epi32(in[0], cospi4);
  u[1] = _mm_sub_epi32(u[1], x);
  u[1] = _mm_add_epi32(u[1], rnding);
  u[1] = _mm_srai_epi32(u[1], bit);

  // (2)
  u[2] = _mm_mullo_epi32(in[10], cospi20);
  x = _mm_mullo_epi32(in[4], cospi44);
  u[2] = _mm_add_epi32(u[2], x);
  u[2] = _mm_add_epi32(u[2], rnding);
  u[2] = _mm_srai_epi32(u[2], bit);

  u[3] = _mm_mullo_epi32(in[10], cospi44);
  x = _mm_mullo_epi32(in[4], cospi20);
  u[3] = _mm_sub_epi32(u[3], x);
  u[3] = _mm_add_epi32(u[3], rnding);
  u[3] = _mm_srai_epi32(u[3], bit);

  // (3)
  u[4] = _mm_mullo_epi32(in[6], cospi36);
  x = _mm_mullo_epi32(in[8], cospi28);
  u[4] = _mm_add_epi32(u[4], x);
  u[4] = _mm_add_epi32(u[4], rnding);
  u[4] = _mm_srai_epi32(u[4], bit);

  u[5] = _mm_mullo_epi32(in[6], cospi28);
  x = _mm_mullo_epi32(in[8], cospi36);
  u[5] = _mm_sub_epi32(u[5], x);
  u[5] = _mm_add_epi32(u[5], rnding);
  u[5] = _mm_srai_epi32(u[5], bit);

  // (4)
  u[6] = _mm_mullo_epi32(in[2], cospi52);
  x = _mm_mullo_epi32(in[12], cospi12);
  u[6] = _mm_add_epi32(u[6], x);
  u[6] = _mm_add_epi32(u[6], rnding);
  u[6] = _mm_srai_epi32(u[6], bit);

  u[7] = _mm_mullo_epi32(in[2], cospi12);
  x = _mm_mullo_epi32(in[12], cospi52);
  u[7] = _mm_sub_epi32(u[7], x);
  u[7] = _mm_add_epi32(u[7], rnding);
  u[7] = _mm_srai_epi32(u[7], bit);

  // stage 3
  v[0] = _mm_add_epi32(u[0], u[4]);
  v[4] = _mm_sub_epi32(u[0], u[4]);
  v[1] = _mm_add_epi32(u[1], u[5]);
  v[5] = _mm_sub_epi32(u[1], u[5]);
  v[2] = _mm_add_epi32(u[2], u[6]);
  v[6] = _mm_sub_epi32(u[2], u[6]);
  v[3] = _mm_add_epi32(u[3], u[7]);
  v[7] = _mm_sub_epi32(u[3], u[7]);

  // stage 4
  u[0] = v[0];
  u[1] = v[1];
  u[2] = v[2];
  u[3] = v[3];

  u[4] = _mm_mullo_epi32(v[4], cospi16);
  x = _mm_mullo_epi32(v[5], cospi48);
  u[4] = _mm_add_epi32(u[4], x);
  u[4] = _mm_add_epi32(u[4], rnding);
  u[4] = _mm_srai_epi32(u[4], bit);

  u[5] = _mm_mullo_epi32(v[4], cospi48);
  x = _mm_mullo_epi32(v[5], cospi16);
  u[5] = _mm_sub_epi32(u[5], x);
  u[5] = _mm_add_epi32(u[5], rnding);
  u[5] = _mm_srai_epi32(u[5], bit);

  u[6] = _mm_mullo_epi32(v[6], cospim48);
  x = _mm_mullo_epi32(v[7], cospi16);
  u[6] = _mm_add_epi32(u[6], x);
  u[6] = _mm_add_epi32(u[6], rnding);
  u[6] = _mm_srai_epi32(u[6], bit);

  u[7] = _mm_mullo_epi32(v[6], cospi16);
  x = _mm_mullo_epi32(v[7], cospim48);
  u[7] = _mm_sub_epi32(u[7], x);
  u[7] = _mm_add_epi32(u[7], rnding);
  u[7] = _mm_srai_epi32(u[7], bit);

  // stage 5
  v[0] = _mm_add_epi32(u[0], u[2]);
  v[2] = _mm_sub_epi32(u[0], u[2]);
  v[1] = _mm_add_epi32(u[1], u[3]);
  v[3] = _mm_sub_epi32(u[1], u[3]);
  v[4] = _mm_add_epi32(u[4], u[6]);
  v[6] = _mm_sub_epi32(u[4], u[6]);
  v[5] = _mm_add_epi32(u[5], u[7]);
  v[7] = _mm_sub_epi32(u[5], u[7]);

  // stage 6
  u[0] = v[0];
  u[1] = v[1];
  u[4] = v[4];
  u[5] = v[5];

  v[0] = _mm_mullo_epi32(v[2], cospi32);
  x = _mm_mullo_epi32(v[3], cospi32);
  u[2] = _mm_add_epi32(v[0], x);
  u[2] = _mm_add_epi32(u[2], rnding);
  u[2] = _mm_srai_epi32(u[2], bit);

  u[3] = _mm_sub_epi32(v[0], x);
  u[3] = _mm_add_epi32(u[3], rnding);
  u[3] = _mm_srai_epi32(u[3], bit);

  v[0] = _mm_mullo_epi32(v[6], cospi32);
  x = _mm_mullo_epi32(v[7], cospi32);
  u[6] = _mm_add_epi32(v[0], x);
  u[6] = _mm_add_epi32(u[6], rnding);
  u[6] = _mm_srai_epi32(u[6], bit);

  u[7] = _mm_sub_epi32(v[0], x);
  u[7] = _mm_add_epi32(u[7], rnding);
  u[7] = _mm_srai_epi32(u[7], bit);

  // stage 7
  out[0] = u[0];
  out[2] = _mm_sub_epi32(kZero, u[4]);
  out[4] = u[6];
  out[6] = _mm_sub_epi32(kZero, u[2]);
  out[8] = u[3];
  out[10] = _mm_sub_epi32(kZero, u[7]);
  out[12] = u[5];
  out[14] = _mm_sub_epi32(kZero, u[1]);

  // Odd 8 points: 1, 3, ..., 15
  // stage 0
  // stage 1
  // stage 2
  // (1)
  u[0] = _mm_mullo_epi32(in[15], cospi4);
  x = _mm_mullo_epi32(in[1], cospi60);
  u[0] = _mm_add_epi32(u[0], x);
  u[0] = _mm_add_epi32(u[0], rnding);
  u[0] = _mm_srai_epi32(u[0], bit);

  u[1] = _mm_mullo_epi32(in[15], cospi60);
  x = _mm_mullo_epi32(in[1], cospi4);
  u[1] = _mm_sub_epi32(u[1], x);
  u[1] = _mm_add_epi32(u[1], rnding);
  u[1] = _mm_srai_epi32(u[1], bit);

  // (2)
  u[2] = _mm_mullo_epi32(in[11], cospi20);
  x = _mm_mullo_epi32(in[5], cospi44);
  u[2] = _mm_add_epi32(u[2], x);
  u[2] = _mm_add_epi32(u[2], rnding);
  u[2] = _mm_srai_epi32(u[2], bit);

  u[3] = _mm_mullo_epi32(in[11], cospi44);
  x = _mm_mullo_epi32(in[5], cospi20);
  u[3] = _mm_sub_epi32(u[3], x);
  u[3] = _mm_add_epi32(u[3], rnding);
  u[3] = _mm_srai_epi32(u[3], bit);

  // (3)
  u[4] = _mm_mullo_epi32(in[7], cospi36);
  x = _mm_mullo_epi32(in[9], cospi28);
  u[4] = _mm_add_epi32(u[4], x);
  u[4] = _mm_add_epi32(u[4], rnding);
  u[4] = _mm_srai_epi32(u[4], bit);

  u[5] = _mm_mullo_epi32(in[7], cospi28);
  x = _mm_mullo_epi32(in[9], cospi36);
  u[5] = _mm_sub_epi32(u[5], x);
  u[5] = _mm_add_epi32(u[5], rnding);
  u[5] = _mm_srai_epi32(u[5], bit);

  // (4)
  u[6] = _mm_mullo_epi32(in[3], cospi52);
  x = _mm_mullo_epi32(in[13], cospi12);
  u[6] = _mm_add_epi32(u[6], x);
  u[6] = _mm_add_epi32(u[6], rnding);
  u[6] = _mm_srai_epi32(u[6], bit);

  u[7] = _mm_mullo_epi32(in[3], cospi12);
  x = _mm_mullo_epi32(in[13], cospi52);
  u[7] = _mm_sub_epi32(u[7], x);
  u[7] = _mm_add_epi32(u[7], rnding);
  u[7] = _mm_srai_epi32(u[7], bit);

  // stage 3
  v[0] = _mm_add_epi32(u[0], u[4]);
  v[4] = _mm_sub_epi32(u[0], u[4]);
  v[1] = _mm_add_epi32(u[1], u[5]);
  v[5] = _mm_sub_epi32(u[1], u[5]);
  v[2] = _mm_add_epi32(u[2], u[6]);
  v[6] = _mm_sub_epi32(u[2], u[6]);
  v[3] = _mm_add_epi32(u[3], u[7]);
  v[7] = _mm_sub_epi32(u[3], u[7]);

  // stage 4
  u[0] = v[0];
  u[1] = v[1];
  u[2] = v[2];
  u[3] = v[3];

  u[4] = _mm_mullo_epi32(v[4], cospi16);
  x = _mm_mullo_epi32(v[5], cospi48);
  u[4] = _mm_add_epi32(u[4], x);
  u[4] = _mm_add_epi32(u[4], rnding);
  u[4] = _mm_srai_epi32(u[4], bit);

  u[5] = _mm_mullo_epi32(v[4], cospi48);
  x = _mm_mullo_epi32(v[5], cospi16);
  u[5] = _mm_sub_epi32(u[5], x);
  u[5] = _mm_add_epi32(u[5], rnding);
  u[5] = _mm_srai_epi32(u[5], bit);

  u[6] = _mm_mullo_epi32(v[6], cospim48);
  x = _mm_mullo_epi32(v[7], cospi16);
  u[6] = _mm_add_epi32(u[6], x);
  u[6] = _mm_add_epi32(u[6], rnding);
  u[6] = _mm_srai_epi32(u[6], bit);

  u[7] = _mm_mullo_epi32(v[6], cospi16);
  x = _mm_mullo_epi32(v[7], cospim48);
  u[7] = _mm_sub_epi32(u[7], x);
  u[7] = _mm_add_epi32(u[7], rnding);
  u[7] = _mm_srai_epi32(u[7], bit);

  // stage 5
  v[0] = _mm_add_epi32(u[0], u[2]);
  v[2] = _mm_sub_epi32(u[0], u[2]);
  v[1] = _mm_add_epi32(u[1], u[3]);
  v[3] = _mm_sub_epi32(u[1], u[3]);
  v[4] = _mm_add_epi32(u[4], u[6]);
  v[6] = _mm_sub_epi32(u[4], u[6]);
  v[5] = _mm_add_epi32(u[5], u[7]);
  v[7] = _mm_sub_epi32(u[5], u[7]);

  // stage 6
  u[0] = v[0];
  u[1] = v[1];
  u[4] = v[4];
  u[5] = v[5];

  v[0] = _mm_mullo_epi32(v[2], cospi32);
  x = _mm_mullo_epi32(v[3], cospi32);
  u[2] = _mm_add_epi32(v[0], x);
  u[2] = _mm_add_epi32(u[2], rnding);
  u[2] = _mm_srai_epi32(u[2], bit);

  u[3] = _mm_sub_epi32(v[0], x);
  u[3] = _mm_add_epi32(u[3], rnding);
  u[3] = _mm_srai_epi32(u[3], bit);

  v[0] = _mm_mullo_epi32(v[6], cospi32);
  x = _mm_mullo_epi32(v[7], cospi32);
  u[6] = _mm_add_epi32(v[0], x);
  u[6] = _mm_add_epi32(u[6], rnding);
  u[6] = _mm_srai_epi32(u[6], bit);

  u[7] = _mm_sub_epi32(v[0], x);
  u[7] = _mm_add_epi32(u[7], rnding);
  u[7] = _mm_srai_epi32(u[7], bit);

  // stage 7
  out[1] = u[0];
  out[3] = _mm_sub_epi32(kZero, u[4]);
  out[5] = u[6];
  out[7] = _mm_sub_epi32(kZero, u[2]);
  out[9] = u[3];
  out[11] = _mm_sub_epi32(kZero, u[7]);
  out[13] = u[5];
  out[15] = _mm_sub_epi32(kZero, u[1]);
}

void vp10_fwd_txfm2d_8x8_sse4_1(const int16_t *input, tran_low_t *coeff,
                                int stride, int tx_type, int bd) {
  __m128i in[16], out[16];
  const TXFM_2D_CFG *cfg = NULL;

  switch (tx_type) {
    case DCT_DCT:
      cfg = &fwd_txfm_2d_cfg_dct_dct_8;
      load_buffer_8x8(input, in, stride, 0, 0, cfg->shift[0]);
      fdct8x8_sse4_1(in, out, cfg->cos_bit_col[2]);
      col_txfm_8x8_rounding(out, -cfg->shift[1]);
      transpose_8x8(out);
      fdct8x8_sse4_1(out, in, cfg->cos_bit_row[2]);
      transpose_8x8(in);
      write_buffer_8x8(in, coeff);
      break;
    case ADST_DCT:
      cfg = &fwd_txfm_2d_cfg_adst_dct_8;
      load_buffer_8x8(input, in, stride, 0, 0, cfg->shift[0]);
      fadst8x8_sse4_1(in, out, cfg->cos_bit_col[2]);
      col_txfm_8x8_rounding(out, -cfg->shift[1]);
      transpose_8x8(out);
      fdct8x8_sse4_1(out, in, cfg->cos_bit_row[2]);
      transpose_8x8(in);
      write_buffer_8x8(in, coeff);
      break;
    case DCT_ADST:
      cfg = &fwd_txfm_2d_cfg_dct_adst_8;
      load_buffer_8x8(input, in, stride, 0, 0, cfg->shift[0]);
      fdct8x8_sse4_1(in, out, cfg->cos_bit_col[2]);
      col_txfm_8x8_rounding(out, -cfg->shift[1]);
      transpose_8x8(out);
      fadst8x8_sse4_1(out, in, cfg->cos_bit_row[2]);
      transpose_8x8(in);
      write_buffer_8x8(in, coeff);
      break;
    case ADST_ADST:
      cfg = &fwd_txfm_2d_cfg_adst_adst_8;
      load_buffer_8x8(input, in, stride, 0, 0, cfg->shift[0]);
      fadst8x8_sse4_1(in, out, cfg->cos_bit_col[2]);
      col_txfm_8x8_rounding(out, -cfg->shift[1]);
      transpose_8x8(out);
      fadst8x8_sse4_1(out, in, cfg->cos_bit_row[2]);
      transpose_8x8(in);
      write_buffer_8x8(in, coeff);
      break;
    default:
      assert(0);
  }
  (void)bd;
}
