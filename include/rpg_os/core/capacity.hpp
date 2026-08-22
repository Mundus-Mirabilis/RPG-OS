// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file capacity.hpp
 * @ingroup rpg_os_core
 * @brief Carrying capacity: weight, size, and item-count limits.
 */
#pragma once

#include <cstdint>

namespace rpg_os {

/// How much a sheet currently carries across the three capacity axes.
///
/// @par Why three axes?
/// Rule systems limit what a hero can carry in different ways: weight (D&D's
/// 15 x STR pounds), volume / size (a backpack holds so many cubic feet or
/// "slots"), and plain item count (a quiver holds N arrows). The engine
/// tracks all three so a ruleset can use whichever combination its rules
/// declare — and a ruleset with no carrying rules simply has every limit off.
struct CarriedLoad {
  double weight{0.0}; ///< total weight in the ruleset's weight unit
  double size{0.0};   ///< total size in the ruleset's size unit
  int32_t items{0};   ///< total item count (sum of quantities, incl. worn)
};

/// A sheet's (or a container's) carrying limits. A field of 0 means that axis
/// has no limit; @ref enabled is false when the ruleset (or item record)
/// declares no carrying rule at all.
struct CarryingCapacity {
  double weight{0.0};  ///< weight limit (0 = unlimited)
  double size{0.0};    ///< size limit (0 = unlimited)
  int32_t items{0};    ///< item-count limit (0 = unlimited)
  bool enabled{false}; ///< whether any axis is limited
};

/// The carry status of a sheet: what it carries, its limits, and whether any
/// axis is at or over its limit. An application uses this to answer "can this
/// character still pick up a sword?" before calling the mutating inventory
/// operations, and to show the current encumbrance state to the player.
struct InventoryStatus {
  CarriedLoad carried;       ///< what the sheet currently carries
  CarryingCapacity capacity; ///< the sheet's limits (0 axis = unlimited)
  bool overWeight{false};    ///< carried.weight exceeds the weight limit
  bool overSize{false};      ///< carried.size exceeds the size limit
  bool overItems{false};     ///< carried.items exceeds the item-count limit
  bool atWeightLimit{false}; ///< weight at or above the limit (no more room)
  bool atSizeLimit{false};   ///< size at or above the limit (no more room)
  bool atItemsLimit{false};  ///< item count at or above the limit
  /// Whether any limited axis is at or over its limit — the inventory can
  /// take no more of that axis.
  [[nodiscard]] bool full() const noexcept {
    return atWeightLimit || atSizeLimit || atItemsLimit;
  }
};

} // namespace rpg_os
