#include "imgui_career.hpp"
#include "imgui_manager_fonts.hpp"

#include "imgui.h"
#include <SDL2/SDL_image.h>
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>

#include "main.hpp"
#include "utils/database.hpp"
#include "base/utils.hpp"

CareerHubState g_CareerHub;

// ---- Font globals -------------------------------------------------------

ImFont *g_ManagerFontSmall   = nullptr;
ImFont *g_ManagerFontRegular = nullptr;
ImFont *g_ManagerFontMedium  = nullptr;
ImFont *g_ManagerFontBold    = nullptr;
ImFont *g_ManagerFontTitle   = nullptr;
ImFont *g_ManagerFontHero    = nullptr;

// ---- Font loading -------------------------------------------------------

void LoadManagerFonts() {
  ImFontAtlas *atlas = ImGui::GetIO().Fonts;

  static const char *kRegularPaths[] = {
    "data/media/fonts/Inter-Regular.ttf",
    "data/media/fonts/Roboto-Regular.ttf",
    "data/media/fonts/NotoSans-Regular.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/liberation-fonts/LiberationSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    nullptr
  };
  static const char *kBoldPaths[] = {
    "data/media/fonts/Inter-SemiBold.ttf",
    "data/media/fonts/Roboto-Bold.ttf",
    "data/media/fonts/NotoSans-Bold.ttf",
    "/usr/share/fonts/noto/NotoSans-Bold.ttf",
    "/usr/share/fonts/liberation-fonts/LiberationSans-Bold.ttf",
    "/usr/share/fonts/TTF/DejaVuSans-Bold.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans-Bold.ttf",
    nullptr
  };

  auto tryLoad = [](ImFontAtlas *a, const char **paths, float sz) -> ImFont * {
    for (; *paths; ++paths) {
      FILE *f = fopen(*paths, "rb");
      if (!f) continue;
      fclose(f);
      ImFont *font = a->AddFontFromFileTTF(*paths, sz);
      if (font) {
        printf("[IMGUI] Loaded manager font: %s (%.0fpx)\n", *paths, sz);
        return font;
      }
    }
    return nullptr;
  };

  g_ManagerFontSmall   = tryLoad(atlas, kRegularPaths, 13.0f);
  g_ManagerFontRegular = tryLoad(atlas, kRegularPaths, 15.0f);
  g_ManagerFontMedium  = tryLoad(atlas, kRegularPaths, 17.0f);
  g_ManagerFontBold    = tryLoad(atlas, kBoldPaths,    18.0f);
  g_ManagerFontTitle   = tryLoad(atlas, kBoldPaths,    24.0f);
  g_ManagerFontHero    = tryLoad(atlas, kBoldPaths,    32.0f);

  if (!g_ManagerFontRegular)
    printf("[IMGUI] Font load failed, using default ImGui font\n");
  else
    printf("[IMGUI] Loaded manager bold font: loaded\n");
}

// ---- DB cell helper -----------------------------------------------------

static std::string DBCell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size() || col >= r->data.at(row).size()) return "";
  return r->data.at(row).at(col);
}

// ---- Badge texture cache ------------------------------------------------

static std::map<std::string, GLuint> s_BadgeCache;
static bool s_BadgeCacheLogged = false;

static GLuint LoadBadgeTex(const std::string &logoRelPath) {
  if (logoRelPath.empty()) return 0;
  std::string fullPath = "databases/default/" + logoRelPath;
  auto it = s_BadgeCache.find(fullPath);
  if (it != s_BadgeCache.end()) return it->second;
  if (!s_BadgeCacheLogged) {
    printf("[IMGUI MANAGER] badge cache initialized\n");
    s_BadgeCacheLogged = true;
  }
  SDL_Surface *surf = IMG_Load(fullPath.c_str());
  if (!surf) {
    printf("[IMGUI MANAGER] failed badge load for team %s, using fallback\n", fullPath.c_str());
    s_BadgeCache[fullPath] = 0;
    return 0;
  }
  SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(surf);
  if (!rgba) { s_BadgeCache[fullPath] = 0; return 0; }
  GLuint texID = 0;
  glGenTextures(1, &texID);
  glBindTexture(GL_TEXTURE_2D, texID);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
  glBindTexture(GL_TEXTURE_2D, 0);
  SDL_FreeSurface(rgba);
  printf("[IMGUI MANAGER] loaded badge for team %s\n", logoRelPath.c_str());
  s_BadgeCache[fullPath] = texID;
  return texID;
}

static void ClearBadgeCache() {
  for (auto &kv : s_BadgeCache)
    if (kv.second != 0) glDeleteTextures(1, &kv.second);
  s_BadgeCache.clear();
  s_BadgeCacheLogged = false;
}

// ---- State management ---------------------------------------------------

void CareerHubState::Clear() {
  active        = false;
  primaryTab    = 0;
  activeTab     = 0;
  pendingAction = 0;
  onPlayMatch   = nullptr;
  onMainMenu    = nullptr;
  manager = {};
  club    = {};
  players.clear();
  fixtures.clear();
  standings.clear();
  ClearBadgeCache();
}

void CareerHubState::LoadFromDB(int managerId, int clubId) {
  active = false;

  {
    std::stringstream q;
    q << "SELECT managers.name, managers.age, managers.nationality, managers.gender, teams.name"
      << " FROM managers LEFT JOIN teams ON managers.club_id = teams.id"
      << " WHERE managers.id = " << managerId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str());
    manager.name        = DBCell(r, 0, 0);
    manager.age         = DBCell(r, 0, 1);
    manager.nationality = DBCell(r, 0, 2);
    manager.gender      = DBCell(r, 0, 3);
    manager.clubName    = DBCell(r, 0, 4);
    delete r;
  }

  {
    std::stringstream q;
    q << "SELECT teams.name, teams.shortname, teams.logo_url, leagues.name"
      << " FROM teams JOIN leagues ON teams.league_id = leagues.id"
      << " WHERE teams.id = " << clubId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str());
    club.name       = DBCell(r, 0, 0);
    club.shortName  = DBCell(r, 0, 1);
    club.logoPath   = DBCell(r, 0, 2);
    club.leagueName = DBCell(r, 0, 3);
    delete r;
  }

  players.clear();
  {
    std::stringstream q;
    q << "SELECT firstname, lastname, role, age, base_stat"
      << " FROM players WHERE team_id = " << clubId
      << " ORDER BY formationorder ASC, base_stat DESC LIMIT 22;";
    DatabaseResult *r = GetDB()->Query(q.str());
    for (unsigned int i = 0; i < r->data.size(); i++) {
      Player p;
      p.firstName = DBCell(r, i, 0);
      p.lastName  = DBCell(r, i, 1);
      p.role      = DBCell(r, i, 2);
      p.age       = DBCell(r, i, 3);
      p.ability   = DBCell(r, i, 4);
      players.push_back(p);
    }
    delete r;
  }

  fixtures.clear();
  {
    std::stringstream q;
    q << "SELECT leagues.name, fixtures.matchday, fixtures.round,"
      << " home.shortname, away.shortname,"
      << " home.logo_url, away.logo_url,"
      << " fixtures.status, fixtures.home_score, fixtures.away_score"
      << " FROM fixtures"
      << " JOIN leagues ON fixtures.league_id = leagues.id"
      << " JOIN teams home ON fixtures.home_team_id = home.id"
      << " JOIN teams away ON fixtures.away_team_id = away.id"
      << " WHERE fixtures.manager_id = " << managerId
      << " ORDER BY leagues.id ASC, fixtures.matchday ASC, fixtures.round ASC;";
    DatabaseResult *r = GetDB()->Query(q.str());
    for (unsigned int i = 0; i < r->data.size(); i++) {
      Fixture f;
      f.league   = DBCell(r, i, 0);
      f.matchday = DBCell(r, i, 1);
      f.round    = DBCell(r, i, 2);
      f.home     = DBCell(r, i, 3);
      f.away     = DBCell(r, i, 4);
      f.homeLogo = DBCell(r, i, 5);
      f.awayLogo = DBCell(r, i, 6);
      f.status   = DBCell(r, i, 7);
      std::string hs = DBCell(r, i, 8);
      std::string as2 = DBCell(r, i, 9);
      f.score = (f.status != "scheduled" && !hs.empty()) ? hs + " - " + as2 : "";
      fixtures.push_back(f);
    }
    delete r;
  }

  standings.clear();
  {
    std::stringstream q;
    q << "SELECT leagues.name, teams.shortname, teams.logo_url,"
      << " standings.played, standings.won, standings.drawn, standings.lost,"
      << " standings.goals_for, standings.goals_against,"
      << " standings.goal_difference, standings.points"
      << " FROM standings"
      << " JOIN leagues ON standings.league_id = leagues.id"
      << " JOIN teams ON standings.team_id = teams.id"
      << " WHERE standings.manager_id = " << managerId
      << " ORDER BY leagues.id ASC, standings.points DESC,"
      << " standings.goal_difference DESC, standings.goals_for DESC;";
    DatabaseResult *r = GetDB()->Query(q.str());
    for (unsigned int i = 0; i < r->data.size(); i++) {
      Standing s;
      s.league   = DBCell(r, i, 0);
      s.team     = DBCell(r, i, 1);
      s.teamLogo = DBCell(r, i, 2);
      s.p        = DBCell(r, i, 3);
      s.w        = DBCell(r, i, 4);
      s.d        = DBCell(r, i, 5);
      s.l        = DBCell(r, i, 6);
      s.gf       = DBCell(r, i, 7);
      s.ga       = DBCell(r, i, 8);
      s.gd       = DBCell(r, i, 9);
      s.pts      = DBCell(r, i, 10);
      standings.push_back(s);
    }
    delete r;
  }

  active = true;
}

