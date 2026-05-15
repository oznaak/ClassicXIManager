#include "imgui_career.hpp"

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

#include "main.hpp"
#include "utils/database.hpp"
#include "base/utils.hpp"

CareerHubState g_CareerHub;

// ---- DB cell helper -----------------------------------------------------

static std::string DBCell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size() || col >= r->data.at(row).size()) return "";
  return r->data.at(row).at(col);
}

// ---- Badge texture cache ------------------------------------------------
// Loaded and used exclusively from the GL render thread.

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
    printf("[IMGUI MANAGER] failed badge load for team %s, using text fallback\n", fullPath.c_str());
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
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h,
               0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
  glBindTexture(GL_TEXTURE_2D, 0);
  SDL_FreeSurface(rgba);

  printf("[IMGUI MANAGER] loaded badge for team %s\n", logoRelPath.c_str());
  s_BadgeCache[fullPath] = texID;
  return texID;
}

static void ClearBadgeCache() {
  for (auto &kv : s_BadgeCache) {
    if (kv.second != 0) glDeleteTextures(1, &kv.second);
  }
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

  // Manager + club name
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

  // Club info including logo + league name
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

  // Squad
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

  // Fixtures — include home/away logo_url
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
      std::string as = DBCell(r, i, 9);
      f.score = (f.status != "scheduled" && !hs.empty()) ? hs + " - " + as : "-";
      fixtures.push_back(f);
    }
    delete r;
  }

  // Standings — include team logo_url
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

static const ImVec4 kBgApp      = ImVec4(0.020f, 0.031f, 0.059f, 1.0f); // #050810
static const ImVec4 kBgPanel    = ImVec4(0.031f, 0.047f, 0.086f, 1.0f); // #080C16
static const ImVec4 kBgCard     = ImVec4(0.047f, 0.071f, 0.129f, 1.0f); // #0C1221
static const ImVec4 kBgCardAlt  = ImVec4(0.035f, 0.055f, 0.102f, 1.0f); // #090E1A
static const ImVec4 kBgRow      = ImVec4(0.035f, 0.055f, 0.102f, 0.5f);
static const ImVec4 kBorder     = ImVec4(0.082f, 0.122f, 0.220f, 0.70f); // #151F38
static const ImVec4 kAccent     = ImVec4(0.741f, 0.102f, 0.788f, 1.0f); // #BD1AC9 magenta
static const ImVec4 kAccentH    = ImVec4(0.863f, 0.318f, 0.918f, 1.0f);
static const ImVec4 kAccentA    = ImVec4(0.576f, 0.047f, 0.620f, 1.0f);
static const ImVec4 kViolet     = ImVec4(0.482f, 0.231f, 0.929f, 1.0f); // #7C3AED violet
static const ImVec4 kVioletH    = ImVec4(0.580f, 0.330f, 0.970f, 1.0f);
static const ImVec4 kVioletA    = ImVec4(0.380f, 0.160f, 0.800f, 1.0f);
static const ImVec4 kTextPri    = ImVec4(0.937f, 0.949f, 0.965f, 1.0f); // #EEF2F6
static const ImVec4 kTextSec    = ImVec4(0.502f, 0.573f, 0.675f, 1.0f); // #8092AC
static const ImVec4 kTextDim    = ImVec4(0.239f, 0.290f, 0.388f, 1.0f); // #3D4A63
static const ImVec4 kSuccess    = ImVec4(0.133f, 0.773f, 0.369f, 1.0f);
static const ImVec4 kWarning    = ImVec4(0.973f, 0.620f, 0.043f, 1.0f);
static const ImVec4 kDanger     = ImVec4(0.937f, 0.267f, 0.267f, 1.0f);
static const ImVec4 kGold       = ImVec4(0.992f, 0.820f, 0.110f, 1.0f);
static const ImVec4 kBlue       = ImVec4(0.361f, 0.682f, 0.941f, 1.0f);

// ---- Theme --------------------------------------------------------------

