#include "imgui_career.hpp"
#include "imgui_manager_fonts.hpp"

#include "imgui.h"
#include <SDL2/SDL_image.h>
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#endif
#include <sstream>
#include <string>
#include <map>
#include <vector>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cctype>
#include <cmath>

#include "main.hpp"
#include "utils/database.hpp"
#include "base/utils.hpp"

CareerHubState g_CareerHub;
PreMatchLineupState g_PreMatchLineup;
bool g_SilentMatchLoadingOverlay = false;
bool g_SilentMatchLoadingOverlayLogged = false;
std::string g_MatchCompetitionLogoPath;
std::string g_MatchCompetitionName;

// ---- Font globals -------------------------------------------------------

ImFont *g_ManagerFontSmall   = nullptr;
ImFont *g_ManagerFontRegular = nullptr;
ImFont *g_ManagerFontMedium  = nullptr;
ImFont *g_ManagerFontBold    = nullptr;
ImFont *g_ManagerFontTitle   = nullptr;
ImFont *g_ManagerFontHero    = nullptr;

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

  g_ManagerFontSmall   = tryLoad(atlas, kRegularPaths, 15.0f);
  g_ManagerFontRegular = tryLoad(atlas, kRegularPaths, 17.0f);
  g_ManagerFontMedium  = tryLoad(atlas, kRegularPaths, 19.0f);
  g_ManagerFontBold    = tryLoad(atlas, kBoldPaths,    20.0f);
  g_ManagerFontTitle   = tryLoad(atlas, kBoldPaths,    27.0f);
  g_ManagerFontHero    = tryLoad(atlas, kBoldPaths,    36.0f);

  if (!g_ManagerFontRegular)
    printf("[IMGUI] Font load failed, using default ImGui font\n");
}

// ---- DB cell helper -----------------------------------------------------

static std::string DBCell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size() || col >= r->data.at(row).size()) return "";
  return r->data.at(row).at(col);
}

// ---- Badge texture cache ------------------------------------------------

static std::map<std::string, GLuint> s_BadgeCache;

GLuint LoadBadgeTex(const std::string &logoRelPath) {
  if (logoRelPath.empty()) return 0;
  std::string fullPath = "databases/default/" + logoRelPath;
  auto it = s_BadgeCache.find(fullPath);
  if (it != s_BadgeCache.end()) return it->second;

  SDL_Surface *surf = IMG_Load(fullPath.c_str());
  if (!surf) { s_BadgeCache[fullPath] = 0; return 0; }
  SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(surf);
  if (!rgba) { s_BadgeCache[fullPath] = 0; return 0; }

  GLuint texID = 0;
  glGenTextures(1, &texID);
  glBindTexture(GL_TEXTURE_2D, texID);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  // Negative LOD bias: prefer a slightly higher-res mipmap level for more detail.
  glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_LOD_BIAS, -0.75f);
  // Anisotropic filtering: sharpens textures sampled at non-integer scales.
  {
    float maxAniso = 1.0f;
    glGetFloatv(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, &maxAniso);
    if (maxAniso > 1.0f)
      glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAX_ANISOTROPY_EXT, maxAniso);
  }
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
  glGenerateMipmap(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, 0);
  SDL_FreeSurface(rgba);
  printf("[IMGUI MANAGER] loaded badge: %s\n", logoRelPath.c_str());
  s_BadgeCache[fullPath] = texID;
  return texID;
}

static void ClearBadgeCache() {
  for (auto &kv : s_BadgeCache)
    if (kv.second) glDeleteTextures(1, &kv.second);
  s_BadgeCache.clear();
}

// ---- State management ---------------------------------------------------

// forward declaration
static void ResetNavState();

void CareerHubState::Clear() {
  active        = false;
  primaryTab    = 0;
  activeTab     = 0;
  pendingAction = 0;
  onPlayMatch   = nullptr;
  onMainMenu    = nullptr;
  onAdvance     = nullptr;
  onPlayFixture = nullptr;
  managerId     = 0;
  clubId        = 0;
  currentDate.clear();
  currentDateDisplay.clear();
  seasonYear      = 0;
  hasTodayFixture     = false;
  todayFixture        = {};
  hasSeasonEnded       = false;
  isAdvancing          = false;
  pendingAdvanceAction = ADVANCE_NONE;
  advanceFramesWaited  = 0;
  onStartNextSeason    = nullptr;
  manager = {};
  club    = {};
  players.clear();
  fixtures.clear();
  standings.clear();
  ClearBadgeCache();
  ResetNavState();
}

// Format ISO date YYYY-MM-DD -> "1 Jul 2026"
static std::string FormatDateDisplay(const std::string &iso) {
  if (iso.size() < 10) return iso;
  int year  = atoi(iso.substr(0, 4).c_str());
  int month = atoi(iso.substr(5, 2).c_str());
  int day   = atoi(iso.substr(8, 2).c_str());
  static const char *kMon[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
  if (month < 1 || month > 12) return iso;
  char buf[24];
  snprintf(buf, sizeof(buf), "%d %s %d", day, kMon[month], year);
  return std::string(buf);
}

void CareerHubState::LoadFromDB(int mgrId, int cId) {
  active = false;
  managerId = mgrId;
  clubId    = cId;

  {
    std::stringstream q;
    q << "SELECT managers.name, managers.age, managers.nationality, managers.gender, teams.name,"
      << " managers.current_date, managers.season_year"
      << " FROM managers LEFT JOIN teams ON managers.club_id = teams.id"
      << " WHERE managers.id = " << mgrId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str());
    manager.name        = DBCell(r, 0, 0);
    manager.age         = DBCell(r, 0, 1);
    manager.nationality = DBCell(r, 0, 2);
    manager.gender      = DBCell(r, 0, 3);
    manager.clubName    = DBCell(r, 0, 4);
    currentDate         = DBCell(r, 0, 5);
    std::string syStr   = DBCell(r, 0, 6);
    seasonYear          = syStr.empty() ? 0 : atoi(syStr.c_str());
    delete r;
  }

  // Initialize current_date if missing (e.g. loaded from old save).
  if (currentDate.empty()) {
    time_t now = time(nullptr);
    struct tm *t = localtime(&now);
    int yr = 1900 + t->tm_year;
    if (seasonYear > 0) yr = seasonYear;
    char buf[16];
    snprintf(buf, sizeof(buf), "%04d-07-01", yr);
    currentDate = buf;
    std::stringstream uq;
    uq << "UPDATE managers SET current_date='" << currentDate << "'"
       << " WHERE id=" << mgrId << ";";
    DatabaseResult *ur = GetDB()->Query(uq.str());
    delete ur;
  }
  currentDateDisplay = FormatDateDisplay(currentDate);
  printf("[CAREER] Loaded manager id=%d current_date=%s season_year=%d\n",
         mgrId, currentDate.c_str(), seasonYear);

  {
    std::stringstream q;
    q << "SELECT teams.name, teams.shortname, teams.logo_url, leagues.name, leagues.id"
      << " FROM teams JOIN leagues ON teams.league_id = leagues.id"
      << " WHERE teams.id = " << clubId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str());
    club.name       = DBCell(r, 0, 0);
    club.shortName  = DBCell(r, 0, 1);
    club.logoPath   = DBCell(r, 0, 2);
    club.leagueName = DBCell(r, 0, 3);
    club.leagueId   = atoi(DBCell(r, 0, 4).c_str());
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
      << " fixtures.status, fixtures.home_score, fixtures.away_score,"
      << " fixtures.fixture_date,"
      << " home.name, away.name, leagues.logo_url, fixtures.league_id"
      << " FROM fixtures"
      << " JOIN leagues ON fixtures.league_id = leagues.id"
      << " JOIN teams home ON fixtures.home_team_id = home.id"
      << " JOIN teams away ON fixtures.away_team_id = away.id"
      << " WHERE fixtures.manager_id = " << managerId
      << " ORDER BY leagues.id ASC, fixtures.matchday ASC, fixtures.round ASC;";
    DatabaseResult *r = GetDB()->Query(q.str());
    for (unsigned int i = 0; i < r->data.size(); i++) {
      Fixture f;
      f.league      = DBCell(r, i, 0);
      f.matchday    = DBCell(r, i, 1);
      f.round       = DBCell(r, i, 2);
      f.home        = DBCell(r, i, 3);
      f.away        = DBCell(r, i, 4);
      f.homeLogo    = DBCell(r, i, 5);
      f.awayLogo    = DBCell(r, i, 6);
      f.status      = DBCell(r, i, 7);
      std::string hs  = DBCell(r, i, 8);
      std::string as2 = DBCell(r, i, 9);
      f.fixtureDate = DBCell(r, i, 10);
      f.homeFull    = DBCell(r, i, 11);
      f.awayFull    = DBCell(r, i, 12);
      f.leagueLogo  = DBCell(r, i, 13);
      f.leagueId    = atoi(DBCell(r, i, 14).c_str());
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
      << " standings.goal_difference, standings.points, teams.name"
      << " FROM standings"
      << " JOIN leagues ON standings.league_id = leagues.id"
      << " JOIN teams ON standings.team_id = teams.id"
      << " WHERE standings.manager_id = " << managerId
      << " ORDER BY leagues.id ASC, standings.points DESC,"
      << " standings.goal_difference DESC, standings.goals_for DESC;";
    DatabaseResult *r = GetDB()->Query(q.str());
    for (unsigned int i = 0; i < r->data.size(); i++) {
      Standing s;
      s.league    = DBCell(r, i, 0);
      s.team      = DBCell(r, i, 1);
      s.teamLogo  = DBCell(r, i, 2);
      s.p         = DBCell(r, i, 3);
      s.w         = DBCell(r, i, 4);
      s.d         = DBCell(r, i, 5);
      s.l         = DBCell(r, i, 6);
      s.gf        = DBCell(r, i, 7);
      s.ga        = DBCell(r, i, 8);
      s.gd        = DBCell(r, i, 9);
      s.pts       = DBCell(r, i, 10);
      s.teamFull  = DBCell(r, i, 11);
      standings.push_back(s);
    }
    delete r;
  }

  // Detect today's fixture for manager's club.
  hasTodayFixture = false;
  todayFixture    = {};
  if (!currentDate.empty() && clubId > 0) {
    std::stringstream fq;
    fq << "SELECT fixtures.id, fixtures.home_team_id, fixtures.away_team_id,"
       << " fixtures.matchday, home.shortname, away.shortname, fixtures.fixture_date,"
       << " fixtures.league_id"
       << " FROM fixtures"
       << " JOIN teams home ON fixtures.home_team_id = home.id"
       << " JOIN teams away ON fixtures.away_team_id = away.id"
       << " WHERE fixtures.manager_id = " << managerId
       << " AND fixtures.fixture_date = '" << currentDate << "'"
       << " AND fixtures.status = 'scheduled'"
       << " AND (fixtures.home_team_id = " << clubId
       << " OR fixtures.away_team_id = " << clubId << ")"
       << " ORDER BY fixtures.matchday ASC, fixtures.id ASC LIMIT 1;";
    DatabaseResult *fr = GetDB()->Query(fq.str());
    if (fr->data.size() > 0) {
      hasTodayFixture          = true;
      todayFixture.id          = atoi(DBCell(fr, 0, 0).c_str());
      todayFixture.homeTeamId  = atoi(DBCell(fr, 0, 1).c_str());
      todayFixture.awayTeamId  = atoi(DBCell(fr, 0, 2).c_str());
      todayFixture.matchday    = atoi(DBCell(fr, 0, 3).c_str());
      todayFixture.homeShort   = DBCell(fr, 0, 4);
      todayFixture.awayShort   = DBCell(fr, 0, 5);
      todayFixture.fixtureDate = DBCell(fr, 0, 6);
      todayFixture.leagueId    = atoi(DBCell(fr, 0, 7).c_str());
      printf("[CAREER] Matchday found fixture=%d date=%s home=%s away=%s\n",
             todayFixture.id, currentDate.c_str(),
             todayFixture.homeShort.c_str(), todayFixture.awayShort.c_str());
    }
    delete fr;
  }

  // Detect end of season: all fixtures for this manager/season are played.
  hasSeasonEnded = false;
  if (seasonYear > 0) {
    std::stringstream cq;
    cq << "SELECT COUNT(*) FROM fixtures WHERE manager_id=" << mgrId
       << " AND season_year=" << seasonYear << ";";
    DatabaseResult *cr = GetDB()->Query(cq.str());
    int total = (cr->data.size() > 0 && !cr->data.at(0).at(0).empty())
                  ? atoi(cr->data.at(0).at(0).c_str()) : 0;
    delete cr;

    if (total > 0) {
      std::stringstream sq;
      sq << "SELECT COUNT(*) FROM fixtures WHERE manager_id=" << mgrId
         << " AND season_year=" << seasonYear << " AND status='scheduled';";
      DatabaseResult *sr = GetDB()->Query(sq.str());
      int remaining = (sr->data.size() > 0 && !sr->data.at(0).at(0).empty())
                        ? atoi(sr->data.at(0).at(0).c_str()) : 0;
      delete sr;
      hasSeasonEnded = (remaining == 0);
      if (hasSeasonEnded)
        printf("[CAREER] Season ended manager=%d season=%d remainingScheduled=0\n",
               mgrId, seasonYear);
    }
  }

  isAdvancing          = false;
  pendingAdvanceAction = ADVANCE_NONE;
  advanceFramesWaited  = 0;
  active = true;
}

