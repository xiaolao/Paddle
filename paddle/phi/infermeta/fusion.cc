/* Copyright (c) 2023 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include "paddle/phi/infermeta/fusion.h"
#include <vector>
#include "paddle/phi/common/layout.h"
#include "paddle/phi/common/scalar.h"
#include "paddle/phi/core/infermeta_utils.h"
#include "paddle/phi/core/meta_tensor.h"
#include "paddle/phi/kernels/cpu/conv_util.h"
#include "paddle/phi/kernels/funcs/common_shape.h"
#include "paddle/phi/kernels/funcs/concat_funcs.h"
#include "paddle/phi/kernels/funcs/strided_slice.h"

namespace phi {

static phi::DDim BroadCastInferShape(const DDim x_dims,
                                     const DDim y_dims,
                                     int axis) {
  std::vector<int> out_dims_array(x_dims.size(), -1);
  if (x_dims != y_dims) {
    int max_dim = std::max(x_dims.size(), y_dims.size());
    if (x_dims.size() == y_dims.size()) {
      PADDLE_ENFORCE_EQ((axis == -1) || (axis == 0),
                        true,
                        phi::errors::InvalidArgument(
                            "axis should be -1 or 0 while the dimension of "
                            "tensor X (%s) is equal to the dimension of "
                            "tensor Y (%s), but received axis: %s",
                            x_dims.size(),
                            y_dims.size(),
                            axis));
    }
    PADDLE_ENFORCE_EQ((axis >= (-1 * max_dim)) && (axis < max_dim),
                      true,
                      phi::errors::InvalidArgument(
                          "The axis range must be [%s, %s), but axis is %s. "
                          "Please set the axis again.",
                          -1 * max_dim,
                          max_dim,
                          axis));
    axis = (axis < 0 ? (std::abs(x_dims.size() - y_dims.size()) + axis + 1)
                     : axis);
    std::vector<int> x_dims_array(max_dim);
    std::vector<int> y_dims_array(max_dim);
    out_dims_array.resize(max_dim);
    funcs::GetBroadcastDimsArrays(x_dims,
                                  y_dims,
                                  x_dims_array.data(),
                                  y_dims_array.data(),
                                  out_dims_array.data(),
                                  max_dim,
                                  axis);

    return phi::make_ddim(out_dims_array);
  }
  return x_dims;
}

void AddActXPUInferMeta(const MetaTensor& x,
                        const MetaTensor& x_max,
                        const MetaTensor& y,
                        const MetaTensor& y_max,
                        int act_type,
                        MetaTensor* out,
                        MetaTensor* out_max) {
  int axis = -1;
  auto x_dims = x.dims();
  auto y_dims = y.dims();
  if (x_dims != y_dims) {
    auto out_dims = BroadCastInferShape(x_dims, y_dims, axis);
    out->set_dims(out_dims);
  } else {
    out->set_dims(x_dims);
  }
  out->set_dtype(x.dtype());
  out->set_layout(x.layout());
  out->share_lod(x);
  out_max->set_dims(phi::make_ddim({6}));
  out_max->set_dtype(x.dtype());
  out_max->set_layout(x.layout());
}

inline int ConvOutSize(int input_size,
                       int filter_size,
                       int dilation,
                       int pad_left,
                       int pad_right,
                       int stride) {
  const int dkernel = dilation * (filter_size - 1) + 1;
  int output_size =
      (input_size + (pad_left + pad_right) - dkernel) / stride + 1;

  return output_size;
}

void Conv2dXPUInferMeta(const MetaTensor& x,
                        const MetaTensor& x_max,
                        const MetaTensor& filter,
                        const MetaTensor& filter_max,
                        const MetaTensor& bias,
                        const MetaTensor& branch,
                        const MetaTensor& branch_max,
                        const std::vector<int>& paddings,
                        const std::vector<int>& dilations,
                        const std::vector<int>& strides,
                        const std::string& padding_algorithm,
                        int groups,
                        bool has_bias,
                        bool has_branch,
                        int act_type,
                        float act_param,
                        MetaTensor* out,
                        MetaTensor* out_max) {
  auto in_dims = x.dims();
  auto filter_dims = filter.dims();
  // do some checks
  PADDLE_ENFORCE_EQ(
      in_dims.size(),
      4,
      phi::errors::InvalidArgument(
          "The input of Op(Conv_xpu) should be a 4-D Tensor. But "
          "received: input's dimension is %u, input's shape is [%s].",
          in_dims.size(),
          in_dims));

  PADDLE_ENFORCE_EQ(
      in_dims.size(),
      filter_dims.size(),
      phi::errors::InvalidArgument(
          "The input's dimension and filter's dimension of "
          "Op(Conv_xpu) should be equal. But received: the input's shape is "
          "[%s], "
          "the input's dimension is %d; the filter's shape is [%s],  "
          "the filter's dimension is %d.",
          in_dims,
          in_dims.size(),
          filter_dims,
          filter_dims.size()));

  const auto input_channels = in_dims[1];
  int stride_size = strides.size();
  int in_sub_stride_size = in_dims.size() - stride_size;
  int dilation_size = dilations.size();
  PADDLE_ENFORCE_EQ(
      in_dims.size(),
      strides.size() + 2U,
      phi::errors::InvalidArgument(
          "The difference of input's dimension and Attr(strides)'s "
          "length must be euqal to 2 for Op(Conv_xpu). "
          "But received: input's dimension is %d, input's shape is [%s]; "
          "Attr(stride)'s length is %d, Attr(stride) is [%s]; "
          "difference of input's dimention and Attr(strides)'s length = %u.",
          in_dims.size(),
          in_dims,
          strides.size(),
          phi::make_ddim(strides),
          in_sub_stride_size));

  for (int i = 0; i < dilation_size; ++i) {
    PADDLE_ENFORCE_GT(
        dilations[i],
        0,
        phi::errors::InvalidArgument(
            "The dilation of Op(Conv) should be larget than 0, but received "
            "dilation is %d.",
            dilations[i]));
  }

  PADDLE_ENFORCE_EQ(
      input_channels,
      filter_dims[1] * groups,
      phi::errors::InvalidArgument(
          "The number of input's channels should be equal to filter's channels "
          "* groups for Op(Conv_xpu). But received: the input's channels is "
          "%d, "
          "the input's shape is [%s]; the filter's channels is %d, the "
          "filter's shape is [%s]; the groups is %d. ",
          input_channels,
          in_dims,
          filter_dims[1],
          filter_dims,
          groups));

  PADDLE_ENFORCE_EQ(
      filter_dims[0] % groups,
      0,
      phi::errors::InvalidArgument(
          "The number of output's channels (filter's first dimension) of "
          "Op(Conv) should be divided by groups. But received: "
          "the output channels is %d, the filter's shape is [%s], "
          "the groups is %d.",
          filter_dims[0],
          filter_dims,
          groups));

  // update paddings and dilations accoring to padding_algorithm
  std::vector<int> paddings_vec = paddings;
  std::vector<int> dilations_vec = dilations;
  DDim in_data_dims = phi::slice_ddim(in_dims, 2, in_dims.size());
  DDim filter_data_dims = phi::slice_ddim(filter_dims, 2, filter_dims.size());
  std::vector<int> ksize = phi::vectorize<int>(filter_data_dims);
  phi::UpdatePaddingAndDilation(&paddings_vec,
                                &dilations_vec,
                                padding_algorithm,
                                in_data_dims,
                                strides,
                                ksize);

  std::vector<int64_t> out_shape({in_dims[0], filter_dims[0]});
  for (size_t i = 0; i < strides.size(); ++i) {
    out_shape.push_back(ConvOutSize(in_dims[i + 2],
                                    filter_dims[i + 2],
                                    dilations[i],
                                    paddings_vec[i * 2],
                                    paddings_vec[i * 2 + 1],
                                    strides[i]));
  }
  // set output and output max dims
  out->set_dims(DDim(out_shape.data(), out_shape.size()));
  out_max->set_dims(phi::make_ddim({6}));
}

void EmbeddingWithEltwiseAddXPUInferMeta(
    const std::vector<const MetaTensor*>& ids,
    const std::vector<const MetaTensor*>& tables,
    const MetaTensor& mask,
    MetaTensor* out,
    MetaTensor* seq_lod,
    MetaTensor* max_seq_len) {
  PADDLE_ENFORCE_GT(ids.size(),
                    0UL,
                    phi::errors::InvalidArgument(
                        "The input ids in EmbeddingWithEltwiseAddXPUInferMeta "
                        "can't be empty."));
  PADDLE_ENFORCE_GT(tables.size(),
                    0UL,
                    phi::errors::InvalidArgument(
                        "The input tables in "
                        "EmbeddingWithEltwiseAddXPUInferMeta can't be empty."));

  auto id_dims = ids[0]->dims();
  auto table_dims = tables[0]->dims();
  out->set_dims(phi::make_ddim({id_dims[0], id_dims[1], table_dims[1]}));
  out->set_dtype(tables[0]->dtype());
  out->set_layout(ids[0]->layout());
}

void FcXPUInferMeta(const MetaTensor& x,
                    const MetaTensor& x_max,
                    const MetaTensor& w,
                    const MetaTensor& w_max,
                    const MetaTensor& bias,
                    int in_num_col_dims,
                    bool transpose_x,
                    float alpha,
                    float beta,
                    int act_type,
                    float act_alpha,
                    MetaTensor* out,
                    MetaTensor* out_max) {
  std::vector<int> out_shape(in_num_col_dims + 1);
  for (int i = 0; i < in_num_col_dims; i++) {
    out_shape[i] = x.dims()[i];
  }
  out_shape[in_num_col_dims] = w.dims()[0];
  out->set_dims(DDim(out_shape.data(), out_shape.size()));
  out->set_dtype(x.dtype());
  out->set_layout(x.layout());
  out_max->set_dims(phi::make_ddim({6}));
  out_max->set_dtype(x.dtype());
  out_max->set_layout(x.layout());
}

void GenerateSequenceXPUInferMeta(const MetaTensor& x,
                                  DataType dtype,
                                  MetaTensor* out) {
  out->set_dims(x.dims());
  out->set_dtype(dtype);
  out->set_layout(x.layout());
}

void MultiEncoderXPUInferMeta(
    const MetaTensor& x,
    const std::vector<const MetaTensor*>& fc_weight,
    const std::vector<const MetaTensor*>& fc_weight_max,
    const std::vector<const MetaTensor*>& fc_bias,
    const std::vector<const MetaTensor*>& ln_scale,
    const std::vector<const MetaTensor*>& ln_bias,
    const MetaTensor& mask,
    const MetaTensor& seq_lod,
    const MetaTensor& max_seq_len,
    int layer_num,
    bool norm_before,
    int hidden_dim,
    int head_num,
    int size_per_head,
    int ffn_hidden_dim_scale,
    int act_type,
    int relative_type,
    int slice_idx,
    MetaTensor* out,
    MetaTensor* x_fp16,
    MetaTensor* out_fp16) {
  auto x_dims = x.dims();
  x_fp16->set_dims(x_dims);
  x_fp16->set_dtype(DataType::FLOAT16);
  x_fp16->set_layout(x.layout());
  out->set_dtype(x.dtype());
  out->set_layout(x.layout());
  out_fp16->set_dtype(DataType::FLOAT16);
  out_fp16->set_layout(x.layout());
  if (slice_idx == -1) {
    out->set_dims(x_dims);
    out_fp16->set_dims(x_dims);
  } else {
    out->set_dims({x_dims[0], x_dims[2]});
    out_fp16->set_dims({x_dims[0], x_dims[2]});
  }
}

void FusedMultiTransformerXpuInferMeta(
    const MetaTensor& x,
    const std::vector<const MetaTensor*>& ln_scale,
    const std::vector<const MetaTensor*>& ln_bias,
    const std::vector<const MetaTensor*>& qkvw,
    const std::vector<const MetaTensor*>& qkvw_max,
    const std::vector<const MetaTensor*>& qkv_bias,
    const std::vector<const MetaTensor*>& out_linear_w,
    const std::vector<const MetaTensor*>& out_linear_wmax,
    const std::vector<const MetaTensor*>& out_linear_bias,
    const std::vector<const MetaTensor*>& ffn_ln_scale,
    const std::vector<const MetaTensor*>& ffn_ln_bias,
    const std::vector<const MetaTensor*>& ffn1_weight,
    const std::vector<const MetaTensor*>& ffn1_weight_max,
    const std::vector<const MetaTensor*>& ffn1_bias,
    const std::vector<const MetaTensor*>& ffn2_weight,
    const std::vector<const MetaTensor*>& ffn2_weight_max,
    const std::vector<const MetaTensor*>& ffn2_bias,
    const std::vector<const MetaTensor*>& cache_kv,
    const std::vector<const MetaTensor*>& pre_caches,
    const std::vector<const MetaTensor*>& rotary_pos_emb,
    const std::vector<const MetaTensor*>& time_step,
    const std::vector<const MetaTensor*>& seq_lengths,
    const std::vector<const MetaTensor*>& src_mask,
    const std::vector<const MetaTensor*>& gather_index,
    bool pre_layer_norm,
    int rotary_emb_dims,
    float epsilon,
    float dropout_rate,
    bool is_test,
    const std::string& dropout_implementation,
    const std::string& act_method,
    bool trans_qkvw,
    int ring_id,
    int gather_axis,
    MetaTensor* out,
    std::vector<MetaTensor*> cache_kv_out) {
  auto x_dim = x.dims();
  auto y_dim = qkvw[0]->dims();
  PADDLE_ENFORCE_EQ(x_dim.size(),
                    3,
                    phi::errors::InvalidArgument(
                        "The dimensions of x must be 3(batch_size, seq_len, "
                        "dim_embed), but received dimensions of Input is [%d]",
                        x_dim.size()));
  PADDLE_ENFORCE_EQ(
      y_dim.size(),
      4,
      phi::errors::InvalidArgument(
          "The dimensions of qkv_weight must be 4(3, num_head, dim_head, "
          "dim_embed), but received dimensions of qkv_weight is [%d]",
          y_dim.size()));
  PADDLE_ENFORCE_EQ(
      x_dim[2],
      trans_qkvw ? y_dim[3] : y_dim[0],
      phi::errors::InvalidArgument(
          "The dimension of x_dim[2] and y_dim[3](trans_qkvw is  true) or "
          "y_dim[0](trans_qkvw is false) must be equal, but received: the "
          "shape of input x = [%s], and the shape of input qkv_weight = [%s]",
          x_dim,
          y_dim));
  if (cache_kv.size() > 0) {
    const auto& c_dim = cache_kv[0]->dims();
    PADDLE_ENFORCE_EQ(
        c_dim.size(),
        5,
        phi::errors::InvalidArgument("The CacheKV must be 5 dims, but got %d",
                                     c_dim.size()));
    PADDLE_ENFORCE_EQ(c_dim[0],
                      2,
                      phi::errors::InvalidArgument(
                          "The first dim of CacheKV must be 2, but got %d",
                          c_dim[0]));  // 2
    PADDLE_ENFORCE_EQ(
        c_dim[3],
        trans_qkvw ? y_dim[1] : y_dim[2],
        phi::errors::InvalidArgument("The fourth dim of CacheKV must be equal "
                                     "with num head %d, but got %d",
                                     trans_qkvw ? y_dim[1] : y_dim[2],
                                     c_dim[3]));  // num_head
    PADDLE_ENFORCE_EQ(
        c_dim[4],
        trans_qkvw ? y_dim[2] : y_dim[3],
        phi::errors::InvalidArgument("The fifth dim of CacheKV must be equal "
                                     "with head size %d, but got %d",
                                     trans_qkvw ? y_dim[2] : y_dim[3],
                                     c_dim[4]));  // head_size
  }

  out->set_dims(x_dim);
  out->set_dtype(x.dtype());
  out->set_layout(x.layout());
}

void YoloBoxXPUInferMeta(const MetaTensor& x,
                         const MetaTensor& x_max,
                         const MetaTensor& grid,
                         const MetaTensor& stride,
                         const MetaTensor& anchor_grid,
                         float offset,
                         MetaTensor* out,
                         MetaTensor* out_max) {
  auto x_dims = x.dims();
  auto x_dims_size = x_dims.size();
  PADDLE_ENFORCE_GT(
      x_dims[x_dims_size - 1],
      4,
      phi::errors::InvalidArgument(
          "The last dim of x should be larget than 4, but received "
          " is %d.",
          x_dims[x_dims_size - 1]));
  // compute left out_dims
  // y[..., 0:2] = (x[..., 0:2] * 2 + self.grid[i]) * self.stride[i]  # xy
  std::vector<int> axes_ = {x_dims_size - 1};
  std::vector<int> infer_flags_ = {1};
  std::vector<int> decrease_axis_ = {-1};
  std::vector<int64_t> strides_ = {1};
  std::vector<int64_t> starts_l = {0};
  std::vector<int64_t> ends_l = {2};
  std::vector<int64_t> left_slice_out_dims_vector(x_dims_size, -1);
  phi::funcs::StridedSliceOutDims(starts_l,
                                  ends_l,
                                  strides_,
                                  axes_,
                                  infer_flags_,
                                  x_dims,
                                  decrease_axis_,
                                  left_slice_out_dims_vector.data(),
                                  1,
                                  true);
  auto left_slice_out_dims = phi::make_ddim(left_slice_out_dims_vector);
  auto grid_dims = grid.dims();
  auto left_add_out_dims =
      BroadCastInferShape(left_slice_out_dims, grid_dims, -1);
  auto stride_dims = stride.dims();
  auto left_mul_out_dims =
      BroadCastInferShape(left_add_out_dims, stride_dims, -1);
  // compute mid out_dims
  // wh = (y[..., 2:4] * 2) ** 2 * self.anchor_grid[i]             # wh
  std::vector<int64_t> starts_m = {2};
  std::vector<int64_t> ends_m = {4};
  std::vector<int64_t> mid_slice_out_dims_vector(x_dims_size, -1);
  phi::funcs::StridedSliceOutDims(starts_m,
                                  ends_m,
                                  strides_,
                                  axes_,
                                  infer_flags_,
                                  x_dims,
                                  decrease_axis_,
                                  mid_slice_out_dims_vector.data(),
                                  1,
                                  true);
  auto mid_slice_out_dims = phi::make_ddim(mid_slice_out_dims_vector);
  auto anchor_grid_dims = anchor_grid.dims();
  auto mid_mul_out_dims =
      BroadCastInferShape(mid_slice_out_dims, anchor_grid_dims, -1);
  // compute right out_dims
  std::vector<int64_t> starts_r = {4};
  std::vector<int64_t> ends_r = {2147483647};
  std::vector<int64_t> right_slice_out_dims_vector(x_dims_size, -1);
  phi::funcs::StridedSliceOutDims(starts_r,
                                  ends_r,
                                  strides_,
                                  axes_,
                                  infer_flags_,
                                  x_dims,
                                  decrease_axis_,
                                  right_slice_out_dims_vector.data(),
                                  1,
                                  true);
  auto right_slice_out_dims = phi::make_ddim(right_slice_out_dims_vector);
  // compute concat out_dims
  std::vector<phi::DDim> in_dims;
  in_dims.reserve(3);
  in_dims.emplace_back(left_mul_out_dims);
  in_dims.emplace_back(mid_mul_out_dims);
  in_dims.emplace_back(right_slice_out_dims);
  phi::DDim out_dim =
      phi::funcs::ComputeAndCheckShape(false, in_dims, x_dims_size - 1);

  out->set_dims(out_dim);
  out->set_dtype(x.dtype());
  out->set_layout(x.layout());
  out_max->set_dims(phi::make_ddim({6}));
  out_max->set_dtype(x.dtype());
  out_max->set_layout(x.layout());
}

}  // namespace phi
