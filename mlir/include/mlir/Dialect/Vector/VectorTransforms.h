//===- VectorTransforms.h - Vector transformations as patterns --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef DIALECT_VECTOR_VECTORTRANSFORMS_H_
#define DIALECT_VECTOR_VECTORTRANSFORMS_H_

#include "mlir/Dialect/Vector/VectorOps.h"
#include "mlir/Dialect/Vector/VectorUtils.h"
#include "mlir/IR/Function.h"
#include "mlir/IR/PatternMatch.h"

namespace mlir {
class MLIRContext;
class OwningRewritePatternList;
class VectorTransferOpInterface;

namespace scf {
class IfOp;
} // namespace scf

/// Collect a set of patterns to convert from the Vector dialect to itself.
/// Should be merged with populateVectorToSCFLoweringPattern.
void populateVectorToVectorConversionPatterns(
    MLIRContext *context, OwningRewritePatternList &patterns,
    ArrayRef<int64_t> coarseVectorShape = {},
    ArrayRef<int64_t> fineVectorShape = {});

namespace vector {

/// Entry point for unrolling declarative pattern rewrites.
/// `op` is unrolled to the `targetShape` as follows, for each of its operands:
///   1. the unrolled type `unrolledVectorType` and number of unrolled instances
///   `numUnrolledInstances` are computed from the `targetShape`. For now it is
///   assumed the unrolling factors divide the vector sizes.
///   2. a fakeFork cast op is inserted that takes the operand and returns
///   `numUnrolledInstances` results of type `unrolledVectorType`.
///   3. the original op is cloned `numUnrolledInstances` times, once for each
///   result of the fakeFork cast op.
///   4. a fakeJoin cast op takes all these results and merges them into a
///   single aggregate vector result whose size matches the original
///   non-unrolled op operand types.
///
/// Example:
///
///    opA(operand0, operand1)  // numUnrolledInstances = 3
///
///            operand0                   operand1
///               |                          |
///             fork                       fork
///        <----------gather all fork ops --------->
///              /|\                        /|\
///          f00 f01 f02                f10 f11 f12
///        <---------- clone op 3 times --------->
///          opA0(f00, f10), opA1(f01, f11), opA2(f02, f12)
///                 \            |            /
///      <-------------------- join ------------------------->
///
/// Other local patterns then kick in iteratively (including DCE) and compose
/// until all the fakeFork and fakeJoin ops are removed.
///
/// This will be extended in the future to support more advanced use cases than
/// simple pointwise ops.
SmallVector<Value, 1> unrollSingleResultVectorOp(OpBuilder &builder,
                                                 Operation *op,
                                                 ArrayRef<int64_t> targetShape);

/// Pattern to apply `unrollSingleResultVectorOp` to a `targetShape`
/// declaratively.
template <typename OpTy>
struct UnrollVectorPattern : public OpRewritePattern<OpTy> {
  using FilterConstraintType = std::function<LogicalResult(OpTy op)>;
  UnrollVectorPattern(
      ArrayRef<int64_t> targetShape, MLIRContext *context,
      FilterConstraintType constraint = [](OpTy op) { return success(); })
      : OpRewritePattern<OpTy>(context),
        targetShape(targetShape.begin(), targetShape.end()),
        filter(constraint) {}
  LogicalResult matchAndRewrite(OpTy op,
                                PatternRewriter &rewriter) const override {
    if (failed(filter(op)))
      return failure();
    auto unrollableVectorOp =
        dyn_cast<VectorUnrollOpInterface>(op.getOperation());
    if (!unrollableVectorOp)
      return failure();
    auto maybeUnrollShape = unrollableVectorOp.getShapeForUnroll();
    if (!maybeUnrollShape)
      return failure();
    auto maybeShapeRatio = shapeRatio(*maybeUnrollShape, targetShape);
    if (!maybeShapeRatio ||
        llvm::all_of(*maybeShapeRatio, [](int64_t v) { return v == 1; }))
      return failure();
    if (op.getOperation()->getNumResults() != 1)
      return failure();
    auto resultVector = unrollSingleResultVectorOp(rewriter, op, targetShape);
    if (resultVector.size() != 1)
      return failure();
    rewriter.replaceOp(op, resultVector.front());
    return success();
  }

private:
  SmallVector<int64_t, 4> targetShape;
  FilterConstraintType filter;
};

/// Split a vector.transfer operation into an unmasked fastpath vector.transfer
/// and a slowpath masked vector.transfer. If `ifOp` is not null and the result
/// is `success, the `ifOp` points to the newly created conditional upon
/// function return. To accomodate for the fact that the original
/// vector.transfer indexing may be arbitrary and the slow path indexes @[0...0]
/// in the temporary buffer, the scf.if op returns a view and values of type
/// index. At this time, only vector.transfer_read is implemented.
///
/// Example (a 2-D vector.transfer_read):
/// ```
///    %1 = vector.transfer_read %0[...], %pad : memref<A...>, vector<...>
/// ```
/// is transformed into:
/// ```
///    %1:3 = scf.if (%inBounds) {
///       scf.yield %0 : memref<A...>, index, index
///     } else {
///       %2 = vector.transfer_read %0[...], %pad : memref<A...>, vector<...>
///       %3 = vector.type_cast %extra_alloc : memref<...> to
///       memref<vector<...>> store %2, %3[] : memref<vector<...>> %4 =
///       memref_cast %extra_alloc: memref<B...> to memref<A...> scf.yield %4 :
///       memref<A...>, index, index
//     }
///    %0 = vector.transfer_read %1#0[%1#1, %1#2] {masked = [false ... false]}
/// ```
/// where `extra_alloc` is a top of the function alloca'ed buffer of one vector.
///
/// Preconditions:
///  1. `xferOp.permutation_map()` must be a minor identity map
///  2. the rank of the `xferOp.memref()` and the rank of the `xferOp.vector()`
///  must be equal. This will be relaxed in the future but requires
///  rank-reducing subviews.
LogicalResult
splitFullAndPartialTransferPrecondition(VectorTransferOpInterface xferOp);
LogicalResult splitFullAndPartialTransfer(OpBuilder &b,
                                          VectorTransferOpInterface xferOp,
                                          scf::IfOp *ifOp = nullptr);

/// Apply `splitFullAndPartialTransfer` selectively via a pattern. This pattern
/// may take an extra filter to perform selection at a finer granularity.
struct VectorTransferFullPartialRewriter : public RewritePattern {
  using FilterConstraintType =
      std::function<LogicalResult(VectorTransferOpInterface op)>;

