/*******************************************************************************
 * Copyright 2022-2023 Intel Corporation
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

#include <algorithm>
#include <functional>
#include <numeric>
#include <utility>
#include "nested_conv_fwd.hpp"
#include "utils.hpp"
#include <compiler/ir/builder.hpp>
#include <compiler/ir/builtin.hpp>
#include <compiler/ir/easy_build.hpp>
#include <compiler/ir/graph/fusion_mgr.hpp>
#include <compiler/ir/transform/auto_cast.hpp>
#include <compiler/ir/transform/constant_fold.hpp>
#include <compiler/ir/transform/tensor_shrink.hpp>
#include <runtime/barrier.hpp>
#include <runtime/config.hpp>
#include <unordered_set>
#include <util/any_map.hpp>
#include <util/math_utils.hpp>
#include <util/reflection.hpp>
#include <util/utils.hpp>

#include <thread>

using namespace dnnl::impl::graph::gc::builder;
namespace dnnl {
namespace impl {
namespace graph {
namespace gc {

using ops::nested_conv_fwd_config_t;
// clang-format off
SC_CLASS(nested_conv_fwd_config_t)
  SC_FIELD(K_block)
  SC_FIELD(C_block)
  SC_FIELD(pack_input)
  SC_FIELD(bs_threads)
  SC_FIELD(oc_threads)
  SC_FIELD(im_oc_block)
  SC_FIELD(im_ic_block)
  SC_FIELD(h_threads)
  SC_FIELD(w_threads)
  SC_FIELD(h_block)
  SC_FIELD(w_block)
  SC_FIELD(im_h_block)
  SC_FIELD(im_w_block)
SC_CLASS_END();
// clang-format on

namespace ops {

config_ptr gen_nested_conv_fwd_t::get_default_config(context_ptr ctx) const {
  auto ret = reflection::general_object_t::make<nested_conv_fwd_config_t>();
  nested_conv_fwd_config_t &cfg
    = *ret.unchecked_get_as<nested_conv_fwd_config_t>();
  if (use_nested_2d_) {
    const int num_threads = runtime_config_t::get().get_num_threads();
    auto thread_split = get_splits(num_threads);
    cfg.bs_threads = mb_ > num_threads || (mb_ == num_threads && oc_ <= 128)
      ? num_threads
      : *(std::find_if(thread_split.rbegin(), thread_split.rend(),
        [&](int split) { return split == 1 || split < mb_; }));
    cfg.oc_threads = num_threads / cfg.bs_threads;
    cfg.h_threads = 1;
    cfg.w_threads = 1;
    auto ic_threads = 1;
    auto default_block = 128;
    cfg.im_oc_block = utils::get_blocks(oc_, 1, default_block).back();
    cfg.im_ic_block = utils::get_blocks(ic_, 1, default_block).back();

    cfg.im_h_block = 1;
    cfg.im_w_block = ow_;

    cfg.h_block = oh_;
    cfg.w_block = ow_;

    if (cfg.oc_threads != 1) {
      int im_oc_num_block = oc_ / cfg.im_oc_block;
      if (im_oc_num_block % cfg.oc_threads != 0) {
        auto get_suitable_block
          = [](int total, int original_block, const std::vector<int> &splits,
              int threads) {
              int suitable_block = original_block;
              for (auto split : splits) {
                int num_block = total / split;
                if (num_block % threads == 0) {
                  if ((total / suitable_block) % threads != 0
                    || std::abs(original_block - split)
                      < std::abs(original_block - suitable_block))
                    suitable_block = split;
                }
              }
              return suitable_block;
            };
        // Get a suitable im_oc_block when im_oc_num_block can't be evenly
        // distributed
        cfg.im_oc_block = get_suitable_block(
          oc_, cfg.im_oc_block, get_splits(oc_), cfg.oc_threads);
      }
    }

    if (try_os_blocking_) {
      cfg.im_w_block = get_os_blocks(ow_, adj_os_).back();
      if (ow_ > 28 && ctx->use_amx()) {
        cfg.im_w_block = utils::get_blocks(ow_, 1, 256).back();
      } else {
        auto os_blocks = get_os_blocks(ow_, adj_os_);
        for (int i = os_blocks.size() - 1; i >= 0; i--) {
          if (os_blocks[i] < 800) {
            cfg.im_w_block = os_blocks[i];
            break;
          }
        }
      }
      bool pack_rows = (cfg.im_w_block > 0 && ow_ % cfg.im_w_block != 0);
      cfg.w_block = pack_rows ? adj_os_ : actual_os_;
      if (mb_ == 1 && num_threads == 4) {
        cfg.im_w_block = utils::get_blocks(ow_, 1, 256).back();
        if (oc_ >= 512) {
          cfg.bs_threads = 1;
          cfg.h_threads = 1;
          cfg.w_threads = 1;
          cfg.oc_threads = num_threads;
        } else {
          cfg.bs_threads = 1;
          cfg.oc_threads = 1;
          cfg.h_threads = num_threads;
          cfg.w_threads = 1;
        }
        cfg.im_oc_block
          = std::min(utils::get_blocks(oc_, 1, default_block).back(),
            oc_ / cfg.oc_threads);
        cfg.w_block
          = utils::divide_and_ceil(
              utils::divide_and_ceil(actual_os_, cfg.im_w_block), cfg.w_threads)
          * cfg.im_w_block;
      }
      pack_rows = (cfg.im_w_block > 0 && ow_ % cfg.im_w_block != 0);
      if (!pack_rows) {
        cfg.im_h_block = 1;
        cfg.h_block = cfg.h_threads == 1
          ? oh_
          : (utils::divide_and_ceil(
               utils::divide_and_ceil(oh_, cfg.im_h_block), cfg.h_threads)
            * cfg.im_h_block);
        cfg.w_block = cfg.w_threads == 1
          ? ow_
          : (utils::divide_and_ceil(
               utils::divide_and_ceil(ow_, cfg.im_w_block), cfg.w_threads)
            * cfg.im_w_block);
      }
    }

    if (is_1x1_conv_) {
      if (ic_ >= 256 && oc_ >= 256 && oh_ <= 14) {
        cfg.im_h_block = oh_;
      } else {
        cfg.im_h_block = 1;
        if (oh_ >= 28 && cfg.bs_threads % 2 == 0) {
          cfg.h_threads = 2;
          cfg.bs_threads /= 2;
        }
      }
      if (mb_ == 1 && num_threads == 4) {
        cfg.im_w_block = ow_;
        if (oc_ >= 512 && ic_ >= 512) {
          cfg.bs_threads = 1;
          cfg.h_threads = 1;
          cfg.w_threads = 1;
          cfg.oc_threads = num_threads;
        } else {
          cfg.bs_threads = 1;
          cfg.oc_threads = 1;
          cfg.h_threads = num_threads;
          cfg.w_threads = 1;
          cfg.im_h_block = 1;
        }
      }

      cfg.im_oc_block = std::min(
        utils::get_blocks(oc_, 1, default_block).back(), oc_ / cfg.oc_threads);
      if (cfg.im_h_block == 1 && cfg.im_oc_block == default_block
        && cfg.im_ic_block == default_block) {
        if (ow_ >= 56 && ow_ % 2 == 0) {
          cfg.im_w_block = ow_ / 2;
        } else if (sw_ == 1 && ow_ >= 28 && oc_ >= ic_ && oc_ >= 512) {
          cfg.im_w_block = ow_ / 2;
        } else {
          cfg.im_w_block = ow_;
        }
      }

      cfg.h_block = cfg.h_threads == 1
        ? oh_
        : (utils::divide_and_ceil(
             utils::divide_and_ceil(oh_, cfg.im_h_block), cfg.h_threads)
          * cfg.im_h_block);
    }

    cfg.K_block
      = utils::divide_and_ceil(
          utils::divide_and_ceil(oc_, cfg.im_oc_block), cfg.oc_threads)
      * cfg.im_oc_block;
    if (oc_ % cfg.K_block != 0) { cfg.K_block = cfg.im_oc_block; }

    cfg.C_block = utils::divide_and_ceil(
                    utils::divide_and_ceil(ic_, cfg.im_ic_block), ic_threads)
      * cfg.im_ic_block;
    if (ic_ % cfg.C_block != 0) { cfg.C_block = cfg.im_ic_block; }
  }
  return std::move(ret);
}

gen_nested_conv_fwd_t::gen_nested_conv_fwd_t(sc_op *owner,
  const sc_dims &stride, const sc_dims &pads_begin,
  std::vector<logical_tensor_t> &&ins, std::vector<logical_tensor_t> &&outs)
  : parent(owner, std::move(ins), std::move(outs)) {
  COMPILE_ASSERT(in_tensors_.size() == 2,
    "Wrong number of inputs, expected to be 2 but got " << in_tensors_.size()
                                                        << ".");
  COMPILE_ASSERT(out_tensors_.size() == 1,
    "Wrong number of output, expected to be 1 but got " << out_tensors_.size()
                                                        << ".");

  auto input_plain_dims = get_input_plain_dims();
  auto weight_plain_dims = get_weight_plain_dims();
  auto out_plain_dims = get_output_plain_dims();
  COMPILE_ASSERT(
    utils::is_one_of(static_cast<int>(input_plain_dims.size()), 3, 4, 5),
    "Wrong input dims, expected to be 3D, 4D or 5D input, but got "
      << input_plain_dims.size() << "D.");
  COMPILE_ASSERT(
    utils::is_one_of(static_cast<int>(weight_plain_dims.size()), 3, 4, 5)
      && (weight_plain_dims.size() == input_plain_dims.size()),
    "Wrong weight dims, only support 3D, 4D or 5D weights, but got "
      << weight_plain_dims.size() << "D.");
  COMPILE_ASSERT(
    utils::is_one_of(static_cast<int>(out_plain_dims.size()), 3, 4, 5)
      && (out_plain_dims.size() == input_plain_dims.size()),
    "Wrong output dims, only support 3D, 4D or 5D weights, but got "
      << out_plain_dims.size() << "D.");

  ndims_ = input_plain_dims.size();
  is_3d_ = (ndims_ == 5);
  is_1d_ = (ndims_ == 3);

  blocking_input_ = get_input_blocking_dims().size() > ndims_;
  blocking_output_ = get_output_blocking_dims().size() > ndims_;
  COMPILE_ASSERT(is_3d_
      ? utils::is_one_of(static_cast<int>(pads_begin.size()), 1, 3)
      : utils::is_one_of(static_cast<int>(pads_begin.size()), 1, 2),
    "Wrong pads_begin dims, should be 1D, 2D or 3D, but got "
      << pads_begin.size() << "D.");
  COMPILE_ASSERT(is_3d_
      ? utils::is_one_of(static_cast<int>(stride.size()), 1, 3)
      : utils::is_one_of(static_cast<int>(stride.size()), 1, 2),
    "Wrong stride dims, should be 1D, 2D or 3D, but got " << stride.size()
                                                          << "D.");
  COMPILE_ASSERT(input_plain_dims[1] == weight_plain_dims[1],
    "expect input_plain_dims[1] == weight_plain_dims[1], but got "
      << input_plain_dims[1] << " vs " << weight_plain_dims[1] << ".");

  mb_ = input_plain_dims[0];
  ic_ = input_plain_dims[1];
  id_ = is_3d_ ? input_plain_dims[2] : 1;
  ih_ = is_1d_ ? 1 : input_plain_dims[ndims_ - 2];
  iw_ = input_plain_dims[ndims_ - 1];
  oc_ = weight_plain_dims[0];
  kd_ = is_3d_ ? weight_plain_dims[2] : 1;
  kh_ = is_1d_ ? 1 : weight_plain_dims[ndims_ - 2];
  kw_ = weight_plain_dims[ndims_ - 1];
  od_ = is_3d_ ? out_plain_dims[2] : 1;
  oh_ = is_1d_ ? 1 : out_plain_dims[ndims_ - 2];
  ow_ = out_plain_dims[ndims_ - 1];
  is_1x1_conv_ = (kd_ == 1 && kh_ == 1 && kw_ == 1);
  pd_ = is_3d_ ? pads_begin[0] : 0;
  ph_ = is_1d_ ? 0 : pads_begin[0], pw_ = pads_begin[0];
  if (owner) { attrs_ = owner->attrs_; }
  if (pads_begin.size() > 1) {
    ph_ = pads_begin[ndims_ - 4];
    pw_ = pads_begin[ndims_ - 3];
  }
  sd_ = is_3d_ ? stride[0] : 1;
  sh_ = is_1d_ ? 1 : stride[0], sw_ = stride[0];
  if (stride.size() > 1) {
    auto stride_size = stride.size();
    sh_ = stride[stride_size - 2];
    sw_ = stride[stride_size - 1];
  }

  // For non 1x1 conv and AMX platform, spatial blocking instead of row
  // blocking is used, which needs to consider the border carefully, as the
  // cross row boundary (contains padding or not) will generate useless output
  // which have to be skipped before storing.
  actual_os_ = oh_ * ow_;
  num_elems_skip_per_ow_ = ((kw_ - 1) / sw_) * sh_ + (sh_ - 1) * ow_;
  adj_os_ = std::min(actual_os_ + num_elems_skip_per_ow_ * (oh_ - 1),
    (ih_ + 2 * ph_) * (iw_ + 2 * pw_));

  bool is_int8
    = utils::is_one_of(get_input_dtype(), datatypes::u8, datatypes::s8);
  // Note: os blocking is only valid for non_1x1, no pad and non 3D conv with
  // amx-int8 only so far.
  bool has_pad = (pd_ > 0) || (ph_ > 0) || (pw_ > 0);
  COMPILE_ASSERT(!has_pad, "nested conv with padding has not been supported");
  try_os_blocking_ = (!is_1x1_conv_) && (!has_pad) && (!is_3d_) && is_int8;
  use_nested_2d_ = (!is_1d_ && !is_3d_);
  COMPILE_ASSERT(use_nested_2d_,
    "expect input is 2D in nested conv2d, but got " << ndims_ - 2 << "D input");
}

float gen_nested_conv_fwd_t::get_gflop() const {
  float result = (float)mb_ * oc_ * 2.0 * ic_ * kd_ * kh_ * kw_ * od_ * oh_
    * ow_ / (float)1e9;
  return result;
}

#define CONV_ARG_LIST \
  const context_ptr &ctx, const nested_conv_fwd_config_t &config, \
    fusion_manager *fusion, expr &output, const expr &input, \
    const expr &weight, std::vector<for_loop> &loops, const int os, \
    const int kpack, const bool use_os_blocking, const bool pack_rows, \
    const expr &os_acc_size, const std::vector<char> &os_mask

void gen_nested_conv_fwd_t::compute_1x1_pack_input_nested(CONV_ARG_LIST) const {
  COMPILE_ASSERT(!is_3d_, "1x1 pack input doens't support 3D conv yet!");
  tensor input1;
  int lanes = get_lanes(ctx, config.im_ic_block, get_input_dtype());
  auto toutput = out_tensors_[0];
  auto out_fmt = toutput.get_format();
  auto oh_expr_ = oh_;
  if (!out_fmt.is_any()) {
    auto out_p2b_map = out_fmt.format_code_.collect_p2b_mapping();
    oh_expr_ = static_cast<int>(get_expr_as_int(
      output.checked_as<tensor>()->dims_[out_p2b_map[is_3d_ ? 3 : 2][0]]));
  }
  if (config.pack_input == 1 && (sd_ > 1 || sh_ > 1 || sw_ > 1)) {
    for_loop ln, lk, ld, lp;
    auto mb_expr = input.checked_as<tensor>()->dims_[0];
    if (blocking_input_) {
      // NCHWc
      auto im_c_num_block = ic_ / config.im_ic_block;
      _tensor_(input_tmp, get_input_dtype(),
        {mb_expr, im_c_num_block, oh_expr_, ow_, config.im_ic_block});
      _named_for_(ln, n, 0, mb_expr, 1, for_type::PARALLEL) {
        _named_for_(lk, c_o, 0, im_c_num_block) {
          _named_for_(lp, p, 0, oh_expr_) {
            _for_(q, 0, ow_) {
              _for_(c_i, 0, config.im_ic_block, (int)lanes) {
                input_tmp[span_t({n, c_o, p, q, c_i}, lanes)]
                  = input[span_t({n, c_o, p * sh_, q * sw_, c_i}, lanes)];
              }
            }
          }
        }
      }
      auto lnk = ln->fuse(lk);
      if (im_c_num_block * mb_
        < runtime_config_t::get().get_num_threads() * 2) {
        auto lnkp = lnk->fuse(lp);
      }
      input1 = input_tmp.static_as<tensor>();
    } else {
      _tensor_(input_tmp, get_input_dtype(), {mb_expr, oh_expr_, ow_, ic_});
      _named_for_(ln, n, 0, mb_expr, 1, for_type::PARALLEL) {
        _named_for_(lp, p, 0, oh_expr_) {
          _for_(q, 0, ow_) {
            _for_(c_i, 0, ic_, (int)lanes) {
              input_tmp[span_t({n, p, q, c_i}, lanes)]
                = input[span_t({n, p * sh_, q * sw_, c_i}, lanes)];
            }
          }
        }
      }
      ln = ln->fuse(lp);
      input1 = input_tmp.static_as<tensor>();
    }
  } else {
    input1 = input.static_as<tensor>();
  }

  int num_threads = runtime_config_t::get().get_num_threads();
  int bs_threads = config.bs_threads;
  int h_threads = config.h_threads;
  int w_threads = config.w_threads;
  int oc_threads = config.oc_threads;
  int ic_threads = 1;

  int oc_block = config.K_block;
  int h_block = config.h_block;
  int w_block = config.w_block;
  int ic_block = config.C_block;
  int im_oc_block = config.im_oc_block;
  int im_ic_block = config.im_ic_block;
  int im_h_block = config.im_h_block;
  int im_w_block = config.im_w_block;

  COMPILE_ASSERT(oc_block % im_oc_block == 0,
    "oc_block % im_oc_block != 0, config is invalid")

  COMPILE_ASSERT(ic_block % im_ic_block == 0,
    "ic_block % im_ic_block != 0, config is invalid")

  COMPILE_ASSERT(
    h_block % im_h_block == 0, "h_block % im_h_block != 0, config is invalid")

  COMPILE_ASSERT(
    w_block % im_w_block == 0, "w_block % im_w_block != 0, config is invalid")

  COMPILE_ASSERT(
    w_block % im_w_block == 0, "w_block % im_w_block != 0, config is invalid")

  COMPILE_ASSERT((im_w_block == ow_ || im_h_block == 1),
    "im_w_block or im_h_block config is invalid")

  // param
  expr output_tmp = output;
  auto tinput = in_tensors_[0];
  auto tweight = in_tensors_[1];
  const auto &input_blocking_dims = tinput.get_blocking_dims();
  const auto &weight_blocking_dims = tweight.get_blocking_dims();
  const auto &output_blocking_dims = toutput.get_blocking_dims();

  for_loop lpbs, lph, lpw, lpoc, lpic, loh, low, looc, loic, lioc, lih, liw;

  int oc_num_block_pt, oc_tail_num_block_pt, h_num_block_pt,
    h_tail_num_block_pt, w_num_block_pt, w_tail_num_block_pt, ic_num_block_pt,
    ic_tail_num_block_pt;

  int oc_used_threads = block_split(utils::divide_and_ceil(oc_, oc_block),
    oc_threads, oc_num_block_pt, oc_tail_num_block_pt);

  int oh_used_threads = block_split(utils::divide_and_ceil(oh_, h_block),
    h_threads, h_num_block_pt, h_tail_num_block_pt);

  int ow_used_threads = block_split(utils::divide_and_ceil(ow_, w_block),
    w_threads, w_num_block_pt, w_tail_num_block_pt);

  int ic_used_threads = block_split(utils::divide_and_ceil(ic_, ic_block),
    ic_threads, ic_num_block_pt, ic_tail_num_block_pt);

  if (ic_used_threads > 1) {
    // barrier
    // output temp buffer
    auto out_dims = output_blocking_dims;
    out_dims[0] *= ic_used_threads;
    _tensor_(out_tmp, toutput.dtype_, dims_to_expr(out_dims));
    output_tmp = out_tmp;
  }

  auto input_expr_dims = input1.checked_as<tensor>()->dims_;
  auto mb_expr_ = input_expr_dims[0];

  _named_for_(lpbs, pbs, 0, mb_expr_, 1, for_type::PARALLEL) {
    _named_for_(lph, ph, 0, h_threads, 1) {
      _named_for_(lpw, pw, 0, w_threads, 1) {
        _named_for_(lpoc, poc, 0, oc_threads, 1) {
          _named_for_(lpic, pic, 0, ic_threads, 1) {
            expr h_num_block = builder::make_select(ph < (oh_used_threads - 1),
                   h_num_block_pt, h_tail_num_block_pt),
                 w_num_block = builder::make_select(pw < (ow_used_threads - 1),
                   w_num_block_pt, w_tail_num_block_pt),
                 oc_num_block
              = builder::make_select(poc < (oc_used_threads - 1),
                oc_num_block_pt, oc_tail_num_block_pt);
            _if_(ph < oh_used_threads && pw < ow_used_threads
              && poc < oc_used_threads && pic < ic_used_threads) {
              // single core
              expr ic_num_block
                = builder::make_select(pic < (ic_used_threads - 1),
                  ic_num_block_pt, ic_tail_num_block_pt);

              expr n = pbs;
              _named_for_(loh, o_h, 0, h_num_block_pt) {
                _named_for_(low, o_w, 0, w_num_block_pt) {
                  _named_for_(looc, o_oc, 0, oc_num_block_pt) {
                    _named_for_(loic, o_ic, 0, ic_num_block_pt) {
                      expr cond = o_h < h_num_block && o_w < w_num_block
                        && o_oc < oc_num_block && o_ic < ic_num_block;
                      _if_(cond) {
                        _named_for_(lih, i_h, 0, h_block / im_h_block) {
                          expr h = (ph * h_num_block_pt * h_block / im_h_block
                                     + o_h * h_block / im_h_block + i_h)
                            * im_h_block;
                          _if_(h < oh_expr_) {
                            _named_for_(liw, i_w, 0, w_block / im_w_block) {
                              expr w
                                = (pw * w_num_block_pt * w_block / im_w_block
                                    + o_w * w_block / im_w_block + i_w)
                                * im_w_block;
                              _if_(w < ow_) {
                                _named_for_(
                                  lioc, i_oc, 0, oc_block / im_oc_block) {
                                  _tensor_(A_list, datatypes::pointer,
                                    {ic_block / im_ic_block});
                                  _tensor_(B_list, datatypes::pointer,
                                    {ic_block / im_ic_block});
                                  expr oc = poc * oc_num_block_pt * oc_block
                                      / im_oc_block
                                    + o_oc * oc_block / im_oc_block + i_oc;
                                  _if_(oc * im_oc_block < oc_) {
                                    _for_(i_c, 0, ic_block / im_ic_block) {
                                      expr ic = pic * ic_num_block_pt * ic_block
                                          / im_ic_block
                                        + o_ic * ic_block / im_ic_block + i_c;
                                      _if_(ic * im_ic_block < ic_) {
                                        std::vector<expr> input_pos
                                          = blocking_input_
                                          ? std::vector<expr> {n, ic, h, w, 0}
                                          : std::vector<expr> {
                                            n, h, w, ic * im_ic_block};

                                        A_list[i_c]
                                          = tensor_ptr(input1, input_pos);
                                        B_list[i_c] = tensor_ptr(weight,
                                          kpack > 1 ? std::vector<expr> {oc, ic,
                                            0, 0, 0, 0, 0}
                                                    : std::vector<expr> {
                                                      oc, ic, 0, 0, 0, 0});
                                      }
                                    }
                                    const auto hint_A_size
                                      = im_h_block * im_w_block * ic_block;
                                    const auto hint_B_size
                                      = im_oc_block * ic_block;
                                    const auto hint_C_size
                                      = im_h_block * im_w_block * im_oc_block;
                                    sc_brgemm_attrs_t brg_attrs {
                                      {brgemm::attr_key::max_bs,
                                        ic_block / im_ic_block},
                                      {brgemm::attr_key::hint_expected_A_size,
                                        hint_A_size},
                                      {brgemm::attr_key::hint_expected_B_size,
                                        hint_B_size},
                                      {brgemm::attr_key::hint_expected_C_size,
                                        hint_C_size},
                                      {brgemm::attr_key::use_interleave_stores,
                                        true},
                                      {brgemm::attr_key::use_uker, true}};

                                    auto LDA
                                      = blocking_input_ ? im_ic_block : ic_;
                                    auto LDC
                                      = blocking_output_ ? im_oc_block : oc_;

                                    std::vector<expr> output_pos
                                      = blocking_output_
                                      ? std::vector<expr> {pic * mb_ + n, oc, h,
                                        w, 0}
                                      : std::vector<expr> {
                                        pic * mb_ + n, h, w, oc * im_oc_block};

                                    if (ic_num_block_pt > 1) {
                                      _if_(o_ic == 0) {
                                        builtin::brgemm_init_list_update(A_list,
                                          B_list,
                                          tensor_ptr(output_tmp, output_pos), 1,
                                          im_h_block * im_w_block, im_oc_block,
                                          im_ic_block, LDA, im_oc_block, LDC,
                                          1 /*useless*/
                                          ,
                                          1 /*useless*/
                                          ,
                                          ic_block / im_ic_block,
                                          get_input_dtype(), get_weight_dtype(),
                                          brg_attrs);
                                      }
                                      _else_ {
                                        builtin::brgemm_list_update(A_list,
                                          B_list,
                                          tensor_ptr(output_tmp, output_pos), 1,
                                          im_h_block * im_w_block, im_oc_block,
                                          im_ic_block, LDA, im_oc_block, LDC,
                                          1 /*useless*/
                                          ,
                                          1 /*useless*/
                                          ,
                                          ic_block / im_ic_block,
                                          get_input_dtype(), get_weight_dtype(),
                                          brg_attrs);
                                      }
                                    } else {
                                      builtin::brgemm_init_list_update(A_list,
                                        B_list,
                                        tensor_ptr(output_tmp, output_pos), 1,
                                        im_h_block * im_w_block, im_oc_block,
                                        im_ic_block, LDA, im_oc_block, LDC,
                                        1 /*useless*/
                                        ,
                                        1 /*useless*/
                                        ,
                                        ic_block / im_ic_block,
                                        get_input_dtype(), get_weight_dtype(),
                                        brg_attrs);
                                    }

                                    if (fusion && ic_used_threads == 1
                                      && ic_num_block_pt == 1) {
                                      _if_(o_ic == (ic_num_block - 1)) {
                                        fusion->create_output_fusion_anchor(
                                          {blocking_output_
                                              ? tensor_slice(output,
                                                {{n, 1UL}, {oc, 1},
                                                  {h, im_h_block},
                                                  {w, im_w_block},
                                                  {0, im_oc_block}})
                                              : tensor_slice(output,
                                                {{n, 1UL}, {h, im_h_block},
                                                  {w, im_w_block},
                                                  {oc * im_oc_block,
                                                    im_oc_block}})});
                                      }
                                    }
                                  }
                                }
                                if (fusion && ic_used_threads == 1
                                  && ic_num_block_pt == 1
                                  && oc_block * oc_used_threads == oc_) {
                                  _if_(o_ic == (ic_num_block - 1)) {
                                    expr anch_c = poc * oc_num_block_pt
                                        * oc_block / im_oc_block
                                      + o_oc * oc_block / im_oc_block;
                                    fusion->create_output_fusion_anchor(
                                      {blocking_output_
                                          ? tensor_slice(output,
                                            {{n, 1UL}, {anch_c, 1},
                                              {h, im_h_block}, {w, im_w_block},
                                              {0, im_oc_block}})
                                          : tensor_slice(output,
                                            {{n, 1UL}, {h, im_h_block},
                                              {w, im_w_block},
                                              {anch_c * im_oc_block,
                                                oc_block}})});
                                  }
                                }
                              }
                            }

                            if (fusion && ic_used_threads == 1
                              && ic_num_block_pt == 1
                              && oc_block * oc_used_threads == oc_
                              && w_block * ow_used_threads == ow_) {
                              _if_(o_ic == (ic_num_block - 1)) {
                                expr anch_c = poc * oc_num_block_pt * oc_block
                                    / im_oc_block
                                  + o_oc * oc_block / im_oc_block;
                                expr anch_w
                                  = (pw * w_num_block_pt * w_block / im_w_block
                                      + o_w * w_block / im_w_block)
                                  * im_w_block;
                                fusion->create_output_fusion_anchor(
                                  {blocking_output_
                                      ? tensor_slice(output,
                                        {{n, 1UL}, {anch_c, 1}, {h, im_h_block},
                                          {anch_w, w_block}, {0, im_oc_block}})
                                      : tensor_slice(output,
                                        {{n, 1UL}, {h, im_h_block},
                                          {anch_w, w_block},
                                          {anch_c * im_oc_block, oc_block}})});
                              }
                            }
                          }
                        }

                        if (fusion && ic_used_threads == 1
                          && ic_num_block_pt == 1
                          && oc_block * oc_used_threads == oc_
                          && w_block * ow_used_threads == ow_
                          && h_block * oh_used_threads == oh_) {
                          _if_(o_ic == (ic_num_block - 1)) {
                            expr anch_c
                              = poc * oc_num_block_pt * oc_block / im_oc_block
                              + o_oc * oc_block / im_oc_block;
                            expr anch_h
                              = (ph * h_num_block_pt * h_block / im_h_block
                                  + o_h * h_block / im_h_block)
                              * im_h_block;
                            expr anch_w
                              = (pw * w_num_block_pt * w_block / im_w_block
                                  + o_w * w_block / im_w_block)
                              * im_w_block;

                            fusion->create_output_fusion_anchor(
                              {blocking_output_
                                  ? tensor_slice(output,
                                    {{n, 1UL}, {anch_c, 1}, {anch_h, h_block},
                                      {anch_w, w_block}, {0, im_oc_block}})
                                  : tensor_slice(output,
                                    {{n, 1UL}, {anch_h, h_block},
                                      {anch_w, w_block},
                                      {anch_c * im_oc_block, oc_block}})});
                          }
                        }
                      }
                    }
                    // TODO(xurui): need to add iterated anchor here to
                    // support more fusion opportunity
                  }
                }
              }
            }

            if (fusion && oc_threads == 1 && h_threads == 1 && w_threads == 1
              && ic_threads == 1) {
              fusion->create_output_fusion_anchor({blocking_output_
                  ? tensor_slice(output,
                    {{pbs, 1UL}, {0, oc_ / im_oc_block}, {0, oh_expr_},
                      {0, ow_}, {0, im_oc_block}})
                  : tensor_slice(
                    output, {{pbs, 1UL}, {0, oh_expr_}, {0, ow_}, {0, oc_}})});
            }
          }
          if (fusion && oc_threads == 1 && h_threads == 1 && w_threads == 1) {
            fusion->create_output_fusion_anchor({blocking_output_
                ? tensor_slice(output,
                  {{pbs, 1UL}, {0, oc_ / im_oc_block}, {0, oh_expr_}, {0, ow_},
                    {0, im_oc_block}})
                : tensor_slice(
                  output, {{pbs, 1UL}, {0, oh_expr_}, {0, ow_}, {0, oc_}})});
          }
        }
        if (fusion && h_threads == 1 && w_threads == 1) {
          fusion->create_output_fusion_anchor({blocking_output_
              ? tensor_slice(output,
                {{pbs, 1UL}, {0, oc_ / im_oc_block}, {0, oh_expr_}, {0, ow_},
                  {0, im_oc_block}})
              : tensor_slice(
                output, {{pbs, 1UL}, {0, oh_expr_}, {0, ow_}, {0, oc_}})});
        }
      }

      if (fusion && h_threads == 1) {
        fusion->create_output_fusion_anchor({blocking_output_
            ? tensor_slice(output,
              {{pbs, 1UL}, {0, oc_ / im_oc_block}, {0, oh_expr_}, {0, ow_},
                {0, im_oc_block}})
            : tensor_slice(
              output, {{pbs, 1UL}, {0, oh_expr_}, {0, ow_}, {0, oc_}})});
      }
    }
    if (fusion && mb_ > 1) {
      fusion->create_output_fusion_anchor(
        {blocking_output_ ? tensor_slice(output,
           {{pbs, 1UL}, {0, oc_ / im_oc_block}, {0, oh_expr_}, {0, ow_},
             {0, im_oc_block}})
                          : tensor_slice(output,
                            {{pbs, 1UL}, {0, oh_expr_}, {0, ow_}, {0, oc_}})});
    }
  }
  loops = {lpbs, lph, lpw, lpoc, lpic};
}

