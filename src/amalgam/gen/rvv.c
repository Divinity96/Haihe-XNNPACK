// Copyright 2021 Google LLC
//
// This source code is licensed under the BSD-style license found in the
// LICENSE file in the root directory of this source tree.

#include <assert.h>
#include <math.h>
#include <time.h>

//#include <riscv_vector.h>
#include "riscv_v_071_fix.h"

#include <xnnpack/common.h>
#include <xnnpack/intrinsics-polyfill.h>
#include <xnnpack/math.h>
#include <xnnpack/raddstoreexpminusmax.h>
#include <xnnpack/vbinary.h>
#include <xnnpack/vunary.h>

static inline vfloat32m4_t eval_poly_horner(vfloat32m4_t x,
                                                  float c6, float c5,
                                                  float c4, float c3, float c2,
                                                  float c1, float c0, size_t vl) {
  vfloat32m4_t z;
  vfloat32m4_t y = __riscv_vfmv_v_f_f32m4(c5, vl);
  y = __riscv_vfmacc_vf_f32m4(y, c6, x, vl);

  z = __riscv_vfmv_v_f_f32m4(c4, vl);
  y = __riscv_vfmadd_vv_f32m4(y, x, z, vl);

  z = __riscv_vfmv_v_f_f32m4(c3, vl);
  y = __riscv_vfmadd_vv_f32m4(y, x, z, vl);

  z = __riscv_vfmv_v_f_f32m4(c2, vl);
  y = __riscv_vfmadd_vv_f32m4(y, x, z, vl);

  z = __riscv_vfmv_v_f_f32m4(c1, vl);
  y = __riscv_vfmadd_vv_f32m4(y, x, z, vl);

  z = __riscv_vfmv_v_f_f32m4(c0, vl);
  y = __riscv_vfmadd_vv_f32m4(y, x, z, vl);
  return y;
}

/// @brief Computes the exponential function on vector of float32 values with a
/// 1-ULP error bound in the range [-87, 0]. Smaller inputs are flushed to
/// exp(-0x1.5d589ep6f) ~= 0x1.6a0a64p-127f while the result is undefined for
/// inputs greater than zero as well as NaNs.
///
/// This function is intended for use in computing softmax, whose inputs are
/// pre-normalized by subtracting the maximum, resulting in inputs in (-inf, 0).
/// One of these inputs will contribute exp(0) = 1 to the final sum, so any
/// inputs flushed upwards to -0x1.5d589ep6f and thus contributing at most
/// 0x1.6a0a64p-127f to the total, will not result of softmax unless at least
/// ~2^100 of them are summed in ascending order.
///
/// Exploitation of these properties results in a faster exponential by avoiding
/// the need to handle edge cases that arise from very large or small exponents.
///
/// @param[in] x Input vector of float32 values
/// @param[in] vl Length of vector x
/// @return Result of applying softexp() to elements of x
static inline vfloat32m4_t softexp_f32m4(
    vfloat32m4_t x, size_t vl,
    const union xnn_f32_expminus_params params[restrict XNN_MIN_ELEMENTS(1)]) {
  // Ensure that q = RN(x/log(2)) >= e_min, so that 2^q can be computed safely
  // with a simple shift into the exponent field.
  // xmin = round(-126.5 * log(2), single, RU) ~ -87.68311309814453125

  const float xmin = params->rvv_rr2_p6.x_min;
  const float r_ln2f = params->rvv_rr2_p6.log2e;
  const float l2uf = params->rvv_rr2_p6.ln2_hi;
  const float l2lf = params->rvv_rr2_p6.ln2_lo;
  const float c6 = params->rvv_rr2_p6.c6;
  const float c5 = params->rvv_rr2_p6.c5;
  const float c4 = params->rvv_rr2_p6.c4;
  const float c3 = params->rvv_rr2_p6.c3;
  const float c2 = params->rvv_rr2_p6.c2;

  // const float xmin = -0x1.5ebb82p6;
  x = __riscv_vfmax_vf_f32m4(x, xmin, vl);

  // 0. Reduction x = s * q ln(2)
  // const float r_ln2f = 0x1.715476p0f;  // single(1/log(2));
  // const float l2uf = 0x1.62e4p-1f;     // round(log(2), 24-8, RN);
  // const float l2lf = 0x1.7f7d1cp-20f;  // round(log(2) - l2uf, single, RN);
  vfloat32m4_t v = __riscv_vfmul_vf_f32m4(x, r_ln2f, vl);

  vint16m2_t q = __riscv_vfncvt_x_f_w_i16m2(v, vl);
  vfloat32m4_t z = __riscv_vfwcvt_f_x_v_f32m4(q, vl);

  // Use Cody-Waite range reduction method (note two constants to represent log(2)) to improve accuracy.
  vfloat32m4_t s = __riscv_vfnmsac_vf_f32m4(x, l2uf, z, vl);
  s = __riscv_vfnmsac_vf_f32m4(s, l2lf, z, vl);

  // 1. Approximate e^s by degree-6 polynomial approximation
  vfloat32m4_t u = eval_poly_horner(s, c6, c5, c4, c3, c2, 1.0f, 1.0f, vl);

  // 2. Reconstruction: compute u = u*2^q
  const int16_t p = (24 - 1);
  const int16_t bias = (128 - 1);
  vint32m4_t qw = __riscv_vwadd_vx_i32m4(q, bias, vl);
  vint32m4_t qq = __riscv_vsll_vx_i32m4(qw, p, vl);
  vfloat32m4_t qf = __riscv_vreinterpret_v_i32m4_f32m4(qq);
  u = __riscv_vfmul_vv_f32m4(u, qf, vl);
  return u;
}

