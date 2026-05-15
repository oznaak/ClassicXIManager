#include "imgui_menu.hpp"
#include "imgui_manager_fonts.hpp"
#include "managercareer.hpp"

#include "../main.hpp"
#include "imgui.h"
#include <sstream>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>

// ---- Global state -------------------------------------------------------

PreCareerState g_PreCareer;

void PreCareerState::Clear() {
  active        = false;
  screen        = PRECAREER_NONE;
  pendingAction = 0;
  pendingId     = 0;
  isTransitioning = false;

  onNewGame      = nullptr;
  onLoadGame     = nullptr;
  onSettings     = nullptr;
  onQuit         = nullptr;
  onCreateProfile = nullptr;
  onBack         = nullptr;
  onLoadManager  = nullptr;
  onSelectLeague = nullptr;
  onStartCareer  = nullptr;

  memset(nameBuffer, 0, sizeof(nameBuffer));
  memset(ageBuf, 0, sizeof(ageBuf));
  nationalityIdx = 0;
  genderIdx      = 0;

  currentManagerId = 0;
  currentLeagueId = 0;
  leagues.clear();
  clubs.clear();
  selectedClubId = 0;
  saves.clear();
}

static std::string SqlEscape(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    if (c == '\'') out += "''";
    else out += c;
  }
  return out;
}

static int LastInsertId() {
  DatabaseResult *r = GetDB()->Query("SELECT last_insert_rowid();");
  int id = 0;
  if (r->data.size() > 0 && r->data.at(0).size() > 0)
    id = atoi(r->data.at(0).at(0).c_str());
  delete r;
  return id;
}

static void LoadPreCareerSaves() {
  g_PreCareer.saves.clear();
  DatabaseResult *result = GetDB()->Query(
    "SELECT managers.id, managers.name, managers.age, managers.nationality, "
    "teams.name "
    "FROM managers "
    "LEFT JOIN teams ON managers.club_id = teams.id "
    "ORDER BY managers.id DESC LIMIT 20;"
  );
  for (unsigned int i = 0; i < result->data.size(); i++) {
    PreCareerState::SaveEntry e;
    e.id          = atoi(result->data.at(i).at(0).c_str());
    e.name        = result->data.at(i).at(1);
    e.age         = atoi(result->data.at(i).at(2).c_str());
    e.nationality = result->data.at(i).at(3);
    e.clubName    = result->data.at(i).at(4);
    if (e.name.empty())     e.name     = "Unnamed Manager";
    if (e.clubName.empty()) e.clubName = "No Club";
    g_PreCareer.saves.push_back(e);
  }
  delete result;
}

static void LoadPreCareerLeagues() {
  g_PreCareer.leagues.clear();
  DatabaseResult *res = GetDB()->Query("SELECT id, name FROM leagues ORDER BY name LIMIT 20;");
  for (unsigned int i = 0; i < res->data.size(); i++) {
    PreCareerState::LeagueItem item;
    item.id   = atoi(res->data.at(i).at(0).c_str());
    item.name = res->data.at(i).at(1);
    if (item.name.empty()) item.name = "League";
    g_PreCareer.leagues.push_back(item);
  }
  delete res;
}

static void LoadPreCareerClubs(int leagueId) {
  g_PreCareer.clubs.clear();
  std::stringstream q;
  q << "SELECT id, name, shortname, logo_url FROM teams WHERE league_id = "
    << leagueId << " ORDER BY name LIMIT 20;";
  DatabaseResult *res = GetDB()->Query(q.str());
  for (unsigned int i = 0; i < res->data.size(); i++) {
    PreCareerState::ClubItem item;
    item.id        = atoi(res->data.at(i).at(0).c_str());
    item.name      = res->data.at(i).at(1);
    item.shortName = res->data.at(i).at(2);
    item.logoPath  = res->data.at(i).at(3);
    if (item.name.empty()) item.name = "Club";
    g_PreCareer.clubs.push_back(item);
  }
  delete res;
}

static void EnterPreCareerMainMenu() {
  g_PreCareer.Clear();
  g_PreCareer.screen = PRECAREER_MAIN_MENU;
  g_PreCareer.active = true;
}

static void EnterPreCareerCreateProfile() {
  if (g_PreCareer.currentManagerId == 0) {
    strncpy(g_PreCareer.nameBuffer, "Manager", sizeof(g_PreCareer.nameBuffer) - 1);
    strncpy(g_PreCareer.ageBuf, "35", sizeof(g_PreCareer.ageBuf) - 1);
    g_PreCareer.nationalityIdx = 0;
    g_PreCareer.genderIdx = 0;
  }
  g_PreCareer.screen = PRECAREER_CREATE_PROFILE;
  g_PreCareer.active = true;
}

static void EnterPreCareerLoadGame() {
  g_PreCareer.Clear();
  LoadPreCareerSaves();
  g_PreCareer.screen = PRECAREER_LOAD_GAME;
  g_PreCareer.active = true;
}

static void EnterPreCareerSelectLeague(int managerId) {
  g_PreCareer.currentManagerId = managerId;
  g_PreCareer.currentLeagueId = 0;
  g_PreCareer.selectedClubId = 0;
  LoadPreCareerLeagues();
  g_PreCareer.screen = PRECAREER_SELECT_LEAGUE;
  g_PreCareer.active = true;
}

