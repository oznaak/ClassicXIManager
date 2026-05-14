#pragma once

#include <string>
#include <vector>
#include <functional>

// Shared state written by the menu thread, read by the GL render thread.
// active=false while loading; set true only after all fields are filled.
// pendingAction is set by ImGui buttons and consumed by callbacks.

struct CareerHubState {
  bool active       = false;
  int  activeTab    = 0; // 0=Manager 1=Club 2=Matches 3=Standings
  int  pendingAction = 0; // 0=none 1=playMatch 2=mainMenu

  std::function<void()> onPlayMatch;
  std::function<void()> onMainMenu;

  struct ManagerInfo {
    std::string name, age, nationality, gender, clubName;
  } manager;

  struct ClubInfo {
    std::string name, shortName;
  } club;

  struct Player {
    std::string firstName, lastName, role, age, ability;
  };
  std::vector<Player> players;

  struct Fixture {
    std::string league, matchday, round, home, away, status, score;
  };
  std::vector<Fixture> fixtures;

  struct Standing {
    std::string league, team;
    std::string p, w, d, l, gf, ga, gd, pts;
  };
  std::vector<Standing> standings;

  void Clear();
  void LoadFromDB(int managerId, int clubId);
};

extern CareerHubState g_CareerHub;

void RenderImGuiCareerHub();