void xnn_f32_raddstoreexpminusmax_ukernel__rvv_rr2_p6_u4v(
    size_t batch,
    const float* input,
    const float* max,
    float* output,
    float* sum,
    const union xnn_f32_expminus_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(batch != 0);
  assert(batch % sizeof(float) == 0);
  assert(input != NULL);
  assert(max != NULL);
  assert(output != NULL);
  assert(sum != NULL);

  size_t n = batch >> 2;
  size_t avl = n;
  size_t vl = __riscv_vsetvl_e32m4(n);

  vfloat32m4_t vsum = __riscv_vfmv_v_f_f32m4(0.0f, vl);
  do {
    vl = __riscv_vsetvl_e32m4(avl);
    avl -= vl;
    vfloat32m4_t vx = __riscv_vle32_v_f32m4(input, vl);
    vx = __riscv_vfsub_vf_f32m4(vx, *max, vl);
    input += vl;
    vfloat32m4_t vexp = softexp_f32m4(vx, vl, params);
    __riscv_vse32_v_f32m4(output, vexp, vl);
    output += vl;
    vsum = __riscv_vfadd_vv_f32m4_tu(vsum, vsum, vexp, vl);
  } while(avl > 0);

  //vfloat32m1_t v0 = __riscv_vfmv_s_f_f32m1(0.0f, 1);
  //*sum = __riscv_vfmv_f_s_f32m1_f32(__riscv_vfredusum_vs_f32m4_f32m1(vsum, v0, n));
  vfloat32m1_t v0, v1;
  v0 = vfmv_s_f_f32m1(v0, 0.0f, 1);
  v1 = vfredosum_vs_f32m4_f32m1(v1, vsum, v0, n);
  *sum = __riscv_vfmv_f_s_f32m1_f32(v1);
}

void xnn_f32_rmax_ukernel__rvv_u8v(
    size_t batch,
    const float* input,
    float* output,
    const union xnn_f32_default_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(batch != 0);
  assert(batch % sizeof(float) == 0);
  assert(input != NULL);
  assert(output != NULL);

  size_t N = batch >> 2;
  size_t avl;
  size_t vl = __riscv_vsetvl_e32m8(N);

  vfloat32m8_t t0 = __riscv_vle32_v_f32m8(input, vl);
  input += vl;

  for (avl = N - vl; avl; avl -= vl, input += vl) {
    vl = __riscv_vsetvl_e32m8(avl);
    vfloat32m8_t vec = __riscv_vle32_v_f32m8(input, vl);
    t0 = __riscv_vfmax_vv_f32m8_tu(t0, t0, vec, vl);
  }

  //vfloat32m1_t fmax = __riscv_vfmv_s_f_f32m1(-INFINITY, 1);
  //output[0] = __riscv_vfmv_f_s_f32m1_f32(__riscv_vfredmax_vs_f32m8_f32m1(t0, fmax, N));
  vfloat32m1_t fmax, v0;
  fmax = vfmv_s_f_f32m1(fmax, -INFINITY, 1);
  v0 = vfredmax_vs_f32m8_f32m1(v0, t0, fmax, N);
  output[0] = __riscv_vfmv_f_s_f32m1_f32(v0);
}