// ---- Navigation state ---------------------------------------------------

enum e_ManagerPage {
  PAGE_HOME = 0,
  PAGE_INBOX,
  PAGE_NEWS,
  PAGE_SCHEDULE,
  PAGE_SQUAD,
  PAGE_TACTICS,
  PAGE_TRAINING,
  PAGE_STAFF,
  PAGE_SCOUTING,
  PAGE_FINANCES,
  PAGE_TRANSFERS,
  PAGE_COMPETITIONS,
  PAGE_PLAYERS,
  PAGE_TEAMS,
  PAGE_SETTINGS,
  PAGE_COUNT
};

static const char *kPageNames[PAGE_COUNT] = {
  "Home", "Inbox", "News", "Schedule",
  "Squad", "Tactics", "Training", "Staff", "Scouting", "Finances", "Transfers",
  "Competitions", "Players", "Teams", "Settings"
};

static e_ManagerPage g_activePage = PAGE_HOME;

static void ResetNavState() {
  g_activePage = PAGE_HOME;
}

// ---- Color palette ------------------------------------------------------

static const ImVec4 kBgApp     = ImVec4(0.027f, 0.043f, 0.086f, 1.0f); // #070B16
static const ImVec4 kBgSidebar = ImVec4(0.043f, 0.063f, 0.125f, 1.0f); // #0B1020
static const ImVec4 kBgHeader  = ImVec4(0.051f, 0.075f, 0.141f, 1.0f); // #0D1324
static const ImVec4 kBgCard    = ImVec4(0.071f, 0.102f, 0.173f, 1.0f); // #121A2C
static const ImVec4 kBgCardAlt = ImVec4(0.094f, 0.129f, 0.212f, 1.0f); // #182136
static const ImVec4 kBorder    = ImVec4(0.149f, 0.196f, 0.290f, 0.80f); // #26324A
static const ImVec4 kAccent    = ImVec4(0.741f, 0.102f, 0.788f, 1.0f); // #BD1AC9 magenta
static const ImVec4 kAccentH   = ImVec4(0.863f, 0.318f, 0.918f, 1.0f);
static const ImVec4 kAccentA   = ImVec4(0.576f, 0.047f, 0.620f, 1.0f);
static const ImVec4 kViolet    = ImVec4(0.482f, 0.231f, 0.929f, 1.0f); // #7C3AED
static const ImVec4 kTextPri   = ImVec4(0.937f, 0.949f, 0.965f, 1.0f);
static const ImVec4 kTextSec   = ImVec4(0.612f, 0.655f, 0.729f, 1.0f); // #9CA7BA
static const ImVec4 kTextDim   = ImVec4(0.239f, 0.290f, 0.388f, 1.0f);
static const ImVec4 kSuccess   = ImVec4(0.133f, 0.773f, 0.369f, 1.0f);
static const ImVec4 kWarning   = ImVec4(0.973f, 0.620f, 0.043f, 1.0f);
static const ImVec4 kDanger    = ImVec4(0.937f, 0.267f, 0.267f, 1.0f);
static const ImVec4 kGold      = ImVec4(0.992f, 0.820f, 0.110f, 1.0f);
static const ImVec4 kBlue      = ImVec4(0.361f, 0.682f, 0.941f, 1.0f);

static inline ImU32 C32(const ImVec4 &v) { return ImGui::ColorConvertFloat4ToU32(v); }

// ---- Theme --------------------------------------------------------------

static void ApplyManagerTheme() {
  ImGuiStyle &st = ImGui::GetStyle();
  st.WindowRounding    = 0.0f;
  st.ChildRounding     = 10.0f;
  st.FrameRounding     = 5.0f;
  st.ScrollbarRounding = 4.0f;
  st.GrabRounding      = 4.0f;
  st.WindowBorderSize  = 0.0f;
  st.ChildBorderSize   = 0.0f;
  st.FrameBorderSize   = 0.0f;
  st.WindowPadding     = ImVec2(0.0f, 0.0f);
  st.FramePadding      = ImVec2(10.0f, 5.0f);
  st.ItemSpacing       = ImVec2(8.0f, 5.0f);
  st.ItemInnerSpacing  = ImVec2(5.0f, 4.0f);
  st.CellPadding       = ImVec2(7.0f, 5.0f);
  st.ScrollbarSize     = 6.0f;
  st.IndentSpacing     = 12.0f;

  ImVec4 *c = st.Colors;
  c[ImGuiCol_WindowBg]              = kBgApp;
  c[ImGuiCol_ChildBg]               = kBgCard;
  c[ImGuiCol_PopupBg]               = kBgCard;
  c[ImGuiCol_Border]                = kBorder;
  c[ImGuiCol_BorderShadow]          = ImVec4(0,0,0,0);
  c[ImGuiCol_FrameBg]               = ImVec4(0.055f,0.082f,0.153f,1.0f);
  c[ImGuiCol_FrameBgHovered]        = ImVec4(0.082f,0.122f,0.220f,1.0f);
  c[ImGuiCol_FrameBgActive]         = ImVec4(0.110f,0.161f,0.282f,1.0f);
  c[ImGuiCol_TitleBg]               = kBgSidebar;
  c[ImGuiCol_TitleBgActive]         = kBgSidebar;
  c[ImGuiCol_TitleBgCollapsed]      = kBgSidebar;
  c[ImGuiCol_ScrollbarBg]           = ImVec4(0,0,0,0);
  c[ImGuiCol_ScrollbarGrab]         = ImVec4(0.110f,0.161f,0.282f,1.0f);
  c[ImGuiCol_ScrollbarGrabHovered]  = ImVec4(0.176f,0.243f,0.396f,1.0f);
  c[ImGuiCol_ScrollbarGrabActive]   = kViolet;
  c[ImGuiCol_CheckMark]             = kAccent;
  c[ImGuiCol_Button]                = ImVec4(0.055f,0.082f,0.153f,1.0f);
  c[ImGuiCol_ButtonHovered]         = ImVec4(0.082f,0.122f,0.220f,1.0f);
  c[ImGuiCol_ButtonActive]          = ImVec4(0.110f,0.161f,0.282f,1.0f);
  c[ImGuiCol_Header]                = ImVec4(0.082f,0.122f,0.220f,0.60f);
  c[ImGuiCol_HeaderHovered]         = ImVec4(0.110f,0.161f,0.282f,0.80f);
  c[ImGuiCol_HeaderActive]          = ImVec4(0.176f,0.243f,0.396f,1.0f);
  c[ImGuiCol_Separator]             = kBorder;
  c[ImGuiCol_SeparatorHovered]      = kViolet;
  c[ImGuiCol_SeparatorActive]       = kViolet;
  c[ImGuiCol_ResizeGrip]            = ImVec4(0,0,0,0);
  c[ImGuiCol_ResizeGripHovered]     = ImVec4(0,0,0,0);
  c[ImGuiCol_ResizeGripActive]      = ImVec4(0,0,0,0);
  c[ImGuiCol_TableHeaderBg]         = ImVec4(0.043f,0.063f,0.125f,1.0f);
  c[ImGuiCol_TableBorderStrong]     = kBorder;
  c[ImGuiCol_TableBorderLight]      = ImVec4(0.055f,0.082f,0.153f,1.0f);
  c[ImGuiCol_TableRowBg]            = ImVec4(0,0,0,0);
  c[ImGuiCol_TableRowBgAlt]         = ImVec4(0.035f,0.055f,0.102f,0.40f);
  c[ImGuiCol_Text]                  = kTextPri;
  c[ImGuiCol_TextDisabled]          = kTextDim;
  c[ImGuiCol_NavHighlight]          = kAccent;
}

// ---- Font helpers -------------------------------------------------------

static inline void PushMgrFont(ImFont *f) { if (f) ImGui::PushFont(f); }
static inline void PopMgrFont(ImFont *f)  { if (f) ImGui::PopFont(); }

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

// ---- Modern card --------------------------------------------------------

