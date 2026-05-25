# Transfer System Design — Phase 1 + 2

> **Scope:** Phase 1 (data foundation: player personality, club identity, scouting knowledge) and Phase 2 (AI market engine: squad needs, negotiations, cascade, deadline chaos). User-facing negotiation UI, special events (galactico, financial collapse), and transfer window inflation are Phase 3+4.

---

## Philosophy

Transfers are driven by pressure, ambition, finance, reputation, ego, timing, and irrationality — not by pure squad optimisation. The engine must produce believable chaos: panic buys, vanity signings, clubs behaving differently from each other, deals that collapse at the last second, and cascade effects that ripple across the market. Perfect logic is forbidden. Controlled irrationality is a first-class requirement.

**Anti-optimisation rule:** The AI must never become mathematically efficient. If the AI always buys the best-value player for each need, the market becomes dead and predictable. Mistakes, ego, panic, reputation bias, irrationality, and bad timing are not edge cases — they are core outputs the system must protect at every stage of implementation.

---

## Transfer Windows

- **Summer:** July 1 – August 31
- **Winter:** January 1 – January 31
- Outside windows: free-agent signings only (rare, flagged as opportunistic)
- Deadline = last calendar day of each window

---

## Architecture Overview

```
player_traits              — hidden per-player personality (seeded at career start)
club_transfer_identity     — hidden per-club DNA + negotiation personality (seeded at career start)
club_player_knowledge      — which clubs know which players (scouting visibility)
club_player_relationship   — relationship memory between clubs and players
player_market_status       — hidden flags: untouchable, wonderkid, franchise_player, etc.
transfer_negotiations      — state machine rows for every deal in flight
negotiation_cooldowns      — blocks same buyer/player pair after collapse
market_scarcity            — per-position global scarcity tracking (price inflation)
transfer_news              — minimal headline log for the news page
player_unhappiness         — tracks broken promises, benching, blocked moves
```

All tables are per-`manager_id` except `market_scarcity` which is global per career. Daily processor `ProcessDailyTransfers()` drives everything.

---

## Section 1 — Player Personality Traits

Table: `player_traits`
Columns: `manager_id`, `player_id`, `ambition`, `loyalty`, `greed`, `ego`, `trophy_hunger`, `adaptability`, `professionalism` — all INTEGER 0–100.

Created at career start. One row per player per career. Seeded from real player attributes + a small per-career XOR seed so the same player is consistent across saves but not pixel-perfect identical.

### Seeding Rules (all clamped 0–100)

| Trait | Primary seeds | Direction |
|---|---|---|
| `ambition` | `sofifaPotential - base_stat` gap, `age < 24` | High potential gap + young = high ambition |
| `loyalty` | `age`, long `contract_expiry` window, low `international_reputation` | Older + long contract + low rep = loyal |
| `greed` | `weekly_wage`, `playervalue`, `international_reputation` | Expensive + famous = greedy |
| `ego` | `international_reputation` (1–5 scale × 20), `playervalue` relative to team avg | High rep + premium value = high ego |
| `trophy_hunger` | `age > 27`, `reputation`, `international_reputation` | Experienced elite player = trophy driven |
| `adaptability` | `age < 26`, `nationality` (non-domestic), `alternative_pos` count | Young + foreign + versatile = adaptable |
| `professionalism` | `base_stat` consistency (high stat + not outlandish potential), `age > 25` | Settled career + mature = professional |

Formula pattern (example for ambition):
```cpp
int potential_gap = std::max(0, p.sofifaPotential - (int)p.base_stat);
float base = (float)potential_gap / 40.0f;       // 0–1
float age_mod = (p.age < 21) ? 0.25f : (p.age < 24) ? 0.10f : 0.0f;
float seed_var = (float)((career_seed ^ p.id * 7919u) % 20 - 10) / 100.0f;
ambition = (int)std::clamp((base + age_mod + seed_var) * 100.0f, 0.0f, 100.0f);
```

### Behavioral Effects (consumed by negotiation engine)

- **High ambition (> 70):** Player can force transfer request if benched 8+ weeks. Reduced loyalty drag on acceptance score.
- **High loyalty (> 70):** Acceptance score penalty when leaving current club. Resists even elite offers.
- **High greed (> 70):** Wage demand multiplier 1.2–1.5×. Agent pressure amplified.
- **High ego (> 65):** Rejects `rotation` or `prospect` promised role. Demands guaranteed starts.
- **High trophy_hunger (> 65):** Prestige weight doubled in acceptance score.
- **Low adaptability (< 30):** League/country penalty in acceptance score. Resists cross-league moves.
- **High professionalism (> 70):** Ignores agent pressure. Negotiates calmly. Slow unhappiness accumulation.

