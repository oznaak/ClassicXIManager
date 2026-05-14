#include "imgui_career.hpp"

#include "imgui.h"
#include <sstream>

#include "main.hpp"
#include "utils/database.hpp"
#include "base/utils.hpp"

CareerHubState g_CareerHub;

// ---- DB helper ---------------------------------------------------------

static std::string DBCell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size() || col >= r->data.at(row).size()) return "";
  return r->data.at(row).at(col);
}

// ---- State management --------------------------------------------------

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

// ---- Style -------------------------------------------------------------

static void ApplyClassicManagerStyle() {
  ImGuiStyle &st = ImGui::GetStyle();
  st.WindowRounding    = 0.0f;
  st.ChildRounding     = 6.0f;
  st.FrameRounding     = 4.0f;
  st.TabRounding       = 4.0f;
  st.ScrollbarRounding = 4.0f;
  st.GrabRounding      = 4.0f;
  st.PopupRounding     = 4.0f;
  st.WindowBorderSize  = 0.0f;
  st.ChildBorderSize   = 1.0f;
  st.FrameBorderSize   = 0.0f;
  st.WindowPadding     = ImVec2(16, 12);
  st.FramePadding      = ImVec2(10, 5);
  st.ItemSpacing       = ImVec2(10, 6);
  st.ItemInnerSpacing  = ImVec2(6, 4);
  st.ScrollbarSize     = 10.0f;
  st.IndentSpacing     = 16.0f;

  ImVec4 *c = st.Colors;
  // Window / backgrounds
  c[ImGuiCol_WindowBg]             = ImVec4(0.027f, 0.051f, 0.102f, 1.00f); // #070D1A
  c[ImGuiCol_ChildBg]              = ImVec4(0.082f, 0.106f, 0.180f, 1.00f); // #151B2E
  c[ImGuiCol_PopupBg]              = ImVec4(0.082f, 0.106f, 0.180f, 0.97f);
  c[ImGuiCol_Border]               = ImVec4(0.192f, 0.220f, 0.329f, 0.80f); // #313854
  c[ImGuiCol_BorderShadow]         = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
  // Frame
  c[ImGuiCol_FrameBg]              = ImVec4(0.125f, 0.149f, 0.227f, 1.00f); // #20263A
  c[ImGuiCol_FrameBgHovered]       = ImVec4(0.165f, 0.192f, 0.278f, 1.00f);
  c[ImGuiCol_FrameBgActive]        = ImVec4(0.192f, 0.220f, 0.310f, 1.00f);
  // Title
  c[ImGuiCol_TitleBg]              = ImVec4(0.027f, 0.051f, 0.102f, 1.00f);
  c[ImGuiCol_TitleBgActive]        = ImVec4(0.027f, 0.051f, 0.102f, 1.00f);
  c[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.027f, 0.051f, 0.102f, 1.00f);
  c[ImGuiCol_MenuBarBg]            = ImVec4(0.027f, 0.051f, 0.102f, 1.00f);
  // Scrollbar
  c[ImGuiCol_ScrollbarBg]          = ImVec4(0.027f, 0.051f, 0.102f, 1.00f);
  c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.192f, 0.220f, 0.329f, 1.00f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.250f, 0.278f, 0.388f, 1.00f);
  c[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.482f, 0.231f, 0.929f, 1.00f);
  // Check / slider
  c[ImGuiCol_CheckMark]            = ImVec4(0.482f, 0.231f, 0.929f, 1.00f);
  c[ImGuiCol_SliderGrab]           = ImVec4(0.482f, 0.231f, 0.929f, 1.00f);
  c[ImGuiCol_SliderGrabActive]     = ImVec4(0.600f, 0.380f, 0.980f, 1.00f);
  // Buttons (default/secondary)
  c[ImGuiCol_Button]               = ImVec4(0.125f, 0.149f, 0.227f, 1.00f);
  c[ImGuiCol_ButtonHovered]        = ImVec4(0.192f, 0.220f, 0.329f, 1.00f);
  c[ImGuiCol_ButtonActive]         = ImVec4(0.250f, 0.278f, 0.388f, 1.00f);
  // Header (Selectable)
  c[ImGuiCol_Header]               = ImVec4(0.192f, 0.220f, 0.329f, 0.60f);
  c[ImGuiCol_HeaderHovered]        = ImVec4(0.250f, 0.278f, 0.388f, 0.80f);
  c[ImGuiCol_HeaderActive]         = ImVec4(0.482f, 0.231f, 0.929f, 0.80f);
  // Separator
  c[ImGuiCol_Separator]            = ImVec4(0.192f, 0.220f, 0.329f, 0.70f);
  c[ImGuiCol_SeparatorHovered]     = ImVec4(0.482f, 0.231f, 0.929f, 0.70f);
  c[ImGuiCol_SeparatorActive]      = ImVec4(0.482f, 0.231f, 0.929f, 1.00f);
  // Resize
  c[ImGuiCol_ResizeGrip]           = ImVec4(0.482f, 0.231f, 0.929f, 0.25f);
  c[ImGuiCol_ResizeGripHovered]    = ImVec4(0.482f, 0.231f, 0.929f, 0.67f);
  c[ImGuiCol_ResizeGripActive]     = ImVec4(0.482f, 0.231f, 0.929f, 0.95f);
  // Table
  c[ImGuiCol_TableHeaderBg]        = ImVec4(0.082f, 0.106f, 0.180f, 1.00f);
  c[ImGuiCol_TableBorderStrong]    = ImVec4(0.192f, 0.220f, 0.329f, 1.00f);
  c[ImGuiCol_TableBorderLight]     = ImVec4(0.125f, 0.149f, 0.227f, 1.00f);
  c[ImGuiCol_TableRowBg]           = ImVec4(0.000f, 0.000f, 0.000f, 0.00f);
  c[ImGuiCol_TableRowBgAlt]        = ImVec4(0.125f, 0.149f, 0.227f, 0.40f);
  // Text
  c[ImGuiCol_Text]                 = ImVec4(0.929f, 0.941f, 0.961f, 1.00f);
  c[ImGuiCol_TextDisabled]         = ImVec4(0.502f, 0.541f, 0.604f, 1.00f);
  // Nav
  c[ImGuiCol_NavHighlight]             = ImVec4(0.482f, 0.231f, 0.929f, 1.00f);
  c[ImGuiCol_NavWindowingHighlight]    = ImVec4(0.482f, 0.231f, 0.929f, 0.70f);
  c[ImGuiCol_NavWindowingDimBg]        = ImVec4(0.000f, 0.000f, 0.000f, 0.50f);
  c[ImGuiCol_ModalWindowDimBg]         = ImVec4(0.000f, 0.000f, 0.000f, 0.50f);
}

