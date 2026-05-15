#include "imgui_career.hpp"

#include "imgui.h"
#include <sstream>
#include <string>

#include "main.hpp"
#include "utils/database.hpp"
#include "base/utils.hpp"

CareerHubState g_CareerHub;

// ---- DB helper ----------------------------------------------------------

static std::string DBCell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size() || col >= r->data.at(row).size()) return "";
  return r->data.at(row).at(col);
}

// ---- State management ---------------------------------------------------

void CareerHubState::Clear() {
  active        = false;
  activeTab     = 0;
  pendingAction = 0;
  onPlayMatch   = nullptr;
  onMainMenu    = nullptr;
  manager = {};
  club    = {};
  players.clear();
  fixtures.clear();
  standings.clear();
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
    q << "SELECT name, shortname FROM teams WHERE id = " << clubId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str());
    club.name      = DBCell(r, 0, 0);
    club.shortName = DBCell(r, 0, 1);
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
      << " home.shortname, away.shortname, fixtures.status,"
      << " fixtures.home_score, fixtures.away_score"
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
      f.status   = DBCell(r, i, 5);
      std::string hs = DBCell(r, i, 6);
      std::string as = DBCell(r, i, 7);
      if (f.status != "scheduled" && !hs.empty())
        f.score = hs + " - " + as;
      else
        f.score = "-";
      fixtures.push_back(f);
    }
    delete r;
  }

  standings.clear();
  {
    std::stringstream q;
    q << "SELECT leagues.name, teams.shortname,"
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
      s.league = DBCell(r, i, 0);
      s.team   = DBCell(r, i, 1);
      s.p      = DBCell(r, i, 2);
      s.w      = DBCell(r, i, 3);
      s.d      = DBCell(r, i, 4);
      s.l      = DBCell(r, i, 5);
      s.gf     = DBCell(r, i, 6);
      s.ga     = DBCell(r, i, 7);
      s.gd     = DBCell(r, i, 8);
      s.pts    = DBCell(r, i, 9);
      standings.push_back(s);
    }
    delete r;
  }

  active = true;
}

// ---- Color palette ------------------------------------------------------