static void EnterPreCareerSelectClub(int managerId, int leagueId) {
  g_PreCareer.currentManagerId = managerId;
  g_PreCareer.currentLeagueId = leagueId;
  g_PreCareer.selectedClubId = 0;
  LoadPreCareerClubs(leagueId);
  g_PreCareer.screen = PRECAREER_SELECT_CLUB;
  g_PreCareer.active = true;
}

static void EnterPreCareerSettingsPlaceholder() {
  g_PreCareer.screen = PRECAREER_SETTINGS_PLACEHOLDER;
  g_PreCareer.active = true;
}

static void PreCareerStartCareer() {
  int managerId = g_PreCareer.currentManagerId;
  int clubId = g_PreCareer.selectedClubId;
  if (managerId == 0 || clubId == 0) return;

  std::stringstream q;
  q << "UPDATE managers SET club_id = " << clubId
    << " WHERE id = " << managerId << ";";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;

  GenerateCareerSeason(managerId);
  GetMenuTask()->RequestManagerCareerPage(managerId);

  g_PreCareer.Clear();
  g_PreCareer.screen = PRECAREER_NONE;
}

static void PreCareerLoadManager(int managerId) {
  GetMenuTask()->RequestManagerCareerPage(managerId);
  g_PreCareer.Clear();
  g_PreCareer.screen = PRECAREER_NONE;
}

static void ProcessPreCareerPendingAction() {
  if (g_PreCareer.pendingAction == 0) return;

  int action = g_PreCareer.pendingAction;
  int pid = g_PreCareer.pendingId;
  g_PreCareer.pendingAction = 0;
  g_PreCareer.pendingId = 0;

  switch (action) {
    case 1:
      EnterPreCareerCreateProfile();
      break;
    case 2:
      EnterPreCareerLoadGame();
      break;
    case 3:
      EnterPreCareerSettingsPlaceholder();
      break;
    case 4:
      GetMenuTask()->QuitGame();
      break;
    case 5: {
      std::string name = g_PreCareer.nameBuffer;
      int age = atoi(g_PreCareer.ageBuf);
      int natIdx = g_PreCareer.nationalityIdx;
      int genIdx = g_PreCareer.genderIdx;
      static const char *kNats[] = {
        "Portugal","England","Spain","France","Germany",
        "Italy","Netherlands","Brazil","Argentina","United States"
      };
      static const char *kGens[] = { "Male", "Female" };
      if (natIdx < 0 || natIdx > 9) natIdx = 0;
      if (genIdx < 0 || genIdx > 1) genIdx = 0;
      std::string nat = kNats[natIdx];
      std::string gen = kGens[genIdx];
      if (name.empty()) name = "Manager";
      if (age < 18) age = 18;
      if (age > 99) age = 99;

      std::stringstream q;
      q << "INSERT INTO managers(name,age,nationality,gender,club_id) VALUES(";
      q << "'" << SqlEscape(name) << "',";
      q << age << ",";
      q << "'" << SqlEscape(nat) << "',";
      q << "'" << SqlEscape(gen) << "',";
      q << "NULL);";
      DatabaseResult *r = GetDB()->Query(q.str());
      delete r;
      int managerId = LastInsertId();
      EnterPreCareerSelectLeague(managerId);
    } break;
    case 6:
      switch (g_PreCareer.screen) {
        case PRECAREER_CREATE_PROFILE:
        case PRECAREER_LOAD_GAME:
        case PRECAREER_SETTINGS_PLACEHOLDER:
          EnterPreCareerMainMenu();
          break;
        case PRECAREER_SELECT_LEAGUE:
          EnterPreCareerCreateProfile();
          break;
        case PRECAREER_SELECT_CLUB:
          EnterPreCareerSelectLeague(g_PreCareer.currentManagerId);
          break;
        default:
          EnterPreCareerMainMenu();
          break;
      }
      break;
    case 7:
      PreCareerLoadManager(pid);
      break;
    case 8:
      EnterPreCareerSelectClub(g_PreCareer.currentManagerId, pid);
      break;
    case 10:
      PreCareerStartCareer();
      break;
    default:
      break;
  }
}

// ---- Color palette (matches imgui_career.cpp) ---------------------------

static const ImVec4 kBgApp     = ImVec4(0.027f, 0.043f, 0.086f, 1.0f);
static const ImVec4 kBgSidebar = ImVec4(0.043f, 0.063f, 0.125f, 1.0f);
static const ImVec4 kBgCard    = ImVec4(0.071f, 0.102f, 0.173f, 1.0f);
static const ImVec4 kBgCardAlt = ImVec4(0.094f, 0.129f, 0.212f, 1.0f);
static const ImVec4 kBorder    = ImVec4(0.149f, 0.196f, 0.290f, 0.80f);
static const ImVec4 kAccent    = ImVec4(0.741f, 0.102f, 0.788f, 1.0f);
static const ImVec4 kAccentH   = ImVec4(0.863f, 0.318f, 0.918f, 1.0f);
static const ImVec4 kAccentA   = ImVec4(0.576f, 0.047f, 0.620f, 1.0f);
static const ImVec4 kViolet    = ImVec4(0.482f, 0.231f, 0.929f, 1.0f);
static const ImVec4 kTextPri   = ImVec4(0.937f, 0.949f, 0.965f, 1.0f);
static const ImVec4 kTextSec   = ImVec4(0.612f, 0.655f, 0.729f, 1.0f);
static const ImVec4 kTextDim   = ImVec4(0.239f, 0.290f, 0.388f, 1.0f);
static const ImVec4 kSuccess   = ImVec4(0.133f, 0.773f, 0.369f, 1.0f);
static const ImVec4 kDanger    = ImVec4(0.937f, 0.267f, 0.267f, 1.0f);
static const ImVec4 kGold      = ImVec4(0.992f, 0.820f, 0.110f, 1.0f);

