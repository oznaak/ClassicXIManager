# Transfer System Phase 3+4 Design

> **Scope:** User-facing negotiation experience (buy-side, sell-side), full loan system (fee, wage split, recall clause, buy-back, option-to-buy), playing time and promise fulfillment tracking, special scripted events (galactico, financial collapse, wonderkid explosion), negotiation psychology, deadline chaos, squad harmony, media pressure, succession targeting, and AI poaching escalation. Builds directly on top of the Phase 1+2 engine.

---

## Philosophy

Phase 3+4 makes the transfer market player-facing while protecting the chaos that makes it feel real.

All user interactions follow the same async pattern the AI engine uses internally — offers take days to resolve, counteroffers arrive in the inbox, deals collapse at any stage. The player participates in the simulation using the same rules as every other club.

**Anti-optimisation is non-negotiable.** Counter-offer psychology, soft budget hiding, momentum decay, player preference profiles, and controlled irrationality exist specifically to prevent the player from solving the market like a math problem. Panic, ego, bad timing, emotional decisions, and irrational bidding wars are not edge cases — they are first-class outputs this system must produce and protect.

---

## Architecture

### New Files

- `src/menu/user_transfer.hpp` — declares all public functions: `InitiateUserBid()`, `RespondToOffer()`, `OfferLoan()`, `RespondToLoanOffer()`, `TickUserNegotiations()`, `SeedSpecialEvents()`, `ProcessSpecialEvents()`, `ProcessMediaPressure()`, `TrackPlayerAppearances()`, `EvaluatePromiseFulfillment()`, `ProcessPoachingEscalation()`
- `src/menu/user_transfer.cpp` — implements all of the above

### Modified Files

- `src/menu/transfer_engine.cpp` — `AdvanceNegotiations()` skips `is_user_bid = 1` rows; `ProcessDailyTransfers()` calls `TickUserNegotiations()`, `ProcessSpecialEvents()`, `ProcessMediaPressure()`, `ProcessPoachingEscalation()`; deadline chaos scaling injected into daily loop
- `src/menu/managercareer.cpp` — `GenerateCareerSeason()` calls `SeedSpecialEvents()`, `SeedPlayerPreferences()`; `AdvanceDay()` calls `TrackPlayerAppearances()` after fixture completion
- `src/menu/imgui_career.cpp` — new primary tab (index 6) for transfer market UI
- `sources.cmake` — register `user_transfer.hpp` and `user_transfer.cpp`

### Schema Migrations (added to EnsureCareerTables)

**Additions to `transfer_negotiations`:**
```sql
ALTER TABLE transfer_negotiations ADD COLUMN is_user_bid INTEGER DEFAULT 0;
ALTER TABLE transfer_negotiations ADD COLUMN user_pending_action TEXT DEFAULT '';
-- user_pending_action: 'accept_counter'|'reject'|'counter'|'' — set by inbox button press
ALTER TABLE transfer_negotiations ADD COLUMN negotiation_momentum INTEGER DEFAULT 0;
-- Range -100 to +100. +10 per positive round, -15 per counter, -20 per stall.
-- momentum > 50: completion chance +15%. momentum < -30: collapse risk +20%.
ALTER TABLE transfer_negotiations ADD COLUMN seller_patience_days INTEGER DEFAULT 7;
-- Days until selling club withdraws offer. Countdown from state entry. 0 = no limit (user-side only).
ALTER TABLE transfer_negotiations ADD COLUMN player_patience_days INTEGER DEFAULT 5;
-- Days until player stops waiting on personal terms. Counted from player_talks entry.
```

**Additions to `player_traits`:**
```sql
ALTER TABLE player_traits ADD COLUMN pref_domestic INTEGER DEFAULT 0;   -- 1 = strongly prefers own nationality league
ALTER TABLE player_traits ADD COLUMN pref_prestige INTEGER DEFAULT 0;   -- 1 = prestige weight doubled
ALTER TABLE player_traits ADD COLUMN pref_wages INTEGER DEFAULT 0;      -- 1 = wage weight doubled, role/prestige halved
ALTER TABLE player_traits ADD COLUMN pref_development INTEGER DEFAULT 0;-- 1 = playtime_fit weight doubled (young players)
ALTER TABLE player_traits ADD COLUMN pref_guaranteed_starts INTEGER DEFAULT 0; -- 1 = rejects rotation/prospect regardless of score
ALTER TABLE player_traits ADD COLUMN hates_rival_club_id INTEGER DEFAULT 0;    -- club_id: -30 on acceptance score if buying club matches
```

