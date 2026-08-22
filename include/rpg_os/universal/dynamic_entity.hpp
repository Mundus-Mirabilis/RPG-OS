// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * @file dynamic_entity.hpp
 * @ingroup rpg_os_universal
 * @brief Dynamic entity (universal mode).
 *
 * Entities hold base attribute values, skill ratings, resource pools, and
 * conditions in @c unordered_map and compute derived stats by evaluating the
 * ruleset's formulas with the AST evaluator. It satisfies the shared
 * @c StatProvider concept, so the same check algorithms from
 * @c core/checks.hpp work here and on the generated specific-mode
 * characters.
 *
 * @par Why maps instead of named members?
 * This is the universal mode: the set of attributes, skills, and derived stats
 * is not known at compile time — it comes from whichever ruleset is loaded.
 * A map is the only representation that can hold "any ruleset's" entities
 * without regenerating code. The price (hash lookups, no compile-time stat
 * names) is exactly what the generated specific mode trades away; the two are
 * complementary, and the parity tests pin them to identical behaviour.
 *
 * @par Ownership
 * A @c DynamicEntity keeps a reference to its @c Ruleset; the ruleset must
 * outlive every entity (the @c RulesetEngine owns both).
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <rpg_os/common/event_system.hpp>
#include <rpg_os/common/json.hpp>
#include <rpg_os/core/advancement.hpp>
#include <rpg_os/core/effects.hpp>
#include <rpg_os/core/entity.hpp>
#include <rpg_os/core/equipment.hpp>
#include <rpg_os/core/inventory.hpp>
#include <rpg_os/core/modifier.hpp>
#include <rpg_os/core/money.hpp>
#include <rpg_os/core/spellbook.hpp>
#include <rpg_os/core/variance.hpp>
#include <rpg_os/universal/expression.hpp>
#include <rpg_os/universal/movement.hpp>
#include <rpg_os/universal/ruleset_loader.hpp>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace rpg_os {

class DynamicEntity;

/// Reads a numeric field from a JSON object; returns false when absent or not
/// numeric.
///
/// @par Why a helper instead of inline lookups?
/// Environment and parameter bags share the same "maybe a number" lookup
/// semantics. Centralising it keeps the error handling (absent / wrong type =>
/// unresolved) identical for @c env.*, @c event.*, and @c action.* paths, so a
/// formula author never sees one prefix behave differently from another.
inline bool readJsonNumber(const Json &obj, std::string_view key, double &out) {
  if (!obj.is_object()) {
    return false;
  }
  const auto it = obj.find(std::string(key));
  if (it == obj.end() || !it->is_number()) {
    return false;
  }
  out = it->get<double>();
  return true;
}

/// Converts a JSON modifier entry into a @ref Modifier.
///
/// @par Why a JSON->Modifier converter here?
/// Both equipped items (`modifiers[]`) and active conditions
/// (`stat_modifiers[]`) express their effects as JSON so a ruleset author
/// never writes C++. This is the single place that turns those JSON entries
/// into the @c core/modifier.hpp steps, so both sources share one vocabulary:
/// `{"type": "add"|"override"|"multiply"|"clamp", "value"/"factor"/bounds}`.
/// Unknown types degrade to a no-op Add(0) rather than throwing — consistent
/// with how the engine treats unexpected rule fields.
inline Modifier parseModifierJson(const Json &mod) {
  Modifier parsed;
  const std::string type = mod.value("type", "add");
  if (type == "override") {
    parsed.type = ModifierType::Override;
    parsed.value = mod.value("value", 0);
  } else if (type == "multiply") {
    parsed.type = ModifierType::Multiply;
    parsed.factor = mod.value("factor", mod.value("value", 1.0));
  } else if (type == "clamp") {
    parsed.type = ModifierType::Clamp;
    parsed.clampMin = mod.value("clamp_min", mod.value("min", 0));
    parsed.clampMax = mod.value("clamp_max", mod.value("max", 0));
  } else { // "add"
    parsed.type = ModifierType::Add;
    parsed.value = mod.value("value", 0);
  }
  return parsed;
}

/// EvalContext for universal mode. Resolves:
///   - bare ids and `actor.X`  -> the actor's stats,
///   - `target.X`              -> the (optional) target's stats,
///   - `effective.X`           -> the target's *effective* stat (raw value plus
///                                equipped-gear and condition modifiers; the
///                                actor when there is no target),
///   - `env.X`                 -> values from an environment bag,
///   - `event.X` / `action.X`  -> values from a parameters bag.
///
/// @par Why one context for all four namespaces?
/// A single formula may legitimately mix scopes (a damage-reduction formula
/// reading both @c actor and @c event). One resolver that knows all four
/// namespaces lets the engine evaluate such formulas naturally, without
/// composing multiple contexts.
class EntityContext final : public EvalContext {
public:
  EntityContext(const DynamicEntity &actor, const DynamicEntity *target, const Json &env,
                const Json &params)
      : m_actor(actor), m_target(target), m_env(env), m_params(params) {}

  [[nodiscard]] bool resolve(std::string_view path, double &out) const override;

private:
  const DynamicEntity &m_actor;
  const DynamicEntity *m_target;
  const Json &m_env;
  const Json &m_params;
};

/**
 * A universal-mode entity: dynamic stats, resources, and conditions.
 *
 * @par Why "dynamic"?
 * Unlike the generated characters, this class can represent *any* ruleset's
 * entities at runtime. It is the workhorse of universal mode, and its
 * flexibility (no recompilation for a new game) is traded against the
 * compile-time guarantees of the specific mode.
 */
class DynamicEntity {
public:
  /// Creates an empty entity bound to `ruleset`. Entities start statless and
  /// are typically populated via @ref loadFromArchetype afterwards.
  DynamicEntity(const Ruleset &ruleset, std::string id)
      : m_ruleset(&ruleset), m_id(std::move(id)), m_entityId(nextEntityId()) {}