static inline ImU32 C32(const ImVec4 &v) { return ImGui::ColorConvertFloat4ToU32(v); }

// ---- Font helpers -------------------------------------------------------

static inline void PushMF(ImFont *f) { if (f) ImGui::PushFont(f); }
static inline void PopMF(ImFont *f)  { if (f) ImGui::PopFont(); }

// ---- Theme --------------------------------------------------------------

static void ApplyPreCareerTheme() {
  ImGuiStyle &st = ImGui::GetStyle();
  st.WindowRounding    = 0.0f;
  st.ChildRounding     = 10.0f;
  st.FrameRounding     = 6.0f;
  st.ScrollbarRounding = 4.0f;
  st.GrabRounding      = 4.0f;
  st.WindowBorderSize  = 0.0f;
  st.ChildBorderSize   = 0.0f;
  st.FrameBorderSize   = 0.0f;
  st.WindowPadding     = ImVec2(0.0f, 0.0f);
  st.FramePadding      = ImVec2(10.0f, 6.0f);
  st.ItemSpacing       = ImVec2(8.0f, 6.0f);
  st.ScrollbarSize     = 6.0f;

  ImVec4 *c = st.Colors;
  c[ImGuiCol_WindowBg]             = kBgApp;
  c[ImGuiCol_ChildBg]              = kBgCard;
  c[ImGuiCol_PopupBg]              = kBgCard;
  c[ImGuiCol_Border]               = kBorder;
  c[ImGuiCol_BorderShadow]         = ImVec4(0,0,0,0);
  c[ImGuiCol_FrameBg]              = ImVec4(0.035f,0.055f,0.102f,1.0f);
  c[ImGuiCol_FrameBgHovered]       = ImVec4(0.055f,0.082f,0.153f,1.0f);
  c[ImGuiCol_FrameBgActive]        = ImVec4(0.082f,0.122f,0.220f,1.0f);
  c[ImGuiCol_Button]               = ImVec4(0.055f,0.082f,0.153f,1.0f);
  c[ImGuiCol_ButtonHovered]        = ImVec4(0.082f,0.122f,0.220f,1.0f);
  c[ImGuiCol_ButtonActive]         = ImVec4(0.110f,0.161f,0.282f,1.0f);
  c[ImGuiCol_Header]               = ImVec4(0.082f,0.122f,0.220f,0.60f);
  c[ImGuiCol_HeaderHovered]        = ImVec4(0.110f,0.161f,0.282f,0.80f);
  c[ImGuiCol_HeaderActive]         = ImVec4(0.176f,0.243f,0.396f,1.0f);
  c[ImGuiCol_Separator]            = kBorder;
  c[ImGuiCol_ScrollbarBg]          = ImVec4(0,0,0,0);
  c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.110f,0.161f,0.282f,1.0f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.176f,0.243f,0.396f,1.0f);
  c[ImGuiCol_ScrollbarGrabActive]  = kViolet;
  c[ImGuiCol_CheckMark]            = kAccent;
  c[ImGuiCol_Text]                 = kTextPri;
  c[ImGuiCol_TextDisabled]         = kTextDim;
}

// ---- Button helpers -----------------------------------------------------

static bool CTABtn(const char *lbl, ImVec2 sz = ImVec2(0,0)) {
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

static bool DangerBtn(const char *lbl, ImVec2 sz = ImVec2(0,0)) {
  ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f,0.04f,0.04f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f,0.07f,0.07f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f,0.10f,0.10f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text,          kDanger);
  bool r = ImGui::Button(lbl, sz);
  ImGui::PopStyleColor(4);
  return r;
}

// ---- Fallback badge (colored initials square) ---------------------------

static void DrawFallbackBadge(const std::string &sn, float sz) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  unsigned int hash = 5381;
  for (char c : sn) hash = ((hash << 5) + hash) ^ (unsigned char)c;
  float hue = (float)(hash % 360) / 360.0f;
  float r, g, b;
  ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.48f, r, g, b);
  dl->AddRectFilled(p, ImVec2(p.x+sz, p.y+sz),
    IM_COL32((int)(r*255),(int)(g*255),(int)(b*255),215), sz*0.22f);
  std::string ini;
  for (unsigned int i = 0; i < sn.size() && (int)ini.size() < 2; i++)
    if (isalpha((unsigned char)sn[i])) ini += (char)toupper((unsigned char)sn[i]);
  if (!ini.empty()) {
    ImVec2 tsz = ImGui::CalcTextSize(ini.c_str());
    dl->AddText(ImVec2(p.x+(sz-tsz.x)*0.5f, p.y+(sz-tsz.y)*0.5f),
                IM_COL32(255,255,255,215), ini.c_str());
  }
  ImGui::Dummy(ImVec2(sz, sz));
}

