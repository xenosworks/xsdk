# Draft for KhronosGroup/glslang: HLSL's fmod floors where HLSL truncates

Not posted. glslang's HLSL front end is deprecated (KhronosGroup/glslang#4210): HLSL bug reports are closed with a
pointer to that issue, while a low-risk pull request fixing an HLSL bug, with tests, that doesn't touch shared
infrastructure may still be taken. So this is written as a pull request, glslang-xenon 40a553d5 on 16.3.0, with the
issue text first should a report be wanted instead.

## Issue

**Title:** HLSL: `fmod` returns GLSL's floored `mod`, with the sign of y; HLSL's has the sign of x

HLSL's `fmod(x, y)` is the remainder of x / y with the sign of x, x - y * trunc(x / y), as C's `fmod`:

- Cg, whose language HLSL shares: "fmod returns the remainder of x divided by y with the same sign as x", its reference
  implementation `frac(abs(a/b))*abs(b)`, negated where `a < 0` (NVIDIA Cg 3.1 Toolkit documentation, `fmod`).
- Wine's HLSL compiler, vkd3d-shader, works it out as `frac(|x / y|) * y`, negated where x / y is negative
  (libs/vkd3d-shader/hlsl.y, `intrinsic_fmod`), and its test wants `fmod(-0.5, 6.5)` to be -0.5 and `fmod(1.1, 0.3)`
  0.2 (tests/hlsl/fmod.shader_test).