// ---- Color palette ------------------------------------------------------

static const ImVec4 kBgApp      = ImVec4(0.020f, 0.031f, 0.059f, 1.0f);
static const ImVec4 kBgPanel    = ImVec4(0.031f, 0.047f, 0.086f, 1.0f);
static const ImVec4 kBgCard     = ImVec4(0.047f, 0.071f, 0.129f, 1.0f);
static const ImVec4 kBgCardAlt  = ImVec4(0.035f, 0.055f, 0.102f, 1.0f);
static const ImVec4 kBorder     = ImVec4(0.082f, 0.122f, 0.220f, 0.70f);
static const ImVec4 kAccent     = ImVec4(0.741f, 0.102f, 0.788f, 1.0f);
static const ImVec4 kAccentH    = ImVec4(0.863f, 0.318f, 0.918f, 1.0f);
static const ImVec4 kAccentA    = ImVec4(0.576f, 0.047f, 0.620f, 1.0f);
static const ImVec4 kViolet     = ImVec4(0.482f, 0.231f, 0.929f, 1.0f);
static const ImVec4 kVioletH    = ImVec4(0.580f, 0.330f, 0.970f, 1.0f);
static const ImVec4 kVioletA    = ImVec4(0.380f, 0.160f, 0.800f, 1.0f);
static const ImVec4 kTextPri    = ImVec4(0.937f, 0.949f, 0.965f, 1.0f);
static const ImVec4 kTextSec    = ImVec4(0.502f, 0.573f, 0.675f, 1.0f);
static const ImVec4 kTextDim    = ImVec4(0.239f, 0.290f, 0.388f, 1.0f);
static const ImVec4 kSuccess    = ImVec4(0.133f, 0.773f, 0.369f, 1.0f);
static const ImVec4 kWarning    = ImVec4(0.973f, 0.620f, 0.043f, 1.0f);
static const ImVec4 kDanger     = ImVec4(0.937f, 0.267f, 0.267f, 1.0f);
static const ImVec4 kGold       = ImVec4(0.992f, 0.820f, 0.110f, 1.0f);
static const ImVec4 kBlue       = ImVec4(0.361f, 0.682f, 0.941f, 1.0f);

static inline ImU32 C32(const ImVec4 &v) { return ImGui::ColorConvertFloat4ToU32(v); }

// ---- Theme --------------------------------------------------------------

static void ApplyManagerTheme() {
  ImGuiStyle &st = ImGui::GetStyle();
  st.WindowRounding    = 0.0f;
  st.ChildRounding     = 10.0f;
  st.FrameRounding     = 5.0f;
  st.ScrollbarRounding = 4.0f;
  st.GrabRounding      = 4.0f;
  st.PopupRounding     = 6.0f;
  st.WindowBorderSize  = 0.0f;
  st.ChildBorderSize   = 0.0f;
  st.FrameBorderSize   = 0.0f;
  st.WindowPadding     = ImVec2(0.0f, 0.0f);
  st.FramePadding      = ImVec2(12.0f, 6.0f);
  st.ItemSpacing       = ImVec2(10.0f, 6.0f);
  st.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
  st.CellPadding       = ImVec2(8.0f, 5.0f);
  st.ScrollbarSize     = 6.0f;
  st.IndentSpacing     = 14.0f;

  ImVec4 *c = st.Colors;
  c[ImGuiCol_WindowBg]              = kBgApp;
  c[ImGuiCol_ChildBg]               = kBgCard;
  c[ImGuiCol_PopupBg]               = kBgCard;
  c[ImGuiCol_Border]                = kBorder;
  c[ImGuiCol_BorderShadow]          = ImVec4(0,0,0,0);
  c[ImGuiCol_FrameBg]               = ImVec4(0.055f, 0.082f, 0.153f, 1.0f);
  c[ImGuiCol_FrameBgHovered]        = ImVec4(0.082f, 0.122f, 0.220f, 1.0f);
  c[ImGuiCol_FrameBgActive]         = ImVec4(0.110f, 0.161f, 0.282f, 1.0f);
  c[ImGuiCol_TitleBg]               = kBgPanel;
  c[ImGuiCol_TitleBgActive]         = kBgPanel;
  c[ImGuiCol_TitleBgCollapsed]      = kBgPanel;
  c[ImGuiCol_MenuBarBg]             = kBgPanel;
  c[ImGuiCol_ScrollbarBg]           = ImVec4(0,0,0,0);
  c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.110f, 0.161f, 0.282f, 1.0f);
  c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.176f, 0.243f, 0.396f, 1.0f);
  c[ImGuiCol_ScrollbarGrabActive]   = kViolet;
  c[ImGuiCol_CheckMark]             = kAccent;
  c[ImGuiCol_SliderGrab]            = kViolet;
  c[ImGuiCol_SliderGrabActive]      = kVioletH;
  c[ImGuiCol_Button]                = ImVec4(0.055f, 0.082f, 0.153f, 1.0f);
  c[ImGuiCol_ButtonHovered]         = ImVec4(0.082f, 0.122f, 0.220f, 1.0f);
  c[ImGuiCol_ButtonActive]          = ImVec4(0.110f, 0.161f, 0.282f, 1.0f);
  c[ImGuiCol_Header]                = ImVec4(0.082f, 0.122f, 0.220f, 0.60f);
  c[ImGuiCol_HeaderHovered]         = ImVec4(0.110f, 0.161f, 0.282f, 0.80f);
  c[ImGuiCol_HeaderActive]          = ImVec4(0.176f, 0.243f, 0.396f, 1.0f);
  c[ImGuiCol_Separator]             = kBorder;
  c[ImGuiCol_SeparatorHovered]      = kViolet;
  c[ImGuiCol_SeparatorActive]       = kViolet;
  c[ImGuiCol_ResizeGrip]            = ImVec4(0,0,0,0);
  c[ImGuiCol_ResizeGripHovered]     = ImVec4(0,0,0,0);
  c[ImGuiCol_ResizeGripActive]      = ImVec4(0,0,0,0);
  c[ImGuiCol_TableHeaderBg]         = ImVec4(0.031f, 0.047f, 0.086f, 1.0f);
  c[ImGuiCol_TableBorderStrong]     = kBorder;
  c[ImGuiCol_TableBorderLight]      = ImVec4(0.055f, 0.082f, 0.153f, 1.0f);
  c[ImGuiCol_TableRowBg]            = ImVec4(0,0,0,0);
  c[ImGuiCol_TableRowBgAlt]         = ImVec4(0.035f, 0.055f, 0.102f, 0.40f);
  c[ImGuiCol_Text]                  = kTextPri;
  c[ImGuiCol_TextDisabled]          = kTextDim;
  c[ImGuiCol_NavHighlight]          = kAccent;
  c[ImGuiCol_NavWindowingHighlight] = kViolet;
  c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0,0,0,0.5f);
  c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0,0,0,0.5f);
}

