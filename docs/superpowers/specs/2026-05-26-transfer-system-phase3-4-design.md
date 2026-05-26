# Transfer System Phase 3+4 Design

> **Scope:** User-facing negotiation experience (buy-side, sell-side), full loan system (fee, wage split, recall clause, option-to-buy), playing time and promise fulfillment tracking, and special scripted events (galactico, financial collapse, wonderkid explosion). Builds directly on top of the Phase 1+2 engine.

---

## Philosophy

Phase 3+4 makes the transfer market player-facing. All user interactions follow the same async pattern the AI engine uses internally — offers take days to resolve, counteroffers arrive in the inbox, and deals can collapse at any stage. The player is not isolated from the market simulation; they participate in it using the same rules.

---

## Architecture

### New Files

- `src/menu/user_transfer.hpp` — declares `InitiateUserBid()`, `RespondToOffer()`, `OfferLoan()`, `RespondToLoanOffer()`, `TickUserNegotiations()`, `SeedSpecialEvents()`, `ProcessSpecialEvents()`
- `src/menu/user_transfer.cpp` — implements all of the above

### Modified Files

- `src/menu/transfer_engine.cpp` — `AdvanceNegotiations()` skips rows with `is_user_bid = 1`; `ProcessDailyTransfers()` calls `TickUserNegotiations()` and `ProcessSpecialEvents()`
- `src/menu/managercareer.cpp` — `GenerateCareerSeason()` calls `SeedSpecialEvents()`; `AdvanceDay()` calls appearance tracking after fixture completion
- `src/menu/imgui_career.cpp` — new primary tab (index 6) for transfer market UI; `DrawTransfersCard()` already wired to `transfer_news`
- `sources.cmake` — register `user_transfer.hpp` and `user_transfer.cpp`

### Modified Tables

**`transfer_negotiations`** — add one column:
```sql
ALTER TABLE transfer_negotiations ADD COLUMN is_user_bid INTEGER DEFAULT 0;
ALTER TABLE transfer_negotiations ADD COLUMN user_pending_action TEXT DEFAULT '';
-- user_pending_action: 'accept_counter'|'reject'|'counter'|'' — set by inbox button press
```

### New Tables

```sql
CREATE TABLE IF NOT EXISTS loan_deals (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  manager_id          INTEGER NOT NULL,
  loaning_club_id     INTEGER NOT NULL,
  receiving_club_id   INTEGER NOT NULL,
  player_id           INTEGER NOT NULL,
  loan_fee            INTEGER DEFAULT 0,
  wage_split_pct      INTEGER DEFAULT 100, -- % covered by loaning club
  recall_clause_after_month INTEGER DEFAULT 0, -- 0 = no recall
  option_to_buy_fee   INTEGER DEFAULT 0,   -- 0 = no option
  option_to_buy_deadline TEXT,
  season_year         INTEGER NOT NULL,
  status              TEXT DEFAULT 'active', -- active/recalled/expired/option_exercised
  created_date        TEXT,
  end_date            TEXT
);

CREATE TABLE IF NOT EXISTS player_appearances (
  manager_id    INTEGER NOT NULL,
  player_id     INTEGER NOT NULL,
  season_year   INTEGER NOT NULL,
  starts        INTEGER DEFAULT 0,
  sub_appearances INTEGER DEFAULT 0,
  PRIMARY KEY (manager_id, player_id, season_year)
);

CREATE TABLE IF NOT EXISTS transfer_special_events (
  id           INTEGER PRIMARY KEY AUTOINCREMENT,
  manager_id   INTEGER NOT NULL,
  event_type   TEXT NOT NULL,  -- 'galactico'|'collapse_sale'|'wonderkid_explosion'
  player_id    INTEGER NOT NULL,
  club_id      INTEGER NOT NULL,
  trigger_date TEXT NOT NULL,
  fired        INTEGER DEFAULT 0,
  metadata_json TEXT DEFAULT '{}'
);
```

---

## Section 1 — Transfer Market UI (New Primary Tab)

New tab at primary tab index 6: **"Transfers"**. Four sub-panels, toggled via a top segmented control:

### "Available" Panel

