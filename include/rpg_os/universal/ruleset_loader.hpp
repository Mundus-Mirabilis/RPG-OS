// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file ruleset_loader.hpp
 * @ingroup rpg_os_universal
 * @brief Ruleset loading and validation (universal / dynamic mode).
 *
 * Parses a ruleset JSON document into a @c Ruleset model: the schema
 * (attributes, derived stats, resources, skills, check types, cost tables,
 * equipment slots, event triggers) plus the raw @c data section (archetypes,
 * items, creatures) which stays as JSON and is read at runtime.
 *
 * @par Why keep the data section as raw JSON?
 * The schema describes *how to compute*; the data section is a database that
 * the engine reads per-entity (a monster's hit-point dice, an item's price).
 * Keeping it as JSON means the engine never materialises the whole bestiary
 * as C++ objects at load time, and new data needs no code change — and the
 * generated specific-mode code can parse the same section through its own
 * strongly typed loaders, which is exactly what the cross-mode parity tests
 * rely on.
 *
 * @par Why validate so aggressively at load time?
 * A ruleset is a data contract: a typo in an attribute id, a formula that
 * references a missing stat, or a cyclic derived-stat chain would otherwise
 * surface mid-session as a confusing runtime error. Validating everything up
 * front (duplicate ids, parseable formulas, resolvable stat references,
 * acyclic derived-stat graphs) turns a bad ruleset into a clear, immediate
 * @c std::invalid_argument at load — and lets the code generator assume the
 * schema is sound.
 */
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <rpg_os/common/event_system.hpp>
#include <rpg_os/common/json.hpp>
#include <rpg_os/core/checks.hpp>
#include <rpg_os/core/cost_table.hpp>
#include <rpg_os/core/money.hpp>
#include <rpg_os/universal/expression.hpp>
#include <rpg_os/universal/movement.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rpg_os {

/**
 * A core attribute definition from a ruleset.
 *
 * @par Why keep min/max/default on the definition?
 * Attributes are the raw inputs to everything else, and their legal band is a
 * ruleset-level decision (The Dark Eye allows 1–30, D&D 5e scores vary).
 * Storing the band lets validation and any UI/clamp logic agree with the
 * source book without hard-coding a game system into the engine.
 */
struct AttributeDef {
  std::string id;
  std::string name;
  int32_t minValue{1};
  int32_t maxValue{30};
  int32_t defaultValue{10};
};

/// A derived stat: an attribute (or other stat) computed from a formula.
///
/// @par Why store both the raw text and the parsed form?
/// The text is kept for diagnostics and for the code generator (which compiles
/// the formula to C++); the parsed @c Expression is what the universal engine
/// evaluates at runtime. Keeping both avoids re-parsing on every evaluation.
struct DerivedStatDef {
  std::string id;
  std::string name;
  std::string formulaText;
  Expression expression; ///< parsed formula
};

/// A resource pool definition (e.g. Hit Points, Astral Energy).
///
/// @par Why is the maximum a *stat id* rather than a number?
/// In the source games a pool's size is itself derived (TDE: LP = 5 + 2*CON;
/// D&D: HP depends on level and CON). Referencing a stat id defers the actual
/// computation to the entity, so the pool maximum always reflects the current
/// stats — including after a CON-draining effect.
struct ResourcePoolDef {
  std::string id;
  std::string name;
  std::string maxStat; ///< stat id that provides the pool maximum
  int32_t minValue{0};
};

/// A skill / talent definition (e.g. a DSA talent with linked attributes).
///
/// @par Why do skills carry their own attribute list?
/// A DSA talent is checked against *its own* three attributes (Climbing rolls
/// COU/AGI/STR, Dancing DEX/AGI/COU). Keeping the list on the skill lets the
/// engine resolve a skill check generically from the definition — and lets
/// the code generator emit a named check method per skill with the right
/// attributes baked in.
struct SkillDef {
  std::string id;
  std::string name;
  std::vector<std::string> attributes;
  int32_t defaultValue{0};
};

/// A named check type: an id plus the recipe for the generic resolver.
///
/// @par Why name-check-types at all?
/// Rulesets reference checks by name (e.g. "dnd5e_attack_melee",
/// "tde_attack"). Mapping a name to a @ref CheckRecipe lets the universal
/// engine resolve checks without hard-coding any game — and gives the code
/// generator the exact recipe it needs to emit named, compiled check methods.
struct CheckTypeDef {
  std::string id;
  CheckRecipe recipe;
};

/// A named cost / progression table.
struct CostTableDef {
  std::string id;
  CostTable table;
};

/// One action inside an event trigger.
///
/// @par Why one struct with all fields instead of per-type structs?
/// The action types are few ("modify_event_damage", "consume_resource",
/// "apply_condition", plus the bookkeeping actions "gain_item",
/// "remove_item", "gain_currency", "spend_currency", "gain_xp") and each
/// uses only a couple of fields. One struct with the union of fields keeps
/// the loader, the engine's executor, and the code generator reading from the
/// same shape — at the cost of a few unused fields per action, which is
/// negligible for ruleset-sized data.
struct EventActionDef {
  std::string type;        ///< the action kind (see the file doc comment)
  std::string resource;    ///< consume_resource: which resource
  int32_t amount{0};       ///< amount: consumed/currency/XP amount, or item quantity
  std::string item;        ///< gain_item / remove_item: the item id
  std::string formulaText; ///< modify_event_damage: damage expression
  Expression formula;      ///< parsed damage expression
  std::string condition;   ///< apply_condition: condition id
  std::string stacksText;  ///< apply_condition: stack count expression
  Expression stacks;       ///< parsed stack count expression
};