// ---- Font helpers -------------------------------------------------------

static inline void PushMgrFont(ImFont *font) { if (font) ImGui::PushFont(font); }
static inline void PopMgrFont(ImFont *font)  { if (font) ImGui::PopFont(); }

// ---- Button helpers -----------------------------------------------------

static bool CTAButton(const char *lbl, ImVec2 sz = ImVec2(0,0)) {
  ImGui::PushStyleColor(ImGuiCol_Button,        kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentH);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kAccentA);
  bool r = ImGui::Button(lbl, sz);
  ImGui::PopStyleColor(3);
  return r;
}
static bool SecBtn(const char *lbl, ImVec2 sz = ImVec2(0,0)) {
  ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.055f,0.082f,0.153f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.082f,0.122f,0.220f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.110f,0.161f,0.282f,1.0f));
  bool r = ImGui::Button(lbl, sz);
  ImGui::PopStyleColor(3);
  return r;
}
static bool TabBtn(const char *lbl, bool active, ImVec2 sz = ImVec2(100,32)) {
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button,        kViolet);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kVioletH);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kVioletA);
    ImGui::PushStyleColor(ImGuiCol_Text,          kTextPri);
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.082f,0.122f,0.220f,0.70f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.110f,0.161f,0.282f,1.0f));
    ImGui::PushStyleColor(ImGuiCol_Text,          kTextSec);
  }
  PushMgrFont(active ? g_ManagerFontBold : g_ManagerFontRegular);
  bool r = ImGui::Button(lbl, sz);
  PopMgrFont(active ? g_ManagerFontBold : g_ManagerFontRegular);
  ImGui::PopStyleColor(4);
  return r;
}

// Top-bar nav button with magenta underline when active.
static bool GhostNavBtn(const char *lbl, bool active, ImVec2 sz = ImVec2(90, 36)) {
  ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.082f,0.122f,0.220f,0.5f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.110f,0.161f,0.282f,0.7f));
  ImGui::PushStyleColor(ImGuiCol_Text,          active ? kTextPri : kTextSec);
  PushMgrFont(active ? g_ManagerFontBold : g_ManagerFontRegular);
  bool r = ImGui::Button(lbl, sz);
  PopMgrFont(active ? g_ManagerFontBold : g_ManagerFontRegular);
  ImGui::PopStyleColor(4);
  if (active) {
    ImVec2 p0 = ImGui::GetItemRectMin();
    ImVec2 p1 = ImGui::GetItemRectMax();
    float inset = sz.x * 0.20f;
    ImGui::GetWindowDrawList()->AddRectFilled(
      ImVec2(p0.x + inset, p1.y - 3.0f),
      ImVec2(p1.x - inset, p1.y),
      C32(kAccent), 1.5f);
  }
  return r;
}

// ---- Modern card drawing ------------------------------------------------
// Draws a custom rounded rect on the parent DrawList, then starts a child
// window with transparent background on top.

static void BeginModernCard(const char *id, ImVec2 size, const char *title = nullptr) {
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
  ImDrawList *dl = ImGui::GetWindowDrawList();

  // Background
  dl->AddRectFilled(p0, p1, C32(kBgCard), 10.0f);
  // Border
  dl->AddRect(p0, p1, C32(kBorder), 10.0f, 0, 1.0f);
  // Subtle top highlight
  dl->AddLine(ImVec2(p0.x + 12, p0.y + 1), ImVec2(p1.x - 12, p0.y + 1),
              IM_COL32(255, 255, 255, 8), 1.0f);

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::BeginChild(id, size, false);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (title) {
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.082f, 0.122f, 0.220f, 0.50f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
}

static void EndModernCard() { ImGui::EndChild(); }

// ---- App background -----------------------------------------------------

static void DrawAppBackground(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 p0 = wp;
  ImVec2 p1 = ImVec2(wp.x + w, wp.y + h);

  dl->AddRectFilled(p0, p1, C32(kBgApp));

  // Bottom-right purple glow
  ImVec2 br = ImVec2(p1.x, p1.y);
  dl->AddCircleFilled(br, w * 0.50f, IM_COL32(55, 18, 110, 12), 48);
  dl->AddCircleFilled(br, w * 0.25f, IM_COL32(80, 28, 150, 16), 48);

  // Top subtle blue tint
  dl->AddRectFilledMultiColor(
    p0, ImVec2(p1.x, wp.y + 100.0f),
    IM_COL32(18, 28, 65, 35), IM_COL32(18, 28, 65, 35),
    IM_COL32(0, 0, 0, 0),    IM_COL32(0, 0, 0, 0));
}

// ---- Badge helpers ------------------------------------------------------

static void DrawFallbackBadge(const std::string &shortname, float sz) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();

  unsigned int hash = 5381;
  for (char c : shortname) hash = ((hash << 5) + hash) ^ (unsigned char)c;
  float hue = (float)(hash % 360) / 360.0f;
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.48f, r, g, b);

  float rnd = sz * 0.22f;
  dl->AddRectFilled(p, ImVec2(p.x + sz, p.y + sz), IM_COL32((int)(r*255),(int)(g*255),(int)(b*255),220), rnd);

  std::string ini;
  for (unsigned int i = 0; i < shortname.size() && (int)ini.size() < 2; i++)
    if (isalpha((unsigned char)shortname[i])) ini += (char)toupper((unsigned char)shortname[i]);
  if (!ini.empty()) {
    ImVec2 tsz = ImGui::CalcTextSize(ini.c_str());
    dl->AddText(ImVec2(p.x + (sz - tsz.x) * 0.5f, p.y + (sz - tsz.y) * 0.5f),
                IM_COL32(255, 255, 255, 220), ini.c_str());
  }
  ImGui::Dummy(ImVec2(sz, sz));
}

static void DrawTeamBadge(const std::string &logoPath, const std::string &shortname, float sz) {
  GLuint tex = LoadBadgeTex(logoPath);
  if (tex != 0)
    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(sz, sz));
  else
    DrawFallbackBadge(shortname, sz);
}

static void DrawTeamLabel(const std::string &logoPath, const std::string &shortname, float badgeSz = 18.0f) {
  float lineH = ImGui::GetTextLineHeight();
  float offY  = (lineH - badgeSz) * 0.5f;
  if (offY > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);

  GLuint tex = LoadBadgeTex(logoPath);
  if (tex != 0)
    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(badgeSz, badgeSz));
  else
    DrawFallbackBadge(shortname, badgeSz);

  if (offY > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() - offY);
  ImGui::SameLine(0, 5);
  ImGui::TextUnformatted(shortname.c_str());
}

// ---- Status pill --------------------------------------------------------

static void DrawStatusPill(const std::string &status) {
  ImVec4 bg, col;
  if (status == "played") {
    bg = ImVec4(0.012f, 0.157f, 0.063f, 1.0f); col = kSuccess;
  } else if (status == "postponed") {
    bg = ImVec4(0.180f, 0.157f, 0.016f, 1.0f); col = kWarning;
  } else {
    bg = ImVec4(0.059f, 0.094f, 0.176f, 1.0f); col = kTextSec;
  }
  const float kPX = 7.0f, kPY = 2.5f;
  ImVec2 tsz  = ImGui::CalcTextSize(status.c_str());
  ImVec2 pill = ImVec2(tsz.x + kPX * 2, tsz.y + kPY * 2);
  ImVec2 p    = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + pill.x, p.y + pill.y), C32(bg), 5.0f);
  dl->AddText(ImVec2(p.x + kPX, p.y + kPY), C32(col), status.c_str());
  ImGui::Dummy(pill);
}

// ---- Ability bar --------------------------------------------------------