static void BeginModernCard(const char *id, ImVec2 size, const char *title = nullptr) {
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, C32(kBgCard), 10.0f);
  dl->AddRect(p0, p1, C32(kBorder), 10.0f, 0, 1.0f);
  dl->AddLine(ImVec2(p0.x + 12, p0.y + 1), ImVec2(p1.x - 12, p0.y + 1),
              IM_COL32(255,255,255,7), 1.0f);

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
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
    ImGui::PushStyleColor(ImGuiCol_Separator, ImVec4(0.082f,0.122f,0.220f,0.50f));
    ImGui::Separator();
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
}
static void EndModernCard() { ImGui::EndChild(); }

// ---- Background ---------------------------------------------------------

static void DrawAppBackground(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  dl->AddRectFilled(wp, ImVec2(wp.x+w, wp.y+h), C32(kBgApp));
  ImVec2 br(wp.x+w, wp.y+h);
  dl->AddCircleFilled(br, w*0.45f, IM_COL32(50,15,105,11), 48);
  dl->AddCircleFilled(br, w*0.22f, IM_COL32(72,24,145,15), 48);
  dl->AddRectFilledMultiColor(wp, ImVec2(wp.x+w, wp.y+80.0f),
    IM_COL32(16,26,62,32), IM_COL32(16,26,62,32),
    IM_COL32(0,0,0,0),    IM_COL32(0,0,0,0));
}

// ---- Badge helpers ------------------------------------------------------

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

static void DrawTeamBadge(const std::string &logoPath, const std::string &sn, float sz) {
  GLuint tex = LoadBadgeTex(logoPath);
  if (tex) ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(sz,sz));
  else     DrawFallbackBadge(sn, sz);
}

static void DrawTeamLabel(const std::string &logoPath, const std::string &sn, float badgeSz = 18.0f) {
  float lh   = ImGui::GetTextLineHeight();
  float offY = (lh - badgeSz) * 0.5f;
  if (offY > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);
  GLuint tex = LoadBadgeTex(logoPath);
  if (tex) ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(badgeSz,badgeSz));
  else     DrawFallbackBadge(sn, badgeSz);
  if (offY > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() - offY);
  ImGui::SameLine(0, 5);
  ImGui::TextUnformatted(sn.c_str());
}

// ---- Status pill --------------------------------------------------------

static void DrawStatusPill(const std::string &status) {
  ImVec4 bg, col;
  if (status == "played") {
    bg  = ImVec4(0.010f,0.130f,0.052f,1.0f); col = kSuccess;
  } else if (status == "postponed") {
    bg  = ImVec4(0.160f,0.140f,0.014f,1.0f); col = kWarning;
  } else {
    bg  = ImVec4(0.055f,0.085f,0.160f,1.0f); col = kTextSec;
  }
  const float kPX = 7.0f, kPY = 2.5f;
  ImVec2 tsz  = ImGui::CalcTextSize(status.c_str());
  ImVec2 pill = ImVec2(tsz.x + kPX*2, tsz.y + kPY*2);
  ImVec2 p    = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x+pill.x, p.y+pill.y), C32(bg), 5.0f);
  dl->AddText(ImVec2(p.x+kPX, p.y+kPY), C32(col), status.c_str());
  ImGui::Dummy(pill);
}

// ---- Ability bar --------------------------------------------------------

static void DrawAbilityBar(const std::string &abilityStr, float width = 60.0f) {
  float val = abilityStr.empty() ? 0.0f : (float)atof(abilityStr.c_str());
  if (val < 0.0f) val = 0.0f;
  if (val > 1.0f) val = 1.0f;
  const float kH = 7.0f;
  float lh = ImGui::GetTextLineHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  float oy = (lh - kH) * 0.5f;
  ImVec2 p0(p.x, p.y + oy), p1(p.x + width, p.y + oy + kH);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(12,20,38,255), 3.0f);
  if (val > 0.005f) {
    ImVec4 fc = (val >= 0.70f) ? kSuccess : (val >= 0.45f) ? kGold : kDanger;
    dl->AddRectFilled(p0, ImVec2(p0.x + width * val, p1.y), C32(fc), 3.0f);
  }
  ImGui::Dummy(ImVec2(width, lh));
  ImGui::SameLine(0, 5);
  char buf[6]; snprintf(buf, sizeof(buf), "%d", (int)(val*100.0f+0.5f));
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted(buf);
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
}

// ---- Layout constants ---------------------------------------------------

static const float kSidebarW = 260.0f;
static const float kTopHdrH  = 52.0f;

// ---- Sidebar nav helpers ------------------------------------------------

static void DrawNavSectionHeader(const char *label) {
  ImGui::Dummy(ImVec2(0, 5.0f));
  ImVec2 p = ImGui::GetCursorScreenPos();
  PushMgrFont(g_ManagerFontSmall);
  float lh = ImGui::GetTextLineHeight();
  ImGui::GetWindowDrawList()->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
    ImVec2(p.x + 4.0f, p.y), C32(kTextDim), label);
  PopMgrFont(g_ManagerFontSmall);
  ImGui::Dummy(ImVec2(0, lh + 3.0f));
}

static void DrawNavItem(const char *label, e_ManagerPage page, int badge = 0) {
  bool active = (g_activePage == page);
  float w  = ImGui::GetContentRegionAvail().x;
  const float kH = 34.0f;

  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImVec2 p1 = ImVec2(p0.x + w, p0.y + kH);

  char btnId[80];
  snprintf(btnId, sizeof(btnId), "##navbtn_%d", (int)page);
  bool clicked = ImGui::InvisibleButton(btnId, ImVec2(w, kH));
  bool hov     = ImGui::IsItemHovered();

  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (active) {
    dl->AddRectFilled(p0, p1, IM_COL32(80, 20, 145, 185), 6.0f);
    dl->AddRectFilled(p0, ImVec2(p0.x + 3.0f, p1.y), C32(kAccent), 1.5f);
  } else if (hov) {
    dl->AddRectFilled(p0, p1, IM_COL32(22, 38, 76, 175), 6.0f);
  }

  // Text via DrawList so we control screen position exactly
  ImFont *font = active ? g_ManagerFontBold : g_ManagerFontRegular;
  if (font) ImGui::PushFont(font);
  float lh = ImGui::GetTextLineHeight();
  ImVec2 tp(p0.x + 14.0f, p0.y + (kH - lh) * 0.5f);
  dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), tp,
              active ? C32(kTextPri) : C32(kTextSec), label);
  if (font) ImGui::PopFont();

  // Badge bubble
  if (badge > 0) {
    char buf[6]; snprintf(buf, sizeof(buf), badge > 9 ? "9+" : "%d", badge);
    ImVec2 btsz = ImGui::CalcTextSize(buf);
    const float br = 9.0f;
    float bcx = p1.x - br - 6.0f;
    float bcy = p0.y + kH * 0.5f;
    dl->AddCircleFilled(ImVec2(bcx, bcy), br, C32(kAccent));
    dl->AddText(ImVec2(bcx - btsz.x * 0.5f, bcy - btsz.y * 0.5f),
                IM_COL32(255, 255, 255, 230), buf);
  }

  if (clicked) g_activePage = page;
}

// ---- Action flags (deferred, consumed after Handle()) -------------------

static bool s_playClicked        = false; // test engine (hardcoded match)
static bool s_advanceClicked     = false; // advance day or play fixture
static bool s_startSeasonClicked = false; // start next season
static bool s_menuClicked        = false;

// ---- DrawSidebar --------------------------------------------------------

static void DrawSidebar(float sideW, float winH) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSidebar);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##sidebar", ImVec2(sideW, winH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  ImDrawList *dl = ImGui::GetWindowDrawList();

  // ---- Brand ----------------------------------------------------------
  ImGui::SetCursorPos(ImVec2(16.0f, 16.0f));
  PushMgrFont(g_ManagerFontBold);
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
  ImGui::TextUnformatted(g_CareerHub.manager.name.empty() ? "Classic Manager" : g_CareerHub.manager.name.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontBold);

  ImGui::Dummy(ImVec2(0, 8.0f));
  {
    ImVec2 sp = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(sp.x + 16, sp.y), ImVec2(sp.x + sideW - 16, sp.y),
                C32(kBorder), 1.0f);
    ImGui::Dummy(ImVec2(0, 8.0f));
  }

  // ---- Club identity --------------------------------------------------
  ImGui::SetCursorPosX(16.0f);
  const float kBadgeSz = 42.0f;
  DrawTeamBadge(g_CareerHub.club.logoPath, g_CareerHub.club.shortName, kBadgeSz);
  ImGui::SameLine(0, 10.0f);
  ImGui::BeginGroup();
  PushMgrFont(g_ManagerFontBold);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  const std::string &cname = g_CareerHub.club.name.empty() ? "Unknown" : g_CareerHub.club.name;
  ImGui::TextUnformatted(cname.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontBold);
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  if (!g_CareerHub.club.leagueName.empty())
    ImGui::TextUnformatted(g_CareerHub.club.leagueName.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  ImGui::EndGroup();

  ImGui::Dummy(ImVec2(0, 4.0f));
  ImGui::SetCursorPosX(16.0f);
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  {
    std::string mgr;
    if (!g_CareerHub.manager.nationality.empty()) mgr = g_CareerHub.manager.nationality;
    if (!g_CareerHub.manager.age.empty()) {
      if (!mgr.empty()) mgr += " \xe2\x80\xa2 Age ";
      else mgr = "Age ";
      mgr += g_CareerHub.manager.age;
    }
    if (!mgr.empty()) ImGui::TextUnformatted(mgr.c_str());
  }
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);

  ImGui::Dummy(ImVec2(0, 10.0f));
  {
    ImVec2 sp = ImGui::GetCursorScreenPos();
    dl->AddLine(ImVec2(sp.x + 16, sp.y), ImVec2(sp.x + sideW - 16, sp.y),
                C32(kBorder), 1.0f);
    ImGui::Dummy(ImVec2(0, 6.0f));
  }

  // ---- Nav (scrollable) -----------------------------------------------
  // Reserve space for bottom section: settings + main menu + separators
  const float kBottomH = 34.0f + 34.0f + 1.0f + 10.0f + 8.0f;
  float navH = ImGui::GetContentRegionAvail().y - kBottomH;
  if (navH < 40.0f) navH = 40.0f;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
  ImGui::BeginChild("##nav_area", ImVec2(0, navH), false);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  DrawNavSectionHeader("MAIN");
  DrawNavItem("Home",     PAGE_HOME);
  DrawNavItem("Inbox",    PAGE_INBOX, 3);
  DrawNavItem("News",     PAGE_NEWS);
  DrawNavItem("Schedule", PAGE_SCHEDULE);
  ImGui::Dummy(ImVec2(0, 6.0f));

  DrawNavSectionHeader("CLUB");
  DrawNavItem("Squad",     PAGE_SQUAD);
  DrawNavItem("Tactics",   PAGE_TACTICS);
  DrawNavItem("Training",  PAGE_TRAINING);
  DrawNavItem("Staff",     PAGE_STAFF);
  DrawNavItem("Scouting",  PAGE_SCOUTING);
  DrawNavItem("Finances",  PAGE_FINANCES);
  DrawNavItem("Transfers", PAGE_TRANSFERS);
  ImGui::Dummy(ImVec2(0, 6.0f));

  DrawNavSectionHeader("WORLD");
  DrawNavItem("Competitions", PAGE_COMPETITIONS);
  DrawNavItem("Players",      PAGE_PLAYERS);
  DrawNavItem("Teams",        PAGE_TEAMS);

  ImGui::EndChild(); // nav_area

  // ---- Bottom ---------------------------------------------------------
  {
    ImVec2 sp = ImGui::GetCursorScreenPos();
    dl->AddLine(sp, ImVec2(sp.x + sideW, sp.y), C32(kBorder), 1.0f);
    ImGui::Dummy(ImVec2(0, 8.0f));
  }

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 0.0f));
  ImGui::BeginChild("##nav_bottom", ImVec2(0, kBottomH - 10.0f), false,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  DrawNavItem("Settings", PAGE_SETTINGS);

  ImGui::SetCursorPosX(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f,0.06f,0.06f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.16f,0.09f,0.09f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.22f,0.12f,0.12f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text,          kDanger);
  float btnW = ImGui::GetContentRegionAvail().x;
  if (ImGui::Button("Main Menu", ImVec2(btnW, 34.0f))) s_menuClicked = true;
  ImGui::PopStyleColor(4);

  ImGui::EndChild(); // nav_bottom
  ImGui::EndChild(); // sidebar
}