void xnn_f32_rminmax_ukernel__rvv_u8v(
    size_t batch,
    const float* input,
    float* output,
    const union xnn_f32_default_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(batch != 0);
  assert(batch % sizeof(float) == 0);
  assert(input != NULL);
  assert(output != NULL);

  size_t N = batch >> 2;
  size_t avl;
  size_t vl = __riscv_vsetvl_e32m8(N);

  vfloat32m8_t t0 = __riscv_vle32_v_f32m8(input, vl);
  input += vl;
  vfloat32m8_t t1 = __riscv_vmv_v_v_f32m8(t0, vl);

  for (avl = N - vl; avl; avl -= vl, input += vl) {
    vl = __riscv_vsetvl_e32m8(avl);
    vfloat32m8_t vec = __riscv_vle32_v_f32m8(input, vl);
    t0 = __riscv_vfmin_vv_f32m8_tu(t0, t0, vec, vl);
    t1 = __riscv_vfmax_vv_f32m8_tu(t1, t1, vec, vl);
  }

  //vfloat32m1_t fmin = __riscv_vfmv_s_f_f32m1(INFINITY, 1);
  //vfloat32m1_t fmax = __riscv_vfmv_s_f_f32m1(-INFINITY, 1);
  //output[0] = __riscv_vfmv_f_s_f32m1_f32(__riscv_vfredmin_vs_f32m8_f32m1(t0, fmin, N));
  //output[1] = __riscv_vfmv_f_s_f32m1_f32(__riscv_vfredmax_vs_f32m8_f32m1(t1, fmax, N));
  vfloat32m1_t fmin, fmax, v0, v1;
  fmin = vfmv_s_f_f32m1(fmin, INFINITY, 1);
  fmax = vfmv_s_f_f32m1(fmax, -INFINITY, 1);
  v0 = vfredmin_vs_f32m8_f32m1(v0, t0, fmin, N);
  v1 = vfredmax_vs_f32m8_f32m1(v1, t1, fmax, N);
  output[0] = __riscv_vfmv_f_s_f32m1_f32(v0);
  output[1] = __riscv_vfmv_f_s_f32m1_f32(v1);
}

void xnn_qs8_vmul_minmax_fp32_ukernel__rvv_u2v(
    size_t batch,
    const int8_t* input_a,
    const int8_t* input_b,
    int8_t* output,
    const union xnn_qs8_mul_minmax_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(batch != 0);
  assert(batch % sizeof(int8_t) == 0);
  assert(input_a != NULL);
  assert(input_b != NULL);
  assert(output != NULL);

  const int32_t a_zero_point = params->fp32_scalar.a_zero_point;
  const int32_t b_zero_point = params->fp32_scalar.b_zero_point;
  const float scale = params->fp32_scalar.scale;
  const float output_min_less_zero_point = params->fp32_scalar.output_min_less_zero_point;
  const float output_max_less_zero_point = params->fp32_scalar.output_max_less_zero_point;
  const float magic_bias = params->fp32_scalar.magic_bias;
  const int32_t magic_bias_less_output_zero_point = params->fp32_scalar.magic_bias_less_output_zero_point;

  do {
    const size_t n = __riscv_vsetvl_e8m2(batch);

    vint8m2_t in_a_i8v = __riscv_vle8_v_i8m2(input_a, n); input_a += n;
    vint8m2_t in_b_i8v = __riscv_vle8_v_i8m2(input_b, n); input_b += n;
    vint16m4_t a_i16v = __riscv_vwsub_vx_i16m4(in_a_i8v, a_zero_point, n);
    vint16m4_t b_i16v = __riscv_vwsub_vx_i16m4(in_b_i8v, b_zero_point, n);

    vint32m8_t acc_i32v = __riscv_vwmul_vv_i32m8(a_i16v, b_i16v, n);
    vfloat32m8_t acc_f32v = __riscv_vfcvt_f_x_v_f32m8(acc_i32v, n);
    acc_f32v = __riscv_vfmul_vf_f32m8(acc_f32v, scale, n);
    acc_f32v = __riscv_vfmin_vf_f32m8(__riscv_vfmax_vf_f32m8(acc_f32v, output_min_less_zero_point, n), output_max_less_zero_point, n);
    acc_f32v = __riscv_vfadd_vf_f32m8(acc_f32v, magic_bias, n);

    vint32m8_t out_i32v = __riscv_vfcvt_x_f_v_i32m8(acc_f32v, n);
    out_i32v = __riscv_vsub_vx_i32m8(out_i32v, magic_bias_less_output_zero_point, n);
    vint16m4_t out_i16v = __riscv_vncvt_x_x_w_i16m4(out_i32v, n);
    vint8m2_t out_i8v = __riscv_vncvt_x_x_w_i8m2(out_i16v, n);
    __riscv_vse8_v_i8m2(output, out_i8v, n); output += n;

    batch -= n;
  } while (batch != 0);
}

