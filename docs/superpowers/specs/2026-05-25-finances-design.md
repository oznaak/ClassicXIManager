# Finance System Design
**Date:** 2026-05-25
**Branch:** CloneManager
**Status:** Approved — ready for implementation planning

---

## 1. Core Philosophy

Simulate financial **behavior**, not audited accounting. The system produces emergent football-like outcomes over many seasons without requiring real-world data beyond `transfer_budget` and prestige values already in the database.

Guiding rules:
- Believable outcomes over exact accounting realism
- Probabilistic systems and hidden modifiers
- Structural inequality between clubs is permanent and intentional
- AI clubs must not optimize perfectly — irrationality is baked in
- The player infers financial health from observed behavior, not transparent dashboards
- Most financial state is hidden; only `transfer_budget`, `wage spending`, and limited debt/board hints are exposed

**Out of scope for v1:** taxes, amortization, inflation, shareholder simulation, FFP, currency systems, style drift.

**Core feedback loop (long-term goal):**
```
financial state → transfer behavior → squad quality → performance → finances
```

---

## 2. Financial Variables

### Per-club persistent state (`club_finances` table)

| Variable | Type | Purpose |
|---|---|---|
| `cash_balance` | INTEGER (£) | Operational liquidity. Rises from TV/commercial, falls from wages/operating. Can go negative — triggers debt spiral. |
| `wage_budget` | INTEGER (£/wk) | Board-set weekly wage ceiling. Exceeding it costs board confidence. Regenerated each season from income. |
| `transfer_budget` | INTEGER (£) | Season allowance for buying players. Season 1 = real-world input. Season 2+ = generated output from the annual cycle. |
| `board_confidence` | INTEGER 0–100 | Board trust in club direction. High = generous budgets. Low = tight purse, forced sales, crisis. |
| `commercial_strength` | REAL 0.0–1.0 | Slowly-evolving hidden modifier for sponsorship, merchandise, commercial deals. Seeded at career start; drifts ±0.005–0.02 per season based on league position. Strong inertia preserves structural inequality. |
| `style_seed` | INTEGER | Immutable deterministic seed for the entire save. Generates the club's financial archetype and all variance rolls. Never stored as a label. |
| `debt_level` | INTEGER (£) | Outstanding obligations. Serviced weekly. Compounds if cash goes negative. Hard ceiling enforced. |
| `institutional_power` | REAL 0.0–1.0 | Hidden permanent modifier derived from prestige + commercial_strength. Grants resilience: softer collapse penalties, better loan access, faster sponsor recovery. |
| `shock_cooldown` | INTEGER | Seasons remaining until next shock is eligible. Prevents shock stacking. |
| `commercial_shock_mult` | REAL | One-season multiplier on commercial income (1.0 = normal). Set by shock events, reset to 1.0 after one annual cycle. |
| `bad_contract_weeks` | INTEGER | Countdown of weeks remaining on a locked bad contract wage penalty. Decremented weekly. |
| `transfer_budget_frozen` | INTEGER 0/1 | If 1, transfer budget is blocked this season (financial investigation shock). Reset at season end. |
| `emergency_credit_used` | INTEGER 0/1 | If 1, the one-time emergency board loan has already been extended. Cannot fire again. |
| `last_weekly_date` | TEXT | Date of last weekly tick. Guards against double-processing. |
| `season_budget_processed` | INTEGER | Season year of last annual cycle run. Guards against double-processing. |
| `season_prize_paid` | INTEGER | Season year of last prize payment. Existing guard, retained. |

---

## 3. Database Schema

### `club_finances` (replaces `career_finances`)

