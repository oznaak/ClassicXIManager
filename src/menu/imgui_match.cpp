#include "imgui_match.hpp"
#include "imgui_career.hpp"
#include "imgui_manager_fonts.hpp"

#include "imgui.h"
#include <SDL2/SDL_image.h>

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#include <GL/glext.h>
#endif

#include <string>
#include <sstream>
#include <cstdio>

#include "main.hpp"
#include "../onthepitch/match.hpp"
#include "../gametask.hpp"

// Shared badge loader defined in imgui_career.cpp.
GLuint LoadBadgeTex(const std::string &logoRelPath);

// ---------------------------------------------------------------------------
// Per-match cached state

static Match      *s_lastMatch        = nullptr;
static bool        s_overlayLogged    = false;
static bool        s_scoreboardHidden = false;

static std::string s_leagueName;
static std::string s_leagueLogoPath;
static GLuint      s_leagueTex        = 0;

static ImU32 s_homeColor     = IM_COL32(210, 0, 0, 255);
static ImU32 s_awayColor     = IM_COL32(0, 40, 220, 255);
static ImU32 s_homeTextColor = IM_COL32(255, 255, 255, 255);
static ImU32 s_awayTextColor = IM_COL32(255, 255, 255, 255);

// ---------------------------------------------------------------------------

static ImU32 Vec3ToCol32(float r, float g, float b) {
  int ri = (int)r; ri = ri < 0 ? 0 : (ri > 255 ? 255 : ri);
  int gi = (int)g; gi = gi < 0 ? 0 : (gi > 255 ? 255 : gi);
  int bi = (int)b; bi = bi < 0 ? 0 : (bi > 255 ? 255 : bi);
  return IM_COL32(ri, gi, bi, 255);
}

static ImU32 TextColorForBg(ImU32 bg) {
  // ImU32 is ABGR: A bits31-24, B bits23-16, G bits15-8, R bits7-0
  float r = (float)((bg >>  0) & 0xFF);
  float g = (float)((bg >>  8) & 0xFF);
  float b = (float)((bg >> 16) & 0xFF);
  float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
  return lum > 155.0f ? IM_COL32(15, 15, 15, 255) : IM_COL32(255, 255, 255, 255);
}

static void AddTextCentered(ImDrawList *dl, ImFont *font, float sz,
                             ImVec2 min, ImVec2 max, ImU32 col, const char *text) {
  ImVec2 ts = font ? font->CalcTextSizeA(sz, FLT_MAX, 0.0f, text)
                   : ImGui::CalcTextSize(text);
  float x = min.x + (max.x - min.x - ts.x) * 0.5f;
  float y = min.y + (max.y - min.y - ts.y) * 0.5f;
  if (font) dl->AddText(font, sz, ImVec2(x, y), col, text);
  else      dl->AddText(ImVec2(x, y), col, text);
}

static void AddTextLeftCY(ImDrawList *dl, ImFont *font, float sz,
                           ImVec2 min, ImVec2 max, ImU32 col, const char *text, float padX) {
  ImVec2 ts = font ? font->CalcTextSizeA(sz, FLT_MAX, 0.0f, text)
                   : ImGui::CalcTextSize(text);
  float x = min.x + padX;
  float y = min.y + (max.y - min.y - ts.y) * 0.5f;
  if (font) dl->AddText(font, sz, ImVec2(x, y), col, text);
  else      dl->AddText(ImVec2(x, y), col, text);
}

// ---------------------------------------------------------------------------

