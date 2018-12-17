/*******************************************************************************
* Copyright 2017-2018 Intel Corporation
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

#ifndef CPU_JIT_UNI_POOLING_HPP
#define CPU_JIT_UNI_POOLING_HPP

#include <assert.h>

#include "c_types_map.hpp"
#include "cpu_pooling_pd.hpp"
#include "cpu_engine.hpp"
#include "jit_uni_pool_kernel_f32.hpp"
#include "type_helpers.hpp"
#include "utils.hpp"

namespace mkldnn {
namespace impl {
namespace cpu {

template <cpu_isa_t isa>
struct jit_uni_pooling_fwd_t: public cpu_primitive_t {
    struct pd_t: public cpu_pooling_fwd_pd_t {
        pd_t(engine_t *engine, const pooling_desc_t *adesc,
                const primitive_attr_t *attr,
                const pooling_fwd_pd_t *hint_fwd_pd)
            : cpu_pooling_fwd_pd_t(engine, adesc, attr, hint_fwd_pd) {}

        DECLARE_COMMON_PD_T(
                JIT_IMPL_NAME_HELPER("jit:", isa, ""),
                jit_uni_pooling_fwd_t<isa>);

        virtual status_t init() override {
            using namespace prop_kind;
            using namespace alg_kind;
            using namespace utils;
            assert(engine()->kind() == engine_kind::cpu);
            bool ok = true
                && mayiuse(isa)
                && set_default_params() == status::success
                && one_of(desc()->prop_kind, forward_training,
                        forward_inference)
                && one_of(desc()->alg_kind, pooling_max,
                        pooling_avg_include_padding,
                        pooling_avg_exclude_padding)
                && !has_zero_dim_memory()
                && everyone_is(data_type::f32, src_pd()->desc()->data_type,
                        dst_pd()->desc()->data_type)
                && everyone_is(desired_fmt(), src_pd()->desc()->format,
                        dst_pd()->desc()->format)
                && attr()->has_default_values();
            if (!ok) return status::unimplemented;

            bool is_training = desc_.prop_kind == forward_training;
            if (desc()->alg_kind == pooling_max && is_training) {
                auto indices_desc = *dst_pd()->desc();
                indices_desc.data_type = pooling_index_data_type(desc());
                ws_pd_ = cpu_memory_t::pd_t(engine_, &indices_desc);
            }

            return jit_uni_pool_kernel_f32<isa>::init_conf(jpp_, desc_,
                    src_pd_.desc(), dst_pd_.desc());
        }
        inline memory_format_t desired_fmt()
        {
            using namespace memory_format;
            return (desc()->src_desc.ndims == 4)
                ? isa == avx512_common ? nChw16c : nChw8c
                : isa == avx512_common ? nCdhw16c : nCdhw8c;
        }

        jit_pool_conf_t jpp_;

    protected:
        virtual status_t set_default_params() override {
            if (dst_pd_.desc()->format == memory_format::any)
               CHECK(dst_pd_.set_format(desired_fmt()));
            return status::success;
        }
    };

    jit_uni_pooling_fwd_t(const pd_t *apd, const input_vector &inputs,
            const output_vector &outputs)
        : cpu_primitive_t(apd, inputs, outputs)
    { kernel_ = new jit_uni_pool_kernel_f32<isa>(pd()->jpp_); }

    ~jit_uni_pooling_fwd_t() { delete kernel_; }

    typedef typename prec_traits<data_type::f32>::type data_t;

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        auto src = CTX_IN_MEM(const data_t *, MKLDNN_ARG_SRC);
        auto dst = CTX_OUT_MEM(data_t *, MKLDNN_ARG_DST);
        auto ws = CTX_OUT_MEM(char *, MKLDNN_ARG_WORKSPACE);

        if (pd()->jpp_.ndims == 5)
            execute_forward_3d(src, dst, ws);
        else
            execute_forward(src, dst, ws);
        UNUSED(ctx);
        return status::success;
    }

private:
    void execute_forward(const data_t *src, data_t *dst, char *indices) const;
    void execute_forward_3d(const data_t *src, data_t *dst,
            char *indices) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd(); }
    jit_uni_pool_kernel_f32<isa> *kernel_;
};

template <cpu_isa_t isa>
struct jit_uni_pooling_bwd_t: public cpu_primitive_t {
    struct pd_t: public cpu_pooling_bwd_pd_t {
        pd_t(engine_t *engine, const pooling_desc_t *adesc,
                const primitive_attr_t *attr,
                const pooling_fwd_pd_t *hint_fwd_pd)
            : cpu_pooling_bwd_pd_t(engine, adesc, attr, hint_fwd_pd) {}

        DECLARE_COMMON_PD_T(
                JIT_IMPL_NAME_HELPER("jit:", isa, ""),
                jit_uni_pooling_bwd_t<isa>);

        virtual status_t init() override {
            using namespace prop_kind;
            using namespace alg_kind;
            using namespace utils;

            assert(engine()->kind() == engine_kind::cpu);
            bool ok = true
                && mayiuse(isa)
                && set_default_params() == status::success
                && one_of(desc()->prop_kind, backward, backward_data)
                && one_of(desc()->alg_kind, pooling_max,
                        pooling_avg_include_padding,
                        pooling_avg_exclude_padding)
                && !has_zero_dim_memory()
                && everyone_is(desired_fmt(), diff_src_pd()->desc()->format,
                        diff_dst_pd()->desc()->format)
                && everyone_is(data_type::f32, diff_src_pd()->desc()->data_type,
                        diff_dst_pd()->desc()->data_type)
                && IMPLICATION(desc()->alg_kind == pooling_max,
                        hint_fwd_pd_ && hint_fwd_pd_->workspace_pd()
                        && hint_fwd_pd_->workspace_pd()->desc()->format
                                == desired_fmt())
                && attr()->has_default_values();
            if (!ok) return status::unimplemented;

            if (desc()->alg_kind == pooling_max)
                ws_pd_ = *(cpu_memory_t::pd_t*)hint_fwd_pd_->workspace_pd();

            return jit_uni_pool_kernel_f32<isa>::init_conf(jpp_, desc_,
                    diff_src_pd_.desc(), diff_dst_pd_.desc());
        }

        inline memory_format_t desired_fmt()
        {
            using namespace memory_format;
            return (desc()->diff_src_desc.ndims == 4)
                ? isa == avx512_common ? nChw16c : nChw8c
                : isa == avx512_common ? nCdhw16c : nCdhw8c;
        }

        jit_pool_conf_t jpp_;

    protected:
        virtual status_t set_default_params() override {
            if (diff_src_pd_.desc()->format == memory_format::any)
               CHECK(diff_src_pd_.set_format(desired_fmt()));
           return status::success;
        }
    };

    jit_uni_pooling_bwd_t(const pd_t *apd, const input_vector &inputs,
            const output_vector &outputs)
        : cpu_primitive_t(apd, inputs, outputs)
    { kernel_ = new jit_uni_pool_kernel_f32<isa>(pd()->jpp_); }

    ~jit_uni_pooling_bwd_t() { delete kernel_; }

    typedef typename prec_traits<data_type::f32>::type data_t;

    virtual status_t execute(const exec_ctx_t &ctx) const override {
        auto diff_dst = CTX_IN_MEM(const data_t *, MKLDNN_ARG_DIFF_DST);
        auto ws = CTX_IN_MEM(const char *, MKLDNN_ARG_WORKSPACE);
        auto diff_src = CTX_OUT_MEM(data_t *, MKLDNN_ARG_DIFF_SRC);

        if (pd()->jpp_.ndims == 5)
            execute_backward_3d(diff_dst, ws, diff_src);
        else
            execute_backward(diff_dst, ws, diff_src);
        UNUSED(ctx);
        return status::success;
    }

private:
    void execute_backward(const data_t *diff_dst, const char *indices,
            data_t *diff_src) const;
    void execute_backward_3d(const data_t *diff_dst, const char *indices,
            data_t *diff_src) const;
    const pd_t *pd() const { return (const pd_t *)primitive_t::pd(); }
    jit_uni_pool_kernel_f32<isa> *kernel_;
};

}
}
}

#endif

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