```sql
CREATE TABLE club_finances (
  manager_id              INTEGER NOT NULL,
  club_id                 INTEGER NOT NULL,
  cash_balance            INTEGER DEFAULT 0,
  wage_budget             INTEGER DEFAULT 0,
  transfer_budget         INTEGER DEFAULT 0,
  board_confidence        INTEGER DEFAULT 50,
  commercial_strength     REAL    DEFAULT 0.5,
  institutional_power     REAL    DEFAULT 0.3,
  style_seed              INTEGER DEFAULT 0,
  debt_level              INTEGER DEFAULT 0,
  shock_cooldown          INTEGER DEFAULT 0,
  commercial_shock_mult   REAL    DEFAULT 1.0,
  bad_contract_weeks      INTEGER DEFAULT 0,
  transfer_budget_frozen  INTEGER DEFAULT 0,
  emergency_credit_used   INTEGER DEFAULT 0,
  wage_overrun_weeks      INTEGER DEFAULT 0,   -- cumulative weeks this season over wage_budget
  last_weekly_date        TEXT,
  season_budget_processed INTEGER DEFAULT 0,
  season_prize_paid       INTEGER DEFAULT 0,
  PRIMARY KEY (manager_id, club_id)
);
```

### `finance_transactions` (extended — player's club only)

```sql
CREATE TABLE finance_transactions (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  manager_id  INTEGER NOT NULL,
  club_id     INTEGER NOT NULL,
  date        TEXT    NOT NULL,
  category    TEXT    NOT NULL,
  description TEXT,
  amount      INTEGER NOT NULL
);
```

AI clubs update `club_finances` balance directly — no transaction log, to avoid DB bloat with 50+ clubs.

---

## 4. Initialization (Career Start — One Pass, All Clubs)

Runs once when a new career is created. Seeds every club in `teams` using only data already available.

### Step 1 — Compute `club_factor` (0.0–1.0)

```
prestige_factor = (intl_prestige * 0.6 + dom_prestige * 0.4) / 10.0
budget_factor   = club.transfer_budget / max_transfer_budget_across_all_clubs
club_factor     = budget_factor * 0.70 + prestige_factor * 0.30
```

Prestige pulls up clubs with strong reputation despite modest budgets (e.g. Benfica).

### Step 2 — Generate `style_seed` (immutable)

```
style_seed = hash(club_id * 31337 + career_start_timestamp)
```

Stable for the entire save. Unique per career. All variance rolls derive from it.

### Step 3 — Derive financial archetype (never stored)

```
archetype = style_seed % 6
```

| # | Archetype | Personality summary |
|---|---|---|
| 0 | Conservative | Saves cash, low wage tolerance, avoids risk |
| 1 | Aggressive | Spends freely, high wages, low reserves |
| 2 | Gambling | Overspends, takes on debt, boom/bust prone |
| 3 | Youth-focused | Low wages, low spend, high player sales (aging players) |
| 4 | Selling | Systematically sells players, maintains healthy cash |
| 5 | Star-focused | Concentrates budget on 2–3 high earners, ignores depth |

### Step 4 — `commercial_strength` (seeded, slowly evolving)

```
-- Initialization (career start only):
base_commercial     = club_factor * 0.70
style_variance      = ((style_seed >> 8) % 30) / 100.0   -- ±15%
commercial_strength = clamp(base_commercial + style_variance, 0.05, 1.0)

-- Annual update (each season end, inside ProcessAnnualCycle):
position_score      = 1.0 - ((position - 1) / (total_clubs - 1))  -- 1.0=1st, 0.0=last
performance_factor  = position_score * 0.5                          -- [0, 0.5]
drift               = (performance_factor - 0.25) * 0.06            -- ±0.015 typical, ±0.03 max

-- Soft saturation near ceiling (top clubs gain very slowly above 0.80)
if drift > 0 and commercial_strength > 0.80:
    drift *= (1.0 - commercial_strength) * 5.0

-- Soft floor protection (small clubs lose brand very slowly below 0.20)
if drift < 0 and commercial_strength < 0.20:
    drift *= commercial_strength * 5.0

inertia             = 0.97
commercial_strength = clamp(old * inertia + drift, 0.05, 1.0)
```

Structural inequality is preserved through seed differences, high inertia, and the asymmetric soft ceiling/floor. No shock-based resets. Change per season: ±0.005–0.02 typical, ±0.03 maximum.

### Step 5 — `institutional_power` (permanent)

```
prestige_score      = (intl_prestige * 0.5 + dom_prestige * 0.3) / 8.0
institutional_power = clamp(prestige_score * 0.6 + commercial_strength * 0.4, 0.05, 1.0)
```

