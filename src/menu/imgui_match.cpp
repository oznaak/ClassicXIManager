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
#include <boost/thread/mutex.hpp>

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

std::vector<QueuedSub>  g_QueuedSubQueue;
std::vector<SubGraphic> g_SubGraphicQueue;
int        g_SubsUsed      = 0;
int        g_WindowsUsed   = 0;
bool       g_SubWindowOpen = false;
bool       g_ImGuiTopBarPauseRequest = false;
bool       g_TopBarSoftPause         = false;
bool       g_MatchStatsVisible       = false;
bool       g_TacticsPanelVisible     = false;
std::vector<TacticChange> g_PendingTacticsChanges;

static boost::mutex g_QueuedSubMutex;
static boost::mutex g_SubGraphicMutex;

bool QueueSubEmpty() {
  boost::mutex::scoped_lock lock(g_QueuedSubMutex);
  return g_QueuedSubQueue.empty();
}

size_t QueueSubSize() {
  boost::mutex::scoped_lock lock(g_QueuedSubMutex);
  return g_QueuedSubQueue.size();
}

bool QueueSubPeekFront(QueuedSub &sub) {
  boost::mutex::scoped_lock lock(g_QueuedSubMutex);
  if (g_QueuedSubQueue.empty()) return false;
  sub = g_QueuedSubQueue.front();
  return true;
}

bool QueueSubPopFront(QueuedSub &sub) {
  boost::mutex::scoped_lock lock(g_QueuedSubMutex);
  if (g_QueuedSubQueue.empty()) return false;
  sub = g_QueuedSubQueue.front();
  g_QueuedSubQueue.erase(g_QueuedSubQueue.begin());
  return true;
}

void QueueSubPush(const QueuedSub &sub) {
  boost::mutex::scoped_lock lock(g_QueuedSubMutex);
  g_QueuedSubQueue.push_back(sub);
}

void QueueSubClear() {
  boost::mutex::scoped_lock lock(g_QueuedSubMutex);
  g_QueuedSubQueue.clear();
}

int QueueSubCountUserForTeam(int teamIdx) {
  boost::mutex::scoped_lock lock(g_QueuedSubMutex);
  int count = 0;
  for (const QueuedSub &sub : g_QueuedSubQueue) {
    if (!sub.aiControlled && sub.teamIdx == teamIdx) count++;
  }
  return count;
}

bool QueueSubHasUserForTeam(int teamIdx) {
  boost::mutex::scoped_lock lock(g_QueuedSubMutex);
  for (const QueuedSub &sub : g_QueuedSubQueue) {
    if (!sub.aiControlled && sub.userTeamIdx == teamIdx) return true;
  }
  return false;
}

bool QueueSubHasAiForTeam(int teamIdx) {
  boost::mutex::scoped_lock lock(g_QueuedSubMutex);
  for (const QueuedSub &sub : g_QueuedSubQueue) {
    if (sub.aiControlled && sub.teamIdx == teamIdx) return true;
  }
  return false;
}

bool SubGraphicPeekForRender(double now, SubGraphic &graphic) {
  boost::mutex::scoped_lock lock(g_SubGraphicMutex);
  if (g_SubGraphicQueue.empty()) return false;
  if (g_SubGraphicQueue.front().startTime < 0.0) g_SubGraphicQueue.front().startTime = now;
  graphic = g_SubGraphicQueue.front();
  return true;
}

void SubGraphicPopFront() {
  boost::mutex::scoped_lock lock(g_SubGraphicMutex);
  if (!g_SubGraphicQueue.empty()) g_SubGraphicQueue.erase(g_SubGraphicQueue.begin());
}

void SubGraphicPush(const SubGraphic &graphic) {
  boost::mutex::scoped_lock lock(g_SubGraphicMutex);
  g_SubGraphicQueue.push_back(graphic);
}

void SubGraphicClear() {
  boost::mutex::scoped_lock lock(g_SubGraphicMutex);
  g_SubGraphicQueue.clear();
}

// Panel drag positions — declared here so ResetMatchOverlayState() can reach them.
static float s_statsPanelX   = -1.f;
static float s_statsPanelY   = -1.f;
static bool  s_statsCompact  = false;
static float s_tacticsPanelX = -1.f;
static float s_tacticsPanelY = -1.f;

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
static void ResetPauseCache();           // forward declaration
static void InitHUDDataIfNeeded(Match*); // forward declaration

// ---------------------------------------------------------------------------
// Tactic instruction table — used by InitHUDDataIfNeeded and RenderTacticsPanel.
// ---------------------------------------------------------------------------
struct TacInstRow {
  const char *key;
  const char *name;
  const char *category;
  struct Preset { const char *label; float value; } presets[4];
};
static const TacInstRow kMatchTacInstr[] = {
  { "position_offense_depth_factor",       "Attacking Depth",    "Attacking",
    {{"Compact",0.25f},{"Balanced",0.5f},{"Expansive",0.75f},{"Total Attack",1.0f}} },
  { "position_offense_width_factor",       "Attacking Width",    "Attacking",
    {{"Narrow",0.3f},{"Balanced",0.6f},{"Wide",0.8f},{"Full Width",1.0f}} },
  { "position_offense_midfieldfocus",      "Midfield in Attack", "Attacking",
    {{"Hold Shape",0.2f},{"Balanced",0.45f},{"Join Attack",0.7f},{"All Forward",0.9f}} },
  { "position_offense_sidefocus_strength", "Flank Play",         "Attacking",
    {{"Central",0.1f},{"Mixed",0.35f},{"Wide Threat",0.6f},{"Wing Ovrlds",0.9f}} },
  { "position_offense_microfocus_strength","Attacking Pressing", "Attacking",
    {{"Loose",0.2f},{"Balanced",0.5f},{"Tight",0.75f},{"Swarm",0.95f}} },
  { "position_defense_depth_factor",       "Defensive Line",     "Defending",
    {{"Deep Block",0.3f},{"Mid Block",0.55f},{"High Line",0.75f},{"Ultra High",0.95f}} },
  { "position_defense_width_factor",       "Defensive Shape",    "Defending",
    {{"Narrow",0.3f},{"Balanced",0.55f},{"Wide",0.8f},{"Spread",1.0f}} },
  { "position_defense_midfieldfocus",      "Midfield Pressure",  "Defending",
    {{"Drop Deep",0.2f},{"Compact",0.45f},{"Press High",0.7f},{"Extreme Press",0.9f}} },
  { "position_defense_sidefocus_strength", "Flank Coverage",     "Defending",
    {{"Narrow",0.1f},{"Balanced",0.4f},{"Cover Wings",0.65f},{"Full Width",0.9f}} },
  { "position_defense_microfocus_strength","Def. Compactness",   "Defending",
    {{"Loose",0.2f},{"Solid",0.5f},{"Compact",0.75f},{"Max Compact",0.95f}} },
  { "dribble_offensiveness",               "Dribble Rate",       "On the Ball",
    {{"Cautious",0.2f},{"Balanced",0.5f},{"Direct",0.7f},{"Expressive",0.9f}} },
  { "dribble_centermagnet",                "Dribble Direction",  "On the Ball",
    {{"Hug Flanks",0.1f},{"Mixed",0.4f},{"Thru Middle",0.7f},{"Central Drive",0.9f}} },
};
static const int kNumMatchTacInstr = 12;

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

// Per-frame hit boxes populated by draw functions, consumed by interaction logic.
struct HitEntry { ImVec2 pos; int idx; };
static std::vector<HitEntry> s_subHits;
static std::vector<HitEntry> s_pitchHits;
static int  s_dragSubIdx = -1;
static int  s_hoverOnIdx = -1;

void ResetMatchOverlayState() {
  s_lastMatch        = nullptr;
  s_overlayLogged    = false;
  s_scoreboardHidden = false;
  s_dragSubIdx       = -1;
  s_hoverOnIdx       = -1;
  s_subHits.clear();
  s_pitchHits.clear();
  g_SubsUsed      = 0;
  g_WindowsUsed   = 0;
  g_SubWindowOpen = false;
  g_MatchStatsVisible     = false;
  g_TacticsPanelVisible   = false;
  g_PendingTacticsChanges.clear();
  s_tacticsPanelX         = -1.f;
  s_tacticsPanelY         = -1.f;
}

// ---------------------------------------------------------------------------
// Player data used for formation board and substitution UI.
// Defined here (before RenderImGuiMatchOverlay) so the always-on HUD can use it.

struct PausePlayer {
  std::string lastName;
  std::string role;        // "GK", "CB", "LB", etc. (formation position)
  float nx       = 0.f;   // [0,1] screen x pre-mapped with engine formula
  float ny       = 0.f;   // [0,1] screen y — 0=top(attack), 1=bottom(GK)
  int   jerseyNumber = 0;
  int   playerDbId   = -1;
};

struct BenchPlayer {
  std::string lastName;
  std::string role;
  int         playersIdx    = 0;
  int         jerseyNumber  = 0;
  int         playerDbId    = -1;
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

static std::vector<BenchPlayer>  s_benchPlayers;       // user team subs
static std::vector<BenchPlayer>  s_awayBenchPlayers;   // opponent subs
static int                       s_subUserTeamIdx   = 0;
static std::string               s_userBadgePath;
static std::string               s_oppBadgePath;

// Live goal log: populated under matchRenderMutex each frame.
struct GoalEntry { int minute; std::string scorer; int teamIdx; };
static std::vector<GoalEntry>    s_goalLog;
static int                       s_lastKnownGoals[2] = {0, 0};

static bool                      s_subPanelActive   = false;
static int                       s_subOffSelected   = -1;
static int                       s_subOnSelected    = -1;

// Live tactic values read from TeamData under matchRenderMutex, updated on preset click.
static std::map<std::string, float> s_liveTactics;

static int CountQueuedSubsForTeam(int teamIdx) {
  return QueueSubCountUserForTeam(teamIdx);
}

static bool HasUserQueuedSubForTeam(int teamIdx) {
  return QueueSubHasUserForTeam(teamIdx);
}

static std::string NormalizeBadgePath(std::string raw) {
  const std::string kPfx = "databases/default/";
  if (raw.substr(0, kPfx.size()) == kPfx) raw = raw.substr(kPfx.size());
  return raw;
}

static bool CanQueueSubForTeam(int teamIdx) {
  int queuedForTeam = CountQueuedSubsForTeam(teamIdx);
  if (g_SubsUsed + queuedForTeam >= 5) return false;
  return g_WindowsUsed < 3 || g_SubWindowOpen || queuedForTeam > 0;
}

static void ResetPauseCache() {
  s_pauseInitialized = false;
  s_pauseTeamName.clear();
  s_pausePlayers.clear();
  s_pauseAwayTeamName.clear();
  s_pauseAwayPlayers.clear();
  s_benchPlayers.clear();
  s_awayBenchPlayers.clear();
  s_userBadgePath.clear();
  s_oppBadgePath.clear();
  s_goalLog.clear();
  s_lastKnownGoals[0] = 0;
  s_lastKnownGoals[1] = 0;
  s_subPanelActive      = false;
  s_subOffSelected      = -1;
  s_subOnSelected       = -1;
  s_liveTactics.clear();
  g_TopBarSoftPause     = false;
  QueueSubClear();
  SubGraphicClear();
}

// Landscape mini-pitch (horizontal) for the bottom HUD.
static void DrawMiniPitchHorizontal(ImDrawList *dl, ImVec2 min, ImVec2 max) {
  const ImU32 grassDark  = IM_COL32(28,  90,  40, 255);
  const ImU32 grassLight = IM_COL32(32, 105,  46, 255);
  const ImU32 lineCol    = IM_COL32(255, 255, 255, 180);
  float w = max.x - min.x;
  float h = max.y - min.y;
  int stripes = 7;
  for (int i = 0; i < stripes; i++) {
    float sy = min.y + h * i / stripes;
    float ey = min.y + h * (i + 1) / stripes;
    dl->AddRectFilled(ImVec2(min.x, sy), ImVec2(max.x, ey),
                      (i & 1) ? grassDark : grassLight);
  }
  dl->AddRect(min, max, lineCol, 0.0f, 0, 1.5f);
  float midX = min.x + w * 0.5f;
  float cy2  = min.y + h * 0.5f;
  dl->AddLine(ImVec2(midX, min.y), ImVec2(midX, max.y), lineCol, 1.5f);
  dl->AddCircle(ImVec2(midX, cy2), h * 0.20f, lineCol, 24, 1.5f);
  float gbH = h * 0.50f;
  float gbW = w * 0.07f;
  float gbY = min.y + (h - gbH) * 0.5f;
  dl->AddRect(ImVec2(min.x,     gbY), ImVec2(min.x + gbW, gbY + gbH), lineCol, 0.0f, 0, 1.2f);
  dl->AddRect(ImVec2(max.x-gbW, gbY), ImVec2(max.x,       gbY + gbH), lineCol, 0.0f, 0, 1.2f);
}

// Draw a stylised jersey shape centred at (cx, cy).
static void DrawJersey(ImDrawList *dl, float cx, float cy,
                       ImU32 bodyColor, ImU32 numColor, int jerseyNum) {
  const float bw = 20.0f, bh = 22.0f, sw = 6.0f, sh = 9.0f, r = 2.5f;
  float bx = cx - bw * 0.5f;
  float by = cy - bh * 0.5f;
  dl->AddRectFilled(ImVec2(bx - sw + 1, by + 1), ImVec2(bx + 2,           by + sh), bodyColor, r);
  dl->AddRectFilled(ImVec2(bx + bw - 2, by + 1), ImVec2(bx + bw + sw - 1, by + sh), bodyColor, r);
  dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + bh), bodyColor, r);
  dl->AddRectFilled(ImVec2(cx - 4, by), ImVec2(cx + 4, by + 4), IM_COL32(0, 0, 0, 60));
  if (jerseyNum > 0) {
    char buf[8]; snprintf(buf, sizeof(buf), "%d", jerseyNum);
    ImFont *f = g_ManagerFontBold ? g_ManagerFontBold : ImGui::GetFont();
    float fs = 10.0f;
    ImVec2 ns = f->CalcTextSizeA(fs, FLT_MAX, 0.f, buf);
    dl->AddText(f, fs, ImVec2(cx - ns.x * 0.5f, cy + bh * 0.15f - ns.y * 0.5f), numColor, buf);
  }
}

