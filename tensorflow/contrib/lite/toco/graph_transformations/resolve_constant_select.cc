/* Copyright 2018 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/
#include <vector>

#include "tensorflow/contrib/lite/toco/graph_transformations/graph_transformations.h"
#include "tensorflow/contrib/lite/toco/graph_transformations/remove_trivial_passthrough.h"
#include "tensorflow/contrib/lite/toco/model.h"
#include "tensorflow/contrib/lite/toco/tooling_util.h"
#include "tensorflow/core/platform/logging.h"

namespace toco {

// Resolves a constant Select operation.
//
// This implementation is looking strictly for all-or-nothing on the select
// condition. It's possible to enhance this by looking per-element and possibly
// producing a Mul op.
bool ResolveConstantSelect::Run(Model* model, std::size_t op_index) {
  auto it = model->operators.begin() + op_index;
  const auto* base_op = it->get();
  if (base_op->type != OperatorType::kSelect) {
    return false;
  }
  const auto* op = static_cast<const SelectOperator*>(base_op);

  CHECK_GE(op->inputs.size(), 3);
  CHECK_EQ(op->outputs.size(), 1);

  // If the output of this op is a non-discardable array such as an input_array
  // or a state array of the model, then this is a job for RemoveUnusedOp, not
  // for constants-propagation.
  if (!IsDiscardableArray(*model, op->outputs[0])) {
    return false;
  }

  auto& output_array = model->GetArray(op->outputs[0]);
  if (output_array.data_type == ArrayDataType::kNone) {
    // Yield until the output type has been set by PropagateArrayDataTypes.
    return false;
  }
  if (!output_array.has_shape()) {
    // Yield until the output shape has been set by PropagateFixedShapes.
    return false;
  }

  // We require the cond input to be constant.
  if (!IsConstantParameterArray(*model, op->inputs[0])) {
    return false;
  }
  const Array& cond_array = model->GetArray(op->inputs[0]);
  CHECK(cond_array.data_type == ArrayDataType::kBool)
      << "Only bool conditions are supported";
  const auto& cond_data = cond_array.GetBuffer<ArrayDataType::kBool>().data;
  if (cond_data.empty()) {
    return false;
  }

  // Check if the condition is the same for all elements.
  bool cond_value = cond_data[0];
  for (size_t i = 1; i < cond_data.size(); ++i) {
    if (cond_data[i] != cond_value) {
      AddMessageF(
          "Cannot resolve %s as constant; cond_array has differing "
          "per-element values",
          LogName(*op));
      return false;
    }
  }

  // Pass-through the selected input.
  return RemoveTrivialPassthroughOp(this, model, op_index, cond_value ? 1 : 2);
}

}  // namespace toco