Effects:
- Emergency loan size scales with `institutional_power`
- Board confidence penalties from shocks are reduced by `institutional_power * 0.4`
- Sponsor recovery (commercial_shock_mult returning to 1.0) is faster for high-IP clubs
- Shock bad outcomes have their severity multiplied by `(1.0 - institutional_power * 0.35)`

Large clubs survive crises more easily. Small clubs hit harder.

### Step 6 — `cash_balance`

```
league_range   = LeagueFP.maxBalance - LeagueFP.minBalance
base_cash      = LeagueFP.minBalance + (league_range * club_factor)

style_cash_mult = {
  conservative: 1.25, selling: 1.10, youth: 1.00,
  aggressive:   0.85, star:    0.75, gambling: 0.65
}

variance_pct   = ((style_seed >> 4) % 21 - 10) / 100.0   -- -10% to +10%
cash_balance   = (long long)(base_cash * style_cash_mult[archetype] * (1.0 + variance_pct))
```

### Step 7 — `wage_budget`

```
est_annual_income = LeagueFP.weeklyTV * 52 * (0.85 + club_factor * 0.30)
                  + (LeagueFP.weeklyTV * 12 * commercial_strength)

wage_ratio = {
  conservative: 0.52, youth: 0.45, selling: 0.48,
  aggressive:   0.68, star:  0.72, gambling: 0.80
}

wage_budget = (long long)((est_annual_income * wage_ratio[archetype]) / 52.0)
```

### Step 8 — `transfer_budget` (season 1 only)

```
transfer_budget = teams.transfer_budget   -- real-world input, season 1 only
```

From season 2 onward it is a generated output of the annual budget cycle.

### Step 9 — `board_confidence`

```
prestige_bonus   = (int)(prestige_factor * 15.0)    -- 0 to +15
variance         = (style_seed % 11) - 5            -- -5 to +5
board_confidence = clamp(50 + prestige_bonus + variance, 35, 75)
```

### Step 10 — `debt_level`, `shock_cooldown`

Both initialized to 0. No debt, no cooldown at career start.

---

## 5. Financial Style Runtime Struct

Re-derived from `style_seed` at runtime wherever financial decisions are made. Never stored as a label.

```cpp
struct FinancialStyle {
    float wage_tolerance;       // wage_budget ceiling as fraction of est. annual income
    float transfer_spend_bias;  // fraction of transfer_budget actually deployed per season
    float commercial_variance;  // annual commercial income swing ±
    float selling_bias;         // probability any player is listed per window
    float risk_appetite;        // probability of taking on debt to complete a signing
    float saving_rate;          // fraction of season surplus retained as cash reserves
    float shock_vulnerability;  // multiplier on bad shock probability
    float shock_opportunity;    // multiplier on good shock probability
    int   sell_threshold;       // board_confidence floor triggering emergency sales
};

static FinancialStyle DeriveStyle(int style_seed) {
    switch (style_seed % 6) {
        case 0: // Conservative
            return { 0.52f, 0.55f, 0.08f, 0.10f, 0.05f, 0.70f, 0.60f, 1.20f, 25 };
        case 1: // Aggressive
            return { 0.68f, 0.85f, 0.15f, 0.15f, 0.35f, 0.25f, 1.10f, 0.90f, 20 };
        case 2: // Gambling
            return { 0.80f, 1.00f, 0.25f, 0.10f, 0.60f, 0.05f, 1.80f, 0.70f, 15 };
        case 3: // Youth-focused
            return { 0.45f, 0.40f, 0.10f, 0.35f, 0.08f, 0.55f, 0.75f, 1.40f, 30 };
        case 4: // Selling
            return { 0.48f, 0.25f, 0.08f, 0.65f, 0.05f, 0.60f, 0.50f, 1.10f, 40 };
        case 5: // Star-focused
            return { 0.72f, 0.70f, 0.18f, 0.20f, 0.30f, 0.20f, 1.20f, 0.85f, 22 };
    }
}
```

---

## 6. Weekly Operational Tick (All Clubs)

Fires once per in-game week, for every club. Runs after the player's club is processed.

### Per-club sequence