void xnn_qs8_vmulc_minmax_fp32_ukernel__rvv_u2v(
    size_t batch,
    const int8_t* input_a,
    const int8_t* input_b,
    int8_t* output,
    const union xnn_qs8_mul_minmax_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(batch != 0);
  assert(batch % sizeof(int8_t) == 0);
  assert(input_a != NULL);
  assert(input_b != NULL);
  assert(output != NULL);

  const int32_t a_zero_point = params->fp32_scalar.a_zero_point;
  const float scale = params->fp32_scalar.scale;
  const float output_min_less_zero_point = params->fp32_scalar.output_min_less_zero_point;
  const float output_max_less_zero_point = params->fp32_scalar.output_max_less_zero_point;
  const float magic_bias = params->fp32_scalar.magic_bias;
  const int32_t magic_bias_less_output_zero_point = params->fp32_scalar.magic_bias_less_output_zero_point;
  const int32_t vb = (int32_t) *input_b - params->fp32_scalar.b_zero_point;

  do {
    const size_t n = __riscv_vsetvl_e8m2(batch);

    vint8m2_t in_a_i8v = __riscv_vle8_v_i8m2(input_a, n); input_a += n;
    vint16m4_t a_i16v = __riscv_vwsub_vx_i16m4(in_a_i8v, a_zero_point, n);

    vint32m8_t acc_i32v = __riscv_vwmul_vx_i32m8(a_i16v, vb, n);
    vfloat32m8_t acc_f32v = __riscv_vfcvt_f_x_v_f32m8(acc_i32v, n);
    acc_f32v = __riscv_vfmul_vf_f32m8(acc_f32v, scale, n);
    acc_f32v = __riscv_vfmin_vf_f32m8(__riscv_vfmax_vf_f32m8(acc_f32v, output_min_less_zero_point, n), output_max_less_zero_point, n);
    acc_f32v = __riscv_vfadd_vf_f32m8(acc_f32v, magic_bias, n);

    vint32m8_t out_i32v = __riscv_vfcvt_x_f_v_i32m8(acc_f32v, n);
    out_i32v = __riscv_vsub_vx_i32m8(out_i32v, magic_bias_less_output_zero_point, n);
    vint16m4_t out_i16v = __riscv_vncvt_x_x_w_i16m4(out_i32v, n);
    vint8m2_t out_i8v = __riscv_vncvt_x_x_w_i8m2(out_i16v, n);
    __riscv_vse8_v_i8m2(output, out_i8v, n); output += n;

    batch -= n;
  } while (batch != 0);
}

