//===- TypeInference.cpp - Type inference passes -----------------*- C++-*-===//
//
// This file is licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PassDetail.h"

#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Interfaces/ControlFlowInterfaces.h"
#include "npcomp/Dialect/Basicpy/IR/BasicpyDialect.h"
#include "npcomp/Dialect/Basicpy/IR/BasicpyOps.h"
#include "npcomp/Dialect/Basicpy/Transforms/Passes.h"
#include "npcomp/Typing/CPASupport.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "basicpy-type-inference"

using namespace llvm;
using namespace mlir;
using namespace mlir::NPCOMP::Basicpy;
using namespace mlir::npcomp::typing;

namespace {

class InitialConstraintGenerator {
public:
  InitialConstraintGenerator(CPA::Context &cpaContext,
                             CPA::ConstraintSet *constraints,
                             CPA::TypeVarSet *typeVars)
      : cpaContext(cpaContext), constraints(constraints), typeVars(typeVars) {}

  /// If a return op was visited, this will be one of them.
  Operation *getLastReturnOp() { return funcReturnOp; }

  /// Gets any ReturnLike ops that do not return from the outer function.
  /// This is used to fixup parent SCF ops and the like.
  llvm::SmallVectorImpl<Operation *> &getInnerReturnLikeOps() {
    return innerReturnLikeOps;
  }

  llvm::DenseMap<Value, CPA::TypeBase *> valueTypeMap;

  CPA::TypeBase *resolveValueType(Value value) {
    CPA::TypeBase *&cpaType = valueTypeMap[value];
    if (cpaType)
      return cpaType;

    Type t = value.getType();
    if (t.isa<UnknownType>()) {
      // Type variable.
      auto *cpaTypeVar = cpaContext.newTypeVar(value);
      cpaType = cpaTypeVar;
      typeVars->getTypeVars().push_back(*cpaTypeVar);
    } else {
      // IR type.
      cpaType = cpaContext.getIRValueType(t);
    }

    return cpaType;
  }

  void addSubtypeConstraint(Value superValue, Value subValue,
                            Operation *context) {
    auto superVt = resolveValueType(superValue);
    auto subVt = resolveValueType(subValue);
    CPA::Constraint *c = cpaContext.newConstraint(superVt, subVt);
    c->setContextOp(context);
    constraints->getConstraints().push_back(*c);
  }

