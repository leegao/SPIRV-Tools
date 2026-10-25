// Copyright (c) 2026 Lee Gao
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// Tests for the Mali-specific passes:
//   FixMaliSpecConstantCompositePass, MaliOptimizationBarrierPass,
//   MaliBitwiseScalarPass, MaliClampZeroPass, MaliPerVertexBlockPass,
//   MaliLockstepPass, LowerClipCullDistancePass.

#include <string>
#include <tuple>

#include "test/opt/pass_fixture.h"
#include "test/opt/pass_utils.h"

namespace spvtools {
namespace opt {
namespace {

using MaliPassTest = PassTest<::testing::Test>;

size_t CountOccurrences(const std::string &text, const std::string &needle) {
    size_t count = 0;
    for (size_t pos = text.find(needle); pos != std::string::npos;
         pos = text.find(needle, pos + 1)) {
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// FixMaliSpecConstantCompositePass
// ---------------------------------------------------------------------------

TEST_F(MaliPassTest, FixMaliSpecConstantCompositePromotesBooleanVector) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %sc SpecId 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
         %sc = OpSpecConstantTrue %bool
     %v4bool = OpTypeVector %bool 4
       %comp = OpConstantComposite %v4bool %sc %sc %sc %sc
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<FixMaliSpecConstantCompositePass>(
        text, /*skip_nop=*/true, /*do_validation=*/true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_NE(std::string::npos,
              std::get<0>(result).find("OpSpecConstantComposite"));
}

TEST_F(MaliPassTest, FixMaliSpecConstantCompositeLeavesPlainConstants) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
     %v4bool = OpTypeVector %bool 4
       %comp = OpConstantComposite %v4bool %true %true %true %true
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<FixMaliSpecConstantCompositePass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

TEST_F(MaliPassTest, FixMaliSpecConstantCompositePromotesMixedOperands) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %sc SpecId 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
         %sc = OpSpecConstantTrue %bool
     %v4bool = OpTypeVector %bool 4
       %comp = OpConstantComposite %v4bool %true %sc %true %sc
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<FixMaliSpecConstantCompositePass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(
        1u, CountOccurrences(std::get<0>(result), "= OpSpecConstantComposite"));
    EXPECT_EQ(0u,
              CountOccurrences(std::get<0>(result), "= OpConstantComposite"));
}

TEST_F(MaliPassTest, FixMaliSpecConstantCompositePromotesSpecConstantOp) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %sc SpecId 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
         %sc = OpSpecConstantTrue %bool
        %not = OpSpecConstantOp %bool LogicalNot %sc
     %v2bool = OpTypeVector %bool 2
       %comp = OpConstantComposite %v2bool %sc %not
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<FixMaliSpecConstantCompositePass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(
        1u, CountOccurrences(std::get<0>(result), "= OpSpecConstantComposite"));
    EXPECT_EQ(0u,
              CountOccurrences(std::get<0>(result), "= OpConstantComposite"));
}

TEST_F(MaliPassTest, FixMaliSpecConstantCompositeLeavesIntegerVector) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %sc SpecId 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %uint_1 = OpConstant %uint 1
         %sc = OpSpecConstant %uint 1
     %v2uint = OpTypeVector %uint 2
       %comp = OpConstantComposite %v2uint %sc %uint_1
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<FixMaliSpecConstantCompositePass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

// ---------------------------------------------------------------------------
// MaliOptimizationBarrierPass
// ---------------------------------------------------------------------------

TEST_F(MaliPassTest, MaliOptimizationBarrierAfterConstantShift) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %uint_4 = OpConstant %uint 4
     %uint_2 = OpConstant %uint 2
       %main = OpFunction %void None %fn
      %label = OpLabel
      %shift = OpShiftLeftLogical %uint %uint_4 %uint_2
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<MaliOptimizationBarrierPass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_NE(std::string::npos, std::get<0>(result).find("OpBitFieldInsert"));
}

TEST_F(MaliPassTest, MaliOptimizationBarrierLeavesVariableShift) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %uint_4 = OpConstant %uint 4
%ptr_private_uint = OpTypePointer Private %uint
     %shift_var = OpVariable %ptr_private_uint Private
       %main = OpFunction %void None %fn
      %label = OpLabel
          %x = OpLoad %uint %shift_var
      %shift = OpShiftLeftLogical %uint %uint_4 %x
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<MaliOptimizationBarrierPass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

TEST_F(MaliPassTest, MaliOptimizationBarrierAddsBarrierToVectorShift) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
       %base = OpConstantComposite %v2uint %uint_1 %uint_2
     %amount = OpConstantComposite %v2uint %uint_1 %uint_3
       %main = OpFunction %void None %fn
      %label = OpLabel
      %shift = OpShiftLeftLogical %v2uint %base %amount
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<MaliOptimizationBarrierPass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    EXPECT_EQ(1u, CountOccurrences(disassembly, "OpBitFieldInsert"));
    // The clone keeps the original shift.
    EXPECT_EQ(1u, CountOccurrences(disassembly, "OpShiftLeftLogical"));
}

TEST_F(MaliPassTest, MaliOptimizationBarrierAddsBarrierToSixtyFourBitShift) {
    const std::string text = R"(
               OpCapability Shader
               OpCapability Int64
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
     %ulong = OpTypeInt 64 0
   %ulong_1 = OpConstant %ulong 1
   %ulong_2 = OpConstant %ulong 2
       %main = OpFunction %void None %fn
      %label = OpLabel
      %shift = OpShiftLeftLogical %ulong %ulong_1 %ulong_2
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<MaliOptimizationBarrierPass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_NE(std::string::npos, std::get<0>(result).find("OpBitFieldInsert"));
}

TEST_F(MaliPassTest, MaliOptimizationBarrierAddsBarrierToSignedShift) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
        %int = OpTypeInt 32 1
      %int_1 = OpConstant %int 1
      %int_2 = OpConstant %int 2
       %main = OpFunction %void None %fn
      %label = OpLabel
      %shift = OpShiftLeftLogical %int %int_1 %int_2
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<MaliOptimizationBarrierPass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_NE(std::string::npos, std::get<0>(result).find("OpBitFieldInsert"));
}

