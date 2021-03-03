// Copyright (C) 2021 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "plaidml_ops.hpp"
#include "plaidml_util.hpp"

#include "ngraph/opsets/opset.hpp"
#include "ngraph/opsets/opset4.hpp"

#include "plaidml/op/lib/ops.h"
#include "plaidml/op/op.h"

using namespace plaidml;          // NOLINT[build/namespaces]
using namespace InferenceEngine;  // NOLINT[build/namespaces]
using namespace plaidml::edsl;
using namespace plaidml::op::lib;

namespace {

edsl::Tensor compute_deformable_convolution(edsl::Tensor I,                  //
                                            edsl::Tensor OFF,                //
                                            edsl::Tensor F,                  //
                                            std::vector<int64_t> I_shape,    //
                                            std::vector<int64_t> OFF_shape,  //
                                            std::vector<int64_t> F_shape,    //
                                            int64_t G,                       //
                                            int64_t DG,                      //
                                            int64_t rank,                    //
                                            std::vector<size_t> strides,     //
                                            std::vector<size_t> dilations,   //
                                            std::vector<TensorDim> pad_befores) {
  auto N = I_shape[0];
  auto CI = I_shape[1];
  auto CO = F_shape[1];
  auto OFF_C = OFF_shape[1];
  if (CI % G != 0 || CO % G != 0 || CI % DG != 0 || OFF_C % DG != 0) {
    THROW_IE_EXCEPTION << "Incorrect shape for DeformableConvolution.";
  }
  std::vector<int64_t> deformed_dims;
  for (auto i = 2; i < rank; ++i) {
    deformed_dims.push_back(F_shape[i] * OFF_shape[i]);
  }
  // Set up the value of the dims to reshape OFF (Deformable values).
  // For example, in 2D, after this operation, the shape of offset will be {N, DG, F_H, F_W, 2, OFF_H, OFF_W}.
  std::vector<int64_t> OFF_reshape_dims;
  OFF_reshape_dims.push_back(N);
  OFF_reshape_dims.push_back(DG);
  OFF_reshape_dims.insert(OFF_reshape_dims.end(), F_shape.begin() + 2, F_shape.end());
  OFF_reshape_dims.push_back(rank - 2);
  OFF_reshape_dims.insert(OFF_reshape_dims.end(), OFF_shape.begin() + 2, OFF_shape.end());
  edsl::Tensor offset = op::reshape(OFF, make_tuple<int64_t>(OFF_reshape_dims));
  // Set up the dims for op::transpose.
  // For example, in 2D, after this operation, the shape of offset will be {N, DG, OFF_H, F_H, OFF_W, F_W, 2}.
  std::vector<int64_t> OFF_transpose_dims;
  OFF_transpose_dims.push_back(0);
  OFF_transpose_dims.push_back(1);
  for (auto i = 0; i < rank - 2; ++i) {
    OFF_transpose_dims.push_back(rank + 1 + i);
    OFF_transpose_dims.push_back(2 + i);
  }
  OFF_transpose_dims.push_back(rank);
  offset = op::transpose(offset, make_tuple<int64_t>(OFF_transpose_dims));
  // Set up the dims for op::reshape.
  // For example, in 2D, after this operation, the shape of offset will be {N, DG, 1, OFF_H*F_H, OFF_W*F_W, 2}.
  std::vector<int64_t> OFF_multiply_dims;
  OFF_multiply_dims.push_back(N);
  OFF_multiply_dims.push_back(DG);
  OFF_multiply_dims.push_back(1);
  OFF_multiply_dims.insert(OFF_multiply_dims.end(), deformed_dims.begin(), deformed_dims.end());
  OFF_multiply_dims.push_back(rank - 2);
  offset = op::reshape(offset, make_tuple<int64_t>(OFF_multiply_dims));
  // Set up the dims to broadcast in the channel dimension.
  // For example, in 2D, after this operation, the shape of offset will be {N, DG, CI/DG, OFF_H*F_H, OFF_W*F_W, 2}.
  std::vector<int64_t> OFF_broadcast_dims, OFF_broadcast_axes;
  OFF_broadcast_dims.push_back(N);
  OFF_broadcast_dims.push_back(DG);
  OFF_broadcast_dims.push_back(CI / DG);
  OFF_broadcast_dims.insert(OFF_broadcast_dims.end(), deformed_dims.begin(), deformed_dims.end());
  OFF_broadcast_dims.push_back(rank - 2);
  for (auto i = 0; i < rank + 2; ++i) {
    OFF_broadcast_axes.push_back(i);
  }
  offset = op::broadcast(offset, OFF_broadcast_dims, OFF_broadcast_axes);
  // Set up the dims for op::reshape.
  // For example, in 2D, after this operation, the shape of offset will be {N, CI, OFF_H*F_H, OFF_W*F_W, 2}.
  std::vector<int64_t> OFF_reshape_channel;
  OFF_reshape_channel.push_back(N);
  OFF_reshape_channel.push_back(CI);
  OFF_reshape_channel.insert(OFF_reshape_channel.end(), deformed_dims.begin(), deformed_dims.end());
  OFF_reshape_channel.push_back(rank - 2);
  offset = op::reshape(offset, make_tuple<int64_t>(OFF_reshape_channel));
  // Define the index of input.
  std::vector<edsl::Tensor> index_vec(rank - 2);
  for (auto i = 0; i < rank - 2; ++i) {
    edsl::Tensor index_vec_0 = edsl::index({edsl::TensorDim(OFF_shape[i + 2]), TensorDim(F_shape[i + 2])}, 0);
    edsl::Tensor index_vec_1 = edsl::index({edsl::TensorDim(OFF_shape[i + 2]), TensorDim(F_shape[i + 2])}, 1);
    index_vec[i] = index_vec_0 * strides[i] + index_vec_1 * dilations[i] - pad_befores[i];
    // Set up the dims for op::reshape.
    std::vector<int64_t> index_reshape_dims(rank + 1, 1);
    index_reshape_dims[i + 2] = deformed_dims[i];
    index_vec[i] = op::reshape(index_vec[i], make_tuple<int64_t>(index_reshape_dims));
    // Set up the dims for op::broadcast.
    // For example, in 2D, after this operation, the shape of index_vec[i] will be {1, 1, OFF_H*F_H, OFF_W*F_W, 1}.
    std::vector<int64_t> index_broadcast_dims(rank + 1, 1);
    for (auto j = 2; j < rank; ++j) {
      index_broadcast_dims[j] = deformed_dims[j - 2];
    }
    std::vector<int64_t> index_broadcast_axes;
    for (auto j = 0; j < rank + 1; ++j) {
      index_broadcast_axes.push_back(j);
    }
    index_vec[i] = op::broadcast(index_vec[i], index_broadcast_dims, index_broadcast_axes);
  }
  edsl::Tensor index = op::concatenate(index_vec, -1);  // The shape of index is {1, 1, NEW_DIM, rank-2}
  edsl::Tensor deformed_index = offset + index;
  // Get deformed index tensor for gatherND and interplation.
  std::vector<edsl::Tensor> deformed_index_vec(rank - 2);
  std::vector<edsl::Tensor> deformed_index_vec_ceil(rank - 2);
  std::vector<edsl::Tensor> deformed_index_vec_floor(rank - 2);
  // Set up the dims to reshape the deformed_index_ceil tensor and deformed_index_floor tensor.
  // For example, in 2D, after this operation, the shapes of deformed_index_ceil tensor and deformed_index_floor tensor
  // will be {N, CI, OFF_H*F_H, OFF_W*F_W, 1}.
  std::vector<int64_t> deformed_index_reshape_dims;
  deformed_index_reshape_dims.push_back(N);
  deformed_index_reshape_dims.push_back(CI);
  deformed_index_reshape_dims.insert(deformed_index_reshape_dims.end(), deformed_dims.begin(), deformed_dims.end());
  deformed_index_reshape_dims.push_back(1);
  auto deformed_index_slice = op::slice(deformed_index).add_dim(0, N).add_dim(0, CI);
  for (auto i = 0; i < rank - 2; ++i) {
    deformed_index_slice = op::slice(deformed_index_slice).add_dim(0, deformed_dims[i]);
  }
  for (auto i = 0; i < rank - 2; ++i) {
    deformed_index_vec[i] = op::slice(deformed_index_slice).add_dim(i);
    deformed_index_vec_ceil[i] = edsl::ceil(deformed_index_vec[i]);
    deformed_index_vec_floor[i] = edsl::floor(deformed_index_vec[i]);
    deformed_index_vec_ceil[i] =
        op::reshape(deformed_index_vec_ceil[i], make_tuple<int64_t>(deformed_index_reshape_dims));
    deformed_index_vec_floor[i] =
        op::reshape(deformed_index_vec_floor[i], make_tuple<int64_t>(deformed_index_reshape_dims));
  }
  // Coord_num is the number of tensor required for multi-linear interpolation.
  int64_t coord_num = static_cast<int64_t>(std::pow(2.0, rank - 2));
  std::vector<edsl::Tensor> deformed_index_coord_vec(coord_num);
  std::vector<edsl::Tensor> deformed_input_vec(coord_num);
  for (auto i = 0; i < coord_num; ++i) {
    std::vector<edsl::Tensor> concat_vec(rank - 2);
    for (auto j = 0; j < rank - 2; ++j) {
      int64_t flag = (i / static_cast<int64_t>(std::pow(2.0, rank - 2 - j - 1))) % 2;
      concat_vec[j] = (flag == 0) ? deformed_index_vec_floor[j] : deformed_index_vec_ceil[j];
    }
    deformed_index_coord_vec[i] = op::concatenate(concat_vec, rank);
    deformed_index_coord_vec[i] = edsl::cast(deformed_index_coord_vec[i], DType::INT32);
    deformed_input_vec[i] = edsl::gather(I, deformed_index_coord_vec[i]).mode(edsl::GatherMode::ND).batchDims(2);
  }
  // Change the shape of deformed_index_ceil tensor and deformed_index_floor tensor to original shape.
  // For example, in 2D, after this operation, the shapes of deformed_index_ceil tensor and deformed_index_floor tensor
  // will be {N, CI, OFF_H*F_H, OFF_W*F_W}.
  deformed_index_reshape_dims.erase(deformed_index_reshape_dims.end() - 1);
  for (auto i = 0; i < rank - 2; ++i) {
    deformed_index_vec_ceil[i] =
        op::reshape(deformed_index_vec_ceil[i], make_tuple<int64_t>(deformed_index_reshape_dims));
    deformed_index_vec_floor[i] =
        op::reshape(deformed_index_vec_floor[i], make_tuple<int64_t>(deformed_index_reshape_dims));
  }
  // Get deformed input tensor with multi-linear interpolation.
  for (auto i = 0; i < rank - 2; ++i) {
    for (auto j = 0; j < static_cast<int64_t>(std::pow(2.0, rank - 2 - i - 1)); ++j) {
      deformed_input_vec[j] = deformed_input_vec[j] * (deformed_index_vec_ceil[i] - deformed_index_vec[i]) +
                              deformed_input_vec[j + static_cast<int64_t>(std::pow(2.0, rank - 2 - i - 1))] *
                                  (1.0 - deformed_index_vec_ceil[i] + deformed_index_vec[i]);
    }
  }
  edsl::Tensor deformed_input = deformed_input_vec[0];
  // Compute DeformableConvolution.
  edsl::Tensor result = op::convolution(deformed_input, F)
                            .strides(std::vector<int64_t>{F_shape.begin() + 2, F_shape.end()})
                            .autopad_mode(plaidml::op::AutoPadMode::VALID)
                            .input_layout(plaidml::op::TensorLayout::NCX)
                            .filter_layout(plaidml::op::TensorLayout::KCX);
  return result;
}

}  // namespace