/// A rules-level reactive effect, e.g. the DSA wound check.
///
/// @par Why are triggers data, not C++?
/// Reactive rules vary per system (a wound check is TDE; D&D has no such
/// thing). Describing them as JSON-driven triggers means adding a new system
/// with new reactions requires only JSON — the engine's executor stays
/// system-agnostic.
struct EventTriggerDef {
  std::string id;
  EventType trigger{EventType::OnDamageTaken};
  std::string conditionText; ///< empty = always fires
  Expression condition;
  std::vector<EventActionDef> actions;
};

/// Optional carrying / encumbrance rules.
///
/// @par Why a capacity *formula* and ratio *levels*?
/// Systems differ wildly in how much a hero can carry (D&D: 15 × STR pounds;
/// BRP: encumbrance points; TDE: abstract). A formula keeps the limit
/// data-driven, and a list of ratio ceilings (carried ÷ capacity) with an
/// attached condition is the generic shape of "encumbered at 50%, heavily
/// encumbered at 100%". A ruleset without an `encumbrance` section simply has
/// @ref enabled == false and the engine reports no carrying limit.
///
/// @par Three capacity axes
/// Weight is the classic limit, but a rule system may also constrain what a
/// hero carries by *size* (volume / slots — a backpack holds so many cubic
/// feet) or by plain *item count* (a quiver holds N arrows). All three are
/// optional formulas; an absent axis has no limit.
struct EncumbranceConfig {
  bool enabled{false};
  std::string weightUnit{"lb"};  ///< unit used by item `weight` fields
  double defaultItemWeight{0.0}; ///< fallback when an item carries no weight
  std::string capacityText;      ///< weight capacity formula; empty = no limit
  Expression capacity;           ///< parsed weight capacity formula
  std::string sizeUnit{"size"};  ///< unit used by item `size` fields
  double defaultItemSize{0.0};   ///< fallback when an item carries no size
  std::string sizeCapacityText;  ///< size capacity formula; empty = no limit
  Expression sizeCapacity;       ///< parsed size capacity formula
  std::string itemCapacityText;  ///< item-count capacity formula; empty = no limit
  Expression itemCapacity;       ///< parsed item-count capacity formula
  struct Level {
    double maxRatio{1.0};    ///< carried/capacity ceiling for this level
    std::string conditionId; ///< condition applied at this level ("" = none)
  };
  std::vector<Level> levels;
};

/// Optional spell-casting configuration.
///
/// @par Why two styles?
/// Pool-based systems (TDE's AE, BRP's PP) charge a per-spell cost drawn from
/// a resource pool; vancian systems (D&D) limit how many spells of each level
/// can be cast per day. The engine already handles the pool cost; this config
/// adds the vancian slot schedule so both are data-driven.
struct SpellcastingConfig {
  std::string style{"pool"};        ///< "pool" or "slots"
  std::map<int32_t, int32_t> slots; ///< level -> spell slots per day (vancian)
};

/**
 * The fully loaded model of one ruleset.
 *
 * @par Why a plain data class instead of hidden state?
 * A @ref Ruleset is a pure value: everything the engine or the code generator
 * needs is publicly readable. Making it a plain aggregate avoids an accessor
 * layer that would add nothing, and lets the universal engine, the loader, and
 * the code generator share it without ceremony.
 *
 * @par Lookup methods return raw pointers instead of optional/iterators
 * The lookup set is small and linear scans are fine at load/validation time;
 * returning `nullptr` for "absent" keeps call sites terse and lets the engine
 * distinguish "known, value 0" from "unknown".
 */
class Ruleset {
public:
  std::string id;
  std::string name;
  std::string source;
  std::string licence;       ///< licence governing the ruleset content (required)
  std::string licenceSource; ///< optional URL where the rights holder states the licence
  std::string licenceNotice; ///< optional verbatim notice the licence requires (e.g. ORC Notice)
  std::string attribution;   ///< optional attribution/credit statement the licence requires
  std::string comment;       ///< optional free-form note for the ruleset author
  std::string cppNamespace;  ///< namespace used by the code generator
  std::string
      spellResource; ///< resource pool that spell casting draws its cost from (empty = none)
  int32_t schemaVersion{0};

  std::vector<AttributeDef> attributes;
  std::vector<DerivedStatDef> derivedStats;
  std::vector<ResourcePoolDef> resourcePools;
  std::vector<SkillDef> skills;
  std::vector<CheckTypeDef> checkTypes;
  std::vector<CostTableDef> costTables;
  std::vector<std::string> equipmentSlots;
  std::vector<EventTriggerDef> eventTriggers;

  /// The ruleset's coinage; an empty `id` means it has no money.
  CurrencySystem currencySystem;
  /// Optional carrying / encumbrance rules.
  EncumbranceConfig encumbrance;
  /// Optional spell-casting configuration (vancian slots).
  SpellcastingConfig spellcasting;
  /// Optional terrain-aware movement rules (see movement.hpp).
  MovementConfig movement;

  /// The raw `data` section (archetypes, items, creatures) as JSON.
  Json data{};

  /// Finds an attribute by id; returns nullptr when absent.
  [[nodiscard]] const AttributeDef *findAttribute(std::string_view statId) const;

  /// Finds a derived stat by id; returns nullptr when absent.
  [[nodiscard]] const DerivedStatDef *findDerivedStat(std::string_view statId) const;

  /// Finds a check type by id; returns nullptr when absent.
  [[nodiscard]] const CheckTypeDef *findCheckType(std::string_view statId) const;

  /// Finds a skill by id; returns nullptr when absent.
  [[nodiscard]] const SkillDef *findSkill(std::string_view statId) const;

  /// Finds a cost table by id; returns nullptr when absent.
  [[nodiscard]] const CostTableDef *findCostTable(std::string_view statId) const;

  /// Whether `statId` names a known stat (attribute, derived stat, or skill).
  /// Skills are counted here because the pool of a talent check is a stat the
  /// resolver must be able to read.
  [[nodiscard]] bool hasStat(std::string_view statId) const;

  /// Whether `poolId` names one of the ruleset's resource pools.
  [[nodiscard]] bool isResourcePool(std::string_view poolId) const;

