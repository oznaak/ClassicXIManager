# Transfer System Design — Phase 1 + 2

> **Scope:** Phase 1 (data foundation: player personality, club identity, scouting knowledge) and Phase 2 (AI market engine: squad needs, negotiations, cascade, deadline chaos). User-facing negotiation UI, special events (galactico, financial collapse), and transfer window inflation are Phase 3+4.

---

## Philosophy

Transfers are driven by pressure, ambition, finance, reputation, ego, timing, and irrationality — not by pure squad optimisation. The engine must produce believable chaos: panic buys, vanity signings, clubs behaving differently from each other, deals that collapse at the last second, and cascade effects that ripple across the market. Perfect logic is forbidden. Controlled irrationality is a first-class requirement.

---

## Transfer Windows

- **Summer:** July 1 – August 31
- **Winter:** January 1 – January 31
- Outside windows: free-agent signings only (rare, flagged as opportunistic)
- Deadline = last calendar day of each window

---

## Architecture Overview

```
player_traits            — hidden per-player personality (seeded at career start)
club_transfer_identity   — hidden per-club DNA (seeded at career start)
club_player_knowledge    — which clubs know which players (scouting visibility)
transfer_negotiations    — state machine rows for every deal in flight
transfer_news            — minimal headline log for the news page
player_unhappiness       — tracks broken promises, benching, blocked moves
```

All tables are per-`manager_id`. Daily processor `ProcessDailyTransfers()` drives everything.

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

### Club Archetypes (hidden, never shown directly)

Derived from trait combinations at career start. Used to seed narrative flavor only — the underlying traits are what the engine actually uses:

- `conservative` — low aggression, low irrationality, high loyalty_to_players
- `aggressive` — high aggression, high wage_willingness, low loyalty_to_players
- `gambling` — high financial_risk_tolerance, high irrationality, high deadline_panic
- `youth_factory` — high youth_focus, high resale_focus, low prestige_bias
- `selling_club` — high selling_pressure, low loyalty_to_players, high resale_focus
- `prestige_driven` — high prestige_bias, high wage_willingness, low resale_focus
- `opportunistic` — high irrationality, mid aggression, high adaptability to market shifts
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
- **Nostalgia move:** re-sign a former player now at a different club (loyalty_to_players > 65, player age > 28)
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

## Section 4 — Squad Need Engine

Runs at: career load, window open, and after every completed or collapsed transfer (cascade trigger).

### Position Groups

Map player `role` to four groups:
- `GK` — goalkeepers
- `DEF` — CB, LB, RB, LWB, RWB
- `MID` — CDM, CM, CAM, LM, RM
- `ATT` — LW, RW, CF, ST

### Need Score Formula

For each group per club:

```
need = quality_gap     * 30   // group avg base_stat below league avg for position
     + depth_gap       * 25   // fewer than 2 players in group
     + age_problem     * 20   // group avg age > 30
     + expiry_pressure * 15   // 2+ players with contract_expiry within 6 months
     + identity_fit    * 10   // age_preference mismatch with group avg age
```

Clamped 0–100.

### Action Thresholds

| Score | Behaviour |
|---|---|
| > 65 | Active search — initiate negotiation this window |
| 40–65 | Monitoring — joins competing bid if opportunity appears |
| < 40 | Ignore — unless irrationality roll fires |

### Imperfection Rules

- Clubs with `professionalism < 30` may ignore a score of 70+
- Clubs with `financial_risk_tolerance < 20` won't act above score 50 if cash is tight
- Irrationality roll can override threshold entirely (vanity/panic buys)
- A club already running 2+ negotiations won't start a third unless `aggression > 75`

---

## Section 5 — Improved Valuation

Player transfer value is dynamic — not a static `playervalue` field. Calculated per negotiation context.

```
contextual_value =
  base_playervalue
  × contract_factor       // 1.3 if 3+ years left, 0.7 if < 1 year, 1.0 otherwise
  × scarcity_factor       // 1.0–1.4: fewer players of same quality/position = higher
  × seller_factor         // 0.7–1.0: selling_pressure reduces asking price
  × buyer_urgency         // 1.0–1.3: need_score > 70 = buyer overpays
  × deadline_factor       // 1.0–1.5: climbs in final 5 days of window
  × hype_factor           // 1.0–1.3: agent_pressure inflates price
```

Selling club's ask = `contextual_value × (1.0 + loyalty_to_players * 0.003)` (loyal clubs overvalue own players).

Buying club's offer starts at 80% of contextual_value, steps up in negotiation.

---

## Section 6 — Agent Pressure

Hidden per-negotiation factor. Not a full agent system — simulates agent behavior as a modifier.

Column `agent_pressure` (0–100) on `transfer_negotiations`. Seeded from:
- Player's `greed` trait (primary driver)
- Player's `international_reputation`
- Whether multiple clubs are pursuing same player (`competing_bid` state)

### Effects