static const ImVec4 kBgMain       = ImVec4(0.024f, 0.039f, 0.071f, 1.00f); // #060A12
static const ImVec4 kBgHeader     = ImVec4(0.031f, 0.051f, 0.094f, 1.00f); // #080D18
static const ImVec4 kBgSubnav     = ImVec4(0.020f, 0.035f, 0.067f, 1.00f); // #050911
static const ImVec4 kBgCard       = ImVec4(0.051f, 0.078f, 0.141f, 1.00f); // #0D1424
static const ImVec4 kBgCardAlt    = ImVec4(0.039f, 0.059f, 0.110f, 1.00f); // #0A0F1C
static const ImVec4 kBorder       = ImVec4(0.102f, 0.149f, 0.259f, 0.70f); // #1A2642
static const ImVec4 kAccentRed    = ImVec4(0.882f, 0.114f, 0.282f, 1.00f); // #E11D48
static const ImVec4 kAccentRedH   = ImVec4(0.941f, 0.247f, 0.380f, 1.00f);
static const ImVec4 kAccentRedA   = ImVec4(0.741f, 0.071f, 0.208f, 1.00f);
static const ImVec4 kAccentViolet = ImVec4(0.482f, 0.231f, 0.929f, 1.00f); // #7C3AED
static const ImVec4 kAccentVioletH= ImVec4(0.580f, 0.330f, 0.970f, 1.00f);
static const ImVec4 kAccentVioletA= ImVec4(0.380f, 0.160f, 0.800f, 1.00f);
static const ImVec4 kTextPrimary  = ImVec4(0.941f, 0.953f, 0.969f, 1.00f); // #F0F3F7
static const ImVec4 kTextMuted    = ImVec4(0.408f, 0.467f, 0.549f, 1.00f); // #687588
static const ImVec4 kTextSubtle   = ImVec4(0.200f, 0.251f, 0.349f, 1.00f); // #334059
static const ImVec4 kGreen        = ImVec4(0.133f, 0.773f, 0.369f, 1.00f); // #22C55E
static const ImVec4 kGold         = ImVec4(0.992f, 0.820f, 0.110f, 1.00f); // #FDCF1C
static const ImVec4 kBlue         = ImVec4(0.361f, 0.682f, 0.941f, 1.00f); // #5CAEF0

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
  st.ItemSpacing       = ImVec2(10.0f, 8.0f);
  st.ItemInnerSpacing  = ImVec2(6.0f, 4.0f);
  st.CellPadding       = ImVec2(8.0f, 5.0f);
  st.ScrollbarSize     = 7.0f;
  st.IndentSpacing     = 16.0f;

  ImVec4 *c = st.Colors;
  c[ImGuiCol_WindowBg]              = kBgMain;
  c[ImGuiCol_ChildBg]               = kBgCard;
  c[ImGuiCol_PopupBg]               = kBgCard;
  c[ImGuiCol_Border]                = kBorder;
  c[ImGuiCol_BorderShadow]          = ImVec4(0,0,0,0);
  c[ImGuiCol_FrameBg]               = ImVec4(0.063f, 0.094f, 0.169f, 1.00f);
  c[ImGuiCol_FrameBgHovered]        = ImVec4(0.094f, 0.137f, 0.239f, 1.00f);
  c[ImGuiCol_FrameBgActive]         = ImVec4(0.125f, 0.180f, 0.306f, 1.00f);
  c[ImGuiCol_TitleBg]               = kBgHeader;
  c[ImGuiCol_TitleBgActive]         = kBgHeader;
  c[ImGuiCol_TitleBgCollapsed]      = kBgHeader;
  c[ImGuiCol_MenuBarBg]             = kBgHeader;
  c[ImGuiCol_ScrollbarBg]           = kBgMain;
  c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.125f, 0.180f, 0.306f, 1.00f);
  c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.188f, 0.255f, 0.400f, 1.00f);
  c[ImGuiCol_ScrollbarGrabActive]   = kAccentViolet;
  c[ImGuiCol_CheckMark]             = kAccentRed;
  c[ImGuiCol_SliderGrab]            = kAccentViolet;
  c[ImGuiCol_SliderGrabActive]      = kAccentVioletH;
  c[ImGuiCol_Button]                = ImVec4(0.063f, 0.094f, 0.169f, 1.00f);
  c[ImGuiCol_ButtonHovered]         = ImVec4(0.094f, 0.137f, 0.239f, 1.00f);
  c[ImGuiCol_ButtonActive]          = ImVec4(0.125f, 0.180f, 0.306f, 1.00f);
  c[ImGuiCol_Header]                = ImVec4(0.094f, 0.137f, 0.239f, 0.60f);
  c[ImGuiCol_HeaderHovered]         = ImVec4(0.125f, 0.180f, 0.306f, 0.80f);
  c[ImGuiCol_HeaderActive]          = ImVec4(0.188f, 0.255f, 0.400f, 1.00f);
  c[ImGuiCol_Separator]             = kBorder;
  c[ImGuiCol_SeparatorHovered]      = kAccentViolet;
  c[ImGuiCol_SeparatorActive]       = kAccentViolet;
  c[ImGuiCol_ResizeGrip]            = ImVec4(0,0,0,0);
  c[ImGuiCol_ResizeGripHovered]     = ImVec4(0,0,0,0);
  c[ImGuiCol_ResizeGripActive]      = ImVec4(0,0,0,0);
  c[ImGuiCol_TableHeaderBg]         = kBgSubnav;
  c[ImGuiCol_TableBorderStrong]     = kBorder;
  c[ImGuiCol_TableBorderLight]      = ImVec4(0.063f, 0.094f, 0.169f, 1.00f);
  c[ImGuiCol_TableRowBg]            = ImVec4(0,0,0,0);
  c[ImGuiCol_TableRowBgAlt]         = ImVec4(0.039f, 0.059f, 0.110f, 0.55f);
  c[ImGuiCol_Text]                  = kTextPrimary;
  c[ImGuiCol_TextDisabled]          = kTextMuted;
  c[ImGuiCol_NavHighlight]          = kAccentRed;
  c[ImGuiCol_NavWindowingHighlight] = kAccentViolet;
  c[ImGuiCol_NavWindowingDimBg]     = ImVec4(0,0,0,0.50f);
  c[ImGuiCol_ModalWindowDimBg]      = ImVec4(0,0,0,0.50f);
}

// ---- Widget helpers -----------------------------------------------------

// Card: bordered child window with optional title. Always call EndCard().
static void BeginCard(const char *id, ImVec2 size, const char *title = nullptr) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgCard);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14.0f, 12.0f));
  ImGui::BeginChild(id, size, true);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
  if (title) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::TextUnformatted(title);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
}

static void EndCard() { ImGui::EndChild(); }

static bool CTAButton(const char *label, ImVec2 sz = ImVec2(0, 0)) {
  ImGui::PushStyleColor(ImGuiCol_Button,        kAccentRed);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentRedH);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kAccentRedA);
  bool r = ImGui::Button(label, sz);
  ImGui::PopStyleColor(3);
  return r;
}

static bool SecondaryButton(const char *label, ImVec2 sz = ImVec2(0, 0)) {
  ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.063f, 0.094f, 0.169f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.094f, 0.137f, 0.239f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.125f, 0.180f, 0.306f, 1.00f));
  bool r = ImGui::Button(label, sz);
  ImGui::PopStyleColor(3);
  return r;
}