// Horizontal formation board for the always-on bottom HUD.
// No team name header — just the pitch with jerseys.
// flipGK=false → GK on left (home); flipGK=true → GK on right (away).
static void DrawFormationHorizontal(
    ImDrawList *dl, ImVec2 origin, float w, float h,
    const std::string & /*teamName*/,
    ImU32 teamColor, ImU32 /*teamTextColor*/,
    const std::vector<PausePlayer> &players, bool flipGK,
    bool collectHits = false)
{
  const float padX = 4.0f;
  const float padY = 4.0f;

  ImVec2 pMin = ImVec2(origin.x + padX, origin.y + padY);
  ImVec2 pMax = ImVec2(origin.x + w - padX, origin.y + h - padY);
  DrawMiniPitchHorizontal(dl, pMin, pMax);
  float pW = pMax.x - pMin.x;
  float pH = pMax.y - pMin.y;

  // Extra horizontal margin so text never overlaps the pitch border lines.
  // Name can be ~7 chars × ~5px = ~35px wide, so 18px centering clearance minimum.
  const float kHMargin = 22.0f;
  const float kVTop    = 16.0f;
  const float kVBot    = 40.0f;  // extra bottom margin keeps RB/RM text inside pitch

  // Compute all positions first so we can run an anti-collision pass.
  struct PlacedPlayer {
    float px, py;
    const PausePlayer *pp;
  };
  std::vector<PlacedPlayer> placed;
  placed.reserve(11);

  for (size_t i = 0; i < players.size() && i < 11; i++) {
    const PausePlayer &pp = players[i];
    float t  = flipGK ? pp.ny : (1.0f - pp.ny);
    float px = pMin.x + t * pW;
    float py = pMin.y + pp.nx * pH;
    if (px < pMin.x + kHMargin) px = pMin.x + kHMargin;
    if (px > pMax.x - kHMargin) px = pMax.x - kHMargin;
    if (py < pMin.y + kVTop)    py = pMin.y + kVTop;
    if (py > pMax.y - kVBot)    py = pMax.y - kVBot;
    placed.push_back({px, py, &pp});
  }

  // Anti-collision: players at similar depth (px) that are too close vertically
  // get nudged apart. Run a few passes so chains resolve.
  const float kMinPyDist = 36.0f;  // minimum py gap between two players at same depth
  for (int pass = 0; pass < 4; pass++) {
    for (size_t a = 0; a < placed.size(); a++) {
      for (size_t b = a + 1; b < placed.size(); b++) {
        if (fabsf(placed[a].px - placed[b].px) > pW * 0.25f) continue; // different depth rows
        float dy = placed[b].py - placed[a].py;
        if (fabsf(dy) < kMinPyDist) {
          float push = (kMinPyDist - fabsf(dy)) * 0.5f + 1.0f;
          if (dy >= 0.0f) { placed[a].py -= push; placed[b].py += push; }
          else            { placed[a].py += push; placed[b].py -= push; }
          // Re-clamp
          if (placed[a].py < pMin.y + kVTop)  placed[a].py = pMin.y + kVTop;
          if (placed[a].py > pMax.y - kVBot)  placed[a].py = pMax.y - kVBot;
          if (placed[b].py < pMin.y + kVTop)  placed[b].py = pMin.y + kVTop;
          if (placed[b].py > pMax.y - kVBot)  placed[b].py = pMax.y - kVBot;
        }
      }
    }
  }

  // Collect starting XI hit boxes for drag/drop (user team only)
  if (collectHits) {
    for (size_t i = 0; i < placed.size(); i++)
      s_pitchHits.push_back({ImVec2(placed[i].px, placed[i].py), (int)i});
  }

  // Helper: draw text centered at cx with a 1px drop shadow.
  auto ShadowText = [&](float cx, float ty, float fs, ImU32 col, const char *txt) {
    ImFont *f = g_ManagerFontBold;
    ImVec2 ts = f ? f->CalcTextSizeA(fs, FLT_MAX, 0.f, txt) : ImGui::CalcTextSize(txt);
    float tx = cx - ts.x * 0.5f;
    if (f) dl->AddText(f, fs, ImVec2(tx + 1, ty + 1), IM_COL32(0, 0, 0, 200), txt);
    else   dl->AddText(ImVec2(tx + 1, ty + 1),         IM_COL32(0, 0, 0, 200), txt);
    if (f) dl->AddText(f, fs, ImVec2(tx, ty), col, txt);
    else   dl->AddText(ImVec2(tx, ty),         col, txt);
  };

  ImU32 numCol = TextColorForBg(teamColor);
  for (const PlacedPlayer &pl : placed) {
    DrawJersey(dl, pl.px, pl.py, teamColor, numCol, pl.pp->jerseyNumber);
    if (!pl.pp->role.empty())
      ShadowText(pl.px, pl.py + 14.0f, 10.0f, IM_COL32(200, 225, 255, 245), pl.pp->role.c_str());
    std::string abbr = pl.pp->lastName.size() > 8 ? pl.pp->lastName.substr(0, 8) : pl.pp->lastName;
    ShadowText(pl.px, pl.py + 25.0f, 10.0f, IM_COL32(255, 255, 255, 230), abbr.c_str());
  }
}

// 3-column substitutes grid.
// Layout (9 subs example, right column = first off bench):
//   SUB9  SUB6  SUB3
//   SUB8  SUB5  SUB2
//   SUB7  SUB4  SUB1
static void DrawSubsList(ImDrawList *dl, ImVec2 origin, float w, float h,
                         ImU32 teamColor,
                         const std::vector<BenchPlayer> &bench,
                         bool interactive = false)
{
  if (bench.empty()) return;

  const int numCols = 3;
  int numSubs = (int)bench.size();
  int numRows = (numSubs + numCols - 1) / numCols;

  const float kPadX = 6.0f;
  const float kPadY = 8.0f;
  float cellW  = w / (float)numCols;
  float cellH  = h / (float)numRows;
  float drawW  = cellW - kPadX;
  float drawH  = cellH - kPadY;

  // Font size: scale with cell but cap so block always fits
  float fSize = drawH * 0.18f;
  if (fSize < 7.0f)  fSize = 7.0f;
  if (fSize > 22.0f) fSize = 22.0f;
  float lineH = fSize + 1.5f;

  // Jersey height: must leave room for 2 name lines + spacing inside drawH
  // blockH = jBH + 3 + lineH*2  <=  drawH
  float jBH_byH = drawH - 3.0f - lineH * 2.0f;
  float jBH_byW = drawW / 1.42f;  // total jersey+sleeve width = jBH * 1.42
  float jBH   = jBH_byH < jBH_byW ? jBH_byH : jBH_byW;
  if (jBH < 10.0f) jBH = 10.0f;
  float jBW   = jBH  * 0.90f;
  float jSW   = jBH  * 0.26f;
  float jSH   = jBH  * 0.40f;
  float jR    = jBH  * 0.10f;

  ImU32 numCol = TextColorForBg(teamColor);
  ImFont *f    = g_ManagerFontBold;

  for (int row = 0; row < numRows; row++) {
    for (int col = 0; col < numCols; col++) {
      // Right column = first off bench (lowest index)
      int idx = (numCols - 1 - col) * numRows + (numRows - 1 - row);
      if (idx < 0 || idx >= numSubs) continue;

      const BenchPlayer &bp = bench[idx];

      float cx = origin.x + col * cellW + cellW * 0.5f;
      float cy = origin.y + row * cellH  + cellH * 0.5f;

      // Record jersey centre for drag/drop hit testing (user's bench only)
      float jcyHit = cy - (jBH + 3.0f + lineH * 2.0f) * 0.5f + jBH * 0.5f;
      if (interactive) s_subHits.push_back({ImVec2(cx, jcyHit), idx});

      // Clip everything to this cell so nothing bleeds into neighbours
      ImVec2 clipMin(origin.x + col * cellW + 2.0f,       origin.y + row * cellH + 2.0f);
      ImVec2 clipMax(origin.x + (col + 1) * cellW - 2.0f, origin.y + (row + 1) * cellH - 2.0f);
      dl->PushClipRect(clipMin, clipMax, true);

      // Block always fits in drawH: jBH sized to leave room for 2 name lines
      float blockH = jBH + 3.0f + lineH * 2.0f;
      float jcy    = cy - blockH * 0.5f + jBH * 0.5f;
      float bx     = cx - jBW * 0.5f;
      float by     = jcy - jBH * 0.5f;

      // Dim jersey when being dragged (ghost follows cursor instead)
      ImU32 drawColor = (idx == s_dragSubIdx)
        ? IM_COL32((teamColor>>0)&0xFF, (teamColor>>8)&0xFF, (teamColor>>16)&0xFF, 80)
        : teamColor;

      // Jersey body + sleeves
      dl->AddRectFilled(ImVec2(bx - jSW + 1, by + 1), ImVec2(bx + 2,           by + jSH), drawColor, jR);
      dl->AddRectFilled(ImVec2(bx + jBW - 2, by + 1), ImVec2(bx + jBW + jSW - 1, by + jSH), drawColor, jR);
      dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + jBW, by + jBH), drawColor, jR);

      // Jersey number — outlined for contrast against any kit colour
      if (bp.jerseyNumber > 0 && f) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", bp.jerseyNumber);
        ImVec2 ns = f->CalcTextSizeA(fSize, FLT_MAX, 0.f, buf);
        float nx = cx - ns.x * 0.5f;
        float ny_num = jcy - ns.y * 0.5f + jBH * 0.1f;
        ImU32 stroke = IM_COL32(0, 0, 0, 180);
        dl->AddText(f, fSize, ImVec2(nx-1, ny_num  ), stroke, buf);
        dl->AddText(f, fSize, ImVec2(nx+1, ny_num  ), stroke, buf);
        dl->AddText(f, fSize, ImVec2(nx,   ny_num-1), stroke, buf);
        dl->AddText(f, fSize, ImVec2(nx,   ny_num+1), stroke, buf);
        dl->AddText(f, fSize, ImVec2(nx,   ny_num  ), numCol, buf);
      }

      // Outlined text helper: 4-direction black stroke + bright white fill
      auto OutlineText = [&](ImFont *font, float fs, float ox, float oy, const char *txt) {
        ImU32 stroke = IM_COL32(0, 0, 0, 220);
        ImU32 fill   = IM_COL32(255, 255, 255, 255);
        dl->AddText(font, fs, ImVec2(ox - 1, oy    ), stroke, txt);
        dl->AddText(font, fs, ImVec2(ox + 1, oy    ), stroke, txt);
        dl->AddText(font, fs, ImVec2(ox,     oy - 1), stroke, txt);
        dl->AddText(font, fs, ImVec2(ox,     oy + 1), stroke, txt);
        dl->AddText(font, fs, ImVec2(ox,     oy    ), fill,   txt);
      };

      // Name below jersey — word-wrap to 2 lines if too wide; shrink only if no spaces
      const std::string &nameStr = bp.lastName;
      float ny = jcy + jBH * 0.5f + 3.0f;
      if (f) {
        ImVec2 ns = f->CalcTextSizeA(fSize, FLT_MAX, 0.f, nameStr.c_str());
        if (ns.x <= drawW) {
          float nx = cx - ns.x * 0.5f;
          OutlineText(f, fSize, nx, ny, nameStr.c_str());
        } else {
          // Find the last space where the prefix still fits in drawW
          std::string line1, line2;
          size_t splitPos = std::string::npos;
          for (size_t i = 0; i < nameStr.size(); i++) {
            if (nameStr[i] == ' ') {
              std::string candidate = nameStr.substr(0, i);
              ImVec2 cs = f->CalcTextSizeA(fSize, FLT_MAX, 0.f, candidate.c_str());
              if (cs.x <= drawW) splitPos = i;
            }
          }
          if (splitPos != std::string::npos) {
            line1 = nameStr.substr(0, splitPos);
            line2 = nameStr.substr(splitPos + 1);
          } else {
            // No usable space — shrink to single line
            float shrunk = fSize * drawW / ns.x;
            if (shrunk < 7.0f) shrunk = 7.0f;
            ns = f->CalcTextSizeA(shrunk, FLT_MAX, 0.f, nameStr.c_str());
            OutlineText(f, shrunk, cx - ns.x * 0.5f, ny, nameStr.c_str());
            line1.clear();
          }
          if (!line1.empty()) {
            ImVec2 ns1 = f->CalcTextSizeA(fSize, FLT_MAX, 0.f, line1.c_str());
            OutlineText(f, fSize, cx - ns1.x * 0.5f, ny, line1.c_str());
            float f2 = fSize;
            ImVec2 ns2 = f->CalcTextSizeA(f2, FLT_MAX, 0.f, line2.c_str());
            if (ns2.x > drawW) {
              f2  = f2 * drawW / ns2.x;
              if (f2 < 7.0f) f2 = 7.0f;
              ns2 = f->CalcTextSizeA(f2, FLT_MAX, 0.f, line2.c_str());
            }
            OutlineText(f, f2, cx - ns2.x * 0.5f, ny + lineH, line2.c_str());
          }
        }
      }

      dl->PopClipRect();
    }
  }
}

