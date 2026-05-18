#pragma once

void RenderImGuiMatchOverlay();

// ---- In-match ImGui pause menu ------------------------------------------

// Set true by IngamePage constructor, cleared by its destructor.
extern bool g_ImGuiIngamePauseMenuActive;

// Set by GL render thread (button click), consumed by IngamePage::Process()
// on the main thread to perform safe page transitions.
// 0=none 1=resume 2=gameplan 3=matchfacts 4=settings 5=leave
extern int g_ImGuiPausePendingAction;

void RenderImGuiMatchPauseOverlay();
