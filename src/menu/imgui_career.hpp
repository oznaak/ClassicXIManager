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
  int  pendingAction = 0; // 0=none 1=playMatch 2=mainMenu

  std::function<void()> onPlayMatch;
  std::function<void()> onMainMenu;

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