**Additions to `club_transfer_identity`:**
```sql
ALTER TABLE club_transfer_identity ADD COLUMN succession_role TEXT DEFAULT '';
-- Set when a key player (base_stat >= 75) is sold from this club. Biases next need evaluation.
-- Cleared after a suitable replacement is signed (same position group, base_stat >= 65).
```

**Additions to `club_finances` (existing table from Phase 1+2 seed):**
```sql
ALTER TABLE club_finances ADD COLUMN board_confidence INTEGER DEFAULT 70;
-- 0-100. Drops from failed negotiations and poor results. Board sacks manager below 20.
```

### New Tables

```sql
CREATE TABLE IF NOT EXISTS loan_deals (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  manager_id              INTEGER NOT NULL,
  loaning_club_id         INTEGER NOT NULL,
  receiving_club_id       INTEGER NOT NULL,
  player_id               INTEGER NOT NULL,
  loan_fee                INTEGER DEFAULT 0,
  wage_split_pct          INTEGER DEFAULT 100, -- % covered by loaning club
  recall_clause_after_month INTEGER DEFAULT 0, -- 0 = no recall
  option_to_buy_fee       INTEGER DEFAULT 0,   -- 0 = no option
  option_to_buy_deadline  TEXT,
  buy_back_fee            INTEGER DEFAULT 0,
  buy_back_expiry         TEXT DEFAULT '',
  season_year             INTEGER NOT NULL,
  status                  TEXT DEFAULT 'active', -- active/recalled/expired/option_exercised/buy_back_triggered
  created_date            TEXT,
  end_date                TEXT
);

CREATE TABLE IF NOT EXISTS player_appearances (
  manager_id      INTEGER NOT NULL,
  player_id       INTEGER NOT NULL,
  season_year     INTEGER NOT NULL,
  starts          INTEGER DEFAULT 0,
  sub_appearances INTEGER DEFAULT 0,
  PRIMARY KEY (manager_id, player_id, season_year)
);

CREATE TABLE IF NOT EXISTS transfer_special_events (
  id            INTEGER PRIMARY KEY AUTOINCREMENT,
  manager_id    INTEGER NOT NULL,
  event_type    TEXT NOT NULL,  -- 'galactico'|'collapse_sale'|'wonderkid_explosion'
  player_id     INTEGER NOT NULL,
  club_id       INTEGER NOT NULL,
  trigger_date  TEXT NOT NULL,
  fired         INTEGER DEFAULT 0,
  metadata_json TEXT DEFAULT '{}'
);

CREATE TABLE IF NOT EXISTS squad_harmony (
  manager_id              INTEGER PRIMARY KEY,
  morale                  INTEGER DEFAULT 70,  -- 0-100
  dressing_room_stability INTEGER DEFAULT 70,  -- 0-100
  last_updated            TEXT
);

CREATE TABLE IF NOT EXISTS media_pressure_events (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  manager_id   INTEGER NOT NULL,
  event_type   TEXT NOT NULL,
  -- 'fans_angry_sale'|'board_demands_signing'|'failed_negotiations_criticism'|'player_public_push'
  severity     INTEGER DEFAULT 1,  -- 1=minor 2=moderate 3=severe
  game_date    TEXT,
  expires_date TEXT,
  fired        INTEGER DEFAULT 0
);
```

---

## Section 1 — Transfer Market UI (New Primary Tab)

New tab at primary tab index 6: **"Transfers"**. Five sub-panels toggled via a top segmented control.

### "Available" Panel

Players where `is_transfer_listed = 1` OR `player_market_status.status IN ('transfer_listed', 'expiring_soon')`. No scouting required. Columns: name, position group, age, club, value, weekly wage, contract expiry. Filterable by position group (GK / CB / FB_WB / DM / CM / AM_W / ST), sortable by value / age / stat.

Each row has two buttons: **Bid** and **Loan**.

### "Scouted" Panel

Players from `scout_reports` where user has at least one completed report. Stats shown at `revealPct`. User can bid or loan regardless of listed status.

### "My Bids" Panel

Active user bids: `transfer_negotiations WHERE buying_club_id=userClubId AND is_user_bid=1 AND state NOT IN ('completed','collapsed')`. Columns: player, selling club, offered fee, state, days in state, momentum indicator (▲ / ▼ / –).

### "Incoming Bids" Panel

