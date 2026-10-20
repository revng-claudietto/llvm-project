//=== llvm/unittest/ADT/EquivalenceClassesTest.cpp - the structure tests --===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/EquivalenceClasses.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace llvm {

TEST(EquivalenceClassesTest, NoMerges) {
  EquivalenceClasses<int> EqClasses;
  // Until we merged any sets, check that every element is only equivalent to
  // itself.
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      if (i == j)
        EXPECT_TRUE(EqClasses.isEquivalent(i, j));
      else
        EXPECT_FALSE(EqClasses.isEquivalent(i, j));
}

TEST(EquivalenceClassesTest, SimpleMerge1) {
  EquivalenceClasses<int> EqClasses;
  // Check that once we merge (A, B), (B, C), (C, D), then all elements belong
  // to one set.
  EqClasses.unionSets(0, 1);
  EqClasses.unionSets(1, 2);
  EqClasses.unionSets(2, 3);
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      EXPECT_TRUE(EqClasses.isEquivalent(i, j));
}

TEST(EquivalenceClassesTest, SimpleMerge2) {
  EquivalenceClasses<int> EqClasses;
  // Check that once we merge (A, B), (C, D), (A, C), then all elements belong
  // to one set.
  EqClasses.unionSets(0, 1);
  EqClasses.unionSets(2, 3);
  EqClasses.unionSets(0, 2);
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j)
      EXPECT_TRUE(EqClasses.isEquivalent(i, j));
}

TEST(EquivalenceClassesTest, TwoSets) {
  EquivalenceClasses<int> EqClasses;
  // Form sets of odd and even numbers, check that we split them into these
  // two sets correcrly.
  for (int i = 0; i < 30; i += 2)
    EqClasses.unionSets(0, i);
  for (int i = 1; i < 30; i += 2)
    EqClasses.unionSets(1, i);

  for (int i = 0; i < 30; i++)
    for (int j = 0; j < 30; j++)
      if (i % 2 == j % 2)
        EXPECT_TRUE(EqClasses.isEquivalent(i, j));
      else
        EXPECT_FALSE(EqClasses.isEquivalent(i, j));
}

TEST(EquivalenceClassesTest, MultipleSets) {
  EquivalenceClasses<int> EqClasses;
  // Split numbers from [0, 100) into sets so that values in the same set have
  // equal remainders (mod 17).
  for (int i = 0; i < 100; i++)
    EqClasses.unionSets(i % 17, i);

  for (int i = 0; i < 100; i++)
    for (int j = 0; j < 100; j++)
      if (i % 17 == j % 17)
        EXPECT_TRUE(EqClasses.isEquivalent(i, j));
      else
        EXPECT_FALSE(EqClasses.isEquivalent(i, j));
}

TEST(EquivalenceClassesTest, InsertionOrder) {
  EquivalenceClasses<int> EqClasses;
  for (int Value : {9, 1, 5, 1})
    EqClasses.insert(Value);
  SmallVector<int> Values;
  for (const auto *Entry : EqClasses)
    Values.push_back(Entry->getData());
  EXPECT_EQ(Values, (SmallVector<int>{9, 1, 5}));
}

TEST(EquivalenceClassesTest, StableNodesAfterGrowth) {
  EquivalenceClasses<int> EqClasses;
  const auto &First = EqClasses.insert(0);
  for (int Value = 1; Value < 4096; ++Value)
    EqClasses.unionSets(0, Value);
  EXPECT_EQ(EqClasses.findLeader(First), EqClasses.findLeader(4095));
  EXPECT_EQ(
      std::distance(EqClasses.member_begin(First), EqClasses.member_end()),
      4096);
}

TEST(EquivalenceClassesTest, CopyAndAssignment) {
  EquivalenceClasses<int> Original;
  Original.unionSets(1, 2);
  Original.insert(3);
  EquivalenceClasses<int> Copy(Original);
  Copy.unionSets(2, 3);
  EXPECT_FALSE(Original.isEquivalent(1, 3));
  EXPECT_TRUE(Copy.isEquivalent(1, 3));
  Copy = Original;
  EXPECT_TRUE(Copy.isEquivalent(1, 2));
  EXPECT_FALSE(Copy.isEquivalent(1, 3));
  EXPECT_EQ(Copy.getNumClasses(), 2u);
  auto &Alias = Copy;
  Copy = Alias;
  EXPECT_TRUE(Copy.isEquivalent(1, 2));
  EXPECT_EQ(Copy.getNumClasses(), 2u);
}

} // llvm