  /// The entity's identifier (e.g. the archetype id). Kept for diagnostics
  /// and for payloads that need to name the actor (e.g. @c attacker_id).
  [[nodiscard]] const std::string &id() const noexcept {
    return m_id;
  }

  /// The entity's unique *instance* id, distinct from @ref id (which names
  /// the archetype or creature type shared by every instance of that kind).
  /// Assigned at construction and never reused, so it can address one living
  /// character in a registry, a @ref EntityHandle, or an event payload.
  [[nodiscard]] EntityId entityId() const noexcept {
    return m_entityId;
  }

  /// Signature of a per-entity state-change callback (see @ref setEventSink).
  using EventSink = std::function<void(EventType, const Json &)>;

  /// Sets (or clears, with an empty sink) the per-entity event sink. When
  /// set, the state-changing mutators emit granular events through it
  /// (@c OnStatChanged, @c OnResourceChanged, @c OnTempHpChanged). The engine
  /// wires this to its event bus for every entity it creates, so applications
  /// observe both engine-driven and direct sheet mutations without polling.
  ///
  /// @par Why not emit from every mutator?
  /// Conditions are announced by the engine's @c applyCondition /
  /// @c removeCondition (which fire @c OnConditionChanged with a rich payload
  /// and keep the timeline in sync); direct @c addCondition / @c removeCondition
  /// calls on the sheet deliberately do not emit, so the engine stays the
  /// single source of condition events.
  void setEventSink(EventSink sink) {
    m_eventSink = std::move(sink);
  }

  /// Suppresses event emission while true. The engine sets this while
  /// restoring a saved state via @ref fromJson so a load does not replay every
  /// state change as an event (no listener spam on a fresh session).
  void setEventsSuppressed(bool suppressed) noexcept {
    m_suppressEvents = suppressed;
  }

  /// StatProvider: returns an attribute, skill, or derived stat value.
  ///
  /// @par Why derive-on-demand instead of precomputing?
  /// Derived stats depend on the current base stats (a CON-draining effect
  /// changes the pool maximum). Computing on read keeps the answer always
  /// consistent with the latest mutations, at the cost of re-evaluating the
  /// formula — the intended trade-off for the dynamic mode.
  [[nodiscard]] int32_t getStat(std::string_view statId) const {
    const auto it = m_stats.find(std::string(statId));
    if (it != m_stats.end()) {
      return it->second;
    }
    if (m_ruleset->findDerivedStat(statId) != nullptr) {
      return computeDerived(statId);
    }
    return 0;
  }

  /// Sets a base attribute (or skill rating) value. Emits @c OnStatChanged
  /// through the entity's event sink when the value actually changes.
  void setBaseAttribute(std::string_view attrId, int32_t value) {
    const auto it = m_stats.find(std::string(attrId));
    const int32_t old = it == m_stats.end() ? 0 : it->second;
    m_stats[std::string(attrId)] = value;
    if (old != value) {
      emitEvent(EventType::OnStatChanged,
                Json{{"stat", std::string(attrId)}, {"old_value", old}, {"new_value", value}});
    }
  }

  /// Returns a base attribute (or skill rating) value, or 0 when unset.
  /// Unlike @ref getStat this never evaluates a derived formula — it reads
  /// only the stored base values, which is what bestiary promotion needs.
  [[nodiscard]] int32_t baseAttribute(std::string_view attrId) const {
    const auto it = m_stats.find(std::string(attrId));
    return it == m_stats.end() ? 0 : it->second;
  }

  /// Whether a base attribute (or skill rating) is set.
  [[nodiscard]] bool hasBaseAttribute(std::string_view attrId) const {
    return m_stats.find(std::string(attrId)) != m_stats.end();
  }

  /// Current value of a resource pool, or 0 when absent.
  [[nodiscard]] int32_t resource(std::string_view resourceId) const {
    const auto it = m_resources.find(std::string(resourceId));
    return it == m_resources.end() ? 0 : it->second.current;
  }

  /// Applies `delta` to a resource pool (clamped to its bounds); returns the
  /// amount actually applied (see @ref ResourcePool::modify). Missing pools
  /// are a no-op returning 0, so damage against an undeclared resource cannot
  /// crash a caller. Emits @c OnResourceChanged through the entity's event
  /// sink when the pool actually moves.
  [[nodiscard]] int32_t modifyResource(std::string_view resourceId, int32_t delta) {
    const auto it = m_resources.find(std::string(resourceId));
    if (it == m_resources.end()) {
      return 0;
    }
    const int32_t old = it->second.current;
    const int32_t applied = it->second.modify(delta);
    if (applied != 0) {
      emitEvent(EventType::OnResourceChanged, Json{{"resource", std::string(resourceId)},
                                                   {"delta", applied},
                                                   {"old_value", old},
                                                   {"new_value", it->second.current}});
    }
    return applied;
  }

  /// Adds `stacks` of a condition (stacks accumulate).
  ///
  /// @par Why stacks?
  /// Several rulesets model conditions that stack (multiple poison doses, or
  /// repeated applications). Counting stacks in the map keeps that information
  /// without a separate per-condition structure.
  void addCondition(std::string_view conditionId, int32_t stacks = 1) {
    m_conditions[std::string(conditionId)] += stacks;
  }

  /// Removes a condition entirely.
  void removeCondition(std::string_view conditionId) {
    m_conditions.erase(std::string(conditionId));
  }

  /// Whether the entity currently has the condition. A condition with stacks
  /// reduced to 0 (or removed) is treated as absent.
  [[nodiscard]] bool hasCondition(std::string_view conditionId) const {
    const auto it = m_conditions.find(std::string(conditionId));
    return it != m_conditions.end() && it->second > 0;
  }