---

## Section 2 — Club Transfer Identity (Transfer DNA)

Table: `club_transfer_identity`
Columns: `manager_id`, `club_id`, plus the traits below — all INTEGER 0–100 unless noted.

Created at career start. Seeded from existing `style_seed`, `institutional_power`, `commercial_strength`, `board_confidence`, `transfer_budget`, `debt_level`.

### Identity Traits

| Trait | Seeds from | Behavioral effect |
|---|---|---|
| `aggression` | high `institutional_power` + high `commercial_strength` | Pursues multiple targets; short patience in negotiations |
| `wage_willingness` | high `transfer_budget` + gambling archetype (`style_seed % 6 == 2`) | Overpays wages; inflates market |
| `age_preference` | `style_seed` variance, −50 to +50 (signed) | Negative = youth (< 23), Positive = experience (> 28) |
| `deadline_panic` | low `board_confidence` + low `institutional_power` | Irrational spend in final days; collapse risk rises |
| `loyalty_to_players` | high `institutional_power` + conservative seed | Resists selling stars; rejects lowball bids |
| `financial_risk_tolerance` | low `debt_level` + high `board_confidence` | Accepts debt to close a deal |
| `selling_pressure` | high `debt_level` + low `cash_balance` | Lists own players; accepts lower fees |
| `youth_focus` | `style_seed` band, low `institutional_power` | Prefers targets age ≤ 22; promotes from depth |
| `domestic_bias` | `style_seed` band, league `popularity` | Prefers players from same country/league |
| `resale_focus` | `style_seed` band, high `commercial_strength` | Targets high-potential young players; sells at peak |
| `prestige_bias` | high `institutional_power` | Prefers famous names over pure quality |
| `irrationality` | `style_seed % 6` extreme values + `deadline_panic` | Controls vanity/panic/nostalgia move probability |

### Negotiation Personality

Each club has a hidden negotiation style derived from identity traits. This is a second layer that shapes how they behave specifically during talks — not just whether to pursue a player, but how they negotiate once a deal is in motion.

Five personalities (one assigned per club, never exposed directly):

| Personality | Derived from | Behavior |
|---|---|---|
| `hard_negotiator` | high `loyalty_to_players` + low `selling_pressure` | Starts high, moves slowly, rarely panics |
| `fast_closer` | high `aggression` + high `wage_willingness` | Overpays quickly to end negotiations; hates stalling |
| `media_manipulator` | high `irrationality` + high `prestige_bias` | Leaks interest publicly; uses competing bids as leverage even when none exist |
| `patient` | high `financial_risk_tolerance` + low `deadline_panic` | Waits out stalls; never panic buys; walks away cleanly |
| `desperate` | high `selling_pressure` + low `board_confidence` | Accepts below-value; rushes through counter-offers; deadline panic is severe |

Negotiation personality modifies: counter-offer increment size, stall tolerance, `competing_bid` fabrication chance (media_manipulator only), and collapse/accept thresholds.

### Club Archetypes (hidden, never shown directly)

Derived from trait combinations at career start. Used only for seeding personality narrative flavor:

- `conservative` — low aggression, low irrationality, high loyalty_to_players
- `aggressive` — high aggression, high wage_willingness, low loyalty_to_players
- `gambling` — high financial_risk_tolerance, high irrationality, high deadline_panic
- `youth_factory` — high youth_focus, high resale_focus, low prestige_bias
- `selling_club` — high selling_pressure, low loyalty_to_players, high resale_focus
- `prestige_driven` — high prestige_bias, high wage_willingness, low resale_focus
- `opportunistic` — high irrationality, mid aggression
- `rebuild` — triggered when board_confidence < 35: selling_pressure spikes, youth_focus rises

### Controlled Irrationality

Each time a club considers initiating a transfer, an `irrationality_roll` fires:

```
irrationality_roll = rng() % 100 < club.irrationality
```

If triggered, the club may:
- **Vanity signing:** target a famous player regardless of squad need (prestige_bias > 60)
- **Panic buy:** sign any available player at the needed position regardless of quality or fee (deadline_panic > 60)
- **Duplicate depth signing:** buy a 3rd player at a position already covered (aggression > 70)
- **Nostalgia move:** re-sign a former player now at a different club (relationship memory: `former_player` flag)
- **Reputation trap:** overpay for a declining veteran still carrying a famous name (prestige_bias > 70, player age > 30)