| Pressure level | Effect |
|---|---|
| > 70 | Wage demand inflated 1.2–1.4×; media leak triggers (news item: "Agent confirms interest") |
| 50–70 | Asking price rises 10–20% mid-negotiation; deal volatility increases |
| < 30 | Clean negotiation; player accepts reasonable offer quickly |

Agent pressure increases by +5 per day the deal stays in `negotiating` or `stalled` states. Represents agent stirring the pot over time.

---

## Section 7 — Negotiation State Machine

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
```

### States

```
initiated
  ↓ (1–3 days)
offer_made
  ↓ (1–4 days) selling club accepts/rejects
negotiating
  ↓↗ (loops)
counter_offer      ← selling club wants more
stalled            ← neither side moves (patience expired)
competing_bid      ← another club enters for same player
  ↓
medical_pending    ← deal agreed, 1-day medical check
  ↓
agreed             ← 1-day confirmation
  ↓
completed          ← player moves; cascade triggers
  OR
collapsed          ← at any state after offer_made
```

### State Transition Logic

**initiated → offer_made:**
Days = `3 - (club.aggression / 50)` clamped 1–3.

**offer_made → negotiating or collapsed:**
Selling club evaluates: `loyalty_to_players` (high = rejects low bids), `selling_pressure` (high = accepts), contextual value vs offered fee. If offered_fee < 60% of contextual_value and selling_pressure < 40: collapsed. Otherwise: negotiating.

**negotiating (daily tick):**
Calculate `acceptance_score` (see Section 8). If score > 65: → `medical_pending`. If score 40–65: → `counter_offer`. If score < 40: → `collapsed`. Roll for `competing_bid` (15% daily chance if player knowledge > 60 at any other club with matching need).

**counter_offer:**
Buying club raises fee by 5–15% and wage by 5–10% (modulated by `wage_willingness`). Returns to `negotiating`. If buying club has already counter-offered 3+ times and score still < 50: → `collapsed`.

**stalled:**
Triggered when `days_in_state > 5` in `negotiating` with no score movement. Deal sits. 30% daily revival chance. Agent pressure increases +10. If stalled > 10 days: collapsed.

**competing_bid:**
Another club enters. Raises `agent_pressure` by 20. Original buyer must increase offer (aggression-weighted) or lose player. Can loop back to `negotiating` or produce `collapsed` for the losing club.

**medical_pending:**
1 day. 95% pass rate. 5% collapse (collapse_reason = "failed_medical"). Memorable rare event.

**collapsed:**
Sets `collapse_reason`. Triggers cascade for buying club (need re-evaluation). If player was listed by selling club and collapses 3+ times: player_unhappiness +20 for selling club (blocked move).

### Deadline Day Special Rules

When `deadline_pressure > 80` (final ~4 days):
- Clubs with `deadline_panic > 50`: aggression +30, wage_willingness +25, patience −40
- `stalled` deals revival chance rises to 60%
- `counter_offer` loops reduced by 1 (less patience for back-and-forth)
- `medical_pending` collapse chance rises to 12% (rushed medicals)
- Irrationality roll fires daily for panicking clubs regardless of need score

---

## Section 8 — Player Acceptance Score

Calculated during `negotiating` state. Score 0–100. Above 65 = player agrees. 40–65 = counter. Below 40 = rejection.

```
acceptance_score =
  wage_fit          * 0.20   // offered_wage vs expected_wage
  + role_fit        * 0.18   // promised_role vs ego threshold
  + prestige_fit    * 0.18   // buying club institutional_power vs player trophy_hunger
  + league_fit      * 0.10   // buying club league reputation vs player adaptability
  + playtime_fit    * 0.14   // squad depth at player position (playing time likelihood)
  + ambition_pull   * 0.10   // ambition trait × prestige delta from current club
  − loyalty_drag    * 0.10   // loyalty trait (cost to leave current club)
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

**playtime_fit:** Inverse of how crowded the player's position is at buying club. Fewer incumbents = higher score. Weighted by ego.

**ambition_pull:** `(ambition / 100.0f) × max(0, buying_club.institutional_power − selling_club.institutional_power)`.

**loyalty_drag:** `(loyalty / 100.0f) × 100` — subtracted raw. High loyalty makes every move harder.

Agent pressure modifies final score: `acceptance_score += (agent_pressure - 50) × 0.10f` (high pressure pushes player toward accepting faster; moderate pressure is neutral).

---

## Section 9 — Squad Role Promises

`promised_role` column on `transfer_negotiations`: `star_player | important | rotation | prospect`.

Role is set by buying club based on:
- How desperate they are (need_score): high need → generous promise
- Ego of player: high ego → forced to promise `star_player` or player rejects
- Club's current depth at position: no competitors → `important` minimum

**Promise tracking:** When a deal completes, the promised role is stored. Phase 3 will track whether promises are kept. Phase 1+2 seeds the data; unhappiness for broken promises is handled now.

---

## Section 10 — Player Unhappiness

Table: `player_unhappiness`
Columns: `manager_id`, `player_id`, `reason TEXT`, `severity INTEGER` (0–100), `created_date TEXT`, `resolved INTEGER` (0/1).

### Triggers