void gen_nested_conv_fwd_t::compute_1x1_no_pack_input_nested(
  CONV_ARG_LIST) const {
  int bs_threads = config.bs_threads;
  int h_threads = config.h_threads;
  int w_threads = config.w_threads;
  int oc_threads = config.oc_threads;
  int ic_threads = 1;

  int oc_block = config.K_block;
  int h_block = config.h_block;
  int w_block = config.w_block;
  int ic_block = config.C_block;
  int im_oc_block = config.im_oc_block;
  int im_ic_block = config.im_ic_block;
  int im_h_block = config.im_h_block;
  int im_w_block = config.im_w_block;

  COMPILE_ASSERT(oc_block % im_oc_block == 0,
    "oc_block % im_oc_block != 0, config is invalid")

  COMPILE_ASSERT(ic_block % im_ic_block == 0,
    "ic_block % im_ic_block != 0, config is invalid")

  COMPILE_ASSERT(
    h_block % im_h_block == 0, "h_block % im_h_block != 0, config is invalid")

  COMPILE_ASSERT(
    w_block % im_w_block == 0, "w_block % im_w_block != 0, config is invalid")

  COMPILE_ASSERT(
    w_block % im_w_block == 0, "w_block % im_w_block != 0, config is invalid")

  // param
  expr output_tmp = output;
  auto tinput = in_tensors_[0];
  auto tweight = in_tensors_[1];
  auto toutput = out_tensors_[0];
  const auto &input_blocking_dims = tinput.get_blocking_dims();
  const auto &weight_blocking_dims = tweight.get_blocking_dims();
  const auto &output_blocking_dims = toutput.get_blocking_dims();
  auto out_fmt = toutput.get_format();
  auto oh_expr_ = oh_;
  if (!out_fmt.is_any()) {
    auto out_p2b_map = out_fmt.format_code_.collect_p2b_mapping();
    oh_expr_ = static_cast<int>(get_expr_as_int(
      output.checked_as<tensor>()->dims_[out_p2b_map[is_3d_ ? 3 : 2][0]]));
  }

  for_loop lpbs, lph, lpw, lpoc, lpic, loh, low, looc, loic, lioc, lih, liw;

  int oc_num_block_pt, oc_tail_num_block_pt, h_num_block_pt,
    h_tail_num_block_pt, w_num_block_pt, w_tail_num_block_pt, ic_num_block_pt,
    ic_tail_num_block_pt;

  int oc_used_threads = block_split(utils::divide_and_ceil(oc_, oc_block),
    oc_threads, oc_num_block_pt, oc_tail_num_block_pt);

  int oh_used_threads = block_split(utils::divide_and_ceil(oh_, h_block),
    h_threads, h_num_block_pt, h_tail_num_block_pt);

  int ow_used_threads = block_split(utils::divide_and_ceil(ow_, w_block),
    w_threads, w_num_block_pt, w_tail_num_block_pt);

  int ic_used_threads = block_split(utils::divide_and_ceil(ic_, ic_block),
    ic_threads, ic_num_block_pt, ic_tail_num_block_pt);

  if (ic_used_threads > 1) {
    // barrier
    // output temp buffer
    auto out_dims = output_blocking_dims;
    out_dims[0] *= ic_used_threads;
    _tensor_(out_tmp, toutput.dtype_, dims_to_expr(out_dims));
    output_tmp = out_tmp;
  }

  auto input_expr_dims = input.checked_as<tensor>()->dims_;
  auto mb_expr_ = input_expr_dims[0];

  _named_for_(lpbs, pbs, 0, mb_expr_, 1, for_type::PARALLEL) {
    _named_for_(lph, ph, 0, h_threads, 1) {
      _named_for_(lpw, pw, 0, w_threads, 1) {
        _named_for_(lpoc, poc, 0, oc_threads, 1) {
          _named_for_(lpic, pic, 0, ic_threads, 1) {
            expr h_num_block = builder::make_select(ph < (oh_used_threads - 1),
                   h_num_block_pt, h_tail_num_block_pt),

                 w_num_block = builder::make_select(pw < (ow_used_threads - 1),
                   w_num_block_pt, w_tail_num_block_pt),

                 oc_num_block
              = builder::make_select(poc < (oc_used_threads - 1),
                oc_num_block_pt, oc_tail_num_block_pt);
            _if_(ph < oh_used_threads && pw < ow_used_threads
              && poc < oc_used_threads && pic < ic_used_threads) {
              // single core
              expr ic_num_block
                = builder::make_select(pic < (ic_used_threads - 1),
                  ic_num_block_pt, ic_tail_num_block_pt);

              expr n = pbs;
              _named_for_(loh, o_h, 0, h_num_block_pt) {
                _named_for_(low, o_w, 0, w_num_block_pt) {
                  _named_for_(looc, o_oc, 0, oc_num_block_pt) {
                    _named_for_(loic, o_ic, 0, ic_num_block_pt) {
                      expr cond = o_h < h_num_block && o_w < w_num_block
                        && o_oc < oc_num_block && o_ic < ic_num_block;
                      _if_(cond) {
                        _named_for_(lih, i_h, 0, h_block / im_h_block) {
                          expr h = (ph * h_num_block_pt * h_block / im_h_block
                                     + o_h * h_block / im_h_block + i_h)
                            * im_h_block;
                          _named_for_(liw, i_w, 0, w_block / im_w_block) {
                            expr w = (pw * w_num_block_pt * w_block / im_w_block
                                       + o_w * w_block / im_w_block + i_w)
                              * im_w_block;
                            _if_(w < ow_) {
                              _named_for_(
                                lioc, i_oc, 0, oc_block / im_oc_block) {
                                expr oc = poc * oc_num_block_pt * oc_block
                                    / im_oc_block
                                  + o_oc * oc_block / im_oc_block + i_oc;
                                _if_(oc * im_oc_block < oc_) {
                                  _for_(im_h_i, 0, im_h_block) {
                                    _if_(h + im_h_i < oh_expr_) {
                                      _tensor_(A_list, datatypes::pointer,
                                        {ic_block / im_ic_block});
                                      _tensor_(B_list, datatypes::pointer,
                                        {ic_block / im_ic_block});

                                      _for_(i_c, 0, ic_block / im_ic_block) {
                                        expr ic = pic * ic_num_block_pt
                                            * ic_block / im_ic_block
                                          + o_ic * ic_block / im_ic_block + i_c;
                                        _if_(ic * im_ic_block < ic_) {
                                          std::vector<expr> input_pos
                                            = blocking_input_
                                            ? std::vector<expr> {n, ic,
                                              (h + im_h_i) * sh_, w * sw_, 0}
                                            : std::vector<expr> {n,
                                              (h + im_h_i) * sh_, w * sw_,
                                              ic * im_ic_block};

                                          A_list[i_c]
                                            = tensor_ptr(input, input_pos);
                                          B_list[i_c] = tensor_ptr(weight,
                                            kpack > 1 ? std::vector<expr> {oc,
                                              ic, 0, 0, 0, 0, 0}
                                                      : std::vector<expr> {
                                                        oc, ic, 0, 0, 0, 0});
                                        }
                                      }
                                      const auto hint_A_size
                                        = im_w_block * ic_block;
                                      const auto hint_B_size
                                        = im_oc_block * ic_block;
                                      const auto hint_C_size
                                        = im_w_block * im_oc_block;

                                      sc_brgemm_attrs_t brg_attrs {
                                        {brgemm::attr_key::max_bs,
                                          ic_block / im_ic_block},
                                        {brgemm::attr_key::hint_expected_A_size,
                                          hint_A_size},
                                        {brgemm::attr_key::hint_expected_B_size,
                                          hint_B_size},
                                        {brgemm::attr_key::hint_expected_C_size,
                                          hint_C_size},
                                        {brgemm::attr_key::
                                            use_interleave_stores,
                                          true},
                                        {brgemm::attr_key::use_uker, true}};

                                      auto LDA = blocking_input_
                                        ? sw_ * im_ic_block
                                        : sw_ * ic_;
                                      auto LDC
                                        = blocking_output_ ? im_oc_block : oc_;

                                      std::vector<expr> output_pos
                                        = blocking_output_
                                        ? std::vector<expr> {pic * mb_ + n, oc,
                                          h + im_h_i, w, 0}
                                        : std::vector<expr> {pic * mb_ + n,
                                          h + im_h_i, w, oc * im_oc_block};

                                      if (ic_num_block_pt > 1) {
                                        _if_(o_ic == 0) {
                                          builtin::brgemm_init_list_update(
                                            A_list, B_list,
                                            tensor_ptr(output_tmp, output_pos),
                                            1, im_w_block, im_oc_block,
                                            im_ic_block, LDA, im_oc_block, LDC,
                                            1 /*useless*/
                                            ,
                                            1 /*useless*/
                                            ,
                                            ic_block / im_ic_block,
                                            get_input_dtype(),
                                            get_weight_dtype(), brg_attrs);
                                        }
                                        _else_ {
                                          builtin::brgemm_list_update(A_list,
                                            B_list,
                                            tensor_ptr(output_tmp, output_pos),
                                            1, im_w_block, im_oc_block,
                                            im_ic_block, LDA, im_oc_block, LDC,
                                            1 /*useless*/
                                            ,
                                            1 /*useless*/
                                            ,
                                            ic_block / im_ic_block,
                                            get_input_dtype(),
                                            get_weight_dtype(), brg_attrs);
                                        }
                                      } else {
                                        builtin::brgemm_init_list_update(A_list,
                                          B_list,
                                          tensor_ptr(output_tmp, output_pos), 1,
                                          im_w_block, im_oc_block, im_ic_block,
                                          LDA, im_oc_block, LDC, 1 /*useless*/
                                          ,
                                          1 /*useless*/
                                          ,
                                          ic_block / im_ic_block,
                                          get_input_dtype(), get_weight_dtype(),
                                          brg_attrs);
                                      }

                                      if (fusion && ic_used_threads == 1
                                        && ic_num_block_pt == 1) {
                                        _if_(o_ic == (ic_num_block - 1)) {
                                          fusion->create_output_fusion_anchor(
                                            {blocking_output_
                                                ? tensor_slice(output,
                                                  {{n, 1UL}, {oc, 1},
                                                    {h + im_h_i, 1},
                                                    {w, im_w_block},
                                                    {0, im_oc_block}})
                                                : tensor_slice(output,
                                                  {{n, 1UL}, {h + im_h_i, 1},
                                                    {w, im_w_block},
                                                    {oc * im_oc_block,
                                                      im_oc_block}})});
                                        }
                                      }
                                    }
                                  }

                                  if (fusion && ic_used_threads == 1) {
                                    _if_(o_ic == (ic_num_block - 1)) {
                                      fusion->create_output_fusion_anchor(
                                        {blocking_output_
                                            ? tensor_slice(output,
                                              {{n, 1UL}, {oc, 1},
                                                {h, im_h_block},
                                                {w, im_w_block},
                                                {0, im_oc_block}})
                                            : tensor_slice(output,
                                              {{n, 1UL}, {h, im_h_block},
                                                {w, im_w_block},
                                                {oc * im_oc_block,
                                                  im_oc_block}})});
                                    }
                                  }
                                }
                              }
                              if (fusion && ic_used_threads == 1
                                && ic_num_block_pt == 1
                                && oc_block * oc_used_threads == oc_) {
                                _if_(o_ic == (ic_num_block - 1)) {
                                  expr anch_c = poc * oc_num_block_pt * oc_block
                                      / im_oc_block
                                    + o_oc * oc_block / im_oc_block;
                                  fusion->create_output_fusion_anchor(
                                    {blocking_output_ ? tensor_slice(output,
                                       {{n, 1UL}, {anch_c, 1}, {h, im_h_block},
                                         {w, im_w_block}, {0, im_oc_block}})
                                                      : tensor_slice(output,
                                                        {{n, 1UL},
                                                          {h, im_h_block},
                                                          {w, im_w_block},
                                                          {anch_c * im_oc_block,
                                                            oc_block}})});
                                }
                              }
                            }
                          }

                          if (fusion && ic_used_threads == 1
                            && ic_num_block_pt == 1
                            && oc_block * oc_used_threads == oc_
                            && w_block * ow_used_threads == ow_) {
                            _if_(o_ic == (ic_num_block - 1)) {
                              expr anch_c
                                = poc * oc_num_block_pt * oc_block / im_oc_block
                                + o_oc * oc_block / im_oc_block;
                              expr anch_w
                                = (pw * w_num_block_pt * w_block / im_w_block
                                    + o_w * w_block / im_w_block)
                                * im_w_block;
                              fusion->create_output_fusion_anchor(
                                {blocking_output_
                                    ? tensor_slice(output,
                                      {{n, 1UL}, {anch_c, 1}, {h, im_h_block},
                                        {anch_w, w_block}, {0, im_oc_block}})
                                    : tensor_slice(output,
                                      {{n, 1UL}, {h, im_h_block},
                                        {anch_w, w_block},
                                        {anch_c * im_oc_block, oc_block}})});
                            }
                          }
                        }

                        if (fusion && ic_used_threads == 1
                          && ic_num_block_pt == 1
                          && oc_block * oc_used_threads == oc_
                          && w_block * ow_used_threads == ow_
                          && h_block * oh_used_threads == oh_) {
                          _if_(o_ic == (ic_num_block - 1)) {
                            expr anch_c
                              = (poc * oc_num_block_pt * oc_block / im_oc_block
                                + o_oc * oc_block / im_oc_block);
                            expr anch_h
                              = (ph * h_num_block_pt * h_block / im_h_block
                                  + o_h * h_block / im_h_block)
                              * im_h_block;
                            expr anch_w
                              = (pw * w_num_block_pt * w_block / im_w_block
                                  + o_w * w_block / im_w_block)
                              * im_w_block;
                            fusion->create_output_fusion_anchor(
                              {blocking_output_
                                  ? tensor_slice(output,
                                    {{n, 1UL}, {anch_c, 1}, {anch_h, h_block},
                                      {anch_w, w_block}, {0, im_oc_block}})
                                  : tensor_slice(output,
                                    {{n, 1UL}, {anch_h, h_block},
                                      {anch_w, w_block},
                                      {anch_c * im_oc_block, oc_block}})});
                          }
                        }
                      }
                    }
                    // TODO(xurui): need to add iterated anchor here to
                    // support more fusion opportunity
                  }
                }
              }
            }

            if (fusion && oc_threads == 1 && ic_threads == 1 && h_threads == 1
              && w_threads == 1) {
              fusion->create_output_fusion_anchor({blocking_output_
                  ? tensor_slice(output,
                    {{pbs, 1UL}, {0, oc_ / im_oc_block}, {0, oh_expr_},
                      {0, ow_}, {0, im_oc_block}})
                  : tensor_slice(
                    output, {{pbs, 1UL}, {0, oh_expr_}, {0, ow_}, {0, oc_}})});
            }
          }
          if (fusion && oc_threads == 1 && h_threads == 1 && w_threads == 1) {
            fusion->create_output_fusion_anchor({blocking_output_
                ? tensor_slice(output,
                  {{pbs, 1UL}, {0, oc_ / im_oc_block}, {0, oh_expr_}, {0, ow_},
                    {0, im_oc_block}})
                : tensor_slice(
                  output, {{pbs, 1UL}, {0, oh_expr_}, {0, ow_}, {0, oc_}})});
          }
        }

        if (fusion && h_threads == 1 && w_threads == 1) {
          fusion->create_output_fusion_anchor({blocking_output_
              ? tensor_slice(output,
                {{pbs, 1UL}, {0, oc_ / im_oc_block}, {0, oh_expr_}, {0, ow_},
                  {0, im_oc_block}})
              : tensor_slice(
                output, {{pbs, 1UL}, {0, oh_expr_}, {0, ow_}, {0, oc_}})});
        }
      }

      if (fusion && h_threads == 1) {
        fusion->create_output_fusion_anchor({blocking_output_
            ? tensor_slice(output,
              {{pbs, 1UL}, {0, oc_ / im_oc_block}, {0, oh_expr_}, {0, ow_},
                {0, im_oc_block}})
            : tensor_slice(
              output, {{pbs, 1UL}, {0, oh_expr_}, {0, ow_}, {0, oc_}})});
      }
    }
    if (fusion && mb_ > 1) {
      fusion->create_output_fusion_anchor(
        {blocking_output_ ? tensor_slice(output,
           {{pbs, 1UL}, {0, oc_ / im_oc_block}, {0, oh_expr_}, {0, ow_},
             {0, im_oc_block}})
                          : tensor_slice(output,
                            {{pbs, 1UL}, {0, oh_expr_}, {0, ow_}, {0, oc_}})});
    }
  }
  loops = {lpbs, lph, lpw, lpoc, lpic};
}