Shows all players where `is_transfer_listed = 1` OR `player_market_status.status IN ('transfer_listed', 'expiring_soon')`. No scouting required. Columns: name, position group, age, club, value, weekly wage, contract expiry. Filterable by position group (GK / CB / FB_WB / DM / CM / AM_W / ST), sortable by value / age / base_stat.

Each row has two buttons: **Bid** (permanent transfer) and **Loan** (loan offer composer).

### "Scouted" Panel

Shows all players from `scout_reports` where the user has at least one completed report (`manager_id = userManagerId`). Stats shown at `revealPct` from the scout report (blurred/partial if low). User can bid or loan regardless of transfer-listed status.

### "My Bids" Panel

All active user bids: `SELECT * FROM transfer_negotiations WHERE manager_id=? AND buying_club_id=userClubId AND is_user_bid=1 AND state NOT IN ('completed','collapsed')`. Columns: player, selling club, offered fee, current state, days in state.

### "Incoming Bids" Panel

All active AI bids on user's players: `SELECT * FROM transfer_negotiations WHERE manager_id=? AND selling_club_id=userClubId AND is_user_bid=0 AND state NOT IN ('completed','collapsed')`. Columns: player, bidding club, offered fee, current state. Each row has **Accept** / **Reject** / **Counter** buttons. Franchise players also show a **Block** button.

---

## Section 2 — Bid Composer

Clicking **Bid** on any player opens an inline composer (not a modal — renders below the player row or as a sidebar panel):

- **Transfer fee field** — integer input, pre-filled with 80% of `playervalue`
- **Weekly wage field** — integer input, pre-filled with player's current wage × 1.1
- **Role promise dropdown** — `rotation` / `important` / `star_player` / `prospect`
- **Submit** button

On submit: `InitiateUserBid(managerId, userClubId, sellingClubId, playerId, fee, wage, role)` inserts a `transfer_negotiations` row with `is_user_bid = 1`, state `initiated`. Inbox confirmation: "Bid submitted for [Player]. Waiting for [Club] to respond."

---

## Section 3 — Buy-Side Negotiation Flow (Async Inbox)

`AvanceNegotiations()` **skips** rows where `is_user_bid = 1`. Instead, `TickUserNegotiations()` in `user_transfer.cpp` runs daily inside `ProcessDailyTransfers()`.

### State machine for user bids

**`initiated` → after 1–2 days:**
Selling club responds. Outcome determined by `CalculateContextualValue()` vs offered fee and selling club's `selling_pressure`:
- Fee ≥ 90% of contextual value OR selling_pressure > 60 → **accept fee** → state `player_talks`. Inbox: "[Club] have accepted your £Xm bid for [Player]. Personal terms now being negotiated."
- Fee 60–89% → **counter fee** → state `counter_offer`. Inbox: "[Club] want £Xm for [Player]." with **Accept** / **Reject** / **Counter** inline buttons.
- Fee < 60% OR player is `franchise_player` with seller loyalty > 70 → **reject** → state `collapsed`. Inbox: "[Club] rejected your bid. [Reason]."

**`player_talks` → 3 days:**
`CalculateAcceptanceScore()` computed once. Outcome:
- Score > 65 → state `medical_pending`. Inbox: "[Player] is happy to join. Medical arranged."
- Score 40–65 → inbox: "[Player] wants improved terms." Buttons: **Improve Wage +10%** / **Improve Wage +20%** / **Improve Role Promise** / **Walk Away**. User picks one; score recalculated. If still < 40 after 2 rounds → `collapsed`.
- Score < 40 → `collapsed`. Inbox: "[Player] has rejected the move."

**`counter_offer` (user receives counter):**
Inbox message contains **Accept Counter** / **Reject** / **Make Counter** buttons. User must respond within 3 in-game days or deal auto-collapses. "Make Counter" opens a fee input inline. Buying counter goes back to seller; seller responds in 1–2 days. Maximum 4 counter rounds before auto-collapse.

**`medical_pending`:**
2% fail chance (5% deadline day). Success → deal completes: `players.team_id` updated to `userClubId`, fee deducted from `club_finances.transfer_budget`. Inbox: "Done! [Player] joins [YourClub]." Failure → `collapsed`. Inbox: "[Player] failed his medical."