static void ApplyManagerTheme() {
  ImGuiStyle &st = ImGui::GetStyle();
  st.WindowRounding    = 0.0f;
  st.ChildRounding     = 8.0f;
  st.FrameRounding     = 5.0f;
  st.ScrollbarRounding = 4.0f;
  st.GrabRounding      = 4.0f;
  st.PopupRounding     = 6.0f;
  st.WindowBorderSize  = 0.0f;
  st.ChildBorderSize   = 1.0f;
  st.FrameBorderSize   = 0.0f;
  st.WindowPadding     = ImVec2(0.0f, 0.0f);
  st.FramePadding      = ImVec2(12.0f, 6.0f);
  st.ItemSpacing       = ImVec2(10.0f, 6.0f);
  st.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
  st.CellPadding       = ImVec2(8.0f, 5.0f);
  st.ScrollbarSize     = 7.0f;
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
  c[ImGuiCol_ScrollbarBg]           = kBgApp;
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
  c[ImGuiCol_TableHeaderBg]         = kBgPanel;
  c[ImGuiCol_TableBorderStrong]     = kBorder;
  c[ImGuiCol_TableBorderLight]      = ImVec4(0.055f, 0.082f, 0.153f, 1.0f);
  c[ImGuiCol_TableRowBg]            = ImVec4(0,0,0,0);
  c[ImGuiCol_TableRowBgAlt]         = ImVec4(0.035f, 0.055f, 0.102f, 0.45f);
  c[ImGuiCol_Text]                  = kTextPri;
  c[ImGuiCol_TextDisabled]          = kTextDim;
  c[ImGuiCol_NavHighlight]          = kAccent;
  c[ImGuiCol_NavWindowingHighlight] = kViolet;
  c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0,0,0,0.5f);
  c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0,0,0,0.5f);
}

// ---- Widget helpers -----------------------------------------------------

// Card: styled bordered child window. Always pair with EndCard().
static void BeginCard(const char *id, ImVec2 size, const char *title = nullptr) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgCard);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::BeginChild(id, size, true);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  if (title) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
}
static void EndCard() { ImGui::EndChild(); }

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
static bool TabBtn(const char *lbl, bool active, ImVec2 sz = ImVec2(100,30)) {
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
  bool r = ImGui::Button(lbl, sz);
  ImGui::PopStyleColor(4);
  return r;
}
static bool GhostBtn(const char *lbl, bool active, ImVec2 sz = ImVec2(88,0)) {
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.082f,0.122f,0.220f,0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.110f,0.161f,0.282f,0.7f));
    ImGui::PushStyleColor(ImGuiCol_Text,          kAccent);
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.082f,0.122f,0.220f,0.5f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.110f,0.161f,0.282f,0.7f));
    ImGui::PushStyleColor(ImGuiCol_Text,          kTextSec);
  }
  bool r = ImGui::Button(lbl, sz);
  ImGui::PopStyleColor(4);
  return r;
}

// Draw a team label: badge (if available) + shortname inline.
static void DrawTeamLabel(const std::string &logoPath, const std::string &shortname, float badgeSz = 18.0f) {
  GLuint tex = LoadBadgeTex(logoPath);
  if (tex != 0) {
    float lineH = ImGui::GetTextLineHeight();
    float offY  = (lineH - badgeSz) * 0.5f;
    if (offY < 0) offY = 0;
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);
    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(badgeSz, badgeSz));
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - offY);
    ImGui::SameLine(0, 5);
  }
  ImGui::TextUnformatted(shortname.c_str());
}

// Draw a status pill using DrawList (rounded rect + text).
static void DrawStatusPill(const std::string &status) {
  ImVec4 bg, col;
  if (status == "played") {
    bg = ImVec4(0.012f, 0.157f, 0.063f, 1.0f);
    col = kSuccess;
  } else if (status == "postponed") {
    bg = ImVec4(0.180f, 0.157f, 0.016f, 1.0f);
    col = kWarning;
  } else {
    bg = ImVec4(0.059f, 0.094f, 0.176f, 1.0f);
    col = kTextSec;
  }
  const float kPX = 7.0f, kPY = 2.5f;
  ImVec2 tsz  = ImGui::CalcTextSize(status.c_str());
  ImVec2 pill = ImVec2(tsz.x + kPX * 2, tsz.y + kPY * 2);
  ImVec2 p    = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + pill.x, p.y + pill.y),
                    ImGui::ColorConvertFloat4ToU32(bg), 5.0f);
  dl->AddText(ImVec2(p.x + kPX, p.y + kPY),
              ImGui::ColorConvertFloat4ToU32(col), status.c_str());
  ImGui::Dummy(pill);
}

// ---- Bar heights --------------------------------------------------------
static const float kHdrH    = 54.0f;
static const float kSubNavH = 44.0f;

// ---- DrawTopBar ---------------------------------------------------------