Active AI bids on user's players: `transfer_negotiations WHERE selling_club_id=userClubId AND is_user_bid=0 AND state NOT IN ('completed','collapsed')`. Each row has **Accept** / **Reject** / **Counter** / **Block** buttons.

### "Squad Management" Panel

User proactive market control (see Section 2B). Lists all user's players with quick-action buttons per player.

---

## Section 2A — Bid Composer

Clicking **Bid** opens an inline composer:
- **Transfer fee** — integer input, pre-filled at 80% of `playervalue`
- **Weekly wage** — integer input, pre-filled at player's current wage × 1.1
- **Role promise** — dropdown: `rotation` / `important` / `star_player` / `prospect`
- **Sell-on clause** — optional: toggle + % input (5–25%). Stored on deal row.
- **Loan-back clause** — optional: toggle + months input. Stored on deal row.
- **Submit**

On submit: `InitiateUserBid()` inserts `transfer_negotiations` with `is_user_bid = 1`, state `initiated`. Inbox confirmation sent.

---

## Section 2B — User Negotiation Leverage (Squad Management Panel)

The user must be able to shape the market proactively, not only react. Six actions available per player in the Squad Management panel:

**Declare Untouchable:** Sets `player_market_status.status = 'franchise_player'` for this window. All incoming bids auto-blocked with a "Not for sale" inbox response. Reversed at window end unless user re-declares.

**Set Asking Price:** User sets a public asking price stored on the player's market status row. AI clubs can see this via their knowledge. Affects `CalculateContextualValue()` — seller factor adjusts based on whether offers meet the stated price. Creates transfer news: "[Player] available for £Xm."

**Transfer List Player:** Sets `is_transfer_listed = 1`. Player appears in Available panel for all clubs. Triggers `SeedClubPlayerKnowledge()` refresh for that player (knowledge bump to 60 for all clubs currently below).

**Delay Decision:** On incoming bids, user can delay response for up to 3 extra days without auto-collapse. Can only be used once per negotiation. Buying club's `deadline_panic` increases by 15 during delay.

**Demand Sell-On Clause:** When accepting a bid, user can demand a sell-on % (5–25%) as a condition. If buying club's `negotiation_personality = 'hard_negotiator'` or `prestige_bias > 70`, they may reject this demand and withdraw. Otherwise accepted, stored on deal.

**Demand Loan-Back:** When selling a player, user can demand the player is loaned back for 6–12 months as part of the deal. Buying club evaluates based on squad need for that position — if need score < 40, they reject; otherwise accepted.

---

## Section 3 — Buy-Side Negotiation Flow (Async Inbox)

`AdvanceNegotiations()` skips `is_user_bid = 1` rows. `TickUserNegotiations()` in `user_transfer.cpp` runs daily inside `ProcessDailyTransfers()`.

### Negotiation Momentum

Every negotiation row carries `negotiation_momentum` (–100 to +100, starts at 0):
- +10: fee accepted without counter
- +8: player accepts terms first round
- –15: counter-offer received
- –20: stall (days > 5 in negotiating)
- +5: each day in `player_talks` without issues
- –25: competing bid arrives

Effects:
- momentum > 50: `CalculateAcceptanceScore()` gets +10 bonus
- momentum < –30: collapse probability increases by 20% per state tick
- momentum < –60: seller patience timer halved

### Negotiation Deadlines

Every AI seller has `seller_patience_days` (default 7, set at negotiation insertion). If user does not respond to a counter within that window, state `collapsed`, reason `seller_withdrew`. Player also has `player_patience_days` (default 5 from `player_talks` entry) — if personal terms drag past this, player walks.

`seller_patience_days` is modulated by:
- `deadline_pressure > 80`: days × 0.5 (urgency makes sellers impatient)
- `negotiation_personality = 'patient'`: days × 1.5
- `negotiation_personality = 'fast_closer'`: days × 0.6

### State Machine (user buy-side)

**`initiated` → day 1–2:**
Outcome driven by fee vs contextual value and seller `selling_pressure`:
- Fee ≥ 90% ctxVal OR selling_pressure > 60 → `player_talks`. Inbox: "[Club] accepted your £Xm bid. Personal terms now being negotiated."
- Fee 60–89% → `counter_offer`. Inbox: "[Club] want £Xm." with counter psychology applied (see Section 4).
- Fee < 60% OR franchise_player + seller loyalty > 70 → `collapsed`. Inbox: "[Club] rejected your bid. [Reason]."

