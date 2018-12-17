/*******************************************************************************
* Copyright 2016-2018 Intel Corporation
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

#ifndef LRN_PD_HPP
#define LRN_PD_HPP

#include "mkldnn.h"

#include "c_types_map.hpp"
#include "primitive_desc.hpp"
#include "memory_pd.hpp"

namespace mkldnn {
namespace impl {

struct lrn_fwd_pd_t: public primitive_desc_t {
    typedef lrn_fwd_pd_t base_class;
    typedef lrn_fwd_pd_t hint_class;
    static constexpr auto base_pkind = primitive_kind::lrn;

    lrn_fwd_pd_t(mkldnn::impl::engine_t *engine, const lrn_desc_t *adesc,
            const primitive_attr_t *attr, const lrn_fwd_pd_t *hint_fwd_pd)
        : primitive_desc_t(engine, attr, primitive_kind::lrn)
        , desc_(*adesc), hint_fwd_pd_(hint_fwd_pd) {}
    virtual ~lrn_fwd_pd_t() {}

    const lrn_desc_t *desc() const { return &desc_; }
    virtual const op_desc_t *op_desc() const override
    { return reinterpret_cast<const op_desc_t *>(this->desc()); }
    virtual void init_info() override { init_info_lrn(this, this->info_); }

    virtual arg_usage_t arg_usage(primitive_arg_index_t arg) const override {
        if (arg == MKLDNN_ARG_SRC)
            return arg_usage_t::input;

        if (arg == MKLDNN_ARG_DST)
            return arg_usage_t::output;

        if (arg == MKLDNN_ARG_WORKSPACE && (workspace_pd() != nullptr))
            return arg_usage_t::output;

        return primitive_desc_t::arg_usage(arg);
    }

    virtual const memory_pd_t *input_pd(int index = 0) const override
    { return index == 0 ? src_pd() : nullptr; }
    virtual const memory_pd_t *output_pd(int index = 0) const override {
        if (index == 0) return dst_pd();
        if (index == 1) return workspace_pd();
        return nullptr;
    }

    virtual int n_inputs() const override { return 1; }
    virtual int n_outputs() const override
    { return 1 + (workspace_pd() != nullptr); }

    virtual status_t query(query_t what, int idx, void *result) const override
    {
        switch (what) {
        case query::lrn_d:
            *(const lrn_desc_t**)result = desc(); break;
        default: return primitive_desc_t::query(what, idx, result);
        }
        return status::success;
    }

    /* common lrn aux functions */

    inline int MB() const { return desc_.data_desc.dims[0]; }
    inline int C() const { return desc_.data_desc.dims[1]; }
    inline int H() const { return desc_.data_desc.dims[2]; }
    inline int W() const { return desc_.data_desc.dims[3]; }

    bool has_zero_dim_memory() const
    { return memory_desc_wrapper(desc_.data_desc).has_zero_dim(); }

protected:
    lrn_desc_t desc_;
    const lrn_fwd_pd_t *hint_fwd_pd_;
};

struct lrn_bwd_pd_t: public primitive_desc_t {
    typedef lrn_bwd_pd_t base_class;
    typedef lrn_fwd_pd_t hint_class;
    static constexpr auto base_pkind = primitive_kind::lrn;

    lrn_bwd_pd_t(mkldnn::impl::engine_t *engine, const lrn_desc_t *adesc,
            const primitive_attr_t *attr, const lrn_fwd_pd_t *hint_fwd_pd)
        : primitive_desc_t(engine, attr, primitive_kind::lrn)
        , desc_(*adesc), hint_fwd_pd_(hint_fwd_pd) {}
    virtual ~lrn_bwd_pd_t() {}

    const lrn_desc_t *desc() const { return &desc_; }
    virtual const op_desc_t *op_desc() const override
    { return reinterpret_cast<const op_desc_t *>(this->desc()); }
    virtual void init_info() override { init_info_lrn(this, this->info_); }

    virtual arg_usage_t arg_usage(primitive_arg_index_t arg) const override {
        if (utils::one_of(arg, MKLDNN_ARG_SRC, MKLDNN_ARG_DIFF_DST))
            return arg_usage_t::input;

        if (arg == MKLDNN_ARG_DIFF_SRC)
            return arg_usage_t::output;

        if (arg == MKLDNN_ARG_WORKSPACE && (workspace_pd() != nullptr))
            return arg_usage_t::input;

        return primitive_desc_t::arg_usage(arg);
    }

    virtual const memory_pd_t *input_pd(int index = 0) const override {
        if (index == 0) return src_pd();
        if (index == 1) return diff_dst_pd();
        if (index == 2) return workspace_pd();
        return nullptr;
    }
    virtual const memory_pd_t *output_pd(int index = 0) const override
    { return index == 0 ? diff_src_pd() : nullptr; }

    virtual int n_inputs() const override
    { return 2 + (workspace_pd() != nullptr); }
    virtual int n_outputs() const override
    { return 1; }

    virtual status_t query(query_t what, int idx, void *result) const override
    {
        switch (what) {
        case query::lrn_d:
            *(const lrn_desc_t**)result = desc(); break;
        default: return primitive_desc_t::query(what, idx, result);
        }
        return status::success;
    }

    /* common lrn aux functions */

    inline int MB() const { return desc_.data_desc.dims[0]; }
    inline int C() const { return desc_.data_desc.dims[1]; }
    inline int H() const { return desc_.data_desc.dims[2]; }
    inline int W() const { return desc_.data_desc.dims[3]; }

    bool has_zero_dim_memory() const
    { return memory_desc_wrapper(desc_.data_desc).has_zero_dim(); }

protected:
    lrn_desc_t desc_;
    const lrn_fwd_pd_t *hint_fwd_pd_;
};

}
}

#endif

// vim: et ts=4 sw=4 cindent cino^=l0,\:0,N-s
