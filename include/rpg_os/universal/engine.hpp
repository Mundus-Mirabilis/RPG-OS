// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file engine.hpp
 * @defgroup rpg_os_universal Universal mode — the dynamic engine
 * @brief RulesetEngine facade (universal mode).
 *
 * The public entry point for applications using the dynamic engine: load a
 * ruleset, create entities from archetypes, calculate stats, resolve named
 * checks, apply damage through the JSON-driven event pipeline, and register
 * event listeners.
 *
 * @par Why a single facade?
 * An application that wants to "just run a ruleset" should not have to wire
 * the loader, the resolver, the entity factory, and the event bus together
 * by hand. The engine owns that wiring: it holds the loaded @c Ruleset, the
 * @c EventBus, and the load state, and exposes one cohesive API. The
 * individual pieces remain separately usable (the code generator and tests
 * use them directly), but the facade is the recommended entry point.
 */
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <expected>
#ifndef __EMSCRIPTEN__
#include <fstream>
#endif
#include <memory>
#include <rpg_os/common/event_system.hpp>
#include <rpg_os/common/json.hpp>
#include <rpg_os/common/types.hpp>
#include <rpg_os/core/capacity.hpp>
#include <rpg_os/core/checks.hpp>
#include <rpg_os/core/dice_engine.hpp>
#include <rpg_os/core/errors.hpp>
#include <rpg_os/core/math.hpp>
#include <rpg_os/core/variance.hpp>
#include <rpg_os/universal/check_resolver.hpp>
#include <rpg_os/universal/dynamic_entity.hpp>
#include <rpg_os/universal/ruleset_loader.hpp>
#ifndef __EMSCRIPTEN__
#include <sstream>
#endif
#include <string>
#include <string_view>
#include <vector>

namespace rpg_os {

/// The universal (dynamic) engine facade.
class RulesetEngine {
public:
  /// Loads a ruleset from a JSON string. Returns false (and records a message
  /// in `lastError()`) on parse or validation failure.
  ///
  /// @par Why swallow exceptions here but not in the loader?
  /// A facade is friendlier to application code when "did the load succeed?"
  /// is a boolean plus a message rather than a try/catch. The loader itself
  /// still throws precise errors for callers that want them; the engine
  /// converts them into @ref lastError for the common case.
  bool loadRulesetFromJson(std::string_view jsonContent) {
    try {
      m_ruleset = RulesetLoader::loadFromString(jsonContent);
      m_loaded = true;
      m_lastError.clear();
      return true;
    } catch (const std::exception &e) {
      m_lastError = e.what();
      m_loaded = false;
      return false;
    }
  }

#ifndef __EMSCRIPTEN__
  /// Loads a ruleset from a file (UTF-8). Returns false on I/O or validation
  /// failure; see `lastError()`.
  ///
  /// @par Why compiled out on WebAssembly?
  /// The WASM binding has no filesystem (`-sFILESYSTEM=0`) and receives
  /// ruleset text via `loadRulesetFromJson`, so the iostream machinery
  /// (`<fstream>`/`<sstream>`, `std::locale`) is excluded from the WASM
  /// binary — it is a significant chunk of its size. Native builds keep it.
  bool loadRulesetFromFile(std::string_view path) {
    const std::string pathStr(path);
    std::ifstream file(pathStr);
    if (!file) {
      m_lastError = "cannot open file '" + pathStr + "'";
      m_loaded = false;
      return false;
    }
    // Read via rdbuf() rather than the istreambuf_iterator range idiom: the
    // iterator path trips a -Wnull-dereference false positive in libstdc++ 13
    // under -Werror, and streaming the streambuf is the idiomatic whole-file
    // read anyway.
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return loadRulesetFromJson(buffer.str());
  }
#endif

  /// Re-runs validation on the loaded ruleset; false when no ruleset is
  /// loaded or validation fails (loading already validates). Useful after a
  /// ruleset has been mutated in place.
  [[nodiscard]] bool validateRuleset() const {
    if (!m_loaded) {
      return false;
    }
    try {
      RulesetLoader::validate(m_ruleset);
      return true;
    } catch (const std::exception &) {
      return false;
    }
  }

  /// Creates an entity from the named archetype in the ruleset's `data`
  /// section (ranged values picked at random), or nullptr when the archetype
  /// (or ruleset) does not exist.
  [[nodiscard]] std::shared_ptr<DynamicEntity> createEntity(std::string_view archetypeId) {
    return createEntity(archetypeId, Variance::Random);
  }

  /// Creates an entity from the named archetype, picking ranged values
  /// according to `variance` (weakest / weak / average / strong / strongest).
  [[nodiscard]] std::shared_ptr<DynamicEntity> createEntity(std::string_view archetypeId,
                                                            Variance variance) {
    DefaultRandom rng;
    return createEntityWith(archetypeId, variance, rng);
  }

  /// Creates an entity from the named archetype with the caller-supplied RNG
  /// (deterministic tests inject a scripted RNG, Monte-Carlo runs inject a
  /// seeded one).
  template <RandomNumberGenerator Rng>
  [[nodiscard]] std::shared_ptr<DynamicEntity> createEntityWith(std::string_view archetypeId,
                                                                Variance variance, Rng &rng) {
    if (!m_loaded || !m_ruleset.data.is_object() || !m_ruleset.data.contains("archetypes")) {
      return nullptr;
    }
    for (const Json &archetype : m_ruleset.data.at("archetypes")) {
      if (archetype.value("id", "") == archetypeId) {
        auto entity = std::make_shared<DynamicEntity>(m_ruleset, std::string(archetypeId));
        entity->loadFromArchetype(archetype, variance, rng);
        attachEntityEvents(*entity);
        return entity;
      }
    }
    return nullptr;
  }

  /// Creates a creature from the named entry in the ruleset's `data.creatures`
  /// section, or nullptr when it does not exist. Bestiary entries may carry
  /// ranged values (e.g. hit points as "2d6"), so a variance can be requested.
  ///
  /// @par Why a separate creation path for creatures?
  /// Archetypes and bestiary entries are authored differently in the source
  /// material (a PC block vs. a monster stat line), and callers think of them
  /// as distinct pools. Keeping both lookup paths explicit lets an application
  /// ask "the goblin" without ambiguity about which section it came from.
  [[nodiscard]] std::shared_ptr<DynamicEntity>
  createCreature(std::string_view creatureId, Variance variance = Variance::Random) {
    DefaultRandom rng;
    return createCreatureWith(creatureId, variance, rng);
  }

  /// Creates a creature from `data.creatures` with the caller-supplied RNG.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] std::shared_ptr<DynamicEntity> createCreatureWith(std::string_view creatureId,
                                                                  Variance variance, Rng &rng) {
    if (!m_loaded || !m_ruleset.data.is_object() || !m_ruleset.data.contains("creatures")) {
      return nullptr;
    }
    for (const Json &creature : m_ruleset.data.at("creatures")) {
      if (creature.value("id", "") == creatureId) {
        auto entity = std::make_shared<DynamicEntity>(m_ruleset, std::string(creatureId));
        entity->loadFromArchetype(creature, variance, rng);
        applyTraitEffects(*entity, rng);
        attachEntityEvents(*entity);
        return entity;
      }
    }
    return nullptr;
  }

  /// Resolves a creature's trait effects (regeneration, damage resistance,
  /// ...) against the creature itself, so a freshly created creature carries
  /// its always-on mechanical traits — recurring heals register as ongoing
  /// effects, resistances are added to the sheet, etc.
  template <RandomNumberGenerator Rng> void applyTraitEffects(DynamicEntity &entity, Rng &rng) {
    for (const std::string &traitId : entity.traits()) {
      const Json *trait = findDataRecord("traits", traitId);
      if (trait == nullptr || !trait->contains("effects") || !trait->at("effects").is_array()) {
        continue;
      }
      (void)resolveEffects(entity, entity, trait->at("effects"), CheckParams{}, rng);
    }
  }

  /// Calculates a stat (attribute, skill rating, or derived stat) for `entity`.
  /// This is a thin passthrough to @ref DynamicEntity::getStat that gives the
  /// facade a uniform "ask the engine for a value" API.
  [[nodiscard]] int32_t calculateStat(const DynamicEntity &entity, std::string_view statId) const {
    return entity.getStat(statId);
  }

  /// Whether `pattern` selects `scope`: `"all"` matches everything, an exact
  /// match selects that check, a trailing `*` matches any scope with the given
  /// prefix (e.g. `"dnd5e_attack_*"`), and a bare category keyword (`"attack"`,
  /// `"save"`, `"check"`, `"skill"`) matches any scope containing that word as
  /// one of its underscore-separated segments — so `"attack"` selects both the
  /// literal attack scope and a check type id like `"dnd5e_attack_melee"`.
  [[nodiscard]] static bool scopeMatches(std::string_view pattern,
                                         std::string_view scope) noexcept {
    if (pattern == "all" || pattern == scope) {
      return true;
    }
    if (!pattern.empty() && pattern.back() == '*') {
      return scope.starts_with(pattern.substr(0, pattern.size() - 1));
    }
    std::size_t begin = 0;
    while (begin <= scope.size()) {
      const std::size_t end = scope.find('_', begin);
      const std::string_view segment =
          scope.substr(begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
      if (segment == pattern) {
        return true;
      }
      if (end == std::string_view::npos) {
        break;
      }
      begin = end + 1;
    }
    return false;
  }

  /// Folds the check modifiers of `actor`'s and (for target-side entries)
  /// `target`'s active conditions and traits into a copy of `params`.
  ///
  /// @par Why a per-check copy rather than a global flag?
  /// Conditions are per-entity and per-situation: a blinded attacker attacks
  /// with disadvantage (its own condition), while attacks against a blinded
  /// defender gain advantage (the *target's* condition). Each check resolves
  /// once, so the modifiers are aggregated at the point the check is made —
  /// named checks via @ref executeCheck, spell attacks via @ref resolveAttack,
  /// saving throws via @ref resolveSave, skill checks via @ref executeSkillCheck
  /// — and folded into the params (a flat bonus into
  /// @c CheckParams::situationalModifier, advantage into
  /// @c CheckParams::advantage, bonus dice into @c CheckParams::bonusDice,
  /// automatic failure into @c CheckParams::autoFail). Both advantage and
  /// disadvantage present on a roll cancel each other, matching the D&D rule.
  [[nodiscard]] CheckParams withConditionModifiers(const DynamicEntity &actor,
                                                   const DynamicEntity *target,
                                                   std::string_view scope,
                                                   CheckParams params) const {
    int32_t bonus = 0;
    int advantage = 0;
    int disadvantage = 0;
    bool autoFail = false;
    std::vector<std::string> bonusDice;
    accumulateCheckModifiers(actor, scope, "actor", bonus, advantage, disadvantage, autoFail,
                             bonusDice);
    if (target != nullptr) {
      accumulateCheckModifiers(*target, scope, "target", bonus, advantage, disadvantage, autoFail,
                               bonusDice);
    }
    // Temporary bonus dice from effects (the `bonus_die` effect kind) apply to
    // the carrier's own checks of the matching scope.
    for (const BonusDie &die : actor.effects().bonusDice()) {
      if (scopeMatches(die.scope, scope)) {
        bonusDice.push_back(die.dice);
      }
    }
    params.situationalModifier += bonus;
    if (advantage > 0 && disadvantage > 0) {
      params.advantage = AdvantageMode::None;
    } else if (advantage > 0) {
      params.advantage = AdvantageMode::Advantage;
    } else if (disadvantage > 0) {
      params.advantage = AdvantageMode::Disadvantage;
    }
    params.autoFail = autoFail;
    for (const std::string &dice : bonusDice) {
      params.bonusDice.push_back(DiceExpression(dice));
    }
    return params;
  }

  /// Accumulates the check modifiers contributed by one entity's active
  /// conditions (scaled by their stacks) and inherent traits, whose declared
  /// `side` equals `wantedSide` and whose `scope` matches `scope` (see
  /// @ref withConditionModifiers).
  void accumulateCheckModifiers(const DynamicEntity &entity, std::string_view scope,
                                std::string_view wantedSide, int32_t &bonus, int &advantage,
                                int &disadvantage, bool &autoFail,
                                std::vector<std::string> &bonusDice) const {
    for (const auto &[conditionId, stacks] : entity.conditions()) {
      const Json *condition = findCondition(conditionId);
      if (condition == nullptr || !condition->contains("check_modifiers")) {
        continue;
      }
      const Json &modifiers = condition->at("check_modifiers");
      if (!modifiers.is_array()) {
        continue;
      }
      for (const Json &mod : modifiers) {
        accumulateModifierEntry(mod, scope, wantedSide, stacks, bonus, advantage, disadvantage,
                                autoFail, bonusDice);
      }
    }
    for (const std::string &traitId : entity.traits()) {
      const Json *trait = findDataRecord("traits", traitId);
      if (trait == nullptr || !trait->contains("check_modifiers")) {
        continue;
      }
      const Json &modifiers = trait->at("check_modifiers");
      if (!modifiers.is_array()) {
        continue;
      }
      for (const Json &mod : modifiers) {
        accumulateModifierEntry(mod, scope, wantedSide, 1, bonus, advantage, disadvantage, autoFail,
                                bonusDice);
      }
    }
  }

  /// Applies one `check_modifiers` entry: matches its side and scope, counts
  /// advantage/disadvantage, folds a (per-stack) bonus into `bonus`, records
  /// an `auto_fail` mode and a `bonus_dice`.
  void accumulateModifierEntry(const Json &mod, std::string_view scope, std::string_view wantedSide,
                               int32_t stacks, int32_t &bonus, int &advantage, int &disadvantage,
                               bool &autoFail, std::vector<std::string> &bonusDice) const {
    const std::string side = mod.value("side", "actor");
    if (side != wantedSide) {
      return;
    }
    if (!scopeMatches(mod.value("scope", "all"), scope)) {
      return;
    }
    const std::string mode = mod.value("mode", "none");
    if (mode == "advantage") {
      ++advantage;
    } else if (mode == "disadvantage") {
      ++disadvantage;
    } else if (mode == "auto_fail") {
      autoFail = true;
    }
    if (mod.contains("bonus")) {
      int32_t modBonus = mod.at("bonus").get<int32_t>();
      if (mod.value("per_stack", false)) {
        modBonus *= stacks;
      }
      bonus += modBonus;
    }
    if (mod.contains("bonus_dice")) {
      bonusDice.push_back(mod.at("bonus_dice").get<std::string>());
    }
  }

  /// Resolves a named check type from the ruleset using a caller-supplied RNG
  /// (deterministic tests inject a scripted RNG).
  ///
  /// @par Why is the target a pointer?
  /// Checks may or may not have a target (a D&D ability check vs. a monster,
  /// or a solo DSA talent check). A `nullptr` target is the "no target" case
  /// and is resolved against @ref NullStatProvider, keeping the common solo
  /// case ergonomic.
  ///
  /// The check is resolved through the *effective* stats of both sides and
  /// with the actors' active-condition check modifiers folded in (a blinded
  /// attacker attacks at disadvantage; a poisoned spellcaster's attack checks
  /// are impaired), so conditions that carry `check_modifiers` affect every
  /// named check automatically.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] CheckResult executeCheck(std::string_view checkTypeId, const DynamicEntity &actor,
                                         const DynamicEntity *target, const CheckParams &params,
                                         Rng &rng) const {
    const CheckParams adjusted = withConditionModifiers(actor, target, checkTypeId, params);
    announceCheckStart(checkTypeId, actor);
    CheckResult result;
    if (target != nullptr) {
      result = CheckResolver::resolve(m_ruleset, actor, *target, checkTypeId, adjusted, rng);
    } else {
      result =
          CheckResolver::resolve(m_ruleset, actor, NullStatProvider{}, checkTypeId, adjusted, rng);
    }
    announceCheckResult(checkTypeId, actor, result);
    return result;
  }

