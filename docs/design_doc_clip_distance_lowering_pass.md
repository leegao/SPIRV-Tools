# Design Document: Clip Distance Lowering Pass

## Motivation

The `ClipDistance` built-in in SPIR-V allows a shader to write a distance to a set of clipping planes. The graphics pipeline then discards fragments for pixels where the interpolated clip distance is negative. However, not all hardware or graphics APIs support `ClipDistance` natively. For example, older hardware or certain mobile GPUs may lack support for this feature.

This pass provides a solution by "lowering" the `ClipDistance` functionality. It transforms the SPIR-V module to emulate the clipping behavior without relying on the `ClipDistance` built-in. This allows shaders that use `ClipDistance` to run on a wider range of hardware and platforms.

## Functionality

The clip distance lowering pass is a SPIR-V to SPIR-V transformation that replaces the use of the `ClipDistance` built-in with explicit logic in the shader to perform clipping. The pass operates on shaders in the vertex, tessellation, and geometry stages.

The high-level functionality of the pass is as follows:

1.  Identify all `ClipDistance` and `CullDistance` output variables in the shader.
2.  For each `OpStore` to a `ClipDistance` or `CullDistance` variable, insert new instructions to check if the stored value is negative.
3.  If a negative value is detected, the pass modifies the `Position` output of the vertex to cause the primitive to be clipped. This is achieved by setting the `w` component of the `Position` vector to a negative value.
4.  After the emulation code is inserted, the pass removes the original `ClipDistance` and `CullDistance` variables, decorations, and capabilities from the module.

The pass is designed to be conservative. If it encounters a situation it cannot handle (e.g., unsupported shader stage or language feature), it will either fail with an error message or leave the shader unmodified.

## Mechanism

The core of the pass is the emulation of clipping using the `w` component of the `Position` output. In homogeneous coordinates, a vertex is considered to be outside the view frustum if its `w` component is negative after the perspective divide. By manipulating `Position.w`, we can effectively "clip" a vertex.

The pass will implement the following logic:

1.  **Find Built-in Variables:** The pass will first identify the `ClipDistance`, `CullDistance`, and `Position` output variables. This is done by scanning the module's entry points and their interfaces. The pass relies on the `OpDecorate` and `OpMemberDecorate` instructions with the `BuiltIn` decoration.

2.  **Locate Stores:** The pass will find all `OpStore` instructions that write to the `ClipDistance` or `CullDistance` variables. These are the points where the clip distances are assigned.

3.  **Inject Clipping Code:** For each `OpStore`, the pass will inject a block of code that performs the following steps:
    *   Load the value being stored to the `ClipDistance` variable.
    *   Check if the value is negative. This check will be performed for each component if the `ClipDistance` is a vector.
    *   If any component is negative, the code will load the pointer to the `Position` output for the current vertex.
    *   It will then construct a new `Position` vector where the `w` component is set to a negative value (e.g., -1.0).
    *   Finally, it will store the modified `Position` vector back to the `Position` output.

4.  **Cleanup:** After injecting the emulation code, the pass will perform a cleanup step to remove all traces of `ClipDistance` and `CullDistance` from the module. This includes:
    *   Removing the `OpVariable` instructions for `ClipDistance` and `CullDistance`.
    *   Removing the `OpDecorate` and `OpMemberDecorate` instructions that refer to `ClipDistance` and `CullDistance`.
    *   Removing the `ClipDistance` and `CullDistance` capabilities from the module.

## Limitations

The clip distance lowering pass has the following limitations:

*   **Unsupported Shader Stages:** The pass will only support Vertex, Tessellation Evaluation, and Geometry shaders. Other shader stages (e.g., Fragment) do not have a `Position` output and cannot be supported.
*   **VariablePointers:** The pass will not support the `VariablePointers` or `VariablePointersStorageBuffer` capabilities. These capabilities make it difficult to statically determine which variable is being accessed.
*   **Scalarization:** The pass will require that aggregate types for `ClipDistance` and `CullDistance` are scalarized first. This means that if `ClipDistance` is an array, it should be broken down into individual floating-point values. The `scalar-replacement` pass can be used for this purpose.
*   **Geometry Shaders:** While the pass will support geometry shaders in principle, the implementation will need to be careful to handle `OpEmitVertex` and `OpEndPrimitive` correctly. The `Position` output must be modified before the vertex is emitted.

## Example

Consider the following simple vertex shader that uses `ClipDistance`:

```spirv
               OpCapability Shader
               OpCapability ClipDistance
               ...
               OpDecorate %clip_dist BuiltIn ClipDistance
               OpDecorate %pos BuiltIn Position
       %void = OpTypeVoid
   %func_ptr = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_Output_v4float = OpTypePointer Output %v4float
       %pos = OpVariable %_ptr_Output_v4float Output
%_ptr_Output_float = OpTypePointer Output %float
  %clip_dist = OpVariable %_ptr_Output_float Output
       ...
       %main = OpFunction %void None %func_ptr
      %label = OpLabel
               ...
               OpStore %clip_dist %some_float_value
               OpStore %pos %some_v4float_value
               OpReturn
               OpFunctionEnd
```

After the clip distance lowering pass, the shader will be transformed to:

```spirv
               OpCapability Shader
               ...
               OpDecorate %pos BuiltIn Position
       %void = OpTypeVoid
   %func_ptr = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_Output_v4float = OpTypePointer Output %v4float
       %pos = OpVariable %_ptr_Output_v4float Output
       ...
       %main = OpFunction %void None %func_ptr
      %label = OpLabel
               ...
  %is_neg = OpFOrdLessThan %some_float_value %float_0
            OpSelectionMerge %merge_label None
            OpBranchConditional %is_neg %clip_label %merge_label
%clip_label = OpLabel
    %pos_w_ptr = OpAccessChain %_ptr_Output_float %pos %uint_3
                 OpStore %pos_w_ptr %float_n1
                 OpBranch %merge_label
%merge_label = OpLabel
               OpStore %pos %some_v4float_value
               OpReturn
               OpFunctionEnd
```

In this example, the `OpStore` to `%clip_dist` is replaced by a conditional branch that checks if `%some_float_value` is negative. If it is, the `w` component of `%pos` is set to -1.0. The original `%clip_dist` variable and its decoration are removed.