| Event | Severity added | Reason |
|---|---|---|
| Transfer collapsed 3+ times (selling club blocked) | +25 | `blocked_move` |
| Player listed but no buyer after 30+ days | +15 | `unwanted` |
| Contract expiring within 3 months, no offer made | +10 | `contract_stall` |
| Negotiation collapsed in `medical_pending` | +10 | `failed_medical_stress` |

Severity accumulates per player per career. Above 60: player generates a transfer_news item ("Player X pushing for move"). Above 80: treated as transfer-listed regardless of club's `loyalty_to_players`. Phase 3 will add playing time and bench triggers.

---

## Section 11 — Transfer Cascade

Every completed transfer triggers:
1. Re-run squad need evaluation for selling club
2. Re-run squad need evaluation for any club that lost a `competing_bid` for the same player
3. If selling club's need score jumps > 50 in any group: they immediately enter `initiated` state for a replacement
4. News item: completed transfer headline

Every collapsed transfer triggers:
1. Re-run need for buying club (may try different target)
2. If collapse_reason = `competing_bid`: winning club gets news item; losing club's need_score urgency flagged
3. Player unhappiness +10 if it was their 2nd+ collapse this window

Cascades are capped at depth 3 (a transfer causes a transfer causes a transfer, no further) to prevent infinite loops.

---

## Section 12 — Transfer News

Table: `transfer_news`
Columns: `id`, `manager_id`, `game_date TEXT`, `headline TEXT`, `category TEXT`, `player_id INTEGER`, `from_club_id INTEGER`, `to_club_id INTEGER`.

Categories and when they fire:

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

Headlines are generated strings: `"{PlayerName} linked with move to {Club}"`, `"{Club} agree fee for {PlayerName}"`, etc. No template table needed — generated inline from player/club names.

---

## Section 13 — Daily Processor

Function: `ProcessDailyTransfers(int managerId, const std::string &currentDate)`

Called from the existing day advance path, after weekly/annual processors.

```
1. Determine window status (in_window, days_to_deadline)
2. Calculate deadline_pressure = max(0, 100 − (days_to_deadline * 5))
3. If in_window:
   a. For each club (all, not just user's):
      - Re-check need scores (cached from last evaluation)
      - Roll irrationality
      - If eligible (need or irrationality): attempt to initiate negotiation
        → select target from knowledge pool (knowledge >= 30, weighted)
        → apply identity filters (age_preference, domestic_bias, youth_focus)
        → insert row into transfer_negotiations (state='initiated')
   b. Advance all in-flight negotiations:
      - Increment days_in_state
      - Run state transition logic (see Section 7)
      - Update deadline_pressure on each row
      - Update agent_pressure (+5 per day in negotiating/stalled)
   c. Complete agreed deals:
      - UPDATE players SET team_id=buying_club_id
      - Deduct offered_fee from buying club transfer_budget
      - Add offered_wage to buying club weekly wage bill
      - Insert transfer_news (completed)
      - Trigger cascade (Section 11)
   d. Handle collapses:
      - Insert transfer_news (collapsed)
      - Update player_unhappiness if relevant
      - Trigger cascade need re-evaluation
4. If NOT in_window:
   - Only process deals already in flight (window may have closed mid-negotiation)
   - No new initiations
```

RNG: all probabilistic decisions use `rng = (unsigned int)(managerId * 7919u ^ clubId * 31337u ^ date_seed)` where `date_seed` is derived from the current date string. Non-deterministic across saves, deterministic within a save for the same inputs.

---

## Tables Summary

```sql
-- Seeded at career start
player_traits (manager_id, player_id, ambition, loyalty, greed, ego,
               trophy_hunger, adaptability, professionalism)

club_transfer_identity (manager_id, club_id, aggression, wage_willingness,
                        age_preference, deadline_panic, loyalty_to_players,
                        financial_risk_tolerance, selling_pressure, youth_focus,
                        domestic_bias, resale_focus, prestige_bias, irrationality)

club_player_knowledge (manager_id, club_id, player_id, knowledge)

-- Live during career
transfer_negotiations (manager_id, buying_club_id, selling_club_id, player_id,
                       state, offered_fee, offered_wage, promised_role,
                       days_in_state, initiated_date, deadline_pressure,
                       agent_pressure, collapse_reason, competing_bid_club_id,
                       acceptance_score, irrationality_driven)

transfer_news (manager_id, game_date, headline, category,
               player_id, from_club_id, to_club_id)

player_unhappiness (manager_id, player_id, reason, severity,
                    created_date, resolved)
```

---

## What This Is NOT (Phase 3+4)

- User negotiation UI (browsing market, making bids, viewing counteroffers)
- Transfer window phases with market inflation tracking
- Special scripted events (galactico signing, financial collapse sale, wonderkid explosion)
- Scouting system evolution (knowledge growing over seasons via staff)
- Playing time tracking for promise fulfillment / unhappiness from benching
- Loan system

Phase 3 builds the player-facing negotiation experience on top of this engine. Phase 4 adds the scripted special events and inflation dynamics.