void gen_nested_conv_fwd_t::compute_conv_no_padding_os_blocking_nested(
  CONV_ARG_LIST) const {
  COMPILE_ASSERT(
    pack_rows, "Use nested conv with os blocking only if pack_rows is true")
  int bs_threads = config.bs_threads;
  int s_threads = config.w_threads;
  int oc_threads = config.oc_threads;
  int ic_threads = 1;

  int oc_block = config.K_block;
  int s_block = config.w_block;
  int ic_block = config.C_block;

  int im_oc_block = config.im_oc_block;
  int im_ic_block = config.im_ic_block;
  int im_s_block = config.im_w_block;

  COMPILE_ASSERT(oc_block % im_oc_block == 0,
    "oc_block % im_oc_block != 0, config is invalid")
  COMPILE_ASSERT(ic_block % im_ic_block == 0,
    "ic_block % im_ic_block != 0, config is invalid");
  COMPILE_ASSERT(
    s_block % im_s_block == 0, "s_block % im_s_block != 0, config is invalid");

  // param
  expr output_tmp = output;
  auto tinput = in_tensors_[0];
  auto tweight = in_tensors_[1];
  auto toutput = out_tensors_[0];
  const auto &input_blocking_dims = tinput.get_blocking_dims();
  const auto &weight_blocking_dims = tweight.get_blocking_dims();
  const auto &output_blocking_dims = toutput.get_blocking_dims();

  for_loop lpbs, lps, lpoc, lpic, los, looc, loic, lioc, lis, lok;

  int bs_num_block_pt, bs_tail_num_block_pt, oc_num_block_pt,
    oc_tail_num_block_pt, s_num_block_pt, s_tail_num_block_pt, ic_num_block_pt,
    ic_tail_num_block_pt;
  int bs_used_threads
    = block_split(mb_, bs_threads, bs_num_block_pt, bs_tail_num_block_pt);
  int oc_used_threads = block_split(utils::divide_and_ceil(oc_, oc_block),
    oc_threads, oc_num_block_pt, oc_tail_num_block_pt);
  int os_used_threads = block_split(utils::divide_and_ceil(os, s_block),
    s_threads, s_num_block_pt, s_tail_num_block_pt);
  int ic_used_threads = block_split(utils::divide_and_ceil(ic_, ic_block),
    ic_threads, ic_num_block_pt, ic_tail_num_block_pt);

  auto input_expr_dims = input.checked_as<tensor>()->dims_;
  auto mb_expr_ = input_expr_dims[0];

  if (ic_used_threads > 1) {
    // barrier
    // output temp buffer
    auto out_dims = output_blocking_dims;
    out_dims[0] *= ic_used_threads;
    _tensor_(out_tmp, toutput.dtype_, dims_to_expr(out_dims));
    output_tmp = out_tmp;
  }
  auto LDA = blocking_input_ ? sw_ * im_ic_block : sw_ * ic_;
  auto LDC = blocking_output_ ? im_oc_block : oc_;

  int oc_split = 1;
  auto nthreads = runtime_config_t::get().get_num_threads();
  bool parallel_space_is_enough
    = (mb_ % nthreads == 0 || utils::divide_and_ceil(mb_, nthreads) > 8);
  auto weight_size
    = math_utils::get_dims_product(in_tensors_[1].get_blocking_dims())
    * utils::get_sizeof_type(get_weight_dtype());
  auto L2_cache_size = ctx->machine_.cpu_flags_.getDCacheSize(2);
  if (weight_size >= L2_cache_size && parallel_space_is_enough
    && oc_threads == 1 && oc_num_block_pt == 1) {
    int num_block = oc_block / im_oc_block;
    int expected_split_num = utils::divide_and_ceil(weight_size, L2_cache_size);
    for (auto &factor : utils::get_factors(num_block)) {
      if (factor >= expected_split_num) {
        expected_split_num = factor;
        break;
      }
    }
    oc_split = num_block < expected_split_num ? 1 : expected_split_num;
  }

  _named_for_(lok, outer_k, 0, oc_split, 1, for_type::PARALLEL) {
    _named_for_(lpbs, pbs, 0, mb_expr_, 1, for_type::PARALLEL) {
      _named_for_(lps, ps, 0, s_threads, 1) {
        _named_for_(lpoc, poc, 0, oc_threads, 1) {
          _named_for_(lpic, pic, 0, ic_threads, 1) {
            expr s_num_block = builder::make_select(ps < (os_used_threads - 1),
                   s_num_block_pt, s_tail_num_block_pt),
                 oc_num_block
              = builder::make_select(poc < (oc_used_threads - 1),
                oc_num_block_pt, oc_tail_num_block_pt);
            _if_(ps < os_used_threads && poc < oc_used_threads
              && pic < ic_used_threads) {
              // single core
              expr ic_num_block
                = builder::make_select(pic < (ic_used_threads - 1),
                  ic_num_block_pt, ic_tail_num_block_pt);

              expr n = pbs;
              _named_for_(los, o_s, 0, s_num_block_pt) {
                _named_for_(looc, o_oc, 0, oc_num_block_pt) {
                  _named_for_(loic, o_ic, 0, ic_num_block_pt) {
                    expr cond = o_s < s_num_block && o_oc < oc_num_block
                      && o_ic < ic_num_block;
                    _if_(cond) {
                      _named_for_(
                        lioc, i_oc, 0, oc_block / im_oc_block / oc_split) {
                        expr oc = poc * oc_num_block_pt * oc_block / im_oc_block
                          + o_oc * oc_block / im_oc_block
                          + outer_k * oc_block / im_oc_block / oc_split + i_oc;

                        _if_(oc * im_oc_block < oc_) {
                          _named_for_(lis, i_s, 0, s_block / im_s_block) {
                            _tensor_(A_list, datatypes::pointer,
                              {kh_ * kw_ * ic_block / im_ic_block});
                            _tensor_(B_list, datatypes::pointer,
                              {kh_ * kw_ * ic_block / im_ic_block});
                            auto im_s_block_idx
                              = ps * s_num_block_pt * s_block / im_s_block
                              + o_s * s_block / im_s_block + i_s;

                            auto out_tsr = tensor_ptr(output,
                              blocking_output_
                                ? std::vector<expr> {n, oc,
                                  (im_s_block_idx * im_s_block) / ow_,
                                  im_s_block_idx * im_s_block % ow_, 0}
                                : std::vector<expr> {n,
                                  (im_s_block_idx * im_s_block) / ow_,
                                  (im_s_block_idx * im_s_block) % ow_,
                                  oc * im_oc_block});

                            int adj_ow = ow_ + num_elems_skip_per_ow_;

                            if (os / im_s_block == 1) {
                              out_tsr = tensor_ptr(output,
                                blocking_output_
                                  ? std::vector<expr> {n, oc, 0, 0, 0}
                                  : std::vector<expr> {
                                    n, 0, 0, oc * config.im_oc_block});
                            } else {
                              auto acc_m = os_acc_size[{im_s_block_idx}];
                              out_tsr = tensor_ptr(output,
                                blocking_output_
                                  ? std::vector<expr> {n, oc, acc_m / ow_,
                                    acc_m % ow_, 0}
                                  : std::vector<expr> {n, acc_m / ow_,
                                    acc_m % ow_, oc * im_oc_block});
                            }

                            _for_(i_c, 0, ic_block / im_ic_block) {
                              expr ic
                                = pic * ic_num_block_pt * ic_block / im_ic_block
                                + o_ic * ic_block / im_ic_block + i_c;
                              _if_(ic * im_ic_block < ic_) {
                                _for_(r, 0, kh_) {
                                  _for_(s, 0, kw_) {
                                    auto idx = i_c * kh_ * kw_ + r * kw_ + s;
                                    auto h = ((im_s_block_idx * im_s_block)
                                      / adj_ow);
                                    auto w = ((im_s_block_idx * im_s_block)
                                      % adj_ow);
                                    std::vector<expr> input_pos
                                      = blocking_input_
                                      ? std::vector<expr> {n, ic, h * sh_ + r,
                                        w * sw_ + s, 0}
                                      : std::vector<expr> {n, h * sh_ + r,
                                        w * sw_ + s, ic * im_ic_block};

                                    A_list[idx] = tensor_ptr(input, input_pos);
                                    B_list[idx] = tensor_ptr(weight,
                                      kpack > 1 ? std::vector<expr> {oc, ic, r,
                                        s, 0, 0, 0}
                                                : std::vector<expr> {
                                                  oc, ic, r, s, 0, 0});
                                  }
                                }
                              }
                            }
                            const auto hint_A_size = im_s_block * im_ic_block
                              * kh_ * kw_ * ic_block / im_ic_block;
                            const auto hint_B_size
                              = im_oc_block * ic_block * kh_ * kw_;
                            const auto hint_C_size = im_s_block * im_oc_block;

                            sc_brgemm_attrs_t brg_attrs {
                              {brgemm::attr_key::max_bs,
                                kh_ * kw_ * ic_block / im_ic_block},
                              {brgemm::attr_key::hint_expected_A_size,
                                hint_A_size},
                              {brgemm::attr_key::hint_expected_B_size,
                                hint_B_size},
                              {brgemm::attr_key::hint_expected_C_size,
                                hint_C_size},
                              {brgemm::attr_key::use_interleave_stores, true},
                              {brgemm::attr_key::use_uker, true},
                              {brgemm::attr_key::bd_mask_level, 2}};

                            builtin::brgemm_init_list_update(A_list, B_list,
                              out_tsr, 1, im_s_block, im_oc_block, im_ic_block,
                              LDA, im_oc_block, LDC, 1 /*useless*/,
                              1 /*useless*/, kh_ * kw_ * ic_block / im_ic_block,
                              get_input_dtype(), get_weight_dtype(), brg_attrs,
                              os_mask, im_s_block_idx, os / im_s_block);

                            if (fusion && ic_used_threads == 1
                              && ic_num_block_pt == 1) {
                              _if_(o_ic == (ic_num_block - 1)) {
                                auto os_num_block = os / im_s_block;
                                if (oh_ % os_num_block == 0) {
                                  fusion->create_output_fusion_anchor(
                                    {blocking_output_
                                        ? tensor_slice(output,
                                          {{n, 1UL}, {oc, 1},
                                            {im_s_block_idx
                                                * (oh_ / os_num_block),
                                              (oh_ / os_num_block)},
                                            {0, ow_}, {0, im_oc_block}})
                                        : tensor_slice(output,
                                          {{n, 1UL},
                                            {im_s_block_idx
                                                * (oh_ / os_num_block),
                                              (oh_ / os_num_block)},
                                            {0, ow_},
                                            {oc * im_oc_block, im_oc_block}})});
                                }
                              }
                            }
                          }
                        }
                      }
                    }
                  }
                }
              }
            }

            if (fusion && oc_threads == 1 && ic_threads == 1
              && s_threads == 1) {
              fusion->create_output_fusion_anchor({blocking_output_
                  ? tensor_slice(output,
                    {{pbs, 1UL},
                      {outer_k * oc_ / im_oc_block / oc_split,
                        oc_ / im_oc_block / oc_split},
                      {0, oh_}, {0, ow_}, {0, im_oc_block}})
                  : tensor_slice(output,
                    {{pbs, 1UL}, {0, oh_}, {0, ow_},
                      {outer_k * oc_ / oc_split, oc_ / oc_split}})});
            }
          }

          if (fusion && oc_threads == 1 && s_threads == 1) {
            fusion->create_output_fusion_anchor({blocking_output_
                ? tensor_slice(output,
                  {{pbs, 1UL},
                    {outer_k * oc_ / im_oc_block / oc_split,
                      oc_ / im_oc_block / oc_split},
                    {0, oh_}, {0, ow_}, {0, im_oc_block}})
                : tensor_slice(output,
                  {{pbs, 1UL}, {0, oh_}, {0, ow_},
                    {outer_k * oc_ / oc_split, oc_ / oc_split}})});
          }
        }
        if (fusion && s_threads == 1) {
          fusion->create_output_fusion_anchor({blocking_output_
              ? tensor_slice(output,
                {{pbs, 1UL},
                  {outer_k * oc_ / im_oc_block / oc_split,
                    oc_ / im_oc_block / oc_split},
                  {0, oh_}, {0, ow_}, {0, im_oc_block}})
              : tensor_slice(output,
                {{pbs, 1UL}, {0, oh_}, {0, ow_},
                  {outer_k * oc_ / oc_split, oc_ / oc_split}})});
        }
      }
      if (fusion && mb_ > 1) {
        fusion->create_output_fusion_anchor(
          {blocking_output_ ? tensor_slice(output,
             {{pbs, 1UL},
               {outer_k * oc_ / im_oc_block / oc_split,
                 oc_ / im_oc_block / oc_split},
               {0, oh_}, {0, ow_}, {0, im_oc_block}})
                            : tensor_slice(output,
                              {{pbs, 1UL}, {0, oh_}, {0, ow_},
                                {outer_k * oc_ / oc_split, oc_ / oc_split}})});
      }
    }
  }

  loops = {lpbs, lps, lpoc, lpic, lok};
}