**`player_talks` → up to `player_patience_days`:**
`CalculateAcceptanceScore()` computed with player preference profile applied:
- `pref_domestic`: cross-nationality league move → –20 on score
- `pref_prestige`: buying club prestige < selling club prestige → –15
- `pref_wages`: wageFit weight doubled
- `pref_development`: playtimeFit weight doubled
- `pref_guaranteed_starts`: auto-rejects `rotation` and `prospect` regardless of score
- `hates_rival_club_id`: if `buyingClubId` matches → –30 on score (can cause rejection even with great offer)

Score > 65 → `medical_pending`. Inbox: "[Player] happy to join. Medical arranged."
Score 40–65 → inbox: "[Player] wants improved terms." Options: **Improve Wage +10%** / **+20%** / **Upgrade Role** / **Walk Away**. Score recomputed. After 2 failed attempts → `collapsed`.
Score < 40 → `collapsed`. Inbox: "[Player] rejected the move."

**`counter_offer` (user receives counter):**
Inbox with **Accept** / **Reject** / **Counter** buttons. Deadline: `seller_patience_days` from counter sent. No response → auto-collapse. Counter fee input available. Max 4 rounds before auto-collapse. Each round: `negotiation_momentum -= 15`.

**`medical_pending`:**
2% fail chance (5% deadline day). Standard outcome — see Phase 1+2 spec.

### Transfer Hijacks

During `counter_offer` or `medical_pending`, another club may enter with a competing offer. Conditions:
- Player `international_reputation ≥ 3` OR `player_market_status = 'wonderkid'`
- A club with `knowledge ≥ 50` for this player that is not currently in cooldown
- Probability: 15% per day during `counter_offer`, 25% per day during `medical_pending`, 40% per day on deadline day

**Hijack mechanics:**
- Competing club offers: fee × 1.15, wage × 1.10, one tier higher role promise
- Inbox fires: "[Competing Club] have entered the race for [Player] with a £Xm offer."
- Player acceptance score recalculated for competing offer — if competing score > user's current score by 15+, player switches preference
- User options: **Raise bid to match** (fee × 1.10 auto-applied) / **Stand firm** (original terms) / **Walk away**
- `negotiation_momentum -= 25` regardless of user response
- If user stands firm and competing score stays higher: deal collapses within 2 days

---

## Section 4 — Counter-Offer Psychology

Counter-offers are not purely numeric. Response behaviour depends on the selling club's personality and context. Applied to both user sell-side (incoming counters from AI) and user buy-side (AI countering user's bid).

### Personality Responses

| Negotiation Personality | Counter behaviour |
|---|---|
| `patient` | Holds position. Will wait full `seller_patience_days`. Counter is modest (5–8%). |
| `fast_closer` | Splits difference immediately. Happy to accept 85–90% of asking price if deal closes fast. |
| `hard_negotiator` | Starts high, concedes slowly. May reject sell-on/loan-back clauses outright. Counter is aggressive (0–3% concession). |
| `media_manipulator` | Leaks negotiation to press mid-deal. Increases agent pressure on player. May bluff about rival interest. |
| `desperate` | Concedes heavily if `selling_pressure > 70`. May accept below-value offers on deadline day. |

### Urgency Modifiers

- `deadline_pressure > 80`: desperate and media_manipulator clubs accept 10–15% lower than normal
- `deadline_pressure > 80` + `negotiation_personality = 'hard_negotiator'`: walks away faster (patience halved) — pride over necessity
- `selling_pressure > 80` + any personality: override to desperate behaviour

### Relationship and Prestige Modifiers

- Previous `rejected_offer` in `club_player_relationship`: selling club demands 15% above normal asking price
- Prestige gap (buying club > selling club by 3+): selling club may refuse bidding war — "We don't negotiate upward with smaller clubs." Hard counter or collapse.
- `irrationality > 70`: wildly overbid or refuse reasonable offers with no logic applied (see Phase 1+2 anti-optimisation rule)

### Mood Escalation

If user takes 3+ days to respond to a counter:
- `patient`: extends patience by 2 days
- `fast_closer`: sends a "final offer" inbox, collapses in 1 day if not accepted
- `hard_negotiator`: withdraws immediately on day 3, sets 21-day cooldown
- `media_manipulator`: generates a "Talks at risk" transfer news headline, raising agent pressure by 15
- `desperate`: panics and lowers asking price by 10%

---

## Section 5 — Sell-Side Flow

