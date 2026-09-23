//===- DemandedBitsTest.cpp - DemandedBits tests --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/DemandedBits.h"
#include "../Support/KnownBitsTest.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueLattice.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/SCCPSolver.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

template <typename Fn1, typename Fn2>
static void TestBinOpExhaustive(Fn1 PropagateFn, Fn2 EvalFn) {
  unsigned Bits = 4;
  unsigned Max = 1 << Bits;
  ForeachKnownBits(Bits, [&](const KnownBits &Known1) {
    ForeachKnownBits(Bits, [&](const KnownBits &Known2) {
      for (unsigned AOut_ = 0; AOut_ < Max; AOut_++) {
        APInt AOut(Bits, AOut_);
        APInt AB1 = PropagateFn(0, AOut, Known1, Known2);
        APInt AB2 = PropagateFn(1, AOut, Known1, Known2);
        {
          // If the propagator claims that certain known bits
          // didn't matter, check it doesn't change its mind
          // when they become unknown.
          KnownBits Known1Redacted;
          KnownBits Known2Redacted;
          Known1Redacted.Zero = Known1.Zero & AB1;
          Known1Redacted.One = Known1.One & AB1;
          Known2Redacted.Zero = Known2.Zero & AB2;
          Known2Redacted.One = Known2.One & AB2;

          APInt AB1R = PropagateFn(0, AOut, Known1Redacted, Known2Redacted);
          APInt AB2R = PropagateFn(1, AOut, Known1Redacted, Known2Redacted);
          EXPECT_EQ(AB1, AB1R);
          EXPECT_EQ(AB2, AB2R);
        }
        ForeachNumInKnownBits(Known1, [&](APInt Value1) {
          ForeachNumInKnownBits(Known2, [&](APInt Value2) {
            APInt ReferenceResult = EvalFn((Value1 & AB1), (Value2 & AB2));
            APInt Result = EvalFn(Value1, Value2);
            EXPECT_EQ(Result & AOut, ReferenceResult & AOut);
          });
        });
      }
    });
  });
}

TEST(DemandedBitsTest, Add) {
  TestBinOpExhaustive(DemandedBits::determineLiveOperandBitsAdd,
                      [](APInt N1, APInt N2) -> APInt { return N1 + N2; });
}

TEST(DemandedBitsTest, Sub) {
  TestBinOpExhaustive(DemandedBits::determineLiveOperandBitsSub,
                      [](APInt N1, APInt N2) -> APInt { return N1 - N2; });
}

class DemandedBitsRangeTest : public testing::Test {
protected:
  LLVMContext Context;
  std::unique_ptr<Module> M;
  Function *F = nullptr;
  std::unique_ptr<AssumptionCache> AC;
  DominatorTree DT;

  void parseAssembly(StringRef IR) {
    SMDiagnostic Error;
    M = parseAssemblyString(IR, Error, Context);
    std::string Message;
    raw_string_ostream Stream(Message);
    Error.print("DemandedBitsRangeTest", Stream);
    ASSERT_TRUE(M) << Stream.str();
    F = M->getFunction("f");
    ASSERT_TRUE(F);
    AC = std::make_unique<AssumptionCache>(*F);
    DT.recalculate(*F);
  }

  Instruction *instruction(StringRef Name) {
    for (Instruction &I : instructions(*F))
      if (I.getName() == Name)
        return &I;
    llvm_unreachable("Missing test instruction");
  }

  Use *operand(StringRef Name, unsigned Index) {
    return &instruction(Name)->getOperandUse(Index);
  }

  DemandedBits analyze(DemandedBits::RangeQuery GetRange = {}) {
    return DemandedBits(*F, *AC, DT, std::move(GetRange));
  }

  // Supply a range only for the selected operand occurrence.
  static DemandedBits::RangeQuery rangeFor(Use *Operand, ConstantRange Range) {
    return [Operand, Range](const Use &U) {
      return &U == Operand
                 ? Range
                 : ConstantRange::getFull(U->getType()->getScalarSizeInBits());
    };
  }
};