  /// Whether `conditionId` names a condition in the ruleset's `data.conditions`.
  [[nodiscard]] bool isCondition(std::string_view conditionId) const;

  /// Whether the ruleset declares a currency system (i.e. has money).
  [[nodiscard]] bool hasCurrency() const noexcept {
    return !currencySystem.id.empty();
  }
};

inline const AttributeDef *Ruleset::findAttribute(std::string_view statId) const {
  for (const AttributeDef &def : attributes) {
    if (def.id == statId) {
      return &def;
    }
  }
  return nullptr;
}

inline const DerivedStatDef *Ruleset::findDerivedStat(std::string_view statId) const {
  for (const DerivedStatDef &def : derivedStats) {
    if (def.id == statId) {
      return &def;
    }
  }
  return nullptr;
}

inline const CheckTypeDef *Ruleset::findCheckType(std::string_view statId) const {
  for (const CheckTypeDef &def : checkTypes) {
    if (def.id == statId) {
      return &def;
    }
  }
  return nullptr;
}

inline const SkillDef *Ruleset::findSkill(std::string_view statId) const {
  for (const SkillDef &def : skills) {
    if (def.id == statId) {
      return &def;
    }
  }
  return nullptr;
}

inline const CostTableDef *Ruleset::findCostTable(std::string_view statId) const {
  for (const CostTableDef &def : costTables) {
    if (def.id == statId) {
      return &def;
    }
  }
  return nullptr;
}

inline bool Ruleset::hasStat(std::string_view statId) const {
  if (findAttribute(statId) != nullptr) {
    return true;
  }
  if (findDerivedStat(statId) != nullptr) {
    return true;
  }
  for (const SkillDef &def : skills) {
    if (def.id == statId) {
      return true;
    }
  }
  return false;
}

inline bool Ruleset::isResourcePool(std::string_view poolId) const {
  for (const ResourcePoolDef &def : resourcePools) {
    if (def.id == poolId) {
      return true;
    }
  }
  return false;
}

inline bool Ruleset::isCondition(std::string_view conditionId) const {
  if (!data.is_object() || !data.contains("conditions") || !data.at("conditions").is_array()) {
    return false;
  }
  for (const Json &condition : data.at("conditions")) {
    if (condition.value("id", "") == conditionId) {
      return true;
    }
  }
  return false;
}

/// Looks up a raw data record by id in a named `data` section (e.g. "spells",
/// "poisons", "diseases", "conditions", "items", "archetypes", "creatures").
/// Returns nullptr when the section or the record does not exist.
///
/// @par Why a free function on Ruleset?
/// The universal engine (@ref RulesetEngine) and a character sheet
/// (@ref DynamicEntity) both look up raw records by id; one scan keeps the two
/// halves of universal mode in agreement about which records exist.
[[nodiscard]] inline const Json *findDataRecord(const Ruleset &ruleset, std::string_view section,
                                                std::string_view id) {
  if (!ruleset.data.is_object()) {
    return nullptr;
  }
  const auto it = ruleset.data.find(std::string(section));
  if (it == ruleset.data.end() || !it->is_array()) {
    return nullptr;
  }
  for (const Json &record : *it) {
    if (record.value("id", "") == id) {
      return &record;
    }
  }
  return nullptr;
}

/// Reads a spell's resource cost exactly as @ref RulesetEngine::castSpell
/// spends it: the `cost` field, else `ae_cost`, else `level` (one point per
/// spell level). Returns 0 when the record declares none of these.
///
/// @par Why a shared reader?
/// The affordability check in the combat simulator must agree with what
/// casting actually spends; one reader keeps the two in lockstep.
[[nodiscard]] inline int32_t spellCost(const Json &spell) {
  if (spell.contains("cost") && spell.at("cost").is_number_integer()) {
    return spell.at("cost").get<int32_t>();
  }
  if (spell.contains("ae_cost") && spell.at("ae_cost").is_number_integer()) {
    return spell.at("ae_cost").get<int32_t>();
  }
  if (spell.contains("level") && spell.at("level").is_number_integer()) {
    return spell.at("level").get<int32_t>();
  }
  return 0;
}

/// Finds the id of the ruleset's primary hit-point pool (the first resource
/// pool with a minimum of 0), e.g. "LP" for The Dark Eye and "HP" for D&D 5e.
///
/// @par Why "minimum of 0" as the heuristic?
/// The primary hit-point pool is the one a creature is reduced to 0 in to die;
/// in both shipped rulesets it is the pool whose minimum is 0 (LP, HP), while
/// secondary pools (Astral Energy, Karma) start above 0. The heuristic avoids
/// hard-coding pool names into the engine.
[[nodiscard]] inline std::string resolveHitPointPool(const Ruleset &ruleset) {
  for (const ResourcePoolDef &pool : ruleset.resourcePools) {
    if (pool.minValue == 0) {
      return pool.id;
    }
  }
  return {};
}

/**
 * Loads and validates a ruleset JSON document.
 *
 * @par Why a static-only class (namespace-like)?
 * Loading is a pure function of the JSON input: it has no state worth keeping
 * in an object. A static-only interface communicates that and keeps the API
 * trivially testable — every parse stage is independently checkable via the
 * validation errors it raises.
 */
class RulesetLoader {
public:
  /// Parses and validates `root`. Throws std::invalid_argument on any error.
  ///
  /// @par Why throw instead of returning a result?
  /// A ruleset that cannot be loaded is unrecoverable for the engine that
  /// hosts it; failing loudly with a descriptive message is clearer than
  /// threading an error code through every consumer.
  static Ruleset load(const Json &root);

  /// Parses and validates a JSON string (convenience wrapper over @ref load).
  static Ruleset loadFromString(std::string_view jsonContent);