void xnn_qu8_vmul_minmax_fp32_ukernel__rvv_u2v(
    size_t batch,
    const uint8_t* input_a,
    const uint8_t* input_b,
    uint8_t* output,
    const union xnn_qu8_mul_minmax_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(batch != 0);
  assert(batch % sizeof(uint8_t) == 0);
  assert(input_a != NULL);
  assert(input_b != NULL);
  assert(output != NULL);

  const int32_t a_zero_point = params->fp32_scalar.a_zero_point;
  const int32_t b_zero_point = params->fp32_scalar.b_zero_point;
  const float scale = params->fp32_scalar.scale;
  const float output_min_less_zero_point = params->fp32_scalar.output_min_less_zero_point;
  const float output_max_less_zero_point = params->fp32_scalar.output_max_less_zero_point;
  const float magic_bias = params->fp32_scalar.magic_bias;
  const int32_t magic_bias_less_output_zero_point = params->fp32_scalar.magic_bias_less_output_zero_point;

  do {
    const size_t n = __riscv_vsetvl_e8m2(batch);

    vuint8m2_t in_a_u8v = __riscv_vle8_v_u8m2(input_a, n); input_a += n;
    vuint8m2_t in_b_u8v = __riscv_vle8_v_u8m2(input_b, n); input_b += n;
    vuint16m4_t a_u16v = __riscv_vwsubu_vx_u16m4(in_a_u8v, a_zero_point, n);
    vuint16m4_t b_u16v = __riscv_vwsubu_vx_u16m4(in_b_u8v, b_zero_point, n);
    vint16m4_t a_i16v = __riscv_vreinterpret_v_u16m4_i16m4(a_u16v);
    vint16m4_t b_i16v = __riscv_vreinterpret_v_u16m4_i16m4(b_u16v);

    vint32m8_t acc_i32v = __riscv_vwmul_vv_i32m8(a_i16v, b_i16v, n);
    vfloat32m8_t acc_f32v = __riscv_vfcvt_f_x_v_f32m8(acc_i32v, n);
    acc_f32v = __riscv_vfmul_vf_f32m8(acc_f32v, scale, n);
    acc_f32v = __riscv_vfmin_vf_f32m8(__riscv_vfmax_vf_f32m8(acc_f32v, output_min_less_zero_point, n), output_max_less_zero_point, n);
    acc_f32v = __riscv_vfadd_vf_f32m8(acc_f32v, magic_bias, n);

    vuint32m8_t out_u32v = __riscv_vfcvt_xu_f_v_u32m8(acc_f32v, n);
    out_u32v = __riscv_vsub_vx_u32m8(out_u32v, magic_bias_less_output_zero_point, n);
    vuint16m4_t out_u16v = __riscv_vncvt_x_x_w_u16m4(out_u32v, n);
    vuint8m2_t out_u8v = __riscv_vncvt_x_x_w_u8m2(out_u16v, n);
    __riscv_vse8_v_u8m2(output, out_u8v, n); output += n;

    batch -= n;
  } while (batch != 0);
}

void xnn_qu8_vmulc_minmax_fp32_ukernel__rvv_u2v(
    size_t batch,
    const uint8_t* input_a,
    const uint8_t* input_b,
    uint8_t* output,
    const union xnn_qu8_mul_minmax_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(batch != 0);
  assert(batch % sizeof(uint8_t) == 0);
  assert(input_a != NULL);
  assert(input_b != NULL);
  assert(output != NULL);

  const int32_t a_zero_point = params->fp32_scalar.a_zero_point;
  const float scale = params->fp32_scalar.scale;
  const float output_min_less_zero_point = params->fp32_scalar.output_min_less_zero_point;
  const float output_max_less_zero_point = params->fp32_scalar.output_max_less_zero_point;
  const float magic_bias = params->fp32_scalar.magic_bias;
  const int32_t magic_bias_less_output_zero_point = params->fp32_scalar.magic_bias_less_output_zero_point;
  const int32_t vb = (int32_t) *input_b - params->fp32_scalar.b_zero_point;

  do {
    const size_t n = __riscv_vsetvl_e8m2(batch);

    vuint8m2_t in_a_u8v = __riscv_vle8_v_u8m2(input_a, n); input_a += n;
    vuint16m4_t a_u16v = __riscv_vwsubu_vx_u16m4(in_a_u8v, a_zero_point, n);
    vint16m4_t a_i16v = __riscv_vreinterpret_v_u16m4_i16m4(a_u16v);

    vint32m8_t acc_i32v = __riscv_vwmul_vx_i32m8(a_i16v, vb, n);
    vfloat32m8_t acc_f32v = __riscv_vfcvt_f_x_v_f32m8(acc_i32v, n);
    acc_f32v = __riscv_vfmul_vf_f32m8(acc_f32v, scale, n);
    acc_f32v = __riscv_vfmin_vf_f32m8(__riscv_vfmax_vf_f32m8(acc_f32v, output_min_less_zero_point, n), output_max_less_zero_point, n);
    acc_f32v = __riscv_vfadd_vf_f32m8(acc_f32v, magic_bias, n);

    vuint32m8_t out_u32v = __riscv_vfcvt_xu_f_v_u32m8(acc_f32v, n);
    out_u32v = __riscv_vsub_vx_u32m8(out_u32v, magic_bias_less_output_zero_point, n);
    vuint16m4_t out_u16v = __riscv_vncvt_x_x_w_u16m4(out_u32v, n);
    vuint8m2_t out_u8v = __riscv_vncvt_x_x_w_u8m2(out_u16v, n);
    __riscv_vse8_v_u8m2(output, out_u8v, n); output += n;

    batch -= n;
  } while (batch != 0);
}