// Color palette
static const ImVec4 kTextPrimary = ImVec4(0.929f, 0.941f, 0.961f, 1.00f);
static const ImVec4 kTextMuted   = ImVec4(0.502f, 0.541f, 0.604f, 1.00f);
static const ImVec4 kAccentLight = ImVec4(0.620f, 0.420f, 0.980f, 1.00f); // bright purple
static const ImVec4 kGold        = ImVec4(1.000f, 0.820f, 0.220f, 1.00f);
static const ImVec4 kLeague      = ImVec4(0.620f, 0.780f, 0.360f, 1.00f);
static const ImVec4 kGreen       = ImVec4(0.270f, 0.750f, 0.400f, 1.00f);
static const ImVec4 kNavBg       = ImVec4(0.047f, 0.067f, 0.129f, 1.00f);

// ---- Helper: top-nav button --------------------------------------------

static bool NavButton(const char *label, bool isActive) {
  if (isActive) {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.482f, 0.231f, 0.929f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.580f, 0.330f, 0.970f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.380f, 0.160f, 0.800f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(1.00f, 1.00f, 1.00f, 1.00f));
  } else {
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.082f, 0.106f, 0.180f, 0.60f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.192f, 0.220f, 0.329f, 0.80f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.250f, 0.278f, 0.388f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_Text,          ImVec4(0.620f, 0.650f, 0.710f, 1.00f));
  }
  bool clicked = ImGui::Button(label, ImVec2(100, 32));
  ImGui::PopStyleColor(4);
  return clicked;
}

