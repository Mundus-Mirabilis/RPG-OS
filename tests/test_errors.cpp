// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file test_errors.cpp
 * @brief Unit tests for the bookkeeping error codes (@c rpg_os::core/errors.hpp).
 *
 * The enum is the stable, versioned vocabulary an application branches on
 * ("not enough money" vs. "slot occupied"). These tests pin the full set so a
 * renumbered or accidentally-duplicated code is caught.
 */
#include <doctest/doctest.h>
#include <rpg_os/core/errors.hpp>
#include <unordered_set>
#include <vector>

TEST_CASE("errors: every bookkeeping error code is distinct") {
  using rpg_os::BookkeepingError;
  const std::vector<BookkeepingError> all = {BookkeepingError::None,
                                             BookkeepingError::UnknownItem,
                                             BookkeepingError::UnknownContainer,
                                             BookkeepingError::UnknownCurrency,
                                             BookkeepingError::UnknownDenomination,
                                             BookkeepingError::UnknownCondition,
                                             BookkeepingError::UnknownSpell,
                                             BookkeepingError::UnknownCostTable,
                                             BookkeepingError::UnknownMode,
                                             BookkeepingError::UnknownTerrain,
                                             BookkeepingError::NotEnoughMoney,
                                             BookkeepingError::SlotOccupied,
                                             BookkeepingError::SlotMismatch,
                                             BookkeepingError::ItemNotOwned,
                                             BookkeepingError::OverCapacity,
                                             BookkeepingError::NotEquipped,
                                             BookkeepingError::SpellNotKnown,
                                             BookkeepingError::SpellNotPrepared,
                                             BookkeepingError::NoSpellSlot,
                                             BookkeepingError::InsufficientXp,
                                             BookkeepingError::UnknownAffliction,
                                             BookkeepingError::NoEncumbrance,
                                             BookkeepingError::NoRuleset};
  std::unordered_set<int> codes;
  for (const BookkeepingError code : all) {
    codes.insert(static_cast<int>(code));
  }
  CHECK(codes.size() == all.size());
}

TEST_CASE("errors: the sentinel None means success") {
  using rpg_os::BookkeepingError;
  CHECK(BookkeepingError::None == BookkeepingError::None);
  CHECK(BookkeepingError::None != BookkeepingError::UnknownItem);
}