Irrationality rolls use the daily RNG seed so each save produces different irrational decisions.

---

## Section 3 — Scouting Visibility (Knowledge Limits)

Table: `club_player_knowledge`
Columns: `manager_id`, `club_id`, `player_id`, `knowledge` (INTEGER 0–100).

Clubs do not instantly know every player. They can only target players whose `knowledge` score is above a threshold.

### Knowledge Seeding (career start)

| Condition | Knowledge added |
|---|---|
| Player in same league as club | +60 |
| Player in same country as club | +25 |
| `international_reputation` >= 4 | +40 (globally known) |
| `international_reputation` == 5 | +70 (global superstar) |
| Same confederation | +10 |
| Base noise (all players) | rng() % 15 |

Clamped 0–100. Players with knowledge < 30 are invisible to that club — never targeted.

Knowledge evolves slowly each season (handled in ProcessAnnualCycle for Phase 3+). For Phase 1+2, seeded values are static.

### Effect on Targeting

When a club selects a transfer target: filter candidate pool to `knowledge >= 30`. Among visible players, weight selection by knowledge score — a 90-knowledge player is much more likely to be targeted than a 31-knowledge player.

---

## Section 4 — Relationship Memory

Table: `club_player_relationship`
Columns: `manager_id`, `club_id`, `player_id`, `relationship_type TEXT`, `created_date TEXT`.

Relationship types:

| Type | Set when | Effect |
|---|---|---|
| `former_player` | Transfer completes away from club | Nostalgia move trigger; acceptance bonus if returning |
| `rejected_offer` | Offer collapsed or player rejected terms | Asking price +15% on next approach; player acceptance −10 |
| `fan_favourite` | Player at club > 3 seasons + high reputation | `loyalty_to_players` effectively +20 for this player; selling generates news |
| `transfer_listed_by_us` | Club sets player as surplus | Acceptance score for leaving +20; agent pressure +15 |
| `unhappy_at_club` | `player_unhappiness.severity > 60` | Available at reduced price; acceptance for any move +15 |
| `blocked_move` | 3+ collapses on same player | Unhappiness +25; player pushes for exit regardless of loyalty |
| `failed_medical` | Medical pending → collapsed | Buying club has 30-day cooldown on this player; news item generated |

Multiple relationship types can exist for the same club/player pair.

---

## Section 5 — Player Market Status

Table: `player_market_status`
Columns: `manager_id`, `player_id`, `status TEXT`, `set_date TEXT`.

One row per player per career. Status is a hidden flag that overrides normal market behavior.

| Status | Set when | Effect |
|---|---|---|
| `transfer_listed` | Club marks player surplus (`is_transfer_listed = 1` or `surplus_to_requirements`) | Visible signal; accepting clubs get reduced asking price |
| `untouchable` | High `loyalty_to_players` + `fan_favourite` relationship | Club refuses all bids; selling_pressure must be > 70 to override |
| `surplus_to_requirements` | Squad size > 28 + player in bottom third of squad rating | Transfer aggression reduced; club accepts lower fees |
| `wonderkid` | Age ≤ 21 + `sofifaPotential >= 85` + `international_reputation >= 3` | Globally known regardless of knowledge; multiple clubs monitor; price inflated |
| `franchise_player` | Captain equivalent: team's highest-rated player at club > 2 seasons | `untouchable` behavior; sale triggers board confidence hit + fan backlash news |
| `expiring_soon` | `contract_expiry` within 6 months | Selling club's asking price drops 40%; player's acceptance of any move +20 |

Status is re-evaluated every 30 days by `ProcessDailyTransfers`.

---

## Section 6 — Squad Need Engine

Runs at: career load, window open, and after every completed or collapsed transfer (cascade trigger).

### Position Groups (7 groups — fine-grained to prevent unrealistic position saturation)

| Group | Roles included |
|---|---|
| `GK` | GK |
| `CB` | CB |
| `FB_WB` | LB, RB, LWB, RWB |
| `DM` | CDM |
| `CM` | CM |
| `AM_W` | CAM, LM, RM, LW, RW |
| `ST` | CF, ST |

This prevents clubs from buying a 6th striker because LW and ST were merged. Each group is evaluated independently.

### Need Score Formula

For each group per club:

```
need = quality_gap     * 30   // group avg base_stat below league avg for that group
     + depth_gap       * 25   // fewer than 2 players in group
     + age_problem     * 20   // group avg age > 30
     + expiry_pressure * 15   // 2+ players with contract_expiry within 6 months
     + identity_fit    * 10   // age_preference mismatch with group avg age
```