- Clang's HLSL lowers `fmod` to SPIR-V's `OpFRem` (llvm/llvm-project#99118), whose result's sign is Operand 1's.

glslang relates `fmod` to `EOpMod` (glslang/HLSL/hlslParseables.cpp:1074), GLSL's `mod`, which becomes `OpFMod`
(SPIRV/GlslangToSpv.cpp:8049), whose result's sign is Operand 2's; constant arguments fold the same way,
`arg0 - arg1 * floor(arg0 / arg1)` (glslang/MachineIndependent/Constant.cpp:955). The two differ wherever x and y have
opposite signs: `fmod(-7.5, 2.0)` is -1.5 in HLSL and 0.5 from glslang.

**Repro** (glslang 16.3.0):

```hlsl
float4 main(float4 x : TEXCOORD0, float4 y : TEXCOORD1) : SV_Target0
{
    return float4(fmod(-7.5, 2.0), fmod(7.5, -2.0), 0.0, 0.0) + fmod(x, y);
}
```

`glslang -D -V -e main -S frag repro.frag -o repro.spv`, disassembled:

```
  %float_0_5 = OpConstant %float 0.5
 %float_n0_5 = OpConstant %float -0.5
         %17 = OpConstantComposite %v4float %float_0_5 %float_n0_5 %float_0 %float_0
         %42 = OpFMod %v4float %27 %30
```

Expected: the constants -1.5 and 1.5, and a remainder with the sign of x, `OpFRem` or x - y * trunc(x / y).

## Pull request

**Title:** HLSL: give fmod the sign of x, as HLSL's truncates

`fmod` was GLSL's `mod`, `OpFMod`, which floors the quotient and takes the sign of y; HLSL's truncates it and takes the
sign of x (sources in the issue above).

- `HlslParseContext::decomposeIntrinsic` writes `fmod(x, y)` as `x - y * trunc(x / y)`, each argument in a temporary so
  it is evaluated once, and a matrix a column at a time, as GLSL.std.450's `Trunc` takes no matrix. This stays in the
  HLSL front end and needs no new operator; legalisation leaves `OpFDiv`, `Trunc`, `OpFMul` and `OpFSub`.
- Constant arguments fold in `TIntermediate::fold` before that, so the fold truncates where the source is HLSL. This
  one line is in shared code, gated on `EShSourceHlsl`; GLSL's `mod` folds as before.
- HLSL's `%` operator is unchanged.

Emitting `OpFRem`, as clang does, would be one instruction, but glslang has no operator for it: `EOpMod` is GLSL's `mod`
too, so it would need a new operator or a source check in `GlslangToSpv`, beside the same fold.

Tests: `hlsl.intrinsics.frag` and `hlsl.intrinsics.vert`'s results change only where they call `fmod`; a new
`hlsl.intrinsics.fmod.frag` checks the folded constants, -1.5 and 1.5, and a matrix's columns. Every gtest passes, and
`Test/runtests`.

The change, against 16.3.0 (glslang-xenon 40a553d5, baseline results left out):

```diff
--- a/glslang/HLSL/hlslParseHelper.cpp
+++ b/glslang/HLSL/hlslParseHelper.cpp
@@ -4900,6 +4900,62 @@ void HlslParseContext::decomposeIntrinsic(const TSourceLoc& loc, TIntermTyped*&
             break;
         }
 
+    case EOpMod:
+        {
+            // fmod(x, y) -> x - y * trunc(x / y), the sign of x; OpFMod's is y's (SPIR-V 1.0, section 3.32.13)
+            TIntermTyped* arg0 = argAggregate->getSequence()[0]->getAsTyped();
+            TIntermTyped* arg1 = argAggregate->getSequence()[1]->getAsTyped();
+
+            // Temporaries, so each argument is evaluated once.
+            TVariable* x = makeInternalVariable("@fmodx", arg0->getType());
+            x->getWritableType().getQualifier().makeTemporary();
+            TVariable* y = makeInternalVariable("@fmody", arg1->getType());
+            y->getWritableType().getQualifier().makeTemporary();
+
+            TIntermTyped* xAssign = intermediate.addAssign(EOpAssign, intermediate.addSymbol(*x, loc), arg0, loc);
+            TIntermTyped* yAssign = intermediate.addAssign(EOpAssign, intermediate.addSymbol(*y, loc), arg1, loc);
+
+            // A matrix's columns one at a time, as GLSL.std.450's Trunc takes no matrix.
+            const auto remainderOf = [&](int column) -> TIntermTyped* {
+                const auto operand = [&](TVariable* v) -> TIntermTyped* {
+                    TIntermTyped* symbol = intermediate.addSymbol(*v, loc);
+                    if (column < 0)
+                        return symbol;
+                    TIntermTyped* index = intermediate.addIndex(EOpIndexDirect, symbol,
+                                                                intermediate.addConstantUnion(column, loc), loc);
+                    index->setType(TType(v->getType(), 0));
+                    return index;
+                };
+                TIntermTyped* quotient = handleBinaryMath(loc, "fmod", EOpDiv, operand(x), operand(y));
+                TIntermTyped* whole = handleUnaryMath(loc, "fmod", EOpTrunc, quotient);
+                TIntermTyped* product = handleBinaryMath(loc, "fmod", EOpMul, operand(y), whole);
+                return handleBinaryMath(loc, "fmod", EOpSub, operand(x), product);
+            };
+
+            TIntermTyped* remainder = nullptr;
+            if (arg0->isMatrix()) {
+                TIntermAggregate* columns = new TIntermAggregate(intermediate.mapTypeToConstructorOp(arg0->getType()));
+                for (int column = 0; column < arg0->getMatrixCols(); ++column)
+                    columns->getSequence().push_back(remainderOf(column));
+                columns->setType(arg0->getType());
+                columns->getWritableType().getQualifier().makeTemporary();
+                columns->setLoc(loc);
+                remainder = columns;
+            } else
+                remainder = remainderOf(-1);
+
+            TIntermAggregate* compoundStatement = intermediate.makeAggregate(xAssign, loc);
+            compoundStatement = intermediate.growAggregate(compoundStatement, yAssign);
+            compoundStatement = intermediate.growAggregate(compoundStatement, remainder);
+            compoundStatement->setOperator(EOpSequence);
+            compoundStatement->setLoc(loc);
+            compoundStatement->setType(node->getType());
+
+            node = compoundStatement;
+
+            break;
+        }
+
     case EOpRcp:
         {
             // rcp(a) -> 1 / a
--- a/glslang/MachineIndependent/Constant.cpp
+++ b/glslang/MachineIndependent/Constant.cpp
@@ -952,7 +952,9 @@ TIntermTyped* TIntermediate::fold(TIntermAggregate* aggrNode)
             {
                 double arg0 = childConstUnions[0][arg0comp].getDConst();
                 double arg1 = childConstUnions[1][arg1comp].getDConst();
-                double result = arg0 - arg1 * floor(arg0 / arg1);
+                // HLSL's fmod truncates the quotient (see decomposeIntrinsic), GLSL's mod floors it.
+                double quotient = getSource() == EShSourceHlsl ? trunc(arg0 / arg1) : floor(arg0 / arg1);
+                double result = arg0 - arg1 * quotient;
                 newConstArray[comp].setDConst(result);
                 break;
             }
```

The same SPIR-V of the repro after it:

```
 %float_n1_5 = OpConstant %float -1.5
  %float_1_5 = OpConstant %float 1.5
         %17 = OpConstantComposite %v4float %float_n1_5 %float_1_5 %float_0 %float_0
         %57 = OpFDiv %v4float %36 %39
         %58 = OpExtInst %v4float %1 Trunc %57
         %60 = OpFSub %v4float %36 %59
```

## Not drafted: DX9's `register(bN)` and `register(iN)`

glslang-xenon 026e7416 keeps a `$Global` bool's `b#` and int's `i#` as a UserSemantic string, `"register(b3)"`, for
xsdk's shader compiler. That is a new convention, not a bug fix, for shader model 3 alone, through a new `TQualifier`
field, which is shared infrastructure, in a front end being retired; upstream's policy above leaves no room for it.
