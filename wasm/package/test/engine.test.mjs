// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

// End-to-end tests for the @mundus-mirabilis/rpg-os WASM package. They run
// against the real built WASM module (wasm/package/dist/) and the shipped
// rulesets, so they validate the whole compile + wrapper pipeline. The parity
// test additionally compares a seeded WASM fight against the native
// rpg_os_example_fight binary (skipped when the binary is not built).

import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { existsSync } from 'node:fs';
import { fileURLToPath } from 'node:url';
import path from 'node:path';
import { init } from '../index.mjs';

const here = path.dirname(fileURLToPath(import.meta.url));
const packageRoot = path.resolve(here, '..');
const repoRoot = path.resolve(packageRoot, '..', '..');
const tdePath = path.join(packageRoot, 'rulesets', 'tde5e_core.json');
const dndPath = path.join(packageRoot, 'rulesets', 'dnd5e_srd.json');
const nativeBinary = path.join(repoRoot, 'build', 'bin', 'rpg_os_example_fight');

let rpg;
test.before(async () => {
  rpg = await init();
});

test.after(() => {
  rpg?.dispose();
});

test('loads the TDE ruleset and exposes its metadata', () => {
  rpg.loadRulesetFromFile(tdePath);
  const meta = rpg.meta();
  assert.equal(meta.id, 'tde5e_core');
  assert.ok(meta.attributes.some((a) => a.id === 'COU'));
  assert.ok(meta.resource_pools.some((p) => p.id === 'LP'));
  // The ruleset's own licensing metadata is exposed (the rules are NOT
  // Apache-2.0): licence, source, and a licence_source link. The ORC ruleset
  // also carries the verbatim ORC Notice and the attribution text.
  assert.ok(typeof meta.licence === 'string' && meta.licence.length > 0);
  assert.ok(typeof meta.source === 'string' && meta.source.length > 0);
  assert.ok(meta.licence_source.startsWith('https://'));
  assert.ok(typeof meta.licence_notice === 'string' && meta.licence_notice.length > 0);
  assert.ok(typeof meta.attribution === 'string' && meta.attribution.length > 0);
  const entries = rpg.entries();
  assert.ok(entries.some((e) => e.id === 'geron' && e.kind === 'archetypes'));
  assert.ok(entries.some((e) => e.id === 'gotongi' && e.kind === 'creatures'));
});

test('creates an entity from a named archetype (geron)', () => {
  const geron = rpg.createEntity('geron');
  assert.equal(geron.getStat('COU'), 12);
  assert.equal(geron.getStat('Attack'), 7);
  assert.equal(geron.getResource('LP'), 31); // 5 + 2*CON (CON 13)
});

test('creates an entity from an arbitrary sheet (no ruleset entry)', () => {
  const hero = rpg.createEntityFromSheet('my_hero', {
    COU: 14, AGI: 15, CON: 13, Attack: 12, Parry: 8, Armor_Rating: 3, Initiative: 12,
  });
  assert.equal(hero.getStat('Attack'), 12);
  assert.equal(hero.getStat('COU'), 14);
  assert.equal(hero.getResource('LP'), 31); // pools are derived from the sheet
  const sheet = hero.toJson();
  assert.equal(sheet.stats.Attack, 12);
});

test('resolves a check against a target', () => {
  const hero = rpg.createEntityFromSheet('my_hero', {
    COU: 14, AGI: 15, CON: 13, Attack: 12, Parry: 8, Armor_Rating: 3, Initiative: 12,
  });
  const toad = rpg.createEntity('toad');
  const result = rpg.check('tde_attack', hero, toad);
  assert.equal(typeof result.is_success, 'boolean');
  assert.ok(Array.isArray(result.raw_dice));
  // The opposed attack is *staged*: the attacker's die is always rolled, but
  // the defender's parry die is only rolled after a successful attack — a
  // missed attack returns after a single die. So at least one die is always
  // present, and a successful attack is exactly two (attack + parry).
  assert.ok(result.raw_dice.length >= 1);
  if (result.is_success) {
    assert.equal(result.raw_dice.length, 2);
  }
});

