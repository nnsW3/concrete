// Part of the Concrete Compiler Project, under the BSD3 License with Zama
// Exceptions. See
// https://github.com/zama-ai/concrete-compiler-internal/blob/master/LICENSE.txt
// for license information.

#include <llvm/ADT/STLExtras.h>
#include <llvm/Support/Error.h>

#include <mlir/Dialect/LLVMIR/LLVMDialect.h>

#include "concretelang/ClientLib/ClientParameters.h"
#include "concretelang/Conversion/Utils/GlobalFHEContext.h"
#include "concretelang/Dialect/Concrete/IR/ConcreteTypes.h"
#include "concretelang/Support/V0Curves.h"

namespace mlir {
namespace concretelang {

const auto securityLevel = SECURITY_LEVEL_128;
const auto keyFormat = KEY_FORMAT_BINARY;
const auto v0Curve = getV0Curves(securityLevel, keyFormat);

// For the v0 the secretKeyID and precision are the same for all gates.
llvm::Expected<CircuitGate> gateFromMLIRType(LweSecretKeyID secretKeyID,
                                             Precision precision,
                                             Variance variance,
                                             mlir::Type type) {
  if (type.isIntOrIndex()) {
    // TODO - The index type is dependant of the target architecture, so
    // actually we assume we target only 64 bits, we need to have some the size
    // of the word of the target system.
    size_t width = 64;
    if (!type.isIndex()) {
      width = type.getIntOrFloatBitWidth();
    }
    return CircuitGate{
        /*.encryption = */ llvm::None,
        /*.shape = */
        {
            /*.width = */ width,
            /*.dimensions = */ std::vector<int64_t>(),
            /*.size = */ 0,
        },
    };
  }
  if (type.isa<mlir::concretelang::Concrete::LweCiphertextType>()) {
    // TODO - Get the width from the LWECiphertextType instead of global
    // precision (could be possible after merge concrete-ciphertext-parameter)
    return CircuitGate{
        /* .encryption = */ llvm::Optional<EncryptionGate>({
            /* .secretKeyID = */ secretKeyID,
            /* .variance = */ variance,
            /* .encoding = */
            {
                /* .precision = */ precision,
            },
        }),
        /*.shape = */
        {
            /*.width = */ precision,
            /*.dimensions = */ std::vector<int64_t>(),
            /*.size = */ 0,
        },
    };
  }
  auto tensor = type.dyn_cast_or_null<mlir::RankedTensorType>();
  if (tensor != nullptr) {
    auto gate = gateFromMLIRType(secretKeyID, precision, variance,
                                 tensor.getElementType());
    if (auto err = gate.takeError()) {
      return std::move(err);
    }
    gate->shape.dimensions = tensor.getShape().vec();
    gate->shape.size = 1;
    for (auto dimSize : gate->shape.dimensions) {
      gate->shape.size *= dimSize;
    }
    return gate;
  }
  return llvm::make_error<llvm::StringError>(
      "cannot convert MLIR type to shape", llvm::inconvertibleErrorCode());
}

ClientParameters emptyClientParametersForV0(llvm::StringRef functionName,
                                            mlir::ModuleOp module) {
  ClientParameters c;
  c.functionName = (std::string)functionName;
  return c;
}

llvm::Expected<ClientParameters>
createClientParametersForV0(V0FHEContext fheContext,
                            llvm::StringRef functionName,
                            mlir::ModuleOp module) {
  auto v0Param = fheContext.parameter;
  Variance encryptionVariance =
      v0Curve->getVariance(1, 1 << v0Param.logPolynomialSize, 64);
  Variance keyswitchVariance = v0Curve->getVariance(1, v0Param.nSmall, 64);
  // Static client parameters from global parameters for v0
  ClientParameters c;
  c.secretKeys = {
      {SMALL_KEY, {/*.size = */ v0Param.nSmall}},
      {BIG_KEY, {/*.size = */ v0Param.getNBigGlweDimension()}},
  };
  c.bootstrapKeys = {
      {
          "bsk_v0",
          {
              /*.inputSecretKeyID = */ SMALL_KEY,
              /*.outputSecretKeyID = */ BIG_KEY,
              /*.level = */ v0Param.brLevel,
              /*.baseLog = */ v0Param.brLogBase,
              /*.glweDimension = */ v0Param.glweDimension,
              /*.variance = */ encryptionVariance,
          },
      },
  };
  c.keyswitchKeys = {
      {
          "ksk_v0",
          {
              /*.inputSecretKeyID = */ BIG_KEY,
              /*.outputSecretKeyID = */ SMALL_KEY,
              /*.level = */ v0Param.ksLevel,
              /*.baseLog = */ v0Param.ksLogBase,
              /*.variance = */ keyswitchVariance,
          },
      },
  };
  c.functionName = (std::string)functionName;
  // Find the input function
  auto rangeOps = module.getOps<mlir::FuncOp>();
  auto funcOp = llvm::find_if(
      rangeOps, [&](mlir::FuncOp op) { return op.getName() == functionName; });
  if (funcOp == rangeOps.end()) {
    return llvm::make_error<llvm::StringError>(
        "cannot find the function for generate client parameters",
        llvm::inconvertibleErrorCode());
  }

  // For the v0 the precision is global
  auto precision = fheContext.constraint.p;

  // Create input and output circuit gate parameters
  auto funcType = (*funcOp).getType();

  auto inputs = funcType.getInputs();

  bool hasContext =
      inputs.empty()
          ? false
          : inputs.back().isa<mlir::concretelang::Concrete::ContextType>();

  for (auto inType = funcType.getInputs().begin();
       inType < funcType.getInputs().end() - hasContext; inType++) {
    auto gate =
        gateFromMLIRType(BIG_KEY, precision, encryptionVariance, *inType);
    if (auto err = gate.takeError()) {
      return std::move(err);
    }
    c.inputs.push_back(gate.get());
  }
  for (auto outType : funcType.getResults()) {
    auto gate =
        gateFromMLIRType(BIG_KEY, precision, encryptionVariance, outType);
    if (auto err = gate.takeError()) {
      return std::move(err);
    }
    c.outputs.push_back(gate.get());
  }
  return c;
}

} // namespace concretelang
} // namespace mlir