// ---- Helper: manager profile row ---------------------------------------

static void ProfileRow(const char *label, const std::string &val) {
  ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
  ImGui::Text("%-14s", label);
  ImGui::PopStyleColor();
  ImGui::SameLine();
  ImGui::TextUnformatted(val.c_str());
}

// ---- Render ------------------------------------------------------------

void RenderImGuiCareerHub() {
  if (!g_CareerHub.active) return;

  ApplyClassicManagerStyle();

  ImGuiIO &io = ImGui::GetIO();
  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(io.DisplaySize, ImGuiCond_Always);

  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##career_hub", nullptr,
               ImGuiWindowFlags_NoTitleBar           |
               ImGuiWindowFlags_NoResize             |
               ImGuiWindowFlags_NoMove               |
               ImGuiWindowFlags_NoCollapse           |
               ImGuiWindowFlags_NoBringToFrontOnFocus |
               ImGuiWindowFlags_NoScrollbar          |
               ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();

  // ---- Topbar --------------------------------------------------------
  const float kTopbarH   = 60.0f;
  const float kBtnW      = 100.0f;
  const float kBtnSpacing = 6.0f;

  bool playClicked = false;
  bool menuClicked = false;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, kNavBg);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
  ImGui::BeginChild("##topbar", ImVec2(0, kTopbarH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Brand + club name (left)
  float textY = (kTopbarH - ImGui::GetTextLineHeight()) * 0.5f;
  ImGui::SetCursorPos(ImVec2(16, textY));
  ImGui::PushStyleColor(ImGuiCol_Text, kAccentLight);
  ImGui::TextUnformatted("CLASSIC MANAGER");
  ImGui::PopStyleColor();
  ImGui::SameLine(0, 8);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
  ImGui::Text("|  %s", g_CareerHub.club.name.c_str());
  ImGui::PopStyleColor();

  // Tab buttons (centered)
  float totalNavW  = 4 * kBtnW + 3 * kBtnSpacing;
  float centerStart = (io.DisplaySize.x - totalNavW) * 0.5f;
  ImGui::SetCursorPos(ImVec2(centerStart, (kTopbarH - 32.0f) * 0.5f));

  if (NavButton("Manager",   g_CareerHub.activeTab == 0)) g_CareerHub.activeTab = 0;
  ImGui::SameLine(0, kBtnSpacing);
  if (NavButton("Club",      g_CareerHub.activeTab == 1)) g_CareerHub.activeTab = 1;
  ImGui::SameLine(0, kBtnSpacing);
  if (NavButton("Matches",   g_CareerHub.activeTab == 2)) g_CareerHub.activeTab = 2;
  ImGui::SameLine(0, kBtnSpacing);
  if (NavButton("Standings", g_CareerHub.activeTab == 3)) g_CareerHub.activeTab = 3;

  // Action buttons (right)
  const float kPlayW  = 110.0f;
  const float kMenuW  = 100.0f;
  float rightStart = io.DisplaySize.x - kPlayW - kMenuW - kBtnSpacing - 16.0f;
  ImGui::SetCursorPos(ImVec2(rightStart, (kTopbarH - 32.0f) * 0.5f));

  ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.482f, 0.231f, 0.929f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.580f, 0.330f, 0.970f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.380f, 0.160f, 0.800f, 1.00f));
  playClicked = ImGui::Button("Play Match", ImVec2(kPlayW, 32));
  ImGui::PopStyleColor(3);

  ImGui::SameLine(0, kBtnSpacing);

  ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.125f, 0.149f, 0.227f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.192f, 0.220f, 0.329f, 1.00f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.250f, 0.278f, 0.388f, 1.00f));
  menuClicked = ImGui::Button("Main Menu", ImVec2(kMenuW, 32));
  ImGui::PopStyleColor(3);

  ImGui::EndChild(); // ##topbar

  // thin divider
  ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.192f, 0.220f, 0.329f, 1.00f));
  ImGui::Separator();
  ImGui::PopStyleColor();

  // ---- Content panel -------------------------------------------------
  float contentH = io.DisplaySize.y - kTopbarH - 2.0f; // 2px for separator
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.027f, 0.051f, 0.102f, 1.00f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20, 16));
  ImGui::BeginChild("##content", ImVec2(0, contentH), false);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  switch (g_CareerHub.activeTab) {

    // ---- Manager -------------------------------------------------------
    case 0: {
      ImGui::PushStyleColor(ImGuiCol_Text, kAccentLight);
      ImGui::TextUnformatted("MANAGER PROFILE");
      ImGui::PopStyleColor();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.082f, 0.106f, 0.180f, 1.00f));
      ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
      ImGui::BeginChild("##mgr_card", ImVec2(380, 190), true);
      ImGui::PopStyleVar();
      ImGui::PopStyleColor();
      ImGui::Spacing();
      const auto &m = g_CareerHub.manager;
      ProfileRow("Name",        m.name);
      ProfileRow("Age",         m.age);
      ProfileRow("Nationality", m.nationality);
      ProfileRow("Gender",      m.gender);
      ProfileRow("Club",        m.clubName);
      ImGui::Spacing();
      ImGui::EndChild();
      break;
    }

    // ---- Club ----------------------------------------------------------
    case 1: {
      std::string header = g_CareerHub.club.name;
      if (!g_CareerHub.club.shortName.empty())
        header = g_CareerHub.club.shortName + "  \xe2\x80\x94  " + header; // em dash
      ImGui::PushStyleColor(ImGuiCol_Text, kAccentLight);
      ImGui::TextUnformatted(header.c_str());
      ImGui::PopStyleColor();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
      ImGui::TextUnformatted("SQUAD");
      ImGui::PopStyleColor();
      ImGui::Spacing();

      float tableH = contentH - 80.0f;
      if (ImGui::BeginTable("##squad", 4,
                            ImGuiTableFlags_BordersInnerV |
                            ImGuiTableFlags_RowBg         |
                            ImGuiTableFlags_ScrollY       |
                            ImGuiTableFlags_SizingStretchProp,
                            ImVec2(0, tableH))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Role",    ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Age",     ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Ability", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();

        for (const auto &p : g_CareerHub.players) {
          ImGui::TableNextRow();
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
      break;
    }

    // ---- Matches -------------------------------------------------------
    case 2: {
      ImGui::PushStyleColor(ImGuiCol_Text, kAccentLight);
      ImGui::TextUnformatted("FIXTURES");
      ImGui::PopStyleColor();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      if (g_CareerHub.fixtures.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
        ImGui::TextUnformatted("No fixtures generated yet.");
        ImGui::PopStyleColor();
      } else {
        float tableH = contentH - 60.0f;
        if (ImGui::BeginTable("##fixtures", 7,
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_RowBg         |
                              ImGuiTableFlags_ScrollY       |
                              ImGuiTableFlags_SizingFixedFit,
                              ImVec2(0, tableH))) {
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableSetupColumn("League", ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("MD",     ImGuiTableColumnFlags_WidthFixed, 36);
          ImGui::TableSetupColumn("Rd",     ImGuiTableColumnFlags_WidthFixed, 36);
          ImGui::TableSetupColumn("Home",   ImGuiTableColumnFlags_WidthFixed, 120);
          ImGui::TableSetupColumn("Away",   ImGuiTableColumnFlags_WidthFixed, 120);
          ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed, 90);
          ImGui::TableSetupColumn("Score",  ImGuiTableColumnFlags_WidthFixed, 70);
          ImGui::TableHeadersRow();

          std::string lastLeague;
          for (const auto &f : g_CareerHub.fixtures) {
            ImGui::TableNextRow();

            if (f.league != lastLeague) {
              ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.082f, 0.116f, 0.196f, 1.0f)));
              lastLeague = f.league;
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, kLeague);
            ImGui::TextUnformatted(f.league.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(f.matchday.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(f.round.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(3);
            ImGui::TextUnformatted(f.home.c_str());
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(f.away.c_str());
            ImGui::TableSetColumnIndex(5);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(f.status.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(6);
            if (f.score != "-") {
              ImGui::PushStyleColor(ImGuiCol_Text, kGreen);
              ImGui::TextUnformatted(f.score.c_str());
              ImGui::PopStyleColor();
            } else {
              ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
              ImGui::TextUnformatted("-");
              ImGui::PopStyleColor();
            }
          }
          ImGui::EndTable();
        }
      }
      break;
    }

    // ---- Standings -----------------------------------------------------
    case 3: {
      ImGui::PushStyleColor(ImGuiCol_Text, kAccentLight);
      ImGui::TextUnformatted("LEAGUE STANDINGS");
      ImGui::PopStyleColor();
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::Spacing();

      if (g_CareerHub.standings.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
        ImGui::TextUnformatted("No standings generated yet.");
        ImGui::PopStyleColor();
      } else {
        float tableH = contentH - 60.0f;
        if (ImGui::BeginTable("##standings", 10,
                              ImGuiTableFlags_BordersInnerV |
                              ImGuiTableFlags_RowBg         |
                              ImGuiTableFlags_ScrollY       |
                              ImGuiTableFlags_SizingFixedFit,
                              ImVec2(0, tableH))) {
          ImGui::TableSetupScrollFreeze(0, 1);
          ImGui::TableSetupColumn("League", ImGuiTableColumnFlags_WidthFixed, 130);
          ImGui::TableSetupColumn("Team",   ImGuiTableColumnFlags_WidthStretch);
          ImGui::TableSetupColumn("P",      ImGuiTableColumnFlags_WidthFixed, 32);
          ImGui::TableSetupColumn("W",      ImGuiTableColumnFlags_WidthFixed, 32);
          ImGui::TableSetupColumn("D",      ImGuiTableColumnFlags_WidthFixed, 32);
          ImGui::TableSetupColumn("L",      ImGuiTableColumnFlags_WidthFixed, 32);
          ImGui::TableSetupColumn("GF",     ImGuiTableColumnFlags_WidthFixed, 36);
          ImGui::TableSetupColumn("GA",     ImGuiTableColumnFlags_WidthFixed, 36);
          ImGui::TableSetupColumn("GD",     ImGuiTableColumnFlags_WidthFixed, 40);
          ImGui::TableSetupColumn("Pts",    ImGuiTableColumnFlags_WidthFixed, 40);
          ImGui::TableHeadersRow();

          std::string lastLeague;
          for (const auto &s : g_CareerHub.standings) {
            ImGui::TableNextRow();

            if (s.league != lastLeague) {
              ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::ColorConvertFloat4ToU32(ImVec4(0.082f, 0.116f, 0.196f, 1.0f)));
              lastLeague = s.league;
            }

            ImGui::TableSetColumnIndex(0);
            ImGui::PushStyleColor(ImGuiCol_Text, kLeague);
            ImGui::TextUnformatted(s.league.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(1);
            ImGui::TextUnformatted(s.team.c_str());
            ImGui::TableSetColumnIndex(2);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(s.p.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(3);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(s.w.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(4);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(s.d.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(5);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(s.l.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(6);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(s.gf.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(7);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(s.ga.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(8);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextMuted);
            ImGui::TextUnformatted(s.gd.c_str());
            ImGui::PopStyleColor();
            ImGui::TableSetColumnIndex(9);
            ImGui::PushStyleColor(ImGuiCol_Text, kGold);
            ImGui::TextUnformatted(s.pts.c_str());
            ImGui::PopStyleColor();
          }
          ImGui::EndTable();
        }
      }
      break;
    }
  }

  ImGui::EndChild(); // ##content
  ImGui::End();      // ##career_hub

  // Only set the flag here. Callbacks are consumed in operator()() after
  // SwapBuffers returns, avoiding deadlock with the message queue.
  if (playClicked && g_CareerHub.pendingAction == 0) {
    g_CareerHub.pendingAction = 1;
    printf("[IMGUI MANAGER] Play Match requested\n");
  }
  if (menuClicked && g_CareerHub.pendingAction == 0) {
    g_CareerHub.pendingAction = 2;
    printf("[IMGUI MANAGER] Main Menu requested\n");
  }
}