Clamped 0–100.

### Squad Size Pressure

Ideal squad size: 24–28 players. Checked after every transfer completion.

- **Squad > 28:** Transfer aggression reduced by 30%; `selling_pressure` increased by 20%; bottom-rated rotation players automatically flagged `surplus_to_requirements`.
- **Squad > 32:** No new signings initiated regardless of need score or irrationality roll. Club actively lists players.
- **Squad < 18:** Emergency override — need scores doubled, irrationality suppressed (pure survival mode).

### Target Prioritisation Tiers

When a club decides to act on a need, targets are ranked into three tiers before selection:

**Tier 1 — Dream targets:** knowledge ≥ 70, quality significantly above current group average, matches club identity (prestige_bias, age_preference). Club will offer above contextual value. Patience is long.

**Tier 2 — Realistic targets:** knowledge ≥ 40, quality at or above group average, affordable (within 60% of transfer_budget). Standard negotiation behavior.

**Tier 3 — Panic alternatives:** Any visible player (knowledge ≥ 30) at the needed position. Triggered when Tier 1 and Tier 2 deals collapse and deadline pressure > 60. Clubs may seriously overpay or accept poor quality.

Clubs start at Tier 1. Failed deals cascade down: Tier 1 collapse → try Tier 2. Tier 2 collapse near deadline → Tier 3. This creates realistic desperation without random jumps.

### Action Thresholds

| Score | Behaviour |
|---|---|
| > 65 | Active search — initiate negotiation this window |
| 40–65 | Monitoring — joins competing bid if opportunity appears |
| < 40 | Ignore — unless irrationality roll fires |

### Imperfection Rules

- Clubs with `irrationality > 70` may ignore need scores entirely (irrational buy regardless of squad state)
- Clubs with `financial_risk_tolerance < 20` won't act above score 50 if cash is tight
- Irrationality roll can override threshold entirely (vanity/panic buys)
- A club already running 2+ negotiations won't start a third unless `aggression > 75`

---

## Section 7 — Improved Valuation

Player transfer value is dynamic. Calculated per negotiation context.

```
contextual_value =
  base_playervalue
  × contract_factor       // 1.3 if 3+ years left, 0.7 if < 1 year, 1.0 otherwise
  × scarcity_factor       // 1.0–1.5: from market_scarcity table (global position scarcity)
  × seller_factor         // 0.7–1.0: selling_pressure reduces asking price
  × buyer_urgency         // 1.0–1.3: need_score > 70 = buyer overpays
  × deadline_factor       // 1.0–1.5: climbs in final 5 days of window
  × hype_factor           // 1.0–1.3: agent_pressure inflates price
  × status_factor         // wonderkid = 1.4, expiring_soon = 0.6, untouchable = 2.0 (rarely sells)
```

Selling club's ask = `contextual_value × (1.0 + loyalty_to_players * 0.003)`.

Buying club's offer starts at 80% of contextual_value, steps up in negotiation.

Rejected offer relationship adds 15% to contextual_value on next approach from same club.

---

## Section 8 — Market Scarcity Memory

Table: `market_scarcity`
Columns: `manager_id`, `position_group TEXT`, `scarcity_score INTEGER` (0–100), `last_updated TEXT`.

Tracks global availability of quality players per position group across all clubs.

### Scarcity Calculation (re-run at window open)

For each position group:
```
available_supply = count of players in group with base_stat > league_avg
                   AND (transfer_listed OR expiring_soon OR surplus_to_requirements)
scarcity_score = max(0, 100 - (available_supply * 8))
```

Scarcity score feeds directly into `scarcity_factor` in contextual valuation:
- scarcity 0–30: factor 1.0 (normal market)
- scarcity 31–60: factor 1.15
- scarcity 61–80: factor 1.30
- scarcity > 80: factor 1.50 (elite players in this position command enormous fees)

This creates organic "crazy transfer years" — if many top strikers sign long contracts or move to the same club, ST prices spike league-wide. Clubs buying in that window pay a premium.

Scarcity persists across windows within a season and decays slowly into the next (youth matures, market adjusts).

---

## Section 9 — Agent Pressure

Hidden per-negotiation factor. Not a full agent system — simulates agent behavior as a modifier.

Column `agent_pressure` (0–100) on `transfer_negotiations`. Seeded from:
- Player's `greed` trait (primary driver)
- Player's `international_reputation`
- Whether multiple clubs are pursuing same player (`competing_bid` state)