  /// Stack count of a condition (0 when absent).
  [[nodiscard]] int32_t conditionStacks(std::string_view conditionId) const {
    const auto it = m_conditions.find(std::string(conditionId));
    return it == m_conditions.end() ? 0 : it->second;
  }

  /// The raw condition -> stack-count map (immutable view).
  [[nodiscard]] const std::unordered_map<std::string, int32_t> &conditions() const noexcept {
    return m_conditions;
  }

  /// Adds `traitId` to the entity's active traits (an always-on, inherent
  /// quality — a monster's Pack Tactics, a species' Darkvision, ...).
  void addTrait(std::string_view traitId) {
    m_traits.insert(std::string(traitId));
  }

  /// Removes `traitId` from the entity's active traits.
  void removeTrait(std::string_view traitId) {
    m_traits.erase(std::string(traitId));
  }

  /// Whether `traitId` is an active trait of the entity.
  [[nodiscard]] bool hasTrait(std::string_view traitId) const {
    return m_traits.find(std::string(traitId)) != m_traits.end();
  }

  /// The entity's active trait ids (immutable view).
  [[nodiscard]] const std::unordered_set<std::string> &traits() const noexcept {
    return m_traits;
  }

  /// The restriction and capability tokens contributed by the entity's active
  /// conditions and traits (deduplicated, source order). Restrictions say what
  /// the entity *cannot* do ("no_action", "no_bonus_action", "no_reaction",
  /// "no_move", "no_speak", "no_concentration", "no_cast", ...); capabilities
  /// say what it *can* do (movement / senses: "swim", "climb", "breath_water",
  /// "darkvision", ...). Both are declared as string arrays on condition /
  /// trait records, so any ruleset can express its own action-economy and
  /// mobility rules without engine changes.
  [[nodiscard]] std::vector<std::string> collectTokens(std::string_view field) const {
    std::vector<std::string> out;
    const auto addToken = [&out](const std::string &token) {
      if (std::find(out.begin(), out.end(), token) == out.end()) {
        out.push_back(token);
      }
    };
    for (const auto &[conditionId, stacks] : m_conditions) {
      if (stacks <= 0) {
        continue;
      }
      const Json *record = findDataRecord("conditions", conditionId);
      if (record == nullptr || !record->contains(field) || !record->at(field).is_array()) {
        continue;
      }
      for (const Json &token : record->at(field)) {
        addToken(token.get<std::string>());
      }
    }
    for (const std::string &traitId : m_traits) {
      const Json *record = findDataRecord("traits", traitId);
      if (record == nullptr || !record->contains(field) || !record->at(field).is_array()) {
        continue;
      }
      for (const Json &token : record->at(field)) {
        addToken(token.get<std::string>());
      }
    }
    return out;
  }

  /// The restriction tokens in effect on the entity (see @ref collectTokens).
  [[nodiscard]] std::vector<std::string> restrictions() const {
    return collectTokens("restrictions");
  }

  /// Whether the entity is under the given restriction (e.g. "no_action").
  [[nodiscard]] bool hasRestriction(std::string_view token) const {
    return std::ranges::any_of(
        restrictions(), [token](const std::string &restriction) { return restriction == token; });
  }

  /// The capability tokens the entity has: from active conditions and traits
  /// (see @ref collectTokens) plus — when a current terrain is set — the
  /// `grants` of every *equipped* (worn) item whose record's `terrain`
  /// section has an entry for that terrain. So "this amulet lets you breathe
  /// underwater" is applied automatically the moment the character is in
  /// water, and a terrain that requires a capability for a movement mode
  /// (e.g. walking in deep water needs "breath_water") becomes possible when
  /// the right item is worn. A passive grant needs the item to be worn, not
  /// merely carried; terrain *harm* (unusable / ruined) still applies to any
  /// carried instance.
  [[nodiscard]] std::vector<std::string> capabilities() const {
    std::vector<std::string> out = collectTokens("capabilities");
    if (m_terrain.empty()) {
      return out;
    }
    const auto addToken = [&out](const std::string &token) {
      if (std::find(out.begin(), out.end(), token) == out.end()) {
        out.push_back(token);
      }
    };
    for (const auto &[slot, itemId] : m_equipment.slots()) {
      const TerrainItemEffect effect = itemTerrainEffect(itemId);
      for (const std::string &grant : effect.grants) {
        addToken(grant);
      }
    }
    return out;
  }

  /// Whether the entity has the given capability (e.g. "swim").
  [[nodiscard]] bool hasCapability(std::string_view token) const {
    return std::ranges::any_of(
        capabilities(), [token](const std::string &capability) { return capability == token; });
  }

  /// The entity's current terrain / surrounding id ("" = none set; the engine
  /// falls back to the ruleset's default terrain). Changing it automatically
  /// changes which item terrain effects apply and which movement modes are
  /// possible.
  [[nodiscard]] const std::string &terrain() const noexcept {
    return m_terrain;
  }
  /// Sets the entity's current terrain / surrounding ("" clears it back to
  /// the ruleset default).
  void setTerrain(std::string_view terrainId) {
    m_terrain = std::string(terrainId);
  }

  /// The terrain-dependent behaviour of `itemId` in the entity's current
  /// terrain (see @ref TerrainItemEffect): whether the item is unusable /
  /// ruined here and what capabilities it grants. All-off when the entity has
  /// no current terrain or the item declares no entry for it.
  [[nodiscard]] TerrainItemEffect itemTerrainStatus(std::string_view itemId) const {
    return itemTerrainEffect(itemId);
  }