// ---- DrawTopHeader ------------------------------------------------------

static void DrawTopHeader(float contentX, float contentW) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgHeader);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##topheader", ImVec2(contentW, kTopHdrH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  ImDrawList *dl = ImGui::GetWindowDrawList();

  // ---- Left: page title -----------------------------------------------
  PushMgrFont(g_ManagerFontTitle);
  float titleH = ImGui::GetTextLineHeight();
  PopMgrFont(g_ManagerFontTitle);
  float titleY = (kTopHdrH - titleH) * 0.5f;
  ImGui::SetCursorPos(ImVec2(20.0f, titleY));
  PushMgrFont(g_ManagerFontTitle);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted(kPageNames[g_activePage]);
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontTitle);

  // ---- Right: search + date + Advance/PlayMatch + Test Engine CTAs -------
  const float kBtnW    = 112.0f;  // each button width
  const float kDateW   = 88.0f;   // wider for "31 Aug 2026"
  const float kSearchW = 140.0f;
  const float kElemH   = 30.0f;
  const float kGap     = 8.0f;
  float elemY     = (kTopHdrH - kElemH) * 0.5f;
  float rightEdge = contentW - 16.0f;

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 5.0f));
  PushMgrFont(g_ManagerFontBold);

  // Test Engine button (rightmost) — hardcoded match launcher.
  ImGui::SetCursorPos(ImVec2(rightEdge - kBtnW, elemY));
  if (SecBtn("Test Engine", ImVec2(kBtnW, kElemH))) s_playClicked = true;

  // Advance / Play Match / Start Season button — priority:
  // 1. isAdvancing -> disabled "Advancing..."
  // 2. hasTodayFixture -> "Play Match"
  // 3. hasSeasonEnded -> "Start Season"
  // 4. else -> "Advance"
  float advBtnX = rightEdge - kBtnW - kGap - kBtnW;
  ImGui::SetCursorPos(ImVec2(advBtnX, elemY));
  if (g_CareerHub.isAdvancing) {
    ImGui::BeginDisabled();
    CTAButton("Advancing...", ImVec2(kBtnW, kElemH));
    ImGui::EndDisabled();
  } else if (g_CareerHub.hasTodayFixture) {
    if (CTAButton("Play Match", ImVec2(kBtnW, kElemH))) s_advanceClicked = true;
  } else if (g_CareerHub.hasSeasonEnded) {
    if (CTAButton("Start Season", ImVec2(kBtnW, kElemH))) s_startSeasonClicked = true;
  } else {
    if (CTAButton("Advance", ImVec2(kBtnW, kElemH))) s_advanceClicked = true;
  }

  PopMgrFont(g_ManagerFontBold);
  ImGui::PopStyleVar();

  // Current date display
  PushMgrFont(g_ManagerFontSmall);
  float dateH = ImGui::GetTextLineHeight();
  PopMgrFont(g_ManagerFontSmall);
  float dateX = advBtnX - kGap - kDateW;
  ImGui::SetCursorPos(ImVec2(dateX, elemY + (kElemH - dateH) * 0.5f));
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  const std::string &dateStr = g_CareerHub.currentDateDisplay.empty()
                                 ? g_CareerHub.currentDate
                                 : g_CareerHub.currentDateDisplay;
  ImGui::TextUnformatted(dateStr.empty() ? "--" : dateStr.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);

  // Search placeholder (draw-list rect + text)
  float searchX = dateX - kGap - kSearchW;
  ImGui::SetCursorPos(ImVec2(searchX, elemY));
  ImVec2 scr = ImGui::GetCursorScreenPos();
  dl->AddRectFilled(scr, ImVec2(scr.x + kSearchW, scr.y + kElemH),
                   C32(ImVec4(0.055f,0.082f,0.153f,1.0f)), 5.0f);
  dl->AddRect(scr, ImVec2(scr.x + kSearchW, scr.y + kElemH),
             C32(kBorder), 5.0f, 0, 1.0f);
  PushMgrFont(g_ManagerFontSmall);
  float sH = ImGui::GetTextLineHeight();
  ImGui::SetCursorPos(ImVec2(searchX + 10.0f, elemY + (kElemH - sH) * 0.5f));
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted("Search...");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);

  // Bottom border
  ImVec2 hp = ImGui::GetWindowPos();
  dl->AddLine(ImVec2(hp.x, hp.y + kTopHdrH - 1),
              ImVec2(hp.x + contentW, hp.y + kTopHdrH - 1),
              C32(kBorder), 1.0f);

  ImGui::EndChild();
}

// ---- Home page cards ----------------------------------------------------

static void DrawMessagesCard(ImVec2 sz) {
  static const struct { const char *from, *subject, *time; } kMsgs[] = {
    { "Board",  "Pre-season objectives confirmed",  "Today"     },
    { "Media",  "Press conference this Friday",      "Today"     },
    { "Staff",  "Fitness report ready",              "Yesterday" },
    { "Board",  "Transfer budget allocated",         "2d ago"    },
    { "Fans",   "Season ticket renewals open",       "3d ago"    },
    { "Staff",  "Training schedule published",       "4d ago"    },
    { "Board",  "Scouting targets list sent",        "5d ago"    },
    { "Media",  "Pre-season preview requested",      "6d ago"    },
  };
  const int kN = 8;
  BeginModernCard("##msgs_ov", sz);
  // Vertically centre the table block within the card.
  {
    PushMgrFont(g_ManagerFontSmall);
    float fh = ImGui::GetTextLineHeight();
    PopMgrFont(g_ManagerFontSmall);
    float tableH = kN * (fh + 14.0f);        // 8 rows × (font + CellPad.y 7×2)
    float innerH = sz.y - 24.0f;             // card WindowPadding top+bot
    float topOff = (innerH - tableH) * 0.5f;
    if (topOff > 2.0f) ImGui::Dummy(ImVec2(0, topOff));
  }
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5.0f, 7.0f));
  if (ImGui::BeginTable("##msg_tbl", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, 0))) {
    ImGui::TableSetupColumn("From",    ImGuiTableColumnFlags_WidthFixed,  55.0f);
    ImGui::TableSetupColumn("Subject", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("When",    ImGuiTableColumnFlags_WidthFixed,  55.0f);
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
  const std::string &cn = g_CareerHub.club.name.empty() ? "Your Club" : g_CareerHub.club.name;
  const std::string &mn = g_CareerHub.manager.name.empty() ? "The Manager" : g_CareerHub.manager.name;

  // Estimate content height to vertically centre it in the card.
  PushMgrFont(g_ManagerFontSmall); float lhSm = ImGui::GetTextLineHeight(); PopMgrFont(g_ManagerFontSmall);
  PushMgrFont(g_ManagerFontBold);  float lhBd = ImGui::GetTextLineHeight(); PopMgrFont(g_ManagerFontBold);
  float contentH = lhSm + 5.0f + lhBd + 5.0f + lhSm * 2.0f;  // tag + headline + body(2 lines)
  float innerH   = sz.y - 24.0f;
  float topOff   = (innerH - contentH) * 0.5f;
  if (topOff > 2.0f) ImGui::Dummy(ImVec2(0, topOff));

  // Tag line: "NEWS  |  LeagueName" — centered horizontally
  {
    const char *lnStr = g_CareerHub.club.leagueName.empty() ? "Club News" : g_CareerHub.club.leagueName.c_str();
    PushMgrFont(g_ManagerFontSmall);
    float newsW  = ImGui::CalcTextSize("NEWS").x;
    float sepW   = ImGui::CalcTextSize("|").x;
    float lnW    = ImGui::CalcTextSize(lnStr).x;
    float totalW = newsW + 16.0f + sepW + 16.0f + lnW;
    float avail  = ImGui::GetContentRegionAvail().x;
    float startX = ImGui::GetCursorPosX() + (avail - totalW) * 0.5f;
    if (startX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(startX);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("NEWS");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 8);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted("|");
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 8);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted(lnStr);
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
  }

  ImGui::Spacing();

  // Headline — centered horizontally
  char headline[128];
  snprintf(headline, sizeof(headline), "%s ready for the new campaign", cn.c_str());
  PushMgrFont(g_ManagerFontBold);
  {
    float hlW   = ImGui::CalcTextSize(headline).x;
    float avail = ImGui::GetContentRegionAvail().x;
    float hlX   = ImGui::GetCursorPosX() + (avail - hlW) * 0.5f;
    if (hlX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(hlX);
  }
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
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
    mn.c_str(), mn.c_str());
  {
    float avail   = ImGui::GetContentRegionAvail().x;
    float bodyW   = avail * 0.88f;
    float bodyOff = (avail - bodyW) * 0.5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + bodyOff);
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + bodyW);
    ImGui::TextWrapped("%s", body);
    ImGui::PopTextWrapPos();
  }
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();
}