static void DrawTopBar(float winW) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgPanel);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 0.0f));
  ImGui::BeginChild("##topbar", ImVec2(0, kHdrH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  float midY    = (kHdrH - ImGui::GetTextLineHeight()) * 0.5f;
  float midBtnY = (kHdrH - 28.0f) * 0.5f;

  // ---- Left: badge + club name ----------------------------------------
  const float kBadgeSz = 34.0f;
  float badgeY = (kHdrH - kBadgeSz) * 0.5f;
  ImGui::SetCursorPos(ImVec2(14.0f, badgeY));
  GLuint clubTex = LoadBadgeTex(g_CareerHub.club.logoPath);
  if (clubTex != 0) {
    ImGui::Image((ImTextureID)(intptr_t)clubTex, ImVec2(kBadgeSz, kBadgeSz));
    ImGui::SameLine(0, 10);
  }
  ImGui::SetCursorPosY(midY);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  const std::string &cname = g_CareerHub.club.name.empty() ? "Career" : g_CareerHub.club.name;
  ImGui::TextUnformatted(cname.c_str());
  ImGui::PopStyleColor();
  if (!g_CareerHub.club.leagueName.empty()) {
    ImGui::SameLine(0, 8);
    ImGui::SetCursorPosY(midY);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted("|");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 8);
    ImGui::SetCursorPosY(midY);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted(g_CareerHub.club.leagueName.c_str());
    ImGui::PopStyleColor();
  }

  // ---- Center: primary navigation tabs --------------------------------
  static const char *kPrimTabs[] = { "Portal","Squad","Recruitment","Match Day","Club","Career" };
  const int kNumPrim = 6;
  const float kBtnW  = 86.0f;
  const float kBtnSp = 2.0f;
  float totalW  = kNumPrim * kBtnW + (kNumPrim - 1) * kBtnSp;
  float navX    = (winW - totalW) * 0.5f;

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 5.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(kBtnSp, 0.0f));
  ImGui::SetCursorPos(ImVec2(navX, midBtnY));
  for (int i = 0; i < kNumPrim; i++) {
    if (i > 0) ImGui::SameLine(0, kBtnSp);
    if (GhostBtn(kPrimTabs[i], g_CareerHub.primaryTab == i, ImVec2(kBtnW, 28.0f)))
      g_CareerHub.primaryTab = i;
  }
  ImGui::PopStyleVar(2);

  // ---- Right: Continue CTA -------------------------------------------
  const float kContW = 110.0f;
  ImGui::SetCursorPos(ImVec2(winW - kContW - 14.0f, midBtnY));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 5.0f));
  CTAButton("Continue", ImVec2(kContW, 28.0f));
  ImGui::PopStyleVar();

  ImGui::EndChild();

  // 1px separator
  ImVec2 sp = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(
    sp, ImVec2(sp.x + winW, sp.y + 1.0f),
    ImGui::ColorConvertFloat4ToU32(kBorder));
  ImGui::Dummy(ImVec2(0, 1.0f));
}

// ---- DrawPortalSubNav ---------------------------------------------------

static bool s_playClicked = false;
static bool s_menuClicked = false;

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
  const float kW = 100.0f, kSp = 4.0f;

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(kSp, 0.0f));
  ImGui::SetCursorPos(ImVec2(14.0f, btnY));
  for (int i = 0; i < kNum; i++) {
    if (i > 0) ImGui::SameLine(0, kSp);
    if (TabBtn(kSubTabs[i], g_CareerHub.activeTab == i, ImVec2(kW, 30.0f)))
      g_CareerHub.activeTab = i;
  }
  ImGui::PopStyleVar(2);

  const float kPW = 120.0f, kMW = 100.0f;
  float rx = winW - kPW - kMW - kSp - 14.0f;
  ImGui::SetCursorPos(ImVec2(rx, btnY));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
  if (CTAButton("Play Match", ImVec2(kPW, 30.0f))) s_playClicked = true;
  ImGui::SameLine(0, kSp);
  if (SecBtn("Main Menu", ImVec2(kMW, 30.0f)))     s_menuClicked = true;
  ImGui::PopStyleVar();

  ImGui::EndChild();

  ImVec2 sp = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(
    sp, ImVec2(sp.x + winW, sp.y + 1.0f),
    ImGui::ColorConvertFloat4ToU32(kBorder));
  ImGui::Dummy(ImVec2(0, 1.0f));
}

// ---- Overview cards -----------------------------------------------------

static void DrawManagerSummaryCard(ImVec2 sz) {
  BeginCard("##mgr_sum", sz, "MANAGER");
  const auto &m = g_CareerHub.manager;

  // Initials circle
  std::string ini;
  if (!m.name.empty()) {
    ini += (char)toupper(m.name[0]);
    size_t sp = m.name.find(' ');
    if (sp != std::string::npos && sp + 1 < m.name.size())
      ini += (char)toupper(m.name[sp + 1]);
  }
  if (!ini.empty()) {
    float r    = 26.0f;
    ImVec2 cpos = ImGui::GetCursorScreenPos();
    ImVec2 cen  = ImVec2(cpos.x + r, cpos.y + r);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddCircleFilled(cen, r, ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f,0.10f,0.70f,1.0f)));
    ImVec2 tsz = ImGui::CalcTextSize(ini.c_str());
    dl->AddText(ImVec2(cen.x - tsz.x * 0.5f, cen.y - tsz.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(kTextPri), ini.c_str());
    ImGui::Dummy(ImVec2(sz.x, r * 2 + 6));
  }

  auto row = [](const char *lbl, const std::string &v) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::Text("%-12s", lbl);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
    ImGui::TextUnformatted(v.empty() ? "\xe2\x80\x94" : v.c_str());
    ImGui::PopStyleColor();
  };
  row("Name",        m.name);
  row("Age",         m.age);
  row("Nation",      m.nationality);
  row("Gender",      m.gender);
  row("Club",        m.clubName);
  EndCard();
}