void xnn_f32_vlrelu_ukernel__rvv_u8v(
    size_t batch,
    const float* input,
    float* output,
    const union xnn_f32_lrelu_params params[restrict XNN_MIN_ELEMENTS(1)])
{
	assert(batch != 0);
	assert(batch % sizeof(float) == 0);
	assert(input != NULL);
	assert(output != NULL);

	const float vslope = params->scalar.slope;
	size_t size = batch / sizeof(float);

	do {
		const size_t n = vsetvl_e32m8(size);

		vfloat32m8_t in_u8v = vle32_v_f32m8(input, n);
		input += n;
		vbool4_t mask = vmflt_vf_f32m8_b4(in_u8v, .0f, n);
		vfloat32m8_t out_u8v = vfmul_vf_f32m8_m(mask, in_u8v, in_u8v, vslope, n);
		//vfloat32m8_t out_u8v = vfmax_vf_f32m8(in_u8v, .0f, n);
		vse32_v_f32m8(output, out_u8v, n);

		output += n;
		size -= n;
	} while (size > 0);
}


void xnn_f32_gemm_ukernel_1x4__rvv_u1v(
    size_t mr, // max row
    size_t nc, // next col
    size_t kc, // dimention inside
    const float* restrict a, // pointer to matrix A
    size_t a_stride, // row direction span for A, num of elem which need to skip from begin of one row to next row
    const float* restrict w, // pointer to matrix B
    float* restrict c, // pointer to matrix C, the result of AxB
    size_t cm_stride, // row direction span for C
    size_t cn_stride, // col direction span for C
    const union xnn_f32_default_params params[restrict XNN_MIN_ELEMENTS(1)]) //some default param
{
	assert(mr != 0);
	assert(mr <= 1); // max process 1 row
	assert(nc != 0);
	assert(kc != 0);
	assert(kc % sizeof(float) == 0);
	assert(a != NULL);
	assert(w != NULL);
	assert(c != NULL);

	const float* a0 = a; // matrix a 0th row pointer
	float* c0 = c; // 0th row start pointer
	size_t kcl = kc / sizeof(float);

	do {
		size_t vl = vsetvl_e32m1(nc); // vector length
		vfloat32m1_t vacc = vle32_v_f32m1(w, vl); // 1st row count
		w += vl;
		for(size_t k = 0; k < kcl ; k++){
			vfloat32m1_t vw = vle32_v_f32m1(w, vl);
			w += vl;
			vacc = vfmacc_vf_f32m1(vacc, *a0, vw, vl); // update 1st row count
			a0++;
		}
		vse32_v_f32m1(c0, vacc, vl); // store 1st row result
		if(nc >= 4){
      		c0 = (float*) ((uintptr_t) c0 + cn_stride); // update 1st row matrix C pointer
      		a0 = (const void*) ((uintptr_t) a0 - kc); // update 1st row matrix A pointer
		}
		nc -= vl;
	} while (nc != 0);
}

void xnn_f32_gemm_ukernel_2x4__rvv_u1v(
    size_t mr,
    size_t nc,
    size_t kc,
    const float* restrict a,
    size_t a_stride,
    const float* restrict w,
    float* restrict c,
    size_t cm_stride,
    size_t cn_stride,
    const union xnn_f32_default_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(mr != 0);
  assert(mr <= 2); // max process 2 row
  assert(nc != 0);
  assert(kc != 0);
  assert(kc % sizeof(float) == 0);
  assert(a != NULL);
  assert(w != NULL);
  assert(c != NULL);

  const float* a0 = a; // matrix a row 0 pointer
  const float* a1 = a + a_stride; // matrix a row 1 pointer
  float* c0 = c; // row 0 start pointer
  float* c1 = c + cm_stride; // row 1 start pointer
  size_t kcl = kc / sizeof(float);

  do {
    size_t vl = vsetvl_e32m1(nc);
    vfloat32m1_t vacc0 = vfsub_vv_f32m1(vle32_v_f32m1(c0, vl), vle32_v_f32m1(c0, vl), vl); // 0th row count
    vfloat32m1_t vacc1 = vfsub_vv_f32m1(vle32_v_f32m1(c1, vl), vle32_v_f32m1(c1, vl), vl); // 1st row count
    w += vl;
    for(size_t k = 0; k < kcl ; k++){
      vfloat32m1_t va0 = vfmv_v_f_f32m1(*a0, vl); // load 0th row of matrix A
      vfloat32m1_t va1 = vfmv_v_f_f32m1(*a1, vl); // load 1st row of matrix A
      vfloat32m1_t vw = vle32_v_f32m1(w, vl); // load w
      vacc0 = vfmacc_vv_f32m1(vacc0, va0, vw, vl); // update 0th row count
      vacc1 = vfmacc_vv_f32m1(vacc1, va1, vw, vl); // update 1st row count
      a0++;
      a1++;
      w += vl; // 【修改位置】移动权重矩阵w的指针，应在内循环外
    }
    vse32_v_f32m1(c0, vacc0, vl); // store 0th row result
    vse32_v_f32m1(c1, vacc1, vl); // store 1st row result
    c0 += cn_stride; // update 0th row matrix C pointer
    c1 += cn_stride; // update 1st row matrix C pointer
    a0 = a; // reset 0th row matrix A pointer
    a1 = a + a_stride; // reset 1st row matrix A pointer
    nc -= vl;

  } while (nc != 0);
}

