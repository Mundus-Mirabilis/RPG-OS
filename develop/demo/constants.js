// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * Shared constants for the ELO Arena demo (app.js and its helper modules).
 *
 * DEFAULT_WEAPON matches the engine's fallback (wasm/bindings.cpp and
 * examples/fight/main.cpp use the same '1d6+4'). The seed LCG mirrors
 * scripts/elo_ranking.py: SEED_MOD wraps the per-run base seed to uint32, and
 * SEED_MUL / SEED_STRIDE / SEED_ADD mix in the fight index so fights within a
 * run stay distinct.
 */
export const DEFAULT_WEAPON = '1d6+4';
export const LIVE_FIGHTS = 100; // Monte-Carlo fights per "Confirm with N live fights" run
export const MAX_ROUNDS = 1000; // guards against draws in a fight

export const SEED_MOD = 0x100000000;
export const SEED_MUL = 1000003;
export const SEED_STRIDE = 31;
export const SEED_ADD = 13;

export const RULESETS = {
  tde5e_core: { label: 'The Dark Eye 5e', file: 'tde5e_core.json' },
  dnd5e_srd: { label: 'D&D 5e SRD', file: 'dnd5e_srd.json' },
};

export const COMBAT_STATS = ['Attack', 'Parry', 'Armor_Rating', 'AC', 'Initiative'];
// The newcomer is ranked Swiss-style against the leaderboard entries closest
// to its current rating (see rankNewcomer in elo.js). A focus on similar
// ratings plus more fights per opponent makes the estimate converge faster.
export const RANK_ROUNDS = 8; // how many similar-rating rounds it plays
export const RANK_OPPONENTS_PER_ROUND = 2; // opponents picked per round (closest rating)
export const GAMES_PER_OPPONENT = 30; // fights per opponent