  /// The effective value of `statId`: the raw value plus every modifier the
  /// sheet's equipped gear and active conditions contribute (through the
  /// shared modifier pipeline in @c core/modifier.hpp), plus any temporary
  /// stat bonuses from active effects (spells, buffs, traits).
  ///
  /// @par Why not replace getStat with this?
  /// Derived-stat formulas evaluate other stats and must not re-apply gear
  /// bonuses (an equipped +1 STR must not inflate STR-derived stats *and*
  /// feed back). @ref getStat stays the raw, formula-safe value; this is the
  /// gameplay-facing number.
  [[nodiscard]] int32_t getEffectiveStat(std::string_view statId) const {
    return applyModifierPipeline(getStat(statId), modifiersFor(statId)) +
           m_effects.bonusFor(statId);
  }

  /// The sheet's owned items (flat inventory).
  [[nodiscard]] Inventory &inventory() noexcept {
    return m_inventory;
  }
  [[nodiscard]] const Inventory &inventory() const noexcept {
    return m_inventory;
  }

  /// The sheet's worn gear (slot -> item).
  [[nodiscard]] Equipment &equipment() noexcept {
    return m_equipment;
  }
  [[nodiscard]] const Equipment &equipment() const noexcept {
    return m_equipment;
  }

  /// The sheet's wealth.
  [[nodiscard]] Money &money() noexcept {
    return m_money;
  }
  [[nodiscard]] const Money &money() const noexcept {
    return m_money;
  }

  /// Current Temporary Hit Points (a damage buffer that is consumed before the
  /// real hit-point pool; see @ref RulesetEngine::applyDamage).
  [[nodiscard]] int32_t temporaryHitPoints() const noexcept {
    return m_tempHp;
  }
  /// Replaces the Temporary Hit Points with `value` (never below 0). Emits
  /// @c OnTempHpChanged through the entity's event sink on a change.
  void setTemporaryHitPoints(int32_t value) {
    const int32_t old = m_tempHp;
    m_tempHp = value > 0 ? value : 0;
    if (m_tempHp != old) {
      emitEvent(EventType::OnTempHpChanged,
                Json{{"delta", m_tempHp - old}, {"old_value", old}, {"new_value", m_tempHp}});
    }
  }
  /// Adds `delta` to Temporary Hit Points (clamped at 0; a positive delta
  /// keeps the higher of the current and new pool, matching "they don't
  /// stack, you keep the higher" D&D rule). Emits @c OnTempHpChanged through
  /// the entity's event sink on a change.
  void addTemporaryHitPoints(int32_t delta) {
    const int32_t old = m_tempHp;
    m_tempHp = std::max<int32_t>(0, m_tempHp + delta);
    if (m_tempHp != old) {
      emitEvent(EventType::OnTempHpChanged,
                Json{{"delta", m_tempHp - old}, {"old_value", old}, {"new_value", m_tempHp}});
    }
  }

  /// The sheet's known / prepared spells.
  [[nodiscard]] Spellbook &spellbook() noexcept {
    return m_spellbook;
  }
  [[nodiscard]] const Spellbook &spellbook() const noexcept {
    return m_spellbook;
  }

  /// The sheet's experience / advancement state.
  [[nodiscard]] Advancement &advancement() noexcept {
    return m_advancement;
  }
  [[nodiscard]] const Advancement &advancement() const noexcept {
    return m_advancement;
  }

  /// The sheet's active-effect timeline (conditions with durations).
  [[nodiscard]] EffectTimeline &effects() noexcept {
    return m_effects;
  }
  [[nodiscard]] const EffectTimeline &effects() const noexcept {
    return m_effects;
  }

  /// Active afflictions (poisons / diseases / curses) applied to the sheet.
  /// The shared @c rpg_os::AppliedAffliction value type round-trips the same
  /// save-state shape as the generated characters.
  [[nodiscard]] std::vector<AppliedAffliction> &afflictions() noexcept {
    return m_afflictions;
  }
  [[nodiscard]] const std::vector<AppliedAffliction> &afflictions() const noexcept {
    return m_afflictions;
  }

  /// Records an applied affliction (called by the engine's applyAffliction).
  void addAffliction(std::string_view section, std::string_view id,
                     const std::vector<std::string> &conditions = {}) {
    m_afflictions.push_back(AppliedAffliction{std::string(section), std::string(id), conditions});
  }

  /// Whether the sheet is resistant (or immune) to a damage type (e.g.
  /// "Ranged" from a magic shield). Populated by structured effects.
  [[nodiscard]] bool hasResistance(std::string_view type) const {
    return m_resistances.find(std::string(type)) != m_resistances.end();
  }
  /// Marks the sheet as resistant to `type`.
  void addResistance(std::string_view type) {
    m_resistances.insert(std::string(type));
  }
  /// Removes the resistance to `type`.
  void removeResistance(std::string_view type) {
    m_resistances.erase(std::string(type));
  }
  /// All damage types the sheet is resistant to (immutable view).
  [[nodiscard]] const std::unordered_set<std::string> &resistances() const noexcept {
    return m_resistances;
  }