// Draw a single team box (HOME or AWAY) with all content horizontally centred.
static void DrawFixtureTeamBox(const char *childId, const char *label,
                                const std::string &logoPath, const std::string &shortName,
                                const std::string &dispName, float teamW, float boxH,
                                float badgeSz, bool highlight, const std::string &userSn) {
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::GetWindowDrawList()->AddRectFilled(p0, ImVec2(p0.x+teamW, p0.y+boxH), C32(kBgCardAlt), 8.0f);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4.0f, 8.0f));
  ImGui::BeginChild(childId, ImVec2(teamW, boxH), false);
  ImGui::PopStyleVar(); ImGui::PopStyleColor();

  PushMgrFont(g_ManagerFontSmall);
  // Vertically centre content: label + badge + name within the box interior.
  {
    float fh      = ImGui::GetTextLineHeight();
    float sp      = ImGui::GetStyle().ItemSpacing.y;
    float contentH = fh + sp + badgeSz + sp + fh;  // label + badge + name
    float innerH   = boxH - 16.0f;                  // WindowPadding.y 8 × 2
    float topOff   = (innerH - contentH) * 0.5f;
    if (topOff > 1.0f) ImGui::Dummy(ImVec2(0, topOff));
  }

  // Label — centred
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  { float tw = ImGui::CalcTextSize(label).x;
    ImGui::SetCursorPosX((teamW - tw) * 0.5f); }
  ImGui::TextUnformatted(label);
  ImGui::PopStyleColor();

  // Badge — centred
  { float bx = (teamW - badgeSz) * 0.5f;
    ImGui::SetCursorPosX(bx > 0 ? bx : 0.0f); }
  { GLuint t = LoadBadgeTex(logoPath);
    if (t) ImGui::Image((ImTextureID)(intptr_t)t, ImVec2(badgeSz, badgeSz));
    else   DrawFallbackBadge(shortName, badgeSz); }

  // Team name — centred, clipped to box width
  ImGui::PushStyleColor(ImGuiCol_Text, highlight ? kAccent : kTextPri);
  { float tw = ImGui::CalcTextSize(dispName.c_str()).x;
    float cx = (teamW - std::min(tw, teamW - 8.0f)) * 0.5f;
    ImGui::SetCursorPosX(cx > 0 ? cx : 0.0f); }
  ImGui::TextUnformatted(dispName.c_str());
  ImGui::PopStyleColor();

  PopMgrFont(g_ManagerFontSmall);
  ImGui::EndChild();
  (void)userSn;
}

static void DrawNextFixtureCard(ImVec2 sz) {
  BeginModernCard("##nxt_fix", sz);  // no card title
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
    ImGui::TextUnformatted("No upcoming fixture.");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    EndModernCard(); return;
  }

  const std::string &homeDisp = f->homeFull.empty() ? f->home : f->homeFull;
  const std::string &awayDisp = f->awayFull.empty() ? f->away : f->awayFull;
  const float kBadge = 42.0f;
  const float kBoxH  = 105.0f;  // 8+15+5+42+5+17+8 = 100px + a few px slack

  // Vertical centering: push content down so it floats in the middle.
  PushMgrFont(g_ManagerFontSmall);
  float hdrH = ImGui::GetTextLineHeight();   // league header line
  PopMgrFont(g_ManagerFontSmall);
  float contentH = hdrH + 6.0f + kBoxH;     // header + spacing + boxes
  float innerH   = sz.y - 24.0f;            // card WindowPadding top+bottom = 24
  float topOff   = (innerH - contentH) * 0.5f;
  if (topOff > 2.0f) ImGui::Dummy(ImVec2(0, topOff));

  // League header — centred horizontally: [logo] LeagueName - Matchweek N
  {
    const float lbSz = 18.0f;
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "%s - Matchweek %s", f->league.c_str(), f->matchday.c_str());
    GLuint lt = LoadBadgeTex(f->leagueLogo);
    PushMgrFont(g_ManagerFontSmall);
    float textW  = ImGui::CalcTextSize(hdr).x;
    float totalW = lt ? (lbSz + 6.0f + textW) : textW;
    float avail  = ImGui::GetContentRegionAvail().x;
    float startX = ImGui::GetCursorPosX() + (avail - totalW) * 0.5f;
    if (startX > ImGui::GetCursorPosX()) ImGui::SetCursorPosX(startX);
    if (lt) {
      float lh = ImGui::GetTextLineHeight();
      float oy = (lh - lbSz) * 0.5f;
      if (oy > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + oy);
      ImGui::Image((ImTextureID)(intptr_t)lt, ImVec2(lbSz, lbSz));
      if (oy > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() - oy);
      ImGui::SameLine(0, 6);
    }
    ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
    ImGui::TextUnformatted(hdr);
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
  }
  ImGui::Spacing();

  float avail = ImGui::GetContentRegionAvail().x;
  const float vsW   = 28.0f;
  const float kMaxTeamW = 170.0f;
  float teamW = (avail - vsW) * 0.5f - 2.0f;
  if (teamW > kMaxTeamW) teamW = kMaxTeamW;
  if (teamW < 40.0f)     teamW = 40.0f;

  // Center the HOME + vs + AWAY block horizontally.
  float totalBoxW = teamW * 2.0f + vsW + 4.0f;
  float boxIndent = (avail - totalBoxW) * 0.5f;
  if (boxIndent > 0.0f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + boxIndent);

  // Capture row origin before HOME box so AWAY can start at the same Y.
  ImVec2 rowOrigin = ImGui::GetCursorScreenPos();

  // Home box
  DrawFixtureTeamBox("##nf_h", "HOME", f->homeLogo, f->home, homeDisp,
                     teamW, kBoxH, kBadge, f->home == sn, sn);

  // Draw "vs" via DrawList at absolute position — no cursor manipulation.
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    PushMgrFont(g_ManagerFontBold);
    ImVec2 vsSz = ImGui::CalcTextSize("vs");
    float vsX = rowOrigin.x + teamW + (vsW - vsSz.x) * 0.5f;
    float vsY = rowOrigin.y + (kBoxH - vsSz.y) * 0.5f;
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(vsX, vsY), C32(kAccent), "vs");
    PopMgrFont(g_ManagerFontBold);
  }

  // Force AWAY box to start at exactly the same screen Y as HOME.
  ImGui::SetCursorScreenPos(ImVec2(rowOrigin.x + teamW + vsW, rowOrigin.y));
  DrawFixtureTeamBox("##nf_a", "AWAY", f->awayLogo, f->away, awayDisp,
                     teamW, kBoxH, kBadge, f->away == sn, sn);

  EndModernCard();
}

static void DrawFixtureScheduleCard(ImVec2 sz) {
  BeginModernCard("##sched", sz);
  const std::string &sn     = g_CareerHub.club.shortName;
  int userLeagueId           = g_CareerHub.club.leagueId;

  // Find current matchweek: user club's next scheduled fixture in their league.
  int currentMD = 0;
  for (const auto &f : g_CareerHub.fixtures) {
    if (f.leagueId != userLeagueId) continue;
    if (f.home != sn && f.away != sn) continue;
    if (f.status == "scheduled") { currentMD = atoi(f.matchday.c_str()); break; }
  }
  // Fallback: last matchday played.
  if (currentMD == 0) {
    for (const auto &f : g_CareerHub.fixtures) {
      if (f.leagueId != userLeagueId) continue;
      if (f.home != sn && f.away != sn) continue;
      int md = atoi(f.matchday.c_str());
      if (md > currentMD) currentMD = md;
    }
  }

  if (currentMD == 0) {
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No fixtures.");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    EndModernCard(); return;
  }

  ImGui::Dummy(ImVec2(0, 4.0f));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
  ImGui::Text("Matchweek %d", currentMD);
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Spacing();

  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5.0f, 6.0f));
  if (ImGui::BeginTable("##sched_tbl", 3,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, 0))) {
    ImGui::TableSetupColumn("Home", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("vs",   ImGuiTableColumnFlags_WidthFixed, 24.0f);
    ImGui::TableSetupColumn("Away", ImGuiTableColumnFlags_WidthStretch);
    PushMgrFont(g_ManagerFontSmall);
    int shown = 0;
    for (const auto &f : g_CareerHub.fixtures) {
      if (f.leagueId != userLeagueId) continue;
      if (atoi(f.matchday.c_str()) != currentMD) continue;
      bool isUserHome = (f.home == sn);
      bool isUserAway = (f.away == sn);
      const std::string &hDisp = f.homeFull.empty() ? f.home : f.homeFull;
      const std::string &aDisp = f.awayFull.empty() ? f.away : f.awayFull;
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      if (isUserHome) ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
      DrawTeamLabel(f.homeLogo, hDisp, 20.0f);
      if (isUserHome) ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      if (!f.score.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
        ImGui::TextUnformatted(f.score.c_str());
        ImGui::PopStyleColor();
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::TextUnformatted("vs");
        ImGui::PopStyleColor();
      }
      ImGui::TableSetColumnIndex(2);
      if (isUserAway) ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
      DrawTeamLabel(f.awayLogo, aDisp, 20.0f);
      if (isUserAway) ImGui::PopStyleColor();
      shown++;
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
  BeginModernCard("##snap", sz);
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

  ImGui::Dummy(ImVec2(0, 4.0f));
  ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 4.0f);
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kBlue);
  ImGui::TextUnformatted(myLeague.c_str());
  ImGui::PopStyleColor();
  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Spacing();

  // Collect pointers to this league's standings (already ordered points DESC).
  std::vector<const CareerHubState::Standing *> rows;
  for (const auto &s : g_CareerHub.standings)
    if (s.league == myLeague) rows.push_back(&s);

  // Find user club position.
  int userIdx = -1;
  for (int i = 0; i < (int)rows.size(); i++)
    if (rows[i]->team == sn) { userIdx = i; break; }

  // 5-row window centred on user, clamped to valid range.
  int total = (int)rows.size();
  int start = 0, end = std::min(4, total - 1);
  if (userIdx >= 0) {
    start = userIdx - 2;
    end   = userIdx + 2;
    if (start < 0)        { end   += -start; start = 0; }
    if (end >= total)     { start  = std::max(0, start - (end - (total - 1))); end = total - 1; }
  }
  if (end - start > 4) end = start + 4;

  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5.0f, 5.0f));
  if (ImGui::BeginTable("##snap_tbl", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, 0))) {
    ImGui::TableSetupColumn("Pos",  ImGuiTableColumnFlags_WidthFixed,  26.0f);
    ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("P",    ImGuiTableColumnFlags_WidthFixed,  20.0f);
    ImGui::TableSetupColumn("Pts",  ImGuiTableColumnFlags_WidthFixed,  32.0f);
    for (int i = start; i <= end && i < total; i++) {
      const CareerHubState::Standing *s = rows[i];
      bool mine = (s->team == sn);
      ImGui::TableNextRow(0, 26.0f);
      if (mine) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(80,20,160,42));
      ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      ImGui::Text("%d.", i + 1);
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      const std::string &dispName = s->teamFull.empty() ? s->team : s->teamFull;
      DrawTeamLabel(s->teamLogo, dispName, 20.0f);
      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted(s->p.c_str());
      ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(3);
      ImGui::PushStyleColor(ImGuiCol_Text, mine ? kGold : kTextPri);
      ImGui::TextUnformatted(s->pts.c_str());
      ImGui::PopStyleColor();
    }
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();
}