  /// Re-validates a loaded ruleset; throws std::invalid_argument on error.
  ///
  /// @par Why is validation re-runnable?
  /// Loading already validates, but a ruleset may be *edited* after load (e.g.
  /// by tooling or a live-edit app). Re-running validation lets such a caller
  /// check the mutated model without re-parsing from JSON.
  static void validate(const Ruleset &ruleset);

private:
  // One parser per schema section keeps each function short and its failure
  // messages local to the section being read — a new section only adds a
  // parser, it never touches the existing ones.
  static void parseAttributes(const Json &obj, Ruleset &out);
  static void parseDerivedStats(const Json &obj, Ruleset &out);
  static void parseResources(const Json &obj, Ruleset &out);
  static void parseSkills(const Json &obj, Ruleset &out);
  static void parseCheckTypes(const Json &obj, Ruleset &out);
  static void parseCostTables(const Json &obj, Ruleset &out);
  static void parseEquipmentSlots(const Json &obj, Ruleset &out);
  static void parseEventTriggers(const Json &obj, Ruleset &out);
  static void parseCurrencies(const Json &obj, Ruleset &out);
  static void parseEncumbrance(const Json &obj, Ruleset &out);
  static void parseSpellcasting(const Json &obj, Ruleset &out);
  static void parseMovement(const Json &obj, Ruleset &out);
  static CheckRecipe parseRecipe(const Json &obj, std::string_view checkId);
  static EventType parseTrigger(std::string_view trigger);

  static std::string error(std::string_view message);
  static void require(bool condition, std::string_view message);
};

inline std::string RulesetLoader::error(std::string_view message) {
  return "ruleset validation failed: " + std::string(message);
}

inline void RulesetLoader::require(bool condition, std::string_view message) {
  if (!condition) {
    throw std::invalid_argument(error(message));
  }
}

inline CheckRecipe RulesetLoader::parseRecipe(const Json &item, std::string_view checkId) {
  const auto requireField = [&](bool condition, const std::string &message) {
    if (!condition) {
      throw std::invalid_argument(error("check_type '" + std::string(checkId) + "' " + message));
    }
  };

  CheckRecipe recipe;
  const std::string resolution = item.value("resolution", "threshold");
  if (resolution == "threshold") {
    recipe.resolution = Resolution::Threshold;
  } else if (resolution == "pool") {
    recipe.resolution = Resolution::Pool;
  } else if (resolution == "opposed") {
    recipe.resolution = Resolution::Opposed;
  } else if (resolution == "resistance") {
    recipe.resolution = Resolution::Resistance;
  } else {
    requireField(false, "has unknown resolution '" + resolution + "'");
  }
  recipe.dice = DiceExpression(item.value("dice", "1d20"));

  const std::string comparison = item.value("comparison", "ge");
  if (comparison == "ge") {
    recipe.comparison = Comparison::GreaterEqual;
  } else if (comparison == "le") {
    recipe.comparison = Comparison::LessEqual;
  } else {
    requireField(false, "has unknown comparison '" + comparison + "'");
  }

  const std::string source = item.value("threshold_source", "difficulty");
  if (source == "difficulty") {
    recipe.thresholdSource = ThresholdSource::Difficulty;
  } else if (source == "actor_stat") {
    recipe.thresholdSource = ThresholdSource::ActorStat;
  } else if (source == "target_stat") {
    recipe.thresholdSource = ThresholdSource::TargetStat;
  } else {
    requireField(false, "has unknown threshold_source '" + source + "'");
  }
  recipe.thresholdStat = item.value("threshold_stat", "");

  if (item.contains("bonus_stats")) {
    for (const Json &stat : item.at("bonus_stats")) {
      recipe.bonusStats.push_back(stat.get<std::string>());
    }
  }
  if (item.contains("pool_attributes")) {
    recipe.numPoolAttributes = 0;
    for (const Json &attr : item.at("pool_attributes")) {
      requireField(recipe.numPoolAttributes < 3, "has more than 3 pool_attributes");
      recipe.poolAttributes[recipe.numPoolAttributes++] = attr.get<std::string>();
    }
  }
  recipe.poolStat = item.value("pool_stat", "");
  recipe.attackStat = item.value("attack_stat", "");
  recipe.parryStat = item.value("parry_stat", "");
  recipe.compareLevels = item.value("compare_levels", false);

  const std::string critical = item.value("critical_style", "none");
  if (critical == "none") {
    recipe.criticalStyle = CriticalStyle::None;
  } else if (critical == "face") {
    recipe.criticalStyle = CriticalStyle::Face;
  } else if (critical == "double") {
    recipe.criticalStyle = CriticalStyle::DoubleRoll;
  } else if (critical == "percentile") {
    recipe.criticalStyle = CriticalStyle::PercentileBand;
  } else {
    requireField(false, "has unknown critical_style '" + critical + "'");
  }
  recipe.criticalFace = item.value("critical_face", 0);
  recipe.criticalConfirm = item.value("critical_confirm", false);

  const std::string fumble = item.value("fumble_style", "none");
  if (fumble == "none") {
    recipe.fumbleStyle = CriticalStyle::None;
  } else if (fumble == "face") {
    recipe.fumbleStyle = CriticalStyle::Face;
  } else if (fumble == "double") {
    recipe.fumbleStyle = CriticalStyle::DoubleRoll;
  } else if (fumble == "percentile") {
    recipe.fumbleStyle = CriticalStyle::PercentileBand;
  } else {
    requireField(false, "has unknown fumble_style '" + fumble + "'");
  }
  recipe.fumbleFace = item.value("fumble_face", 0);
  recipe.fumbleConfirm = item.value("fumble_confirm", false);

  const std::string grading = item.value("grading", "none");
  if (grading == "none") {
    recipe.grading = Grading::None;
  } else if (grading == "percentile") {
    recipe.grading = Grading::Percentile;
  } else if (grading == "pool_quality") {
    recipe.grading = Grading::PoolQuality;
  } else {
    requireField(false, "has unknown grading '" + grading + "'");
  }

  const std::string difficulty = item.value("difficulty_mode", "to_threshold");
  if (difficulty == "to_threshold") {
    recipe.difficultyMode = DifficultyMode::ToThreshold;
  } else if (difficulty == "to_stat") {
    recipe.difficultyMode = DifficultyMode::ToStat;
  } else {
    requireField(false, "has unknown difficulty_mode '" + difficulty + "'");
  }

  const std::string multiplier = item.value("difficulty_multiplier", "none");
  if (multiplier == "none") {
    recipe.difficultyMultiplier = DifficultyMultiplier::None;
  } else if (multiplier == "double_halve") {
    recipe.difficultyMultiplier = DifficultyMultiplier::DoubleHalve;
  } else {
    requireField(false, "has unknown difficulty_multiplier '" + multiplier + "'");
  }
  return recipe;
}

