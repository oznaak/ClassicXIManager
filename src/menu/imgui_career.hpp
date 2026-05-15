#pragma once

#include <string>
#include <vector>
#include <functional>

// Shared state written by the menu thread, read by the GL render thread.
// active=false while loading; set true only after all fields are filled.
// pendingAction is set by ImGui buttons and consumed by callbacks.

struct CareerHubState {
  bool active        = false;
  int  primaryTab    = 0; // 0=Portal 1=Squad 2=Recruitment 3=MatchDay 4=Club 5=Career
  int  activeTab     = 0; // portal sub-tab: 0=Overview 1=Manager 2=Club 3=Matches 4=Standings
  int  pendingAction = 0; // 0=none 1=testEngine 2=mainMenu 3=advance 4=playFixture

  std::function<void()> onPlayMatch;   // hardcoded test engine (action 1)
  std::function<void()> onMainMenu;    // action 2
  std::function<void()> onAdvance;     // action 3: advance career day by 1
  std::function<void()> onPlayFixture; // action 4: play today's scheduled fixture

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
  bool         hasTodayFixture = false;
  TodayFixture todayFixture;

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