void xnn_f32_gemm_ukernel_4x4__rvv_u1v(
  size_t mr,
  size_t nc,
  size_t kc,
  const float* restrict a,
  size_t a_stride,
  const float* restrict w,
  float* restrict c,
  size_t cm_stride,
  size_t cn_stride,
  const union xnn_f32_default_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(mr != 0);
  assert(mr <= 4); // Corrected max process 4 rows
  assert(nc != 0);
  assert(kc != 0);
  assert(kc % sizeof(float) == 0);
  assert(a != NULL);
  assert(w != NULL);
  assert(c != NULL);

  // each row
  for (size_t m = 0; m < mr; ++m) {
    const float* a0 = a + m * a_stride; // read matrix a by row
    float* c0 = c + m * cm_stride; // update matrix c by row
    size_t kcl = kc / sizeof(float);

    // Reset nc for each row
    size_t current_nc = nc;
    do {
      size_t vl = vsetvl_e32m1(current_nc);
      vfloat32m1_t vacc = vfsub_vv_f32m1(vle32_v_f32m1(c0, vl), vle32_v_f32m1(c0, vl), vl); // Correctly initialize vacc
      for(size_t k = 0; k < kcl ; k++){
        vfloat32m1_t vw = vle32_v_f32m1(w, vl); // load vector w correctly
        w += vl;
        vacc = vfmacc_vf_f32m1(vacc, *a0++, vw, vl); // multiplication and accumulation
      }
      vse32_v_f32m1(c0, vacc, vl); // store result to matrix c
      c0 += cn_stride; // update c0 pointer to write to the next 4 columns correctly
      a0 -= kc; // reset a0 pointer to return to the beginning of the current row correctly
      current_nc -= vl;
    } while (current_nc != 0);

    // Reset weight pointer for each row
    w -= kcl * vl;
  }
}

void xnn_f32_gemm_ukernel_4x2__rvv_u1v(
    size_t mr,
    size_t nc,
    size_t kc,
    const float* restrict a,
    size_t a_stride,
    const float* restrict w,
    float* restrict c,
    size_t cm_stride,
    size_t cn_stride,
    const union xnn_f32_default_params params[restrict XNN_MIN_ELEMENTS(1)])
{
  assert(mr != 0);
  assert(mr <= 4); // max process 4 row
  assert(nc != 0);
  assert(kc != 0);
  assert(kc % sizeof(float) == 0);
  assert(a != NULL);
  assert(w != NULL);
  assert(c != NULL);

  // each row
  for (size_t m = 0; m < mr; ++m) {
    const float* a0 = a + m * a_stride; // support multi-row
    float* c0 = c + m * cm_stride; // support multi-row

    size_t kcl = kc / sizeof(float);
    size_t vl = vsetvl_e32m1(2); // set the vector length to 2 for 2 cols processing
    vfloat32m1_t vacc = vfsub_vv_f32m1(vle32_v_f32m1(c0, vl), vle32_v_f32m1(c0, vl), vl); // Correctly initialize vacc to 0

    for(size_t k = 0; k < kcl; ++k) {
      vfloat32m1_t vw = vle32_v_f32m1(w, vl);
      w += vl;
      vacc = vfmacc_vf_f32m1(vacc, a0[k], vw, vl); // correct multiplication and accumulation
    }

    vse32_v_f32m1(c0, vacc, vl); // store result
  }
}