### Effects

| Pressure level | Effect |
|---|---|
| > 70 | Wage demand inflated 1.2–1.4×; media leak triggers (`agent_leak` news item) |
| 50–70 | Asking price rises 10–20% mid-negotiation; deal volatility increases |
| < 30 | Clean negotiation; player accepts reasonable offer quickly |

Agent pressure increases by +5 per day the deal stays in `negotiating` or `stalled` states.

`media_manipulator` clubs can fabricate competing bid pressure (raise agent_pressure +15 without an actual competing deal existing) to force faster decisions.

---

## Section 10 — Financial Protection

Hard limits that prevent AI clubs from imploding permanently due to irrationality or deadline chaos.

### Per-Club Limits (enforced before any transfer commitment)

| Guard | Rule |
|---|---|
| **Operating reserve** | Club cannot spend below `min_reserve = weekly_wages * 8`. If a transfer would breach this, it is blocked even for gambling archetypes. |
| **Max wage/revenue ratio** | `total_weekly_wages / weekly_revenue` must not exceed 0.85 after any signing. Revenue = weeklyTV + weeklySponsors. |
| **Transfer budget floor** | Transfer budget cannot go negative. Clubs can offer only what they have. |
| **Debt panic threshold** | If `debt_level > transfer_budget * 2`: selling_pressure set to 80, no new signings initiated (survival mode), board_confidence −10. |
| **Wage overrun block** | If current squad wages already exceed `wage_budget`: no new wage commitments unless `financial_risk_tolerance > 75`. |

These guards are checked inside the daily processor before any `initiated` → `offer_made` transition. Irrational clubs can still behave emotionally — they just cannot physically bankrupt themselves into a non-recoverable state.

---

## Section 11 — Negotiation State Machine

Table: `transfer_negotiations`

Key columns:
```
manager_id, buying_club_id, selling_club_id, player_id
state TEXT                          -- current state
offered_fee INTEGER                 -- current bid
offered_wage INTEGER                -- weekly wage offered
promised_role TEXT                  -- star_player|important|rotation|prospect
days_in_state INTEGER               -- how long in current state
initiated_date TEXT                 -- YYYY-MM-DD
deadline_pressure INTEGER           -- 0–100, rises as window end approaches
agent_pressure INTEGER              -- 0–100
collapse_reason TEXT                -- populated on collapse
competing_bid_club_id INTEGER       -- club competing for same player (if any)
acceptance_score INTEGER            -- last calculated player acceptance (0–100)
irrationality_driven INTEGER        -- 1 if this deal was triggered by irrationality roll
tier INTEGER                        -- 1, 2, or 3 (target priority tier)
counter_offer_count INTEGER         -- how many times buyer has raised bid
```

### States

```
initiated
  ↓ (1–3 days)
offer_made
  ↓ (1–4 days) selling club accepts/rejects
negotiating
  ↓↗ (loops — deals sit, revive, restart)
counter_offer      ← selling club wants more / buyer raises
stalled            ← neither side moves (patience expired)
competing_bid      ← another club enters for same player
player_waiting     ← player taking time to decide (high loyalty, weighing options)
  ↓
medical_pending    ← deal agreed, 1-day medical check
  ↓
completed          ← player moves; cascade triggers
  OR
collapsed          ← at any state after offer_made
```

### State Transition Logic

**initiated → offer_made:**
Days = `3 - (club.aggression / 50)` clamped 1–3. Fast_closer personality: always 1 day.

**offer_made → negotiating or collapsed:**
Selling club evaluates `loyalty_to_players`, `selling_pressure`, contextual value vs offered fee, and player market status (`untouchable` = auto-reject unless selling_pressure > 70). If fee < 60% of contextual_value and selling_pressure < 40: collapsed. Otherwise: negotiating.

**negotiating (daily tick):**
Calculate `acceptance_score` (see Section 12). If score > 65: → `player_waiting` (high loyalty player) or `medical_pending` (everyone else). If score 40–65: → `counter_offer`. If score < 40: → `collapsed`. 15% daily roll for `competing_bid` if player knowledge ≥ 60 at another club with matching need.

**counter_offer:**
Buyer raises fee 5–15% and wage 5–10% (modulated by `wage_willingness`). `fast_closer` raises 20% immediately. Returns to `negotiating`. After `counter_offer_count > 3` and score still < 50: → `collapsed` unless deadline_pressure > 80 (desperation override).