// ---- DrawHomePage -------------------------------------------------------
// Layout: Left 28% (Messages) | Center 42% (Story + Fixture + Agenda) | Right 30% (Schedule + Snapshot)

static void DrawHomePage(float w, float h) {
  const float kPad = 16.0f, kGap = 10.0f;
  float usW    = w - 2.0f*kPad - 2.0f*kGap;
  float leftW  = usW * 0.28f;
  float centerW = usW * 0.42f;
  float rightW  = usW - leftW - centerW;
  float colH   = h - 12.0f;

  // Fixed, content-driven panel heights — panels end near their content.
  const float kStoryH  = 115.0f;   // news tag + headline + 2 body lines
  const float kFixtH   = 175.0f;   // league header + home/away boxes (105px) + padding
  const float kUpcomH  = 5.0f * 27.0f + 28.0f;   // 5 rows (CellPad 6*2=12 + font 15) + card pad only
  const float kSnapH   = 210.0f;   // 5 rows × 26px fixed + header ~55px + 24 card pad + slack
  // Messages: card takes full column height; content is centred inside it.
  PushMgrFont(g_ManagerFontSmall);
  float msgFontH = ImGui::GetTextLineHeight();
  PopMgrFont(g_ManagerFontSmall);
  float kMsgH   = 8.0f * (msgFontH + 14.0f) + 56.0f;  // 56 = comfortable top+bot breathing room
  // Fixture Schedule: pre-count fixtures for current matchweek to size card to content.
  float schedH;
  {
    const std::string &snS = g_CareerHub.club.shortName;
    int ulid = g_CareerHub.club.leagueId, cmd = 0;
    for (const auto &f : g_CareerHub.fixtures) {
      if (f.leagueId != ulid || (f.home != snS && f.away != snS)) continue;
      if (f.status == "scheduled") { cmd = atoi(f.matchday.c_str()); break; }
    }
    if (cmd == 0) for (const auto &f : g_CareerHub.fixtures) {
      if (f.leagueId != ulid || (f.home != snS && f.away != snS)) continue;
      int md = atoi(f.matchday.c_str()); if (md > cmd) cmd = md;
    }
    int cnt = 0;
    if (cmd > 0) for (const auto &f : g_CareerHub.fixtures)
      if (f.leagueId == ulid && atoi(f.matchday.c_str()) == cmd) cnt++;
    // Badge (20px) drives row height, not font (15px). CellPadding.y = 6*2 = 12.
    float rowH = (msgFontH > 20.0f ? msgFontH : 20.0f) + 12.0f;
    schedH = (cnt > 0 ? cnt : 1) * rowH + 76.0f;  // rows + MW label + sep + spacing + card pad
    if (schedH < 80.0f) schedH = 80.0f;
  }
  float agendaH = kUpcomH;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));

  // Left: Messages — fixed content height, no stretching.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_l", ImVec2(leftW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  DrawMessagesCard(ImVec2(leftW, kMsgH));
  ImGui::EndChild();

  ImGui::SameLine(0, kGap);

  // Center: Story + NextFixture + Upcoming
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_c", ImVec2(centerW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  DrawTopStoryCard(ImVec2(centerW, kStoryH));
  ImGui::Dummy(ImVec2(0, kGap));
  DrawNextFixtureCard(ImVec2(centerW, kFixtH));
  ImGui::Dummy(ImVec2(0, kGap));
  {
    // Inline upcoming card — fixed 5-row height
    BeginModernCard("##agenda", ImVec2(centerW, agendaH));
    const std::string &snA = g_CareerHub.club.shortName;
    int shown = 0;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5.0f, 6.0f));
    if (ImGui::BeginTable("##ag", 3,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX,
                          ImVec2(0, 0))) {
      ImGui::TableSetupColumn("MW",  ImGuiTableColumnFlags_WidthFixed, 32.0f);
      ImGui::TableSetupColumn("H/A", ImGuiTableColumnFlags_WidthFixed, 22.0f);
      ImGui::TableSetupColumn("Opp", ImGuiTableColumnFlags_WidthStretch);
      PushMgrFont(g_ManagerFontSmall);
      for (const auto &f : g_CareerHub.fixtures) {
        if (shown >= 5) break;
        if (f.home != snA && f.away != snA) continue;
        bool ih = (f.home == snA);
        const std::string &oppFull = ih ? f.awayFull : f.homeFull;
        const std::string &opp     = oppFull.empty() ? (ih ? f.away : f.home) : oppFull;
        const std::string &logo    = ih ? f.awayLogo : f.homeLogo;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::Text("MW%s", f.matchday.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        ImGui::PushStyleColor(ImGuiCol_Text, ih ? kSuccess : kTextSec);
        ImGui::TextUnformatted(ih ? "H" : "A");
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        DrawTeamLabel(logo, opp, 20.0f);
        shown++;
      }
      PopMgrFont(g_ManagerFontSmall);
      ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    EndModernCard();
  }
  ImGui::EndChild();

  ImGui::SameLine(0, kGap);

  // Right: Fixture Schedule + League Snapshot
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_r", ImVec2(rightW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  DrawFixtureScheduleCard(ImVec2(rightW, schedH));
  ImGui::Dummy(ImVec2(0, kGap));
  DrawLeagueSnapshotCard(ImVec2(rightW, kSnapH));
  ImGui::EndChild();
}

// ---- DrawSquadPage ------------------------------------------------------

static int s_squadFilter = 0;

static void DrawSquadPage(float w, float h) {
  const float kPad = 16.0f, kGap = 10.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 14.0f;

  // Header card
  float hdrH = 68.0f;
  BeginModernCard("##sqhdr", ImVec2(usW, hdrH));
  const auto &cl = g_CareerHub.club;
  DrawTeamBadge(cl.logoPath, cl.shortName, 44.0f);
  ImGui::SameLine(0, 14.0f);
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

  // Toolbar card
  float toolH = 38.0f;
  BeginModernCard("##sqtool", ImVec2(usW, toolH));
  ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 3.0f);
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(9.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(4.0f, 0.0f));
  SecBtn("In Possession");
  ImGui::SameLine(0, 4);
  SecBtn("Out of Possession");
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
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.58f,0.33f,0.97f,1.0f));
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

  // Squad table card
  float squadH = usH - hdrH - toolH - kGap * 2.0f - 6.0f;
  if (squadH < 60.0f) squadH = 60.0f;
  BeginModernCard("##sqtbl", ImVec2(usW, squadH), "SQUAD");
  float tblH = squadH - 56.0f;
  if (tblH < 30.0f) tblH = 30.0f;
  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(8.0f, 6.0f));
  if (ImGui::BeginTable("##sq", 4,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, tblH))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("Name",    ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("Role",    ImGuiTableColumnFlags_WidthFixed, 152.0f);
    ImGui::TableSetupColumn("Age",     ImGuiTableColumnFlags_WidthFixed,  38.0f);
    ImGui::TableSetupColumn("Ability", ImGuiTableColumnFlags_WidthFixed,  92.0f);
    PushMgrFont(g_ManagerFontSmall);
    ImGui::TableHeadersRow();
    for (const auto &p : g_CareerHub.players) {
      ImGui::TableNextRow(0, 30.0f);
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
      DrawAbilityBar(p.ability, 60.0f);
    }
    if (g_CareerHub.players.empty()) {
      ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted("No squad data.");
      ImGui::PopStyleColor();
    }
    PopMgrFont(g_ManagerFontSmall);
    ImGui::EndTable();
  }
  ImGui::PopStyleVar();
  EndModernCard();
}

// ---- DrawSchedulePage ---------------------------------------------------

