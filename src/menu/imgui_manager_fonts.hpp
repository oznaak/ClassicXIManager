#pragma once
#include "imgui.h"

// Font hierarchy for the career hub UI.
// Defined in imgui_career.cpp; loaded by LoadManagerFonts().
// All pointers are nullptr if font loading failed — PushFont(nullptr) is safe.

extern ImFont *g_ManagerFontSmall;    // regular 13px
extern ImFont *g_ManagerFontRegular;  // regular 15px
extern ImFont *g_ManagerFontMedium;   // regular 17px
extern ImFont *g_ManagerFontBold;     // bold    18px
extern ImFont *g_ManagerFontTitle;    // bold    24px
extern ImFont *g_ManagerFontHero;     // bold    32px

// Call once after ImGui::CreateContext(), before the first NewFrame().
void LoadManagerFonts();