// ---- Decorative background ----------------------------------------------

static void DrawPreCareerBackground(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  dl->AddRectFilled(wp, ImVec2(wp.x+w, wp.y+h), C32(kBgApp));
  // subtle corner glow
  dl->AddCircleFilled(ImVec2(wp.x + w * 0.15f, wp.y + h * 0.5f),
                      h * 0.60f, IM_COL32(55,12,110,12), 48);
  dl->AddCircleFilled(ImVec2(wp.x + w * 0.15f, wp.y + h * 0.5f),
                      h * 0.28f, IM_COL32(80,18,160,18), 48);
  dl->AddRectFilledMultiColor(wp, ImVec2(wp.x+w, wp.y+90.0f),
    IM_COL32(16,26,62,30), IM_COL32(16,26,62,30),
    IM_COL32(0,0,0,0), IM_COL32(0,0,0,0));
}

// ---- Centered card helper -----------------------------------------------

// Renders a card-shaped child window centered on screen.
// Returns true if BeginChild succeeded (always does).
static bool BeginCenteredCard(const char *id, float cardW, float cardH,
                               float winW, float winH) {
  float x = (winW - cardW) * 0.5f;
  float y = (winH - cardH) * 0.5f;
  if (y < 10.0f) y = 10.0f;

  ImVec2 p0 = ImVec2(ImGui::GetWindowPos().x + x, ImGui::GetWindowPos().y + y);
  ImVec2 p1 = ImVec2(p0.x + cardW, p0.y + cardH);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, C32(kBgCard), 14.0f);
  dl->AddRect(p0, p1, C32(kBorder), 14.0f, 0, 1.0f);
  dl->AddLine(ImVec2(p0.x+16,p0.y+1), ImVec2(p1.x-16,p0.y+1),
              IM_COL32(255,255,255,6), 1.0f);

  ImGui::SetCursorPos(ImVec2(x, y));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(32.0f, 28.0f));
  ImGui::BeginChild(id, ImVec2(cardW, cardH), false);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  return true;
}

static void EndCenteredCard() { ImGui::EndChild(); }

// =========================================================================
// Screen: Main Menu
// =========================================================================

static void DrawMainMenuScreen(float winW, float winH) {
  const float kLeftW = winW * 0.44f;
  const float kRightW = winW - kLeftW;

  // ---- Left hero panel ------------------------------------------------
  ImGui::SetCursorPos(ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##mm_left", ImVec2(kLeftW, winH), false,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();

  // Vertical centering: place content at ~35% from top
  float topPad = winH * 0.28f;
  ImGui::Dummy(ImVec2(0, topPad));
  ImGui::SetCursorPosX(52.0f);

  PushMF(g_ManagerFontHero);
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::TextUnformatted("Classic");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontHero);

  ImGui::SetCursorPosX(52.0f);
  PushMF(g_ManagerFontHero);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Manager");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontHero);

  ImGui::Dummy(ImVec2(0, 14.0f));
  ImGui::SetCursorPosX(54.0f);
  PushMF(g_ManagerFontRegular);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("Build your career. Shape your club.");
  ImGui::PopStyleColor();
  ImGui::SetCursorPosX(54.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("Watch football unfold.");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontRegular);

  // Bottom version text
  float versionY = winH - 32.0f;
  ImGui::SetCursorPos(ImVec2(54.0f, versionY));
  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("Pre-alpha \xe2\x80\x94 Career Mode");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);

  ImGui::EndChild();

  // ---- Vertical accent line -------------------------------------------
  {
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(
      ImVec2(wp.x + kLeftW, wp.y + winH * 0.15f),
      ImVec2(wp.x + kLeftW, wp.y + winH * 0.85f),
      C32(kBorder), 1.0f);
  }

  // ---- Right action card ----------------------------------------------
  const float kCardW = 340.0f;
  const float kCardH = 320.0f;
  float cardX = kLeftW + (kRightW - kCardW) * 0.5f;
  float cardY = (winH - kCardH) * 0.5f;
  if (cardY < 10.0f) cardY = 10.0f;

  ImVec2 p0 = ImVec2(ImGui::GetWindowPos().x + cardX, ImGui::GetWindowPos().y + cardY);
  ImVec2 p1 = ImVec2(p0.x + kCardW, p0.y + kCardH);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, C32(kBgCard), 14.0f);
  dl->AddRect(p0, p1, C32(kBorder), 14.0f, 0, 1.0f);
  dl->AddLine(ImVec2(p0.x+16,p0.y+1), ImVec2(p1.x-16,p0.y+1),
              IM_COL32(255,255,255,6), 1.0f);

  ImGui::SetCursorPos(ImVec2(cardX, cardY));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(28.0f, 24.0f));
  ImGui::BeginChild("##mm_card", ImVec2(kCardW, kCardH), false);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("CHOOSE YOUR PATH");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::Dummy(ImVec2(0, 14.0f));

  float btnW = kCardW - 56.0f;
  const float kBtnH = 44.0f;

  PushMF(g_ManagerFontBold);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));

  if (CTABtn("New Career", ImVec2(btnW, kBtnH))) {
    printf("[IMGUI MAIN MENU] New Game requested\n");
    g_PreCareer.pendingAction = 1;
  }

  ImGui::Dummy(ImVec2(0, 6.0f));

  if (SecBtn("Load Career", ImVec2(btnW, kBtnH))) {
    printf("[IMGUI MAIN MENU] Load Game requested\n");
    g_PreCareer.pendingAction = 2;
  }

  ImGui::Dummy(ImVec2(0, 6.0f));

  if (SecBtn("Settings", ImVec2(btnW, kBtnH))) {
    printf("[IMGUI MAIN MENU] Settings requested\n");
    g_PreCareer.pendingAction = 3;
  }

  ImGui::Dummy(ImVec2(0, 6.0f));

  if (DangerBtn("Quit", ImVec2(btnW, kBtnH))) {
    printf("[IMGUI MAIN MENU] Quit requested\n");
    g_PreCareer.pendingAction = 4;
  }

  ImGui::PopStyleVar();
  PopMF(g_ManagerFontBold);

  ImGui::EndChild();
}

