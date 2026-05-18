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
#include "../gamedefines.hpp"
#include "../onthepitch/match.hpp"
#include "../gametask.hpp"
#include "../data/playerdata.hpp"
#include "careermatchcontext.hpp"

// Shared badge loader defined in imgui_career.cpp.
GLuint LoadBadgeTex(const std::string &logoRelPath);

// ---- Pause menu globals -------------------------------------------------
// Written by IngamePage (main thread), read by GL render thread.
bool g_ImGuiIngamePauseMenuActive = false;
// Written by GL render thread on button click, read+cleared by IngamePage::Process() on main thread.
int  g_ImGuiPausePendingAction    = 0;

// ---------------------------------------------------------------------------
// Per-match cached state

static Match      *s_lastMatch        = nullptr;
static bool        s_overlayLogged    = false;
static bool        s_scoreboardHidden = false;

static std::string s_leagueName;
static std::string s_leagueLogoPath;
static GLuint      s_leagueTex        = 0;

static ImU32 s_homeColor      = IM_COL32(210, 0, 0, 255);   // home primary (row bg + strip)
static ImU32 s_awayColor      = IM_COL32(0, 40, 220, 255);  // away row bg (primary or secondary)
static ImU32 s_awayPrimary    = IM_COL32(0, 40, 220, 255);  // away primary always (strip only)
static ImU32 s_homeTextColor  = IM_COL32(255, 255, 255, 255);
static ImU32 s_awayTextColor  = IM_COL32(255, 255, 255, 255);

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

// Perceptual color distance in RGB space.
static float ColorDistance(ImU32 a, ImU32 b) {
  float dr = (float)((int)((a >> 0) & 0xFF) - (int)((b >> 0) & 0xFF));
  float dg = (float)((int)((a >> 8) & 0xFF) - (int)((b >> 8) & 0xFF));
  float db = (float)((int)((a >>16) & 0xFF) - (int)((b >>16) & 0xFF));
  return sqrtf(dr*dr + dg*dg + db*db);
}

// Boost a color so it is bright enough to read on a dark background.
static ImU32 BrightenForDark(ImU32 col, float minLum = 110.0f) {
  float r = (float)((col >>  0) & 0xFF);
  float g = (float)((col >>  8) & 0xFF);
  float b = (float)((col >> 16) & 0xFF);
  float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
  if (lum >= minLum) return col;
  float boost = (lum < 1.0f) ? 255.0f : minLum / lum;
  int ri = (int)(r * boost); if (ri > 255) ri = 255;
  int gi = (int)(g * boost); if (gi > 255) gi = 255;
  int bi = (int)(b * boost); if (bi > 255) bi = 255;
  return IM_COL32(ri, gi, bi, 255);
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
static void ResetPauseCache(); // forward declaration

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

  // Reset pause cache so it re-captures on the next pause
  ResetPauseCache();

  // Capture team kit colors
  s_homeColor     = IM_COL32(210, 0, 0, 255);
  s_awayColor     = IM_COL32(0, 40, 220, 255);
  s_awayPrimary   = s_awayColor;
  s_homeTextColor = IM_COL32(255, 255, 255, 255);
  s_awayTextColor = IM_COL32(255, 255, 255, 255);

  if (match->GetTeam(0) && match->GetTeam(0)->GetTeamData()) {
    Vector3 c = match->GetTeam(0)->GetTeamData()->GetColor1();
    s_homeColor     = Vec3ToCol32(c.coords[0], c.coords[1], c.coords[2]);
    s_homeTextColor = TextColorForBg(s_homeColor);
  }
  if (match->GetTeam(1) && match->GetTeam(1)->GetTeamData()) {
    Vector3 c1 = match->GetTeam(1)->GetTeamData()->GetColor1();
    s_awayPrimary = Vec3ToCol32(c1.coords[0], c1.coords[1], c1.coords[2]);
    s_awayColor   = s_awayPrimary; // default: primary as row background
    s_awayTextColor = TextColorForBg(s_awayColor);

    if (ColorDistance(s_homeColor, s_awayPrimary) < 80.0f) {
      // Clash: use secondary color as away row background, primary as text
      Vector3 c2 = match->GetTeam(1)->GetTeamData()->GetColor2();
      s_awayColor     = Vec3ToCol32(c2.coords[0], c2.coords[1], c2.coords[2]);
      s_awayTextColor = BrightenForDark(s_awayPrimary); // primary color as text
      printf("[IMGUI MATCH] Away color clash — secondary bg, primary text\n");
    }
  }
}

