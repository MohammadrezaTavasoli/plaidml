// Copyright (C) 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "plaidml_ops.hpp"
#include "plaidml_util.hpp"

#include "ngraph/opsets/opset.hpp"
#include "ngraph/opsets/opset4.hpp"

#include "plaidml/op/op.h"

using namespace plaidml;          // NOLINT[build/namespaces]
using namespace InferenceEngine;  // NOLINT[build/namespaces]

namespace PlaidMLPlugin {

void registerRange() {
  registerOp("range", [](const Context& ctx) {
    auto* layer = dynamic_cast<ngraph::opset4::Range*>(ctx.layer);
    auto type = to_plaidml(layer->get_element_type());
    if (type != plaidml::DType::INT64) {
      // TODO: Expand type support by casting `range_data` based on `type`
      THROW_IE_EXCEPTION << "PlaidML plugin currently only supports i64 for Range op";
    }
    auto start = cast_constant_operand<int64_t>(0, layer)[0];
    auto stop = cast_constant_operand<int64_t>(1, layer)[0];
    auto step = cast_constant_operand<int64_t>(2, layer)[0];
    std::vector<int64_t> range_data;
    if (step == 0) {
      THROW_IE_EXCEPTION << "Range requires non-zero step value";
    }
    if (step > 0) {
      int64_t curr_val = start;
      while (curr_val < stop) {
        range_data.push_back(curr_val);
        curr_val += step;
      }
    } else {
      int64_t curr_val = start;
      while (curr_val > stop) {
        range_data.push_back(curr_val);
        curr_val += step;
      }
    }
    std::vector<int64_t> dims(1, range_data.size());
    TensorShape shape(type, dims);
    Buffer buffer(shape);
    buffer.copy_from(range_data.data());
    return edsl::make_tuple(edsl::Constant(buffer, layer->get_friendly_name()));
  });
}

}  // namespace PlaidMLPlugin
