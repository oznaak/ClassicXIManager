#pragma once

#include <string>
#include <vector>
#include <functional>

enum AdvanceAction {
  ADVANCE_NONE         = 0,
  ADVANCE_NEXT_DAY     = 1,
  ADVANCE_START_SEASON = 2
};

// Shared state written by the menu thread, read by the GL render thread.
// active=false while loading; set true only after all fields are filled.
// pendingAction is set by ImGui deferred code and consumed by callbacks in the renderer.

struct CareerHubState {
  bool active        = false;
  int  primaryTab    = 0; // 0=Portal 1=Squad 2=Recruitment 3=MatchDay 4=Club 5=Career
  int  activeTab     = 0; // portal sub-tab: 0=Overview 1=Manager 2=Club 3=Matches 4=Standings
  int  pendingAction = 0; // 0=none 1=testEngine 2=mainMenu 3=advance 4=playFixture 5=startNextSeason

  std::function<void()> onPlayMatch;       // hardcoded test engine (action 1)
  std::function<void()> onMainMenu;        // action 2
  std::function<void()> onAdvance;         // action 3: advance career day by 1
  std::function<void()> onPlayFixture;     // action 4: play today's scheduled fixture
  std::function<void()> onStartNextSeason; // action 5: rollover to next season

  // Career date state
  int managerId  = 0;
  int clubId     = 0;
  std::string currentDate;        // YYYY-MM-DD
  std::string currentDateDisplay; // "1 Jul 2026"
  int seasonYear = 0;

  struct TodayFixture {
    int id         = 0;
    int homeTeamId = 0;
    int awayTeamId = 0;
    int leagueId   = 0;
    int matchday   = 0;
    std::string homeShort;
    std::string awayShort;
    std::string fixtureDate;
  };
  bool         hasTodayFixture  = false;
  TodayFixture todayFixture;
  bool         hasSeasonEnded        = false; // true when no scheduled fixtures remain
  bool         isAdvancing           = false; // true while modal overlay is shown
  AdvanceAction pendingAdvanceAction = ADVANCE_NONE; // queued advance type
  int           advanceFramesWaited  = 0;    // single-frame defer counter

  struct ManagerInfo {
    std::string name, age, nationality, gender, clubName;
  } manager;

  struct ClubInfo {
    std::string name, shortName, logoPath, leagueName;
  } club;

  struct Player {
    std::string firstName, lastName, role, age, ability;
  };
  std::vector<Player> players;

  struct Fixture {
    std::string league, matchday, round;
    std::string home, away;
    std::string homeLogo, awayLogo;
    std::string status, score;
    std::string fixtureDate;
  };
  std::vector<Fixture> fixtures;

  struct Standing {
    std::string league, team, teamLogo;
    std::string p, w, d, l, gf, ga, gd, pts;
  };
  std::vector<Standing> standings;

  void Clear();
  void LoadFromDB(int managerId, int clubId);
};

extern CareerHubState g_CareerHub;

void RenderImGuiCareerHub();