// ---------------------------------------------------------------------------
// Forward declarations — defined after RenderImGuiMatchOverlay.
void RenderMatchStatsPanel();
void RenderTacticsPanel();
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

  // Populate HUD cache while we hold the mutex (TeamData is safe to read here).
  InitHUDDataIfNeeded(match);

  // Track goals live — detect new goals and log scorer + minute.
  for (int ti = 0; ti < 2; ti++) {
    int cur = match->GetScore(ti);
    if (cur > s_lastKnownGoals[ti]) {
      int minute = (int)(match->GetMatchTime_ms() / 60000) + 1;
      std::string scorerName;
      if (match->GetLastGoalTeamID() == ti) {
        Player *sc = match->GetLastGoalScorer();
        if (sc && sc->GetPlayerData()) scorerName = sc->GetPlayerData()->GetDisplayName();
      }
      // Map raw team index to user/opp for consistent display
      GoalEntry ge;
      ge.minute   = minute;
      ge.scorer   = scorerName;
      ge.teamIdx  = ti; // raw engine team index; resolved against s_subUserTeamIdx at draw time
      s_goalLog.push_back(ge);
      s_lastKnownGoals[ti] = cur;
    }
  }

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

  // ---- Substitution banner (BBC Sport style) --------------------------------
  {
    SubGraphic g_SubGraphic;
    const double now = ImGui::GetTime();
    if (SubGraphicPeekForRender(now, g_SubGraphic)) {
      const double elapsed   = now - g_SubGraphic.startTime;
      const double kDuration = 5.5;
      if (elapsed >= kDuration) {
        SubGraphicPopFront();
      } else {
      float alpha = 1.0f;
      if (elapsed < 0.3) alpha = (float)(elapsed / 0.3f);           // fade in
      if (elapsed > kDuration - 0.8) alpha = (float)((kDuration - elapsed) / 0.8);
      if (alpha < 0.f) alpha = 0.f;
      const ImGuiIO &io = ImGui::GetIO();
      const float sw = io.DisplaySize.x, sh = io.DisplaySize.y;

      // Dimensions — BBC style: wide card, yellow header, two player rows, badges flanking
      const float bw   = 460.0f;
      const float hdrH = 34.0f;
      const float rowH = 36.0f;
      const float bh   = hdrH + rowH * 2.0f;
      const float bx   = (sw - bw) * 0.5f;
      const float by   = 60.0f;
      const float badgeW = 54.0f;
      const float innerX = bx + badgeW;
      const float innerW = bw - badgeW * 2.0f;

      auto A = [&](int v) { return (int)(v * alpha); };

      // Shadow
      dl->AddRectFilled(ImVec2(bx + 4, by + 4), ImVec2(bx + bw + 4, by + bh + 4),
                        IM_COL32(0, 0, 0, A(80)));

      // Header bar — same navy as scoreboard league logo area
      dl->AddRectFilled(ImVec2(bx, by), ImVec2(bx + bw, by + hdrH),
                        IM_COL32(12, 20, 55, A(248)));
      // Header text — "SUBSTITUTION" in white
      const char *hdrTxt = "SUBSTITUTION";
      ImVec2 htsz = g_ManagerFontBold
          ? g_ManagerFontBold->CalcTextSizeA(15.0f, FLT_MAX, 0.f, hdrTxt) : ImVec2(100,15);
      dl->AddText(g_ManagerFontBold, 15.0f,
                  ImVec2(bx + bw * 0.5f - htsz.x * 0.5f, by + (hdrH - htsz.y) * 0.5f),
                  IM_COL32(255, 255, 255, A(255)), hdrTxt);

      // Player rows background — white; text — black
      const ImU32 rowBgCol  = IM_COL32(255, 255, 255, A(255));
      const ImU32 rowTxtCol = IM_COL32(15,  15,  15,  A(255));
      dl->AddRectFilled(ImVec2(bx, by + hdrH), ImVec2(bx + bw, by + bh), rowBgCol);

      // Divider between the two player rows
      float midY = by + hdrH + rowH;
      ImU32 divCol = (ImU32)((rowTxtCol & 0x00FFFFFF) | ((unsigned int)A(80) << 24));
      dl->AddLine(ImVec2(innerX + 2, midY), ImVec2(innerX + innerW - 2, midY), divCol, 1.0f);

      // League logo (left side, spanning both rows)
      {
        static GLuint s_subLeagueTex = 0;
        static std::string s_subLeaguePath;
        std::string leagueLogoPath = g_SubGraphic.leagueLogoPath.empty()
                                       ? s_leagueLogoPath
                                       : g_SubGraphic.leagueLogoPath;
        if (s_subLeaguePath != leagueLogoPath) {
          s_subLeagueTex  = 0;
          s_subLeaguePath = leagueLogoPath;
        }
        if (s_subLeagueTex == 0 && !s_subLeaguePath.empty())
          s_subLeagueTex = LoadBadgeTex(s_subLeaguePath);
        if (s_subLeagueTex) {
          const float pad = 8.0f;
          const float sz  = rowH * 2.0f - pad * 2.0f;
          dl->AddRectFilled(ImVec2(bx, by + hdrH), ImVec2(bx + badgeW, by + bh),
                            rowBgCol);
          dl->AddImage((ImTextureID)(intptr_t)s_subLeagueTex,
                       ImVec2(bx + (badgeW - sz) * 0.5f, by + hdrH + pad),
                       ImVec2(bx + (badgeW + sz) * 0.5f, by + hdrH + pad + sz),
                       ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255,A(255)));
        }
      }

      // Club badge (right side, spanning both rows)
      {
        static GLuint s_subBadgeTex = 0;
        static std::string s_subBadgePath;
        if (s_subBadgePath != g_SubGraphic.teamBadgePath) {
          s_subBadgeTex  = 0;
          s_subBadgePath = g_SubGraphic.teamBadgePath;
        }
        if (s_subBadgeTex == 0 && !s_subBadgePath.empty())
          s_subBadgeTex = LoadBadgeTex(s_subBadgePath);
        if (s_subBadgeTex) {
          const float pad = 7.0f;
          const float sz  = rowH * 2.0f - pad * 2.0f;
          float rx = bx + bw - badgeW;
          dl->AddRectFilled(ImVec2(rx, by + hdrH), ImVec2(bx + bw, by + bh),
                            rowBgCol);
          dl->AddImage((ImTextureID)(intptr_t)s_subBadgeTex,
                       ImVec2(rx + (badgeW - sz) * 0.5f, by + hdrH + pad),
                       ImVec2(rx + (badgeW + sz) * 0.5f, by + hdrH + pad + sz),
                       ImVec2(0,0), ImVec2(1,1), IM_COL32(255,255,255,A(255)));
        }
      }

      // OUT row — red left-pointing triangle + name
      auto DrawPlayerRow = [&](float ry, const char *name, bool isOut) {
        ImU32 arrowCol = isOut ? IM_COL32(210, 30, 30, A(255)) : IM_COL32(30, 160, 60, A(255));
        ImU32 txtCol   = rowTxtCol;
        // Arrow triangle
        const float arrowSz = 12.0f;
        float ax = innerX + 14.0f;
        float ay = ry + rowH * 0.5f;
        if (isOut) { // left-pointing ◄
          dl->AddTriangleFilled(ImVec2(ax,             ay),
                                ImVec2(ax + arrowSz,   ay - arrowSz * 0.6f),
                                ImVec2(ax + arrowSz,   ay + arrowSz * 0.6f),
                                arrowCol);
        } else {     // right-pointing ►
          dl->AddTriangleFilled(ImVec2(ax + arrowSz,   ay),
                                ImVec2(ax,             ay - arrowSz * 0.6f),
                                ImVec2(ax,             ay + arrowSz * 0.6f),
                                arrowCol);
        }
        // Name
        if (g_ManagerFontBold) {
          ImVec2 nsz = g_ManagerFontBold->CalcTextSizeA(15.0f, FLT_MAX, 0.f, name);
          float nx = innerX + (innerW - nsz.x) * 0.5f + 8.0f; // slight right offset for arrow
          float ny = ry + (rowH - nsz.y) * 0.5f;
          dl->AddText(g_ManagerFontBold, 15.0f, ImVec2(nx, ny), txtCol, name);
        }
      };

      DrawPlayerRow(by + hdrH,        g_SubGraphic.nameOut.c_str(), true);
      DrawPlayerRow(by + hdrH + rowH, g_SubGraphic.nameIn.c_str(),  false);

      // Outer border
      dl->AddRect(ImVec2(bx, by), ImVec2(bx + bw, by + bh),
                  IM_COL32(12, 20, 55, A(200)), 0.0f, 0, 1.5f);
      }
    }
  }

  // ---- Always-on top-right controls bar ------------------------------------
  {
    const ImGuiIO &io2 = ImGui::GetIO();
    const float sw2  = io2.DisplaySize.x;
    const float btnH = 28.0f;
    const float iconW = 32.0f;
    const float spdW  = 34.0f;
    const float gap   = 4.0f;
    const float marginR = 16.0f;  // right margin from screen edge
    const float marginT = 12.0f;  // top margin from screen edge
    // Actual width = play(iconW+gap) + pause(iconW+gap) + extraGap
    //              + 4*(spdW+gap) + extraGap + settings(iconW)
    // = 3*iconW + 8*gap + 4*spdW
    float totalW = iconW * 3 + gap * 8 + spdW * 4;
    float barX   = sw2 - totalW - marginR;
    float barY2  = marginT;
    float winX   = barX - 6.0f;
    float winY   = barY2 - 4.0f;
    float winW   = totalW + 14.0f;  // extra padding so settings button is not clipped
    float winH   = btnH + 8.0f;

    ImGui::SetNextWindowPos(ImVec2(winX, winY), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##topbar", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *tbdl = ImGui::GetWindowDrawList();

    // Background pill drawn in window draw list (behind buttons)
    tbdl->AddRectFilled(ImVec2(winX, winY), ImVec2(winX + winW, winY + winH),
                        IM_COL32(10, 14, 28, 220), 8.0f);

    auto gameTaskForPause = GetGameTask();
    Match *matchForPause = gameTaskForPause ? gameTaskForPause->GetMatch() : nullptr;
    bool isPaused = g_TopBarSoftPause || g_ImGuiIngamePauseMenuActive ||
                    (matchForPause && matchForPause->GetPause());
    float cx2 = barX;
    float cy2 = barY2;

    // Generic icon button — draws bg rect, returns click, advances cx2
    auto IconBtn = [&](const char *id, bool active, bool enabled) -> bool {
      ImVec2 bmin(cx2, cy2);
      ImVec2 bmax(cx2 + iconW, cy2 + btnH);
      ImGui::SetCursorScreenPos(bmin);
      ImGui::PushID(id);
      bool clicked = ImGui::InvisibleButton(id, ImVec2(iconW, btnH)) && enabled;
      ImGui::PopID();
      bool hov = ImGui::IsItemHovered();
      ImU32 bg = active ? IM_COL32(255, 200, 0, 255)
               : hov    ? IM_COL32(50, 65, 110, 255)
                        : IM_COL32(22, 32, 60, 200);
      tbdl->AddRectFilled(bmin, bmax, bg, 5.0f);
      cx2 += iconW + gap;
      return clicked;
    };

    // Helpers: draw icons at the PREVIOUS button position (cx2 was advanced already)
    auto DrawPlay = [&](bool active) {
      float bx = cx2 - iconW - gap, by = cy2;
      ImU32 col = active ? IM_COL32(15,15,15,255) : IM_COL32(200,215,255,230);
      float mx = bx + iconW * 0.5f + 2.0f, my = by + btnH * 0.5f;
      tbdl->AddTriangleFilled(ImVec2(mx - 7, my - 7), ImVec2(mx - 7, my + 7),
                              ImVec2(mx + 7, my), col);
    };
    auto DrawPause = [&](bool active) {
      float bx = cx2 - iconW - gap, by = cy2;
      ImU32 col = active ? IM_COL32(15,15,15,255) : IM_COL32(200,215,255,230);
      float mx = bx + iconW * 0.5f, my = by + btnH * 0.5f;
      float rw = 5.0f, rh = 11.0f;
      tbdl->AddRectFilled(ImVec2(mx - rw - 2, my - rh*0.5f),
                          ImVec2(mx - 2,       my + rh*0.5f), col, 1.5f);
      tbdl->AddRectFilled(ImVec2(mx + 2,       my - rh*0.5f),
                          ImVec2(mx + 2 + rw,  my + rh*0.5f), col, 1.5f);
    };
    auto DrawSettings = [&]() {
      float bx = cx2 - iconW - gap, by = cy2;
      ImU32 col = IM_COL32(200, 215, 255, 230);
      float mx = bx + iconW * 0.5f, my = by + btnH * 0.5f;
      float lw = 13.0f, lh = 2.0f;
      tbdl->AddRectFilled(ImVec2(mx - lw*0.5f, my - 5 - lh*0.5f),
                          ImVec2(mx + lw*0.5f, my - 5 + lh*0.5f), col, 1.0f);
      tbdl->AddRectFilled(ImVec2(mx - lw*0.5f, my     - lh*0.5f),
                          ImVec2(mx + lw*0.5f, my     + lh*0.5f), col, 1.0f);
      tbdl->AddRectFilled(ImVec2(mx - lw*0.5f, my + 5 - lh*0.5f),
                          ImVec2(mx + lw*0.5f, my + 5 + lh*0.5f), col, 1.0f);
    };

    // Play button
    bool playClicked = IconBtn("##play", !isPaused, isPaused);
    if (playClicked) {
      g_TopBarSoftPause = false;
      if (matchForPause && matchForPause->GetPause()) matchForPause->Pause(false);
    }
    DrawPlay(!isPaused);

    // Pause button
    if (IconBtn("##pause", isPaused, !isPaused)) g_TopBarSoftPause = true;
    DrawPause(isPaused);

    cx2 += gap; // extra gap before speed buttons

    int curSpeed = GetConfiguration()->GetInt("match_speed_multiplier", 1);
    const int   speeds[]  = {1, 2, 4, 8};
    const char *sLabels[] = {"x1", "x2", "x4", "x8"};
    for (int i = 0; i < 4; i++) {
      ImVec2 bmin(cx2, cy2);
      ImVec2 bmax(cx2 + spdW, cy2 + btnH);
      ImGui::SetCursorScreenPos(bmin);
      char sid[16]; snprintf(sid, sizeof(sid), "##spd%d", i);
      ImGui::PushID(sid);
      bool clicked = ImGui::InvisibleButton(sid, ImVec2(spdW, btnH));
      ImGui::PopID();
      bool active = (curSpeed == speeds[i]);
      bool hov    = ImGui::IsItemHovered();
      ImU32 bg = active ? IM_COL32(255, 200, 0, 255)
               : hov    ? IM_COL32(50, 65, 110, 255)
                        : IM_COL32(22, 32, 60, 200);
      ImU32 tc = active ? IM_COL32(15, 15, 15, 255) : IM_COL32(200, 215, 255, 220);
      tbdl->AddRectFilled(bmin, bmax, bg, 5.0f);
      AddTextCentered(tbdl, g_ManagerFontBold, 13.0f, bmin, bmax, tc, sLabels[i]);
      if (clicked) GetConfiguration()->Set("match_speed_multiplier", (float)speeds[i]);
      cx2 += spdW + gap;
    }

    cx2 += gap;
    // Settings button (3 horizontal lines icon)
    if (IconBtn("##settings", false, true)) g_ImGuiTopBarPauseRequest = true;
    DrawSettings();

    ImGui::End();
    ImGui::PopStyleVar(2);
  }

  // ---- Always-on bottom HUD: formation boards flanking the engine's radar ----
  // Radar is at x=38%, y=78%, w=24%, h=18% of screen (match.cpp:316).
  // Formation boards are exactly the same size, placed left and right of it.
  if (s_pauseInitialized && !s_pausePlayers.empty()) {
    // Clear per-frame hit boxes before draw functions repopulate them
    s_subHits.clear();
    s_pitchHits.clear();
    const ImGuiIO &io3 = ImGui::GetIO();
    const float sw3 = io3.DisplaySize.x;
    const float sh3 = io3.DisplaySize.y;

    // Radar geometry (percentages from match.cpp constructor)
    const float rX = 0.38f, rY = 0.78f, rW = 0.24f, rH = 0.18f;
    float radarL = rX * sw3;
    float radarT = rY * sh3;
    float boardW = rW * sw3;
    float boardH = rH * sh3;

    // Boards hug the radar edges (negative = slight inward overlap)
    const float kBoardGap = -8.0f;
    float leftX  = radarL - boardW - kBoardGap;
    float rightX = (rX + rW) * sw3 + kBoardGap;

    // Subs panel: 3 columns, wide enough for full surnames
    const float subsW = 240.0f;

    // ---- Left formation board: user's team ---------------------------------
    {
      ImGui::SetNextWindowPos(ImVec2(leftX, radarT), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(boardW, boardH), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::Begin("##hud_left", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
      ImDrawList *ldl  = ImGui::GetWindowDrawList();
      ImVec2      lpos = ImGui::GetWindowPos();
      DrawFormationHorizontal(ldl, lpos, boardW, boardH,
                              s_pauseTeamName, s_pauseUserColor, s_pauseUserTextColor,
                              s_pausePlayers, false, /*collectHits=*/true);
      ImGui::End();
      ImGui::PopStyleVar(2);
    }

    // ---- Right formation board: opponent -----------------------------------
    {
      ImGui::SetNextWindowPos(ImVec2(rightX, radarT), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(boardW, boardH), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::Begin("##hud_right", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
      ImDrawList *rdl  = ImGui::GetWindowDrawList();
      ImVec2      rpos = ImGui::GetWindowPos();
      DrawFormationHorizontal(rdl, rpos, boardW, boardH,
                              s_pauseAwayTeamName, s_pauseOppColor, s_pauseOppTextColor,
                              s_pauseAwayPlayers, true);
      ImGui::End();
      ImGui::PopStyleVar(2);
    }

    // ---- User subs: left of user board (clamped to screen) -----------------
    if (!s_benchPlayers.empty()) {
      float subsX = leftX - subsW;
      if (subsX < 4.0f) subsX = 4.0f;
      ImGui::SetNextWindowPos(ImVec2(subsX, radarT), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(subsW, boardH), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::Begin("##subs_home", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
      ImDrawList *sdl = ImGui::GetWindowDrawList();
      ImVec2      sp  = ImGui::GetWindowPos();
      DrawSubsList(sdl, sp, subsW, boardH, s_pauseUserColor, s_benchPlayers, /*interactive=*/true);
      ImGui::End();
      ImGui::PopStyleVar(2);
    }

    // ---- Opponent subs: right of opponent board (clamped to screen) --------
    if (!s_awayBenchPlayers.empty()) {
      float awaySubsX = rightX + boardW;
      if (awaySubsX + subsW > sw3 - 4.0f) awaySubsX = sw3 - subsW - 4.0f;
      ImGui::SetNextWindowPos(ImVec2(awaySubsX, radarT), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(subsW, boardH), ImGuiCond_Always);
      ImGui::SetNextWindowBgAlpha(0.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
      ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
      ImGui::Begin("##subs_away", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
        ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
      ImDrawList *adl = ImGui::GetWindowDrawList();
      ImVec2      ap  = ImGui::GetWindowPos();
      DrawSubsList(adl, ap, subsW, boardH, s_pauseOppColor, s_awayBenchPlayers);
      ImGui::End();
      ImGui::PopStyleVar(2);
    }

    // ---- Floating tab buttons: above the radar, no background ---------------
    const char *tabLabels[] = {"MATCH FACTS", "TACTICS", "SHOUTS"};
    const float tbtnW   = 86.0f;
    const float tbtnH   = 26.0f;
    const float tbtnGap = 5.0f;
    float tabRowW = tbtnW * 3 + tbtnGap * 2;
    float tabCX   = radarL + boardW * 0.5f;  // center of radar
    float tabX0   = tabCX - tabRowW * 0.5f;
    float tabY0   = radarT - tbtnH - 10.0f;  // 10px above radar top

    ImGui::SetNextWindowPos(ImVec2(tabX0, tabY0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(tabRowW, tbtnH), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##tab_btns", nullptr,
      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
      ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImDrawList *tdl  = ImGui::GetWindowDrawList();
    ImVec2      tpos = ImGui::GetWindowPos();
    float tabX = tpos.x;
    float tabY = tpos.y;
    for (int i = 0; i < 3; i++) {
      ImVec2 bmin(tabX, tabY);
      ImVec2 bmax(tabX + tbtnW, tabY + tbtnH);
      ImGui::SetCursorScreenPos(bmin);
      char cid[16]; snprintf(cid, sizeof(cid), "##tab%d", i);
      ImGui::PushID(cid);
      bool tabClicked = ImGui::InvisibleButton(cid, ImVec2(tbtnW, tbtnH));
      ImGui::PopID();
      bool hov    = ImGui::IsItemHovered();
      bool active = (i == 0 && g_MatchStatsVisible) || (i == 1 && g_TacticsPanelVisible);
      ImU32 bg = active ? IM_COL32(255, 200, 40, 255)
               : hov    ? IM_COL32(50, 70, 120, 240)
                        : IM_COL32(18, 26, 52, 210);
      ImU32 tc = active ? IM_COL32(15, 15, 15, 255) : IM_COL32(180, 210, 255, 230);
      tdl->AddRectFilled(bmin, bmax, bg, 5.0f);
      tdl->AddRect(bmin, bmax, IM_COL32(55, 80, 140, 180), 5.0f, 0, 1.0f);
      AddTextCentered(tdl, g_ManagerFontBold, 11.0f, bmin, bmax, tc, tabLabels[i]);
      if (tabClicked && i == 0) g_MatchStatsVisible  = !g_MatchStatsVisible;
      if (tabClicked && i == 1) g_TacticsPanelVisible = !g_TacticsPanelVisible;
      tabX += tbtnW + tbtnGap;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);

    // ---- W/S budget labels + pending-sub feedback below user's board -------
    {
      ImDrawList *fdl_hud = ImGui::GetForegroundDrawList();
      ImFont     *fnt     = g_ManagerFontBold;
      const float fs      = 11.0f;
      const float labelY  = radarT + boardH + 4.0f;

      // W: X/3 — left-aligned under board left edge
      char wBuf[16]; snprintf(wBuf, sizeof(wBuf), "W: %d/3", g_WindowsUsed);
      ImU32 wCol = g_WindowsUsed >= 3 ? IM_COL32(255, 80, 80, 240) : IM_COL32(200, 220, 255, 220);
      fdl_hud->AddText(fnt, fs, ImVec2(leftX + 2.0f, labelY), wCol, wBuf);

      // S: X/5 — right-aligned under board right edge
      char sBuf[16]; snprintf(sBuf, sizeof(sBuf), "S: %d/5", g_SubsUsed);
      ImU32 sCol = g_SubsUsed >= 5 ? IM_COL32(255, 80, 80, 240) : IM_COL32(200, 220, 255, 220);
      if (fnt) {
        ImVec2 sz = fnt->CalcTextSizeA(fs, FLT_MAX, 0.f, sBuf);
        fdl_hud->AddText(fnt, fs, ImVec2(leftX + boardW - sz.x - 2.0f, labelY), sCol, sBuf);
      }

      // Pending-sub: highlight stroke around board + centered "Waiting for stoppage..." text
      if (HasUserQueuedSubForTeam(s_subUserTeamIdx)) {
        // Animated alpha pulse using time
        float t      = (float)fmod(ImGui::GetTime() * 2.0, 1.0);
        float alpha  = 0.5f + 0.5f * sinf(t * 3.14159f * 2.0f);
        ImU32 stroke = IM_COL32((s_pauseUserColor>>0)&0xFF,
                                (s_pauseUserColor>>8)&0xFF,
                                (s_pauseUserColor>>16)&0xFF,
                                (int)(180 * alpha + 75));
        fdl_hud->AddRect(ImVec2(leftX - 2, radarT - 2),
                         ImVec2(leftX + boardW + 2, radarT + boardH + 2),
                         stroke, 3.0f, 0, 3.0f);

        const char *waitTxt = "Waiting for stoppage...";
        if (fnt) {
          ImVec2 ws = fnt->CalcTextSizeA(10.0f, FLT_MAX, 0.f, waitTxt);
          float  wx = leftX + boardW * 0.5f - ws.x * 0.5f;
          float  wy = labelY + fs + 3.0f;
          fdl_hud->AddText(fnt, 10.0f, ImVec2(wx+1,wy+1), IM_COL32(0,0,0,180),     waitTxt);
          fdl_hud->AddText(fnt, 10.0f, ImVec2(wx,  wy),   IM_COL32(255,220,100,230), waitTxt);
        }
      }
    }

    // ---- Drag-and-drop substitution interaction ----------------------------
    ImDrawList *fdl2     = ImGui::GetForegroundDrawList();
    ImVec2      mouse    = ImGui::GetMousePos();
    const float kSubR    = 24.0f;   // click radius on bench jerseys
    const float kPitchR  = 30.0f;   // snap radius to pitch player

    // Find nearest bench hit
    int nearSub = -1; float nearSubD = kSubR;
    for (auto &h : s_subHits) {
      float d = hypotf(mouse.x - h.pos.x, mouse.y - h.pos.y);
      if (d < nearSubD) { nearSubD = d; nearSub = h.idx; }
    }

    // Find nearest pitch hit (only while dragging)
    int nearPitch = -1; float nearPitchD = kPitchR;
    if (s_dragSubIdx >= 0) {
      for (auto &h : s_pitchHits) {
        float d = hypotf(mouse.x - h.pos.x, mouse.y - h.pos.y);
        if (d < nearPitchD) { nearPitchD = d; nearPitch = h.idx; }
      }
    }

    // Start drag — only if budget allows
    bool budgetOk = CanQueueSubForTeam(s_subUserTeamIdx);
    if (s_dragSubIdx < 0 && budgetOk && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && nearSub >= 0)
      s_dragSubIdx = nearSub;

    // Update hover target
    if (s_dragSubIdx >= 0)
      s_hoverOnIdx = nearPitch;

    // Highlight hovered pitch player
    if (s_hoverOnIdx >= 0) {
      for (auto &h : s_pitchHits) {
        if (h.idx == s_hoverOnIdx) {
          fdl2->AddCircleFilled(h.pos, 22.0f, IM_COL32(255, 220, 50, 55));
          fdl2->AddCircle(h.pos, 22.0f, IM_COL32(255, 220, 50, 230), 24, 2.5f);
          break;
        }
      }
    }

    // Draw ghost jersey following cursor
    if (s_dragSubIdx >= 0 && s_dragSubIdx < (int)s_benchPlayers.size()) {
      const BenchPlayer &dragged = s_benchPlayers[s_dragSubIdx];
      // Slightly larger ghost jersey
      const float gBW = 28.0f, gBH = 30.0f, gSW = 8.0f, gSH = 12.0f, gR = 3.0f;
      float gbx = mouse.x - gBW * 0.5f, gby = mouse.y - gBH * 0.5f;
      fdl2->AddRectFilled(ImVec2(gbx - gSW + 1, gby + 1), ImVec2(gbx + 2,            gby + gSH), s_pauseUserColor, gR);
      fdl2->AddRectFilled(ImVec2(gbx + gBW - 2, gby + 1), ImVec2(gbx + gBW + gSW - 1, gby + gSH), s_pauseUserColor, gR);
      fdl2->AddRectFilled(ImVec2(gbx, gby), ImVec2(gbx + gBW, gby + gBH), s_pauseUserColor, gR);
      // Number on ghost
      if (dragged.jerseyNumber > 0 && g_ManagerFontBold) {
        char buf[8]; snprintf(buf, sizeof(buf), "%d", dragged.jerseyNumber);
        ImVec2 ns = g_ManagerFontBold->CalcTextSizeA(11.0f, FLT_MAX, 0.f, buf);
        fdl2->AddText(g_ManagerFontBold, 11.0f,
          ImVec2(mouse.x - ns.x * 0.5f, mouse.y + gBH * 0.1f - ns.y * 0.5f),
          TextColorForBg(s_pauseUserColor), buf);
      }
      // Name below ghost
      if (g_ManagerFontBold) {
        ImVec2 ns = g_ManagerFontBold->CalcTextSizeA(10.0f, FLT_MAX, 0.f, dragged.lastName.c_str());
        float tx = mouse.x - ns.x * 0.5f, ty = mouse.y + gBH * 0.5f + 3.0f;
        fdl2->AddText(g_ManagerFontBold, 10.0f, ImVec2(tx-1,ty+1), IM_COL32(0,0,0,200), dragged.lastName.c_str());
        fdl2->AddText(g_ManagerFontBold, 10.0f, ImVec2(tx, ty),    IM_COL32(255,255,255,255), dragged.lastName.c_str());
      }
    }

    // Commit substitution on mouse release
    if (s_dragSubIdx >= 0 && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
      if (budgetOk &&
          s_hoverOnIdx >= 0 && s_hoverOnIdx < (int)s_pausePlayers.size() &&
          s_dragSubIdx < (int)s_benchPlayers.size()) {
        const BenchPlayer  &bp  = s_benchPlayers[s_dragSubIdx];
        const PausePlayer  &off = s_pausePlayers[s_hoverOnIdx];

        // Push onto sub queue (fires on next dead ball, not during goal celebration)
        QueuedSub qs;
        qs.pending        = true;
        qs.teamIdx        = s_subUserTeamIdx;
        qs.userTeamIdx    = s_subUserTeamIdx;
        qs.offIdx         = s_hoverOnIdx;
        qs.onIdx          = bp.playersIdx;
        qs.offPlayerDbId  = off.playerDbId;
        qs.onPlayerDbId   = bp.playerDbId;
        qs.nameOut        = off.lastName;
        qs.nameIn         = bp.lastName;
        qs.teamBadgePath  = s_userBadgePath;
        qs.leagueLogoPath = s_leagueLogoPath;
        qs.teamColor      = s_pauseUserColor;
        QueueSubPush(qs);

        // Update formation board immediately so display reflects the change
        PausePlayer incoming;
        incoming.lastName    = bp.lastName;
        incoming.role        = off.role;
        incoming.nx          = off.nx;
        incoming.ny          = off.ny;
        incoming.jerseyNumber = bp.jerseyNumber;
        incoming.playerDbId  = bp.playerDbId;

        s_pausePlayers.erase(s_pausePlayers.begin() + s_hoverOnIdx);
        s_pausePlayers.push_back(incoming);

        // Remove the incoming player from the bench list
        for (int i = 0; i < (int)s_benchPlayers.size(); i++) {
          if (s_benchPlayers[i].playersIdx == bp.playersIdx) {
            s_benchPlayers.erase(s_benchPlayers.begin() + i);
            break;
          }
        }
      }
      s_dragSubIdx = -1;
      s_hoverOnIdx = -1;
    }

    // Cancel drag on right-click or Escape
    if (s_dragSubIdx >= 0 && (ImGui::IsMouseClicked(ImGuiMouseButton_Right) ||
                               ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
      s_dragSubIdx = -1;
      s_hoverOnIdx = -1;
    }
  }

  // Live match stats panel (non-pausing overlay)
  RenderMatchStatsPanel();

  // Live tactics panel (non-pausing overlay)
  RenderTacticsPanel();
}

// ---------------------------------------------------------------------------
// Pause menu overlay
// ---------------------------------------------------------------------------
// (PausePlayer, BenchPlayer, static vars, ResetPauseCache defined above RenderImGuiMatchOverlay)

static float Clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

// Build a PausePlayer using the same formula as planmap.cpp.
static PausePlayer MakePausePlayer(TeamData *td, int i) {
  PausePlayer pp;
  PlayerData *pd = td->GetPlayerData(i);
  pp.lastName    = pd->GetDisplayName();
  pp.jerseyNumber = pd->GetJerseyNumber();
  pp.playerDbId  = pd->GetDatabaseID();
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

// Draw a formation panel (header + mini pitch + jerseys) into a draw list.
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

  for (size_t i = 0; i < players.size() && i < 11; i++) {
    const PausePlayer &pp = players[i];
    float px = pitchMin.x + pp.nx * pW;
    float py = pitchMin.y + pp.ny * pH;

    // Clamp so text doesn't run off edges
    if (px < pitchMin.x + 16) px = pitchMin.x + 16;
    if (px > pitchMax.x - 16) px = pitchMax.x - 16;
    if (py < pitchMin.y + 14) py = pitchMin.y + 14;
    if (py > pitchMax.y - 28) py = pitchMax.y - 28;

    DrawJersey(dl, px, py, teamColor, TextColorForBg(teamColor), pp.jerseyNumber);

    // Playing position above jersey
    if (!pp.role.empty()) {
      ImVec2 rs = g_ManagerFontBold
          ? g_ManagerFontBold->CalcTextSizeA(11.0f, FLT_MAX, 0.f, pp.role.c_str())
          : ImVec2(20, 10);
      dl->AddText(g_ManagerFontBold, 11.0f,
                  ImVec2(px - rs.x * 0.5f, py - 11 - rs.y),
                  IM_COL32(255, 255, 255, 240), pp.role.c_str());
    }

    // Surname below jersey
    std::string abbr = pp.lastName.size() > 8 ? pp.lastName.substr(0, 8) : pp.lastName;
    ImVec2 ns = g_ManagerFontBold
        ? g_ManagerFontBold->CalcTextSizeA(11.0f, FLT_MAX, 0.f, abbr.c_str())
        : ImVec2(30, 10);
    dl->AddText(g_ManagerFontBold, 11.0f,
                ImVec2(px - ns.x * 0.5f, py + 13.0f),
                IM_COL32(255, 255, 255, 220), abbr.c_str());
  }
  if (players.empty()) {
    dl->AddText(nullptr, 11.0f, ImVec2(pitchMin.x + 4, pitchMin.y + 4),
                IM_COL32(200, 220, 255, 150), "No data");
  }
}

static void DrawSubstitutionPanel(float px, float py, float pw, float ph) {
  const ImU32 kBgPanel   = IM_COL32(14, 20, 38, 250);
  const ImU32 kBgRow     = IM_COL32(22, 32, 60, 255);
  const ImU32 kBgRowSel  = s_pauseUserColor;
  const ImU32 kHeader    = IM_COL32(180, 200, 240, 200);
  const ImU32 kText      = IM_COL32(220, 230, 255, 255);
  const ImU32 kDim       = IM_COL32(120, 140, 180, 160);
  const ImU32 kBtnConf   = IM_COL32(50, 200, 80, 255);
  const ImU32 kBtnConfDis= IM_COL32(40, 60, 40, 200);
  const ImU32 kBtnCancel = IM_COL32(180, 50, 50, 255);
  const float kRound     = 6.0f;
  const float kRowH      = 34.0f;
  const float kFooterH   = 56.0f;
  const float kHeaderH   = 42.0f;
  const float kColW      = (pw - 2.0f) * 0.5f;
  const float kListH     = ph - kHeaderH - kFooterH;

  ImGui::SetNextWindowPos(ImVec2(px, py), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(pw, ph), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 8.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, kBgPanel);
  ImGui::Begin("##sub_panel", nullptr,
    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
    ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar |
    ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav);

  ImDrawList *dl    = ImGui::GetWindowDrawList();
  ImVec2      origin= ImGui::GetWindowPos();

  // ---- Header ---------------------------------------------------------------
  int queuedForTeam = CountQueuedSubsForTeam(s_subUserTeamIdx);
  int plannedSubs = g_SubsUsed + queuedForTeam;
  bool budgetOk = CanQueueSubForTeam(s_subUserTeamIdx);
  char hdr[64];
  snprintf(hdr, sizeof(hdr), "SUBSTITUTIONS  —  %d / 5 used, %d / 3 windows",
           plannedSubs, g_WindowsUsed);
  ImVec2 hMin = ImVec2(origin.x, origin.y);
  ImVec2 hMax = ImVec2(origin.x + pw, origin.y + kHeaderH);
  dl->AddRectFilled(hMin, hMax, IM_COL32(18, 28, 52, 255));
  AddTextCentered(dl, g_ManagerFontBold, 15.0f, hMin, hMax, kHeader, hdr);
  // Separator
  dl->AddLine(ImVec2(origin.x, origin.y + kHeaderH),
              ImVec2(origin.x + pw, origin.y + kHeaderH),
              IM_COL32(50, 65, 110, 200), 1.0f);

  // ---- Two-column list area -------------------------------------------------
  float listTop = origin.y + kHeaderH;

  // Column headers
  const float kColHdrH = 26.0f;
  dl->AddRectFilled(ImVec2(origin.x,          listTop),
                    ImVec2(origin.x + kColW,   listTop + kColHdrH),
                    IM_COL32(20, 32, 60, 255));
  dl->AddRectFilled(ImVec2(origin.x + kColW + 2.0f, listTop),
                    ImVec2(origin.x + pw,              listTop + kColHdrH),
                    IM_COL32(20, 32, 60, 255));
  AddTextCentered(dl, g_ManagerFontBold, 13.0f,
                  ImVec2(origin.x, listTop), ImVec2(origin.x + kColW, listTop + kColHdrH),
                  kDim, "ON PITCH");
  AddTextCentered(dl, g_ManagerFontBold, 13.0f,
                  ImVec2(origin.x + kColW + 2.0f, listTop),
                  ImVec2(origin.x + pw, listTop + kColHdrH),
                  kDim, "BENCH");

  listTop += kColHdrH;
  const float scrollAreaH = kListH - kColHdrH;

  // Left scroll (starters)
  ImGui::SetNextWindowPos(ImVec2(origin.x, listTop), ImGuiCond_Always);
  ImGui::SetCursorScreenPos(ImVec2(origin.x, listTop));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 28, 52, 255));
  ImGui::BeginChild("##sub_starters", ImVec2(kColW, scrollAreaH), false, 0);
  {
    ImDrawList *ld = ImGui::GetWindowDrawList();
    ImVec2 cp = ImGui::GetWindowPos();
    for (int i = 0; i < (int)s_pausePlayers.size(); i++) {
      const PausePlayer &pp = s_pausePlayers[i];
      bool selected = (s_subOffSelected == i);
      ImVec2 rMin = ImVec2(cp.x, cp.y + i * kRowH);
      ImVec2 rMax = ImVec2(cp.x + kColW, rMin.y + kRowH - 1.0f);
      ImGui::SetCursorScreenPos(rMin);
      ImGui::PushID(10000 + i);
      if (ImGui::InvisibleButton("##off", ImVec2(kColW, kRowH - 1.0f)))
        s_subOffSelected = (s_subOffSelected == i) ? -1 : i;
      ImGui::PopID();
      ld->AddRectFilled(rMin, rMax, selected ? kBgRowSel : kBgRow, 3.0f);
      // Role badge
      ImVec2 badgeMin = ImVec2(rMin.x + 6, rMin.y + (kRowH - 18.0f) * 0.5f);
      ImVec2 badgeMax = ImVec2(badgeMin.x + 32, badgeMin.y + 18.0f);
      ld->AddRectFilled(badgeMin, badgeMax, IM_COL32(40, 60, 100, 200), 3.0f);
      AddTextCentered(ld, g_ManagerFontSmall, 11.0f, badgeMin, badgeMax,
                      IM_COL32(180, 200, 240, 255), pp.role.c_str());
      // Name
      if (g_ManagerFontSmall)
        ld->AddText(g_ManagerFontSmall, 13.0f,
                    ImVec2(badgeMax.x + 6, rMin.y + (kRowH - g_ManagerFontSmall->FontSize) * 0.5f),
                    selected ? IM_COL32(15, 15, 15, 255) : kText, pp.lastName.c_str());
      // Divider
      ld->AddLine(ImVec2(rMin.x, rMax.y), ImVec2(rMax.x, rMax.y),
                  IM_COL32(30, 45, 80, 160), 1.0f);
    }
    ImGui::Dummy(ImVec2(kColW, s_pausePlayers.size() * kRowH));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  // Vertical divider between columns
  dl->AddLine(ImVec2(origin.x + kColW, listTop),
              ImVec2(origin.x + kColW, listTop + scrollAreaH),
              IM_COL32(50, 65, 110, 200), 2.0f);

  // Right scroll (bench)
  ImGui::SetCursorScreenPos(ImVec2(origin.x + kColW + 2.0f, listTop));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(18, 28, 52, 255));
  ImGui::BeginChild("##sub_bench", ImVec2(kColW - 2.0f, scrollAreaH), false, 0);
  {
    ImDrawList *rd = ImGui::GetWindowDrawList();
    ImVec2 cp = ImGui::GetWindowPos();
    for (int i = 0; i < (int)s_benchPlayers.size(); i++) {
      const BenchPlayer &bp = s_benchPlayers[i];
      bool selected = (s_subOnSelected == bp.playersIdx);
      ImVec2 rMin = ImVec2(cp.x, cp.y + i * kRowH);
      ImVec2 rMax = ImVec2(cp.x + kColW - 2.0f, rMin.y + kRowH - 1.0f);
      ImGui::SetCursorScreenPos(rMin);
      ImGui::PushID(20000 + i);
      if (ImGui::InvisibleButton("##on", ImVec2(kColW - 2.0f, kRowH - 1.0f)))
        s_subOnSelected = (s_subOnSelected == bp.playersIdx) ? -1 : bp.playersIdx;
      ImGui::PopID();
      rd->AddRectFilled(rMin, rMax, selected ? kBgRowSel : kBgRow, 3.0f);
      ImVec2 badgeMin = ImVec2(rMin.x + 6, rMin.y + (kRowH - 18.0f) * 0.5f);
      ImVec2 badgeMax = ImVec2(badgeMin.x + 32, badgeMin.y + 18.0f);
      rd->AddRectFilled(badgeMin, badgeMax, IM_COL32(40, 60, 100, 200), 3.0f);
      AddTextCentered(rd, g_ManagerFontSmall, 11.0f, badgeMin, badgeMax,
                      IM_COL32(180, 200, 240, 255), bp.role.c_str());
      if (g_ManagerFontSmall)
        rd->AddText(g_ManagerFontSmall, 13.0f,
                    ImVec2(badgeMax.x + 6, rMin.y + (kRowH - g_ManagerFontSmall->FontSize) * 0.5f),
                    selected ? IM_COL32(15, 15, 15, 255) : kText, bp.lastName.c_str());
      rd->AddLine(ImVec2(rMin.x, rMax.y), ImVec2(rMax.x, rMax.y),
                  IM_COL32(30, 45, 80, 160), 1.0f);
    }
    ImGui::Dummy(ImVec2(kColW - 2.0f, s_benchPlayers.size() * kRowH));
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();

  // ---- Footer: sub summary + confirm/cancel ---------------------------------
  float fY = origin.y + kHeaderH + kListH;
  dl->AddRectFilled(ImVec2(origin.x, fY), ImVec2(origin.x + pw, fY + kFooterH),
                    IM_COL32(12, 18, 32, 255));
  dl->AddLine(ImVec2(origin.x, fY), ImVec2(origin.x + pw, fY),
              IM_COL32(50, 65, 110, 200), 1.0f);

  // Summary text
  if (s_subOffSelected >= 0 && s_subOffSelected < (int)s_pausePlayers.size() &&
      s_subOnSelected  >= 0) {
    // Find bench player name for s_subOnSelected
    std::string onName;
    for (const auto &bp : s_benchPlayers)
      if (bp.playersIdx == s_subOnSelected) { onName = bp.lastName; break; }
    char summary[128];
    snprintf(summary, sizeof(summary), "%s  →  %s",
             s_pausePlayers[s_subOffSelected].lastName.c_str(), onName.c_str());
    float tY = fY + (kFooterH - 14.0f) * 0.5f;
    if (g_ManagerFontSmall)
      dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(origin.x + 16.0f, tY),
                  kDim, summary);
  }

  // Buttons
  const float btnW  = 110.0f;
  const float btnH  = 30.0f;
  const float btnY  = fY + (kFooterH - btnH) * 0.5f;
  bool canConfirm   = (s_subOffSelected >= 0 && s_subOnSelected >= 0 && budgetOk);

  // Confirm
  ImVec2 confMin = ImVec2(origin.x + pw - btnW * 2.0f - 24.0f, btnY);
  ImVec2 confMax = ImVec2(confMin.x + btnW, btnY + btnH);
  ImGui::SetCursorScreenPos(confMin);
  ImGui::PushID(30001);
  bool confClicked = ImGui::InvisibleButton("##sub_confirm", ImVec2(btnW, btnH)) && canConfirm;
  ImGui::PopID();
  dl->AddRectFilled(confMin, confMax, canConfirm ? kBtnConf : kBtnConfDis, kRound);
  AddTextCentered(dl, g_ManagerFontBold, 13.0f, confMin, confMax,
                  IM_COL32(255, 255, 255, canConfirm ? 255 : 100), "CONFIRM");
  if (confClicked) {
    // Capture names before mutating the display lists
    std::string nameOut, nameIn;
    if (s_subOffSelected >= 0 && s_subOffSelected < (int)s_pausePlayers.size())
      nameOut = s_pausePlayers[s_subOffSelected].lastName;
    for (auto &bp : s_benchPlayers)
      if (bp.playersIdx == s_subOnSelected) { nameIn = bp.lastName; break; }

    BenchPlayer incoming;
    for (auto it = s_benchPlayers.begin(); it != s_benchPlayers.end(); ++it) {
      if (it->playersIdx == s_subOnSelected) { incoming = *it; break; }
    }

    // Push substitution onto queue — fires on next dead ball (gametask.cpp)
    QueuedSub qs2;
    qs2.pending        = true;
    qs2.teamIdx        = s_subUserTeamIdx;
    qs2.userTeamIdx    = s_subUserTeamIdx;
    qs2.offIdx         = s_subOffSelected;
    qs2.onIdx          = s_subOnSelected;
    qs2.offPlayerDbId  = (s_subOffSelected >= 0 && s_subOffSelected < (int)s_pausePlayers.size()) ? s_pausePlayers[s_subOffSelected].playerDbId : -1;
    qs2.onPlayerDbId   = incoming.playerDbId;
    qs2.nameOut        = nameOut;
    qs2.nameIn         = nameIn;
    qs2.teamBadgePath  = s_userBadgePath;
    qs2.leagueLogoPath = s_leagueLogoPath;
    qs2.teamColor      = (unsigned int)s_pauseUserColor;
    QueueSubPush(qs2);

    // Update display lists so user can queue a 2nd sub immediately
    for (auto it = s_benchPlayers.begin(); it != s_benchPlayers.end(); ++it) {
      if (it->playersIdx == s_subOnSelected) { incoming = *it; s_benchPlayers.erase(it); break; }
    }
    // Inherit the outgoing player's formation slot (position on pitch + role label)
    std::string outgoingRole;
    float outgoingNx = 0.5f, outgoingNy = 0.5f;
    if (s_subOffSelected >= 0 && s_subOffSelected < (int)s_pausePlayers.size()) {
      outgoingRole = s_pausePlayers[s_subOffSelected].role;
      outgoingNx   = s_pausePlayers[s_subOffSelected].nx;
      outgoingNy   = s_pausePlayers[s_subOffSelected].ny;
    }
    if (s_subOffSelected < (int)s_pausePlayers.size())
      s_pausePlayers.erase(s_pausePlayers.begin() + s_subOffSelected);
    PausePlayer newPP;
    newPP.lastName = incoming.lastName;
    newPP.role     = outgoingRole;
    newPP.nx       = outgoingNx;
    newPP.ny       = outgoingNy;
    newPP.jerseyNumber = incoming.jerseyNumber;
    newPP.playerDbId = incoming.playerDbId;
    s_pausePlayers.push_back(newPP);
    s_subOffSelected = -1;
    s_subOnSelected  = -1;
  }

  // Cancel
  ImVec2 cancMin = ImVec2(origin.x + pw - btnW - 8.0f, btnY);
  ImVec2 cancMax = ImVec2(cancMin.x + btnW, btnY + btnH);
  ImGui::SetCursorScreenPos(cancMin);
  ImGui::PushID(30002);
  bool cancClicked = ImGui::InvisibleButton("##sub_cancel", ImVec2(btnW, btnH));
  ImGui::PopID();
  dl->AddRectFilled(cancMin, cancMax, kBtnCancel, kRound);
  AddTextCentered(dl, g_ManagerFontBold, 13.0f, cancMin, cancMax,
                  IM_COL32(255, 255, 255, 255), "< BACK");
  if (cancClicked) {
    s_subOffSelected = -1;
    s_subOnSelected  = -1;
    s_subPanelActive = false;
  }

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(2);
}

// Populate the per-match HUD/pause data cache (GL thread — TeamData read-only during match).
// Called every frame from RenderImGuiMatchOverlay so the data is ready before ESC is pressed.
static void InitHUDDataIfNeeded(Match *match) {
  if (s_pauseInitialized) return;

  int userClubId = g_CareerMatchContext.userClubId;
  int teamIdx = 0;
  if (userClubId > 0 &&
      match->GetTeam(1) && match->GetTeam(1)->GetTeamData() &&
      match->GetTeam(1)->GetTeamData()->GetDatabaseID() == userClubId)
    teamIdx = 1;
  int oppIdx = 1 - teamIdx;

  TeamData *td = match->GetTeam(teamIdx) ? match->GetTeam(teamIdx)->GetTeamData() : nullptr;
  if (td) {
    s_pauseTeamName      = td->GetName();
    Vector3 c            = td->GetColor1();
    s_pauseUserColor     = Vec3ToCol32(c.coords[0], c.coords[1], c.coords[2]);
    s_pauseUserTextColor = TextColorForBg(s_pauseUserColor);
    int n = std::min(td->GetPlayerNum(), 11);
    for (int i = 0; i < n; i++) s_pausePlayers.push_back(MakePausePlayer(td, i));

    s_subUserTeamIdx = teamIdx;
    {
      std::string raw = td->GetLogoUrl();
      s_userBadgePath = NormalizeBadgePath(raw);
    }
    int total = std::min(td->GetPlayerNum(), 20);
    for (int i = 11; i < total; i++) {
      BenchPlayer bp;
      PlayerData *pd = td->GetPlayerData(i);
      bp.lastName    = pd->GetDisplayName();
      const std::string &raw = pd->GetRoleRaw();
      bp.role        = raw.empty() ? "SUB" : raw;
      bp.playersIdx  = i;
      bp.jerseyNumber = pd->GetJerseyNumber();
      bp.playerDbId  = pd->GetDatabaseID();
      s_benchPlayers.push_back(bp);
    }
  }

  TeamData *tdOpp = match->GetTeam(oppIdx) ? match->GetTeam(oppIdx)->GetTeamData() : nullptr;
  if (tdOpp) {
    s_pauseAwayTeamName = tdOpp->GetName();
    {
      std::string raw = tdOpp->GetLogoUrl();
      s_oppBadgePath = NormalizeBadgePath(raw);
    }
    Vector3 c           = tdOpp->GetColor1();
    s_pauseOppColor     = Vec3ToCol32(c.coords[0], c.coords[1], c.coords[2]);
    s_pauseOppTextColor = TextColorForBg(s_pauseOppColor);
    int n = std::min(tdOpp->GetPlayerNum(), 11);
    for (int i = 0; i < n; i++) s_pauseAwayPlayers.push_back(MakePausePlayer(tdOpp, i));
    int totalOpp = std::min(tdOpp->GetPlayerNum(), 20);
    for (int i = 11; i < totalOpp; i++) {
      BenchPlayer bp;
      PlayerData *pd = tdOpp->GetPlayerData(i);
      bp.lastName    = pd->GetDisplayName();
      const std::string &raw = pd->GetRoleRaw();
      bp.role        = raw.empty() ? "SUB" : raw;
      bp.playersIdx  = i;
      bp.jerseyNumber = pd->GetJerseyNumber();
      bp.playerDbId  = pd->GetDatabaseID();
      s_awayBenchPlayers.push_back(bp);
    }
  }

  if (s_pauseTeamName.empty())     s_pauseTeamName     = g_CareerHub.club.name;
  if (s_pauseTeamName.empty())     s_pauseTeamName     = "Your Team";
  if (s_pauseAwayTeamName.empty()) s_pauseAwayTeamName = "Opponents";

  // Seed live tactics from user team's TeamData (the authoritative source during the match).
  // g_CareerHub is cleared before match starts, so we can't rely on it.
  if (td) {
    const Properties &up = td->GetTactics().userProperties;
    for (int ti = 0; ti < kNumMatchTacInstr; ti++) {
      const char *key = kMatchTacInstr[ti].key;
      // Default from the factory preset array if TeamData has no value stored
      float def = kMatchTacInstr[ti].presets[1].value; // "Balanced" preset as fallback
      s_liveTactics[key] = up.GetReal(key, def);
    }
    printf("[IMGUI HUD] Tactics cached from TeamData (%zu keys)\n", s_liveTactics.size());
  }

  s_pauseInitialized = true;
  printf("[IMGUI HUD] Cache: %zu user / %zu opp players\n",
         s_pausePlayers.size(), s_pauseAwayPlayers.size());
}

// ---------------------------------------------------------------------------
// Live match stats panel — drawn over the game without pausing it.
// ---------------------------------------------------------------------------

static void DrawStatBar(ImDrawList *dl, ImVec2 rowMin, float rowW, float rowH,
                        int valA, int valB, ImU32 colorA, ImU32 colorB) {
  int total = valA + valB;
  float fA   = (total > 0) ? (float)valA / (float)total : 0.5f;
  float fB   = 1.0f - fA;

  const float trackH  = 6.0f;
  const float trackY  = rowMin.y + rowH * 0.5f - trackH * 0.5f;
  const float trackX0 = rowMin.x + 8.0f;
  const float trackX1 = rowMin.x + rowW - 8.0f;
  const float trackW  = trackX1 - trackX0;

  // Track bg
  dl->AddRectFilled(ImVec2(trackX0, trackY), ImVec2(trackX1, trackY + trackH),
                    IM_COL32(40, 50, 80, 200), 3.0f);
  // Team A fill (left)
  float splitX = trackX0 + trackW * fA;
  if (fA > 0.01f)
    dl->AddRectFilled(ImVec2(trackX0, trackY), ImVec2(splitX, trackY + trackH),
                      colorA, 3.0f);
  // Team B fill (right)
  if (fB > 0.01f)
    dl->AddRectFilled(ImVec2(splitX, trackY), ImVec2(trackX1, trackY + trackH),
                      colorB, 3.0f);
  // Centre divider
  dl->AddLine(ImVec2(trackX0 + trackW * 0.5f, trackY - 2),
              ImVec2(trackX0 + trackW * 0.5f, trackY + trackH + 2),
              IM_COL32(80, 100, 160, 180), 1.0f);
}

void RenderMatchStatsPanel() {
  if (!g_MatchStatsVisible) return;

  Match *match = GetGameTask() ? GetGameTask()->GetMatch() : nullptr;
  if (!match) { g_MatchStatsVisible = false; return; }

  MatchData *md = match->GetMatchData();
  if (!md) return;

  ImGuiIO &io  = ImGui::GetIO();
  const float sw = io.DisplaySize.x;
  const float sh = io.DisplaySize.y;

  // Resolve user vs opponent team indices.
  int uIdx = s_subUserTeamIdx;
  int oIdx = 1 - uIdx;

  ImU32 colorA = s_pauseUserColor;
  ImU32 colorB = s_pauseOppColor;

  // Badge textures (loaded/cached on GL thread).
  static GLuint s_statBadgeA = 0; static std::string s_statBadgeAPath;
  static GLuint s_statBadgeB = 0; static std::string s_statBadgeBPath;
  if (s_statBadgeAPath != s_userBadgePath) { s_statBadgeA = 0; s_statBadgeAPath = s_userBadgePath; }
  if (s_statBadgeBPath != s_oppBadgePath)  { s_statBadgeB = 0; s_statBadgeBPath = s_oppBadgePath; }
  if (s_statBadgeA == 0 && !s_statBadgeAPath.empty()) s_statBadgeA = LoadBadgeTex(s_statBadgeAPath);
  if (s_statBadgeB == 0 && !s_statBadgeBPath.empty()) s_statBadgeB = LoadBadgeTex(s_statBadgeBPath);

  // Live scores.
  int scoreA = md->GetGoalCount(uIdx);
  int scoreB = md->GetGoalCount(oIdx);

  // --- Stats ---
  struct StatRow { const char *label; int a; int b; bool isPercent; };

  unsigned long posU     = md->GetPossessionTime_ms(uIdx);
  unsigned long posO     = md->GetPossessionTime_ms(oIdx);
  unsigned long posTotal = posU + posO;
  int posPercA = (posTotal > 0) ? (int)roundf((float)posU / (float)posTotal * 100.f) : 50;
  int posPercB = 100 - posPercA;

  int passAttU = md->GetPassesAttempted(uIdx), passAttO = md->GetPassesAttempted(oIdx);
  int passComU = md->GetPassesCompleted(uIdx), passComO = md->GetPassesCompleted(oIdx);
  int passPercU = passAttU > 0 ? (int)roundf((float)passComU / (float)passAttU * 100.f) : 0;
  int passPercO = passAttO > 0 ? (int)roundf((float)passComO / (float)passAttO * 100.f) : 0;

  StatRow fullRows[] = {
    { "Shots",           md->GetShots(uIdx),         md->GetShots(oIdx),         false },
    { "Shots On Target", md->GetShotsOnTarget(uIdx), md->GetShotsOnTarget(oIdx), false },
    { "Corners",         md->GetCorners(uIdx),        md->GetCorners(oIdx),        false },
    { "Fouls",           md->GetFouls(uIdx),          md->GetFouls(oIdx),          false },
    { "Offsides",        md->GetOffsides(uIdx),       md->GetOffsides(oIdx),       false },
    { "Passes",          passAttU,                     passAttO,                    false },
    { "Pass Accuracy",   passPercU,                    passPercO,                   true  },
    { "Yellow Cards",    md->GetYellowCards(uIdx),    md->GetYellowCards(oIdx),    false },
    { "Red Cards",       md->GetRedCards(uIdx),       md->GetRedCards(oIdx),       false },
  };
  StatRow compactRows[] = {
    { "Shots",         md->GetShots(uIdx), md->GetShots(oIdx), false },
    { "Pass Accuracy", passPercU,          passPercO,          true  },
  };
  const StatRow *rows   = s_statsCompact ? compactRows : fullRows;
  const int      numRows = s_statsCompact ? 2 : 9;

  // --- Panel geometry ---
  const float kRound   = 12.0f;
  const float panW     = 460.0f;
  const float hdrH     = 42.0f;  // "MATCH STATS" title
  const float scoreH   = 70.0f;  // badge + score row
  const float goalLogH = (float)std::max(1, (int)s_goalLog.size()) * 16.0f + 8.0f;
  const float posRowH  = 44.0f;
  const float statRowH = 38.0f;
  const float panH     = hdrH + scoreH + goalLogH + posRowH + statRowH * numRows;
  // Initialize to center on first display, then let the user drag it.
  if (s_statsPanelX < 0.f) {
    s_statsPanelX = (sw - panW) * 0.5f;
    s_statsPanelY = (sh - panH) * 0.5f - 50.0f;
  }
  // Clamp to screen.
  if (s_statsPanelX < 0.f)          s_statsPanelX = 0.f;
  if (s_statsPanelY < 0.f)          s_statsPanelY = 0.f;
  if (s_statsPanelX + panW > sw)    s_statsPanelX = sw - panW;
  if (s_statsPanelY + panH > sh)    s_statsPanelY = sh - panH;

  ImGui::SetNextWindowPos(ImVec2(s_statsPanelX, s_statsPanelY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(panW, panH), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));

  ImGui::Begin("##matchstats", nullptr,
    ImGuiWindowFlags_NoTitleBar        |
    ImGuiWindowFlags_NoResize          |
    ImGuiWindowFlags_NoMove            |
    ImGuiWindowFlags_NoScrollbar       |
    ImGuiWindowFlags_NoSavedSettings   |
    ImGuiWindowFlags_NoFocusOnAppearing|
    ImGuiWindowFlags_NoNav);

  ImDrawList *dl   = ImGui::GetWindowDrawList();
  ImVec2      wPos = ImGui::GetWindowPos();
  float       curY = wPos.y;

  // Rounded background — drawn here so it tracks wPos, not a pre-computed position.
  ImDrawList *bgdl = ImGui::GetBackgroundDrawList();
  bgdl->AddRectFilled(ImVec2(wPos.x, wPos.y), ImVec2(wPos.x + panW, wPos.y + panH),
                      IM_COL32(0, 0, 0, 70), kRound, ImDrawFlags_RoundCornersTop);
  bgdl->AddRectFilled(ImVec2(wPos.x, wPos.y), ImVec2(wPos.x + panW, wPos.y + panH),
                      IM_COL32(10, 16, 34, 252), kRound, ImDrawFlags_RoundCornersTop);

  // ── Header ──────────────────────────────────────────────────────────────
  dl->AddRectFilled(ImVec2(wPos.x, curY), ImVec2(wPos.x + panW, curY + hdrH),
                    IM_COL32(16, 24, 52, 255), kRound, ImDrawFlags_RoundCornersTop);
  AddTextCentered(dl, g_ManagerFontBold, 13.0f,
                  ImVec2(wPos.x, curY), ImVec2(wPos.x + panW, curY + hdrH),
                  IM_COL32(200, 215, 255, 255), "MATCH STATS");

  // Drag handle — covers header minus the two right-side buttons.
  ImGui::SetCursorScreenPos(ImVec2(wPos.x, curY));
  ImGui::InvisibleButton("##drag_stats", ImVec2(panW - 74.0f, hdrH));
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
    s_statsPanelX += delta.x;
    s_statsPanelY += delta.y;
    if (s_statsPanelX < 0.f)       s_statsPanelX = 0.f;
    if (s_statsPanelY < 0.f)       s_statsPanelY = 0.f;
    if (s_statsPanelX + panW > sw) s_statsPanelX = sw - panW;
    if (s_statsPanelY + panH > sh) s_statsPanelY = sh - panH;
  }
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

  // Compact / expand toggle button (left of X).
  ImVec2 cMin(wPos.x + panW - 66.0f, curY + 7.0f);
  ImVec2 cMax(wPos.x + panW - 39.0f, curY + hdrH - 7.0f);
  ImGui::SetCursorScreenPos(cMin);
  ImGui::InvisibleButton("##compact_stats", ImVec2(cMax.x - cMin.x, cMax.y - cMin.y));
  bool cHov = ImGui::IsItemHovered();
  dl->AddRectFilled(cMin, cMax, cHov ? IM_COL32(60,100,200,220) : IM_COL32(40,50,90,180), 4.0f);
  {
    float icx = (cMin.x + cMax.x) * 0.5f;
    float icy = (cMin.y + cMax.y) * 0.5f;
    const float ts  = 5.0f;
    ImU32 iconCol = cHov ? IM_COL32(255,255,255,255) : IM_COL32(180,200,240,210);
    if (!s_statsCompact) {
      // Two triangles pointing toward each other = compress
      dl->AddTriangleFilled(ImVec2(icx-ts, icy-1.5f), ImVec2(icx+ts, icy-1.5f), ImVec2(icx, icy+ts-1.5f), iconCol); // ▼ top half
      dl->AddTriangleFilled(ImVec2(icx-ts, icy+1.5f), ImVec2(icx+ts, icy+1.5f), ImVec2(icx, icy-ts+1.5f), iconCol); // ▲ bottom half
    } else {
      // Two triangles pointing away from each other = expand
      dl->AddTriangleFilled(ImVec2(icx-ts, icy-1.5f), ImVec2(icx+ts, icy-1.5f), ImVec2(icx, icy-ts-1.5f), iconCol); // ▲ top half
      dl->AddTriangleFilled(ImVec2(icx-ts, icy+1.5f), ImVec2(icx+ts, icy+1.5f), ImVec2(icx, icy+ts+1.5f), iconCol); // ▼ bottom half
    }
  }
  if (ImGui::IsItemClicked()) {
    s_statsCompact = !s_statsCompact;
    s_statsPanelX = -1.f; // re-center after resize so panel doesn't go off-screen
    s_statsPanelY = -1.f;
  }

  // Close [X]
  ImVec2 xMin(wPos.x + panW - 34.0f, curY + 7.0f);
  ImVec2 xMax(wPos.x + panW - 7.0f,  curY + hdrH - 7.0f);
  ImGui::SetCursorScreenPos(xMin);
  ImGui::InvisibleButton("##close_stats", ImVec2(xMax.x - xMin.x, xMax.y - xMin.y));
  bool xHov = ImGui::IsItemHovered();
  dl->AddRectFilled(xMin, xMax, xHov ? IM_COL32(220,60,60,220) : IM_COL32(40,50,90,180), 4.0f);
  AddTextCentered(dl, g_ManagerFontBold, 13.0f, xMin, xMax, IM_COL32(255,255,255,255), "X");
  if (ImGui::IsItemClicked()) g_MatchStatsVisible = false;
  curY += hdrH;

  // ── Score row: BadgeA  TeamA  ScoreA - ScoreB  TeamB  BadgeB ────────────
  dl->AddRectFilled(ImVec2(wPos.x, curY), ImVec2(wPos.x + panW, curY + scoreH),
                    IM_COL32(14, 22, 48, 255));

  const float bdgSz = 40.0f;        // badge size
  const float midX  = wPos.x + panW * 0.5f;
  const float bdgY  = curY + (scoreH - bdgSz) * 0.5f;

  // Team A badge (left side)
  float aRight = midX - 30.0f;      // right edge of team-A area
  if (s_statBadgeA) {
    dl->AddImage((ImTextureID)(intptr_t)s_statBadgeA,
                 ImVec2(aRight - bdgSz - 4.0f, bdgY),
                 ImVec2(aRight - 4.0f,          bdgY + bdgSz));
  }

  // Team B badge (right side)
  float bLeft = midX + 30.0f;       // left edge of team-B area
  if (s_statBadgeB) {
    dl->AddImage((ImTextureID)(intptr_t)s_statBadgeB,
                 ImVec2(bLeft + 4.0f,          bdgY),
                 ImVec2(bLeft + bdgSz + 4.0f,  bdgY + bdgSz));
  }

  // Score "A - B" centred
  char scoreBuf[16];
  snprintf(scoreBuf, sizeof(scoreBuf), "%d - %d", scoreA, scoreB);
  AddTextCentered(dl, g_ManagerFontBold, 22.0f,
                  ImVec2(midX - 30, curY), ImVec2(midX + 30, curY + scoreH),
                  IM_COL32(255, 255, 255, 255), scoreBuf);

  // Colour accent strips at sides
  dl->AddRectFilled(ImVec2(wPos.x, curY), ImVec2(wPos.x + 4.0f, curY + scoreH), colorA);
  dl->AddRectFilled(ImVec2(wPos.x + panW - 4.0f, curY), ImVec2(wPos.x + panW, curY + scoreH), colorB);
  curY += scoreH;

  // ── Goal log ─────────────────────────────────────────────────────────────
  dl->AddRectFilled(ImVec2(wPos.x, curY), ImVec2(wPos.x + panW, curY + goalLogH),
                    IM_COL32(11, 18, 40, 255));
  {
    float gly = curY + 4.0f;
    if (s_goalLog.empty()) {
      AddTextCentered(dl, g_ManagerFontHero, 10.0f,
                      ImVec2(wPos.x, gly), ImVec2(wPos.x + panW, gly + 16),
                      IM_COL32(80, 100, 150, 180), "No goals yet");
    } else {
      for (const GoalEntry &ge : s_goalLog) {
        bool isUser = (ge.teamIdx == uIdx);
        char buf[64];
        snprintf(buf, sizeof(buf), "%d'  %s", ge.minute,
                 ge.scorer.empty() ? "Goal" : ge.scorer.c_str());
        // User goals left-aligned, opponent goals right-aligned
        ImU32 gCol = isUser ? BrightenForDark(colorA, 160.f) : BrightenForDark(colorB, 160.f);
        if (isUser) {
          dl->AddText(g_ManagerFontBold, 11.0f, ImVec2(wPos.x + 10, gly), gCol, buf);
        } else {
          ImVec2 tsz = g_ManagerFontBold
            ? g_ManagerFontBold->CalcTextSizeA(11.0f, FLT_MAX, 0, buf)
            : ImVec2(80, 11);
          dl->AddText(g_ManagerFontBold, 11.0f,
                      ImVec2(wPos.x + panW - tsz.x - 10, gly), gCol, buf);
        }
        gly += 16.0f;
      }
    }
  }
  curY += goalLogH;

  // ── Possession row ────────────────────────────────────────────────────────
  dl->AddRectFilled(ImVec2(wPos.x, curY), ImVec2(wPos.x + panW, curY + posRowH),
                    IM_COL32(12, 18, 42, 255));

  AddTextCentered(dl, g_ManagerFontHero, 10.0f,
                  ImVec2(wPos.x, curY), ImVec2(wPos.x + panW, curY + posRowH * 0.42f),
                  IM_COL32(140, 160, 210, 180), "Possession");

  char pABuf[8], pBBuf[8];
  snprintf(pABuf, sizeof(pABuf), "%d%%", posPercA);
  snprintf(pBBuf, sizeof(pBBuf), "%d%%", posPercB);

  float pmid = curY + posRowH * 0.5f + 4.0f;
  AddTextCentered(dl, g_ManagerFontBold, 13.0f,
                  ImVec2(wPos.x + 4, curY), ImVec2(wPos.x + 54, curY + posRowH),
                  IM_COL32(255,255,255,255), pABuf);
  AddTextCentered(dl, g_ManagerFontBold, 13.0f,
                  ImVec2(wPos.x + panW - 54, curY), ImVec2(wPos.x + panW - 4, curY + posRowH),
                  IM_COL32(255,255,255,255), pBBuf);

  {
    const float tH  = 7.0f;
    const float tX0 = wPos.x + 58.0f, tX1 = wPos.x + panW - 58.0f;
    const float tY  = pmid - tH * 0.5f;
    float spX = tX0 + (tX1 - tX0) * ((float)posPercA / 100.0f);
    dl->AddRectFilled(ImVec2(tX0, tY), ImVec2(tX1, tY + tH), IM_COL32(30,40,70,200), 3.0f);
    if (posPercA > 0) dl->AddRectFilled(ImVec2(tX0, tY), ImVec2(spX, tY + tH), colorA, 3.0f);
    if (posPercB > 0) dl->AddRectFilled(ImVec2(spX, tY), ImVec2(tX1, tY + tH), colorB, 3.0f);
  }
  curY += posRowH;

  // ── Stat rows ─────────────────────────────────────────────────────────────
  for (int i = 0; i < numRows; i++) {
    const StatRow &r = rows[i];
    ImU32 rowBg = (i % 2 == 0) ? IM_COL32(16,24,46,255) : IM_COL32(12,18,38,255);
    dl->AddRectFilled(ImVec2(wPos.x, curY), ImVec2(wPos.x + panW, curY + statRowH), rowBg);

    AddTextCentered(dl, g_ManagerFontHero, 10.0f,
                    ImVec2(wPos.x, curY),
                    ImVec2(wPos.x + panW, curY + statRowH * 0.42f),
                    IM_COL32(140, 160, 210, 180), r.label);

    DrawStatBar(dl, ImVec2(wPos.x + 58.0f, curY + statRowH * 0.42f),
                panW - 116.0f, statRowH * 0.58f, r.a, r.b, colorA, colorB);

    char vA[16], vB[16];
    if (r.isPercent) {
      snprintf(vA, sizeof(vA), "%d%%", r.a);
      snprintf(vB, sizeof(vB), "%d%%", r.b);
    } else {
      snprintf(vA, sizeof(vA), "%d", r.a);
      snprintf(vB, sizeof(vB), "%d", r.b);
    }
    AddTextCentered(dl, g_ManagerFontBold, 13.0f,
                    ImVec2(wPos.x + 4, curY), ImVec2(wPos.x + 56, curY + statRowH),
                    IM_COL32(255,255,255,255), vA);
    AddTextCentered(dl, g_ManagerFontBold, 13.0f,
                    ImVec2(wPos.x + panW - 56, curY), ImVec2(wPos.x + panW - 4, curY + statRowH),
                    IM_COL32(255,255,255,255), vB);
    curY += statRowH;
  }

  // Top-rounded border only
  dl->AddRect(ImVec2(wPos.x, wPos.y), ImVec2(wPos.x + panW, wPos.y + panH),
              IM_COL32(55, 75, 140, 180), kRound, ImDrawFlags_RoundCornersTop, 1.5f);

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
}

void RenderTacticsPanel() {
  if (!g_TacticsPanelVisible) return;

  const ImGuiIO &io  = ImGui::GetIO();
  const float    sw  = io.DisplaySize.x;
  const float    sh  = io.DisplaySize.y;

  const float kRound    = 12.0f;
  const float panW      = 490.0f;
  const float hdrH      = 42.0f;
  const float scrollH   = 444.0f;  // content area height (scrollable)
  const float panH      = hdrH + scrollH;

  if (s_tacticsPanelX < 0.f) {
    // Default: slightly left of center so it doesn't overlap match stats
    s_tacticsPanelX = (sw - panW) * 0.5f - 230.0f;
    s_tacticsPanelY = (sh - panH) * 0.5f - 50.0f;
  }
  if (s_tacticsPanelX < 0.f)          s_tacticsPanelX = 0.f;
  if (s_tacticsPanelY < 0.f)          s_tacticsPanelY = 0.f;
  if (s_tacticsPanelX + panW > sw)    s_tacticsPanelX = sw - panW;
  if (s_tacticsPanelY + panH > sh)    s_tacticsPanelY = sh - panH;

  ImGui::SetNextWindowPos(ImVec2(s_tacticsPanelX, s_tacticsPanelY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(panW, panH), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,    ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding,   0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,      ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 0));

  ImGui::Begin("##tactics_panel", nullptr,
    ImGuiWindowFlags_NoTitleBar        |
    ImGuiWindowFlags_NoResize          |
    ImGuiWindowFlags_NoMove            |
    ImGuiWindowFlags_NoScrollbar       |
    ImGuiWindowFlags_NoSavedSettings   |
    ImGuiWindowFlags_NoFocusOnAppearing|
    ImGuiWindowFlags_NoNav);

  ImDrawList *dl   = ImGui::GetWindowDrawList();
  ImVec2      wPos = ImGui::GetWindowPos();
  float       curY = wPos.y;

  // Rounded background on the back draw list (top corners only).
  ImDrawList *bgdl = ImGui::GetBackgroundDrawList();
  bgdl->AddRectFilled(ImVec2(wPos.x, wPos.y), ImVec2(wPos.x + panW, wPos.y + panH),
                      IM_COL32(0, 0, 0, 70), kRound, ImDrawFlags_RoundCornersTop);
  bgdl->AddRectFilled(ImVec2(wPos.x, wPos.y), ImVec2(wPos.x + panW, wPos.y + panH),
                      IM_COL32(10, 16, 34, 252), kRound, ImDrawFlags_RoundCornersTop);

  // ── Header ────────────────────────────────────────────────────────────────
  dl->AddRectFilled(ImVec2(wPos.x, curY), ImVec2(wPos.x + panW, curY + hdrH),
                    IM_COL32(16, 24, 52, 255), kRound, ImDrawFlags_RoundCornersTop);
  // Club color accent strip on left
  dl->AddRectFilled(ImVec2(wPos.x, curY), ImVec2(wPos.x + 4.f, curY + hdrH), s_pauseUserColor);
  AddTextCentered(dl, g_ManagerFontBold, 17.0f,
                  ImVec2(wPos.x, curY), ImVec2(wPos.x + panW, curY + hdrH),
                  IM_COL32(200, 215, 255, 255), "TACTICS");

  // Drag handle
  ImGui::SetCursorScreenPos(ImVec2(wPos.x, curY));
  ImGui::InvisibleButton("##drag_tac", ImVec2(panW - 40.0f, hdrH));
  if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
    ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left);
    ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
    s_tacticsPanelX += delta.x;
    s_tacticsPanelY += delta.y;
    if (s_tacticsPanelX < 0.f)       s_tacticsPanelX = 0.f;
    if (s_tacticsPanelY < 0.f)       s_tacticsPanelY = 0.f;
    if (s_tacticsPanelX + panW > sw) s_tacticsPanelX = sw - panW;
    if (s_tacticsPanelY + panH > sh) s_tacticsPanelY = sh - panH;
  }
  if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);

  // Close [X]
  ImVec2 txMin(wPos.x + panW - 34.0f, curY + 7.0f);
  ImVec2 txMax(wPos.x + panW - 7.0f,  curY + hdrH - 7.0f);
  ImGui::SetCursorScreenPos(txMin);
  ImGui::InvisibleButton("##close_tac", ImVec2(txMax.x - txMin.x, txMax.y - txMin.y));
  bool txHov = ImGui::IsItemHovered();
  dl->AddRectFilled(txMin, txMax, txHov ? IM_COL32(220,60,60,220) : IM_COL32(40,50,90,180), 4.0f);
  AddTextCentered(dl, g_ManagerFontBold, 17.0f, txMin, txMax, IM_COL32(255,255,255,255), "X");
  if (ImGui::IsItemClicked()) g_TacticsPanelVisible = false;
  curY += hdrH;

  // ── Scrollable content ────────────────────────────────────────────────────
  const float kRowH   = 30.0f;
  const float kSecH   = 22.0f;
  const float kBtnGap = 3.0f;
  const float kPadX   = 8.0f;

  // Category colours matching career screen
  const ImU32 kCatBgAtk  = IM_COL32(18, 100, 42, 220);
  const ImU32 kCatBgDef  = IM_COL32(16, 56, 130, 220);
  const ImU32 kCatBgBall = IM_COL32(130, 75, 10, 220);
  const ImU32 kCatTxtAtk  = IM_COL32(50, 210, 95, 255);
  const ImU32 kCatTxtDef  = IM_COL32(90, 165, 255, 255);
  const ImU32 kCatTxtBall = IM_COL32(255, 190, 60, 255);

  ImGui::SetCursorScreenPos(ImVec2(wPos.x, curY));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(10, 16, 34, 255));
  ImGui::BeginChild("##tac_scroll", ImVec2(panW, scrollH), false, ImGuiWindowFlags_None);
  ImDrawList *cdl  = ImGui::GetWindowDrawList();
  ImVec2      cpos = ImGui::GetWindowPos();
  float       cy   = cpos.y;

  const char *lastCat = nullptr;
  for (int ti = 0; ti < kNumMatchTacInstr; ti++) {
    const TacInstRow &ins = kMatchTacInstr[ti];

    // Section header on category change
    if (!lastCat || strcmp(lastCat, ins.category) != 0) {
      lastCat = ins.category;
      ImU32 catBg  = kCatBgAtk,  catTxt = kCatTxtAtk;
      if (strcmp(ins.category, "Defending")    == 0) { catBg = kCatBgDef;  catTxt = kCatTxtDef;  }
      if (strcmp(ins.category, "On the Ball")  == 0) { catBg = kCatBgBall; catTxt = kCatTxtBall; }
      cdl->AddRectFilled(ImVec2(cpos.x, cy), ImVec2(cpos.x + panW, cy + kSecH), catBg);
      cdl->AddRectFilled(ImVec2(cpos.x, cy), ImVec2(cpos.x + 3.f,  cy + kSecH), catTxt);
      char secBuf[48]; snprintf(secBuf, sizeof(secBuf), "   %s", ins.category);
      if (g_ManagerFontSmall)
        cdl->AddText(g_ManagerFontSmall, 15.0f,
                     ImVec2(cpos.x + 8, cy + (kSecH - 15.0f) * 0.5f), catTxt, secBuf);
      cy += kSecH;
    }

    // Row bg (alternating)
    ImU32 rowBg = (ti % 2 == 0) ? IM_COL32(14, 22, 46, 255) : IM_COL32(11, 17, 38, 255);
    cdl->AddRectFilled(ImVec2(cpos.x, cy), ImVec2(cpos.x + panW, cy + kRowH), rowBg);

    // Tactic label (left 28%)
    float nameW = panW * 0.28f;
    if (g_ManagerFontSmall)
      cdl->AddText(g_ManagerFontSmall, 15.0f,
                   ImVec2(cpos.x + kPadX, cy + (kRowH - 15.0f) * 0.5f),
                   IM_COL32(185, 200, 230, 255), ins.name);

    // Resolve active preset from the live cache (populated from TeamData at match start).
    float curVal  = ins.presets[1].value; // fallback: "Balanced"
    auto  it = s_liveTactics.find(ins.key);
    if (it != s_liveTactics.end()) curVal = it->second;
    int selPreset = 0;
    float bestD = 9999.f;
    for (int pi = 0; pi < 4; pi++) {
      float d = fabsf(ins.presets[pi].value - curVal);
      if (d < bestD) { bestD = d; selPreset = pi; }
    }

    // Preset buttons (right 64%)
    float btnAreaX = cpos.x + nameW;
    float btnAreaW = panW - nameW - kPadX;
    float btnW     = (btnAreaW - kBtnGap * 3.0f) / 4.0f;
    float btnH     = kRowH - 6.0f;
    float btnY     = cy + 3.0f;

    for (int pi = 0; pi < 4; pi++) {
      float bx = btnAreaX + pi * (btnW + kBtnGap);
      bool  sel = (pi == selPreset);

      ImVec2 bMin(bx, btnY);
      ImVec2 bMax(bx + btnW, btnY + btnH);

      char btnId[64]; snprintf(btnId, sizeof(btnId), "##t%d_p%d", ti, pi);
      ImGui::SetCursorScreenPos(bMin);
      ImGui::PushID(btnId);
      bool clicked = ImGui::InvisibleButton(btnId, ImVec2(btnW, btnH));
      ImGui::PopID();
      bool hov = ImGui::IsItemHovered();

      ImU32 bg = sel ? s_pauseUserColor
               : hov ? IM_COL32(40, 55, 110, 220)
                     : IM_COL32(20, 30, 60, 200);
      cdl->AddRectFilled(bMin, bMax, bg, 4.0f);

      ImU32 tc = sel ? TextColorForBg(s_pauseUserColor)
               : hov ? IM_COL32(220, 230, 255, 255)
                     : IM_COL32(130, 150, 200, 210);
      AddTextCentered(cdl, g_ManagerFontSmall, 15.0f, bMin, bMax, tc, ins.presets[pi].label);

      if (clicked) {
        float newVal = ins.presets[pi].value;
        // Update local cache for immediate visual feedback
        s_liveTactics[ins.key] = newVal;
        // Queue for game-thread application to TeamData (takes effect within ~1 s)
        TacticChange tc2;
        tc2.key     = ins.key;
        tc2.value   = newVal;
        tc2.teamIdx = s_subUserTeamIdx;
        g_PendingTacticsChanges.push_back(tc2);
        // Persist to DB using s_liveTactics as source of truth
        {
          std::stringstream xml;
          for (const auto &kv : s_liveTactics)
            xml << "<" << kv.first << ">" << kv.second << "</" << kv.first << ">\n";
          std::string xmlStr = xml.str();
          std::string esc; esc.reserve(xmlStr.size());
          for (char c : xmlStr) { if (c == '\'') esc += "''"; else esc += c; }
          int clubId = g_CareerMatchContext.userClubId;
          if (clubId > 0) {
            std::stringstream q;
            q << "UPDATE teams SET tactics_xml='" << esc << "' WHERE id=" << clubId << ";";
            DatabaseResult *r = GetDB()->Query(q.str());
            delete r;
          }
        }
      }
    }

    // Row bottom divider
    cdl->AddLine(ImVec2(cpos.x, cy + kRowH - 1), ImVec2(cpos.x + panW, cy + kRowH - 1),
                 IM_COL32(30, 45, 80, 120), 1.0f);
    cy += kRowH;
  }

  ImGui::Dummy(ImVec2(panW, 4.0f)); // bottom padding
  ImGui::EndChild();
  ImGui::PopStyleColor();

  // Top-rounded border only
  dl->AddRect(ImVec2(wPos.x, wPos.y), ImVec2(wPos.x + panW, wPos.y + panH),
              IM_COL32(55, 75, 140, 180), kRound, ImDrawFlags_RoundCornersTop, 1.5f);

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(4);
}

void RenderImGuiMatchPauseOverlay() {
  if (!g_ImGuiIngamePauseMenuActive) return;
  if (g_ImGuiPausePendingAction != 0) return;

  ImGuiIO &io = ImGui::GetIO();

  // Dim overlay behind all ImGui windows
  ImGui::GetBackgroundDrawList()->AddRectFilled(
      ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 145));

  const float popW = 380.0f;
  const float popH = 200.0f;
  const float popX = (io.DisplaySize.x - popW) * 0.5f;
  const float popY = (io.DisplaySize.y - popH) * 0.5f;

  ImGui::SetNextWindowPos(ImVec2(popX, popY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(popW, popH), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
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

  ImDrawList *dl   = ImGui::GetWindowDrawList();
  ImVec2      wPos = ImGui::GetWindowPos();

  // Title bar strip
  dl->AddRectFilled(ImVec2(wPos.x, wPos.y),
                    ImVec2(wPos.x + popW, wPos.y + 44.0f),
                    IM_COL32(20, 30, 58, 255), 10.0f);
  AddTextCentered(dl, g_ManagerFontBold, 16.0f,
                  ImVec2(wPos.x, wPos.y),
                  ImVec2(wPos.x + popW, wPos.y + 44.0f),
                  IM_COL32(255, 255, 255, 255), "SETTINGS");

  // Placeholder text
  AddTextCentered(dl, g_ManagerFontHero, 14.0f,
                  ImVec2(wPos.x, wPos.y + 60.0f),
                  ImVec2(wPos.x + popW, wPos.y + 130.0f),
                  IM_COL32(160, 180, 220, 180), "Coming soon");

  // Resume button
  const float btnW = 140.0f;
  const float btnH = 32.0f;
  ImVec2 btnMin = ImVec2(wPos.x + (popW - btnW) * 0.5f, wPos.y + popH - btnH - 16.0f);
  ImVec2 btnMax = ImVec2(btnMin.x + btnW, btnMin.y + btnH);
  ImGui::SetCursorScreenPos(btnMin);
  ImGui::InvisibleButton("##resume_btn", ImVec2(btnW, btnH));
  bool hov = ImGui::IsItemHovered();
  dl->AddRectFilled(btnMin, btnMax,
                    hov ? IM_COL32(255, 255, 255, 210) : IM_COL32(30, 44, 80, 220), 6.0f);
  dl->AddRect(btnMin, btnMax, IM_COL32(80, 110, 180, 160), 6.0f, 0, 1.0f);
  ImU32 btnTxt = hov ? IM_COL32(15, 15, 15, 255) : IM_COL32(180, 200, 240, 255);
  AddTextCentered(dl, g_ManagerFontBold, 13.0f, btnMin, btnMax, btnTxt, "RESUME");
  if (ImGui::IsItemClicked()) g_ImGuiPausePendingAction = 1;

  ImGui::End();
  ImGui::PopStyleColor();
  ImGui::PopStyleVar(3);
}