static void DrawSchedulePage(float w, float h) {
  const float kPad = 16.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 20.0f;
  const std::string &sn = g_CareerHub.club.shortName;

  std::vector<std::string> leagues;
  for (const auto &f : g_CareerHub.fixtures) {
    bool found = false;
    for (const auto &l : leagues) if (l == f.league) { found = true; break; }
    if (!found) leagues.push_back(f.league);
  }

  BeginModernCard("##sched_outer", ImVec2(usW, usH), "FIXTURES");
  ImGui::BeginChild("##sc_scroll", ImVec2(0, usH - 58.0f), false);

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

    int cnt = 0;
    for (const auto &f : g_CareerHub.fixtures) if (f.league == lg) cnt++;
    float rH   = 30.0f;
    float cardH = 34.0f + (float)cnt * rH + 32.0f + 24.0f;

    ImVec2 cp0 = ImGui::GetCursorScreenPos();
    ImVec2 cp1 = ImVec2(cp0.x + usW - 28.0f, cp0.y + cardH);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cp0, cp1, C32(kBgCardAlt), 8.0f);
    dl->AddRect(cp0, cp1, C32(kBorder), 8.0f, 0, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    std::string cid = "##sc_lg_" + lg;
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
    std::string tid = "##sct_" + lg;
    if (ImGui::BeginTable(tid.c_str(), 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("MD",     ImGuiTableColumnFlags_WidthFixed,  32.0f);
      ImGui::TableSetupColumn("Rd",     ImGuiTableColumnFlags_WidthFixed,  26.0f);
      ImGui::TableSetupColumn("Home",   ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Away",   ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,  86.0f);
      ImGui::TableSetupColumn("Score",  ImGuiTableColumnFlags_WidthFixed,  52.0f);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::TableHeadersRow();
      for (const auto &f : g_CareerHub.fixtures) {
        if (f.league != lg) continue;
        bool myGame = (f.home == sn || f.away == sn);
        ImGui::TableNextRow(0, rH);
        if (myGame)
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(75,18,140,42));
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

// ---- DrawCompetitionsPage -----------------------------------------------

static void DrawCompetitionsPage(float w, float h) {
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

  BeginModernCard("##comp_outer", ImVec2(usW, usH), "LEAGUE STANDINGS");
  ImGui::BeginChild("##cp_scroll", ImVec2(0, usH - 58.0f), false);

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

    int cnt = 0;
    for (const auto &s : g_CareerHub.standings) if (s.league == lg) cnt++;
    float rH    = 30.0f;
    float cardH = 34.0f + (float)cnt * rH + 34.0f + 24.0f;

    ImVec2 cp0 = ImGui::GetCursorScreenPos();
    ImVec2 cp1 = ImVec2(cp0.x + usW - 28.0f, cp0.y + cardH);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(cp0, cp1, C32(kBgCardAlt), 8.0f);
    dl->AddRect(cp0, cp1, C32(kBorder), 8.0f, 0, 1.0f);

    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12.0f, 10.0f));
    std::string cid = "##cp_lg_" + lg;
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
    std::string tid = "##cpt_" + lg;
    if (ImGui::BeginTable(tid.c_str(), 10,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_PadOuterX)) {
      ImGui::TableSetupColumn("#",    ImGuiTableColumnFlags_WidthFixed,  22.0f);
      ImGui::TableSetupColumn("Team", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("P",    ImGuiTableColumnFlags_WidthFixed,  24.0f);
      ImGui::TableSetupColumn("W",    ImGuiTableColumnFlags_WidthFixed,  24.0f);
      ImGui::TableSetupColumn("D",    ImGuiTableColumnFlags_WidthFixed,  24.0f);
      ImGui::TableSetupColumn("L",    ImGuiTableColumnFlags_WidthFixed,  24.0f);
      ImGui::TableSetupColumn("GF",   ImGuiTableColumnFlags_WidthFixed,  28.0f);
      ImGui::TableSetupColumn("GA",   ImGuiTableColumnFlags_WidthFixed,  28.0f);
      ImGui::TableSetupColumn("GD",   ImGuiTableColumnFlags_WidthFixed,  32.0f);
      ImGui::TableSetupColumn("Pts",  ImGuiTableColumnFlags_WidthFixed,  36.0f);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::TableHeadersRow();

      int pos = 1;
      for (const auto &s : g_CareerHub.standings) {
        if (s.league != lg) continue;
        bool mine = (s.team == sn);
        ImGui::TableNextRow(0, rH);
        if (mine) {
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(75,18,140,42));
        }
        ImGui::TableSetColumnIndex(0);
        {
          ImVec2 ps = ImGui::GetCursorScreenPos();
          const float cr = 8.5f;
          float cy = ps.y + (rH - 10.0f) * 0.5f + cr * 0.5f;
          ImDrawList *dl2 = ImGui::GetWindowDrawList();
          dl2->AddCircleFilled(ImVec2(ps.x + cr, cy), cr,
            mine ? C32(kViolet) : IM_COL32(18,30,56,255));
          char nb[4]; snprintf(nb, sizeof(nb), "%d", pos);
          ImVec2 ts = ImGui::CalcTextSize(nb);
          dl2->AddText(ImVec2(ps.x + cr - ts.x*0.5f, cy - ts.y*0.5f),
                       mine ? IM_COL32(255,255,255,230) : C32(kTextDim), nb);
          ImGui::Dummy(ImVec2(cr*2, rH - 12.0f));
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
  float ch = h * 0.35f;
  if (ch < 80.0f) ch = 80.0f;
  BeginModernCard("##soon", ImVec2(cw, ch));
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
  ImGui::TextUnformatted("Focus is on the Home dashboard and core career flow.");
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();
}

// ---- DrawWorkspace ------------------------------------------------------

static void DrawWorkspace(float contentW, float workH) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgApp);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##workspace", ImVec2(contentW, workH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  switch (g_activePage) {
    case PAGE_HOME:         DrawHomePage(contentW, workH);         break;
    case PAGE_SQUAD:        DrawSquadPage(contentW, workH);        break;
    case PAGE_SCHEDULE:     DrawSchedulePage(contentW, workH);     break;
    case PAGE_COMPETITIONS: DrawCompetitionsPage(contentW, workH); break;
    default:
      DrawComingSoonPage(contentW, workH, kPageNames[g_activePage]);
      break;
  }

  ImGui::EndChild();
}

// ---- DrawAdvancingModalOverlay ------------------------------------------
// Drawn via the foreground drawlist so it floats above all ImGui windows.
// The normal career hub is still rendered underneath.

static void DrawAdvancingModalOverlay() {
  ImVec2 display = ImGui::GetIO().DisplaySize;
  ImDrawList *fg = ImGui::GetForegroundDrawList();

  // Semi-transparent full-screen dim.
  fg->AddRectFilled(ImVec2(0, 0), display, IM_COL32(0, 0, 0, 120));

  // Centered card.
  const float kCardW = 420.0f, kCardH = 140.0f, kRounding = 12.0f;
  ImVec2 cardPos((display.x - kCardW) * 0.5f, (display.y - kCardH) * 0.5f);
  ImVec2 cardEnd(cardPos.x + kCardW, cardPos.y + kCardH);
  fg->AddRectFilled(cardPos, cardEnd, IM_COL32(18, 26, 44, 245), kRounding);
  fg->AddRect(cardPos, cardEnd, IM_COL32(120, 80, 255, 180), kRounding, 0, 1.5f);

  // Animated dots: cycle "." / ".." / "..." at ~1.5 Hz.
  double t = ImGui::GetTime();
  int dotCount = 1 + (int)(fmod(t * 1.5, 3.0));
  std::string dots(dotCount, '.');

  const char *titleBase = (g_CareerHub.pendingAdvanceAction == ADVANCE_START_SEASON)
                            ? "Starting new season" : "Advancing";
  std::string titleStr = std::string(titleBase) + dots;
  const char *subtitle = "Processing fixtures and career events";

  // Draw title text via drawlist (avoids cursor/window context dependency).
  ImFont *titleFont = g_ManagerFontTitle   ? g_ManagerFontTitle   : ImGui::GetIO().Fonts->Fonts[0];
  ImFont *subFont   = g_ManagerFontRegular ? g_ManagerFontRegular : ImGui::GetIO().Fonts->Fonts[0];

  ImVec2 titleSz = titleFont->CalcTextSizeA(titleFont->FontSize, FLT_MAX, 0.0f, titleStr.c_str());
  float titleX = cardPos.x + (kCardW - titleSz.x) * 0.5f;
  float titleY = cardPos.y + 34.0f;
  fg->AddText(titleFont, titleFont->FontSize,
              ImVec2(titleX, titleY), IM_COL32(237, 242, 246, 255), titleStr.c_str());

  ImVec2 subSz = subFont->CalcTextSizeA(subFont->FontSize, FLT_MAX, 0.0f, subtitle);
  float subX = cardPos.x + (kCardW - subSz.x) * 0.5f;
  float subY = titleY + titleSz.y + 10.0f;
  fg->AddText(subFont, subFont->FontSize,
              ImVec2(subX, subY), IM_COL32(61, 74, 99, 255), subtitle);
}

// ---- DrawManagerShell ---------------------------------------------------

static void DrawManagerShell(float winW, float winH) {
  // Sidebar (full height, left edge)
  ImGui::SetCursorPos(ImVec2(0, 0));
  DrawSidebar(kSidebarW, winH);

  // Vertical 1px divider
  {
    ImVec2 wp = ImGui::GetWindowPos();
    ImGui::GetWindowDrawList()->AddLine(
      ImVec2(wp.x + kSidebarW, wp.y),
      ImVec2(wp.x + kSidebarW, wp.y + winH),
      C32(kBorder), 1.0f);
  }

  float cx = kSidebarW + 1.0f;
  float cw = winW - cx;

  // Top header
  ImGui::SetCursorPos(ImVec2(cx, 0));
  DrawTopHeader(cx, cw);

  // Workspace
  ImGui::SetCursorPos(ImVec2(cx, kTopHdrH));
  DrawWorkspace(cw, winH - kTopHdrH);
}

// ---- Entry point --------------------------------------------------------

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

  s_playClicked        = false;
  s_advanceClicked     = false;
  s_startSeasonClicked = false;
  s_menuClicked        = false;

  // Always draw the normal hub behind any overlay.
  DrawAppBackground(winW, winH);
  DrawManagerShell(winW, winH);

  ImGui::End();

  // Modal overlay drawn via foreground drawlist — sits above all ImGui windows.
  if (g_CareerHub.isAdvancing) {
    DrawAdvancingModalOverlay();
  }

  // ---- Deferred action dispatch ----------------------------------------
  // Immediate actions (Test Engine, Main Menu, Play Fixture) fire right away.
  // Advance/Start Season set isAdvancing=true and defer work by one frame so
  // the overlay is guaranteed to appear before processing begins.

  if (g_CareerHub.pendingAction == 0) {
    if (s_playClicked) {
      g_CareerHub.pendingAction = 1;
      printf("[IMGUI MANAGER] Test Engine requested\n");

    } else if (s_menuClicked) {
      g_CareerHub.pendingAction = 2;
      printf("[IMGUI MANAGER] Main Menu requested\n");

    } else if (s_startSeasonClicked && !g_CareerHub.isAdvancing) {
      g_CareerHub.isAdvancing          = true;
      g_CareerHub.pendingAdvanceAction = ADVANCE_START_SEASON;
      g_CareerHub.advanceFramesWaited  = 0;
      printf("[IMGUI MANAGER] Start Season: overlay shown, work deferred one frame\n");

    } else if (s_advanceClicked && !g_CareerHub.isAdvancing) {
      if (g_CareerHub.hasTodayFixture) {
        g_CareerHub.pendingAction = 4; // Play Fixture — no overlay needed
        printf("[IMGUI MANAGER] Play Fixture requested fixture=%d\n",
               g_CareerHub.todayFixture.id);
      } else {
        g_CareerHub.isAdvancing          = true;
        g_CareerHub.pendingAdvanceAction = ADVANCE_NEXT_DAY;
        g_CareerHub.advanceFramesWaited  = 0;
        printf("[IMGUI MANAGER] Advance Day: overlay shown, work deferred one frame\n");
      }
    }

    // Single-frame defer: let the overlay render for one frame before firing work.
    if (g_CareerHub.isAdvancing && g_CareerHub.pendingAdvanceAction != ADVANCE_NONE) {
      if (g_CareerHub.advanceFramesWaited == 0) {
        // First frame with overlay visible — wait one more before firing.
        g_CareerHub.advanceFramesWaited = 1;
      } else {
        // Overlay has been visible at least one frame — fire the work now.
        if (g_CareerHub.pendingAdvanceAction == ADVANCE_NEXT_DAY) {
          g_CareerHub.pendingAction = 3;
          printf("[IMGUI MANAGER] Firing AdvanceDay after overlay frame\n");
        } else if (g_CareerHub.pendingAdvanceAction == ADVANCE_START_SEASON) {
          g_CareerHub.pendingAction = 5;
          printf("[IMGUI MANAGER] Firing StartNextSeason after overlay frame\n");
        }
        g_CareerHub.pendingAdvanceAction = ADVANCE_NONE;
      }
    }
  }
}

// ============================================================================
// Silent match loading overlay
// Covers MenuScene background during LoadingMatchPage silent handoff.
// ============================================================================

void RenderImGuiSilentMatchLoadingOverlay() {
  if (!g_SilentMatchLoadingOverlay) return;

  if (!g_SilentMatchLoadingOverlayLogged) {
    printf("[RENDER] Silent match loading cover active\n");
    g_SilentMatchLoadingOverlayLogged = true;
  }

  ImGuiIO &io = ImGui::GetIO();
  ImDrawList *fg = ImGui::GetForegroundDrawList();

  fg->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 255));

  const char *txt = "Loading match...";
  ImVec2 ts = ImGui::CalcTextSize(txt);
  fg->AddText(
    ImVec2((io.DisplaySize.x - ts.x) * 0.5f, (io.DisplaySize.y - ts.y) * 0.5f),
    IM_COL32(180, 185, 200, 255),
    txt
  );

}

