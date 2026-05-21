#pragma once
#include <string>

void RenderImGuiMatchOverlay();

// ---- In-match ImGui pause menu ------------------------------------------

// Set true by IngamePage constructor, cleared by its destructor.
extern bool g_ImGuiIngamePauseMenuActive;

// Set by GL render thread (button click), consumed by IngamePage::Process()
// on the main thread to perform safe page transitions.
// 0=none 1=resume 2=gameplan 3=matchfacts 4=settings 5=leave
extern int g_ImGuiPausePendingAction;

// Substitution queued by the user (GL thread) — executed on the next dead ball (game thread).
struct QueuedSub {
  bool        pending = false;
  int         teamIdx = 0;
  int         offIdx  = -1;  // players[] index of outgoing player
  int         onIdx   = -1;  // players[] index of incoming bench player
  std::string nameOut;       // for the substitution graphic
  std::string nameIn;
};
extern QueuedSub g_QueuedSub;

// Substitution banner shown on the pitch for ~3 seconds after a sub fires.
struct SubGraphic {
  bool        active    = false;
  std::string nameOut;
  std::string nameIn;
  double      startTime = 0.0; // ImGui::GetTime() when activated
};
extern SubGraphic g_SubGraphic;

void RenderImGuiMatchPauseOverlay();

// Called by StopMatch (under matchRenderMutex) to clear stale match pointer cache.
void ResetMatchOverlayState();