**stalled:**
Triggered when `days_in_state > 5` in `negotiating` with no score movement. Deal sits. 30% daily revival chance (60% during deadline week). `patient` clubs wait; `desperate` clubs collapse after 5 stalled days. Agent pressure +10 per stalled day.

**competing_bid:**
Another club enters. `agent_pressure` +20. Original buyer must increase offer (aggression-weighted) or withdraw. `media_manipulator` clubs can fabricate this state to pressure deals. Can loop back to `negotiating`.

**player_waiting:**
Player with loyalty > 65 takes 2–4 days before deciding. During this window: competing_bid chance rises to 25% daily. If another offer arrives: → `competing_bid`. Otherwise resolves to `medical_pending` or `collapsed`.

**medical_pending:**
1 day. Pass rate: **normal = 98%, deadline day (final 3 days) = 95%**. Collapse on failure; collapse_reason = `failed_medical`.

**collapsed:**
Sets `collapse_reason`. Inserts cooldown row. Triggers cascade. Updates player_unhappiness if relevant. Updates club_player_relationship (`rejected_offer` or `blocked_move`).

### Deadline Day Special Rules

When `deadline_pressure > 80` (final ~4 days of window):
- Clubs with `deadline_panic > 50`: effective aggression +30, wage_willingness +25, stall tolerance −40
- `stalled` deals revival chance rises to 60%
- `counter_offer` loops reduced by 1 (less patience)
- Medical collapse chance: 5% (up from 2%)
- Irrationality roll fires daily for panicking clubs regardless of need score
- Tier 3 targeting unlocked for clubs whose Tier 1 and 2 deals collapsed

---

## Section 12 — Player Acceptance Score

Calculated during `negotiating` state. Score 0–100. Above 65 = moves to medical/waiting. 40–65 = counter. Below 40 = rejection.

```
acceptance_score =
  wage_fit          * 0.20   // offered_wage vs expected_wage
  + role_fit        * 0.18   // promised_role vs ego threshold
  + prestige_fit    * 0.18   // buying club institutional_power vs player trophy_hunger
  + league_fit      * 0.10   // buying club league reputation vs player adaptability
  + playtime_fit    * 0.14   // squad depth at player position
  + ambition_pull   * 0.10   // ambition × prestige delta from current club
  − loyalty_drag    * 0.10   // loyalty trait cost to leave
```

### Component Formulas

**wage_fit:** `offered_wage / expected_wage` clamped 0–1, where `expected_wage = weekly_wage × (1.0 + greed/200.0f)`.

**role_fit:**
- `star_player` promised: +100 if ego > 50, +60 otherwise
- `important` promised: +70 always
- `rotation` promised: +40 if ego < 40, −10 if ego > 65
- `prospect` promised: +50 if age < 21 and ambition > 60, −20 if ego > 50

**prestige_fit:** `(buying_club.institutional_power / 100.0f) × (0.5 + trophy_hunger/200.0f) × 100`.

**league_fit:** `league_reputation_score × (adaptability / 100.0f)`. Cross-continental move penalised unless adaptability > 60.

**playtime_fit:** Inverse of how crowded the player's position group is at buying club. Fewer incumbents = higher score. Ego amplifies this.

**ambition_pull:** `(ambition / 100.0f) × max(0, buying_club.institutional_power − selling_club.institutional_power)`.

**loyalty_drag:** `(loyalty / 100.0f) × 100` — subtracted raw.

**Relationship modifiers (applied after base score):**
- `former_player` relationship: +8
- `rejected_offer` previously: −10
- `unhappy_at_club` status: +15
- `fan_favourite` at selling club: loyalty_drag ×1.3

**Agent pressure modifier:** `acceptance_score += (agent_pressure − 50) × 0.10f`.

---

## Section 13 — Squad Role Promises

`promised_role` column on `transfer_negotiations`: `star_player | important | rotation | prospect`.

Role set by buying club:
- High need_score → generous promise
- High ego player → must promise `star_player` or `important` or player auto-rejects
- Current squad depth at position → fewer incumbents = more generous promise

Promise stored on completed deal. Phase 3 will track fulfillment. Phase 1+2 seeds the promise and evaluates unhappiness for violations detected during AI processing (e.g., club buys another player at same position within 30 days of a `star_player` promise).

---

## Section 14 — Negotiation Cooldowns

Table: `negotiation_cooldowns`
Columns: `manager_id`, `buying_club_id`, `player_id`, `cooldown_until TEXT` (YYYY-MM-DD), `reason TEXT`.