static bool TabBtn(const char *label, bool active, ImVec2 sz = ImVec2(100.0f, 30.0f)) {
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button,        kAccentViolet);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentVioletH);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kAccentVioletA);
    ImGui::PushStyleColor(ImGuiCol_Text,          kTextPrimary);
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.094f, 0.137f, 0.239f, 0.70f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.125f, 0.180f, 0.306f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text,          kTextMuted);
  }
  bool r = ImGui::Button(label, sz);
  ImGui::PopStyleColor(4);
  return r;
}

static bool GhostNavBtn(const char *label, bool active) {
  if (active) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.094f,0.137f,0.239f,0.50f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.125f,0.180f,0.306f,0.70f));
    ImGui::PushStyleColor(ImGuiCol_Text,          kAccentRed);
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.094f,0.137f,0.239f,0.50f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.125f,0.180f,0.306f,0.70f));
    ImGui::PushStyleColor(ImGuiCol_Text,          kTextMuted);
  }
  bool r = ImGui::Button(label, ImVec2(88.0f, 0.0f));
  ImGui::PopStyleColor(4);
  return r;
}

// ---- Bar heights --------------------------------------------------------

static const float kHeaderH = 54.0f;
static const float kSubnavH = 44.0f;

// ---- Top header ---------------------------------------------------------

static void RenderTopHeader(float winW) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgHeader);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 0.0f));
  ImGui::BeginChild("##hdr", ImVec2(0, kHeaderH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  float midTextY = (kHeaderH - ImGui::GetTextLineHeight()) * 0.5f;
  float midBtnY  = (kHeaderH - 28.0f) * 0.5f;

  // Brand
  ImGui::SetCursorPos(ImVec2(16.0f, midTextY));
  ImGui::PushStyleColor(ImGuiCol_Text, kAccentRed);
  ImGui::TextUnformatted("CM");
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 8);
  ImGui::SetCursorPosY(midTextY);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSubtle);
  ImGui::TextUnformatted("|");
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 8);
  ImGui::SetCursorPosY(midTextY);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
  const std::string &cname = g_CareerHub.club.name.empty() ? "Career" : g_CareerHub.club.name;
  ImGui::TextUnformatted(cname.c_str());
  ImGui::PopStyleColor();

  // Centered global navigation (Portal active, others placeholder)
  static const char *kNavLabels[] = { "Portal", "Squad", "Recruitment", "Match Day", "Club", "Career" };
  const int kNumNav = 6;
  const float kNavBtnW = 88.0f;
  const float kNavSpacing = 2.0f;
  float totalNavW = kNumNav * kNavBtnW + (kNumNav - 1) * kNavSpacing;
  float navStartX = (winW - totalNavW) * 0.5f;

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(kNavSpacing, 0.0f));
  ImGui::SetCursorPos(ImVec2(navStartX, midBtnY));
  for (int i = 0; i < kNumNav; i++) {
    if (i > 0) ImGui::SameLine(0, kNavSpacing);
    GhostNavBtn(kNavLabels[i], i == 0); // only Portal highlighted
  }
  ImGui::PopStyleVar(2);

  // Continue button (right side, CTA, placeholder)
  const float kContW = 110.0f;
  ImGui::SetCursorPos(ImVec2(winW - kContW - 16.0f, midBtnY));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 6.0f));
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

// ---- Sub-navigation -----------------------------------------------------

static bool s_playClicked = false;
static bool s_menuClicked = false;

static void RenderSubNavigation(float winW) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSubnav);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f, 0.0f));
  ImGui::BeginChild("##subnav", ImVec2(0, kSubnavH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  float btnY = (kSubnavH - 30.0f) * 0.5f;

  static const char *kTabLabels[] = { "Overview", "Manager", "Club", "Matches", "Standings" };
  const int kNumTabs = 5;
  const float kTabW = 100.0f;
  const float kTabSpacing = 4.0f;

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(kTabSpacing, 0.0f));
  ImGui::SetCursorPos(ImVec2(16.0f, btnY));
  for (int i = 0; i < kNumTabs; i++) {
    if (i > 0) ImGui::SameLine(0, kTabSpacing);
    if (TabBtn(kTabLabels[i], g_CareerHub.activeTab == i, ImVec2(kTabW, 30.0f)))
      g_CareerHub.activeTab = i;
  }
  ImGui::PopStyleVar(2);

  // Right: Play Match (CTA) + Main Menu (secondary)
  const float kPlayW = 120.0f;
  const float kMenuW = 100.0f;
  float rightX = winW - kPlayW - kMenuW - kTabSpacing - 16.0f;
  ImGui::SetCursorPos(ImVec2(rightX, btnY));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 6.0f));
  if (CTAButton("Play Match", ImVec2(kPlayW, 30.0f)))     s_playClicked = true;
  ImGui::SameLine(0, kTabSpacing);
  if (SecondaryButton("Main Menu", ImVec2(kMenuW, 30.0f))) s_menuClicked = true;
  ImGui::PopStyleVar();

  ImGui::EndChild();

  // 1px separator
  ImVec2 sp = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(
    sp, ImVec2(sp.x + winW, sp.y + 1.0f),
    ImGui::ColorConvertFloat4ToU32(kBorder));
  ImGui::Dummy(ImVec2(0, 1.0f));
}