// An independent reference: enumerate each shift amount and union its demand,
// rather than reproducing the analysis's logarithmic interval propagation.
static APInt referenceShiftDemand(const BinaryOperator &Shift, APInt Output,
                                  unsigned Min, unsigned Max) {
  unsigned Width = Output.getBitWidth();
  APInt Expected(Width, 0);
  for (unsigned Amount = Min; Amount <= Max; ++Amount) {
    APInt Bits = Output;
    if (Shift.getOpcode() == Instruction::Shl) {
      Bits = Output.lshr(Amount);
      if (Shift.hasNoSignedWrap())
        Bits |= APInt::getHighBitsSet(Width, Amount + 1);
      if (Shift.hasNoUnsignedWrap())
        Bits |= APInt::getHighBitsSet(Width, Amount);
    } else {
      Bits = Output.shl(Amount);
      if (Shift.isExact())
        Bits |= APInt::getLowBitsSet(Width, Amount);
      if (Shift.getOpcode() == Instruction::AShr &&
          (Output & APInt::getHighBitsSet(Width, Amount)).getBoolValue())
        Bits.setSignBit();
    }
    Expected |= Bits;
  }
  return Expected;
}

TEST_F(DemandedBitsRangeTest, ShiftIntervals) {
  // Include non-power-of-two intervals and sparse demanded output masks.
  for (StringRef Opcode : {"shl", "shl nuw", "shl nsw", "lshr", "lshr exact",
                           "ashr", "ashr exact"}) {
    for (unsigned Mask : {1U, 3U, 0x55U, 0x80U, 0xffU}) {
      ASSERT_NO_FATAL_FAILURE(parseAssembly(
          (Twine("define i8 @f(i8 %x, i8 %amount) {\n %shift = ") + Opcode +
           " i8 %x, %amount\n %masked = and i8 %shift, " + Twine(Mask) +
           "\n ret i8 %masked\n}")
              .str()));
      auto *Shift = cast<BinaryOperator>(instruction("shift"));
      for (unsigned Min = 0; Min < 8; ++Min) {
        for (unsigned Max = Min; Max < 8; ++Max) {
          SCOPED_TRACE(Opcode.str() + " " + std::to_string(Mask) + " [" +
                       std::to_string(Min) + "," + std::to_string(Max) + "]");
          auto Range = ConstantRange(APInt(8, Min), APInt(8, Max + 1));
          auto DB = analyze(rangeFor(operand("shift", 1), Range));
          EXPECT_EQ(DB.getDemandedBits(operand("shift", 0)),
                    referenceShiftDemand(*Shift, APInt(8, Mask), Min, Max));
        }
      }
    }
  }
}

// The same IR is used to check interval endpoints, caching and fallbacks.
static constexpr StringLiteral ShiftIR = R"(
  define i8 @f(i64 %x, i64 %amount) {
    %shift = lshr i64 %x, %amount
    %low = trunc i64 %shift to i8
    ret i8 %low
  }
)";

TEST_F(DemandedBitsRangeTest, NonPowerOfTwoInterval) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(ShiftIR));
  auto Range = ConstantRange(APInt(64, 0), APInt(64, 6));
  auto DB = analyze(rangeFor(operand("shift", 1), Range));
  // Output bits 0..7 shifted right by 0..5 demand input bits 0..12.
  EXPECT_EQ(DB.getDemandedBits(operand("shift", 0)),
            APInt::getLowBitsSet(64, 13));
}

TEST_F(DemandedBitsRangeTest, CachesOperandRanges) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(ShiftIR));
  auto Range = ConstantRange(APInt(64, 0), APInt(64, 6));
  auto GetRange = rangeFor(operand("shift", 1), Range);
  unsigned Queries = 0;
  auto DB = analyze([&](const Use &U) {
    ++Queries;
    return GetRange(U);
  });
  APInt First = DB.getDemandedBits(operand("shift", 0));
  EXPECT_EQ(Queries, 1U);
  EXPECT_EQ(DB.getDemandedBits(operand("shift", 0)), First);
  EXPECT_EQ(Queries, 1U);
}

