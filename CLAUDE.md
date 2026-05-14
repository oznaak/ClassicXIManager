# CLAUDE.md

## Project

We are building a native desktop football management game by forking `vi3itor/GameplayFootball`.

The goal is not to create a browser game and not to continue the previous TypeScript 2D prototype. The current direction is:

```txt
GameplayFootball C++ engine
+ AI vs AI football matches
+ manager/career database layer
+ Football Manager-style screens
+ later: tactical controls injected into GameplayFootball AI
```

GameplayFootball already provides the valuable part:

```txt
3D pitch
players
animations
ball physics
stadium/camera
CPU AI
match renderer
```

Our job is to turn it into a tactical manager game around that engine.

---

## Environment

Developer machine:

```txt
Arch Linux
CMake 4.x
GCC 16.x
NVIDIA OpenGL
fish shell
```

Build dependencies installed:

```bash
git base-devel cmake pkgconf \
boost boost-libs \
sdl2 sdl2_image sdl2_ttf sdl2_gfx \
openal mesa libglvnd glu sqlite
```

## IMPORTANT: Claude Code cannot compile this project

Claude Code runs inside a container where the Arch Linux packages (SDL2, Boost,
SQLite, etc.) are not available at standard paths. **Do not attempt to run cmake
or make from within Claude Code.** It will always fail.

After every code patch, remind the user to build manually from their Arch terminal.

---

## Full clean build (run from your Arch terminal)

```bash
cd ~/Documents/projects/GameplayFootball

rm -rf build
mkdir -p build
cp -R data/. build/

cmake -S . -B build \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
  -DBoost_NO_BOOST_CMAKE=ON \
  -DCMAKE_IGNORE_PREFIX_PATH=/opt/cuda

cmake --build build -j"$(nproc)"
```

Run:

```bash
cd build
./gameplayfootball
```

The `data/` folder must be copied into `build/`:

```bash
mkdir -p build
cp -R data/. build/
```

---

## Important CMake patch

Arch Boost 1.91 no longer exposes `boost_system` the old way.

In `CMakeLists.txt`, this must be patched:

```cmake
FIND_PACKAGE(Boost REQUIRED COMPONENTS system thread filesystem)
```

Change to:

```cmake
FIND_PACKAGE(Boost REQUIRED COMPONENTS thread filesystem)
```

And remove `Boost::system` from the link list.

Keep:

```cmake
Boost::filesystem
Boost::thread
```

Do not re-add `Boost::system` unless the build environment changes.

---

## Current stable product direction

### Main Menu

Main menu should eventually show:

```txt
New Game
Load Game
Settings
Quit
```

Behavior:

```txt
New Game -> manager creation flow
Load Game -> list saved managers/careers
Settings -> original GameplayFootball settings
Quit -> exits game
```

### New Game flow

Desired flow:

```txt
New Game
  -> Create Manager Profile
  -> Select League
  -> Select Club
  -> Generate Career Season
  -> Career Hub / Main Screen
```

Manager profile fields:

```txt
Manager Name
Manager Age
Manager Nationality
Gender: Male/Female
```

Database table:

```sql
CREATE TABLE IF NOT EXISTS managers (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name VARCHAR(128),
  age INTEGER,
  nationality VARCHAR(64),
  gender VARCHAR(16),
  club_id INTEGER,
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
```

`manager_id` currently acts as the save/career ID.

Later we can split this into:

```txt
managers
careers
```

But not yet.

---

## Existing database content

There are currently 4 leagues in `leagues`:

```txt
1 Premier League
2 Bundesliga
3 Eredivisie
4 La Liga
```

There are currently 8 clubs in `teams`:

```txt
1 AFC Ajax              league 3
2 Arsenal FC            league 1
3 FC Barcelona          league 4
4 Bayern Munich         league 2
5 Borussia Dortmund     league 2
6 Manchester United     league 1
7 PSV                   league 3
8 Real Madrid CF        league 4
```

Each league currently has 2 teams.

Current expected generated season:

```txt
4 leagues
2 clubs per league
2 fixtures per league because home/away
8 fixtures total
8 standings rows total
```

---

## Career season generation goal

After selecting club and clicking `Start Career`, generate all leagues, not only the selected league.