static void DrawClubSummaryCard(ImVec2 sz) {
  BeginCard("##club_sum", sz, "CLUB");
  const auto &cl = g_CareerHub.club;

  float badgeSz = 40.0f;
  GLuint tex = LoadBadgeTex(cl.logoPath);
  if (tex != 0) {
    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(badgeSz, badgeSz));
    ImGui::SameLine(0, 12);
    ImGui::BeginGroup();
  }
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted(cl.name.empty() ? "\xe2\x80\x94" : cl.name.c_str());
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted(cl.leagueName.empty() ? "League" : cl.leagueName.c_str());
  ImGui::PopStyleColor();
  if (tex != 0) ImGui::EndGroup();

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Spacing();

  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::Text("Squad size");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::Text("%d players", (int)g_CareerHub.players.size());
  ImGui::PopStyleColor();
  EndCard();
}

static void DrawSeasonSummaryCard(ImVec2 sz) {
  BeginCard("##season_sum", sz, "SEASON");
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::Text("Fixtures generated");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::Text("%d", (int)g_CareerHub.fixtures.size());
  ImGui::PopStyleColor();

  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::Text("Standings rows");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::Text("%d", (int)g_CareerHub.standings.size());
  ImGui::PopStyleColor();

  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::Text("Season");
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("2026/27");
  ImGui::PopStyleColor();

  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("More data coming soon.");
  ImGui::PopStyleColor();
  EndCard();
}

static void DrawNextFixtureCard(ImVec2 sz) {
  BeginCard("##nxt_fix", sz, "NEXT FIXTURE");

  const std::string &sn = g_CareerHub.club.shortName;
  const CareerHubState::Fixture *f = nullptr;
  for (const auto &x : g_CareerHub.fixtures) {
    if (x.status == "scheduled" && (x.home == sn || x.away == sn)) { f = &x; break; }
  }
  if (!f)
    for (const auto &x : g_CareerHub.fixtures)
      if (x.home == sn || x.away == sn) { f = &x; break; }

  if (!f) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No upcoming fixtures.");
    ImGui::PopStyleColor();
    EndCard();
    return;
  }

  ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
  ImGui::Text("%s  \xe2\x80\xa2  Matchday %s", f->league.c_str(), f->matchday.c_str());
  ImGui::PopStyleColor();
  ImGui::Spacing();

  float avail  = ImGui::GetContentRegionAvail().x;
  float teamW  = (avail - 48.0f) * 0.5f;
  if (teamW < 40.0f) teamW = 40.0f;
  const float kBadge = 42.0f;

  // Home box
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgCardAlt);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
  ImGui::BeginChild("##nf_h", ImVec2(teamW, 80), true);
  ImGui::PopStyleVar(2); ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec); ImGui::TextUnformatted("HOME"); ImGui::PopStyleColor();
  GLuint hTex = LoadBadgeTex(f->homeLogo);
  if (hTex) { ImGui::Image((ImTextureID)(intptr_t)hTex, ImVec2(kBadge, kBadge)); ImGui::SameLine(0,6); }
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kBadge - ImGui::GetTextLineHeight()) * (hTex ? 0.5f : 0.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, (f->home==sn) ? kAccent : kTextPri);
  ImGui::TextUnformatted(f->home.c_str()); ImGui::PopStyleColor();
  ImGui::EndChild();

  ImGui::SameLine(0, 4);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 30);
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent); ImGui::TextUnformatted("vs"); ImGui::PopStyleColor();
  ImGui::SameLine(0, 4);
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 30);

  // Away box
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgCardAlt);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 8.0f));
  ImGui::BeginChild("##nf_a", ImVec2(teamW, 80), true);
  ImGui::PopStyleVar(2); ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec); ImGui::TextUnformatted("AWAY"); ImGui::PopStyleColor();
  GLuint aTex = LoadBadgeTex(f->awayLogo);
  if (aTex) { ImGui::Image((ImTextureID)(intptr_t)aTex, ImVec2(kBadge, kBadge)); ImGui::SameLine(0,6); }
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (kBadge - ImGui::GetTextLineHeight()) * (aTex ? 0.5f : 0.0f));
  ImGui::PushStyleColor(ImGuiCol_Text, (f->away==sn) ? kAccent : kTextPri);
  ImGui::TextUnformatted(f->away.c_str()); ImGui::PopStyleColor();
  ImGui::EndChild();

  EndCard();
}