TEST_F(DemandedBitsRangeTest, Fallbacks) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(ShiftIR));
  auto Original = analyze();
  APInt Expected = Original.getDemandedBits(operand("shift", 0));
  EXPECT_EQ(Expected, APInt::getAllOnes(64));
  // Unknown, empty and possibly out-of-bounds amounts remain conservative.
  for (ConstantRange Range :
       {ConstantRange::getFull(64), ConstantRange::getEmpty(64),
        ConstantRange(APInt(64, 0), APInt(64, 65))}) {
    SCOPED_TRACE(Range);
    auto DB = analyze(rangeFor(operand("shift", 1), Range));
    EXPECT_EQ(DB.getDemandedBits(operand("shift", 0)), Expected);
  }
}

TEST_F(DemandedBitsRangeTest, ConflictingKnownBits) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(R"(
    define i8 @f(i64 %x, i64 %amount) {
      %bounded = and i64 %amount, 7
      %shift = lshr i64 %x, %bounded
      %low = trunc i64 %shift to i8
      ret i8 %low
    }
  )"));
  auto Range = ConstantRange(APInt(64, 8), APInt(64, 16));
  auto DB = analyze(rangeFor(operand("shift", 1), Range));
  // Ignore the contradictory range: the mask still proves amounts 0..7.
  EXPECT_EQ(DB.getDemandedBits(operand("shift", 0)),
            APInt::getLowBitsSet(64, 15));
}

TEST_F(DemandedBitsRangeTest, DisjointRangeWithoutConflictingBits) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(R"(
    define i8 @f(i64 %x, i64 %amount) {
      %zero = and i64 %amount, 0
      %five = or i64 %zero, 5
      %shift = lshr i64 %x, %five
      %low = trunc i64 %shift to i8
      ret i8 %low
    }
  )"));
  auto Range = ConstantRange(APInt(64, 6), APInt(64, 9));
  auto DB = analyze(rangeFor(operand("shift", 1), Range));
  // [6, 9) fixes none of the low four bits, so merging known bits does not
  // reveal the contradiction. The empty intersection must still fall back
  // to ValueTracking's constant amount of five.
  EXPECT_EQ(DB.getDemandedBits(operand("shift", 0)),
            APInt::getBitsSet(64, 5, 13));
}

TEST_F(DemandedBitsRangeTest, PreservesKnownBitsInsideRange) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(R"(
    define i8 @f(i8 %x, i8 %input) {
      %mask = and i8 %input, 253
      %masked = and i8 %x, %mask
      ret i8 %masked
    }
  )"));
  auto Range = ConstantRange(APInt(8, 0), APInt(8, 128));
  auto DB = analyze(rangeFor(operand("masked", 1), Range));
  // The range clears bit 7, while ValueTracking clears bit 1. Converting the
  // combined information to an interval and back would forget bit 1.
  EXPECT_EQ(DB.getDemandedBits(operand("masked", 0)), APInt(8, 0x7d));
}

TEST_F(DemandedBitsRangeTest, KnownBitsTransfers) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(R"(
    define i64 @f(i64 %x, i64 %mask) {
      %value = add i64 %x, 1
      %masked = and i64 %value, %mask
      ret i64 %masked
    }
  )"));
  auto Range = ConstantRange(APInt(64, 0), APInt(64, 256));
  auto DB = analyze(rangeFor(operand("masked", 1), Range));
  // The mask can only retain the low byte of the addition.
  EXPECT_EQ(DB.getDemandedBits(instruction("value")),
            APInt::getLowBitsSet(64, 8));
}

TEST_F(DemandedBitsRangeTest, UsesStaySeparate) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(R"(
    define i8 @f(i64 %x, i64 %y, i64 %amount, i1 %c) {
    entry:
      %left_value = add i64 %x, 1
      %right_value = add i64 %y, 1
      br i1 %c, label %left, label %right
    left:
      %left_shift = lshr i64 %left_value, %amount
      %left_low = trunc i64 %left_shift to i8
      ret i8 %left_low
    right:
      %right_shift = lshr i64 %right_value, %amount
      %right_low = trunc i64 %right_shift to i8
      ret i8 %right_low
    }
  )"));
  auto Range = ConstantRange(APInt(64, 0), APInt(64, 8));
  auto DB = analyze(rangeFor(operand("left_shift", 1), Range));
  // Only the left use has bounded amounts; the right use still needs 64 bits.
  EXPECT_EQ(DB.getDemandedBits(instruction("left_value")),
            APInt::getLowBitsSet(64, 15));
  EXPECT_EQ(DB.getDemandedBits(instruction("right_value")),
            APInt::getAllOnes(64));
}

