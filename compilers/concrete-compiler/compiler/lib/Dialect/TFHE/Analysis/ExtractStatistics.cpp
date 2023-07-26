#include <concretelang/Dialect/TFHE/Analysis/ExtractStatistics.h>

#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/SCF/IR/SCF.h>

#include <concretelang/Dialect/TFHE/IR/TFHEOps.h>

using namespace mlir::concretelang;
using namespace mlir;

using TFHE::ExtractTFHEStatisticsPass;

// #########
// Utilities
// #########

template <typename Op> std::string locationOf(Op op) {
  auto location = std::string();
  auto locationStream = llvm::raw_string_ostream(location);
  op.getLoc()->print(locationStream);
  return location.substr(5, location.size() - 2 - 5); // remove loc(" and ")
}

// #######
// scf.for
// #######

int64_t calculateNumberOfIterations(int64_t start, int64_t stop, int64_t step) {
  int64_t high;
  int64_t low;

  if (step > 0) {
    low = start;
    high = stop;
  } else {
    low = stop;
    high = start;
    step = -step;
  }

  if (low >= high) {
    return 0;
  }

  return ((high - low - 1) / step) + 1;
}

std::optional<StringError> calculateNumberOfIterations(scf::ForOp &op,
                                                       int64_t &result) {
  mlir::Value startValue = op.getLowerBound();
  mlir::Value stopValue = op.getUpperBound();
  mlir::Value stepValue = op.getStep();

  auto startOp =
      llvm::dyn_cast_or_null<arith::ConstantOp>(startValue.getDefiningOp());
  auto stopOp =
      llvm::dyn_cast_or_null<arith::ConstantOp>(stopValue.getDefiningOp());
  auto stepOp =
      llvm::dyn_cast_or_null<arith::ConstantOp>(stepValue.getDefiningOp());

  if (!startOp || !stopOp || !stepOp) {
    return StringError("only static loops can be analyzed");
  }

  auto startAttr = startOp.getValue().cast<mlir::IntegerAttr>();
  auto stopAttr = stopOp.getValue().cast<mlir::IntegerAttr>();
  auto stepAttr = stepOp.getValue().cast<mlir::IntegerAttr>();

  if (!startOp || !stopOp || !stepOp) {
    return StringError("only integer loops can be analyzed");
  }

  int64_t start = startAttr.getInt();
  int64_t stop = stopAttr.getInt();
  int64_t step = stepAttr.getInt();

  result = calculateNumberOfIterations(start, stop, step);
  return std::nullopt;
}

static std::optional<StringError> on_enter(scf::ForOp &op,
                                           ExtractTFHEStatisticsPass &pass) {
  int64_t numberOfIterations;

  std::optional<StringError> error =
      calculateNumberOfIterations(op, numberOfIterations);
  if (error.has_value()) {
    return error;
  }

  assert(numberOfIterations > 0);
  pass.iterations *= (uint64_t)numberOfIterations;
  return std::nullopt;
}

static std::optional<StringError> on_exit(scf::ForOp &op,
                                          ExtractTFHEStatisticsPass &pass) {
  int64_t numberOfIterations;

  std::optional<StringError> error =
      calculateNumberOfIterations(op, numberOfIterations);
  if (error.has_value()) {
    return error;
  }

  assert(numberOfIterations > 0);
  pass.iterations /= (uint64_t)numberOfIterations;
  return std::nullopt;
}

// #############
// TFHE.add_glwe
// #############

static std::optional<StringError> on_enter(TFHE::AddGLWEOp &op,
                                           ExtractTFHEStatisticsPass &pass) {
  auto resultingKey = op.getType().getKey().getNormalized();

  auto location = locationOf(op);
  auto operation = PrimitiveOperation::ENCRYPTED_ADDITION;
  auto keys = std::vector<std::pair<KeyType, size_t>>();
  auto count = pass.iterations;

  std::pair<KeyType, size_t> key =
      std::make_pair(KeyType::SECRET, (size_t)resultingKey->index);
  keys.push_back(key);

  pass.feedback.statistics.push_back(Statistic{
      location,
      operation,
      keys,
      count,
  });

  return std::nullopt;
}