  /// Serializes the whole sheet (stats, resources, conditions, inventory,
  /// equipment, money, spellbook, advancement, effects) to a JSON object —
  /// the save-game form. @ref id is included but restored by the caller
  /// (it is fixed at construction).
  void toJson(Json &out) const {
    out = Json::object();
    out["id"] = m_id;
    out["entity_id"] = m_entityId.value;
    out["stats"] = m_stats;
    out["terrain"] = m_terrain;
    Json resources = Json::object();
    for (const auto &[id, pool] : m_resources) {
      resources[id] = pool.current;
    }
    out["resources"] = resources;
    out["conditions"] = m_conditions;
    Json traits = Json::array();
    for (const std::string &trait : m_traits) {
      traits.push_back(trait);
    }
    out["traits"] = traits;
    Json inventoryJson;
    m_inventory.toJson(inventoryJson);
    out["inventory"] = inventoryJson;
    Json equipmentJson;
    m_equipment.toJson(equipmentJson);
    out["equipment"] = equipmentJson;
    out["money"] = m_money.baseUnits();
    out["temp_hp"] = m_tempHp;
    Json spellbookJson;
    m_spellbook.toJson(spellbookJson);
    out["spellbook"] = spellbookJson;
    Json advancementJson;
    m_advancement.toJson(advancementJson);
    out["advancement"] = advancementJson;
    Json effectsJson;
    m_effects.toJson(effectsJson);
    out["effects"] = effectsJson;
    Json afflictions = Json::array();
    for (const AppliedAffliction &aff : m_afflictions) {
      afflictions.push_back(
          {{"section", aff.section}, {"id", aff.id}, {"conditions", aff.conditions}});
    }
    out["afflictions"] = afflictions;
    Json resistances = Json::array();
    for (const std::string &type : m_resistances) {
      resistances.push_back(type);
    }
    out["resistances"] = resistances;
  }

  /// Restores the sheet from the JSON form produced by @ref toJson (the id is
  /// kept from construction). State not present in `in` is left unchanged.
  void fromJson(const Json &in) {
    if (!in.is_object()) {
      return;
    }
    if (in.contains("entity_id") && in.at("entity_id").is_number_unsigned()) {
      m_entityId = EntityId{in.at("entity_id").get<uint64_t>()};
    }
    restoreStats(in);
    restoreConditions(in);
    restoreTraits(in);
    restoreResources(in);
    restoreInventory(in);
    restoreEquipment(in);
    restoreMoney(in);
    restoreTempHp(in);
    restoreSpellbook(in);
    restoreAdvancement(in);
    restoreEffects(in);
    restoreAfflictions(in);
    restoreResistances(in);
    restoreTerrain(in);
  }

  /// (Re)creates the resource pools from the ruleset definitions, computing
  /// each maximum from the current stats. Newly created pools start at their
  /// full derived maximum; existing pools keep their current value while
  /// their bounds are refreshed.
  ///
  /// @par Why public?
  /// A sheet built by hand — a character entered into a form, restored from
  /// JSON, or otherwise never loaded from an archetype — has no pools yet.
  /// @c createFighter needs this so arbitrary characters get their hit-point
  /// pools before a fight.
  void refreshResources() {
    for (const ResourcePoolDef &def : m_ruleset->resourcePools) {
      const int32_t maxValue = getStat(def.maxStat);
      const auto it = m_resources.find(def.id);
      if (it == m_resources.end()) {
        m_resources.emplace(def.id, ResourcePool{maxValue, def.minValue, maxValue});
      } else {
        it->second.min = def.minValue;
        it->second.max = maxValue;
      }
    }
  }

  /// Loads attributes, skills, resources, and conditions from an archetype
  /// JSON record (ranged values picked at random), then (re)initializes the
  /// resource pools. This overload uses a fresh entropy-seeded RNG.
  void loadFromArchetype(const Json &archetype) {
    DefaultRandom rng;
    loadFromArchetype(archetype, Variance::Random, rng);
  }

  /// As above, but picks ranged values according to `variance` (weakest, weak,
  /// average, strong, strongest, or random) with the caller-supplied RNG.
  /// The template overload exists so deterministic tests and Monte-Carlo runs
  /// can inject a scripted/seeded RNG.
  template <RandomNumberGenerator Rng>
  void loadFromArchetype(const Json &archetype, Variance variance, Rng &rng) {
    loadAttributes(archetype, variance, rng);
    loadSkills(archetype, variance, rng);
    refreshResources();
    loadResources(archetype, variance, rng);
    loadConditions(archetype);
    loadTraits(archetype);
    loadWealth(archetype);
    loadEquipment(archetype);
    loadSpellsKnown(archetype);
    loadXp(archetype);
    loadLevel(archetype);
  }

private:
  // ---- serialization helpers (see @ref fromJson) ---------------------------
  /// Restores the stat map from a serialized sheet.
  void restoreStats(const Json &in) {
    if (in.contains("stats") && in.at("stats").is_object()) {
      m_stats = in.at("stats").get<std::unordered_map<std::string, int32_t>>();
    }
  }
  /// Restores the active conditions and their stack counts.
  void restoreConditions(const Json &in) {
    if (in.contains("conditions") && in.at("conditions").is_object()) {
      m_conditions = in.at("conditions").get<std::unordered_map<std::string, int32_t>>();
    }
  }
  /// Restores the trait ids.
  void restoreTraits(const Json &in) {
    if (in.contains("traits") && in.at("traits").is_array()) {
      m_traits.clear();
      for (const Json &trait : in.at("traits")) {
        m_traits.insert(trait.get<std::string>());
      }
    }
  }
  /// Restores the resource pool current values.
  void restoreResources(const Json &in) {
    if (in.contains("resources") && in.at("resources").is_object()) {
      for (const auto &[id, value] : in.at("resources").items()) {
        m_resources[id].current = value.get<int32_t>();
      }
    }
  }
  /// Restores the inventory (may be absent).
  void restoreInventory(const Json &in) {
    if (in.contains("inventory")) {
      m_inventory.fromJson(in.at("inventory"));
    }
  }
  /// Restores the equipped items (may be absent).
  void restoreEquipment(const Json &in) {
    if (in.contains("equipment")) {
      m_equipment.fromJson(in.at("equipment"));
    }
  }
  /// Restores the money purse (may be absent).
  void restoreMoney(const Json &in) {
    if (in.contains("money") && in.at("money").is_number_integer()) {
      m_money = Money{in.at("money").get<int64_t>()};
    }
  }
  /// Restores the temporary hit points (may be absent).
  void restoreTempHp(const Json &in) {
    if (in.contains("temp_hp") && in.at("temp_hp").is_number_integer()) {
      m_tempHp = in.at("temp_hp").get<int32_t>();
    }
  }
  /// Restores the spellbook (may be absent).
  void restoreSpellbook(const Json &in) {
    if (in.contains("spellbook")) {
      m_spellbook.fromJson(in.at("spellbook"));
    }
  }
  /// Restores the advancement record (may be absent).
  void restoreAdvancement(const Json &in) {
    if (in.contains("advancement")) {
      m_advancement.fromJson(in.at("advancement"));
    }
  }
  /// Restores the effect timeline (may be absent).
  void restoreEffects(const Json &in) {
    if (in.contains("effects")) {
      m_effects.fromJson(in.at("effects"));
    }
  }
  /// Restores the applied afflictions.
  void restoreAfflictions(const Json &in) {
    if (in.contains("afflictions") && in.at("afflictions").is_array()) {
      m_afflictions.clear();
      for (const Json &entry : in.at("afflictions")) {
        AppliedAffliction aff;
        aff.section = entry.value("section", "");
        aff.id = entry.value("id", "");
        if (entry.contains("conditions") && entry.at("conditions").is_array()) {
          for (const Json &cond : entry.at("conditions")) {
            aff.conditions.push_back(cond.get<std::string>());
          }
        }
        m_afflictions.push_back(std::move(aff));
      }
    }
  }
  /// Restores the damage-type resistances.
  void restoreResistances(const Json &in) {
    if (in.contains("resistances") && in.at("resistances").is_array()) {
      m_resistances.clear();
      for (const Json &type : in.at("resistances")) {
        m_resistances.insert(type.get<std::string>());
      }
    }
  }
  /// Restores the current terrain / surrounding.
  void restoreTerrain(const Json &in) {
    if (in.contains("terrain") && in.at("terrain").is_string()) {
      m_terrain = in.at("terrain").get<std::string>();
    }
  }