inline EventType RulesetLoader::parseTrigger(std::string_view trigger) {
  if (trigger == "on_before_check_roll") {
    return EventType::OnBeforeCheckRoll;
  }
  if (trigger == "on_after_check_roll") {
    return EventType::OnAfterCheckRoll;
  }
  if (trigger == "on_damage_calculated") {
    return EventType::OnDamageCalculated;
  }
  if (trigger == "on_damage_taken") {
    return EventType::OnDamageTaken;
  }
  if (trigger == "on_turn_start") {
    return EventType::OnTurnStart;
  }
  if (trigger == "on_turn_end") {
    return EventType::OnTurnEnd;
  }
  throw std::invalid_argument(error("unknown event trigger '" + std::string(trigger) + "'"));
}

inline void RulesetLoader::parseAttributes(const Json &obj, Ruleset &out) {
  if (!obj.contains("attributes")) {
    return;
  }
  for (const Json &item : obj.at("attributes")) {
    require(item.contains("id"), "attribute entry missing 'id'");
    require(item.contains("name"),
            "attribute '" + item.at("id").get<std::string>() + "' missing 'name'");
    AttributeDef def;
    def.id = item.at("id").get<std::string>();
    def.name = item.at("name").get<std::string>();
    def.minValue = item.value("min", 1);
    def.maxValue = item.value("max", 30);
    def.defaultValue = item.value("default", 10);
    out.attributes.push_back(std::move(def));
  }
}

inline void RulesetLoader::parseDerivedStats(const Json &obj, Ruleset &out) {
  if (!obj.contains("derived_stats")) {
    return;
  }
  for (const Json &item : obj.at("derived_stats")) {
    require(item.contains("id"), "derived_stat entry missing 'id'");
    require(item.contains("formula"),
            "derived_stat '" + item.at("id").get<std::string>() + "' missing 'formula'");
    DerivedStatDef def;
    def.id = item.at("id").get<std::string>();
    def.name = item.value("name", def.id);
    def.formulaText = item.at("formula").get<std::string>();
    def.expression = Expression(def.formulaText);
    out.derivedStats.push_back(std::move(def));
  }
}

inline void RulesetLoader::parseResources(const Json &obj, Ruleset &out) {
  if (!obj.contains("resource_pools")) {
    return;
  }
  for (const Json &item : obj.at("resource_pools")) {
    require(item.contains("id"), "resource_pool entry missing 'id'");
    require(item.contains("max_stat"),
            "resource_pool '" + item.at("id").get<std::string>() + "' missing 'max_stat'");
    ResourcePoolDef def;
    def.id = item.at("id").get<std::string>();
    def.name = item.value("name", def.id);
    def.maxStat = item.at("max_stat").get<std::string>();
    def.minValue = item.value("min", 0);
    out.resourcePools.push_back(std::move(def));
  }
}

inline void RulesetLoader::parseSkills(const Json &obj, Ruleset &out) {
  if (!obj.contains("skills")) {
    return;
  }
  for (const Json &item : obj.at("skills")) {
    require(item.contains("id"), "skill entry missing 'id'");
    require(item.contains("attributes"),
            "skill '" + item.at("id").get<std::string>() + "' missing 'attributes'");
    SkillDef def;
    def.id = item.at("id").get<std::string>();
    def.name = item.value("name", def.id);
    def.defaultValue = item.value("default", 0);
    for (const Json &attr : item.at("attributes")) {
      def.attributes.push_back(attr.get<std::string>());
    }
    out.skills.push_back(std::move(def));
  }
}

inline void RulesetLoader::parseCheckTypes(const Json &obj, Ruleset &out) {
  if (!obj.contains("check_types")) {
    return;
  }
  for (const auto &[key, item] : obj.at("check_types").items()) {
    CheckTypeDef def;
    def.id = key;
    def.recipe = parseRecipe(item, key);
    out.checkTypes.push_back(std::move(def));
  }
}

inline void RulesetLoader::parseCostTables(const Json &obj, Ruleset &out) {
  if (!obj.contains("cost_tables")) {
    return;
  }
  for (const auto &[key, item] : obj.at("cost_tables").items()) {
    require(item.contains("type"), "cost_table '" + key + "' missing 'type'");
    const std::string type = item.at("type").get<std::string>();
    require(type == "threshold" || type == "multiplier",
            "cost_table '" + key + "' has unknown type '" + type + "'");
    CostTableDef def;
    def.id = key;
    const CostTable::Kind kind =
        type == "threshold" ? CostTable::Kind::Threshold : CostTable::Kind::Multiplier;
    const double baseFactor = item.value("base_factor", 1.0);
    std::vector<CostTable::Entry> entries;
    if (item.contains("thresholds")) {
      for (const Json &entry : item.at("thresholds")) {
        require(entry.contains("key") && entry.contains("value"),
                "cost_table '" + key + "' entry missing 'key' or 'value'");
        entries.push_back(
            CostTable::Entry{entry.at("key").get<int32_t>(), entry.at("value").get<int32_t>()});
      }
    } else if (item.contains("values")) {
      for (const auto &[idx, value] : item.at("values").items()) {
        const int32_t keyInt = static_cast<int32_t>(std::stoi(idx));
        entries.push_back(CostTable::Entry{keyInt, value.get<int32_t>()});
      }
    }
    def.table = CostTable::fromEntries(kind, baseFactor, std::move(entries));
    out.costTables.push_back(std::move(def));
  }
}