static void DrawAbilityBar(const std::string &abilityStr, float width = 62.0f) {
  float val = abilityStr.empty() ? 0.0f : (float)atof(abilityStr.c_str());
  if (val < 0.0f) val = 0.0f;
  if (val > 1.0f) val = 1.0f;

  const float kH  = 7.0f;
  float lineH = ImGui::GetTextLineHeight();
  ImVec2 p    = ImGui::GetCursorScreenPos();
  float offY  = (lineH - kH) * 0.5f;
  ImVec2 p0(p.x, p.y + offY);
  ImVec2 p1(p.x + width, p.y + offY + kH);
  ImDrawList *dl = ImGui::GetWindowDrawList();

  dl->AddRectFilled(p0, p1, IM_COL32(15, 22, 40, 255), 3.0f);
  if (val > 0.005f) {
    ImVec4 fc = (val >= 0.70f) ? kSuccess : (val >= 0.45f) ? kGold : kDanger;
    dl->AddRectFilled(p0, ImVec2(p0.x + width * val, p1.y), C32(fc), 3.0f);
  }
  ImGui::Dummy(ImVec2(width, lineH));
  ImGui::SameLine(0, 6);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d", (int)(val * 100.0f + 0.5f));
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted(buf);
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
}

// ---- Bar heights --------------------------------------------------------

static const float kHdrH    = 76.0f;
static const float kSubNavH = 44.0f;

// ---- DrawTopBar ---------------------------------------------------------

static bool s_playClicked = false;
static bool s_menuClicked = false;

static void DrawTopBar(float winW) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##topbar", ImVec2(0, kHdrH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  // Subtle bottom border on the panel bg
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();

  // ---- Left: badge + club identity ------------------------------------
  const float kBadgeSz = 52.0f;
  float badgeX = 18.0f;
  float badgeY = (kHdrH - kBadgeSz) * 0.5f;
  ImGui::SetCursorPos(ImVec2(badgeX, badgeY));
  DrawTeamBadge(g_CareerHub.club.logoPath, g_CareerHub.club.shortName, kBadgeSz);

  const std::string &cname  = g_CareerHub.club.name.empty() ? "Career" : g_CareerHub.club.name;
  const std::string &cleague = g_CareerHub.club.leagueName;

  // Measure text heights
  PushMgrFont(g_ManagerFontTitle);
  float nameH = ImGui::GetTextLineHeight();
  PopMgrFont(g_ManagerFontTitle);
  PushMgrFont(g_ManagerFontSmall);
  float lgH = ImGui::GetTextLineHeight();
  PopMgrFont(g_ManagerFontSmall);

  float grpH = nameH + (cleague.empty() ? 0.0f : lgH + 4.0f);
  float grpY = (kHdrH - grpH) * 0.5f;
  float textX = badgeX + kBadgeSz + 14.0f;

  ImGui::SetCursorPos(ImVec2(textX, grpY));
  PushMgrFont(g_ManagerFontTitle);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted(cname.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontTitle);

  if (!cleague.empty()) {
    ImGui::SetCursorPos(ImVec2(textX, grpY + nameH + 4.0f));
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted(cleague.c_str());
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
  }

  // ---- Center: primary navigation ------------------------------------
  static const char *kPrimTabs[] = { "Portal","Squad","Recruitment","Match Day","Club","Career" };
  const int kNumPrim = 6;
  const float kBtnW  = 90.0f, kBtnSp = 3.0f;
  float totalNavW = kNumPrim * kBtnW + (kNumPrim - 1) * kBtnSp;
  float navX = (winW - totalNavW) * 0.5f;
  float navY = (kHdrH - 34.0f) * 0.5f;

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 5.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(kBtnSp, 0.0f));
  for (int i = 0; i < kNumPrim; i++) {
    ImGui::SetCursorPos(ImVec2(navX + i * (kBtnW + kBtnSp), navY));
    if (GhostNavBtn(kPrimTabs[i], g_CareerHub.primaryTab == i, ImVec2(kBtnW, 34.0f)))
      g_CareerHub.primaryTab = i;
  }
  ImGui::PopStyleVar(2);

  // ---- Right: search placeholder + date + Continue -------------------
  const float kContW   = 112.0f;
  const float kDateW   = 72.0f;
  const float kSearchW = 148.0f;
  const float kElemH   = 30.0f;
  const float kRGap    = 8.0f;
  float elemY  = (kHdrH - kElemH) * 0.5f;
  float rightX = winW - 18.0f;

  // Continue button
  ImGui::SetCursorPos(ImVec2(rightX - kContW, elemY));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 5.0f));
  CTAButton("Continue", ImVec2(kContW, kElemH));
  ImGui::PopStyleVar();

  // Date placeholder
  float dateX = rightX - kContW - kRGap - kDateW;
  ImGui::SetCursorPos(ImVec2(dateX, elemY + (kElemH - lgH) * 0.5f));
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("May 2026");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);

  // Search placeholder (draw-list rect)
  float searchX = dateX - kRGap - kSearchW;
  ImGui::SetCursorPos(ImVec2(searchX, elemY));
  ImVec2 searchScr = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(searchScr, ImVec2(searchScr.x + kSearchW, searchScr.y + kElemH),
                    C32(ImVec4(0.055f,0.082f,0.153f,1.0f)), 5.0f);
  dl->AddRect(searchScr, ImVec2(searchScr.x + kSearchW, searchScr.y + kElemH),
              C32(kBorder), 5.0f, 0, 1.0f);
  ImGui::SetCursorPos(ImVec2(searchX + 10.0f, elemY + (kElemH - lgH) * 0.5f));
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("Search...");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);

  ImGui::EndChild();

  // 1px separator
  ImVec2 sp = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(
    sp, ImVec2(sp.x + winW, sp.y + 1.0f), C32(kBorder));
  ImGui::Dummy(ImVec2(0, 1.0f));
}

// ---- DrawPortalSubNav ---------------------------------------------------

static void DrawPortalSubNav(float winW) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 0.0f));
  ImGui::BeginChild("##subnav", ImVec2(0, kSubNavH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  float btnY = (kSubNavH - 30.0f) * 0.5f;
  static const char *kSubTabs[] = { "Overview","Manager","Club","Matches","Standings" };
  const int kNum = 5;
  const float kW = 102.0f, kSp = 4.0f;

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(kSp, 0.0f));
  ImGui::SetCursorPos(ImVec2(14.0f, btnY));
  for (int i = 0; i < kNum; i++) {
    if (i > 0) ImGui::SameLine(0, kSp);
    if (TabBtn(kSubTabs[i], g_CareerHub.activeTab == i, ImVec2(kW, 30.0f)))
      g_CareerHub.activeTab = i;
  }
  ImGui::PopStyleVar(2);

  const float kPW = 122.0f, kMW = 102.0f, kSepW = kSp;
  float rx = winW - kPW - kMW - kSepW - 18.0f;
  ImGui::SetCursorPos(ImVec2(rx, btnY));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
  if (CTAButton("Play Match", ImVec2(kPW, 30.0f))) s_playClicked = true;
  ImGui::SameLine(0, kSepW);
  if (SecBtn("Main Menu", ImVec2(kMW, 30.0f)))     s_menuClicked = true;
  ImGui::PopStyleVar();

  ImGui::EndChild();

  ImVec2 sp = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(
    sp, ImVec2(sp.x + winW, sp.y + 1.0f), C32(kBorder));
  ImGui::Dummy(ImVec2(0, 1.0f));
}

// ---- Overview cards -----------------------------------------------------

static void DrawMessagesCard(ImVec2 sz) {
  static const struct { const char *from, *subject, *time; } kMsgs[] = {
    { "Board",  "Pre-season objectives confirmed",   "Today"     },
    { "Media",  "Press conference this Friday",       "Today"     },
    { "Staff",  "Fitness report ready",               "Yesterday" },
    { "Board",  "Transfer budget allocated",          "2d ago"    },
    { "Fans",   "Season ticket renewals open",        "3d ago"    },
    { "Staff",  "Training schedule published",        "4d ago"    },
    { "Board",  "Scouting targets list sent",         "5d ago"    },
    { "Media",  "Pre-season preview requested",       "6d ago"    },
  };
  const int kN = 8;
  BeginModernCard("##msgs_ov", sz, "MESSAGES");

  float tblH = sz.y - 54.0f;
  if (tblH < 20.0f) tblH = 20.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 7.0f));
  if (ImGui::BeginTable("##msg_tbl", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, tblH))) {
    ImGui::TableSetupColumn("From",    ImGuiTableColumnFlags_WidthFixed,  58.0f);
    ImGui::TableSetupColumn("Subject", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("When",    ImGuiTableColumnFlags_WidthFixed,  58.0f);
    PushMgrFont(g_ManagerFontSmall);
    for (int i = 0; i < kN; i++) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kViolet);
      ImGui::TextUnformatted(kMsgs[i].from);
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(kMsgs[i].subject);
      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      ImGui::TextUnformatted(kMsgs[i].time);
      ImGui::PopStyleColor();
    }
    PopMgrFont(g_ManagerFontSmall);
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  EndModernCard();
}