  LogicalResult runOnFunction(FuncOp funcOp) {
    // Iterate and create type nodes for entry block arguments, as these
    // must be resolved no matter what.
    if (funcOp.getBody().empty())
      return success();

    auto &entryBlock = funcOp.getBody().front();
    for (auto blockArg : entryBlock.getArguments()) {
      resolveValueType(blockArg);
    }

    // Then walk ops, creating equations.
    LLVM_DEBUG(llvm::dbgs() << "POPULATE CHILD OPS:\n");
    auto result = funcOp.walk([&](Operation *childOp) -> WalkResult {
      if (childOp == funcOp)
        return WalkResult::advance();
      LLVM_DEBUG(llvm::dbgs() << "  + POPULATE: " << *childOp << "\n");
      // Special op handling.
      // Many of these (that are not standard ops) should become op
      // interfaces.
      // --------------------
      if (auto op = dyn_cast<SelectOp>(childOp)) {
        // Note that the condition is always i1 and not subject to type
        // inference.
        addSubtypeConstraint(op.true_value(), op.false_value(), op);
        return WalkResult::advance();
      }
      if (auto op = dyn_cast<ToBooleanOp>(childOp)) {
        // Note that the result is always i1 and not subject to type
        // inference.
        resolveValueType(op.operand());
        return WalkResult::advance();
      }
      if (auto op = dyn_cast<scf::IfOp>(childOp)) {
        // Note that the condition is always i1 and not subject to type
        // inference.
        for (auto result : op.getResults()) {
          resolveValueType(result);
        }
        return WalkResult::advance();
      }
      if (auto yieldOp = dyn_cast<scf::YieldOp>(childOp)) {
        auto scfParentOp = yieldOp.getParentOp();
        if (scfParentOp->getNumResults() != yieldOp.getNumOperands()) {
          yieldOp.emitWarning()
              << "cannot run type inference on yield due to arity mismatch";
          return WalkResult::advance();
        }
        for (auto it :
             llvm::zip(scfParentOp->getResults(), yieldOp.getOperands())) {
          addSubtypeConstraint(std::get<1>(it), std::get<0>(it), yieldOp);
        }
        return WalkResult::advance();
      }
      if (auto op = dyn_cast<UnknownCastOp>(childOp)) {
        addSubtypeConstraint(op.operand(), op.result(), op);
        return WalkResult::advance();
      }
      if (auto op = dyn_cast<BinaryExprOp>(childOp)) {
        // TODO: This should really be applying arithmetic promotion, not
        // strict equality.
        addSubtypeConstraint(op.left(), op.right(), op);
        addSubtypeConstraint(op.left(), op.result(), op);
        return WalkResult::advance();
      }
      if (auto op = dyn_cast<BinaryCompareOp>(childOp)) {
        // TODO: This should really be applying arithmetic promotion, not
        // strict equality.
        addSubtypeConstraint(op.left(), op.right(), op);
        return WalkResult::advance();
      }

      // Fallback trait based equations.
      // ----------------------
      // Ensure that constant nodes get assigned a constant type.
      if (childOp->hasTrait<OpTrait::ConstantLike>()) {
        resolveValueType(childOp->getResult(0));
        return WalkResult::advance();
      }
      // Function returns must all have the same types.
      if (childOp->hasTrait<OpTrait::ReturnLike>()) {
        if (childOp->getParentOp() == funcOp) {
          if (funcReturnOp) {
            if (funcReturnOp->getNumOperands() != childOp->getNumOperands()) {
              childOp->emitOpError() << "different arity of function returns";
              return WalkResult::interrupt();
            }
            for (auto it : llvm::zip(funcReturnOp->getOperands(),
                                     childOp->getOperands())) {
              addSubtypeConstraint(std::get<0>(it), std::get<1>(it), childOp);
            }
          }
          funcReturnOp = childOp;
          return WalkResult::advance();
        } else {
          innerReturnLikeOps.push_back(childOp);
        }
      }

      childOp->emitRemark() << "unhandled op in type inference";

      return WalkResult::advance();
    });

    return success(result.wasInterrupted());
  }

private:
  // The last encountered ReturnLike op.
  Operation *funcReturnOp = nullptr;
  llvm::SmallVector<Operation *, 4> innerReturnLikeOps;
  CPA::Context &cpaContext;
  CPA::ConstraintSet *constraints;
  CPA::TypeVarSet *typeVars;
};

class CPAFunctionTypeInferencePass
    : public CPAFunctionTypeInferenceBase<CPAFunctionTypeInferencePass> {
public:
  void runOnOperation() override {
    FuncOp func = getOperation();
    if (func.getBody().empty())
      return;

    CPA::Context cpaContext;
    auto constraints = cpaContext.newConstraintSet();
    auto typeVars = cpaContext.newTypeVarSet();

    InitialConstraintGenerator p(cpaContext, constraints, typeVars);
    p.runOnFunction(func);

    llvm::errs() << "CONSTRAINTS:\n";
    llvm::errs() << "------------\n";
    constraints->print(llvm::errs());

    llvm::errs() << "TYPEVARS:\n";
    llvm::errs() << "---------\n";
    typeVars->print(llvm::errs());
  }
};

} // namespace

std::unique_ptr<OperationPass<FuncOp>>
mlir::NPCOMP::Basicpy::createCPAFunctionTypeInferencePass() {
  return std::make_unique<CPAFunctionTypeInferencePass>();
}