  /// Resolves a named check type using a fresh default RNG. Convenience for
  /// application code that does not care about reproducibility.
  [[nodiscard]] CheckResult executeCheck(std::string_view checkTypeId, const DynamicEntity &actor,
                                         const DynamicEntity *target,
                                         const CheckParams &params) const {
    DefaultRandom rng;
    return executeCheck(checkTypeId, actor, target, params, rng);
  }

  /// Resolves a named check type against the *effective* stats of both sides
  /// (raw value plus equipped-item and condition modifiers). This is what
  /// combat and anything that should reflect gear/status use.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] CheckResult
  executeCheckEffective(std::string_view checkTypeId, const DynamicEntity &actor,
                        const DynamicEntity *target, const CheckParams &params, Rng &rng) const {
    const CheckParams adjusted = withConditionModifiers(actor, target, checkTypeId, params);
    announceCheckStart(checkTypeId, actor);
    CheckResult result;
    if (target != nullptr) {
      result = CheckResolver::resolve(m_ruleset, EffectiveStatProvider{actor},
                                      EffectiveStatProvider{*target}, checkTypeId, adjusted, rng);
    } else {
      result = CheckResolver::resolve(m_ruleset, EffectiveStatProvider{actor}, NullStatProvider{},
                                      checkTypeId, adjusted, rng);
    }
    announceCheckResult(checkTypeId, actor, result);
    return result;
  }

  /// Resolves a check against effective stats using a fresh default RNG.
  [[nodiscard]] CheckResult executeCheckEffective(std::string_view checkTypeId,
                                                  const DynamicEntity &actor,
                                                  const DynamicEntity *target,
                                                  const CheckParams &params) const {
    DefaultRandom rng;
    return executeCheckEffective(checkTypeId, actor, target, params, rng);
  }

