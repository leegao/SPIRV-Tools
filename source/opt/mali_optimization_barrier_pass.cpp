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

#include "source/opt/mali_optimization_barrier_pass.h"

#include <cstdio>
#include <memory>
#include <vector>

#include "source/opt/ir_builder.h"

#define LOG_(level, fmt, ...)                                         \
  do {                                                                \
    char buffer[256];                                                 \
    snprintf(buffer, sizeof(buffer), "OptimizationBarrierPass: " fmt, \
             ##__VA_ARGS__);                                          \
    consumer()(level, __FUNCTION__, {__LINE__, 0, 0}, buffer);        \
  } while (0)

#define LOGD(fmt, ...) LOG_(SPV_MSG_DEBUG, fmt, ##__VA_ARGS__)
#define LOG(fmt, ...) LOG_(SPV_MSG_INFO, fmt, ##__VA_ARGS__)
#define LOGE(fmt, ...) LOG_(SPV_MSG_ERROR, fmt, ##__VA_ARGS__)

namespace spvtools {
namespace opt {

Pass::Status MaliOptimizationBarrierPass::Process() {
  bool modified = false;
  auto scalar_modifications = 0u;
  auto vector_modifications = 0u;

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
        if (inst->NumInOperands() < 2) continue;
        const auto shift_amount_id = inst->GetSingleWordInOperand(1);
        const analysis::Constant* shift_constant =
            context()->get_constant_mgr()->FindDeclaredConstant(
                shift_amount_id);

        // If the shift is a constant (may trigger constant folding bug)
        if (!shift_constant) continue;
        const bool shift_is_scalar =
            shift_constant->type()->AsInteger() != nullptr;
        const analysis::Vector* shift_vector =
            shift_constant->type()->AsVector();
        const bool shift_is_vector =
            shift_vector != nullptr &&
            shift_vector->element_type()->AsInteger() != nullptr;

        if (!shift_is_scalar && !shift_is_vector) continue;

        // The zero inserted by the barrier is derived from the shift's
        // result type.
        const uint32_t original_result_type_id = inst->type_id();
        analysis::Type* result_type =
            context()->get_type_mgr()->GetType(original_result_type_id);
        const bool result_is_vector =
            result_type != nullptr && result_type->AsVector() != nullptr;
        const uint32_t const_0_id =
            result_is_vector
                ? GetOrCreateConstantZeroVector(original_result_type_id)
                : GetOrCreateConstantZero(original_result_type_id);
        if (const_0_id == 0) {
          LOGE("Failed to get or create a zero constant");
          return Status::Failure;
        }

        // uint 0's for the OpBitFieldInsert operands
        uint32_t const_0_uint_id =
            context()->get_constant_mgr()->GetUIntConstId(0);
        if (const_0_uint_id == 0) {
          LOGE("Failed to get or create %%uint_0");
          return Status::Failure;
        }

        // Allocate a temp result id for the shift
        uint32_t temp_result_id = context()->TakeNextId();
        if (temp_result_id == 0) {
          LOGE("Failed to allocate new temp result_id for %s",
               inst->PrettyPrint().c_str());
          return Status::Failure;
        }
        // Clone the shift under a temporary id and turn the original
        // instruction into the barrier.  This keeps the original
        // result id (and all of its uses) valid at every step.
        std::unique_ptr<Instruction> cloned(inst->Clone(context()));
        cloned->SetResultId(temp_result_id);
        Instruction* shifted = inst->InsertBefore(std::move(cloned));
        get_def_use_mgr()->AnalyzeInstDefUse(shifted);

        // %original_result_id = OpBitFieldInsert %type
        //     %temp_result_id 0 0 0
        inst->SetOpcode(spv::Op::OpBitFieldInsert);
        inst->SetInOperands(Instruction::OperandList{
            {SPV_OPERAND_TYPE_ID, {temp_result_id}},     // base
            {SPV_OPERAND_TYPE_ID, {const_0_id}},         // insert
            {SPV_OPERAND_TYPE_ID, {const_0_uint_id}},    // offset
            {SPV_OPERAND_TYPE_ID, {const_0_uint_id}}});  // count
        context()->UpdateDefUse(inst);
        modified = true;
        if (result_is_vector) {
          vector_modifications += 1;
        } else {
          scalar_modifications += 1;
        }
      }
    }
  }

  if (modified) {
    LOG("Added optimization barriers to all functions in the module, "
        "scalar barriers = %d, vector barriers = %d",
        scalar_modifications, vector_modifications);
  }
  return modified ? Status::SuccessWithChange : Status::SuccessWithoutChange;
}

uint32_t MaliOptimizationBarrierPass::GetOrCreateConstantZero(
    const uint32_t type_id) const {
  analysis::TypeManager* type_mgr = context()->get_type_mgr();
  analysis::ConstantManager* const_mgr = context()->get_constant_mgr();

  analysis::Type* type = type_mgr->GetType(type_id);
  if (!type) return 0;
  const auto* int_type = type->AsInteger();
  if (!int_type) {
    return 0;
  }

  // GetUIntConstId/GetSIntConstId always produce a 32-bit constant, which is
  // the wrong type for a 64-bit shift.  Build one of the exact result type.
  std::vector<uint32_t> words((int_type->width() + 31) / 32, 0);
  const analysis::Constant* zero = const_mgr->GetConstant(type, words);
  if (!zero) return 0;
  Instruction* def = const_mgr->GetDefiningInstruction(zero, type_id);
  return def ? def->result_id() : 0;
}

uint32_t MaliOptimizationBarrierPass::GetOrCreateConstantZeroVector(
    const uint32_t type_id) const {
  analysis::TypeManager* type_mgr = context()->get_type_mgr();
  analysis::ConstantManager* const_mgr = context()->get_constant_mgr();

  analysis::Type* type = type_mgr->GetType(type_id);
  if (!type) {
    return 0;
  }

  const auto* vec_type = type->AsVector();
  if (!vec_type) {
    return 0;
  }

  if (!vec_type->element_type()->AsInteger()) {
    return 0;
  }

  const uint32_t component_type_id = type_mgr->GetId(vec_type->element_type());
  if (component_type_id == 0) {
    return 0;
  }
  const uint32_t component = GetOrCreateConstantZero(component_type_id);
  if (component == 0) {
    return 0;
  }

  std::vector<uint32_t> component_ids(vec_type->element_count(), component);
  const analysis::Constant* zero = const_mgr->GetConstant(type, component_ids);
  if (!zero) {
    return 0;
  }
  Instruction* def = const_mgr->GetDefiningInstruction(zero, type_id);
  return def ? def->result_id() : 0;
}

}  // namespace opt
}  // namespace spvtools