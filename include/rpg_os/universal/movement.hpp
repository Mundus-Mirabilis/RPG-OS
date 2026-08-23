// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file movement.hpp
 * @ingroup rpg_os_universal
 * @brief Terrain-aware movement and the surrounding's effect on a sheet.
 *
 * A ruleset may declare an optional `movement` section: named movement modes
 * (walk, swim, fly, climb, burrow, ...) with a base speed formula, and named
 * terrains / surroundings (land, water, snow, ...) that multiply each mode's
 * speed, may forbid a mode outright or require a capability, change how
 * exhausting the mode is, and decide whether the mover can rest / regenerate
 * here. The carried load (encumbrance) can slow every mode through a list of
 * ratio -> speed-factor steps. A ruleset without a `movement` section is
 * deliberately unconstrained: every mode is possible at its declared speed
 * with no exhaustion and full regeneration.
 */
#pragma once

#include <map>
#include <rpg_os/universal/expression.hpp>
#include <string>
#include <string_view>
#include <vector>

namespace rpg_os {

/// One movement mode a ruleset declares (walk, swim, fly, climb, burrow, ...).
struct MovementModeDef {
  std::string id;
  std::string name;
  std::string speedText;  ///< base speed: a stat id, a formula, or a constant
  Expression speed;       ///< parsed base speed
  double factor{1.0};     ///< the mode's own speed multiplier (climb = 0.5, ...)
  double exhaustion{0.0}; ///< base fatigue cost per distance unit
};

/// Per-mode behaviour of a movement mode inside one terrain.
struct TerrainModeRule {
  double speedFactor{1.0};        ///< multiplier on the mode's speed here
  double costFactor{1.0};         ///< multiplier on the mode's exhaustion here
  bool possible{true};            ///< whether the mode works here at all
  std::string requiresCapability; ///< capability the mover must have ("" = none)
};

/// One terrain / surrounding (land, water, snow, ...).
struct TerrainDef {
  std::string id;
  std::string name;
  std::string description;
  /// How resting works here: "normal" (default), "none" (no rest /
  /// regeneration), or "half" (only a half rest).
  std::string regeneration{"normal"};
  /// mode id -> rule; a mode without an entry uses the default (possible, at
  /// full speed, no extra exhaustion).
  std::map<std::string, TerrainModeRule> modes;
};

/// One speed-reduction step from the carried load (encumbrance).
struct LoadSpeedLevel {
  double maxRatio{1.0}; ///< carried/capacity ratio ceiling for this step
  double factor{1.0};   ///< speed multiplier while at or under this ratio
};

/// The ruleset's optional movement rules.
struct MovementConfig {
  std::map<std::string, MovementModeDef> modes;
  std::map<std::string, TerrainDef> terrains;
  std::string defaultTerrain;             ///< terrain used when none is set
  std::string exhaustionPool;             ///< resource pool movement drains ("" = none)
  double exhaustionPerDistance{1.0};      ///< base fatigue per distance unit
  std::vector<LoadSpeedLevel> loadLevels; ///< carried load -> speed factor
};

/// One possible movement for a sheet in a terrain.
struct MovementOption {
  std::string modeId;
  std::string modeName;
  double baseSpeed{0.0}; ///< the mode's declared base speed (0 = unconstrained)
  double speed{0.0};     ///< effective speed here (terrain + load)
  bool possible{true};
  std::string reason;            ///< why the mode is not possible
  double exhaustionPerUnit{0.0}; ///< fatigue per distance unit (terrain-adjusted)
  bool regeneration{true};       ///< whether resting/regenerating works here
};

/// The complete movement picture for a sheet in a terrain.
struct MovementStatus {
  std::string terrainId;
  bool loadReducesSpeed{false}; ///< the carried load slowed every mode
  std::vector<MovementOption> options;
};

/// The outcome of actually moving.
struct MovementOutcome {
  std::string modeId;
  std::string terrainId;
  double distance{0.0};
  double speed{0.0};
  double timeUnits{0.0};  ///< distance / speed (0 when speed is unconstrained)
  double exhaustion{0.0}; ///< total fatigue cost of this move
  bool possible{true};
  std::string reason;
};

/// Terrain-dependent behaviour of an item in a sheet's current terrain (from
/// the item record's `terrain` object).
struct TerrainItemEffect {
  bool unusable{false};            ///< the item cannot be used here
  bool ruined{false};              ///< the item is ruined by this terrain
  std::vector<std::string> grants; ///< capabilities the item grants here
};

/// A single creature-declared movement mode (its stat-block speed). The
/// `mode` id refers to a movement mode from the ruleset's `movement.modes`
/// (walk, swim, fly, ...); a creature without its own entry uses the mode's
/// default formula from the movement section.
struct MovementSpeed {
  double value{0.0}; ///< base speed in the ruleset's distance unit (0 = none)
  bool hover{false}; ///< e.g. a D&D fly speed "(hover)" — can stay aloft
};

} // namespace rpg_os