inline void RulesetLoader::parseEquipmentSlots(const Json &obj, Ruleset &out) {
  if (!obj.contains("equipment_slots")) {
    return;
  }
  for (const Json &slot : obj.at("equipment_slots")) {
    require(slot.contains("id"), "equipment_slot entry missing 'id'");
    out.equipmentSlots.push_back(slot.at("id").get<std::string>());
  }
}

inline void RulesetLoader::parseEventTriggers(const Json &obj, Ruleset &out) {
  if (!obj.contains("event_triggers")) {
    return;
  }
  for (const Json &item : obj.at("event_triggers")) {
    require(item.contains("id"), "event_trigger entry missing 'id'");
    require(item.contains("trigger"),
            "event_trigger '" + item.at("id").get<std::string>() + "' missing 'trigger'");
    EventTriggerDef def;
    def.id = item.at("id").get<std::string>();
    def.trigger = parseTrigger(item.at("trigger").get<std::string>());
    def.conditionText = item.value("condition", "");
    if (!def.conditionText.empty()) {
      def.condition = Expression(def.conditionText);
    }
    for (const Json &action : item.value("actions", Json::array())) {
      require(action.contains("type"), "event action missing 'type'");
      EventActionDef act;
      act.type = action.at("type").get<std::string>();
      act.resource = action.value("resource", "");
      act.amount = action.value("amount", 0);
      act.item = action.value("item", "");
      act.condition = action.value("condition", "");
      act.formulaText = action.value("formula", "");
      if (!act.formulaText.empty()) {
        act.formula = Expression(act.formulaText);
      }
      act.stacksText = action.value("stacks", "");
      if (!act.stacksText.empty()) {
        act.stacks = Expression(act.stacksText);
      }
      def.actions.push_back(std::move(act));
    }
    out.eventTriggers.push_back(std::move(def));
  }
}

inline void RulesetLoader::parseCurrencies(const Json &obj, Ruleset &out) {
  if (!obj.contains("currencies")) {
    return;
  }
  const Json &cur = obj.at("currencies");
  require(cur.is_object(), "'currencies' must be an object");
  out.currencySystem.id = cur.value("id", "coins");
  out.currencySystem.name = cur.value("name", out.currencySystem.id);
  out.currencySystem.baseUnit = cur.value("base_unit", "");
  require(!out.currencySystem.baseUnit.empty(), "'currencies' missing 'base_unit'");
  require(cur.contains("denominations") && cur.at("denominations").is_array(),
          "'currencies' missing 'denominations' array");
  for (const Json &denom : cur.at("denominations")) {
    require(denom.contains("id"), "currency denomination missing 'id'");
    Denomination d;
    d.id = denom.at("id").get<std::string>();
    d.name = denom.value("name", d.id);
    d.symbol = denom.value("symbol", d.id);
    d.perBase = denom.value("per_base", 1);
    out.currencySystem.denominations.push_back(std::move(d));
  }
  // The base unit must be one of the declared denominations.
  require(out.currencySystem.find(out.currencySystem.baseUnit) != nullptr,
          "currency base_unit '" + out.currencySystem.baseUnit + "' is not a denomination");
}

inline void RulesetLoader::parseEncumbrance(const Json &obj, Ruleset &out) {
  if (!obj.contains("encumbrance")) {
    return;
  }
  const Json &enc = obj.at("encumbrance");
  require(enc.is_object(), "'encumbrance' must be an object");
  out.encumbrance.enabled = true;
  out.encumbrance.weightUnit = enc.value("weight_unit", "lb");
  out.encumbrance.defaultItemWeight = enc.value("default_weight", 0.0);
  out.encumbrance.capacityText = enc.value("capacity", "");
  if (!out.encumbrance.capacityText.empty()) {
    out.encumbrance.capacity = Expression(out.encumbrance.capacityText);
  }
  out.encumbrance.sizeUnit = enc.value("size_unit", "size");
  out.encumbrance.defaultItemSize = enc.value("default_item_size", 0.0);
  out.encumbrance.sizeCapacityText = enc.value("size_capacity", "");
  if (!out.encumbrance.sizeCapacityText.empty()) {
    out.encumbrance.sizeCapacity = Expression(out.encumbrance.sizeCapacityText);
  }
  out.encumbrance.itemCapacityText = enc.value("item_capacity", "");
  if (!out.encumbrance.itemCapacityText.empty()) {
    out.encumbrance.itemCapacity = Expression(out.encumbrance.itemCapacityText);
  }
  if (enc.contains("levels") && enc.at("levels").is_array()) {
    for (const Json &level : enc.at("levels")) {
      EncumbranceConfig::Level l;
      l.maxRatio = level.value("max_ratio", 1.0);
      l.conditionId = level.value("condition", "");
      out.encumbrance.levels.push_back(std::move(l));
    }
  }
}

inline void RulesetLoader::parseSpellcasting(const Json &obj, Ruleset &out) {
  if (!obj.contains("spellcasting")) {
    return;
  }
  const Json &sc = obj.at("spellcasting");
  require(sc.is_object(), "'spellcasting' must be an object");
  const std::string style = sc.value("style", "pool");
  require(style == "pool" || style == "slots", "'spellcasting' style must be 'pool' or 'slots'");
  out.spellcasting.style = style;
  if (sc.contains("slots") && sc.at("slots").is_object()) {
    for (const auto &[level, count] : sc.at("slots").items()) {
      out.spellcasting.slots[std::stoi(level)] = count.get<int32_t>();
    }
  }
}