// ============================================================================
// Pre-match lineup presentation
// ============================================================================

static void DrawLineupTable(const char *tableId,
                            const std::vector<PreMatchLineupPlayer> &home,
                            const std::vector<PreMatchLineupPlayer> &away,
                            float padX, ImU32 textCol) {
  ImGui::SetCursorPosX(padX);
  ImGuiTableFlags tflags = ImGuiTableFlags_NoHostExtendX |
                           ImGuiTableFlags_SizingStretchSame;
  if (!ImGui::BeginTable(tableId, 2, tflags)) return;
  ImGui::TableSetupColumn("home", ImGuiTableColumnFlags_WidthStretch, 1.0f);
  ImGui::TableSetupColumn("away", ImGuiTableColumnFlags_WidthStretch, 1.0f);

  int maxRows = (int)std::max(home.size(), away.size());
  ImGui::PushStyleColor(ImGuiCol_Text, textCol);
  for (int i = 0; i < maxRows; i++) {
    ImGui::TableNextRow();
    ImGui::TableSetColumnIndex(0);
    if (i < (int)home.size()) {
      const PreMatchLineupPlayer &p = home[i];
      char buf[128];
      snprintf(buf, sizeof(buf), "%-3s  %2d  %s",
               p.role.empty() ? "" : p.role.c_str(), p.number, p.name.c_str());
      ImGui::TextUnformatted(buf);
    }
    ImGui::TableSetColumnIndex(1);
    if (i < (int)away.size()) {
      const PreMatchLineupPlayer &p = away[i];
      char buf[128];
      snprintf(buf, sizeof(buf), "%-3s  %2d  %s",
               p.role.empty() ? "" : p.role.c_str(), p.number, p.name.c_str());
      ImGui::TextUnformatted(buf);
    }
  }
  ImGui::PopStyleColor();
  ImGui::EndTable();
}

void RenderImGuiPreMatchLineup() {
  if (!g_PreMatchLineup.active) return;

  ImGuiIO &io = ImGui::GetIO();
  float winW = io.DisplaySize.x;
  float winH = io.DisplaySize.y;

  // Persist competition branding so the in-match scoreboard can use it
  // even after g_PreMatchLineup is cleared by GamePage.
  if (!g_PreMatchLineup.competitionLogoPath.empty())
    g_MatchCompetitionLogoPath = g_PreMatchLineup.competitionLogoPath;
  if (!g_PreMatchLineup.competitionName.empty())
    g_MatchCompetitionName = g_PreMatchLineup.competitionName;

  // Track elapsed time for auto-continue.
  if (g_PreMatchLineup.startedAt == 0.0) {
    g_PreMatchLineup.startedAt = ImGui::GetTime();
    g_SilentMatchLoadingOverlay = false; // prematch card is now rendering — drop the cover
  }
  double elapsed = ImGui::GetTime() - g_PreMatchLineup.startedAt;

  // Full-screen dark navy background.
  ImDrawList *bg = ImGui::GetBackgroundDrawList();
  bg->AddRectFilled(ImVec2(0, 0), ImVec2(winW, winH),
                    IM_COL32(12, 18, 35, 255));

  // Centered match-sheet card.
  float cardW = 880.0f;
  float cardH = 860.0f;
  if (cardW > winW * 0.92f) cardW = winW * 0.92f;
  if (cardH > winH * 0.95f) cardH = winH * 0.95f;
  float cardX = (winW - cardW) * 0.5f;
  float cardY = (winH - cardH) * 0.5f;

  // Card background — on BackgroundDrawList so the ImGui window renders on top of it.
  bg->AddRectFilled(ImVec2(cardX, cardY),
                    ImVec2(cardX + cardW, cardY + cardH),
                    IM_COL32(22, 30, 50, 248), 8.0f);
  bg->AddRect(ImVec2(cardX, cardY),
              ImVec2(cardX + cardW, cardY + cardH),
              IM_COL32(60, 80, 130, 200), 8.0f, 0, 1.5f);

  // ImGui window covering the card.
  ImGui::SetNextWindowPos(ImVec2(cardX, cardY), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(cardW, cardH), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
  ImGui::Begin("##prematch_card", nullptr,
               ImGuiWindowFlags_NoTitleBar   |
               ImGuiWindowFlags_NoResize     |
               ImGuiWindowFlags_NoMove       |
               ImGuiWindowFlags_NoCollapse   |
               ImGuiWindowFlags_NoScrollbar  |
               ImGuiWindowFlags_NoScrollWithMouse |
               ImGuiWindowFlags_NoBringToFrontOnFocus);
  ImGui::PopStyleVar(2);

  float pad = 20.0f;
  ImGui::SetCursorPos(ImVec2(pad, pad));

  // ---- Competition header ------------------------------------------------
  {
    float leagueBadgeSz = 28.0f;
    GLuint lt = LoadBadgeTex(g_PreMatchLineup.competitionLogoPath);
    ImGui::SetCursorPosX(pad);
    if (lt) {
      ImGui::Image((ImTextureID)(intptr_t)lt, ImVec2(leagueBadgeSz, leagueBadgeSz));
      ImGui::SameLine(0.0f, 8.0f);
    }
    ImFont *font = g_ManagerFontMedium ? g_ManagerFontMedium : ImGui::GetFont();
    ImGui::PushFont(font);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 200, 240, 255));
    ImGui::TextUnformatted(g_PreMatchLineup.competitionName.c_str());
    ImGui::PopStyleColor();
    ImGui::PopFont();
  }

  float sepY = ImGui::GetCursorPosY() + 4.0f;
  ImGui::GetWindowDrawList()->AddLine(
    ImVec2(cardX + pad, cardY + sepY),
    ImVec2(cardX + cardW - pad, cardY + sepY),
    IM_COL32(60, 80, 130, 180), 1.0f);
  ImGui::SetCursorPosY(sepY + 10.0f);

  // ---- Team row ----------------------------------------------------------
  {
    float badgeSz = 64.0f;
    ImGui::SetCursorPosX(pad);
    if (ImGui::BeginTable("##team_row", 2,
                          ImGuiTableFlags_NoHostExtendX |
                          ImGuiTableFlags_SizingStretchSame)) {
      ImGui::TableSetupColumn("home", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableSetupColumn("away", ImGuiTableColumnFlags_WidthStretch, 1.0f);
      ImGui::TableNextRow();

      ImGui::TableSetColumnIndex(0);
      {
        GLuint hb = LoadBadgeTex(g_PreMatchLineup.homeBadgePath);
        if (hb) {
          ImGui::Image((ImTextureID)(intptr_t)hb, ImVec2(badgeSz, badgeSz));
          ImGui::SameLine(0.0f, 8.0f);
        }
        ImFont *bigFont = g_ManagerFontBold ? g_ManagerFontBold : ImGui::GetFont();
        ImGui::PushFont(bigFont);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 240, 255, 255));
        ImGui::TextUnformatted(g_PreMatchLineup.homeTeamName.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
      }

      ImGui::TableSetColumnIndex(1);
      {
        GLuint ab = LoadBadgeTex(g_PreMatchLineup.awayBadgePath);
        if (ab) {
          ImGui::Image((ImTextureID)(intptr_t)ab, ImVec2(badgeSz, badgeSz));
          ImGui::SameLine(0.0f, 8.0f);
        }
        ImFont *bigFont = g_ManagerFontBold ? g_ManagerFontBold : ImGui::GetFont();
        ImGui::PushFont(bigFont);
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(235, 240, 255, 255));
        ImGui::TextUnformatted(g_PreMatchLineup.awayTeamName.c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
      }

      ImGui::EndTable();
    }
  }

  // ---- Divider -----------------------------------------------------------
  {
    float dy = ImGui::GetCursorPosY();
    ImGui::GetWindowDrawList()->AddLine(
      ImVec2(cardX + pad, cardY + dy),
      ImVec2(cardX + cardW - pad, cardY + dy),
      IM_COL32(60, 80, 130, 180), 1.0f);
    ImGui::SetCursorPosY(dy + 8.0f);
  }

  // ---- Starting XI (table) -----------------------------------------------
  {
    ImFont *listFont = g_ManagerFontSmall ? g_ManagerFontSmall : ImGui::GetFont();
    ImGui::PushFont(listFont);
    DrawLineupTable("##lineup_xi",
                    g_PreMatchLineup.homeStartingXI,
                    g_PreMatchLineup.awayStartingXI,
                    pad, IM_COL32(210, 220, 240, 255));
    ImGui::PopFont();
  }

  // ---- Substitutes -------------------------------------------------------
  if (g_PreMatchLineup.hasBench &&
      (!g_PreMatchLineup.homeBench.empty() || !g_PreMatchLineup.awayBench.empty())) {
    float dy = ImGui::GetCursorPosY();
    ImGui::GetWindowDrawList()->AddLine(
      ImVec2(cardX + pad, cardY + dy),
      ImVec2(cardX + cardW - pad, cardY + dy),
      IM_COL32(60, 80, 130, 120), 1.0f);
    ImGui::SetCursorPosY(dy + 6.0f);
    ImGui::SetCursorPosX(pad);
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 150, 200, 200));
    ImGui::TextUnformatted("Substitutes");
    ImGui::PopStyleColor();

    ImFont *listFont = g_ManagerFontSmall ? g_ManagerFontSmall : ImGui::GetFont();
    ImGui::PushFont(listFont);
    DrawLineupTable("##lineup_bench",
                    g_PreMatchLineup.homeBench,
                    g_PreMatchLineup.awayBench,
                    pad, IM_COL32(170, 185, 215, 200));
    ImGui::PopFont();
  }


  // ---- Footer ------------------------------------------------------------
  {
    float footerY = cardH - 42.0f;
    ImGui::SetCursorPos(ImVec2(pad, footerY));

    const char *footerMsg = (elapsed >= 2.5 || g_PreMatchLineup.continueRequested)
      ? "Starting match..."
      : "Click or press any key to continue";

    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(160, 180, 220, 200));
    float tw = ImGui::CalcTextSize(footerMsg).x;
    ImGui::SetCursorPosX((cardW - tw) * 0.5f);
    ImGui::TextUnformatted(footerMsg);
    ImGui::PopStyleColor();
  }

  // ---- Continue logic (only while waiting for user) ----------------------
  if (!g_PreMatchLineup.continueRequested) {
    bool doContinue = false;
    if (elapsed >= 3.0) doContinue = true;
    if (ImGui::IsMouseClicked(0)) doContinue = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter)  ||
        ImGui::IsKeyPressed(ImGuiKey_Space)  ||
        ImGui::IsKeyPressed(ImGuiKey_Escape))   doContinue = true;

    if (doContinue) {
      printf("[PREMATCH] Continue requested\n");
      g_PreMatchLineup.continueRequested = true;
    }
  }

  ImGui::End();
}