  /// Resolves a skill check for a named skill, using the skill's own linked
  /// attributes and the skill rating as the pool (the generic pool
  /// resolution applied to the skill's three linked attributes). Skill checks
  /// have no target. Throws std::invalid_argument for an unknown skill or a
  /// skill without exactly three linked attributes.
  ///
  /// @par Why drive it from the skill definition rather than a check type?
  /// A skill check's attributes live on the skill definition, not in a named
  /// check type. Building a generic pool recipe from those attributes keeps
  /// the skill as the single source of truth and requires no ruleset-specific
  /// code — the pool resolution (and its double-roll criticals and quality
  /// grading) is entirely described by the recipe.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] CheckResult executeSkillCheck(std::string_view skillId, const DynamicEntity &actor,
                                              const CheckParams &params, Rng &rng) const {
    const SkillDef *skill = m_ruleset.findSkill(skillId);
    if (skill == nullptr) {
      throw std::invalid_argument("unknown skill '" + std::string(skillId) + "'");
    }
    if (skill->attributes.size() != 3) {
      throw std::invalid_argument("skill '" + std::string(skillId) +
                                  "' is not a three-attribute skill check");
    }
    CheckRecipe recipe = makePoolRecipe();
    recipe.numPoolAttributes = 3;
    recipe.poolAttributes = {skill->attributes[0], skill->attributes[1], skill->attributes[2]};
    recipe.poolStat = std::string(skillId);
    const CheckParams adjusted = withConditionModifiers(actor, nullptr, "skill", params);
    announceCheckStart(skillId, actor);
    const CheckResult result = resolveCheck(actor, NullStatProvider{}, recipe, adjusted, rng);
    announceCheckResult(skillId, actor, result);
    return result;
  }

  /// Resolves a skill check using a fresh default RNG. Convenience overload.
  [[nodiscard]] CheckResult executeSkillCheck(std::string_view skillId, const DynamicEntity &actor,
                                              const CheckParams &params) const {
    DefaultRandom rng;
    return executeSkillCheck(skillId, actor, params, rng);
  }

  /// Applies `rawDamage` to `target`'s resource `resourceId` through the
  /// event pipeline:
  ///   1. `OnDamageCalculated` rule triggers may reduce the damage (e.g. DSA
  ///      armor rating) by modifying `event.damage`;
  ///   2. the final damage is applied to the resource;
  ///   3. `OnDamageTaken` rule triggers run (e.g. wound checks).
  /// Returns the damage actually applied.
  ///
  /// @par Why route damage through events even when no triggers exist?
  /// It is cheaper to always run the pipeline than to ask "are there any
  /// relevant triggers?" first — with no triggers registered the loop is a
  /// no-op — and it guarantees ruleset-defined reactions (armor absorption,
  /// wound checks) fire uniformly for universal and specific mode alike.
  int32_t applyDamage(DynamicEntity &actor, DynamicEntity &target, std::string_view resourceId,
                      int32_t rawDamage, const Json &env = {}) {
    EventData data;
    data.payload = {{"damage", rawDamage},       {"raw_damage", rawDamage},
                    {"attacker_id", actor.id()}, {"attacker_instance_id", actor.entityId().value},
                    {"target_id", target.id()},  {"target_instance_id", target.entityId().value}};
    fireEvent(EventType::OnDamageCalculated, data, actor, &target, env);
    int32_t finalDamage = data.getInt("damage", rawDamage);
    if (finalDamage < 0) {
      finalDamage = 0;
    }
    // Temporary Hit Points are a buffer: they absorb damage before the real
    // hit-point pool ("Lose Temporary Hit Points First").
    if (target.temporaryHitPoints() > 0 && finalDamage > 0) {
      const int32_t absorbed = std::min(target.temporaryHitPoints(), finalDamage);
      target.addTemporaryHitPoints(-absorbed);
      finalDamage -= absorbed;
      data.payload["temporary_hp_absorbed"] = absorbed;
    }
    const int32_t applied = target.modifyResource(resourceId, -finalDamage);
    data.payload["applied_damage"] = -applied;
    fireEvent(EventType::OnDamageTaken, data, actor, &target, env);
    return applied;
  }

  /// Looks up a raw data record by id in a named `data` section (e.g.
  /// "spells", "poisons", "diseases", "conditions", "items", "archetypes",
  /// "creatures"). Returns nullptr when the section or record does not exist.
  ///
  /// @par Why raw JSON?
  /// The `data` database is intentionally free-form — each ruleset's records
  /// carry exactly the fields its source book has. The engine exposes every
  /// record verbatim so an application can read anything the ruleset covers
  /// (magic, illness, equipment, ...) without the engine having to model it.
  [[nodiscard]] const Json *findDataRecord(std::string_view section, std::string_view id) const {
    return rpg_os::findDataRecord(m_ruleset, section, id);
  }

  /// The named data record accessors for the common sections.
  [[nodiscard]] const Json *findSpell(std::string_view id) const {
    return findDataRecord("spells", id);
  }
  [[nodiscard]] const Json *findItem(std::string_view id) const {
    return findDataRecord("items", id);
  }
  [[nodiscard]] const Json *findCondition(std::string_view id) const {
    return findDataRecord("conditions", id);
  }
  [[nodiscard]] const Json *findPoison(std::string_view id) const {
    return findDataRecord("poisons", id);
  }
  [[nodiscard]] const Json *findDisease(std::string_view id) const {
    return findDataRecord("diseases", id);
  }
  [[nodiscard]] const Json *findArchetype(std::string_view id) const {
    return findDataRecord("archetypes", id);
  }
  [[nodiscard]] const Json *findCreature(std::string_view id) const {
    return findDataRecord("creatures", id);
  }

  /// The outcome of casting a spell through @ref castSpell.
  struct SpellResult {
    bool cast{false};         ///< the spell was cast (cost paid; declared check passed)
    CheckResult check;        ///< the casting check, when the spell declares one
    int32_t cost{0};          ///< resource points spent
    int32_t appliedDamage{0}; ///< damage applied to the target (when the spell deals damage)
    std::string resourceId;   ///< the resource pool the cost was drawn from
    std::string denied;       ///< why the cast was refused ("", "no_action", "no_cast", ...)
    std::vector<std::string> choicesRequired; ///< option-group ids needing a caller selection
    std::vector<int32_t> damageDice;          ///< raw damage dice rolled by the spell's effects
  };

  /// Whether `entity` may take the given action ("action", "bonus_action",
  /// "reaction", "move", "speak", "concentrate", "cast"), given its active
  /// restrictions. A caller can query this before attempting an action; the
  /// engine also enforces it in @ref castSpell and the combat helpers.
  ///
  /// Casting a spell requires the standard action, so a creature that cannot
  /// take actions (`no_action`, e.g. paralyzed) also cannot cast — even
  /// without an explicit `no_cast` restriction. The engine reports the *most
  /// specific* reason on @ref SpellResult::denied (an explicit `no_cast` wins
  /// over the implied `no_action`).
  [[nodiscard]] bool actionAllowed(const DynamicEntity &entity, std::string_view action) const {
    if (entity.hasRestriction("no_" + std::string(action))) {
      return false;
    }
    if (action == "cast" && entity.hasRestriction("no_action")) {
      return false;
    }
    return true;
  }

  /// The outcome of applying an affliction through @ref applyAffliction.
  struct AfflictionResult {
    bool resisted{false};      ///< a declared save was passed
    bool saveRolled{false};    ///< a save check was rolled
    int32_t effectsApplied{0}; ///< how many structured effects were applied
  };

  /// Casts a spell from the ruleset's `data.spells` (see @ref SpellResult).
  ///
  /// Applies a successfully-cast spell's structured `effects` (or its bare
  /// `damage` field) to `target`, accumulating the damage dealt and any pending
  /// option-group choices on `result`. Requires the casting check to have
  /// passed — a failed check fizzles (the action and the resource are spent but
  /// the spell has no effect, and no dice are rolled for its damage, so the
  /// combat log can trust that a fizzle changes no stat).
  template <RandomNumberGenerator Rng>
  void applySpellEffects(DynamicEntity &actor, DynamicEntity *target, const Json *spell,
                         SpellResult &result, const CheckParams &params, Rng &rng,
                         int32_t qualityLevel,
                         const std::unordered_map<std::string, int32_t> *selections) {
    if (!result.cast || target == nullptr) {
      return;
    }
    if (spell->contains("effects") && spell->at("effects").is_array()) {
      const EffectsResult effects = resolveEffects(actor, *target, spell->at("effects"), params,
                                                   rng, qualityLevel, selections);
      result.appliedDamage = effects.damageDealt;
      result.choicesRequired = effects.choicesRequired;
      result.damageDice = effects.damageDice;
    } else if (spell->contains("damage")) {
      // `applyDamage` reports the (negative) pool delta, so negate it into the
      // positive "damage dealt" the caller expects. The dice are recorded the
      // same observation-only way as the effects path.
      const Json &damageJson = spell->at("damage");
      int32_t damage = 0;
      if (damageJson.is_string()) {
        const DiceExpression diceExpr(damageJson.get<std::string>());
        const std::vector<int> rolled = diceExpr.roll(rng);
        damage = diceExpr.constant();
        const auto &groups = diceExpr.dice();
        for (std::size_t g = 0, i = 0; g < groups.size(); ++g) {
          for (int j = 0; j < groups[g].count; ++j, ++i) {
            damage += groups[g].sign * rolled[i];
            result.damageDice.push_back(rolled[i]);
          }
        }
      } else {
        damage = readVariantValue(damageJson, Variance::Random, rng);
      }
      const std::string hitPool = resolveHitPointPoolId();
      if (!hitPool.empty()) {
        result.appliedDamage = -applyDamage(actor, *target, hitPool, damage);
      }
    }
  }

  /// The casting is fully data-driven: the cost is read from the spell record
  /// (`cost` / `ae_cost`, or `level` — one point per level), drawn from the
  /// ruleset's declared `spell_resource` (or the caller-supplied resource),
  /// and the spell's `check` field (a named check type or an "A/B/C"
  /// attribute list) is resolved when present. A spell with `damage` applies
  /// the rolled damage to the target's primary hit-point pool through the
  /// event pipeline. Throws std::invalid_argument for an unknown spell.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] SpellResult castSpell(std::string_view spellId, DynamicEntity &actor,
                                      DynamicEntity *target, const CheckParams &params, Rng &rng) {
    return castSpell(spellId, actor, target, spellResourceId(), params, rng);
  }

  /// As above, but draws the cost from `resourceId` instead of the ruleset's
  /// declared spell resource. `selections` (optional) resolves the spell's
  /// `options` effects: a map of option-group id -> chosen option index.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] SpellResult
  castSpell(std::string_view spellId, DynamicEntity &actor, DynamicEntity *target,
            std::string_view resourceId, const CheckParams &params, Rng &rng,
            const std::unordered_map<std::string, int32_t> *selections = nullptr) {
    SpellResult result;
    const Json *spell = findSpell(spellId);
    if (spell == nullptr) {
      throw std::invalid_argument("unknown spell '" + std::string(spellId) + "'");
    }
    // The actor's restrictions are enforced: a creature that cannot take
    // actions (incapacitated, stunned, ...) or cannot cast is refused before
    // any resource is spent, with the reason recorded on the result.
    if (!actionAllowed(actor, "action") || !actionAllowed(actor, "cast")) {
      result.cast = false;
      result.denied = actor.hasRestriction("no_cast") ? "no_cast" : "no_action";
      return result;
    }
    // Cost: `cost` or `ae_cost`, else `level` (one point per level).
    result.cost = rpg_os::spellCost(*spell);
    result.resourceId = std::string(resourceId);
    if (result.cost > 0 && !resourceId.empty()) {
      if (actor.resource(resourceId) < result.cost) {
        result.cast = false; // cannot afford the spell
        return result;
      }
      (void)actor.modifyResource(resourceId, -result.cost);
    }

    // Casting check (named check type, or an "A/B/C" attribute list).
    if (spell->contains("check") && spell->at("check").is_string()) {
      const std::string checkText = spell->at("check").get<std::string>();
      if (m_ruleset.findCheckType(checkText) != nullptr) {
        const NullStatProvider noTarget;
        result.check =
            target != nullptr
                ? CheckResolver::resolve(m_ruleset, actor, *target, checkText, params, rng)
                : CheckResolver::resolve(m_ruleset, actor, noTarget, checkText, params, rng);
      } else {
        result.check = resolveSpellCheck(actor, checkText, params, rng);
      }
      result.cast = result.check.isSuccess;
    } else {
      result.cast = true;
    }

    // Structured effects take precedence over the bare `damage` field: they
    // are the extracted, machine-readable form of the spell's prose (saves,
    // half-on-save damage, conditions, healing, resistances). A spell with no
    // `effects` falls back to the simple damage field. Both paths require the
    // casting check to have passed: a failed check fizzles — the action and
    // the resource are spent, but the spell has no effect (and no dice are
    // rolled for its damage), so the combat log can trust that a fizzle
    // changes no stat.
    if (result.cast) {
      // The casting check's quality level (The Dark Eye's QL) is threaded into
      // the effect formulas so QL-scaled spells ("2D6 + QLx2") resolve from
      // data alone.
      applySpellEffects(actor, target, spell, result, params, rng, result.check.qualityLevel,
                        selections);
    }

    // Announce the cast attempt (cast = true when it went off). Refused casts
    // (not allowed to act / not enough resource) return before reaching here
    // and are not announced; a failed casting check still announces with
    // cast = false so UI can narrate the fizzle.
    EventData data;
    data.payload = {{"spell", std::string(spellId)},
                    {"actor_id", actor.id()},
                    {"instance_id", actor.entityId().value},
                    {"cast", result.cast}};
    fireEvent(EventType::OnSpellCast, data, actor, target, Json{});
    return result;
  }

  /// Casts a spell using a fresh default RNG (convenience overload).
  [[nodiscard]] SpellResult castSpell(std::string_view spellId, DynamicEntity &actor,
                                      DynamicEntity *target, const CheckParams &params) {
    DefaultRandom rng;
    return castSpell(spellId, actor, target, params, rng);
  }

  /// Casts a spell with a fresh default RNG, resolving the spell's `options`
  /// effects via `selections` (option-group id -> chosen option index).
  [[nodiscard]] SpellResult castSpell(std::string_view spellId, DynamicEntity &actor,
                                      DynamicEntity *target, const CheckParams &params,
                                      const std::unordered_map<std::string, int32_t> &selections) {
    DefaultRandom rng;
    return castSpell(spellId, actor, target, spellResourceId(), params, rng, &selections);
  }
  /// `victim` (see @ref AfflictionResult).
  ///
  /// The application is fully data-driven: if the record declares a `save`
  /// stat, the victim rolls a generic roll-under check against it and the
  /// affliction is resisted on success. Otherwise (or on a failed save) every
  /// structured `effects[]` entry is applied: a resource pool id damages that
  /// pool, a condition id (or the effect's `condition` field) applies stacks,
  /// and any other stat id is reduced by `amount`. Prose-only effects are left
  /// for the caller, who can read the full record via @ref findPoison /
  /// @ref findDisease. Throws std::invalid_argument for an unknown record.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] AfflictionResult applyAffliction(std::string_view section, std::string_view id,
                                                 DynamicEntity &victim, const CheckParams &params,
                                                 Rng &rng) {
    AfflictionResult result;
    const Json *affliction = findDataRecord(section, id);
    if (affliction == nullptr) {
      throw std::invalid_argument("unknown " + std::string(section) + " '" + std::string(id) + "'");
    }
    if (affliction->contains("save") && affliction->at("save").is_string()) {
      const std::string save = affliction->at("save").get<std::string>();
      if (m_ruleset.hasStat(save)) {
        result.saveRolled = true;
        CheckRecipe recipe;
        recipe.resolution = Resolution::Threshold;
        recipe.dice = "1d20"_dice;
        recipe.comparison = Comparison::LessEqual;
        recipe.thresholdSource = ThresholdSource::ActorStat;
        recipe.thresholdStat = save;
        recipe.difficultyMode = DifficultyMode::ToStat;
        const CheckResult saveResult =
            resolveCheck(victim, NullStatProvider{}, recipe, params, rng);
        if (saveResult.isSuccess) {
          result.resisted = true;
          return result;
        }
      }
    }
    if (affliction->contains("effects") && affliction->at("effects").is_array()) {
      std::vector<std::string> appliedConditions;
      for (const Json &effect : affliction->at("effects")) {
        const std::string stat = effect.value("stat", "");
        if (stat.empty()) {
          continue;
        }
        int32_t amount = 1;
        if (effect.contains("amount")) {
          amount = readVariantValue(effect.at("amount"), Variance::Random, rng);
        }
        if (m_ruleset.isResourcePool(stat)) {
          (void)victim.modifyResource(stat, -amount);
          ++result.effectsApplied;
        } else if (effect.contains("condition") && effect.at("condition").is_string()) {
          const std::string conditionId = effect.at("condition").get<std::string>();
          victim.addCondition(conditionId, amount);
          appliedConditions.push_back(conditionId);
          ++result.effectsApplied;
        } else if (m_ruleset.isCondition(stat)) {
          victim.addCondition(stat, amount);
          appliedConditions.push_back(stat);
          ++result.effectsApplied;
        } else if (m_ruleset.findAttribute(stat) != nullptr) {
          victim.setBaseAttribute(stat, victim.baseAttribute(stat) - amount);
          ++result.effectsApplied;
        }
      }
      // Remember what was applied so an application can cure it later.
      if (result.effectsApplied > 0) {
        victim.addAffliction(section, id, appliedConditions);
        EventData data;
        data.payload = {{"section", std::string(section)},
                        {"affliction", std::string(id)},
                        {"victim_id", victim.id()},
                        {"effects_applied", result.effectsApplied}};
        fireEvent(EventType::OnAfflictionApplied, data, victim, nullptr, Json{});
      }
    }
    return result;
  }

  /// Applies an affliction using a fresh default RNG (convenience overload).
  [[nodiscard]] AfflictionResult applyAffliction(std::string_view section, std::string_view id,
                                                 DynamicEntity &victim, const CheckParams &params) {
    DefaultRandom rng;
    return applyAffliction(section, id, victim, params, rng);
  }

  // ------------------------------------------------------------------------
  // Bookkeeping: economy
  // ------------------------------------------------------------------------

  /// Whether the loaded ruleset declares a currency system.
  [[nodiscard]] bool hasCurrency() const noexcept {
    return m_ruleset.hasCurrency();
  }

  /// The loaded ruleset's currency system (valid only when hasCurrency()).
  [[nodiscard]] const CurrencySystem &currencySystem() const noexcept {
    return m_ruleset.currencySystem;
  }

  /// The price of one unit of `itemId`, parsed from the item record's `cost` /
  /// `price` / `value` field into Money. A record without a price field is
  /// free (Money{0}); a non-numeric, non-parseable price yields
  /// UnknownDenomination. UnknownItem / UnknownCurrency otherwise.
  [[nodiscard]] std::expected<Money, BookkeepingError> itemPrice(std::string_view itemId) const {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    const Json *item = findItem(itemId);
    if (item == nullptr) {
      return std::unexpected(BookkeepingError::UnknownItem);
    }
    if (!m_ruleset.hasCurrency()) {
      return std::unexpected(BookkeepingError::UnknownCurrency);
    }
    const Json *price = nullptr;
    if (item->contains("cost")) {
      price = &item->at("cost");
    } else if (item->contains("price")) {
      price = &item->at("price");
    } else if (item->contains("value")) {
      price = &item->at("value");
    }
    if (price == nullptr) {
      return Money{0};
    }
    if (price->is_number_integer()) {
      return Money{price->get<int64_t>()};
    }
    if (price->is_string()) {
      return parsePriceString(price->get<std::string>());
    }
    return Money{0};
  }

  /// Adds `quantity` of `itemId` to a sheet's inventory. Fires OnItemAdded.
  [[nodiscard]] std::expected<void, BookkeepingError>
  addItem(DynamicEntity &sheet, std::string_view itemId, int32_t quantity = 1) {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    if (findItem(itemId) == nullptr) {
      return std::unexpected(BookkeepingError::UnknownItem);
    }
    sheet.inventory().add(ItemInstance{std::string(itemId), quantity, {}});
    EventData data;
    data.payload = {
        {"item", std::string(itemId)}, {"quantity", quantity}, {"owner_id", sheet.id()}};
    fireEvent(EventType::OnItemAdded, data, sheet, nullptr, Json{});
    return {};
  }

  /// Removes `quantity` of `itemId` from a sheet's inventory; ItemNotOwned when
  /// the sheet does not carry enough. Fires OnItemRemoved.
  [[nodiscard]] std::expected<void, BookkeepingError>
  removeItem(DynamicEntity &sheet, std::string_view itemId, int32_t quantity = 1) {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    if (findItem(itemId) == nullptr) {
      return std::unexpected(BookkeepingError::UnknownItem);
    }
    if (!sheet.inventory().remove(itemId, quantity)) {
      return std::unexpected(BookkeepingError::ItemNotOwned);
    }
    EventData data;
    data.payload = {
        {"item", std::string(itemId)}, {"quantity", quantity}, {"owner_id", sheet.id()}};
    fireEvent(EventType::OnItemRemoved, data, sheet, nullptr, Json{});
    return {};
  }

  /// Transfers `amount` from one sheet's wealth to another's (null recipient =
  /// the money leaves the economy, e.g. a tax). NotEnoughMoney when the payer
  /// cannot cover it. Fires OnCurrencyChanged.
  [[nodiscard]] std::expected<void, BookkeepingError> pay(DynamicEntity &from, DynamicEntity *to,
                                                          Money amount) {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    if (amount.isNegative() || from.money() < amount) {
      return std::unexpected(BookkeepingError::NotEnoughMoney);
    }
    from.money() -= amount;
    if (to != nullptr) {
      to->money() += amount;
    }
    EventData data;
    data.payload = {{"amount", amount.baseUnits()},
                    {"payer_id", from.id()},
                    {"recipient_id", to != nullptr ? to->id() : std::string{}}};
    fireEvent(EventType::OnCurrencyChanged, data, from, to, Json{});
    return {};
  }

  /// Buys `quantity` of `itemId` for `buyer` from `seller` (null seller = the
  /// money leaves the economy). Returns the total price paid. Fires the item
  /// and currency events.
  [[nodiscard]] std::expected<Money, BookkeepingError>
  buy(DynamicEntity &buyer, DynamicEntity *seller, std::string_view itemId, int32_t quantity = 1) {
    auto price = itemPrice(itemId);
    if (!price) {
      return std::unexpected(price.error());
    }
    const Money total = (*price) * quantity;
    auto paid = pay(buyer, seller, total);
    if (!paid) {
      return std::unexpected(paid.error());
    }
    auto added = addItem(buyer, itemId, quantity);
    if (!added) {
      return std::unexpected(added.error());
    }
    return total;
  }

  // ------------------------------------------------------------------------
  // Bookkeeping: equipment & encumbrance
  // ------------------------------------------------------------------------

  /// The outcome of equipping an item.
  struct EquipResult {
    std::string slotId;         ///< the slot filled
    std::string itemId;         ///< the item equipped
    std::string previousItemId; ///< item that was in the slot (empty = free)
    int32_t quantityBefore{0};  ///< how many of the item were carried before
  };

  /// Equips `itemId` into `slotId` on `sheet`. The slot must be declared by
  /// the ruleset, the item must exist, its `slot` field (when present) must
  /// match, and the sheet must carry the item. An occupied slot is swapped:
  /// the old item returns to the inventory. Fires OnEquipChanged.
  [[nodiscard]] std::expected<EquipResult, BookkeepingError>
  equip(DynamicEntity &sheet, std::string_view slotId, std::string_view itemId) {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    bool slotOk = false;
    for (const std::string &slot : m_ruleset.equipmentSlots) {
      if (slot == slotId) {
        slotOk = true;
        break;
      }
    }
    if (!slotOk) {
      return std::unexpected(BookkeepingError::SlotMismatch);
    }
    const Json *item = findItem(itemId);
    if (item == nullptr) {
      return std::unexpected(BookkeepingError::UnknownItem);
    }
    const std::string itemSlot = item->value("slot", "");
    if (!itemSlot.empty() && itemSlot != slotId) {
      return std::unexpected(BookkeepingError::SlotMismatch);
    }
    if (sheet.inventory().count(itemId) <= 0) {
      return std::unexpected(BookkeepingError::ItemNotOwned);
    }
    EquipResult result;
    result.slotId = std::string(slotId);
    result.itemId = std::string(itemId);
    result.quantityBefore = sheet.inventory().count(itemId);
    if (sheet.equipment().isEquipped(slotId)) {
      result.previousItemId = std::string(sheet.equipment().itemIn(slotId));
      (void)sheet.equipment().unequip(slotId);
      if (!result.previousItemId.empty()) {
        sheet.inventory().add(ItemInstance{result.previousItemId, 1, {}});
      }
    }
    (void)sheet.equipment().equip(slotId, itemId);
    (void)sheet.inventory().remove(itemId, 1);
    EventData data;
    data.payload = {
        {"slot", std::string(slotId)}, {"item", std::string(itemId)}, {"owner_id", sheet.id()}};
    fireEvent(EventType::OnEquipChanged, data, sheet, nullptr, Json{});
    return result;
  }

  /// Unequips `slotId` on `sheet`; the item returns to the inventory. Fails
  /// with NotEquipped when the slot is free. Fires OnEquipChanged.
  [[nodiscard]] std::expected<void, BookkeepingError> unequip(DynamicEntity &sheet,
                                                              std::string_view slotId) {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    if (!sheet.equipment().isEquipped(slotId)) {
      return std::unexpected(BookkeepingError::NotEquipped);
    }
    const std::string itemId = std::string(sheet.equipment().unequip(slotId));
    sheet.inventory().add(ItemInstance{itemId, 1, {}});
    EventData data;
    data.payload = {{"slot", std::string(slotId)}, {"item", itemId}, {"owner_id", sheet.id()}};
    fireEvent(EventType::OnEquipChanged, data, sheet, nullptr, Json{});
    return {};
  }

  /// Per-unit weight of `itemId` from its record's `weight` field (parsed),
  /// or the ruleset's default item weight when the record carries none.
  [[nodiscard]] double itemWeight(std::string_view itemId) const {
    const EncumbranceConfig &cfg = m_ruleset.encumbrance;
    double unit = cfg.defaultItemWeight;
    const Json *item = findItem(itemId);
    if (item != nullptr && item->contains("weight")) {
      const Json &weightField = item->at("weight");
      if (weightField.is_number()) {
        unit = weightField.get<double>();
      } else if (weightField.is_string()) {
        const double parsed = parseWeightValue(weightField.get<std::string>());
        if (parsed >= 0.0) {
          unit = parsed;
        }
      }
    }
    return unit;
  }

  /// Per-unit size of `itemId` from its record's `size` field, or the
  /// ruleset's default item size when the record carries none.
  [[nodiscard]] double itemSize(std::string_view itemId) const {
    const EncumbranceConfig &cfg = m_ruleset.encumbrance;
    double unit = cfg.defaultItemSize;
    const Json *item = findItem(itemId);
    if (item != nullptr && item->contains("size") && item->at("size").is_number()) {
      unit = item->at("size").get<double>();
    }
    return unit;
  }

  /// Total weight the sheet carries: every flat item, everything inside
  /// containers (bag-in-bags, visited recursively), plus equipped gear. Each
  /// item's weight comes from its record's `weight` field (parsed) or the
  /// ruleset's default.
  [[nodiscard]] Weight carriedWeight(const DynamicEntity &sheet) const {
    return Weight{carriedLoad(sheet).weight, m_ruleset.encumbrance.weightUnit};
  }

  /// How much the sheet carries across all three capacity axes (weight, size,
  /// item count): every flat item, everything inside containers (visited
  /// recursively), plus equipped gear. Weight and size come from each item
  /// record's `weight` / `size` field (or the ruleset defaults); the item
  /// count is the sum of all quantities.
  [[nodiscard]] CarriedLoad carriedLoad(const DynamicEntity &sheet) const {
    CarriedLoad load;
    const auto addItem = [&](std::string_view itemId, int32_t quantity) {
      load.weight += itemWeight(itemId) * quantity;
      load.size += itemSize(itemId) * quantity;
      load.items += quantity;
    };
    sheet.inventory().visitStacks(
        [&](const ItemInstance &stack) { addItem(stack.itemId, stack.quantity); });
    for (const auto &[slot, itemId] : sheet.equipment().slots()) {
      addItem(itemId, 1);
    }
    return load;
  }

  /// Adds `quantity` of `itemId` into the container `containerItemId` (which
  /// must be carried as a single unit). UnknownContainer / UnknownItem
  /// otherwise; a container that declares a `capacity` refuses items that
  /// would overflow it with OverCapacity. Fires OnItemAdded.
  [[nodiscard]] std::expected<void, BookkeepingError>
  addItemToContainer(DynamicEntity &sheet, std::string_view containerItemId,
                     std::string_view itemId, int32_t quantity = 1) {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    if (findItem(itemId) == nullptr) {
      return std::unexpected(BookkeepingError::UnknownItem);
    }
    if (sheet.inventory().count(containerItemId) != 1) {
      return std::unexpected(BookkeepingError::UnknownContainer);
    }
    auto fits = canAddToContainer(sheet, containerItemId, itemId, quantity);
    if (!fits) {
      return std::unexpected(fits.error());
    }
    if (!*fits) {
      return std::unexpected(BookkeepingError::OverCapacity);
    }
    if (!sheet.inventory().putInto(containerItemId,
                                   ItemInstance{std::string(itemId), quantity, {}})) {
      return std::unexpected(BookkeepingError::UnknownContainer);
    }
    EventData data;
    data.payload = {{"item", std::string(itemId)},
                    {"container", std::string(containerItemId)},
                    {"quantity", quantity},
                    {"owner_id", sheet.id()}};
    fireEvent(EventType::OnItemAdded, data, sheet, nullptr, Json{});
    return {};
  }

  /// Removes `quantity` of `itemId` from inside `containerItemId`. Returns
  /// ItemNotOwned when the container or the item is not present in it.
  [[nodiscard]] std::expected<void, BookkeepingError>
  removeItemFromContainer(DynamicEntity &sheet, std::string_view containerItemId,
                          std::string_view itemId, int32_t quantity = 1) {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    if (findItem(itemId) == nullptr) {
      return std::unexpected(BookkeepingError::UnknownItem);
    }
    if (!sheet.inventory().removeFrom(containerItemId, itemId, quantity)) {
      return std::unexpected(BookkeepingError::ItemNotOwned);
    }
    EventData data;
    data.payload = {{"item", std::string(itemId)},
                    {"container", std::string(containerItemId)},
                    {"quantity", quantity},
                    {"owner_id", sheet.id()}};
    fireEvent(EventType::OnItemRemoved, data, sheet, nullptr, Json{});
    return {};
  }

  /// The sheet's carrying capacity, from the ruleset's `encumbrance.capacity`
  /// formula evaluated against the sheet. NoEncumbrance when the ruleset has
  /// no carrying rules.
  [[nodiscard]] std::expected<Weight, BookkeepingError>
  carryingCapacity(const DynamicEntity &sheet) const {
    const EncumbranceConfig &cfg = m_ruleset.encumbrance;
    if (!cfg.enabled || cfg.capacityText.empty()) {
      return std::unexpected(BookkeepingError::NoEncumbrance);
    }
    const Json emptyEnv = Json::object();
    const Json emptyParams = Json::object();
    const EntityContext context(sheet, nullptr, emptyEnv, emptyParams);
    return Weight{cfg.capacity.evaluate(context), cfg.weightUnit};
  }

  /// The encumbrance level of `sheet`: 0 = unencumbered, then one per level
  /// whose carried/capacity ratio ceiling the sheet is at or under. Returns 0
  /// when the ruleset has no carrying rules.
  [[nodiscard]] std::expected<int32_t, BookkeepingError>
  encumbranceLevel(const DynamicEntity &sheet) const {
    const EncumbranceConfig &cfg = m_ruleset.encumbrance;
    if (!cfg.enabled || cfg.capacityText.empty() || cfg.levels.empty()) {
      return 0;
    }
    auto capacity = carryingCapacity(sheet);
    if (!capacity) {
      return 0;
    }
    if (capacity->value <= 0.0) {
      return 0;
    }
    const double ratio = carriedWeight(sheet).value / capacity->value;
    int32_t level = 0;
    for (std::size_t i = 0; i < cfg.levels.size(); ++i) {
      if (ratio <= cfg.levels[i].maxRatio) {
        level = static_cast<int32_t>(i);
        break;
      }
      level = static_cast<int32_t>(cfg.levels.size());
    }
    return level;
  }

  /// Applies the condition of the sheet's current encumbrance level and clears
  /// the other levels' conditions (bookkeeping for "you are now encumbered").
  [[nodiscard]] std::expected<void, BookkeepingError>
  updateEncumbrance(DynamicEntity &sheet) const {
    const EncumbranceConfig &cfg = m_ruleset.encumbrance;
    auto level = encumbranceLevel(sheet);
    if (!level) {
      return std::unexpected(level.error());
    }
    for (std::size_t i = 0; i < cfg.levels.size(); ++i) {
      if (cfg.levels[i].conditionId.empty()) {
        continue;
      }
      if (static_cast<int32_t>(i) == *level) {
        sheet.addCondition(cfg.levels[i].conditionId, 1);
        dispatchEvent(EventType::OnConditionChanged, Json{{"condition", cfg.levels[i].conditionId},
                                                          {"stacks", 1},
                                                          {"action", "applied"},
                                                          {"actor_id", sheet.id()},
                                                          {"instance_id", sheet.entityId().value}});
      } else {
        sheet.removeCondition(cfg.levels[i].conditionId);
        dispatchEvent(EventType::OnConditionChanged, Json{{"condition", cfg.levels[i].conditionId},
                                                          {"stacks", 0},
                                                          {"action", "removed"},
                                                          {"actor_id", sheet.id()},
                                                          {"instance_id", sheet.entityId().value}});
      }
    }
    return {};
  }

  /// The sheet's carrying limits across all three axes (weight / size / item
  /// count), from the ruleset's `encumbrance` section. An axis with no rule
  /// is 0 (unlimited); @ref CarryingCapacity::enabled is false when the
  /// ruleset declares no carrying rules at all.
  [[nodiscard]] CarryingCapacity capacity(const DynamicEntity &sheet) const {
    const EncumbranceConfig &cfg = m_ruleset.encumbrance;
    CarryingCapacity cap;
    if (!cfg.enabled) {
      return cap;
    }
    const Json emptyEnv = Json::object();
    const Json emptyParams = Json::object();
    const EntityContext context(sheet, nullptr, emptyEnv, emptyParams);
    if (!cfg.capacityText.empty()) {
      cap.weight = cfg.capacity.evaluate(context);
    }
    if (!cfg.sizeCapacityText.empty()) {
      cap.size = cfg.sizeCapacity.evaluate(context);
    }
    if (!cfg.itemCapacityText.empty()) {
      cap.items = static_cast<int32_t>(cfg.itemCapacity.evaluate(context));
    }
    cap.enabled =
        !cfg.capacityText.empty() || !cfg.sizeCapacityText.empty() || !cfg.itemCapacityText.empty();
    return cap;
  }

  /// The sheet's current carrying status: what it carries, its limits, and
  /// which (if any) axis is at or over its limit.
  [[nodiscard]] InventoryStatus inventoryStatus(const DynamicEntity &sheet) const {
    InventoryStatus status;
    status.carried = carriedLoad(sheet);
    status.capacity = capacity(sheet);
    const double weightLimit = status.capacity.weight;
    const double sizeLimit = status.capacity.size;
    const int32_t itemLimit = status.capacity.items;
    status.overWeight = weightLimit > 0.0 && status.carried.weight > weightLimit;
    status.overSize = sizeLimit > 0.0 && status.carried.size > sizeLimit;
    status.overItems = itemLimit > 0 && status.carried.items > itemLimit;
    status.atWeightLimit = weightLimit > 0.0 && status.carried.weight >= weightLimit;
    status.atSizeLimit = sizeLimit > 0.0 && status.carried.size >= sizeLimit;
    status.atItemsLimit = itemLimit > 0 && status.carried.items >= itemLimit;
    return status;
  }

  /// Whether adding `quantity` of `itemId` would stay within every carrying
  /// limit (weight, size, item count) of `sheet`. UnknownItem when the item
  /// is not in the data database; a ruleset without carrying rules always
  /// allows.
  [[nodiscard]] std::expected<bool, BookkeepingError>
  canCarry(const DynamicEntity &sheet, std::string_view itemId, int32_t quantity = 1) const {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    if (findItem(itemId) == nullptr) {
      return std::unexpected(BookkeepingError::UnknownItem);
    }
    const InventoryStatus status = inventoryStatus(sheet);
    if (!status.capacity.enabled) {
      return true;
    }
    const double addedWeight = itemWeight(itemId) * quantity;
    const double addedSize = itemSize(itemId) * quantity;
    const bool overWeight = status.capacity.weight > 0.0 &&
                            status.carried.weight + addedWeight > status.capacity.weight;
    const bool overSize =
        status.capacity.size > 0.0 && status.carried.size + addedSize > status.capacity.size;
    const bool overItems =
        status.capacity.items > 0 && status.carried.items + quantity > status.capacity.items;
    return !overWeight && !overSize && !overItems;
  }

  /// The carrying limits declared by the container item `containerItemId`
  /// (its `capacity` field). UnknownItem when the item is unknown; an item
  /// without a `capacity` field is not a limited container
  /// (@ref CarryingCapacity::enabled == false).
  [[nodiscard]] std::expected<CarryingCapacity, BookkeepingError>
  containerCapacity(std::string_view containerItemId) const {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    const Json *item = findItem(containerItemId);
    if (item == nullptr) {
      return std::unexpected(BookkeepingError::UnknownItem);
    }
    CarryingCapacity cap;
    if (!item->contains("capacity") || !item->at("capacity").is_object()) {
      return cap;
    }
    const Json &capacityField = item->at("capacity");
    cap.weight = capacityField.value("weight", 0.0);
    cap.size = capacityField.value("size", 0.0);
    cap.items = capacityField.value("items", 0);
    cap.enabled = cap.weight > 0.0 || cap.size > 0.0 || cap.items > 0;
    return cap;
  }

  /// How much is currently stored inside the container `containerItemId`
  /// (weight / size / item count of its contents, nested containers walked
  /// recursively). An absent container returns an empty load.
  [[nodiscard]] CarriedLoad containerLoad(const DynamicEntity &sheet,
                                          std::string_view containerItemId) const {
    CarriedLoad load;
    const std::vector<ItemInstance> *contents = sheet.inventory().contentsOf(containerItemId);
    if (contents == nullptr) {
      return load;
    }
    const auto addItem = [&](auto &&self, const ItemInstance &stack) -> void {
      load.weight += itemWeight(stack.itemId) * stack.quantity;
      load.size += itemSize(stack.itemId) * stack.quantity;
      load.items += stack.quantity;
      for (const ItemInstance &nested : stack.contents) {
        self(self, nested);
      }
    };
    for (const ItemInstance &stack : *contents) {
      addItem(addItem, stack);
    }
    return load;
  }

  /// Whether adding `quantity` of `itemId` inside the container
  /// `containerItemId` would stay within the container's own capacity
  /// (weight / size / item count). UnknownItem / UnknownContainer for
  /// unknown item or (non-single-unit) container; a container without a
  /// `capacity` field always accepts.
  [[nodiscard]] std::expected<bool, BookkeepingError>
  canAddToContainer(const DynamicEntity &sheet, std::string_view containerItemId,
                    std::string_view itemId, int32_t quantity = 1) const {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    if (findItem(itemId) == nullptr) {
      return std::unexpected(BookkeepingError::UnknownItem);
    }
    if (sheet.inventory().count(containerItemId) != 1) {
      return std::unexpected(BookkeepingError::UnknownContainer);
    }
    auto cap = containerCapacity(containerItemId);
    if (!cap) {
      return std::unexpected(cap.error());
    }
    if (!cap->enabled) {
      return true;
    }
    const CarriedLoad inside = containerLoad(sheet, containerItemId);
    const double addedWeight = itemWeight(itemId) * quantity;
    const double addedSize = itemSize(itemId) * quantity;
    const bool overWeight = cap->weight > 0.0 && inside.weight + addedWeight > cap->weight;
    const bool overSize = cap->size > 0.0 && inside.size + addedSize > cap->size;
    const bool overItems = cap->items > 0 && inside.items + quantity > cap->items;
    return !overWeight && !overSize && !overItems;
  }

  // ------------------------------------------------------------------------
  // Bookkeeping: movement & terrain
  // ------------------------------------------------------------------------

  /// The ruleset's movement configuration (empty when it declares no movement
  /// rules — movement is then unconstrained: every mode possible, no
  /// exhaustion, full regeneration).
  [[nodiscard]] const MovementConfig &movementConfig() const noexcept {
    return m_ruleset.movement;
  }

  /// Whether the loaded ruleset declares any movement rules.
  [[nodiscard]] bool hasMovement() const noexcept {
    return !m_ruleset.movement.modes.empty();
  }

  /// Looks up a terrain by id; nullptr when unknown. The empty id resolves to
  /// the ruleset's default terrain (nullptr when the ruleset has no movement).
  [[nodiscard]] const TerrainDef *findTerrain(std::string_view terrainId) const {
    const MovementConfig &mov = m_ruleset.movement;
    if (mov.terrains.empty()) {
      return nullptr;
    }
    const std::string id = terrainId.empty() ? mov.defaultTerrain : std::string(terrainId);
    const auto it = mov.terrains.find(id);
    return it == mov.terrains.end() ? nullptr : &it->second;
  }

  /// The terrain id in effect for `sheet`: its declared current terrain, else
  /// the ruleset's default terrain ("" when the ruleset has no movement).
  [[nodiscard]] std::string effectiveTerrain(const DynamicEntity &sheet) const {
    if (!sheet.terrain().empty()) {
      return sheet.terrain();
    }
    return m_ruleset.movement.defaultTerrain;
  }

  /// The base speed of movement mode `modeId` evaluated against `sheet`
  /// (before terrain and load multipliers). 0 when unconstrained or the mode
  /// is unknown.
  [[nodiscard]] double baseMovementSpeed(const DynamicEntity &sheet,
                                         std::string_view modeId) const {
    // A sheet's own declared movement speed (a creature's stat-block speed)
    // takes precedence over the ruleset's default mode formula.
    if (sheet.hasMovementSpeed(modeId)) {
      return sheet.movementSpeed(modeId);
    }
    const MovementConfig &mov = m_ruleset.movement;
    const auto it = mov.modes.find(std::string(modeId));
    if (it == mov.modes.end()) {
      return 0.0;
    }
    const Json emptyEnv = Json::object();
    const Json emptyParams = Json::object();
    const EntityContext context(sheet, nullptr, emptyEnv, emptyParams);
    return it->second.speed.evaluate(context);
  }

  /// The speed factor the carried load applies to every movement mode: from
  /// the movement section's `load.levels` (carried/capacity ratio -> factor),
  /// 1.0 when the ruleset has no carrying rules or no load steps.
  [[nodiscard]] double loadSpeedFactor(const DynamicEntity &sheet) const {
    const MovementConfig &mov = m_ruleset.movement;
    if (mov.loadLevels.empty()) {
      return 1.0;
    }
    auto capacity = carryingCapacity(sheet);
    if (!capacity || capacity->value <= 0.0) {
      return 1.0;
    }
    const double ratio = carriedWeight(sheet).value / capacity->value;
    double factor = mov.loadLevels.back().factor;
    for (const LoadSpeedLevel &level : mov.loadLevels) {
      if (ratio <= level.maxRatio) {
        factor = level.factor;
        break;
      }
    }
    return factor;
  }

  /// One movement option for `sheet` in `modeId` within `terrainId` (empty =
  /// the sheet's effective terrain): effective speed (base x mode factor x
  /// terrain factor x load factor), whether the mode is possible here (a
  /// terrain may forbid it or require a capability), and the per-unit
  /// exhaustion cost. UnknownMode / UnknownTerrain for ids the ruleset does
  /// not declare; when the ruleset has no movement rules at all, any mode is
  /// possible, unconstrained (speed 0), and free of exhaustion.
  [[nodiscard]] std::expected<MovementOption, BookkeepingError>
  movementSpeed(const DynamicEntity &sheet, std::string_view modeId,
                std::string_view terrainId = {}) const {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    const MovementConfig &mov = m_ruleset.movement;
    if (mov.modes.empty() && !sheet.hasMovementSpeed(modeId)) {
      return MovementOption{
          std::string(modeId), std::string(modeId), 0.0, 0.0, true, {}, 0.0, true};
    }
    const TerrainDef *terrain = findTerrain(terrainId);
    if (terrain == nullptr) {
      return std::unexpected(BookkeepingError::UnknownTerrain);
    }
    const auto modeIt = mov.modes.find(std::string(modeId));
    if (modeIt == mov.modes.end() && !sheet.hasMovementSpeed(modeId)) {
      return std::unexpected(BookkeepingError::UnknownMode);
    }
    // A movement mode the ruleset does not declare but the creature does (e.g.
    // a bestiary entry that flies in a ruleset without a fly mode) uses a
    // synthesized default (full speed, no base exhaustion) so its own speed is
    // still queryable through the terrain rules.
    MovementModeDef synthesized;
    const MovementModeDef *mode = &synthesized;
    if (modeIt != mov.modes.end()) {
      mode = &modeIt->second;
    } else {
      synthesized.id = std::string(modeId);
      synthesized.name = std::string(modeId);
      synthesized.speedText = "0";
      synthesized.speed = Expression("0");
      synthesized.factor = 1.0;
      synthesized.exhaustion = 0.0;
    }
    MovementOption option;
    option.modeId = mode->id;
    option.modeName = mode->name;
    option.baseSpeed = baseMovementSpeed(sheet, mode->id);
    const TerrainModeRule *rule = nullptr;
    const auto ruleIt = terrain->modes.find(mode->id);
    if (ruleIt != terrain->modes.end()) {
      rule = &ruleIt->second;
    }
    const double terrainFactor = rule != nullptr ? rule->speedFactor : 1.0;
    const double costFactor = rule != nullptr ? rule->costFactor : 1.0;
    option.speed = option.baseSpeed * mode->factor * terrainFactor * loadSpeedFactor(sheet);
    option.exhaustionPerUnit = mov.exhaustionPerDistance * mode->exhaustion * costFactor;
    option.regeneration = terrain->regeneration != "none";
    if (rule != nullptr && !rule->possible) {
      option.possible = false;
      option.reason = "not possible in " + terrain->id;
    } else if (rule != nullptr && !rule->requiresCapability.empty() &&
               !sheet.hasCapability(rule->requiresCapability)) {
      option.possible = false;
      option.reason = "requires " + rule->requiresCapability;
    }
    return option;
  }

  /// Every movement option for `sheet` in `terrainId` (empty = effective
  /// terrain), one per declared mode. UnknownTerrain for an undeclared
  /// terrain; when the ruleset has no movement rules, a single unconstrained
  /// "walk" option is returned.
  [[nodiscard]] std::expected<MovementStatus, BookkeepingError>
  movementStatus(const DynamicEntity &sheet, std::string_view terrainId = {}) const {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    const MovementConfig &mov = m_ruleset.movement;
    MovementStatus status;
    status.terrainId = terrainId.empty() ? effectiveTerrain(sheet) : std::string(terrainId);
    status.loadReducesSpeed = loadSpeedFactor(sheet) < 1.0;
    if (mov.modes.empty()) {
      MovementOption walk;
      walk.modeId = "walk";
      walk.modeName = "Walk";
      status.options.push_back(std::move(walk));
      return status;
    }
    if (findTerrain(status.terrainId) == nullptr) {
      return std::unexpected(BookkeepingError::UnknownTerrain);
    }
    // List every rule-declared mode plus any mode the sheet declares for
    // itself (a creature's stat-block speed may use a mode the ruleset does
    // not define, e.g. fly in a ruleset without a fly mode).
    std::vector<std::string> modeIds;
    for (const auto &[modeId, mode] : mov.modes) {
      modeIds.push_back(modeId);
    }
    for (const auto &[modeId, speed] : sheet.movementSpeeds()) {
      if (mov.modes.find(modeId) == mov.modes.end()) {
        modeIds.push_back(modeId);
      }
    }
    for (const std::string &modeId : modeIds) {
      auto option = movementSpeed(sheet, modeId, status.terrainId);
      if (option) {
        status.options.push_back(*option);
      }
    }
    return status;
  }

  /// The exhaustion cost to move `distance` in `modeId` in `terrainId`
  /// (empty = effective terrain): distance x the mode's per-unit cost after
  /// the terrain's cost multiplier. 0 when the ruleset has no movement rules.
  [[nodiscard]] std::expected<double, BookkeepingError>
  movementCost(const DynamicEntity &sheet, std::string_view modeId, double distance,
               std::string_view terrainId = {}) const {
    auto option = movementSpeed(sheet, modeId, terrainId);
    if (!option) {
      return std::unexpected(option.error());
    }
    return option->exhaustionPerUnit * distance;
  }

  /// Whether `sheet` can rest / regenerate in `terrainId` (empty = effective
  /// terrain): true unless the terrain declares `regeneration` "none" (a long
  /// rest restores nothing there) or "half" (it restores half). Always true
  /// when the ruleset has no movement rules.
  [[nodiscard]] bool canRegenerate(const DynamicEntity &sheet,
                                   std::string_view terrainId = {}) const {
    const MovementConfig &mov = m_ruleset.movement;
    if (mov.modes.empty()) {
      return true;
    }
    const std::string id = terrainId.empty() ? effectiveTerrain(sheet) : std::string(terrainId);
    const TerrainDef *terrain = findTerrain(id);
    if (terrain == nullptr) {
      return true;
    }
    return terrain->regeneration != "none";
  }

  /// Moves `sheet` `distance` in `modeId` within `terrainId` (empty =
  /// effective terrain): reports the outcome (distance, speed, time, total
  /// exhaustion) and drains the exhaustion pool declared in the movement
  /// section (clamped at 0; no pool declared = no deduction). Returns an
  /// error when the mode is not possible in the terrain.
  [[nodiscard]] std::expected<MovementOutcome, BookkeepingError>
  move(DynamicEntity &sheet, std::string_view modeId, double distance,
       std::string_view terrainId = {}) const {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    const std::string usedTerrain =
        terrainId.empty() ? effectiveTerrain(sheet) : std::string(terrainId);
    auto option = movementSpeed(sheet, modeId, usedTerrain);
    if (!option) {
      return std::unexpected(option.error());
    }
    MovementOutcome outcome;
    outcome.modeId = option->modeId;
    outcome.terrainId = usedTerrain;
    outcome.distance = distance;
    outcome.speed = option->speed;
    outcome.exhaustion = option->exhaustionPerUnit * distance;
    outcome.possible = option->possible;
    outcome.reason = option->reason;
    if (option->speed > 0.0) {
      outcome.timeUnits = distance / option->speed;
    }
    if (outcome.possible && !m_ruleset.movement.exhaustionPool.empty()) {
      (void)sheet.modifyResource(m_ruleset.movement.exhaustionPool,
                                 -static_cast<int32_t>(outcome.exhaustion));
    }
    return outcome;
  }

  // ------------------------------------------------------------------------
  // Bookkeeping: conditions & effects
  // ------------------------------------------------------------------------

  /// Applies `stacks` of `conditionId` to `sheet` for `duration` ticks
  /// (0 = permanent). The condition must be declared in `data.conditions`;
  /// throws std::invalid_argument otherwise. Fires OnConditionChanged.
  void applyCondition(DynamicEntity &sheet, std::string_view conditionId, int32_t stacks = 1,
                      int32_t duration = 0, std::string_view source = {}) {
    if (!m_ruleset.isCondition(conditionId)) {
      throw std::invalid_argument("unknown condition '" + std::string(conditionId) + "'");
    }
    sheet.addCondition(conditionId, stacks);
    sheet.effects().add(
        ActiveEffect{std::string(conditionId), stacks, duration, std::string(source)});
    fireConditionChanged(sheet, conditionId, stacks, "applied");
  }

  /// Removes `conditionId` entirely (stacks and effect timeline). Fires
  /// OnConditionChanged.
  void removeCondition(DynamicEntity &sheet, std::string_view conditionId) {
    sheet.removeCondition(conditionId);
    (void)sheet.effects().remove(conditionId);
    fireConditionChanged(sheet, conditionId, 0, "removed");
  }

  /// Fires @c OnConditionChanged for `sheet`, announcing `action`
  /// ("applied"/"removed"/"expired"). Centralised so every condition path —
  /// the facade wrappers, structured effects, encumbrance, and duration
  /// expiry — reports the same payload shape (`condition`, `stacks`, `action`,
  /// `actor_id`, `instance_id`). Runs the ruleset's JSON triggers first, then
  /// notifies user listeners.
  void fireConditionChanged(DynamicEntity &sheet, std::string_view conditionId, int32_t stacks,
                            std::string_view action) {
    EventData data;
    data.payload = {{"condition", std::string(conditionId)},
                    {"stacks", stacks},
                    {"action", std::string(action)},
                    {"actor_id", sheet.id()},
                    {"instance_id", sheet.entityId().value}};
    fireEvent(EventType::OnConditionChanged, data, sheet, nullptr, Json{});
  }

  /// Applies a temporary `value` bonus to `sheet`'s effective `stat` for
  /// `duration` ticks (0 = permanent until removed). Buffs from spells and
  /// traits that raise a stat for a while use this (e.g. The Dark Eye's
  /// Perception-boosting spell).
  void applyStatBonus(DynamicEntity &sheet, std::string_view stat, int32_t value,
                      int32_t duration = 0, std::string_view source = {}) {
    sheet.effects().addBonus(StatBonus{std::string(stat), value, duration, std::string(source)});
    EventData data;
    data.payload = {{"stat", std::string(stat)},
                    {"value", value},
                    {"duration", duration},
                    {"actor_id", sheet.id()},
                    {"instance_id", sheet.entityId().value}};
    fireEvent(EventType::OnStatChanged, data, sheet, nullptr, Json{});
  }

  /// Removes every temporary stat bonus on `stat` (returns how many removed).
  int32_t removeStatBonus(DynamicEntity &sheet, std::string_view stat) {
    return sheet.effects().removeBonuses(stat);
  }

  /// Adds an inherent trait (e.g. a monster's Pack Tactics) to `sheet`. The
  /// trait must be declared in `data.traits`; its check and stat modifiers
  /// apply to every check the sheet makes from then on.
  void applyTrait(DynamicEntity &sheet, std::string_view traitId) {
    sheet.addTrait(traitId);
  }

  /// Removes `traitId` from `sheet`'s active traits.
  void removeTrait(DynamicEntity &sheet, std::string_view traitId) {
    sheet.removeTrait(traitId);
  }

  /// Advances the sheet's effect timeline: durations tick down, expired
  /// conditions are removed (both from the timeline and the stack map).
  /// Returns how many effects expired.
  int32_t tickEffects(DynamicEntity &sheet) {
    std::vector<std::string> hadTimers;
    for (const ActiveEffect &effect : sheet.effects().effects()) {
      hadTimers.push_back(effect.conditionId);
    }
    const int32_t expired = sheet.effects().tick();
    for (const std::string &conditionId : hadTimers) {
      if (sheet.effects().stacks(conditionId) <= 0) {
        if (sheet.hasCondition(conditionId)) {
          fireConditionChanged(sheet, conditionId, 0, "expired");
        }
        sheet.removeCondition(conditionId);
      }
    }
    return expired;
  }

  /// Runs one full turn for `sheet`: fires OnTurnStart, resolves the
  /// start-of-turn recurring effects, ticks effect durations, resolves the
  /// end-of-turn recurring effects, then fires OnTurnEnd.
  template <RandomNumberGenerator Rng> void runTurn(DynamicEntity &sheet, Rng &rng) {
    EventData start;
    start.payload = {{"actor_id", sheet.id()}};
    fireEvent(EventType::OnTurnStart, start, sheet, nullptr, Json{});
    processOngoing(sheet, "start_of_turn", rng);
    tickEffects(sheet);
    processOngoing(sheet, "end_of_turn", rng);
    EventData end;
    end.payload = {{"actor_id", sheet.id()}};
    fireEvent(EventType::OnTurnEnd, end, sheet, nullptr, Json{});
  }

  /// Runs one full turn using a fresh default RNG (convenience overload).
  void runTurn(DynamicEntity &sheet) {
    DefaultRandom rng;
    runTurn(sheet, rng);
  }

  /// Re-resolves every recurring (ongoing) effect registered on `sheet` for
  /// the given `phase` ("start_of_turn" or "end_of_turn"): each stored effect
  /// fires once, its remaining repetitions decrease, and expired ones are
  /// removed. Returns how many effects fired.
  ///
  /// @par Why resolve against the sheet itself?
  /// A recurring effect (ongoing damage, regeneration, a poison's per-round
  /// damage) acts on the sheet on its own turn. The stored effect is resolved
  /// with the sheet as both source and target: fixed-dice effects are exact;
  /// a save DC that references the original caster's stats is an approximation
  /// (the ruleset should use a fixed or target-based DC for recurring effects).
  template <RandomNumberGenerator Rng>
  int32_t processOngoing(DynamicEntity &sheet, std::string_view phase, Rng &rng) {
    int32_t fired = 0;
    std::vector<Json> toFire;
    for (const OngoingEffect &ongoing : sheet.effects().ongoing()) {
      if (ongoing.phase == phase) {
        toFire.push_back(ongoing.effect);
      }
    }
    for (const Json &effect : toFire) {
      Json batch = Json::array();
      batch.push_back(effect);
      (void)resolveEffects(sheet, sheet, batch, CheckParams{}, rng);
      ++fired;
    }
    if (fired > 0) {
      sheet.effects().tickPhase(phase);
    }
    return fired;
  }

  // ------------------------------------------------------------------------
  // Bookkeeping: magic
  // ------------------------------------------------------------------------

  /// Adds `spellId` to the sheet's spellbook as known and prepared. Returns
  /// false when the spell is unknown.
  ///
  /// @note Unlike @ref castSpell (which throws for an unknown spell, because a
  /// spell that is cast must exist to resolve), this is a query-style "add to
  /// the book if it exists" helper — an unknown spell is an expected answer a
  /// caller may want to display, so it is reported via the boolean rather than
  /// an exception.
  bool prepareSpell(DynamicEntity &sheet, std::string_view spellId) const {
    if (findSpell(spellId) == nullptr) {
      return false;
    }
    sheet.spellbook().learn(spellId);
    sheet.spellbook().prepare(spellId);
    return true;
  }

  /// Free spell slots of `level` remaining today (0 when the ruleset does not
  /// use vancian slots, or the level has no schedule).
  [[nodiscard]] int32_t spellSlotsRemaining(const DynamicEntity &sheet, int32_t level) const {
    const auto it = m_ruleset.spellcasting.slots.find(level);
    if (m_ruleset.spellcasting.style != "slots" || it == m_ruleset.spellcasting.slots.end()) {
      return 0;
    }
    return it->second - sheet.spellbook().slotsUsed(level);
  }

  /// Marks one spell slot of `level` used; NoSpellSlot when none are free.
  [[nodiscard]] std::expected<void, BookkeepingError> spendSpellSlot(DynamicEntity &sheet,
                                                                     int32_t level) {
    if (spellSlotsRemaining(sheet, level) <= 0) {
      return std::unexpected(BookkeepingError::NoSpellSlot);
    }
    sheet.spellbook().markSlotUsed(level);
    return {};
  }

  /// Restores all spell slots (a long rest).
  void recoverSpellSlots(DynamicEntity &sheet) {
    sheet.spellbook().recoverAllSlots();
  }

  /// Casts a spell that must be prepared. For vancian rulesets the spell's
  /// level slot is spent instead of a resource pool; for pool rulesets the
  /// normal cost applies. Unlike @ref castSpell, an unprepared spell is not
  /// cast (cast = false, no cost spent).
  template <RandomNumberGenerator Rng>
  [[nodiscard]] SpellResult castSpellPrepared(std::string_view spellId, DynamicEntity &actor,
                                              DynamicEntity *target, const CheckParams &params,
                                              Rng &rng) {
    SpellResult result;
    if (!actor.spellbook().hasPrepared(spellId)) {
      return result; // cast stays false
    }
    if (m_ruleset.spellcasting.style == "slots") {
      const Json *spell = findSpell(spellId);
      const int32_t level = spell != nullptr ? spell->value("level", 1) : 1;
      if (spellSlotsRemaining(actor, level) <= 0) {
        return result;
      }
      result = castSpell(spellId, actor, target, "", params, rng);
      if (result.cast) {
        (void)spendSpellSlot(actor, level);
      }
    } else {
      result = castSpell(spellId, actor, target, params, rng);
    }
    return result;
  }

  /// Casts a prepared spell using a fresh default RNG (convenience overload).
  [[nodiscard]] SpellResult castSpellPrepared(std::string_view spellId, DynamicEntity &actor,
                                              DynamicEntity *target, const CheckParams &params) {
    DefaultRandom rng;
    return castSpellPrepared(spellId, actor, target, params, rng);
  }

  // ------------------------------------------------------------------------
  // Structured effects (the extracted, machine-readable form of prose rules)
  // ------------------------------------------------------------------------

  /// The outcome of resolving a batch of structured effects.
  ///
  /// @par Why a typed result?
  /// An application that triggers a spell, a magic item, or a monster trait
  /// needs to know what actually happened — how much damage landed, whether a
  /// condition was applied, how much was healed — so it can narrate and react.
  struct EffectsResult {
    int32_t damageDealt{0};                   ///< net damage applied to the target
    int32_t conditionsApplied{0};             ///< how many condition effects landed
    int32_t healingDone{0};                   ///< hit points restored
    int32_t savesPassed{0};                   ///< how many saves the target passed
    int32_t statBonusesApplied{0};            ///< how many temporary stat bonuses landed
    std::vector<std::string> choicesRequired; ///< option-group ids needing a caller selection
    std::vector<int32_t> damageDice;          ///< raw damage dice rolled by the effects (in order)
  };

  /// Resolves a batch of structured effects (the `effects` array on spells,
  /// magic items, monster traits, ...) against `target`, driven by `source`.
  ///
  /// @par Why structured effects instead of parsing prose?
  /// The engine is data-driven: it interprets structured data, it does not
  /// read English. Rules that used to live only in a `description` are
  /// transcribed (by the extraction scripts / a ruleset author) into this
  /// effect vocabulary, so their mechanical consequences — saving throws,
  /// damage with half-on-save, conditions, healing, resistances — are applied
  /// automatically and consistently. Prose stays as the human-readable layer.
  ///
  /// Supported effect kinds:
  ///   - @c damage: rolls `dice`, applies it through the damage pipeline; an
  ///     optional `save` halves it (on_success "half") or negates it ("none");
  ///     an optional formula `add` (evaluated over the source's stats and the
  ///     casting check's quality level via `env.ql`) is added to the roll.
  ///   - @c condition: applies `condition` (stacks, optional `duration` in
  ///     ticks) unless the target passes the optional `save`.
  ///   - @c heal: rolls `dice` (+ optional formula `add`) into the target's
  ///     hit-point pool.
  ///   - @c resist: adds the listed `types` to the target's resistances.
  ///   - @c stat_bonus: applies a temporary `add` (number or formula) to the
  ///     target's effective `stat` for `duration` ticks (0 = permanent).
  ///   - @c options: a caller-side choice. The effect carries an `id` and a
  ///     list of mutually exclusive `options` (effect records). The engine
  ///     never chooses for the caller: without a selection (via the
  ///     `selections` map) a required option group is reported in
  ///     `choicesRequired` and nothing is applied; an `optional` group is
  ///     skipped silently; with a selection the chosen option resolves.
  ///
  /// Any effect may additionally carry an `ongoing` object
  /// (`{"at": "start_of_turn"|"end_of_turn", "duration": N}`) making it
  /// recurring: after the base effect resolves once, it is remembered on the
  /// target and re-resolved at the declared phase of each of the target's
  /// turns until `duration` more applications have fired.
  ///
  /// `selections` (optional) resolves `options` groups: a map of option-group
  /// id -> index into that group's `options` array.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] EffectsResult
  resolveEffects(DynamicEntity &source, DynamicEntity &target, const Json &effects,
                 const CheckParams &params, Rng &rng, int32_t qualityLevel = 0,
                 const std::unordered_map<std::string, int32_t> *selections = nullptr) {
    EffectsResult result;
    if (!effects.is_array()) {
      return result;
    }
    const std::string hitPool = resolveHitPointPoolId();
    for (const Json &effect : effects) {
      const std::string kind = effect.value("kind", "");
      bool applied = true; // whether the base effect actually took effect
      if (kind == "damage") {
        applied =
            resolveDamageEffect(source, target, effect, params, rng, qualityLevel, hitPool, result);
      } else if (kind == "condition") {
        applied = resolveConditionEffect(source, target, effect, params, rng, qualityLevel, result);
      } else if (kind == "heal") {
        applied = resolveHealEffect(source, target, effect, rng, qualityLevel, hitPool, result);
      } else if (kind == "temp_hp") {
        applied = resolveTempHpEffect(source, target, effect, rng, qualityLevel);
      } else if (kind == "resist") {
        applied = resolveResistEffect(target, effect);
      } else if (kind == "stat_bonus") {
        applied = resolveStatBonusEffect(source, target, effect, qualityLevel, result);
      } else if (kind == "bonus_die") {
        applied = resolveBonusDieEffect(target, effect);
      } else if (kind == "options") {
        applied = resolveOptionsEffect(source, target, effect, params, rng, qualityLevel, result,
                                       selections);
      }

      // Recurring (ongoing) effects: after the base effect resolves and
      // *applies* (the attack hit / the save failed / it is unconditional),
      // remember what to re-apply at the declared phase of the target's turn.
      // The stored record is the `ongoing.effect` when given (the recurring
      // part may differ from the initial one — an arrow deals 4d4 now and 2d4
      // at the end of its next turn), else the base effect with `ongoing`
      // stripped, so a re-resolution never re-registers.
      if (applied && effect.contains("ongoing") && effect.at("ongoing").is_object()) {
        const Json &ongoingJson = effect.at("ongoing");
        OngoingEffect ongoing;
        if (ongoingJson.contains("effect")) {
          ongoing.effect = ongoingJson.at("effect");
        } else {
          ongoing.effect = effect;
          ongoing.effect.erase("ongoing");
        }
        ongoing.phase = ongoingJson.value("at", "end_of_turn");
        ongoing.remaining = ongoingJson.value("duration", 1);
        ongoing.source = "effect";
        target.effects().addOngoing(ongoing);
      }
    }
    return result;
  }

  // ---- per-kind effect resolvers -------------------------------------------------

  /// Applies a @c damage effect: rolls `dice` (+ optional formula `add`),
  /// applies it through the damage pipeline (halving or negating on a save or
  /// an attack miss as declared). Returns whether the effect actually took
  /// effect (false when a save/attack fully negated it) — that gates whether a
  /// recurring `ongoing` re-application is registered.
  template <RandomNumberGenerator Rng>
  bool resolveDamageEffect(DynamicEntity &source, DynamicEntity &target, const Json &effect,
                           const CheckParams &params, Rng &rng, int32_t qualityLevel,
                           const std::string &hitPool, EffectsResult &result) {
    if (!effect.contains("dice")) {
      return false;
    }
    // A dice expression is rolled explicitly so the individual dice can be
    // reported on the result (a fight transcript narrates them). This is
    // observation-only: roll() consumes exactly the RNG stream rollSum()
    // would, so a call that records dice and one that only sums stay
    // byte-identical (pinned by the combat tests). Plain numbers and range
    // objects keep their generic readVariantValue path (no dice to record).
    int32_t final = 0;
    const Json &diceJson = effect.at("dice");
    if (diceJson.is_string()) {
      const DiceExpression diceExpr(diceJson.get<std::string>());
      const std::vector<int> rolled = diceExpr.roll(rng);
      final = diceExpr.constant();
      const auto &groups = diceExpr.dice();
      for (std::size_t g = 0, i = 0; g < groups.size(); ++g) {
        for (int j = 0; j < groups[g].count; ++j, ++i) {
          final += groups[g].sign * rolled[i];
          result.damageDice.push_back(rolled[i]);
        }
      }
    } else {
      final = static_cast<int32_t>(readVariantValue(diceJson, Variance::Random, rng));
    }
    if (effect.contains("add")) {
      final += evaluateEffectAdd(source, &target, effect.at("add"), qualityLevel);
    }
    bool applied = true;
    if (effect.contains("save")) {
      if (resolveSave(source, target, effect.at("save"), params, rng, qualityLevel)) {
        ++result.savesPassed;
        if (effect.at("save").value("on_success", "none") == "half") {
          final /= 2;
        } else {
          final = 0;
          applied = false;
        }
      }
    } else if (effect.contains("attack")) {
      const Json &attack = effect.at("attack");
      if (!resolveAttack(source, target, attack, params, rng)) {
        if (attack.value("on_miss", "none") == "half") {
          final /= 2;
        } else {
          final = 0;
          applied = false;
        }
      }
    }
    if (final > 0 && !hitPool.empty()) {
      // applyDamage reports the negative pool delta; negate for "dealt".
      result.damageDealt += -applyDamage(source, target, hitPool, final);
    }
    return applied;
  }

  /// Applies a @c condition effect: applies `condition` (stacks, optional
  /// `duration`) unless the target passes the optional save or attack.
  /// Returns whether the condition was applied (false when resisted).
  template <RandomNumberGenerator Rng>
  bool resolveConditionEffect(DynamicEntity &source, DynamicEntity &target, const Json &effect,
                              const CheckParams &params, Rng &rng, int32_t qualityLevel,
                              EffectsResult &result) {
    const std::string conditionId = effect.value("condition", "");
    if (conditionId.empty()) {
      return false;
    }
    if (effect.contains("save") &&
        resolveSave(source, target, effect.at("save"), params, rng, qualityLevel)) {
      ++result.savesPassed;
      return false;
    }
    if (effect.contains("attack") &&
        !resolveAttack(source, target, effect.at("attack"), params, rng)) {
      return false;
    }
    // `stacks` may be a plain number or a formula over the source's stats and
    // the casting check's quality level (`env.ql`) — the latter lets QL-scaled
    // conditions ("QL 3: 2 levels of Pain") resolve from data.
    int32_t stacks = 1;
    if (effect.contains("stacks")) {
      const Json &stacksJson = effect.at("stacks");
      if (stacksJson.is_number()) {
        stacks = stacksJson.get<int32_t>();
      } else if (stacksJson.is_string()) {
        const Json env = {{"ql", qualityLevel}};
        const EntityContext context(source, &target, env, Json{});
        stacks = math::toStat(Expression(stacksJson.get<std::string>()).evaluate(context));
      }
    }
    target.addCondition(conditionId, stacks);
    fireConditionChanged(target, conditionId, stacks, "applied");
    const int32_t duration = effect.value("duration", 0);
    if (duration > 0) {
      target.effects().add(ActiveEffect{conditionId, stacks, duration, "effect"});
    }
    ++result.conditionsApplied;
    return true;
  }

  /// Applies a @c heal effect: rolls `dice` (+ optional formula `add`) into the
  /// target's hit-point pool. Returns whether anything was applied.
  template <RandomNumberGenerator Rng>
  bool resolveHealEffect(DynamicEntity &source, DynamicEntity &target, const Json &effect, Rng &rng,
                         int32_t qualityLevel, const std::string &hitPool, EffectsResult &result) {
    if (!effect.contains("dice") || hitPool.empty()) {
      return false;
    }
    int32_t amount =
        static_cast<int32_t>(readVariantValue(effect.at("dice"), Variance::Random, rng));
    if (effect.contains("add")) {
      amount += evaluateEffectAdd(source, &target, effect.at("add"), qualityLevel);
    }
    result.healingDone += target.modifyResource(hitPool, amount);
    return true;
  }

  /// Applies a @c temp_hp effect: grants temporary hit points. Returns whether
  /// anything was granted.
  template <RandomNumberGenerator Rng>
  bool resolveTempHpEffect(DynamicEntity &source, DynamicEntity &target, const Json &effect,
                           Rng &rng, int32_t qualityLevel) {
    if (!effect.contains("dice")) {
      return false;
    }
    int32_t amount =
        static_cast<int32_t>(readVariantValue(effect.at("dice"), Variance::Random, rng));
    if (effect.contains("add")) {
      amount += evaluateEffectAdd(source, &target, effect.at("add"), qualityLevel);
    }
    target.addTemporaryHitPoints(amount);
    return true;
  }

  /// Applies a @c resist effect: adds the listed damage `types` to the target's
  /// resistances. Always applies (an empty list is a no-op).
  bool resolveResistEffect(DynamicEntity &target, const Json &effect) {
    if (effect.contains("types")) {
      for (const Json &type : effect.at("types")) {
        target.addResistance(type.get<std::string>());
      }
    }
    return true;
  }

  /// Applies a @c stat_bonus effect: a temporary `add` (number or formula) to
  /// the target's effective `stat` for `duration` ticks (0 = permanent).
  /// Returns whether a bonus was applied.
  bool resolveStatBonusEffect(DynamicEntity &source, DynamicEntity &target, const Json &effect,
                              int32_t qualityLevel, EffectsResult &result) {
    const std::string stat = effect.value("stat", "");
    if (stat.empty()) {
      return false;
    }
    const int32_t value = effect.contains("add")
                              ? evaluateEffectAdd(source, &target, effect.at("add"), qualityLevel)
                              : 0;
    const int32_t duration = effect.value("duration", 0);
    target.effects().addBonus(StatBonus{stat, value, duration, "effect"});
    ++result.statBonusesApplied;
    return true;
  }

  /// Applies a @c bonus_die effect: grants a temporary die (`dice` + `scope`)
  /// added to matching checks. Returns whether a die was granted.
  bool resolveBonusDieEffect(DynamicEntity &target, const Json &effect) {
    const std::string dice = effect.value("dice", "");
    if (dice.empty()) {
      return false;
    }
    const std::string scope = effect.value("scope", "all");
    const int32_t duration = effect.value("duration", 0);
    target.effects().addBonusDie(BonusDie{dice, scope, duration, "effect"});
    return true;
  }

  /// Resolves an @c options group: a caller-side choice, so the engine never
  /// picks for the caller. Without a selection a required group is reported in
  /// `choicesRequired` (nothing applied); an optional group is skipped
  /// silently; with a selection the chosen option resolves (including its own
  /// save/attack/ongoing logic). Returns whether a choice was applied.
  template <RandomNumberGenerator Rng>
  bool resolveOptionsEffect(DynamicEntity &source, DynamicEntity &target, const Json &effect,
                            const CheckParams &params, Rng &rng, int32_t qualityLevel,
                            EffectsResult &result,
                            const std::unordered_map<std::string, int32_t> *selections) {
    const std::string groupId = effect.value("id", "");
    const Json &options = effect.at("options");
    int32_t chosen = -1;
    if (!groupId.empty() && selections != nullptr) {
      const auto it = selections->find(groupId);
      if (it != selections->end()) {
        chosen = it->second;
      }
    }
    if (chosen < 0) {
      if (!effect.value("optional", false)) {
        result.choicesRequired.push_back(groupId);
      }
      return false;
    }
    if (!options.is_array() || chosen >= static_cast<int32_t>(options.size())) {
      return false;
    }
    Json batch = Json::array();
    batch.push_back(options.at(static_cast<std::size_t>(chosen)));
    const EffectsResult sub =
        resolveEffects(source, target, batch, params, rng, qualityLevel, selections);
    result.damageDealt += sub.damageDealt;
    result.conditionsApplied += sub.conditionsApplied;
    result.healingDone += sub.healingDone;
    result.savesPassed += sub.savesPassed;
    result.statBonusesApplied += sub.statBonusesApplied;
    result.choicesRequired.insert(result.choicesRequired.end(), sub.choicesRequired.begin(),
                                  sub.choicesRequired.end());
    return true;
  }

  /// Evaluates an effect's `add` amount: a flat integer or a formula over the
  /// acting entity's stats plus the casting check's quality level (`env.ql`).
  /// Formula `add` lets a ruleset express quality-scaled effects ("2D6 + QLx2"
  /// becomes @c dice 2d6 + @c add "env.ql * 2") without engine changes.
  [[nodiscard]] int32_t evaluateEffectAdd(const DynamicEntity &source, const DynamicEntity *target,
                                          const Json &add, int32_t qualityLevel) const {
    if (add.is_number()) {
      return add.get<int32_t>();
    }
    if (add.is_string()) {
      const Json env = {{"ql", qualityLevel}};
      const EntityContext context(source, target, env, Json{});
      return math::toStat(Expression(add.get<std::string>()).evaluate(context));
    }
    return 0;
  }

  /// Resolves structured effects using a fresh default RNG (convenience).
  [[nodiscard]] EffectsResult
  resolveEffects(DynamicEntity &source, DynamicEntity &target, const Json &effects,
                 const CheckParams &params, int32_t qualityLevel = 0,
                 const std::unordered_map<std::string, int32_t> *selections = nullptr) {
    DefaultRandom rng;
    return resolveEffects(source, target, effects, params, rng, qualityLevel, selections);
  }

  /// The outcome of a structured effect's saving throw.
  ///
  /// @par Why a template returning bool?
  /// A save is a threshold check: with `comparison` "ge" (default, D&D style)
  /// the target rolls `save.dice` (default 1d20), adds its `save.stat`, and
  /// succeeds when the total is >= the DC — the DC being a fixed number or a
  /// formula evaluated against the *source* (the caster), so "8 + proficiency
  /// + ability modifier" style DCs are expressed in data rather than in C++.
  /// With `comparison` "le" (The Dark Eye's resistance) the target rolls the
  /// bare die under stat + dc, where dc is a modifier (typically the caster's
  /// quality level as a penalty, e.g. "-env.ql"). Returns true when the
  /// target saved.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] bool resolveSave(const DynamicEntity &source, DynamicEntity &target,
                                 const Json &save, const CheckParams &params, Rng &rng,
                                 int32_t qualityLevel = 0) {
    const bool rollUnder = save.value("comparison", "ge") == "le";
    int32_t dc = rollUnder ? 0 : 10;
    if (save.contains("dc")) {
      const Json &dcJson = save.at("dc");
      if (dcJson.is_number()) {
        dc = dcJson.get<int32_t>();
      } else if (dcJson.is_string()) {
        const Json env = {{"ql", qualityLevel}};
        const EntityContext context(source, &target, env, Json{});
        dc = math::toStat(Expression(dcJson.get<std::string>()).evaluate(context));
      }
    }
    const std::string stat = save.value("stat", "");
    CheckRecipe recipe;
    recipe.resolution = Resolution::Threshold;
    recipe.dice = DiceExpression(save.value("dice", "1d20"));
    if (rollUnder) {
      // The Dark Eye's resistance: roll the bare die under stat + dc, where
      // dc is a modifier (the caster's QL applies a penalty, dc = "-env.ql").
      recipe.comparison = Comparison::LessEqual;
      recipe.thresholdSource = ThresholdSource::ActorStat;
      recipe.thresholdStat = stat;
      recipe.difficultyMode = DifficultyMode::ToStat;
    } else {
      // D&D style: roll d20 + stat against the DC (ToThreshold).
      recipe.comparison = Comparison::GreaterEqual;
      recipe.thresholdSource = ThresholdSource::Difficulty;
      recipe.difficultyMode = DifficultyMode::ToThreshold;
      if (!stat.empty()) {
        recipe.bonusStats = {stat};
      }
    }
    CheckParams adjusted = withConditionModifiers(target, nullptr, "save", params);
    adjusted.difficulty = dc;
    announceCheckStart("save", target);
    const CheckResult roll = resolveCheck(target, NullStatProvider{}, recipe, adjusted, rng);
    announceCheckResult("save", target, roll);
    return roll.isSuccess;
  }

  /// Whether the source's attack roll beats the target's defence (an attack
  /// roll instead of a saving throw — spell attacks, breath weapons, ...).
  /// The source rolls `dice` (default 1d20), adds each stat id in
  /// `bonus_stats` (resolved on the source, e.g. the ability `_mod` plus
  /// `proficiency_bonus`), and hits when the total is >= the target's
  /// `target_stat` (default `AC`).
  template <RandomNumberGenerator Rng>
  [[nodiscard]] bool resolveAttack(const DynamicEntity &source, DynamicEntity &target,
                                   const Json &attack, const CheckParams &params, Rng &rng) {
    CheckRecipe recipe;
    recipe.resolution = Resolution::Threshold;
    recipe.dice = DiceExpression(attack.value("dice", "1d20"));
    recipe.comparison = Comparison::GreaterEqual;
    recipe.thresholdSource = ThresholdSource::TargetStat;
    recipe.thresholdStat = attack.value("target_stat", "AC");
    recipe.difficultyMode = DifficultyMode::ToThreshold;
    if (attack.contains("bonus_stats")) {
      for (const Json &stat : attack.at("bonus_stats")) {
        recipe.bonusStats.push_back(stat.get<std::string>());
      }
    }
    // The attacker's own conditions (a blinded caster attacks at disadvantage)
    // and the target's "attacked" conditions (attacks against a blinded target
    // gain advantage) both shape the roll.
    const CheckParams adjusted = withConditionModifiers(source, &target, "attack", params);
    announceCheckStart("attack", source);
    const CheckResult roll = resolveCheck(source, target, recipe, adjusted, rng);
    announceCheckResult("attack", source, roll);
    return roll.isSuccess;
  }

  // ------------------------------------------------------------------------
  // Bookkeeping: advancement, rest, time, afflictions
  // ------------------------------------------------------------------------

  /// Grants `xp` to `sheet` and updates its level from the ruleset's
  /// `xp_to_level` cost table (when present). Returns the LevelUp outcome and
  /// fires OnLevelUp when the level changed.
  [[nodiscard]] std::expected<LevelUp, BookkeepingError> gainXp(DynamicEntity &sheet, int64_t xp) {
    if (!m_loaded) {
      return std::unexpected(BookkeepingError::NoRuleset);
    }
    sheet.advancement().gainXp(xp);
    const CostTableDef *table = m_ruleset.findCostTable("xp_to_level");
    if (table == nullptr) {
      return LevelUp{sheet.advancement().level, sheet.advancement().level, false};
    }
    const int32_t newLevel = table->table.lookup(static_cast<int32_t>(sheet.advancement().xp()));
    LevelUp result{sheet.advancement().level, newLevel, newLevel != sheet.advancement().level};
    sheet.advancement().level = newLevel;
    if (result.leveled) {
      EventData data;
      data.payload = {
          {"from_level", result.fromLevel}, {"to_level", result.toLevel}, {"actor_id", sheet.id()}};
      fireEvent(EventType::OnLevelUp, data, sheet, nullptr, Json{});
    }
    return result;
  }

  /// The cost (from `costTableId`) to advance a rating that is currently
  /// `currentRating` — a per-point multiplier (TDE AP columns). (D&D's
  /// XP-to-level table is consumed by gainXp instead.)
  [[nodiscard]] std::expected<int32_t, BookkeepingError>
  improvementCost(std::string_view costTableId, int32_t currentRating) const {
    const CostTableDef *table = m_ruleset.findCostTable(costTableId);
    if (table == nullptr) {
      return std::unexpected(BookkeepingError::UnknownCostTable);
    }
    return table->table.lookup(currentRating);
  }

  /// A short rest: tick effect durations and notify. Resources are unchanged.
  void shortRest(DynamicEntity &sheet) {
    tickEffects(sheet);
    EventData data;
    data.payload = {{"actor_id", sheet.id()}, {"kind", "short"}};
    fireEvent(EventType::OnRest, data, sheet, nullptr, Json{});
  }

  /// The regeneration scale of the terrain `sheet` is in: 1.0 (normal),
  /// 0.5 (half), or 0.0 (none) — the fraction of a long rest's resource
  /// restoration that applies there. Always 1.0 when the ruleset has no
  /// movement rules or the terrain is unknown.
  [[nodiscard]] double terrainRegenerationScale(const DynamicEntity &sheet,
                                                std::string_view terrainId = {}) const {
    const MovementConfig &mov = m_ruleset.movement;
    if (mov.modes.empty()) {
      return 1.0;
    }
    const std::string id = terrainId.empty() ? effectiveTerrain(sheet) : std::string(terrainId);
    const TerrainDef *terrain = findTerrain(id);
    if (terrain == nullptr) {
      return 1.0;
    }
    if (terrain->regeneration == "none") {
      return 0.0;
    }
    if (terrain->regeneration == "half") {
      return 0.5;
    }
    return 1.0;
  }

  /// A long rest: restores every resource pool, recovers spell slots, and
  /// ends timed conditions (effects whose duration expired). The terrain the
  /// sheet is in may block or halve regeneration ("none" / "half" — see
  /// @ref canRegenerate); the ruleset's default terrain always restores fully.
  void longRest(DynamicEntity &sheet) {
    const double regen = terrainRegenerationScale(sheet);
    for (const ResourcePoolDef &def : m_ruleset.resourcePools) {
      const int32_t max = sheet.getStat(def.maxStat);
      const int32_t missing = max - sheet.resource(def.id);
      (void)sheet.modifyResource(def.id, static_cast<int32_t>(missing * regen));
    }
    recoverSpellSlots(sheet);
    tickEffects(sheet);
    EventData data;
    data.payload = {{"actor_id", sheet.id()}, {"kind", "long"}};
    fireEvent(EventType::OnRest, data, sheet, nullptr, Json{});
  }

  /// Applies a curse from `data.curses` (the same generic save + effects path
  /// as poisons / diseases) and records it on the victim for later curing.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] AfflictionResult applyCurse(std::string_view curseId, DynamicEntity &victim,
                                            const CheckParams &params, Rng &rng) {
    return applyAffliction("curses", curseId, victim, params, rng);
  }

  /// Ends an active affliction on `victim`: removes the conditions its effects
  /// applied and forgets the record. Returns false when the affliction was not
  /// active (already cured or never applied).
  bool cureAffliction(DynamicEntity &victim, std::string_view section, std::string_view id) {
    for (auto it = victim.afflictions().begin(); it != victim.afflictions().end(); ++it) {
      if (it->section == section && it->id == id) {
        for (const std::string &condition : it->conditions) {
          removeCondition(victim, condition);
        }
        victim.afflictions().erase(it);
        return true;
      }
    }
    return false;
  }

  /// Registers a user-facing event listener (returns a handle for removal).
  /// User listeners run *after* the ruleset's own JSON-driven triggers, so
  /// they observe the final, mutated payload.
  [[nodiscard]] uint64_t registerEventListener(EventType type, EventBus::Callback callback) {
    return m_eventBus.addListener(type, std::move(callback));
  }

  /// Removes a previously registered event listener.
  bool unregisterEventListener(uint64_t listenerId) {
    return m_eventBus.removeListener(listenerId);
  }

  /// Wires `entity`'s granular state-change events (stat / resource / temp-HP
  /// changes) to this engine's event bus, so an application observes both
  /// engine-driven and direct sheet mutations without polling. The engine
  /// calls this for every entity it creates; the entity must not outlive the
  /// engine (the same lifetime rule as its ruleset reference).
  void attachEntityEvents(DynamicEntity &entity) {
    entity.setEventSink([this, entityId = entity.entityId()](EventType type, const Json &payload) {
      EventData data;
      data.payload = payload;
      data.payload["instance_id"] = entityId.value;
      m_eventBus.dispatch(type, data);
    });
  }

  /// Announces that game time advanced — used by @ref GameSession::advanceTime,
  /// which owns the world clock. Dispatches @c OnTimePassed with the elapsed
  /// and resulting day (listener-only; no ruleset triggers exist for it).
  void announceTimePassed(int32_t days, int32_t newDay) {
    dispatchEvent(EventType::OnTimePassed, Json{{"days", days}, {"day", newDay}});
  }

  /// The loaded ruleset (only valid when `loaded()` is true).
  [[nodiscard]] const Ruleset &ruleset() const noexcept {
    return m_ruleset;
  }

  /// Whether a ruleset has been loaded successfully.
  [[nodiscard]] bool loaded() const noexcept {
    return m_loaded;
  }

  /// Message from the last failed load / validation. Empty after a successful
  /// load.
  [[nodiscard]] const std::string &lastError() const noexcept {
    return m_lastError;
  }