TEST_F(MaliPassTest, MaliOptimizationBarrierLeavesNonShift) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
       %main = OpFunction %void None %fn
      %label = OpLabel
        %sum = OpIAdd %uint %uint_1 %uint_2
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<MaliOptimizationBarrierPass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
    EXPECT_EQ(0u, CountOccurrences(std::get<0>(result), "OpBitFieldInsert"));
}

// ---------------------------------------------------------------------------
// MaliBitwiseScalarPass
// ---------------------------------------------------------------------------

TEST_F(MaliPassTest, MaliBitwiseScalarScalarisesConstructedOperand) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
       %main = OpFunction %void None %fn
      %label = OpLabel
          %v = OpCompositeConstruct %v2uint %uint_1 %uint_2
      %splat = OpCompositeConstruct %v2uint %uint_1 %uint_1
          %r = OpBitwiseOr %v2uint %v %splat
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliBitwiseScalarPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    EXPECT_NE(std::string::npos, disassembly.find("OpCompositeExtract"));
    EXPECT_EQ(2u, CountOccurrences(disassembly, "OpBitwiseOr"));
}

TEST_F(MaliPassTest, MaliBitwiseScalarLeavesLoadedOperands) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
%ptr_v2uint = OpTypePointer Private %v2uint
      %a_ptr = OpVariable %ptr_v2uint Private
      %b_ptr = OpVariable %ptr_v2uint Private
       %main = OpFunction %void None %fn
      %label = OpLabel
          %a = OpLoad %v2uint %a_ptr
          %b = OpLoad %v2uint %b_ptr
          %r = OpBitwiseAnd %v2uint %a %b
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliBitwiseScalarPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

TEST_F(MaliPassTest, MaliBitwiseScalarRewriteAllTakesLoadedOperands) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
%ptr_v2uint = OpTypePointer Private %v2uint
      %a_ptr = OpVariable %ptr_v2uint Private
      %b_ptr = OpVariable %ptr_v2uint Private
       %main = OpFunction %void None %fn
      %label = OpLabel
          %a = OpLoad %v2uint %a_ptr
          %b = OpLoad %v2uint %b_ptr
          %r = OpBitwiseAnd %v2uint %a %b
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<MaliBitwiseScalarPass>(
        text, true, true, /*rewrite_all=*/true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpBitwiseAnd"));
}

TEST_F(MaliPassTest, MaliBitwiseScalarScalarisesShiftLeft) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
       %main = OpFunction %void None %fn
      %label = OpLabel
          %v = OpCompositeConstruct %v2uint %uint_1 %uint_2
      %shift = OpShiftLeftLogical %v2uint %v %v
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliBitwiseScalarPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    EXPECT_EQ(2u, CountOccurrences(disassembly, "OpShiftLeftLogical"));
    // Two source lanes per operand, two operands.
    EXPECT_EQ(4u, CountOccurrences(disassembly, "OpCompositeExtract"));
}

TEST_F(MaliPassTest, MaliBitwiseScalarScalarisesNot) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
       %main = OpFunction %void None %fn
      %label = OpLabel
          %v = OpCompositeConstruct %v2uint %uint_1 %uint_2
          %r = OpNot %v2uint %v
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliBitwiseScalarPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    EXPECT_EQ(2u, CountOccurrences(disassembly, "OpNot"));
    EXPECT_EQ(2u, CountOccurrences(disassembly, "OpCompositeExtract"));
}

TEST_F(MaliPassTest, MaliBitwiseScalarLooksThroughCopyObject) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
       %main = OpFunction %void None %fn
      %label = OpLabel
          %v = OpCompositeConstruct %v2uint %uint_1 %uint_2
       %copy = OpCopyObject %v2uint %v
          %r = OpBitwiseXor %v2uint %copy %v
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliBitwiseScalarPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpBitwiseXor"));
}

TEST_F(MaliPassTest, MaliBitwiseScalarScalarisesVectorShuffleOperand) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
       %main = OpFunction %void None %fn
      %label = OpLabel
          %v = OpCompositeConstruct %v2uint %uint_1 %uint_2
       %shuf = OpVectorShuffle %v2uint %v %v 1 0
          %r = OpBitwiseAnd %v2uint %shuf %v
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliBitwiseScalarPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpBitwiseAnd"));
}