1. Compute TV income (league base × quality × commercial_strength)
2. Compute wage expense (batch query: `SELECT team_id, SUM(weekly_wage) FROM players GROUP BY team_id`)
3. Compute operating cost (league fixed)
4. Compute matchday income (if home fixture played this week)
5. Apply net to `cash_balance`
6. Deduct debt installment (if `debt_level > 0`)
7. Handle insolvency (if `cash_balance < 0`)
8. Check wage overrun → update `board_confidence`
9. Decrement `bad_contract_weeks`
10. Write back to DB

### Formulas

```cpp
float qual = CalcClubQualityFactor(clubPlayers);  // existing function

long long tv = (long long)(lfp->weeklyTV
             * (0.85f + qual * 0.30f)
             * (0.80f + cf.commercial_strength * 0.40f));

long long wages     = wageMap[club_id];   // from batch query
long long operating = lfp->weeklyOperating;

long long matchday = 0;
if (hadHomeFixture) {
    float md_qual = 0.50f + qual * 0.50f;
    matchday = (long long)(lfp->matchdayHome
              * (0.25f + md_qual * 1.00f)
              * (0.70f + cf.commercial_strength * 0.60f));
}

long long net = tv - wages - operating + matchday;
cf.cash_balance += net;
```

### Debt servicing

```cpp
if (cf.debt_level > 0) {
    long long installment = std::max(1000LL, cf.debt_level / 50);
    cf.cash_balance -= installment;
    cf.debt_level   -= installment;
    if (cf.debt_level < 0) cf.debt_level = 0;
}
```

### Insolvency handling

```cpp
if (cf.cash_balance < 0) {
    long long shortfall = -cf.cash_balance;

    // Check hard debt ceiling first
    long long max_debt = (long long)(est_annual_income * 2.5f);
    if (cf.debt_level + shortfall > max_debt) {
        // Debt ceiling breached — hard crisis (see Section 8)
        TriggerDebtCrisis(cf);
    } else {
        cf.debt_level   += shortfall;
        cf.cash_balance  = 0;
        cf.board_confidence = std::max(0, cf.board_confidence - 3);
    }
}
```

### Wage overrun

```cpp
if (wages > cf.wage_budget) {
    cf.wage_overrun_weeks++;
    int overrun_pct = (int)(((float)(wages - cf.wage_budget) / cf.wage_budget) * 100.0f);
    int penalty     = 1 + (overrun_pct / 15);   // 1–4 pts/wk depending on severity
    // institutional_power softens the penalty for large clubs
    penalty = (int)(penalty * (1.0f - cf.institutional_power * 0.30f));
    cf.board_confidence = std::max(0, cf.board_confidence - penalty);
} else {
    // Reset counter at season boundary (handled in annual cycle)
}
```

### Club quality factor for AI clubs

`CalcClubQualityFactor` currently only runs for the player's club. For AI clubs, approximate it with a pre-computed lookup using the same batch player query:

```cpp
// After batch wage query, also batch avg base_stat per team
// SELECT team_id, AVG(base_stat) FROM players WHERE team_id IN (...) GROUP BY team_id
// Map avg_stat (roughly 50–90) to 0.0–1.0: qual = (avg_stat - 50.0f) / 40.0f
float qual = std::clamp((avgStat - 50.0f) / 40.0f, 0.0f, 1.0f);
```

Two batch queries replace N per-club queries.

### Batch wage query (performance — one query for all AI clubs)

```sql
SELECT team_id, SUM(weekly_wage)
FROM players
WHERE team_id IN (SELECT club_id FROM club_finances WHERE manager_id = ?)
GROUP BY team_id;
```

Results stored in `std::map<int, long long>` and consumed during the per-club loop.

### DB write strategy

| Club | Written fields | Transaction log |
|---|---|---|
| Player's club | All fields + full line-item transactions | Yes |
| AI clubs | `cash_balance`, `board_confidence`, `debt_level`, `bad_contract_weeks` | No |

---

## 7. Annual Budget Cycle (Season End, All Clubs)

Runs once per club when `hasSeasonEnded == true` and `season_budget_processed != season_year`.

### Step 1 — Revenue resolution