static void DrawTopStoryCard(ImVec2 sz) {
  BeginModernCard("##story", sz);
  const std::string &cname = g_CareerHub.club.name.empty() ? "Your Club" : g_CareerHub.club.name;
  const std::string &mgr   = g_CareerHub.manager.name.empty() ? "The Manager" : g_CareerHub.manager.name;

  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::TextUnformatted("NEWS");
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 8);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("|");
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 8);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted(g_CareerHub.club.leagueName.empty() ? "Club News" : g_CareerHub.club.leagueName.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);

  ImGui::Spacing();
  PushMgrFont(g_ManagerFontBold);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  char headline[128];
  snprintf(headline, sizeof(headline), "%s ready for the new campaign", cname.c_str());
  ImGui::TextUnformatted(headline);
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontBold);

  ImGui::Spacing();
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  char body[256];
  snprintf(body, sizeof(body),
    "%s has named the squad for the upcoming season. The club has ambitious"
    " goals and %s believes this group can compete at the highest level.",
    mgr.c_str(), mgr.c_str());
  ImGui::TextWrapped("%s", body);
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);

  EndModernCard();
}

static void DrawNextFixtureCard(ImVec2 sz) {
  BeginModernCard("##nxt_fix", sz, "NEXT FIXTURE");
  const std::string &sn = g_CareerHub.club.shortName;

  const CareerHubState::Fixture *f = nullptr;
  for (const auto &x : g_CareerHub.fixtures)
    if (x.status == "scheduled" && (x.home == sn || x.away == sn)) { f = &x; break; }
  if (!f)
    for (const auto &x : g_CareerHub.fixtures)
      if (x.home == sn || x.away == sn) { f = &x; break; }

  if (!f) {
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No upcoming fixtures.");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    EndModernCard(); return;
  }

  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
  ImGui::Text("%s  \xe2\x80\xa2  Matchday %s", f->league.c_str(), f->matchday.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  ImGui::Spacing();

  float avail  = ImGui::GetContentRegionAvail().x;
  float vsW    = 42.0f;
  float teamW  = (avail - vsW) * 0.5f - 2.0f;
  if (teamW < 40.0f) teamW = 40.0f;
  const float kBadge = 44.0f;

  // Home box
  ImVec2 homeBoxSz(teamW, 88.0f);
  ImVec2 hBox0 = ImGui::GetCursorScreenPos();
  ImVec2 hBox1 = ImVec2(hBox0.x + homeBoxSz.x, hBox0.y + homeBoxSz.y);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(hBox0, hBox1, C32(kBgCardAlt), 8.0f);

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
  ImGui::BeginChild("##nf_h", homeBoxSz, false);
  ImGui::PopStyleVar(); ImGui::PopStyleColor();
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("HOME");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  ImGui::Spacing();
  {
    float bx = (teamW - 8.0f * 2.0f - kBadge) * 0.5f;
    if (bx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + bx);
  }
  GLuint hTex = LoadBadgeTex(f->homeLogo);
  if (hTex) ImGui::Image((ImTextureID)(intptr_t)hTex, ImVec2(kBadge, kBadge));
  else DrawFallbackBadge(f->home, kBadge);
  ImGui::Spacing();
  PushMgrFont(g_ManagerFontBold);
  ImGui::PushStyleColor(ImGuiCol_Text, (f->home==sn) ? kAccent : kTextPri);
  float nx = (teamW - 8*2 - ImGui::CalcTextSize(f->home.c_str()).x) * 0.5f;
  if (nx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + nx);
  ImGui::TextUnformatted(f->home.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontBold);
  ImGui::EndChild();

  // VS
  ImGui::SameLine(0, 2);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (88.0f - ImGui::GetTextLineHeight()) * 0.5f);
  PushMgrFont(g_ManagerFontBold);
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::TextUnformatted("vs");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontBold);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - (88.0f - ImGui::GetTextLineHeight()) * 0.5f);
  ImGui::SameLine(0, 2);

  // Away box
  ImVec2 aBox0 = ImGui::GetCursorScreenPos();
  ImVec2 aBox1 = ImVec2(aBox0.x + teamW, aBox0.y + 88.0f);
  dl->AddRectFilled(aBox0, aBox1, C32(kBgCardAlt), 8.0f);

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
  ImGui::BeginChild("##nf_a", ImVec2(teamW, 88.0f), false);
  ImGui::PopStyleVar(); ImGui::PopStyleColor();
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("AWAY");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  ImGui::Spacing();
  {
    float bx = (teamW - 8.0f * 2.0f - kBadge) * 0.5f;
    if (bx > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + bx);
  }
  GLuint aTex = LoadBadgeTex(f->awayLogo);
  if (aTex) ImGui::Image((ImTextureID)(intptr_t)aTex, ImVec2(kBadge, kBadge));
  else DrawFallbackBadge(f->away, kBadge);
  ImGui::Spacing();
  PushMgrFont(g_ManagerFontBold);
  ImGui::PushStyleColor(ImGuiCol_Text, (f->away==sn) ? kAccent : kTextPri);
  float ax = (teamW - 8*2 - ImGui::CalcTextSize(f->away.c_str()).x) * 0.5f;
  if (ax > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ax);
  ImGui::TextUnformatted(f->away.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontBold);
  ImGui::EndChild();

  EndModernCard();
}