static void DrawFixturesPreviewCard(ImVec2 sz) {
  BeginCard("##fix_prev", sz, "UPCOMING FIXTURES");
  const std::string &sn = g_CareerHub.club.shortName;
  int shown = 0;

  float tblH = sz.y - 58.0f;
  if (tblH < 30.0f) tblH = 30.0f;

  if (ImGui::BeginTable("##fp_tbl", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, tblH))) {
    ImGui::TableSetupColumn("Opponent", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Comp",     ImGuiTableColumnFlags_WidthFixed, 52.0f);
    ImGui::TableSetupColumn("H/A",      ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("Status",   ImGuiTableColumnFlags_WidthFixed, 86.0f);
    for (const auto &f : g_CareerHub.fixtures) {
      if (f.home != sn && f.away != sn) continue;
      if (shown >= 7) break;
      bool ih = (f.home == sn);
      const std::string &opp  = ih ? f.away : f.home;
      const std::string &logo = ih ? f.awayLogo : f.homeLogo;
      std::string comp = f.league.size() > 7 ? f.league.substr(0, 7) : f.league;

      ImGui::TableNextRow(0, 28.0f);
      ImGui::TableSetColumnIndex(0);
      DrawTeamLabel(logo, opp, 16.0f);
      ImGui::TableSetColumnIndex(1);
      ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
      ImGui::TextUnformatted(comp.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, ih ? kSuccess : kTextSec);
      ImGui::TextUnformatted(ih ? "H" : "A");
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(3);
      if (f.score != "-" && !f.score.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kSuccess);
        ImGui::TextUnformatted(f.score.c_str());
        ImGui::PopStyleColor();
      } else {
        DrawStatusPill(f.status);
      }
      shown++;
    }
    if (shown == 0) {
      ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted("No fixtures found.");
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }
  EndCard();
}

static void DrawLeagueSnapshotCard(ImVec2 sz) {
  BeginCard("##league_snap", sz, "LEAGUE SNAPSHOT");
  const std::string &sn      = g_CareerHub.club.shortName;
  const std::string &myLeague = g_CareerHub.club.leagueName;

  if (myLeague.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No standings data.");
    ImGui::PopStyleColor();
    EndCard(); return;
  }

  ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
  ImGui::TextUnformatted(myLeague.c_str());
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Spacing();

  float tblH = sz.y - 70.0f;
  if (tblH < 30.0f) tblH = 30.0f;
  if (ImGui::BeginTable("##snap_tbl", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, tblH))) {
    ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("P",    ImGuiTableColumnFlags_WidthFixed, 22.0f);
    ImGui::TableSetupColumn("Pts",  ImGuiTableColumnFlags_WidthFixed, 36.0f);

    int pos = 1;
    for (const auto &s : g_CareerHub.standings) {
      if (s.league != myLeague) continue;
      bool mine = (s.team == sn);
      ImGui::TableNextRow(0, 26.0f);
      if (mine) {
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
          ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f,0.08f,0.65f,0.25f)));
      }
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      ImGui::Text("%d. ", pos);
      ImGui::PopStyleColor();
      ImGui::SameLine(0,2);
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
  EndCard();
}

static void DrawMessagesCard(ImVec2 sz) {
  static const struct { const char *from, *subject, *when; } kMsgs[] = {
    { "Board",  "Pre-season objectives confirmed", "Today"     },
    { "Media",  "Press conference scheduled",       "Yesterday" },
    { "Staff",  "Fitness report available",          "2d ago"    },
    { "Board",  "Transfer budget allocated",         "3d ago"    },
    { "Fans",   "Season ticket renewals open",       "4d ago"    },
    { "Staff",  "Training schedule published",       "5d ago"    },
  };
  const int kN = 6;
  BeginCard("##msgs_ov", sz, "MESSAGES");
  float tblH = sz.y - 58.0f;
  if (tblH < 20.0f) tblH = 20.0f;
  if (ImGui::BeginTable("##msg_tbl", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, tblH))) {
    ImGui::TableSetupColumn("From",    ImGuiTableColumnFlags_WidthFixed,  66.0f);
    ImGui::TableSetupColumn("Subject", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("When",    ImGuiTableColumnFlags_WidthFixed,  60.0f);
    for (int i = 0; i < kN; i++) {
      ImGui::TableNextRow(0, 27.0f);
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kViolet);
      ImGui::TextUnformatted(kMsgs[i].from);
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(kMsgs[i].subject);
      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      ImGui::TextUnformatted(kMsgs[i].when);
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }
  EndCard();
}