void gen_nested_conv_fwd_t::compute_conv_no_padding_nested(
  CONV_ARG_LIST) const {
  int bs_threads = config.bs_threads;
  int h_threads = config.h_threads;
  int w_threads = config.w_threads;
  int oc_threads = config.oc_threads;
  int ic_threads = 1;

  int oc_block = config.K_block;
  int h_block = config.h_block;
  int w_block = config.w_block;
  int ic_block = config.C_block;
  int im_oc_block = config.im_oc_block;
  int im_ic_block = config.im_ic_block;
  int im_h_block = config.im_h_block;
  int im_w_block = config.im_w_block;

  COMPILE_ASSERT(oc_block % im_oc_block == 0,
    "oc_block % im_oc_block != 0, config is invalid")
  COMPILE_ASSERT(ic_block % im_ic_block == 0,
    "ic_block % im_ic_block != 0, config is invalid")
  COMPILE_ASSERT(
    h_block % im_h_block == 0, "h_block % im_h_block != 0, config is invalid")
  COMPILE_ASSERT(
    w_block % im_w_block == 0, "w_block % im_w_block != 0, config is invalid")

  // param
  expr output_tmp = output;
  auto tinput = in_tensors_[0];
  auto tweight = in_tensors_[1];
  auto toutput = out_tensors_[0];
  const auto &input_blocking_dims = tinput.get_blocking_dims();
  const auto &weight_blocking_dims = tweight.get_blocking_dims();
  const auto &output_blocking_dims = toutput.get_blocking_dims();

  for_loop lpbs, lph, lpw, lpoc, lpic, loh, low, looc, loic, lioc, lih, liw,
    lok;

  int oc_num_block_pt, oc_tail_num_block_pt, h_num_block_pt,
    h_tail_num_block_pt, w_num_block_pt, w_tail_num_block_pt, ic_num_block_pt,
    ic_tail_num_block_pt;

  int oc_used_threads = block_split(utils::divide_and_ceil(oc_, oc_block),
    oc_threads, oc_num_block_pt, oc_tail_num_block_pt);
  int oh_used_threads = block_split(utils::divide_and_ceil(oh_, h_block),
    h_threads, h_num_block_pt, h_tail_num_block_pt);

  int ow_used_threads = block_split(utils::divide_and_ceil(ow_, w_block),
    w_threads, w_num_block_pt, w_tail_num_block_pt);

  int ic_used_threads = block_split(utils::divide_and_ceil(ic_, ic_block),
    ic_threads, ic_num_block_pt, ic_tail_num_block_pt);

  if (ic_used_threads > 1) {
    // barrier
    // output temp buffer
    auto out_dims = output_blocking_dims;
    out_dims[0] *= ic_used_threads;
    _tensor_(out_tmp, toutput.dtype_, dims_to_expr(out_dims));
    output_tmp = out_tmp;
  }

  auto input_expr_dims = input.checked_as<tensor>()->dims_;
  auto mb_expr_ = input_expr_dims[0];

  auto LDA = blocking_input_ ? sw_ * im_ic_block : sw_ * ic_;
  auto LDC = blocking_output_ ? im_oc_block : oc_;

  int oc_split = 1;
  auto nthreads = runtime_config_t::get().get_num_threads();
  bool parallel_space_is_enough
    = (mb_ % nthreads == 0 || utils::divide_and_ceil(mb_, nthreads) > 8);
  auto weight_size
    = math_utils::get_dims_product(in_tensors_[1].get_blocking_dims())
    * utils::get_sizeof_type(get_weight_dtype());
  auto L2_cache_size = ctx->machine_.cpu_flags_.getDCacheSize(2);
  if (weight_size >= L2_cache_size && parallel_space_is_enough
    && oc_threads == 1 && oc_num_block_pt == 1) {
    int num_block = oc_block / im_oc_block;
    int expected_split_num = utils::divide_and_ceil(weight_size, L2_cache_size);
    for (auto &factor : utils::get_factors(num_block)) {
      if (factor >= expected_split_num) {
        expected_split_num = factor;
        break;
      }
    }
    oc_split = num_block < expected_split_num ? 1 : expected_split_num;
  }

  _named_for_(lok, outer_k, 0, oc_split, 1, for_type::PARALLEL) {
    _named_for_(lpbs, pbs, 0, mb_expr_, 1, for_type::PARALLEL) {
      _named_for_(lph, ph, 0, h_threads, 1) {
        _named_for_(lpw, pw, 0, w_threads, 1) {
          _named_for_(lpoc, poc, 0, oc_threads, 1) {
            _named_for_(lpic, pic, 0, ic_threads, 1) {
              expr h_num_block
                = builder::make_select(ph < (oh_used_threads - 1),
                  h_num_block_pt, h_tail_num_block_pt),
                w_num_block = builder::make_select(pw < (ow_used_threads - 1),
                  w_num_block_pt, w_tail_num_block_pt),
                oc_num_block = builder::make_select(poc < (oc_used_threads - 1),
                  oc_num_block_pt, oc_tail_num_block_pt);

              _if_(ph < oh_used_threads && pw < ow_used_threads
                && poc < oc_used_threads && pic < ic_used_threads) {
                // single core
                expr ic_num_block
                  = builder::make_select(pic < (ic_used_threads - 1),
                    ic_num_block_pt, ic_tail_num_block_pt);

                expr n = pbs;
                _named_for_(loh, o_h, 0, h_num_block_pt) {
                  _named_for_(low, o_w, 0, w_num_block_pt) {
                    _named_for_(looc, o_oc, 0, oc_num_block_pt) {
                      _named_for_(loic, o_ic, 0, ic_num_block_pt) {
                        expr cond = o_h < h_num_block && o_w < w_num_block
                          && o_oc < oc_num_block && o_ic < ic_num_block;
                        _if_(cond) {
                          _named_for_(lih, i_h, 0, h_block / im_h_block) {
                            expr h = (ph * h_num_block_pt * h_block / im_h_block
                                       + o_h * h_block / im_h_block + i_h)
                              * im_h_block;
                            _named_for_(liw, i_w, 0, w_block / im_w_block) {
                              expr w
                                = (pw * w_num_block_pt * w_block / im_w_block
                                    + o_w * w_block / im_w_block + i_w)
                                * im_w_block;
                              _if_(w < ow_) {
                                _named_for_(
                                  lioc, i_oc, 0, oc_block / im_oc_block) {
                                  expr oc = poc * oc_num_block_pt * oc_block
                                      / im_oc_block
                                    + o_oc * oc_block / im_oc_block
                                    + outer_k * oc_block / im_oc_block
                                      / oc_split
                                    + i_oc;
                                  _if_(oc * im_oc_block < oc_) {
                                    _tensor_(A_list, datatypes::pointer,
                                      {kh_ * kw_ * ic_block / im_ic_block});
                                    _tensor_(B_list, datatypes::pointer,
                                      {kh_ * kw_ * ic_block / im_ic_block});

                                    _for_(im_h_i, 0, im_h_block) {
                                      _if_(h + im_h_i < oh_) {
                                        _for_(i_c, 0, ic_block / im_ic_block) {
                                          expr ic = pic * ic_num_block_pt
                                              * ic_block / im_ic_block
                                            + o_ic * ic_block / im_ic_block
                                            + i_c;
                                          _if_(ic * im_ic_block < ic_) {
                                            _for_(r, 0, kh_) {
                                              _for_(s, 0, kw_) {
                                                auto idx = i_c * kh_ * kw_
                                                  + r * kw_ + s;
                                                std::vector<expr> input_pos
                                                  = blocking_input_
                                                  ? std::vector<expr> {n, ic,
                                                    (h + im_h_i) * sh_ + r,
                                                    w * sw_ + s, 0}
                                                  : std::vector<expr> {n,
                                                    (h + im_h_i) * sh_ + r,
                                                    w * sw_ + s,
                                                    ic * im_ic_block};

                                                A_list[idx] = tensor_ptr(
                                                  input, input_pos);
                                                B_list[idx] = tensor_ptr(weight,
                                                  kpack > 1
                                                    ? std::vector<expr> {oc, ic,
                                                      r, s, 0, 0, 0}
                                                    : std::vector<expr> {
                                                      oc, ic, r, s, 0, 0});
                                              }
                                            }
                                          }
                                        }
                                        const auto hint_A_size
                                          = im_w_block * ic_block * kh_ * kw_;
                                        const auto hint_B_size
                                          = im_oc_block * ic_block * kh_ * kw_;
                                        const auto hint_C_size
                                          = im_w_block * im_oc_block;

                                        sc_brgemm_attrs_t brg_attrs {
                                          {brgemm::attr_key::max_bs,
                                            kh_ * kw_ * ic_block / im_ic_block},
                                          {brgemm::attr_key::
                                              hint_expected_A_size,
                                            hint_A_size},
                                          {brgemm::attr_key::
                                              hint_expected_B_size,
                                            hint_B_size},
                                          {brgemm::attr_key::
                                              hint_expected_C_size,
                                            hint_C_size},
                                          {brgemm::attr_key::
                                              use_interleave_stores,
                                            true},
                                          {brgemm::attr_key::use_uker, true},
                                          {brgemm::attr_key::bd_mask_level, 0}};

                                        std::vector<expr> output_pos
                                          = blocking_output_
                                          ? std::vector<expr> {pic * mb_ + n,
                                            oc, h + im_h_i, w, 0}
                                          : std::vector<expr> {pic * mb_ + n,
                                            h + im_h_i, w, oc * im_oc_block};

                                        if (ic_num_block_pt > 1) {
                                          _if_(o_ic == 0) {
                                            builtin::brgemm_init_list_update(
                                              A_list, B_list,
                                              tensor_ptr(
                                                output_tmp, output_pos),
                                              1, im_w_block, im_oc_block,
                                              im_ic_block, LDA, im_oc_block,
                                              LDC, 1 /*useless*/
                                              ,
                                              1 /*useless*/
                                              ,
                                              kh_ * kw_ * ic_block
                                                / im_ic_block,
                                              get_input_dtype(),
                                              get_weight_dtype(), brg_attrs);
                                          }
                                          _else_ {
                                            builtin::brgemm_list_update(A_list,
                                              B_list,
                                              tensor_ptr(
                                                output_tmp, output_pos),
                                              1, im_w_block, im_oc_block,
                                              im_ic_block, LDA, im_oc_block,
                                              LDC, 1 /*useless*/
                                              ,
                                              1 /*useless*/
                                              ,
                                              kh_ * kw_ * ic_block
                                                / im_ic_block,
                                              get_input_dtype(),
                                              get_weight_dtype(), brg_attrs);
                                          }
                                        } else {
                                          builtin::brgemm_init_list_update(
                                            A_list, B_list,
                                            tensor_ptr(output_tmp, output_pos),
                                            1, im_w_block, im_oc_block,
                                            im_ic_block, LDA, im_oc_block, LDC,
                                            1 /*useless*/
                                            ,
                                            1 /*useless*/
                                            ,
                                            kh_ * kw_ * ic_block / im_ic_block,
                                            get_input_dtype(),
                                            get_weight_dtype(), brg_attrs);
                                        }

                                        if (fusion && ic_used_threads == 1
                                          && ic_num_block_pt == 1) {
                                          _if_(o_ic == (ic_num_block - 1)) {
                                            fusion->create_output_fusion_anchor(
                                              {blocking_output_
                                                  ? tensor_slice(output,
                                                    {{n, 1UL}, {oc, 1},
                                                      {h + im_h_i, 1},
                                                      {w, im_w_block},
                                                      {0, im_oc_block}})
                                                  : tensor_slice(output,
                                                    {{n, 1UL}, {h + im_h_i, 1},
                                                      {w, im_w_block},
                                                      {oc * im_oc_block,
                                                        im_oc_block}})});
                                          }
                                        }
                                      }
                                    }
                                    if (fusion && ic_used_threads == 1
                                      && ic_num_block_pt == 1) {
                                      _if_(o_ic == (ic_num_block - 1)) {
                                        fusion->create_output_fusion_anchor(
                                          {blocking_output_
                                              ? tensor_slice(output,
                                                {{n, 1UL}, {oc, 1},
                                                  {h, im_h_block},
                                                  {w, im_w_block},
                                                  {0, im_oc_block}})
                                              : tensor_slice(output,
                                                {{n, 1UL}, {h, im_h_block},
                                                  {w, im_w_block},
                                                  {oc * im_oc_block,
                                                    im_oc_block}})});
                                      }
                                    }
                                  }
                                }
                                if (fusion && ic_used_threads == 1
                                  && ic_num_block_pt == 1
                                  && oc_block * oc_used_threads == oc_) {
                                  _if_(o_ic == (ic_num_block - 1)) {
                                    expr anch_c = poc * oc_num_block_pt
                                        * oc_block / im_oc_block
                                      + o_oc * oc_block / im_oc_block
                                      + outer_k * oc_block / im_oc_block
                                        / oc_split;
                                    fusion->create_output_fusion_anchor(
                                      {blocking_output_
                                          ? tensor_slice(output,
                                            {{n, 1UL}, {anch_c, 1},
                                              {h, im_h_block}, {w, im_w_block},
                                              {0, im_oc_block}})
                                          : tensor_slice(output,
                                            {{n, 1UL}, {h, im_h_block},
                                              {w, im_w_block},
                                              {anch_c * im_oc_block,
                                                oc_block}})});
                                  }
                                }
                              }
                            }

                            if (fusion && ic_used_threads == 1
                              && ic_num_block_pt == 1
                              && oc_block * oc_used_threads == oc_
                              && w_block * ow_used_threads == ow_) {
                              _if_(o_ic == (ic_num_block - 1)) {
                                expr anch_c = poc * oc_num_block_pt * oc_block
                                    / im_oc_block
                                  + o_oc * oc_block / im_oc_block
                                  + outer_k * oc_block / im_oc_block / oc_split;
                                expr anch_w
                                  = (pw * w_num_block_pt * w_block / im_w_block
                                      + o_w * w_block / im_w_block)
                                  * im_w_block;
                                fusion->create_output_fusion_anchor(
                                  {blocking_output_
                                      ? tensor_slice(output,
                                        {{n, 1UL}, {anch_c, 1}, {h, im_h_block},
                                          {anch_w, w_block}, {0, im_oc_block}})
                                      : tensor_slice(output,
                                        {{n, 1UL}, {h, im_h_block},
                                          {anch_w, w_block},
                                          {anch_c * im_oc_block, oc_block}})});
                              }
                            }
                          }

                          if (fusion && ic_used_threads == 1
                            && ic_num_block_pt == 1
                            && oc_block * oc_used_threads == oc_
                            && w_block * ow_used_threads == ow_
                            && h_block * oh_used_threads == oh_) {
                            _if_(o_ic == (ic_num_block - 1)) {
                              expr anch_c
                                = poc * oc_num_block_pt * oc_block / im_oc_block
                                + o_oc * oc_block / im_oc_block
                                + outer_k * oc_block / im_oc_block / oc_split;
                              expr anch_h
                                = (ph * h_num_block_pt * h_block / im_h_block
                                    + o_h * h_block / im_h_block)
                                * im_h_block;
                              expr anch_w
                                = (pw * w_num_block_pt * w_block / im_w_block
                                    + o_w * w_block / im_w_block)
                                * im_w_block;
                              fusion->create_output_fusion_anchor(
                                {blocking_output_
                                    ? tensor_slice(output,
                                      {{n, 1UL}, {anch_c, 1}, {anch_h, h_block},
                                        {anch_w, w_block}, {0, im_oc_block}})
                                    : tensor_slice(output,
                                      {{n, 1UL}, {anch_h, h_block},
                                        {anch_w, w_block},
                                        {anch_c * im_oc_block, oc_block}})});
                            }
                          }
                        }
                      }
                      // TODO(xurui): need to add iterated anchor here to
                      // support more fusion opportunity
                    }
                  }
                }
              }

              if (fusion && oc_threads == 1 && ic_threads == 1 && h_threads == 1
                && w_threads == 1) {
                fusion->create_output_fusion_anchor({blocking_output_
                    ? tensor_slice(output,
                      {{pbs, 1UL},
                        {outer_k * oc_ / im_oc_block / oc_split,
                          oc_ / im_oc_block / oc_split},
                        {0, oh_}, {0, ow_}, {0, im_oc_block}})
                    : tensor_slice(output,
                      {{pbs, 1UL}, {0, oh_}, {0, ow_},
                        {outer_k * oc_ / oc_split, oc_ / oc_split}})});
              }
            }

            if (fusion && oc_threads == 1 && h_threads == 1 && w_threads == 1) {
              fusion->create_output_fusion_anchor({blocking_output_
                  ? tensor_slice(output,
                    {{pbs, 1UL},
                      {outer_k * oc_ / im_oc_block / oc_split,
                        oc_ / im_oc_block / oc_split},
                      {0, oh_}, {0, ow_}, {0, im_oc_block}})
                  : tensor_slice(output,
                    {{pbs, 1UL}, {0, oh_}, {0, ow_},
                      {outer_k * oc_ / oc_split, oc_ / oc_split}})});
            }
          }
          if (fusion && h_threads == 1 && w_threads == 1) {
            fusion->create_output_fusion_anchor({blocking_output_
                ? tensor_slice(output,
                  {{pbs, 1UL},
                    {outer_k * oc_ / im_oc_block / oc_split,
                      oc_ / im_oc_block / oc_split},
                    {0, oh_}, {0, ow_}, {0, im_oc_block}})
                : tensor_slice(output,
                  {{pbs, 1UL}, {0, oh_}, {0, ow_},
                    {outer_k * oc_ / oc_split, oc_ / oc_split}})});
          }
        }

        if (fusion && h_threads == 1) {
          fusion->create_output_fusion_anchor({blocking_output_
              ? tensor_slice(output,
                {{pbs, 1UL},
                  {outer_k * oc_ / im_oc_block / oc_split,
                    oc_ / im_oc_block / oc_split},
                  {0, oh_}, {0, ow_}, {0, im_oc_block}})
              : tensor_slice(output,
                {{pbs, 1UL}, {0, oh_}, {0, ow_},
                  {outer_k * oc_ / oc_split, oc_ / oc_split}})});
        }
      }
      if (fusion && mb_ > 1) {
        fusion->create_output_fusion_anchor(
          {blocking_output_ ? tensor_slice(output,
             {{pbs, 1UL},
               {outer_k * oc_ / im_oc_block / oc_split,
                 oc_ / im_oc_block / oc_split},
               {0, oh_}, {0, ow_}, {0, im_oc_block}})
                            : tensor_slice(output,
                              {{pbs, 1UL}, {0, oh_}, {0, ow_},
                                {outer_k * oc_ / oc_split, oc_ / oc_split}})});
      }
    }
  }
  loops = {lpbs, lph, lpw, lpoc, lpic, lok};
}

