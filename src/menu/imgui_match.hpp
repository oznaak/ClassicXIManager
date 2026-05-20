#pragma once

void RenderImGuiMatchOverlay();

// ---- In-match ImGui pause menu ------------------------------------------

// Set true by IngamePage constructor, cleared by its destructor.
extern bool g_ImGuiIngamePauseMenuActive;

// Set by GL render thread (button click), consumed by IngamePage::Process()
// on the main thread to perform safe page transitions.
// 0=none 1=resume 2=gameplan 3=matchfacts 4=settings 5=leave
extern int g_ImGuiPausePendingAction;

// Substitution request written by ImGui (GL thread), consumed by IngamePage::Process() (main thread).
struct PendingSub {
  bool pending = false;
  int  teamIdx = 0;   // match team index for the user's team (0 or 1)
  int  offIdx  = -1;  // index in team->GetAllPlayers() of the player coming OFF
  int  onIdx   = -1;  // index in team->GetAllPlayers() of the player coming ON
};
extern PendingSub g_PendingSub;

void RenderImGuiMatchPauseOverlay();

// Called by StopMatch (under matchRenderMutex) to clear stale match pointer cache.
void ResetMatchOverlayState();