```cpp
int   rng_seed = cf.style_seed ^ season_year;
float rng01    = SeededRand(rng_seed, 0);   // deterministic 0.0–1.0

// Commercial (sponsorships, kit deals, naming rights)
float comm_variance = 1.0f + (rng01 * 2.0f - 1.0f) * style.commercial_variance;
comm_variance      *= cf.commercial_shock_mult;   // shock modifier (default 1.0)
long long commercial = (long long)(lfp->weeklyTV * 12.0f
                     * cf.commercial_strength * comm_variance);

// Sponsorship (board confidence signals attractiveness)
float conf_factor   = 0.70f + (cf.board_confidence / 100.0f) * 0.60f;
float rng02         = SeededRand(rng_seed, 1);
long long sponsorship = (long long)(lfp->weeklyTV * 8.0f
                      * cf.commercial_strength * conf_factor
                      * (0.90f + rng02 * 0.20f));

// Merchandise (domestic prestige + position proxy)
float merch_base    = (dom_prestige / 10.0f) * 0.8f + 0.2f;
float rng03         = SeededRand(rng_seed, 2);
long long merchandise = (long long)(lfp->weeklyTV * 4.0f
                       * merch_base * (0.85f + rng03 * 0.30f));

// Player sales (AI clubs — actual transfer market hooks later)
long long player_sales = 0;
float rng04 = SeededRand(rng_seed, 3);
if (rng04 < style.selling_bias * 0.6f) {
    player_sales = (long long)(cf.transfer_budget * (0.20f + rng04 * 0.40f));
}

// Reset shock multiplier after consumption
cf.commercial_shock_mult = 1.0f;
```

### Step 2 — Annual expenses (non-weekly)

```cpp
long long facilities_cost = (long long)(lfp->weeklyOperating * 8.0f);
long long youth_cost      = (long long)(lfp->weeklyOperating
                          * (archetype == YOUTH_FOCUSED ? 6.0f : 2.0f));
long long bonus_cost      = (league_position <= 3)
                          ? (long long)(cf.wage_budget * 4.0f) : 0LL;

long long annual_income  = commercial + sponsorship + merchandise
                         + player_sales + season_prize_money;
long long annual_expense = facilities_cost + youth_cost + bonus_cost;
long long season_profit  = annual_income - annual_expense;
```

### Step 3 — Board confidence update

```cpp
// Performance component
int expected_pos  = (int)((1.0f - club_factor) * total_clubs) + 1;
int position_delta = expected_pos - actual_position;   // positive = overperformed

int perf_delta;
if      (position_delta >=  3) perf_delta = +12;
else if (position_delta >=  1) perf_delta = +6;
else if (position_delta ==  0) perf_delta = +2;
else if (position_delta >= -2) perf_delta = -8;
else                           perf_delta = -18;

// institutional_power softens negative performance penalties
if (perf_delta < 0)
    perf_delta = (int)(perf_delta * (1.0f - cf.institutional_power * 0.40f));

// Financial health component
int fin_delta = 0;
if (season_profit > 0)                                    fin_delta += 5;
if (cf.cash_balance > lfp->minBalance)                    fin_delta += 3;
if (cf.debt_level > (long long)(annual_income * 0.40f))   fin_delta -= 12;
if (wages_overrun_weeks > 8)                              fin_delta -= 10;
if (cf.cash_balance < 0)                                  fin_delta -= 15;

// Annual confidence decay (raised expectations / complacency)
int decay = -1;

cf.board_confidence = std::clamp(
    cf.board_confidence + perf_delta + fin_delta + decay, 0, 100);
```

### Step 4 — Next season transfer budget (generated output)

```cpp
long long rollover    = (long long)(cf.transfer_budget * 0.50f);
float conf_factor_t   = (cf.board_confidence - 50) / 50.0f;   // -1.0 to +1.0
long long injection   = (long long)(std::max(0LL, cf.cash_balance)
                      * (0.15f + conf_factor_t * 0.20f));
long long prize_slice = (long long)(season_prize_money * 0.25f);

float style_mult = 0.70f + style.transfer_spend_bias * 0.60f;
float conf_mult  = 1.00f + conf_factor_t * 0.35f;
float rng_t      = SeededRand(rng_seed, 4);
float variance   = 0.90f + rng_t * 0.20f;

long long raw_budget = (long long)((rollover + injection + prize_slice)
                     * style_mult * conf_mult * variance);

long long floor_budget = lfp->minBalance / 4LL;
long long ceil_budget  = (long long)(lfp->maxBalance * 1.20f);

if (cf.transfer_budget_frozen) {
    cf.transfer_budget         = 0LL;
    cf.transfer_budget_frozen  = 0;
} else {
    cf.transfer_budget = std::clamp(raw_budget, floor_budget, ceil_budget);
}
```

