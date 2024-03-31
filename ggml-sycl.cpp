//
// MIT license
// Copyright (C) 2024 Intel Corporation
// SPDX-License-Identifier: MIT
//

//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//

#include <algorithm>
#include <assert.h>
#include <atomic>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <float.h>
#include <limits>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <cmath>
#include <iostream>
#include <fstream>
#include <stdio.h>
#include <stdlib.h>
#include <regex>

#include <sycl/sycl.hpp>
#include <sycl/half_type.hpp>

#include "ggml-sycl.h"
#include "ggml.h"
#include "ggml-backend-impl.h"

#include "ggml-sycl/backend.hpp"

bool   ggml_sycl_loaded(void);
bool   ggml_sycl_can_mul_mat(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * dst);
void   ggml_sycl_free_data(struct ggml_tensor * tensor);
void   ggml_sycl_assign_buffers(struct ggml_tensor * tensor);
void   ggml_sycl_assign_buffers_no_scratch(struct ggml_tensor * tensor);
void   ggml_sycl_assign_buffers_force_inplace(struct ggml_tensor * tensor);
void   ggml_sycl_assign_buffers_no_alloc(struct ggml_tensor * tensor);
void   ggml_sycl_copy_to_device(struct ggml_tensor * tensor);
void   ggml_sycl_set_main_device(int main_device);
void   ggml_sycl_set_mul_mat_q(bool mul_mat_q);
void   ggml_sycl_set_scratch_size(size_t scratch_size);
void   ggml_sycl_free_scratch(void);
void   ggml_sycl_get_device_description(int device, char * description, size_t description_size);
bool   ggml_backend_is_sycl(ggml_backend_t backend);
int    ggml_backend_sycl_get_device(ggml_backend_t backend);

void dev2dev_memcpy(sycl::queue &q_dst, sycl::queue &q_src, void *ptr_dst,
                    const void *ptr_src, size_t size) {
    char *host_buf = (char *)malloc(size);
    q_src.memcpy(host_buf, (const char *)ptr_src, size).wait();
    q_dst.memcpy((char *)ptr_dst, host_buf, size).wait();
    free(host_buf);
}

template <typename T>
using to_t_sycl_t = void (*)(const void *__restrict__ x, T *__restrict__ y,
                             int k, dpct::queue_ptr stream);
typedef to_t_sycl_t<float> to_fp32_sycl_t;
typedef to_t_sycl_t<sycl::half> to_fp16_sycl_t;

typedef void (*dequantize_kernel_t)(const void * vx, const int ib, const int iqs, dfloat2 & v);
typedef void (*dot_kernel_k_t)(const void * __restrict__ vx, const int ib, const int iqs, const float * __restrict__ y, float & v);
typedef void (*cpy_kernel_t)(const char * cx, char * cdst);
typedef void (*ggml_sycl_func_t)(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst);
typedef void (*ggml_sycl_op_mul_mat_t)(
    const ggml_tensor *src0, const ggml_tensor *src1, ggml_tensor *dst,
    const char *src0_dd_i, const float *src1_ddf_i, const char *src1_ddq_i,
    float *dst_dd_i, const int64_t row_low, const int64_t row_high,
    const int64_t src1_ncols, const int64_t src1_padded_row_size,
    const dpct::queue_ptr &stream);
typedef void (*ggml_sycl_op_flatten_t)(const ggml_tensor *src0,
                                       const ggml_tensor *src1,
                                       ggml_tensor *dst, const float *src0_dd,
                                       const float *src1_dd, float *dst_dd,
                                       const dpct::queue_ptr &main_stream);

static __dpct_inline__ float warp_reduce_sum(float x,
                                             const sycl::nd_item<3> &item_ct1) {
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        /*
        DPCT1096:98: The right-most dimension of the work-group used in the SYCL
        kernel that calls this function may be less than "32". The function
        "dpct::permute_sub_group_by_xor" may return an unexpected result on the
        CPU device. Modify the size of the work-group to ensure that the value
        of the right-most dimension is a multiple of "32".
        */
        x += dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), x, mask);
    }
    return x;
}

static __dpct_inline__ sycl::float2
warp_reduce_sum(sycl::float2 a, const sycl::nd_item<3> &item_ct1) {
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        a.x() += dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), a.x(),
                                                mask);
        a.y() += dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), a.y(),
                                                mask);
    }
    return a;
}

static __dpct_inline__ float warp_reduce_max(float x,
                                             const sycl::nd_item<3> &item_ct1) {
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        /*
        DPCT1096:97: The right-most dimension of the work-group used in the SYCL
        kernel that calls this function may be less than "32". The function
        "dpct::permute_sub_group_by_xor" may return an unexpected result on the
        CPU device. Modify the size of the work-group to ensure that the value
        of the right-most dimension is a multiple of "32".
        */
        x = sycl::fmax(x, dpct::permute_sub_group_by_xor(
                              item_ct1.get_sub_group(), x, mask));
    }
    return x;
}

static __dpct_inline__ float op_repeat(const float a, const float b) {
    return b;
    GGML_UNUSED(a);
}

static __dpct_inline__ float op_add(const float a, const float b) {
    return a + b;
}

static __dpct_inline__ float op_mul(const float a, const float b) {
    return a * b;
}

static __dpct_inline__ float op_div(const float a, const float b) {
    return a / b;
}

template<float (*bin_op)(const float, const float), typename src0_t, typename src1_t, typename dst_t>
static void k_bin_bcast(const src0_t * src0, const src1_t * src1, dst_t * dst,
        int ne0, int ne1, int ne2, int ne3,
        int ne10, int ne11, int ne12, int ne13,
        /*int s0, */ int s1,  int s2,  int s3,
        /*int s10,*/ int s11, int s12, int s13,
        const sycl::nd_item<3> &item_ct1) {
    const int i0s = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                    item_ct1.get_local_id(2);
    const int i1 = (item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                    item_ct1.get_local_id(1));
    const int i2 = (item_ct1.get_local_range(0) * item_ct1.get_group(0) +
                    item_ct1.get_local_id(0)) /
                   ne3;
    const int i3 = (item_ct1.get_local_range(0) * item_ct1.get_group(0) +
                    item_ct1.get_local_id(0)) %
                   ne3;

    if (i0s >= ne0 || i1 >= ne1 || i2 >= ne2 || i3 >= ne3) {
        return;
    }

    const int i11 = i1 % ne11;
    const int i12 = i2 % ne12;
    const int i13 = i3 % ne13;

    const size_t i_src0 = i3*s3 + i2*s2 + i1*s1;
    const size_t i_src1 = i13*s13 + i12*s12 + i11*s11;
    const size_t i_dst  = i_src0;

    const src0_t * src0_row = src0 + i_src0;
    const src1_t * src1_row = src1 + i_src1;
    dst_t * dst_row = dst + i_dst;

    for (int i0 = i0s; i0 < ne0;
         i0 += item_ct1.get_local_range(2) * item_ct1.get_group_range(2)) {
        const int i10 = i0 % ne10;
        dst_row[i0] = (dst_t)bin_op(src0 ? (float)src0_row[i0] : 0.0f, (float)src1_row[i10]);
    }
}

template<float (*bin_op)(const float, const float), typename src0_t, typename src1_t, typename dst_t>
static void k_bin_bcast_unravel(const src0_t * src0, const src1_t * src1, dst_t * dst,
        int ne0, int ne1, int ne2, int ne3,
        int ne10, int ne11, int ne12, int ne13,
        /*int s0, */ int s1,  int s2,  int s3,
        /*int s10,*/ int s11, int s12, int s13,
        const sycl::nd_item<3> &item_ct1) {

    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    const int i3 = i/(ne2*ne1*ne0);
    const int i2 = (i/(ne1*ne0)) % ne2;
    const int i1 = (i/ne0) % ne1;
    const int i0 = i % ne0;

    if (i0 >= ne0 || i1 >= ne1 || i2 >= ne2 || i3 >= ne3) {
        return;
    }

    const int i11 = i1 % ne11;
    const int i12 = i2 % ne12;
    const int i13 = i3 % ne13;

    const size_t i_src0 = i3*s3 + i2*s2 + i1*s1;
    const size_t i_src1 = i13*s13 + i12*s12 + i11*s11;
    const size_t i_dst  = i_src0;

    const src0_t * src0_row = src0 + i_src0;
    const src1_t * src1_row = src1 + i_src1;
    dst_t * dst_row = dst + i_dst;

    const int i10 = i0 % ne10;
    dst_row[i0] = (dst_t)bin_op(src0 ? (float)src0_row[i0] : 0.0f, (float)src1_row[i10]);
}

static void acc_f32(const float * x, const float * y, float * dst, const int ne,
    const int ne10, const int ne11, const int ne12,
    const int nb1, const int nb2, int offset, const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);
    if (i >= ne) {
        return;
    }
    int src1_idx = i - offset;
    int oz = src1_idx / nb2;
    int oy = (src1_idx - (oz * nb2)) / nb1;
    int ox = src1_idx % nb1;
    if (src1_idx >= 0 && ox < ne10 && oy < ne11 && oz < ne12) {
        dst[i] = x[i] + y[ox + oy * ne10 + oz * ne10 * ne11];
    } else {
        dst[i] = x[i];
    }
}

static void gelu_f32(const float * x, float * dst, const int k,
                     const sycl::nd_item<3> &item_ct1) {
    const float GELU_COEF_A    = 0.044715f;
    const float SQRT_2_OVER_PI = 0.79788456080286535587989211986876f;
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    if (i >= k) {
        return;
    }

    float xi = x[i];
    dst[i] = 0.5f * xi *
             (1.0f +
              sycl::tanh(SQRT_2_OVER_PI * xi * (1.0f + GELU_COEF_A * xi * xi)));
}

static void silu_f32(const float * x, float * dst, const int k,
                     const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    if (i >= k) {
        return;
    }
    dst[i] = x[i] / (1.0f + sycl::native::exp(-x[i]));
}

static void gelu_quick_f32(const float *x, float *dst, int k,
                           const sycl::nd_item<3> &item_ct1) {
    const float GELU_QUICK_COEF = -1.702f;
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);
    if (i >= k) {
        return;
    }
    dst[i] = x[i] * (1.0f / (1.0f + sycl::native::exp(GELU_QUICK_COEF * x[i])));
}

static void tanh_f32(const float *x, float *dst, int k,
                     const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);
    if (i >= k) {
        return;
    }
    dst[i] = sycl::tanh((float)(x[i]));
}

static void relu_f32(const float * x, float * dst, const int k,
                     const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    if (i >= k) {
        return;
    }
    dst[i] = sycl::fmax((float)(x[i]), (float)0);
}

static void hardsigmoid_f32(const float * x, float * dst, const int k,
                            const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    if (i >= k) {
        return;
    }
    dst[i] = sycl::fmin(1.0f, sycl::fmax(0.0f, (x[i] + 3.0f) / 6.0f));
}

static void hardswish_f32(const float * x, float * dst, const int k,
                          const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    if (i >= k) {
        return;
    }
    dst[i] = x[i] * sycl::fmin(1.0f, sycl::fmax(0.0f, (x[i] + 3.0f) / 6.0f));
}

static void leaky_relu_f32(const float *x, float *dst, const int k, const float negative_slope,
                           const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);
    if (i >= k) {
        return;
    }
    dst[i] = sycl::fmax((float)(x[i]), (float)0) +
             sycl::fmin((float)(x[i]), 0.0f) * negative_slope;
}

static void sqr_f32(const float * x, float * dst, const int k,
                    const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    if (i >= k) {
        return;
    }
    dst[i] = x[i] * x[i];
}

static void norm_f32(const float * x, float * dst, const int ncols, const float eps,
                     const sycl::nd_item<3> &item_ct1, sycl::float2 *s_sum, int block_size) {
    const int row = item_ct1.get_group(2) * item_ct1.get_local_range(1) +
                    item_ct1.get_local_id(1);
    const int tid = item_ct1.get_local_id(2);

    sycl::float2 mean_var = sycl::float2(0.f, 0.f);

    for (int col = tid; col < ncols; col += block_size) {
        const float xi = x[row*ncols + col];
        mean_var.x() += xi;
        mean_var.y() += xi * xi;
    }

    // sum up partial sums
    mean_var = warp_reduce_sum(mean_var, item_ct1);
    if (block_size > WARP_SIZE) {

        int warp_id = item_ct1.get_local_id(2) / WARP_SIZE;
        int lane_id = item_ct1.get_local_id(2) % WARP_SIZE;
        if (lane_id == 0) {
            s_sum[warp_id] = mean_var;
        }
        /*
        DPCT1118:0: SYCL group functions and algorithms must be encountered in
        converged control flow. You may need to adjust the code.
        */
        item_ct1.barrier(sycl::access::fence_space::local_space);
        mean_var = s_sum[lane_id];
        mean_var = warp_reduce_sum(mean_var, item_ct1);
    }

    const float mean = mean_var.x() / ncols;
    const float var = mean_var.y() / ncols - mean * mean;
    const float inv_std = sycl::rsqrt(var + eps);

    for (int col = tid; col < ncols; col += block_size) {
        dst[row*ncols + col] = (x[row*ncols + col] - mean) * inv_std;
    }
}

static void concat_f32(const float  *x,const float  *y, float *dst, const int ne0, const int ne02,
                       const sycl::nd_item<3> &item_ct1) {
    int nidx = item_ct1.get_local_id(2) +
               item_ct1.get_group(2) * item_ct1.get_local_range(2);
    if (nidx >= ne0) {
        return;
    }
    // operation
    int offset_dst = nidx + item_ct1.get_group(1) * ne0 +
                     item_ct1.get_group(0) * ne0 * item_ct1.get_group_range(1);
    if (item_ct1.get_group(0) < ne02) { // src0
        int offset_src =
            nidx + item_ct1.get_group(1) * ne0 +
            item_ct1.get_group(0) * ne0 * item_ct1.get_group_range(1);
            dst[offset_dst] = x[offset_src];
    } else {
        int offset_src =
            nidx + item_ct1.get_group(1) * ne0 +
            (item_ct1.get_group(0) - ne02) * ne0 * item_ct1.get_group_range(1);
            dst[offset_dst] = y[offset_src];
    }
}

static void upscale_f32(const float  *x, float *dst, const int ne00, const int nb02, const int scale_factor,
                        const sycl::nd_item<3> &item_ct1) {
    int ne0 = ne00 * scale_factor;
    int nidx = item_ct1.get_local_id(2) +
               item_ct1.get_group(2) * item_ct1.get_local_range(2);
    if (nidx >= ne0) {
        return;
    }
    // operation
    int i00 = nidx / scale_factor;
    int i01 = item_ct1.get_group(1) / scale_factor;
    int offset_src = i00 + i01 * ne00 + item_ct1.get_group(0) * nb02;
    int offset_dst = nidx + item_ct1.get_group(1) * ne0 +
                     item_ct1.get_group(0) * ne0 * item_ct1.get_group_range(1);
    dst[offset_dst] = x[offset_src];
}

static void pad_f32(const float  *x, float *dst, const int ne0, const int ne00, const int ne01, const int ne02,
                    const sycl::nd_item<3> &item_ct1) {
    int nidx = item_ct1.get_local_id(2) +
               item_ct1.get_group(2) * item_ct1.get_local_range(2);
    if (nidx >= ne0) {
        return;
    }

    // operation
    int offset_dst = nidx + item_ct1.get_group(1) * ne0 +
                     item_ct1.get_group(0) * ne0 * item_ct1.get_group_range(1);
    if (nidx < ne00 && item_ct1.get_group(1) < ne01 &&
        item_ct1.get_group(0) < ne02) {
        int offset_src = nidx + item_ct1.get_group(1) * ne00 +
                         item_ct1.get_group(0) * ne00 * ne01;
            dst[offset_dst] = x[offset_src];
    } else {
        dst[offset_dst] = 0.0f;
    }
}

static void group_norm_f32(const float * x, float * dst, const int group_size, const int ne_elements, const float eps,
                           const sycl::nd_item<3> &item_ct1, float *s_sum, int block_size) {
    int start = item_ct1.get_group(2) * group_size;
    int end = start + group_size;

    start += item_ct1.get_local_id(2);

    if (end >= ne_elements) {
        end = ne_elements;
    }

    float tmp = 0.0f; // partial sum for thread in warp

    for (int j = start; j < end; j += block_size) {
        tmp += x[j];
    }

    tmp = warp_reduce_sum(tmp, item_ct1);
    if (block_size > WARP_SIZE) {

        int warp_id = item_ct1.get_local_id(2) / WARP_SIZE;
        int lane_id = item_ct1.get_local_id(2) % WARP_SIZE;
        if (lane_id == 0) {
            s_sum[warp_id] = tmp;
        }
        /*
        DPCT1118:1: SYCL group functions and algorithms must be encountered in
        converged control flow. You may need to adjust the code.
        */
        /*
        DPCT1065:54: Consider replacing sycl::nd_item::barrier() with
        sycl::nd_item::barrier(sycl::access::fence_space::local_space) for
        better performance if there is no access to global memory.
        */
        item_ct1.barrier();
        tmp = s_sum[lane_id];
        tmp = warp_reduce_sum(tmp, item_ct1);
    }

    float mean = tmp / group_size;
    tmp = 0.0f;

    for (int j = start; j < end; j += block_size) {
        float xi = x[j] - mean;
        dst[j] = xi;
        tmp += xi * xi;
    }

    tmp = warp_reduce_sum(tmp, item_ct1);
    if (block_size > WARP_SIZE) {

        int warp_id = item_ct1.get_local_id(2) / WARP_SIZE;
        int lane_id = item_ct1.get_local_id(2) % WARP_SIZE;
        if (lane_id == 0) {
            s_sum[warp_id] = tmp;
        }
        /*
        DPCT1118:2: SYCL group functions and algorithms must be encountered in
        converged control flow. You may need to adjust the code.
        */
        /*
        DPCT1065:55: Consider replacing sycl::nd_item::barrier() with
        sycl::nd_item::barrier(sycl::access::fence_space::local_space) for
        better performance if there is no access to global memory.
        */
        item_ct1.barrier();
        tmp = s_sum[lane_id];
        tmp = warp_reduce_sum(tmp, item_ct1);
    }

    float variance = tmp / group_size;
    float scale = sycl::rsqrt(variance + eps);
    for (int j = start; j < end; j += block_size) {
        dst[j] *= scale;
    }
}

static void rms_norm_f32(const float * x, float * dst, const int ncols, const float eps,
                         const sycl::nd_item<3> &item_ct1, float *s_sum, int block_size) {
    const int row = item_ct1.get_group(2) * item_ct1.get_local_range(1) +
                    item_ct1.get_local_id(1);
    const int tid = item_ct1.get_local_id(2);

    float tmp = 0.0f; // partial sum for thread in warp

    for (int col = tid; col < ncols; col += block_size) {
        const float xi = x[row*ncols + col];
        tmp += xi * xi;
    }

    // sum up partial sums
    tmp = warp_reduce_sum(tmp, item_ct1);
    if (block_size > WARP_SIZE) {

        int warp_id = item_ct1.get_local_id(2) / WARP_SIZE;
        int lane_id = item_ct1.get_local_id(2) % WARP_SIZE;
        if (lane_id == 0) {
            s_sum[warp_id] = tmp;
        }
        /*
        DPCT1118:3: SYCL group functions and algorithms must be encountered in
        converged control flow. You may need to adjust the code.
        */
        item_ct1.barrier(sycl::access::fence_space::local_space);
        tmp = s_sum[lane_id];
        tmp = warp_reduce_sum(tmp, item_ct1);
    }

    const float mean = tmp / ncols;
    const float scale = sycl::rsqrt(mean + eps);

    for (int col = tid; col < ncols; col += block_size) {
        dst[row*ncols + col] = scale * x[row*ncols + col];
    }
}

static __dpct_inline__ void dequantize_q4_0(const void *vx, const int ib,
                                            const int iqs, dfloat2 &v) {
    const block_q4_0 * x = (const block_q4_0 *) vx;

    const dfloat d = x[ib].d;

    const int vui = x[ib].qs[iqs];

    v.x() = vui & 0xF;
    v.y() = vui >> 4;

#ifdef GGML_SYCL_F16
    // v = v - {8.0f, 8.0f};
    // v = v * {d, d};
    v.s0() = (v.s0() - 8.0f) * d;
    v.s1() = (v.s1() - 8.0f) * d;

#else
    v.x() = (v.x() - 8.0f) * d;
    v.y() = (v.y() - 8.0f) * d;
#endif // GGML_SYCL_F16
}

static __dpct_inline__ void dequantize_q4_1(const void *vx, const int ib,
                                            const int iqs, dfloat2 &v) {
    const block_q4_1 * x = (const block_q4_1 *) vx;

    const dfloat d = x[ib].dm[0];
    const dfloat m = x[ib].dm[1];

    const int vui = x[ib].qs[iqs];

    v.x() = vui & 0xF;
    v.y() = vui >> 4;

#ifdef GGML_SYCL_F16
    // v = v * {d, d};
    // v = v + {m, m};
    v.s0() = (v.s0() * d) + m;
    v.s1() = (v.s1() * d) + m;

#else
    v.x() = (v.x() * d) + m;
    v.y() = (v.y() * d) + m;
#endif // GGML_SYCL_F16
}

static __dpct_inline__ void dequantize_q5_0(const void *vx, const int ib,
                                            const int iqs, dfloat2 &v) {
    const block_q5_0 * x = (const block_q5_0 *) vx;

    const dfloat d = x[ib].d;

    uint32_t qh;
    memcpy(&qh, x[ib].qh, sizeof(qh));

    const int xh_0 = ((qh >> (iqs +  0)) << 4) & 0x10;
    const int xh_1 = ((qh >> (iqs + 12))     ) & 0x10;

    v.x() = ((x[ib].qs[iqs] & 0xf) | xh_0);
    v.y() = ((x[ib].qs[iqs] >> 4) | xh_1);

#ifdef GGML_SYCL_F16
    // v = v - {16.0f, 16.0f};
    // v = v * {d, d};
    v.s0() = (v.s0() - 16.0f) * d;
    v.s1() = (v.s1() - 16.0f) * d;

#else
    v.x() = (v.x() - 16.0f) * d;
    v.y() = (v.y() - 16.0f) * d;
#endif // GGML_SYCL_F16
}

static __dpct_inline__ void dequantize_q5_1(const void *vx, const int ib,
                                            const int iqs, dfloat2 &v) {
    const block_q5_1 * x = (const block_q5_1 *) vx;

    const dfloat d = x[ib].dm[0];
    const dfloat m = x[ib].dm[1];

    uint32_t qh;
    memcpy(&qh, x[ib].qh, sizeof(qh));

    const int xh_0 = ((qh >> (iqs +  0)) << 4) & 0x10;
    const int xh_1 = ((qh >> (iqs + 12))     ) & 0x10;

    v.x() = ((x[ib].qs[iqs] & 0xf) | xh_0);
    v.y() = ((x[ib].qs[iqs] >> 4) | xh_1);

#ifdef GGML_SYCL_F16
    // v = v * {d, d};
    // v = v + {m, m};
    v.s0() = (v.s0() * d) + m;
    v.s1() = (v.s1() * d) + m;
#else
    v.x() = (v.x() * d) + m;
    v.y() = (v.y() * d) + m;
#endif // GGML_SYCL_F16
}

static __dpct_inline__ void dequantize_q8_0(const void *vx, const int ib,
                                            const int iqs, dfloat2 &v) {
    const block_q8_0 * x = (const block_q8_0 *) vx;

    const dfloat d = x[ib].d;

    v.x() = x[ib].qs[iqs + 0];
    v.y() = x[ib].qs[iqs + 1];

#ifdef GGML_SYCL_F16
    // v = v * {d, d};
    v.s0() *= d;
    v.s1() *= d;
#else
    v.x() *= d;
    v.y() *= d;
#endif // GGML_SYCL_F16
}

template<typename dst_t>
static void dequantize_block_q4_0(const void * __restrict__ vx, dst_t * __restrict__ yy, int nb32,
                                  const sycl::nd_item<3> &item_ct1) {

    const int i = item_ct1.get_group(2);

    // assume 32 threads
    const int tid = item_ct1.get_local_id(2);
    const int il  = tid/8;
    const int ir  = tid%8;
    const int ib = 8*i + ir;
    if (ib >= nb32) {
        return;
    }

    dst_t * y = yy + 256*i + 32*ir + 4*il;

    const block_q4_0 * x = (const block_q4_0 *)vx + ib;
    const float d = sycl::vec<sycl::half, 1>(x->d)
                        .convert<float, sycl::rounding_mode::automatic>()[0];
    const float dm = -8*d;

    const uint8_t * q = x->qs + 4*il;

    for (int l = 0; l < 4; ++l) {
        y[l+ 0] = d * (q[l] & 0xF) + dm;
        y[l+16] = d * (q[l] >>  4) + dm;
    }
}

template<typename dst_t>
static void dequantize_block_q4_1(const void * __restrict__ vx, dst_t * __restrict__ yy, int nb32,
                                  const sycl::nd_item<3> &item_ct1) {

    const int i = item_ct1.get_group(2);

    // assume 32 threads
    const int tid = item_ct1.get_local_id(2);
    const int il  = tid/8;
    const int ir  = tid%8;
    const int ib = 8*i + ir;
    if (ib >= nb32) {
        return;
    }

    dst_t * y = yy + 256*i + 32*ir + 4*il;

    const block_q4_1 * x = (const block_q4_1 *)vx + ib;
    const sycl::float2 d =
        x->dm.convert<float, sycl::rounding_mode::automatic>();

    const uint8_t * q = x->qs + 4*il;

    for (int l = 0; l < 4; ++l) {
        y[l + 0] = d.x() * (q[l] & 0xF) + d.y();
        y[l + 16] = d.x() * (q[l] >> 4) + d.y();
    }
}


//================================== k-quants

template<typename dst_t>
static void dequantize_block_q2_K(const void * __restrict__ vx, dst_t * __restrict__ yy,
                                  const sycl::nd_item<3> &item_ct1) {

    const int i = item_ct1.get_group(2);
    const block_q2_K * x = (const block_q2_K *) vx;

    const int tid = item_ct1.get_local_id(2);
#if QK_K == 256
    const int n   = tid/32;
    const int l   = tid - 32*n;
    const int is  = 8*n + l/16;

    const uint8_t q = x[i].qs[32*n + l];
    dst_t * y = yy + i*QK_K + 128*n;

    float dall = x[i].dm[0];
    float dmin = x[i].dm[1];
    y[l+ 0] = dall * (x[i].scales[is+0] & 0xF) * ((q >> 0) & 3) - dmin * (x[i].scales[is+0] >> 4);
    y[l+32] = dall * (x[i].scales[is+2] & 0xF) * ((q >> 2) & 3) - dmin * (x[i].scales[is+2] >> 4);
    y[l+64] = dall * (x[i].scales[is+4] & 0xF) * ((q >> 4) & 3) - dmin * (x[i].scales[is+4] >> 4);
    y[l+96] = dall * (x[i].scales[is+6] & 0xF) * ((q >> 6) & 3) - dmin * (x[i].scales[is+6] >> 4);
#else
    const int is = tid/16;  // 0 or 1
    const int il = tid%16;  // 0...15
    const uint8_t q = x[i].qs[il] >> (2*is);
    dst_t * y = yy + i*QK_K + 16*is + il;

    float dall = x[i].dm[0];
    float dmin = x[i].dm[1];
    y[ 0] = dall * (x[i].scales[is+0] & 0xF) * ((q >> 0) & 3) - dmin * (x[i].scales[is+0] >> 4);
    y[32] = dall * (x[i].scales[is+2] & 0xF) * ((q >> 4) & 3) - dmin * (x[i].scales[is+2] >> 4);
#endif

}

template<typename dst_t>
static void dequantize_block_q3_K(const void * __restrict__ vx, dst_t * __restrict__ yy,
                                  const sycl::nd_item<3> &item_ct1) {

    const int i = item_ct1.get_group(2);
    const block_q3_K * x = (const block_q3_K *) vx;

#if QK_K == 256
    const int r = item_ct1.get_local_id(2) / 4;
    const int tid = r/2;
    const int is0 = r%2;
    const int l0 = 16 * is0 + 4 * (item_ct1.get_local_id(2) % 4);
    const int n = tid / 4;
    const int j = tid - 4*n;

    uint8_t m = 1 << (4*n + j);
    int is = 8*n + 2*j + is0;
    int shift = 2*j;

    int8_t us = is <  4 ? (x[i].scales[is-0] & 0xF) | (((x[i].scales[is+8] >> 0) & 3) << 4) :
                is <  8 ? (x[i].scales[is-0] & 0xF) | (((x[i].scales[is+4] >> 2) & 3) << 4) :
                is < 12 ? (x[i].scales[is-8] >>  4) | (((x[i].scales[is+0] >> 4) & 3) << 4) :
                          (x[i].scales[is-8] >>  4) | (((x[i].scales[is-4] >> 6) & 3) << 4);
    float d_all = x[i].d;
    float dl = d_all * (us - 32);

    dst_t * y = yy + i*QK_K + 128*n + 32*j;
    const uint8_t * q = x[i].qs + 32*n;
    const uint8_t * hm = x[i].hmask;

    for (int l = l0; l < l0+4; ++l) y[l] = dl * ((int8_t)((q[l] >> shift) & 3) - ((hm[l] & m) ? 0 : 4));
#else
    const int tid = item_ct1.get_local_id(2);
    const int is  = tid/16;  // 0 or 1
    const int il  = tid%16;  // 0...15
    const int im  = il/8;    // 0...1
    const int in  = il%8;    // 0...7

    dst_t * y = yy + i*QK_K + 16*is + il;

    const uint8_t q = x[i].qs[il] >> (2*is);
    const uint8_t h = x[i].hmask[in] >> (2*is + im);
    const float   d = (float)x[i].d;

    if (is == 0) {
        y[ 0] = d * ((x[i].scales[0] & 0xF) - 8) * ((int8_t)((q >> 0) & 3) - ((h >> 0) & 1 ? 0 : 4));
        y[32] = d * ((x[i].scales[1] & 0xF) - 8) * ((int8_t)((q >> 4) & 3) - ((h >> 4) & 1 ? 0 : 4));
    } else {
        y[ 0] = d * ((x[i].scales[0] >>  4) - 8) * ((int8_t)((q >> 0) & 3) - ((h >> 0) & 1 ? 0 : 4));
        y[32] = d * ((x[i].scales[1] >>  4) - 8) * ((int8_t)((q >> 4) & 3) - ((h >> 4) & 1 ? 0 : 4));
    }
#endif

}

#if QK_K == 256
static inline void get_scale_min_k4(int j, const uint8_t * q, uint8_t & d, uint8_t & m) {
    if (j < 4) {
        d = q[j] & 63; m = q[j + 4] & 63;
    } else {
        d = (q[j+4] & 0xF) | ((q[j-4] >> 6) << 4);
        m = (q[j+4] >>  4) | ((q[j-0] >> 6) << 4);
    }
}
#endif

template<typename dst_t>
static void dequantize_block_q4_K(const void * __restrict__ vx, dst_t * __restrict__ yy,
                                  const sycl::nd_item<3> &item_ct1) {
    const block_q4_K * x = (const block_q4_K *) vx;

    const int i = item_ct1.get_group(2);

#if QK_K == 256
    // assume 32 threads
    const int tid = item_ct1.get_local_id(2);
    const int il  = tid/8;
    const int ir  = tid%8;
    const int is  = 2*il;
    const int n   = 4;

    dst_t * y = yy + i*QK_K + 64*il + n*ir;

    const float dall = x[i].dm[0];
    const float dmin = x[i].dm[1];

    const uint8_t * q = x[i].qs + 32*il + n*ir;

    uint8_t sc, m;
    get_scale_min_k4(is + 0, x[i].scales, sc, m);
    const float d1 = dall * sc; const float m1 = dmin * m;
    get_scale_min_k4(is + 1, x[i].scales, sc, m);
    const float d2 = dall * sc; const float m2 = dmin * m;
    for (int l = 0; l < n; ++l) {
        y[l + 0] = d1 * (q[l] & 0xF) - m1;
        y[l +32] = d2 * (q[l] >>  4) - m2;
    }
#else
    const int tid = item_ct1.get_local_id(2);
    const uint8_t * q = x[i].qs;
    dst_t * y = yy + i*QK_K;
    const float d = (float)x[i].dm[0];
    const float m = (float)x[i].dm[1];
    y[tid+ 0] = d * (x[i].scales[0] & 0xF) * (q[tid] & 0xF) - m * (x[i].scales[0] >> 4);
    y[tid+32] = d * (x[i].scales[1] & 0xF) * (q[tid] >>  4) - m * (x[i].scales[1] >> 4);
#endif
}

template<typename dst_t>
static void dequantize_block_q5_K(const void * __restrict__ vx, dst_t * __restrict__ yy,
                                  const sycl::nd_item<3> &item_ct1) {
    const block_q5_K * x = (const block_q5_K *) vx;

    const int i = item_ct1.get_group(2);

#if QK_K == 256
    // assume 64 threads - this is very slightly better than the one below
    const int tid = item_ct1.get_local_id(2);
    const int il  = tid/16;   // il is in 0...3
    const int ir  = tid%16;   // ir is in 0...15
    const int is  = 2*il;     // is is in 0...6

    dst_t * y = yy + i*QK_K + 64*il + 2*ir;

    const float dall = x[i].dm[0];
    const float dmin = x[i].dm[1];

    const uint8_t * ql = x[i].qs + 32*il + 2*ir;
    const uint8_t * qh = x[i].qh + 2*ir;

    uint8_t sc, m;
    get_scale_min_k4(is + 0, x[i].scales, sc, m);
    const float d1 = dall * sc; const float m1 = dmin * m;
    get_scale_min_k4(is + 1, x[i].scales, sc, m);
    const float d2 = dall * sc; const float m2 = dmin * m;

    uint8_t   hm  = 1 << (2*il);
    y[ 0] = d1 * ((ql[ 0] & 0xF) + (qh[ 0] & hm ? 16 : 0)) - m1;
    y[ 1] = d1 * ((ql[ 1] & 0xF) + (qh[ 1] & hm ? 16 : 0)) - m1;
    hm <<= 1;
    y[32] = d2 * ((ql[ 0] >>  4) + (qh[ 0] & hm ? 16 : 0)) - m2;
    y[33] = d2 * ((ql[ 1] >>  4) + (qh[ 1] & hm ? 16 : 0)) - m2;
#else
    const int tid = item_ct1.get_local_id(2);
    const uint8_t q = x[i].qs[tid];
    const int im = tid/8;  // 0...3
    const int in = tid%8;  // 0...7
    const int is = tid/16; // 0 or 1
    const uint8_t h = x[i].qh[in] >> im;
    const float d = x[i].d;
    dst_t * y = yy + i*QK_K + tid;
    y[ 0] = d * x[i].scales[is+0] * ((q & 0xF) - ((h >> 0) & 1 ? 0 : 16));
    y[32] = d * x[i].scales[is+2] * ((q >>  4) - ((h >> 4) & 1 ? 0 : 16));
#endif
}

template<typename dst_t>
static void dequantize_block_q6_K(const void * __restrict__ vx, dst_t * __restrict__ yy,
                                  const sycl::nd_item<3> &item_ct1) {
    const block_q6_K * x = (const block_q6_K *) vx;

    const int i = item_ct1.get_group(2);
#if QK_K == 256

    // assume 64 threads - this is very slightly better than the one below
    const int tid = item_ct1.get_local_id(2);
    const int ip  = tid/32;   // ip is 0 or 1
    const int il  = tid - 32*ip; // 0...32
    const int is  = 8*ip + il/16;

    dst_t * y = yy + i*QK_K + 128*ip + il;

    const float d = x[i].d;

    const uint8_t * ql = x[i].ql + 64*ip + il;
    const uint8_t   qh = x[i].qh[32*ip + il];
    const int8_t  * sc = x[i].scales + is;

    y[ 0] = d * sc[0] * ((int8_t)((ql[ 0] & 0xF) | (((qh >> 0) & 3) << 4)) - 32);
    y[32] = d * sc[2] * ((int8_t)((ql[32] & 0xF) | (((qh >> 2) & 3) << 4)) - 32);
    y[64] = d * sc[4] * ((int8_t)((ql[ 0]  >> 4) | (((qh >> 4) & 3) << 4)) - 32);
    y[96] = d * sc[6] * ((int8_t)((ql[32]  >> 4) | (((qh >> 6) & 3) << 4)) - 32);
#else

    // assume 32 threads
    const int tid = item_ct1.get_local_id(2);
    const int ip  = tid/16;         // 0 or 1
    const int il  = tid - 16*ip;    // 0...15

    dst_t * y = yy + i*QK_K + 16*ip + il;

    const float d = x[i].d;

    const uint8_t   ql = x[i].ql[16*ip + il];
    const uint8_t   qh = x[i].qh[il] >> (2*ip);
    const int8_t  * sc = x[i].scales;

    y[ 0] = d * sc[ip+0] * ((int8_t)((ql & 0xF) | (((qh >> 0) & 3) << 4)) - 32);
    y[32] = d * sc[ip+2] * ((int8_t)((ql  >> 4) | (((qh >> 4) & 3) << 4)) - 32);
#endif
}

template<typename dst_t>
static void dequantize_block_iq2_xxs(const void * __restrict__ vx, dst_t * __restrict__ yy,
                                     const sycl::nd_item<3> &item_ct1,
                                     const uint64_t *iq2xxs_grid_ptr,
                                     const uint8_t *ksigns_iq2xs_ptr,
                                     const uint8_t *kmask_iq2xs_ptr) {

    const int i = item_ct1.get_group(2);
    const block_iq2_xxs * x = (const block_iq2_xxs  *) vx;

    const int tid = item_ct1.get_local_id(2);
#if QK_K == 256
    const int il = tid/8; // 0...3
    const int ib = tid%8; // 0...7
    dst_t * y = yy + i*QK_K + 32*ib + 8*il;
    const uint16_t * q2 = x[i].qs + 4*ib;
    const uint8_t  * aux8 = (const uint8_t *)q2;
    const uint8_t  * grid = (const uint8_t *)(iq2xxs_grid_ptr + aux8[il]);
    const uint32_t aux32 = q2[2] | (q2[3] << 16);
    const float d = (float)x[i].d * (0.5f + (aux32 >> 28)) * 0.25f;
    const uint8_t signs = ksigns_iq2xs_ptr[(aux32 >> 7*il) & 127];
    for (int j = 0; j < 8; ++j) y[j] = d * grid[j] * (signs & kmask_iq2xs_ptr[j] ? -1.f : 1.f);
#else
    assert(false);
#endif

}

template<typename dst_t>
static void dequantize_block_iq2_xs(const void * __restrict__ vx, dst_t * __restrict__ yy,
                                    const sycl::nd_item<3> &item_ct1,
                                    const uint64_t *iq2xs_grid,
                                    const uint8_t *ksigns_iq2xs,
                                    const uint8_t *kmask_iq2xs) {

    const int i = item_ct1.get_group(2);
    const block_iq2_xs * x = (const block_iq2_xs *) vx;

    const int tid = item_ct1.get_local_id(2);
#if QK_K == 256
    const int il = tid/8; // 0...3
    const int ib = tid%8; // 0...7
    dst_t * y = yy + i*QK_K + 32*ib + 8*il;
    const uint16_t * q2 = x[i].qs + 4*ib;
    const uint8_t  * grid = (const uint8_t *)(iq2xs_grid + (q2[il] & 511));
    const float d = (float)x[i].d * (0.5f + ((x[i].scales[ib] >> 4*(il/2)) & 0xf)) * 0.25f;
    const uint8_t signs = ksigns_iq2xs[q2[il] >> 9];
    for (int j = 0; j < 8; ++j) y[j] = d * grid[j] * (signs & kmask_iq2xs[j] ? -1.f : 1.f);
#else
    assert(false);
#endif

}

template<typename dst_t>
static void dequantize_block_iq3_xxs(const void * __restrict__ vx, dst_t * __restrict__ yy,
                                     const sycl::nd_item<3> &item_ct1,
                                     const uint32_t *iq3xxs_grid,
                                     const uint8_t *ksigns_iq2xs,
                                     const uint8_t *kmask_iq2xs) {

    const int i = item_ct1.get_group(2);
    const block_iq3_xxs * x = (const block_iq3_xxs  *) vx;

    const int tid = item_ct1.get_local_id(2);
#if QK_K == 256
    const int il = tid/8; // 0...3
    const int ib = tid%8; // 0...7
    dst_t * y = yy + i*QK_K + 32*ib + 8*il;
    const uint8_t  * q3 = x[i].qs + 8*ib;
    const uint16_t * gas = (const uint16_t *)(x[i].qs + QK_K/4) + 2*ib;
    const uint8_t  * grid1 = (const uint8_t *)(iq3xxs_grid + q3[2*il+0]);
    const uint8_t  * grid2 = (const uint8_t *)(iq3xxs_grid + q3[2*il+1]);
    const uint32_t aux32 = gas[0] | (gas[1] << 16);
    const float d = (float)x[i].d * (0.5f + (aux32 >> 28)) * 0.5f;
    const uint8_t signs = ksigns_iq2xs[(aux32 >> 7*il) & 127];
    for (int j = 0; j < 4; ++j) {
        y[j+0] = d * grid1[j] * (signs & kmask_iq2xs[j+0] ? -1.f : 1.f);
        y[j+4] = d * grid2[j] * (signs & kmask_iq2xs[j+4] ? -1.f : 1.f);
    }
#else
    assert(false);
#endif

}

template<typename dst_t>
static void dequantize_block_iq3_s(const void * __restrict__ vx, dst_t * __restrict__ yy,
                                     const sycl::nd_item<3> &item_ct1,
                                     const uint32_t *iq3s_grid,
                                     const uint8_t *ksigns_iq2xs,
                                     const uint8_t *kmask_iq2xs) {

    const int i = item_ct1.get_group(2);
    const block_iq3_s * x = (const block_iq3_s  *) vx;

    const int tid = item_ct1.get_local_id(2);
#if QK_K == 256
    const int il = tid/8; // 0...3
    const int ib = tid%8; // 0...7
    dst_t * y = yy + i*QK_K + 32*ib + 8*il;
    const uint8_t  * qs = x[i].qs + 8*ib;
    const uint8_t  * grid1 = (const uint8_t *)(iq3s_grid + qs[2*il+0]);
    const uint8_t  * grid2 = (const uint8_t *)(iq3s_grid + qs[2*il+1]);
    const float d = (float)x[i].d * (1 + 2*((x[i].scales[ib/2] >> 4*(ib%2)) & 0xf));
    const uint8_t signs = x[i].signs[4*ib + il];
    for (int j = 0; j < 4; ++j) {
        y[j+0] = d * grid1[j] * (signs & kmask_iq2xs[j+0] ? -1.f : 1.f);
        y[j+4] = d * grid2[j] * (signs & kmask_iq2xs[j+4] ? -1.f : 1.f);
    }
#else
    assert(false);
#endif

}

template<typename dst_t>
static void dequantize_block_iq1_s(const void * __restrict__ vx, dst_t * __restrict__ yy,
                                     const sycl::nd_item<3> &item_ct1,
                                     const uint32_t *iq1s_grid,
                                     const uint8_t *ksigns_iq2xs,
                                     const uint8_t *kmask_iq2xs) {
    const int i = item_ct1.get_group(2);
    const block_iq1_s * x = (const block_iq1_s  *) vx;

    const int tid = item_ct1.get_local_id(2);
#if QK_K == 256
    const int il = tid/8; // 0...3
    const int ib = tid%8; // 0...7
    dst_t * y = yy + i*QK_K + 32*ib + 8*il;
    const uint8_t  * qs = x[i].qs + 8*ib;
    const uint8_t  * grid1 = (const uint8_t *)(iq1s_grid + qs[2*il+0]);
    const uint8_t  * grid2 = (const uint8_t *)(iq1s_grid + qs[2*il+1]);
    const float d = (float)x[i].d * (2*((x[i].qh[ib] >> 12) & 0xf) + 1);
    const uint8_t signs = ksigns_iq2xs[(x[i].qh[ib] >> 3*il) & 7];
    for (int j = 0; j < 4; ++j) {
        y[j+0] = d * grid1[j] * (signs & kmask_iq2xs[j+0] ? -1.f : 1.f);
        y[j+4] = d * grid2[j] * (signs & kmask_iq2xs[j+4] ? -1.f : 1.f);
    }
#else
    assert(false);
#endif

}

/*
DPCT1110:4: The total declared local variable size in device function
dequantize_mul_mat_vec_q2_k exceeds 128 bytes and may cause high register
pressure. Consult with your hardware vendor to find the total register size
available and adjust the code, or use smaller sub-group size to avoid high
register pressure.
*/
static void dequantize_mul_mat_vec_q2_k(const void *__restrict__ vx,
                                        const float *__restrict__ yy,
                                        float *__restrict__ dst,
                                        const int ncols, int nrows,
                                        const sycl::nd_item<3> &item_ct1) {

    static_assert(16%K_QUANTS_PER_ITERATION == 0, "16 must be divisible by K_QUANTS_PER_ITERATION");

    const int row = item_ct1.get_group(2) * item_ct1.get_local_range(1) +
                    item_ct1.get_local_id(1);
    if (row > nrows) return;

    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row;

    const block_q2_K * x = (const block_q2_K *)vx + ib0;

    float tmp = 0; // partial sum for thread in warp

#if QK_K == 256
    const int tid =
        item_ct1.get_local_id(2) / K_QUANTS_PER_ITERATION; // 0...31 or 0...15
    const int ix =
        item_ct1.get_local_id(2) % K_QUANTS_PER_ITERATION; // 0 or 0,1

    const int step = 16/K_QUANTS_PER_ITERATION;

    const int im = tid/step;                             // 0 or 1. 0 computes 0..., 1 computes 128...
    const int in = tid - step*im;                        // 0...15 or 0...7

    const int l0 = K_QUANTS_PER_ITERATION*in;            // 0...15 or 0...14 in steps of 2
    const int q_offset = 32*im + l0;
    const int s_offset = 8*im;
    const int y_offset = 128*im + l0;

    uint32_t aux[4];
    const uint8_t * d = (const uint8_t *)aux;
    const uint8_t * m = (const uint8_t *)(aux + 2);

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        const float   * y = yy + i * QK_K + y_offset;
        const uint8_t * q = x[i].qs + q_offset;

        const float dall = x[i].dm[0];
        const float dmin = x[i].dm[1];

        const uint32_t * a = (const uint32_t *)(x[i].scales + s_offset);
        aux[0] = a[0] & 0x0f0f0f0f;
        aux[1] = a[1] & 0x0f0f0f0f;
        aux[2] = (a[0] >> 4) & 0x0f0f0f0f;
        aux[3] = (a[1] >> 4) & 0x0f0f0f0f;

        float sum1 = 0, sum2 = 0;
        for (int l = 0; l < K_QUANTS_PER_ITERATION; ++l) {
            sum1 += y[l+ 0] * d[0] * ((q[l+ 0] >> 0) & 3)
                  + y[l+32] * d[2] * ((q[l+ 0] >> 2) & 3)
                  + y[l+64] * d[4] * ((q[l+ 0] >> 4) & 3)
                  + y[l+96] * d[6] * ((q[l+ 0] >> 6) & 3)
                  + y[l+16] * d[1] * ((q[l+16] >> 0) & 3)
                  + y[l+48] * d[3] * ((q[l+16] >> 2) & 3)
                  + y[l+80] * d[5] * ((q[l+16] >> 4) & 3)
                  +y[l+112] * d[7] * ((q[l+16] >> 6) & 3);
            sum2 += y[l+ 0] * m[0] + y[l+32] * m[2] + y[l+64] * m[4] + y[ l+96] * m[6]
                  + y[l+16] * m[1] + y[l+48] * m[3] + y[l+80] * m[5] + y[l+112] * m[7];

        }
        tmp += dall * sum1 - dmin * sum2;

    }
#else
    const int tid = item_ct1.get_local_id(2) /
                    (2 * K_QUANTS_PER_ITERATION); // 0...15 or 0...7
    const int ix = item_ct1.get_local_id(2) %
                   (2 * K_QUANTS_PER_ITERATION); // 0....1 or 0...3
    const int offset = tid * K_QUANTS_PER_ITERATION;

    uint32_t uaux[2];
    const uint8_t * d = (const uint8_t *)uaux;


    for (int i = ix; i < num_blocks_per_row; i += 2*K_QUANTS_PER_ITERATION) {

        const float   * y = yy + i * QK_K + offset;
        const uint8_t * q = x[i].qs + offset;
        const uint32_t * s = (const uint32_t *)x[i].scales;

        uaux[0] = s[0] & 0x0f0f0f0f;
        uaux[1] = (s[0] >> 4) & 0x0f0f0f0f;

        const sycl::float2 dall =
            x[i].dm.convert<float, sycl::rounding_mode::automatic>();

        float sum1 = 0, sum2 = 0;
        for (int l = 0; l < K_QUANTS_PER_ITERATION; ++l) {
            const uint8_t ql = q[l];
            sum1 += y[l+ 0] * d[0] * ((ql >> 0) & 3)
                  + y[l+16] * d[1] * ((ql >> 2) & 3)
                  + y[l+32] * d[2] * ((ql >> 4) & 3)
                  + y[l+48] * d[3] * ((ql >> 6) & 3);
            sum2 += y[l+0] * d[4] + y[l+16] * d[5] + y[l+32] * d[6] + y[l+48] * d[7];
        }
        tmp += dall.x() * sum1 - dall.y() * sum2;
    }

#endif

    // sum up partial sums and write back result
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        tmp +=
            dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), tmp, mask);
    }

    if (item_ct1.get_local_id(2) == 0) {
        dst[row] = tmp;
    }
}

/*
DPCT1110:5: The total declared local variable size in device function
dequantize_mul_mat_vec_q3_k exceeds 128 bytes and may cause high register
pressure. Consult with your hardware vendor to find the total register size
available and adjust the code, or use smaller sub-group size to avoid high
register pressure.
*/
static void dequantize_mul_mat_vec_q3_k(const void *__restrict__ vx,
                                        const float *__restrict__ yy,
                                        float *__restrict__ dst,
                                        const int ncols, int nrows,
                                        const sycl::nd_item<3> &item_ct1) {

    const int row = item_ct1.get_group(2) * item_ct1.get_local_range(1) +
                    item_ct1.get_local_id(1);
    if (row > nrows) return;

    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row;

    const block_q3_K * x = (const block_q3_K *)vx + ib0;

    float tmp = 0; // partial sum for thread in warp

#if QK_K == 256

    const uint16_t kmask1 = 0x0303;
    const uint16_t kmask2 = 0x0f0f;

    const int tid =
        item_ct1.get_local_id(2) / K_QUANTS_PER_ITERATION; // 0...31 or 0...16
    const int ix =
        item_ct1.get_local_id(2) % K_QUANTS_PER_ITERATION; // 0 or 0,1

    const int n  = K_QUANTS_PER_ITERATION;               // iterations in the inner loop
    const int step = 16/K_QUANTS_PER_ITERATION;
    const int im = tid/step;                             // 0 or 1. 0 computes 0..., 1 computes 128...
    const int in = tid - step*im;                        // 0....15 or 0...7

    const uint8_t m = 1 << (4*im);

    const int l0 = n*in;                                 // 0...15 or 0...14 in steps of 2
    const int q_offset =  32*im + l0;
    const int y_offset = 128*im + l0;

    uint16_t utmp[4];
    const int8_t * s = (const int8_t *)utmp;

    const uint16_t s_shift = 4*im;

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        const float   * y  = yy + i * QK_K + y_offset;
        const uint8_t * q = x[i].qs + q_offset;
        const uint8_t * h = x[i].hmask + l0;

        const uint16_t * a = (const uint16_t *)x[i].scales;
        utmp[0] = ((a[0] >> s_shift) & kmask2) | (((a[4] >> (s_shift + 0)) & kmask1) << 4);
        utmp[1] = ((a[1] >> s_shift) & kmask2) | (((a[5] >> (s_shift + 0)) & kmask1) << 4);
        utmp[2] = ((a[2] >> s_shift) & kmask2) | (((a[4] >> (s_shift + 2)) & kmask1) << 4);
        utmp[3] = ((a[3] >> s_shift) & kmask2) | (((a[5] >> (s_shift + 2)) & kmask1) << 4);

        const float d = x[i].d;

        float sum = 0;
        for (int l = 0; l < n; ++l) {
            sum += y[l+ 0] * (s[0] - 32) * (((q[l] >> 0) & 3) - (h[l] & (m << 0) ? 0 : 4))
                 + y[l+32] * (s[2] - 32) * (((q[l] >> 2) & 3) - (h[l] & (m << 1) ? 0 : 4))
                 + y[l+64] * (s[4] - 32) * (((q[l] >> 4) & 3) - (h[l] & (m << 2) ? 0 : 4))
                 + y[l+96] * (s[6] - 32) * (((q[l] >> 6) & 3) - (h[l] & (m << 3) ? 0 : 4));
            sum += y[l+16] * (s[1] - 32) * (((q[l+16] >> 0) & 3) - (h[l+16] & (m << 0) ? 0 : 4))
                 + y[l+48] * (s[3] - 32) * (((q[l+16] >> 2) & 3) - (h[l+16] & (m << 1) ? 0 : 4))
                 + y[l+80] * (s[5] - 32) * (((q[l+16] >> 4) & 3) - (h[l+16] & (m << 2) ? 0 : 4))
                + y[l+112] * (s[7] - 32) * (((q[l+16] >> 6) & 3) - (h[l+16] & (m << 3) ? 0 : 4));
        }
        tmp += d * sum;

    }
#else

    const int tid = item_ct1.get_local_id(2)/(2*K_QUANTS_PER_ITERATION);  // 0...15 or 0...7
    const int ix  = item_ct1.get_local_id(2)%(2*K_QUANTS_PER_ITERATION);  // 0....1 or 0...3
    const int offset = tid * K_QUANTS_PER_ITERATION;         // 0...15 or 0...14
    const int in = offset/8;                                 // 0 or 1
    const int im = offset%8;                                 // 0...7

    for (int i = ix; i < num_blocks_per_row; i += 2*K_QUANTS_PER_ITERATION) {

        const float   * y = yy + i * QK_K + offset;
        const uint8_t * q = x[i].qs + offset;
        const uint8_t * s = x[i].scales;

        const float dall = (float)x[i].d;

        float sum = 0;
        for (int l = 0; l < K_QUANTS_PER_ITERATION; ++l) {
            const uint8_t hl = x[i].hmask[im+l] >> in;
            const uint8_t ql = q[l];
            sum += y[l+ 0] * dall * ((s[0] & 0xF) - 8) * ((int8_t)((ql >> 0) & 3) - ((hl >> 0) & 1 ? 0 : 4))
                 + y[l+16] * dall * ((s[0] >>  4) - 8) * ((int8_t)((ql >> 2) & 3) - ((hl >> 2) & 1 ? 0 : 4))
                 + y[l+32] * dall * ((s[1] & 0xF) - 8) * ((int8_t)((ql >> 4) & 3) - ((hl >> 4) & 1 ? 0 : 4))
                 + y[l+48] * dall * ((s[1] >>  4) - 8) * ((int8_t)((ql >> 6) & 3) - ((hl >> 6) & 1 ? 0 : 4));
        }
        tmp += sum;
    }
#endif

    // sum up partial sums and write back result
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        tmp +=
            dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), tmp, mask);
    }

    if (item_ct1.get_local_id(2) == 0) {
        dst[row] = tmp;
    }
}

/*
DPCT1110:6: The total declared local variable size in device function
dequantize_mul_mat_vec_q4_k exceeds 128 bytes and may cause high register
pressure. Consult with your hardware vendor to find the total register size
available and adjust the code, or use smaller sub-group size to avoid high
register pressure.
*/
static void dequantize_mul_mat_vec_q4_k(const void *__restrict__ vx,
                                        const float *__restrict__ yy,
                                        float *__restrict__ dst,
                                        const int ncols, int nrows,
                                        const sycl::nd_item<3> &item_ct1) {

    const int row = item_ct1.get_group(2) * item_ct1.get_local_range(1) +
                    item_ct1.get_local_id(1);
    if (row > nrows) return;
    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row;

    const block_q4_K * x = (const block_q4_K *)vx + ib0;

#if QK_K == 256
    const uint16_t kmask1 = 0x3f3f;
    const uint16_t kmask2 = 0x0f0f;
    const uint16_t kmask3 = 0xc0c0;

    const int tid =
        item_ct1.get_local_id(2) / K_QUANTS_PER_ITERATION; // 0...31 or 0...16
    const int ix =
        item_ct1.get_local_id(2) % K_QUANTS_PER_ITERATION; // 0 or 0,1

    const int step = 8/K_QUANTS_PER_ITERATION;           // 8 or 4

    const int il  = tid/step;                            // 0...3
    const int ir  = tid - step*il;                       // 0...7 or 0...3
    const int n   = 2 * K_QUANTS_PER_ITERATION;          // 2 or 4

    const int im = il/2;  // 0 or 1. 0 computes 0,32 + 128,160, 1 computes 64,96 + 192,224
    const int in = il%2;

    const int l0 = n*(2*ir + in);
    const int q_offset = 32*im + l0;
    const int y_offset = 64*im + l0;

    uint16_t aux[4];
    const uint8_t * sc = (const uint8_t *)aux;

#if K_QUANTS_PER_ITERATION == 2
    uint32_t q32[4];
    const uint8_t * q4 = (const uint8_t *)q32;
#else
    uint16_t q16[4];
    const uint8_t * q4 = (const uint8_t *)q16;
#endif

    float tmp = 0; // partial sum for thread in warp

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        const float   * y1 = yy + i*QK_K + y_offset;
        const float   * y2 = y1 + 128;

        const float dall = x[i].dm[0];
        const float dmin = x[i].dm[1];

        const uint16_t * a = (const uint16_t *)x[i].scales;
        aux[0] = a[im+0] & kmask1;
        aux[1] = a[im+2] & kmask1;
        aux[2] = ((a[im+4] >> 0) & kmask2) | ((a[im+0] & kmask3) >> 2);
        aux[3] = ((a[im+4] >> 4) & kmask2) | ((a[im+2] & kmask3) >> 2);

#if K_QUANTS_PER_ITERATION == 2
        const uint32_t * q1 = (const uint32_t *)(x[i].qs + q_offset);
        const uint32_t * q2 = q1 + 16;

        q32[0] = q1[0] & 0x0f0f0f0f;
        q32[1] = q1[0] & 0xf0f0f0f0;
        q32[2] = q2[0] & 0x0f0f0f0f;
        q32[3] = q2[0] & 0xf0f0f0f0;

        sycl::float4 s = {0.f, 0.f, 0.f, 0.f};
        float smin = 0;
        for (int l = 0; l < 4; ++l) {
            s.x() += y1[l] * q4[l + 0]; s.y() += y1[l + 32] * q4[l + 4];
            s.z() += y2[l] * q4[l + 8]; s.w() += y2[l + 32] * q4[l + 12];
            smin += y1[l] * sc[2] + y1[l+32] * sc[3] + y2[l] * sc[6] + y2[l+32] * sc[7];
        }
        tmp += dall * (s.x() * sc[0] + s.y() * sc[1] * 1.f / 16.f +
                       s.z() * sc[4] + s.w() * sc[5] * 1.f / 16.f) -
               dmin * smin;
#else
        const uint16_t * q1 = (const uint16_t *)(x[i].qs + q_offset);
        const uint16_t * q2 = q1 + 32;

        q16[0] = q1[0] & 0x0f0f;
        q16[1] = q1[0] & 0xf0f0;
        q16[2] = q2[0] & 0x0f0f;
        q16[3] = q2[0] & 0xf0f0;

        float4 s = {0.f, 0.f, 0.f, 0.f};
        float smin = 0;
        for (int l = 0; l < 2; ++l) {
            s.x += y1[l] * q4[l+0]; s.y += y1[l+32] * q4[l+2];
            s.z += y2[l] * q4[l+4]; s.w += y2[l+32] * q4[l+6];
            smin += y1[l] * sc[2] + y1[l+32] * sc[3] + y2[l] * sc[6] + y2[l+32] * sc[7];
        }
        tmp += dall * (s.x * sc[0] + s.y * sc[1] * 1.f/16.f + s.z * sc[4] + s.w * sc[5] * 1.f/16.f) - dmin * smin;
#endif

    }
#else
    const int tid = item_ct1.get_local_id(2)/(2*K_QUANTS_PER_ITERATION);  // 0...15
    const int ix  = item_ct1.get_local_id(2)%(2*K_QUANTS_PER_ITERATION);

    const int step = tid * K_QUANTS_PER_ITERATION;

    uint16_t aux16[2];
    const uint8_t * s = (const uint8_t *)aux16;

    float tmp = 0;

    for (int i = ix; i < num_blocks_per_row; i += 2*K_QUANTS_PER_ITERATION) {
        const uint8_t * q = x[i].qs + step;
        const float   * y = yy + i*QK_K + step;
        const uint16_t * a = (const uint16_t *)x[i].scales;
        aux16[0] = a[0] & 0x0f0f;
        aux16[1] = (a[0] >> 4) & 0x0f0f;
        const float d = (float)x[i].dm[0];
        const float m = (float)x[i].dm[1];
        float sum = 0.f;
        for (int j = 0; j < K_QUANTS_PER_ITERATION; ++j) {
            sum += y[j+ 0] * (d * s[0] * (q[j+ 0] & 0xF) - m * s[2])
                 + y[j+16] * (d * s[0] * (q[j+16] & 0xF) - m * s[2])
                 + y[j+32] * (d * s[1] * (q[j+ 0] >>  4) - m * s[3])
                 + y[j+48] * (d * s[1] * (q[j+16] >>  4) - m * s[3]);
        }
        tmp += sum;
    }

#endif

    // sum up partial sums and write back result
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        tmp +=
            dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), tmp, mask);
    }

    if (tid == 0) {
        dst[row] = tmp;
    }
}

/*
DPCT1110:7: The total declared local variable size in device function
dequantize_mul_mat_vec_q5_k exceeds 128 bytes and may cause high register
pressure. Consult with your hardware vendor to find the total register size
available and adjust the code, or use smaller sub-group size to avoid high
register pressure.
*/
static void dequantize_mul_mat_vec_q5_k(const void *__restrict__ vx,
                                        const float *__restrict__ yy,
                                        float *__restrict__ dst,
                                        const int ncols,
                                        const sycl::nd_item<3> &item_ct1) {

    const int row = item_ct1.get_group(2);
    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row;

    const block_q5_K * x = (const block_q5_K *)vx + ib0;

    float tmp = 0; // partial sum for thread in warp

#if QK_K == 256
    const uint16_t kmask1 = 0x3f3f;
    const uint16_t kmask2 = 0x0f0f;
    const uint16_t kmask3 = 0xc0c0;

    const int tid = item_ct1.get_local_id(2) / 2; // 0...15
    const int ix = item_ct1.get_local_id(2) % 2;

    const int il  = tid/4;     // 0...3
    const int ir  = tid - 4*il;// 0...3
    const int n   = 2;

    const int im = il/2;  // 0 or 1. 0 computes 0,32 + 128,160, 1 computes 64,96 + 192,224
    const int in = il%2;

    const int l0 = n*(2*ir + in);
    const int q_offset = 32*im + l0;
    const int y_offset = 64*im + l0;

    const uint8_t hm1  = 1 << (2*im);
    const uint8_t hm2  = hm1 << 4;

    uint16_t aux[4];
    const uint8_t * sc = (const uint8_t *)aux;

    uint16_t q16[8];
    const uint8_t * q4 = (const uint8_t *)q16;

    for (int i = ix; i < num_blocks_per_row; i += 2) {

        const uint8_t * ql1 = x[i].qs + q_offset;
        const uint8_t * qh  = x[i].qh + l0;
        const float   * y1  = yy + i*QK_K + y_offset;
        const float   * y2  = y1 + 128;

        const float dall = x[i].dm[0];
        const float dmin = x[i].dm[1];

        const uint16_t * a = (const uint16_t *)x[i].scales;
        aux[0] = a[im+0] & kmask1;
        aux[1] = a[im+2] & kmask1;
        aux[2] = ((a[im+4] >> 0) & kmask2) | ((a[im+0] & kmask3) >> 2);
        aux[3] = ((a[im+4] >> 4) & kmask2) | ((a[im+2] & kmask3) >> 2);

        sycl::float4 sum = {0.f, 0.f, 0.f, 0.f};
        float smin = 0;
        const uint16_t * q1 = (const uint16_t *)ql1;
        const uint16_t * q2 = q1 + 32;
        q16[0] = q1[0] & 0x0f0f;
        q16[1] = q1[8] & 0x0f0f;
        q16[2] = (q1[0] >> 4) & 0x0f0f;
        q16[3] = (q1[8] >> 4) & 0x0f0f;
        q16[4] = q2[0] & 0x0f0f;
        q16[5] = q2[8] & 0x0f0f;
        q16[6] = (q2[0] >> 4) & 0x0f0f;
        q16[7] = (q2[8] >> 4) & 0x0f0f;
        for (int l = 0; l < n; ++l) {
            sum.x() +=
                y1[l + 0] * (q4[l + 0] + (qh[l + 0] & (hm1 << 0) ? 16 : 0)) +
                y1[l + 16] * (q4[l + 2] + (qh[l + 16] & (hm1 << 0) ? 16 : 0));
            sum.y() +=
                y1[l + 32] * (q4[l + 4] + (qh[l + 0] & (hm1 << 1) ? 16 : 0)) +
                y1[l + 48] * (q4[l + 6] + (qh[l + 16] & (hm1 << 1) ? 16 : 0));
            sum.z() +=
                y2[l + 0] * (q4[l + 8] + (qh[l + 0] & (hm2 << 0) ? 16 : 0)) +
                y2[l + 16] * (q4[l + 10] + (qh[l + 16] & (hm2 << 0) ? 16 : 0));
            sum.w() +=
                y2[l + 32] * (q4[l + 12] + (qh[l + 0] & (hm2 << 1) ? 16 : 0)) +
                y2[l + 48] * (q4[l + 14] + (qh[l + 16] & (hm2 << 1) ? 16 : 0));
            smin += (y1[l] + y1[l+16]) * sc[2] + (y1[l+32] + y1[l+48]) * sc[3]
                  + (y2[l] + y2[l+16]) * sc[6] + (y2[l+32] + y2[l+48]) * sc[7];
        }
        tmp += dall * (sum.x() * sc[0] + sum.y() * sc[1] + sum.z() * sc[4] +
                       sum.w() * sc[5]) -
               dmin * smin;
    }

#else
    const int tid = item_ct1.get_local_id(2)/(2*K_QUANTS_PER_ITERATION);  // 0...15
    const int ix  = item_ct1.get_local_id(2)%(2*K_QUANTS_PER_ITERATION);
    const int step = tid * K_QUANTS_PER_ITERATION;
    const int im = step/8;
    const int in = step%8;

    for (int i = ix; i < num_blocks_per_row; i += 2*K_QUANTS_PER_ITERATION) {
        const uint8_t * q = x[i].qs + step;
        const int8_t  * s = x[i].scales;
        const float   * y = yy + i*QK_K + step;
        const float     d = x[i].d;
        float sum = 0.f;
        for (int j = 0; j < K_QUANTS_PER_ITERATION; ++j) {
            const uint8_t h = x[i].qh[in+j] >> im;
            sum += y[j+ 0] * d * s[0] * ((q[j+ 0] & 0xF) - ((h >> 0) & 1 ? 0 : 16))
                 + y[j+16] * d * s[1] * ((q[j+16] & 0xF) - ((h >> 2) & 1 ? 0 : 16))
                 + y[j+32] * d * s[2] * ((q[j+ 0] >>  4) - ((h >> 4) & 1 ? 0 : 16))
                 + y[j+48] * d * s[3] * ((q[j+16] >>  4) - ((h >> 6) & 1 ? 0 : 16));
        }
        tmp += sum;
    }
#endif

    // sum up partial sums and write back result
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        tmp +=
            dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), tmp, mask);
    }

    if (item_ct1.get_local_id(2) == 0) {
        dst[row] = tmp;
    }
}

static void dequantize_mul_mat_vec_q6_k(const void * __restrict__ vx, const float * __restrict__ yy, float * __restrict__ dst, const int ncols, int nrows,
                                        const sycl::nd_item<3> &item_ct1) {

    static_assert(16%K_QUANTS_PER_ITERATION == 0, "16 must be divisible by K_QUANTS_PER_ITERATION");

    const int row = item_ct1.get_group(2) * item_ct1.get_local_range(1) +
                    item_ct1.get_local_id(1);
    if (row > nrows) return;

    const int num_blocks_per_row = ncols / QK_K;
    const int ib0 = row*num_blocks_per_row;

    const block_q6_K * x = (const block_q6_K *)vx + ib0;

#if QK_K == 256

    const int tid =
        item_ct1.get_local_id(2) / K_QUANTS_PER_ITERATION; // 0...31 or 0...16
    const int ix =
        item_ct1.get_local_id(2) % K_QUANTS_PER_ITERATION; // 0 or 0, 1

    const int step = 16/K_QUANTS_PER_ITERATION;          // 16 or 8

    const int im = tid/step;                             // 0 or 1. 0 computes 0..., 1 computes 128...
    const int in = tid - step*im;                        // 0...15 or 0...7

#if K_QUANTS_PER_ITERATION == 1
    const int l0 = K_QUANTS_PER_ITERATION*in;            // 0...15
    const int is = 0;
#else
    const int l0 = 4 * in;                               // 0, 4, 8, ..., 28
    const int is = in / 4;
#endif
    const int ql_offset = 64*im + l0;
    const int qh_offset = 32*im + l0;
    const int s_offset  =  8*im + is;
    const int y_offset = 128*im + l0;

    float tmp = 0; // partial sum for thread in warp

    for (int i = ix; i < num_blocks_per_row; i += K_QUANTS_PER_ITERATION) {

        const float   * y  = yy + i * QK_K + y_offset;
        const uint8_t * ql = x[i].ql + ql_offset;
        const uint8_t * qh = x[i].qh + qh_offset;
        const int8_t  * s  = x[i].scales + s_offset;

        const float d = x[i].d;

#if K_QUANTS_PER_ITERATION == 1
        float sum = y[ 0] * s[0] * d * ((int8_t)((ql[ 0] & 0xF) | ((qh[ 0] & 0x03) << 4)) - 32)
                  + y[16] * s[1] * d * ((int8_t)((ql[16] & 0xF) | ((qh[16] & 0x03) << 4)) - 32)
                  + y[32] * s[2] * d * ((int8_t)((ql[32] & 0xF) | ((qh[ 0] & 0x0c) << 2)) - 32)
                  + y[48] * s[3] * d * ((int8_t)((ql[48] & 0xF) | ((qh[16] & 0x0c) << 2)) - 32)
                  + y[64] * s[4] * d * ((int8_t)((ql[ 0]  >> 4) | ((qh[ 0] & 0x30) >> 0)) - 32)
                  + y[80] * s[5] * d * ((int8_t)((ql[16]  >> 4) | ((qh[16] & 0x30) >> 0)) - 32)
                  + y[96] * s[6] * d * ((int8_t)((ql[32]  >> 4) | ((qh[ 0] & 0xc0) >> 2)) - 32)
                  +y[112] * s[7] * d * ((int8_t)((ql[48]  >> 4) | ((qh[16] & 0xc0) >> 2)) - 32);
        tmp += sum;
#else
        float sum = 0;
        for (int l = 0; l < 4; ++l) {
            sum += y[l+ 0] * s[0] * d * ((int8_t)((ql[l+ 0] & 0xF) | (((qh[l] >> 0) & 3) << 4)) - 32)
                 + y[l+32] * s[2] * d * ((int8_t)((ql[l+32] & 0xF) | (((qh[l] >> 2) & 3) << 4)) - 32)
                 + y[l+64] * s[4] * d * ((int8_t)((ql[l+ 0]  >> 4) | (((qh[l] >> 4) & 3) << 4)) - 32)
                 + y[l+96] * s[6] * d * ((int8_t)((ql[l+32]  >> 4) | (((qh[l] >> 6) & 3) << 4)) - 32);
        }
        tmp += sum;
#endif

    }

#else

    const int tid = item_ct1.get_local_id(2)/(2*K_QUANTS_PER_ITERATION);  // 0...7
    const int ix  = item_ct1.get_local_id(2)%(2*K_QUANTS_PER_ITERATION);  // 0...3

    const int step = tid * K_QUANTS_PER_ITERATION;

    float tmp = 0; // partial sum for thread in warp

    for (int i = ix; i < num_blocks_per_row; i += 2*K_QUANTS_PER_ITERATION) {

        const float   * y  = yy + i * QK_K + step;
        const uint8_t * ql = x[i].ql + step;
        const uint8_t * qh = x[i].qh + step;
        const int8_t  * s  = x[i].scales;

        const float d = x[i+0].d;

        float sum = 0;
        for (int j = 0; j < K_QUANTS_PER_ITERATION; ++j) {
            sum += y[j+ 0] * s[0] * d * ((int8_t)((ql[j+ 0] & 0xF) | ((qh[j] & 0x03) << 4)) - 32)
                 + y[j+16] * s[1] * d * ((int8_t)((ql[j+16] & 0xF) | ((qh[j] & 0x0c) << 2)) - 32)
                 + y[j+32] * s[2] * d * ((int8_t)((ql[j+ 0] >>  4) | ((qh[j] & 0x30) >> 0)) - 32)
                 + y[j+48] * s[3] * d * ((int8_t)((ql[j+16] >>  4) | ((qh[j] & 0xc0) >> 2)) - 32);
        }
        tmp += sum;

    }

#endif

    // sum up partial sums and write back result
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        tmp +=
            dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), tmp, mask);
    }

    if (tid == 0) {
        dst[row] = tmp;
    }
}

static void convert_f16(const void * vx, const int ib, const int iqs, dfloat2 & v){
    const sycl::half *x = (const sycl::half *)vx;

    // automatic half -> float type cast if dfloat == float
    v.x() = x[ib + iqs + 0];
    v.y() = x[ib + iqs + 1];
}

static void convert_f32(const void * vx, const int ib, const int iqs, dfloat2 & v){
    const float * x = (const float *) vx;

    // automatic half -> float type cast if dfloat == float
    v.x() = x[ib + iqs + 0];
    v.y() = x[ib + iqs + 1];
}

static void quantize_q8_1(const float * __restrict__ x, void * __restrict__ vy, const int kx, const int kx_padded,
                          const sycl::nd_item<3> &item_ct1) {
    const int ix = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                   item_ct1.get_local_id(2);

    if (ix >= kx_padded) {
        return;
    }

    const int iy = item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                   item_ct1.get_local_id(1);

    const int i_padded = iy*kx_padded + ix;

    block_q8_1 * y = (block_q8_1 *) vy;

    const int ib = i_padded / QK8_1; // block index
    const int iqs = i_padded % QK8_1; // quant index

    const float xi = ix < kx ? x[iy*kx + ix] : 0.0f;
    float amax = sycl::fabs((float)xi);
    float sum = xi;

#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        amax = sycl::fmax(amax, dpct::permute_sub_group_by_xor(
                                    item_ct1.get_sub_group(), amax, mask));
        sum +=
            dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), sum, mask);
    }

    const float d = amax / 127;
    const int8_t q = amax == 0.0f ? 0 : sycl::round(xi / d);

    y[ib].qs[iqs] = q;

    if (iqs > 0) {
        return;
    }

    reinterpret_cast<sycl::half &>(y[ib].ds.x()) = d;
    reinterpret_cast<sycl::half &>(y[ib].ds.y()) = sum;
}

template<int qk, int qr, dequantize_kernel_t dequantize_kernel, typename dst_t>
static void k_get_rows(
            const void * src0, const int32_t * src1, dst_t * dst,
            int64_t ne00, /*int64_t ne01, int64_t ne02, int64_t ne03,*/
            /*int64_t ne10, int64_t ne11,*/ int64_t ne12, /*int64_t ne13,*/
            /*size_t s0,*/ size_t s1, size_t s2, size_t s3,
            /*size_t nb00,*/ size_t nb01, size_t nb02, size_t nb03,
            size_t s10, size_t s11, size_t s12,
            const sycl::nd_item<3> &item_ct1/*, size_t s13*/) {

    const int i00 = (item_ct1.get_group(2) * item_ct1.get_local_range(2) +
                     item_ct1.get_local_id(2)) *
                    2;
    const int i10 = item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                    item_ct1.get_local_id(1);
    const int i11 = (item_ct1.get_group(0) * item_ct1.get_local_range(0) +
                     item_ct1.get_local_id(0)) /
                    ne12;
    const int i12 = (item_ct1.get_group(0) * item_ct1.get_local_range(0) +
                     item_ct1.get_local_id(0)) %
                    ne12;

    if (i00 >= ne00) {
        return;
    }

    const int i01 = src1[i10*s10 + i11*s11 + i12*s12];

    dst_t * dst_row = dst + i10*s1 + i11*s2 + i12*s3;
    const void * src0_row = (const char *)src0 + i01*nb01 + i11*nb02 + i12*nb03;

    const int ib = i00/qk; // block index
    const int iqs = (i00%qk)/qr; // quant index
    const int iybs = i00 - i00%qk; // dst block start index
    const int y_offset = qr == 1 ? 1 : qk/2;

    // dequantize
    dfloat2 v;
    dequantize_kernel(src0_row, ib, iqs, v);

    dst_row[iybs + iqs + 0] = v.x();
    dst_row[iybs + iqs + y_offset] = v.y();
}

template<typename src0_t, typename dst_t>
static void k_get_rows_float(
            const src0_t * src0, const int32_t * src1, dst_t * dst,
            int64_t ne00, /*int64_t ne01, int64_t ne02, int64_t ne03,*/
            /*int64_t ne10, int64_t ne11,*/ int64_t ne12, /*int64_t ne13,*/
            /*size_t s0,*/ size_t s1, size_t s2, size_t s3,
            /*size_t nb00,*/ size_t nb01, size_t nb02, size_t nb03,
            size_t s10, size_t s11, size_t s12,
            const sycl::nd_item<3> &item_ct1/*, size_t s13*/) {

    const int i00 = item_ct1.get_group(2) * item_ct1.get_local_range(2) +
                    item_ct1.get_local_id(2);
    const int i10 = item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                    item_ct1.get_local_id(1);
    const int i11 = (item_ct1.get_group(0) * item_ct1.get_local_range(0) +
                     item_ct1.get_local_id(0)) /
                    ne12;
    const int i12 = (item_ct1.get_group(0) * item_ct1.get_local_range(0) +
                     item_ct1.get_local_id(0)) %
                    ne12;

    if (i00 >= ne00) {
        return;
    }

    const int i01 = src1[i10*s10 + i11*s11 + i12*s12];

    dst_t * dst_row = dst + i10*s1 + i11*s2 + i12*s3;
    const src0_t * src0_row = (const src0_t *)((const char *)src0 + i01*nb01 + i11*nb02 + i12*nb03);

    dst_row[i00] = src0_row[i00];
}

template <int qk, int qr, dequantize_kernel_t dequantize_kernel, typename dst_t>
static void dequantize_block(const void * __restrict__ vx, dst_t * __restrict__ y, const int k,
                             const sycl::nd_item<3> &item_ct1) {
    const int i = 2 * (item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                       item_ct1.get_local_id(2));

    if (i >= k) {
        return;
    }

    const int ib = i/qk; // block index
    const int iqs = (i%qk)/qr; // quant index
    const int iybs = i - i%qk; // y block start index
    const int y_offset = qr == 1 ? 1 : qk/2;

    // dequantize
    dfloat2 v;
    dequantize_kernel(vx, ib, iqs, v);

    y[iybs + iqs + 0] = v.x();
    y[iybs + iqs + y_offset] = v.y();
}

template <typename src_t, typename dst_t>
static void convert_unary(const void * __restrict__ vx, dst_t * __restrict__ y, const int k,
                          const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    if (i >= k) {
        return;
    }

    const src_t * x = (src_t *) vx;

    y[i] = x[i];
}

template <int mmq_y>
static __dpct_inline__ void
allocate_tiles_q5_0(int **x_ql, sycl::half2 **x_dm, int **x_qh, int **x_sc,
                    int *tile_x_ql_q5_0, float *tile_x_d_q5_0) {
    (void)x_qh; (void)x_sc;

    *x_ql = tile_x_ql_q5_0;
    *x_dm = (sycl::half2 *)tile_x_d_q5_0;
}

template <int mmq_y, int nwarps, bool need_check>
static __dpct_inline__ void
load_tiles_q5_0(const void *__restrict__ vx, int *__restrict__ x_ql,
                sycl::half2 *__restrict__ x_dm, int *__restrict__ x_qh,
                int *__restrict__ x_sc, const int &i_offset, const int &i_max,
                const int &k, const int &blocks_per_row) {
    (void)x_qh; (void)x_sc;

    GGML_SYCL_ASSUME(i_offset >= 0);
    GGML_SYCL_ASSUME(i_offset <  nwarps);
    GGML_SYCL_ASSUME(k >= 0);
    GGML_SYCL_ASSUME(k <  WARP_SIZE);

    const int kbx  = k / QI5_0;
    const int kqsx = k % QI5_0;

    const block_q5_0 * bx0 = (const block_q5_0 *) vx;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps) {
        int i = i0 + i_offset;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q5_0 * bxi = bx0 + i*blocks_per_row + kbx;

        const int ql = get_int_from_uint8(bxi->qs, kqsx);
        const int qh = get_int_from_uint8(bxi->qh, 0) >> (4 * (k % QI5_0));

        int qs0 = (ql >>  0)   & 0x0F0F0F0F;
        qs0    |= (qh <<  4)   & 0x00000010;  // 0 ->  4
        qs0    |= (qh << 11)   & 0x00001000;  // 1 -> 12
        qs0    |= (qh << 18)   & 0x00100000;  // 2 -> 20
        qs0    |= (qh << 25)   & 0x10000000;  // 3 -> 28
        qs0 = dpct::vectorized_binary<sycl::char4>(
            qs0, 0x10101010, dpct::sub_sat()); // subtract 16

        x_ql[i * (2*WARP_SIZE + 1) + 2*k+0] = qs0;

        int qs1 = (ql >>  4)   & 0x0F0F0F0F;
        qs1    |= (qh >> 12)   & 0x00000010;  // 16 ->  4
        qs1    |= (qh >>  5)   & 0x00001000;  // 17 -> 12
        qs1    |= (qh <<  2)   & 0x00100000;  // 18 -> 20
        qs1    |= (qh <<  9)   & 0x10000000;  // 19 -> 28
        qs1 = dpct::vectorized_binary<sycl::char4>(
            qs1, 0x10101010, dpct::sub_sat()); // subtract 16

        x_ql[i * (2*WARP_SIZE + 1) + 2*k+1] = qs1;
    }

    const int blocks_per_tile_x_row = WARP_SIZE / QI5_0;
    const int kbxd = k % blocks_per_tile_x_row;
    float * x_dmf = (float *) x_dm;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * QI5_0) {
        int i = i0 + i_offset * QI5_0 + k / blocks_per_tile_x_row;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q5_0 * bxi = bx0 + i*blocks_per_row + kbxd;

        x_dmf[i * (WARP_SIZE/QI5_0) + i / QI5_0 + kbxd] = bxi->d;
    }
}

static __dpct_inline__ float vec_dot_q5_0_q8_1_mul_mat(
    const int *__restrict__ x_ql, const sycl::half2 *__restrict__ x_dm,
    const int *__restrict__ x_qh, const int *__restrict__ x_sc,
    const int *__restrict__ y_qs, const sycl::half2 *__restrict__ y_ds,
    const int &i, const int &j, const int &k) {
    (void)x_qh; (void)x_sc;

    const int kyqs = k % (QI8_1/2) + QI8_1 * (k / (QI8_1/2));
    const int index_bx = i * (WARP_SIZE/QI5_0) + i/QI5_0 + k/QI5_0;
    const float * x_dmf = (const float *) x_dm;
    const float * y_df  = (const float *) y_ds;

    int u[2*VDR_Q5_0_Q8_1_MMQ];

#pragma unroll
    for (int l = 0; l < VDR_Q5_0_Q8_1_MMQ; ++l) {
        u[2*l+0] = y_qs[j * WARP_SIZE + (kyqs + l)         % WARP_SIZE];
        u[2*l+1] = y_qs[j * WARP_SIZE + (kyqs + l + QI5_0) % WARP_SIZE];
    }

    return vec_dot_q8_0_q8_1_impl<QR5_0*VDR_Q5_0_Q8_1_MMQ>
        (&x_ql[i * (2*WARP_SIZE + 1) + 2 * k], u, x_dmf[index_bx], y_df[j * (WARP_SIZE/QI8_1) + (2*k/QI8_1) % (WARP_SIZE/QI8_1)]);
}

template <int mmq_y>
static __dpct_inline__ void
allocate_tiles_q5_1(int **x_ql, sycl::half2 **x_dm, int **x_qh, int **x_sc,
                    int *tile_x_ql_q5_1, sycl::half2 *tile_x_dm_q5_1) {
    (void)x_qh; (void)x_sc;

    *x_ql = tile_x_ql_q5_1;
    *x_dm = tile_x_dm_q5_1;
}

template <int mmq_y, int nwarps, bool need_check>
static __dpct_inline__ void
load_tiles_q5_1(const void *__restrict__ vx, int *__restrict__ x_ql,
                sycl::half2 *__restrict__ x_dm, int *__restrict__ x_qh,
                int *__restrict__ x_sc, const int &i_offset, const int &i_max,
                const int &k, const int &blocks_per_row) {
    (void)x_qh; (void)x_sc;

    GGML_SYCL_ASSUME(i_offset >= 0);
    GGML_SYCL_ASSUME(i_offset < nwarps);
    GGML_SYCL_ASSUME(k >= 0);
    GGML_SYCL_ASSUME(k <  WARP_SIZE);

    const int kbx  = k / QI5_1;
    const int kqsx = k % QI5_1;

    const block_q5_1 * bx0 = (const block_q5_1 *) vx;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps) {
        int i = i0 + i_offset;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q5_1 * bxi = bx0 + i*blocks_per_row + kbx;

        const int ql = get_int_from_uint8_aligned(bxi->qs, kqsx);
        const int qh = get_int_from_uint8_aligned(bxi->qh, 0) >> (4 * (k % QI5_1));

        int qs0 = (ql >>  0) & 0x0F0F0F0F;
        qs0    |= (qh <<  4) & 0x00000010; // 0 ->  4
        qs0    |= (qh << 11) & 0x00001000; // 1 -> 12
        qs0    |= (qh << 18) & 0x00100000; // 2 -> 20
        qs0    |= (qh << 25) & 0x10000000; // 3 -> 28

        x_ql[i * (2*WARP_SIZE + 1) + 2*k+0] = qs0;

        int qs1 = (ql >>  4) & 0x0F0F0F0F;
        qs1    |= (qh >> 12) & 0x00000010; // 16 ->  4
        qs1    |= (qh >>  5) & 0x00001000; // 17 -> 12
        qs1    |= (qh <<  2) & 0x00100000; // 18 -> 20
        qs1    |= (qh <<  9) & 0x10000000; // 19 -> 28

        x_ql[i * (2*WARP_SIZE + 1) + 2*k+1] = qs1;
    }

    const int blocks_per_tile_x_row = WARP_SIZE / QI5_1;
    const int kbxd = k % blocks_per_tile_x_row;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * QI5_1) {
        int i = i0 + i_offset * QI5_1 + k / blocks_per_tile_x_row;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q5_1 * bxi = bx0 + i*blocks_per_row + kbxd;

        x_dm[i * (WARP_SIZE/QI5_1) + i / QI5_1 + kbxd] = bxi->dm;
    }
}

static __dpct_inline__ float vec_dot_q5_1_q8_1_mul_mat(
    const int *__restrict__ x_ql, const sycl::half2 *__restrict__ x_dm,
    const int *__restrict__ x_qh, const int *__restrict__ x_sc,
    const int *__restrict__ y_qs, const sycl::half2 *__restrict__ y_ds,
    const int &i, const int &j, const int &k) {
    (void)x_qh; (void)x_sc;

    const int kyqs = k % (QI8_1/2) + QI8_1 * (k / (QI8_1/2));
    const int index_bx = i * (WARP_SIZE/QI5_1) + + i/QI5_1 + k/QI5_1;

    int u[2*VDR_Q5_1_Q8_1_MMQ];

#pragma unroll
    for (int l = 0; l < VDR_Q5_1_Q8_1_MMQ; ++l) {
        u[2*l+0] = y_qs[j * WARP_SIZE + (kyqs + l)         % WARP_SIZE];
        u[2*l+1] = y_qs[j * WARP_SIZE + (kyqs + l + QI5_1) % WARP_SIZE];
    }

    return vec_dot_q8_1_q8_1_impl<QR5_1*VDR_Q5_1_Q8_1_MMQ>
        (&x_ql[i * (2*WARP_SIZE + 1) + 2 * k], u, x_dm[index_bx], y_ds[j * (WARP_SIZE/QI8_1) + (2*k/QI8_1) % (WARP_SIZE/QI8_1)]);
}

template <int mmq_y>
static __dpct_inline__ void
allocate_tiles_q8_0(int **x_ql, sycl::half2 **x_dm, int **x_qh, int **x_sc,
                    int *tile_x_qs_q8_0, float *tile_x_d_q8_0) {
    (void)x_qh; (void)x_sc;

    *x_ql = tile_x_qs_q8_0;
    *x_dm = (sycl::half2 *)tile_x_d_q8_0;
}

template <int mmq_y, int nwarps, bool need_check>
static __dpct_inline__ void
load_tiles_q8_0(const void *__restrict__ vx, int *__restrict__ x_ql,
                sycl::half2 *__restrict__ x_dm, int *__restrict__ x_qh,
                int *__restrict__ x_sc, const int &i_offset, const int &i_max,
                const int &k, const int &blocks_per_row) {
    (void)x_qh; (void)x_sc;

    GGML_SYCL_ASSUME(i_offset >= 0);
    GGML_SYCL_ASSUME(i_offset <  nwarps);
    GGML_SYCL_ASSUME(k >= 0);
    GGML_SYCL_ASSUME(k <  WARP_SIZE);

    const int kbx  = k / QI8_0;
    const int kqsx = k % QI8_0;
    float * x_dmf = (float *) x_dm;

    const block_q8_0 * bx0 = (const block_q8_0 *) vx;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps) {
        int i = i0 + i_offset;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q8_0 * bxi = bx0 + i*blocks_per_row + kbx;

        x_ql[i * (WARP_SIZE + 1) + k] = get_int_from_int8(bxi->qs, kqsx);
    }

    const int blocks_per_tile_x_row = WARP_SIZE / QI8_0;
    const int kbxd = k % blocks_per_tile_x_row;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * QI8_0) {
        int i = i0 + i_offset * QI8_0 + k / blocks_per_tile_x_row;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q8_0 * bxi = bx0 + i*blocks_per_row + kbxd;

        x_dmf[i * (WARP_SIZE/QI8_0) + i / QI8_0 + kbxd] = bxi->d;
    }
}

static __dpct_inline__ float vec_dot_q8_0_q8_1_mul_mat(
    const int *__restrict__ x_ql, const sycl::half2 *__restrict__ x_dm,
    const int *__restrict__ x_qh, const int *__restrict__ x_sc,
    const int *__restrict__ y_qs, const sycl::half2 *__restrict__ y_ds,
    const int &i, const int &j, const int &k) {
    (void)x_qh; (void)x_sc;

    const float * x_dmf = (const float *) x_dm;
    const float * y_df  = (const float *) y_ds;

    return vec_dot_q8_0_q8_1_impl<VDR_Q8_0_Q8_1_MMQ>
        (&x_ql[i * (WARP_SIZE + 1) + k], &y_qs[j * WARP_SIZE + k], x_dmf[i * (WARP_SIZE/QI8_0) + i/QI8_0 + k/QI8_0],
         y_df[j * (WARP_SIZE/QI8_1) + k/QI8_1]);
}

template <int mmq_y>
static __dpct_inline__ void
allocate_tiles_q2_K(int **x_ql, sycl::half2 **x_dm, int **x_qh, int **x_sc,
                    int *tile_x_ql_q2_K, sycl::half2 *tile_x_dm_q2_K,
                    int *tile_x_sc_q2_K) {
    (void)x_qh;

    *x_ql = tile_x_ql_q2_K;
    *x_dm = tile_x_dm_q2_K;
    *x_sc = tile_x_sc_q2_K;
}

template <int mmq_y, int nwarps, bool need_check>
static __dpct_inline__ void
load_tiles_q2_K(const void *__restrict__ vx, int *__restrict__ x_ql,
                sycl::half2 *__restrict__ x_dm, int *__restrict__ x_qh,
                int *__restrict__ x_sc, const int &i_offset, const int &i_max,
                const int &k, const int &blocks_per_row) {
    (void)x_qh;

    GGML_SYCL_ASSUME(i_offset >= 0);
    GGML_SYCL_ASSUME(i_offset <  nwarps);
    GGML_SYCL_ASSUME(k >= 0);
    GGML_SYCL_ASSUME(k <  WARP_SIZE);

    const int kbx  = k / QI2_K;
    const int kqsx = k % QI2_K;

    const block_q2_K * bx0 = (const block_q2_K *) vx;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps) {
        int i = i0 + i_offset;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q2_K * bxi = bx0 + i*blocks_per_row + kbx;

        x_ql[i * (WARP_SIZE + 1) + k] = get_int_from_uint8_aligned(bxi->qs, kqsx);
    }

    const int blocks_per_tile_x_row = WARP_SIZE / QI2_K;
    const int kbxd = k % blocks_per_tile_x_row;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * QI2_K) {
        int i = (i0 + i_offset * QI2_K + k / blocks_per_tile_x_row) % mmq_y;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q2_K * bxi = bx0 + i*blocks_per_row + kbxd;

        x_dm[i * (WARP_SIZE/QI2_K) + i / QI2_K + kbxd] = bxi->dm;
    }

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * 4) {
        int i = i0 + i_offset * 4 + k / (WARP_SIZE/4);

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q2_K * bxi = bx0 + i*blocks_per_row + (k % (WARP_SIZE/4)) / (QI2_K/4);

        x_sc[i * (WARP_SIZE/4) + i / 4 + k % (WARP_SIZE/4)] = get_int_from_uint8_aligned(bxi->scales, k % (QI2_K/4));
    }
}

static __dpct_inline__ float vec_dot_q2_K_q8_1_mul_mat(
    const int *__restrict__ x_ql, const sycl::half2 *__restrict__ x_dm,
    const int *__restrict__ x_qh, const int *__restrict__ x_sc,
    const int *__restrict__ y_qs, const sycl::half2 *__restrict__ y_ds,
    const int &i, const int &j, const int &k) {
    (void)x_qh;

    const int kbx = k / QI2_K;
    const int ky  = (k % QI2_K) * QR2_K;
    const float * y_df = (const float *) y_ds;

    int v[QR2_K*VDR_Q2_K_Q8_1_MMQ];

    const int kqsx = i * (WARP_SIZE + 1) + kbx*QI2_K + (QI2_K/2) * (ky/(2*QI2_K)) + ky % (QI2_K/2);
    const int shift = 2 * ((ky % (2*QI2_K)) / (QI2_K/2));

#pragma unroll
    for (int l = 0; l < QR2_K*VDR_Q2_K_Q8_1_MMQ; ++l) {
        v[l] = (x_ql[kqsx + l] >> shift) & 0x03030303;
    }

    const uint8_t * scales = ((const uint8_t *) &x_sc[i * (WARP_SIZE/4) + i/4 + kbx*4]) + ky/4;

    const int index_y = j * WARP_SIZE + (QR2_K*k) % WARP_SIZE;
    return vec_dot_q2_K_q8_1_impl_mmq(v, &y_qs[index_y], scales, x_dm[i * (WARP_SIZE/QI2_K) + i/QI2_K + kbx], y_df[index_y/QI8_1]);
}

template <int mmq_y>
static __dpct_inline__ void
allocate_tiles_q3_K(int **x_ql, sycl::half2 **x_dm, int **x_qh, int **x_sc,
                    int *tile_x_ql_q3_K, sycl::half2 *tile_x_dm_q3_K,
                    int *tile_x_qh_q3_K, int *tile_x_sc_q3_K) {

    *x_ql = tile_x_ql_q3_K;
    *x_dm = tile_x_dm_q3_K;
    *x_qh = tile_x_qh_q3_K;
    *x_sc = tile_x_sc_q3_K;
}

template <int mmq_y, int nwarps, bool need_check>
static __dpct_inline__ void
load_tiles_q3_K(const void *__restrict__ vx, int *__restrict__ x_ql,
                sycl::half2 *__restrict__ x_dm, int *__restrict__ x_qh,
                int *__restrict__ x_sc, const int &i_offset, const int &i_max,
                const int &k, const int &blocks_per_row) {

    GGML_SYCL_ASSUME(i_offset >= 0);
    GGML_SYCL_ASSUME(i_offset <  nwarps);
    GGML_SYCL_ASSUME(k >= 0);
    GGML_SYCL_ASSUME(k <  WARP_SIZE);

    const int kbx  = k / QI3_K;
    const int kqsx = k % QI3_K;

    const block_q3_K * bx0 = (const block_q3_K *) vx;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps) {
        int i = i0 + i_offset;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q3_K * bxi = bx0 + i*blocks_per_row + kbx;

        x_ql[i * (WARP_SIZE + 1) + k] = get_int_from_uint8(bxi->qs, kqsx);
    }

    const int blocks_per_tile_x_row = WARP_SIZE / QI3_K;
    const int kbxd = k % blocks_per_tile_x_row;
    float * x_dmf = (float *) x_dm;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * QI3_K) {
        int i = (i0 + i_offset * QI3_K + k / blocks_per_tile_x_row) % mmq_y;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q3_K * bxi = bx0 + i*blocks_per_row + kbxd;

        x_dmf[i * (WARP_SIZE/QI3_K) + i / QI3_K + kbxd] = bxi->d;
    }

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * 2) {
        int i = i0 + i_offset * 2 + k / (WARP_SIZE/2);

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q3_K * bxi = bx0 + i*blocks_per_row + (k % (WARP_SIZE/2)) / (QI3_K/2);

        // invert the mask with ~ so that a 0/1 results in 4/0 being subtracted
        x_qh[i * (WARP_SIZE/2) + i / 2 + k % (WARP_SIZE/2)] = ~get_int_from_uint8(bxi->hmask, k % (QI3_K/2));
    }

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * 4) {
        int i = i0 + i_offset * 4 + k / (WARP_SIZE/4);

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q3_K * bxi = bx0 + i*blocks_per_row + (k % (WARP_SIZE/4)) / (QI3_K/4);

        const int ksc = k % (QI3_K/4);

        const int ksc_low = ksc % (QI3_K/8);
        const int shift_low = 4 * (ksc / (QI3_K/8));
        const int sc_low = (get_int_from_uint8(bxi->scales, ksc_low) >> shift_low) & 0x0F0F0F0F;

        const int ksc_high = QI3_K/8;
        const int shift_high = 2 * ksc;
        const int sc_high = ((get_int_from_uint8(bxi->scales, ksc_high) >> shift_high) << 4) & 0x30303030;

        const int sc = dpct::vectorized_binary<sycl::char4>(
            sc_low | sc_high, 0x20202020, dpct::sub_sat());

        x_sc[i * (WARP_SIZE/4) + i / 4 + k % (WARP_SIZE/4)] = sc;
    }
}

static __dpct_inline__ float vec_dot_q3_K_q8_1_mul_mat(
    const int *__restrict__ x_ql, const sycl::half2 *__restrict__ x_dm,
    const int *__restrict__ x_qh, const int *__restrict__ x_sc,
    const int *__restrict__ y_qs, const sycl::half2 *__restrict__ y_ds,
    const int &i, const int &j, const int &k) {

    const int kbx  = k / QI3_K;
    const int ky  = (k % QI3_K) * QR3_K;
    const float * x_dmf = (const float *) x_dm;
    const float * y_df  = (const float *) y_ds;

    const int8_t * scales = ((const int8_t *) (x_sc + i * (WARP_SIZE/4) + i/4 + kbx*4)) + ky/4;

    int v[QR3_K*VDR_Q3_K_Q8_1_MMQ];

#pragma unroll
    for (int l = 0; l < QR3_K*VDR_Q3_K_Q8_1_MMQ; ++l) {
        const int kqsx = i * (WARP_SIZE + 1) + kbx*QI3_K + (QI3_K/2) * (ky/(2*QI3_K)) + ky % (QI3_K/2);
        const int shift = 2 * ((ky % 32) / 8);
        const int vll = (x_ql[kqsx + l] >> shift) & 0x03030303;

        const int vh = x_qh[i * (WARP_SIZE/2) + i/2 + kbx * (QI3_K/2) + (ky+l)%8] >> ((ky+l) / 8);
        const int vlh = (vh << 2) & 0x04040404;

        v[l] = dpct::vectorized_binary<sycl::char4>(vll, vlh, dpct::sub_sat());
    }

    const int index_y = j * WARP_SIZE + (k*QR3_K) % WARP_SIZE;
    return vec_dot_q3_K_q8_1_impl_mmq(v, &y_qs[index_y], scales, x_dmf[i * (WARP_SIZE/QI3_K) + i/QI3_K + kbx], y_df[index_y/QI8_1]);
}


template <int mmq_y>
static __dpct_inline__ void
allocate_tiles_q4_K(int **x_ql, sycl::half2 **x_dm, int **x_qh, int **x_sc,
                    int *tile_x_ql_q4_K, sycl::half2 *tile_x_dm_q4_K,
                    int *tile_x_sc_q4_K) {
    (void)x_qh;

    *x_ql = tile_x_ql_q4_K;
    *x_dm = tile_x_dm_q4_K;
    *x_sc = tile_x_sc_q4_K;
}

template <int mmq_y, int nwarps, bool need_check>
static __dpct_inline__ void
load_tiles_q4_K(const void *__restrict__ vx, int *__restrict__ x_ql,
                sycl::half2 *__restrict__ x_dm, int *__restrict__ x_qh,
                int *__restrict__ x_sc, const int &i_offset, const int &i_max,
                const int &k, const int &blocks_per_row) {
    (void)x_qh;

    GGML_SYCL_ASSUME(i_offset >= 0);
    GGML_SYCL_ASSUME(i_offset <  nwarps);
    GGML_SYCL_ASSUME(k >= 0);
    GGML_SYCL_ASSUME(k <  WARP_SIZE);

    const int kbx  = k / QI4_K; // == 0 if QK_K == 256
    const int kqsx = k % QI4_K; // == k if QK_K == 256

    const block_q4_K * bx0 = (const block_q4_K *) vx;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps) {
        int i = i0 + i_offset;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q4_K * bxi = bx0 + i*blocks_per_row + kbx;

        x_ql[i * (WARP_SIZE + 1) + k] = get_int_from_uint8_aligned(bxi->qs, kqsx);
    }

    const int blocks_per_tile_x_row = WARP_SIZE / QI4_K; // == 1 if QK_K == 256
    const int kbxd = k % blocks_per_tile_x_row;          // == 0 if QK_K == 256

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * QI4_K) {
        int i = (i0 + i_offset * QI4_K + k / blocks_per_tile_x_row) % mmq_y;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q4_K * bxi = bx0 + i*blocks_per_row + kbxd;

#if QK_K == 256
        x_dm[i * (WARP_SIZE/QI4_K) + i / QI4_K + kbxd] = bxi->dm;
#else
        x_dm[i * (WARP_SIZE/QI4_K) + i / QI4_K + kbxd] = {bxi->dm[0], bxi->dm[1]};
#endif
    }

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * 8) {
        int i = (i0 + i_offset * 8 + k / (WARP_SIZE/8)) % mmq_y;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q4_K * bxi = bx0 + i*blocks_per_row + (k % (WARP_SIZE/8)) / (QI4_K/8);

        const int * scales = (const int *) bxi->scales;

        const int ksc = k % (WARP_SIZE/8);

        // scale arrangement after the following two lines: sc0,...,sc3, sc4,...,sc7, m0,...,m3, m4,...,m8
        int scales8 = (scales[(ksc%2) + (ksc!=0)] >> (4 * (ksc & (ksc/2)))) & 0x0F0F0F0F; // lower 4 bits
        scales8    |= (scales[ksc/2]              >> (2 * (ksc % 2)))       & 0x30303030; // upper 2 bits

        x_sc[i * (WARP_SIZE/8) + i / 8 + ksc] = scales8;
    }
}

static __dpct_inline__ float vec_dot_q4_K_q8_1_mul_mat(
    const int *__restrict__ x_ql, const sycl::half2 *__restrict__ x_dm,
    const int *__restrict__ x_qh, const int *__restrict__ x_sc,
    const int *__restrict__ y_qs, const sycl::half2 *__restrict__ y_ds,
    const int &i, const int &j, const int &k) {
    (void)x_qh;

    const uint8_t * sc = ((const uint8_t *) &x_sc[i * (WARP_SIZE/8) + i/8 + k/16]) + 2*((k % 16) / 8);

    const int index_y = j * WARP_SIZE + (QR4_K*k) % WARP_SIZE;
    return vec_dot_q4_K_q8_1_impl_mmq(&x_ql[i * (WARP_SIZE + 1) + k], &y_qs[index_y], sc, sc+8,
                                      x_dm[i * (WARP_SIZE/QI4_K) + i/QI4_K], &y_ds[index_y/QI8_1]);
}

template <int mmq_y>
static __dpct_inline__ void
allocate_tiles_q5_K(int **x_ql, sycl::half2 **x_dm, int **x_qh, int **x_sc,
                    int *tile_x_ql_q5_K, sycl::half2 *tile_x_dm_q5_K,
                    int *tile_x_sc_q5_K) {
    (void)x_qh;

    *x_ql = tile_x_ql_q5_K;
    *x_dm = tile_x_dm_q5_K;
    *x_sc = tile_x_sc_q5_K;
}

template <int mmq_y, int nwarps, bool need_check>
static __dpct_inline__ void
load_tiles_q5_K(const void *__restrict__ vx, int *__restrict__ x_ql,
                sycl::half2 *__restrict__ x_dm, int *__restrict__ x_qh,
                int *__restrict__ x_sc, const int &i_offset, const int &i_max,
                const int &k, const int &blocks_per_row) {
    (void)x_qh;

    GGML_SYCL_ASSUME(i_offset >= 0);
    GGML_SYCL_ASSUME(i_offset <  nwarps);
    GGML_SYCL_ASSUME(k >= 0);
    GGML_SYCL_ASSUME(k <  WARP_SIZE);

    const int kbx  = k / QI5_K; // == 0 if QK_K == 256
    const int kqsx = k % QI5_K; // == k if QK_K == 256

    const block_q5_K * bx0 = (const block_q5_K *) vx;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps) {
        int i = i0 + i_offset;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q5_K * bxi = bx0 + i*blocks_per_row + kbx;
        const int ky = QR5_K*kqsx;

        const int ql = get_int_from_uint8_aligned(bxi->qs, kqsx);
        const int ql0 = (ql >> 0) & 0x0F0F0F0F;
        const int ql1 = (ql >> 4) & 0x0F0F0F0F;

        const int qh = get_int_from_uint8_aligned(bxi->qh, kqsx % (QI5_K/4));
        const int qh0 = ((qh >> (2 * (kqsx / (QI5_K/4)) + 0)) << 4) & 0x10101010;
        const int qh1 = ((qh >> (2 * (kqsx / (QI5_K/4)) + 1)) << 4) & 0x10101010;

        const int kq0 = ky - ky % (QI5_K/2) + k % (QI5_K/4) + 0;
        const int kq1 = ky - ky % (QI5_K/2) + k % (QI5_K/4) + (QI5_K/4);

        x_ql[i * (2*WARP_SIZE + 1) + kq0] = ql0 | qh0;
        x_ql[i * (2*WARP_SIZE + 1) + kq1] = ql1 | qh1;
    }

    const int blocks_per_tile_x_row = WARP_SIZE / QI5_K; // == 1 if QK_K == 256
    const int kbxd = k % blocks_per_tile_x_row;          // == 0 if QK_K == 256

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * QI5_K) {
        int i = (i0 + i_offset * QI5_K + k / blocks_per_tile_x_row) % mmq_y;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q5_K * bxi = bx0 + i*blocks_per_row + kbxd;

#if QK_K == 256
        x_dm[i * (WARP_SIZE/QI5_K) + i / QI5_K + kbxd] = bxi->dm;
#endif
    }

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * 8) {
        int i = (i0 + i_offset * 8 + k / (WARP_SIZE/8)) % mmq_y;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q5_K * bxi = bx0 + i*blocks_per_row + (k % (WARP_SIZE/8)) / (QI5_K/8);

        const int * scales = (const int *) bxi->scales;

        const int ksc = k % (WARP_SIZE/8);

        // scale arrangement after the following two lines: sc0,...,sc3, sc4,...,sc7, m0,...,m3, m4,...,m8
        int scales8 = (scales[(ksc%2) + (ksc!=0)] >> (4 * (ksc & (ksc/2)))) & 0x0F0F0F0F; // lower 4 bits
        scales8    |= (scales[ksc/2]              >> (2 * (ksc % 2)))       & 0x30303030; // upper 2 bits

        x_sc[i * (WARP_SIZE/8) + i / 8 + ksc] = scales8;
    }
}

static __dpct_inline__ float vec_dot_q5_K_q8_1_mul_mat(
    const int *__restrict__ x_ql, const sycl::half2 *__restrict__ x_dm,
    const int *__restrict__ x_qh, const int *__restrict__ x_sc,
    const int *__restrict__ y_qs, const sycl::half2 *__restrict__ y_ds,
    const int &i, const int &j, const int &k) {
    (void)x_qh;

    const uint8_t * sc = ((const uint8_t *) &x_sc[i * (WARP_SIZE/8) + i/8 + k/16]) + 2 * ((k % 16) / 8);

    const int index_x = i * (QR5_K*WARP_SIZE + 1) +  QR5_K*k;
    const int index_y = j * WARP_SIZE             + (QR5_K*k) % WARP_SIZE;
    return vec_dot_q5_K_q8_1_impl_mmq(&x_ql[index_x], &y_qs[index_y], sc, sc+8,
                                      x_dm[i * (WARP_SIZE/QI5_K) + i/QI5_K], &y_ds[index_y/QI8_1]);
}

template <int mmq_y>
static __dpct_inline__ void
allocate_tiles_q6_K(int **x_ql, sycl::half2 **x_dm, int **x_qh, int **x_sc,
                    int *tile_x_ql, sycl::half2 *tile_x_dm, int *tile_x_sc) {
    (void)x_qh;

    *x_ql = tile_x_ql;
    *x_dm = tile_x_dm;
    *x_sc = tile_x_sc;
}

template <int mmq_y, int nwarps, bool need_check>
static __dpct_inline__ void
load_tiles_q6_K(const void *__restrict__ vx, int *__restrict__ x_ql,
                sycl::half2 *__restrict__ x_dm, int *__restrict__ x_qh,
                int *__restrict__ x_sc, const int &i_offset, const int &i_max,
                const int &k, const int &blocks_per_row) {
    (void)x_qh;

    GGML_SYCL_ASSUME(i_offset >= 0);
    GGML_SYCL_ASSUME(i_offset <  nwarps);
    GGML_SYCL_ASSUME(k >= 0);
    GGML_SYCL_ASSUME(k <  WARP_SIZE);

    const int kbx  = k / QI6_K; // == 0 if QK_K == 256
    const int kqsx = k % QI6_K; // == k if QK_K == 256

    const block_q6_K * bx0 = (const block_q6_K *) vx;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps) {
        int i = i0 + i_offset;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q6_K * bxi = bx0 + i*blocks_per_row + kbx;
        const int ky = QR6_K*kqsx;

        const int ql = get_int_from_uint8(bxi->ql, kqsx);
        const int ql0 = (ql >> 0) & 0x0F0F0F0F;
        const int ql1 = (ql >> 4) & 0x0F0F0F0F;

        const int qh = get_int_from_uint8(bxi->qh, (QI6_K/4) * (kqsx / (QI6_K/2)) + kqsx % (QI6_K/4));
        const int qh0 = ((qh >> (2 * ((kqsx % (QI6_K/2)) / (QI6_K/4)))) << 4) & 0x30303030;
        const int qh1 =  (qh >> (2 * ((kqsx % (QI6_K/2)) / (QI6_K/4))))       & 0x30303030;

        const int kq0 = ky - ky % QI6_K + k % (QI6_K/2) + 0;
        const int kq1 = ky - ky % QI6_K + k % (QI6_K/2) + (QI6_K/2);

        x_ql[i * (2 * WARP_SIZE + 1) + kq0] =
            dpct::vectorized_binary<sycl::char4>(ql0 | qh0, 0x20202020,
                                                 dpct::sub_sat());
        x_ql[i * (2 * WARP_SIZE + 1) + kq1] =
            dpct::vectorized_binary<sycl::char4>(ql1 | qh1, 0x20202020,
                                                 dpct::sub_sat());
    }

    const int blocks_per_tile_x_row = WARP_SIZE / QI6_K; // == 1 if QK_K == 256
    const int kbxd = k % blocks_per_tile_x_row;          // == 0 if QK_K == 256
    float * x_dmf = (float *) x_dm;

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * QI6_K) {
        int i = (i0 + i_offset * QI6_K + k / blocks_per_tile_x_row) % mmq_y;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q6_K * bxi = bx0 + i*blocks_per_row + kbxd;

        x_dmf[i * (WARP_SIZE/QI6_K) + i / QI6_K + kbxd] = bxi->d;
    }

#pragma unroll
    for (int i0 = 0; i0 < mmq_y; i0 += nwarps * 8) {
        int i = (i0 + i_offset * 8 + k / (WARP_SIZE/8)) % mmq_y;

        if (need_check) {
            i = sycl::min(i, i_max);
        }

        const block_q6_K * bxi = bx0 + i*blocks_per_row + (k % (WARP_SIZE/8)) / 4;

        x_sc[i * (WARP_SIZE/8) + i / 8 + k % (WARP_SIZE/8)] = get_int_from_int8(bxi->scales, k % (QI6_K/8));
    }
}

static __dpct_inline__ float vec_dot_q6_K_q8_1_mul_mat(
    const int *__restrict__ x_ql, const sycl::half2 *__restrict__ x_dm,
    const int *__restrict__ x_qh, const int *__restrict__ x_sc,
    const int *__restrict__ y_qs, const sycl::half2 *__restrict__ y_ds,
    const int &i, const int &j, const int &k) {
    (void)x_qh;

    const float * x_dmf = (const float *) x_dm;
    const float * y_df  = (const float *) y_ds;

    const int8_t * sc = ((const int8_t *) &x_sc[i * (WARP_SIZE/8) + i/8 + k/8]);

    const int index_x = i * (QR6_K*WARP_SIZE + 1) +  QR6_K*k;
    const int index_y = j * WARP_SIZE             + (QR6_K*k) % WARP_SIZE;
    return vec_dot_q6_K_q8_1_impl_mmq(&x_ql[index_x], &y_qs[index_y], sc, x_dmf[i * (WARP_SIZE/QI6_K) + i/QI6_K], &y_df[index_y/QI8_1]);
}

template <int qk, int qr, dequantize_kernel_t dequantize_kernel>
static void dequantize_mul_mat_vec(const void * __restrict__ vx, const dfloat * __restrict__ y, float * __restrict__ dst, const int ncols, const int nrows,
                                   const sycl::nd_item<3> &item_ct1) {
    // qk = quantized weights per x block
    // qr = number of quantized weights per data value in x block
    const int row = item_ct1.get_group(2) * item_ct1.get_local_range(1) +
                    item_ct1.get_local_id(1);

    if (row >= nrows) {
        return;
    }

    const int tid = item_ct1.get_local_id(2);

    const int iter_stride = 2*GGML_SYCL_DMMV_X;
    const int vals_per_iter = iter_stride / WARP_SIZE; // num quantized vals per thread and i iter
    const int y_offset = qr == 1 ? 1 : qk/2;

// partial sum for each thread
#ifdef GGML_SYCL_F16
    sycl::half2 tmp = {0.0f, 0.0f}; // two sums for f16 to take advantage of half2 intrinsics
#else
    float tmp = 0.0f;
#endif // GGML_SYCL_F16

    for (int i = 0; i < ncols; i += iter_stride) {
        const int col = i + vals_per_iter*tid;
        const int ib = (row*ncols + col)/qk; // x block index
        const int iqs = (col%qk)/qr; // x quant index
        const int iybs = col - col%qk; // y block start index

// processing >2 values per i iter is faster for fast GPUs
#pragma unroll
        for (int j = 0; j < vals_per_iter; j += 2) {
            // process 2 vals per j iter

            // dequantize
            // for qr = 2 the iqs needs to increase by 1 per j iter because 2 weights per data val
            dfloat2 v;
            dequantize_kernel(vx, ib, iqs + j/qr, v);

            // matrix multiplication
            // for qr = 2 the y index needs to increase by 1 per j iter because of y_offset = qk/2
#ifdef GGML_SYCL_F16
            dfloat2 t1{y[iybs + iqs + j / qr + 0],
                        y[iybs + iqs + j / qr + y_offset]};

            tmp += v * t1;
#else
            tmp += v.x() * y[iybs + iqs + j / qr + 0];
            tmp += v.y() * y[iybs + iqs + j / qr + y_offset];
#endif // GGML_SYCL_F16
        }
    }

    // sum up partial sums and write back result
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        tmp +=
            dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), tmp, mask);
    }

    if (tid == 0) {
#ifdef GGML_SYCL_F16
        dst[row] = tmp.x() + tmp.y();
#else
        dst[row] = tmp;
#endif // GGML_SYCL_F16
    }
}

static void mul_mat_p021_f16_f32(
    const void * __restrict__ vx, const float * __restrict__ y, float * __restrict__ dst,
    const int ncols_x, const int nrows_x, const int nchannels_x, const int nchannels_y,
    const sycl::nd_item<3> &item_ct1) {

    const sycl::half *x = (const sycl::half *)vx;

    const int row_x = item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                      item_ct1.get_local_id(1);
    const int channel = item_ct1.get_local_range(0) * item_ct1.get_group(0) +
                        item_ct1.get_local_id(0);
    const int channel_x = channel / (nchannels_y / nchannels_x);

    const int nrows_y = ncols_x;
    const int nrows_dst = nrows_x;
    const int row_dst = row_x;

    float tmp = 0.0f;

    for (int col_x0 = 0; col_x0 < ncols_x;
         col_x0 += item_ct1.get_local_range(2)) {
        const int col_x = col_x0 + item_ct1.get_local_id(2);

        if (col_x >= ncols_x) {
            break;
        }

        // x is transposed and permuted
        const int ix = row_x*nchannels_x*ncols_x + channel_x*ncols_x + col_x;
        const float xi =
            sycl::vec<sycl::half, 1>(x[ix])
                .convert<float, sycl::rounding_mode::automatic>()[0];

        const int row_y = col_x;


        // y is not transposed but permuted
        const int iy = channel*nrows_y + row_y;

        tmp += xi * y[iy];
    }

    // dst is not transposed and not permuted
    const int idst = channel*nrows_dst + row_dst;

    // sum up partial sums and write back result
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        tmp +=
            dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), tmp, mask);
    }

    if (item_ct1.get_local_id(2) == 0) {
        dst[idst] = tmp;
    }
}

static void mul_mat_vec_nc_f16_f32( // nc == non-contiguous
    const void * __restrict__ vx, const float * __restrict__ y, float * __restrict__ dst, const int ncols_x, const int nrows_x,
    const int row_stride_x, const int channel_stride_x, const int channel_x_divisor,
    const sycl::nd_item<3> &item_ct1) {

    const sycl::half *x = (const sycl::half *)vx;

    const int row_x = item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                      item_ct1.get_local_id(1);
    const int channel = item_ct1.get_local_range(0) * item_ct1.get_group(0) +
                        item_ct1.get_local_id(0);
    const int channel_x = channel / channel_x_divisor;

    const int nrows_y   = ncols_x;
    const int nrows_dst = nrows_x;
    const int row_dst   = row_x;

    const int idst = channel*nrows_dst + row_dst;

    float tmp = 0.0f;

    for (int col_x0 = 0; col_x0 < ncols_x;
         col_x0 += item_ct1.get_local_range(2)) {
        const int col_x = col_x0 + item_ct1.get_local_id(2);

        if (col_x >= ncols_x) {
            break;
        }

        const int row_y = col_x;

        const int ix = channel_x*channel_stride_x + row_x*row_stride_x + col_x;
        const int iy = channel*nrows_y + row_y;

        const float xi =
            sycl::vec<sycl::half, 1>(x[ix])
                .convert<float, sycl::rounding_mode::automatic>()[0];

        tmp += xi * y[iy];
    }

    // sum up partial sums and write back result
#pragma unroll
    for (int mask = 16; mask > 0; mask >>= 1) {
        tmp +=
            dpct::permute_sub_group_by_xor(item_ct1.get_sub_group(), tmp, mask);
    }

    if (item_ct1.get_local_id(2) == 0) {
        dst[idst] = tmp;
    }
}

static void cpy_1_f32_f32(const char * cxi, char * cdsti) {
    const float * xi = (const float *) cxi;
    float * dsti = (float *) cdsti;

    *dsti = *xi;
}

static void cpy_1_f32_f16(const char * cxi, char * cdsti) {
    const float * xi = (const float *) cxi;
    sycl::half *dsti = (sycl::half *)cdsti;

    *dsti = sycl::vec<float, 1>(*xi)
                .convert<sycl::half, sycl::rounding_mode::automatic>()[0];
}

static void cpy_1_f16_f16(const char * cxi, char * cdsti) {
    const sycl::half *xi = (const sycl::half *)cxi;
    sycl::half *dsti = (sycl::half *)cdsti;

    *dsti = *xi;
}

static void cpy_1_f16_f32(const char * cxi, char * cdsti) {
    const sycl::half *xi = (const sycl::half *)cxi;
    float * dsti = (float *) cdsti;

    *dsti = *xi;
}

static void cpy_1_i16_i16(const char * cxi, char * cdsti) {
    const int16_t *xi = (const int16_t *)cxi;
    int16_t *dsti = (int16_t *)cdsti;

    *dsti = *xi;
}

static void cpy_1_i32_i32(const char * cxi, char * cdsti) {
    const int32_t *xi = (const int32_t *)cxi;
    int32_t *dsti = (int32_t *)cdsti;

    *dsti = *xi;
}

template <cpy_kernel_t cpy_1>
static void cpy_f32_f16(const char * cx, char * cdst, const int ne,
                        const int ne00, const int ne01, const int ne02, const int nb00, const int nb01, const int nb02,
                        const int nb03, const int ne10, const int ne11, const int ne12, const int nb10, const int nb11,
                        const int nb12, const int nb13, const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    if (i >= ne) {
        return;
    }

    // determine indices i02/i12, i01/i11, i00/i10 as a function of index i of flattened tensor
    // then combine those indices with the corresponding byte offsets to get the total offsets
    const int i03 = i/(ne00 * ne01 * ne02);
    const int i02 = (i - i03*ne00*ne01*ne02 )/ (ne00*ne01);
    const int i01 = (i - i03*ne00*ne01*ne02  -  i02*ne01*ne00) / ne00;
    const int i00 = i - i03*ne00*ne01*ne02 - i02*ne01*ne00 - i01*ne00;
    const int x_offset = i00*nb00 + i01*nb01 + i02*nb02 + i03 * nb03;

    const int i13 = i/(ne10 * ne11 * ne12);
    const int i12 = (i - i13*ne10*ne11*ne12) / (ne10*ne11);
    const int i11 = (i - i13*ne10*ne11*ne12 - i12*ne10*ne11) / ne10;
    const int i10 = i - i13*ne10*ne11*ne12 - i12*ne10*ne11 - i11*ne10;
    const int dst_offset = i10*nb10 + i11*nb11 + i12*nb12 + i13 * nb13;

    cpy_1(cx + x_offset, cdst + dst_offset);
}

static void cpy_blck_f32_q8_0(const char * cxi, char * cdsti) {
    const float * xi = (const float *) cxi;
    block_q8_0 * dsti = (block_q8_0 *) cdsti;

    float amax = 0.0f; // absolute max

    for (int j = 0; j < QK8_0; j++) {
        const float v = xi[j];
        amax = sycl::fmax(amax, sycl::fabs((float)v));
    }

    const float d = amax / ((1 << 7) - 1);
    const float id = d ? 1.0f/d : 0.0f;

    dsti->d = d;

    for (int j = 0; j < QK8_0; ++j) {
        const float x0 = xi[j]*id;

        dsti->qs[j] = sycl::round((float)x0);
    }
}

static void cpy_blck_f32_q4_0(const char * cxi, char * cdsti) {
    const float * xi = (const float *) cxi;
    block_q4_0 * dsti = (block_q4_0 *) cdsti;

    float amax = 0.0f;
    float vmax = 0.0f;

    for (int j = 0; j < QK4_0; ++j) {
        const float v = xi[j];
        if (amax < sycl::fabs((float)v)) {
            amax = sycl::fabs((float)v);
            vmax = v;
        }
    }

    const float d  = vmax / -8;
    const float id = d ? 1.0f/d : 0.0f;

    dsti->d = d;

    for (int j = 0; j < QK4_0/2; ++j) {
        const float x0 = xi[0       + j]*id;
        const float x1 = xi[QK4_0/2 + j]*id;

        const uint8_t xi0 = dpct::min(15, (int8_t)(x0 + 8.5f));
        const uint8_t xi1 = dpct::min(15, (int8_t)(x1 + 8.5f));

        dsti->qs[j]  = xi0;
        dsti->qs[j] |= xi1 << 4;
    }
}

static void cpy_blck_f32_q4_1(const char * cxi, char * cdsti) {
    const float * xi = (const float *) cxi;
    block_q4_1 * dsti = (block_q4_1 *) cdsti;

    float vmin = FLT_MAX;
    float vmax = -FLT_MAX;

    for (int j = 0; j < QK4_1; ++j) {
        const float v = xi[j];

        if (v < vmin) vmin = v;
        if (v > vmax) vmax = v;
    }

    const float d  = (vmax - vmin) / ((1 << 4) - 1);
    const float id = d ? 1.0f/d : 0.0f;

    dsti->dm.x() = d;
    dsti->dm.y() = vmin;

    for (int j = 0; j < QK4_1/2; ++j) {
        const float x0 = (xi[0       + j] - vmin)*id;
        const float x1 = (xi[QK4_1/2 + j] - vmin)*id;

        const uint8_t xi0 = dpct::min(15, (int8_t)(x0 + 0.5f));
        const uint8_t xi1 = dpct::min(15, (int8_t)(x1 + 0.5f));

        dsti->qs[j]  = xi0;
        dsti->qs[j] |= xi1 << 4;
    }
}

template <cpy_kernel_t cpy_blck, int qk>
static void cpy_f32_q(const char * cx, char * cdst, const int ne,
                      const int ne00, const int ne01, const int ne02, const int nb00, const int nb01, const int nb02,
                      const int nb03, const int ne10, const int ne11, const int ne12, const int nb10, const int nb11,
                      const int nb12, const int nb13, const sycl::nd_item<3> &item_ct1) {
    const int i = (item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                   item_ct1.get_local_id(2)) *
                  qk;

    if (i >= ne) {
        return;
    }

    const int i03 = i/(ne00 * ne01 * ne02);
    const int i02 = (i - i03*ne00*ne01*ne02 )/ (ne00*ne01);
    const int i01 = (i - i03*ne00*ne01*ne02  -  i02*ne01*ne00) / ne00;
    const int i00 = i - i03*ne00*ne01*ne02 - i02*ne01*ne00 - i01*ne00;
    const int x_offset = i00*nb00 + i01*nb01 + i02*nb02 + i03 * nb03;

    const int i13 = i/(ne10 * ne11 * ne12);
    const int i12 = (i - i13*ne10*ne11*ne12) / (ne10*ne11);
    const int i11 = (i - i13*ne10*ne11*ne12 - i12*ne10*ne11) / ne10;
    const int i10 = i - i13*ne10*ne11*ne12 - i12*ne10*ne11 - i11*ne10;
    const int dst_offset = (i10/qk)*nb10 + i11*nb11 + i12*nb12 + i13*nb13;

    cpy_blck(cx + x_offset, cdst + dst_offset);
}

static float rope_yarn_ramp(const float low, const float high, const int i0) {
    const float y = (i0 / 2 - low) / sycl::max(0.001f, high - low);
    return 1.0f - sycl::min(1.0f, sycl::max(0.0f, y));
}

struct rope_corr_dims {
    float v[4];
};

// YaRN algorithm based on LlamaYaRNScaledRotaryEmbedding.py from https://github.com/jquesnelle/yarn
// MIT licensed. Copyright (c) 2023 Jeffrey Quesnelle and Bowen Peng.
static void rope_yarn(
    float theta_extrap, float freq_scale, rope_corr_dims corr_dims, int64_t i0, float ext_factor, float mscale,
    float * cos_theta, float * sin_theta
) {
    // Get n-d rotational scaling corrected for extrapolation
    float theta_interp = freq_scale * theta_extrap;
    float theta = theta_interp;
    if (ext_factor != 0.0f) {
        float ramp_mix = rope_yarn_ramp(corr_dims.v[0], corr_dims.v[1], i0) * ext_factor;
        theta = theta_interp * (1 - ramp_mix) + theta_extrap * ramp_mix;

        // Get n-d magnitude scaling corrected for interpolation
        mscale *= 1.0f + 0.1f * sycl::log(1.0f / freq_scale);
    }
    *cos_theta = sycl::cos(theta) * mscale;
    *sin_theta = sycl::sin(theta) * mscale;
}

// rope == RoPE == rotary positional embedding
template<typename T, bool has_pos>
static void rope(
    const T * x, T * dst, int ncols, const int32_t * pos, float freq_scale, int p_delta_rows, float freq_base,
    float ext_factor, float attn_factor, rope_corr_dims corr_dims
,
    const sycl::nd_item<3> &item_ct1) {
    const int col = 2 * (item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                         item_ct1.get_local_id(1));

    if (col >= ncols) {
        return;
    }

    const int row = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                    item_ct1.get_local_id(2);
    const int i = row*ncols + col;
    const int i2 = row/p_delta_rows;

    const int p = has_pos ? pos[i2] : 0;
    const float theta_base = p * dpct::pow(freq_base, -float(col) / ncols);

    float cos_theta, sin_theta;
    rope_yarn(theta_base, freq_scale, corr_dims, col, ext_factor, attn_factor, &cos_theta, &sin_theta);

    const float x0 = x[i + 0];
    const float x1 = x[i + 1];

    dst[i + 0] = x0*cos_theta - x1*sin_theta;
    dst[i + 1] = x0*sin_theta + x1*cos_theta;
}

template<typename T, bool has_pos>
static void rope_neox(
    const T * x, T * dst, int ncols, int n_dims, const int32_t * pos, float freq_scale, int p_delta_rows,
    float ext_factor, float attn_factor, rope_corr_dims corr_dims, float theta_scale, float inv_ndims
,
    const sycl::nd_item<3> &item_ct1) {
    const int col = 2 * (item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                         item_ct1.get_local_id(1));

    if (col >= ncols) {
        return;
    }

    const int row = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                    item_ct1.get_local_id(2);
    const int ib = col / n_dims;
    const int ic = col % n_dims;

    if (ib > 0) {
        const int i = row*ncols + ib*n_dims + ic;

        dst[i + 0] = x[i + 0];
        dst[i + 1] = x[i + 1];

        return;
    }

    const int i  = row*ncols + ib*n_dims + ic/2;
    const int i2 = row/p_delta_rows;

    float cur_rot = inv_ndims * ic - ib;

    const int p = has_pos ? pos[i2] : 0;
    const float theta_base =
        p * freq_scale * dpct::pow(theta_scale, col / 2.0f);

    float cos_theta, sin_theta;
    rope_yarn(theta_base, freq_scale, corr_dims, cur_rot, ext_factor, attn_factor, &cos_theta, &sin_theta);

    const float x0 = x[i + 0];
    const float x1 = x[i + n_dims/2];

    dst[i + 0]        = x0*cos_theta - x1*sin_theta;
    dst[i + n_dims/2] = x0*sin_theta + x1*cos_theta;
}

static void rope_glm_f32(
    const float * x, float * dst, int ncols, const int32_t * pos, float freq_scale, int p_delta_rows, float freq_base,
    int n_ctx
, const sycl::nd_item<3> &item_ct1) {
    const int col = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                    item_ct1.get_local_id(2);
    const int half_n_dims = ncols/4;

    if (col >= half_n_dims) {
        return;
    }

    const int row = item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                    item_ct1.get_local_id(1);
    const int i = row*ncols + col;
    const int i2 = row/p_delta_rows;

    const float col_theta_scale = dpct::pow(freq_base, -2.0f * col / ncols);
     // FIXME: this is likely wrong
    const int p = pos != nullptr ? pos[i2] : 0;

    const float theta = sycl::min(p, n_ctx - 2) * freq_scale * col_theta_scale;
    const float sin_theta = sycl::sin((float)theta);
    const float cos_theta = sycl::cos((float)theta);

    const float x0 = x[i + 0];
    const float x1 = x[i + half_n_dims];

    dst[i + 0]           = x0*cos_theta - x1*sin_theta;
    dst[i + half_n_dims] = x0*sin_theta + x1*cos_theta;

    const float block_theta =
        ((float)sycl::max(p - n_ctx - 2, 0)) * col_theta_scale;
    const float sin_block_theta = sycl::sin((float)block_theta);
    const float cos_block_theta = sycl::cos((float)block_theta);

    const float x2 = x[i + half_n_dims * 2];
    const float x3 = x[i + half_n_dims * 3];

    dst[i + half_n_dims * 2] = x2*cos_block_theta - x3*sin_block_theta;
    dst[i + half_n_dims * 3] = x2*sin_block_theta + x3*cos_block_theta;
}

static void alibi_f32(const float * x, float * dst, const int ncols, const int k_rows,
                                 const int n_heads_log2_floor, const float m0, const float m1,
                                 const sycl::nd_item<3> &item_ct1) {
    const int col = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                    item_ct1.get_local_id(2);

    if (col >= ncols) {
        return;
    }

    const int row = item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                    item_ct1.get_local_id(1);
    const int i = row*ncols + col;

    const int k = row/k_rows;

    float m_k;
    if (k < n_heads_log2_floor) {
        m_k = dpct::pow(m0, k + 1);
    } else {
        m_k = dpct::pow(m1, 2 * (k - n_heads_log2_floor) + 1);
    }

    dst[i] = col * m_k + x[i];
}

static void k_sum_rows_f32(const float * x, float * dst, const int ncols,
                           const sycl::nd_item<3> &item_ct1) {
    const int row = item_ct1.get_group(1);
    const int col = item_ct1.get_local_id(2);

    float sum = 0.0f;
    for (int i = col; i < ncols; i += item_ct1.get_local_range(2)) {
        sum += x[row * ncols + i];
    }

    sum = warp_reduce_sum(sum, item_ct1);

    if (col == 0) {
        dst[row] = sum;
    }
}

template<typename T>
static inline void swap(T & a, T & b) {
    T tmp = a;
    a = b;
    b = tmp;
}

template<ggml_sort_order order>
static void k_argsort_f32_i32(const float * x, int * dst, const int ncols,
                              const sycl::nd_item<3> &item_ct1) {
    // bitonic sort
    int col = item_ct1.get_local_id(2);
    int row = item_ct1.get_group(1);

    if (col >= ncols) return;

    const float * x_row = x + row * ncols;
    int * dst_row = dst + row * ncols;

    // initialize indices
    if (col < ncols) {
        dst_row[col] = col;
    }
    /*
    DPCT1065:58: Consider replacing sycl::nd_item::barrier() with
    sycl::nd_item::barrier(sycl::access::fence_space::local_space) for better
    performance if there is no access to global memory.
    */
    item_ct1.barrier();

    for (int k = 2; k <= ncols; k *= 2) {
        for (int j = k / 2; j > 0; j /= 2) {
            int ixj = col ^ j;
            if (ixj > col) {
                if ((col & k) == 0) {
                    if (order == GGML_SORT_ORDER_ASC ? x_row[dst_row[col]] > x_row[dst_row[ixj]] : x_row[dst_row[col]] < x_row[dst_row[ixj]]) {
                        swap(dst_row[col], dst_row[ixj]);
                    }
                } else {
                    if (order == GGML_SORT_ORDER_ASC ? x_row[dst_row[col]] < x_row[dst_row[ixj]] : x_row[dst_row[col]] > x_row[dst_row[ixj]]) {
                        swap(dst_row[col], dst_row[ixj]);
                    }
                }
            }
            /*
            DPCT1118:11: SYCL group functions and algorithms must be encountered
            in converged control flow. You may need to adjust the code.
            */
            /*
            DPCT1065:59: Consider replacing sycl::nd_item::barrier() with
            sycl::nd_item::barrier(sycl::access::fence_space::local_space) for
            better performance if there is no access to global memory.
            */
            item_ct1.barrier();
        }
    }
}

static void diag_mask_inf_f32(const float * x, float * dst, const int ncols, const int rows_per_channel, const int n_past,
                              const sycl::nd_item<3> &item_ct1) {
    const int col = item_ct1.get_local_range(1) * item_ct1.get_group(1) +
                    item_ct1.get_local_id(1);
    const int row = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                    item_ct1.get_local_id(2);

    if (col >= ncols) {
        return;
    }

    const int i = row*ncols + col;
    //dst[i] = col > (n_past + row % rows_per_channel) ? -INFINITY : x[i];
    //dst[i] = x[i] - (col > n_past + row % rows_per_channel) * INT_MAX; // equivalent within rounding error but slightly faster on GPU
    dst[i] = x[i] - (col > n_past + row % rows_per_channel) * FLT_MAX;
}


template <bool vals_smem, int ncols_template, int block_size_template>
static void soft_max_f32(const float * x, const float * mask, const float *pos, float * dst, const int ncols_par,
                         const int nrows_y, const float scale, const float max_bias, const float m0,
                         const float m1, uint32_t n_head_log2, const sycl::nd_item<3> &item_ct1, float *buf) {
    const int ncols = ncols_template == 0 ? ncols_par : ncols_template;

    const int tid = item_ct1.get_local_id(2);
    const int rowx = item_ct1.get_group(2);
    const int rowy = rowx % nrows_y; // broadcast the mask (y) in the row dimension

    const int block_size = block_size_template == 0 ? item_ct1.get_local_range(2) : block_size_template;

    const int warp_id = item_ct1.get_local_id(2) / WARP_SIZE;
    const int lane_id = item_ct1.get_local_id(2) % WARP_SIZE;

    float slope = 0.0f;

    // ALiBi
    if (max_bias > 0.0f) {
        const uint32_t h = rowx/nrows_y; // head index

        const float base = h < n_head_log2 ? m0 : m1;
        const int   exp  = h < n_head_log2 ? h + 1 : 2*(h - n_head_log2) + 1;

        slope = sycl::pow(base, float(exp));
    }

    float * vals = vals_smem ? buf + WARP_SIZE : dst + rowx*ncols;
    float max_val = -INFINITY;

    for (int col0 = 0; col0 < ncols; col0 += block_size) {
        const int col = col0 + tid;

        if (ncols_template == 0 && col >= ncols) {
            break;
        }

        const int ix = rowx*ncols + col;
        const int iy = rowy*ncols + col;

        const float val = x[ix]*scale + (mask ? mask[iy] : 0.0f) + (pos ? slope*pos[col] : 0.0f);

        vals[col] = val;
        max_val = sycl::max(max_val, val);
    }

    // find the max value in the block
    max_val = warp_reduce_max(max_val, item_ct1);
    if (block_size > WARP_SIZE) {
        if (warp_id == 0) {
            buf[lane_id] = -INFINITY;
        }
        item_ct1.barrier(sycl::access::fence_space::local_space);

        if (lane_id == 0) {
            buf[warp_id] = max_val;
        }
        item_ct1.barrier(sycl::access::fence_space::local_space);

        max_val = buf[lane_id];
        max_val = warp_reduce_max(max_val, item_ct1);
    }

    float tmp = 0.f;

#pragma unroll
    for (int col0 = 0; col0 < ncols; col0 += block_size) {
        const int col = col0 + tid;
                if (ncols_template == 0 && col >= ncols) {
            break;
        }

        const float val = sycl::native::exp(vals[col] - max_val);
        tmp += val;
        vals[col] = val;
    }

    // find the sum of exps in the block
    tmp = warp_reduce_sum(tmp, item_ct1);
    if (block_size > WARP_SIZE) {
        if (warp_id == 0) {
            buf[lane_id] = 0.f;
        }
        item_ct1.barrier(sycl::access::fence_space::local_space);

        if (lane_id == 0) {
            buf[warp_id] = tmp;
        }
        item_ct1.barrier(sycl::access::fence_space::local_space);

        tmp = buf[lane_id];
        tmp = warp_reduce_sum(tmp, item_ct1);
    }

    const float inv_sum = 1.f / tmp;

#pragma unroll
    for (int col0 = 0; col0 < ncols; col0 += block_size) {
        const int col = col0 + tid;

        if (ncols_template == 0 && col >= ncols) {
            return;
        }

        const int idst = rowx*ncols + col;
        dst[idst] = vals[col] * inv_sum;
    }
}

static void scale_f32(const float * x, float * dst, const float scale, const int k,
                      const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    if (i >= k) {
        return;
    }

    dst[i] = scale * x[i];
}

static void clamp_f32(const float * x, float * dst, const float min, const float max, const int k,
                      const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_range(2) * item_ct1.get_group(2) +
                  item_ct1.get_local_id(2);

    if (i >= k) {
        return;
    }

    dst[i] = x[i] < min ? min : (x[i] > max ? max : x[i]);
}

template <typename T>
static void im2col_kernel(const float *x, T *dst, int offset_delta,
                           int IW, int IH, int OW, int KW, int KH,
                           int pelements, int CHW, int s0, int s1, int p0,
                           int p1, int d0, int d1,
                           const sycl::nd_item<3> &item_ct1) {
    const int i = item_ct1.get_local_id(2) +
                  item_ct1.get_group(2) * item_ct1.get_local_range(2);
    if (i >= pelements) {
        return;
    }

    const int ksize = OW * (KH > 1 ? KW : 1);
    const int kx = i / ksize;
    const int kd = kx * ksize;
    const int ky = (i - kd) / OW;
    const int ix = i % OW;

    const int64_t iiw = ix * s0 + kx * d0 - p0;
    const int64_t iih = item_ct1.get_group(1) * s1 + ky * d1 - p1;

    const int64_t offset_dst =
        (item_ct1.get_group(1) * OW + ix) * CHW +
        (item_ct1.get_group(0) * (KW * KH) + ky * KW + kx);

    if (iih < 0 || iih >= IH || iiw < 0 || iiw >= IW) {
        dst[offset_dst] =
            sycl::vec<float, 1>(0.0f)
                .convert<sycl::half, sycl::rounding_mode::automatic>()[0];
    } else {
        const int64_t offset_src = item_ct1.get_group(0) * offset_delta;
        dst[offset_dst] =
            sycl::vec<float, 1>(x[offset_src + iih * IW + iiw])
                .convert<sycl::half, sycl::rounding_mode::automatic>()[0];
    }
}

template <typename Ti, typename To>
static  void pool2d_nchw_kernel(
        const int ih, const int iw, const int oh, const int ow,
        const int kh, const int kw, const int sh, const int sw,
        const int ph, const int pw, const int parallel_elements,
        const Ti* src, To* dst, const enum ggml_op_pool op,
        const sycl::nd_item<3> &item_ct1) {
        int idx = item_ct1.get_local_id(2) +
                  item_ct1.get_group(2) * item_ct1.get_local_range(2);
        if (idx >= parallel_elements) {
            return;
        }

        const int I_HW = ih * iw;
        const int O_HW = oh * ow;
        const int nc = idx / O_HW;
        const int cur_oh = idx % O_HW / ow;
        const int cur_ow = idx % O_HW % ow;
        const Ti* i_ptr = src + nc * I_HW;
        To* o_ptr = dst + nc * O_HW;
        const int start_h = cur_oh * sh - ph;
        const int bh = sycl::max(0, start_h);
        const int eh = sycl::min(ih, start_h + kh);
        const int start_w = cur_ow * sw - pw;
        const int bw = sycl::max(0, start_w);
        const int ew = sycl::min(iw, start_w + kw);

        To res = 0;

        switch (op) {
            case GGML_OP_POOL_AVG: res = 0; break;
            case GGML_OP_POOL_MAX: res = -FLT_MAX; break;
        }

        for (int i = bh; i < eh; i += 1) {
            for (int j = bw; j < ew; j += 1) {
#if DPCT_COMPATIBILITY_TEMP >= 350
                /*
                DPCT1098:106: The '*' expression is used instead of the __ldg
                call. These two expressions do not provide the exact same
                functionality. Check the generated code for potential precision
                and/or performance issues.
                */
                Ti cur = *(i_ptr + i * iw + j);
#else
                Ti cur = i_ptr[i * iw + j];
#endif
                switch (op) {
                    case GGML_OP_POOL_AVG: res += (cur / (kh * kw)); break;
                    case GGML_OP_POOL_MAX: res = sycl::max(res, (To)cur); break;
                }
            }
        }
        o_ptr[cur_oh * ow + cur_ow] = res;
}

template <int qk, int qr, dequantize_kernel_t dq>
static void get_rows_sycl(const ggml_tensor *src0, const ggml_tensor *src1,
                          ggml_tensor *dst, const void *src0_dd,
                          const int32_t *src1_dd, float *dst_dd,
                          dpct::queue_ptr stream) {

    GGML_TENSOR_BINARY_OP_LOCALS

    const sycl::range<3> block_dims(1, 1, SYCL_GET_ROWS_BLOCK_SIZE);
    const int block_num_x = (ne00 + 2*SYCL_GET_ROWS_BLOCK_SIZE - 1) / (2*SYCL_GET_ROWS_BLOCK_SIZE);
    const sycl::range<3> block_nums(ne11 * ne12, ne10, block_num_x);

    // strides in elements
    //const size_t s0 = nb0 / ggml_element_size(dst);
    const size_t s1 = nb1 / ggml_element_size(dst);
    const size_t s2 = nb2 / ggml_element_size(dst);
    const size_t s3 = nb3 / ggml_element_size(dst);

    const size_t s10 = nb10 / ggml_element_size(src1);
    const size_t s11 = nb11 / ggml_element_size(src1);
    const size_t s12 = nb12 / ggml_element_size(src1);
    //const size_t s13 = nb13 / ggml_element_size(src1);

    GGML_ASSERT(ne00 % 2 == 0);

    stream->parallel_for(sycl::nd_range<3>(block_nums * block_dims, block_dims),
                         [=](sycl::nd_item<3> item_ct1) {
                             k_get_rows<qk, qr, dq>(
                                 src0_dd, src1_dd, dst_dd, ne00, ne12, s1, s2,
                                 s3, nb01, nb02, nb03, s10, s11, s12, item_ct1);
                         });

    (void) dst;
}

template <typename src0_t>
static void get_rows_sycl_float(const ggml_tensor *src0,
                                const ggml_tensor *src1, ggml_tensor *dst,
                                const src0_t *src0_dd, const int32_t *src1_dd,
                                float *dst_dd, dpct::queue_ptr stream) {

    GGML_TENSOR_BINARY_OP_LOCALS

    const sycl::range<3> block_dims(1, 1, SYCL_GET_ROWS_BLOCK_SIZE);
    const int block_num_x = (ne00 + SYCL_GET_ROWS_BLOCK_SIZE - 1) / SYCL_GET_ROWS_BLOCK_SIZE;
    const sycl::range<3> block_nums(ne11 * ne12, ne10, block_num_x);

    // strides in elements
    //const size_t s0 = nb0 / ggml_element_size(dst);
    const size_t s1 = nb1 / ggml_element_size(dst);
    const size_t s2 = nb2 / ggml_element_size(dst);
    const size_t s3 = nb3 / ggml_element_size(dst);

    const size_t s10 = nb10 / ggml_element_size(src1);
    const size_t s11 = nb11 / ggml_element_size(src1);
    const size_t s12 = nb12 / ggml_element_size(src1);
    //const size_t s13 = nb13 / ggml_element_size(src1);

    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) {
                k_get_rows_float(src0_dd, src1_dd, dst_dd, ne00, ne12, s1, s2,
                                 s3, nb01, nb02, nb03, s10, s11, s12, item_ct1);
            });
    }

    (void) dst;
}

template<float (*bin_op)(const float, const float)>
struct bin_bcast_sycl {
    template <typename src0_t, typename src1_t, typename dst_t>
    void operator()(const struct ggml_tensor *src0,
                    const struct ggml_tensor *src1, struct ggml_tensor *dst,
                    const src0_t *src0_dd, const src1_t *src1_dd, dst_t *dst_dd,
                    dpct::queue_ptr stream) {

        GGML_TENSOR_BINARY_OP_LOCALS

        int nr0 = ne10/ne0;
        int nr1 = ne11/ne1;
        int nr2 = ne12/ne2;
        int nr3 = ne13/ne3;

        int nr[4] = { nr0, nr1, nr2, nr3 };

        // collapse dimensions until first broadcast dimension
        int64_t cne0[] = {ne0, ne1, ne2, ne3};
        int64_t cne1[] = {ne10, ne11, ne12, ne13};
        size_t cnb0[] = {nb0, nb1, nb2, nb3};
        size_t cnb1[] = {nb10, nb11, nb12, nb13};
        auto collapse = [](int64_t cne[]) {
            cne[0] *= cne[1];
            cne[1] = cne[2];
            cne[2] = cne[3];
            cne[3] = 1;
        };

        auto collapse_nb = [](size_t cnb[], int64_t cne[]) {
            cnb[1] *= cne[1];
            cnb[2] *= cne[2];
            cnb[3] *= cne[3];
        };

        for (int i = 0; i < 4; i++) {
            if (nr[i] != 1) {
                break;
            }
            if (i > 0) {
                collapse_nb(cnb0, cne0);
                collapse_nb(cnb1, cne1);
                collapse(cne0);
                collapse(cne1);
            }
        }
        {
            int64_t ne0 = cne0[0];
            int64_t ne1 = cne0[1];
            int64_t ne2 = cne0[2];
            int64_t ne3 = cne0[3];

            int64_t ne10 = cne1[0];
            int64_t ne11 = cne1[1];
            int64_t ne12 = cne1[2];
            int64_t ne13 = cne1[3];

            size_t nb0 = cnb0[0];
            size_t nb1 = cnb0[1];
            size_t nb2 = cnb0[2];
            size_t nb3 = cnb0[3];

            size_t nb10 = cnb1[0];
            size_t nb11 = cnb1[1];
            size_t nb12 = cnb1[2];
            size_t nb13 = cnb1[3];

            size_t s0 = nb0 / sizeof(dst_t);
            size_t s1 = nb1 / sizeof(dst_t);
            size_t s2 = nb2 / sizeof(dst_t);
            size_t s3 = nb3 / sizeof(dst_t);

            size_t s10 = nb10 / sizeof(src1_t);
            size_t s11 = nb11 / sizeof(src1_t);
            size_t s12 = nb12 / sizeof(src1_t);
            size_t s13 = nb13 / sizeof(src1_t);

            GGML_ASSERT(s0 == 1);
            GGML_ASSERT(s10 == 1);

            const int block_size = 128;

            int64_t hne0 = std::max(ne0/2LL, 1LL);

            sycl::range<3> block_dims(1, 1, 1);
            block_dims[2] = std::min<unsigned int>(hne0, block_size);
            block_dims[1] = std::min<unsigned int>(
                ne1, block_size / (unsigned int)block_dims[2]);
            block_dims[0] = std::min(
                std::min<unsigned int>(
                    ne2 * ne3, block_size / (unsigned int)block_dims[2] /
                                   (unsigned int)block_dims[1]),
                64U);

            sycl::range<3> block_nums(
                (ne2 * ne3 + block_dims[0] - 1) / block_dims[0],
                (ne1 + block_dims[1] - 1) / block_dims[1],
                (hne0 + block_dims[2] - 1) / block_dims[2]);

            if (block_nums[0] > 65535) {
                // this is the maximum number of blocks in z direction, fallback to 1D grid kernel
                int block_num = (ne0*ne1*ne2*ne3 + block_size - 1) / block_size;
                {
                    dpct::has_capability_or_fail(stream->get_device(),
                                                 {sycl::aspect::fp16});

                    stream->parallel_for(
                        sycl::nd_range<3>(sycl::range<3>(1, 1, block_num) *
                                              sycl::range<3>(1, 1, block_size),
                                          sycl::range<3>(1, 1, block_size)),
                        [=](sycl::nd_item<3> item_ct1) {
                            k_bin_bcast_unravel<bin_op>(
                                src0_dd, src1_dd, dst_dd, ne0, ne1, ne2, ne3,
                                ne10, ne11, ne12, ne13, s1, s2, s3, s11, s12,
                                s13, item_ct1);
                        });
                }
            } else {
                /*
                DPCT1049:16: The work-group size passed to the SYCL kernel may
                exceed the limit. To get the device limit, query
                info::device::max_work_group_size. Adjust the work-group size if
                needed.
                */
                dpct::has_capability_or_fail(stream->get_device(),
                                             {sycl::aspect::fp16});

                stream->parallel_for(
                    sycl::nd_range<3>(block_nums * block_dims, block_dims),
                    [=](sycl::nd_item<3> item_ct1) {
                        k_bin_bcast<bin_op>(src0_dd, src1_dd, dst_dd, ne0, ne1,
                                            ne2, ne3, ne10, ne11, ne12, ne13,
                                            s1, s2, s3, s11, s12, s13,
                                            item_ct1);
                    });
            }
        }
    }
};

static void acc_f32_sycl(const float *x, const float *y, float *dst,
                         const int n_elements, const int ne10, const int ne11,
                         const int ne12, const int nb1, const int nb2,
                         const int offset, dpct::queue_ptr stream) {
    int num_blocks = (n_elements + SYCL_ACC_BLOCK_SIZE - 1) / SYCL_ACC_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_ACC_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_ACC_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            acc_f32(x, y, dst, n_elements, ne10, ne11, ne12, nb1, nb2, offset,
                    item_ct1);
        });
}

static void gelu_f32_sycl(const float *x, float *dst, const int k,
                          dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_GELU_BLOCK_SIZE - 1) / SYCL_GELU_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_GELU_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_GELU_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            gelu_f32(x, dst, k, item_ct1);
        });
}

static void silu_f32_sycl(const float *x, float *dst, const int k,
                          dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_SILU_BLOCK_SIZE - 1) / SYCL_SILU_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_SILU_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_SILU_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            silu_f32(x, dst, k, item_ct1);
        });
}

static void gelu_quick_f32_sycl(const float *x, float *dst, const int k,
                                dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_GELU_BLOCK_SIZE - 1) / SYCL_GELU_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_GELU_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_GELU_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            gelu_quick_f32(x, dst, k, item_ct1);
        });
}

static void tanh_f32_sycl(const float *x, float *dst, const int k,
                          dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_TANH_BLOCK_SIZE - 1) / SYCL_TANH_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_TANH_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_TANH_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            tanh_f32(x, dst, k, item_ct1);
        });
}

static void relu_f32_sycl(const float *x, float *dst, const int k,
                          dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_RELU_BLOCK_SIZE - 1) / SYCL_RELU_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_RELU_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_RELU_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            relu_f32(x, dst, k, item_ct1);
        });
}

static void hardsigmoid_f32_sycl(const float *x, float *dst, const int k,
                                 dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_HARDSIGMOID_BLOCK_SIZE - 1) / SYCL_HARDSIGMOID_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_HARDSIGMOID_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_HARDSIGMOID_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            hardsigmoid_f32(x, dst, k, item_ct1);
        });
}

static void hardswish_f32_sycl(const float *x, float *dst, const int k,
                               dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_HARDSWISH_BLOCK_SIZE - 1) / SYCL_HARDSWISH_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_HARDSWISH_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_HARDSWISH_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            hardswish_f32(x, dst, k, item_ct1);
        });
}

static void leaky_relu_f32_sycl(const float *x, float *dst, const int k,
                                const float negative_slope,
                                dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_RELU_BLOCK_SIZE - 1) / SYCL_RELU_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_RELU_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_RELU_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            leaky_relu_f32(x, dst, k, negative_slope, item_ct1);
        });
}

static void sqr_f32_sycl(const float *x, float *dst, const int k,
                         dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_SQR_BLOCK_SIZE - 1) / SYCL_SQR_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_SQR_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_SQR_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            sqr_f32(x, dst, k, item_ct1);
        });
}

static void norm_f32_sycl(const float *x, float *dst, const int ncols,
                          const int nrows, const float eps,
                          dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % WARP_SIZE == 0);
    if (ncols < 1024) {
        const sycl::range<3> block_dims(1, 1, WARP_SIZE);
        stream->submit([&](sycl::handler &cgh) {
            sycl::local_accessor<sycl::float2, 1> s_sum_acc_ct1(
                sycl::range<1>(32), cgh);

            cgh.parallel_for(
                sycl::nd_range<3>(sycl::range<3>(1, 1, nrows) * block_dims,
                                  block_dims),
                [=](sycl::nd_item<3> item_ct1)
                    [[intel::reqd_sub_group_size(32)]] {
                        norm_f32(x, dst, ncols, eps, item_ct1,
                                            s_sum_acc_ct1.get_pointer(), WARP_SIZE);
                    });
        });
    } else {
        const int work_group_size = g_work_group_size;
        const sycl::range<3> block_dims(1, 1, work_group_size);
        /*
        DPCT1049:17: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        stream->submit([&](sycl::handler &cgh) {
            sycl::local_accessor<sycl::float2, 1> s_sum_acc_ct1(
                sycl::range<1>(32), cgh);

            cgh.parallel_for(
                sycl::nd_range<3>(sycl::range<3>(1, 1, nrows) * block_dims,
                                  block_dims),
                [=](sycl::nd_item<3> item_ct1)
                    [[intel::reqd_sub_group_size(32)]] {
                        norm_f32(x, dst, ncols, eps, item_ct1,
                                       s_sum_acc_ct1.get_pointer(), work_group_size);
                    });
        });
    }
}

static void group_norm_f32_sycl(const float *x, float *dst,
                                const int num_groups, const int group_size,
                                const int ne_elements, dpct::queue_ptr stream) {
    static const float eps = 1e-6f;
    if (group_size < 1024) {
        const sycl::range<3> block_dims(1, 1, WARP_SIZE);
        stream->submit([&](sycl::handler &cgh) {
            sycl::local_accessor<float, 1> s_sum_acc_ct1(sycl::range<1>(32),
                                                         cgh);

            const float eps_ct4 = eps;

            cgh.parallel_for(
                sycl::nd_range<3>(sycl::range<3>(1, 1, num_groups) * block_dims,
                                  block_dims),
                [=](sycl::nd_item<3> item_ct1)
                    [[intel::reqd_sub_group_size(32)]] {
                        group_norm_f32(
                            x, dst, group_size, ne_elements, eps_ct4, item_ct1,
                            s_sum_acc_ct1.get_pointer(), WARP_SIZE);
                    });
        });
    } else {
        const int work_group_size = g_work_group_size;
        const sycl::range<3> block_dims(1, 1, work_group_size);
        /*
        DPCT1049:18: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */

        stream->submit([&](sycl::handler &cgh) {
            sycl::local_accessor<float, 1> s_sum_acc_ct1(sycl::range<1>(32),
                                                         cgh);

            const float eps_ct4 = eps;

            cgh.parallel_for(
                sycl::nd_range<3>(sycl::range<3>(1, 1, num_groups) * block_dims,
                                  block_dims),
                [=](sycl::nd_item<3> item_ct1)
                    [[intel::reqd_sub_group_size(32)]] {
                        group_norm_f32(x, dst, group_size, ne_elements,
                                             eps_ct4, item_ct1,
                                             s_sum_acc_ct1.get_pointer(), work_group_size);
                    });
        });
    }
}

static void concat_f32_sycl(const float *x, const float *y, float *dst,
                            const int ne0, int ne1, int ne2, int ne02,
                            dpct::queue_ptr stream) {
    int num_blocks = (ne0 + SYCL_CONCAT_BLOCK_SIZE - 1) / SYCL_CONCAT_BLOCK_SIZE;
    sycl::range<3> gridDim(ne2, ne1, num_blocks);
    stream->parallel_for(
        sycl::nd_range<3>(gridDim *
                              sycl::range<3>(1, 1, SYCL_CONCAT_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_CONCAT_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            concat_f32(x, y, dst, ne0, ne02, item_ct1);
        });
}

static void upscale_f32_sycl(const float *x, float *dst, const int ne00,
                             const int ne01, const int ne02,
                             const int scale_factor, dpct::queue_ptr stream) {
    int ne0 = (ne00 * scale_factor);
    int num_blocks = (ne0 + SYCL_UPSCALE_BLOCK_SIZE - 1) / SYCL_UPSCALE_BLOCK_SIZE;
    sycl::range<3> gridDim(ne02, (ne01 * scale_factor), num_blocks);
    stream->parallel_for(
        sycl::nd_range<3>(gridDim *
                              sycl::range<3>(1, 1, SYCL_UPSCALE_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_UPSCALE_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            upscale_f32(x, dst, ne00, ne00 * ne01, scale_factor, item_ct1);
        });
}

static void pad_f32_sycl(const float *x, float *dst, const int ne00,
                         const int ne01, const int ne02, const int ne0,
                         const int ne1, const int ne2, dpct::queue_ptr stream) {
    int num_blocks = (ne0 + SYCL_PAD_BLOCK_SIZE - 1) / SYCL_PAD_BLOCK_SIZE;
    sycl::range<3> gridDim(ne2, ne1, num_blocks);
    stream->parallel_for(
        sycl::nd_range<3>(gridDim * sycl::range<3>(1, 1, SYCL_PAD_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_PAD_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            pad_f32(x, dst, ne0, ne00, ne01, ne02, item_ct1);
        });
}

static void rms_norm_f32_sycl(const float *x, float *dst, const int ncols,
                              const int nrows, const float eps,
                              dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % WARP_SIZE == 0);
    // printf("%s ncols=%d, nrows=%d, WARP_SIZE=%d\n", __func__, ncols, nrows, WARP_SIZE);
    if (ncols < 1024) {
        const sycl::range<3> block_dims(1, 1, WARP_SIZE);
        stream->submit([&](sycl::handler &cgh) {
            sycl::local_accessor<float, 1> s_sum_acc_ct1(sycl::range<1>(32),
                                                         cgh);

            cgh.parallel_for(
                sycl::nd_range<3>(sycl::range<3>(1, 1, nrows) * block_dims,
                                  block_dims),
                [=](sycl::nd_item<3> item_ct1)
                    [[intel::reqd_sub_group_size(32)]] {
                        rms_norm_f32(x, dst, ncols, eps, item_ct1,
                                                s_sum_acc_ct1.get_pointer(), WARP_SIZE);
                    });
        });
    } else {
        const int work_group_size = g_work_group_size;
        const sycl::range<3> block_dims(1, 1, work_group_size);
        /*
        DPCT1049:19: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        stream->submit([&](sycl::handler &cgh) {
            sycl::local_accessor<float, 1> s_sum_acc_ct1(sycl::range<1>(32),
                                                         cgh);

            cgh.parallel_for(
                sycl::nd_range<3>(sycl::range<3>(1, 1, nrows) * block_dims,
                                  block_dims),
                [=](sycl::nd_item<3> item_ct1)
                    [[intel::reqd_sub_group_size(32)]] {
                        rms_norm_f32(x, dst, ncols, eps, item_ct1,
                                           s_sum_acc_ct1.get_pointer(), work_group_size);
                    });
        });
    }
}

static void quantize_row_q8_1_sycl(const float *x, void *vy, const int kx,
                                   const int ky, const int kx_padded,
                                   dpct::queue_ptr stream) {
    const int block_num_x = (kx_padded + SYCL_QUANTIZE_BLOCK_SIZE - 1) / SYCL_QUANTIZE_BLOCK_SIZE;
    const sycl::range<3> num_blocks(1, ky, block_num_x);
    const sycl::range<3> block_size(1, 1, SYCL_DEQUANTIZE_BLOCK_SIZE);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(num_blocks * block_size, block_size),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
                quantize_q8_1(x, vy, kx, kx_padded, item_ct1);
            });
    }
}

template <int qk, int qr, dequantize_kernel_t dequantize_kernel, typename dst_t>
static void dequantize_block_sycl(const void *__restrict__ vx,
                                  dst_t *__restrict__ y, const int k,
                                  dpct::queue_ptr stream) {
    const int num_blocks = (k + 2*SYCL_DEQUANTIZE_BLOCK_SIZE - 1) / (2*SYCL_DEQUANTIZE_BLOCK_SIZE);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});
        stream->parallel_for(
            sycl::nd_range<3>(
                sycl::range<3>(1, 1, num_blocks) *
                    sycl::range<3>(1, 1, SYCL_DEQUANTIZE_BLOCK_SIZE),
                sycl::range<3>(1, 1, SYCL_DEQUANTIZE_BLOCK_SIZE)),
            [=](sycl::nd_item<3> item_ct1) {
                dequantize_block<qk, qr, dequantize_kernel>(vx, y, k, item_ct1);
            });
    }
}

template <typename dst_t>
static void dequantize_row_q2_K_sycl(const void *vx, dst_t *y, const int k,
                                     dpct::queue_ptr stream) {
    const int nb = k / QK_K;
#if QK_K == 256
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 64),
                                               sycl::range<3>(1, 1, 64)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q2_K(vx, y, item_ct1);
                             });
    }
#else
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q2_K(vx, y, item_ct1);
                             });
    }

#endif
}

template <typename dst_t>
static void dequantize_row_q3_K_sycl(const void *vx, dst_t *y, const int k,
                                     dpct::queue_ptr stream) {
    const int nb = k / QK_K;
#if QK_K == 256
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 64),
                                               sycl::range<3>(1, 1, 64)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q3_K(vx, y, item_ct1);
                             });
    }
#else
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q3_K(vx, y, item_ct1);
                             });
    }
#endif
}

template <typename dst_t>
static void dequantize_row_q4_0_sycl(const void *vx, dst_t *y, const int k,
                                     dpct::queue_ptr stream) {
    const int nb32 = k / 32;
    const int nb = (k + 255) / 256;
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q4_0(vx, y, nb32, item_ct1);
                             });
    }
}

template <typename dst_t>
static void dequantize_row_q4_1_sycl(const void *vx, dst_t *y, const int k,
                                     dpct::queue_ptr stream) {
    const int nb32 = k / 32;
    const int nb = (k + 255) / 256;
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q4_1(vx, y, nb32, item_ct1);
                             });
    }
}


template <typename dst_t>
static void dequantize_row_q4_K_sycl(const void *vx, dst_t *y, const int k,
                                     dpct::queue_ptr stream) {
    const int nb = k / QK_K;
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q4_K(vx, y, item_ct1);
                             });
    }
}

template <typename dst_t>
static void dequantize_row_q5_K_sycl(const void *vx, dst_t *y, const int k,
                                     dpct::queue_ptr stream) {
    const int nb = k / QK_K;
#if QK_K == 256
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 64),
                                               sycl::range<3>(1, 1, 64)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q5_K(vx, y, item_ct1);
                             });
    }
#else
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q5_K(vx, y, item_ct1);
                             });
    }

#endif
}

template <typename dst_t>
static void dequantize_row_q6_K_sycl(const void *vx, dst_t *y, const int k,
                                     dpct::queue_ptr stream) {
    const int nb = k / QK_K;
#if QK_K == 256
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 64),
                                               sycl::range<3>(1, 1, 64)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q6_K(vx, y, item_ct1);
                             });
    }
#else
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_q6_K(vx, y, item_ct1);
                             });
    }

#endif
}


template <typename dst_t>
static void dequantize_row_iq2_xxs_sycl(const void *vx, dst_t *y, const int k,
                                        dpct::queue_ptr stream) {
    const int nb = k / QK_K;
    {
        iq2xxs_grid.init(*stream);
        ksigns_iq2xs.init(*stream);
        kmask_iq2xs.init(*stream);

        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->submit([&](sycl::handler &cgh) {
            auto iq2xxs_grid_ptr_ct1 = iq2xxs_grid.get_ptr();
            auto ksigns_iq2xs_ptr_ct1 = ksigns_iq2xs.get_ptr();
            auto kmask_iq2xs_ptr_ct1 = kmask_iq2xs.get_ptr();

            cgh.parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_iq2_xxs(
                                     vx, y, item_ct1, iq2xxs_grid_ptr_ct1,
                                     ksigns_iq2xs_ptr_ct1, kmask_iq2xs_ptr_ct1);
                             });
        });
    }
}

template <typename dst_t>
static void dequantize_row_iq2_xs_sycl(const void *vx, dst_t *y, const int k,
                                       dpct::queue_ptr stream) {
    const int nb = k / QK_K;
    {
        iq2xs_grid.init(*stream);
        ksigns_iq2xs.init(*stream);
        kmask_iq2xs.init(*stream);

        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->submit([&](sycl::handler &cgh) {
            auto iq2xs_grid_ptr_ct1 = iq2xs_grid.get_ptr();
            auto ksigns_iq2xs_ptr_ct1 = ksigns_iq2xs.get_ptr();
            auto kmask_iq2xs_ptr_ct1 = kmask_iq2xs.get_ptr();

            cgh.parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_iq2_xs(
                                     vx, y, item_ct1, iq2xs_grid_ptr_ct1,
                                     ksigns_iq2xs_ptr_ct1, kmask_iq2xs_ptr_ct1);
                             });
        });
    }
}

template <typename dst_t>
static void dequantize_row_iq3_xxs_sycl(const void *vx, dst_t *y, const int k,
                                        dpct::queue_ptr stream) {
    const int nb = k / QK_K;
    {
        iq3xxs_grid.init(*stream);
        ksigns_iq2xs.init(*stream);
        kmask_iq2xs.init(*stream);

        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->submit([&](sycl::handler &cgh) {
            auto iq3xxs_grid_ptr_ct1 = iq3xxs_grid.get_ptr();
            auto ksigns_iq2xs_ptr_ct1 = ksigns_iq2xs.get_ptr();
            auto kmask_iq2xs_ptr_ct1 = kmask_iq2xs.get_ptr();

            cgh.parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_iq3_xxs(
                                     vx, y, item_ct1, iq3xxs_grid_ptr_ct1,
                                     ksigns_iq2xs_ptr_ct1, kmask_iq2xs_ptr_ct1);
                             });
        });
    }
}

template <typename dst_t>
static void dequantize_row_iq3_s_sycl(const void *vx, dst_t *y, const int k,
                                        dpct::queue_ptr stream) {
    const int nb = k / QK_K;
    {
        iq3s_grid.init(*stream);
        ksigns_iq2xs.init(*stream);
        kmask_iq2xs.init(*stream);

        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->submit([&](sycl::handler &cgh) {
            auto iq3s_grid_ptr_ct1 = iq3s_grid.get_ptr();
            auto ksigns_iq2xs_ptr_ct1 = ksigns_iq2xs.get_ptr();
            auto kmask_iq2xs_ptr_ct1 = kmask_iq2xs.get_ptr();

            cgh.parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_iq3_s(
                                     vx, y, item_ct1, iq3s_grid_ptr_ct1,
                                     ksigns_iq2xs_ptr_ct1, kmask_iq2xs_ptr_ct1);
                             });
        });
    }
}

template <typename dst_t>
static void dequantize_row_iq1_s_sycl(const void *vx, dst_t *y, const int k,
                                        dpct::queue_ptr stream) {
    const int nb = k / QK_K;
    {
        iq1s_grid_gpu.init(*stream);
        ksigns_iq2xs.init(*stream);
        kmask_iq2xs.init(*stream);

        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->submit([&](sycl::handler &cgh) {
            auto iq1s_grid_ptr_ct1 = iq1s_grid_gpu.get_ptr();
            auto ksigns_iq2xs_ptr_ct1 = ksigns_iq2xs.get_ptr();
            auto kmask_iq2xs_ptr_ct1 = kmask_iq2xs.get_ptr();

            cgh.parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, nb) *
                                                   sycl::range<3>(1, 1, 32),
                                               sycl::range<3>(1, 1, 32)),
                             [=](sycl::nd_item<3> item_ct1) {
                                 dequantize_block_iq1_s(
                                     vx, y, item_ct1, iq1s_grid_ptr_ct1,
                                     ksigns_iq2xs_ptr_ct1, kmask_iq2xs_ptr_ct1);
                             });
        });
    }
}

template <typename src_t, typename dst_t>
static void convert_unary_sycl(const void *__restrict__ vx,
                               dst_t *__restrict__ y, const int k,
                               dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_DEQUANTIZE_BLOCK_SIZE - 1) / SYCL_DEQUANTIZE_BLOCK_SIZE;
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(
                sycl::range<3>(1, 1, num_blocks) *
                    sycl::range<3>(1, 1, SYCL_DEQUANTIZE_BLOCK_SIZE),
                sycl::range<3>(1, 1, SYCL_DEQUANTIZE_BLOCK_SIZE)),
            [=](sycl::nd_item<3> item_ct1) {
                convert_unary<src_t>(vx, y, k, item_ct1);
            });
    }
}


static to_fp16_sycl_t ggml_get_to_fp16_sycl(ggml_type type) try {
    int id;
    switch (type) {
        case GGML_TYPE_Q4_0:
            return dequantize_block_sycl<QK4_0, QR4_0, dequantize_q4_0>;
        case GGML_TYPE_Q4_1:
            return dequantize_block_sycl<QK4_1, QR4_1, dequantize_q4_1>;
        case GGML_TYPE_Q5_0:
            return dequantize_block_sycl<QK5_0, QR5_0, dequantize_q5_0>;
        case GGML_TYPE_Q5_1:
            return dequantize_block_sycl<QK5_1, QR5_1, dequantize_q5_1>;
        case GGML_TYPE_Q8_0:
            return dequantize_block_sycl<QK8_0, QR8_0, dequantize_q8_0>;
        case GGML_TYPE_Q2_K:
            return dequantize_row_q2_K_sycl;
        case GGML_TYPE_Q3_K:
            return dequantize_row_q3_K_sycl;
        case GGML_TYPE_Q4_K:
            return dequantize_row_q4_K_sycl;
        case GGML_TYPE_Q5_K:
            return dequantize_row_q5_K_sycl;
        case GGML_TYPE_Q6_K:
            return dequantize_row_q6_K_sycl;
        case GGML_TYPE_IQ2_XXS:
            return dequantize_row_iq2_xxs_sycl;
        case GGML_TYPE_IQ2_XS:
            return dequantize_row_iq2_xs_sycl;
        case GGML_TYPE_IQ3_XXS:
            return dequantize_row_iq3_xxs_sycl;
        case GGML_TYPE_IQ3_S:
            return dequantize_row_iq3_s_sycl;
        case GGML_TYPE_IQ1_S:
            return dequantize_row_iq1_s_sycl;
        case GGML_TYPE_F32:
            return convert_unary_sycl<float>;
        default:
            return nullptr;
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static to_fp32_sycl_t ggml_get_to_fp32_sycl(ggml_type type) {
    switch (type) {
        case GGML_TYPE_Q4_0:
            return dequantize_row_q4_0_sycl;
        case GGML_TYPE_Q4_1:
            return dequantize_row_q4_1_sycl;
        case GGML_TYPE_Q5_0:
            return dequantize_block_sycl<QK5_0, QR5_0, dequantize_q5_0>;
        case GGML_TYPE_Q5_1:
            return dequantize_block_sycl<QK5_1, QR5_1, dequantize_q5_1>;
        case GGML_TYPE_Q8_0:
            return dequantize_block_sycl<QK8_0, QR8_0, dequantize_q8_0>;
        case GGML_TYPE_Q2_K:
            return dequantize_row_q2_K_sycl;
        case GGML_TYPE_Q3_K:
            return dequantize_row_q3_K_sycl;
        case GGML_TYPE_Q4_K:
            return dequantize_row_q4_K_sycl;
        case GGML_TYPE_Q5_K:
            return dequantize_row_q5_K_sycl;
        case GGML_TYPE_Q6_K:
            return dequantize_row_q6_K_sycl;
        case GGML_TYPE_IQ2_XXS:
            return dequantize_row_iq2_xxs_sycl;
        case GGML_TYPE_IQ2_XS:
            return dequantize_row_iq2_xs_sycl;
        case GGML_TYPE_IQ3_XXS:
            return dequantize_row_iq3_xxs_sycl;
        case GGML_TYPE_IQ3_S:
            return dequantize_row_iq3_s_sycl;
        case GGML_TYPE_IQ1_S:
            return dequantize_row_iq1_s_sycl;
        case GGML_TYPE_F16:
            return convert_unary_sycl<sycl::half>;
        default:
            return nullptr;
    }
}

static void dequantize_mul_mat_vec_q4_0_sycl(const void *vx, const dfloat *y,
                                             float *dst, const int ncols,
                                             const int nrows,
                                             dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % GGML_SYCL_DMMV_X == 0);
    const int block_num_y = (nrows + GGML_SYCL_MMV_Y - 1) / GGML_SYCL_MMV_Y;
    // the number of rows may exceed maximum grid size in the y or z dimensions, use the x dimension instead
    const sycl::range<3> block_nums(1, 1, block_num_y);
    const sycl::range<3> block_dims(1, GGML_SYCL_MMV_Y, WARP_SIZE);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
                dequantize_mul_mat_vec<QK4_0, QR4_0, dequantize_q4_0>(
                    vx, y, dst, ncols, nrows, item_ct1);
            });
    }
}

static void dequantize_mul_mat_vec_q4_1_sycl(const void *vx, const dfloat *y,
                                             float *dst, const int ncols,
                                             const int nrows,
                                             dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % GGML_SYCL_DMMV_X == 0);
    const int block_num_y = (nrows + GGML_SYCL_MMV_Y - 1) / GGML_SYCL_MMV_Y;
    const sycl::range<3> block_nums(1, 1, block_num_y);
    const sycl::range<3> block_dims(1, GGML_SYCL_MMV_Y, WARP_SIZE);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
                dequantize_mul_mat_vec<QK4_1, QR4_1, dequantize_q4_1>(
                    vx, y, dst, ncols, nrows, item_ct1);
            });
    }
}

static void dequantize_mul_mat_vec_q5_0_sycl(const void *vx, const dfloat *y,
                                             float *dst, const int ncols,
                                             const int nrows,
                                             dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % GGML_SYCL_DMMV_X == 0);
    const int block_num_y = (nrows + GGML_SYCL_MMV_Y - 1) / GGML_SYCL_MMV_Y;
    const sycl::range<3> block_nums(1, 1, block_num_y);
    const sycl::range<3> block_dims(1, GGML_SYCL_MMV_Y, WARP_SIZE);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
                dequantize_mul_mat_vec<QK5_0, QR5_0, dequantize_q5_0>(
                    vx, y, dst, ncols, nrows, item_ct1);
            });
    }
}

static void dequantize_mul_mat_vec_q5_1_sycl(const void *vx, const dfloat *y,
                                             float *dst, const int ncols,
                                             const int nrows,
                                             dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % GGML_SYCL_DMMV_X == 0);
    const int block_num_y = (nrows + GGML_SYCL_MMV_Y - 1) / GGML_SYCL_MMV_Y;
    const sycl::range<3> block_nums(1, 1, block_num_y);
    const sycl::range<3> block_dims(1, GGML_SYCL_MMV_Y, WARP_SIZE);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
                dequantize_mul_mat_vec<QK5_1, QR5_1, dequantize_q5_1>(
                    vx, y, dst, ncols, nrows, item_ct1);
            });
    }
}

static void dequantize_mul_mat_vec_q8_0_sycl(const void *vx, const dfloat *y,
                                             float *dst, const int ncols,
                                             const int nrows,
                                             dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % GGML_SYCL_DMMV_X == 0);
    const int block_num_y = (nrows + GGML_SYCL_MMV_Y - 1) / GGML_SYCL_MMV_Y;
    const sycl::range<3> block_nums(1, 1, block_num_y);
    const sycl::range<3> block_dims(1, GGML_SYCL_MMV_Y, WARP_SIZE);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
                dequantize_mul_mat_vec<QK8_0, QR8_0, dequantize_q8_0>(
                    vx, y, dst, ncols, nrows, item_ct1);
            });
    }
}

static void dequantize_mul_mat_vec_q2_K_sycl(const void *vx, const float *y,
                                             float *dst, const int ncols,
                                             const int nrows,
                                             dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % QK_K == 0);
    const int ny = 2; // very slightly faster than 1 even when K_QUANTS_PER_ITERATION = 2
    const int block_num_y = (nrows + ny - 1) / ny;
    const sycl::range<3> block_nums(1, 1, block_num_y);
    const sycl::range<3> block_dims(1, ny, 32);
    stream->parallel_for(
        sycl::nd_range<3>(block_nums * block_dims, block_dims),
        [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
            dequantize_mul_mat_vec_q2_k(vx, y, dst, ncols, nrows, item_ct1);
        });
}

static void dequantize_mul_mat_vec_q3_K_sycl(const void *vx, const float *y,
                                             float *dst, const int ncols,
                                             const int nrows,
                                             dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % QK_K == 0);
    const int ny = 2 / K_QUANTS_PER_ITERATION;
    const int block_num_y = (nrows + ny - 1) / ny;
    const sycl::range<3> block_nums(1, 1, block_num_y);
    const sycl::range<3> block_dims(1, ny, 32);
    stream->parallel_for(
        sycl::nd_range<3>(block_nums * block_dims, block_dims),
        [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
            dequantize_mul_mat_vec_q3_k(vx, y, dst, ncols, nrows, item_ct1);
        });
}

static void dequantize_mul_mat_vec_q4_K_sycl(const void *vx, const float *y,
                                             float *dst, const int ncols,
                                             const int nrows,
                                             dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % QK_K == 0);
    const int ny = 2 / K_QUANTS_PER_ITERATION;
    const int block_num_y = (nrows + ny - 1) / ny;
    const sycl::range<3> block_nums(1, 1, block_num_y);
    const sycl::range<3> block_dims(1, ny, 32);
    stream->parallel_for(
        sycl::nd_range<3>(block_nums * block_dims, block_dims),
        [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
            dequantize_mul_mat_vec_q4_k(vx, y, dst, ncols, nrows, item_ct1);
        });
}

static void dequantize_mul_mat_vec_q5_K_sycl(const void *vx, const float *y,
                                             float *dst, const int ncols,
                                             const int nrows,
                                             dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % QK_K == 0);
    const sycl::range<3> block_dims(1, 1, 32);
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, nrows) * block_dims, block_dims),
        [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
            dequantize_mul_mat_vec_q5_k(vx, y, dst, ncols, item_ct1);
        });
}

static void dequantize_mul_mat_vec_q6_K_sycl(const void *vx, const float *y,
                                             float *dst, const int ncols,
                                             const int nrows,
                                             dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % QK_K == 0);
    const int ny = 2 / K_QUANTS_PER_ITERATION;
    const int block_num_y = (nrows + ny - 1) / ny;
    const sycl::range<3> block_nums(1, 1, block_num_y);
    const sycl::range<3> block_dims(1, ny, 32);
    stream->parallel_for(
        sycl::nd_range<3>(block_nums * block_dims, block_dims),
        [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
            dequantize_mul_mat_vec_q6_k(vx, y, dst, ncols, nrows, item_ct1);
        });
}

static void convert_mul_mat_vec_f16_sycl(const void *vx, const dfloat *y,
                                         float *dst, const int ncols,
                                         const int nrows,
                                         dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % GGML_SYCL_DMMV_X == 0);
    const int block_num_y = (nrows + GGML_SYCL_MMV_Y - 1) / GGML_SYCL_MMV_Y;
    const sycl::range<3> block_nums(1, 1, block_num_y);
    const sycl::range<3> block_dims(1, GGML_SYCL_MMV_Y, WARP_SIZE);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
                dequantize_mul_mat_vec<1, 1, convert_f16>(vx, y, dst, ncols,
                                                          nrows, item_ct1);
            });
    }
}

static void ggml_mul_mat_p021_f16_f32_sycl(const void *vx, const float *y,
                                           float *dst, const int ncols_x,
                                           const int nrows_x,
                                           const int nchannels_x,
                                           const int nchannels_y,
                                           dpct::queue_ptr stream) {

    const sycl::range<3> block_nums(nchannels_y, nrows_x, 1);
    const sycl::range<3> block_dims(1, 1, WARP_SIZE);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
                mul_mat_p021_f16_f32(vx, y, dst, ncols_x, nrows_x, nchannels_x,
                                     nchannels_y, item_ct1);
            });
    }
}

static void ggml_mul_mat_vec_nc_f16_f32_sycl(
    const void *vx, const float *y, float *dst, const int ncols_x,
    const int nrows_x, const int row_stride_x, const int nchannels_x,
    const int nchannels_y, const int channel_stride_x, dpct::queue_ptr stream) {

    const sycl::range<3> block_nums(nchannels_y, nrows_x, 1);
    const sycl::range<3> block_dims(1, 1, WARP_SIZE);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
                mul_mat_vec_nc_f16_f32(vx, y, dst, ncols_x, nrows_x,
                                       row_stride_x, channel_stride_x,
                                       nchannels_y / nchannels_x, item_ct1);
            });
    }
}

static void
ggml_cpy_f16_f32_sycl(const char *cx, char *cdst, const int ne, const int ne00,
                      const int ne01, const int ne02, const int nb00,
                      const int nb01, const int nb02, const int nb03,
                      const int ne10, const int ne11, const int ne12,
                      const int nb10, const int nb11, const int nb12,
                      const int nb13, dpct::queue_ptr stream) {

    const int num_blocks = (ne + SYCL_CPY_BLOCK_SIZE - 1) / SYCL_CPY_BLOCK_SIZE;
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                                  sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE),
                              sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE)),
            [=](sycl::nd_item<3> item_ct1) {
                cpy_f32_f16<cpy_1_f16_f32>(cx, cdst, ne, ne00, ne01, ne02, nb00,
                                           nb01, nb02, nb03, ne10, ne11, ne12,
                                           nb10, nb11, nb12, nb13, item_ct1);
            });
    }
}

static void ggml_cpy_f32_f32_sycl(const char *cx, char *cdst, const int ne,
                                  const int ne00, const int ne01,
                                  const int ne02, const int nb00,
                                  const int nb01, const int nb02,
                                  const int nb03, const int ne10,
                                  const int ne11, const int ne12,
                                  const int nb10, const int nb11,
                                  const int nb12, const int nb13,
                                  dpct::queue_ptr stream) {

    const int num_blocks = (ne + SYCL_CPY_BLOCK_SIZE - 1) / SYCL_CPY_BLOCK_SIZE;
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                                  sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE),
                              sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE)),
            [=](sycl::nd_item<3> item_ct1) {
                cpy_f32_f16<cpy_1_f32_f32>(cx, cdst, ne, ne00, ne01, ne02, nb00, nb01, nb02,
                                           nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13,
                                           item_ct1);
            });
    }
}

static void ggml_cpy_f32_f16_sycl(const char *cx, char *cdst, const int ne,
                                  const int ne00, const int ne01,
                                  const int ne02, const int nb00,
                                  const int nb01, const int nb02,
                                  const int nb03, const int ne10,
                                  const int ne11, const int ne12,
                                  const int nb10, const int nb11,
                                  const int nb12, const int nb13,
                                  dpct::queue_ptr stream) {

    const int num_blocks = (ne + SYCL_CPY_BLOCK_SIZE - 1) / SYCL_CPY_BLOCK_SIZE;
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                                  sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE),
                              sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE)),
            [=](sycl::nd_item<3> item_ct1) {
                cpy_f32_f16<cpy_1_f32_f16>(cx, cdst, ne, ne00, ne01, ne02, nb00, nb01, nb02,
                                           nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13,
                                           item_ct1);
            });
    }
}

static void ggml_cpy_f32_q8_0_sycl(const char *cx, char *cdst, const int ne,
                                   const int ne00, const int ne01,
                                   const int ne02, const int nb00,
                                   const int nb01, const int nb02,
                                   const int nb03, const int ne10,
                                   const int ne11, const int ne12,
                                   const int nb10, const int nb11,
                                   const int nb12, const int nb13,
                                   dpct::queue_ptr stream) {

    GGML_ASSERT(ne % QK8_0 == 0);
    const int num_blocks = ne / QK8_0;
    stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks),
                                           sycl::range<3>(1, 1, 1)),
                         [=](sycl::nd_item<3> item_ct1) {
                             cpy_f32_q<cpy_blck_f32_q8_0, QK8_0>(
                                 cx, cdst, ne, ne00, ne01, ne02, nb00, nb01, nb02,
                                 nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13,
                                 item_ct1);
                         });
}

static void ggml_cpy_f32_q4_0_sycl(const char *cx, char *cdst, const int ne,
                                   const int ne00, const int ne01,
                                   const int ne02, const int nb00,
                                   const int nb01, const int nb02,
                                   const int nb03, const int ne10,
                                   const int ne11, const int ne12,
                                   const int nb10, const int nb11,
                                   const int nb12, const int nb13,
                                   dpct::queue_ptr stream) {

    GGML_ASSERT(ne % QK4_0 == 0);
    const int num_blocks = ne / QK4_0;
    stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks),
                                           sycl::range<3>(1, 1, 1)),
                         [=](sycl::nd_item<3> item_ct1) {
                             cpy_f32_q<cpy_blck_f32_q4_0, QK4_0>(
                                 cx, cdst, ne, ne00, ne01, ne02, nb00, nb01, nb02,
                                 nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13,
                                 item_ct1);
                         });
}

static void ggml_cpy_f32_q4_1_sycl(const char *cx, char *cdst, const int ne,
                                   const int ne00, const int ne01,
                                   const int ne02, const int nb00,
                                   const int nb01, const int nb02,
                                   const int nb03, const int ne10,
                                   const int ne11, const int ne12,
                                   const int nb10, const int nb11,
                                   const int nb12, const int nb13,
                                   dpct::queue_ptr stream) {

    GGML_ASSERT(ne % QK4_1 == 0);
    const int num_blocks = ne / QK4_1;
    stream->parallel_for(sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks),
                                           sycl::range<3>(1, 1, 1)),
                         [=](sycl::nd_item<3> item_ct1) {
                             cpy_f32_q<cpy_blck_f32_q4_1, QK4_1>(
                                 cx, cdst, ne, ne00, ne01, ne02, nb00, nb01, nb02,
                                 nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13,
                                 item_ct1);
                         });
}

static void ggml_cpy_f16_f16_sycl(const char *cx, char *cdst, const int ne,
                                  const int ne00, const int ne01,
                                  const int ne02, const int nb00,
                                  const int nb01, const int nb02,
                                  const int nb03, const int ne10,
                                  const int ne11, const int ne12,
                                  const int nb10, const int nb11,
                                  const int nb12, const int nb13,
                                  dpct::queue_ptr stream) {

    const int num_blocks = (ne + SYCL_CPY_BLOCK_SIZE - 1) / SYCL_CPY_BLOCK_SIZE;
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                                  sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE),
                              sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE)),
            [=](sycl::nd_item<3> item_ct1) {
                cpy_f32_f16<cpy_1_f16_f16>(cx, cdst, ne, ne00, ne01, ne02, nb00, nb01, nb02,
                                           nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13,
                                           item_ct1);
            });
    }
}

static void ggml_cpy_i16_i16_sycl(const char *cx, char *cdst, const int ne,
                                  const int ne00, const int ne01,
                                  const int ne02, const int nb00,
                                  const int nb01, const int nb02,
                                  const int nb03, const int ne10,
                                  const int ne11, const int ne12,
                                  const int nb10, const int nb11,
                                  const int nb12, const int nb13,
                                  dpct::queue_ptr stream) {

    const int num_blocks = (ne + SYCL_CPY_BLOCK_SIZE - 1) / SYCL_CPY_BLOCK_SIZE;
    {
        // dpct::has_capability_or_fail(stream->get_device(),
        //                              {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                                  sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE),
                              sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE)),
            [=](sycl::nd_item<3> item_ct1) {
                cpy_f32_f16<cpy_1_i16_i16>(cx, cdst, ne, ne00, ne01, ne02, nb00, nb01, nb02,
                                           nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13,
                                           item_ct1);
            });
    }
}

static void ggml_cpy_i32_i32_sycl(const char *cx, char *cdst, const int ne,
                                  const int ne00, const int ne01,
                                  const int ne02, const int nb00,
                                  const int nb01, const int nb02,
                                  const int nb03, const int ne10,
                                  const int ne11, const int ne12,
                                  const int nb10, const int nb11,
                                  const int nb12, const int nb13,
                                  dpct::queue_ptr stream) {

    const int num_blocks = (ne + SYCL_CPY_BLOCK_SIZE - 1) / SYCL_CPY_BLOCK_SIZE;
    {
        // dpct::has_capability_or_fail(stream->get_device(),
        //                              {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                                  sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE),
                              sycl::range<3>(1, 1, SYCL_CPY_BLOCK_SIZE)),
            [=](sycl::nd_item<3> item_ct1) {
                cpy_f32_f16<cpy_1_i32_i32>(cx, cdst, ne, ne00, ne01, ne02, nb00, nb01, nb02,
                                           nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13,
                                           item_ct1);
            });
    }
}

static void scale_f32_sycl(const float *x, float *dst, const float scale,
                           const int k, dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_SCALE_BLOCK_SIZE - 1) / SYCL_SCALE_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_SCALE_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_SCALE_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            scale_f32(x, dst, scale, k, item_ct1);
        });
}

static void clamp_f32_sycl(const float *x, float *dst, const float min,
                           const float max, const int k,
                           dpct::queue_ptr stream) {
    const int num_blocks = (k + SYCL_CLAMP_BLOCK_SIZE - 1) / SYCL_CLAMP_BLOCK_SIZE;
    stream->parallel_for(
        sycl::nd_range<3>(sycl::range<3>(1, 1, num_blocks) *
                              sycl::range<3>(1, 1, SYCL_CLAMP_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_CLAMP_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            clamp_f32(x, dst, min, max, k, item_ct1);
        });
}

template <typename T>
static void rope_sycl(const T *x, T *dst, int ncols, int nrows,
                      const int32_t *pos, float freq_scale, int p_delta_rows,
                      float freq_base, float ext_factor, float attn_factor,
                      rope_corr_dims corr_dims, dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % 2 == 0);
    const sycl::range<3> block_dims(1, SYCL_ROPE_BLOCK_SIZE, 1);
    const int num_blocks_x = (ncols + 2*SYCL_ROPE_BLOCK_SIZE - 1) / (2*SYCL_ROPE_BLOCK_SIZE);
    const sycl::range<3> block_nums(1, num_blocks_x, nrows);
    if (pos == nullptr) {
        /*
        DPCT1049:40: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) {
                rope<T, false>(x, dst, ncols, pos, freq_scale, p_delta_rows,
                               freq_base, ext_factor, attn_factor, corr_dims,
                               item_ct1);
            });
    } else {
        /*
        DPCT1049:41: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) {
                rope<T, true>(x, dst, ncols, pos, freq_scale, p_delta_rows,
                              freq_base, ext_factor, attn_factor, corr_dims,
                              item_ct1);
            });
    }
}

template <typename T>
static void rope_neox_sycl(const T *x, T *dst, int ncols, int n_dims, int nrows,
                           const int32_t *pos, float freq_scale,
                           int p_delta_rows, float freq_base, float ext_factor,
                           float attn_factor, rope_corr_dims corr_dims,
                           dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % 2 == 0);
    const sycl::range<3> block_dims(1, SYCL_ROPE_BLOCK_SIZE, 1);
    const int num_blocks_x = (ncols + 2*SYCL_ROPE_BLOCK_SIZE - 1) / (2*SYCL_ROPE_BLOCK_SIZE);
    const sycl::range<3> block_nums(1, num_blocks_x, nrows);

    const float theta_scale = powf(freq_base, -2.0f/n_dims);
    const float inv_ndims = -1.0f / n_dims;

    if (pos == nullptr) {
        /*
        DPCT1049:42: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) {
                rope_neox<T, false>(x, dst, ncols, n_dims, pos, freq_scale,
                                    p_delta_rows, ext_factor, attn_factor,
                                    corr_dims, theta_scale, inv_ndims,
                                    item_ct1);
            });
    } else {
        /*
        DPCT1049:43: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) {
                rope_neox<T, true>(x, dst, ncols, n_dims, pos, freq_scale,
                                   p_delta_rows, ext_factor, attn_factor,
                                   corr_dims, theta_scale, inv_ndims, item_ct1);
            });
    }
}

static void rope_glm_f32_sycl(const float *x, float *dst, int ncols, int nrows,
                              const int32_t *pos, float freq_scale,
                              int p_delta_rows, float freq_base, int n_ctx,
                              dpct::queue_ptr stream) {
    GGML_ASSERT(ncols % 4 == 0);
    const sycl::range<3> block_dims(1, 1, SYCL_ROPE_BLOCK_SIZE / 4);
    const int num_blocks_x = (ncols + SYCL_ROPE_BLOCK_SIZE - 1) / SYCL_ROPE_BLOCK_SIZE;
    const sycl::range<3> block_nums(1, nrows, num_blocks_x);
    stream->parallel_for(sycl::nd_range<3>(block_nums * block_dims, block_dims),
                         [=](sycl::nd_item<3> item_ct1) {
                             rope_glm_f32(x, dst, ncols, pos, freq_scale,
                                          p_delta_rows, freq_base, n_ctx,
                                          item_ct1);
                         });
}

static void alibi_f32_sycl(const float *x, float *dst, const int ncols,
                           const int nrows, const int k_rows,
                           const int n_heads_log2_floor, const float m0,
                           const float m1, dpct::queue_ptr stream) {
    const sycl::range<3> block_dims(1, 1, SYCL_ALIBI_BLOCK_SIZE);
    const int num_blocks_x = (ncols + SYCL_ALIBI_BLOCK_SIZE - 1) / (SYCL_ALIBI_BLOCK_SIZE);
    const sycl::range<3> block_nums(1, nrows, num_blocks_x);
    stream->parallel_for(sycl::nd_range<3>(block_nums * block_dims, block_dims),
                         [=](sycl::nd_item<3> item_ct1) {
                             alibi_f32(x, dst, ncols, k_rows,
                                       n_heads_log2_floor, m0, m1, item_ct1);
                         });
}

static void sum_rows_f32_sycl(const float *x, float *dst, const int ncols,
                              const int nrows, dpct::queue_ptr stream) {
    const sycl::range<3> block_dims(1, 1, WARP_SIZE);
    const sycl::range<3> block_nums(1, nrows, 1);
    stream->parallel_for(sycl::nd_range<3>(block_nums * block_dims, block_dims),
                         [=](sycl::nd_item<3> item_ct1)
                             [[intel::reqd_sub_group_size(32)]] {
                                 k_sum_rows_f32(x, dst, ncols, item_ct1);
                             });
}

static void argsort_f32_i32_sycl(const float *x, int *dst, const int ncols,
                                 const int nrows, ggml_sort_order order,
                                 dpct::queue_ptr stream) {
    // bitonic sort requires ncols to be power of 2
    GGML_ASSERT((ncols & (ncols - 1)) == 0);

    const sycl::range<3> block_dims(1, 1, ncols);
    const sycl::range<3> block_nums(1, nrows, 1);
    if (order == GGML_SORT_ORDER_ASC) {
        /*
        DPCT1049:44: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) {
                k_argsort_f32_i32<GGML_SORT_ORDER_ASC>(x, dst, ncols, item_ct1);
            });
    } else if (order == GGML_SORT_ORDER_DESC) {
        /*
        DPCT1049:45: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        stream->parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) {
                k_argsort_f32_i32<GGML_SORT_ORDER_DESC>(x, dst, ncols, item_ct1);
            });
    } else {
        GGML_ASSERT(false);
    }
}

static void diag_mask_inf_f32_sycl(const float *x, float *dst,
                                   const int ncols_x, const int nrows_x,
                                   const int rows_per_channel, const int n_past,
                                   dpct::queue_ptr stream) {
    const sycl::range<3> block_dims(1, SYCL_DIAG_MASK_INF_BLOCK_SIZE, 1);
    const int block_num_x = (ncols_x + SYCL_DIAG_MASK_INF_BLOCK_SIZE - 1) / SYCL_DIAG_MASK_INF_BLOCK_SIZE;
    const sycl::range<3> block_nums(1, block_num_x, nrows_x);
    stream->parallel_for(sycl::nd_range<3>(block_nums * block_dims, block_dims),
                         [=](sycl::nd_item<3> item_ct1) {
                             diag_mask_inf_f32(x, dst, ncols_x,
                                               rows_per_channel, n_past,
                                               item_ct1);
                         });
}

template <bool vals_smem, int ncols_template, int block_size_template>
static void soft_max_f32_submitter(const float * x, const float * mask, const float *pos, float * dst, const int ncols_par,
                                   const int nrows_y, const float scale, const float max_bias, const float m0,
                                   const float m1, uint32_t n_head_log2, sycl::range<3> block_nums, sycl::range<3> block_dims,
                                   const size_t n_local_scratch, dpct::queue_ptr stream) {
    stream->submit([&](sycl::handler &cgh) {
        sycl::local_accessor<float, 1> local_buf_acc(n_local_scratch, cgh);

        cgh.parallel_for(
            sycl::nd_range<3>(block_nums * block_dims, block_dims),
            [=](sycl::nd_item<3> item_ct1) [[intel::reqd_sub_group_size(32)]] {
                soft_max_f32<vals_smem, ncols_template, block_size_template>(x, mask, pos, dst, ncols_par,
                                                                             nrows_y, scale, max_bias, m0,
                                                                             m1, n_head_log2, item_ct1,
                                                                             local_buf_acc.get_pointer());
            });
    });
}

static void soft_max_f32_sycl(const float * x, const float * mask, const float * pos,
                              float * dst, const int ncols_x, const int nrows_x,
                              const int nrows_y, const float scale, const float max_bias,
                              dpct::queue_ptr stream) {
    int nth = WARP_SIZE;
    while (nth < ncols_x && nth < SYCL_SOFT_MAX_BLOCK_SIZE) nth *= 2;
    const sycl::range<3> block_dims(1, 1, nth);
    const sycl::range<3> block_nums(1, 1, nrows_x);
    const size_t n_local_scratch = (GGML_PAD(ncols_x, WARP_SIZE) + WARP_SIZE);
    static_assert(SYCL_SOFT_MAX_BLOCK_SIZE == 1024, "These values need to be adjusted.");

    const uint32_t n_head_kv   = nrows_x/nrows_y;
    const uint32_t n_head_log2 = 1u << (uint32_t) floorf(log2f((float) n_head_kv));

    const float m0 = powf(2.0f, -(max_bias       ) / n_head_log2);
    const float m1 = powf(2.0f, -(max_bias / 2.0f) / n_head_log2);

    const size_t local_mem_size = stream->get_device().get_info<sycl::info::device::local_mem_size>();
    if (n_local_scratch*sizeof(float) < local_mem_size) {
        switch (ncols_x) {
            case 32:
                soft_max_f32_submitter<true, 32, 32>(x, mask, pos, dst, ncols_x, nrows_y, scale,
                                                     max_bias, m0, m1, n_head_log2, block_nums,
                                                     block_dims, n_local_scratch, stream);
                break;
            case 64:
                soft_max_f32_submitter<true, 64, 64>(x, mask, pos, dst, ncols_x, nrows_y, scale,
                                                     max_bias, m0, m1, n_head_log2, block_nums,
                                                     block_dims, n_local_scratch, stream);
                break;
            case 128:
                soft_max_f32_submitter<true, 128, 128>(x, mask, pos, dst, ncols_x, nrows_y, scale,
                                                       max_bias, m0, m1, n_head_log2, block_nums,
                                                       block_dims, n_local_scratch, stream);
                break;
            case 256:
                soft_max_f32_submitter<true, 256, 256>(x, mask, pos, dst, ncols_x, nrows_y, scale,
                                                       max_bias, m0, m1, n_head_log2, block_nums,
                                                       block_dims, n_local_scratch, stream);
                break;
            case 512:
                soft_max_f32_submitter<true, 512, 512>(x, mask, pos, dst, ncols_x, nrows_y, scale,
                                                       max_bias, m0, m1, n_head_log2, block_nums,
                                                       block_dims, n_local_scratch, stream);
                break;
            case 1024:
                soft_max_f32_submitter<true, 1024, 1024>(x, mask, pos, dst, ncols_x, nrows_y, scale,
                                                         max_bias, m0, m1, n_head_log2, block_nums,
                                                         block_dims, n_local_scratch, stream);
                break;
            case 2048:
                soft_max_f32_submitter<true, 2048, 1024>(x, mask, pos, dst, ncols_x, nrows_y, scale,
                                                         max_bias, m0, m1, n_head_log2, block_nums,
                                                         block_dims, n_local_scratch, stream);
                break;
            case 4096:
                soft_max_f32_submitter<true, 4096, 1024>(x, mask, pos, dst, ncols_x, nrows_y, scale,
                                                         max_bias, m0, m1, n_head_log2, block_nums,
                                                         block_dims, n_local_scratch, stream);
                break;
            default:
                soft_max_f32_submitter<true, 0, 0>(x, mask, pos, dst, ncols_x, nrows_y, scale,
                                                   max_bias, m0, m1, n_head_log2, block_nums,
                                                   block_dims, n_local_scratch, stream);
                break;
        }
    } else {
        soft_max_f32_submitter<false, 0, 0>(x, mask, pos, dst, ncols_x, nrows_y, scale,
                                            max_bias, m0, m1, n_head_log2, block_nums,
                                            block_dims, WARP_SIZE, stream);
    }
}

template <typename T>
static void im2col_sycl(const float *x, T *dst, int IW, int IH,
                                int OW, int OH, int KW, int KH, int IC,
                                int offset_delta, int s0, int s1, int p0,
                                int p1, int d0, int d1,
                                dpct::queue_ptr stream) {
    const int parallel_elements = OW * KW * KH;
    const int num_blocks = (parallel_elements + SYCL_IM2COL_BLOCK_SIZE - 1) / SYCL_IM2COL_BLOCK_SIZE;
    sycl::range<3> block_nums(IC, OH, num_blocks);
    {
        dpct::has_capability_or_fail(stream->get_device(),
                                     {sycl::aspect::fp16});

        stream->parallel_for(
            sycl::nd_range<3>(block_nums *
                                  sycl::range<3>(1, 1, SYCL_IM2COL_BLOCK_SIZE),
                              sycl::range<3>(1, 1, SYCL_IM2COL_BLOCK_SIZE)),
            [=](sycl::nd_item<3> item_ct1) {
                im2col_kernel(x, dst, offset_delta, IW, IH, OW, KW, KH,
                               parallel_elements, (IC * KH * KW), s0, s1, p0,
                               p1, d0, d1, item_ct1);
            });
    }
}

// buffer pool for sycl
#define MAX_SYCL_BUFFERS 256

struct scoped_spin_lock {
    std::atomic_flag& lock;
    scoped_spin_lock(std::atomic_flag& lock) : lock(lock) {
        while (lock.test_and_set(std::memory_order_acquire)) {
            ; // spin
        }
    }
    ~scoped_spin_lock() {
        lock.clear(std::memory_order_release);
    }
    scoped_spin_lock(const scoped_spin_lock&) = delete;
    scoped_spin_lock& operator=(const scoped_spin_lock&) = delete;
};

static std::atomic_flag g_sycl_pool_lock = ATOMIC_FLAG_INIT;

// #define DEBUG_SYCL_MALLOC
struct sycl_buffer {
    void * ptr = nullptr;
    size_t size = 0;
};

static sycl_buffer g_sycl_buffer_pool[GGML_SYCL_MAX_DEVICES][MAX_SYCL_BUFFERS];
static size_t g_sycl_pool_size[GGML_SYCL_MAX_DEVICES] = {0};

static void *ggml_sycl_pool_malloc_leg(int device_index, size_t size, size_t *actual_size) try {
    scoped_spin_lock lock(g_sycl_pool_lock);
    // GGML_SYCL_DEBUG("ggml_sycl_pool_malloc_leg device_index %d size=%lu\n", device_index, size);
#ifdef DEBUG_SYCL_MALLOC
    int nnz = 0;
    size_t max_size = 0;
#endif
    size_t best_diff = 1ull << 36;
    int ibest = -1;
    for (int i = 0; i < MAX_SYCL_BUFFERS; ++i) {
        sycl_buffer& b = g_sycl_buffer_pool[device_index][i];
        if (b.ptr != nullptr) {
#ifdef DEBUG_SYCL_MALLOC
            ++nnz;
            if (b.size > max_size) max_size = b.size;
#endif
            if (b.size >= size) {
                size_t diff = b.size - size;
                if (diff < best_diff) {
                    best_diff = diff;
                    ibest = i;
                    if (!best_diff) {
                        void * ptr = b.ptr;
                        *actual_size = b.size;
                        b.ptr = nullptr;
                        b.size = 0;
                        // GGML_SYCL_DEBUG("ggml_sycl_pool_malloc_leg return 1 %p and rm in pool\n", ptr);
                        return ptr;
                    }
                }
            }
        }
    }
    if (ibest >= 0) {
        sycl_buffer& b = g_sycl_buffer_pool[device_index][ibest];
        void * ptr = b.ptr;
        *actual_size = b.size;
        b.ptr = nullptr;
        b.size = 0;
        // GGML_SYCL_DEBUG("ggml_sycl_pool_malloc_leg return 2 %p and rm in pool\n", ptr);
        return ptr;
    }
    void * ptr;
    size_t look_ahead_size = (size_t) (1.05 * size);
    look_ahead_size = 256 * ((look_ahead_size + 255)/256);

    const dpct::queue_ptr stream = g_syclStreams[device_index][0];
    SYCL_CHECK(
        CHECK_TRY_ERROR(ptr = (void *)sycl::malloc_device(
                             look_ahead_size, *stream)));
    *actual_size = look_ahead_size;
    g_sycl_pool_size[device_index] += look_ahead_size;

#ifdef DEBUG_SYCL_MALLOC
    fprintf(stderr, "%s[%d]: %d buffers, max_size = %u MB, pool_size = %u MB, requested %u MB\n", __func__, id, nnz,
            (uint32_t)(max_size/1024/1024), (uint32_t)(g_sycl_pool_size[id]/1024/1024), (uint32_t)(size/1024/1024));
#endif
    // GGML_SYCL_DEBUG("ggml_sycl_pool_malloc_leg look_ahead_size=%lu, return %p\n", look_ahead_size, ptr);
    return ptr;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_sycl_pool_free_leg(int device_index, void *ptr, size_t size) try {
    scoped_spin_lock lock(g_sycl_pool_lock);
    const dpct::queue_ptr stream = g_syclStreams[device_index][0];
    for (int i = 0; i < MAX_SYCL_BUFFERS; ++i) {
        sycl_buffer& b = g_sycl_buffer_pool[device_index][i];
        if (b.ptr == nullptr) {
            b.ptr = ptr;
            b.size = size;
            return;
        }
    }
    fprintf(stderr, "WARNING: sycl buffer pool full, increase MAX_SYCL_BUFFERS\n");
    SYCL_CHECK(CHECK_TRY_ERROR(sycl::free(ptr, *stream)));
    g_sycl_pool_size[device_index] -= size;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

// pool with virtual memory
/*
DPCT1082:64: Migration of CUmemGenericAllocationHandle type is not supported.
*/
// static std::vector<CUmemGenericAllocationHandle>
//     g_sycl_pool_handles[GGML_SYCL_MAX_DEVICES];
static dpct::device_ptr g_sycl_pool_addr[GGML_SYCL_MAX_DEVICES] = {0};
static size_t g_sycl_pool_used[GGML_SYCL_MAX_DEVICES] = {0};

static void *ggml_sycl_pool_malloc_vmm(int device_index, size_t size, size_t *actual_size) try {
    GGML_UNUSED(device_index);
    GGML_UNUSED(size);
    GGML_UNUSED(actual_size);
    return NULL;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_sycl_pool_free_vmm(int device_index, void *ptr, size_t size) try {
    scoped_spin_lock lock(g_sycl_pool_lock);
#ifdef DEBUG_SYCL_MALLOC
    printf("sycl pool[%d]: freed %llu bytes at %llx\n", device_index, (unsigned long long) size, ptr);
#endif

    g_sycl_pool_used[device_index] -= size;

    // all deallocations must be in reverse order of the allocations
    GGML_ASSERT(ptr == (void *) (g_sycl_pool_addr[device_index] + g_sycl_pool_used[device_index]));
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void *ggml_sycl_pool_malloc(int device_index, size_t size, size_t *actual_size) try {
    if (g_device_caps[device_index].vmm) {
        return ggml_sycl_pool_malloc_vmm(device_index, size, actual_size);
    } else {
        return ggml_sycl_pool_malloc_leg(device_index, size, actual_size);
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_sycl_pool_free(int device_index, void *ptr, size_t size) try {
    if (g_device_caps[device_index].vmm) {
        ggml_sycl_pool_free_vmm(device_index, ptr, size);
    } else {
        ggml_sycl_pool_free_leg(device_index, ptr, size);
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}


template<typename T>
struct sycl_pool_alloc {
    int device_index = -1;
    int device_id = -1;
    T * ptr = nullptr;
    size_t actual_size = 0;

    // size is in number of elements
    T * alloc(size_t size) {
        GGML_ASSERT(ptr == nullptr);
        device_id = get_current_device_id();
        device_index = g_sycl_gpu_mgr->get_index(device_id);
        ptr = (T *) ggml_sycl_pool_malloc(device_index, size * sizeof(T), &this->actual_size);
        // GGML_SYCL_DEBUG("sycl_pool_alloc %lu return %p actual size=%lu\n", size * sizeof(T), ptr, this->actual_size);
        return ptr;
    }

    sycl_pool_alloc(size_t size) {
        alloc(size);
    }

    ~sycl_pool_alloc() {
        if (ptr != nullptr) {
            ggml_sycl_pool_free(device_index, ptr, actual_size);
        }
    }

    T * get() {
        return ptr;
    }

    sycl_pool_alloc() = default;
    sycl_pool_alloc(const sycl_pool_alloc &) = delete;
    sycl_pool_alloc(sycl_pool_alloc &&) = delete;
    sycl_pool_alloc& operator=(const sycl_pool_alloc &) = delete;
    sycl_pool_alloc& operator=(sycl_pool_alloc &&) = delete;
};

static bool g_sycl_loaded = false;

bool ggml_sycl_loaded(void) {
    return g_sycl_loaded;
}

void print_device_detail(int id, sycl::device &device, std::string device_type) {

    dpct::device_info prop;
    SYCL_CHECK(CHECK_TRY_ERROR(
        dpct::get_device_info(prop, device)));

    std::string version;
    version += std::to_string(prop.get_major_version());
    version += ".";
    version += std::to_string(prop.get_minor_version());

    device_type = std::regex_replace(device_type, std::regex("ext_oneapi_"), "");

    fprintf(stderr, "|%2d|%18s|%45s|%10s|%11d|%8d|%7d|%15lu|\n", id, device_type.c_str(),
            prop.get_name(), version.c_str(), prop.get_max_compute_units(),
            prop.get_max_work_group_size(), prop.get_max_sub_group_size(),
            prop.get_global_mem_size());
}

void ggml_backend_sycl_print_sycl_devices() {
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_print_sycl_devices\n");
    int device_count = dpct::dev_mgr::instance().device_count();
    std::map<std::string, size_t> DeviceNums;
    fprintf(stderr, "found %d SYCL devices:\n", device_count);
    fprintf(stderr, "|  |                  |                                             |Compute   |Max compute|Max work|Max sub|               |\n");
    fprintf(stderr, "|ID|       Device Type|                                         Name|capability|units      |group   |group  |Global mem size|\n");
    fprintf(stderr, "|--|------------------|---------------------------------------------|----------|-----------|--------|-------|---------------|\n");
    for (int id = 0; id < device_count; ++id) {
        sycl::device device = dpct::dev_mgr::instance().get_device(id);
        sycl::backend backend = device.get_backend();
        std::string backend_type = get_device_backend_and_type(device);
        int type_id=DeviceNums[backend_type]++;
        std::stringstream device_type;
        device_type << "[" <<  backend_type << ":" << std::to_string(type_id) << "]";
        print_device_detail(id, device, device_type.str());
    }
}

void print_gpu_device_list() {
    GGML_ASSERT(g_sycl_gpu_mgr);

    char* hint=NULL;
    if (g_ggml_sycl_backend_gpu_mode == SYCL_SINGLE_GPU_MODE) {
        hint = "use %d SYCL GPUs: [%s] with Max compute units:%d\n";
    } else {
        hint = "detect %d SYCL GPUs: [%s] with top Max compute units:%d\n";
    }
    fprintf(stderr, hint,
        g_sycl_gpu_mgr->get_gpu_count(),
        g_sycl_gpu_mgr->gpus_list.c_str(),
        g_sycl_gpu_mgr->max_compute_units);
}

int get_sycl_env(const char *env_name, int default_val) {
    char *user_device_string = getenv(env_name);
    int user_number = default_val;

    unsigned n;
    if (user_device_string != NULL &&
        sscanf(user_device_string, " %u", &n) == 1) {
        user_number = (int)n;
    } else {
        user_number = default_val;
    }
    return user_number;
}

int get_work_group_size(int user_device_id) {
    dpct::device_info prop;
    dpct::get_device_info(prop,
                          dpct::dev_mgr::instance().get_device(user_device_id));
    return prop.get_max_work_group_size();
}

static void ggml_init_sycl() try {
    static bool initialized = false;

    if (!initialized) {
        fprintf(stderr, "[SYCL] call ggml_init_sycl\n");
        g_ggml_sycl_debug = get_sycl_env("GGML_SYCL_DEBUG", 0);

        fprintf(stderr, "%s: GGML_SYCL_DEBUG: %d\n", __func__, g_ggml_sycl_debug);

#if defined(GGML_SYCL_F16)
        fprintf(stderr, "%s: GGML_SYCL_F16: yes\n", __func__);
#else
        fprintf(stderr, "%s: GGML_SYCL_F16: no\n", __func__);
#endif

/* NOT REMOVE, keep it for next optimize for XMX.
#if defined(SYCL_USE_XMX)
        fprintf(stderr, "%s: SYCL_USE_XMX: yes\n", __func__);
#else
        fprintf(stderr, "%s: SYCL_USE_XMX: no\n", __func__);
#endif
*/

        if (CHECK_TRY_ERROR(g_all_sycl_device_count =
                            dpct::dev_mgr::instance().device_count()) != 0) {
            initialized = true;
            g_sycl_loaded = false;
            return;
        }
        GGML_ASSERT(g_all_sycl_device_count <= GGML_SYCL_MAX_DEVICES);
        ggml_backend_sycl_print_sycl_devices();
        initialized = true;
        g_sycl_loaded = true;
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

void ggml_init_by_gpus(int device_count) try {
    g_device_count = device_count;
    g_work_group_size = g_sycl_gpu_mgr->work_group_size;

    int64_t total_vram = 0;

    print_gpu_device_list();

    for (int id = 0; id < GGML_SYCL_MAX_DEVICES; ++id) {
        g_device_caps[id].vmm = 0;
        g_device_caps[id].device_id = -1;
        g_device_caps[id].cc = 0;
        g_tensor_split[id] = 0;
        g_default_tensor_split[id] = 0;
    }

    for (int i = 0; i < g_device_count; ++i) {
        int device_id = g_sycl_gpu_mgr->gpus[i];
        g_device_caps[i].vmm = 0;

        dpct::device_info prop;
        SYCL_CHECK(CHECK_TRY_ERROR(dpct::get_device_info(
            prop, dpct::dev_mgr::instance().get_device(device_id))));

        g_default_tensor_split[i] = total_vram;
        total_vram += prop.get_global_mem_size();

        g_device_caps[i].cc =
            100 * prop.get_major_version() + 10 * prop.get_minor_version();
    }

    for (int i = 0; i < g_device_count; ++i) {
        g_default_tensor_split[i] /= total_vram;
    }

    for (int i = 0; i < g_device_count; ++i) {
        SYCL_CHECK(ggml_sycl_set_device(i));

        // create sycl streams
        for (int is = 0; is < MAX_STREAMS; ++is) {
            SYCL_CHECK(CHECK_TRY_ERROR(
                g_syclStreams[i][is] =
                    dpct::get_current_device().create_queue(
                        g_sycl_gpu_mgr->get_co_ctx(), dpct::get_current_device())));
        }

        const dpct::queue_ptr stream = g_syclStreams[i][0];
        // create sycl handle
        SYCL_CHECK(CHECK_TRY_ERROR(g_sycl_handles[i] = stream));
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static dpct::err0 ggml_sycl_cpy_tensor_2d(void *dst,
                                          const struct ggml_tensor *src,
                                          int64_t i3, int64_t i2,
                                          int64_t i1_low, int64_t i1_high,
                                          dpct::queue_ptr stream) try {

    dpct::memcpy_direction kind;
    char * src_ptr;
    if (src->backend == GGML_BACKEND_TYPE_CPU) {
        kind = dpct::host_to_device;
        src_ptr = (char *) src->data;
        // GGML_SYCL_DEBUG("ggml_sycl_cpy_tensor_2d  GGML_BACKEND_TYPE_CPU src_ptr %p\n", src_ptr);
    } else if (src->backend == GGML_BACKEND_TYPE_GPU || src->backend == GGML_BACKEND_TYPE_GPU_SPLIT) {
        GGML_ASSERT(src->backend != GGML_BACKEND_TYPE_GPU_SPLIT || (i1_low == 0 && i1_high == src->ne[1]));
        kind = dpct::device_to_device;
        ggml_tensor_extra_gpu * extra = (ggml_tensor_extra_gpu *) src->extra;
        int id;
        SYCL_CHECK(CHECK_TRY_ERROR(
            id = get_current_device_id()));
        // GGML_SYCL_DEBUG("current device index %d\n", id);
        src_ptr = (char *) extra->data_device[id];
    } else {
        // GGML_SYCL_DEBUG("GGML_ASSERT(false)\n");
        GGML_ASSERT(false);
    }
    char * dst_ptr = (char *) dst;

    GGML_TENSOR_LOCALS_1(int64_t, ne, src, ne);
    GGML_TENSOR_LOCALS(int64_t, nb, src, nb);
    const enum ggml_type type = src->type;
    const int64_t ts = ggml_type_size(type);
    const int64_t bs = ggml_blck_size(type);
    int64_t i1_diff = i1_high - i1_low;

    const char * x = src_ptr + i1_low*nb1 + i2*nb2 + i3*nb3;
    if (nb0 == ts && nb1 == ts*ne0/bs) {
        // GGML_SYCL_DEBUG("stream->memcpy: dst_ptr=%p, x=%p, size=%lu\n", dst_ptr, x, i1_diff * nb1);
        // return CHECK_TRY_ERROR(stream->memcpy(dst_ptr, x, i1_diff * nb1));
        return CHECK_TRY_ERROR(dpct::async_dpct_memcpy(dst_ptr, x, i1_diff * nb1,
                                    kind, *stream));

    } else if (nb0 == ts) {
        return CHECK_TRY_ERROR(
            dpct::async_dpct_memcpy(dst_ptr, ts * ne0 / bs, x, nb1,
                                    ts * ne0 / bs, i1_diff, kind, *stream));
    } else {
        for (int64_t i1 = 0; i1 < i1_diff; i1++) {
            const void * rx = (const void *) ((const char *) x + i1*nb1);
            void * rd = (void *) (dst_ptr + i1*ts*ne0/bs);
            // pretend the row is a matrix with cols=1
            dpct::err0 r = CHECK_TRY_ERROR(dpct::async_dpct_memcpy(
                rd, ts / bs, rx, nb0, ts / bs, ne0, kind, *stream));
            /*
            DPCT1001:85: The statement could not be removed.
            */
            /*
            DPCT1000:86: Error handling if-stmt was detected but could not be
            rewritten.
            */
            if (r != 0) return r;
        }
        return 0;
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_sycl_op_get_rows(const ggml_tensor *src0,
                                  const ggml_tensor *src1, ggml_tensor *dst,
                                  const float *src0_d, const float *src1_d,
                                  float *dst_d, const dpct::queue_ptr &stream) {

    GGML_ASSERT(src1->type == GGML_TYPE_I32);
    GGML_ASSERT(dst->type == GGML_TYPE_F32);

    GGML_ASSERT(src0->nb[0] == ggml_type_size(src0->type));
    GGML_ASSERT(src1->nb[0] == ggml_type_size(src1->type));
    GGML_ASSERT(dst->nb[0] == ggml_type_size(dst->type));

    const int32_t * src1_i32 = (const int32_t *) src1_d;

    switch (src0->type) {
        case GGML_TYPE_F16:
            get_rows_sycl_float(src0, src1, dst, (const sycl::half *)src0_d,
                                src1_i32, dst_d, stream);
            break;
        case GGML_TYPE_F32:
            get_rows_sycl_float(src0, src1, dst, src0_d, src1_i32, dst_d, stream);
            break;
        case GGML_TYPE_Q4_0:
            get_rows_sycl<QK4_0, QR4_0, dequantize_q4_0>(src0, src1, dst, src0_d, src1_i32, dst_d, stream);
            break;
        case GGML_TYPE_Q4_1:
            get_rows_sycl<QK4_1, QR4_1, dequantize_q4_1>(src0, src1, dst, src0_d, src1_i32, dst_d, stream);
            break;
        case GGML_TYPE_Q5_0:
            get_rows_sycl<QK5_0, QR5_0, dequantize_q5_0>(src0, src1, dst, src0_d, src1_i32, dst_d, stream);
            break;
        case GGML_TYPE_Q5_1:
            get_rows_sycl<QK5_1, QR5_1, dequantize_q5_1>(src0, src1, dst, src0_d, src1_i32, dst_d, stream);
            break;
        case GGML_TYPE_Q8_0:
            get_rows_sycl<QK8_0, QR8_0, dequantize_q8_0>(src0, src1, dst, src0_d, src1_i32, dst_d, stream);
            break;
        default:
            // TODO: k-quants
            fprintf(stderr, "%s: unsupported type: %s\n", __func__, ggml_type_name(src0->type));
            GGML_ASSERT(false);
            break;
    }
}

template <class op>
inline void ggml_sycl_op_bin_bcast(const ggml_tensor *src0,
                                   const ggml_tensor *src1, ggml_tensor *dst,
                                   const float *src0_dd, const float *src1_dd,
                                   float *dst_dd,
                                   const dpct::queue_ptr &main_stream) {

    if (src0->type == GGML_TYPE_F32 && dst->type == GGML_TYPE_F32) {
        op()(src0, src1, dst, src0_dd, src1_dd, dst_dd, main_stream);
    } else if (src0->type == GGML_TYPE_F16 && dst->type == GGML_TYPE_F16) {
        op()(src0, src1, dst, (const sycl::half *)src0_dd, src1_dd,
             (sycl::half *)dst_dd, main_stream);
    } else if (src0->type == GGML_TYPE_F16 && dst->type == GGML_TYPE_F32) {
        op()(src0, src1, dst, (const sycl::half *)src0_dd, src1_dd, dst_dd,
             main_stream);
    } else if (src0->type == GGML_TYPE_I32 && dst->type == GGML_TYPE_I32) {
        op()(src0, src1, dst, (const int32_t *)src0_dd, (const int32_t *)src1_dd, (int32_t *)dst_dd,
             main_stream);
    } else if (src0->type == GGML_TYPE_I16 && dst->type == GGML_TYPE_I16) {
        op()(src0, src1, dst, (const int16_t *)src0_dd, (const int16_t *)src1_dd, (int16_t *)dst_dd,
             main_stream);
    } else {
        fprintf(stderr, "%s: unsupported types: dst: %s, src0: %s, src1: %s\n", __func__,
            ggml_type_name(dst->type), ggml_type_name(src0->type), ggml_type_name(src1->type));
        GGML_ASSERT(false);
    }
}

static void ggml_sycl_op_repeat(const ggml_tensor *src0,
                                const ggml_tensor *src1, ggml_tensor *dst,
                                const float *src0_d, const float *src1_d,
                                float *dst_d,
                                const dpct::queue_ptr &main_stream) {

    ggml_sycl_op_bin_bcast<bin_bcast_sycl<op_repeat>>(dst, src0, dst, nullptr, src0_d, dst_d, main_stream);

    (void) src1;
    (void) src1_d;
}

inline void ggml_sycl_op_add(const ggml_tensor *src0, const ggml_tensor *src1,
                             ggml_tensor *dst, const float *src0_dd,
                             const float *src1_dd, float *dst_dd,
                             const dpct::queue_ptr &main_stream) {

    ggml_sycl_op_bin_bcast<bin_bcast_sycl<op_add>>(src0, src1, dst, src0_dd, src1_dd, dst_dd, main_stream);
}

inline void ggml_sycl_op_acc(const ggml_tensor *src0, const ggml_tensor *src1,
                             ggml_tensor *dst, const float *src0_dd,
                             const float *src1_dd, float *dst_dd,
                             const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT(src1->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);
    GGML_ASSERT(dst->ne[3] == 1); // just 3D tensors supported

    int nb1 = dst->op_params[0] / 4; // 4 bytes of float32
    int nb2 = dst->op_params[1] / 4; // 4 bytes of float32
    // int nb3 = dst->op_params[2] / 4; // 4 bytes of float32 - unused
    int offset = dst->op_params[3] / 4; // offset in bytes

    acc_f32_sycl(src0_dd, src1_dd, dst_dd, ggml_nelements(dst), src1->ne[0], src1->ne[1], src1->ne[2], nb1, nb2, offset, main_stream);

    (void) dst;
}

inline void ggml_sycl_op_mul(const ggml_tensor *src0, const ggml_tensor *src1,
                             ggml_tensor *dst, const float *src0_dd,
                             const float *src1_dd, float *dst_dd,
                             const dpct::queue_ptr &main_stream) {

    ggml_sycl_op_bin_bcast<bin_bcast_sycl<op_mul>>(src0, src1, dst, src0_dd, src1_dd, dst_dd, main_stream);
}

inline void ggml_sycl_op_div(const ggml_tensor *src0, const ggml_tensor *src1,
                             ggml_tensor *dst, const float *src0_dd,
                             const float *src1_dd, float *dst_dd,
                             const dpct::queue_ptr &main_stream) {

    ggml_sycl_op_bin_bcast<bin_bcast_sycl<op_div>>(src0, src1, dst, src0_dd, src1_dd, dst_dd, main_stream);
}

inline void ggml_sycl_op_gelu(const ggml_tensor *src0, const ggml_tensor *src1,
                              ggml_tensor *dst, const float *src0_dd,
                              const float *src1_dd, float *dst_dd,
                              const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    gelu_f32_sycl(src0_dd, dst_dd, ggml_nelements(src0), main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_silu(const ggml_tensor *src0, const ggml_tensor *src1,
                              ggml_tensor *dst, const float *src0_dd,
                              const float *src1_dd, float *dst_dd,
                              const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    silu_f32_sycl(src0_dd, dst_dd, ggml_nelements(src0), main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_gelu_quick(const ggml_tensor *src0,
                                    const ggml_tensor *src1, ggml_tensor *dst,
                                    const float *src0_dd, const float *src1_dd,
                                    float *dst_dd,
                                    const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    gelu_quick_f32_sycl(src0_dd, dst_dd, ggml_nelements(src0), main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_tanh(const ggml_tensor *src0, const ggml_tensor *src1,
                              ggml_tensor *dst, const float *src0_dd,
                              const float *src1_dd, float *dst_dd,
                              const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);
    tanh_f32_sycl(src0_dd, dst_dd, ggml_nelements(src0), main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_relu(const ggml_tensor *src0, const ggml_tensor *src1,
                              ggml_tensor *dst, const float *src0_dd,
                              const float *src1_dd, float *dst_dd,
                              const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    relu_f32_sycl(src0_dd, dst_dd, ggml_nelements(src0), main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

static void ggml_sycl_op_hardsigmoid(const ggml_tensor *src0,
                                     const ggml_tensor *src1, ggml_tensor *dst,
                                     const float *src0_dd, const float *src1_dd,
                                     float *dst_dd,
                                     const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    hardsigmoid_f32_sycl(src0_dd, dst_dd, ggml_nelements(src0), main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

static void ggml_sycl_op_hardswish(const ggml_tensor *src0,
                                   const ggml_tensor *src1, ggml_tensor *dst,
                                   const float *src0_dd, const float *src1_dd,
                                   float *dst_dd, const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    hardswish_f32_sycl(src0_dd, dst_dd, ggml_nelements(src0), main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_leaky_relu(const ggml_tensor *src0,
                                    const ggml_tensor *src1, ggml_tensor *dst,
                                    const float *src0_dd, const float *src1_dd,
                                    float *dst_dd,
                                    const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    float negative_slope;
    memcpy(&negative_slope, dst->op_params, sizeof(float));

    leaky_relu_f32_sycl(src0_dd, dst_dd, ggml_nelements(src0), negative_slope, main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_sqr(const ggml_tensor *src0, const ggml_tensor *src1,
                             ggml_tensor *dst, const float *src0_dd,
                             const float *src1_dd, float *dst_dd,
                             const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    sqr_f32_sycl(src0_dd, dst_dd, ggml_nelements(src0), main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_norm(const ggml_tensor *src0, const ggml_tensor *src1,
                              ggml_tensor *dst, const float *src0_dd,
                              const float *src1_dd, float *dst_dd,
                              const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    const int64_t ne00 = src0->ne[0];
    const int64_t nrows = ggml_nrows(src0);

    float eps;
    memcpy(&eps, dst->op_params, sizeof(float));

    norm_f32_sycl(src0_dd, dst_dd, ne00, nrows, eps, main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_group_norm(const ggml_tensor *src0,
                                    const ggml_tensor *src1, ggml_tensor *dst,
                                    const float *src0_dd, const float *src1_dd,
                                    float *dst_dd,
                                    const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    int num_groups = dst->op_params[0];
    int group_size = src0->ne[0] * src0->ne[1] * ((src0->ne[2] + num_groups - 1) / num_groups);
    group_norm_f32_sycl(src0_dd, dst_dd, num_groups, group_size, src0->ne[0] * src0->ne[1] * src0->ne[2], main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_concat(const ggml_tensor *src0,
                                const ggml_tensor *src1, ggml_tensor *dst,
                                const float *src0_dd, const float *src1_dd,
                                float *dst_dd,
                                const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT(src1->type == GGML_TYPE_F32);
    GGML_ASSERT(dst->type == GGML_TYPE_F32);

    for (int i3 = 0; i3 < dst->ne[3]; i3++) {
        concat_f32_sycl(src0_dd + i3 * (src0->nb[3] / 4), src1_dd + i3 * (src1->nb[3] / 4), dst_dd + i3 * (dst->nb[3] / 4), dst->ne[0], dst->ne[1], dst->ne[2], src0->ne[2], main_stream);
    }

    (void) src1;
    (void) dst;
}

inline void ggml_sycl_op_upscale(const ggml_tensor *src0,
                                 const ggml_tensor *src1, ggml_tensor *dst,
                                 const float *src0_dd, const float *src1_dd,
                                 float *dst_dd,
                                 const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT(dst->type == GGML_TYPE_F32);
    GGML_ASSERT(src0->ne[3] == 1 && dst->ne[3] == 1); // just 3D tensors

    const int scale_factor = dst->op_params[0];

    upscale_f32_sycl(src0_dd, dst_dd, src0->ne[0], src0->ne[1], src0->ne[2], scale_factor, main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_pad(const ggml_tensor *src0, const ggml_tensor *src1,
                             ggml_tensor *dst, const float *src0_dd,
                             const float *src1_dd, float *dst_dd,
                             const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT(dst->type == GGML_TYPE_F32);
    GGML_ASSERT(src0->ne[3] == 1 && dst->ne[3] == 1); // just 3D tensors

    pad_f32_sycl(src0_dd, dst_dd,
        src0->ne[0], src0->ne[1], src0->ne[2],
        dst->ne[0], dst->ne[1], dst->ne[2], main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_rms_norm(const ggml_tensor *src0,
                                  const ggml_tensor *src1, ggml_tensor *dst,
                                  const float *src0_dd, const float *src1_dd,
                                  float *dst_dd,
                                  const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    const int64_t ne00 = src0->ne[0];
    const int64_t nrows = ggml_nrows(src0);

    float eps;
    memcpy(&eps, dst->op_params, sizeof(float));

    rms_norm_f32_sycl(src0_dd, dst_dd, ne00, nrows, eps, main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

static int64_t get_row_rounding(ggml_type type, const std::array<float, GGML_SYCL_MAX_DEVICES> & tensor_split) {
    int64_t min_compute_capability = INT_MAX;
    int64_t max_compute_capability = INT_MIN;
    for (int i = 0; i < g_device_count; ++i) {
        if (tensor_split[i] < (i + 1 < g_device_count ? tensor_split[i + 1] : 1.0f)) {
            if (min_compute_capability > g_device_caps[i].cc) {
                min_compute_capability = g_device_caps[i].cc;
            }
            if (max_compute_capability < g_device_caps[i].cc) {
                max_compute_capability = g_device_caps[i].cc;
            }
        }
    }

    switch(type) {
        case GGML_TYPE_Q4_0:
        case GGML_TYPE_Q4_1:
            return max_compute_capability >= VER_GEN9 ? 128 : 64;
        case GGML_TYPE_Q5_0:
        case GGML_TYPE_Q5_1:
        case GGML_TYPE_Q8_0:
            return 64;
        case GGML_TYPE_F16:
        case GGML_TYPE_F32:
            return 1;
        case GGML_TYPE_Q2_K:
        case GGML_TYPE_Q3_K:
        case GGML_TYPE_Q4_K:
        case GGML_TYPE_Q5_K:
        case GGML_TYPE_IQ2_XXS:
        case GGML_TYPE_IQ2_XS:
        case GGML_TYPE_IQ1_S:
        case GGML_TYPE_IQ3_XXS:
            return max_compute_capability >= VER_GEN9 ? 128 : 64;
        case GGML_TYPE_IQ3_S:
            return max_compute_capability >= VER_GEN9 ? 128 : 64;
        case GGML_TYPE_Q6_K:
            return 64;
        default:
            GGML_ASSERT(false);
    }

}

inline void ggml_sycl_op_dequantize_mul_mat_vec(
    const ggml_tensor *src0, const ggml_tensor *src1, ggml_tensor *dst,
    const char *src0_dd_i, const float *src1_ddf_i, const char *src1_ddq_i,
    float *dst_dd_i, const int64_t row_low, const int64_t row_high,
    const int64_t src1_ncols, const int64_t src1_padded_row_size,
    const dpct::queue_ptr &stream) {

    const int64_t ne00 = src0->ne[0];
    const int64_t row_diff = row_high - row_low;

    GGML_ASSERT(src1->type == GGML_TYPE_F32);

    // on some GPUs it is faster to convert src1 to half and to use half precision intrinsics
#ifdef GGML_SYCL_F16
    sycl_pool_alloc<sycl::half> src1_dfloat_a;
    sycl::half *src1_dfloat = nullptr; // dfloat == half

    bool src1_convert_f16 =
        src0->type == GGML_TYPE_Q4_0 || src0->type == GGML_TYPE_Q4_1 ||
        src0->type == GGML_TYPE_Q5_0 || src0->type == GGML_TYPE_Q5_1 ||
        src0->type == GGML_TYPE_Q8_0 || src0->type == GGML_TYPE_F16;

    if (src1_convert_f16) {
        src1_dfloat = src1_dfloat_a.alloc(ne00);
        const to_fp16_sycl_t to_fp16_sycl = ggml_get_to_fp16_sycl(src1->type);
        GGML_ASSERT(to_fp16_sycl != nullptr);
        to_fp16_sycl(src1_ddf_i, src1_dfloat, ne00, stream);
    }
#else
    const dfloat * src1_dfloat = (const dfloat *) src1_ddf_i; // dfloat == float, no conversion
#endif // GGML_SYCL_F16

    switch (src0->type) {
        case GGML_TYPE_Q4_0:
            dequantize_mul_mat_vec_q4_0_sycl(src0_dd_i, src1_dfloat, dst_dd_i, ne00, row_diff, stream);
            break;
        case GGML_TYPE_Q4_1:
            dequantize_mul_mat_vec_q4_1_sycl(src0_dd_i, src1_dfloat, dst_dd_i, ne00, row_diff, stream);
            break;
        case GGML_TYPE_Q5_0:
            dequantize_mul_mat_vec_q5_0_sycl(src0_dd_i, src1_dfloat, dst_dd_i, ne00, row_diff, stream);
            break;
        case GGML_TYPE_Q5_1:
            dequantize_mul_mat_vec_q5_1_sycl(src0_dd_i, src1_dfloat, dst_dd_i, ne00, row_diff, stream);
            break;
        case GGML_TYPE_Q8_0:
            dequantize_mul_mat_vec_q8_0_sycl(src0_dd_i, src1_dfloat, dst_dd_i, ne00, row_diff, stream);
            break;
        case GGML_TYPE_Q2_K:
            dequantize_mul_mat_vec_q2_K_sycl(src0_dd_i, src1_ddf_i, dst_dd_i, ne00, row_diff, stream);
            break;
        case GGML_TYPE_Q3_K:
            dequantize_mul_mat_vec_q3_K_sycl(src0_dd_i, src1_ddf_i, dst_dd_i, ne00, row_diff, stream);
            break;
        case GGML_TYPE_Q4_K:
            dequantize_mul_mat_vec_q4_K_sycl(src0_dd_i, src1_ddf_i, dst_dd_i, ne00, row_diff, stream);
            break;
        case GGML_TYPE_Q5_K:
            dequantize_mul_mat_vec_q5_K_sycl(src0_dd_i, src1_ddf_i, dst_dd_i, ne00, row_diff, stream);
            break;
        case GGML_TYPE_Q6_K:
            dequantize_mul_mat_vec_q6_K_sycl(src0_dd_i, src1_ddf_i, dst_dd_i, ne00, row_diff, stream);
            break;
        case GGML_TYPE_F16:
            convert_mul_mat_vec_f16_sycl(src0_dd_i, src1_dfloat, dst_dd_i, ne00, row_diff, stream);
            break;
        default:
            GGML_ASSERT(false);
            break;
    }

    (void) src1;
    (void) dst;
    (void) src1_ddq_i;
    (void) src1_ncols;
    (void) src1_padded_row_size;
}

inline void ggml_sycl_op_mul_mat_sycl(
    const ggml_tensor *src0, const ggml_tensor *src1, ggml_tensor *dst,
    const char *src0_dd_i, const float *src1_ddf_i, const char *src1_ddq_i,
    float *dst_dd_i, const int64_t row_low, const int64_t row_high,
    const int64_t src1_ncols, const int64_t src1_padded_row_size,
    const dpct::queue_ptr &stream) try {

    GGML_ASSERT(src0_dd_i  != nullptr);
    GGML_ASSERT(src1_ddf_i != nullptr);
    GGML_ASSERT(dst_dd_i   != nullptr);

    const int64_t ne00 = src0->ne[0];
    const int64_t ne10 = src1->ne[0];

    const int64_t ne0 = dst->ne[0];

    const int64_t row_diff = row_high - row_low;

    int id;
    SYCL_CHECK(
        CHECK_TRY_ERROR(id = get_current_device_id()));

    // the main device has a larger memory buffer to hold the results from all GPUs
    // ldc == nrows of the matrix that cuBLAS writes into
    int ldc = dst->backend == GGML_BACKEND_TYPE_GPU && id == g_main_device ? ne0 : row_diff;

#ifdef GGML_SYCL_F16
    bool use_fp16 = true;  // TODO(Yu) SYCL capability check
#else
    bool use_fp16 = false;
#endif
    if ((src0->type == GGML_TYPE_F16 || ggml_is_quantized(src0->type)) &&
        use_fp16 && ggml_is_contiguous(src0) && row_diff == src0->ne[1] &&
        dst->op_params[0] == GGML_PREC_DEFAULT) {

        // GGML_SYCL_DEBUG("ggml_sycl_op_mul_mat_sycl - fp16 path\n");
        sycl_pool_alloc<sycl::half> src0_as_f16;
        if (src0->type != GGML_TYPE_F16) {
            const to_fp16_sycl_t to_fp16_sycl = ggml_get_to_fp16_sycl(src0->type);
            GGML_ASSERT(to_fp16_sycl != nullptr);
            size_t ne = row_diff*ne00;
            src0_as_f16.alloc(ne);
            to_fp16_sycl(src0_dd_i, src0_as_f16.get(), ne, stream);
        }
        const sycl::half *src0_ptr = src0->type == GGML_TYPE_F16
                                         ? (const sycl::half *)src0_dd_i
                                         : src0_as_f16.get();

        sycl_pool_alloc<sycl::half> src1_as_f16;
        if (src1->type != GGML_TYPE_F16) {
            const to_fp16_sycl_t to_fp16_sycl = ggml_get_to_fp16_sycl(src1->type);
            GGML_ASSERT(to_fp16_sycl != nullptr);
            size_t ne = src1_ncols*ne10;
            src1_as_f16.alloc(ne);
            to_fp16_sycl(src1_ddf_i, src1_as_f16.get(), ne, stream);
        }
        const sycl::half *src1_ptr = src1->type == GGML_TYPE_F16
                ? (const sycl::half *)src1->data + src1_padded_row_size
                                         : src1_as_f16.get();
        sycl_pool_alloc<sycl::half> dst_f16(row_diff * src1_ncols);

        const sycl::half alpha_f16 = 1.0f;
        const sycl::half beta_f16 = 0.0f;
        SYCL_CHECK(CHECK_TRY_ERROR(g_sycl_handles[id] = stream));
        SYCL_CHECK(CHECK_TRY_ERROR(dpct::gemm(
            *g_sycl_handles[id], oneapi::mkl::transpose::trans,
            oneapi::mkl::transpose::nontrans, row_diff, src1_ncols, ne10,
            &alpha_f16, src0_ptr, dpct::library_data_t::real_half, ne00,
            src1_ptr, dpct::library_data_t::real_half, ne10, &beta_f16,
            dst_f16.get(), dpct::library_data_t::real_half, ldc,
            dpct::library_data_t::real_half)));
        g_sycl_handles[id]->wait();
        const to_fp32_sycl_t to_fp32_sycl = ggml_get_to_fp32_sycl(GGML_TYPE_F16);
        to_fp32_sycl(dst_f16.get(), dst_dd_i, row_diff*src1_ncols, stream);
    }
    else {
        // GGML_SYCL_DEBUG("ggml_sycl_op_mul_mat_sycl - fp32 path\n");
        sycl_pool_alloc<float> src0_ddq_as_f32;
        sycl_pool_alloc<float> src1_ddq_as_f32;
        if (src0->type != GGML_TYPE_F32) {
            const to_fp32_sycl_t to_fp32_sycl = ggml_get_to_fp32_sycl(src0->type);
            GGML_ASSERT(to_fp32_sycl != nullptr);
            src0_ddq_as_f32.alloc(row_diff*ne00);
            to_fp32_sycl(src0_dd_i, src0_ddq_as_f32.get(), row_diff*ne00, stream);
        }
        if (src1->type != GGML_TYPE_F32) {
            const to_fp32_sycl_t to_fp32_sycl = ggml_get_to_fp32_sycl(src1->type);
            GGML_ASSERT(to_fp32_sycl != nullptr);
            src1_ddq_as_f32.alloc(src1_ncols*ne10);
            to_fp32_sycl(src1_ddf_i, src1_ddq_as_f32.get(), src1_ncols*ne10, stream);
        }
        const float * src0_ddf_i = src0->type == GGML_TYPE_F32 ? (const float *) src0_dd_i : src0_ddq_as_f32.get();
        const float * src1_ddf1_i = src1->type == GGML_TYPE_F32 ? (const float *) src1_ddf_i : src1_ddq_as_f32.get();

        const float alpha = 1.0f;
        const float beta = 0.0f;

        SYCL_CHECK(CHECK_TRY_ERROR(g_sycl_handles[id] = stream));
        SYCL_CHECK(CHECK_TRY_ERROR(oneapi::mkl::blas::column_major::gemm(
            *g_sycl_handles[id], oneapi::mkl::transpose::trans,
            oneapi::mkl::transpose::nontrans, row_diff, src1_ncols, ne10,
            dpct::get_value(&alpha, *g_sycl_handles[id]), src0_ddf_i, ne00,
            src1_ddf1_i, ne10, dpct::get_value(&beta, *g_sycl_handles[id]),
            dst_dd_i, ldc)));
        g_sycl_handles[id]->wait();
    }
    (void) dst;
    (void) src1_ddq_i;
    (void) src1_padded_row_size;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

inline void ggml_sycl_op_rope(const ggml_tensor *src0, const ggml_tensor *src1,
                              ggml_tensor *dst, const float *src0_dd,
                              const float *src1_dd, float *dst_dd,
                              const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32 || src0->type == GGML_TYPE_F16);
    GGML_ASSERT( dst->type == GGML_TYPE_F32 ||  dst->type == GGML_TYPE_F16);
    GGML_ASSERT(src0->type == dst->type);

    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];
    const int64_t ne2 = dst->ne[2];
    const int64_t nrows = ggml_nrows(src0);

    //const int n_past      = ((int32_t *) dst->op_params)[0];
    const int n_dims      = ((int32_t *) dst->op_params)[1];
    const int mode        = ((int32_t *) dst->op_params)[2];
    const int n_ctx       = ((int32_t *) dst->op_params)[3];
    const int n_orig_ctx  = ((int32_t *) dst->op_params)[4];

    // RoPE alteration for extended context
    float freq_base, freq_scale, ext_factor, attn_factor, beta_fast, beta_slow;
    memcpy(&freq_base,   (int32_t *) dst->op_params +  5, sizeof(float));
    memcpy(&freq_scale,  (int32_t *) dst->op_params +  6, sizeof(float));
    memcpy(&ext_factor,  (int32_t *) dst->op_params +  7, sizeof(float));
    memcpy(&attn_factor, (int32_t *) dst->op_params +  8, sizeof(float));
    memcpy(&beta_fast,   (int32_t *) dst->op_params +  9, sizeof(float));
    memcpy(&beta_slow,   (int32_t *) dst->op_params + 10, sizeof(float));

    const int32_t * pos = nullptr;
    if ((mode & 1) == 0) {
        GGML_ASSERT(src1->type == GGML_TYPE_I32);
        GGML_ASSERT(src1->ne[0] == ne2);
        pos = (const int32_t *) src1_dd;
    }

    const bool is_neox = mode & 2;
    const bool is_glm  = mode & 4;

    rope_corr_dims corr_dims;
    ggml_rope_yarn_corr_dims(n_dims, n_orig_ctx, freq_base, beta_fast, beta_slow, corr_dims.v);

    // compute
    if (is_glm) {
        GGML_ASSERT(false);
        rope_glm_f32_sycl(src0_dd, dst_dd, ne00, nrows, pos, freq_scale, ne01, freq_base, n_ctx, main_stream);
    } else if (is_neox) {
        if (src0->type == GGML_TYPE_F32) {
            rope_neox_sycl(
                (const float *)src0_dd, (float *)dst_dd, ne00, n_dims, nrows, pos, freq_scale, ne01, freq_base, ext_factor,
                attn_factor, corr_dims, main_stream
            );
        } else if (src0->type == GGML_TYPE_F16) {
            rope_neox_sycl((const sycl::half *)src0_dd, (sycl::half *)dst_dd,
                           ne00, n_dims, nrows, pos, freq_scale, ne01,
                           freq_base, ext_factor, attn_factor, corr_dims,
                           main_stream);
        } else {
            GGML_ASSERT(false);
        }
    } else {
        if (src0->type == GGML_TYPE_F32) {
            rope_sycl(
                (const float *)src0_dd, (float *)dst_dd, ne00, nrows, pos, freq_scale, ne01, freq_base, ext_factor,
                attn_factor, corr_dims, main_stream
            );
        } else if (src0->type == GGML_TYPE_F16) {
            rope_sycl((const sycl::half *)src0_dd, (sycl::half *)dst_dd, ne00,
                      nrows, pos, freq_scale, ne01, freq_base, ext_factor,
                      attn_factor, corr_dims, main_stream);
        } else {
            GGML_ASSERT(false);
        }
    }

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_alibi(const ggml_tensor *src0, const ggml_tensor *src1,
                               ggml_tensor *dst, const float *src0_dd,
                               const float *src1_dd, float *dst_dd,
                               const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    GGML_TENSOR_LOCALS_3(int64_t, ne0, src0, ne);
    const int64_t nrows = ggml_nrows(src0);

    //const int n_past = ((int32_t *) dst->op_params)[0];
    const int n_head = ((int32_t *) dst->op_params)[1];
    float max_bias;
    memcpy(&max_bias, (int32_t *) dst->op_params + 2, sizeof(float));

    //GGML_ASSERT(ne01 + n_past == ne00);
    GGML_ASSERT(n_head == ne02);

    const int n_heads_log2_floor = 1 << (int) floor(log2(n_head));

    const float m0 = powf(2.0f, -(max_bias) / n_heads_log2_floor);
    const float m1 = powf(2.0f, -(max_bias / 2.0f) / n_heads_log2_floor);

    alibi_f32_sycl(src0_dd, dst_dd, ne00, nrows, ne01, n_heads_log2_floor, m0, m1, main_stream);

    (void) src1;
    (void) src1_dd;
}

static void ggml_sycl_op_pool2d(const ggml_tensor *src0,
                                const ggml_tensor *src1, ggml_tensor *dst,
                                const float *src0_dd, const float *src1_dd,
                                float *dst_dd, const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    const int32_t * opts = (const int32_t *)dst->op_params;
    enum ggml_op_pool op = static_cast<ggml_op_pool>(opts[0]);
    const int k0 = opts[1];
    const int k1 = opts[2];
    const int s0 = opts[3];
    const int s1 = opts[4];
    const int p0 = opts[5];
    const int p1 = opts[6];

    const int64_t IH = src0->ne[1];
    const int64_t IW = src0->ne[0];

    const int64_t N = dst->ne[3];
    const int64_t OC = dst->ne[2];
    const int64_t OH = dst->ne[1];
    const int64_t OW = dst->ne[0];

    const int parallel_elements = N * OC * OH * OW;
    const int num_blocks = (parallel_elements + SYCL_POOL2D_BLOCK_SIZE - 1) / SYCL_POOL2D_BLOCK_SIZE;
    sycl::range<3> block_nums(1, 1, num_blocks);
    main_stream->parallel_for(
        sycl::nd_range<3>(block_nums *
                              sycl::range<3>(1, 1, SYCL_IM2COL_BLOCK_SIZE),
                          sycl::range<3>(1, 1, SYCL_IM2COL_BLOCK_SIZE)),
        [=](sycl::nd_item<3> item_ct1) {
            pool2d_nchw_kernel(IH, IW, OH, OW, k1, k0, s1, s0, p1, p0,
                               parallel_elements, src0_dd, dst_dd, op,
                               item_ct1);
        });

    (void) src1;
    (void) src1_dd;
}

inline void ggml_sycl_op_im2col(const ggml_tensor *src0,
                                const ggml_tensor *src1, ggml_tensor *dst,
                                const float *src0_dd, const float *src1_dd,
                                float *dst_dd,
                                const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F16);
    GGML_ASSERT(src1->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F16 || dst->type == GGML_TYPE_F32);

    const int32_t s0 = ((const int32_t*)(dst->op_params))[0];
    const int32_t s1 = ((const int32_t*)(dst->op_params))[1];
    const int32_t p0 = ((const int32_t*)(dst->op_params))[2];
    const int32_t p1 = ((const int32_t*)(dst->op_params))[3];
    const int32_t d0 = ((const int32_t*)(dst->op_params))[4];
    const int32_t d1 = ((const int32_t*)(dst->op_params))[5];

    const bool is_2D = ((const int32_t*)(dst->op_params))[6] == 1;

    const int64_t IC = src1->ne[is_2D ? 2 : 1];
    const int64_t IH = is_2D ? src1->ne[1] : 1;
    const int64_t IW =         src1->ne[0];

    const int64_t KH = is_2D ? src0->ne[1] : 1;
    const int64_t KW =         src0->ne[0];

    const int64_t OH = is_2D ? dst->ne[2] : 1;
    const int64_t OW =         dst->ne[1];

    const size_t delta_offset = src1->nb[is_2D ? 2 : 1] / 4; // nb is byte offset, src is type float32

    if (dst->type == GGML_TYPE_F16) {
        im2col_sycl(src1_dd, (sycl::half *)dst_dd, IW, IH, OW, OH, KW, KH, IC, delta_offset, s0, s1, p0, p1, d0, d1, main_stream);
    } else {
        im2col_sycl(src1_dd, (float *)dst_dd, IW, IH, OW, OH, KW, KH, IC, delta_offset, s0, s1, p0, p1, d0, d1, main_stream);
    }

    (void) src0;
    (void) src0_dd;
}

inline void ggml_sycl_op_sum_rows(const ggml_tensor *src0,
                                  const ggml_tensor *src1, ggml_tensor *dst,
                                  const float *src0_dd, const float *src1_dd,
                                  float *dst_dd,
                                  const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    const int64_t ncols = src0->ne[0];
    const int64_t nrows = ggml_nrows(src0);

    sum_rows_f32_sycl(src0_dd, dst_dd, ncols, nrows, main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_argsort(const ggml_tensor *src0,
                                 const ggml_tensor *src1, ggml_tensor *dst,
                                 const float *src0_dd, const float *src1_dd,
                                 float *dst_dd,
                                 const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_I32);

    const int64_t ncols = src0->ne[0];
    const int64_t nrows = ggml_nrows(src0);

    enum ggml_sort_order order = (enum ggml_sort_order) dst->op_params[0];

    argsort_f32_i32_sycl(src0_dd, (int *)dst_dd, ncols, nrows, order, main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_diag_mask_inf(const ggml_tensor *src0,
                                       const ggml_tensor *src1,
                                       ggml_tensor *dst, const float *src0_dd,
                                       const float *src1_dd, float *dst_dd,
                                       const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];
    const int nrows0 = ggml_nrows(src0);

    const int n_past = ((int32_t *) dst->op_params)[0];

    diag_mask_inf_f32_sycl(src0_dd, dst_dd, ne00, nrows0, ne01, n_past, main_stream);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_soft_max(const ggml_tensor *src0,
                                  const ggml_tensor *src1, ggml_tensor *dst,
                                  const float *src0_dd, const float *src1_dd,
                                  float *dst_dd,
                                  const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    GGML_ASSERT(!src1 || src1->type == GGML_TYPE_F32); // src1 contains mask and it is optional

    const int64_t ne00 = src0->ne[0];
    const int64_t nrows_x = ggml_nrows(src0);
    const int64_t nrows_y = src0->ne[1];

    float scale = 1.0f;
    float max_bias = 0.0f;

    memcpy(&scale, dst->op_params + 0, sizeof(float));
    memcpy(&max_bias, dst->op_params + 1, sizeof(float));

    // positions tensor
    float * src2_dd = nullptr;
    sycl_pool_alloc<float> src2_f;

    ggml_tensor * src2 = dst->src[2];
    const bool use_src2 = src2 != nullptr;

    if (use_src2) {
        const bool src2_on_device = src2->backend == GGML_BACKEND_TYPE_GPU;

        if (src2_on_device) {
            ggml_tensor_extra_gpu * src2_extra = (ggml_tensor_extra_gpu *) src2->extra;
            src2_dd = (float *) src2_extra->data_device[g_main_device];
        } else {
            src2_dd = src2_f.alloc(ggml_nelements(src2));
            SYCL_CHECK(ggml_sycl_cpy_tensor_2d(src2_dd, src2, 0, 0, 0, 1, main_stream));
        }
    }

    soft_max_f32_sycl(src0_dd, src1 ? src1_dd : nullptr, src2_dd, dst_dd, ne00,
                      nrows_x, nrows_y, scale, max_bias, main_stream);
}

inline void ggml_sycl_op_scale(const ggml_tensor *src0, const ggml_tensor *src1,
                               ggml_tensor *dst, const float *src0_dd,
                               const float *src1_dd, float *dst_dd,
                               const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    float scale;
    memcpy(&scale, dst->op_params, sizeof(float));

    scale_f32_sycl(src0_dd, dst_dd, scale, ggml_nelements(src0), main_stream);
    /*
    DPCT1010:87: SYCL uses exceptions to report errors and does not use the
    error codes. The call was replaced with 0. You need to rewrite this code.
    */
    SYCL_CHECK(0);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

inline void ggml_sycl_op_clamp(const ggml_tensor *src0, const ggml_tensor *src1,
                               ggml_tensor *dst, const float *src0_dd,
                               const float *src1_dd, float *dst_dd,
                               const dpct::queue_ptr &main_stream) {

    GGML_ASSERT(src0->type == GGML_TYPE_F32);
    GGML_ASSERT( dst->type == GGML_TYPE_F32);

    float min;
    float max;
    memcpy(&min, dst->op_params, sizeof(float));
    memcpy(&max, (float *) dst->op_params + 1, sizeof(float));

    clamp_f32_sycl(src0_dd, dst_dd, min, max, ggml_nelements(src0), main_stream);
    /*
    DPCT1010:88: SYCL uses exceptions to report errors and does not use the
    error codes. The call was replaced with 0. You need to rewrite this code.
    */
    SYCL_CHECK(0);

    (void) src1;
    (void) dst;
    (void) src1_dd;
}

static void ggml_sycl_op_flatten(const ggml_tensor *src0,
                                 const ggml_tensor *src1, ggml_tensor *dst,
                                 const ggml_sycl_op_flatten_t op) try {
    const int64_t nrows0 = ggml_nrows(src0);

    const bool use_src1 = src1 != nullptr;
    const int64_t nrows1 = use_src1 ? ggml_nrows(src1) : 1;

    GGML_ASSERT(!use_src1 || src1->backend != GGML_BACKEND_TYPE_GPU_SPLIT);
    GGML_ASSERT(              dst->backend != GGML_BACKEND_TYPE_GPU_SPLIT);

    ggml_tensor_extra_gpu * src0_extra =            (ggml_tensor_extra_gpu *) src0->extra;
    ggml_tensor_extra_gpu * src1_extra = use_src1 ? (ggml_tensor_extra_gpu *) src1->extra : nullptr;
    ggml_tensor_extra_gpu * dst_extra  =            (ggml_tensor_extra_gpu *)  dst->extra;

    const bool src0_on_device =             src0->backend == GGML_BACKEND_TYPE_GPU || src0->backend == GGML_BACKEND_TYPE_GPU_SPLIT;
    const bool src1_on_device = use_src1 && src1->backend == GGML_BACKEND_TYPE_GPU;
    const bool  dst_on_device =              dst->backend == GGML_BACKEND_TYPE_GPU;

    // dd = data device
    float * src0_ddf = nullptr;
    float * src1_ddf = nullptr;
    float *  dst_ddf = nullptr;

    sycl_pool_alloc<float> src0_f;
    sycl_pool_alloc<float> src1_f;
    sycl_pool_alloc<float>  dst_f;

    ggml_sycl_set_device(g_main_device);
    dpct::queue_ptr main_stream = g_syclStreams[g_main_device][0];
    // GGML_SYCL_DEBUG("g_main_device=%d, main_stream=%p src0_on_device=%d, src1_on_device=%d, dst_on_device=%d\n",
        // g_main_device, main_stream, src0_on_device, src1_on_device, dst_on_device);

    if (src0_on_device) {
        src0_ddf = (float *) src0_extra->data_device[g_main_device];
    } else {
        src0_ddf = src0_f.alloc(ggml_nelements(src0));
        // GGML_SYCL_DEBUG("before ggml_sycl_cpy_tensor_2d src0_ddf=%p, src0=%p\n", src0_ddf, src0);
        SYCL_CHECK(ggml_sycl_cpy_tensor_2d(src0_ddf, src0, 0, 0, 0, nrows0, main_stream));
    }

    if (use_src1) {
        if (src1_on_device) {
            src1_ddf = (float *) src1_extra->data_device[g_main_device];
        } else {
            src1_ddf = src1_f.alloc(ggml_nelements(src1));
            SYCL_CHECK(ggml_sycl_cpy_tensor_2d(src1_ddf, src1, 0, 0, 0, nrows1, main_stream));
        }
    }
    if (dst_on_device) {
        dst_ddf = (float *) dst_extra->data_device[g_main_device];
    } else {
        dst_ddf = dst_f.alloc(ggml_nelements(dst));
    }

    // GGML_SYCL_DEBUG("op src0=%p, src1=%p, dst=%p, src0_ddf=%p, src1_ddf=%p, dst_ddf=%p, main_stream=%p\n",
        // src0, src1, dst, src0_ddf, src1_ddf, dst_ddf, main_stream);
    // do the computation
    op(src0, src1, dst, src0_ddf, src1_ddf, dst_ddf, main_stream);
    /*
    DPCT1010:89: SYCL uses exceptions to report errors and does not use the
    error codes. The call was replaced with 0. You need to rewrite this code.
    */
    SYCL_CHECK(0);

    // copy dst to host if necessary
    if (!dst_on_device) {
        SYCL_CHECK(CHECK_TRY_ERROR(
            main_stream->memcpy(dst->data, dst_ddf, ggml_nbytes(dst)).wait()));
    }

    if (dst->backend == GGML_BACKEND_TYPE_CPU) {
        SYCL_CHECK(CHECK_TRY_ERROR(
            dpct::get_current_device().queues_wait_and_throw()));
    }
    // print_ggml_tensor("tensor", dst);
}
catch (sycl::exception const &exc) {

  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_sycl_set_peer_access(const int n_tokens) {
    static bool peer_access_enabled = false;

    const bool enable_peer_access = n_tokens <= GGML_SYCL_PEER_MAX_BATCH_SIZE;

    if (peer_access_enabled == enable_peer_access) {
        return;
    }

#ifdef NDEBUG
    for (int i = 0; i < g_device_count; ++i) {
        SYCL_CHECK(ggml_sycl_set_device(i));
        // SYCL_CHECK(syclDeviceSynchronize());
    }

    for (int i = 0; i < g_device_count; ++i) {
        SYCL_CHECK(ggml_sycl_set_device(i));

        for (int id_other = 0; id_other < g_device_count; ++id_other) {
            if (i == id_other) {
                continue;
            }
            if (i != g_main_device && id_other != g_main_device) {
                continue;
            }

            // int can_access_peer;
            // SYCL_CHECK(syclDeviceCanAccessPeer(&can_access_peer, id, id_other));
            // if (can_access_peer) {
            //     if (enable_peer_access) {
            //         SYCL_CHECK(syclDeviceEnablePeerAccess(id_other, 0));
            //     } else {
            //         SYCL_CHECK(syclDeviceDisablePeerAccess(id_other));
            //     }
            // }
        }
    }
#endif // NDEBUG

    peer_access_enabled = enable_peer_access;
}

struct ggml_backend_sycl_split_buffer_type_context {
    std::array<float, GGML_SYCL_MAX_DEVICES> tensor_split;
};

static void ggml_sycl_op_mul_mat(const ggml_tensor *src0,
                                 const ggml_tensor *src1, ggml_tensor *dst,
                                 ggml_sycl_op_mul_mat_t op,
                                 const bool convert_src1_to_q8_1) try {

    GGML_TENSOR_LOCALS(int64_t, ne0, src0, ne);

    GGML_TENSOR_LOCALS(int64_t, ne1, src1, ne);
    const int64_t nrows1 = ggml_nrows(src1);

    GGML_ASSERT(ne03 == ne13);

    const int64_t ne0 = dst->ne[0];
    const int64_t ne1 = dst->ne[1];

    const int nb2 = dst->nb[2];
    const int nb3 = dst->nb[3];

    GGML_ASSERT(dst->backend != GGML_BACKEND_TYPE_GPU_SPLIT);
    GGML_ASSERT(src1->backend != GGML_BACKEND_TYPE_GPU_SPLIT);
    GGML_ASSERT(src1->type == GGML_TYPE_F32 || (src1->ne[2] == 1 && src1->ne[3] == 1));

    GGML_ASSERT(ne12 >= ne02 && ne12 % ne02 == 0);

    const int64_t i02_divisor = ne12 / ne02;

    const size_t src0_ts = ggml_type_size(src0->type);
    const size_t src0_bs = ggml_blck_size(src0->type);
    const size_t q8_1_ts = sizeof(block_q8_1);
    const size_t q8_1_bs = QK8_1;

    ggml_tensor_extra_gpu * src0_extra = (ggml_tensor_extra_gpu *) src0->extra;
    ggml_tensor_extra_gpu * src1_extra = (ggml_tensor_extra_gpu *) src1->extra;
    ggml_tensor_extra_gpu *  dst_extra = (ggml_tensor_extra_gpu *)  dst->extra;

    const bool src0_on_device = src0->backend == GGML_BACKEND_TYPE_GPU || src0->backend == GGML_BACKEND_TYPE_GPU_SPLIT;
    const bool src0_is_contiguous = ggml_is_contiguous(src0);
    const bool src1_is_contiguous = ggml_is_contiguous(src1);

    int64_t src1_padded_col_size = GGML_PAD(ne10, MATRIX_ROW_PADDING);

    const bool split = src0->backend == GGML_BACKEND_TYPE_GPU_SPLIT;
    GGML_ASSERT(!(split && ne02 > 1));
    GGML_ASSERT(!(split && ne03 > 1));
    GGML_ASSERT(!(split && ne02 < ne12));

    std::array<float, GGML_SYCL_MAX_DEVICES> tensor_split;
    if (split) {
        // TODO: check that src0->buffer->buft is a split buffer type, replace GGML_BACKEND_TYPE_GPU_SPLIT check
        // GGML_ASSERT(src0->buffer != nullptr && src0->buffer->buft == ...);
        ggml_backend_sycl_split_buffer_type_context * buft_ctx = (ggml_backend_sycl_split_buffer_type_context *) src0->buffer->buft->context;
        tensor_split = buft_ctx->tensor_split;
    }

    struct dev_data {
        sycl_pool_alloc<char> src0_dd_alloc;
        sycl_pool_alloc<float> src1_ddf_alloc;
        sycl_pool_alloc<char> src1_ddq_alloc;
        sycl_pool_alloc<float> dst_dd_alloc;

        char *src0_dd = nullptr;
        float *src1_ddf = nullptr; // float
        char *src1_ddq = nullptr;  // q8_1
        float *dst_dd = nullptr;

        int64_t row_low;
        int64_t row_high;
    };

    dev_data dev[GGML_SYCL_MAX_DEVICES];

    int used_devices = 0;
    dpct::queue_ptr main_stream = g_syclStreams[g_main_device][0];

    for (int i = 0; i < g_device_count; ++i) {
        // by default, use all rows
        dev[i].row_low  = 0;
        dev[i].row_high = ne01;

        // for multi GPU, get the row boundaries from tensor split
        // and round to mul_mat_q tile sizes
        if (split) {
            const int64_t rounding = get_row_rounding(src0->type, tensor_split);

            if (i != 0) {
                dev[i].row_low  = ne01*tensor_split[i];
                if (dev[i].row_low < ne01) {
                    dev[i].row_low -= dev[i].row_low % rounding;
                }
            }

            if (i != g_device_count - 1) {
                dev[i].row_high  = ne01*tensor_split[i + 1];
                if (dev[i].row_high < ne01) {
                    dev[i].row_high -= dev[i].row_high % rounding;
                }
            }
        }
    }

    for (int i = 0; i < g_device_count; ++i) {
        if ((!split && i != g_main_device) || dev[i].row_low == dev[i].row_high) {
            continue;
        }

        used_devices++;

        const bool src1_on_device = src1->backend == GGML_BACKEND_TYPE_GPU && i == g_main_device;
        const bool  dst_on_device =  dst->backend == GGML_BACKEND_TYPE_GPU && i == g_main_device;

        ggml_sycl_set_device(i);
        dpct::queue_ptr stream = g_syclStreams[i][0];

        if (src0_on_device && src0_is_contiguous) {
            dev[i].src0_dd = (char *) src0_extra->data_device[i];
        } else {
            dev[i].src0_dd = dev[i].src0_dd_alloc.alloc(ggml_nbytes(src0));
        }

        if (src1_on_device && src1_is_contiguous) {
            dev[i].src1_ddf = (float *) src1_extra->data_device[i];
        } else {
            dev[i].src1_ddf = dev[i].src1_ddf_alloc.alloc(ggml_nelements(src1));
        }

        if (convert_src1_to_q8_1) {
            dev[i].src1_ddq = dev[i].src1_ddq_alloc.alloc(nrows1*src1_padded_col_size*q8_1_ts/q8_1_bs);

            if (src1_on_device && src1_is_contiguous) {
                quantize_row_q8_1_sycl(dev[i].src1_ddf, dev[i].src1_ddq, ne10, nrows1, src1_padded_col_size, stream);
                /*
                DPCT1010:90: SYCL uses exceptions to report errors and does not
                use the error codes. The call was replaced with 0. You need to
                rewrite this code.
                */
                SYCL_CHECK(0);
            }
        }

        if (dst_on_device) {
            dev[i].dst_dd = (float *) dst_extra->data_device[i];
        } else {
            const size_t size_dst_ddf = split ? (dev[i].row_high - dev[i].row_low)*ne1 : ggml_nelements(dst);
            dev[i].dst_dd = dev[i].dst_dd_alloc.alloc(size_dst_ddf);
        }
    }

    // if multiple devices are used they need to wait for the main device
    // here an event is recorded that signals that the main device has finished calculating the input data
    if (split && used_devices > 1) {
        ggml_sycl_set_device(g_main_device);
        /*
        DPCT1024:91: The original code returned the error code that was further
        consumed by the program logic. This original code was replaced with 0.
        You may need to rewrite the program logic consuming the error code.
        */
        SYCL_CHECK(CHECK_TRY_ERROR(
            *src0_extra->events[g_main_device][0] =
                g_syclStreams[g_main_device][0]->ext_oneapi_submit_barrier()));
    }

    const int64_t src1_col_stride = split && used_devices > 1 ? MUL_MAT_SRC1_COL_STRIDE : ne11;
    for (int64_t src1_col_0 = 0; src1_col_0 < ne11; src1_col_0 += src1_col_stride) {
        const int64_t is = split ? (src1_col_0/src1_col_stride) % MAX_STREAMS : 0;
        const int64_t src1_ncols = src1_col_0 + src1_col_stride > ne11 ? ne11 - src1_col_0 : src1_col_stride;

        for (int i = 0; i < g_device_count; ++i) {
            if ((!split && i != g_main_device) || dev[i].row_low == dev[i].row_high) {
                continue;
            }

            const bool src1_on_device = src1->backend == GGML_BACKEND_TYPE_GPU && i == g_main_device;
            const bool  dst_on_device =  dst->backend == GGML_BACKEND_TYPE_GPU && i == g_main_device;
            const int64_t row_diff = dev[i].row_high - dev[i].row_low;

            ggml_sycl_set_device(i);
            dpct::queue_ptr stream = g_syclStreams[i][is];

            // wait for main GPU data if necessary
            if (split && (i != g_main_device || is != 0)) {
                /*
                DPCT1009:163: SYCL uses exceptions to report errors and does not
                use the error codes. The original code was commented out and a
                warning string was inserted. You need to rewrite this code.
                */
                SYCL_CHECK(CHECK_TRY_ERROR(stream->ext_oneapi_submit_barrier(
                    {*src0_extra->events[g_main_device][0]})));
            }

            for (int64_t i0 = 0; i0 < ne13*ne12; ++i0) {
                const int64_t i03 = i0 / ne12;
                const int64_t i02 = i0 % ne12;

                const size_t src1_ddq_i_offset = (i0*ne11 + src1_col_0) * src1_padded_col_size*q8_1_ts/q8_1_bs;

                // for split tensors the data begins at i0 == i0_offset_low
                char  *  src0_dd_i =  dev[i].src0_dd + (i0/i02_divisor) * (ne01*ne00*src0_ts)/src0_bs;
                float * src1_ddf_i = dev[i].src1_ddf + (i0*ne11 + src1_col_0) * ne10;
                char  * src1_ddq_i = dev[i].src1_ddq +  src1_ddq_i_offset;
                float *   dst_dd_i =   dev[i].dst_dd + (i0*ne1  + src1_col_0) * (dst_on_device ? ne0 : row_diff);

                // the main device memory buffer can be on VRAM scratch, with space for all partial results
                // in that case an offset on dst_ddf_i is needed
                if (dst->backend == GGML_BACKEND_TYPE_GPU && i == g_main_device) {
                    dst_dd_i += dev[i].row_low; // offset is 0 if no tensor split
                }

                // copy src0, src1 to device if necessary
                if (src1->backend == GGML_BACKEND_TYPE_GPU && src1_is_contiguous) {
                    if (i != g_main_device) {
                        if (convert_src1_to_q8_1) {
                            char * src1_ddq_i_source = dev[g_main_device].src1_ddq + src1_ddq_i_offset;
                          SYCL_CHECK(CHECK_TRY_ERROR(stream->memcpy(
                                src1_ddq_i, src1_ddq_i_source,
                                src1_ncols * src1_padded_col_size * q8_1_ts /
                                    q8_1_bs).wait()));
                        } else {

                            float * src1_ddf_i_source = (float *) src1_extra->data_device[g_main_device];
                            src1_ddf_i_source += (i0*ne11 + src1_col_0) * ne10;

                            SYCL_CHECK(CHECK_TRY_ERROR(dev2dev_memcpy(*stream, *main_stream,
                                src1_ddf_i, src1_ddf_i_source,
                                src1_ncols * ne10 * sizeof(float))));
                        }
                    }
                } else if (src1->backend == GGML_BACKEND_TYPE_CPU || (src1_on_device && !src1_is_contiguous)) {
                    SYCL_CHECK(ggml_sycl_cpy_tensor_2d(
                                   src1_ddf_i, src1, i03, i02, src1_col_0, src1_col_0+src1_ncols, stream));
                } else {
                    GGML_ASSERT(false);
                }

                if (convert_src1_to_q8_1 && (src1->backend == GGML_BACKEND_TYPE_CPU || !src1_is_contiguous)) {
                    quantize_row_q8_1_sycl(src1_ddf_i, src1_ddq_i, ne10, src1_ncols, src1_padded_col_size, stream);
                    /*
                    DPCT1010:92: SYCL uses exceptions to report errors and does
                    not use the error codes. The call was replaced with 0. You
                    need to rewrite this code.
                    */
                    SYCL_CHECK(0);
                }

                if (src1_col_0 == 0 && (!src0_on_device || !src0_is_contiguous) && i02 % i02_divisor == 0) {
                    SYCL_CHECK(ggml_sycl_cpy_tensor_2d(src0_dd_i, src0, i03, i02/i02_divisor, dev[i].row_low, dev[i].row_high, stream));
                }
                if (src1->type == GGML_TYPE_F16) {
                    src1_padded_col_size = (i0 * ne11 + src1_col_0) * ne10;
                }
                // do the computation
                op(src0, src1, dst, src0_dd_i, src1_ddf_i, src1_ddq_i, dst_dd_i,
                    dev[i].row_low, dev[i].row_high, src1_ncols, src1_padded_col_size, stream);
                /*
                DPCT1010:93: SYCL uses exceptions to report errors and does not
                use the error codes. The call was replaced with 0. You need to
                rewrite this code.
                */
                SYCL_CHECK(0);

                // copy dst to host or other device if necessary
                if (!dst_on_device) {
                    void * dst_off_device;
                    dpct::memcpy_direction kind;
                    if (dst->backend == GGML_BACKEND_TYPE_CPU) {
                        dst_off_device = dst->data;
                        kind = dpct::device_to_host;
                    } else if (dst->backend == GGML_BACKEND_TYPE_GPU) {
                        dst_off_device = dst_extra->data_device[g_main_device];
                        kind = dpct::device_to_device;
                    } else {
                        GGML_ASSERT(false);
                    }
                    if (split) {
                        // src0 = weight matrix is saved as a transposed matrix for better memory layout.
                        // dst is NOT transposed.
                        // The outputs of matrix matrix multiplications can therefore NOT simply be concatenated for >1 GPU.
                        // Instead they need to be copied to the correct slice in ne0 = dst row index.
                        // If dst is a vector with ne0 == 1 then you don't have to do this but it still produces correct results.
                        float * dhf_dst_i = (float *) ((char *) dst_off_device + i02*nb2 + i03*nb3);
                        GGML_ASSERT(dst->nb[1] == ne0*sizeof(float));
                        dhf_dst_i += src1_col_0*ne0 + dev[i].row_low;

                        //todo, dirty solution. Need be updated when device2device memcpy() is supported.
                        if (kind == dpct::device_to_device) {
                            size_t dst_size = ggml_nbytes_pad(dst);
                            float *host_buf = (float *)malloc(dst_size);
                            SYCL_CHECK(CHECK_TRY_ERROR(dpct::async_dpct_memcpy(
                                host_buf, ne0 * sizeof(float), dst_dd_i,
                                row_diff * sizeof(float), row_diff * sizeof(float),
                                src1_ncols, dpct::device_to_host, *stream)));
                            dpct::dev_mgr::instance().get_device(g_sycl_gpu_mgr->gpus[i]).queues_wait_and_throw();
                            SYCL_CHECK(CHECK_TRY_ERROR(dpct::async_dpct_memcpy(
                                dhf_dst_i, ne0 * sizeof(float), host_buf,
                                row_diff * sizeof(float), row_diff * sizeof(float),
                                src1_ncols, dpct::host_to_device, *main_stream)));
                            dpct::dev_mgr::instance().get_device(g_sycl_gpu_mgr->gpus[g_main_device]).queues_wait_and_throw();
                            free(host_buf);
                        } else {
                            SYCL_CHECK(CHECK_TRY_ERROR(dpct::async_dpct_memcpy(
                                dhf_dst_i, ne0 * sizeof(float), dst_dd_i,
                                row_diff * sizeof(float), row_diff * sizeof(float),
                                src1_ncols, kind, *stream)));
                        }
                    } else {
                        float * dhf_dst_i = (float *) ((char *) dst_off_device + i02*nb2 + i03*nb3);
                        GGML_ASSERT(dst->nb[1] == ne0*sizeof(float));
                        dhf_dst_i += src1_col_0*ne0;
                        SYCL_CHECK(CHECK_TRY_ERROR(
                            stream->memcpy(dhf_dst_i, dst_dd_i,
                                           src1_ncols * ne0 * sizeof(float)).wait()));
                    }
                }

                // add event for the main device to wait on until other device is done
                if (split && (i != g_main_device || is != 0)) {
                    /*
                    DPCT1024:94: The original code returned the error code that
                    was further consumed by the program logic. This original
                    code was replaced with 0. You may need to rewrite the
                    program logic consuming the error code.
                    */
                    SYCL_CHECK(CHECK_TRY_ERROR(
                        *src0_extra->events[i][is] =
                            stream->ext_oneapi_submit_barrier()));
                }
            }
        }
    }

    // main device waits for all other devices to be finished
    if (split && g_device_count > 1) {
        int64_t is_max = (ne11 + MUL_MAT_SRC1_COL_STRIDE - 1) / MUL_MAT_SRC1_COL_STRIDE;
        is_max = is_max <= MAX_STREAMS ? is_max : MAX_STREAMS;

        ggml_sycl_set_device(g_main_device);
        for (int i = 0; i < g_device_count; ++i) {
            if (dev[i].row_low == dev[i].row_high) {
                continue;
            }
            for (int64_t is = 0; is < is_max; ++is) {
                SYCL_CHECK(CHECK_TRY_ERROR(
                    g_syclStreams[g_main_device][0]->ext_oneapi_submit_barrier(
                        {*src0_extra->events[i][is]})));
            }
        }
    }

    if (dst->backend == GGML_BACKEND_TYPE_CPU) {
        SYCL_CHECK(ggml_sycl_set_device(g_main_device));
        SYCL_CHECK(CHECK_TRY_ERROR(
            dpct::get_current_device().queues_wait_and_throw()));
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}


static void ggml_sycl_repeat(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_repeat);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_get_rows(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_get_rows);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_add(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_add);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_acc(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_acc);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_mul(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_mul);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_div(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_div);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_gelu(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_gelu);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_silu(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_silu);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_gelu_quick(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_gelu_quick);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_tanh(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_tanh);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_relu(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_relu);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_hardsigmoid(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_hardsigmoid);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_hardswish(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_hardswish);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_leaky_relu(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_leaky_relu);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_sqr(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_sqr);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_norm(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_norm);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_group_norm(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_group_norm);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_concat(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_concat);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_upscale(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_upscale);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

static void ggml_sycl_pad(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_pad);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}


static void ggml_sycl_rms_norm(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_SYCL_DEBUG("call %s\n", __func__);
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_rms_norm);
    GGML_SYCL_DEBUG("call %s done\n", __func__);
}

bool ggml_sycl_can_mul_mat(const struct ggml_tensor * src0, const struct ggml_tensor * src1, struct ggml_tensor * dst) {
    if (!g_sycl_loaded) return false;

    const int64_t ne10 = src1->ne[0];

    const int64_t ne0 = dst->ne[0];
    const int64_t ne1 = dst->ne[1];

    // TODO: find the optimal values for these
    return (src0->type == GGML_TYPE_F32 || src0->type == GGML_TYPE_F16 || ggml_is_quantized(src0->type)) &&
            src1->type == GGML_TYPE_F32 &&
             dst->type == GGML_TYPE_F32 &&
            (ne0 >= 32 && ne1 >= 32 && ne10 >= 32);
}

static void ggml_sycl_mul_mat_vec_p021(const ggml_tensor *src0,
                                       const ggml_tensor *src1,
                                       ggml_tensor *dst) try {
    GGML_ASSERT(ggml_is_permuted(src0) && ggml_is_permuted(src1));
    GGML_ASSERT(src0->backend != GGML_BACKEND_TYPE_GPU_SPLIT);
    GGML_ASSERT(src0->nb[0] <= src0->nb[1] && src0->nb[2] <= src0->nb[3]); // 0213 permutation
    GGML_ASSERT(src1->nb[0] <= src1->nb[1] && src1->nb[2] <= src1->nb[3]); // 0213 permutation
    GGML_ASSERT(src0->type == GGML_TYPE_F16);
    GGML_ASSERT(src1->type == GGML_TYPE_F32);

    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];
    const int64_t ne02 = src0->ne[2];

    const int64_t ne12 = src1->ne[2];

    SYCL_CHECK(ggml_sycl_set_device(g_main_device));
    dpct::queue_ptr main_stream = g_syclStreams[g_main_device][0];

    ggml_tensor_extra_gpu * src0_extra = (ggml_tensor_extra_gpu *) src0->extra;
    void * src0_ddq = src0_extra->data_device[g_main_device];

    ggml_tensor_extra_gpu * src1_extra = (ggml_tensor_extra_gpu *) src1->extra;
    float * src1_ddf = (float *) src1_extra->data_device[g_main_device];

    ggml_tensor_extra_gpu * dst_extra = (ggml_tensor_extra_gpu *) dst->extra;
    float * dst_ddf = (float *) dst_extra->data_device[g_main_device];

    ggml_mul_mat_p021_f16_f32_sycl(src0_ddq, src1_ddf, dst_ddf, ne00, ne01, ne02, ne12, main_stream);
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_sycl_mul_mat_vec_nc(const ggml_tensor *src0,
                                     const ggml_tensor *src1,
                                     ggml_tensor *dst) try {
    GGML_ASSERT(!ggml_is_transposed(src0));
    GGML_ASSERT(!ggml_is_transposed(src1));
    GGML_ASSERT(!ggml_is_permuted(src0));
    GGML_ASSERT(src0->backend != GGML_BACKEND_TYPE_GPU_SPLIT);
    GGML_ASSERT(src0->type == GGML_TYPE_F16);
    GGML_ASSERT(src1->type == GGML_TYPE_F32);

    const int64_t ne00 = src0->ne[0];
    const int64_t ne01 = src0->ne[1];
    const int64_t ne02 = src0->ne[2];

    const int64_t nb01 = src0->nb[1];
    const int64_t nb02 = src0->nb[2];

    const int64_t ne12 = src1->ne[2];

    SYCL_CHECK(ggml_sycl_set_device(g_main_device));
    dpct::queue_ptr main_stream = g_syclStreams[g_main_device][0];

    ggml_tensor_extra_gpu * src0_extra = (ggml_tensor_extra_gpu *) src0->extra;
    void * src0_ddq = src0_extra->data_device[g_main_device];

    ggml_tensor_extra_gpu * src1_extra = (ggml_tensor_extra_gpu *) src1->extra;
    float * src1_ddf = (float *) src1_extra->data_device[g_main_device];

    ggml_tensor_extra_gpu * dst_extra = (ggml_tensor_extra_gpu *) dst->extra;
    float * dst_ddf = (float *) dst_extra->data_device[g_main_device];

    const int64_t row_stride_x = nb01 / sizeof(sycl::half);
    const int64_t channel_stride_x = nb02 / sizeof(sycl::half);

    ggml_mul_mat_vec_nc_f16_f32_sycl(src0_ddq, src1_ddf, dst_ddf, ne00, ne01, row_stride_x, ne02, ne12, channel_stride_x, main_stream);
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void k_compute_batched_ptrs(const sycl::half *src0_as_f16,
                                   const sycl::half *src1_as_f16, char *dst,
                                   const void **ptrs_src, void **ptrs_dst,
                                   int64_t ne12, int64_t ne13, int64_t ne23,
                                   size_t nb02, size_t nb03, size_t nb12,
                                   size_t nb13, size_t nbd2, size_t nbd3,
                                   int64_t r2, int64_t r3,
                                   const sycl::nd_item<3> &item_ct1) {
    int64_t i13 = item_ct1.get_group(2) * item_ct1.get_local_range(2) +
                  item_ct1.get_local_id(2);
    int64_t i12 = item_ct1.get_group(1) * item_ct1.get_local_range(1) +
                  item_ct1.get_local_id(1);

    if (i13 >= ne13 || i12 >= ne12) {
        return;
    }

    int64_t i03 = i13 / r3;
    int64_t i02 = i12 / r2;

    ptrs_src[0*ne23 + i12 + i13*ne12] = (const char *) src0_as_f16 + i02*nb02 + i03*nb03;
    ptrs_src[1*ne23 + i12 + i13*ne12] = (const char *) src1_as_f16 + i12*nb12 + i13*nb13;
    ptrs_dst[0*ne23 + i12 + i13*ne12] = (      char *)         dst + i12*nbd2 + i13*nbd3;
}

static void ggml_sycl_mul_mat_batched_sycl(const ggml_tensor *src0,
                                             const ggml_tensor *src1,
                                             ggml_tensor *dst) try {
    GGML_ASSERT(!ggml_is_transposed(src0));
    GGML_ASSERT(!ggml_is_transposed(src1));
    GGML_ASSERT(src0->backend != GGML_BACKEND_TYPE_GPU_SPLIT);
    GGML_ASSERT(src0->type == GGML_TYPE_F16);

    GGML_TENSOR_BINARY_OP_LOCALS

    const int64_t ne_dst = ggml_nelements(dst);

    SYCL_CHECK(ggml_sycl_set_device(g_main_device));
    dpct::queue_ptr main_stream = g_syclStreams[g_main_device][0];

    bool no_mixed_dtypes = main_stream->get_backend() == sycl::backend::ext_oneapi_cuda ||
                           main_stream->get_backend() == sycl::backend::ext_oneapi_hip;

    SYCL_CHECK(
        CHECK_TRY_ERROR(g_sycl_handles[g_main_device] = main_stream));

    ggml_tensor_extra_gpu * src0_extra = (ggml_tensor_extra_gpu *) src0->extra;
    void * src0_ddq = src0_extra->data_device[g_main_device];
    sycl::half *src0_as_f16 = (sycl::half *)src0_ddq;

    ggml_tensor_extra_gpu * src1_extra = (ggml_tensor_extra_gpu *) src1->extra;
    float * src1_ddf = (float *) src1_extra->data_device[g_main_device];

    ggml_tensor_extra_gpu * dst_extra = (ggml_tensor_extra_gpu *) dst->extra;
    float * dst_ddf = (float *) dst_extra->data_device[g_main_device];

    // convert src1 to fp16
    sycl_pool_alloc<sycl::half> src1_f16_alloc;
    if (src1->type != GGML_TYPE_F16) {
        const to_fp16_sycl_t to_fp16_sycl = ggml_get_to_fp16_sycl(src1->type);
        const int64_t ne_src1 = ggml_nelements(src1);
        src1_f16_alloc.alloc(ne_src1);
        GGML_ASSERT(to_fp16_sycl != nullptr);
        to_fp16_sycl(src1_ddf, src1_f16_alloc.get(), ne_src1, main_stream);
    }
    sycl::half *src1_f16 = src1->type == GGML_TYPE_F16 ? (sycl::half *)src1_ddf
                                                       : src1_f16_alloc.get();

    sycl_pool_alloc<sycl::half> dst_f16;
    char * dst_t;

    dpct::library_data_t cu_compute_type = dpct::library_data_t::real_float;
    dpct::library_data_t cu_data_type = dpct::library_data_t::real_float;
    if (no_mixed_dtypes) {
        cu_compute_type = dpct::library_data_t::real_half;
        cu_data_type = dpct::library_data_t::real_half;
    }

    // dst strides
    size_t nbd2 = dst->nb[2];
    size_t nbd3 = dst->nb[3];

    const float alpha_f32 = 1.0f;
    const float beta_f32 = 0.0f;

    const sycl::half alpha_f16 = 1.0f;
    const sycl::half beta_f16 = 0.0f;

    const void * alpha = &alpha_f32;
    const void * beta  = &beta_f32;
    if (no_mixed_dtypes) {
        alpha = &alpha_f16;
        beta  = &beta_f16;
    }

    // TODO: Renable (dst->op_params[0] =! GGML_PREC_DEFAULT) pathway
    // when oneMKL open source supports half, half, float, float: datatypes

    dst_t = (char *) dst_ddf;
    if (no_mixed_dtypes) {
        dst_t = (char *) dst_f16.alloc(ne_dst);

        nbd2 /= sizeof(float) / sizeof(sycl::half);
        nbd3 /= sizeof(float) / sizeof(sycl::half);
    }

    GGML_ASSERT(ne12 % ne02 == 0);
    GGML_ASSERT(ne13 % ne03 == 0);

    // broadcast factors
    const int64_t r2 = ne12/ne02;
    const int64_t r3 = ne13/ne03;

#if 0
    // use syclGemmEx
    {
        for (int i13 = 0; i13 < ne13; ++i13) {
            for (int i12 = 0; i12 < ne12; ++i12) {
                int i03 = i13 / r3;
                int i02 = i12 / r2;

                SYCL_CHECK(
                        syclGemmEx(g_sycl_handles[g_main_device], CUBLAS_OP_T, CUBLAS_OP_N,
                            ne01, ne11, ne10,
                            alpha, (const char *) src0_as_f16 + i02*src0->nb[2]   + i03*src0->nb[3]  , SYCL_R_16F,   nb01/sizeof(half),
                                   (const char *) src1_as_f16 + i12*src1->nb[2]/2 + i13*src1->nb[3]/2, SYCL_R_16F,   nb11/sizeof(float),
                            beta,  (      char *)       dst_t + i12*nbd2          + i13*nbd3,          cu_data_type, ne01,
                            cu_compute_type,
                            CUBLAS_GEMM_DEFAULT_TENSOR_OP));
            }
        }
    }
#else
    if (r2 == 1 && r3 == 1 && src0->nb[2]*src0->ne[2] == src0->nb[3] && src1->nb[2]*src1->ne[2] == src1->nb[3]) {
        // there is no broadcast and src0, src1 are contiguous across dims 2, 3
        SYCL_CHECK(CHECK_TRY_ERROR(dpct::gemm_batch(
            *g_sycl_handles[g_main_device], oneapi::mkl::transpose::trans,
            oneapi::mkl::transpose::nontrans, ne01, ne11, ne10, alpha,
            (const char *)src0_as_f16, dpct::library_data_t::real_half,
            nb01 / nb00, nb02 / nb00,
            (const char *)src1_f16, dpct::library_data_t::real_half,
            nb11 / nb10, nb12 / nb10, beta,
            (char *)dst_t, cu_data_type, ne01, nb2 / nb0,
            ne12 * ne13, cu_compute_type)));
        g_sycl_handles[g_main_device]->wait();
    } else {
        const int ne23 = ne12*ne13;

        sycl_pool_alloc<const void *> ptrs_src(2*ne23);
        sycl_pool_alloc<      void *> ptrs_dst(1*ne23);

        sycl::range<3> block_dims(1, ne12, ne13);
        /*
        DPCT1049:47: The work-group size passed to the SYCL kernel may exceed
        the limit. To get the device limit, query
        info::device::max_work_group_size. Adjust the work-group size if needed.
        */
        {
            dpct::has_capability_or_fail(main_stream->get_device(),
                                         {sycl::aspect::fp16});

            main_stream->submit([&](sycl::handler &cgh) {
                const void **ptrs_src_get = ptrs_src.get();
                void **ptrs_dst_get = ptrs_dst.get();
                size_t nb12_scaled = src1->type == GGML_TYPE_F16 ? nb12 : nb12 / 2;
                size_t nb13_scaled = src1->type == GGML_TYPE_F16 ? nb13 : nb13 / 2;
                cgh.parallel_for(sycl::nd_range<3>(block_dims, block_dims),
                                 [=](sycl::nd_item<3> item_ct1) {
                                     k_compute_batched_ptrs(
                                         src0_as_f16, src1_f16,
                                         dst_t, ptrs_src_get,
                                         ptrs_dst_get, ne12, ne13, ne23,
                                         nb02, nb03, nb12_scaled, nb13_scaled,
                                         nbd2, nbd3, r2, r3, item_ct1);
                                 });
            }).wait();
        }
        SYCL_CHECK(CHECK_TRY_ERROR(dpct::gemm_batch(
            *g_sycl_handles[g_main_device], oneapi::mkl::transpose::trans,
            oneapi::mkl::transpose::nontrans, ne01, ne11, ne10, alpha,
            (const void **)(ptrs_src.get() + 0 * ne23),
            dpct::library_data_t::real_half, nb01 / nb00,
            (const void **)(ptrs_src.get() + 1 * ne23),
            dpct::library_data_t::real_half, nb11 / nb10, beta,
            (void **)(ptrs_dst.get() + 0 * ne23), cu_data_type, ne01, ne23,
            cu_compute_type)));
        g_sycl_handles[g_main_device]->wait();
    }
#endif

    if (no_mixed_dtypes) {
        const to_fp32_sycl_t to_fp32_sycl = ggml_get_to_fp32_sycl(GGML_TYPE_F16);
        to_fp32_sycl(dst_f16.get(), dst_ddf, ne_dst, main_stream);
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}


static void ggml_sycl_mul_mat(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    const bool all_on_device =
        (src0->backend == GGML_BACKEND_TYPE_GPU || src0->backend == GGML_BACKEND_TYPE_GPU_SPLIT) &&
        (src1->backend == GGML_BACKEND_TYPE_GPU) &&
        ( dst->backend == GGML_BACKEND_TYPE_GPU);

    const bool split = src0->backend == GGML_BACKEND_TYPE_GPU_SPLIT;

    int64_t min_compute_capability = INT_MAX;
    for (int i = 0; i < g_device_count; ++i) {
        if (min_compute_capability > g_device_caps[i].cc && g_tensor_split[i] < (i + 1 < g_device_count ? g_tensor_split[i + 1] : 1.0f)) {
            min_compute_capability = g_device_caps[i].cc;
        }
    }

#ifdef SYCL_USE_XMX
    const bool use_xmx = true;
#else
    const bool use_xmx = false;
#endif

    // debug helpers
    //printf("src0: %8d %8d %8d %8d\n", src0->ne[0], src0->ne[1], src0->ne[2], src0->ne[3]);
    //printf("      %8d %8d %8d %8d\n", src0->nb[0], src0->nb[1], src0->nb[2], src0->nb[3]);
    //printf("src1: %8d %8d %8d %8d\n", src1->ne[0], src1->ne[1], src1->ne[2], src1->ne[3]);
    //printf("      %8d %8d %8d %8d\n", src1->nb[0], src1->nb[1], src1->nb[2], src1->nb[3]);
    //printf("src0 is contiguous %d, transposed %d, type = %s, name = %s\n", ggml_is_contiguous(src0), ggml_is_transposed(src0), ggml_type_name(src0->type), src0->name);
    //printf("src1 is contiguous %d, transposed %d, type = %s, name = %s\n", ggml_is_contiguous(src1), ggml_is_transposed(src1), ggml_type_name(src1->type), src1->name);

    if (!split && all_on_device && !use_xmx && src0->type == GGML_TYPE_F16 && ggml_is_permuted(src0) && ggml_is_permuted(src1) && src1->ne[1] == 1) {
        // KQ single-batch
        // GGML_SYCL_DEBUG("ggml_sycl_mul_mat_vec_p021\n");
        ggml_sycl_mul_mat_vec_p021(src0, src1, dst);
    } else if (!split && all_on_device && !use_xmx && src0->type == GGML_TYPE_F16 && !ggml_is_contiguous(src0) && !ggml_is_transposed(src1) && src1->ne[1] == 1) {
        // KQV single-batch
        // GGML_SYCL_DEBUG("ggml_sycl_mul_mat_vec_nc\n");
        ggml_sycl_mul_mat_vec_nc(src0, src1, dst);
    } else if (!split && all_on_device && use_xmx && src0->type == GGML_TYPE_F16 && !ggml_is_transposed(src0) && !ggml_is_transposed(src1)) {
        // KQ + KQV multi-batch
        // GGML_SYCL_DEBUG("ggml_sycl_mul_mat_batched_sycl\n");
        ggml_sycl_mul_mat_batched_sycl(src0, src1, dst);
    } else if (src0->type == GGML_TYPE_F32) {
        // GGML_SYCL_DEBUG("ggml_sycl_op_mul_mat\n");
        ggml_sycl_op_mul_mat(src0, src1, dst, ggml_sycl_op_mul_mat_sycl, false);
    } else if (ggml_is_quantized(src0->type) || src0->type == GGML_TYPE_F16) {
        // GGML_SYCL_DEBUG("ggml_is_quantized or GGML_TYPE_F16\n");
        if (src1->ne[1] == 1 && src0->ne[0] % GGML_SYCL_DMMV_X == 0) {
#ifdef GGML_SYCL_FORCE_DMMV
            const bool use_mul_mat_vec_q = false;
#else
            const bool use_mul_mat_vec_q = min_compute_capability >= VER_4VEC && ggml_is_quantized(src0->type) && ggml_nrows(src1) == 1;
#endif // GGML_SYCL_FORCE_DMMV

            if (use_mul_mat_vec_q) {
                // NOTE: this kernel does not support ggml_nrows(src1) > 1
                // GGML_SYCL_DEBUG("ggml_sycl_mul_mat ggml_sycl_op_mul_mat_vec_q path\n");
                ggml_sycl_op_mul_mat(src0, src1, dst, ggml_sycl_op_mul_mat_vec_q, true);
            } else {
                // GGML_SYCL_DEBUG("ggml_sycl_mul_mat ggml_sycl_op_dequantize_mul_mat_vec path\n");
                ggml_sycl_op_mul_mat(src0, src1, dst, ggml_sycl_op_dequantize_mul_mat_vec, false);
            }
        } else {
            bool use_mul_mat_q = min_compute_capability >= VER_4VEC && ggml_is_quantized(src0->type);

            if (use_xmx && min_compute_capability >= VER_GEN9 && src1->ne[1] > XMX_MAX_BATCH_SIZE) {
                use_mul_mat_q = false;
            }

            if (use_mul_mat_q) {
                // GGML_SYCL_DEBUG("ggml_sycl_mul_mat ggml_sycl_op_mul_mat_q path\n");
                ggml_sycl_op_mul_mat(src0, src1, dst, ggml_sycl_op_mul_mat_q, true);
            } else {
                // GGML_SYCL_DEBUG("ggml_sycl_mul_mat ggml_sycl_op_mul_mat_sycl path\n");
                ggml_sycl_op_mul_mat(src0, src1, dst, ggml_sycl_op_mul_mat_sycl, false);
            }
        }
    } else {
        GGML_ASSERT(false);
    }
}

#if 0
template<typename ... Srcs>
static __global__ void k_compute_batched_ptrs_id(
        const void ** ptrs_src, void ** ptrs_dst,
        int ne12, int ne13,
        int ne23,
        int nb02, int nb03,
        int nb12, int nb13,
        int nb2, int nb3,
        int r2, int r3,
        ggml_type src0_type, half * src0_as_f16, int64_t src0_ne,
        const half * src1_f16, half * dst_f16,
        const int32_t * ids, const int id,
        Srcs... src0s) {

    int i = ids[id];

    half * src0_f16;
    const void * srcs_ar[] = { (const half *) src0s... };
    if (src0_type == GGML_TYPE_F16) {
        src0_f16 = (half *) srcs_ar[i];
    } else {
        src0_f16 = src0_as_f16;
        if (item_ct1.get_local_id(2) == 0 && threadIdx.y == 0) {
            const to_fp16_sycl_t to_fp16 = ggml_get_to_fp16_sycl(src0_type);
            to_fp16(srcs_ar[i], src0_f16, src0_ne, syclStreamFireAndForget);
        }
    }

    int i13 = blockIdx.x * blockDim.x + item_ct1.get_local_id(2);
    int i12 = blockIdx.y * blockDim.y + threadIdx.y;

    if (i13 >= ne13 || i12 >= ne12) {
        return;
    }

    int i03 = i13 / r3;
    int i02 = i12 / r2;

    ptrs_src[0*ne23 + i12 + i13*ne12] = (const char *) src0_f16 + i02*nb02   + i03*nb03;
    ptrs_src[1*ne23 + i12 + i13*ne12] = (const char *) src1_f16 + i12*nb12/2 + i13*nb13/2;
    ptrs_dst[0*ne23 + i12 + i13*ne12] = (      char *)  dst_f16 + i12* nb2/2 + i13* nb3/2;
}

static void ggml_sycl_mul_mat_id_sycl(ggml_tensor * dst) {
    const struct ggml_tensor * ids = dst->src[0];
    const struct ggml_tensor * src1 = dst->src[1];
    const struct ggml_tensor * src00 = dst->src[2];

    const int id = dst->op_params[0];

    GGML_ASSERT(!ggml_is_transposed(src00));
    GGML_ASSERT(!ggml_is_transposed(src1));

    GGML_ASSERT(src00->backend != GGML_BACKEND_TYPE_GPU_SPLIT);
    GGML_ASSERT(src1->type == GGML_TYPE_F32);

    GGML_TENSOR_LOCALS(int64_t, ne0, src00, ne);

    //const int64_t nb01 = src00->nb[1];
    GGML_TENSOR_LOCALS(int64_t, nb0, src00, nb);

    GGML_TENSOR_LOCALS(int64_t, ne1, src1, ne);

    GGML_TENSOR_LOCALS(int64_t, nb1, src1, nb);
    //const int64_t nb11 = src1->nb[1];

    const int64_t ne1 = ggml_nelements(src1);
    const int64_t ne  = ggml_nelements(dst);

    SYCL_CHECK(ggml_sycl_set_device(g_main_device));
    syclStream_t main_stream = g_syclStreams[g_main_device][0];

    SYCL_CHECK(syclSetStream(g_sycl_handles[g_main_device], main_stream));

    //ggml_tensor_extra_gpu * src0_extra = (ggml_tensor_extra_gpu *) src0->extra;
    //void * src0_ddq = src0_extra->data_device[g_main_device];
    //half * src0_as_f16 = (half *) src0_ddq;

    ggml_tensor_extra_gpu * src1_extra = (ggml_tensor_extra_gpu *) src1->extra;
    float * src1_ddf = (float *) src1_extra->data_device[g_main_device];

    ggml_tensor_extra_gpu * dst_extra = (ggml_tensor_extra_gpu *) dst->extra;
    float * dst_ddf = (float *) dst_extra->data_device[g_main_device];

    // convert src1 to fp16
    const to_fp16_sycl_t to_fp16_sycl = ggml_get_to_fp16_sycl(src1->type);
    GGML_ASSERT(to_fp16_sycl != nullptr);

    size_t src1_as = 0;
    half * src1_as_f16 = (half *) ggml_sycl_pool_malloc(g_main_device, ne1 * sizeof(half), &src1_as);
    to_fp16_sycl(src1_ddf, src1_as_f16, ne1, main_stream);

    size_t dst_as = 0;
    half * dst_f16 = (half *) ggml_sycl_pool_malloc(g_main_device, ne * sizeof(half), &dst_as);

    GGML_ASSERT(ne12 % ne02 == 0);
    GGML_ASSERT(ne13 % ne03 == 0);

    // broadcast factors
    const int64_t r2 = ne12/ne02;
    const int64_t r3 = ne13/ne03;

    const half alpha_f16 = 1.0f;
    const half beta_f16  = 0.0f;

    // use syclGemmBatchedEx
    const int ne23 = ne12*ne13;

    const void ** ptrs_src = nullptr;
          void ** ptrs_dst = nullptr;

    size_t ptrs_src_s = 0;
    size_t ptrs_dst_s = 0;

    ptrs_src = (const void **) ggml_sycl_pool_malloc(g_main_device, 2*ne23*sizeof(void *), &ptrs_src_s);
    ptrs_dst = (      void **) ggml_sycl_pool_malloc(g_main_device, 1*ne23*sizeof(void *), &ptrs_dst_s);

    int64_t src0_ne = ggml_nelements(src00);
    half * src0_as_f16 = nullptr;
    size_t src0_as = 0;
    if (src00->type != GGML_TYPE_F16) {
        src0_as_f16 = (half *) ggml_sycl_pool_malloc(g_main_device, src0_ne * sizeof(half), &src0_as);
    }

    static_assert(GGML_MAX_SRC == 6, "GGML_MAX_SRC == 6");
    dim3 block_dims(ne13, ne12);
    k_compute_batched_ptrs_id<<<1, block_dims, 0, main_stream>>>(
            ptrs_src, ptrs_dst,
            ne12, ne13,
            ne23,
            ne00*ne01*sizeof(half), ne00*ne01*ne02*sizeof(half),
            nb12, nb13,
            dst->nb[2], dst->nb[3],
            r2, r3,
            src00->type, src0_as_f16, src0_ne,
            src1_as_f16, dst_f16,
            (const int *)((ggml_tensor_extra_gpu *)ids->extra)->data_device[g_main_device], id,
            dst->src[2] ? (const half *)((ggml_tensor_extra_gpu *)dst->src[2]->extra)->data_device[g_main_device] : nullptr,
            dst->src[3] ? (const half *)((ggml_tensor_extra_gpu *)dst->src[3]->extra)->data_device[g_main_device] : nullptr,
            dst->src[4] ? (const half *)((ggml_tensor_extra_gpu *)dst->src[4]->extra)->data_device[g_main_device] : nullptr,
            dst->src[5] ? (const half *)((ggml_tensor_extra_gpu *)dst->src[5]->extra)->data_device[g_main_device] : nullptr
    );
    SYCL_CHECK(syclGetLastError());

    SYCL_CHECK(
    syclGemmBatchedEx(g_sycl_handles[g_main_device], CUBLAS_OP_T, CUBLAS_OP_N,
            ne01, ne11, ne10,
            &alpha_f16, (const void **) (ptrs_src + 0*ne23), SYCL_R_16F, ne00,
                        (const void **) (ptrs_src + 1*ne23), SYCL_R_16F, ne10,
            &beta_f16,  (      void **) (ptrs_dst + 0*ne23), SYCL_R_16F, ne01,
            ne23,
            CUBLAS_COMPUTE_16F,
            CUBLAS_GEMM_DEFAULT_TENSOR_OP));

    if (src0_as != 0) {
        ggml_sycl_pool_free(g_main_device, src0_as_f16, src0_as);
    }
    if (ptrs_src_s != 0) {
        ggml_sycl_pool_free(g_main_device, ptrs_src, ptrs_src_s);
    }
    if (ptrs_dst_s != 0) {
        ggml_sycl_pool_free(g_main_device, ptrs_dst, ptrs_dst_s);
    }

    const to_fp32_sycl_t to_fp32_sycl = ggml_get_to_fp32_sycl(GGML_TYPE_F16);
    to_fp32_sycl(dst_f16, dst_ddf, ne, main_stream);

    ggml_sycl_pool_free(g_main_device, src1_as_f16, src1_as);
    ggml_sycl_pool_free(g_main_device, dst_f16, dst_as);
}
#endif

static void ggml_sycl_mul_mat_id(const ggml_tensor *src0,
                                 const ggml_tensor *src1,
                                 ggml_tensor *dst) try {
#if 0
    ggml_sycl_mul_mat_id_sycl(dst);
    // TODO: mmq/mmv support
#endif

    const int64_t nb11 = src1->nb[1];
    const int64_t nb1  =  dst->nb[1];

    const struct ggml_tensor * ids = src0;
    const int32_t id = ((int32_t *) dst->op_params)[0];
    const int32_t n_as = ((int32_t *) dst->op_params)[1];

    std::vector<char> ids_host(ggml_nbytes(ids));

    const dpct::queue_ptr stream = g_syclStreams[g_main_device][0];

    if (ids->backend == GGML_BACKEND_TYPE_GPU) {
        const char * ids_dev = (const char *)((const ggml_tensor_extra_gpu *)ids->extra)->data_device[g_main_device];
        SYCL_CHECK(CHECK_TRY_ERROR(
            stream->memcpy(ids_host.data(), ids_dev, ggml_nbytes(ids)).wait()));
        // SYCL_CHECK(CHECK_TRY_ERROR(stream->wait()));
    } else {
        memcpy(ids_host.data(), ids->data, ggml_nbytes(ids));
    }

    const ggml_tensor_extra_gpu * src1_extra = (const ggml_tensor_extra_gpu *) src1->extra;
    const ggml_tensor_extra_gpu * dst_extra = (const ggml_tensor_extra_gpu *) dst->extra;

    ggml_tensor_extra_gpu src1_row_extra;
    ggml_tensor_extra_gpu dst_row_extra;

    ggml_tensor src1_row = *src1;
    ggml_tensor dst_row = *dst;

    src1_row.backend = GGML_BACKEND_TYPE_GPU;
    dst_row.backend  = GGML_BACKEND_TYPE_GPU;

    src1_row.extra = &src1_row_extra;
    dst_row.extra = &dst_row_extra;

    char * src1_original = src1->backend == GGML_BACKEND_TYPE_CPU ?
        (char *) src1->data : (char *) src1_extra->data_device[g_main_device];
    char * dst_original  =  dst->backend == GGML_BACKEND_TYPE_CPU ?
        (char *)  dst->data : (char *)  dst_extra->data_device[g_main_device];

    if (src1->ne[1] == 1) {
        GGML_ASSERT(src1->backend == GGML_BACKEND_TYPE_GPU);
        GGML_ASSERT(dst->backend  == GGML_BACKEND_TYPE_GPU);

        for (int64_t i01 = 0; i01 < ids->ne[1]; i01++) {
            //int32_t row_id;
            //SYCL_CHECK(syclMemcpyAsync(&row_id, ids_dev + i01*ids->nb[1] + id*ids->nb[0], sizeof(int32_t), syclMemcpyDeviceToHost, g_syclStreams[g_main_device][0]));
            //SYCL_CHECK(syclStreamSynchronize(g_syclStreams[g_main_device][0]));

            const int32_t row_id = *(const int32_t *) (ids_host.data() + i01*ids->nb[1] + id*ids->nb[0]);

            GGML_ASSERT(row_id >= 0 && row_id < n_as);

            const struct ggml_tensor * src0_row = dst->src[row_id + 2];

            src1_row_extra.data_device[g_main_device] = src1_original + i01*src1->nb[1];
            src1_row.data = (char *) src1->data + i01*src1->nb[1]; // TODO why is this set?

            dst_row_extra.data_device[g_main_device] = dst_original + i01*dst->nb[1];
            dst_row.data = (char *) dst->data + i01*dst->nb[1]; // TODO why is this set?

            ggml_sycl_mul_mat(src0_row, &src1_row, &dst_row);
        }
    } else {
        sycl_pool_alloc<char> src1_contiguous(sizeof(float)*ggml_nelements(src1));
        sycl_pool_alloc<char>  dst_contiguous(sizeof(float)*ggml_nelements(dst));

        src1_row_extra.data_device[g_main_device] = src1_contiguous.get();
        dst_row_extra.data_device[g_main_device]  =  dst_contiguous.get();

        for (int32_t row_id = 0; row_id < n_as; ++row_id) {
            const struct ggml_tensor * src0_row = dst->src[row_id + 2];

            int64_t num_src1_rows = 0;
            for (int64_t i01 = 0; i01 < ids->ne[1]; i01++) {
                const int32_t row_id_i = *(const int32_t *) (ids_host.data() + i01*ids->nb[1] + id*ids->nb[0]);

                if (row_id_i != row_id) {
                    continue;
                }

                GGML_ASSERT(row_id >= 0 && row_id < n_as);

                SYCL_CHECK(CHECK_TRY_ERROR(
                    stream->memcpy(src1_contiguous.get() + num_src1_rows * nb11,
                                   src1_original + i01 * nb11, nb11).wait()));
                num_src1_rows++;
            }

            if (num_src1_rows == 0) {
                continue;
            }

            src1_row.ne[1] = num_src1_rows;
            dst_row.ne[1] = num_src1_rows;

            src1_row.nb[1] = nb11;
            src1_row.nb[2] = num_src1_rows*nb11;
            src1_row.nb[3] = num_src1_rows*nb11;

            dst_row.nb[1] = nb1;
            dst_row.nb[2] = num_src1_rows*nb1;
            dst_row.nb[3] = num_src1_rows*nb1;

            ggml_sycl_mul_mat(src0_row, &src1_row, &dst_row);

            num_src1_rows = 0;
            for (int64_t i01 = 0; i01 < ids->ne[1]; i01++) {
                const int32_t row_id_i = *(const int32_t *) (ids_host.data() + i01*ids->nb[1] + id*ids->nb[0]);

                if (row_id_i != row_id) {
                    continue;
                }

                GGML_ASSERT(row_id >= 0 && row_id < n_as);

                SYCL_CHECK(CHECK_TRY_ERROR(stream->memcpy(
                    dst_original + i01 * nb1,
                    dst_contiguous.get() + num_src1_rows * nb1, nb1).wait()));
                num_src1_rows++;
            }
        }
    }

    if (dst->backend == GGML_BACKEND_TYPE_CPU) {
        SYCL_CHECK(CHECK_TRY_ERROR(stream->wait()));
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_sycl_scale(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_scale);
}

static void ggml_sycl_clamp(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_clamp);
}

static void ggml_sycl_cpy(const ggml_tensor *src0, const ggml_tensor *src1,
                          ggml_tensor *dst) try {
    const int64_t ne = ggml_nelements(src0);
    GGML_ASSERT(ne == ggml_nelements(src1));

    GGML_ASSERT(src0->backend == GGML_BACKEND_TYPE_GPU);
    GGML_ASSERT(src1->backend == GGML_BACKEND_TYPE_GPU);

    GGML_ASSERT(ggml_nbytes(src0) <= INT_MAX);
    GGML_ASSERT(ggml_nbytes(src1) <= INT_MAX);

    GGML_TENSOR_BINARY_OP_LOCALS;

    SYCL_CHECK(ggml_sycl_set_device(g_main_device));
    dpct::queue_ptr main_stream = g_syclStreams[g_main_device][0];

    const ggml_tensor_extra_gpu * src0_extra = (ggml_tensor_extra_gpu *) src0->extra;
    const ggml_tensor_extra_gpu * src1_extra = (ggml_tensor_extra_gpu *) src1->extra;

    char * src0_ddc = (char *) src0_extra->data_device[g_main_device];
    char * src1_ddc = (char *) src1_extra->data_device[g_main_device];

    if (src0->type == GGML_TYPE_F32 && src1->type == GGML_TYPE_F32) {
        ggml_cpy_f32_f32_sycl (src0_ddc, src1_ddc, ne, ne00, ne01, ne02, nb00, nb01, nb02, nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13, main_stream);
    } else if (src0->type == GGML_TYPE_F32 && src1->type == GGML_TYPE_F16) {
        ggml_cpy_f32_f16_sycl (src0_ddc, src1_ddc, ne, ne00, ne01, ne02, nb00, nb01, nb02, nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13, main_stream);
    } else if (src0->type == GGML_TYPE_F32 && src1->type == GGML_TYPE_Q8_0) {
        ggml_cpy_f32_q8_0_sycl(src0_ddc, src1_ddc, ne, ne00, ne01, ne02, nb00, nb01, nb02, nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13, main_stream);
    } else if (src0->type == GGML_TYPE_F32 && src1->type == GGML_TYPE_Q4_0) {
        ggml_cpy_f32_q4_0_sycl(src0_ddc, src1_ddc, ne, ne00, ne01, ne02, nb00, nb01, nb02, nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13, main_stream);
    } else if (src0->type == GGML_TYPE_F32 && src1->type == GGML_TYPE_Q4_1) {
        ggml_cpy_f32_q4_1_sycl(src0_ddc, src1_ddc, ne, ne00, ne01, ne02, nb00, nb01, nb02, nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13, main_stream);
    } else if (src0->type == GGML_TYPE_F16 && src1->type == GGML_TYPE_F32) {
        ggml_cpy_f16_f32_sycl (src0_ddc, src1_ddc, ne, ne00, ne01, ne02, nb00, nb01, nb02, nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13, main_stream);
    } else if (src0->type == GGML_TYPE_F16 && src1->type == GGML_TYPE_F16) {
        ggml_cpy_f16_f16_sycl (src0_ddc, src1_ddc, ne, ne00, ne01, ne02, nb00, nb01, nb02, nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13, main_stream);
    } else if (src0->type == GGML_TYPE_I16 && src1->type == GGML_TYPE_I16) {
        ggml_cpy_i16_i16_sycl (src0_ddc, src1_ddc, ne, ne00, ne01, ne02, nb00, nb01, nb02, nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13, main_stream);
    } else if (src0->type == GGML_TYPE_I32 && src1->type == GGML_TYPE_I32) {
        ggml_cpy_i32_i32_sycl (src0_ddc, src1_ddc, ne, ne00, ne01, ne02, nb00, nb01, nb02, nb03, ne10, ne11, ne12, nb10, nb11, nb12, nb13, main_stream);
    } else {
        fprintf(stderr, "%s: unsupported type combination (%s to %s)\n", __func__,
                ggml_type_name(src0->type), ggml_type_name(src1->type));
        GGML_ASSERT(false);
    }

    (void) dst;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_sycl_dup(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    // TODO: why do we pass dst as src1 here?
    ggml_sycl_cpy(src0, dst, nullptr);
    (void) src1;
}

static void ggml_sycl_diag_mask_inf(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_diag_mask_inf);
}

static void ggml_sycl_soft_max(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_soft_max);
}

static void ggml_sycl_rope(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_ASSERT(ggml_is_contiguous(src0)); // TODO: this restriction is temporary until non-cont support is implemented
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_rope);
}

static void ggml_sycl_alibi(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_alibi);
}

static void ggml_sycl_pool2d(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_pool2d);
}

static void ggml_sycl_im2col(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_im2col);
}

static void ggml_sycl_sum_rows(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_ASSERT(ggml_is_contiguous(src0));
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_sum_rows);
}

static void ggml_sycl_argsort(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    GGML_ASSERT(ggml_is_contiguous(src0));
    ggml_sycl_op_flatten(src0, src1, dst, ggml_sycl_op_argsort);
}

static void ggml_sycl_nop(const ggml_tensor * src0, const ggml_tensor * src1, ggml_tensor * dst) {
    (void) src0;
    (void) src1;
    (void) dst;
}

static size_t ggml_nbytes_split(const struct ggml_tensor * tensor, int nrows_split) {
    static_assert(GGML_MAX_DIMS == 4, "GGML_MAX_DIMS is not 4 - update this function");

    return nrows_split*ggml_row_size(tensor->type, tensor->ne[0]);
}

void ggml_sycl_free_data(struct ggml_tensor *tensor) try {
    if (!tensor || !tensor->extra || (tensor->backend != GGML_BACKEND_TYPE_GPU && tensor->backend != GGML_BACKEND_TYPE_GPU_SPLIT) ) {
        return;
    }

    ggml_tensor_extra_gpu * extra = (ggml_tensor_extra_gpu *) tensor->extra;

    for (int i = 0; i < g_device_count; ++i) {
        const dpct::queue_ptr stream = g_syclStreams[i][0];
        if (extra->data_device[i] != nullptr) {
            SYCL_CHECK(ggml_sycl_set_device(i));
            SYCL_CHECK(CHECK_TRY_ERROR(sycl::free(extra->data_device[i], *stream)));
        }

        for (int64_t is = 0; is < MAX_STREAMS; ++is) {
            if (extra->events[i][is] != nullptr) {
                SYCL_CHECK(ggml_sycl_set_device(i));
                SYCL_CHECK(CHECK_TRY_ERROR(
                    dpct::destroy_event(extra->events[i][is])));
            }
        }
    }

    delete extra;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static ggml_tensor_extra_gpu * g_temp_tensor_extras = nullptr;
static size_t g_temp_tensor_extra_index = 0;

static ggml_tensor_extra_gpu * ggml_sycl_alloc_temp_tensor_extra() {
    if (g_temp_tensor_extras == nullptr) {
        g_temp_tensor_extras = new ggml_tensor_extra_gpu[GGML_SYCL_MAX_NODES];
    }

    size_t alloc_index = g_temp_tensor_extra_index;
    g_temp_tensor_extra_index = (g_temp_tensor_extra_index + 1) % GGML_SYCL_MAX_NODES;
    ggml_tensor_extra_gpu * extra = &g_temp_tensor_extras[alloc_index];
    memset(extra, 0, sizeof(*extra));

    return extra;
}

static void ggml_sycl_assign_buffers_impl(struct ggml_tensor *tensor,
                                          bool scratch, bool force_inplace,
                                          bool no_alloc) try {
    if (scratch && g_scratch_size == 0) {
        return;
    }

    tensor->backend = GGML_BACKEND_TYPE_GPU;

    if (tensor->src[0] != nullptr && tensor->src[0]->backend == GGML_BACKEND_TYPE_CPU) {
        const ggml_op src0_op = tensor->src[0]->op;
        if (src0_op == GGML_OP_RESHAPE || src0_op == GGML_OP_TRANSPOSE || src0_op == GGML_OP_VIEW || src0_op == GGML_OP_PERMUTE) {
            ggml_sycl_assign_buffers_impl(tensor->src[0], scratch, force_inplace, no_alloc);
        }
    }
    if (tensor->op == GGML_OP_CPY && tensor->src[1]->backend == GGML_BACKEND_TYPE_CPU) {
        ggml_sycl_assign_buffers_impl(tensor->src[1], scratch, force_inplace, no_alloc);
    }

    if (scratch && no_alloc) {
        return;
    }

    ggml_tensor_extra_gpu * extra;

    const bool inplace = (tensor->src[0] != nullptr && tensor->src[0]->data == tensor->data) ||
        tensor->op == GGML_OP_VIEW ||
        force_inplace;
    const size_t size = ggml_nbytes(tensor);

    SYCL_CHECK(ggml_sycl_set_device(g_main_device));
    const dpct::queue_ptr stream = g_syclStreams[g_main_device][0];

    if (inplace && (tensor->src[0]->backend == GGML_BACKEND_TYPE_GPU || tensor->src[0]->backend == GGML_BACKEND_TYPE_GPU_SPLIT)) {
        ggml_tensor_extra_gpu * src0_extra = (ggml_tensor_extra_gpu * ) tensor->src[0]->extra;
        char * src0_ddc = (char *) src0_extra->data_device[g_main_device];
        size_t offset = 0;
        if (tensor->op == GGML_OP_VIEW) {
            memcpy(&offset, tensor->op_params, sizeof(size_t));
        }
        extra = ggml_sycl_alloc_temp_tensor_extra();
        extra->data_device[g_main_device] = src0_ddc + offset;
    } else if (tensor->op == GGML_OP_CPY) {
        ggml_tensor_extra_gpu * src1_extra = (ggml_tensor_extra_gpu * ) tensor->src[1]->extra;
        void * src1_ddv = src1_extra->data_device[g_main_device];
        extra = ggml_sycl_alloc_temp_tensor_extra();
        extra->data_device[g_main_device] = src1_ddv;
    } else if (scratch) {
        GGML_ASSERT(size <= g_scratch_size);
        if (g_scratch_offset + size > g_scratch_size) {
            g_scratch_offset = 0;
        }

        char * data = (char *) g_scratch_buffer;
        if (data == nullptr) {
            SYCL_CHECK(CHECK_TRY_ERROR(
                data = (char *)sycl::malloc_device(
                    g_scratch_size, *stream)));
            g_scratch_buffer = data;
        }
        extra = ggml_sycl_alloc_temp_tensor_extra();
        extra->data_device[g_main_device] = data + g_scratch_offset;

        g_scratch_offset += size;

        GGML_ASSERT(g_scratch_offset <= g_scratch_size);
    } else { // allocate new buffers outside of scratch
        void * data;
        SYCL_CHECK(CHECK_TRY_ERROR(data = (void *)sycl::malloc_device(
                                        size, *stream)));
        SYCL_CHECK(CHECK_TRY_ERROR(
            (*stream).memset(data, 0, size).wait()));
        extra = new ggml_tensor_extra_gpu;
        memset(extra, 0, sizeof(*extra));
        extra->data_device[g_main_device] = data;
    }

    tensor->extra = extra;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

void ggml_sycl_copy_to_device(struct ggml_tensor *tensor) try {
    GGML_ASSERT(tensor->backend == GGML_BACKEND_TYPE_GPU);
    GGML_ASSERT(ggml_is_contiguous(tensor));

    ggml_tensor_extra_gpu * extra = (ggml_tensor_extra_gpu *) tensor->extra;
    SYCL_CHECK(ggml_sycl_set_device(g_main_device));
    const dpct::queue_ptr stream = g_syclStreams[g_main_device][0];
    SYCL_CHECK(CHECK_TRY_ERROR((*stream)
                                    .memcpy(extra->data_device[g_main_device],
                                            tensor->data, ggml_nbytes(tensor))
                                    .wait()));
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

void ggml_sycl_assign_buffers(struct ggml_tensor * tensor) {
    ggml_sycl_assign_buffers_impl(tensor, true, false, false);
}

void ggml_sycl_assign_buffers_no_alloc(struct ggml_tensor * tensor) {
    ggml_sycl_assign_buffers_impl(tensor, true, false, true);
}

void ggml_sycl_assign_buffers_no_scratch(struct ggml_tensor * tensor) {
    ggml_sycl_assign_buffers_impl(tensor, false, false, false);
}

void ggml_sycl_assign_buffers_force_inplace(struct ggml_tensor * tensor) {
    ggml_sycl_assign_buffers_impl(tensor, false, true, false);
}

void ggml_sycl_set_main_device(const int main_device) try {
    if (g_main_device == main_device) return;
    check_allow_gpu_index(main_device);
    g_main_device = main_device;
    g_main_device_id = g_sycl_gpu_mgr->gpus[main_device];

    if (g_ggml_sycl_debug) {
        dpct::device_info prop;
        SYCL_CHECK(CHECK_TRY_ERROR(dpct::get_device_info(
            prop, dpct::dev_mgr::instance().get_device(g_main_device_id))));
        fprintf(stderr, "Using device %d (%s) as main device\n",
                g_main_device_id, prop.get_name());
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

void ggml_sycl_set_scratch_size(const size_t scratch_size) {
    // this is a hack to not completely break llama.cpp when using multiple models or contexts simultaneously
    // it still won't always work as expected, but it's better than nothing
    if (scratch_size > g_scratch_size) {
        ggml_sycl_free_scratch();
    }
    g_scratch_size = std::max(g_scratch_size, scratch_size);
}

void ggml_sycl_free_scratch() try {
    if (g_scratch_buffer == nullptr) {
        return;
    }
    ggml_sycl_set_device(g_main_device);
    const dpct::queue_ptr stream = g_syclStreams[g_main_device][0];

    SYCL_CHECK(CHECK_TRY_ERROR(
        sycl::free(g_scratch_buffer, *stream)));
    g_scratch_buffer = nullptr;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

bool ggml_sycl_compute_forward(struct ggml_compute_params * params, struct ggml_tensor * tensor) {
    if (!g_sycl_loaded) return false;

    ggml_sycl_func_t func;
    const bool any_on_device = tensor->backend == GGML_BACKEND_TYPE_GPU
        || (tensor->src[0] != nullptr && (tensor->src[0]->backend == GGML_BACKEND_TYPE_GPU || tensor->src[0]->backend == GGML_BACKEND_TYPE_GPU_SPLIT))
        || (tensor->src[1] != nullptr && tensor->src[1]->backend == GGML_BACKEND_TYPE_GPU);

    if (!any_on_device && tensor->op != GGML_OP_MUL_MAT && tensor->op != GGML_OP_MUL_MAT_ID) {
        return false;
    }

    if (tensor->op == GGML_OP_MUL_MAT) {
        if (tensor->src[0]->ne[3] != tensor->src[1]->ne[3]) {
#ifndef NDEBUG
            fprintf(stderr, "%s: cannot compute %s: src0->ne[3] = %" PRId64 ", src1->ne[3] = %" PRId64 " - fallback to CPU\n", __func__, tensor->name, tensor->src[0]->ne[3], tensor->src[1]->ne[3]);
#endif
            return false;
        }
    }

    switch (tensor->op) {
        case GGML_OP_REPEAT:
            func = ggml_sycl_repeat;
            break;
        case GGML_OP_GET_ROWS:
            func = ggml_sycl_get_rows;
            break;
        case GGML_OP_DUP:
            func = ggml_sycl_dup;
            break;
        case GGML_OP_ADD:
            func = ggml_sycl_add;
            break;
        case GGML_OP_ACC:
            func = ggml_sycl_acc;
            break;
        case GGML_OP_MUL:
            func = ggml_sycl_mul;
            break;
        case GGML_OP_DIV:
            func = ggml_sycl_div;
            break;
        case GGML_OP_UNARY:
            switch (ggml_get_unary_op(tensor)) {
                case GGML_UNARY_OP_GELU:
                    func = ggml_sycl_gelu;
                    break;
                case GGML_UNARY_OP_SILU:
                    func = ggml_sycl_silu;
                    break;
                case GGML_UNARY_OP_GELU_QUICK:
                    func = ggml_sycl_gelu_quick;
                    break;
                case GGML_UNARY_OP_TANH:
                    func = ggml_sycl_tanh;
                    break;
                case GGML_UNARY_OP_RELU:
                    func = ggml_sycl_relu;
                    break;
                case GGML_UNARY_OP_HARDSIGMOID:
                    func = ggml_sycl_hardsigmoid;
                    break;
                case GGML_UNARY_OP_HARDSWISH:
                    func = ggml_sycl_hardswish;
                    break;
                default:
                    return false;
            }
            break;
        case GGML_OP_NORM:
            func = ggml_sycl_norm;
            break;
        case GGML_OP_GROUP_NORM:
            func = ggml_sycl_group_norm;
            break;
        case GGML_OP_CONCAT:
            func = ggml_sycl_concat;
            break;
        case GGML_OP_UPSCALE:
            func = ggml_sycl_upscale;
            break;
        case GGML_OP_PAD:
            func = ggml_sycl_pad;
            break;
        case GGML_OP_LEAKY_RELU:
            func = ggml_sycl_leaky_relu;
            break;
        case GGML_OP_RMS_NORM:
            func = ggml_sycl_rms_norm;
            break;
        case GGML_OP_MUL_MAT:
            if (!any_on_device && !ggml_sycl_can_mul_mat(tensor->src[0], tensor->src[1], tensor)) {
                return false;
            }
            func = ggml_sycl_mul_mat;
            break;
        case GGML_OP_MUL_MAT_ID:
            if (!any_on_device && !ggml_sycl_can_mul_mat(tensor->src[2], tensor->src[1], tensor)) {
                return false;
            }
            func = ggml_sycl_mul_mat_id;
            break;
        case GGML_OP_SCALE:
            func = ggml_sycl_scale;
            break;
        case GGML_OP_SQR:
            func = ggml_sycl_sqr;
            break;
        case GGML_OP_CLAMP:
            func = ggml_sycl_clamp;
            break;
        case GGML_OP_CPY:
            func = ggml_sycl_cpy;
            break;
        case GGML_OP_CONT:
            func = ggml_sycl_dup;
            break;
        case GGML_OP_NONE:
        case GGML_OP_RESHAPE:
        case GGML_OP_VIEW:
        case GGML_OP_PERMUTE:
        case GGML_OP_TRANSPOSE:
            func = ggml_sycl_nop;
            break;
        case GGML_OP_DIAG_MASK_INF:
            func = ggml_sycl_diag_mask_inf;
            break;
        case GGML_OP_SOFT_MAX:
            func = ggml_sycl_soft_max;
            break;
        case GGML_OP_ROPE:
            func = ggml_sycl_rope;
            break;
        case GGML_OP_ALIBI:
            func = ggml_sycl_alibi;
            break;
        case GGML_OP_IM2COL:
            func = ggml_sycl_im2col;
            break;
        case GGML_OP_POOL_2D:
            func = ggml_sycl_pool2d;
            break;
        case GGML_OP_SUM_ROWS:
            func = ggml_sycl_sum_rows;
            break;
        case GGML_OP_ARGSORT:
            func = ggml_sycl_argsort;
            break;
        default:
            return false;
    }

    if (tensor->src[0] != nullptr && tensor->src[0]->backend == GGML_BACKEND_TYPE_GPU_SPLIT) {
        ggml_sycl_set_peer_access(tensor->src[1]->ne[1]);
    }

    if (params->ith != 0) {
        return true;
    }
    if (params->type == GGML_TASK_TYPE_INIT || params->type == GGML_TASK_TYPE_FINALIZE) {
        return true;
    }
    func(tensor->src[0], tensor->src[1], tensor);
    return true;
}

GGML_API GGML_CALL void   ggml_sycl_get_gpu_list(int *id_list, int max_len) try {
    GGML_SYCL_DEBUG("[SYCL] call ggml_sycl_get_gpu_list\n");
    for(int i=0;i<max_len;i++) id_list[i] = -1;

    if (!g_sycl_gpu_mgr) {
        g_sycl_gpu_mgr = new sycl_gpu_mgr();
    }

    for (int i=0;i< g_sycl_gpu_mgr->gpus.size();i++){
        if (i>=max_len) break;
        id_list[i] = g_sycl_gpu_mgr->gpus[i];
    }
    return;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

int ggml_sycl_get_device_count() try {
    int device_count;
    if (CHECK_TRY_ERROR(device_count =
                             dpct::dev_mgr::instance().device_count()) != 0) {
        return 0;
    }
    return device_count;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

GGML_API GGML_CALL void ggml_sycl_get_device_description(int device, char *description,
                                      size_t description_size) try {
    GGML_SYCL_DEBUG("[SYCL] call ggml_sycl_get_device_description\n");
    dpct::device_info prop;
    int device_id = g_sycl_gpu_mgr->gpus[device];
    SYCL_CHECK(CHECK_TRY_ERROR(dpct::get_device_info(
        prop, dpct::dev_mgr::instance().get_device(device_id))));
    snprintf(description, description_size, "%s", prop.get_name());
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

GGML_CALL void ggml_backend_sycl_get_device_memory(int device, size_t *free,
                                                   size_t *total) try {
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_get_device_memory\n");
    ggml_sycl_set_device(device);

    /*
    DPCT1009:218: SYCL uses exceptions to report errors and does not use the
    error codes. The original code was commented out and a warning string was
    inserted. You need to rewrite this code.
    */
    /*
    DPCT1106:217: 'cudaMemGetInfo' was migrated with the Intel extensions for
    device information which may not be supported by all compilers or runtimes.
    You may need to adjust the code.
    */
   int device_id = g_sycl_gpu_mgr->gpus[device];
    SYCL_CHECK(CHECK_TRY_ERROR(
        dpct::dev_mgr::instance().get_device(device_id).get_memory_info(*free, *total)));
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

////////////////////////////////////////////////////////////////////////////////

// backend interface

#define UNUSED GGML_UNUSED

// sycl buffer

struct ggml_backend_sycl_buffer_context {
    int device;
    void * dev_ptr = nullptr;
    ggml_tensor_extra_gpu * temp_tensor_extras = nullptr;
    size_t temp_tensor_extra_index = 0;
    std::string name;

     ggml_backend_sycl_buffer_context(int device, void * dev_ptr) :
        device(device), dev_ptr(dev_ptr) {
            check_allow_gpu_index(device);
            int id = g_sycl_gpu_mgr->gpus[device];
            name = (GGML_SYCL_NAME + std::to_string(id));
        }

    ~ ggml_backend_sycl_buffer_context() {
        delete[] temp_tensor_extras;
    }

    ggml_tensor_extra_gpu * ggml_sycl_alloc_temp_tensor_extra() {
        if (temp_tensor_extras == nullptr) {
            temp_tensor_extras = new ggml_tensor_extra_gpu[GGML_SYCL_MAX_NODES];
        }

        size_t alloc_index = temp_tensor_extra_index;
        temp_tensor_extra_index = (temp_tensor_extra_index + 1) % GGML_SYCL_MAX_NODES;
        ggml_tensor_extra_gpu * extra = &temp_tensor_extras[alloc_index];
        memset(extra, 0, sizeof(*extra));

        return extra;
    }
};

GGML_CALL static const char * ggml_backend_sycl_buffer_get_name(ggml_backend_buffer_t buffer) {
    ggml_backend_sycl_buffer_context * ctx = (ggml_backend_sycl_buffer_context *)buffer->context;
    return ctx->name.c_str();
}

GGML_CALL static bool ggml_backend_buffer_is_sycl(ggml_backend_buffer_t buffer) {
    return buffer->iface.get_name == ggml_backend_sycl_buffer_get_name;
}

static void
ggml_backend_sycl_buffer_free_buffer(ggml_backend_buffer_t buffer) try {
    ggml_backend_sycl_buffer_context * ctx = ( ggml_backend_sycl_buffer_context *)buffer->context;
    ggml_sycl_set_device(ctx->device);
    const dpct::queue_ptr stream = g_syclStreams[ctx->device][0];

    SYCL_CHECK(
        CHECK_TRY_ERROR(sycl::free(ctx->dev_ptr, *stream)));
    delete ctx;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void * ggml_backend_sycl_buffer_get_base(ggml_backend_buffer_t buffer) {
    ggml_backend_sycl_buffer_context * ctx = ( ggml_backend_sycl_buffer_context *)buffer->context;
    return ctx->dev_ptr;
}

GGML_CALL static void
ggml_backend_sycl_buffer_init_tensor(ggml_backend_buffer_t buffer,
                                     ggml_tensor *tensor) try {
    ggml_backend_sycl_buffer_context * ctx = (ggml_backend_sycl_buffer_context *)buffer->context;

    if (tensor->view_src != NULL && tensor->view_offs == 0) {
        assert(tensor->view_src->buffer->buft == buffer->buft);
        tensor->backend = tensor->view_src->backend;
        tensor->extra = tensor->view_src->extra;
        return;
    }

    ggml_tensor_extra_gpu * extra = ctx->ggml_sycl_alloc_temp_tensor_extra();

    extra->data_device[ctx->device] = tensor->data;
    tensor->backend = GGML_BACKEND_TYPE_GPU;
    tensor->extra = extra;

    if (ggml_is_quantized(tensor->type)) {
        // initialize padding to 0 to avoid possible NaN values
        size_t original_size = ggml_nbytes(tensor);
        size_t padded_size = ggml_backend_buft_get_alloc_size(buffer->buft, tensor);

        if (padded_size > original_size && tensor->view_src == nullptr) {
            SYCL_CHECK(CHECK_TRY_ERROR(g_syclStreams[ctx->device][0]->memset(
                (char *)tensor->data + original_size, 0,
                padded_size - original_size).wait()));
        }
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_backend_sycl_buffer_set_tensor(ggml_backend_buffer_t buffer,
                                                ggml_tensor *tensor,
                                                const void *data, size_t offset,
                                                size_t size) try {
    GGML_ASSERT(tensor->backend == GGML_BACKEND_TYPE_GPU);

    ggml_backend_sycl_buffer_context * ctx = ( ggml_backend_sycl_buffer_context *)buffer->context;

    ggml_sycl_set_device(ctx->device);
    const dpct::queue_ptr stream = g_syclStreams[ctx->device][0];
    SYCL_CHECK(
        CHECK_TRY_ERROR(dpct::dev_mgr::instance().get_device(ctx->device).queues_wait_and_throw()));

    SYCL_CHECK(
        CHECK_TRY_ERROR((*stream)
                             .memcpy((char *)tensor->data + offset, data, size)
                             .wait()));
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_backend_sycl_buffer_get_tensor(ggml_backend_buffer_t buffer,
                                                const ggml_tensor *tensor,
                                                void *data, size_t offset,
                                                size_t size) try {
    GGML_ASSERT(tensor->backend == GGML_BACKEND_TYPE_GPU);

    ggml_backend_sycl_buffer_context * ctx = ( ggml_backend_sycl_buffer_context *)buffer->context;

    ggml_sycl_set_device(ctx->device);
    const dpct::queue_ptr stream = g_syclStreams[ctx->device][0];

    SYCL_CHECK(
        CHECK_TRY_ERROR(dpct::dev_mgr::instance().get_device(ctx->device).queues_wait_and_throw()));

    SYCL_CHECK(CHECK_TRY_ERROR(
        (*stream)
            .memcpy(data, (const char *)tensor->data + offset, size)
            .wait()));
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

GGML_CALL static bool
ggml_backend_sycl_buffer_cpy_tensor(ggml_backend_buffer_t buffer,
                                    const ggml_tensor *src,
                                    ggml_tensor *dst) try {
    if (ggml_backend_buffer_is_sycl(src->buffer)) {
        ggml_backend_sycl_buffer_context * src_ctx = (ggml_backend_sycl_buffer_context *)src->buffer->context;
        ggml_backend_sycl_buffer_context * dst_ctx = (ggml_backend_sycl_buffer_context *)buffer->context;

        ggml_sycl_set_device(src_ctx->device);
        /*
        DPCT1009:198: SYCL uses exceptions to report errors and does not use the
        error codes. The original code was commented out and a warning string
        was inserted. You need to rewrite this code.
        */
        SYCL_CHECK(CHECK_TRY_ERROR(
            dpct::dev_mgr::instance().get_device(src_ctx->device).queues_wait_and_throw()));
        ggml_sycl_set_device(dst_ctx->device);
        /*
        DPCT1009:199: SYCL uses exceptions to report errors and does not use the
        error codes. The original code was commented out and a warning string
        was inserted. You need to rewrite this code.
        */
        SYCL_CHECK(CHECK_TRY_ERROR(
            dpct::dev_mgr::instance().get_device(dst_ctx->device).queues_wait_and_throw()));
        /*
        DPCT1009:200: SYCL uses exceptions to report errors and does not use the
        error codes. The original code was commented out and a warning string
        was inserted. You need to rewrite this code.
        */

        dpct::queue_ptr stream_dst = g_syclStreams[dst_ctx->device][0];
        dpct::queue_ptr stream_src = g_syclStreams[src_ctx->device][0];
        size_t size = ggml_nbytes(src);

        //todo. it's dirty solutino to walkaroud known issue:device2device cross GPUs.
        dev2dev_memcpy(*stream_dst, *stream_src, dst->data, src->data, size);

//todo, it's known issue：error in device2device cross GPUs. reused when the issue is fixed. DON"T remove
#if 0
        SYCL_CHECK(CHECK_TRY_ERROR((*stream).memcpy(
            (char *)dst->data, (const char *)src->data, size).wait()));

        /*
        DPCT1009:201: SYCL uses exceptions to report errors and does not use the
        error codes. The original code was commented out and a warning string
        was inserted. You need to rewrite this code.
        */
        SYCL_CHECK(CHECK_TRY_ERROR(
            dpct::dev_mgr::instance().get_device(dst_ctx->device).queues_wait_and_throw()));
#endif
        return true;
    }
    return false;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}


static void ggml_backend_sycl_buffer_clear(ggml_backend_buffer_t buffer,
                                           uint8_t value) try {
     ggml_backend_sycl_buffer_context * ctx = ( ggml_backend_sycl_buffer_context *)buffer->context;

    ggml_sycl_set_device(ctx->device);
    const dpct::queue_ptr stream = g_syclStreams[ctx->device][0];
    SYCL_CHECK(
        CHECK_TRY_ERROR(dpct::get_current_device().queues_wait_and_throw()));

    SYCL_CHECK(CHECK_TRY_ERROR((*stream)
                                    .memset(ctx->dev_ptr, value, buffer->size)
                                    .wait()));
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static struct ggml_backend_buffer_i ggml_backend_sycl_buffer_interface = {
    /* .get_name        = */ ggml_backend_sycl_buffer_get_name,
    /* .free_buffer     = */ ggml_backend_sycl_buffer_free_buffer,
    /* .get_base        = */ ggml_backend_sycl_buffer_get_base,
    /* .init_tensor     = */ ggml_backend_sycl_buffer_init_tensor,
    /* .set_tensor      = */ ggml_backend_sycl_buffer_set_tensor,
    /* .get_tensor      = */ ggml_backend_sycl_buffer_get_tensor,
    /* .cpy_tensor      = */ ggml_backend_sycl_buffer_cpy_tensor,
    /* .clear           = */ ggml_backend_sycl_buffer_clear,
    /* .reset           = */ NULL,
};

// sycl buffer type
struct ggml_backend_sycl_buffer_type_context {
    int device;
    std::string name;
};

struct ggml_backend_sycl_context {
    int device;
    std::string name;
};

GGML_CALL static const char * ggml_backend_sycl_buffer_type_name(ggml_backend_buffer_type_t buft) {
    ggml_backend_sycl_buffer_type_context * ctx = (ggml_backend_sycl_buffer_type_context *)buft->context;

    return ctx->name.c_str();
}
GGML_CALL static ggml_backend_buffer_t
ggml_backend_sycl_buffer_type_alloc_buffer(ggml_backend_buffer_type_t buft,
                                           size_t size) try {
    ggml_backend_sycl_buffer_type_context * buft_ctx = (ggml_backend_sycl_buffer_type_context *)buft->context;
    ggml_sycl_set_device(buft_ctx->device);
    const dpct::queue_ptr stream = g_syclStreams[buft_ctx->device][0];
    size = std::max(size, (size_t)1); // syclMalloc returns null for size 0

    void * dev_ptr;
    SYCL_CHECK(CHECK_TRY_ERROR(dev_ptr = (void *)sycl::malloc_device(
                                    size, *stream)));
    ggml_backend_sycl_buffer_context * ctx = new  ggml_backend_sycl_buffer_context(buft_ctx->device, dev_ptr);
    return ggml_backend_buffer_init(buft, ggml_backend_sycl_buffer_interface, ctx, size);
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

GGML_CALL static size_t ggml_backend_sycl_buffer_type_get_alignment(ggml_backend_buffer_type_t buft) {
    return 128;
    UNUSED(buft);
}

static size_t ggml_backend_sycl_buffer_type_get_max_size(ggml_backend_buffer_type_t buft) {
    return dpct::get_current_device().get_max_mem_alloc_size();

    UNUSED(buft);
}

GGML_CALL static size_t ggml_backend_sycl_buffer_type_get_alloc_size(ggml_backend_buffer_type_t buft, const ggml_tensor * tensor) {
    size_t size = ggml_nbytes(tensor);
    int64_t ne0 = tensor->ne[0];

    if (ggml_is_quantized(tensor->type)) {
        if (ne0 % MATRIX_ROW_PADDING != 0) {
            size += ggml_row_size(tensor->type, MATRIX_ROW_PADDING - ne0 % MATRIX_ROW_PADDING);
        }
    }

    return size;

    UNUSED(buft);
}

GGML_CALL static bool ggml_backend_sycl_buffer_type_supports_backend(ggml_backend_buffer_type_t buft, ggml_backend_t backend) {
    if (!ggml_backend_is_sycl(backend)) {
        return false;
    }
    ggml_backend_sycl_buffer_type_context * buft_ctx = (ggml_backend_sycl_buffer_type_context *)buft->context;
    ggml_backend_sycl_context * sycl_ctx = (ggml_backend_sycl_context *)backend->context;
    return buft_ctx->device == sycl_ctx->device;
}

static ggml_backend_buffer_type_i ggml_backend_sycl_buffer_type_interface = {
    /* .get_name         = */ ggml_backend_sycl_buffer_type_name,
    /* .alloc_buffer     = */ ggml_backend_sycl_buffer_type_alloc_buffer,
    /* .get_alignment    = */ ggml_backend_sycl_buffer_type_get_alignment,
    /* .get_max_size     = */ ggml_backend_sycl_buffer_type_get_max_size,
    /* .get_alloc_size   = */ ggml_backend_sycl_buffer_type_get_alloc_size,
    /* .supports_backend = */ ggml_backend_sycl_buffer_type_supports_backend,
    /* .is_host          = */ nullptr,
};

ggml_backend_buffer_type_t ggml_backend_sycl_buffer_type(int device_index) {
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_buffer_type\n");

    if (device_index>=g_device_count or device_index<0) {
        printf("ggml_backend_sycl_buffer_type error: device_index:%d is out of range [0, %d], miss to call ggml_backend_sycl_set_single_device()\n",
            device_index, g_device_count-1);
        GGML_ASSERT(device_index<g_device_count);
    }
    static struct ggml_backend_buffer_type ggml_backend_sycl_buffer_types[GGML_SYCL_MAX_DEVICES];

    if (!g_ggml_backend_sycl_buffer_type_initialized) {
        for (int i = 0; i < g_device_count; i++) {
            ggml_backend_sycl_buffer_types[i] = {
                /* .iface    = */ ggml_backend_sycl_buffer_type_interface,
                /* .context  = */ new ggml_backend_sycl_buffer_type_context{i, GGML_SYCL_NAME + std::to_string(g_sycl_gpu_mgr->gpus[i])},
            };
        }
        g_ggml_backend_sycl_buffer_type_initialized = true;
    }
    return &ggml_backend_sycl_buffer_types[device_index];
}

// sycl split buffer type
static void get_row_split(int64_t * row_low, int64_t * row_high, const ggml_tensor * tensor, const std::array<float, GGML_SYCL_MAX_DEVICES> & tensor_split, int id) {
    const int64_t nrows = ggml_nrows(tensor);
    const int64_t rounding = get_row_rounding(tensor->type, tensor_split);

    *row_low = id == 0 ? 0 : nrows*tensor_split[id];
    *row_low -= *row_low % rounding;
    if (id == g_device_count - 1) {
        *row_high = nrows;
    } else {
        *row_high = nrows*tensor_split[id + 1];
        *row_high -= *row_high % rounding;
    }
}

struct ggml_backend_sycl_split_buffer_context {
    ~ggml_backend_sycl_split_buffer_context() try {
        for (ggml_tensor_extra_gpu * extra : tensor_extras) {
            for (int i = 0; i < g_device_count; ++i) {
                // int id = g_sycl_gpu_mgr->gpus[i];
                for (int64_t is = 0; is < MAX_STREAMS; ++is) {
                    if (extra->events[i][is] != nullptr) {
                        /*
                        DPCT1009:206: SYCL uses exceptions to report errors and
                        does not use the error codes. The original code was
                        commented out and a warning string was inserted. You
                        need to rewrite this code.
                        */
                        SYCL_CHECK(CHECK_TRY_ERROR(
                            dpct::destroy_event(extra->events[i][is])));
                    }
                }
                if (extra->data_device[i] != nullptr) {
                    /*
                    DPCT1009:207: SYCL uses exceptions to report errors and does
                    not use the error codes. The original code was commented out
                    and a warning string was inserted. You need to rewrite this
                    code.
                    */
                    ggml_sycl_set_device(i);
                    SYCL_CHECK(CHECK_TRY_ERROR(sycl::free(
                        extra->data_device[i], *g_syclStreams[i][0])));
                }
            }
            delete extra;
        }
    }
    catch (sycl::exception const &exc) {
      std::cerr << exc.what() << "Exception caught at file:" << __FILE__
                << ", line:" << __LINE__ << std::endl;
      std::exit(1);
    }

    std::vector<ggml_tensor_extra_gpu *> tensor_extras;
};

GGML_CALL static const char * ggml_backend_sycl_split_buffer_get_name(ggml_backend_buffer_t buffer) {
    return GGML_SYCL_NAME "_Split";

    UNUSED(buffer);
}

// unused at the moment
//static bool ggml_backend_buffer_is_sycl_split(ggml_backend_buffer_t buffer) {
//    return buffer->iface.get_name == ggml_backend_sycl_split_buffer_get_name;
//}

GGML_CALL static void ggml_backend_sycl_split_buffer_free_buffer(ggml_backend_buffer_t buffer) {
    ggml_backend_sycl_split_buffer_context * ctx = (ggml_backend_sycl_split_buffer_context *)buffer->context;
    delete ctx;
}

GGML_CALL static void * ggml_backend_sycl_split_buffer_get_base(ggml_backend_buffer_t buffer) {
    // the pointers are stored in the tensor extras, this is just a dummy address and never dereferenced
    return (void *)0x1000;

    UNUSED(buffer);
}

GGML_CALL static void
ggml_backend_sycl_split_buffer_init_tensor(ggml_backend_buffer_t buffer,
                                           ggml_tensor *tensor) try {
    GGML_ASSERT(tensor->view_src == nullptr); // views of split tensors are not supported

    ggml_backend_sycl_split_buffer_context * ctx = (ggml_backend_sycl_split_buffer_context *)buffer->context;
    ggml_backend_sycl_split_buffer_type_context * buft_ctx = (ggml_backend_sycl_split_buffer_type_context *)buffer->buft->context;

    const int64_t ne0 = tensor->ne[0];

    ggml_tensor_extra_gpu * extra = new ggml_tensor_extra_gpu{};

    ctx->tensor_extras.push_back(extra);

    for (int i = 0; i < g_device_count; ++i) {
        // int id = g_sycl_gpu_mgr->gpus[i];
        int64_t row_low, row_high;
        get_row_split(&row_low, &row_high, tensor, buft_ctx->tensor_split, i);

        int64_t nrows_split = row_high - row_low;
        if (nrows_split == 0) {
            continue;
        }

        size_t size = ggml_nbytes_split(tensor, nrows_split);
        const size_t original_size = size;

        // pad last row to a multiple of 512 elements to avoid out-of-bounds memory accesses
        if (ne0 % MATRIX_ROW_PADDING != 0) {
            size += ggml_row_size(tensor->type, MATRIX_ROW_PADDING - ne0 % MATRIX_ROW_PADDING);
        }

        // FIXME: do not crash if cudaMalloc fails
        // currently, init_tensor cannot fail, it needs to be fixed in ggml-backend first
        ggml_sycl_set_device(i);
        char * buf;
        /*
        DPCT1009:208: SYCL uses exceptions to report errors and does not use the
        error codes. The original code was commented out and a warning string
        was inserted. You need to rewrite this code.
        */
        SYCL_CHECK(CHECK_TRY_ERROR(buf = (char *)sycl::malloc_device(
                                        size, *g_syclStreams[i][0])));

        // set padding to 0 to avoid possible NaN values
        if (size > original_size) {
            /*
            DPCT1009:209: SYCL uses exceptions to report errors and does not use
            the error codes. The original code was commented out and a warning
            string was inserted. You need to rewrite this code.
            */
            SYCL_CHECK(CHECK_TRY_ERROR(
                (*g_syclStreams[i][0])
                    .memset(buf + original_size, 0, size - original_size)
                    .wait()));
        }

        extra->data_device[i] = buf;

        for (int64_t is = 0; is < MAX_STREAMS; ++is) {
            /*
            DPCT1009:210: SYCL uses exceptions to report errors and does not use
            the error codes. The original code was commented out and a warning
            string was inserted. You need to rewrite this code.
            */
            SYCL_CHECK(
                CHECK_TRY_ERROR(extra->events[i][is] = new sycl::event()));
        }
    }
    tensor->backend = GGML_BACKEND_TYPE_GPU_SPLIT;
    tensor->extra = extra;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

GGML_CALL static void
ggml_backend_sycl_split_buffer_set_tensor(ggml_backend_buffer_t buffer,
                                          ggml_tensor *tensor, const void *data,
                                          size_t offset, size_t size) try {
    // split tensors must always be set in their entirety at once
    GGML_ASSERT(offset == 0);
    GGML_ASSERT(size == ggml_nbytes(tensor));

    ggml_backend_sycl_split_buffer_type_context * buft_ctx = (ggml_backend_sycl_split_buffer_type_context *)buffer->buft->context;

    const int64_t ne0 = tensor->ne[0];
    const size_t nb1 = tensor->nb[1];
    ggml_tensor_extra_gpu * extra = (ggml_tensor_extra_gpu *)tensor->extra;

    for (int i = 0; i < g_device_count; ++i) {
        // int id = g_sycl_gpu_mgr->gpus[i];
        int64_t row_low, row_high;
        get_row_split(&row_low, &row_high, tensor, buft_ctx->tensor_split, i);

        int64_t nrows_split = row_high - row_low;
        if (nrows_split == 0) {
            continue;
        }

        const size_t offset_split = row_low*nb1;
        size_t size = ggml_nbytes_split(tensor, nrows_split);
        const size_t original_size = size;

        // pad last row to a multiple of 512 elements to avoid out-of-bounds memory accesses
        if (ne0 % MATRIX_ROW_PADDING != 0) {
            size += ggml_row_size(tensor->type, MATRIX_ROW_PADDING - ne0 % MATRIX_ROW_PADDING);
        }

        const char * buf_host = (const char *)data + offset_split;
        /*
        DPCT1009:211: SYCL uses exceptions to report errors and does not use the
        error codes. The original code was commented out and a warning string
        was inserted. You need to rewrite this code.
        */
        ggml_sycl_set_device(i);
        SYCL_CHECK(CHECK_TRY_ERROR(
            (*g_syclStreams[i][0])
                .memcpy(extra->data_device[i], buf_host, original_size)
                .wait()));
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

GGML_CALL static void
ggml_backend_sycl_split_buffer_get_tensor(ggml_backend_buffer_t buffer,
                                          const ggml_tensor *tensor, void *data,
                                          size_t offset, size_t size) try {
    // split tensors must always be set in their entirety at once
    GGML_ASSERT(offset == 0);
    GGML_ASSERT(size == ggml_nbytes(tensor));

    ggml_backend_sycl_split_buffer_type_context * buft_ctx = (ggml_backend_sycl_split_buffer_type_context *)buffer->buft->context;

    const int64_t ne0 = tensor->ne[0];
    const size_t nb1 = tensor->nb[1];
    ggml_tensor_extra_gpu * extra = (ggml_tensor_extra_gpu *)tensor->extra;

    for (int i = 0; i < g_device_count; ++i) {
        // int id = g_sycl_gpu_mgr->gpus[i];
        int64_t row_low, row_high;
        get_row_split(&row_low, &row_high, tensor, buft_ctx->tensor_split, i);

        int64_t nrows_split = row_high - row_low;
        if (nrows_split == 0) {
            continue;
        }

        const size_t offset_split = row_low*nb1;
        size_t size = ggml_nbytes_split(tensor, nrows_split);
        const size_t original_size = size;

        // pad last row to a multiple of 512 elements to avoid out-of-bounds memory accesses
        if (ne0 % MATRIX_ROW_PADDING != 0) {
            size += ggml_row_size(tensor->type, MATRIX_ROW_PADDING - ne0 % MATRIX_ROW_PADDING);
        }

        char * buf_host = (char *)data + offset_split;
        /*
        DPCT1009:212: SYCL uses exceptions to report errors and does not use the
        error codes. The original code was commented out and a warning string
        was inserted. You need to rewrite this code.
        */
        ggml_sycl_set_device(i);
        SYCL_CHECK(CHECK_TRY_ERROR(
            (*g_syclStreams[i][0])
                .memcpy(buf_host, extra->data_device[i], original_size)
                .wait()));
    }
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

GGML_CALL static void ggml_backend_sycl_split_buffer_clear(ggml_backend_buffer_t buffer, uint8_t value) {
    UNUSED(buffer);
    UNUSED(value);
}

static struct ggml_backend_buffer_i ggml_backend_sycl_split_buffer_interface = {
    /* .get_name        = */ ggml_backend_sycl_split_buffer_get_name,
    /* .free_buffer     = */ ggml_backend_sycl_split_buffer_free_buffer,
    /* .get_base        = */ ggml_backend_sycl_split_buffer_get_base,
    /* .init_tensor     = */ ggml_backend_sycl_split_buffer_init_tensor,
    /* .set_tensor      = */ ggml_backend_sycl_split_buffer_set_tensor,
    /* .get_tensor      = */ ggml_backend_sycl_split_buffer_get_tensor,
    /* .cpy_tensor      = */ NULL,
    /* .clear           = */ ggml_backend_sycl_split_buffer_clear,
    /* .reset           = */ NULL,
};

GGML_CALL static const char * ggml_backend_sycl_split_buffer_type_name(ggml_backend_buffer_type_t buft) {
    return GGML_SYCL_NAME "_Split";

    UNUSED(buft);
}

GGML_CALL static ggml_backend_buffer_t ggml_backend_sycl_split_buffer_type_alloc_buffer(ggml_backend_buffer_type_t buft, size_t size) {
    // since we don't know the exact split after rounding, we cannot allocate the device buffers at this point
    // instead, we allocate them for each tensor separately in init_tensor
    // however, the size still represents the maximum cumulative size of all the device buffers after the tensors are allocated,
    // as returned by get_alloc_size. this limit is enforced during tensor allocation by ggml-alloc, so it must be correct.
    ggml_backend_sycl_split_buffer_context * ctx = new ggml_backend_sycl_split_buffer_context();

    return ggml_backend_buffer_init(buft, ggml_backend_sycl_split_buffer_interface, ctx, size);
}

GGML_CALL static size_t ggml_backend_sycl_split_buffer_type_get_alignment(ggml_backend_buffer_type_t buft) {
    return 128;
    UNUSED(buft);
}

GGML_CALL static size_t ggml_backend_sycl_split_buffer_type_get_alloc_size(ggml_backend_buffer_type_t buft, const ggml_tensor * tensor) {
    ggml_backend_sycl_split_buffer_type_context * ctx = (ggml_backend_sycl_split_buffer_type_context *)buft->context;

    size_t total_size = 0;

    const int64_t ne0 = tensor->ne[0];

    for (int i = 0; i < g_device_count; ++i) {
        // int id = g_sycl_gpu_mgr->gpus[i];
        int64_t row_low, row_high;
        get_row_split(&row_low, &row_high, tensor, ctx->tensor_split, i);

        int64_t nrows_split = row_high - row_low;
        if (nrows_split == 0) {
            continue;
        }

        total_size += ggml_nbytes_split(tensor, nrows_split);

        // pad last row to a multiple of 512 elements to avoid out-of-bounds memory accesses
        if (ne0 % MATRIX_ROW_PADDING != 0) {
            total_size += ggml_row_size(tensor->type, MATRIX_ROW_PADDING - ne0 % MATRIX_ROW_PADDING);
        }
    }

    return total_size;
}

GGML_CALL static bool ggml_backend_sycl_split_buffer_type_supports_backend(ggml_backend_buffer_type_t buft, ggml_backend_t backend) {
    return ggml_backend_is_sycl(backend);

    UNUSED(buft);
}

GGML_CALL static bool ggml_backend_sycl_split_buffer_type_is_host(ggml_backend_buffer_type_t buft) {
    return false;

    UNUSED(buft);
}

static ggml_backend_buffer_type_i ggml_backend_sycl_split_buffer_type_interface = {
    /* .get_name         = */ ggml_backend_sycl_split_buffer_type_name,
    /* .alloc_buffer     = */ ggml_backend_sycl_split_buffer_type_alloc_buffer,
    /* .get_alignment    = */ ggml_backend_sycl_split_buffer_type_get_alignment,
    /* .get_max_size     = */ NULL, // defaults to SIZE_MAX
    /* .get_alloc_size   = */ ggml_backend_sycl_split_buffer_type_get_alloc_size,
    /* .supports_backend = */ ggml_backend_sycl_split_buffer_type_supports_backend,
    /* .is_host          = */ ggml_backend_sycl_split_buffer_type_is_host,
};

GGML_CALL ggml_backend_buffer_type_t ggml_backend_sycl_split_buffer_type(const float * tensor_split) {
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_split_buffer_type\n");
    ggml_init_sycl();
    // FIXME: this is not thread safe
    static std::map<std::array<float, GGML_SYCL_MAX_DEVICES>, struct ggml_backend_buffer_type> buft_map;

    std::array<float, GGML_SYCL_MAX_DEVICES> tensor_split_arr = {};

    bool all_zero = tensor_split == nullptr || std::all_of(tensor_split, tensor_split + GGML_SYCL_MAX_DEVICES, [](float x) { return x == 0.0f; });
    if (all_zero) {
        tensor_split_arr = g_default_tensor_split;
    } else {
        float split_sum = 0.0f;
        for (int i = 0; i < g_device_count; ++i) {
            // int id = g_sycl_gpu_mgr->gpus[i];
            tensor_split_arr[i] = split_sum;
            split_sum += tensor_split[i];
        }
        for (int i = 0; i < g_device_count; ++i) {
            // int id = g_sycl_gpu_mgr->gpus[i];
            tensor_split_arr[i] /= split_sum;
        }
    }

    auto it = buft_map.find(tensor_split_arr);
    if (it != buft_map.end()) {
        return &it->second;
    }

    struct ggml_backend_buffer_type buft {
        /* .iface   = */ ggml_backend_sycl_split_buffer_type_interface,
        /* .context = */ new ggml_backend_sycl_split_buffer_type_context{tensor_split_arr},
    };

    auto result = buft_map.emplace(tensor_split_arr, buft);
    return &result.first->second;
}

// host buffer type

GGML_CALL static const char * ggml_backend_sycl_host_buffer_type_name(ggml_backend_buffer_type_t buft) {
    return GGML_SYCL_NAME "_Host";

    UNUSED(buft);
}

GGML_CALL static const char * ggml_backend_sycl_host_buffer_name(ggml_backend_buffer_t buffer) {
    return GGML_SYCL_NAME "_Host";

    UNUSED(buffer);
}

static void ggml_backend_sycl_host_buffer_free_buffer(ggml_backend_buffer_t buffer) {
    ggml_sycl_host_free(buffer->context);
}

static ggml_backend_buffer_t ggml_backend_sycl_host_buffer_type_alloc_buffer(ggml_backend_buffer_type_t buft, size_t size) {
    void * ptr = ggml_sycl_host_malloc(size);

    if (ptr == nullptr) {
        // fallback to cpu buffer
        return ggml_backend_buft_alloc_buffer(ggml_backend_cpu_buffer_type(), size);
    }

    // FIXME: this is a hack to avoid having to implement a new buffer type
    ggml_backend_buffer_t buffer = ggml_backend_cpu_buffer_from_ptr(ptr, size);
    buffer->buft = buft;
    buffer->iface.get_name = ggml_backend_sycl_host_buffer_name;
    buffer->iface.free_buffer = ggml_backend_sycl_host_buffer_free_buffer;

    return buffer;
}

ggml_backend_buffer_type_t ggml_backend_sycl_host_buffer_type() {
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_host_buffer_type\n");
    static struct ggml_backend_buffer_type ggml_backend_sycl_buffer_type_host = {
        /* .iface    = */ {
            /* .get_name         = */ ggml_backend_sycl_host_buffer_type_name,
            /* .alloc_buffer     = */ ggml_backend_sycl_host_buffer_type_alloc_buffer,
            /* .get_alignment    = */ ggml_backend_cpu_buffer_type()->iface.get_alignment,
            /* .get_max_size     = */ NULL, // TODO: return device.maxBufferLength
            /* .get_alloc_size   = */ ggml_backend_cpu_buffer_type()->iface.get_alloc_size,
            /* .supports_backend = */ ggml_backend_cpu_buffer_type()->iface.supports_backend,
            /* .is_host          = */ ggml_backend_cpu_buffer_type()->iface.is_host,
        },
        /* .context  = */ nullptr,
    };

    return &ggml_backend_sycl_buffer_type_host;
}

// backend

GGML_CALL static const char * ggml_backend_sycl_name(ggml_backend_t backend) {

    ggml_backend_sycl_context * sycl_ctx = (ggml_backend_sycl_context *)backend->context;

    return sycl_ctx->name.c_str();
}

GGML_CALL static void ggml_backend_sycl_free(ggml_backend_t backend) {
    ggml_backend_sycl_context * sycl_ctx = (ggml_backend_sycl_context *)backend->context;

    delete sycl_ctx;
    delete backend;
}


GGML_CALL static ggml_backend_buffer_type_t ggml_backend_sycl_get_default_buffer_type(ggml_backend_t backend) {
    ggml_backend_sycl_context * sycl_ctx = (ggml_backend_sycl_context *)backend->context;
    return ggml_backend_sycl_buffer_type(sycl_ctx->device);
}

GGML_CALL static void ggml_backend_sycl_set_tensor_async(ggml_backend_t backend,
                                               ggml_tensor *tensor,
                                               const void *data, size_t offset,
                                               size_t size) try {
    ggml_backend_sycl_context * sycl_ctx = (ggml_backend_sycl_context *)backend->context;
    GGML_ASSERT(tensor->buffer->buft == ggml_backend_sycl_buffer_type(sycl_ctx->device) && "unsupported buffer type");
    GGML_ASSERT(tensor->backend == GGML_BACKEND_TYPE_GPU);
    SYCL_CHECK(CHECK_TRY_ERROR(g_syclStreams[sycl_ctx->device][0]->memcpy(
        (char *)tensor->data + offset, data, size).wait()));
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

GGML_CALL static void ggml_backend_sycl_get_tensor_async(ggml_backend_t backend,
                                               const ggml_tensor *tensor,
                                               void *data, size_t offset,
                                               size_t size) try {
    ggml_backend_sycl_context * sycl_ctx = (ggml_backend_sycl_context *)backend->context;
    GGML_ASSERT(tensor->buffer->buft == ggml_backend_sycl_buffer_type(sycl_ctx->device) && "unsupported buffer type");
    GGML_ASSERT(tensor->backend == GGML_BACKEND_TYPE_GPU);
    SYCL_CHECK(CHECK_TRY_ERROR(g_syclStreams[sycl_ctx->device][0]->memcpy(
        data, (const char *)tensor->data + offset, size).wait()));
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

GGML_CALL static bool ggml_backend_sycl_cpy_tensor_async(ggml_backend_t backend,
                                                         const ggml_tensor *src,
                                                         ggml_tensor *dst) try {
    ggml_backend_sycl_context * sycl_ctx = (ggml_backend_sycl_context *)backend->context;
    if (dst->buffer->buft == ggml_backend_sycl_buffer_type(sycl_ctx->device) && ggml_backend_buffer_is_sycl(src->buffer)) {
        /*
        DPCT1009:215: SYCL uses exceptions to report errors and does not use the
        error codes. The original code was commented out and a warning string
        was inserted. You need to rewrite this code.
        */
        SYCL_CHECK(CHECK_TRY_ERROR(g_syclStreams[sycl_ctx->device][0]->memcpy(
            dst->data, src->data, ggml_nbytes(dst)).wait()));
        return true;
    }

    return false;
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

static void ggml_backend_sycl_synchronize(ggml_backend_t backend) try {
    ggml_backend_sycl_context * sycl_ctx = (ggml_backend_sycl_context *)backend->context;
    SYCL_CHECK(CHECK_TRY_ERROR(g_syclStreams[sycl_ctx->device][0]->wait()));

    UNUSED(backend);
}
catch (sycl::exception const &exc) {
  std::cerr << exc.what() << "Exception caught at file:" << __FILE__
            << ", line:" << __LINE__ << std::endl;
  std::exit(1);
}

GGML_CALL static ggml_status ggml_backend_sycl_graph_compute(ggml_backend_t backend, ggml_cgraph * cgraph) {
    ggml_backend_sycl_context * sycl_ctx = (ggml_backend_sycl_context *)backend->context;
    ggml_sycl_set_main_device(sycl_ctx->device);

    ggml_compute_params params = {};
    params.type = GGML_TASK_TYPE_COMPUTE;
    params.ith = 0;
    for (int i = 0; i < cgraph->n_nodes; i++) {
        ggml_tensor * node = cgraph->nodes[i];
        if (ggml_is_empty(node) || node->op == GGML_OP_RESHAPE || node->op == GGML_OP_TRANSPOSE || node->op == GGML_OP_VIEW || node->op == GGML_OP_PERMUTE || node->op == GGML_OP_NONE) {
            continue;
        }
#ifndef NDEBUG
        assert(node->backend == GGML_BACKEND_TYPE_GPU || node->backend == GGML_BACKEND_TYPE_GPU_SPLIT);
        assert(node->buffer->buft == ggml_backend_sycl_buffer_type(sycl_ctx->device));
        assert(node->extra != nullptr);

        for (int j = 0; j < GGML_MAX_SRC; j++) {
            if (node->src[j] != nullptr) {
                assert(node->src[j]->backend == GGML_BACKEND_TYPE_GPU || node->src[j]->backend == GGML_BACKEND_TYPE_GPU_SPLIT);
                assert(node->src[j]->buffer->buft == ggml_backend_sycl_buffer_type(sycl_ctx->device));
                assert(node->src[j]->extra != nullptr);
            }
        }
#endif
        bool ok = ggml_sycl_compute_forward(&params, node);
        if (!ok) {
            fprintf(stderr, "%s: error: op not supported %s (%s)\n", __func__, node->name, ggml_op_name(node->op));
        }
        GGML_ASSERT(ok);
    }

    return GGML_STATUS_SUCCESS;
}

GGML_CALL static bool ggml_backend_sycl_supports_op(ggml_backend_t backend, const ggml_tensor * op) {
    switch (op->op) {
        case GGML_OP_UNARY:
            switch (ggml_get_unary_op(op)) {
                case GGML_UNARY_OP_GELU:
                case GGML_UNARY_OP_SILU:
                case GGML_UNARY_OP_RELU:
                case GGML_UNARY_OP_HARDSIGMOID:
                case GGML_UNARY_OP_HARDSWISH:
                case GGML_UNARY_OP_GELU_QUICK:
                case GGML_UNARY_OP_TANH:
                    return true;
                default:
                    return false;
            }
            break;
        case GGML_OP_MUL_MAT:
        case GGML_OP_MUL_MAT_ID:
            {
                struct ggml_tensor * a;
                struct ggml_tensor * b;
                if (op->op == GGML_OP_MUL_MAT) {
                    a = op->src[0];
                    b = op->src[1];
                } else {
                    a = op->src[2];
                    b = op->src[1];
                }
                if (a->ne[3] != b->ne[3]) {
                    return false;
                }
                ggml_type a_type = a->type;
                if (a_type == GGML_TYPE_IQ4_NL || a_type == GGML_TYPE_IQ2_S ||
                    a_type == GGML_TYPE_IQ4_XS) {
                    return false;
                }
                return true;
            } break;
        case GGML_OP_GET_ROWS:
            {
                switch (op->src[0]->type) {
                    case GGML_TYPE_F16:
                    case GGML_TYPE_F32:
                    case GGML_TYPE_Q4_0:
                    case GGML_TYPE_Q4_1:
                    case GGML_TYPE_Q5_0:
                    case GGML_TYPE_Q5_1:
                    case GGML_TYPE_Q8_0:
                        return true;
                    default:
                        return false;
                }
            } break;
        case GGML_OP_CPY:
            {
                ggml_type src0_type = op->src[0]->type;
                ggml_type src1_type = op->src[1]->type;
                if (src0_type == GGML_TYPE_F32 && src1_type == GGML_TYPE_F32) {
                    return true;
                }
                if (src0_type == GGML_TYPE_F32 && src1_type == GGML_TYPE_F16) {
                    return true;
                }
                if (src0_type == GGML_TYPE_F32 && src1_type == GGML_TYPE_Q8_0) {
                    return true;
                }
                if (src0_type == GGML_TYPE_F32 && src1_type == GGML_TYPE_Q4_0) {
                    return true;
                }
                if (src0_type == GGML_TYPE_F32 && src1_type == GGML_TYPE_Q4_1) {
                    return true;
                }
                if (src0_type == GGML_TYPE_F16 && src1_type == GGML_TYPE_F16) {
                    return true;
                }
                if (src0_type == GGML_TYPE_F16 && src1_type == GGML_TYPE_F32) {
                    return true;
                }
                return false;
            } break;
        case GGML_OP_CONCAT:
            {
                ggml_type src0_type = op->src[0]->type;
                return src0_type != GGML_TYPE_I32 && src0_type != GGML_TYPE_I16;
            } break;
        case GGML_OP_DUP:
        case GGML_OP_NONE:
        case GGML_OP_RESHAPE:
        case GGML_OP_REPEAT:
        case GGML_OP_VIEW:
        case GGML_OP_PERMUTE:
        case GGML_OP_TRANSPOSE:
        case GGML_OP_NORM:
        case GGML_OP_ADD:
        case GGML_OP_MUL:
        case GGML_OP_DIV:
        case GGML_OP_RMS_NORM:
        case GGML_OP_SCALE:
        case GGML_OP_SQR:
        case GGML_OP_CLAMP:
        case GGML_OP_CONT:
        case GGML_OP_DIAG_MASK_INF:
        case GGML_OP_SOFT_MAX:
        case GGML_OP_ROPE:
        case GGML_OP_ALIBI:
        case GGML_OP_IM2COL:
        case GGML_OP_POOL_2D:
        case GGML_OP_SUM_ROWS:
        case GGML_OP_ARGSORT:
        case GGML_OP_ACC:
        case GGML_OP_GROUP_NORM:
        case GGML_OP_UPSCALE:
        case GGML_OP_PAD:
        case GGML_OP_LEAKY_RELU:
            return true;
        default:
            return false;
    }

    UNUSED(backend);
}

GGML_CALL static bool ggml_backend_sycl_offload_op(ggml_backend_t backend, const ggml_tensor * op) {
    const int min_batch_size = 32;
    return op->ne[1] >= min_batch_size && op->op != GGML_OP_GET_ROWS;
    GGML_UNUSED(backend);
}


static ggml_backend_i ggml_backend_sycl_interface = {
    /* .get_name                = */ ggml_backend_sycl_name,
    /* .free                    = */ ggml_backend_sycl_free,
    /* .get_default_buffer_type = */ ggml_backend_sycl_get_default_buffer_type,
    /* .set_tensor_async        = */ ggml_backend_sycl_set_tensor_async,
    /* .get_tensor_async        = */ ggml_backend_sycl_get_tensor_async,
    /* .cpy_tensor_async        = */ NULL, //ggml_backend_sycl_cpy_tensor_async, // TODO: update for the new interface
    /* .synchronize             = */ ggml_backend_sycl_synchronize,
    /* .graph_plan_create       = */ NULL,
    /* .graph_plan_free         = */ NULL,
    /* .graph_plan_compute      = */ NULL,
    /* .graph_compute           = */ ggml_backend_sycl_graph_compute,
    /* .supports_op             = */ ggml_backend_sycl_supports_op,
    /* .offload_op              = */ ggml_backend_sycl_offload_op,
    /* .event_new               = */ NULL,
    /* .event_free              = */ NULL,
    /* .event_record            = */ NULL,
    /* .event_wait              = */ NULL,
    /* .event_synchronize       = */ NULL,
};

static ggml_guid_t ggml_backend_sycl_guid() {
    static ggml_guid guid = { 0x58, 0x05, 0x13, 0x8f, 0xcd, 0x3a, 0x61, 0x9d, 0xe7, 0xcd, 0x98, 0xa9, 0x03, 0xfd, 0x7c, 0x53 };
    return &guid;
}

GGML_CALL ggml_backend_t ggml_backend_sycl_init(int device) {
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_init\n");
    ggml_init_sycl();

    check_allow_gpu_index(device);

    // not strictly necessary, but it may reduce the overhead of the first graph_compute
    ggml_sycl_set_main_device(device);
    int id = g_sycl_gpu_mgr->gpus[device];
    ggml_backend_sycl_context * ctx = new ggml_backend_sycl_context {
        /* .device = */ device,
        /* .name   = */ GGML_SYCL_NAME + std::to_string(id),
    };

    ggml_backend_t sycl_backend = new ggml_backend {
        /* .guid      = */ ggml_backend_sycl_guid(),
        /* .interface = */ ggml_backend_sycl_interface,
        /* .context   = */ ctx
    };

    return sycl_backend;
}

bool ggml_backend_is_sycl(ggml_backend_t backend) {
    return backend != NULL && ggml_guid_matches(backend->guid, ggml_backend_sycl_guid());
}

GGML_CALL int ggml_backend_sycl_get_device_count() {
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_get_device_count\n");
    if (!g_sycl_gpu_mgr) g_sycl_gpu_mgr = new sycl_gpu_mgr();
    return g_sycl_gpu_mgr->get_gpu_count();
}

GGML_CALL static ggml_backend_t ggml_backend_reg_sycl_init(const char * params, void * user_data) {
    ggml_backend_t sycl_backend = ggml_backend_sycl_init((int) (intptr_t) user_data);
    return sycl_backend;

    UNUSED(params);
}

GGML_API GGML_CALL int ggml_backend_sycl_get_device_index(int device_id) {
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_get_device_index\n");
    return g_sycl_gpu_mgr->get_index(device_id);
}

GGML_API GGML_CALL int ggml_backend_sycl_get_device_id(int device_index) {
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_get_device_id\n");
    return g_sycl_gpu_mgr->gpus[device_index];
}

GGML_API GGML_CALL void ggml_backend_sycl_set_single_device_mode(int main_gpu_id) {
    ggml_init_sycl();
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_set_single_device_mode\n");
    fprintf(stderr, "ggml_backend_sycl_set_single_device: use single device: [%d]\n", main_gpu_id);
    GGML_ASSERT(main_gpu_id<g_all_sycl_device_count);

    if (g_sycl_gpu_mgr) {
        delete g_sycl_gpu_mgr;
    }
    g_sycl_gpu_mgr = new sycl_gpu_mgr(main_gpu_id);
    g_ggml_sycl_backend_gpu_mode = SYCL_SINGLE_GPU_MODE;
    ggml_init_by_gpus(g_sycl_gpu_mgr->get_gpu_count());
    g_ggml_backend_sycl_buffer_type_initialized = false;
}

GGML_API GGML_CALL void ggml_backend_sycl_set_mul_device_mode() {
    ggml_init_sycl();
    GGML_SYCL_DEBUG("[SYCL] call ggml_backend_sycl_set_mul_device_mode\n");

    if (g_ggml_sycl_backend_gpu_mode == SYCL_MUL_GPU_MODE) {
        return;
    }

    fprintf(stderr, "ggml_backend_sycl_set_mul_device_mode: true\n");

    if (g_sycl_gpu_mgr) {
        delete g_sycl_gpu_mgr;
    }
    g_sycl_gpu_mgr = new sycl_gpu_mgr();
    g_ggml_sycl_backend_gpu_mode = SYCL_MUL_GPU_MODE;
    ggml_init_by_gpus(g_sycl_gpu_mgr->get_gpu_count());
    g_ggml_backend_sycl_buffer_type_initialized = false;
}

extern "C" int ggml_backend_sycl_reg_devices();

int ggml_backend_sycl_reg_devices() {
    ggml_backend_sycl_set_mul_device_mode();
    assert(g_device_count>0);
    for (int i = 0; i < g_device_count; i++) {
        int id = g_sycl_gpu_mgr->gpus[i];
        char name[128];
        snprintf(name, sizeof(name), "%s%d", GGML_SYCL_NAME, id);
        ggml_backend_register(name, ggml_backend_reg_sycl_init, ggml_backend_sycl_buffer_type(i), (void *) (intptr_t) i);
    }
    return g_device_count;
}