  // ---- archetype-loading helpers (see @ref loadFromArchetype) ---------------
  /// Loads attribute values (ranged values picked per `variance`).
  template <RandomNumberGenerator Rng>
  void loadAttributes(const Json &archetype, Variance variance, Rng &rng) {
    if (archetype.contains("attributes")) {
      for (const auto &[statId, value] : archetype.at("attributes").items()) {
        m_stats[statId] = readVariantValue(value, variance, rng);
      }
    }
  }
  /// Loads skill values (ranged values picked per `variance`).
  template <RandomNumberGenerator Rng>
  void loadSkills(const Json &archetype, Variance variance, Rng &rng) {
    if (archetype.contains("skills")) {
      for (const auto &[skillId, value] : archetype.at("skills").items()) {
        m_stats[skillId] = readVariantValue(value, variance, rng);
      }
    }
  }
  /// Loads resource pool current values (ranged values picked per `variance`).
  template <RandomNumberGenerator Rng>
  void loadResources(const Json &archetype, Variance variance, Rng &rng) {
    if (archetype.contains("resources")) {
      for (const auto &[resourceId, value] : archetype.at("resources").items()) {
        m_resources[resourceId].current = readVariantValue(value, variance, rng);
      }
    }
  }
  /// Loads the starting conditions and their stack counts.
  void loadConditions(const Json &archetype) {
    if (archetype.contains("conditions")) {
      for (const auto &[conditionId, value] : archetype.at("conditions").items()) {
        m_conditions[conditionId] = value.get<int32_t>();
      }
    }
  }
  /// Loads the starting trait ids.
  void loadTraits(const Json &archetype) {
    if (archetype.contains("traits") && archetype.at("traits").is_array()) {
      for (const Json &trait : archetype.at("traits")) {
        if (trait.is_string()) {
          m_traits.insert(trait.get<std::string>());
        }
      }
    }
  }
  /// Bookkeeping fields an archetype (or bestiary entry) may declare. Each is
  /// optional: a ruleset without money / gear / spells simply omits them.
  void loadWealth(const Json &archetype) {
    if (archetype.contains("wealth")) {
      const Json &wealth = archetype.at("wealth");
      if (wealth.is_object() && !m_ruleset->currencySystem.id.empty()) {
        int64_t total = 0;
        for (const auto &[denom, qty] : wealth.items()) {
          total += m_ruleset->currencySystem.valueOf(denom, qty.get<int64_t>());
        }
        m_money = Money{total};
      } else if (wealth.is_number_integer()) {
        m_money = Money{wealth.get<int64_t>()};
      }
    }
  }
  /// Loads starting equipment (either a slot->item map or a list of entries).
  void loadEquipment(const Json &archetype) {
    if (archetype.contains("equipment")) {
      const Json &equip = archetype.at("equipment");
      if (equip.is_object()) {
        for (const auto &[slot, item] : equip.items()) {
          (void)m_equipment.equip(slot, item.get<std::string>());
        }
      } else if (equip.is_array()) {
        for (const Json &entry : equip) {
          (void)m_equipment.equip(entry.value("slot", ""), entry.value("item", ""));
        }
      }
    }
  }
  /// Loads the spells the character knows.
  void loadSpellsKnown(const Json &archetype) {
    if (archetype.contains("spells_known")) {
      for (const Json &spellId : archetype.at("spells_known")) {
        m_spellbook.learn(spellId.get<std::string>());
      }
    }
  }
  /// Loads the starting experience points.
  void loadXp(const Json &archetype) {
    if (archetype.contains("xp")) {
      m_advancement.gainXp(archetype.at("xp").get<int64_t>());
    }
  }
  /// Loads the starting level.
  void loadLevel(const Json &archetype) {
    if (archetype.contains("level")) {
      m_advancement.level = archetype.at("level").get<int32_t>();
    }
  }