static void ResetPerMatch(Match *match) {
  s_lastMatch        = match;
  s_overlayLogged    = false;
  s_scoreboardHidden = false;
  s_leagueTex        = 0;

  // League name: prefer persistent capture from prematch screen, then career hub
  s_leagueName = g_MatchCompetitionName;
  if (s_leagueName.empty()) s_leagueName = g_CareerHub.club.leagueName;
  if (s_leagueName.empty()) s_leagueName = "League";

  // League logo: prefer persistent capture from prematch screen
  s_leagueLogoPath = g_MatchCompetitionLogoPath;

  // Fallback: DB query via career hub league id
  if (s_leagueLogoPath.empty()) {
    int lid = g_CareerHub.club.leagueId;
    if (lid > 0) {
      std::stringstream q;
      q << "SELECT logo_url FROM leagues WHERE id = " << lid << " LIMIT 1";
      DatabaseResult *res = GetDB()->Query(q.str());
      if (res && !res->data.empty() && !res->data[0].empty())
        s_leagueLogoPath = res->data[0][0];
      delete res;
    }
  }

  printf("[IMGUI MATCH] league='%s' logoPath='%s'\n",
         s_leagueName.c_str(), s_leagueLogoPath.c_str());

  // Capture team kit colors
  s_homeColor = IM_COL32(210, 0, 0, 255);
  s_awayColor = IM_COL32(0, 40, 220, 255);
  if (match->GetTeam(0) && match->GetTeam(0)->GetTeamData()) {
    Vector3 c = match->GetTeam(0)->GetTeamData()->GetColor1();
    s_homeColor = Vec3ToCol32(c.coords[0], c.coords[1], c.coords[2]);
  }
  if (match->GetTeam(1) && match->GetTeam(1)->GetTeamData()) {
    Vector3 c = match->GetTeam(1)->GetTeamData()->GetColor1();
    s_awayColor = Vec3ToCol32(c.coords[0], c.coords[1], c.coords[2]);
  }
  s_homeTextColor = TextColorForBg(s_homeColor);
  s_awayTextColor = TextColorForBg(s_awayColor);
}

// ---------------------------------------------------------------------------