test('a fight is reproducible for a fixed seed', () => {
  const geronA = rpg.specFromId('geron');
  const toadA = rpg.specFromId('toad');
  const geronB = rpg.specFromId('geron');
  const toadB = rpg.specFromId('toad');
  const first = rpg.fight(geronA, toadA, { seed: 12345, maxRounds: 1000 });
  const second = rpg.fight(geronB, toadB, { seed: 12345, maxRounds: 1000 });
  assert.deepEqual(first, second);
});

test('fightDetail returns the full transcript and matches the plain fight', () => {
  rpg.loadRulesetFromFile(tdePath);
  const geron = rpg.specFromId('geron');
  const toad = rpg.specFromId('toad');
  const seed = 12345;
  const plain = rpg.fight(geron, toad, { seed, maxRounds: 1000 });
  const detail = rpg.fightDetail(geron, toad, { seed, maxRounds: 1000 });

  // The transcript is observation-only: the same seed must produce the same
  // fight (same winner, rounds, and hit-point pools).
  assert.equal(detail.winner_index, plain.winner_index);
  assert.equal(detail.rounds, plain.rounds);
  assert.deepEqual(detail.max_lp, plain.max_lp);
  assert.deepEqual(detail.remaining_lp, plain.remaining_lp);
  assert.equal(detail.a, plain.a);
  assert.equal(detail.b, plain.b);
  assert.ok(detail.hp_pool.length > 0);

  // The transcript mirrors the outcome.
  assert.equal(detail.log.winner_index, plain.winner_index);
  assert.equal(detail.log.rounds.length, plain.rounds);
  assert.deepEqual(detail.log.max_lp, plain.max_lp);
  assert.equal(detail.log.names[0], plain.a);
  assert.equal(detail.log.names[1], plain.b);

  // Every round records the initiative dice and the actions with their dice.
  for (const round of detail.log.rounds) {
    assert.ok(round.round >= 1);
    assert.equal(round.init_roll.length, 2);
    assert.equal(round.init_total.length, 2);
    assert.ok([0, 1].includes(round.goes_first));
    assert.ok(round.actions.length >= 1 && round.actions.length <= 2);
    for (const action of round.actions) {
      assert.ok(['attack', 'cast'].includes(action.kind));
      assert.ok(action.check_dice.length >= 1);
      assert.ok(action.damage >= 0);
      assert.ok(action.target_hp >= 0);
    }
  }

  // When the fight is decided (not a draw), the final action brings the
  // loser's hit points to <= 0.
  if (plain.winner_index !== -1) {
    const lastRound = detail.log.rounds[detail.log.rounds.length - 1];
    const finalAction = lastRound.actions[lastRound.actions.length - 1];
    assert.ok(finalAction.target_hp <= 0, 'loser reaches 0 hit points');
  }
});

test("fightDetail names the spells and lists each combatant's known spells", () => {
  rpg.loadRulesetFromFile(tdePath);
  const magus = rpg.specFromId('magister');
  const toad = rpg.specFromId('toad');
  const detail = rpg.fightDetail(magus, toad, { seed: 42, maxRounds: 100 });

  // The transcript header lists each combatant's known spells (human names)
  // and their starting spell resource.
  assert.deepEqual(detail.log.spells[0].sort(), ['Fulminictus', 'Ignifaxius', "Witch’s Claws"].sort());
  assert.deepEqual(detail.log.spells[1], []); // the toad knows no spells
  assert.equal(detail.log.resource_id, 'AE');
  assert.ok(detail.log.resource_pool[0] > 0); // the magister's Astral Energy
  assert.ok(detail.log.resource_pool[1] >= 0);

  // Every cast action carries the human-readable spell name (not just the id)
  // and shows the spell-resource pool running down.
  const casts = detail.log.rounds.flatMap((r) => r.actions).filter((a) => a.kind === 'cast');
  assert.ok(casts.length >= 1, 'the magister should cast at least once');
  for (const cast of casts) {
    assert.equal(cast.spell_name, 'Fulminictus'); // strongest damaging spell
    assert.ok(cast.spell.length > 0);
    assert.ok(cast.check_dice.length >= 1); // TDE rolls a casting check
    assert.ok(cast.resource.length > 0); // TDE spends AE
    assert.ok(cast.cost >= 4);
    assert.equal(cast.resource, 'AE');
    assert.ok(cast.resource_before > cast.resource_after); // the pool depletes
    assert.equal(cast.resource_before - cast.resource_after, cast.cost);
  }
});

