#pragma once

#include <string>
#include <vector>
#include <functional>
#include <map>

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
    std::string type; // "league" or "ko"
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
    int leagueId = 0;
    std::string currency; // e.g. "\xC2\xA3" or "\xE2\x82\xAC"
  } club;

  struct Player {
    int         id            = 0;
    std::string firstName, lastName, role, age, ability;
    int         formationOrder = -1;
    int         weeklywage    = 0;
    std::string contractExpiry;
    float       baseStat      = 0.0f;
    int         potential     = 0;
    std::string foot;          // "L" or "R"
    int         stamina       = 0; // 0-100
    float       height        = 0.0f; // metres e.g. 1.80
    float       reputation    = 0.0f; // 1-20 scale
  };
  std::vector<Player> players;

  struct Fixture {
    std::string league, matchday, round;
    std::string home, away;          // shortnames — used for user-club matching
    std::string homeFull, awayFull;  // full names — used for display
    std::string homeLogo, awayLogo;
    std::string leagueLogo;
    int         leagueId = 0;
    std::string status, score;
    std::string fixtureDate;
  };
  std::vector<Fixture> fixtures;

  struct Standing {
    std::string league, team, teamFull, teamLogo;
    std::string p, w, d, l, gf, ga, gd, pts;
  };
  std::vector<Standing> standings;

  std::map<std::string, float> tactics; // tactic key → current value (0.0-1.0)

  struct StaffMember {
    int         id         = 0;
    std::string firstName, lastName, nationality, role;
    int         age        = 0;
    int         rating     = 0; // 1-5
    int         weeklywage = 0;
  };
  std::vector<StaffMember> staff; // currently hired staff for this manager

  struct ScoutQueueEntry {
    int         playerId    = 0;
    std::string firstName, lastName;
    std::string role, age;
    std::string clubName, clubLogoPath, clubShortName;
    std::string dueDate;    // YYYY-MM-DD when report arrives
    int         scoutRating = 1;
  };
  std::vector<ScoutQueueEntry> scoutQueue;

  struct ScoutReport {
    int         playerId  = 0;
    std::string firstName, lastName;
    std::string role, age;
    std::string clubName, clubLogoPath, clubShortName;
    float       revealPct = 0.0f; // 0.0-1.0 fraction of stats revealed
  };
  std::vector<ScoutReport> scoutReports;

  struct InboxMessage {
    int         id          = 0;
    int         templateId  = 0;
    std::string senderType; // "board"|"staff"|"media"|"fans"|"players"|"competition"|"transfers"|"finance"
    std::string senderName;
    std::string subject;
    std::string body;
    std::string category;
    std::string gameDate;   // YYYY-MM-DD
    bool        isRead      = false;
    bool        isStarred   = false;
    bool        hasTask     = false;
    bool        taskDone    = false;
  };
  std::vector<InboxMessage> inbox;

  struct FinanceTransaction {
    std::string date;
    std::string category;   // 'tv_rights','matchday','wages','operating','prize'
    std::string description;
    long long   amount = 0; // £ — positive=income, negative=expense
  };

  struct FinanceState {
    long long balance          = 0;
    long long weeklyTV         = 0;  // income per week
    long long weeklyWages      = 0;  // expense per week (players + staff)
    long long weeklyOperating  = 0;  // expense per week (fixed)
    long long matchdayMin      = 0;  // per match, worst case (weak opponent, poor form)
    long long matchdayMax      = 0;  // per match, best case (strong opponent, top of table)
    long long seasonPrize1st   = 0;  // prize for 1st in user's league
    long long seasonPrize2nd   = 0;
    std::vector<FinanceTransaction> recent; // last 40 transactions
  } finances;

  void Clear();
  void LoadFromDB(int managerId, int clubId);
};

extern CareerHubState g_CareerHub;

void RenderImGuiCareerHub();

// ---- Pre-match lineup presentation state -----------------------------------

struct PreMatchLineupPlayer {
  int number = 0;
  std::string name;
  std::string role;
};

struct PreMatchLineupState {
  bool active           = false;
  bool valid            = false;
  double startedAt      = 0.0;

  int fixtureId  = 0;
  int managerId  = 0;
  int leagueId   = 0;
  int homeTeamId = 0;
  int awayTeamId = 0;

  std::string competitionName;
  std::string competitionLogoPath;

  std::string homeTeamName;
  std::string awayTeamName;
  std::string homeBadgePath;
  std::string awayBadgePath;

  std::vector<PreMatchLineupPlayer> homeStartingXI;
  std::vector<PreMatchLineupPlayer> awayStartingXI;
  std::vector<PreMatchLineupPlayer> homeBench;
  std::vector<PreMatchLineupPlayer> awayBench;
  bool hasBench = false;

  bool continueRequested = false;
  int  matchSpeed = 1; // 1, 2, 4, or 8

  void Clear() { *this = PreMatchLineupState(); }
};

extern PreMatchLineupState g_PreMatchLineup;

// Persists across match start so RenderImGuiMatchOverlay can use the competition logo
// even after g_PreMatchLineup is cleared by GamePage.
extern std::string g_MatchCompetitionLogoPath;
extern std::string g_MatchCompetitionName;

void RenderImGuiPreMatchLineup();

// Full-screen black overlay drawn while silent LoadingMatchPage does its handoff.
// Set true by LoadingMatchPage(skipVisual), cleared after SetMenuAction(e_MenuAction_Game).
extern bool g_SilentMatchLoadingOverlay;
extern bool g_SilentMatchLoadingOverlayLogged;
void RenderImGuiSilentMatchLoadingOverlay();
