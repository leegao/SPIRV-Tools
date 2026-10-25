# SPIR-V Tools
[![OpenSSF Scorecard](https://api.securityscorecards.dev/projects/github.com/KhronosGroup/SPIRV-Tools/badge)](https://securityscorecards.dev/viewer/?uri=github.com/KhronosGroup/SPIRV-Tools)

Note: MaliBitwiseScalarPass, MaliClampZeroPass, MaliPerVertexBlockPass, and MaliLockstepPass are adapted from https://github.com/ASEbrahim/MaliBridge

| Pass                               | Optimizer flag                       | C++ factory                                |
| ---------------------------------- | ------------------------------------ | ------------------------------------------ |
| `FixMaliSpecConstantCompositePass` | `--fix-mali-spec-constant-composite` | `CreateFixMaliSpecConstantCompositePass()` |
| `MaliOptimizationBarrierPass`      | `--mali-optimization-barrier`        | `CreateMaliOptimizationBarrierPass()`      |
| `MaliBitwiseScalarPass`            | `--mali-bitwise-scalar`              | `CreateMaliBitwiseScalarPass(bool)`        |
| `MaliClampZeroPass`                | `--mali-clamp-zero`                  | `CreateMaliClampZeroPass(bool, bool)`      |
| `MaliPerVertexBlockPass`           | `--mali-pervertex-block`             | `CreateMaliPerVertexBlockPass()`           |
| `MaliLockstepPass`                 | `--mali-lockstep`                    | `CreateMaliLockstepPass()`                 |
| `LowerClipCullDistancePass`        | `--remove-clip-cull-dist`            | `CreateRemoveClipCullDistPass()`           |


Should be run them in this order:
spec-constant fix, per-vertex block, shift barrier, bitwise scalar,
clamp zero, lockstep, clip/cull distance.

The per-vertex pass goes first
because it changes the interface, and the spec-constant fix goes first because
the other passes inspect constant instructions.

---

## FixMaliSpecConstantCompositePass

### Problem

Some Mali drivers mis-compile a boolean vector that is declared as an
`OpConstantComposite` whose constituents are specialization constants (for
example the `gl_SampleMask`-style `%v4bool` built by glslang). The vector is
treated as a plain constant instead of a specialization-dependent value.

### Transformation

For every `OpConstantComposite` instruction:

1. The result type must be `OpTypeVector` of `OpTypeBool`.
2. At least one constituent must be a specialization constant
   (`OpSpecConstant`, `OpSpecConstantTrue`, `OpSpecConstantFalse`,
   `OpSpecConstantComposite`, or `OpSpecConstantOp`).
3. The instruction's opcode is changed to `OpSpecConstantComposite`.

The operands and result id are unchanged, so every use remains valid and the
value becomes specialization-dependent exactly as intended.

### Preconditions and limits

- Requires the `Shader` capability.
- Only boolean vectors are handled. Arrays and structs whose constituents are
  specialization constants are left alone; turning those into specialization
  constants is not required for the Mali bug and would change more of the
  module.
- Non-boolean vectors (for example integer vectors) are left alone.

### Design notes

The pass uses `Module::GetConstants()` and a read-only def-use lookup. It does
not need to rebuild def-use information because it only changes opcodes, not
operands or definitions.

---

## MaliOptimizationBarrierPass

### Problem

The Mali compiler may constant-fold a left shift whose shift amount is a
compile-time constant into an incorrect value. The generated code must keep the
shift opaque to the driver's constant folder.

### Transformation

For every `OpShiftLeftLogical` in a function body:

1. The shift amount must resolve to a declared constant
   (`analysis::ConstantManager::FindDeclaredConstant`). A scalar integer or a
   vector of integers is accepted.
2. The original shift is cloned and the clone is given a fresh result id.
   The clone is inserted immediately before the original instruction.
3. The original instruction is rewritten in place as

    ```text
    %original = OpBitFieldInsert %type %clone %zero %zero32 %zero32
    ```

    where `%zero` is a zero constant of the shift's _result_ type (a zero vector
    for vector results) and `%zero32` is an unsigned 32-bit zero used for the
    offset and count. Inserting zero bits at offset zero is a semantic no-op,
    but the instruction is not a foldable shift. The shape of `%zero` is taken
    from the result type, not from the shift amount.

Keeping the original result id on the barrier means every existing use stays
valid without a rename, and the cloned shift remains available to the barrier.

### Preconditions and limits

- Only `OpShiftLeftLogical` is handled. Other shift directions are not part of
  the observed Mali bug.
- The shift amount must be a declared constant; a value loaded at run time is
  left alone.
- The zero constant is created with the exact result type and width, including
  16/32/64-bit integers and vectors. `ConstantManager::GetUIntConstId` and
  `GetSIntConstId` are avoided because they always produce 32-bit constants.
- `OpBitFieldInsert` requires `Insert` to have the same type as the result and
  `Offset`/`Count` to be integer scalars; the pass builds exactly that shape.

### Design notes

The pass updates def-use information with `IRContext::UpdateDefUse` after both
the clone and the rewrite. The clone is inserted before the original
instruction, which keeps the module valid at every intermediate step (the
original result id always has a definition).

---

## MaliBitwiseScalarPass

### Problem

MK-03, Arm erratum 3843237 (r18p0-r53p0): Mali mis-compiles an integer vector
bitwise operation (including shifts) when an operand is a lane swizzle, a
vector assembled lane by lane, or a constant composite. The vector operation
is scalarised so the compiler never sees the problematic vector form.

### Transformation

The pass targets these vector operations:

- `OpShiftLeftLogical`, `OpShiftRightLogical`, `OpShiftRightArithmetic`
- `OpBitwiseOr`, `OpBitwiseXor`, `OpBitwiseAnd`
- `OpNot`

An instruction is a candidate when:

- its result type is an integer vector with 1..16 lanes, and
- `rewrite_all` is set, **or** at least one source operand is a _pattern
  operand_. A pattern operand is an `OpVectorShuffle`,
  `OpCompositeConstruct`, `OpCompositeInsert`, `OpConstantComposite`,
  `OpSpecConstantComposite`, or an `OpSpecConstantOp` producing a shuffle or
  insert. `OpCopyObject` and `OpBitcast` are looked through (up to eight
  levels).

A candidate is rewritten as:

```text
%lane0 = OpCompositeExtract %component %src0 0
%lane1 = OpCompositeExtract %component %src1 0
%r0    = OpBitwiseOr %component %lane0 %lane1
...    (one scalar op per lane)
%result = OpCompositeConstruct %vector %r0 %r1 ...
```

For `OpNot` only one source is extracted per lane.

### Preconditions and limits

- Every source must be an integer vector with the same lane count and component
  width as the result. A mismatch leaves the instruction untouched.
- Scalar operations are never rewritten, even with `rewrite_all`.
- At most 16 lanes are considered.
- The pass does not try to fold or simplify the scalar results; it only changes
  the representation.

### Design notes

All source types are validated before any instruction is created, so a failed
rewrite does not leave partially built instructions behind. The final rewrite
uses `IRContext::UpdateDefUse` (not `AnalyzeInstDefUse`) so the uses of the old
vector operands are removed from the def-use manager.

---

## MaliClampZeroPass

### Problem

MK-32, Arm erratum 3779215 (r38p1-r49p0 and r50p0-r51p0; fixed in r49p1 and
r52p0): Mali mis-compiles a min/max/clamp whose limit is a constant zero when
the clamped value is selected at run time from compile-time constants. The
clamp can degrade into a plain min or max.

### Transformation

The pass inspects `OpExtInst` instructions from `GLSL.std.450`:

- Clamp: `FClamp`, `UClamp`, `SClamp`, `NClamp`
- Min/max: `FMin`, `UMin`, `SMin`, `FMax`, `UMax`, `SMax`, `NMin`, `NMax`

For a clamp, at least one limit must be a constant zero. For a min/max, exactly
one operand must be a constant zero (otherwise the instruction is not
rewritable or has no zero limit). Unless `rewrite_all` is set, the non-zero
value must also be _constant-selected_: it must be provably built from
compile-time constants.

The constant-zero operand is replaced with an **opaque zero**:

- Default mode: a `Private` variable is created per scalar type. A zero is
  stored to it at the top of every entry point, and a load is inserted at the
  top of each function that needs it. Vector zeros are built with
  `OpCompositeConstruct` from the opaque scalar zero. The load defeats
  constant propagation while remaining zero by construction (and Private
  variables are zero-initialized anyway).
- `bitfield_insert` mode (32-bit types): the zero is synthesized with
  `OpBitFieldInsert` instead of a memory round trip. Integer zeros use the zero
  constant as both `Base` and `Insert` with offset/count zero; float zeros are
  bitcast to `uint`, passed through `OpBitFieldInsert`, and bitcast back.

`rewrite_all` skips the constant-selected proof and rewrites every matching
zero limit.

### Constant-selected analysis

`IsConstantSelected` follows values through:

- `OpSelect` (both value operands)
- `OpPhi` (all incoming values)
- `OpLoad` (through `OpAccessChain`, `OpInBoundsAccessChain`,
  `OpPtrAccessChain`, `OpInBoundsPtrAccessChain`, and `OpCopyObject` to the
  root variable; all stores to that variable must themselves be
  constant-selected)
- `OpCompositeExtract`, `OpVectorShuffle`, `OpCompositeInsert`
- `OpExtInst` and ordinary arithmetic/logical value instructions

Root variables are limited to `Function`, `Private`, and `Output` storage.
A variable written by `OpCopyMemory`/`OpCopyMemorySized` is rejected
conservatively, because the copy can write through a pointer the analysis did
not trace.

### Preconditions and limits

- Only `GLSL.std.450` is recognized.
- A "constant zero" is `OpConstant` with all literal words zero,
  `OpConstantNull`, or a composite of constant zeros. `-0.0` is not treated as
  zero (it is not bitwise zero).
- 64-bit float/integer `bitfield_insert` zeros fall back to the Private-variable
  path.
- The pass needs at least one entry point to seed the Private variable; without
  one the variable is still zero-initialized, so the load remains semantically
  correct.
- The pass clears its caches at the start of `Process()`, so one pass object can
  be run again on a different module.

### Design notes

The caches are keyed by `(function result id, type id)` because each function
needs its own load. A single `Private` variable and store are shared across the
module for a given scalar type. The pass updates def-use information with
`IRContext::UpdateDefUse` when it replaces an operand.

---

## MaliPerVertexBlockPass

### Problem

Mali r44p1 (fixed in r47p0) crashes inside its compiler on
geometry/tessellation pipelines whose per-vertex built-ins are loose
variables instead of members of a `gl_PerVertex` block. glslang always emits
the block; Zink emits loose variables.

### Transformation

The pass runs only when the module has exactly one entry point and its execution
model is `Geometry`, `TessellationControl`, or `TessellationEvaluation`.

For the `Input` and `Output` storage classes separately:

1. Collect loose variables decorated `BuiltIn` with one of `Position`,
   `PointSize`, `ClipDistance`, or `CullDistance`.
2. Validate that every direct use is rewritable:
    - `OpAccessChain` / `OpInBoundsAccessChain` with the variable as the base
      (and, for arrayed interfaces, at least one index into the outer array);
    - `OpLoad` / `OpStore` of the whole variable for non-arrayed interfaces;
    - `OpEntryPoint`, `OpDecorate`, or `OpName`.
      Any other use (for example `OpCopyObject`, `OpPtrAccessChain`, or
      `OpCopyMemory`) makes the pass leave the module unchanged.
3. Build one `OpTypeStruct` whose members are the built-in types, add
   `OpMemberDecorate ... BuiltIn <builtin>` (and `Invariant` when present), and
   decorate the struct type with `Block`.
4. For arrayed interfaces (inputs, and tessellation-control outputs), build an
   `OpTypeArray` of the new struct using the original outer array length. All
   members must share that length.
5. Add a new `OpVariable` for the block, redirect access chains to the block
   base with the member index inserted (after the array index when arrayed),
   and redirect whole loads/stores through a new member access chain.
6. Replace the old variables in the entry point interface with the block
   variable, remove their names/decorations, and delete the variables.

### Preconditions and limits

- At most eight loose built-ins per interface.
- Unexpected decorations on a folded built-in (anything other than `BuiltIn`
  and `Invariant`) make the pass bail out. Ordinary interface variables
  decorated with `Location` and similar are ignored, not treated as errors.
- The pass does not rewrite `OpPtrAccessChain`, pointer copies, or whole-array
  loads/stores. Those cases are detected before any module change.
- Member offsets are not emitted. Interface blocks do not require them for the
  Input/Output storage classes used here, and omitting them avoids imposing
  std140/std430 layout rules on per-vertex built-ins.
- The pass is idempotent: after a successful run there are no loose per-vertex
  built-ins left to collect.

### Design notes

All validation (decorations, use set, array lengths, pointer/array types) is
done before types and variables are created. Internal type-manager failures
return `Status::Failure` rather than leaving a partially modified module.
`IRContext::KillNamesAndDecorates` removes `OpName` and `OpDecorate` before
`KillInst`, which keeps debug names from dangling.

---

## MaliLockstepPass

### Problem

Compute shaders written for a 32-lane wave often use a strided loop

```text
for (i = gl_LocalInvocationID.x; i <= n; i += W) { ... Workgroup access ... }
```

where `W` is the workgroup X size. Different invocations can take different
numbers of iterations, so a workgroup barrier cannot be placed inside the loop.
Mali needs every invocation in the workgroup to execute the same trip count.

### Transformation

The pass requires:

- exactly one `GLCompute` entry point;
- an `OpExecutionMode ... LocalSize` with X size at least 2;
- a `LocalInvocationId` built-in variable, and its `.x` obtained as lane 0 of a
  load (a conversion of that value is also accepted for widened inductions);
- a loop header with an `OpPhi` whose pre-loop incoming value is that
  `LocalInvocationId.x` (or a conversion of it) and whose back-edge incoming is
  `OpIAdd(phi, W)` where `W` is the constant LocalSize X;
- a single exit block reached by a conditional branch on
  `OpULessThanEqual(n, phi)` or `OpUGreaterThanEqual(phi, n)`, with `n` defined
  in a block that dominates the header;
- at least one Workgroup memory access in the loop (load/store, atomics, or
  copy memory, including accesses through pointers derived by access chains).

It then adds a second induction variable in the header:

```text
%b = OpPhi %type %zero %preheader %new_add %back_edge
%new_add = OpIAdd %type %b %W
```

and builds a fresh comparison

```text
%cond2 = OpULessThanEqual %bool %n %b        (or the mirrored >= form)
```

which is used by the exit branch. The original comparison is left intact for
any other users. The original per-thread induction variable still drives the
loop body; only the trip-count test becomes workgroup-uniform.

### Preconditions and limits

- Structured control flow is assumed. Loop membership is computed with the
  dominator analysis: a block is in the loop when the header dominates it and
  the merge block does not.
- One exit block and one conditional exit are required. `OpSwitch` exits,
  multiple exits, and direct branches to the merge block cause the pass to
  skip the loop.
- Only unsigned comparisons are recognized.
- The stride must be a non-specialization `OpConstant` equal to LocalSize X.
  A 64-bit induction is supported, including its `OpUConvert` initial value and
  two-word constants.
- `LocalSizeId` is not handled; only the literal `LocalSize` execution mode is.
- The transformation intentionally makes some invocations execute extra
  iterations. It is only safe for loop bodies that are already guarded so that
  extra iterations do not write out of range; this is the same assumption the
  workaround has always made.

### Design notes

The new phi uses a zero constant created with the exact induction type and
width (not `GetUIntConstId`, which is always 32-bit). The increment is inserted
next to the original `OpIAdd` and then re-pointed at the new phi; this keeps the
def-use manager consistent at every step. The fresh comparison is inserted
before any `OpSelectionMerge`/`OpLoopMerge` (and before intervening debug line
instructions) so the structured-control-flow rules keep holding.

---

## LowerClipCullDistancePass

### Problem

Some Mali drivers do not implement the `ClipDistance`/`CullDistance`
built-ins. The pass emulates them with explicit distance checks against the
`Position` built-in.

### Transformation

For each entry point, the pass finds the clip/cull distance variables and the
position variable, finds stores to the distance outputs, and injects code that
clips (or culls) the primitive when a distance becomes negative. The distance
variables and their decorations are then removed, along with now-dead stores.

### Preconditions and limits

- Requires the `Shader` capability.
- Does not support the `VariablePointers` or
  `VariablePointersStorageBuffer` capabilities; it returns `Status::Failure`
  for those modules.
- Missing `Position` is handled by the pass's existing diagnostics and cleanup
  path.
- Supported execution models are the graphics stages listed in
  `remove_clip_cull_dist_pass.cpp`; other stages are left alone.

### Design notes

This is the oldest Mali pass in the tree and is documented here for
completeness. Unlike the other passes it can add blocks and branches, so it is
run last by the compatibility layer.

---

NEWS 2023-01-11: Development occurs on the `main` branch.

## Overview

The SPIR-V Tools project provides an API and commands for processing SPIR-V
modules.

The project includes an assembler, binary module parser, disassembler,
validator, and optimizer for SPIR-V. Except for the optimizer, all are based
on a common static library.  The library contains all of the implementation
details, and is used in the standalone tools whilst also enabling integration
into other code bases directly. The optimizer implementation resides in its
own library, which depends on the core library.

The interfaces have stabilized:
We don't anticipate making a breaking change for existing features.

SPIR-V is defined by the Khronos Group Inc.
See the [SPIR-V Registry][spirv-registry] for the SPIR-V specification,
headers, and XML registry.

## Downloads

The official releases for SPIRV-Tools can be found on LunarG's
[SDK download page](https://vulkan.lunarg.com/sdk/home).

For convenience, here are also links to the latest builds (HEAD).
Those are untested automated builds. Those are not official releases, nor
are guaranteed to work. Official releases builds are in the Vulkan SDK.

<img alt="Linux" src="kokoro/img/linux.png" width="20px" height="20px" hspace="2px"/>[![Linux Build Status](https://storage.googleapis.com/spirv-tools/badges/build_status_linux_clang_release.svg)](https://storage.googleapis.com/spirv-tools/badges/build_link_linux_clang_release.html)
<img alt="MacOS" src="kokoro/img/macos.png" width="20px" height="20px" hspace="2px"/>[![MacOS Build Status](https://storage.googleapis.com/spirv-tools/badges/build_status_macos_clang_release.svg)](https://storage.googleapis.com/spirv-tools/badges/build_link_macos_clang_release.html)
<img alt="Windows" src="kokoro/img/windows.png" width="20px" height="20px" hspace="2px"/>[![Windows Build Status](https://storage.googleapis.com/spirv-tools/badges/build_status_windows_vs2022_release.svg)](https://storage.googleapis.com/spirv-tools/badges/build_link_windows_vs2022_release.html)

[More downloads](docs/downloads.md)

## Versioning SPIRV-Tools

See [`CHANGES`](CHANGES) for a high level summary of recent changes, by version.

SPIRV-Tools project version numbers are of the form `v`*year*`.`*index* and with
an optional `-dev` suffix to indicate work in progress.  For example, the
following versions are ordered from oldest to newest:

* `v2016.0`
* `v2016.1-dev`
* `v2016.1`
* `v2016.2-dev`
* `v2016.2`

Use the `--version` option on each command line tool to see the software
version.  An API call reports the software version as a C-style string.

## Releases

The official releases for SPIRV-Tools can be found on LunarG's
[SDK download page](https://vulkan.lunarg.com/sdk/home).

You can find either the prebuilt, and QA tested binaries, or download the
SDK Config, which lists the commits to use to build the release from scratch.

GitHub releases are deprecated, and we will not publish new releases until
further notice.

## Supported features

### Assembler, binary parser, and disassembler

* Support for SPIR-V 1.0, through 1.5
  * Based on SPIR-V syntax described by JSON grammar files in the
    [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers) repository.
  * Usually, support for a new version of SPIR-V is ready within days after
    publication.
* Support for extended instruction sets:
  * GLSL std450 version 1.0 Rev 3
  * OpenCL version 1.0 Rev 2
* Assembler only does basic syntax checking.  No cross validation of
  IDs or types is performed, except to check literal arguments to
  `OpConstant`, `OpSpecConstant`, and `OpSwitch`.
* Where tools expect binary input, a hex stream may be provided instead.  See
  `spirv-dis --help`.

See [`docs/syntax.md`](docs/syntax.md) for the assembly language syntax.

### Validator

The validator checks validation rules described by the SPIR-V specification.

Khronos recommends that tools that create or transform SPIR-V modules use the
validator to ensure their outputs are valid, and that tools that consume SPIR-V
modules optionally use the validator to protect themselves from bad inputs.
This is especially encouraged for debug and development scenarios.

The validator has one-sided error: it will only return an error when it has
implemented a rule check and the module violates that rule.

The validator is incomplete.
See the [CHANGES](CHANGES) file for reports on completed work, and
the [Validator
sub-project](https://github.com/KhronosGroup/SPIRV-Tools/projects/1) for planned
and in-progress work.

*Note*: The validator checks some Universal Limits, from section 2.17 of the SPIR-V spec.
The validator will fail on a module that exceeds those minimum upper bound limits.
The validator has been parameterized to allow larger values, for use when targeting
a more-than-minimally-capable SPIR-V consumer.

See [`tools/val/val.cpp`](tools/val/val.cpp) or run `spirv-val --help` for the command-line help.

### Optimizer

The optimizer is a collection of code transforms, or "passes".
Transforms are written for a diverse set of reasons:

* To restructure, simplify, or normalize the code for further processing.
* To eliminate undesirable code.
* To improve code quality in some metric such as size or performance.
  **Note**: These transforms are not guaranteed to actually improve any
  given metric. Users should always measure results for their own situation.

As of this writing, there are 67 transforms including examples such as:
* Simplification
  * Strip debug info
  * Strip reflection info
* Specialization Constants
  * Set spec constant default value
  * Freeze spec constant to default value
  * Fold `OpSpecConstantOp` and `OpSpecConstantComposite`
  * Unify constants
  * Eliminate dead constant
* Code Reduction
  * Inline all function calls exhaustively
  * Convert local access chains to inserts/extracts
  * Eliminate local load/store in single block
  * Eliminate local load/store with single store
  * Eliminate local load/store with multiple stores
  * Eliminate local extract from insert
  * Eliminate dead instructions (aggressive)
  * Eliminate dead branches
  * Merge single successor / single predecessor block pairs
  * Eliminate common uniform loads
  * Remove duplicates: Capabilities, extended instruction imports, types, and
    decorations.
* Normalization
  * Compact IDs
  * Canonicalize IDs
  * CFG cleanup
  * Flatten decorations
  * Merge returns
  * Convert AMD-specific instructions to KHR instructions
* Code improvement
  * Conditional constant propagation
  * If-conversion
  * Loop fission
  * Loop fusion
  * Loop-invariant code motion
  * Loop unroll
* Other
  * Graphics robust access
  * Upgrade memory model to VulkanKHR

Additionally, certain sets of transformations have been packaged into
higher-level recipes.  These include:

* Optimization for size (`spirv-opt -Os`)
* Optimization for performance (`spirv-opt -O`)

For the latest list with detailed documentation, please refer to
[`include/spirv-tools/optimizer.hpp`](include/spirv-tools/optimizer.hpp).

For suggestions on using the code reduction options, please refer to this [white paper](https://www.lunarg.com/shader-compiler-technologies/white-paper-spirv-opt/).


### Linker

*Note:* The linker is still under development.

Current features:
* Combine multiple SPIR-V binary modules together.
* Combine into a library (exports are retained) or an executable (no symbols
  are exported).

See the [CHANGES](CHANGES) file for reports on completed work, and the [General
sub-project](https://github.com/KhronosGroup/SPIRV-Tools/projects/2) for
planned and in-progress work.


### Reducer

*Note:* The reducer is still under development.

The reducer simplifies and shrinks a SPIR-V module with respect to a
user-supplied *interestingness function*.  For example, given a large
SPIR-V module that cause some SPIR-V compiler to fail with a given
fatal error message, the reducer could be used to look for a smaller
version of the module that causes the compiler to fail with the same
fatal error message.

To suggest an additional capability for the reducer, [file an
issue](https://github.com/KhronosGroup/SPIRV-Tools/issues]) with
"Reducer:" as the start of its title.


### Fuzzer

*Note:* The fuzzer is still under development.

The fuzzer applies semantics-preserving transformations to a SPIR-V binary
module, to produce an equivalent module.  The original and transformed modules
should produce essentially identical results when executed on identical inputs:
their results should differ only due to floating-point round-off, if at all.
Significant differences in results can pinpoint bugs in tools that process
SPIR-V binaries, such as miscompilations.  This *metamorphic testing* approach
is similar to the method used by the [GraphicsFuzz
project](https://github.com/google/graphicsfuzz) for fuzzing of GLSL shaders.

To suggest an additional capability for the fuzzer, [file an
issue](https://github.com/KhronosGroup/SPIRV-Tools/issues]) with
"Fuzzer:" as the start of its title.


### Diff

*Note:* The diff tool is still under development.

The diff tool takes two SPIR-V files, either in binary or text format and
produces a diff-style comparison between the two.  The instructions between the
src and dst modules are matched as best as the tool can, and output is produced
(in src id-space) that shows which instructions are removed in src, added in dst
or modified between them.  The order of instructions are not retained.

Matching instructions between two SPIR-V modules is not trivial, and thus a
number of heuristics are applied in this tool.  In particular, without debug
information, match functions is nontrivial as they can be reordered.  As such,
this tool is primarily useful to produce the diff of two SPIR-V modules derived
from the same source, for example before and after a modification to the shader,
before and after a transformation, or SPIR-V produced from different tools.


### Extras

* [Utility filters](#utility-filters)
* Build target `spirv-tools-vimsyntax` generates file `spvasm.vim`.
  Copy that file into your `$HOME/.vim/syntax` directory to get SPIR-V assembly syntax
  highlighting in Vim.  This build target is not built by default.

## Contributing

The SPIR-V Tools project is maintained by members of the The Khronos Group Inc.,
and is hosted at https://github.com/KhronosGroup/SPIRV-Tools.

Consider joining the `public_spirv_tools_dev@khronos.org` mailing list, via
[https://www.khronos.org/spir/spirv-tools-mailing-list/](https://www.khronos.org/spir/spirv-tools-mailing-list/).
The mailing list is used to discuss development plans for the SPIRV-Tools as an open source project.
Once discussion is resolved,
specific work is tracked via issues and sometimes in one of the
[projects][spirv-tools-projects].

(To provide feedback on the SPIR-V _specification_, file an issue on the
[SPIRV-Headers][spirv-headers] GitHub repository.)

See [`docs/projects.md`](docs/projects.md) to see how we use the
[GitHub Project
feature](https://help.github.com/articles/tracking-the-progress-of-your-work-with-projects/)
to organize planned and in-progress work.

Contributions via merge request are welcome. Changes should:
* Be provided under the [Apache 2.0](#license).
* You'll be prompted with a one-time "click-through"
  [Khronos Open Source Contributor License Agreement][spirv-tools-cla]
  (CLA) dialog as part of submitting your pull request or
  other contribution to GitHub.
* Include tests to cover updated functionality.
* C++ code should follow the [Google C++ Style Guide][cpp-style-guide].
* Code should be formatted with `clang-format`.
  [kokoro/check-format/build.sh](kokoro/check-format/build.sh)
  shows how to download it. Note that we currently use
  `clang-format version 5.0.0` for SPIRV-Tools. Settings are defined by
  the included [.clang-format](.clang-format) file.

We intend to maintain a linear history on the GitHub `main` branch.

### Getting the source

Example of getting sources, assuming SPIRV-Tools is configured as a standalone project:

    git clone https://github.com/KhronosGroup/SPIRV-Tools.git   spirv-tools
    cd spirv-tools

    # Check out sources for dependencies, at versions known to work together,
    # as listed in the DEPS file.
    python3 utils/git-sync-deps

For some kinds of development, you may need the latest sources from the third-party projects:

    git clone https://github.com/KhronosGroup/SPIRV-Headers.git spirv-tools/external/spirv-headers
    git clone https://github.com/google/googletest.git          spirv-tools/external/googletest
    git clone https://github.com/google/effcee.git              spirv-tools/external/effcee
    git clone https://github.com/google/re2.git                 spirv-tools/external/re2
    git clone https://github.com/abseil/abseil-cpp.git          spirv-tools/external/abseil_cpp
    git clone https://github.com/microsoft/mimalloc.git         spirv-tools/external/mimalloc

#### Dependency on Effcee

Some tests depend on the [Effcee][effcee] library for stateful matching.
Effcee itself depends on [RE2][re2], and RE2 depends on [Abseil][abseil-cpp].

* If SPIRV-Tools is configured as part of a larger project that already uses
  Effcee, then that project should include Effcee before SPIRV-Tools.
* Otherwise, SPIRV-Tools expects Effcee sources to appear in `external/effcee`,
  RE2 sources to appear in `external/re2`, and Abseil sources to appear in
  `external/abseil_cpp`.

#### Dependency on mimalloc

SPIRV-Tools may be configured to use the [mimalloc][mimalloc] library to improve memory
allocation performance. In order to avoid unexpectedly changing allocation behavior of
applications that link SPIRV-Tools libraries statically, this option has no effect on
the static libraries.

In the CMake build, usage of mimalloc is controlled by the `SPIRV_TOOLS_USE_MIMALLOC`
option. This variable defaults on `ON` when building for Windows and `OFF` when building
for other platforms. Enabling this option on non-Windows platforms is supported and is
expected to work normally, but this has not been tested as thoroughly and extensively as
the Windows version. In the future, the `SPIRV_TOOLS_USE_MIMALLOC` option may default to
`ON` for non-Windows platforms as well.

*Note*: mimalloc is currently only supported when building with CMake. When using Bazel,
mimalloc is not used.

### Source code organization

* `example`: demo code of using SPIRV-Tools APIs
* `external/googletest`: Intended location for the
  [googletest][googletest] sources, not provided
* `external/effcee`: Location of [Effcee][effcee] sources, if the `effcee` library
  is not already configured by an enclosing project.
* `external/re2`: Location of [RE2][re2] sources, if the `re2` library is not already
  configured by an enclosing project.
  (The Effcee project already requires RE2.)
* `external/abseil_cpp`: Location of [Abseil][abseil-cpp] sources, if Abseil is
   not already configured by an enclosing project.
  (The RE2 project already requires Abseil.)
* `external/mimalloc`: Intended location for [mimalloc][mimalloc] sources, not provided
* `include/`: API clients should add this directory to the include search path
* `external/spirv-headers`: Intended location for
  [SPIR-V headers][spirv-headers], not provided
* `include/spirv-tools/libspirv.h`: C API public interface
* `source/`: API implementation
* `test/`: Tests, using the [googletest][googletest] framework
* `tools/`: Command line executables

### Tests

The project contains a number of tests, used to drive development
and ensure correctness.  The tests are written using the
[googletest][googletest] framework.  The `googletest`
source is not provided with this project.  There are two ways to enable
tests:
* If SPIR-V Tools is configured as part of an enclosing project, then the
  enclosing project should configure `googletest` before configuring SPIR-V Tools.
* If SPIR-V Tools is configured as a standalone project, then download the
  `googletest` source into the `<spirv-dir>/external/googletest` directory before
  configuring and building the project.

## Build

*Note*: Prebuilt binaries are available from the [downloads](docs/downloads.md) page.

First [get the sources](#getting-the-source).
Then build using CMake, Bazel, Android ndk-build, or the Emscripten SDK.

### Build using CMake
You can build the project using [CMake][cmake]:

```sh
cd <spirv-dir>
mkdir build && cd build
cmake [-G <platform-generator>] <spirv-dir>
```

Once the build files have been generated, build using the appropriate build
command (e.g. `ninja`, `make`, `msbuild`, etc.; this depends on the platform
generator used above), or use your IDE, or use CMake to run the appropriate build
command for you:

```sh
cmake --build . [--config Debug]  # runs `make` or `ninja` or `msbuild` etc.
```

#### Note about the fuzzer

The SPIR-V fuzzer, `spirv-fuzz`, can only be built via CMake, and is disabled by
default. To build it, clone protobuf and use the `SPIRV_BUILD_FUZZER` CMake
option, like so:

```sh
# In <spirv-dir> (the SPIRV-Tools repo root):
git clone --depth=1 --branch v3.13.0.1 https://github.com/protocolbuffers/protobuf external/protobuf

# In your build directory:
cmake [-G <platform-generator>] <spirv-dir> -DSPIRV_BUILD_FUZZER=ON
cmake --build . --config Debug
```

You can also add `-DSPIRV_ENABLE_LONG_FUZZER_TESTS=ON` to build additional
fuzzer tests.


### Build using Bazel
You can also use [Bazel](https://bazel.build/) to build the project.

```sh
bazel build :all
```

### Build a node.js package using Emscripten

The SPIRV-Tools core library can be built to a WebAssembly [node.js](https://nodejs.org)
module. The resulting `SpirvTools` WebAssembly module only exports methods to
assemble and disassemble SPIR-V modules.

First, make sure you have the [Emscripten SDK](https://emscripten.org).
Then:

```sh
cd <spirv-dir>
./source/wasm/build.sh
```

The resulting node package, with JavaScript and TypeScript bindings, is
written to `<spirv-dir>/out/web`.

Note: This builds the package locally. It does *not* publish it to [npm](https://npmjs.org).

To test the result:

```sh
node ./test/wasm/test.js
```

### Tools you'll need

For building and testing SPIRV-Tools, the following tools should be
installed regardless of your OS:

- [CMake](http://www.cmake.org/): if using CMake for generating compilation
targets, you need to install CMake Version 2.8.12 or later.
- [Python 3](http://www.python.org/): for utility scripts and running the test
suite.
- [Bazel](https://bazel.build/) (optional): if building the source with Bazel,
you need to install Bazel Version 7.0.2 on your machine. Other versions may
also work, but are not verified.
- [Emscripten SDK](https://emscripten.org) (optional): if building the
  WebAssembly module.

SPIRV-Tools is regularly tested with the following compilers:

On Linux
- GCC version 9.4
- Clang version 10.0

On MacOS
- AppleClang 15.0

On Windows
- Visual Studio 2022

Note: Other compilers or later versions may work, but they are not tested.

### CMake options

The following CMake options are supported:

* `SPIRV_BUILD_FUZZER={ON|OFF}`, default `OFF` - Build the spirv-fuzz tool.
* `SPIRV_COLOR_TERMINAL={ON|OFF}`, default `ON` - Enables color console output.
* `SPIRV_SKIP_TESTS={ON|OFF}`, default `OFF`- Build only the library and
  the command line tools.  This will prevent the tests from being built.
* `SPIRV_SKIP_EXECUTABLES={ON|OFF}`, default `OFF`- Build only the library, not
  the command line tools and tests.
* `SPIRV_USE_SANITIZER=<sanitizer>`, default is no sanitizing - On UNIX
  platforms with an appropriate version of `clang` this option enables the use
  of the sanitizers documented [here][clang-sanitizers].
  This should only be used with a debug build.
* `SPIRV_WARN_EVERYTHING={ON|OFF}`, default `OFF` - On UNIX platforms enable
  more strict warnings.  The code might not compile with this option enabled.
  For Clang, enables `-Weverything`.  For GCC, enables `-Wpedantic`.
  See [`CMakeLists.txt`](CMakeLists.txt) for details.
* `SPIRV_WERROR={ON|OFF}`, default `ON` - Forces a compilation error on any
  warnings encountered by enabling the compiler-specific compiler front-end
  option.  No compiler front-end options are enabled when this option is OFF.

Additionally, you can pass additional C preprocessor definitions to SPIRV-Tools
via setting `SPIRV_TOOLS_EXTRA_DEFINITIONS`. For example, by setting it to
`/D_ITERATOR_DEBUG_LEVEL=0` on Windows, you can disable checked iterators and
iterator debugging.

### Android ndk-build

SPIR-V Tools supports building static libraries `libSPIRV-Tools.a` and
`libSPIRV-Tools-opt.a` for Android.  Using the Android NDK r25c or later:

```
cd <spirv-dir>

export ANDROID_NDK=/path/to/your/ndk   # NDK r25c or later

mkdir build && cd build
mkdir libs
mkdir app

$ANDROID_NDK/ndk-build -C ../android_test     \
                      NDK_PROJECT_PATH=.      \
                      NDK_LIBS_OUT=`pwd`/libs \
                      NDK_APP_OUT=`pwd`/app
```

### Updating DEPS

Occasionally the entries in [DEPS](DEPS) will need to be updated. This is done on
demand when there is a request to do this, often due to downstream breakages.
To update `DEPS`, run `utils/roll_deps.sh` and confirm that tests pass.
The script requires Chromium's
[`depot_tools`](https://chromium.googlesource.com/chromium/tools/depot_tools).

## Library

### Usage

The internals of the library use C++17 features, and are exposed via both a C
and C++ API.

In order to use the library from an application, the include path should point
to `<spirv-dir>/include`, which will enable the application to include the
header `<spirv-dir>/include/spirv-tools/libspirv.h{|pp}` then linking against
the static library in `<spirv-build-dir>/source/libSPIRV-Tools.a` or
`<spirv-build-dir>/source/SPIRV-Tools.lib`.
For optimization, the header file is
`<spirv-dir>/include/spirv-tools/optimizer.hpp`, and the static library is
`<spirv-build-dir>/source/libSPIRV-Tools-opt.a` or
`<spirv-build-dir>/source/SPIRV-Tools-opt.lib`.

* `SPIRV-Tools` CMake target: Creates the static library:
  * `<spirv-build-dir>/source/libSPIRV-Tools.a` on Linux and OS X.
  * `<spirv-build-dir>/source/libSPIRV-Tools.lib` on Windows.
* `SPIRV-Tools-opt` CMake target: Creates the static library:
  * `<spirv-build-dir>/source/libSPIRV-Tools-opt.a` on Linux and OS X.
  * `<spirv-build-dir>/source/libSPIRV-Tools-opt.lib` on Windows.

#### Entry points

The interfaces are still under development, and are expected to change.

There are five main entry points into the library in the C interface:

* `spvTextToBinary`: An assembler, translating text to a binary SPIR-V module.
* `spvBinaryToText`: A disassembler, translating a binary SPIR-V module to
  text.
* `spvBinaryParse`: The entry point to a binary parser API.  It issues callbacks
  for the header and each parsed instruction.  The disassembler is implemented
  as a client of `spvBinaryParse`.
* `spvValidate` implements the validator functionality. *Incomplete*
* `spvValidateBinary` implements the validator functionality. *Incomplete*

The C++ interface is comprised of three classes, `SpirvTools`, `Optimizer` and
`Linker`, all in the `spvtools` namespace.
* `SpirvTools` provides `Assemble`, `Disassemble`, and `Validate` methods.
* `Optimizer` provides methods for registering and running optimization passes.
* `Linker` provides methods for combining together multiple binaries.

## Command line tools

Command line tools, which wrap the above library functions, are provided to
assemble or disassemble shader files.  It's a convention to name SPIR-V
assembly and binary files with suffix `.spvasm` and `.spv`, respectively.

### Assembler tool

The assembler reads the assembly language text, and emits the binary form.

The standalone assembler is the executable called `spirv-as`, and is located in
`<spirv-build-dir>/tools/spirv-as`.  The functionality of the assembler is implemented
by the `spvTextToBinary` library function.

* `spirv-as` - the standalone assembler
  * `<spirv-dir>/tools/as`

Use option `-h` to print help.

### Disassembler tool

The disassembler reads the binary form, and emits assembly language text.

The standalone disassembler is the executable called `spirv-dis`, and is located in
`<spirv-build-dir>/tools/spirv-dis`. The functionality of the disassembler is implemented
by the `spvBinaryToText` library function.

* `spirv-dis` - the standalone disassembler
  * `<spirv-dir>/tools/dis`

Use option `-h` to print help.

The output includes syntax colouring when printing to the standard output stream,
on Linux, Windows, and OS X.

### Linker tool

The linker combines multiple SPIR-V binary modules together, resulting in a single
binary module as output.

This is a work in progress.
The linker does not support OpenCL program linking options related to math
flags. (See section 5.6.5.2 in OpenCL 1.2)

* `spirv-link` - the standalone linker
  * `<spirv-dir>/tools/link`

### Optimizer tool

The optimizer processes a SPIR-V binary module, applying transformations
in the specified order.

This is a work in progress, with initially only few available transformations.

* `spirv-opt` - the standalone optimizer
  * `<spirv-dir>/tools/opt`

### Validator tool

*Warning:* This functionality is under development, and is incomplete.

The standalone validator is the executable called `spirv-val`, and is located in
`<spirv-build-dir>/tools/spirv-val`. The functionality of the validator is implemented
by the `spvValidate` library function.

The validator operates on the binary form.

* `spirv-val` - the standalone validator
  * `<spirv-dir>/tools/val`

### Reducer tool

The reducer shrinks a SPIR-V binary module, guided by a user-supplied
*interestingness test*.

This is a work in progress, with initially only shrinks a module in a few ways.

* `spirv-reduce` - the standalone reducer
  * `<spirv-dir>/tools/reduce`

Run `spirv-reduce --help` to see how to specify interestingness.

### Fuzzer tool

The fuzzer transforms a SPIR-V binary module into a semantically-equivalent
SPIR-V binary module by applying transformations in a randomized fashion.

This is a work in progress, with initially only a few semantics-preserving
transformations.

* `spirv-fuzz` - the standalone fuzzer
  * `<spirv-dir>/tools/fuzz`

Run `spirv-fuzz --help` for a detailed list of options.

### Control flow dumper tool

The control flow dumper prints the control flow graph for a SPIR-V module as a
[GraphViz](http://www.graphviz.org/) graph.

This is experimental.

* `spirv-cfg` - the control flow graph dumper
  * `<spirv-dir>/tools/cfg`

### Diff tool

*Warning:* This functionality is under development, and is incomplete.

The diff tool produces a diff-style comparison between two SPIR-V modules.

* `spirv-diff` - the standalone diff tool
  * `<spirv-dir>`/tools/diff`

### Utility filters

* `spirv-lesspipe.sh` - Automatically disassembles `.spv` binary files for the
  `less` program, on compatible systems.  For example, set the `LESSOPEN`
  environment variable as follows, assuming both `spirv-lesspipe.sh` and
  `spirv-dis` are on your executable search path:
  ```
   export LESSOPEN='| spirv-lesspipe.sh "%s"'
  ```
  Then you page through a disassembled module as follows:
  ```
  less foo.spv
  ```
  * The `spirv-lesspipe.sh` script will pass through any extra arguments to
    `spirv-dis`.  So, for example, you can turn off colours and friendly ID
    naming as follows:
    ```
    export LESSOPEN='| spirv-lesspipe.sh "%s" --no-color --raw-id'
    ```

* [vim-spirv](https://github.com/kbenzie/vim-spirv) - A vim plugin which
  supports automatic disassembly of `.spv` files using the `:edit` command and
  assembly using the `:write` command. The plugin also provides additional
  features which include; syntax highlighting; highlighting of all ID's matching
  the ID under the cursor; and highlighting errors where the `Instruction`
  operand of `OpExtInst` is used without an appropriate `OpExtInstImport`.

* `50spirv-tools.el` - Automatically disassembles '.spv' binary files when
  loaded into the emacs text editor, and re-assembles them when saved,
  provided any modifications to the file are valid.  This functionality
  must be explicitly requested by defining the symbol
  SPIRV_TOOLS_INSTALL_EMACS_HELPERS as follows:
  ```
  cmake -DSPIRV_TOOLS_INSTALL_EMACS_HELPERS=true ...
  ```

  In addition, this helper is only installed if the directory /etc/emacs/site-start.d
  exists, which is typically true if emacs is installed on the system.

  Note that symbol IDs are not currently preserved through a load/edit/save operation.
  This may change if the ability is added to spirv-as.


### Tests

Tests are only built when googletest is found.

#### Running test with CMake

Use `ctest -j <num threads>` to run all the tests. To run tests using all threads:
```shell
ctest -j$(nproc)
```

To run a single test target, use `ctest [-j <N>] -R <test regex>`. For example,
you can run all `opt` tests with:
```shell
ctest -R 'spirv-tools-test_opt'
```

#### Running test with Bazel

Use `bazel test :all` to run all tests. This will run tests in parallel by default.

To run a single test target, specify `:my_test_target` instead of `:all`. Test target
names get printed when you run `bazel test :all`. For example, you can run
`opt_def_use_test` with:

on linux:
```shell
bazel test --cxxopt=-std=c++17 :opt_def_use_test
```

on windows:
```shell
bazel test --cxxopt=/std:c++17 :opt_def_use_test
```

## Future Work
<a name="future"></a>

_See the [projects pages](https://github.com/KhronosGroup/SPIRV-Tools/projects)
for more information._

### Assembler and disassembler

* The disassembler could emit helpful annotations in comments.  For example:
  * Use variable name information from debug instructions to annotate
    key operations on variables.
  * Show control flow information by annotating `OpLabel` instructions with
    that basic block's predecessors.
* Error messages could be improved.

### Validator

This is a work in progress.

### Linker

* The linker could accept math transformations such as allowing MADs, or other
  math flags passed at linking-time in OpenCL.
* Linkage attributes can not be applied through a group.
* Check decorations of linked functions attributes.
* Remove dead instructions, such as OpName targeting imported symbols.

## Licence
<a name="license"></a>
Full license terms are in [LICENSE](LICENSE)
```
Copyright (c) 2015-2016 The Khronos Group Inc.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
```

[spirv-tools-cla]: https://cla-assistant.io/KhronosGroup/SPIRV-Tools
[spirv-tools-projects]: https://github.com/KhronosGroup/SPIRV-Tools/projects
[spirv-tools-mailing-list]: https://www.khronos.org/spir/spirv-tools-mailing-list
[spirv-registry]: https://www.khronos.org/registry/spir-v/
[spirv-headers]: https://github.com/KhronosGroup/SPIRV-Headers
[googletest]: https://github.com/google/googletest
[googletest-pull-612]: https://github.com/google/googletest/pull/612
[googletest-issue-610]: https://github.com/google/googletest/issues/610
[effcee]: https://github.com/google/effcee
[re2]: https://github.com/google/re2
[abseil-cpp]: https://github.com/abseil/abseil-cpp
[mimalloc]: https://github.com/microsoft/mimalloc
[CMake]: https://cmake.org/
[cpp-style-guide]: https://google.github.io/styleguide/cppguide.html
[clang-sanitizers]: http://clang.llvm.org/docs/UsersManual.html#controlling-code-generation