// #################
// TFHE.add_glwe_int
// #################

static std::optional<StringError> on_enter(TFHE::AddGLWEIntOp &op,
                                           ExtractTFHEStatisticsPass &pass) {
  auto resultingKey = op.getType().getKey().getNormalized();

  auto location = locationOf(op);
  auto operation = PrimitiveOperation::CLEAR_ADDITION;
  auto keys = std::vector<std::pair<KeyType, size_t>>();
  auto count = pass.iterations;

  std::pair<KeyType, size_t> key =
      std::make_pair(KeyType::SECRET, (size_t)resultingKey->index);
  keys.push_back(key);

  pass.feedback.statistics.push_back(Statistic{
      location,
      operation,
      keys,
      count,
  });

  return std::nullopt;
}

// ###################
// TFHE.bootstrap_glwe
// ###################

static std::optional<StringError> on_enter(TFHE::BootstrapGLWEOp &op,
                                           ExtractTFHEStatisticsPass &pass) {
  auto bsk = op.getKey();

  auto location = locationOf(op);
  auto operation = PrimitiveOperation::PBS;
  auto keys = std::vector<std::pair<KeyType, size_t>>();
  auto count = pass.iterations;

  std::pair<KeyType, size_t> key =
      std::make_pair(KeyType::BOOTSTRAP, (size_t)bsk.getIndex());
  keys.push_back(key);

  pass.feedback.statistics.push_back(Statistic{
      location,
      operation,
      keys,
      count,
  });

  return std::nullopt;
}

// ###################
// TFHE.keyswitch_glwe
// ###################

static std::optional<StringError> on_enter(TFHE::KeySwitchGLWEOp &op,
                                           ExtractTFHEStatisticsPass &pass) {
  auto ksk = op.getKey();

  auto location = locationOf(op);
  auto operation = PrimitiveOperation::KEY_SWITCH;
  auto keys = std::vector<std::pair<KeyType, size_t>>();
  auto count = pass.iterations;

  std::pair<KeyType, size_t> key =
      std::make_pair(KeyType::KEY_SWITCH, (size_t)ksk.getIndex());
  keys.push_back(key);

  pass.feedback.statistics.push_back(Statistic{
      location,
      operation,
      keys,
      count,
  });

  return std::nullopt;
}

// #################
// TFHE.mul_glwe_int
// #################

static std::optional<StringError> on_enter(TFHE::MulGLWEIntOp &op,
                                           ExtractTFHEStatisticsPass &pass) {
  auto resultingKey = op.getType().getKey().getNormalized();

  auto location = locationOf(op);
  auto operation = PrimitiveOperation::CLEAR_MULTIPLICATION;
  auto keys = std::vector<std::pair<KeyType, size_t>>();
  auto count = pass.iterations;

  std::pair<KeyType, size_t> key =
      std::make_pair(KeyType::SECRET, (size_t)resultingKey->index);
  keys.push_back(key);

  pass.feedback.statistics.push_back(Statistic{
      location,
      operation,
      keys,
      count,
  });

  return std::nullopt;
}

// #############
// TFHE.neg_glwe
// #############

static std::optional<StringError> on_enter(TFHE::NegGLWEOp &op,
                                           ExtractTFHEStatisticsPass &pass) {
  auto resultingKey = op.getType().getKey().getNormalized();

  auto location = locationOf(op);
  auto operation = PrimitiveOperation::ENCRYPTED_NEGATION;
  auto keys = std::vector<std::pair<KeyType, size_t>>();
  auto count = pass.iterations;

  std::pair<KeyType, size_t> key =
      std::make_pair(KeyType::SECRET, (size_t)resultingKey->index);
  keys.push_back(key);

  pass.feedback.statistics.push_back(Statistic{
      location,
      operation,
      keys,
      count,
  });

  return std::nullopt;
}