// ---- Overview cards -----------------------------------------------------

static void RenderMessagesCard(ImVec2 sz) {
  static const struct { const char *from; const char *subject; const char *when; } kMsgs[] = {
    { "Board",   "Pre-season objectives confirmed",  "Today"     },
    { "Media",   "Press conference scheduled",        "Yesterday" },
    { "Staff",   "Fitness report available",           "2d ago"    },
    { "Board",   "Transfer budget allocated",          "3d ago"    },
    { "Fans",    "Season ticket renewals open",        "4d ago"    },
    { "Staff",   "Training schedule published",        "5d ago"    },
    { "Media",   "Interview request pending",          "6d ago"    },
    { "Board",   "End of season review reminder",      "7d ago"    },
  };
  const int kN = 8;

  BeginCard("##msgs", sz, "MESSAGES");
  float tableH = sz.y - 64.0f;
  if (tableH < 40.0f) tableH = 40.0f;

  if (ImGui::BeginTable("##msgtbl", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_PadOuterX,
                        ImVec2(0, tableH))) {
    ImGui::TableSetupColumn("From",    ImGuiTableColumnFlags_WidthFixed,  72.0f);
    ImGui::TableSetupColumn("Subject", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("When",    ImGuiTableColumnFlags_WidthFixed,  64.0f);
    for (int i = 0; i < kN; i++) {
      ImGui::TableNextRow(0, 28.0f);
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kAccentViolet);
      ImGui::TextUnformatted(kMsgs[i].from);
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(kMsgs[i].subject);
      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSubtle);
      ImGui::TextUnformatted(kMsgs[i].when);
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }
  EndCard();
}

static void RenderNewsCard(ImVec2 sz) {
  BeginCard("##news", sz, "TOP STORY");
  const std::string cname = g_CareerHub.club.name.empty() ? "Your Club" : g_CareerHub.club.name;
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPrimary);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 3));
  ImGui::TextWrapped("%s linked with new signing as transfer window opens", cname.c_str());
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
  ImGui::TextWrapped("Pre-season activity expected as clubs finalise squads. Several deals could be confirmed within the coming days.");
  ImGui::PopStyleColor();
  ImGui::Spacing();
  // Story dots
  for (int i = 0; i < 4; i++) {
    if (i > 0) ImGui::SameLine(0, 6);
    ImVec2 cp = ImGui::GetCursorScreenPos();
    ImVec4 col = (i == 0) ? kAccentRed : kTextSubtle;
    ImGui::GetWindowDrawList()->AddCircleFilled(
      ImVec2(cp.x + 5, cp.y + 7), 4.0f, ImGui::ColorConvertFloat4ToU32(col));
    ImGui::Dummy(ImVec2(10, 14));
  }
  EndCard();
}

static void RenderNextFixtureCard(ImVec2 sz) {
  BeginCard("##nxtfix", sz, "NEXT FIXTURE");

  const std::string &sn = g_CareerHub.club.shortName;
  const CareerHubState::Fixture *f = nullptr;
  for (const auto &x : g_CareerHub.fixtures) {
    if (x.status == "scheduled" && (x.home == sn || x.away == sn)) { f = &x; break; }
  }
  if (!f) {
    for (const auto &x : g_CareerHub.fixtures) {
      if (x.home == sn || x.away == sn) { f = &x; break; }
    }
  }

  if (f) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
    ImGui::Text("%s  \xe2\x80\xa2  MD %s", f->league.c_str(), f->matchday.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();

    float avail = ImGui::GetContentRegionAvail().x;
    float teamW = (avail - 36.0f) * 0.5f;
    if (teamW < 40.0f) teamW = 40.0f;

    // Home team box
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgCardAlt);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::BeginChild("##nf_home", ImVec2(teamW, 50), true);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::TextUnformatted("HOME");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPrimary);
    ImGui::TextUnformatted(f->home.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();

    ImGui::SameLine(0, 4);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 16);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccentRed);
    ImGui::TextUnformatted("vs");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 4);
    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 16);

    // Away team box
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgCardAlt);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10.0f, 6.0f));
    ImGui::BeginChild("##nf_away", ImVec2(teamW, 50), true);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::TextUnformatted("AWAY");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPrimary);
    ImGui::TextUnformatted(f->away.c_str());
    ImGui::PopStyleColor();
    ImGui::EndChild();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::TextUnformatted("No fixtures scheduled.");
    ImGui::PopStyleColor();
  }
  EndCard();
}

