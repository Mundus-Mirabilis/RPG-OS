// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file errors.hpp
 * @ingroup rpg_os_core
 * @brief Bookkeeping error codes.
 *
 * The bookkeeping layer (inventory, equipment, currency, advancement, ...)
 * reports *expected* failures — "not enough money", "slot occupied", "item
 * not owned" — as typed error codes carried in a @c std::expected rather than
 * as exceptions. Programmer errors (a malformed ruleset, an unknown id passed
 * where one must exist) still throw @c std::invalid_argument; these codes are
 * for the everyday state-machine outcomes an application has to handle and
 * display.
 */
#pragma once

namespace rpg_os {

/// Error codes for fallible bookkeeping operations.
///
/// @par Why an enum and not a message string?
/// An application needs to *branch* on the reason an operation failed (show
/// "not enough money" vs. "slot occupied"), and it needs the reason to be
/// stable across versions. A fixed enum gives it that; free-form strings
/// invite brittle matching. Messages can still be rendered from the code.
enum class BookkeepingError {
  None,                ///< no error (success)
  UnknownItem,         ///< no item record with that id
  UnknownContainer,    ///< no (single-unit) container with that id
  UnknownCurrency,     ///< the ruleset defines no currency system
  UnknownDenomination, ///< an unknown coin / denomination id
  UnknownCondition,    ///< no condition record with that id
  UnknownSpell,        ///< no spell record with that id
  UnknownCostTable,    ///< no cost table with that id
  UnknownMode,         ///< no movement mode with that id
  UnknownTerrain,      ///< no terrain with that id
  NotEnoughMoney,      ///< the payer lacks the funds
  SlotOccupied,        ///< an item is already equipped in the slot
  SlotMismatch,        ///< the item cannot go into the requested slot
  ItemNotOwned,        ///< the sheet does not carry the item
  OverCapacity,        ///< the inventory is full
  NotEquipped,         ///< nothing is equipped in the slot
  SpellNotKnown,       ///< the spell is not in the spellbook
  SpellNotPrepared,    ///< the spell is not prepared for casting
  NoSpellSlot,         ///< no spell slot of the required level is free
  InsufficientXp,      ///< not enough experience / points for the advancement
  UnknownAffliction,   ///< no affliction record (poison / disease / curse)
  NoEncumbrance,       ///< the ruleset defines no encumbrance rules
  NoRuleset,           ///< an operation was attempted before a ruleset was loaded
};

} // namespace rpg_os