void gen_nested_conv_fwd_t::schedule_loops(context_ptr ctx,
  const nested_conv_fwd_config_t &config, stmt body,
  std::vector<for_loop> &fors) const {
  if (use_nested_2d_) {
    const auto pack_rows
      = (config.im_w_block > 0 && ow_ % config.im_w_block != 0);
    if (try_os_blocking_ && pack_rows) {
      COMPILE_ASSERT(static_cast<int>(fors.size()) == 5,
        "expected to have 4 for loops, but got " << fors.size()
                                                 << " for loops.");
      auto lpbs = fors[0], lps = fors[1], lpoc = fors[2], lpic = fors[3],
           lok = fors[4];
      lok->fuse(lpbs)->fuse(lps)->fuse(lpoc)->fuse(lpic);
    } else {
      COMPILE_ASSERT(static_cast<int>(fors.size()) == 6,
        "expected to have 5 for loops, but got " << fors.size()
                                                 << " for loops.");
      auto lpbs = fors[0], lph = fors[1], lpw = fors[2], lpoc = fors[3],
           lpic = fors[4], lok = fors[5];
      lok->fuse(lpbs)->fuse(lph)->fuse(lpw)->fuse(lpoc)->fuse(lpic);
    }
  }
}

bool gen_nested_conv_fwd_t::generate(context_ptr ctx,
  const nested_conv_fwd_config_t &config, fusion_manager *fusion,
  const std::vector<expr> &inputs, const std::vector<expr> &outputs,
  std::vector<for_loop> &loops) const {
  COMPILE_ASSERT(inputs.size() == 2,
    "Expecting 2 inputs for conv, but got " << inputs.size() << " inputs.");
  COMPILE_ASSERT(outputs.size() == 1,
    "Expecting 1 output for conv, but got " << outputs.size() << " output.");

  int K_block = config.K_block;
  int C_block = config.C_block;
  int im_s_block = config.im_w_block;

  int pack_input = config.pack_input;
  const bool use_os_blocking = try_os_blocking_ && ctx->use_amx();
  const bool pack_rows
    = use_os_blocking && (im_s_block > 0 && ow_ % im_s_block != 0);
  int os = actual_os_;

  COMPILE_ASSERT(K_block && (oc_ % K_block == 0),
    "oc should be dividable by K_block, but got oc=" << oc_ << " K_block="
                                                     << K_block << ".");
  COMPILE_ASSERT(C_block && (ic_ % C_block == 0),
    "ic should be dividable by C_block, but got ic=" << ic_ << " C_block="
                                                     << C_block << ".");

  // kpack is used to determine the vnni block format
  //  +----+--------------+
  //  | 1  | FP32         |
  //  +----+--------------+
  //  | 2  | VNNI_BF16    |
  //  +----+--------------+
  //  | 4  | VNNI_INT8    |
  //  +----+--------------+
  int kpack = 1;
  auto dtypeInput = get_input_dtype();
  auto dtypeWeight = get_weight_dtype();
  auto dtypeOutput = get_output_dtype();
  if (dtypeInput == datatypes::bf16) {
    COMPILE_ASSERT((dtypeWeight == datatypes::bf16),
      "Weights should be bf16 as "
      "data, the mixed datatypes is not supported yet!");
    COMPILE_ASSERT((dtypeOutput == datatypes::f32),
      "Output should be f32 when data and weights are in bf16.");
    kpack = 2;
  }
  if (utils::is_one_of(dtypeInput, datatypes::s8, datatypes::u8)) {
    COMPILE_ASSERT((dtypeWeight == datatypes::s8),
      "Weights should be s8 when \
            data is s8/u8, the mixed datatypes is not supported yet!");
    COMPILE_ASSERT((dtypeOutput == datatypes::s32),
      "Output should be s32 when data and weights are in "
      "s8/u8.");
    kpack = 4;
  }

  std::vector<char> os_mask = {};
  expr os_acc_size = expr();
  if (pack_rows) {
    os = adj_os_;
    int adj_ow = ow_ + num_elems_skip_per_ow_;
    os_mask.resize(os);
    for (int i = 0; i < os; ++i) {
      if (i % adj_ow < ow_) {
        os_mask[i] = 1;
      } else {
        os_mask[i] = 0;
      }
    }

    int im_os_num_block = os / im_s_block;
    _tensor_(conv_os_acc_size, datatypes::s32, {im_os_num_block});
    int acc_size = 0;
    int blk_size = 0;
    for (int i = 0; i < im_os_num_block; ++i) {
      blk_size = std::accumulate(os_mask.begin() + i * im_s_block,
        os_mask.begin() + (i + 1) * im_s_block, 0);
      conv_os_acc_size[i] = acc_size;
      acc_size += blk_size;
    }
    os_acc_size = conv_os_acc_size;
  }

  if (use_os_blocking) {
    COMPILE_ASSERT((im_s_block > 0) && (os % im_s_block == 0),
      "os should be dividable by im_w_block, but got os="
        << os << " im_w_block=" << config.im_w_block << ".");
  } else {
    COMPILE_ASSERT((config.im_h_block > 0) && (oh_ % config.im_h_block == 0),
      "oh should be dividable by im_h_block, but got oh="
        << oh_ << " im_h_block=" << config.im_h_block << ".");
    COMPILE_ASSERT((config.im_w_block > 0) && (ow_ % config.im_w_block == 0),
      "ow should be dividable by tile_q, but got ow="
        << ow_ << " im_w_block=" << config.im_w_block << ".");
  }

  expr output = outputs[op_params_t::out];
  expr input = inputs[op_params_t::in_data];
  expr weight = inputs[op_params_t::in_weight];

  if (is_1x1_conv_) {
    COMPILE_ASSERT(
      pd_ == 0 && ph_ == 0 && pw_ == 0, "1x1 conv doesn't support padding!");
    COMPILE_ASSERT(
      !inverse_filter_, "1x1 conv doesn't support inverse convolution.");
    if (pack_input == 0 && (sd_ > 1 || sh_ > 1 || sw_ > 1)) {
      compute_1x1_no_pack_input_nested(
        ctx, config, fusion, output, input, weight, loops, os, kpack);
    } else {
      compute_1x1_pack_input_nested(
        ctx, config, fusion, output, input, weight, loops, os, kpack);
    }
  } else {
    if (pd_ == 0 && ph_ == 0 && pw_ == 0) {
      COMPILE_ASSERT(!inverse_filter_,
        "conv NxN (no padding) does not support inverse "
        "convolution.");
      if (is_3d_) {
        COMPILE_ASSERT(!is_3d_,
          "nested conv fwd does not support 3d convolution currently.");
      } else {
        if (use_os_blocking && pack_rows) {
          compute_conv_no_padding_os_blocking_nested(ctx, config, fusion,
            output, input, weight, loops, os, kpack, use_os_blocking, pack_rows,
            os_acc_size, os_mask);
        } else {
          compute_conv_no_padding_nested(ctx, config, fusion, output, input,
            weight, loops, os, kpack, use_os_blocking, pack_rows, os_acc_size,
            os_mask);
        }
      }
    }
  }
  return true;
}
#undef CONV_ARG_LIST

} // namespace ops
} // namespace gc
} // namespace graph
} // namespace impl
} // namespace dnnl
