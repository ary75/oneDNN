/*******************************************************************************
* Copyright 2022 Intel Corporation
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
*     http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*******************************************************************************/

#ifndef GPU_JIT_GEMM_PD_HPP
#define GPU_JIT_GEMM_PD_HPP

#include <vector>

#include "common/c_types_map.hpp"
#include "gpu/gpu_gemm_pd.hpp"
#include "gpu/jit/gemm/gen_gemm_kernel_generator.hpp"

namespace dnnl {
namespace impl {
namespace gpu {
namespace jit {

#define GEMM_MAX_PO 36

struct jit_gemm_pd_t : public gpu_gemm_pd_t {
    using gpu_gemm_pd_t::gpu_gemm_pd_t;

    struct binary_src_t {
        enum type_t { none, scales, bias, binary } type;
        int index;

        binary_src_t(type_t type_, int index_) : type(type_), index(index_) {}
    };

    status_t init_post_ops() {
        using namespace primitive_kind;
        using namespace alg_kind;
        using namespace data_type;

        // Examine post-ops and remember binary srcs.
        post_ops_ = attr()->post_ops_;
        binary_srcs_.reserve(post_ops_.len() + 4);

        bool ok = true;

        for (int i = 0; i < post_ops_.len(); i++) {
            const auto &e = post_ops_.entry_[i];
            switch (e.kind) {
                case binary:
                    ok &= gemm_kernel_generator_t<ngen::HW::Unknown>::
                                    supportedBinaryOp(e.binary.alg)
                            && is_md_gemm_compatible_plain_format(
                                    &e.binary.src1_desc);
                    binary_srcs_.push_back(
                            binary_src_t {binary_src_t::binary, int(i)});
                    break;
                case sum:
                    ok &= !with_sum_;
                    with_sum_ = true;
                    sum_at_begin_ = (i == 0);
                    binary_srcs_.push_back(
                            binary_src_t {binary_src_t::none, 0});
                    beta_ = e.sum.scale;
                    break;
                case eltwise:
                    ok &= jit_eltwise_injector_f32_is_supported(e.eltwise.alg);
                    binary_srcs_.push_back(
                            binary_src_t {binary_src_t::none, 0});
                    break;
                default: return status::unimplemented;
            }
        }

        if (!ok) return status::unimplemented;

        return status::success;
    }

    dim_t ld_binary(int idx) const {
        switch (binary_srcs_[idx].type) {
            case binary_src_t::binary: {
                const auto &entry = post_ops_.entry_[idx];
                assert(entry.kind == primitive_kind::binary);
                return gemm_desc_t::get_ld(entry.binary.src1_desc);
            }
            case binary_src_t::bias: return desc()->ld_bias();
            default: return 1;
        }
    }

    dim_t stride_binary(int idx, int stride = 0) const {
        switch (binary_srcs_[idx].type) {
            case binary_src_t::binary: {
                const auto &entry = post_ops_.entry_[idx];
                assert(entry.kind == primitive_kind::binary);
                return gemm_desc_t::get_stride(entry.binary.src1_desc, stride);
            }
            default: return 0;
        }
    }

    const post_ops_t *post_ops() const { return &post_ops_; }
    const std::vector<binary_src_t> &binary_srcs() const {
        return binary_srcs_;
    }

    float beta_ = 0.0f;

    bool with_sum_ = false;
    bool sum_at_begin_ = false;

    bool bias_via_binary_ = false;

    post_ops_t post_ops_;
    std::vector<binary_src_t> binary_srcs_;

    memory_desc_t wei_scales_md, src_scales_md, c_scales_md;
};

} // namespace jit
} // namespace gpu
} // namespace impl
} // namespace dnnl

#endif