### Step 5 — Next season wage budget

```cpp
long long est_annual = (long long)(lfp->weeklyTV * 52.0f
                     * (0.85f + club_factor * 0.30f))
                     + commercial + sponsorship;

cf.wage_budget = (long long)((est_annual * style.wage_tolerance) / 52.0f);
```

### Step 6 — Cash retention

```cpp
if (season_profit > 0) {
    long long banked = (long long)(season_profit * style.saving_rate);
    cf.cash_balance += banked;
}
```

### Step 7 — Shock cooldown tick

```cpp
if (cf.shock_cooldown > 0) cf.shock_cooldown--;
```

### Step 8 — Reset seasonal counters and mark processed

```cpp
cf.wage_overrun_weeks      = 0;   // reset for new season
cf.season_budget_processed = season_year;
```

---

## 8. Debt Ceiling & Crisis System

### Hard debt ceiling

```cpp
long long max_debt = (long long)(est_annual_income * 2.5f);
```

When `debt_level` would exceed `max_debt`:

```cpp
void TriggerDebtCrisis(ClubFinances &cf) {
    // 1. Transfer embargo
    cf.transfer_budget_frozen = 1;

    // 2. Forced player sales (hooks into transfer system later)
    cf.emergency_sale_flag = true;

    // 3. Board confidence penalty (softened by institutional_power)
    int penalty = (int)(25.0f * (1.0f - cf.institutional_power * 0.40f));
    cf.board_confidence = std::max(0, cf.board_confidence - penalty);

    // 4. Emergency board intervention (if not already used)
    if (!cf.emergency_credit_used) {
        long long credit = (long long)(lfp->minBalance
                         * (0.5f + cf.institutional_power * 1.0f));
        cf.cash_balance        += credit;
        cf.debt_level          += credit;
        cf.emergency_credit_used = 1;
        cf.shock_cooldown        = 2;
    } else {
        // Second crisis: no safety net — debt stays at ceiling, weekly cash fully consumed by servicing
        cf.debt_level = max_debt;
    }
}
```

Large clubs (`institutional_power` ≈ 0.9) receive a larger credit line and a softer confidence penalty. Small clubs get minimal credit and a harsh board reaction. Gambling clubs can hit the ceiling twice if the first crisis doesn't force enough structural change — but the second time there is no board loan.

---

## 9. Shock System

### Eligibility and frequency

```cpp
float base_shock_prob = 0.15f;   // 12–18% target range
float roll            = SeededRand(cf.style_seed ^ (season_year * 1031), 5);
bool  shock_fires     = (cf.shock_cooldown == 0)
                     && (roll < base_shock_prob * style.shock_vulnerability);
```

At base (1.0× vulnerability, conservative): ~9% per season.
Gambling (1.80× vulnerability): ~27% per season.
Average across all styles: ~15% per season.

### Bad vs good selection

```cpp
float bad_vs_good   = SeededRand(cf.style_seed ^ season_year, 6);
float bad_threshold = 0.62f / style.shock_opportunity;
bool  is_bad        = (bad_vs_good < bad_threshold);
```

### Bad shocks

| ID | Name | Condition | Effect |
|---|---|---|---|
| 0 | Sponsorship collapse | any | `commercial_shock_mult` = 0.45–0.65 for one season |
| 1 | Bad contract | aggressive/gambling/star | `debt_level` += 15–25% of annual wages; `bad_contract_weeks` = 104 |
| 2 | Ownership crisis | `board_confidence` < 45 | `board_confidence` –20; `transfer_budget` halved |
| 3 | Stadium emergency | `dom_prestige` < 5 | `cash_balance` –8–15% |
| 4 | Wage revolt | wage overrun > 12 weeks | `board_confidence` –25; emergency sales forced |
| 5 | Financial investigation | gambling style | `transfer_budget_frozen` = 1; `debt_level` += fine |

All bad shock penalties are reduced by `institutional_power * 0.35` (severity multiplier). Large clubs hit softer.