TEST_F(DemandedBitsRangeTest, SCCPLoop) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(R"(
    declare i1 @again()
    define i8 @f(i64 %x, i64 %y, i3 %seed, i3 %mask) {
    entry:
      %value = add i64 %x, %y
      %start = zext i3 %seed to i64
      %wide_mask = zext i3 %mask to i64
      br label %loop
    loop:
      %amount = phi i64 [ %start, %entry ], [ %next, %loop ]
      %next = xor i64 %amount, %wide_mask
      %shift = lshr i64 %value, %amount
      %low = trunc i64 %shift to i8
      %continue = call i1 @again()
      br i1 %continue, label %loop, label %exit
    exit:
      ret i8 %low
    }
  )"));
  TargetLibraryInfoImpl Impl;
  TargetLibraryInfo TLI(Impl);
  SCCPSolver Solver(
      M->getDataLayout(),
      [&TLI](Function &) -> const TargetLibraryInfo & { return TLI; }, Context);
  Solver.markBlockExecutable(&F->getEntryBlock());
  for (Argument &A : F->args())
    Solver.markOverdefined(&A);
  do {
    Solver.solve();
  } while (Solver.resolvedUndefsIn(*F));

  // SCCP preserves the [0, 8) invariant through the XOR backedge, without
  // needing a branch condition or rewriting the IR.
  auto DB = analyze([&Solver](const Use &U) {
    if (auto *C = dyn_cast<ConstantInt>(U))
      return ConstantRange(C->getValue());
    const ValueLatticeElement &State = Solver.getLatticeValueFor(U.get());
    return State.isConstantRange(/*UndefAllowed=*/false)
               ? State.getConstantRange(/*UndefAllowed=*/false)
               : ConstantRange::getFull(U->getType()->getIntegerBitWidth());
  });
  // Eight output bits plus seven possible shift positions require 15 bits.
  EXPECT_EQ(DB.getDemandedBits(instruction("value")),
            APInt::getLowBitsSet(64, 15));
}

TEST_F(DemandedBitsRangeTest, SkipsUndefAndPoison) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(ShiftIR));
  Use *Amount = operand("shift", 1);
  Value *Unknowns[] = {UndefValue::get(Amount->get()->getType()),
                       PoisonValue::get(Amount->get()->getType())};
  for (Value *Unknown : Unknowns) {
    SCOPED_TRACE(isa<PoisonValue>(Unknown) ? "poison" : "undef");
    Amount->set(Unknown);
    auto DB = analyze([](const Use &U) {
      ADD_FAILURE() << "Must not query an undef or poison operand";
      return ConstantRange::getFull(U->getType()->getIntegerBitWidth());
    });
    EXPECT_EQ(DB.getDemandedBits(operand("shift", 0)), APInt::getAllOnes(64));
  }
}

TEST_F(DemandedBitsRangeTest, SkipsVectors) {
  ASSERT_NO_FATAL_FAILURE(parseAssembly(R"(
    define <2 x i8> @f(<2 x i64> %x, <2 x i64> %amount) {
      %shift = lshr <2 x i64> %x, %amount
      %low = trunc <2 x i64> %shift to <2 x i8>
      ret <2 x i8> %low
    }
  )"));
  auto DB = analyze([](const Use &U) {
    ADD_FAILURE() << "Must not query a vector operand";
    return ConstantRange::getFull(U->getType()->getScalarSizeInBits());
  });
  EXPECT_EQ(DB.getDemandedBits(operand("shift", 0)), APInt::getAllOnes(64));
}

} // anonymous namespace
