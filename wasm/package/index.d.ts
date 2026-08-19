// Type definitions for @mundus-mirabilis/rpg-os

/** Metadata of a loaded ruleset, used to build a character entry form. */
export interface RulesetMeta {
  id: string;
  name: string;
  spell_resource: string;
  /** Source document the rules were transcribed from. */
  source: string;
  /** The ruleset's OWN licence (not the Apache-2.0 licence of the engine). */
  licence: string;
  /** Optional URL where the rights holder states this licence. */
  licence_source: string;
  /** Optional verbatim notice the licence requires (e.g. the ORC Notice). */
  licence_notice: string;
  /** Optional attribution/credit statement the licence requires. */
  attribution: string;
  attributes: Array<{ id: string; name: string; min: number; max: number; default: number }>;
  skills: Array<{ id: string; name: string; default: number; attributes: string[] }>;
  derived_stats: Array<{ id: string; name: string; formula: string }>;
  resource_pools: Array<{ id: string; name: string; max_stat: string; min: number }>;
}

/** A named ruleset entry (archetype or bestiary creature). */
export interface RulesetEntry {
  id: string;
  name: string;
  kind: 'archetypes' | 'creatures';
}

/** Result of resolving a check. */
export interface CheckResult {
  is_success: boolean;
  is_critical_success: boolean;
  is_critical_failure: boolean;
  margin: number;
  remaining_pool: number;
  quality_level: number;
  raw_dice: number[];
}

/** Result of one fight. `winner_index` is -1 on a draw. */
export interface FightOutcome {
  winner_index: number;
  rounds: number;
  max_lp: [number, number];
  remaining_lp: [number, number];
  a: string;
  b: string;
}

/** One combatant's action in a fight round, as recorded in a `FightLog`. */
export interface FightActionLog {
  /** 0 or 1 — which combatant acted. */
  actor: number;
  /** 0 or 1 — which combatant was acted upon. */
  target: number;
  /** "attack" or "cast". */
  kind: string;
  /** The spell id (kind === "cast", else ""). */
  spell: string;
  /** The human-readable spell name (kind === "cast", else ""). */
  spell_name: string;
  /** The attack/parry or casting-check dice (raw). */
  check_dice: number[];
  /** The attack landed / the cast resolved. */
  is_hit: boolean;
  /** Raw damage dice (weapon attacks and spell damage). */
  damage_dice: number[];
  /** Hit points removed from the target (the actual loss; >= 0). */
  damage: number;
  /** The target's hit points before the action. */
  hp_before: number;
  /** The target's remaining hit points after the action. */
  target_hp: number;
  /** Resource points spent (spell cost), 0 for a weapon. */
  cost: number;
  /** The pool the cost came from ("", "AE", ...). */
  resource: string;
  /** The caster's spell-resource pool before the cast (0 when the ruleset has none). */
  resource_before: number;
  /** The caster's spell-resource pool after the cast (0 when the ruleset has none). */
  resource_after: number;
}

/** One round of a fight as recorded in a `FightLog`. */
export interface FightRoundLog {
  /** 1-based round number. */
  round: number;
  /** Each combatant's Initiative stat. */
  init_stat: [number, number];
  /** Each combatant's raw 1d6 initiative die. */
  init_roll: [number, number];
  /** Stat + roll, for each combatant. */
  init_total: [number, number];
  /** 0 or 1 — which combatant acted first. */
  goes_first: number;
  /** The actions taken, in the order they happened. */
  actions: FightActionLog[];
}

/** The full transcript of one fight. */
export interface FightLog {
  /** Combatant names. */
  names: [string, string];
  /** Each combatant's known spells (human-readable names), in combatant order. */
  spells: [string[], string[]];
  /** The spell-resource pool id (e.g. "AE"), "" when the ruleset has none. */
  resource_id: string;
  /** Each combatant's starting spell-resource value (0 when the ruleset has none). */
  resource_pool: [number, number];
  /** Starting hit points of both combatants. */
  max_lp: [number, number];
  /** 0, 1, or -1 (draw). */
  winner_index: number;
  /** Per-round detail. */
  rounds: FightRoundLog[];
}

/** Result of `fightDetail`: the outcome plus the full transcript. */
export interface FightDetailOutcome extends FightOutcome {
  /** The hit-point pool the fight was fought over (e.g. "LP", "HP"). */
  hp_pool: string;
  /** The fight's full transcript. */
  log: FightLog;
}

export interface CreateEntityOptions {
  /** 0 weakest … 4 strongest, 2 = average (default). */
  variance?: number;
  /** Seed for ranged values; default 1. */
  seed?: number;
}

export interface CheckOptions {
  /** +1 advantage, -1 disadvantage, 0 neutral (default). */
  advantage?: number;
}

export interface FightOptions {
  /** Max rounds before a draw; default 1000. */
  maxRounds?: number;
  /** Seed for a reproducible fight; default 0. */
  seed?: number;
  /** Let spellcasters cast; default true. */
  useMagic?: boolean;
}

/** A live character sheet (wraps a C++ DynamicEntity handle). */
export class Entity {
  setStat(stat: string, value: number): this;
  getStat(stat: string): number;
  getResource(pool: string): number;
  modifyResource(pool: string, delta: number): number;
  refreshResources(): this;
  toJson(): Record<string, unknown>;
  dispose(): void;
}

/** A static combatant description (wraps a C++ CombatantSpec handle). */
export class CombatantSpec {
  dispose(): void;
}

/** The engine: one loaded ruleset and the operations over it. */
export class RpgOs {
  loadRuleset(json: string): this;
  loadRulesetFromFile(filePath: string): this;
  meta(): RulesetMeta;
  entries(): RulesetEntry[];
  createEntity(id: string, options?: CreateEntityOptions): Entity;
  /**
   * Creates an entity from an arbitrary character sheet — a plain stats map
   * like `{ COU: 12, Attack: 12, ... }` or a full `DynamicEntity` save form
   * (`{ stats: {...}, resources: {...}, ... }`).
   */
  createEntityFromSheet(id: string, stats: Record<string, number> | object): Entity;
  check(checkType: string, actor: Entity, target?: Entity | null, options?: CheckOptions): CheckResult;
  specFromId(id: string, weapon?: string): CombatantSpec;
  specFromEntity(entity: Entity, weapon?: string): CombatantSpec;
  fight(specA: CombatantSpec, specB: CombatantSpec, options?: FightOptions): FightOutcome;
  fightDetail(specA: CombatantSpec, specB: CombatantSpec, options?: FightOptions): FightDetailOutcome;
  dispose(): void;
}

/**
 * Initializes the engine (loads + instantiates the WASM module once).
 * Works in Node and the browser.
 */
export function init(): Promise<RpgOs>;

export default init;