TEST_F(MaliPassTest, MaliBitwiseScalarScalarisesConstantVectors) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
     %uint_3 = OpConstant %uint 3
          %a = OpConstantComposite %v2uint %uint_1 %uint_2
          %b = OpConstantComposite %v2uint %uint_3 %uint_2
       %main = OpFunction %void None %fn
      %label = OpLabel
          %r = OpBitwiseOr %v2uint %a %b
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliBitwiseScalarPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpBitwiseOr"));
}

TEST_F(MaliPassTest, MaliBitwiseScalarLeavesScalarOperations) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
       %main = OpFunction %void None %fn
      %label = OpLabel
          %r = OpBitwiseOr %uint %uint_1 %uint_2
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliBitwiseScalarPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
    EXPECT_EQ(0u, CountOccurrences(std::get<0>(result), "OpCompositeExtract"));
}

// ---------------------------------------------------------------------------
// MaliClampZeroPass
// ---------------------------------------------------------------------------

namespace {

const char *kClampZeroModule = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
      %float = OpTypeFloat 32
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
%ptr_private_bool = OpTypePointer Private %bool
   %cond_var = OpVariable %ptr_private_bool Private
       %glsl = OpExtInstImport "GLSL.std.450"
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpStore %cond_var %true
       %cond = OpLoad %bool %cond_var
        %sel = OpSelect %float %cond %float_0 %float_1
          %r = OpExtInst %float %glsl FClamp %sel %float_0 %float_1
               OpReturn
               OpFunctionEnd
)";

} // namespace

TEST_F(MaliPassTest, MaliClampZeroGivesConstantSelectedClampAnOpaqueZero) {
    auto result = SinglePassRunAndDisassemble<MaliClampZeroPass>(
        kClampZeroModule, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    // The original bool load plus the new opaque float zero load.
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpLoad"));
}

TEST_F(MaliPassTest, MaliClampZeroBitFieldInsertVariant) {
    auto result = SinglePassRunAndDisassemble<MaliClampZeroPass>(
        kClampZeroModule, true, true, /*rewrite_all=*/false,
        /*bitfield_insert=*/true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_NE(std::string::npos, std::get<0>(result).find("OpBitFieldInsert"));
}

TEST_F(MaliPassTest, MaliClampZeroIntegerBitFieldInsertVariant) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
       %uint = OpTypeInt 32 0
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
%ptr_private_bool = OpTypePointer Private %bool
   %cond_var = OpVariable %ptr_private_bool Private
       %glsl = OpExtInstImport "GLSL.std.450"
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpStore %cond_var %true
       %cond = OpLoad %bool %cond_var
        %sel = OpSelect %uint %cond %uint_0 %uint_1
          %r = OpExtInst %uint %glsl UMin %sel %uint_0
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<MaliClampZeroPass>(
        text, true, true, /*rewrite_all=*/false, /*bitfield_insert=*/true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(1u, CountOccurrences(std::get<0>(result), "OpBitFieldInsert"));
}

TEST_F(MaliPassTest, MaliClampZeroLeavesNonSelectedValue) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
%ptr_private_float = OpTypePointer Private %float
        %x = OpVariable %ptr_private_float Private
       %glsl = OpExtInstImport "GLSL.std.450"
       %main = OpFunction %void None %fn
      %label = OpLabel
        %v = OpLoad %float %x
          %r = OpExtInst %float %glsl FClamp %v %float_0 %float_1
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliClampZeroPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

TEST_F(MaliPassTest, MaliClampZeroRewritesMinWithConstantSelectedValue) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
      %float = OpTypeFloat 32
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
%ptr_private_bool = OpTypePointer Private %bool
   %cond_var = OpVariable %ptr_private_bool Private
       %glsl = OpExtInstImport "GLSL.std.450"
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpStore %cond_var %true
       %cond = OpLoad %bool %cond_var
        %sel = OpSelect %float %cond %float_0 %float_1
          %r = OpExtInst %float %glsl FMin %sel %float_0
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliClampZeroPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    // The bool load plus the opaque float zero load.
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpLoad"));
}

TEST_F(MaliPassTest, MaliClampZeroRewritesVectorMin) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
     %v2bool = OpTypeVector %bool 2
     %true2 = OpConstantComposite %v2bool %true %true
       %uint = OpTypeInt 32 0
     %v2uint = OpTypeVector %uint 2
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
       %zero = OpConstantComposite %v2uint %uint_0 %uint_0
        %one = OpConstantComposite %v2uint %uint_1 %uint_1
       %glsl = OpExtInstImport "GLSL.std.450"
       %main = OpFunction %void None %fn
      %label = OpLabel
        %sel = OpSelect %v2uint %true2 %zero %one
          %r = OpExtInst %v2uint %glsl UMin %sel %zero
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliClampZeroPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    // One opaque scalar load; one vector construction from it.
    EXPECT_EQ(1u, CountOccurrences(disassembly, "OpLoad"));
    EXPECT_EQ(1u, CountOccurrences(disassembly, "OpCompositeConstruct"));
}

