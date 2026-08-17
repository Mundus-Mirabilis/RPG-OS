// Copyright (c) 2026 Christian Mayer and the Mundus Mirabilis contributors.
// SPDX-License-Identifier: Apache-2.0

/**
 * The ELO rating math for the demo. The canonical implementation lives in
 * Python (scripts/elo_ranking.py); this is a small JS port used only for the
 * interactive, serverless parts of the page — ranking a character entered
 * into the form (the fights run in the WASM engine) and computing head-to-head
 * win probabilities from stored ratings. The formula is identical to the
 * Python version: expected = 1/(1+10^((b-a)/400)), rating += K*(score - expected).
 */

/** Expected score of `a` against `b` (0..1) — the ELO win probability. */
export function expectedScore(a, b) {
  return 1 / (1 + 10 ** ((b - a) / 400));
}

/** One ELO update after `score` (0, 0.5, 1) against a rated opponent. */
export function updateRating(rating, expected, score, k = 32) {
  return rating + k * (score - expected);
}

/** Win probability of combatant `a` over `b` from their ELO ratings. */
export function winProbability(a, b) {
  return expectedScore(a, b);
}

/**
 * Ranks a newcomer (a live combatant spec) against the top `opponents` of the
 * leaderboard using the standard ELO procedure: play `gamesPerOpponent`
 * Monte-Carlo fights against each opponent (in the WASM engine) and update the
 * rating with the K-factor — the same procedure scripts/elo_ranking.py uses.
 *
 * Returns `{ rating, wins, losses, draws, games }`.
 */
export async function rankNewcomer(rpg, spec, opponents, options = {}) {
  const gamesPerOpponent = options.gamesPerOpponent ?? 20;
  const k = options.k ?? 32;
  const initial = options.initial ?? 1500;
  const maxRounds = options.maxRounds ?? 1000;
  let rating = initial;
  let wins = 0;
  let losses = 0;
  let draws = 0;
  for (let o = 0; o < opponents.length; o += 1) {
    const opp = opponents[o];
    const oppSpec = rpg.specFromId(opp.id);
    let w = 0;
    let l = 0;
    let d = 0;
    for (let i = 0; i < gamesPerOpponent; i += 1) {
      const seed = (o * 1000003 + i * 31 + 7) >>> 0;
      const outcome = rpg.fight(spec, oppSpec, { seed, maxRounds });
      if (outcome.winner_index === 0) w += 1;
      else if (outcome.winner_index === 1) l += 1;
      else d += 1;
    }
    oppSpec.dispose();
    const score = (w + 0.5 * d) / gamesPerOpponent;
    rating = updateRating(rating, expectedScore(rating, opp.rating), score, k);
    wins += w;
    losses += l;
    draws += d;
  }
  return { rating, wins, losses, draws, games: wins + losses + draws };
}