`AdvanceNegotiations()` detects `selling_club_id = userClubId AND is_user_bid = 0` transitioning to `offer_made`. Pauses auto-advance, fires inbox. Row waits for user response — no auto-timeout for user sell-side (user sets the pace on selling).

### Response Options

**Accept:** Advances to `negotiating`. AI club negotiates personal terms via `CalculateAcceptanceScore()`. Result arrives in inbox within 1–3 days.

**Reject:** `collapsed`. 14-day cooldown for buyer. Inbox confirmation.

**Counter:** User sets asking price. Counter psychology applied to buyer's response (Section 4).

**Block (franchise players):** `negotiation_cooldown` of 90 days inserted. Player marked not-for-sale this window.

### Squad Harmony Impact on Selling

When user completes a sale, `ProcessSquadHarmonyOnSale()` evaluates the sold player:

- **Captain** (`formationOrder = 0` or designated captain flag): `squad_harmony.dressing_room_stability -= 20`, `squad_harmony.morale -= 15`. Media pressure event fired: `fans_angry_sale`, severity 2.
- **Fan favourite** (player with `international_reputation ≥ 3` AND has been at club ≥ 2 seasons — tracked via earliest fixture appearance date): `squad_harmony.morale -= 10`. Media event: `fans_angry_sale`, severity 1.
- **Elite performer** (`base_stat ≥ 80`): `board_confidence -= 10` in `club_finances`. Succession role set in `club_transfer_identity.succession_role`.

Buying a high-profile player (`international_reputation ≥ 4`) temporarily boosts: `squad_harmony.morale += 15`, `squad_harmony.dressing_room_stability += 5`. Decays back to baseline at 2 points/week.

Squad harmony values (0–100) affect:
- `morale < 40`: players accumulate unhappiness 2× faster
- `dressing_room_stability < 30`: `negotiation_momentum` on user buy-side starts at –20 (word spreads)
- `morale > 80`: user's players have +5 on acceptance score when approached by AI clubs (happy players less likely to leave)

---

## Section 6 — Loan System

### Loan Composer (user loans out)

From squad screen or Squad Management panel. Fields:
- **Receiving club** — clubs with `knowledge ≥ 30` for this player
- **Loan fee** (default £0)
- **Wage split %** — slider 0–100 (% loaning club covers)
- **Recall clause** — month picker (0 = no recall)
- **Option to buy** — toggle + fee + deadline
- **Buy-back clause** — toggle + fee + expiry date. If set: loaning club retains right to re-sign player at `buy_back_fee` within `buy_back_expiry`, regardless of receiving club's consent.

### Loan AI Intelligence Filters

AI clubs evaluating a loan offer check (in `user_transfer.cpp` `ProcessAILoanDecision()`):