static void RenderCalendarCard(ImVec2 sz) {
  BeginCard("##cal", sz, "CALENDAR");

  ImDrawList *dl   = ImGui::GetWindowDrawList();
  float avail      = ImGui::GetContentRegionAvail().x;
  const int kCols  = 7;
  const float kGap = 2.0f;
  float cellW      = (avail - kGap * (kCols - 1)) / kCols;
  float cellH      = 24.0f;

  static const char *kDayNames[] = { "M", "T", "W", "T", "F", "S", "S" };

  // Day name header row drawn via DrawList
  ImVec2 cur = ImGui::GetCursorScreenPos();
  for (int d = 0; d < kCols; d++) {
    float x = cur.x + d * (cellW + kGap);
    ImVec4 col = (d >= 5) ? kAccentRed : kTextMuted;
    float tw = ImGui::CalcTextSize(kDayNames[d]).x;
    dl->AddText(ImVec2(x + (cellW - tw) * 0.5f, cur.y + 3),
                ImGui::ColorConvertFloat4ToU32(col), kDayNames[d]);
  }
  ImGui::Dummy(ImVec2(avail, cellH));

  // 5 weeks of day cells drawn via DrawList (no child windows needed)
  int dayNum = 1;
  for (int week = 0; week < 5 && dayNum <= 30; week++) {
    cur = ImGui::GetCursorScreenPos();
    int daysThisRow = 0;
    for (int col = 0; col < kCols && dayNum <= 30; col++) {
      float x    = cur.x + col * (cellW + kGap);
      bool isMD  = (dayNum == 4 || dayNum == 11 || dayNum == 18 || dayNum == 25);
      bool isWkd = (col >= 5);

      ImVec4 bgCol = isMD ? ImVec4(0.25f, 0.09f, 0.50f, 0.65f) : kBgCardAlt;
      dl->AddRectFilled(ImVec2(x, cur.y), ImVec2(x + cellW, cur.y + cellH),
                        ImGui::ColorConvertFloat4ToU32(bgCol), 4.0f);

      char buf[4];
      snprintf(buf, sizeof(buf), "%d", dayNum);
      float tw = ImGui::CalcTextSize(buf).x;
      ImVec4 tc = isMD ? kAccentRed : (isWkd ? kTextMuted : kTextPrimary);
      dl->AddText(ImVec2(x + (cellW - tw) * 0.5f, cur.y + 4),
                  ImGui::ColorConvertFloat4ToU32(tc), buf);
      dayNum++;
      daysThisRow++;
    }
    ImGui::Dummy(ImVec2(avail, cellH + kGap));
  }

  EndCard();
}