TEST_F(MaliPassTest, MaliClampZeroSharesOneOpaqueZeroBetweenClamps) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
      %float = OpTypeFloat 32
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
%ptr_private_bool = OpTypePointer Private %bool
   %cond_var = OpVariable %ptr_private_bool Private
       %glsl = OpExtInstImport "GLSL.std.450"
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpStore %cond_var %true
       %cond = OpLoad %bool %cond_var
        %sel = OpSelect %float %cond %float_0 %float_1
         %r1 = OpExtInst %float %glsl FClamp %sel %float_0 %float_1
         %r2 = OpExtInst %float %glsl FClamp %sel %float_0 %float_1
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliClampZeroPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    // cond_var and one shared opaque zero variable; one load each.
    EXPECT_EQ(2u, CountOccurrences(disassembly, "OpVariable"));
    EXPECT_EQ(2u, CountOccurrences(disassembly, "OpLoad"));
}

TEST_F(MaliPassTest, MaliClampZeroFollowsConstantStoreForValue) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
      %float = OpTypeFloat 32
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
%ptr_private_bool = OpTypePointer Private %bool
%ptr_private_float = OpTypePointer Private %float
   %cond_var = OpVariable %ptr_private_bool Private
        %xvar = OpVariable %ptr_private_float Private
       %glsl = OpExtInstImport "GLSL.std.450"
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpStore %cond_var %true
       %cond = OpLoad %bool %cond_var
        %sel = OpSelect %float %cond %float_0 %float_1
               OpStore %xvar %sel
          %x = OpLoad %float %xvar
          %r = OpExtInst %float %glsl FClamp %x %float_0 %float_1
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliClampZeroPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    // cond_var, xvar, and the new opaque zero load.
    EXPECT_EQ(3u, CountOccurrences(std::get<0>(result), "OpLoad"));
}

TEST_F(MaliPassTest, MaliClampZeroLeavesValueWithAliasingCopyMemory) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
      %float = OpTypeFloat 32
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
%ptr_private_bool = OpTypePointer Private %bool
%ptr_private_float = OpTypePointer Private %float
   %cond_var = OpVariable %ptr_private_bool Private
        %xvar = OpVariable %ptr_private_float Private
        %yvar = OpVariable %ptr_private_float Private
       %glsl = OpExtInstImport "GLSL.std.450"
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpStore %cond_var %true
       %cond = OpLoad %bool %cond_var
        %sel = OpSelect %float %cond %float_0 %float_1
               OpStore %xvar %sel
               OpCopyMemory %xvar %yvar
          %x = OpLoad %float %xvar
          %r = OpExtInst %float %glsl FClamp %x %float_0 %float_1
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliClampZeroPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