---

## Section 4 — Sell-Side Flow

`AdvanceNegotiations()` detects when a row with `selling_club_id = userClubId AND is_user_bid = 0` transitions from `initiated` to `offer_made`. It pauses auto-advance for that row and fires an inbox message: "**[Club]** have submitted a **£Xm** bid for **[Player]**." Row stays at `offer_made` until user responds — no timeout.

### User response options

**Accept:** Row advances to `negotiating`. AI club negotiates personal terms with the player automatically via `CalculateAcceptanceScore()`. Result arrives as inbox within 1–3 days.
- Player accepts → `medical_pending` → deal completes: `players.team_id` updated to buyer's club, fee credited to user's `club_finances.balance`. Inbox: "[Player] completes move to [Club] for £Xm."
- Player rejects → `collapsed`. Inbox: "[Player] decided to stay."

**Reject:** State `collapsed`. Buyer gets 14-day cooldown. Inbox: "You rejected [Club]'s bid for [Player]."

**Counter:** User sets asking price. State stays `offer_made`. Buyer responds in 1–2 days (aggression / negotiation_personality driven).

**Block (franchise players only):** Player marked not-for-sale. `negotiation_cooldowns` row inserted with 90-day cooldown for that buyer. Inbox: "You have blocked [Club]'s approach for [Player]."

---

## Section 5 — Loan System

### User loans out a player

From the squad screen (or Scouted/Available panel), a **Loan Out** button opens the loan composer:
- **Receiving club** — dropdown of clubs with `club_player_knowledge.knowledge ≥ 30` for this player
- **Loan fee** — integer input (default £0)
- **Wage split %** — slider 0–100 (% your club covers; default 50)
- **Recall clause** — optional month picker (1–10; 0 = no recall)
- **Option to buy** — toggle; if on, fee input + deadline picker

On submit: `OfferLoan()` sends offer to selected AI club. AI responds in 1–2 days based on `youth_focus` and squad need for that position group. Inbox: "[Club] have agreed to take [Player] on loan." or "[Club] declined."

### User receives a loan offer (AI-initiated)

`ProcessDailyTransfers()` — separate from permanent transfers, lower need threshold (score > 25 triggers loan approach). Arrives as inbox with offered terms. **Accept** / **Reject** / **Counter** buttons. Counter opens a loan counter-composer.

### User takes a player on loan

Same market browser panels. **Loan** button opens loan offer composer:
- Loan fee + wage split % (receiving club covers this %)
- Recall clause toggle (gives the seller the right to recall)
- Option-to-buy toggle with fee + deadline

Same async inbox flow as permanent bids. Resolved via `loan_deals` row rather than `transfer_negotiations`.

### Recall clause

On the 1st of the trigger month, `ProcessDailyTransfers()` checks `loan_deals` for `recall_clause_after_month = currentMonth AND status = 'active'`. If the loaning club is the user: inbox "Your recall window for [Player] is open. Recall now?" with **Recall** / **Let run** buttons. Decision window: 7 days; if no response, loan continues. If recalled: `players.team_id` reverted, `loan_deals.status = 'recalled'`.

If the loaning club is AI: `ProcessDailyTransfers()` auto-decides based on their squad needs. If need score for that position > 60 → recall. Inbox notification: "[Club] have recalled [Player]."

### Option to buy

`ProcessDailyTransfers()` checks `loan_deals` where `option_to_buy_fee > 0 AND option_to_buy_deadline = AddDays(currentDate, 14)`. Fires inbox: "[Player]'s option-to-buy expires in 14 days. Exercise for £Xm?" **Exercise** / **Let expire** buttons. Exercise → permanent transfer completes immediately, `loan_deals.status = 'option_exercised'`, `players.team_id` updated.

### Season end

`ProcessAnnualCycle()` expires all `loan_deals` with `status = 'active'`, reverts `players.team_id` to `loaning_club_id`, inserts transfer news "Loan deal expires — [Player] returns to [Club]."

---

## Section 6 — Playing Time and Promise Fulfillment

### Tracking appearances