static void RenderFixtureScheduleCard(ImVec2 sz) {
  BeginCard("##fixsched", sz, "FIXTURE SCHEDULE");

  const std::string &sn = g_CareerHub.club.shortName;
  int shown = 0;

  float tableH = sz.y - 58.0f;
  if (tableH < 40.0f) tableH = 40.0f;

  if (ImGui::BeginTable("##fstbl", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_PadOuterX,
                        ImVec2(0, tableH))) {
    ImGui::TableSetupColumn("Opponent",    ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Comp",        ImGuiTableColumnFlags_WidthFixed, 56.0f);
    ImGui::TableSetupColumn("H/A",         ImGuiTableColumnFlags_WidthFixed, 26.0f);
    ImGui::TableSetupColumn("Score",       ImGuiTableColumnFlags_WidthFixed, 58.0f);

    for (const auto &f : g_CareerHub.fixtures) {
      if (f.home != sn && f.away != sn) continue;
      if (shown >= 8) break;

      bool isHome = (f.home == sn);
      const std::string &opp = isHome ? f.away : f.home;
      std::string comp = f.league.size() > 7 ? f.league.substr(0, 7) : f.league;

      ImGui::TableNextRow(0, 26.0f);
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(opp.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
      ImGui::TextUnformatted(comp.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, isHome ? kGreen : kTextMuted);
      ImGui::TextUnformatted(isHome ? "H" : "A");
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(3);
      if (f.score != "-" && !f.score.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
        ImGui::TextUnformatted(f.score.c_str());
        ImGui::PopStyleColor();
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSubtle);
        ImGui::TextUnformatted("\xe2\x80\x94"); // em dash
        ImGui::PopStyleColor();
      }
      shown++;
    }

    if (shown == 0) {
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
      ImGui::TextUnformatted("No fixtures found.");
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }
  EndCard();
}

static void RenderCompetitionCard(ImVec2 sz) {
  BeginCard("##comp", sz, "COMPETITION");
  const std::string &sn = g_CareerHub.club.shortName;
  std::string league;
  for (const auto &f : g_CareerHub.fixtures) {
    if (f.home == sn || f.away == sn) { league = f.league; break; }
  }
  if (!league.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
    ImGui::TextUnformatted(league.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::TextUnformatted("Group Stage");
    ImGui::PopStyleColor();
  } else {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::TextUnformatted("No competition data.");
    ImGui::PopStyleColor();
  }
  EndCard();
}

// ---- Overview tab -------------------------------------------------------

static void RenderOverviewTab(float w, float h) {
  const float kPad  = 14.0f;
  const float kGap  = 10.0f;
  float colW = (w - kPad * 2.0f - kGap * 2.0f) / 3.0f;
  float colH = h - 14.0f;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));

  // Left: Messages
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_l", ImVec2(colW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  RenderMessagesCard(ImVec2(colW, colH));
  ImGui::EndChild();

  ImGui::SameLine(0, kGap);

  // Center: News + Next Fixture + Calendar
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_c", ImVec2(colW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  float newsH = colH * 0.26f;
  float fixH  = colH * 0.24f;
  float calH  = colH - newsH - fixH - kGap * 2.0f;
  RenderNewsCard(ImVec2(colW, newsH));
  ImGui::Dummy(ImVec2(0, kGap));
  RenderNextFixtureCard(ImVec2(colW, fixH));
  ImGui::Dummy(ImVec2(0, kGap));
  RenderCalendarCard(ImVec2(colW, calH));
  ImGui::EndChild();

  ImGui::SameLine(0, kGap);

  // Right: Fixture Schedule + Competition
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_r", ImVec2(colW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  float schedH = colH * 0.68f;
  float compH  = colH - schedH - kGap;
  RenderFixtureScheduleCard(ImVec2(colW, schedH));
  ImGui::Dummy(ImVec2(0, kGap));
  RenderCompetitionCard(ImVec2(colW, compH));
  ImGui::EndChild();
}

// ---- Manager tab --------------------------------------------------------

static void RenderManagerTab(float w, float h) {
  const float kPad = 14.0f;
  const float kGap = 10.0f;
  float leftW  = w * 0.36f - kPad;
  float rightW = w - leftW - kGap - kPad * 2.0f;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##mgr_l", ImVec2(leftW, h - 12.0f), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();

  BeginCard("##mgr_profile", ImVec2(leftW, 230.0f), "MANAGER PROFILE");
  ImGui::Spacing();
  const auto &m = g_CareerHub.manager;
  auto profRow = [](const char *lbl, const std::string &val) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::Text("%-14s", lbl);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPrimary);
    ImGui::TextUnformatted(val.empty() ? "\xe2\x80\x94" : val.c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
  };
  profRow("Name",        m.name);
  profRow("Age",         m.age);
  profRow("Nationality", m.nationality);
  profRow("Gender",      m.gender);
  profRow("Club",        m.clubName);
  EndCard();

  ImGui::EndChild();
  ImGui::SameLine(0, kGap);

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##mgr_r", ImVec2(rightW, h - 12.0f), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();

  float cardH = (h - 12.0f - kGap * 2.0f) / 3.0f;
  if (cardH < 60.0f) cardH = 60.0f;

  BeginCard("##mgr_summary", ImVec2(rightW, cardH), "CAREER SUMMARY");
  ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
  ImGui::TextUnformatted("No previous clubs.");
  ImGui::TextUnformatted("First season as a manager.");
  ImGui::PopStyleColor();
  EndCard();

  ImGui::Dummy(ImVec2(0, kGap));

  BeginCard("##mgr_job", ImVec2(rightW, cardH), "CURRENT JOB");
  auto jobRow = [](const char *lbl, const char *val) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::Text("%-10s", lbl);
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPrimary);
    ImGui::TextUnformatted(val);
    ImGui::PopStyleColor();
    ImGui::Spacing();
  };
  jobRow("Club",   m.clubName.empty() ? "\xe2\x80\x94" : m.clubName.c_str());
  jobRow("Season", "2026/27");
  EndCard();

  ImGui::Dummy(ImVec2(0, kGap));

  BeginCard("##mgr_objectives", ImVec2(rightW, cardH), "OBJECTIVES");
  ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
  ImGui::TextUnformatted("Objectives will be set by the board.");
  ImGui::TextUnformatted("Check back at the start of the season.");
  ImGui::PopStyleColor();
  EndCard();

  ImGui::EndChild();
}

// ---- Club tab -----------------------------------------------------------

