// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file senses.hpp
 * @ingroup rpg_os_universal
 * @brief A creature's declared senses (stat-block perception).
 *
 * A bestiary entry may declare its senses as a structured `senses` object
 * mapping sense id -> range, plus an optional `passive_perception` integer.
 * The `Sense` value type is deliberately separate from `movement.hpp`: senses
 * are perception, not locomotion. The universal engine exposes them via
 * `DynamicEntity::senses` / `hasSense` / `findSense` / `passivePerception`,
 * and folds the sense ids into `DynamicEntity::capabilities()` so
 * `hasCapability("darkvision")` is true for a creature whose stat block lists
 * it.
 */
#pragma once

#include <string>

namespace rpg_os {

/// A single creature-declared sense (its stat-block senses). The `id` is a
/// short machine-readable id ("blindsight", "darkvision", "tremorsense",
/// "truesight", ...); `name` is the human-readable sense name; `range` is the
/// sense's range in the ruleset's distance unit (0 = no stated range, e.g.
/// "Blindsight 30 ft." is range 30 while a passive sense has none);
/// `note` preserves any qualifier the source attached (e.g. "unimpeded by
/// magical darkness").
struct Sense {
  std::string id;
  std::string name;
  double range{0.0};
  std::string note;
};

} // namespace rpg_os