After a deal collapses:
- Same buyer cannot initiate on same player for **7–14 days** (randomised: 7 + rng()%8).
- `rejected_offer` relationship is written simultaneously.
- Selling club's `loyalty_to_players` effectively +15 for this buyer for the rest of the window.

Cooldown also applied after `failed_medical`: 30-day block from same buyer.

During the daily processor, target selection skips any player with an active cooldown for that buyer.

---

## Section 15 — Contract Renewal AI

Without AI contract renewals, the market becomes distorted after 2–3 seasons as all contracts expire simultaneously.

`ProcessContractRenewals(managerId, currentDate)` runs monthly (1st of each month), outside transfer windows.

### Rules

**Key player renewal (loyalty_to_players > 50):**
If a player has `contract_expiry` within 12 months AND `player_market_status` is `franchise_player` or `fan_favourite`: club initiates renewal. Offered wage = current wage × 1.05–1.20 (based on player's recent performance tier).

**Proactive selling of expiring players (resale_focus > 60):**
If contract < 8 months and player is NOT a key player: club lists player (`transfer_listed` status). Avoids losing player for free.

**Veteran walk (age > 32, contract expiring):**
Club offers minimal renewal or allows expiry depending on `loyalty_to_players`. Low loyalty_to_players clubs let veterans walk; high loyalty clubs over-extend aging stars (irrational but realistic).

**Player triggers:**
High `ambition` (> 70) players in the final year of contract push for move rather than renewal. High `loyalty` (> 70) players accept below-market renewals. `greed` > 70 players demand 30%+ wage increase.

---

## Section 16 — Player Unhappiness

Table: `player_unhappiness`
Columns: `manager_id`, `player_id`, `reason TEXT`, `severity INTEGER` (0–100), `created_date TEXT`, `resolved INTEGER` (0/1).

### Triggers

| Event | Severity added | Reason |
|---|---|---|
| Transfer collapsed 3+ times (selling club blocked) | +25 | `blocked_move` |
| Player listed but no buyer after 30+ days | +15 | `unwanted` |
| Contract expiring within 3 months, no renewal offer | +10 | `contract_stall` |
| `medical_pending` collapse | +10 | `failed_medical_stress` |
| Star player promise + club signs competitor at same position within 30 days | +20 | `broken_promise` |

Severity accumulates. Above 60: `player_unrest` news item fires. Above 80: player treated as `transfer_listed` regardless of `loyalty_to_players`. Phase 3 adds playing time and bench triggers.

---

## Section 17 — Transfer Cascade

Every completed transfer triggers:
1. Re-evaluate squad needs for selling club (lost a player)
2. Re-evaluate squad needs for any club that lost a `competing_bid` on same player
3. Re-evaluate market scarcity for affected position group
4. If selling club's need > 50: immediately enter `initiated` for a replacement (cascade depth +1)
5. News item: completed transfer

Every collapsed transfer triggers:
1. Re-evaluate needs for buying club
2. If `collapse_reason = competing_bid`: winning club news item; losing club's need urgency flagged
3. Player unhappiness +10 if 2nd+ collapse this window
4. Cascade depth +1 if replacement search begins

**Cascade depth cap: 3.** A transfer triggers at most 2 further transfers in a single chain. Prevents infinite loops while still allowing realistic ripple effects.

---

## Section 18 — Transfer News

Table: `transfer_news`
Columns: `id`, `manager_id`, `game_date TEXT`, `headline TEXT`, `category TEXT`, `player_id INTEGER`, `from_club_id INTEGER`, `to_club_id INTEGER`.

| Category | Trigger |
|---|---|
| `rumour` | `initiated` state starts |
| `bid` | `offer_made` |
| `agreed` | `medical_pending` reached |
| `completed` | Transfer finalised |
| `collapsed` | Deal dies after `offer_made` |
| `competing_bid` | `competing_bid` state entered |
| `agent_leak` | `agent_pressure > 70` during `negotiating` |
| `player_unrest` | `player_unhappiness.severity > 60` |
| `medical_fail` | `failed_medical` collapse |
| `contract_renewal` | Key player renews contract |
| `player_listed` | `transfer_listed` or `surplus_to_requirements` set |

Headlines generated inline: `"{PlayerName} linked with move to {Club}"`, `"{Club} agree fee for {PlayerName}"`, `"Deal collapses — {PlayerName} stays at {Club}"`, etc.

---

## Section 19 — Daily Processor

Function: `ProcessDailyTransfers(int managerId, const std::string &currentDate)`

Called from the existing day advance path, after weekly/annual processors.

```
1. Determine window status (in_window, days_to_deadline)
2. Calculate deadline_pressure = max(0, 100 − (days_to_deadline * 5))
3. Update player_market_status flags (re-evaluate every 30 days)
4. If in_window:
   a. Update market_scarcity (at window open only; cached thereafter)
   b. For each club (all clubs, not just user's):
      - Check financial guards (Section 10) — skip if in debt panic or at wage limit
      - Check squad size (Section 6) — suppress new signings if squad > 32
      - Re-check need scores
      - Roll irrationality
      - If eligible: select target tier → filter by knowledge + cooldowns + identity traits
        → insert transfer_negotiations row (state='initiated')
   c. Advance all in-flight negotiations:
      - Increment days_in_state
      - Run state transition logic (Section 11)
      - Update deadline_pressure per row
      - Update agent_pressure (+5/day in negotiating/stalled)
      - Apply negotiation personality modifiers
   d. Complete deals (state='completed'):
      - UPDATE players SET team_id = buying_club_id
      - Deduct offered_fee from transfer_budget
      - Add offered_wage to squad wage bill
      - Write club_player_relationship (former_player for selling club)
      - Insert transfer_news (completed)
      - Trigger cascade (Section 17)
   e. Handle collapses:
      - Set collapse_reason
      - Insert negotiation_cooldowns row
      - Write club_player_relationship (rejected_offer or blocked_move)
      - Insert transfer_news (collapsed)
      - Update player_unhappiness if relevant
      - Trigger cascade need re-evaluation
5. Contract renewal check (1st of month, outside window):
   - Run ProcessContractRenewals (Section 15)
6. If NOT in_window:
   - Only process deals already in flight (window closed mid-negotiation)
   - Free-agent signings eligible (rare roll, low probability)
   - No new competitive signings initiated
```

**RNG:** All probabilistic decisions use `rng = (unsigned int)(managerId * 7919u ^ clubId * 31337u ^ date_seed)` where `date_seed` = days since epoch from current date string. Non-deterministic across saves, deterministic within a save for identical inputs.

---

## Tables Summary

```sql
-- Seeded at career start
player_traits (manager_id, player_id,
               ambition, loyalty, greed, ego, trophy_hunger, adaptability, professionalism)

club_transfer_identity (manager_id, club_id,
                        aggression, wage_willingness, age_preference, deadline_panic,
                        loyalty_to_players, financial_risk_tolerance, selling_pressure,
                        youth_focus, domestic_bias, resale_focus, prestige_bias,
                        irrationality, negotiation_personality TEXT)

club_player_knowledge (manager_id, club_id, player_id, knowledge)

-- Live relationship and status memory
club_player_relationship (manager_id, club_id, player_id, relationship_type, created_date)

player_market_status (manager_id, player_id, status, set_date)

-- Live transfer engine
transfer_negotiations (manager_id, buying_club_id, selling_club_id, player_id,
                       state, offered_fee, offered_wage, promised_role,
                       days_in_state, initiated_date, deadline_pressure, agent_pressure,
                       collapse_reason, competing_bid_club_id, acceptance_score,
                       irrationality_driven, tier, counter_offer_count)

negotiation_cooldowns (manager_id, buying_club_id, player_id, cooldown_until, reason)

market_scarcity (manager_id, position_group, scarcity_score, last_updated)

-- Output
transfer_news (manager_id, game_date, headline, category,
               player_id, from_club_id, to_club_id)

player_unhappiness (manager_id, player_id, reason, severity, created_date, resolved)
```

---

## DB Migration Order

1. `player_traits`
2. `club_transfer_identity`
3. `club_player_knowledge`
4. `club_player_relationship`
5. `player_market_status`
6. `market_scarcity`
7. `transfer_negotiations`
8. `negotiation_cooldowns`
9. `transfer_news`
10. `player_unhappiness`

Seed in this order at career start: identity tables first (1–3), then relationship/status (4–5), then market state (6). Live tables (7–10) start empty.

---

## What This Is NOT (Phase 3+4)

- User-facing negotiation UI (browsing market, making bids, viewing counteroffers)
- Transfer window market inflation tracking visible to player
- Special scripted events (galactico, financial collapse sale, wonderkid explosion trigger)
- Scouting system growth (knowledge evolving via staff over seasons)
- Playing time tracking for promise fulfillment / unhappiness from benching
- Loan system
- Transfer fee installment / add-ons

Phase 3 builds the player-facing negotiation experience on top of this engine. Phase 4 adds scripted special events and inflation dynamics visible to the player.