TEST_F(MaliPassTest, MaliClampZeroSixtyFourBitBitFieldInsertFallsBack) {
    const std::string text = R"(
               OpCapability Shader
               OpCapability Float64
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %true = OpConstantTrue %bool
     %double = OpTypeFloat 64
   %double_0 = OpConstant %double 0
   %double_1 = OpConstant %double 1
%ptr_private_bool = OpTypePointer Private %bool
   %cond_var = OpVariable %ptr_private_bool Private
       %glsl = OpExtInstImport "GLSL.std.450"
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpStore %cond_var %true
       %cond = OpLoad %bool %cond_var
        %sel = OpSelect %double %cond %double_0 %double_1
          %r = OpExtInst %double %glsl FClamp %sel %double_0 %double_1
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<MaliClampZeroPass>(
        text, true, true, /*rewrite_all=*/false, /*bitfield_insert=*/true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    // 64-bit has no single-word OpBitFieldInsert path; it uses a variable.
    EXPECT_EQ(0u, CountOccurrences(std::get<0>(result), "OpBitFieldInsert"));
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpLoad"));
}

TEST_F(MaliPassTest, MaliClampZeroRewriteAllTakesUnselectedValue) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main"
               OpExecutionMode %main LocalSize 1 1 1
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
    %float_0 = OpConstant %float 0
    %float_1 = OpConstant %float 1
%ptr_private_float = OpTypePointer Private %float
        %x = OpVariable %ptr_private_float Private
       %glsl = OpExtInstImport "GLSL.std.450"
       %main = OpFunction %void None %fn
      %label = OpLabel
        %v = OpLoad %float %x
          %r = OpExtInst %float %glsl FClamp %v %float_0 %float_1
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<MaliClampZeroPass>(
        text, true, true, /*rewrite_all=*/true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpLoad"));
}

// ---------------------------------------------------------------------------
// MaliPerVertexBlockPass
// ---------------------------------------------------------------------------

TEST_F(MaliPassTest, MaliPerVertexBlockFoldsLooseGeometryOutput) {
    const std::string text = R"(
               OpCapability Geometry
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Geometry %main "main" %pos
               OpExecutionMode %main InputPoints
               OpExecutionMode %main OutputPoints
               OpExecutionMode %main OutputVertices 1
               OpDecorate %pos BuiltIn Position
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%ptr_out_v4float = OpTypePointer Output %v4float
        %pos = OpVariable %ptr_out_v4float Output
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliPerVertexBlockPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    EXPECT_NE(std::string::npos, disassembly.find("OpTypeStruct"));
    EXPECT_NE(std::string::npos, disassembly.find("OpMemberDecorate"));
    EXPECT_NE(std::string::npos, disassembly.find("BuiltIn Position"));
    EXPECT_NE(std::string::npos, disassembly.find("Block"));
}

TEST_F(MaliPassTest, MaliPerVertexBlockLeavesVertexShader) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Vertex %main "main" %pos
               OpDecorate %pos BuiltIn Position
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%ptr_out_v4float = OpTypePointer Output %v4float
        %pos = OpVariable %ptr_out_v4float Output
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliPerVertexBlockPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

TEST_F(MaliPassTest, MaliPerVertexBlockFoldsGeometryInputAndOutput) {
    const std::string text = R"(
               OpCapability Geometry
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Geometry %main "main" %in_pos %out_pos
               OpExecutionMode %main InputPoints
               OpExecutionMode %main OutputPoints
               OpExecutionMode %main OutputVertices 1
               OpDecorate %in_pos BuiltIn Position
               OpDecorate %out_pos BuiltIn Position
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %uint_1 = OpConstant %uint 1
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
   %v4_array = OpTypeArray %v4float %uint_1
%ptr_in_v4_array = OpTypePointer Input %v4_array
%ptr_out_v4float = OpTypePointer Output %v4float
     %in_pos = OpVariable %ptr_in_v4_array Input
    %out_pos = OpVariable %ptr_out_v4float Output
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliPerVertexBlockPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    EXPECT_EQ(2u, CountOccurrences(disassembly, "OpTypeStruct"));
    EXPECT_EQ(2u, CountOccurrences(disassembly, "BuiltIn Position"));
    EXPECT_EQ(2u, CountOccurrences(disassembly, " Block"));
    // The input keeps its array shape, with the block as the element type.
    EXPECT_NE(std::string::npos, disassembly.find("OpTypeArray"));
}

TEST_F(MaliPassTest, MaliPerVertexBlockIgnoresOrdinaryInterfaceVariables) {
    const std::string text = R"(
               OpCapability Geometry
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Geometry %main "main" %pos %color
               OpExecutionMode %main InputPoints
               OpExecutionMode %main OutputPoints
               OpExecutionMode %main OutputVertices 1
               OpDecorate %pos BuiltIn Position
               OpDecorate %color Location 0
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%ptr_out_v4float = OpTypePointer Output %v4float
        %pos = OpVariable %ptr_out_v4float Output
      %color = OpVariable %ptr_out_v4float Output
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliPerVertexBlockPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    EXPECT_EQ(1u, CountOccurrences(disassembly, "OpTypeStruct"));
    // The unrelated Location variable stays in the interface.
    EXPECT_NE(std::string::npos, disassembly.find("Location 0"));
    EXPECT_EQ(2u, CountOccurrences(disassembly, "OpVariable"));
}

TEST_F(MaliPassTest, MaliPerVertexBlockFoldsTessControlArrayedOutput) {
    const std::string text = R"(
               OpCapability Tessellation
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint TessellationControl %main "main" %pos
               OpExecutionMode %main OutputVertices 3
               OpDecorate %pos BuiltIn Position
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %uint = OpTypeInt 32 0
     %uint_3 = OpConstant %uint 3
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
   %v4_array = OpTypeArray %v4float %uint_3
%ptr_out_v4_array = OpTypePointer Output %v4_array
        %pos = OpVariable %ptr_out_v4_array Output
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliPerVertexBlockPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    EXPECT_EQ(1u, CountOccurrences(disassembly, "OpTypeStruct"));
    EXPECT_EQ(1u, CountOccurrences(disassembly, " Block"));
    EXPECT_NE(std::string::npos, disassembly.find("OpTypeArray"));
}

TEST_F(MaliPassTest, MaliPerVertexBlockKeepsInvariantDecoration) {
    const std::string text = R"(
               OpCapability Geometry
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Geometry %main "main" %pos
               OpExecutionMode %main InputPoints
               OpExecutionMode %main OutputPoints
               OpExecutionMode %main OutputVertices 1
               OpDecorate %pos BuiltIn Position
               OpDecorate %pos Invariant
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%ptr_out_v4float = OpTypePointer Output %v4float
        %pos = OpVariable %ptr_out_v4float Output
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliPerVertexBlockPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_NE(std::string::npos, std::get<0>(result).find("Invariant"));
}

TEST_F(MaliPassTest, MaliPerVertexBlockLeavesUnsupportedUse) {
    const std::string text = R"(
               OpCapability Geometry
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Geometry %main "main" %pos
               OpExecutionMode %main InputPoints
               OpExecutionMode %main OutputPoints
               OpExecutionMode %main OutputVertices 1
               OpDecorate %pos BuiltIn Position
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%ptr_out_v4float = OpTypePointer Output %v4float
        %pos = OpVariable %ptr_out_v4float Output
       %main = OpFunction %void None %fn
      %label = OpLabel
       %copy = OpCopyObject %ptr_out_v4float %pos
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliPerVertexBlockPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

// ---------------------------------------------------------------------------
// MaliLockstepPass
// ---------------------------------------------------------------------------

namespace {

const char *kLockstepModule = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_LocalInvocationId
               OpExecutionMode %main LocalSize 32 1 1
               OpDecorate %gl_LocalInvocationId BuiltIn LocalInvocationId
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%ptr_input_v3uint = OpTypePointer Input %v3uint
%gl_LocalInvocationId = OpVariable %ptr_input_v3uint Input
     %uint_0 = OpConstant %uint 0
    %uint_32 = OpConstant %uint 32
    %uint_64 = OpConstant %uint 64
%ptr_wg_uint = OpTypePointer Workgroup %uint
         %wg = OpVariable %ptr_wg_uint Workgroup
       %main = OpFunction %void None %fn
      %entry = OpLabel
       %liid = OpLoad %v3uint %gl_LocalInvocationId
        %tid = OpCompositeExtract %uint %liid 0
               OpBranch %header
     %header = OpLabel
          %i = OpPhi %uint %tid %entry %inext %continue
               OpLoopMerge %merge %continue None
               OpBranch %body
       %body = OpLabel
        %val = OpLoad %uint %wg
               OpStore %wg %val
       %cond = OpULessThanEqual %bool %uint_64 %i
               OpSelectionMerge %exit None
               OpBranchConditional %cond %exit %continue
       %exit = OpLabel
               OpBranch %merge
   %continue = OpLabel
      %inext = OpIAdd %uint %i %uint_32
               OpBranch %header
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

} // namespace

TEST_F(MaliPassTest, MaliLockstepMakesStridedLoopUniform) {
    auto result = SinglePassRunAndDisassemble<MaliLockstepPass>(kLockstepModule,
                                                                true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpPhi"));
}

TEST_F(MaliPassTest, MaliLockstepLeavesLoopWithoutWorkgroupAccess) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_LocalInvocationId
               OpExecutionMode %main LocalSize 32 1 1
               OpDecorate %gl_LocalInvocationId BuiltIn LocalInvocationId
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%ptr_input_v3uint = OpTypePointer Input %v3uint
%gl_LocalInvocationId = OpVariable %ptr_input_v3uint Input
     %uint_0 = OpConstant %uint 0
    %uint_32 = OpConstant %uint 32
    %uint_64 = OpConstant %uint 64
%ptr_private_uint = OpTypePointer Private %uint
         %x = OpVariable %ptr_private_uint Private
       %main = OpFunction %void None %fn
      %entry = OpLabel
       %liid = OpLoad %v3uint %gl_LocalInvocationId
        %tid = OpCompositeExtract %uint %liid 0
               OpBranch %header
     %header = OpLabel
          %i = OpPhi %uint %tid %entry %inext %continue
               OpLoopMerge %merge %continue None
               OpBranch %body
       %body = OpLabel
        %val = OpLoad %uint %x
               OpStore %x %val
       %cond = OpULessThanEqual %bool %uint_64 %i
               OpSelectionMerge %exit None
               OpBranchConditional %cond %exit %continue
       %exit = OpLabel
               OpBranch %merge
   %continue = OpLabel
      %inext = OpIAdd %uint %i %uint_32
               OpBranch %header
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliLockstepPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

TEST_F(MaliPassTest, MaliLockstepUsesUGreaterThanEqual) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_LocalInvocationId
               OpExecutionMode %main LocalSize 32 1 1
               OpDecorate %gl_LocalInvocationId BuiltIn LocalInvocationId
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%ptr_input_v3uint = OpTypePointer Input %v3uint
%gl_LocalInvocationId = OpVariable %ptr_input_v3uint Input
     %uint_0 = OpConstant %uint 0
    %uint_32 = OpConstant %uint 32
    %uint_64 = OpConstant %uint 64
%ptr_wg_uint = OpTypePointer Workgroup %uint
         %wg = OpVariable %ptr_wg_uint Workgroup
       %main = OpFunction %void None %fn
      %entry = OpLabel
       %liid = OpLoad %v3uint %gl_LocalInvocationId
        %tid = OpCompositeExtract %uint %liid 0
               OpBranch %header
     %header = OpLabel
          %i = OpPhi %uint %tid %entry %inext %continue
               OpLoopMerge %merge %continue None
               OpBranch %body
       %body = OpLabel
        %val = OpLoad %uint %wg
               OpStore %wg %val
       %cond = OpUGreaterThanEqual %bool %i %uint_64
               OpSelectionMerge %exit None
               OpBranchConditional %cond %exit %continue
       %exit = OpLabel
               OpBranch %merge
   %continue = OpLabel
      %inext = OpIAdd %uint %i %uint_32
               OpBranch %header
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliLockstepPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpPhi"));
}

TEST_F(MaliPassTest, MaliLockstepHandlesSixtyFourBitInduction) {
    const std::string text = R"(
               OpCapability Shader
               OpCapability Int64
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_LocalInvocationId
               OpExecutionMode %main LocalSize 32 1 1
               OpDecorate %gl_LocalInvocationId BuiltIn LocalInvocationId
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
      %ulong = OpTypeInt 64 0
     %v3uint = OpTypeVector %uint 3
%ptr_input_v3uint = OpTypePointer Input %v3uint
%gl_LocalInvocationId = OpVariable %ptr_input_v3uint Input
    %ulong_32 = OpConstant %ulong 32
    %ulong_64 = OpConstant %ulong 64
%ptr_wg_uint = OpTypePointer Workgroup %uint
         %wg = OpVariable %ptr_wg_uint Workgroup
       %main = OpFunction %void None %fn
      %entry = OpLabel
       %liid = OpLoad %v3uint %gl_LocalInvocationId
        %tid = OpCompositeExtract %uint %liid 0
      %tid64 = OpUConvert %ulong %tid
               OpBranch %header
     %header = OpLabel
          %i = OpPhi %ulong %tid64 %entry %inext %continue
               OpLoopMerge %merge %continue None
               OpBranch %body
       %body = OpLabel
        %val = OpLoad %uint %wg
               OpStore %wg %val
       %cond = OpULessThanEqual %bool %ulong_64 %i
               OpSelectionMerge %exit None
               OpBranchConditional %cond %exit %continue
       %exit = OpLabel
               OpBranch %merge
   %continue = OpLabel
      %inext = OpIAdd %ulong %i %ulong_32
               OpBranch %header
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliLockstepPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpPhi"));
}

TEST_F(MaliPassTest, MaliLockstepIgnoresWorkgroupAccessOutsideLoop) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_LocalInvocationId
               OpExecutionMode %main LocalSize 32 1 1
               OpDecorate %gl_LocalInvocationId BuiltIn LocalInvocationId
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%ptr_input_v3uint = OpTypePointer Input %v3uint
%gl_LocalInvocationId = OpVariable %ptr_input_v3uint Input
    %uint_32 = OpConstant %uint 32
    %uint_64 = OpConstant %uint 64
%ptr_wg_uint = OpTypePointer Workgroup %uint
         %wg = OpVariable %ptr_wg_uint Workgroup
       %main = OpFunction %void None %fn
      %entry = OpLabel
       %liid = OpLoad %v3uint %gl_LocalInvocationId
        %tid = OpCompositeExtract %uint %liid 0
     %outside = OpLoad %uint %wg
               OpBranch %header
     %header = OpLabel
          %i = OpPhi %uint %tid %entry %inext %continue
               OpLoopMerge %merge %continue None
               OpBranch %body
       %body = OpLabel
       %cond = OpULessThanEqual %bool %uint_64 %i
               OpSelectionMerge %exit None
               OpBranchConditional %cond %exit %continue
       %exit = OpLabel
               OpBranch %merge
   %continue = OpLabel
      %inext = OpIAdd %uint %i %uint_32
               OpBranch %header
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliLockstepPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

TEST_F(MaliPassTest, MaliLockstepLeavesNonStridedIncrement) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_LocalInvocationId
               OpExecutionMode %main LocalSize 32 1 1
               OpDecorate %gl_LocalInvocationId BuiltIn LocalInvocationId
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%ptr_input_v3uint = OpTypePointer Input %v3uint
%gl_LocalInvocationId = OpVariable %ptr_input_v3uint Input
     %uint_1 = OpConstant %uint 1
    %uint_64 = OpConstant %uint 64
%ptr_wg_uint = OpTypePointer Workgroup %uint
         %wg = OpVariable %ptr_wg_uint Workgroup
       %main = OpFunction %void None %fn
      %entry = OpLabel
       %liid = OpLoad %v3uint %gl_LocalInvocationId
        %tid = OpCompositeExtract %uint %liid 0
               OpBranch %header
     %header = OpLabel
          %i = OpPhi %uint %tid %entry %inext %continue
               OpLoopMerge %merge %continue None
               OpBranch %body
       %body = OpLabel
        %val = OpLoad %uint %wg
               OpStore %wg %val
       %cond = OpULessThanEqual %bool %uint_64 %i
               OpSelectionMerge %exit None
               OpBranchConditional %cond %exit %continue
       %exit = OpLabel
               OpBranch %merge
   %continue = OpLabel
      %inext = OpIAdd %uint %i %uint_1
               OpBranch %header
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliLockstepPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

TEST_F(MaliPassTest, MaliLockstepPreservesOriginalComparisonUses) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_LocalInvocationId
               OpExecutionMode %main LocalSize 32 1 1
               OpDecorate %gl_LocalInvocationId BuiltIn LocalInvocationId
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%ptr_input_v3uint = OpTypePointer Input %v3uint
%gl_LocalInvocationId = OpVariable %ptr_input_v3uint Input
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
    %uint_32 = OpConstant %uint 32
    %uint_64 = OpConstant %uint 64
%ptr_wg_uint = OpTypePointer Workgroup %uint
         %wg = OpVariable %ptr_wg_uint Workgroup
       %main = OpFunction %void None %fn
      %entry = OpLabel
       %liid = OpLoad %v3uint %gl_LocalInvocationId
        %tid = OpCompositeExtract %uint %liid 0
               OpBranch %header
     %header = OpLabel
          %i = OpPhi %uint %tid %entry %inext %continue
               OpLoopMerge %merge %continue None
               OpBranch %body
       %body = OpLabel
        %val = OpLoad %uint %wg
               OpStore %wg %val
       %cond = OpULessThanEqual %bool %uint_64 %i
       %side = OpSelect %uint %cond %uint_1 %uint_2
               OpSelectionMerge %exit None
               OpBranchConditional %cond %exit %continue
       %exit = OpLabel
               OpBranch %merge
   %continue = OpLabel
      %inext = OpIAdd %uint %i %uint_32
               OpBranch %header
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliLockstepPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    // The original comparison stays for %side, and a new one feeds the branch.
    EXPECT_EQ(2u, CountOccurrences(disassembly, "OpULessThanEqual"));
    EXPECT_NE(std::string::npos, disassembly.find("OpSelect"));
}

TEST_F(MaliPassTest, MaliLockstepDetectsAtomicWorkgroupAccess) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_LocalInvocationId
               OpExecutionMode %main LocalSize 32 1 1
               OpDecorate %gl_LocalInvocationId BuiltIn LocalInvocationId
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%ptr_input_v3uint = OpTypePointer Input %v3uint
%gl_LocalInvocationId = OpVariable %ptr_input_v3uint Input
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
    %uint_32 = OpConstant %uint 32
    %uint_64 = OpConstant %uint 64
%ptr_wg_uint = OpTypePointer Workgroup %uint
         %wg = OpVariable %ptr_wg_uint Workgroup
       %main = OpFunction %void None %fn
      %entry = OpLabel
       %liid = OpLoad %v3uint %gl_LocalInvocationId
        %tid = OpCompositeExtract %uint %liid 0
               OpBranch %header
     %header = OpLabel
          %i = OpPhi %uint %tid %entry %inext %continue
               OpLoopMerge %merge %continue None
               OpBranch %body
       %body = OpLabel
     %atomic = OpAtomicIAdd %uint %wg %uint_2 %uint_0 %uint_1
       %cond = OpULessThanEqual %bool %uint_64 %i
               OpSelectionMerge %exit None
               OpBranchConditional %cond %exit %continue
       %exit = OpLabel
               OpBranch %merge
   %continue = OpLabel
      %inext = OpIAdd %uint %i %uint_32
               OpBranch %header
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliLockstepPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(2u, CountOccurrences(std::get<0>(result), "OpPhi"));
}