inline void RulesetLoader::parseMovement(const Json &obj, Ruleset &out) {
  if (!obj.contains("movement")) {
    return;
  }
  const Json &mov = obj.at("movement");
  require(mov.is_object(), "'movement' must be an object");
  MovementConfig &config = out.movement;
  config.defaultTerrain = mov.value("default_terrain", "");
  if (mov.contains("modes") && mov.at("modes").is_object()) {
    for (const auto &[modeId, modeJson] : mov.at("modes").items()) {
      require(modeJson.is_object(), "'movement.modes.<id>' must be an object");
      MovementModeDef mode;
      mode.id = modeId;
      mode.name = modeJson.value("name", modeId);
      mode.speedText = modeJson.value("speed", "");
      if (!mode.speedText.empty()) {
        mode.speed = Expression(mode.speedText);
      }
      mode.factor = modeJson.value("factor", 1.0);
      mode.exhaustion = modeJson.value("exhaustion", 0.0);
      config.modes[modeId] = std::move(mode);
    }
  }
  if (mov.contains("terrains") && mov.at("terrains").is_object()) {
    for (const auto &[terrainId, terrainJson] : mov.at("terrains").items()) {
      require(terrainJson.is_object(), "'movement.terrains.<id>' must be an object");
      TerrainDef terrain;
      terrain.id = terrainId;
      terrain.name = terrainJson.value("name", terrainId);
      terrain.description = terrainJson.value("description", "");
      terrain.regeneration = terrainJson.value("regeneration", "normal");
      require(terrain.regeneration == "normal" || terrain.regeneration == "none" ||
                  terrain.regeneration == "half",
              "'movement.terrains.<id>.regeneration' must be 'normal', 'none', or 'half'");
      if (terrainJson.contains("modes") && terrainJson.at("modes").is_object()) {
        for (const auto &[modeId, ruleJson] : terrainJson.at("modes").items()) {
          require(ruleJson.is_object(), "'movement.terrains.<id>.modes.<mode>' must be an object");
          TerrainModeRule rule;
          rule.speedFactor = ruleJson.value("factor", 1.0);
          rule.costFactor = ruleJson.value("cost_factor", 1.0);
          rule.possible = ruleJson.value("possible", true);
          rule.requiresCapability = ruleJson.value("requires", "");
          terrain.modes[modeId] = std::move(rule);
        }
      }
      config.terrains[terrainId] = std::move(terrain);
    }
  }
  if (mov.contains("exhaustion") && mov.at("exhaustion").is_object()) {
    const Json &exhaustion = mov.at("exhaustion");
    config.exhaustionPool = exhaustion.value("pool", "");
    config.exhaustionPerDistance = exhaustion.value("per_distance", 1.0);
  }
  if (mov.contains("load") && mov.at("load").is_object()) {
    const Json &load = mov.at("load");
    if (load.contains("levels") && load.at("levels").is_array()) {
      for (const Json &level : load.at("levels")) {
        LoadSpeedLevel l;
        l.maxRatio = level.value("max_ratio", 1.0);
        l.factor = level.value("factor", 1.0);
        config.loadLevels.push_back(std::move(l));
      }
    }
  }
  // A ruleset with a movement section always has a terrain to move in: an
  // implicit "land" when none is declared, and a default terrain id.
  if (config.terrains.empty()) {
    TerrainDef implicit;
    implicit.id = "land";
    implicit.name = "Land";
    config.terrains["land"] = std::move(implicit);
  }
  if (config.defaultTerrain.empty()) {
    config.defaultTerrain = config.terrains.begin()->first;
  }
}

inline Ruleset RulesetLoader::load(const Json &root) {
  require(root.is_object(), "ruleset root must be a JSON object");
  require(root.contains("schema_version"), "ruleset missing 'schema_version'");
  require(root.contains("ruleset_id"), "ruleset missing 'ruleset_id'");
  require(root.contains("licence"), "ruleset missing required 'licence'");
  require(root.at("licence").is_string() && !root.at("licence").get<std::string>().empty(),
          "ruleset 'licence' must be a non-empty string");

  Ruleset out;
  out.schemaVersion = root.at("schema_version").get<int32_t>();
  out.id = root.at("ruleset_id").get<std::string>();
  out.name = root.value("ruleset_name", out.id);
  out.source = root.value("source", "");
  out.licence = root.value("licence", "");
  out.licenceSource = root.value("licence_source", "");
  out.licenceNotice = root.value("licence_notice", "");
  out.attribution = root.value("attribution", "");
  out.comment = root.value("comment", "");
  out.cppNamespace = root.value("namespace", "rpg_os::generated::" + out.id);
  out.spellResource = root.value("spell_resource", "");

  parseAttributes(root, out);
  parseDerivedStats(root, out);
  parseResources(root, out);
  parseSkills(root, out);
  parseCheckTypes(root, out);
  parseCostTables(root, out);
  parseEquipmentSlots(root, out);
  parseEventTriggers(root, out);
  parseCurrencies(root, out);
  parseEncumbrance(root, out);
  parseSpellcasting(root, out);
  parseMovement(root, out);
  if (root.contains("data")) {
    out.data = root.at("data");
  }

  validate(out);
  return out;
}

inline Ruleset RulesetLoader::loadFromString(std::string_view jsonContent) {
  Json root = Json::parse(jsonContent);
  return load(root);
}