Tables to create:

```sql
CREATE TABLE IF NOT EXISTS fixtures (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  manager_id INTEGER NOT NULL,
  league_id INTEGER NOT NULL,
  season_year INTEGER NOT NULL,
  round INTEGER NOT NULL,
  matchday INTEGER NOT NULL,
  home_team_id INTEGER NOT NULL,
  away_team_id INTEGER NOT NULL,
  status VARCHAR(32) DEFAULT 'scheduled',
  home_score INTEGER,
  away_score INTEGER,
  stats_json TEXT,
  scorers_json TEXT,
  cards_json TEXT,
  played_at DATETIME,
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
```

```sql
CREATE TABLE IF NOT EXISTS standings (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  manager_id INTEGER NOT NULL,
  league_id INTEGER NOT NULL,
  team_id INTEGER NOT NULL,
  played INTEGER DEFAULT 0,
  won INTEGER DEFAULT 0,
  drawn INTEGER DEFAULT 0,
  lost INTEGER DEFAULT 0,
  goals_for INTEGER DEFAULT 0,
  goals_against INTEGER DEFAULT 0,
  goal_difference INTEGER DEFAULT 0,
  points INTEGER DEFAULT 0,
  created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
  UNIQUE(manager_id, league_id, team_id)
);
```

Fixture generation:

```txt
For every league:
  get teams in that league
  every pair plays twice:
    team A home vs team B away
    team B home vs team A away
```

For current data, each league has 2 teams, so each league produces:

```txt
matchday 1: A vs B
matchday 2: B vs A
```

---

## Career UI direction

Do not use dynamic in-place tabs that destroy/rebuild widgets during signal callbacks. That caused heap corruption and crashes.

Use separate pages instead.

Safe navigation pattern in this codebase:

```cpp
this->Exit();
windowManager->GetPageFactory()->CreatePage((int)e_PageID_TargetPage, properties, 0);
delete this;
```

This is how existing GameplayFootball pages work.

Do not remove `delete this` from real page transitions. The engine expects it.

Do not destroy child grids/widgets inside a live button callback unless you fully understand GUI ownership.

### Desired safe page structure

Use a Career Hub page with buttons:

```txt
Club
Manager
Matches
Standings
Play Match
Main Menu
```

Each button should open a separate page:

```txt
Club Page      -> club info + club player list
Manager Page   -> manager profile info
Matches Page   -> all fixtures for all leagues
Standings Page -> standings for all leagues
Play Match     -> later starts match
Main Menu      -> returns to main menu
```

Avoid in-place tab switching for now.

---

## Load Game

`Load Game` should show saved managers from `managers` table.

Clicking a manager should load the career by passing `managerId` to the Career Hub / Main Screen.

No match launch should happen during load.

Desired query:

```sql
SELECT managers.id, managers.name, managers.age, managers.nationality, managers.gender,
       teams.name
FROM managers
LEFT JOIN teams ON managers.club_id = teams.id
ORDER BY managers.id DESC
LIMIT 20;
```

---

## AI vs AI manager mode

We successfully proved AI-vs-AI works.

The key was clearing controller sides:

```cpp
std::vector<SideSelection> sides;
GetMenuTask()->SetControllerSetup(sides);
```

Known-safe teams:

```txt
3 = FC Barcelona
8 = Real Madrid CF
```

Starting direct quickstart AI-vs-AI with those teams worked.

Manager mode should use zero human controllers.

---

## AI difficulty

We forced max AI difficulty because GameplayFootball uses `matchDifficulty` even for AI teams.

Search showed:

```txt
src/onthepitch/player/player.cpp
src/onthepitch/player/controller/playercontroller.cpp
src/onthepitch/player/controller/elizacontroller.cpp
```

At difficulty `1.0`:

```txt
CPU multiplier = 1.0
reaction penalty = 0ms
hunt distance = 1.0x
```

Patch in `src/onthepitch/match.cpp` should be config-driven:

```cpp
const bool managerMode =
  GetConfiguration()->GetReal("manager_mode", 0.0f) > 0.5f;

if (managerMode) {
  matchDifficulty = GetConfiguration()->GetReal("manager_ai_difficulty", 1.0f);

  if (matchDifficulty < 0.0f) matchDifficulty = 0.0f;
  if (matchDifficulty > 1.0f) matchDifficulty = 1.0f;

  printf("[MANAGER MODE] AI difficulty set to %.2f\n", matchDifficulty);
} else {
  matchDifficulty = GetConfiguration()->GetReal("match_difficulty", 0.8f);
}
```

---

## Major crash lessons

Several crashes occurred from unsafe GUI lifecycle modifications:

```txt
boost::signals2::mutex::lock assertion
malloc(): unaligned tcache chunk detected
corrupted size vs. prev_size
pthread priority assertion
SIGSEGV after MenuScene recreation
```

Root cause was almost certainly GUI object lifetime misuse:

```txt
destroying/rebuilding grids during button signal callbacks
recreating the same page as a fake tab switch
removing delete this from real page transitions
mixing lifecycle styles
```

Rules:

```txt
1. For real page changes, follow original code style:
   this->Exit(); CreatePage(...); delete this;

2. Do not use in-place tab switching with grid destruction.

3. Do not manually delete child widgets unless following existing engine patterns.

4. Do not create LoadingMatchPage directly from unstable manager pages until this path is understood.

5. Prefer conservative separate pages.
```

---

## Files touched / likely relevant

Important menu files:

```txt
src/menu/mainmenu.cpp
src/menu/mainmenu.hpp
src/menu/menutask.cpp
src/menu/menutask.hpp
src/menu/pagefactory.cpp
src/menu/pagefactory.hpp
src/menu/managercareer.cpp
src/menu/managercareer.hpp
sources.cmake
```

Important match files:

```txt
src/onthepitch/match.cpp
src/onthepitch/match.hpp
src/onthepitch/teamAIcontroller.cpp
src/onthepitch/teamAIcontroller.hpp
src/onthepitch/player/controller/elizacontroller.cpp
```

Useful search commands:

```bash
rg "TeamAIController|UpdateTactics|baseTeamTactics|liveTeamTactics|offensivenessBias" src
rg "GetHumanGamerCount|HumanGamer|ElizaController|PlayerController|Controller" src/onthepitch src/menu
rg "MatchData|LoadingMatch|TeamSelect|MatchOptions|new Match|StartMatch" src
rg "tacticsdebug|scoreboard|radar|gamepage" src/menu/ingame
rg "delete this" src/menu
rg "CreatePage" src/menu
```

---

## Immediate next safe task

Recover/stabilize to the last working state, then implement conservatively:

```txt
1. Main Menu works
2. New Game -> create manager -> select league -> select club -> start career
3. Load Game -> list managers -> Career Hub
4. Career Hub -> separate pages:
   - Club Page
   - Manager Page
   - Matches Page
   - Standings Page
5. Generate fixtures + standings on Start Career
6. Only after stable, revisit Play Match
```

Do not implement dynamic tabs yet.

---

## Coding style guidance for this repo

The codebase is old C++.

Use existing patterns even if modern C++ would be cleaner.

Prefer:

```cpp
DatabaseResult *result = GetDB()->Query(query);
delete result;
```

Prefer `std::stringstream` for SQL because the repo already uses this style.

Prefer `boost::bind` because existing GUI signals use it.

Prefer separate `Gui2Page` classes over complex dynamic UI mutation.

Avoid introducing a new dependency unless necessary.

---

## Product direction after stable career UI

After the career shell is stable:

```txt
simulate non-user fixtures
play selected/user fixture through 3D match engine
store score/stats/scorers/cards in fixtures
update standings
advance matchday
add tactical screen
map manager tactics into TeamAIController values
```

Tactical values to map later:

```txt
Mentality -> offensivenessBias / dribble_offensiveness
Line Height -> position_defense_depth_factor
Attacking Width -> position_offense_width_factor
Defensive Width -> position_defense_width_factor
Pressing -> position_defense_microfocus_strength / midfieldfocus
Tempo/Risk -> dribble_offensiveness / sidefocus / pass tendencies later
```

Role ideas later:

```txt
Advanced Forward
False 9
Inside Forward
Winger
Drop Between Defenders
Support Fullback
Ball Playing Defender
```

Do not start role logic until the career flow is stable.