static void RenderClubTab(float w, float h) {
  const float kPad = 14.0f;
  const float kGap = 10.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usableW = w - kPad * 2.0f;

  // Club header card
  float hdrH = 70.0f;
  BeginCard("##club_hdr", ImVec2(usableW, hdrH));
  const auto &cl = g_CareerHub.club;
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPrimary);
  ImGui::SetWindowFontScale(1.15f);
  ImGui::TextUnformatted(cl.name.empty() ? "\xe2\x80\x94" : cl.name.c_str());
  ImGui::SetWindowFontScale(1.0f);
  ImGui::PopStyleColor();
  if (!cl.shortName.empty()) {
    ImGui::SameLine(0, 14);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSubtle);
    ImGui::TextUnformatted("|");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 14);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::TextUnformatted(cl.shortName.c_str());
    ImGui::PopStyleColor();
  }
  EndCard();

  ImGui::Dummy(ImVec2(0, kGap));

  float squadCardH = h - hdrH - kGap - 8.0f - 12.0f;
  if (squadCardH < 80.0f) squadCardH = 80.0f;
  BeginCard("##club_squad", ImVec2(usableW, squadCardH), "SQUAD");

  float tableH = squadCardH - 58.0f;
  if (tableH < 40.0f) tableH = 40.0f;

  if (ImGui::BeginTable("##squadtbl", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX |
                        ImGuiTableFlags_SizingStretchProp,
                        ImVec2(0, tableH))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Role",    ImGuiTableColumnFlags_WidthFixed, 160.0f);
    ImGui::TableSetupColumn("Age",     ImGuiTableColumnFlags_WidthFixed,  44.0f);
    ImGui::TableSetupColumn("Ability", ImGuiTableColumnFlags_WidthFixed,  60.0f);
    ImGui::TableHeadersRow();

    for (const auto &p : g_CareerHub.players) {
      ImGui::TableNextRow(0, 26.0f);
      ImGui::TableSetColumnIndex(0);
      ImGui::Text("%s %s", p.firstName.c_str(), p.lastName.c_str());
      ImGui::TableSetColumnIndex(1);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
      ImGui::TextUnformatted(p.role.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
      ImGui::TextUnformatted(p.age.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(p.ability.c_str());
    }
    ImGui::EndTable();
  }
  EndCard();
}

// ---- Matches tab --------------------------------------------------------

static void RenderMatchesTab(float w, float h) {
  const float kPad = 14.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usableW = w - kPad * 2.0f;
  float usableH = h - 20.0f;

  BeginCard("##matches_outer", ImVec2(usableW, usableH), "FIXTURES");

  // Collect league order
  std::vector<std::string> leagueOrder;
  for (const auto &f : g_CareerHub.fixtures) {
    bool found = false;
    for (const auto &l : leagueOrder) if (l == f.league) { found = true; break; }
    if (!found) leagueOrder.push_back(f.league);
  }

  const std::string &sn = g_CareerHub.club.shortName;

  ImGui::BeginChild("##matches_scroll", ImVec2(0, usableH - 58.0f), false);

  for (unsigned int li = 0; li < leagueOrder.size(); li++) {
    const std::string &league = leagueOrder.at(li);
    if (li > 0) { ImGui::Spacing(); ImGui::Spacing(); }

    ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
    ImGui::Text("  %s", league.c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    std::string tblId = "##fix_" + league;
    if (ImGui::BeginTable(tblId.c_str(), 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX,
                          ImVec2(0, 0))) {
      ImGui::TableSetupColumn("MD",     ImGuiTableColumnFlags_WidthFixed,  36.0f);
      ImGui::TableSetupColumn("Rd",     ImGuiTableColumnFlags_WidthFixed,  36.0f);
      ImGui::TableSetupColumn("Home",   ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Away",   ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,  88.0f);
      ImGui::TableSetupColumn("Score",  ImGuiTableColumnFlags_WidthFixed,  66.0f);
      ImGui::TableHeadersRow();

      for (const auto &f : g_CareerHub.fixtures) {
        if (f.league != league) continue;
        bool myGame = (f.home == sn || f.away == sn);

        ImGui::TableNextRow(0, 26.0f);
        if (myGame) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.09f, 0.50f, 0.22f)));
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.09f, 0.50f, 0.30f)));
        }
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
        ImGui::TextUnformatted(f.matchday.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
        ImGui::TextUnformatted(f.round.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(f.home.c_str());
        ImGui::TableSetColumnIndex(3);
        ImGui::TextUnformatted(f.away.c_str());
        ImGui::TableSetColumnIndex(4);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSubtle);
        ImGui::TextUnformatted(f.status.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(5);
        if (f.score != "-" && !f.score.empty()) {
          ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
          ImGui::TextUnformatted(f.score.c_str());
          ImGui::PopStyleColor();
        } else {
          ImGui::PushStyleColor(ImGuiCol_Text, kTextSubtle);
          ImGui::TextUnformatted("\xe2\x80\x94");
          ImGui::PopStyleColor();
        }
      }
      ImGui::EndTable();
    }
  }

  if (g_CareerHub.fixtures.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::TextUnformatted("No fixtures generated yet.");
    ImGui::PopStyleColor();
  }

  ImGui::EndChild();
  EndCard();
}

// ---- Standings tab ------------------------------------------------------