inline void RulesetLoader::validate(const Ruleset &ruleset) {
  std::unordered_set<std::string> seen;
  // Attribute ids must be unique.
  for (const AttributeDef &def : ruleset.attributes) {
    require(seen.insert(def.id).second, "duplicate attribute id '" + def.id + "'");
  }
  for (const DerivedStatDef &def : ruleset.derivedStats) {
    require(seen.insert(def.id).second, "duplicate derived stat id '" + def.id + "'");
  }
  for (const SkillDef &def : ruleset.skills) {
    require(seen.insert(def.id).second, "duplicate skill id '" + def.id + "'");
  }
  std::unordered_set<std::string> checkIds;
  for (const CheckTypeDef &def : ruleset.checkTypes) {
    require(checkIds.insert(def.id).second, "duplicate check type id '" + def.id + "'");
  }
  std::unordered_set<std::string> tableIds;
  for (const CostTableDef &def : ruleset.costTables) {
    require(tableIds.insert(def.id).second, "duplicate cost table id '" + def.id + "'");
  }

  // Derived-stat formulas: bare identifiers must be known stats.
  for (const DerivedStatDef &def : ruleset.derivedStats) {
    for (const std::string &ident : def.expression.identifiers()) {
      if (ident.find('.') != std::string::npos) {
        continue; // dotted paths (actor.*, target.*, env.*, ...) are allowed
      }
      require(ruleset.hasStat(ident),
              "derived stat '" + def.id + "' references unknown stat '" + ident + "'");
    }
  }

  // Derived-stat graph must be acyclic.
  {
    std::unordered_map<std::string, std::vector<std::string>> graph;
    for (const DerivedStatDef &def : ruleset.derivedStats) {
      for (const std::string &ident : def.expression.identifiers()) {
        if (ident.find('.') == std::string::npos && ruleset.findDerivedStat(ident) != nullptr) {
          graph[def.id].push_back(ident);
        }
      }
    }
    enum class State : uint8_t { Unvisited, Visiting, Done };
    std::unordered_map<std::string, State> state;
    std::function<void(const std::string &)> visit = [&](const std::string &nodeId) {
      state[nodeId] = State::Visiting;
      for (const std::string &dep : graph[nodeId]) {
        if (state[dep] == State::Visiting) {
          throw std::invalid_argument(error("derived stat cycle involving '" + nodeId + "'"));
        }
        if (state[dep] == State::Unvisited) {
          visit(dep);
        }
      }
      state[nodeId] = State::Done;
    };
    for (const DerivedStatDef &def : ruleset.derivedStats) {
      if (state[def.id] == State::Unvisited) {
        visit(def.id);
      }
    }
  }

  // Resource max stats must exist.
  for (const ResourcePoolDef &def : ruleset.resourcePools) {
    require(ruleset.hasStat(def.maxStat),
            "resource pool '" + def.id + "' references unknown max_stat '" + def.maxStat + "'");
  }

  // Encumbrance capacity formula: bare identifiers must be known stats.
  if (!ruleset.encumbrance.capacityText.empty()) {
    for (const std::string &ident : ruleset.encumbrance.capacity.identifiers()) {
      if (ident.find('.') == std::string::npos) {
        require(ruleset.hasStat(ident),
                "encumbrance capacity references unknown stat '" + ident + "'");
      }
    }
  }

  // Skill attribute references must exist.
  for (const SkillDef &def : ruleset.skills) {
    for (const std::string &attr : def.attributes) {
      require(ruleset.findAttribute(attr) != nullptr,
              "skill '" + def.id + "' references unknown attribute '" + attr + "'");
    }
  }

  // Check type recipe references must exist. The combat simulator promotes a
  // bestiary entry's attack/defence fields into the stats "Attack", "Parry",
  // "Armor_Rating", "AC", and "Initiative" (see combat.hpp), so a check type
  // may reference those names even when the ruleset has no such stat of its
  // own — that is how a D&D-style ruleset declares its attack-vs-AC recipe.
  const auto knownStat = [&ruleset](const std::string &stat) {
    return ruleset.hasStat(stat) || stat == "Attack" || stat == "Parry" || stat == "Armor_Rating" ||
           stat == "AC" || stat == "Initiative";
  };
  for (const CheckTypeDef &def : ruleset.checkTypes) {
    const CheckRecipe &recipe = def.recipe;
    for (const std::string &stat : recipe.bonusStats) {
      require(knownStat(stat),
              "check type '" + def.id + "' references unknown bonus stat '" + stat + "'");
    }
    if (recipe.thresholdSource == ThresholdSource::TargetStat ||
        recipe.thresholdSource == ThresholdSource::ActorStat) {
      require(knownStat(recipe.thresholdStat), "check type '" + def.id +
                                                   "' references unknown threshold stat '" +
                                                   recipe.thresholdStat + "'");
    }
    if (!recipe.poolStat.empty()) {
      require(knownStat(recipe.poolStat),
              "check type '" + def.id + "' references unknown pool stat '" + recipe.poolStat + "'");
    }
    if (!recipe.attackStat.empty()) {
      require(knownStat(recipe.attackStat), "check type '" + def.id +
                                                "' references unknown attack stat '" +
                                                recipe.attackStat + "'");
    }
    if (!recipe.parryStat.empty()) {
      require(knownStat(recipe.parryStat), "check type '" + def.id +
                                               "' references unknown parry stat '" +
                                               recipe.parryStat + "'");
    }
    for (std::size_t i = 0; i < recipe.numPoolAttributes; ++i) {
      require(knownStat(recipe.poolAttributes[i]), "check type '" + def.id +
                                                       "' references unknown pool attribute '" +
                                                       recipe.poolAttributes[i] + "'");
    }
    // A pool check needs as many dice as attributes.
    if (recipe.resolution == Resolution::Pool) {
      require(recipe.dice.dieCount() == recipe.numPoolAttributes,
              "check type '" + def.id + "' pool resolution dice count must match pool_attributes");
    }
  }

  // Event triggers: action types must be known (conditions and formulas were
  // already parsed by Expression at load time).
  for (const EventTriggerDef &def : ruleset.eventTriggers) {
    for (const EventActionDef &action : def.actions) {
      require(action.type == "modify_event_damage" || action.type == "consume_resource" ||
                  action.type == "apply_condition" || action.type == "gain_item" ||
                  action.type == "remove_item" || action.type == "gain_currency" ||
                  action.type == "spend_currency" || action.type == "gain_xp",
              "event action in '" + def.id + "' has unknown type '" + action.type + "'");
    }
  }
}

} // namespace rpg_os