// =========================================================================
// Screen: Create Manager Profile
// =========================================================================

static const char *kNationalities[] = {
  "Portugal", "England", "Spain", "France", "Germany",
  "Italy", "Netherlands", "Brazil", "Argentina", "United States"
};
static const int kNationalityCount = 10;

static const char *kGenders[] = { "Male", "Female" };
static const int kGenderCount  = 2;

static void DrawCreateProfileScreen(float winW, float winH) {
  const float kCardW = 480.0f;
  const float kCardH = 520.0f;

  BeginCenteredCard("##cp_card", kCardW, kCardH, winW, winH);

  // Title
  PushMF(g_ManagerFontTitle);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Create Manager");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontTitle);

  ImGui::Dummy(ImVec2(0, 2.0f));
  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("Define your identity before the season begins.");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);

  ImGui::Dummy(ImVec2(0, 20.0f));

  float fW = kCardW - 64.0f; // field width

  // ---- Manager Name ---------------------------------------------------
  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("MANAGER NAME");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::SetNextItemWidth(fW);
  PushMF(g_ManagerFontRegular);
  ImGui::InputText("##mgr_name", g_PreCareer.nameBuffer,
                   sizeof(g_PreCareer.nameBuffer));
  PopMF(g_ManagerFontRegular);

  ImGui::Dummy(ImVec2(0, 12.0f));

  // ---- Manager Age ----------------------------------------------------
  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("AGE  (18 \xe2\x80\x93 99)");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::SetNextItemWidth(fW);
  PushMF(g_ManagerFontRegular);
  ImGui::InputText("##mgr_age", g_PreCareer.ageBuf,
                   sizeof(g_PreCareer.ageBuf),
                   ImGuiInputTextFlags_CharsDecimal);
  PopMF(g_ManagerFontRegular);

  ImGui::Dummy(ImVec2(0, 12.0f));

  // ---- Nationality ----------------------------------------------------
  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("NATIONALITY");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::SetNextItemWidth(fW);
  PushMF(g_ManagerFontRegular);
  ImGui::Combo("##mgr_nat", &g_PreCareer.nationalityIdx,
               kNationalities, kNationalityCount);
  PopMF(g_ManagerFontRegular);

  ImGui::Dummy(ImVec2(0, 12.0f));

  // ---- Gender ---------------------------------------------------------
  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("GENDER");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::SetNextItemWidth(fW);
  PushMF(g_ManagerFontRegular);
  ImGui::Combo("##mgr_gen", &g_PreCareer.genderIdx,
               kGenders, kGenderCount);
  PopMF(g_ManagerFontRegular);

  ImGui::Dummy(ImVec2(0, 24.0f));

  // ---- Buttons --------------------------------------------------------
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
  PushMF(g_ManagerFontBold);

  if (CTABtn("Create Manager Profile", ImVec2(fW, 44.0f)))
    g_PreCareer.pendingAction = 5;

  ImGui::Dummy(ImVec2(0, 6.0f));

  if (SecBtn("Back", ImVec2(fW, 36.0f))) {
    printf("[IMGUI CREATE MANAGER] Back requested\n");
    g_PreCareer.pendingAction = 6;
  }

  PopMF(g_ManagerFontBold);
  ImGui::PopStyleVar();

  EndCenteredCard();
}