static void DrawAgendaCard(ImVec2 sz) {
  BeginModernCard("##agenda", sz, "UPCOMING");
  const std::string &sn = g_CareerHub.club.shortName;
  int shown = 0;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
  if (ImGui::BeginTable("##ag_tbl", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, sz.y - 54.0f))) {
    ImGui::TableSetupColumn("MD",   ImGuiTableColumnFlags_WidthFixed,  28.0f);
    ImGui::TableSetupColumn("H/A",  ImGuiTableColumnFlags_WidthFixed,  22.0f);
    ImGui::TableSetupColumn("Opp",  ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Comp", ImGuiTableColumnFlags_WidthFixed,  68.0f);
    PushMgrFont(g_ManagerFontSmall);
    for (const auto &f : g_CareerHub.fixtures) {
      if (shown >= 5) break;
      if (f.home != sn && f.away != sn) continue;
      bool ih = (f.home == sn);
      const std::string &opp  = ih ? f.away : f.home;
      const std::string &logo = ih ? f.awayLogo : f.homeLogo;
      std::string comp = f.league.size() > 9 ? f.league.substr(0, 9) : f.league;

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      ImGui::Text("MD%s", f.matchday.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      ImGui::PushStyleColor(ImGuiCol_Text, ih ? kSuccess : kTextSec);
      ImGui::TextUnformatted(ih ? "H" : "A");
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(2);
      DrawTeamLabel(logo, opp, 14.0f);
      ImGui::TableSetColumnIndex(3);
      ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
      ImGui::TextUnformatted(comp.c_str());
      ImGui::PopStyleColor();
      shown++;
    }
    PopMgrFont(g_ManagerFontSmall);
    if (shown == 0) {
      ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted("—");
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  EndModernCard();
}

static void DrawFixtureScheduleCard(ImVec2 sz) {
  BeginModernCard("##sched", sz, "FIXTURE SCHEDULE");
  const std::string &sn = g_CareerHub.club.shortName;
  int shown = 0;

  float tblH = sz.y - 54.0f;
  if (tblH < 20.0f) tblH = 20.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(6.0f, 6.0f));
  if (ImGui::BeginTable("##sched_tbl", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, tblH))) {
    ImGui::TableSetupColumn("MD",     ImGuiTableColumnFlags_WidthFixed,  28.0f);
    ImGui::TableSetupColumn("H/A",    ImGuiTableColumnFlags_WidthFixed,  22.0f);
    ImGui::TableSetupColumn("Opp",    ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed,  72.0f);
    PushMgrFont(g_ManagerFontSmall);
    for (const auto &f : g_CareerHub.fixtures) {
      if (f.home != sn && f.away != sn) continue;
      if (shown++ >= 20) break;
      bool ih = (f.home == sn);
      const std::string &opp  = ih ? f.away : f.home;
      const std::string &logo = ih ? f.awayLogo : f.homeLogo;

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      ImGui::Text("MD%s", f.matchday.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      ImGui::PushStyleColor(ImGuiCol_Text, ih ? kSuccess : kTextSec);
      ImGui::TextUnformatted(ih ? "H" : "A");
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(2);
      DrawTeamLabel(logo, opp, 14.0f);
      ImGui::TableSetColumnIndex(3);
      if (!f.score.empty())
        DrawStatusPill("played");
      else
        DrawStatusPill(f.status);
    }
    PopMgrFont(g_ManagerFontSmall);
    if (shown == 0) {
      ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted("No fixtures.");
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  EndModernCard();
}

static void DrawLeagueSnapshotCard(ImVec2 sz) {
  BeginModernCard("##snap", sz, "LEAGUE SNAPSHOT");
  const std::string &sn       = g_CareerHub.club.shortName;
  const std::string &myLeague = g_CareerHub.club.leagueName;

  if (myLeague.empty()) {
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No standings data.");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    EndModernCard(); return;
  }

  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
  ImGui::TextUnformatted(myLeague.c_str());
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Spacing();

  float tblH = sz.y - 70.0f;
  if (tblH < 20.0f) tblH = 20.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5.0f, 5.0f));
  if (ImGui::BeginTable("##snap_tbl", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, tblH))) {
    ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("P",    ImGuiTableColumnFlags_WidthFixed, 20.0f);
    ImGui::TableSetupColumn("Pts",  ImGuiTableColumnFlags_WidthFixed, 34.0f);
    int pos = 1;
    for (const auto &s : g_CareerHub.standings) {
      if (s.league != myLeague) continue;
      bool mine = (s.team == sn);
      ImGui::TableNextRow(0, 26.0f);
      if (mine) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
          IM_COL32(80, 20, 160, 45));
      }
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      ImGui::Text("%d.", pos);
      ImGui::PopStyleColor();
      ImGui::SameLine(0, 3);
      DrawTeamLabel(s.teamLogo, s.team, 14.0f);
      ImGui::TableSetColumnIndex(1);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted(s.p.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, mine ? kGold : kTextPri);
      ImGui::TextUnformatted(s.pts.c_str());
      ImGui::PopStyleColor();
      pos++;
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();
}

// ---- DrawOverviewPage ---------------------------------------------------
// Layout: Left 30% (Messages) | Center 40% (Story + Fixture + Agenda) | Right 30% (Schedule + Snapshot)

static void DrawOverviewPage(float w, float h) {
  const float kPad = 16.0f, kGap = 16.0f;
  float usW = w - 2.0f * kPad - 2.0f * kGap;
  float leftW   = usW * 0.28f;
  float centerW = usW * 0.42f;
  float rightW  = usW - leftW - centerW;
  float colH = h - 16.0f;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));

  // Left: Messages
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_l", ImVec2(leftW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  DrawMessagesCard(ImVec2(leftW, colH));
  ImGui::EndChild();

  ImGui::SameLine(0, kGap);

  // Center: Top Story + Next Fixture + Agenda
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_c", ImVec2(centerW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  float storyH  = colH * 0.24f;
  float fixtH   = colH * 0.43f;
  float agendaH = colH - storyH - fixtH - kGap * 2.0f;
  if (agendaH < 60.0f) agendaH = 60.0f;
  DrawTopStoryCard(ImVec2(centerW, storyH));
  ImGui::Dummy(ImVec2(0, kGap));
  DrawNextFixtureCard(ImVec2(centerW, fixtH));
  ImGui::Dummy(ImVec2(0, kGap));
  DrawAgendaCard(ImVec2(centerW, agendaH));
  ImGui::EndChild();

  ImGui::SameLine(0, kGap);

  // Right: Fixture Schedule + League Snapshot
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_r", ImVec2(rightW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  float schedH = colH * 0.56f;
  float snapH  = colH - schedH - kGap;
  DrawFixtureScheduleCard(ImVec2(rightW, schedH));
  ImGui::Dummy(ImVec2(0, kGap));
  DrawLeagueSnapshotCard(ImVec2(rightW, snapH));
  ImGui::EndChild();
}

// ---- DrawManagerPage ----------------------------------------------------

static void DrawManagerPage(float w, float h) {
  const float kPad = 16.0f, kGap = 14.0f;
  float leftW  = w * 0.34f - kPad;
  float rightW = w - leftW - kGap - kPad * 2.0f;
  float colH   = h - 14.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##mgr_l", ImVec2(leftW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();

  float profileH = colH * 0.62f;
  BeginModernCard("##mgr_card", ImVec2(leftW, profileH), "MANAGER PROFILE");
  const auto &m = g_CareerHub.manager;

  // Initials circle avatar
  std::string ini;
  if (!m.name.empty()) {
    ini += (char)toupper((unsigned char)m.name[0]);
    size_t sp = m.name.find(' ');
    if (sp != std::string::npos && sp + 1 < m.name.size())
      ini += (char)toupper((unsigned char)m.name[sp + 1]);
  }
  {
    float cr = 32.0f;
    ImVec2 cpos = ImGui::GetCursorScreenPos();
    ImVec2 cen  = ImVec2(cpos.x + cr, cpos.y + cr);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(cen, cr, IM_COL32(88, 26, 178, 200));
    if (!ini.empty()) {
      PushMgrFont(g_ManagerFontTitle);
      ImVec2 tsz = ImGui::CalcTextSize(ini.c_str());
      dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                  ImVec2(cen.x - tsz.x * 0.5f, cen.y - tsz.y * 0.5f),
                  IM_COL32(255, 255, 255, 230), ini.c_str());
      PopMgrFont(g_ManagerFontTitle);
    }
    ImGui::Dummy(ImVec2(leftW - 28.0f, cr * 2 + 12));
  }

  auto prow = [](const char *lbl, const std::string &v) {
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::Text("%-12s", lbl);
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
    ImGui::TextUnformatted(v.empty() ? "\xe2\x80\x94" : v.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  };
  prow("Name",        m.name);
  prow("Age",         m.age);
  prow("Nationality", m.nationality);
  prow("Gender",      m.gender);
  prow("Club",        m.clubName);
  prow("League",      g_CareerHub.club.leagueName);
  EndModernCard();

  ImGui::EndChild();
  ImGui::SameLine(0, kGap);

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##mgr_r", ImVec2(rightW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();

  float ch = (colH - kGap * 2.0f) / 3.0f;
  BeginModernCard("##mgr_c1", ImVec2(rightW, ch), "CAREER SUMMARY");
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("No previous clubs.");
  ImGui::TextUnformatted("First season as a manager.");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();
  ImGui::Dummy(ImVec2(0, kGap));

  BeginModernCard("##mgr_c2", ImVec2(rightW, ch), "CURRENT JOB");
  auto jr = [](const char *l, const char *v) {
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::Text("%-10s", l);
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
    ImGui::TextUnformatted(v);
    ImGui::PopStyleColor();
    ImGui::Spacing();
  };
  jr("Club",   g_CareerHub.manager.clubName.empty() ? "\xe2\x80\x94" : g_CareerHub.manager.clubName.c_str());
  jr("League", g_CareerHub.club.leagueName.empty()  ? "\xe2\x80\x94" : g_CareerHub.club.leagueName.c_str());
  jr("Season", "2026/27");
  EndModernCard();
  ImGui::Dummy(ImVec2(0, kGap));

  BeginModernCard("##mgr_c3", ImVec2(rightW, ch), "OBJECTIVES");
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("Objectives will be set by the board.");
  ImGui::TextUnformatted("Check back at the start of the season.");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();

  ImGui::EndChild();
}