  /// Resolves a modifier numeric `value` (in `mod` under `key`) that may be a
  /// plain number or a formula string evaluated against this entity. A formula
  /// is compiled once per text (cached) and evaluated against the wearer's own
  /// stats, so a gear modifier can depend on the wearer — D&D medium armour
  /// caps its Dexterity contribution as `"10 + min(DEX_mod, 2) + 4"`. An
  /// absent, non-numeric, or unresolvable value degrades to `fallback` rather
  /// than throwing — consistent with how the engine treats unexpected rule
  /// fields.
  [[nodiscard]] int32_t resolveModifierNumber(const Json &mod, std::string_view key,
                                              int32_t fallback, const EntityContext &ctx) const {
    const auto it = mod.find(std::string(key));
    if (it == mod.end()) {
      return fallback;
    }
    const Json &value = *it;
    if (value.is_number()) {
      return value.get<int32_t>();
    }
    if (value.is_string()) {
      const std::string text = value.get<std::string>();
      auto cit = m_formulaCache.find(text);
      if (cit == m_formulaCache.end()) {
        try {
          cit = m_formulaCache.emplace(text, rpg_os::Expression(text)).first;
        } catch (const std::exception &) {
          return fallback; // malformed formula: ignore, like other misuse
        }
      }
      try {
        return math::toStat(cit->second.evaluate(ctx));
      } catch (const std::exception &) {
        return fallback; // unresolvable identifier / division by zero
      }
    }
    return fallback;
  }

  /// As @ref resolveModifierNumber, but for a multiplier (`factor`) kept as a
  /// double.
  [[nodiscard]] double resolveModifierFactor(const Json &mod, std::string_view key, double fallback,
                                             const EntityContext &ctx) const {
    const auto it = mod.find(std::string(key));
    if (it == mod.end()) {
      return fallback;
    }
    const Json &value = *it;
    if (value.is_number()) {
      return value.get<double>();
    }
    if (value.is_string()) {
      const std::string text = value.get<std::string>();
      auto cit = m_formulaCache.find(text);
      if (cit == m_formulaCache.end()) {
        try {
          cit = m_formulaCache.emplace(text, rpg_os::Expression(text)).first;
        } catch (const std::exception &) {
          return fallback;
        }
      }
      try {
        return cit->second.evaluate(ctx);
      } catch (const std::exception &) {
        return fallback;
      }
    }
    return fallback;
  }

  /// Appends the modifiers a single data record contributes to `statId`: gear
  /// `modifiers` (stat field `target_stat`) and condition / trait
  /// `stat_modifiers` (stat field `stat`) share one shape — an array of
  /// modifier objects — so one helper serves all three. `stackScale` multiplies
  /// Add modifiers (a condition's per-stack modifiers scale with its stacks).
  ///
  /// @par Numeric vs formula values
  /// A modifier's `value` / `factor` / clamp bounds may be a plain number or a
  /// formula string resolved against the *wearer* (see @ref
  /// resolveModifierNumber). Numeric modifiers take the shared
  /// @ref parseModifierJson fast path; any string-valued field switches to the
  /// type-aware path so the formula is evaluated, never left as a raw string.
  void appendRecordModifiers(std::vector<Modifier> &result, const EntityContext &ctx,
                             const Json *record, std::string_view arrayField,
                             std::string_view statField, std::string_view statId,
                             int32_t stackScale = 1) const {
    if (record == nullptr || !record->contains(arrayField) || !record->at(arrayField).is_array()) {
      return;
    }
    const auto hasFormulaField = [](const Json &mod) {
      for (const char *key : {"value", "factor", "clamp_min", "clamp_max", "min", "max"}) {
        if (mod.contains(key) && mod.at(key).is_string()) {
          return true;
        }
      }
      return false;
    };
    for (const Json &mod : record->at(arrayField)) {
      if (mod.value(statField, "") != statId) {
        continue;
      }
      Modifier parsed;
      if (!hasFormulaField(mod)) {
        parsed = parseModifierJson(mod); // numeric fast path
      } else {
        const std::string type = mod.value("type", "add");
        if (type == "override") {
          parsed.type = ModifierType::Override;
          parsed.value = resolveModifierNumber(mod, "value", 0, ctx);
        } else if (type == "multiply") {
          parsed.type = ModifierType::Multiply;
          parsed.factor = mod.contains("factor")
                              ? resolveModifierFactor(mod, "factor", 1.0, ctx)
                              : static_cast<double>(resolveModifierNumber(mod, "value", 1, ctx));
        } else if (type == "clamp") {
          parsed.type = ModifierType::Clamp;
          parsed.clampMin =
              resolveModifierNumber(mod, mod.contains("clamp_min") ? "clamp_min" : "min", 0, ctx);
          parsed.clampMax =
              resolveModifierNumber(mod, mod.contains("clamp_max") ? "clamp_max" : "max", 0, ctx);
        } else { // "add"
          parsed.type = ModifierType::Add;
          parsed.value = resolveModifierNumber(mod, "value", 0, ctx);
        }
      }
      if (stackScale != 1 && parsed.type == ModifierType::Add) {
        parsed.value *= stackScale;
      }
      result.push_back(parsed);
    }
  }

  /// Collects the modifier steps affecting `statId` from the sheet's equipped
  /// items and active conditions. Gear modifiers apply once per equipped item;
  /// a condition's per-stack modifiers scale additively with its stack count
  /// (Fear I-IV in The Dark Eye is "-1 per level on checks").
  [[nodiscard]] std::vector<Modifier> modifiersFor(std::string_view statId) const {
    std::vector<Modifier> result;
    // One context for the whole sweep: formula-valued modifiers resolve against
    // this entity, and reusing the context avoids rebuilding it per record.
    const EntityContext ctx(*this, nullptr, Json::object(), Json::object());
    for (const auto &[slot, itemId] : m_equipment.slots()) {
      appendRecordModifiers(result, ctx, findDataRecord("items", itemId), "modifiers",
                            "target_stat", statId);
    }
    for (const auto &[conditionId, stacks] : m_conditions) {
      if (stacks <= 0) {
        continue;
      }
      appendRecordModifiers(result, ctx, findDataRecord("conditions", conditionId),
                            "stat_modifiers", "stat", statId, stacks);
    }
    for (const std::string &traitId : m_traits) {
      appendRecordModifiers(result, ctx, findDataRecord("traits", traitId), "stat_modifiers",
                            "stat", statId);
    }
    return result;
  }

