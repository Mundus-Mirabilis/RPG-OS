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

/** The ELO formula's divisor (spread) and K-factor, and the standard starting
 * strength. These mirror scripts/elo_ranking.py (--k 32, --initial 1000). */
export const ELO_DIVISOR = 400;
export const ELO_K = 32;
export const ELO_STANDARD = 1000;

/** Expected score of `a` against `b` (0..1) — the ELO win probability. */
export function expectedScore(a, b) {
  return 1 / (1 + 10 ** ((b - a) / ELO_DIVISOR));
}

/** One ELO update after `score` (0, 0.5, 1) against a rated opponent. */
export function updateRating(rating, expected, score, k = ELO_K) {
  return rating + k * (score - expected);
}

/**
 * Ranks a newcomer (a live combatant spec) against the leaderboard using the
 * standard ELO procedure, Swiss-style: over `rounds` rounds the newcomer plays
 * the `opponentsPerRound` leaderboard entries whose rating is closest to its
 * *current* rating ("play someone of your level"), and after
 * `gamesPerOpponent` Monte-Carlo fights per opponent (in the WASM engine) the
 * rating is updated with the K-factor — the same procedure
 * scripts/elo_ranking.py uses. Matching similar ratings makes the estimate
 * converge quickly, and a genuinely average character converges to the
 * standard starting strength (`initial`, the ELO standard 1000).
 *
 * Returns `{ rating, wins, losses, draws, games, opponents }`.
 */
export async function rankNewcomer(rpg, spec, opponents, options = {}) {
  const gamesPerOpponent = options.gamesPerOpponent ?? 20;
  const opponentsPerRound = options.opponentsPerRound ?? 2;
  const rounds = options.rounds ?? 8;
  const k = options.k ?? ELO_K;
  const initial = options.initial ?? ELO_STANDARD; // the ELO standard strength
  const maxRounds = options.maxRounds ?? 1000;
  let rating = initial;
  let wins = 0;
  let losses = 0;
  let draws = 0;
  let opponentsPlayed = 0;
  const played = new Set();

  for (let round = 0; round < rounds; round += 1) {
    // The leaderboard entries closest to the newcomer's current rating that it
    // has not yet met this ranking — the "similar strength" focus.
    const byCloseness = [...opponents]
      .filter((opp) => !played.has(opp.id))
      .sort((a, b) => Math.abs(a.rating - rating) - Math.abs(b.rating - rating));
    const picks = byCloseness.slice(0, opponentsPerRound);
    if (picks.length === 0) break;
    for (const opp of picks) {
      played.add(opp.id);
      const oppSpec = rpg.specFromId(opp.id);
      let w = 0;
      let l = 0;
      let d = 0;
      for (let i = 0; i < gamesPerOpponent; i += 1) {
        // The newcomer seed space: a (round, opponentsPlayed, index) LCG that is
        // deliberately distinct from the live-fight mixing in app.js and the
        // Python tournament seed in scripts/elo_ranking.py — each ranking path
        // draws from its own space, so the same numeric seed never collides
        // across the three.
        const seed = (round * 1000003 + opponentsPlayed * 31 + i * 7 + 1) >>> 0;
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
      opponentsPlayed += 1;
    }
  }
  return {
    rating, wins, losses, draws,
    games: wins + losses + draws, opponents: opponentsPlayed,
  };
}