// #################
// TFHE.sub_int_glwe
// #################

static std::optional<StringError> on_enter(TFHE::SubGLWEIntOp &op,
                                           ExtractTFHEStatisticsPass &pass) {
  auto resultingKey = op.getType().getKey().getNormalized();

  auto location = locationOf(op);
  auto keys = std::vector<std::pair<KeyType, size_t>>();
  auto count = pass.iterations;

  std::pair<KeyType, size_t> key =
      std::make_pair(KeyType::SECRET, (size_t)resultingKey->index);
  keys.push_back(key);

  // clear - encrypted = clear + neg(encrypted)

  auto operation = PrimitiveOperation::ENCRYPTED_NEGATION;
  pass.feedback.statistics.push_back(Statistic{
      location,
      operation,
      keys,
      count,
  });

  operation = PrimitiveOperation::CLEAR_ADDITION;
  pass.feedback.statistics.push_back(Statistic{
      location,
      operation,
      keys,
      count,
  });

  return std::nullopt;
}

// #################
// TFHE.wop_pbs_glwe
// #################

static std::optional<StringError> on_enter(TFHE::WopPBSGLWEOp &op,
                                           ExtractTFHEStatisticsPass &pass) {
  auto bsk = op.getBsk();
  auto ksk = op.getKsk();
  auto pksk = op.getPksk();

  auto location = locationOf(op);
  auto operation = PrimitiveOperation::WOP_PBS;
  auto keys = std::vector<std::pair<KeyType, size_t>>();
  auto count = pass.iterations;

  std::pair<KeyType, size_t> key =
      std::make_pair(KeyType::BOOTSTRAP, (size_t)bsk.getIndex());
  keys.push_back(key);

  key = std::make_pair(KeyType::KEY_SWITCH, (size_t)ksk.getIndex());
  keys.push_back(key);

  key = std::make_pair(KeyType::PACKING_KEY_SWITCH, (size_t)pksk.getIndex());
  keys.push_back(key);

  pass.feedback.statistics.push_back(Statistic{
      location,
      operation,
      keys,
      count,
  });

  return std::nullopt;
}

// ########
// Dispatch
// ########

#define DISPATCH_ENTER(type)                                                   \
  if (auto typedOp = llvm::dyn_cast<type>(op)) {                               \
    std::optional<StringError> error = on_enter(typedOp, *this);               \
    if (error.has_value()) {                                                   \
      return error;                                                            \
    }                                                                          \
  }

#define DISPATCH_EXIT(type)                                                    \
  if (auto typedOp = llvm::dyn_cast<type>(op)) {                               \
    std::optional<StringError> error = on_exit(typedOp, *this);                \
    if (error.has_value()) {                                                   \
      return error;                                                            \
    }                                                                          \
  }

std::optional<StringError>
ExtractTFHEStatisticsPass::enter(mlir::Operation *op) {
  DISPATCH_ENTER(scf::ForOp)
  DISPATCH_ENTER(TFHE::AddGLWEOp)
  DISPATCH_ENTER(TFHE::AddGLWEIntOp)
  DISPATCH_ENTER(TFHE::BootstrapGLWEOp)
  DISPATCH_ENTER(TFHE::KeySwitchGLWEOp)
  DISPATCH_ENTER(TFHE::MulGLWEIntOp)
  DISPATCH_ENTER(TFHE::NegGLWEOp)
  DISPATCH_ENTER(TFHE::SubGLWEIntOp)
  DISPATCH_ENTER(TFHE::WopPBSGLWEOp)
  return std::nullopt;
}

std::optional<StringError>
ExtractTFHEStatisticsPass::exit(mlir::Operation *op) {
  DISPATCH_EXIT(scf::ForOp)
  return std::nullopt;
}