static void RenderStandingsTab(float w, float h) {
  const float kPad = 14.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usableW = w - kPad * 2.0f;
  float usableH = h - 20.0f;

  BeginCard("##standings_outer", ImVec2(usableW, usableH), "LEAGUE STANDINGS");

  // Collect league order
  std::vector<std::string> leagueOrder;
  for (const auto &s : g_CareerHub.standings) {
    bool found = false;
    for (const auto &l : leagueOrder) if (l == s.league) { found = true; break; }
    if (!found) leagueOrder.push_back(s.league);
  }

  const std::string &sn = g_CareerHub.club.shortName;

  ImGui::BeginChild("##standings_scroll", ImVec2(0, usableH - 58.0f), false);

  for (unsigned int li = 0; li < leagueOrder.size(); li++) {
    const std::string &league = leagueOrder.at(li);
    if (li > 0) { ImGui::Spacing(); ImGui::Spacing(); }

    ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
    ImGui::Text("  %s", league.c_str());
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();

    std::string tblId = "##std_" + league;
    if (ImGui::BeginTable(tblId.c_str(), 9,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_PadOuterX,
                          ImVec2(0, 0))) {
      ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("P",    ImGuiTableColumnFlags_WidthFixed, 30.0f);
      ImGui::TableSetupColumn("W",    ImGuiTableColumnFlags_WidthFixed, 30.0f);
      ImGui::TableSetupColumn("D",    ImGuiTableColumnFlags_WidthFixed, 30.0f);
      ImGui::TableSetupColumn("L",    ImGuiTableColumnFlags_WidthFixed, 30.0f);
      ImGui::TableSetupColumn("GF",   ImGuiTableColumnFlags_WidthFixed, 34.0f);
      ImGui::TableSetupColumn("GA",   ImGuiTableColumnFlags_WidthFixed, 34.0f);
      ImGui::TableSetupColumn("GD",   ImGuiTableColumnFlags_WidthFixed, 38.0f);
      ImGui::TableSetupColumn("Pts",  ImGuiTableColumnFlags_WidthFixed, 42.0f);
      ImGui::TableHeadersRow();

      for (const auto &s : g_CareerHub.standings) {
        if (s.league != league) continue;
        bool myTeam = (s.team == sn);

        ImGui::TableNextRow(0, 28.0f);
        if (myTeam) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.09f, 0.50f, 0.22f)));
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1,
            ImGui::ColorConvertFloat4ToU32(ImVec4(0.25f, 0.09f, 0.50f, 0.30f)));
        }

        ImGui::TableSetColumnIndex(0);
        if (myTeam) {
          ImGui::PushStyleColor(ImGuiCol_Text, kAccentRed);
          ImGui::TextUnformatted(s.team.c_str());
          ImGui::PopStyleColor();
        } else {
          ImGui::TextUnformatted(s.team.c_str());
        }

        auto statCol = [](const std::string &v) {
          ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
          ImGui::TextUnformatted(v.c_str());
          ImGui::PopStyleColor();
        };
        ImGui::TableSetColumnIndex(1); statCol(s.p);
        ImGui::TableSetColumnIndex(2); statCol(s.w);
        ImGui::TableSetColumnIndex(3); statCol(s.d);
        ImGui::TableSetColumnIndex(4); statCol(s.l);
        ImGui::TableSetColumnIndex(5); statCol(s.gf);
        ImGui::TableSetColumnIndex(6); statCol(s.ga);
        ImGui::TableSetColumnIndex(7); statCol(s.gd);
        ImGui::TableSetColumnIndex(8);
        ImGui::PushStyleColor(ImGuiCol_Text, kGold);
        ImGui::TextUnformatted(s.pts.c_str());
        ImGui::PopStyleColor();
      }
      ImGui::EndTable();
    }
  }

  if (g_CareerHub.standings.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
    ImGui::TextUnformatted("No standings generated yet.");
    ImGui::PopStyleColor();
  }

  ImGui::EndChild();
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
  ImGui::Begin("##career_root", nullptr,
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

  RenderTopHeader(winW);
  RenderSubNavigation(winW);

  // Content area for tab renderers
  float contentH = winH - kHeaderH - 1.0f - kSubnavH - 1.0f;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgMain);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::BeginChild("##content", ImVec2(winW, contentH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  switch (g_CareerHub.activeTab) {
    case 0: RenderOverviewTab(winW, contentH);   break;
    case 1: RenderManagerTab(winW, contentH);    break;
    case 2: RenderClubTab(winW, contentH);       break;
    case 3: RenderMatchesTab(winW, contentH);    break;
    case 4: RenderStandingsTab(winW, contentH);  break;
    default: break;
  }

  ImGui::EndChild();
  ImGui::End();

  // Set pending action flags outside ImGui::End() — consumed by operator()()
  // in the GL thread after Handle() returns (deferred action pattern).
  if (s_playClicked && g_CareerHub.pendingAction == 0) {
    g_CareerHub.pendingAction = 1;
    printf("[IMGUI MANAGER] Play Match requested\n");
  }
  if (s_menuClicked && g_CareerHub.pendingAction == 0) {
    g_CareerHub.pendingAction = 2;
    printf("[IMGUI MANAGER] Main Menu requested\n");
  }
}