// ---- DrawSquadPage (was DrawClubPage) -----------------------------------

static int s_squadFilter = 0; // 0=All, 1=GK, 2=DEF, 3=MID, 4=ATT — visual only

static void DrawSquadPage(float w, float h) {
  const float kPad = 16.0f, kGap = 12.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 14.0f;

  // Club header card
  float hdrH = 72.0f;
  BeginModernCard("##squad_hdr", ImVec2(usW, hdrH));
  const auto &cl = g_CareerHub.club;
  const float kBadgeSz = 48.0f;
  float badgeY = (hdrH - 24.0f - kBadgeSz) * 0.5f; // 24=padding top in card
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (badgeY > 0 ? badgeY * 0.3f : 0));
  DrawTeamBadge(cl.logoPath, cl.shortName, kBadgeSz);
  ImGui::SameLine(0, 16);
  ImGui::BeginGroup();
  PushMgrFont(g_ManagerFontTitle);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted(cl.name.empty() ? "\xe2\x80\x94" : cl.name.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontTitle);
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("Senior Squad");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  ImGui::EndGroup();
  EndModernCard();

  ImGui::Dummy(ImVec2(0, kGap));

  // Toolbar: filter chips
  float toolbarH = 36.0f;
  BeginModernCard("##squad_tools", ImVec2(usW, toolbarH));
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f);

  // View mode buttons (visual)
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(4.0f, 0.0f));
  SecBtn("In Possession");
  ImGui::SameLine(0, 4);
  SecBtn("Out of Possession");

  // Position filter chips
  ImGui::SameLine(0, 14);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("|");
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 14);

  static const char *kPos[] = { "All","GK","DEF","MID","ATT" };
  for (int i = 0; i < 5; i++) {
    if (i > 0) ImGui::SameLine(0, 4);
    bool sel = (s_squadFilter == i);
    if (sel) {
      ImGui::PushStyleColor(ImGuiCol_Button,        kViolet);
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kVioletH);
      ImGui::PushStyleColor(ImGuiCol_Text,          kTextPri);
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.08f,0.12f,0.22f,0.7f));
      ImGui::PushStyleColor(ImGuiCol_Text,          kTextSec);
    }
    if (ImGui::Button(kPos[i])) s_squadFilter = i;
    ImGui::PopStyleColor(3);
  }
  ImGui::PopStyleVar(2);
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();

  ImGui::Dummy(ImVec2(0, kGap));

  // Squad table
  float squadH = usH - hdrH - toolbarH - kGap * 2.0f - 8.0f;
  if (squadH < 60.0f) squadH = 60.0f;
  BeginModernCard("##squad_card", ImVec2(usW, squadH), "SQUAD");
  float tblH = squadH - 56.0f;
  if (tblH < 30.0f) tblH = 30.0f;

  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 6.0f));
  if (ImGui::BeginTable("##squad_tbl", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, tblH))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Role",    ImGuiTableColumnFlags_WidthFixed, 155.0f);
    ImGui::TableSetupColumn("Age",     ImGuiTableColumnFlags_WidthFixed,  40.0f);
    ImGui::TableSetupColumn("Ability", ImGuiTableColumnFlags_WidthFixed,  96.0f);
    PushMgrFont(g_ManagerFontSmall);
    ImGui::TableHeadersRow();
    PopMgrFont(g_ManagerFontSmall);

    for (const auto &p : g_CareerHub.players) {
      ImGui::TableNextRow(0, 30.0f);
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%s %s", p.firstName.c_str(), p.lastName.c_str());
      ImGui::TableSetColumnIndex(1);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted(p.role.c_str());
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);
      ImGui::TableSetColumnIndex(2);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted(p.age.c_str());
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);
      ImGui::TableSetColumnIndex(3);
      DrawAbilityBar(p.ability, 62.0f);
    }
    if (g_CareerHub.players.empty()) {
      ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted("No squad data.");
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  EndModernCard();
}

// ---- DrawMatchesPage ----------------------------------------------------