// ---- DrawOverviewPage ---------------------------------------------------

static void DrawOverviewPage(float w, float h) {
  const float kPad = 14.0f, kGap = 10.0f;
  float colW = (w - kPad * 2.0f - kGap * 2.0f) / 3.0f;
  float colH  = h - 14.0f;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));

  // Left column
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_l", ImVec2(colW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  float mgrH  = colH * 0.44f;
  float clubH = colH * 0.30f;
  float seaH  = colH - mgrH - clubH - kGap * 2.0f;
  DrawManagerSummaryCard(ImVec2(colW, mgrH));
  ImGui::Dummy(ImVec2(0, kGap));
  DrawClubSummaryCard(ImVec2(colW, clubH));
  ImGui::Dummy(ImVec2(0, kGap));
  DrawSeasonSummaryCard(ImVec2(colW, seaH));
  ImGui::EndChild();

  ImGui::SameLine(0, kGap);

  // Center column
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_c", ImVec2(colW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  float nxtH  = colH * 0.38f;
  float prevH = colH - nxtH - kGap;
  DrawNextFixtureCard(ImVec2(colW, nxtH));
  ImGui::Dummy(ImVec2(0, kGap));
  DrawFixturesPreviewCard(ImVec2(colW, prevH));
  ImGui::EndChild();

  ImGui::SameLine(0, kGap);

  // Right column
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_r", ImVec2(colW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  float snapH = colH * 0.55f;
  float msgH  = colH - snapH - kGap;
  DrawLeagueSnapshotCard(ImVec2(colW, snapH));
  ImGui::Dummy(ImVec2(0, kGap));
  DrawMessagesCard(ImVec2(colW, msgH));
  ImGui::EndChild();
}

// ---- DrawManagerPage ----------------------------------------------------

static void DrawManagerPage(float w, float h) {
  const float kPad = 14.0f, kGap = 10.0f;
  float leftW  = w * 0.36f - kPad;
  float rightW = w - leftW - kGap - kPad * 2.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float colH = h - 14.0f;

  // Left: profile
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##mgr_l", ImVec2(leftW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();

  BeginCard("##mgr_card", ImVec2(leftW, colH * 0.60f), "MANAGER PROFILE");
  ImGui::Spacing();
  const auto &m = g_CareerHub.manager;

  // Initials circle
  std::string ini;
  if (!m.name.empty()) {
    ini += (char)toupper(m.name[0]);
    size_t sp = m.name.find(' ');
    if (sp != std::string::npos && sp + 1 < m.name.size())
      ini += (char)toupper(m.name[sp + 1]);
  }
  float cr = 30.0f;
  ImVec2 cpos = ImGui::GetCursorScreenPos();
  ImVec2 cen  = ImVec2(cpos.x + cr, cpos.y + cr);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddCircleFilled(cen, cr, ImGui::ColorConvertFloat4ToU32(ImVec4(0.35f,0.10f,0.70f,1.0f)));
  if (!ini.empty()) {
    ImVec2 tsz = ImGui::CalcTextSize(ini.c_str());
    dl->AddText(ImVec2(cen.x - tsz.x * 0.5f, cen.y - tsz.y * 0.5f),
                ImGui::ColorConvertFloat4ToU32(kTextPri), ini.c_str());
  }
  ImGui::Dummy(ImVec2(leftW, cr * 2 + 10));

  auto prow = [](const char *lbl, const std::string &v) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::Text("%-12s", lbl);
    ImGui::PopStyleColor();
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
  EndCard();

  ImGui::EndChild();
  ImGui::SameLine(0, kGap);

  // Right: supplementary cards
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##mgr_r", ImVec2(rightW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();

  float ch = (colH - kGap * 2.0f) / 3.0f;
  BeginCard("##mgr_c1", ImVec2(rightW, ch), "CAREER SUMMARY");
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("No previous clubs.");
  ImGui::TextUnformatted("First season as a manager.");
  ImGui::PopStyleColor();
  EndCard();
  ImGui::Dummy(ImVec2(0, kGap));

  BeginCard("##mgr_c2", ImVec2(rightW, ch), "CURRENT JOB");
  auto jr = [](const char *l, const char *v) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::Text("%-10s", l);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
    ImGui::TextUnformatted(v);
    ImGui::PopStyleColor();
    ImGui::Spacing();
  };
  jr("Club",   g_CareerHub.manager.clubName.empty() ? "\xe2\x80\x94" : g_CareerHub.manager.clubName.c_str());
  jr("League", g_CareerHub.club.leagueName.empty()  ? "\xe2\x80\x94" : g_CareerHub.club.leagueName.c_str());
  jr("Season", "2026/27");
  EndCard();
  ImGui::Dummy(ImVec2(0, kGap));

  BeginCard("##mgr_c3", ImVec2(rightW, ch), "OBJECTIVES");
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("Objectives will be set by the board.");
  ImGui::TextUnformatted("Check back at the start of the season.");
  ImGui::PopStyleColor();
  EndCard();

  ImGui::EndChild();
}

