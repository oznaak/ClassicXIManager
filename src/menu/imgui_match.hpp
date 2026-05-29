#pragma once
#include <cstddef>
#include <string>
#include <vector>

void RenderImGuiMatchOverlay();

// ---- In-match ImGui pause menu ------------------------------------------

// Set true by IngamePage constructor, cleared by its destructor.
extern bool g_ImGuiIngamePauseMenuActive;

// Set by GL thread (top-bar pause/settings button), consumed by GamePage::Process()
// to create IngamePage (same as ESC press).
extern bool g_ImGuiTopBarPauseRequest;

// Set by GL render thread (button click), consumed by IngamePage::Process()
// on the main thread to perform safe page transitions.
// 0=none 1=resume 2=gameplan 3=matchfacts 4=settings 5=leave
extern int g_ImGuiPausePendingAction;

// Soft pause: top-bar pause button freezes match without opening ESC menu.
// Set/cleared by GL thread; read by GameTask::ProcessPhase() to skip match->Process().
extern bool g_TopBarSoftPause;

// Substitution queued by the user (GL thread) — executed on the next dead ball (game thread).
struct QueuedSub {
  bool         pending        = false;
  int          teamIdx        = 0;
  int          offIdx         = -1;
  int          onIdx          = -1;
  int          offPlayerDbId  = -1;
  int          onPlayerDbId   = -1;
  std::string  nameOut;
  std::string  nameIn;
  std::string  teamBadgePath;
  std::string  leagueLogoPath;
  unsigned int teamColor      = 0;  // club primary color (same as scoreboard)
  bool          aiControlled   = false;
  int           userTeamIdx    = -1;
};
extern std::vector<QueuedSub> g_QueuedSubQueue;
bool QueueSubEmpty();
size_t QueueSubSize();
bool QueueSubPeekFront(QueuedSub &sub);
bool QueueSubPopFront(QueuedSub &sub);
void QueueSubPush(const QueuedSub &sub);
void QueueSubClear();
int QueueSubCountUserForTeam(int teamIdx);
bool QueueSubHasUserForTeam(int teamIdx);
bool QueueSubHasAiForTeam(int teamIdx);

// Substitution budget tracking (reset each match)
extern int  g_SubsUsed;      // total subs executed this match (0–5)
extern int  g_WindowsUsed;   // distinct stoppage windows used (0–3)
extern bool g_SubWindowOpen; // true while a window is open (play hasn't resumed yet)

// Substitution banner shown on the pitch for ~5 seconds after a sub fires.
struct SubGraphic {
  bool         active       = false;
  std::string  nameOut;
  std::string  nameIn;
  std::string  teamBadgePath;
  std::string  leagueLogoPath;
  unsigned int teamColor    = 0;  // club primary color
  double       startTime    = 0.0; // ImGui::GetTime() when activated; -1 = set on first frame
};
extern std::vector<SubGraphic> g_SubGraphicQueue; // shown one at a time, front-first
bool SubGraphicPeekForRender(double now, SubGraphic &graphic);
void SubGraphicPopFront();
void SubGraphicPush(const SubGraphic &graphic);
void SubGraphicClear();

void RenderImGuiMatchPauseOverlay();

// Live match stats panel (non-pausing floating overlay).
extern bool g_MatchStatsVisible;

// Called by StopMatch (under matchRenderMutex) to clear stale match pointer cache.
void ResetMatchOverlayState();

// Live tactics panel (non-pausing floating overlay).
extern bool g_TacticsPanelVisible;
extern bool g_MatchPlanPanelVisible;
extern int  g_MatchPlanMentality;  // -2 full defensive .. 0 default .. +2 full attacking
extern int  g_MatchPlanAggression; // -2 soft .. 0 default .. +2 extreme

// Tactic change queued by GL thread — applied to TeamData by gametask.cpp each frame.
struct TacticChange {
  std::string key;
  float       value   = 0.f;
  int         teamIdx = 0;
};
extern std::vector<TacticChange> g_PendingTacticsChanges;