static void DrawSettingsPlaceholderScreen(float winW, float winH) {
  const float kCardW = 520.0f;
  const float kCardH = 320.0f;

  BeginCenteredCard("##settings_card", kCardW, kCardH, winW, winH);

  PushMF(g_ManagerFontTitle);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Settings");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontTitle);

  ImGui::Dummy(ImVec2(0, 14.0f));

  PushMF(g_ManagerFontRegular);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("Settings screen coming soon.");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontRegular);

  ImGui::Dummy(ImVec2(0, 24.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
  PushMF(g_ManagerFontBold);
  if (SecBtn("Back", ImVec2(180.0f, 40.0f))) {
    printf("[IMGUI MAIN MENU] Settings Back requested\n");
    g_PreCareer.pendingAction = 6;
  }
  PopMF(g_ManagerFontBold);
  ImGui::PopStyleVar();

  EndCenteredCard();
}

// =========================================================================
// Screen: Load Game
// =========================================================================

static void DrawLoadGameScreen(float winW, float winH) {
  const float kCardW = 640.0f;
  const float kCardH = winH * 0.72f < 460.0f ? 460.0f : winH * 0.72f;

  BeginCenteredCard("##lg_card", kCardW, kCardH, winW, winH);

  // Header
  PushMF(g_ManagerFontTitle);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Load Career");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontTitle);

  ImGui::Dummy(ImVec2(0, 4.0f));
  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::Text("%d career%s found", (int)g_PreCareer.saves.size(),
              g_PreCareer.saves.size() == 1 ? "" : "s");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);

  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, 6.0f));

  // Save list
  float listH = kCardH - 160.0f;
  if (listH < 80.0f) listH = 80.0f;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##lg_scroll", ImVec2(0, listH), false);
  ImGui::PopStyleColor();

  if (g_PreCareer.saves.empty()) {
    ImGui::Dummy(ImVec2(0, 24.0f));
    PushMF(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No careers found.");
    ImGui::PopStyleColor();
    PopMF(g_ManagerFontBold);
    ImGui::Dummy(ImVec2(0, 6.0f));
    PushMF(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted("Start a new career to begin.");
    ImGui::PopStyleColor();
    PopMF(g_ManagerFontSmall);
  }

  float rowW = kCardW - 64.0f;
  for (unsigned int i = 0; i < g_PreCareer.saves.size(); i++) {
    const auto &s = g_PreCareer.saves.at(i);

    // Row card
    ImVec2 rp0 = ImGui::GetCursorScreenPos();
    const float kRowH = 64.0f;
    ImVec2 rp1 = ImVec2(rp0.x + rowW, rp0.y + kRowH);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    char rowId[32]; snprintf(rowId, sizeof(rowId), "##lg_row_%d", s.id);
    bool clicked = ImGui::InvisibleButton(rowId, ImVec2(rowW, kRowH));
    bool hov = ImGui::IsItemHovered();

    if (hov)
      dl->AddRectFilled(rp0, rp1, C32(kBgCardAlt), 8.0f);
    else
      dl->AddRectFilled(rp0, rp1, IM_COL32(18,28,52,120), 8.0f);
    dl->AddRect(rp0, rp1, C32(kBorder), 8.0f, 0, 0.8f);

    // Name
    ImFont *bf = g_ManagerFontBold ? g_ManagerFontBold : nullptr;
    if (bf) ImGui::PushFont(bf);
    float lh = ImGui::GetTextLineHeight();
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(rp0.x + 14.0f, rp0.y + (kRowH - lh * 2.2f) * 0.5f),
                C32(hov ? kAccent : kTextPri), s.name.c_str());
    if (bf) ImGui::PopFont();

    // Sub info
    ImFont *sf = g_ManagerFontSmall ? g_ManagerFontSmall : nullptr;
    char sub[128];
    snprintf(sub, sizeof(sub), "%s  \xe2\x80\xa2  %s  \xe2\x80\xa2  Age %d",
             s.clubName.empty() ? "No Club" : s.clubName.c_str(),
             s.nationality.empty() ? "" : s.nationality.c_str(),
             s.age);
    if (sf) ImGui::PushFont(sf);
    float slh = ImGui::GetTextLineHeight();
    float subY = rp0.y + (kRowH + lh * 0.2f) * 0.5f;
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(rp0.x + 14.0f, subY),
                C32(kTextSec), sub);
    if (sf) ImGui::PopFont();

    if (clicked) {
      g_PreCareer.pendingAction = 7;
      g_PreCareer.pendingId     = s.id;
    }

    ImGui::Dummy(ImVec2(0, 6.0f));
  }

  ImGui::EndChild();

  // Back button
  ImGui::Dummy(ImVec2(0, 10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
  PushMF(g_ManagerFontBold);
  if (SecBtn("Back", ImVec2(rowW, 38.0f)))
    g_PreCareer.pendingAction = 6;
  PopMF(g_ManagerFontBold);
  ImGui::PopStyleVar();

  EndCenteredCard();
}

// =========================================================================
// Screen: Select League
// =========================================================================

static void DrawSelectLeagueScreen(float winW, float winH) {
  const int kLeagueCount = (int)g_PreCareer.leagues.size();
  const float kRowH  = 52.0f;
  const float kCardW = 420.0f;
  float innerH = 80.0f + (float)kLeagueCount * (kRowH + 6.0f) + 60.0f;
  float kCardH = innerH < 280.0f ? 280.0f : (innerH > winH - 40.0f ? winH - 40.0f : innerH);

  BeginCenteredCard("##sl_card", kCardW, kCardH, winW, winH);

  PushMF(g_ManagerFontTitle);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Select League");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontTitle);

  ImGui::Dummy(ImVec2(0, 4.0f));
  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("Choose the competition you will manage in.");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, 8.0f));

  float itemW = kCardW - 64.0f;

  // Scrollable league list
  float listH = kCardH - 160.0f;
  if (listH < 60.0f) listH = 60.0f;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##sl_scroll", ImVec2(0, listH), false);
  ImGui::PopStyleColor();

  if (g_PreCareer.leagues.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No leagues found in database.");
    ImGui::PopStyleColor();
  }

  for (unsigned int i = 0; i < g_PreCareer.leagues.size(); i++) {
    const auto &lg = g_PreCareer.leagues.at(i);
    ImVec2 rp0 = ImGui::GetCursorScreenPos();
    ImVec2 rp1 = ImVec2(rp0.x + itemW, rp0.y + kRowH);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    char bid[32]; snprintf(bid, sizeof(bid), "##sl_lg_%d", lg.id);
    bool clicked = ImGui::InvisibleButton(bid, ImVec2(itemW, kRowH));
    bool hov = ImGui::IsItemHovered();

    dl->AddRectFilled(rp0, rp1,
      hov ? C32(kBgCardAlt) : IM_COL32(18,28,52,100), 8.0f);
    dl->AddRect(rp0, rp1, C32(kBorder), 8.0f, 0, 0.8f);
    if (hov) dl->AddRectFilled(rp0, ImVec2(rp0.x+3.0f, rp1.y), C32(kAccent), 2.0f);

    // League initial badge
    {
      std::string ini;
      for (unsigned int c = 0; c < lg.name.size() && (int)ini.size() < 2; c++)
        if (isupper((unsigned char)lg.name[c])) ini += lg.name[c];
      if (ini.empty() && !lg.name.empty()) ini = lg.name.substr(0, 2);
      unsigned int hash = 5381;
      for (char c : lg.name) hash = ((hash << 5) + hash) ^ (unsigned char)c;
      float hue = (float)(hash % 360) / 360.0f;
      float r, g, b2;
      ImGui::ColorConvertHSVtoRGB(hue, 0.50f, 0.45f, r, g, b2);
      float bsz = 28.0f;
      float bx = rp0.x + 10.0f;
      float by = rp0.y + (kRowH - bsz) * 0.5f;
      dl->AddRectFilled(ImVec2(bx,by), ImVec2(bx+bsz,by+bsz),
        IM_COL32((int)(r*255),(int)(g*255),(int)(b2*255),200), 5.0f);
      ImVec2 tsz = ImGui::CalcTextSize(ini.c_str());
      dl->AddText(ImVec2(bx+(bsz-tsz.x)*0.5f, by+(bsz-tsz.y)*0.5f),
                  IM_COL32(255,255,255,210), ini.c_str());
    }

    ImFont *bf = hov ? g_ManagerFontBold : g_ManagerFontRegular;
    if (bf) ImGui::PushFont(bf);
    float lh = ImGui::GetTextLineHeight();
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(rp0.x + 46.0f, rp0.y + (kRowH - lh) * 0.5f),
                C32(hov ? kTextPri : kTextSec), lg.name.c_str());
    if (bf) ImGui::PopFont();

    if (clicked) {
      g_PreCareer.pendingAction = 8;
      g_PreCareer.pendingId     = lg.id;
    }

    ImGui::Dummy(ImVec2(0, 6.0f));
  }

  ImGui::EndChild();

  ImGui::Dummy(ImVec2(0, 10.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
  PushMF(g_ManagerFontBold);
  if (SecBtn("Back", ImVec2(itemW, 38.0f)))
    g_PreCareer.pendingAction = 6;
  PopMF(g_ManagerFontBold);
  ImGui::PopStyleVar();

  EndCenteredCard();
}

// =========================================================================
// Screen: Select Club
// =========================================================================

static void DrawSelectClubScreen(float winW, float winH) {
  const int kClubCount = (int)g_PreCareer.clubs.size();
  const float kRowH    = 56.0f;
  const float kCardW   = 520.0f;
  float innerH = 100.0f + (float)kClubCount * (kRowH + 6.0f) + 110.0f;
  float kCardH = innerH < 320.0f ? 320.0f : (innerH > winH - 40.0f ? winH - 40.0f : innerH);

  BeginCenteredCard("##sc_card", kCardW, kCardH, winW, winH);

  PushMF(g_ManagerFontTitle);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Select Club");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontTitle);

  ImGui::Dummy(ImVec2(0, 4.0f));
  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("Click a club to select it, then press Start Career.");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, 8.0f));

  float itemW = kCardW - 64.0f;

  float listH = kCardH - 210.0f;
  if (listH < 60.0f) listH = 60.0f;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##sc_scroll", ImVec2(0, listH), false);
  ImGui::PopStyleColor();

  if (g_PreCareer.clubs.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No clubs found in this league.");
    ImGui::PopStyleColor();
  }

  for (unsigned int i = 0; i < g_PreCareer.clubs.size(); i++) {
    const auto &cl = g_PreCareer.clubs.at(i);
    bool sel = (g_PreCareer.selectedClubId == cl.id);

    ImVec2 rp0 = ImGui::GetCursorScreenPos();
    ImVec2 rp1 = ImVec2(rp0.x + itemW, rp0.y + kRowH);
    ImDrawList *dl = ImGui::GetWindowDrawList();

    char bid[32]; snprintf(bid, sizeof(bid), "##sc_cl_%d", cl.id);
    bool clicked = ImGui::InvisibleButton(bid, ImVec2(itemW, kRowH));
    bool hov = ImGui::IsItemHovered();

    if (sel)
      dl->AddRectFilled(rp0, rp1, IM_COL32(80,20,145,185), 8.0f);
    else if (hov)
      dl->AddRectFilled(rp0, rp1, C32(kBgCardAlt), 8.0f);
    else
      dl->AddRectFilled(rp0, rp1, IM_COL32(18,28,52,100), 8.0f);
    dl->AddRect(rp0, rp1, sel ? C32(kAccent) : C32(kBorder), 8.0f, 0, sel ? 1.5f : 0.8f);

    // Club badge
    const std::string &sn = cl.shortName.empty() ? cl.name : cl.shortName;
    {
      float bsz = 32.0f;
      float bx = rp0.x + 10.0f;
      float by = rp0.y + (kRowH - bsz) * 0.5f;
      unsigned int hash = 5381;
      for (char c : sn) hash = ((hash << 5) + hash) ^ (unsigned char)c;
      float hue = (float)(hash % 360) / 360.0f;
      float r, g, b;
      ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.48f, r, g, b);
      dl->AddRectFilled(ImVec2(bx,by), ImVec2(bx+bsz,by+bsz),
        IM_COL32((int)(r*255),(int)(g*255),(int)(b*255),215), bsz*0.22f);
      std::string ini;
      for (unsigned int c2 = 0; c2 < sn.size() && (int)ini.size() < 2; c2++)
        if (isalpha((unsigned char)sn[c2])) ini += (char)toupper((unsigned char)sn[c2]);
      if (!ini.empty()) {
        ImVec2 tsz = ImGui::CalcTextSize(ini.c_str());
        dl->AddText(ImVec2(bx+(bsz-tsz.x)*0.5f, by+(bsz-tsz.y)*0.5f),
                    IM_COL32(255,255,255,215), ini.c_str());
      }
    }

    // Full name
    ImFont *bf = sel ? g_ManagerFontBold : (hov ? g_ManagerFontBold : g_ManagerFontRegular);
    if (bf) ImGui::PushFont(bf);
    float lh = ImGui::GetTextLineHeight();
    ImVec4 nc = sel ? kAccent : (hov ? kTextPri : kTextSec);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(rp0.x + 50.0f, rp0.y + (kRowH - lh) * 0.5f),
                C32(nc), cl.name.c_str());
    if (bf) ImGui::PopFont();

    if (sel) {
      PushMF(g_ManagerFontSmall);
      ImVec2 tsz = ImGui::CalcTextSize("Selected");
      dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                  ImVec2(rp1.x - tsz.x - 12.0f, rp0.y + (kRowH - ImGui::GetTextLineHeight()) * 0.5f),
                  C32(kGold), "Selected");
      PopMF(g_ManagerFontSmall);
    }

    // Direct update — no deferred action needed (not a page transition)
    if (clicked) g_PreCareer.selectedClubId = cl.id;

    ImGui::Dummy(ImVec2(0, 6.0f));
  }

  ImGui::EndChild();
  ImGui::Dummy(ImVec2(0, 12.0f));

  // Buttons
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
  PushMF(g_ManagerFontBold);

  bool canStart = (g_PreCareer.selectedClubId > 0);
  if (!canStart) {
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.40f);
    SecBtn("Start Career", ImVec2(itemW, 44.0f));
    ImGui::PopStyleVar();
  } else {
    if (CTABtn("Start Career", ImVec2(itemW, 44.0f)))
      g_PreCareer.pendingAction = 10;
  }

  ImGui::Dummy(ImVec2(0, 6.0f));
  if (SecBtn("Back", ImVec2(itemW, 36.0f)))
    g_PreCareer.pendingAction = 6;

  PopMF(g_ManagerFontBold);
  ImGui::PopStyleVar();

  EndCenteredCard();
}