// ---- DrawClubPage -------------------------------------------------------

static void DrawClubPage(float w, float h) {
  const float kPad = 14.0f, kGap = 10.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 14.0f;

  // Club header card
  float hdrH = 80.0f;
  BeginCard("##club_hdr", ImVec2(usW, hdrH));
  const auto &cl = g_CareerHub.club;
  float badgeSz = 52.0f;
  GLuint tex = LoadBadgeTex(cl.logoPath);
  if (tex) {
    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(badgeSz, badgeSz));
    ImGui::SameLine(0, 14);
    ImGui::BeginGroup();
  }
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 8);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::SetWindowFontScale(1.18f);
  ImGui::TextUnformatted(cl.name.empty() ? "\xe2\x80\x94" : cl.name.c_str());
  ImGui::SetWindowFontScale(1.0f);
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted(cl.leagueName.empty() ? "" : cl.leagueName.c_str());
  ImGui::PopStyleColor();
  if (tex) ImGui::EndGroup();
  EndCard();

  ImGui::Dummy(ImVec2(0, kGap));

  // Squad card
  float squadH = usH - hdrH - kGap - 6.0f;
  BeginCard("##squad_card", ImVec2(usW, squadH), "SQUAD");
  float tblH = squadH - 56.0f;
  if (tblH < 30.0f) tblH = 30.0f;

  if (ImGui::BeginTable("##squad_tbl", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX |
                        ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0, tblH))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Role",    ImGuiTableColumnFlags_WidthFixed, 158.0f);
    ImGui::TableSetupColumn("Age",     ImGuiTableColumnFlags_WidthFixed,  42.0f);
    ImGui::TableSetupColumn("Ability", ImGuiTableColumnFlags_WidthFixed,  58.0f);
    ImGui::TableHeadersRow();
    for (const auto &p : g_CareerHub.players) {
      ImGui::TableNextRow(0, 27.0f);
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%s %s", p.firstName.c_str(), p.lastName.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted(p.role.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted(p.age.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(p.ability.c_str());
    }
    ImGui::EndTable();
  }
  EndCard();
}

// ---- DrawMatchesPage ----------------------------------------------------

static void DrawMatchesPage(float w, float h) {
  const float kPad = 14.0f;
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

  BeginCard("##matches_outer", ImVec2(usW, usH), "FIXTURES");
  ImGui::BeginChild("##m_scroll", ImVec2(0, usH - 56.0f), false);

  if (g_CareerHub.fixtures.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No fixtures generated yet.");
    ImGui::PopStyleColor();
  }

  for (unsigned int li = 0; li < leagues.size(); li++) {
    const std::string &lg = leagues.at(li);
    if (li > 0) { ImGui::Spacing(); ImGui::Spacing(); }

    ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
    ImGui::Text("  %s", lg.c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    std::string tblId = "##fix_" + lg;
    if (ImGui::BeginTable(tblId.c_str(), 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("MD",     ImGuiTableColumnFlags_WidthFixed,  32.0f);
      ImGui::TableSetupColumn("Rd",     ImGuiTableColumnFlags_WidthFixed,  32.0f);
      ImGui::TableSetupColumn("Home",   ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Away",   ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,  96.0f);
      ImGui::TableSetupColumn("Score",  ImGuiTableColumnFlags_WidthFixed,  60.0f);
      ImGui::TableHeadersRow();

      for (const auto &f : g_CareerHub.fixtures) {
        if (f.league != lg) continue;
        bool myGame = (f.home == sn || f.away == sn);
        ImGui::TableNextRow(0, 28.0f);
        if (myGame) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.30f,0.07f,0.55f,0.20f)));
        }
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
        ImGui::TextUnformatted(f.matchday.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
        ImGui::TextUnformatted(f.round.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        DrawTeamLabel(f.homeLogo, f.home, 16.0f);
        ImGui::TableSetColumnIndex(3);
        DrawTeamLabel(f.awayLogo, f.away, 16.0f);
        ImGui::TableSetColumnIndex(4);
        DrawStatusPill(f.status);
        ImGui::TableSetColumnIndex(5);
        if (f.score != "-" && !f.score.empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, kSuccess);
          ImGui::TextUnformatted(f.score.c_str());
          ImGui::PopStyleColor();
        } else {
          ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
          ImGui::TextUnformatted("\xe2\x80\x94");
          ImGui::PopStyleColor();
        }
      }
      ImGui::EndTable();
    }
  }
  ImGui::EndChild();
  EndCard();
}