test('a D&D bestiary spellcaster casts its spells (dragons, liches, ...)', () => {
  rpg.loadRulesetFromFile(dndPath);
  const dragon = rpg.specFromId('ancient_gold_dragon');
  const rat = rpg.specFromId('giant_rat');
  const detail = rpg.fightDetail(dragon, rat, { seed: 1, maxRounds: 20 });

  // The dragon's innate spellcasting is carried onto its combatant spec and
  // shown in the transcript header; the rat is pure melee. D&D has no spell
  // resource, so no pool is reported.
  assert.ok(detail.log.spells[0].length >= 3);
  assert.ok(detail.log.spells[0].includes('Flame Strike'));
  assert.deepEqual(detail.log.spells[1], []);
  assert.equal(detail.log.resource_id, '');
  assert.deepEqual(detail.log.resource_pool, [0, 0]);

  // The dragon prefers casting over its weapon, and the cast carries the
  // spell name plus the rolled damage dice (D&D spells have no cast check and
  // no spell-resource cost to report).
  const first = detail.log.rounds[0].actions[0];
  assert.equal(first.kind, 'cast');
  assert.equal(first.spell_name, 'Flame Strike');
  assert.ok(first.damage_dice.length >= 1);
  assert.equal(first.check_dice.length, 0); // no casting check in D&D
  assert.equal(first.resource, '');
  assert.equal(first.resource_before, 0);
  assert.equal(first.resource_after, 0);
});

test('a hand-built character beats a toad (arbitrary sheet fights)', () => {
  rpg.loadRulesetFromFile(tdePath);
  const hero = rpg.createEntityFromSheet('my_hero', {
    COU: 14, AGI: 15, CON: 13, Attack: 12, Parry: 8, Armor_Rating: 3, Initiative: 12,
  });
  const heroSpec = rpg.specFromEntity(hero);
  const toadSpec = rpg.specFromId('toad');
  let heroWins = 0;
  for (let i = 0; i < 20; i += 1) {
    const outcome = rpg.fight(heroSpec, toadSpec, { seed: i, maxRounds: 100 });
    if (outcome.winner_index === 0) heroWins += 1;
  }
  assert.ok(heroWins >= 19, `hero should beat the toad ~always, won ${heroWins}/20`);
});

test('parity: seeded WASM fight matches the native rpg_os_example_fight binary', { skip: !existsSync(nativeBinary) }, () => {
  rpg.loadRulesetFromFile(tdePath);
  const seed = 12345;
  const nativeOut = execFileSync(nativeBinary, [
    '--root', repoRoot, '--ruleset', tdePath, '--csv', '--batch', '1', '--rounds', '1000',
    '--seed', String(seed), 'geron', 'toad',
  ], { encoding: 'utf8' });
  const fields = nativeOut.trim().split('\t');
  assert.equal(fields.length, 5, `unexpected native CSV: ${nativeOut.trim()}`);
  const [winnerId, , winnerLp, , rounds] = fields;

  const geron = rpg.specFromId('geron');
  const toad = rpg.specFromId('toad');
  const outcome = rpg.fight(geron, toad, { seed, maxRounds: 1000 });

  const expectedWinner = winnerId === 'DRAW' ? -1 : winnerId === 'geron' ? 0 : 1;
  assert.equal(outcome.winner_index, expectedWinner, 'winner index matches native');
  assert.equal(outcome.rounds, Number(rounds), 'rounds match native');
  const expectedWinnerLp = expectedWinner === 0 ? outcome.remaining_lp[0] : outcome.remaining_lp[1];
  assert.equal(expectedWinnerLp, Number(winnerLp), 'winner LP matches native');
});