void RenderImGuiMatchOverlay() {
  auto gameTask = GetGameTask();
  if (!gameTask) return;

  gameTask->matchLifetimeMutex.lock();
  Match *match = gameTask->GetMatch();
  if (!match) {
    gameTask->matchLifetimeMutex.unlock();
    return;
  }

  if (match != s_lastMatch) ResetPerMatch(match);

  if (!s_overlayLogged) {
    printf("[IMGUI MATCH] Match overlay active\n");
    s_overlayLogged = true;
  }

  if (!s_scoreboardHidden) {
    match->HideScoreboard();
    s_scoreboardHidden = true;
  }

  unsigned long t_ms = match->GetMatchTime_ms();
  int homeGoals      = match->GetScore(0);
  int awayGoals      = match->GetScore(1);
  std::string homeName = match->GetTeam(0)->GetTeamData()->GetName();
  std::string awayName = match->GetTeam(1)->GetTeamData()->GetName();

  gameTask->matchLifetimeMutex.unlock();

  if (homeName.empty()) homeName = "Home";
  if (awayName.empty()) awayName = "Away";

  int minutes = (int)(t_ms / 60000);
  int seconds  = (int)(t_ms / 1000) % 60;
  char timeBuf[16];
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d", minutes, seconds);

  char homeBuf[8], awayBuf[8];
  snprintf(homeBuf, sizeof(homeBuf), "%d", homeGoals);
  snprintf(awayBuf, sizeof(awayBuf), "%d", awayGoals);

  // Load league tex on first use (GL thread — safe)
  if (s_leagueTex == 0 && !s_leagueLogoPath.empty())
    s_leagueTex = LoadBadgeTex(s_leagueLogoPath);

  // ---- Layout constants ---------------------------------------------------
  const float x0      = 16.0f;
  const float y0      = 16.0f;
  const float leagueW = 92.0f;
  const float teamW   = 220.0f;
  const float scoreW  = 58.0f;
  const float stripW  = 8.0f;
  const float rowH    = 50.0f;
  const float totalH  = rowH * 2.0f;

  const float leagueX = x0;
  const float teamX   = x0 + leagueW;
  const float scoreX  = teamX + teamW;
  const float stripX  = scoreX + scoreW;

  const float homeY   = y0;
  const float awayY   = y0 + rowH;
  const float botY    = y0 + totalH;

  ImDrawList *dl = ImGui::GetForegroundDrawList();

  const ImU32 navyBg   = IM_COL32(12, 20, 55, 248);
  const ImU32 blackBg  = IM_COL32(6, 6, 6, 252);
  const ImU32 scoreBg  = IM_COL32(238, 238, 238, 255);
  const ImU32 scoreTxt = IM_COL32(12, 12, 12, 255);

  // ---- Left: league branding block ----------------------------------------
  // Top half: navy + league logo / text
  dl->AddRectFilled(ImVec2(leagueX,          homeY),
                    ImVec2(leagueX + leagueW, awayY), navyBg);
  // Bottom half: black + clock
  dl->AddRectFilled(ImVec2(leagueX,          awayY),
                    ImVec2(leagueX + leagueW, botY),  blackBg);

  if (s_leagueTex) {
    const float pad   = 7.0f;
    const float imgSz = rowH - pad * 2.0f;
    const float imgX  = leagueX + (leagueW - imgSz) * 0.5f;
    dl->AddImage((ImTextureID)(intptr_t)s_leagueTex,
                 ImVec2(imgX,        homeY + pad),
                 ImVec2(imgX + imgSz, homeY + pad + imgSz));
  } else {
    // Text fallback: two-line league name
    AddTextCentered(dl, g_ManagerFontSmall, 11.0f,
                    ImVec2(leagueX + 3, homeY),
                    ImVec2(leagueX + leagueW - 3, awayY),
                    IM_COL32(190, 210, 255, 230), s_leagueName.c_str());
  }

  // Clock
  AddTextCentered(dl, g_ManagerFontHero, 32.0f,
                  ImVec2(leagueX, awayY),
                  ImVec2(leagueX + leagueW, botY),
                  IM_COL32(255, 255, 255, 255), timeBuf);

  // ---- Home team row -------------------------------------------------------
  dl->AddRectFilled(ImVec2(teamX, homeY),
                    ImVec2(teamX + teamW, awayY), s_homeColor);
  AddTextLeftCY(dl, g_ManagerFontHero, 30.0f,
                ImVec2(teamX, homeY), ImVec2(teamX + teamW, awayY),
                s_homeTextColor, homeName.c_str(), 12.0f);

  // ---- Away team row -------------------------------------------------------
  dl->AddRectFilled(ImVec2(teamX, awayY),
                    ImVec2(teamX + teamW, botY), s_awayColor);
  AddTextLeftCY(dl, g_ManagerFontHero, 30.0f,
                ImVec2(teamX, awayY), ImVec2(teamX + teamW, botY),
                s_awayTextColor, awayName.c_str(), 12.0f);

  // ---- Score column --------------------------------------------------------
  dl->AddRectFilled(ImVec2(scoreX, homeY),
                    ImVec2(scoreX + scoreW, awayY), scoreBg);
  AddTextCentered(dl, g_ManagerFontHero, 30.0f,
                  ImVec2(scoreX, homeY), ImVec2(scoreX + scoreW, awayY),
                  scoreTxt, homeBuf);

  dl->AddRectFilled(ImVec2(scoreX, awayY),
                    ImVec2(scoreX + scoreW, botY), scoreBg);
  AddTextCentered(dl, g_ManagerFontHero, 30.0f,
                  ImVec2(scoreX, awayY), ImVec2(scoreX + scoreW, botY),
                  scoreTxt, awayBuf);

  // Thin divider between home/away score cells
  dl->AddLine(ImVec2(scoreX + 6, awayY),
              ImVec2(scoreX + scoreW - 6, awayY),
              IM_COL32(170, 170, 170, 200), 1.0f);

  // ---- Kit color strip (far right) ----------------------------------------
  dl->AddRectFilled(ImVec2(stripX, homeY),
                    ImVec2(stripX + stripW, awayY), s_homeColor);
  dl->AddRectFilled(ImVec2(stripX, awayY),
                    ImVec2(stripX + stripW, botY),  s_awayColor);

  // ---- Outer border -------------------------------------------------------
  const float totalW = leagueW + teamW + scoreW + stripW;
  dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + totalW, botY),
              IM_COL32(50, 55, 80, 180), 2.0f, 0, 1.0f);
}