// ---- DrawStandingsPage --------------------------------------------------

static void DrawStandingsPage(float w, float h) {
  const float kPad = 14.0f;
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

  BeginCard("##std_outer", ImVec2(usW, usH), "LEAGUE STANDINGS");
  ImGui::BeginChild("##s_scroll", ImVec2(0, usH - 56.0f), false);

  if (g_CareerHub.standings.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No standings generated yet.");
    ImGui::PopStyleColor();
  }

  for (unsigned int li = 0; li < leagues.size(); li++) {
    const std::string &lg = leagues.at(li);
    if (li > 0) { ImGui::Spacing(); ImGui::Spacing(); }

    ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
    ImGui::Text("  %s", lg.c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    std::string tblId = "##std_" + lg;
    if (ImGui::BeginTable(tblId.c_str(), 10,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed,  24.0f);
      ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("P",    ImGuiTableColumnFlags_WidthFixed,  28.0f);
      ImGui::TableSetupColumn("W",    ImGuiTableColumnFlags_WidthFixed,  28.0f);
      ImGui::TableSetupColumn("D",    ImGuiTableColumnFlags_WidthFixed,  28.0f);
      ImGui::TableSetupColumn("L",    ImGuiTableColumnFlags_WidthFixed,  28.0f);
      ImGui::TableSetupColumn("GF",   ImGuiTableColumnFlags_WidthFixed,  32.0f);
      ImGui::TableSetupColumn("GA",   ImGuiTableColumnFlags_WidthFixed,  32.0f);
      ImGui::TableSetupColumn("GD",   ImGuiTableColumnFlags_WidthFixed,  36.0f);
      ImGui::TableSetupColumn("Pts",  ImGuiTableColumnFlags_WidthFixed,  40.0f);
      ImGui::TableHeadersRow();

      int pos = 1;
      for (const auto &s : g_CareerHub.standings) {
        if (s.league != lg) continue;
        bool mine = (s.team == sn);
        ImGui::TableNextRow(0, 28.0f);
        if (mine) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.30f,0.07f,0.55f,0.20f)));
        }

        auto stat = [](const std::string &v) {
          ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
          ImGui::TextUnformatted(v.c_str());
          ImGui::PopStyleColor();
        };

        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::Text("%d", pos);
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        DrawTeamLabel(s.teamLogo, s.team, 16.0f);
        ImGui::TableSetColumnIndex(2); stat(s.p);
        ImGui::TableSetColumnIndex(3); stat(s.w);
        ImGui::TableSetColumnIndex(4); stat(s.d);
        ImGui::TableSetColumnIndex(5); stat(s.l);
        ImGui::TableSetColumnIndex(6); stat(s.gf);
        ImGui::TableSetColumnIndex(7); stat(s.ga);
        ImGui::TableSetColumnIndex(8); stat(s.gd);
        ImGui::TableSetColumnIndex(9);
        ImGui::PushStyleColor(ImGuiCol_Text, mine ? kGold : kTextPri);
        ImGui::TextUnformatted(s.pts.c_str());
        ImGui::PopStyleColor();
        pos++;
      }
      ImGui::EndTable();
    }
  }
  ImGui::EndChild();
  EndCard();
}

// ---- DrawComingSoonPage -------------------------------------------------

static void DrawComingSoonPage(float w, float h, const char *section) {
  const float kPad = 14.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float cw = w - kPad * 2.0f;
  float ch = h * 0.38f;
  if (ch < 80.0f) ch = 80.0f;

  BeginCard("##soon_card", ImVec2(cw, ch));
  ImGui::Spacing(); ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::SetWindowFontScale(1.20f);
  ImGui::TextUnformatted(section);
  ImGui::SetWindowFontScale(1.0f);
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("This section is coming soon.");
  ImGui::Spacing();
  ImGui::TextUnformatted("Focus is currently on the Portal and career hub.");
  ImGui::PopStyleColor();
  EndCard();
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

  DrawTopBar(winW);
  DrawPortalSubNav(winW);

  float contentH = winH - kHdrH - 1.0f - kSubNavH - 1.0f;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgApp);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("##content", ImVec2(winW, contentH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  if (g_CareerHub.primaryTab == 0) {
    // Portal: sub-tabs
    switch (g_CareerHub.activeTab) {
      case 0: DrawOverviewPage(winW, contentH);   break;
      case 1: DrawManagerPage(winW, contentH);    break;
      case 2: DrawClubPage(winW, contentH);       break;
      case 3: DrawMatchesPage(winW, contentH);    break;
      case 4: DrawStandingsPage(winW, contentH);  break;
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