// =========================================================================
// Entry point
// =========================================================================

void RenderImGuiPreCareer() {
  if (!g_PreCareer.active || g_PreCareer.isTransitioning) return;

  ApplyPreCareerTheme();

  ImGuiIO &io = ImGui::GetIO();
  float winW = io.DisplaySize.x;
  float winH = io.DisplaySize.y;

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##precareer_root", nullptr,
               ImGuiWindowFlags_NoTitleBar          |
               ImGuiWindowFlags_NoResize            |
               ImGuiWindowFlags_NoMove              |
               ImGuiWindowFlags_NoCollapse          |
               ImGuiWindowFlags_NoBringToFrontOnFocus |
               ImGuiWindowFlags_NoScrollbar         |
               ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();

  DrawPreCareerBackground(winW, winH);

  switch (g_PreCareer.screen) {
    case PRECAREER_MAIN_MENU:
      DrawMainMenuScreen(winW, winH);
      break;
    case PRECAREER_CREATE_PROFILE:
      DrawCreateProfileScreen(winW, winH);
      break;
    case PRECAREER_LOAD_GAME:
      DrawLoadGameScreen(winW, winH);
      break;
    case PRECAREER_SELECT_LEAGUE:
      DrawSelectLeagueScreen(winW, winH);
      break;
    case PRECAREER_SELECT_CLUB:
      DrawSelectClubScreen(winW, winH);
      break;
    case PRECAREER_SETTINGS_PLACEHOLDER:
      DrawSettingsPlaceholderScreen(winW, winH);
      break;
    default:
      break;
  }

  ImGui::End();
  ProcessPreCareerPendingAction();
}
