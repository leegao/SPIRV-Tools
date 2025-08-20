// Copyright (c) 2025 Lee Gao
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "mali_optimization_barrier_pass.h"

#include "source/opt/ir_builder.h"
#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#define LOG_(level, fmt, ...)                                         \
  do {                                                                \
    char buffer[256];                                                 \
    snprintf(buffer, sizeof(buffer), "OptimizationBarrierPass: " fmt, \
             ## __VA_ARGS__);                                          \
    consumer()(level, __FUNCTION__, {__LINE__, 0, 0}, buffer);        \
  } while (0)

#define LOGD(fmt, ...) LOG_(SPV_MSG_DEBUG, fmt, ## __VA_ARGS__)
#define LOG(fmt, ...) LOG_(SPV_MSG_INFO, fmt, ## __VA_ARGS__)
#define LOGE(fmt, ...) LOG_(SPV_MSG_ERROR, fmt, ## __VA_ARGS__)

namespace spvtools {
namespace opt {

Pass::Status MaliOptimizationBarrierPass::Process() {
  bool modified = false;

  // Iterate over all functions in the module.
  for (auto& func : *context()->module()) {
    if (func.IsDeclaration()) {
      continue;
    }

    for (auto block_itr = func.begin(); block_itr != func.end(); ++block_itr) {
      for (auto inst_itr = block_itr->begin(); inst_itr != block_itr->end();
           ++inst_itr) {
        Instruction* inst = &*inst_itr;
        // Look for OpShiftLeftLogical <Value> <Shift>
        if (inst->opcode() != spv::Op::OpShiftLeftLogical) continue;
        const auto shift_amount_id = inst->GetSingleWordInOperand(1);
        const analysis::Constant* shift_constant =
            context()->get_constant_mgr()->FindDeclaredConstant(
                shift_amount_id);

        // If the shift is a constant (may trigger constant folding bug)
        // TODO: generalize to ivecs and uvecs as well
        if (!shift_constant || !shift_constant->type()->AsInteger()) continue;

        uint32_t original_result_type_id = inst->type_id();
        uint32_t original_result_id = inst->result_id();
        uint32_t const_0_id = GetOrCreateConstantZero(original_result_type_id);
        if (const_0_id == 0) {
          LOGE("Failed to get or create %%int_0 or %%uint_9");
          return Status::Failure;
        }

        // Allocate a temp result id for the shift
        uint32_t temp_result_id = context()->TakeNextId();
        if (temp_result_id == 0) {
          LOGE("Failed to allocate new temp result_id for %s",
               inst->PrettyPrint().c_str());
          return Status::Failure;
        }
        inst->SetResultId(temp_result_id);

        // Create an OpBitFieldInsert no-op instruction to block constant
        // folding optimizations that may be broken on some drivers
        // %original_result_id = OpBitFieldInsert %type %temp_result_id 0 0 0
        InstructionBuilder builder(context(), inst->NextNode());
        auto noop_barrier_inst =
            std::make_unique<Instruction>(
                context(),
                spv::Op::OpBitFieldInsert,
                original_result_type_id,
                original_result_id,
                Instruction::OperandList{
                    {SPV_OPERAND_TYPE_ID, {temp_result_id}},
                    {SPV_OPERAND_TYPE_ID, {const_0_id}},
                    {SPV_OPERAND_TYPE_ID, {const_0_id}},
                    {SPV_OPERAND_TYPE_ID, {const_0_id}}});
        builder.AddInstruction(std::move(noop_barrier_inst));
        modified = true;
      }
    }
  }

  if (modified) {
    LOGD("Added optimization barriers to all functions in the module");
  }
  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

uint32_t MaliOptimizationBarrierPass::GetOrCreateConstantZero(
    const uint32_t type_id) const {
  analysis::Type* type = context()->get_type_mgr()->GetType(type_id);
  if (!type) return 0;
  const analysis::Integer* int_type = type->AsInteger();
  if (!int_type) {
    return 0;
  }
  if (int_type->IsSigned()) {
    return context()->get_constant_mgr()->GetSIntConstId(0);
  }
  return context()->get_constant_mgr()->GetUIntConstId(0);
}

}  // namespace opt
}  // namespace spvtools