  explicit VectorTransferFullPartialRewriter(
      MLIRContext *context,
      FilterConstraintType filter =
          [](VectorTransferOpInterface op) { return success(); },
      PatternBenefit benefit = 1)
      : RewritePattern(benefit, MatchAnyOpTypeTag()), filter(filter) {}

  /// Performs the rewrite.
  LogicalResult matchAndRewrite(Operation *op,
                                PatternRewriter &rewriter) const override;

private:
  FilterConstraintType filter;
};

} // namespace vector

//===----------------------------------------------------------------------===//
// Finer-grained patterns exposed for more control over individual lowerings.
//===----------------------------------------------------------------------===//

/// Progressive lowering of a `vector.contract %a, %b, %c` with row-major matmul
/// semantics to:
/// ```
///    %flattened_a = vector.shape_cast %a
///    %flattened_b = vector.shape_cast %b
///    %flattened_d = vector.matmul %flattened_a, %flattened_b
///    %d = vector.shape_cast %%flattened_d
///    %e = add %c, %d
/// ```
/// `vector.matmul` later lowers to `llvm.matrix.multiply`.
//
/// This only kicks in when VectorTransformsOptions is set to OuterProduct and
/// the vector.contract op is a row-major matrix multiply.
class ContractionOpToMatmulOpLowering
    : public OpRewritePattern<vector::ContractionOp> {
public:
  using OpRewritePattern<vector::ContractionOp>::OpRewritePattern;
  using FilterConstraintType =
      std::function<LogicalResult(vector::ContractionOp op)>;

  static LogicalResult defaultFilter(vector::ContractionOp op) {
    return success();
  }

  ContractionOpToMatmulOpLowering(
      vector::VectorTransformsOptions vectorTransformsOptions,
      MLIRContext *context, FilterConstraintType constraint = defaultFilter)
      : OpRewritePattern<vector::ContractionOp>(context),
        vectorTransformsOptions(vectorTransformsOptions), filter(constraint) {}

  LogicalResult match(vector::ContractionOp op) const override;
  void rewrite(vector::ContractionOp op,
               PatternRewriter &rewriter) const override;

private:
  /// Options to control the vector patterns.
  vector::VectorTransformsOptions vectorTransformsOptions;
  FilterConstraintType filter;
};

/// Progressive lowering of a `vector.contract %a, %b, %c` with row-major matmul
/// semantics to a reduction_size-unrolled sequence:
/// ```
///    %at = vector.transpose %a, [1, 0]
///    %bRow0 = vector.extract %b[0]
///    %atRow0 = vector.extract %at[0]
///    %c0 = vector.outerproduct %atRow0, %bRow0, %c
///    ...
///    %bRowK = vector.extract %b[K]
///    %atRowK = vector.extract %at[K]
///    %cK = vector.outerproduct %atRowK, %bRowK, %cK-1
/// ```
///
/// This only kicks in when VectorTransformsOptions is set to OuterProduct and
/// the vector.contract op is a row-major matrix multiply.
class ContractionOpToOuterProductOpLowering
    : public OpRewritePattern<vector::ContractionOp> {
public:
  using OpRewritePattern<vector::ContractionOp>::OpRewritePattern;
  using FilterConstraintType =
      std::function<LogicalResult(vector::ContractionOp op)>;

  static LogicalResult defaultFilter(vector::ContractionOp op) {
    return success();
  }

  ContractionOpToOuterProductOpLowering(
      vector::VectorTransformsOptions vectorTransformsOptions,
      MLIRContext *context, FilterConstraintType constraint = defaultFilter)
      : OpRewritePattern<vector::ContractionOp>(context),
        vectorTransformsOptions(vectorTransformsOptions), filter(constraint) {}

  LogicalResult match(vector::ContractionOp op) const override;
  void rewrite(vector::ContractionOp op,
               PatternRewriter &rewriter) const override;

private:
  /// Options to control the vector patterns.
  vector::VectorTransformsOptions vectorTransformsOptions;
  FilterConstraintType filter;
};

/// Progressive lowering of ContractionOp.
///
/// One:
///   %x = vector.contract with at least one free/batch dimension
/// is replaced by:
///   %a = vector.contract with one less free/batch dimension
///   %b = vector.contract with one less free/batch dimension
///   ..
///   %x = combine %a %b ..
/// until a pure contraction is reached (no free/batch dimensions),
/// which is replaced by a dot-product.
///
/// This only kicks in when either VectorTransformsOptions is set
/// to Dot or when other contraction patterns fail.
class ContractionOpLowering : public OpRewritePattern<vector::ContractionOp> {
public:
  using OpRewritePattern<vector::ContractionOp>::OpRewritePattern;
  using FilterConstraintType =
      std::function<LogicalResult(vector::ContractionOp op)>;

  static LogicalResult defaultFilter(vector::ContractionOp op) {
    return success();
  }

  ContractionOpLowering(vector::VectorTransformsOptions vectorTransformsOptions,
                        MLIRContext *context,
                        FilterConstraintType constraint = defaultFilter)
      : OpRewritePattern<vector::ContractionOp>(context),
        vectorTransformsOptions(vectorTransformsOptions), filter(constraint) {}

  LogicalResult matchAndRewrite(vector::ContractionOp op,
                                PatternRewriter &rewriter) const override;

private:
  /// Options to control the vector patterns.
  vector::VectorTransformsOptions vectorTransformsOptions;
  FilterConstraintType filter;
  // Lower one parallel dimension.
  Value lowerParallel(vector::ContractionOp op, int64_t lhsIndex,
                      int64_t rhsIndex, PatternRewriter &rewriter) const;
  // Lower one reduction dimension.
  Value lowerReduction(vector::ContractionOp op,
                       PatternRewriter &rewriter) const;
};

} // namespace mlir

#endif // DIALECT_VECTOR_VECTORTRANSFORMS_H_