void ResetMatchOverlayState() {
  s_lastMatch     = nullptr;
  s_overlayLogged = false;
  s_scoreboardHidden = false;
}

// ---------------------------------------------------------------------------

void RenderImGuiMatchOverlay() {
  auto gameTask = GetGameTask();
  if (!gameTask) return;

  // Use matchRenderMutex (not matchLifetimeMutex) to avoid deadlock with PutPhase.
  // PutPhase holds matchLifetimeMutex while waiting for OpenGL vertex-buffer uploads;
  // those uploads block on the OpenGL thread, which is this thread. Using the same
  // mutex would cause a circular deadlock. matchRenderMutex is independent of PutPhase.
  gameTask->matchRenderMutex.lock();
  Match *match = gameTask->GetMatch();
  if (!match) {
    gameTask->matchRenderMutex.unlock();
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

  gameTask->matchRenderMutex.unlock();

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

  // ---- Home team row — primary background, white text ----------------------
  dl->AddRectFilled(ImVec2(teamX, homeY),
                    ImVec2(teamX + teamW, awayY), s_homeColor);
  AddTextLeftCY(dl, g_ManagerFontHero, 30.0f,
                ImVec2(teamX, homeY), ImVec2(teamX + teamW, awayY),
                s_homeTextColor, homeName.c_str(), 12.0f);

  // ---- Away team row — primary bg (or secondary if clash), text adapts -----
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

  // Separator between home and away rows — spans team names + score columns
  dl->AddLine(ImVec2(teamX, awayY), ImVec2(stripX, awayY),
              IM_COL32(255, 255, 255, 160), 1.5f);

  // Thin divider between home/away score cells
  dl->AddLine(ImVec2(scoreX + 6, awayY),
              ImVec2(scoreX + scoreW - 6, awayY),
              IM_COL32(170, 170, 170, 200), 1.0f);

  // ---- Kit color strip (far right) — same logic as row background ----------
  dl->AddRectFilled(ImVec2(stripX, homeY),
                    ImVec2(stripX + stripW, awayY), s_homeColor);
  dl->AddRectFilled(ImVec2(stripX, awayY),
                    ImVec2(stripX + stripW, botY),  s_awayColor);

  // ---- Outer border -------------------------------------------------------
  const float totalW = leagueW + teamW + scoreW + stripW;
  dl->AddRect(ImVec2(x0, y0), ImVec2(x0 + totalW, botY),
              IM_COL32(50, 55, 80, 180), 2.0f, 0, 1.0f);
}

// ---------------------------------------------------------------------------
// Pause menu overlay
// ---------------------------------------------------------------------------

struct PausePlayer {
  std::string lastName;
  std::string role;  // "GK", "CB", "LB", etc.
  float nx = 0.f;   // [0,1] screen x pre-mapped with engine formula
  float ny = 0.f;   // [0,1] screen y — 0=top(attack), 1=bottom(GK)
};

static bool                      s_pauseInitialized   = false;
static std::string               s_pauseTeamName;
static std::vector<PausePlayer>  s_pausePlayers;
static ImU32                     s_pauseUserColor      = IM_COL32(210, 0, 0, 255);
static ImU32                     s_pauseUserTextColor  = IM_COL32(255, 255, 255, 255);
static std::string               s_pauseAwayTeamName;
static std::vector<PausePlayer>  s_pauseAwayPlayers;
static ImU32                     s_pauseOppColor       = IM_COL32(0, 40, 220, 255);
static ImU32                     s_pauseOppTextColor   = IM_COL32(255, 255, 255, 255);

static void ResetPauseCache() {
  s_pauseInitialized = false;
  s_pauseTeamName.clear();
  s_pausePlayers.clear();
  s_pauseAwayTeamName.clear();
  s_pauseAwayPlayers.clear();
}

static float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// Build a PausePlayer using the same formula as planmap.cpp.
static PausePlayer MakePausePlayer(TeamData *td, int i) {
  PausePlayer pp;
  pp.lastName = td->GetPlayerData(i)->GetLastName();
  FormationEntry fe = td->GetFormationEntry(i);
  pp.role = GetRoleName(fe.role);
  Vector3 pos = fe.databasePosition;
  if (fe.role != e_PlayerRole_GK) {
    pos.coords[0] *= 0.8f;
    pos.coords[0] += 0.1f;
  }
  // coords[0] = depth (GK ~-1, attackers ~+1) → portrait Y (negated so GK at bottom)
  // coords[1] = lateral (left=-1, right=+1)   → portrait X
  pp.nx = Clamp01(pos.coords[1] * -0.44f + 0.5f);
  pp.ny = Clamp01(pos.coords[0] * -0.44f + 0.5f);
  return pp;
}

static bool PauseTile(ImDrawList *dl, const char *id,
                      ImVec2 pos, ImVec2 sz, const char *label) {
  ImGui::SetCursorScreenPos(pos);
  ImGui::InvisibleButton(id, sz);
  bool hovered = ImGui::IsItemHovered();
  bool clicked = ImGui::IsItemClicked();

  ImU32 bg   = hovered ? IM_COL32(255, 255, 255, 240) : IM_COL32(25, 35, 60, 230);
  ImU32 txtC = hovered ? IM_COL32(12, 12, 12, 255)    : IM_COL32(255, 255, 255, 255);

  dl->AddRectFilled(pos, ImVec2(pos.x + sz.x, pos.y + sz.y), bg, 6.0f);
  dl->AddRect(pos, ImVec2(pos.x + sz.x, pos.y + sz.y),
              IM_COL32(60, 80, 130, 120), 6.0f, 0, 1.0f);
  AddTextCentered(dl, g_ManagerFontTitle, 22.0f,
                  pos, ImVec2(pos.x + sz.x, pos.y + sz.y), txtC, label);
  return clicked;
}

static void DrawMiniPitch(ImDrawList *dl, ImVec2 min, ImVec2 max) {
  const ImU32 grassDark  = IM_COL32(28,  90,  40, 255);
  const ImU32 grassLight = IM_COL32(32, 105,  46, 255);
  const ImU32 lineCol    = IM_COL32(255, 255, 255, 180);
  float w = max.x - min.x;
  float h = max.y - min.y;

  int stripes = 8;
  for (int i = 0; i < stripes; i++) {
    float sx = min.x + w * i / stripes;
    float ex = min.x + w * (i + 1) / stripes;
    dl->AddRectFilled(ImVec2(sx, min.y), ImVec2(ex, max.y),
                      (i & 1) ? grassDark : grassLight);
  }

  dl->AddRect(min, max, lineCol, 0.0f, 0, 1.5f);

  float midY = min.y + h * 0.5f;
  dl->AddLine(ImVec2(min.x, midY), ImVec2(max.x, midY), lineCol, 1.5f);

  float cx = min.x + w * 0.5f;
  float cr = h * 0.14f;
  dl->AddCircle(ImVec2(cx, midY), cr, lineCol, 24, 1.5f);

  float gw = w * 0.35f;
  float gh = h * 0.10f;
  float gx = min.x + (w - gw) * 0.5f;
  dl->AddRect(ImVec2(gx, min.y),      ImVec2(gx + gw, min.y + gh), lineCol, 0.0f, 0, 1.2f);
  dl->AddRect(ImVec2(gx, max.y - gh), ImVec2(gx + gw, max.y),      lineCol, 0.0f, 0, 1.2f);
}

// Draw a formation panel (header + mini pitch + dots) into a draw list.
// panelH: height used for the panel — caller reserves footer space separately.
static void DrawFormationPanel(
    ImDrawList *dl,
    ImVec2 origin, float w, float panelH,
    const std::string &teamName,
    ImU32 teamColor, ImU32 teamTextColor,
    const std::vector<PausePlayer> &players)
{
  const float headerH   = 46.0f;
  const float pitchPadX = 12.0f;
  const float pitchPadY = 10.0f;

  // Header strip
  dl->AddRectFilled(origin, ImVec2(origin.x + w, origin.y + headerH), teamColor);
  AddTextCentered(dl, g_ManagerFontBold, 17.0f,
                  origin, ImVec2(origin.x + w, origin.y + headerH),
                  teamTextColor, teamName.c_str());

  // Pitch
  ImVec2 pitchMin = ImVec2(origin.x + pitchPadX, origin.y + headerH + pitchPadY);
  ImVec2 pitchMax = ImVec2(origin.x + w - pitchPadX, origin.y + panelH - pitchPadY);
  DrawMiniPitch(dl, pitchMin, pitchMax);

  float pW = pitchMax.x - pitchMin.x;
  float pH = pitchMax.y - pitchMin.y;
  const float dotR    = 7.5f;
  const float roleSz  = 12.5f;
  const float nameSz  = 13.5f;

  for (size_t i = 0; i < players.size() && i < 11; i++) {
    const PausePlayer &pp = players[i];
    float dotX = pitchMin.x + pp.nx * pW;
    float dotY = pitchMin.y + pp.ny * pH;

    dl->AddCircleFilled(ImVec2(dotX, dotY), dotR, teamColor);
    dl->AddCircle(ImVec2(dotX, dotY), dotR, IM_COL32(255, 255, 255, 200), 12, 1.0f);

    // Role abbreviation above dot (bold, white)
    if (!pp.role.empty()) {
      ImVec2 rs = g_ManagerFontBold
          ? g_ManagerFontBold->CalcTextSizeA(roleSz, FLT_MAX, 0.f, pp.role.c_str())
          : ImGui::CalcTextSize(pp.role.c_str());
      dl->AddText(g_ManagerFontBold, roleSz,
                  ImVec2(dotX - rs.x * 0.5f, dotY - dotR - rs.y - 1.f),
                  IM_COL32(255, 255, 255, 255), pp.role.c_str());
    }

    // Surname below dot (bold, slightly larger, up to 8 chars)
    std::string abbr = pp.lastName.size() > 8 ? pp.lastName.substr(0, 8) : pp.lastName;
    ImVec2 ns = g_ManagerFontBold
        ? g_ManagerFontBold->CalcTextSizeA(nameSz, FLT_MAX, 0.f, abbr.c_str())
        : ImGui::CalcTextSize(abbr.c_str());
    dl->AddText(g_ManagerFontBold, nameSz,
                ImVec2(dotX - ns.x * 0.5f, dotY + dotR + 2.f),
                IM_COL32(255, 255, 255, 220), abbr.c_str());
  }
  if (players.empty()) {
    dl->AddText(nullptr, 11.0f, ImVec2(pitchMin.x + 4, pitchMin.y + 4),
                IM_COL32(200, 220, 255, 150), "No data");
  }
}

void RenderImGuiMatchPauseOverlay() {
  if (!g_ImGuiIngamePauseMenuActive) return;
  if (g_ImGuiPausePendingAction != 0) return;

  // Populate cache on first frame of pause (GL thread — TeamData read-only during match)
  if (!s_pauseInitialized) {
    auto gt = GetGameTask();
    if (gt) {
      gt->matchRenderMutex.lock();
      Match *match = gt->GetMatch();
      if (match) {
        // Use g_CareerMatchContext.userClubId — g_CareerHub is cleared when the
        // career page exits before the match starts, so clubId would be 0 there.
        int userClubId = g_CareerMatchContext.userClubId;
        int teamIdx = 0;
        if (userClubId > 0 &&
            match->GetTeam(1) && match->GetTeam(1)->GetTeamData() &&
            match->GetTeam(1)->GetTeamData()->GetDatabaseID() == userClubId)
          teamIdx = 1;
        int oppIdx = 1 - teamIdx;

        // User team
        TeamData *td = match->GetTeam(teamIdx) ? match->GetTeam(teamIdx)->GetTeamData() : nullptr;
        if (td) {
          s_pauseTeamName = td->GetName();
          Vector3 c = td->GetColor1();
          s_pauseUserColor     = Vec3ToCol32(c.coords[0], c.coords[1], c.coords[2]);
          s_pauseUserTextColor = TextColorForBg(s_pauseUserColor);
          int n = std::min(td->GetPlayerNum(), 11);
          for (int i = 0; i < n; i++)
            s_pausePlayers.push_back(MakePausePlayer(td, i));
        }

        // Opponent team
        TeamData *tdOpp = match->GetTeam(oppIdx) ? match->GetTeam(oppIdx)->GetTeamData() : nullptr;
        if (tdOpp) {
          s_pauseAwayTeamName = tdOpp->GetName();
          Vector3 c = tdOpp->GetColor1();
          s_pauseOppColor     = Vec3ToCol32(c.coords[0], c.coords[1], c.coords[2]);
          s_pauseOppTextColor = TextColorForBg(s_pauseOppColor);
          int n = std::min(tdOpp->GetPlayerNum(), 11);
          for (int i = 0; i < n; i++)
            s_pauseAwayPlayers.push_back(MakePausePlayer(tdOpp, i));
        }
      }
      gt->matchRenderMutex.unlock();
    }
    if (s_pauseTeamName.empty())     s_pauseTeamName     = g_CareerHub.club.name;
    if (s_pauseTeamName.empty())     s_pauseTeamName     = "Your Team";
    if (s_pauseAwayTeamName.empty()) s_pauseAwayTeamName = "Opponents";
    s_pauseInitialized = true;
    printf("[IMGUI PAUSE] Cache: %zu user / %zu opp players\n",
           s_pausePlayers.size(), s_pauseAwayPlayers.size());
  }

  ImGuiIO &io = ImGui::GetIO();

  // Dim overlay behind all ImGui windows
  ImGui::GetBackgroundDrawList()->AddRectFilled(
      ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 145));

  // Popup dimensions
  const float popW = std::min(1280.0f, io.DisplaySize.x - 40.0f);
  const float popH = std::min(620.0f,  io.DisplaySize.y - 60.0f);
  const float popX = (io.DisplaySize.x - popW) * 0.5f;
  const float popY = (io.DisplaySize.y - popH) * 0.5f;

  // Three-panel widths: left (user formation) | mid (tiles) | right (opp formation)
  const float leftW  = std::floor(popW * 0.265f);
  const float rightW = leftW;
  const float midW   = popW - leftW - rightW;
  const float footerH = 40.0f;

  ImGui::SetNextWindowPos(ImVec2(popX, popY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(popW, popH), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(14, 20, 38, 250));

  ImGui::Begin("##pause_overlay", nullptr,
    ImGuiWindowFlags_NoTitleBar        |
    ImGuiWindowFlags_NoResize          |
    ImGuiWindowFlags_NoMove            |
    ImGuiWindowFlags_NoScrollbar       |
    ImGuiWindowFlags_NoSavedSettings   |
    ImGuiWindowFlags_NoFocusOnAppearing|
    ImGuiWindowFlags_NoNav);

  // ---- LEFT PANEL: user formation + Game Plan footer -----------------------
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 28, 52, 255));
  ImGui::BeginChild("##pause_left", ImVec2(leftW, popH), false,
                    ImGuiWindowFlags_NoScrollbar);
  {
    ImDrawList *ld      = ImGui::GetWindowDrawList();
    ImVec2      lOrigin = ImGui::GetWindowPos();

    DrawFormationPanel(ld, lOrigin, leftW, popH - footerH,
                       s_pauseTeamName, s_pauseUserColor, s_pauseUserTextColor,
                       s_pausePlayers);

    // Game Plan footer
    ImVec2 btnMin = ImVec2(lOrigin.x, lOrigin.y + popH - footerH);
    ImVec2 btnMax = ImVec2(lOrigin.x + leftW, lOrigin.y + popH);
    ImGui::SetCursorScreenPos(btnMin);
    ImGui::InvisibleButton("##gameplan_btn", ImVec2(leftW, footerH));
    bool gpHov = ImGui::IsItemHovered();
    ld->AddRectFilled(btnMin, btnMax,
                      gpHov ? IM_COL32(255, 255, 255, 200) : IM_COL32(30, 44, 80, 220));
    ImU32 gpTxt = gpHov ? IM_COL32(15, 15, 15, 255) : IM_COL32(180, 200, 240, 255);
    AddTextCentered(ld, g_ManagerFontBold, 14.0f, btnMin, btnMax, gpTxt, "GAME PLAN");
    if (ImGui::IsItemClicked()) g_ImGuiPausePendingAction = 2;
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  // ---- MIDDLE PANEL: 2x2 option tiles --------------------------------------
  ImGui::SameLine(0, 0);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(12, 18, 32, 255));
  ImGui::BeginChild("##pause_mid", ImVec2(midW, popH), false,
                    ImGuiWindowFlags_NoScrollbar);
  {
    ImDrawList *md      = ImGui::GetWindowDrawList();
    ImVec2      mOrigin = ImGui::GetWindowPos();

    // Separator lines on both sides
    md->AddLine(ImVec2(mOrigin.x,          mOrigin.y + 16),
                ImVec2(mOrigin.x,          mOrigin.y + popH - 16),
                IM_COL32(50, 65, 110, 160), 1.0f);
    md->AddLine(ImVec2(mOrigin.x + midW - 1, mOrigin.y + 16),
                ImVec2(mOrigin.x + midW - 1, mOrigin.y + popH - 16),
                IM_COL32(50, 65, 110, 160), 1.0f);

    const float tilePad = 14.0f;
    const float tileW   = (midW - tilePad * 3.0f) * 0.5f;
    const float tileH   = (popH - tilePad * 3.0f) * 0.5f;

    ImVec2 t00 = ImVec2(mOrigin.x + tilePad,             mOrigin.y + tilePad);
    ImVec2 t10 = ImVec2(mOrigin.x + tilePad * 2 + tileW, mOrigin.y + tilePad);
    ImVec2 t01 = ImVec2(mOrigin.x + tilePad,             mOrigin.y + tilePad * 2 + tileH);
    ImVec2 t11 = ImVec2(mOrigin.x + tilePad * 2 + tileW, mOrigin.y + tilePad * 2 + tileH);
    ImVec2 tsz = ImVec2(tileW, tileH);

    if (PauseTile(md, "##resume",     t00, tsz, "Resume Match")) g_ImGuiPausePendingAction = 1;
    if (PauseTile(md, "##matchfacts", t10, tsz, "Match Facts"))  g_ImGuiPausePendingAction = 3;
    if (PauseTile(md, "##settings",   t01, tsz, "Settings"))     g_ImGuiPausePendingAction = 4;
    if (PauseTile(md, "##leave",      t11, tsz, "Leave Match"))  g_ImGuiPausePendingAction = 5;
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  // ---- RIGHT PANEL: opponent formation (no footer) -------------------------
  ImGui::SameLine(0, 0);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 28, 52, 255));
  ImGui::BeginChild("##pause_right", ImVec2(rightW, popH), false,
                    ImGuiWindowFlags_NoScrollbar);
  {
    ImDrawList *rd      = ImGui::GetWindowDrawList();
    ImVec2      rOrigin = ImGui::GetWindowPos();

    DrawFormationPanel(rd, rOrigin, rightW, popH,
                       s_pauseAwayTeamName, s_pauseOppColor, s_pauseOppTextColor,
                       s_pauseAwayPlayers);
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}