  /// Looks up a raw data record by id in a `data` section (items, conditions,
  /// ...) — the sheet-side twin of the engine's record finders.
  [[nodiscard]] const Json *findDataRecord(std::string_view section, std::string_view id) const {
    return rpg_os::findDataRecord(*m_ruleset, section, id);
  }

  /// The terrain behaviour of `itemId` in the entity's current terrain (from
  /// the item record's `terrain` object). All-off when no terrain is set or
  /// the item declares no entry for the current terrain.
  [[nodiscard]] TerrainItemEffect itemTerrainEffect(std::string_view itemId) const {
    TerrainItemEffect effect;
    if (m_terrain.empty()) {
      return effect;
    }
    const Json *item = findDataRecord("items", itemId);
    if (item == nullptr || !item->contains("terrain") || !item->at("terrain").is_object()) {
      return effect;
    }
    const Json &terrainSection = item->at("terrain");
    const auto it = terrainSection.find(m_terrain);
    if (it == terrainSection.end() || !it->is_object()) {
      return effect;
    }
    const Json &entry = *it;
    effect.unusable = entry.value("unusable", false);
    effect.ruined = entry.value("ruined", false);
    if (entry.contains("grants") && entry.at("grants").is_array()) {
      for (const Json &token : entry.at("grants")) {
        effect.grants.push_back(token.get<std::string>());
      }
    }
    return effect;
  }

  /// Evaluates a derived-stat formula for this entity (no target / env).
  /// An empty environment/parameters bag keeps the evaluation total even
  /// though this path only ever resolves bare stat ids.
  [[nodiscard]] int32_t computeDerived(std::string_view statId) const {
    const DerivedStatDef *def = m_ruleset->findDerivedStat(statId);
    if (def == nullptr) {
      return 0;
    }
    const Json emptyEnv = Json::object();
    const Json emptyParams = Json::object();
    const EntityContext context(*this, nullptr, emptyEnv, emptyParams);
    return math::toStat(def->expression.evaluate(context));
  }

  /// Emits `payload` for `type` through the entity's event sink, unless event
  /// emission is suppressed (restoring a saved state) or no sink is set.
  void emitEvent(EventType type, const Json &payload) const {
    if (m_suppressEvents || m_eventSink == nullptr) {
      return;
    }
    m_eventSink(type, payload);
  }

  const Ruleset *m_ruleset;
  std::string m_id;
  EntityId m_entityId{};
  EventSink m_eventSink; // empty by default (no per-entity events until set)
  bool m_suppressEvents{false};
  std::unordered_map<std::string, int32_t> m_stats;
  std::unordered_map<std::string, ResourcePool> m_resources;
  std::unordered_map<std::string, int32_t> m_conditions;
  std::unordered_set<std::string> m_traits;
  Inventory m_inventory;
  Equipment m_equipment;
  Money m_money;
  int32_t m_tempHp{0};
  Spellbook m_spellbook;
  Advancement m_advancement;
  EffectTimeline m_effects;
  std::vector<AppliedAffliction> m_afflictions;
  std::unordered_set<std::string> m_resistances;
  /// Compiled formula modifiers, cached per formula text so the (small, fixed)
  /// set of formulas a ruleset uses is parsed once per entity instead of once
  /// per check. Mutable: populated lazily from @ref getEffectiveStat.
  mutable std::unordered_map<std::string, rpg_os::Expression> m_formulaCache;
  std::string m_terrain; ///< current terrain / surrounding ("" = ruleset default)
};

inline bool EntityContext::resolve(std::string_view path, double &out) const {
  const std::size_t dot = path.find('.');
  if (dot == std::string_view::npos) {
    out = static_cast<double>(m_actor.getStat(path));
    return true;
  }
  const std::string_view prefix = path.substr(0, dot);
  const std::string_view rest = path.substr(dot + 1);
  if (prefix == "actor") {
    out = static_cast<double>(m_actor.getStat(rest));
    return true;
  }
  if (prefix == "target") {
    out = m_target != nullptr ? static_cast<double>(m_target->getStat(rest)) : 0.0;
    return true;
  }
  if (prefix == "effective") {
    const DynamicEntity &subject = m_target != nullptr ? *m_target : m_actor;
    out = static_cast<double>(subject.getEffectiveStat(rest));
    return true;
  }
  if (prefix == "env") {
    return readJsonNumber(m_env, rest, out);
  }
  if (prefix == "event" || prefix == "action") {
    return readJsonNumber(m_params, rest, out);
  }
  return false;
}

/// A StatProvider adapter that resolves stats through a sheet's effective
/// value (base + equipped-item modifiers + active-condition modifiers) instead
/// of the raw stored value.
///
/// @par Why a wrapper instead of changing getStat?
/// Derived-stat formulas must read raw values (an equipped +1 STR must not
/// feed back into STR itself through a formula). Keeping @ref DynamicEntity::
/// getStat raw and wrapping it for *resolution* lets checks and combat see the
/// gear-influenced number while formulas stay stable.
struct EffectiveStatProvider {
  const DynamicEntity &entity;
  [[nodiscard]] int32_t getStat(std::string_view statId) const {
    return entity.getEffectiveStat(statId);
  }
};

static_assert(StatProvider<DynamicEntity>);

} // namespace rpg_os
