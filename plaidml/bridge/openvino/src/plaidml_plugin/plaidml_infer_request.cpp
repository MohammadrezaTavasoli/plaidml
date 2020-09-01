// Copyright (C) 2020 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include "plaidml_infer_request.hpp"

#include <vector>

#include "ie_layers.h"  // NOLINT[build/include_subdir]

#include "pmlc/util/logging.h"

using namespace plaidml;          // NOLINT[build/namespaces]
using namespace InferenceEngine;  // NOLINT[build/namespaces]

static Blob::Ptr make_shared_blob(const TensorDesc& desc) {
  const auto prec = desc.getPrecision();

#define CASE(prec) \
  case prec:       \
    return InferenceEngine::make_shared_blob<PrecisionTrait<prec>::value_type>(desc);

  switch (prec) {
    CASE(Precision::FP32);
    CASE(Precision::FP16);
    CASE(Precision::Q78);
    CASE(Precision::U16);
    CASE(Precision::U8);
    CASE(Precision::I8);
    CASE(Precision::BOOL);
    CASE(Precision::I16);
    CASE(Precision::I32);
    CASE(Precision::I64);
    CASE(Precision::BIN);

    default:
      THROW_IE_EXCEPTION << "The plugin does not support input " << prec.name() << " precision";
  }
}

namespace PlaidMLPlugin {

PlaidMLInferRequest::PlaidMLInferRequest(const InputsDataMap& networkInputs, const OutputsDataMap& networkOutputs,
                                         const edsl::Program& program,
                                         const std::unordered_map<std::string, edsl::Tensor>& tensorIOMap)
    : InferRequestInternal(networkInputs, networkOutputs),
      tensorIOMap_(tensorIOMap),
      binder_(program),
      exec_(binder_.compile()) {
  IVLOG(1, "Program:\n" << program.str());
  AllocateInputs();
  AllocateOutputs();
}

void PlaidMLInferRequest::InferImpl() {
  IVLOG(1, "PlaidMLInferRequest::InferImpl>");
  IVLOG(2, "  _inputs: " << _inputs);
  IVLOG(3, "  tensorIOMap_: " << tensorIOMap_);
  execDataPreprocessing(_inputs);

  SyncInput();
  exec_->run();
  SyncOutput();
}

void PlaidMLInferRequest::GetPerformanceCounts(std::map<std::string, InferenceEngineProfileInfo>& perfMap) const {
  throw std::logic_error("PlaidMLInferRequest::GetPerformanceCounts not implemented");
}

void PlaidMLInferRequest::AllocateInputs() {
  for (const auto& kvp : _networkInputs) {
    const auto& name = kvp.first;
    const auto& desc = kvp.second->getTensorDesc();
    auto info = _inputs.emplace(name, make_shared_blob(desc));
    info.first->second->allocate();
  }
}

void PlaidMLInferRequest::AllocateOutputs() {
  for (const auto& kvp : _networkOutputs) {
    const auto& name = kvp.first;
    const auto& desc = kvp.second->getTensorDesc();
    auto info = _outputs.emplace(name, make_shared_blob(desc));
    info.first->second->allocate();
  }
}

void PlaidMLInferRequest::SyncInput() {
  for (const auto& kvp : _networkInputs) {
    const auto& name = kvp.first;
    const auto& tensor = tensorIOMap_.at(name);
    if (true) {  // TODO: just delete this and use the else, this is a hack
      auto view = binder_.input(tensor).mmap_discard();
      size_t view_size = view.size();
      std::vector<uint8_t> direct_u8_vec(view_size);
      memcpy(direct_u8_vec.data(), _inputs[name]->buffer(), direct_u8_vec.size() * sizeof(uint8_t));
      if (VLOG_IS_ON(1)) {
        // For pretty-printing (if you print uint8_t directly it's interpreted as a char)
        std::vector<unsigned short> direct_u8_numeric_vec(direct_u8_vec.begin(),  // NOLINT(runtime/int)
                                                          direct_u8_vec.end());
        IVLOG(1, "Syncing input '" << name << "' which has buffer (interpreted as u8 directly) of "
                                   << direct_u8_numeric_vec);
      }
      memcpy(view.data(), direct_u8_vec.data(), view_size);
    } else {
      binder_.input(tensor).copy_from(_inputs[name]->buffer());
    }
  }
}

void PlaidMLInferRequest::SyncOutput() {
  for (const auto& kvp : _networkOutputs) {
    const auto& name = kvp.first;
    const auto& tensor = tensorIOMap_.at(name);
    if (true) {  // TODO: just delete this and use the else, this is a hack
      auto view = binder_.output(tensor).mmap_current();
      size_t view_size = view.size();
      std::vector<uint8_t> raw_vec(view_size);  // TODO: Set to the specific type I'm using in tests
      memcpy(raw_vec.data(), view.data(), view.size());
      std::vector<float> float_vec(raw_vec.begin(), raw_vec.end());
      IVLOG(1, "Syncing output '" << name << "' which has float buffer " << float_vec);
      memcpy(_outputs[name]->buffer(), float_vec.data(), float_vec.size() * sizeof(float));
    } else {
      binder_.output(tensor).copy_into(_outputs[name]->buffer());
    }
  }
}

}  // namespace PlaidMLPlugin
