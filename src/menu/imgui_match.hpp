#pragma once
#include <string>

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
  std::string  nameOut;
  std::string  nameIn;
  std::string  teamBadgePath;
  std::string  leagueLogoPath;
  unsigned int teamColor      = 0;  // club primary color (same as scoreboard)
};
extern QueuedSub g_QueuedSub;

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
extern SubGraphic g_SubGraphic;

void RenderImGuiMatchPauseOverlay();

// Called by StopMatch (under matchRenderMutex) to clear stale match pointer cache.
void ResetMatchOverlayState();