static void DrawMatchesPage(float w, float h) {
  const float kPad = 16.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 20.0f;
  const std::string &sn = g_CareerHub.club.shortName;

  // Gather league order
  std::vector<std::string> leagues;
  for (const auto &f : g_CareerHub.fixtures) {
    bool found = false;
    for (const auto &l : leagues) if (l == f.league) { found = true; break; }
    if (!found) leagues.push_back(f.league);
  }

  // Outer scrollable card
  BeginModernCard("##matches_outer", ImVec2(usW, usH), "FIXTURES");
  ImGui::BeginChild("##m_scroll", ImVec2(0, usH - 58.0f), false);

  if (g_CareerHub.fixtures.empty()) {
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No fixtures generated yet.");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
  }

  for (unsigned int li = 0; li < leagues.size(); li++) {
    const std::string &lg = leagues.at(li);
    if (li > 0) ImGui::Dummy(ImVec2(0, 10.0f));

    // Count rows for this league to size the inner card
    int count = 0;
    for (const auto &f : g_CareerHub.fixtures) if (f.league == lg) count++;
    float rowH  = 30.0f;
    float hdrH2 = 30.0f; // league title row
    float tblH  = (float)count * rowH + 32.0f; // 32 = table header
    float cardH = hdrH2 + tblH + 28.0f; // 28 = card padding top+bottom

    // Draw league card background on the scroll child DrawList
    ImVec2 cp0 = ImGui::GetCursorScreenPos();
    ImVec2 cp1 = ImVec2(cp0.x + usW - 28.0f, cp0.y + cardH);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cp0, cp1, C32(kBgCardAlt), 8.0f);
    dl->AddRect(cp0, cp1, C32(kBorder), 8.0f, 0, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    std::string cid = "##mlg_" + lg;
    ImGui::BeginChild(cid.c_str(), ImVec2(usW - 28.0f, cardH), false);
    ImGui::PopStyleVar(); ImGui::PopStyleColor();

    // League header
    PushMgrFont(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
    ImGui::TextUnformatted(lg.c_str());
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
    ImGui::Separator();
    ImGui::PopStyleColor();

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(7.0f, 5.0f));
    std::string tblId = "##fix_" + lg;
    if (ImGui::BeginTable(tblId.c_str(), 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("MD",     ImGuiTableColumnFlags_WidthFixed,  32.0f);
      ImGui::TableSetupColumn("Rd",     ImGuiTableColumnFlags_WidthFixed,  28.0f);
      ImGui::TableSetupColumn("Home",   ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Away",   ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,  90.0f);
      ImGui::TableSetupColumn("Score",  ImGuiTableColumnFlags_WidthFixed,  56.0f);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::TableHeadersRow();

      for (const auto &f : g_CareerHub.fixtures) {
        if (f.league != lg) continue;
        bool myGame = (f.home == sn || f.away == sn);
        ImGui::TableNextRow(0, rowH);
        if (myGame) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            IM_COL32(75, 18, 140, 45));
        }
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::TextUnformatted(f.matchday.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::TextUnformatted(f.round.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        DrawTeamLabel(f.homeLogo, f.home, 20.0f);
        ImGui::TableSetColumnIndex(3);
        DrawTeamLabel(f.awayLogo, f.away, 20.0f);
        ImGui::TableSetColumnIndex(4);
        DrawStatusPill(f.status);
        ImGui::TableSetColumnIndex(5);
        if (!f.score.empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, kSuccess);
          ImGui::TextUnformatted(f.score.c_str());
          ImGui::PopStyleColor();
        } else {
          ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
          ImGui::TextUnformatted("\xe2\x80\x94");
          ImGui::PopStyleColor();
        }
      }
      PopMgrFont(g_ManagerFontSmall);
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
  }

  ImGui::EndChild();
  EndModernCard();
}

// ---- DrawStandingsPage --------------------------------------------------

static void DrawStandingsPage(float w, float h) {
  const float kPad = 16.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 20.0f;
  const std::string &sn = g_CareerHub.club.shortName;

  std::vector<std::string> leagues;
  for (const auto &s : g_CareerHub.standings) {
    bool found = false;
    for (const auto &l : leagues) if (l == s.league) { found = true; break; }
    if (!found) leagues.push_back(s.league);
  }

  BeginModernCard("##std_outer", ImVec2(usW, usH), "LEAGUE STANDINGS");
  ImGui::BeginChild("##s_scroll", ImVec2(0, usH - 58.0f), false);

  if (g_CareerHub.standings.empty()) {
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No standings generated yet.");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
  }

  for (unsigned int li = 0; li < leagues.size(); li++) {
    const std::string &lg = leagues.at(li);
    if (li > 0) ImGui::Dummy(ImVec2(0, 12.0f));

    int count = 0;
    for (const auto &s : g_CareerHub.standings) if (s.league == lg) count++;
    float rowH  = 30.0f;
    float tblH  = (float)count * rowH + 34.0f;
    float cardH = 34.0f + tblH + 28.0f;

    ImVec2 cp0 = ImGui::GetCursorScreenPos();
    ImVec2 cp1 = ImVec2(cp0.x + usW - 28.0f, cp0.y + cardH);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cp0, cp1, C32(kBgCardAlt), 8.0f);
    dl->AddRect(cp0, cp1, C32(kBorder), 8.0f, 0, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    std::string cid = "##slg_" + lg;
    ImGui::BeginChild(cid.c_str(), ImVec2(usW - 28.0f, cardH), false);
    ImGui::PopStyleVar(); ImGui::PopStyleColor();

    PushMgrFont(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
    ImGui::TextUnformatted(lg.c_str());
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
    ImGui::Separator();
    ImGui::PopStyleColor();

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(7.0f, 5.0f));
    std::string tblId = "##std_" + lg;
    if (ImGui::BeginTable(tblId.c_str(), 10,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed,  24.0f);
      ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("P",    ImGuiTableColumnFlags_WidthFixed,  26.0f);
      ImGui::TableSetupColumn("W",    ImGuiTableColumnFlags_WidthFixed,  26.0f);
      ImGui::TableSetupColumn("D",    ImGuiTableColumnFlags_WidthFixed,  26.0f);
      ImGui::TableSetupColumn("L",    ImGuiTableColumnFlags_WidthFixed,  26.0f);
      ImGui::TableSetupColumn("GF",   ImGuiTableColumnFlags_WidthFixed,  30.0f);
      ImGui::TableSetupColumn("GA",   ImGuiTableColumnFlags_WidthFixed,  30.0f);
      ImGui::TableSetupColumn("GD",   ImGuiTableColumnFlags_WidthFixed,  34.0f);
      ImGui::TableSetupColumn("Pts",  ImGuiTableColumnFlags_WidthFixed,  38.0f);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::TableHeadersRow();

      int pos = 1;
      for (const auto &s : g_CareerHub.standings) {
        if (s.league != lg) continue;
        bool mine = (s.team == sn);
        ImGui::TableNextRow(0, rowH);
        if (mine) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            IM_COL32(75, 18, 140, 45));
        }

        ImGui::TableSetColumnIndex(0);
        // Position in circle
        {
          ImVec2 ps = ImGui::GetCursorScreenPos();
          float r = 9.0f;
          float cy = ps.y + (rowH - 24.0f) * 0.5f + r;
          ImDrawList *dl2 = ImGui::GetWindowDrawList();
          dl2->AddCircleFilled(ImVec2(ps.x + r, cy), r,
            mine ? C32(kViolet) : IM_COL32(20, 32, 58, 255));
          char nbuf[4]; snprintf(nbuf, sizeof(nbuf), "%d", pos);
          ImVec2 tsz = ImGui::CalcTextSize(nbuf);
          dl2->AddText(ImVec2(ps.x + r - tsz.x * 0.5f, cy - tsz.y * 0.5f),
                       mine ? IM_COL32(255,255,255,230) : C32(kTextDim), nbuf);
          ImGui::Dummy(ImVec2(r * 2, rowH - 12.0f));
        }
        ImGui::TableSetColumnIndex(1);
        DrawTeamLabel(s.teamLogo, s.team, 18.0f);
        auto stat = [](const std::string &v) {
          ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
          ImGui::TextUnformatted(v.c_str());
          ImGui::PopStyleColor();
        };
        ImGui::TableSetColumnIndex(2); stat(s.p);
        ImGui::TableSetColumnIndex(3); stat(s.w);
        ImGui::TableSetColumnIndex(4); stat(s.d);
        ImGui::TableSetColumnIndex(5); stat(s.l);
        ImGui::TableSetColumnIndex(6); stat(s.gf);
        ImGui::TableSetColumnIndex(7); stat(s.ga);
        ImGui::TableSetColumnIndex(8); stat(s.gd);
        ImGui::TableSetColumnIndex(9);
        PushMgrFont(g_ManagerFontBold);
        ImGui::PushStyleColor(ImGuiCol_Text, mine ? kGold : kTextPri);
        ImGui::TextUnformatted(s.pts.c_str());
        ImGui::PopStyleColor();
        PopMgrFont(g_ManagerFontBold);
        pos++;
      }
      PopMgrFont(g_ManagerFontSmall);
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
  }

  ImGui::EndChild();
  EndModernCard();
}

// ---- DrawComingSoonPage -------------------------------------------------

static void DrawComingSoonPage(float w, float h, const char *section) {
  const float kPad = 16.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float cw = w - kPad * 2.0f;
  float ch = h * 0.36f;
  if (ch < 80.0f) ch = 80.0f;

  BeginModernCard("##soon_card", ImVec2(cw, ch));
  ImGui::Spacing(); ImGui::Spacing();
  PushMgrFont(g_ManagerFontTitle);
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::TextUnformatted(section);
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontTitle);
  ImGui::Spacing();
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("This section is coming soon.");
  ImGui::Spacing();
  ImGui::TextUnformatted("Current focus is on the Portal hub and career flow.");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();
}

// ---- Main entry point ---------------------------------------------------

void RenderImGuiCareerHub() {
  if (!g_CareerHub.active) return;

  ApplyManagerTheme();

  ImGuiIO &io = ImGui::GetIO();
  float winW = io.DisplaySize.x;
  float winH = io.DisplaySize.y;

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##mgr_root", nullptr,
               ImGuiWindowFlags_NoTitleBar          |
               ImGuiWindowFlags_NoResize            |
               ImGuiWindowFlags_NoMove              |
               ImGuiWindowFlags_NoCollapse          |
               ImGuiWindowFlags_NoBringToFrontOnFocus |
               ImGuiWindowFlags_NoScrollbar         |
               ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();

  s_playClicked = false;
  s_menuClicked = false;

  DrawAppBackground(winW, winH);
  DrawTopBar(winW);
  DrawPortalSubNav(winW);

  float contentH = winH - kHdrH - 1.0f - kSubNavH - 1.0f;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("##content", ImVec2(winW, contentH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  if (g_CareerHub.primaryTab == 0) {
    switch (g_CareerHub.activeTab) {
      case 0: DrawOverviewPage(winW, contentH);  break;
      case 1: DrawManagerPage(winW, contentH);   break;
      case 2: DrawSquadPage(winW, contentH);     break;
      case 3: DrawMatchesPage(winW, contentH);   break;
      case 4: DrawStandingsPage(winW, contentH); break;
      default: break;
    }
  } else {
    static const char *kPrimNames[] = {
      "Portal", "Squad", "Recruitment", "Match Day", "Club", "Career"
    };
    const char *name = (g_CareerHub.primaryTab < 6) ? kPrimNames[g_CareerHub.primaryTab] : "Unknown";
    DrawComingSoonPage(winW, contentH, name);
  }

  ImGui::EndChild();
  ImGui::End();

  // Deferred action flags — consumed by operator()() after Handle() returns.
  if (s_playClicked && g_CareerHub.pendingAction == 0) {
    g_CareerHub.pendingAction = 1;
    printf("[IMGUI MANAGER] Play Match requested\n");
  }
  if (s_menuClicked && g_CareerHub.pendingAction == 0) {
    g_CareerHub.pendingAction = 2;
    printf("[IMGUI MANAGER] Main Menu requested\n");
  }
}