1. **Guaranteed playtime:** Count existing players in target position group at receiving club. If count ≥ 3, loan rejected (player won't get minutes).
2. **League reputation:** Receiving club's `domestic_prestige` vs loaning club's. If gap > 4 tiers down, ambition trait check — high ambition (> 65) player rejects downgrade.
3. **Development quality:** If player `age ≤ 22 AND sofifaPotential ≥ 80`: receiving club must have `domestic_prestige ≥ 4`. Wonderkids not loaned to weak clubs.
4. **Nationality familiarity:** `pref_domestic = 1` players require receiving club's league to match player's nationality league, or a known top-5 league.
5. **Tactical fit:** Position group match required. No striker loaned to club with `AM_W` need only.

### Buy-Back Clause

If `buy_back_fee > 0 AND buy_back_expiry != ''`:
- `ProcessDailyTransfers()` checks daily for `loan_deals` where `buy_back_expiry <= AddDays(currentDate, 30)` and player's value has increased significantly (`playervalue >= buy_back_fee × 1.5`).
- If user is loaning club and above condition met: inbox "Consider exercising buy-back on [Player]? Buy-back window closes [date]. Current value: £Xm. Buy-back fee: £Ym."
- **Exercise buy-back:** Player transferred back to loaning club at `buy_back_fee`. `loan_deals.status = 'buy_back_triggered'`.
- AI clubs with buy-back clauses exercise automatically if `EvaluateSquadNeeds()` score > 60 for that position and they have budget.

### Recall Clause, Option to Buy, Season End

Unchanged from original spec.

---

## Section 7 — Playing Time and Promise Fulfillment

### Tracking Appearances

`TrackPlayerAppearances(managerId, fixtureId)` called in `AdvanceDay()` after fixture completion. Reads starting XI from formation data. Starters: `starts += 1`. Named bench players who appeared (simplified: any bench slot that was active in a match where the user team played more than 60 minutes — use fixture completion flag): `sub_appearances += 1`.

### Promise Thresholds

Evaluated monthly on 1st in `EvaluatePromiseFulfillment()`:

| Promised role | Kept if | Violated if |
|---|---|---|
| `star_player` | starts ≥ fixtures × 0.75 | starts < fixtures × 0.50 |
| `important` | starts ≥ fixtures × 0.50 | starts < fixtures × 0.30 |
| `rotation` | (starts + subs) ≥ fixtures × 0.40 | appearances < fixtures × 0.20 |
| `prospect` | any appearance | 0 appearances after month 3 |

**Violation:** `AddUnhappiness(managerId, pid, 'promise_broken', 20, currentDate)`. Inbox notification. If `severity > 80`: auto-flagged `transfer_listed`, agent pressure set to 80.

### Promise Decay

Monthly, on the same pass as evaluation: for any player with `player_unhappiness.reason = 'promise_broken' AND resolved = 0`:
- If the current month's promise is **being kept** (threshold met): `severity -= 5` (minimum 0).
- If severity reaches 0: `resolved = 1`. Inbox: "[Player]'s happiness has recovered."

This prevents temporary benching due to injury/rotation from permanently destroying morale.

---

## Section 8 — Special Events (Background Simulation)

Seeded in `SeedSpecialEvents()` at career start. One of each type per career. Checked daily in `ProcessSpecialEvents()`.

### Galactico Signing

**Seed:** Buying club `international_prestige ≥ 8`. Target `international_reputation ≥ 4` at club with `international_prestige ≤ 6`. Trigger: summer window, day 20–28.

**Fire behaviour (state machine injection — not a bypass):**
On trigger date, `ProcessSpecialEvents()` inserts a `transfer_negotiations` row with:
- `aggression` overridden to 100 for buying club
- `offered_fee = playervalue × 2.2` (massively above market)
- `offered_wage = current_wage × 2.5`
- `promised_role = 'star_player'`
- `tier = 1`, `irrationality_driven = 1`
- `seller_patience_days = 1` for each state (deal moves at 1 day per state)
- `deadline_pressure = 90` (injected directly)
- Selling club's `selling_pressure` temporarily set to 85 for this deal

The deal runs through all normal states but at extreme speed. `CalculateAcceptanceScore()` returns near-100 due to wage/prestige. Unless the selling club is also `international_prestige ≥ 8` (in which case they resist with `selling_pressure = 20`), the deal completes within 4–6 in-game days.

Transfer news on deal start: "[Elite Club] make shock move for [Player]." On completion: "[Elite Club] complete blockbuster signing of [Player]." All players in same position group: `playervalue × 1.15`.

### Financial Collapse Forced Sale

**Seed:** Club with `debt_level > transfer_budget × 2.5` at season start. Target: highest-value non-franchise player. Trigger: first 7 days of summer or winter window.

**Fire behaviour:** `is_transfer_listed = 1`, `player_market_status.status = 'transfer_listed'`, seller `selling_pressure = 90`, `playervalue × 0.70`. Transfer news: "[Club] forced to sell [Player] amid financial crisis." Normal AI negotiations pick this up immediately.

### Wonderkid Explosion

**Seed:** `age ≤ 21`, `sofifaPotential ≥ 88`, `status = 'wonderkid'`. Trigger: either window, day 5–15.

**Fire behaviour:** `international_reputation += 1` (capped at 5). `playervalue × 1.40`. Status → `franchise_player`. All `club_player_knowledge` rows for this player with `knowledge > 0` → `knowledge = MAX(knowledge, 80)`. Transfer news: "[Player] named in national squad — clubs circle the [age]-year-old." AI clubs with elevated knowledge begin competing immediately — this naturally triggers the hijack system if any user bid was in progress.

---

## Section 9 — Negotiation Momentum

Every `transfer_negotiations` row (user and AI) tracks `negotiation_momentum` (–100 to +100, starts 0):

| Event | Momentum change |
|---|---|
| Fee accepted without counter | +10 |
| Player accepts terms first round | +8 |
| Each day progressing without issue | +3 |
| Counter-offer sent/received | –15 |
| Stall (days > 5 in negotiating) | –20 |
| Competing bid arrives | –25 |
| User delays decision | –8 |
| Deadline pressure > 80 | +5/day (urgency creates momentum) |
| Failed medical | –50 (immediate) |

**Effects:**
- `momentum > 50`: acceptance score +10, seller patience extended +2 days
- `momentum < –30`: collapse probability +20% per state tick
- `momentum < –60`: seller patience halved, player patience halved

3 consecutive positive rounds (3 ticks with net momentum gain): `negotiation_momentum += 15` bonus — "deal has legs" — printed in transfer news as "Talks progressing well between [Club] and [Club] over [Player]."

Repeated counters (3+ rounds): every additional counter adds `–5` to collapse threshold. After 6 counter rounds: deal collapses regardless of offers — "Both clubs unable to reach agreement."

---

## Section 10 — Soft Budget Hiding

AI club financial state is never exposed to the user. The following behaviours are implemented to create uncertainty:

**Sell-before-buy:** AI clubs with `financial_risk_tolerance < 40` will not initiate new buy negotiations while `transfer_budget < playervalue × 0.6`. They complete an ongoing sale first, then re-enter the market. This makes AI clubs appear to "disappear" mid-window.

**Sudden withdrawal:** AI clubs with `negotiation_personality = 'fast_closer'` and a deal stalling past 8 days may withdraw entirely even if they could afford the deal. They allocate budget elsewhere. Inbox: "[Club] have withdrawn from the race for [Player]."

**Wage failure:** Even when fee is agreed, AI clubs with low `wage_willingness` may fail player personal terms not because the player refused, but because the club silently couldn't match wage demands. State collapses as `negotiation_failed` with no detailed reason shown to user — just "[Club] and [Player] unable to agree personal terms."

**Phantom interest:** `media_manipulator` clubs may generate a transfer news rumour about a player they have no intention of signing, driving up that player's `playervalue` temporarily and increasing competing bid probability for genuine buyers.

---

## Section 11 — Deadline Day Chaos

Final 48 hours of each window (last 2 days of July/August/January). `ProcessDailyTransfers()` applies chaos scaling:

- **Competing bid probability:** 40% per day (vs 15% normal)
- **`deadline_panic`** for all clubs set to their base value + 40 (capped at 100) for these 2 days only
- **Irrationality rolls:** doubled — clubs with `irrationality > 30` behave as if `irrationality = MIN(100, irrationality × 1.5)`
- **Stalled negotiations revived:** All `stalled` state rows get a forced revive roll — 70% chance to re-enter `negotiating` (vs 30% normal)
- **Emergency loans:** Any club that collapses a permanent deal in the last 3 days of window gets an automatic loan initiation for the same position group (`effectiveNeed = 80`, `tier = 2`)
- **Wage inflation:** `offered_wage` on all new initiations during final 48h multiplied by 1.15
- **Seller patience halved:** All `seller_patience_days` values × 0.5 for these 2 days
- **Transfer news frequency:** Every state change fires a transfer news headline (normally only initiated/completed/collapsed generate headlines)

---

## Section 12 — Player Preference Profiles

Seeded in `SeedPlayerPreferences()` called from `GenerateCareerSeason()`. Adds to `player_traits` rows. One dominant preference per player (with small random chance of a second).

### Seeding Rules

| Preference | Triggered when |
|---|---|
| `pref_domestic` | `nationality` matches league nationality AND `adaptability < 35` |
| `pref_prestige` | `ego > 65` AND `international_reputation ≥ 3` |
| `pref_wages` | `greed > 70` |
| `pref_development` | `age ≤ 22` AND `sofifaPotential ≥ 80` AND `ambition > 60` |
| `pref_guaranteed_starts` | `ego > 60` AND `age ≥ 27` (veteran ego) |
| `hates_rival_club_id` | Seeded for 15% of players at derby clubs (where two clubs share same league and city — approximated by same `league_id` and a seed roll). One club hates the other. |

### Application in Acceptance Score

Applied inside `CalculateAcceptanceScore()` before final score clamping:
- `pref_domestic = 1` AND buying club is cross-nationality league: score –20
- `pref_prestige = 1`: `prestigeFit` weight doubled (36 instead of 18)
- `pref_wages = 1`: `wageFit` weight doubled (40 instead of 20), `prestigeFit` and `roleFit` halved
- `pref_development = 1`: `playtimeFit` weight doubled (28 instead of 14)
- `pref_guaranteed_starts = 1`: if `promisedRole` is `rotation` or `prospect`, score set to 0 (hard reject)
- `hates_rival_club_id` matches `buyingClubId`: score –30 (stackable with other negatives — can push to 0 even with great offer)

---

## Section 13 — Succession Transfers

When a key player (`base_stat ≥ 75`) is sold from a club, `club_transfer_identity.succession_role` is set to that player's position group. This biases the club's next need evaluation.

### Mechanism in EvaluateSquadNeeds()

If `succession_role` is set for this club:
- The matching position group gets `+35` added to its raw need score before normalisation
- This ensures the selling club immediately prioritises finding a like-for-like replacement

### Archetype Matching in AttemptInitiateNegotiations()

When `succession_role` is set and the best need group matches it, the target selection query adds an additional filter:

If the sold player was a high-stat player (`base_stat ≥ 80`): `AND p.base_stat >= 70` (seek quality replacement).
If sold player was young (`age ≤ 24`): `AND p.age <= 26` (seek similar profile).
If sold player was a physical player (`atStrength + atJumping > 140`): prefer players with similar combined attribute (approximated via base_stat threshold for now).

**Clearing succession role:** Once a replacement is signed (transfer completes, player in same position group, `base_stat ≥ 65`), `succession_role` is cleared on `club_transfer_identity`.

---

## Section 14 — AI Poaching Escalation

After user overperformance events, top clubs aggressively target the user's best players. `ProcessPoachingEscalation()` runs at the end of each season (called from `ProcessAnnualCycle()`).

### Trigger Conditions

Evaluated at season end in `managercareer.cpp` `AdvanceDay()` when `allFixturesComplete = true`. Calls `ProcessPoachingEscalation(managerId, userClubId, currentDate, seasonYear)`. Any of the following:
- User's club finished in top 2 of their league (check `standings` table: rank 1 or 2)
- User's club won ≥ 60% of their fixtures in the season (won / played ≥ 0.6)
- User's best player (`base_stat` highest in squad) has `international_reputation ≥ 4`

### Escalation Behaviour

For each trigger condition met:
1. Identify user's top 3 players by `base_stat`
2. For each: bump `club_player_knowledge` to 90 for all clubs with `international_prestige ≥ 7`
3. These clubs' `aggression` temporarily set to `MIN(100, aggression + 25)` for the next window
4. `EvaluateSquadNeeds()` need score for matching position group set to 80 for these clubs (override)
5. Transfer news at window open: "[Elite Club] interested in [Player] after impressive season."

### Difficulty Scaling

Poaching escalation only fires if the user's club `domestic_prestige ≤ 6`. Elite clubs poaching from elite clubs is normal market behaviour — it's already handled by standard negotiation. This system specifically creates pressure for overperforming mid-table and smaller clubs, preventing saves from becoming too comfortable.

---

## Section 15 — Media Pressure

Not full media simulation. Modifiers only. Events seeded into `media_pressure_events` and resolved in `ProcessMediaPressure()` (daily).

### Event Types and Triggers

| Event | Trigger | Effect | Duration |
|---|---|---|---|
| `fans_angry_sale` | Selling player with `international_reputation ≥ 3` | `squad_harmony.morale -= 10/15/20` by severity | 30 days |
| `board_demands_signing` | Window is 50%+ through, user has 0 completed signings, board_confidence < 50 | `board_confidence -= 5/week` if still no signing | Until signing or window end |
| `failed_negotiations_criticism` | 3+ collapsed user buy-side deals in same window | Transfer news: "Manager struggles in transfer market." `board_confidence -= 8` | One-time |
| `player_public_push` | Player `unhappiness.severity > 70` | Transfer news: "[Player] pushing for move." Player's `selling_pressure` on club's `club_transfer_identity` raised to 75. | Until deal or unhappiness resolved |

### Effect Resolution

`ProcessMediaPressure()` runs daily:
1. Check `expires_date`. If `currentDate > expires_date` AND `fired = 1`: decay effect (restore morale/confidence at +2/day until baseline).
2. Active events apply their modifiers continuously.
3. `board_confidence` changes are written to `club_finances.board_confidence`.
4. `squad_harmony` changes are written to `squad_harmony` table.

---

## What This Is NOT (Deferred)

- Transfer fee installments / add-on clauses (e.g. "£5m after 20 appearances")
- Agent representation (user hires an agent to negotiate on their behalf)
- Squad registration limits
- International transfer window variations (some leagues have different windows)
- Tribunal fees for out-of-contract signings
- Knowledge evolution via staff over full seasons (still static from Phase 1+2 seed)
- Formation / tactical fit scoring in acceptance engine