namespace PlaidMLPlugin {

void registerDeformableConvolution() {
  registerOp("DeformableConvolution", [](const Context& ctx) {
    auto* layer = ngraph::as_type<ngraph::opset4::DeformableConvolution>(ctx.layer);
    IE_ASSERT(ctx.operands.size() == 3);
    auto I = ctx.operands.at(0);
    // OFF means offset, and it is deformable values tensor in OpenVINO doc.
    auto OFF = ctx.operands.at(1);
    auto F = ctx.operands.at(2);
    auto I_shape = I.compute_shape().sizes();
    auto OFF_shape = OFF.compute_shape().sizes();
    auto F_shape = F.compute_shape().sizes();
    auto G = layer->get_group();
    if (G != 1) {
      THROW_IE_EXCEPTION << "Group not equal 1 isn't supported in openvino for now.";
    }
    auto DG = layer->get_deformable_group();
    auto rank = I.rank();
    auto autopad_mode = to_plaidml(layer->get_auto_pad());
    // Compute manual_padding.
    std::vector<int64_t> manual_padding;
    if (autopad_mode == plaidml::op::AutoPadMode::EXPLICIT) {
      auto pads_begin = layer->get_pads_begin();
      auto pads_end = layer->get_pads_end();
      manual_padding.insert(manual_padding.end(), pads_begin.begin(), pads_begin.end());
      manual_padding.insert(manual_padding.end(), pads_end.begin(), pads_end.end());
    }
    while (manual_padding.size() < 2 * (rank - 2)) {
      manual_padding.push_back(0);
    }
    auto strides = layer->get_strides();
    auto dilations = layer->get_dilations();
    // Compute pad_before and the shape of output.
    std::vector<TensorDim> pad_befores, output_sizes;
    for (auto i = 0; i < rank - 2; ++i) {
      auto pad_before_and_output = compute_padding_and_output_size(
          TensorDim(I_shape[i + 2]), TensorDim(F_shape[i + 2]), strides[i], autopad_mode, manual_padding[i],
          manual_padding[i + rank - 2], dilations[i], 1, false);
      pad_befores.push_back(pad_before_and_output.first);
      output_sizes.push_back(pad_before_and_output.second);
    }
    // Compute the spatial size of filter;
    auto F_spatial_size = 1;
    for (auto i = 0; i < F_shape.size() - 2; ++i) {
      F_spatial_size *= F_shape[i + 2];
    }
    // Validate the shape of offset.
    if (OFF_shape[1] != (rank - 2) * DG * F_spatial_size) {
      THROW_IE_EXCEPTION << "Incorrect shape for DeformableConvolution.";
    }

    edsl::Tensor O = compute_deformable_convolution(I, OFF, F, I_shape, OFF_shape, F_shape, G, DG, rank, strides,
                                                    dilations, pad_befores);
    return edsl::make_tuple(O);
  });
}

}  // namespace PlaidMLPlugin