private:
  /// Parses a price string like "150 GP", "1 SP", "45 ST", or "1,500 GP" into
  /// Money using the ruleset's currency system. "—", "Varies", and empty
  /// strings mean no price (Money{0}); a bare number is base units; a coin
  /// symbol that matches no denomination is an UnknownDenomination error.
  [[nodiscard]] std::expected<Money, BookkeepingError>
  parsePriceString(std::string_view text) const {
    std::string trimmed = trimWhitespace(text);
    if (trimmed.empty() || trimmed == "-" || trimmed == "—" || trimmed == "Varies" ||
        trimmed == "varies") {
      return Money{0};
    }
    std::size_t pos = 0;
    bool sawDigit = false;
    while (pos < trimmed.size()) {
      const char c = trimmed[pos];
      if (c == ',' || c == '.' || c == ' ' || (c >= '0' && c <= '9')) {
        if (c >= '0' && c <= '9') {
          sawDigit = true;
        }
        ++pos;
        continue;
      }
      break;
    }
    if (!sawDigit) {
      return Money{0};
    }
    std::string number = trimmed.substr(0, pos);
    std::string symbol = trimWhitespace(trimmed.substr(pos));
    number.erase(std::remove(number.begin(), number.end(), ','), number.end());
    number.erase(std::remove(number.begin(), number.end(), ' '), number.end());
    const int64_t amount = static_cast<int64_t>(std::llround(std::strtod(number.c_str(), nullptr)));
    if (symbol.empty()) {
      return Money{amount}; // a bare number is base units
    }
    for (const Denomination &d : m_ruleset.currencySystem.denominations) {
      if (d.symbol == symbol || d.id == symbol) {
        return Money{amount * d.perBase};
      }
    }
    return std::unexpected(BookkeepingError::UnknownDenomination);
  }

  /// Dispatches `type` with `payload` to user listeners only (no ruleset JSON
  /// triggers). Used by the @c const check paths and by @ref updateEncumbrance,
  /// where the engine must not mutate through rule actions. The granular event
  /// types dispatched here (@c OnBeforeCheckRoll, @c OnAfterCheckRoll,
  /// @c OnCheckResolved, and @c OnConditionChanged from encumbrance) have no
  /// JSON triggers in any shipped ruleset, so listener-only dispatch is
  /// equivalent to @ref fireEvent for them.
  void dispatchEvent(EventType type, Json payload) const {
    EventData data;
    data.payload = std::move(payload);
    m_eventBus.dispatch(type, data);
  }

  /// Fires @c OnBeforeCheckRoll just before a check's dice land, naming the
  /// actor so UI can show "X is rolling &lt;check&gt;" without polling.
  void announceCheckStart(std::string_view checkTypeId, const DynamicEntity &actor) const {
    dispatchEvent(EventType::OnBeforeCheckRoll, Json{{"check_type", std::string(checkTypeId)},
                                                     {"actor_id", actor.id()},
                                                     {"instance_id", actor.entityId().value}});
  }

  /// Announces a resolved check: @c OnAfterCheckRoll with the raw dice, then
  /// @c OnCheckResolved with the interpreted outcome (success, success level,
  /// quality level, margin). Both name the rolling entity via @c actor_id and
  /// @c instance_id, so UI can hook "the hero failed the Climb check" (a
  /// resolved check with @c is_success false) without polling the sheet.
  void announceCheckResult(std::string_view checkTypeId, const DynamicEntity &actor,
                           const CheckResult &result) const {
    dispatchEvent(EventType::OnAfterCheckRoll, Json{{"check_type", std::string(checkTypeId)},
                                                    {"actor_id", actor.id()},
                                                    {"instance_id", actor.entityId().value},
                                                    {"raw_dice", result.rawDiceRolls}});
    dispatchEvent(EventType::OnCheckResolved,
                  Json{{"check_type", std::string(checkTypeId)},
                       {"actor_id", actor.id()},
                       {"instance_id", actor.entityId().value},
                       {"is_success", result.isSuccess},
                       {"success_level", static_cast<int32_t>(result.successLevel)},
                       {"quality_level", result.qualityLevel},
                       {"margin_of_success", result.marginOfSuccess},
                       {"raw_dice", result.rawDiceRolls}});
  }

  /// Runs the ruleset's JSON-driven triggers for `type` (mutating `data`), then
  /// notifies user-facing listeners. The ordering is deliberate: ruleset rules
  /// (e.g. armor absorption) must see the payload first and mutate it, so
  /// user callbacks observe the final state — matching how a tabletop GM would
  /// apply the book rule before announcing the outcome.
  void fireEvent(EventType type, EventData &data, DynamicEntity &actor, DynamicEntity *target,
                 const Json &env) {
    runRuleTriggers(type, data, actor, target, env);
    m_eventBus.dispatch(type, data);
  }

  /// Executes the ruleset's event triggers of `type` against the payload.
  /// Each trigger may carry a condition; only triggers whose condition
  /// evaluates non-zero actually fire their actions.
  void runRuleTriggers(EventType type, EventData &data, DynamicEntity &actor, DynamicEntity *target,
                       const Json &env) {
    for (const EventTriggerDef &trigger : m_ruleset.eventTriggers) {
      if (trigger.trigger != type) {
        continue;
      }
      if (!trigger.conditionText.empty()) {
        const EntityContext context(actor, target, env, data.payload);
        if (trigger.condition.evaluate(context) == 0.0) {
          continue;
        }
      }
      for (const EventActionDef &action : trigger.actions) {
        executeAction(action, data, actor, target, env);
      }
    }
  }

  /// Executes a single event action. Modifies `data` for
  /// `modify_event_damage`; otherwise mutates the actor (when no target) or
  /// the target.
  ///
  /// @par Why target-if-present-else-actor for resource/condition actions?
  /// "Consume a resource" and "apply a condition" act on whoever was hit when
  /// a target exists, and on the acting character for solo effects. That
  /// single rule keeps action semantics uniform across trigger sites.
  void executeAction(const EventActionDef &action, EventData &data, DynamicEntity &actor,
                     DynamicEntity *target, const Json &env) {
    const EntityContext context(actor, target, env, data.payload);
    DynamicEntity &subject = target != nullptr ? *target : actor;
    if (action.type == "modify_event_damage") {
      const double value = action.formula.evaluate(context);
      data.payload["damage"] = value;
    } else if (action.type == "consume_resource") {
      (void)subject.modifyResource(action.resource, -action.amount);
    } else if (action.type == "apply_condition") {
      const int32_t stacks =
          action.stacksText.empty() ? 1 : math::toStat(action.stacks.evaluate(context));
      subject.addCondition(action.condition, stacks);
    } else if (action.type == "gain_item" || action.type == "remove_item") {
      if (!action.item.empty()) {
        if (action.type == "gain_item") {
          subject.inventory().add(ItemInstance{action.item, action.amount, {}});
        } else {
          (void)subject.inventory().remove(action.item, action.amount);
        }
      }
    } else if (action.type == "gain_currency" || action.type == "spend_currency") {
      const Money amount{action.amount};
      if (action.type == "gain_currency") {
        subject.money() += amount;
      } else {
        subject.money() -= amount;
      }
    } else if (action.type == "gain_xp") {
      subject.advancement().gainXp(action.amount);
    }
  }

  /// The generic pool-check recipe shared by skill checks and the "A/B/C"
  /// casting-check form: a 3d20 roll against up to three linked attributes,
  /// with double-roll criticals and pool-quality grading (The Dark Eye).
  static CheckRecipe makePoolRecipe() {
    CheckRecipe recipe;
    recipe.resolution = Resolution::Pool;
    recipe.dice = "3d20"_dice;
    recipe.criticalStyle = CriticalStyle::DoubleRoll;
    recipe.fumbleStyle = CriticalStyle::DoubleRoll;
    recipe.grading = Grading::PoolQuality;
    recipe.difficultyMode = DifficultyMode::ToStat;
    return recipe;
  }

  /// The ruleset's declared spell resource (empty when none).
  [[nodiscard]] std::string spellResourceId() const {
    return m_ruleset.spellResource;
  }

  /// The id of the ruleset's primary hit-point pool (the first resource pool
  /// with a minimum of 0), e.g. "HP" / "LP". Empty when none exists.
  [[nodiscard]] std::string resolveHitPointPoolId() const {
    return rpg_os::resolveHitPointPool(m_ruleset);
  }

  /// Resolves a spell's casting check from its raw "A/B/C" attribute list
  /// (e.g. "SGC/SGC/INT", or "COU/INT/CHA (modified by Spirit)" — the
  /// parenthetical suffix and whitespace are ignored). The check is a generic
  /// 3d20 pool roll against the listed attributes with no skill pool.
  template <RandomNumberGenerator Rng>
  [[nodiscard]] CheckResult resolveSpellCheck(const DynamicEntity &actor,
                                              std::string_view checkText, const CheckParams &params,
                                              Rng &rng) const {
    CheckRecipe recipe = makePoolRecipe();

    const std::size_t paren = checkText.find('(');
    const std::string_view head =
        paren == std::string_view::npos ? checkText : checkText.substr(0, paren);
    std::size_t begin = 0;
    while (begin < head.size()) {
      while (begin < head.size() && (head[begin] == ' ' || head[begin] == '/')) {
        ++begin;
      }
      if (begin >= head.size()) {
        break;
      }
      std::size_t end = begin;
      while (end < head.size() && head[end] != '/' && head[end] != ' ') {
        ++end;
      }
      if (recipe.numPoolAttributes < 3) {
        recipe.poolAttributes[recipe.numPoolAttributes++] =
            std::string(head.substr(begin, end - begin));
      }
      begin = end;
    }
    announceCheckStart("spell", actor);
    const CheckResult result = resolveCheck(actor, NullStatProvider{}, recipe, params, rng);
    announceCheckResult("spell", actor, result);
    return result;
  }

  Ruleset m_ruleset;
  bool m_loaded{false};
  std::string m_lastError;
  EventBus m_eventBus;
};

} // namespace rpg_os