### Good shocks

| ID | Name | Condition | Effect |
|---|---|---|---|
| 0 | Sponsorship windfall | `commercial_strength` > 0.5 | `commercial_shock_mult` = 1.30–1.50 for one season |
| 1 | Ownership injection | `board_confidence` > 65 | `cash_balance` += 15–40% of `transfer_budget` |
| 2 | Stadium naming rights | `dom_prestige` >= 7 | `cash_balance` += league tier prize equivalent |
| 3 | Youth breakthrough | youth-focused style | `transfer_budget` += 5–20% |
| 4 | European windfall | `intl_prestige` >= 7 | `cash_balance` += prize proxy |

### Shock cooldown

All shocks set `shock_cooldown` to 2–4 seasons depending on severity. This caps how frequently any single club can be disrupted.

---

## 10. Long-Term Stability Mechanisms

| Problem | Mechanism |
|---|---|
| Infinite money inflation | Transfer budget ceiling per league tier (`maxBalance * 1.20`) |
| Cash hoarding dominance | Cash earns nothing; board recycles surplus via annual injection |
| All clubs becoming rich | Prize money is zero-sum within league |
| Clubs mathematically immortal | Hard debt ceiling; second crisis has no safety net |
| Perfect AI optimization | Style-baked irrationality (gambling spends 100%, aggressive overpays) |
| Dead stable equilibrium | Mandatory commercial variance; annual confidence decay –1; shock system |
| Shock stacking | `shock_cooldown` prevents consecutive disruptions |
| Floor collapse | `transfer_budget` floor = `minBalance / 4` |
| Large club invincibility | `institutional_power` softens but does not eliminate penalties |

---

## 11. Player-Facing Exposure

Only these signals are visible to the player:

| Visible | Where |
|---|---|
| `transfer_budget` | Finances screen, transfer market |
| Weekly wage spending vs wage budget | Finances screen (gauge only, no raw numbers for AI clubs) |
| `board_confidence` level (low/medium/high/critical) | Inbox messages, board status hints |
| Debt hint ("Club carrying financial obligations") | Finances screen, board messages |
| Emergency sale news | Inbox — "Club X has listed Player Y" |
| Transfer embargo news | Transfer screen — "Club X not accepting bids this window" |

Hidden from player: `style_seed`, `commercial_strength`, `institutional_power`, archetype label, exact `debt_level` for AI clubs, `shock_cooldown`, all multipliers.

---

## 12. Future Integration Hooks (AI Transfers)

When AI transfers are implemented, `club_finances` feeds directly into transfer decision-making:

```
can_buy(club, player) →
    transfer_budget_frozen == 0
    AND transfer_budget >= asking_price * style.risk_appetite_premium
    AND (cash_balance > 0 OR risk_appetite > rand())

willing_to_sell(club, player) →
    selling_bias > rand()
    OR board_confidence < sell_threshold
    OR emergency_sale_flag == true

asking_price_modifier(club) →
    selling club:       0.85× (undervalues players)
    gambling club:      1.00× (takes what it can get)
    conservative club:  1.15× (holds out)
    institutional_power uplift: +0–10% for large clubs (know their worth)
```

The feedback loop closes: financial state → transfer behavior → squad quality (via CalcClubQualityFactor) → performance → standings position → annual budget cycle → financial state.

---

## 13. Balancing Notes

- **Shock frequency at 15% base** produces roughly one shock every 6–7 seasons for a conservative club, every 3–4 seasons for a gambling club. Long periods of stability with occasional disruption.
- **Debt ceiling at 2.5× annual income** mirrors rough real-world distress thresholds. A PL club earning ~£150M/yr hits ceiling at £375M debt — reachable in 3–4 seasons of sustained overspending.
- **Board confidence decay of –1/season** means a club must finish at or above expected position every 3 seasons just to stay neutral. Underperforming two seasons in a row produces meaningful tightening within 4–5 seasons.
- **institutional_power** should be tuned so that a top club (IP ≈ 0.9) receives roughly 35–40% softer penalties, not immunity. Crises should still be painful — just survivable.
- **Do not tune for exact real-world club outcomes.** The system should produce plausible ranges across many careers, not reproduce specific club trajectories.