After every completed fixture, `AdvanceDay()` calls `TrackPlayerAppearances(managerId, fixtureId)` in `user_transfer.cpp`. This reads the user's starting XI (from the formation/lineup data already stored per fixture) and increments `player_appearances.starts` for each starter. Named substitutes increment `sub_appearances`. Row is `INSERT OR IGNORE` then `UPDATE`.

### Promise evaluation (monthly, 1st of month)

`ProcessDailyTransfers()` calls `EvaluatePromiseFulfillment(managerId, currentDate)` on the 1st of each month. For every user's player with a `promised_role` on their completed transfer:

Query `player_appearances` for this season. Count fixtures the user has played (`SELECT COUNT(*) FROM fixtures WHERE manager_id=? AND (home_team_id=userClubId OR away_team_id=userClubId) AND status='completed'`).

| Promised role | Kept if | Violated if |
|---|---|---|
| `star_player` | `starts ≥ fixtures_played × 0.75` | `starts < fixtures_played × 0.50` |
| `important` | `starts ≥ fixtures_played × 0.50` | `starts < fixtures_played × 0.30` |
| `rotation` | `(starts + sub_appearances) ≥ fixtures_played × 0.40` | appearances < fixtures_played × 0.20 |
| `prospect` | any appearance | 0 appearances after month 3 of season |

**Violation:** `AddUnhappiness(managerId, playerId, 'promise_broken', 20, currentDate)`. Inbox: "[Player] is unhappy — you promised him a [role] role but he has only started X of Y matches." If `player_unhappiness.severity > 80`: player flagged `transfer_listed` in `player_market_status`, agent pressure set to 80 in all their `transfer_negotiations` rows.

---

## Section 7 — Special Events (Background Simulation)

Seeded once per career in `SeedSpecialEvents()` called from `GenerateCareerSeason()`. One of each type per career. Checked daily in `ProcessSpecialEvents()` called from `ProcessDailyTransfers()`.

### Galactico Signing

**Seed conditions:** Buying club has `international_prestige ≥ 8`. Target player has `international_reputation ≥ 4` at a club with `international_prestige ≤ 6`. Trigger date: summer window, day 20–28 (randomised).

**Fire behaviour:** On trigger date, deal completes instantly regardless of any existing negotiation state — bypasses the state machine entirely. `players.team_id` updated. All `transfer_negotiations` rows for this player set to `collapsed`. Transfer news headline: "[Elite Club] complete stunning £Xm signing of [Player] in blockbuster deal." All players in the same position group get `playervalue` bumped 15% globally (hype effect).

### Financial Collapse Forced Sale

**Seed conditions:** A club where `debt_level > transfer_budget × 2.5` at season start. Target: their highest-value player not flagged `franchise_player`. Trigger date: first 7 days of summer or winter window.

**Fire behaviour:** Target player gets `is_transfer_listed = 1`, `player_market_status.status = 'transfer_listed'`, and selling club's `selling_pressure` set to 90 in `club_transfer_identity`. Target `playervalue` reduced by 30%. Transfer news: "[Club] forced to sell [Player] amid financial crisis." Normal AI negotiations pick this up immediately since player is listed and cheap.

### Wonderkid Explosion

**Seed conditions:** Player with `age ≤ 21`, `sofifaPotential ≥ 88`, `player_market_status.status = 'wonderkid'`. Trigger date: either window, day 5–15 of the window.

**Fire behaviour:** `international_reputation` incremented by 1 (capped at 5). `playervalue` increased 40%. `player_market_status.status` updated to `franchise_player`. All `club_player_knowledge` rows for this player where `knowledge > 0` get bumped to `knowledge = MAX(knowledge, 80)`. Transfer news: "[Player] named in national squad — clubs circle the [age]-year-old." AI clubs with now-elevated knowledge begin targeting them immediately.

---

## What This Is NOT (Deferred)

- Transfer fee installments / add-on clauses
- Agent representation (user hires an agent to negotiate on their behalf)
- Squad registration limits
- International transfer window variations
- Tribunal fees for out-of-contract signings
- Knowledge evolution via staff over seasons (still static from Phase 1+2 seed)