TEST_F(MaliPassTest, MaliLockstepLeavesSingleInvocationWorkgroup) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint GLCompute %main "main" %gl_LocalInvocationId
               OpExecutionMode %main LocalSize 1 1 1
               OpDecorate %gl_LocalInvocationId BuiltIn LocalInvocationId
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %bool = OpTypeBool
       %uint = OpTypeInt 32 0
     %v3uint = OpTypeVector %uint 3
%ptr_input_v3uint = OpTypePointer Input %v3uint
%gl_LocalInvocationId = OpVariable %ptr_input_v3uint Input
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
     %uint_2 = OpConstant %uint 2
%ptr_wg_uint = OpTypePointer Workgroup %uint
         %wg = OpVariable %ptr_wg_uint Workgroup
       %main = OpFunction %void None %fn
      %entry = OpLabel
       %liid = OpLoad %v3uint %gl_LocalInvocationId
        %tid = OpCompositeExtract %uint %liid 0
               OpBranch %header
     %header = OpLabel
          %i = OpPhi %uint %tid %entry %inext %continue
               OpLoopMerge %merge %continue None
               OpBranch %body
       %body = OpLabel
        %val = OpLoad %uint %wg
               OpStore %wg %val
       %cond = OpULessThanEqual %bool %uint_2 %i
               OpSelectionMerge %exit None
               OpBranchConditional %cond %exit %continue
       %exit = OpLabel
               OpBranch %merge
   %continue = OpLabel
      %inext = OpIAdd %uint %i %uint_1
               OpBranch %header
      %merge = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result =
        SinglePassRunAndDisassemble<MaliLockstepPass>(text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

// ---------------------------------------------------------------------------
// LowerClipCullDistancePass (CreateRemoveClipCullDistPass)
// ---------------------------------------------------------------------------

TEST_F(MaliPassTest, LowerClipCullDistanceRemovesClipDistance) {
    const std::string text = R"(
               OpCapability Shader
               OpCapability ClipDistance
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %clip
               OpExecutionMode %main OriginUpperLeft
               OpDecorate %clip BuiltIn ClipDistance
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
    %float_1 = OpConstant %float 1
%ptr_out_float = OpTypePointer Output %float
       %clip = OpVariable %ptr_out_float Output
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpStore %clip %float_1
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<LowerClipCullDistancePass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    const std::string &disassembly = std::get<0>(result);
    EXPECT_EQ(0u, CountOccurrences(disassembly, "ClipDistance"));
    EXPECT_EQ(0u, CountOccurrences(disassembly, "OpStore"));
}

TEST_F(MaliPassTest, LowerClipCullDistanceRemovesCullDistance) {
    const std::string text = R"(
               OpCapability Shader
               OpCapability CullDistance
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %cull
               OpExecutionMode %main OriginUpperLeft
               OpDecorate %cull BuiltIn CullDistance
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
      %float = OpTypeFloat 32
    %float_1 = OpConstant %float 1
%ptr_out_float = OpTypePointer Output %float
       %cull = OpVariable %ptr_out_float Output
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpStore %cull %float_1
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<LowerClipCullDistancePass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithChange, std::get<1>(result));
    EXPECT_EQ(0u, CountOccurrences(std::get<0>(result), "CullDistance"));
}

TEST_F(MaliPassTest, LowerClipCullDistanceLeavesShaderWithoutClipDistance) {
    const std::string text = R"(
               OpCapability Shader
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main"
               OpExecutionMode %main OriginUpperLeft
       %void = OpTypeVoid
         %fn = OpTypeFunction %void
       %main = OpFunction %void None %fn
      %label = OpLabel
               OpReturn
               OpFunctionEnd
)";

    auto result = SinglePassRunAndDisassemble<LowerClipCullDistancePass>(
        text, true, true);
    EXPECT_EQ(Pass::Status::SuccessWithoutChange, std::get<1>(result));
}

} // namespace
} // namespace opt
} // namespace spvtools
