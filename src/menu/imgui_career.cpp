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
#include <algorithm>
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

  // Glyph range: Basic Latin + Latin-1 Supplement + Euro sign (U+20AC)
  static const ImWchar kGlyphRanges[] = {
    0x0020, 0x00FF,  // Basic Latin + Latin-1 Supplement
    0x20AC, 0x20AC,  // Euro sign €
    0
  };

  auto tryLoad = [](ImFontAtlas *a, const char **paths, float sz) -> ImFont * {
    for (; *paths; ++paths) {
      FILE *f = fopen(*paths, "rb");
      if (!f) continue;
      fclose(f);
      ImFont *font = a->AddFontFromFileTTF(*paths, sz, nullptr, kGlyphRanges);
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
  tactics.clear();
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
    q << "SELECT id, firstname, lastname, role, age, base_stat,"
      << " formationorder, weekly_wage, contract_expiry, player_potential,"
      << " foot, stamina"
      << " FROM players WHERE team_id = " << clubId
      << " ORDER BY"
      << "  CASE WHEN formationorder IS NULL OR formationorder < 0 THEN 999"
      << "       ELSE formationorder END ASC,"
      << "  base_stat DESC LIMIT 50;";
    DatabaseResult *r = GetDB()->Query(q.str());
    for (unsigned int i = 0; i < r->data.size(); i++) {
      Player p;
      p.id             = atoi(DBCell(r, i, 0).c_str());
      p.firstName      = DBCell(r, i, 1);
      p.lastName       = DBCell(r, i, 2);
      p.role           = DBCell(r, i, 3);
      p.age            = DBCell(r, i, 4);
      p.ability        = DBCell(r, i, 5);
      p.baseStat       = p.ability.empty() ? 0.0f : (float)atof(p.ability.c_str());
      std::string foStr = DBCell(r, i, 6);
      p.formationOrder  = foStr.empty() ? -1 : atoi(foStr.c_str());
      std::string wStr  = DBCell(r, i, 7);
      p.weeklywage      = wStr.empty() ? 0 : atoi(wStr.c_str());
      p.contractExpiry  = DBCell(r, i, 8);
      std::string potStr = DBCell(r, i, 9);
      p.potential        = potStr.empty() ? 0 : atoi(potStr.c_str());
      p.foot             = DBCell(r, i, 10);
      std::string stStr  = DBCell(r, i, 11);
      p.stamina          = stStr.empty() ? 0 : atoi(stStr.c_str());
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
       << " fixtures.league_id, COALESCE(fixtures.type,'league')"
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
      todayFixture.type        = DBCell(fr, 0, 8);
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

  // Load tactics from teams.tactics_xml
  {
    tactics.clear();
    // Seed factory defaults first
    tactics["position_offense_depth_factor"]      = 0.9f;
    tactics["position_defense_depth_factor"]      = 0.75f;
    tactics["position_offense_width_factor"]      = 0.9f;
    tactics["position_defense_width_factor"]      = 0.8f;
    tactics["position_offense_midfieldfocus"]     = 0.6f;
    tactics["position_defense_midfieldfocus"]     = 0.5f;
    tactics["position_offense_sidefocus_strength"]  = 0.1f;
    tactics["position_defense_sidefocus_strength"]  = 0.4f;
    tactics["position_offense_microfocus_strength"] = 0.7f;
    tactics["position_defense_microfocus_strength"] = 0.8f;
    tactics["dribble_offensiveness"]              = 0.5f;
    tactics["dribble_centermagnet"]               = 0.5f;

    std::stringstream tq;
    tq << "SELECT tactics_xml FROM teams WHERE id=" << cId << " LIMIT 1;";
    DatabaseResult *tr = GetDB()->Query(tq.str());
    if (tr->data.size() > 0 && !tr->data.at(0).at(0).empty()) {
      std::string xml = tr->data.at(0).at(0);
      size_t pos = 0;
      while (pos < xml.size()) {
        size_t os = xml.find('<', pos);
        if (os == std::string::npos) break;
        size_t oe = xml.find('>', os);
        if (oe == std::string::npos) break;
        std::string key = xml.substr(os + 1, oe - os - 1);
        if (!key.empty() && key[0] != '/') {
          size_t vs = oe + 1;
          std::string closeTag = "</" + key + ">";
          size_t ve = xml.find(closeTag, vs);
          if (ve != std::string::npos) {
            float val = (float)atof(xml.substr(vs, ve - vs).c_str());
            tactics[key] = val;
            pos = ve + closeTag.size();
          } else { pos = oe + 1; }
        } else { pos = oe + 1; }
      }
    }
    delete tr;
  }

  active = true;
}

// ---- Navigation state ---------------------------------------------------

enum e_ManagerPage {
  PAGE_HOME = 0,
  PAGE_INBOX,
  PAGE_NEWS,
  PAGE_CALENDAR,   // new: my-club calendar view under MAIN
  PAGE_SQUAD,
  PAGE_TACTICS,
  PAGE_TRAINING,
  PAGE_STAFF,
  PAGE_SCOUTING,
  PAGE_FINANCES,
  PAGE_TRANSFERS,
  PAGE_COMPETITIONS,
  PAGE_SCHEDULE,   // all-league fixtures browser (was Schedule, now Fixtures in WORLD)
  PAGE_PLAYERS,
  PAGE_TEAMS,
  PAGE_SETTINGS,
  PAGE_COUNT
};

static const char *kPageNames[PAGE_COUNT] = {
  "Home", "Inbox", "News", "Schedule",
  "Squad", "Tactics", "Training", "Staff", "Scouting", "Finances", "Transfers",
  "Competitions", "Fixtures", "Players", "Teams", "Settings"
};

static e_ManagerPage g_activePage = PAGE_HOME;
static bool          s_calInit    = false;
static bool          s_compInit   = false;
static int           s_compCountry = -1;
static int           s_compLeague  = -1;
static bool          s_schedInit     = false;
static bool          s_schedClubInit = false;
static int           s_schedCountry  = -1;
static int           s_schedLeague   = -1;
static int           s_schedClub     = -1;

static void ResetNavState() {
  g_activePage    = PAGE_HOME;
  s_calInit       = false;
  s_compInit      = false;
  s_compCountry   = -1;
  s_compLeague    = -1;
  s_schedInit     = false;
  s_schedClubInit = false;
  s_schedCountry  = -1;
  s_schedLeague   = -1;
  s_schedClub     = -1;
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

// ---- Date formatter -----------------------------------------------------

static std::string FormatFixtureDate(const std::string &iso) {
  if (iso.size() < 10) return iso;
  static const char *kMon[] = {"Jan","Feb","Mar","Apr","May","Jun",
                                "Jul","Aug","Sep","Oct","Nov","Dec"};
  int m = atoi(iso.substr(5, 2).c_str());
  int d = atoi(iso.substr(8, 2).c_str());
  if (m < 1 || m > 12) return iso;
  char buf[16];
  snprintf(buf, sizeof(buf), "%d %s", d, kMon[m - 1]);
  return std::string(buf);
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
  DrawNavItem("Schedule", PAGE_CALENDAR);
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
  DrawNavItem("Fixtures",      PAGE_SCHEDULE);
  DrawNavItem("Competitions",  PAGE_COMPETITIONS);
  DrawNavItem("Players",       PAGE_PLAYERS);
  DrawNavItem("Teams",         PAGE_TEAMS);

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
        if (f.status != "scheduled") continue;
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
  {
    ImVec2 snapPos = ImGui::GetCursorScreenPos();
    DrawLeagueSnapshotCard(ImVec2(rightW, kSnapH));
    // Click anywhere on the League Snapshot card → go to Competitions
    ImVec2 snapMax(snapPos.x + rightW, snapPos.y + kSnapH);
    if (ImGui::IsMouseHoveringRect(snapPos, snapMax, false)) {
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        g_activePage = PAGE_COMPETITIONS;
    }
  }
  ImGui::EndChild();
}

// ---- Star rating renderer -----------------------------------------------

static void DrawStars(float value, float maxValue, ImU32 filledCol) {
  float stars = (maxValue > 0.0f) ? (value / maxValue) * 5.0f : 0.0f;
  stars = std::max(0.0f, std::min(5.0f, stars));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float kW = 9.0f, kH = 7.0f, kGap = 2.0f;
  float lh = ImGui::GetTextLineHeight();
  float oy = (lh - kH) * 0.5f;
  ImU32 emptyCol = IM_COL32(30, 44, 72, 200);
  for (int i = 0; i < 5; i++) {
    float x0 = p.x + (float)i * (kW + kGap);
    float fill = std::max(0.0f, std::min(1.0f, stars - (float)i));
    dl->AddRectFilled(ImVec2(x0, p.y + oy),
                      ImVec2(x0 + kW, p.y + oy + kH), emptyCol, 2.0f);
    if (fill > 0.02f)
      dl->AddRectFilled(ImVec2(x0, p.y + oy),
                        ImVec2(x0 + kW * fill, p.y + oy + kH), filledCol, 2.0f);
  }
  ImGui::Dummy(ImVec2(5.0f * (kW + kGap) - kGap, lh));
}

// ---- Country / league filter cache (shared by Competitions + Schedule) --

struct CountryEntry { int id; std::string name; };
struct LeagueEntry  { int id; int countryId; std::string name; };

static std::vector<CountryEntry> s_filterCountries;
static std::vector<LeagueEntry>  s_filterLeagues;
static bool s_filterCacheLoaded = false;

static void EnsureFilterCache() {
  if (s_filterCacheLoaded) return;
  {
    DatabaseResult *r = GetDB()->Query("SELECT id, name FROM countries ORDER BY name;");
    for (unsigned int i = 0; i < r->data.size(); i++) {
      CountryEntry c;
      c.id   = atoi(r->data[i][0].c_str());
      c.name = r->data[i][1];
      s_filterCountries.push_back(c);
    }
    delete r;
  }
  {
    DatabaseResult *r = GetDB()->Query(
        "SELECT id, country_id, name FROM leagues ORDER BY country_id, name;");
    for (unsigned int i = 0; i < r->data.size(); i++) {
      LeagueEntry l;
      l.id        = atoi(r->data[i][0].c_str());
      l.countryId = atoi(r->data[i][1].c_str());
      l.name      = r->data[i][2];
      s_filterLeagues.push_back(l);
    }
    delete r;
  }
  s_filterCacheLoaded = true;
}

// ---- DrawSquadPage ------------------------------------------------------

// Position slot definitions for 4-3-3
struct PosSlot { int fo; const char *label; };
static const PosSlot kPosSlots[] = {
  {0,"GK"},{1,"LB"},{2,"CB"},{3,"CB"},{4,"RB"},
  {5,"CM"},{6,"CM"},{7,"LM"},{8,"AM"},{9,"RM"},{10,"CF"},
  {11,"S1"},{12,"S2"},{13,"S3"},{14,"S4"},
  {15,"S5"},{16,"S6"},{17,"S7"},{18,"S8"},{19,"S9"}
};
static const int kNumSlots = 20;

static const char *PosLabel(int fo) {
  for (int i = 0; i < kNumSlots; i++)
    if (kPosSlots[i].fo == fo) return kPosSlots[i].label;
  return "—";
}

// Pending POS swap — executed after the frame to avoid iterating while modifying
static int s_swapPlayerA = -1; // player id
static int s_swapFoA     = -1; // its current fo
static int s_swapFoB     = -1; // target fo

static void FlushSquadSwap() {
  if (s_swapPlayerA < 0) return;
  // Find the player currently occupying the target slot (if any)
  int otherPlayerId = -1;
  for (const auto &p : g_CareerHub.players) {
    if (p.id != s_swapPlayerA && p.formationOrder == s_swapFoB) {
      otherPlayerId = p.id;
      break;
    }
  }
  if (otherPlayerId >= 0) {
    std::stringstream q;
    q << "UPDATE players SET formationorder=" << s_swapFoA
      << " WHERE id=" << otherPlayerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str()); delete r;
  }
  {
    std::stringstream q;
    q << "UPDATE players SET formationorder=" << s_swapFoB
      << " WHERE id=" << s_swapPlayerA << ";";
    DatabaseResult *r = GetDB()->Query(q.str()); delete r;
  }
  g_CareerHub.LoadFromDB(g_CareerHub.managerId, g_CareerHub.clubId);
  s_swapPlayerA = s_swapFoA = s_swapFoB = -1;
}

static void DrawSquadPage(float w, float h) {
  FlushSquadSwap();

  const float kPad = 16.0f, kGap = 8.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 14.0f;

  // Header
  const float hdrH   = 80.0f;
  const float badgeS = 44.0f;
  const auto &cl = g_CareerHub.club;
  ImVec2 hdrTop = ImGui::GetCursorScreenPos();
  BeginModernCard("##sqhdr", ImVec2(usW, hdrH));
  // Badge — vertically centred using screen coords
  ImGui::SetCursorScreenPos(ImVec2(hdrTop.x + 18.0f,
                                   hdrTop.y + (hdrH - badgeS) * 0.5f));
  DrawTeamBadge(cl.logoPath, cl.shortName, badgeS);
  // Text block — measure total height then centre the group
  PushMgrFont(g_ManagerFontTitle);
  float titleH = ImGui::GetTextLineHeight();
  PopMgrFont(g_ManagerFontTitle);
  PushMgrFont(g_ManagerFontSmall);
  float subH = ImGui::GetTextLineHeight();
  PopMgrFont(g_ManagerFontSmall);
  float textBlockH = titleH + 4.0f + subH;
  ImGui::SetCursorScreenPos(ImVec2(hdrTop.x + 18.0f + badgeS + 14.0f,
                                   hdrTop.y + (hdrH - textBlockH) * 0.5f));
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

  // Table
  float squadH = usH - hdrH - kGap - 4.0f;
  if (squadH < 60.0f) squadH = 60.0f;
  BeginModernCard("##sqtbl", ImVec2(usW, squadH));
  float tblH = squadH - 32.0f;
  if (tblH < 30.0f) tblH = 30.0f;

  // Sort state — persists across frames, reset when player list changes size
  static std::vector<int> s_squadSortIdx;
  static int  s_squadSortLastSize = -1;

  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5.0f, 4.0f));
  // Col indices: 0=POS 1=Name 2=Position 3=Wage 4=Age 5=Foot 6=Expires 7=Ability 8=Potential
  //              9=CON 10=SHP 11=Morale 12=Happiness 13=L5 14=Apps
  static const int kColAge = 4, kColFoot = 5, kColAbility = 7, kColPotential = 8;
  if (ImGui::BeginTable("##sqfm", 15,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX |
        ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate,
        ImVec2(0, tblH))) {
    ImGui::TableSetupScrollFreeze(2, 1);
    ImGui::TableSetupColumn("POS",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  54.0f);
    ImGui::TableSetupColumn("Name",      ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 180.0f);
    ImGui::TableSetupColumn("Position",  ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  90.0f);
    ImGui::TableSetupColumn("Wage",      ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  80.0f);
    ImGui::TableSetupColumn("Age",       ImGuiTableColumnFlags_WidthFixed,  36.0f);
    ImGui::TableSetupColumn("Foot",      ImGuiTableColumnFlags_WidthFixed,  36.0f);
    ImGui::TableSetupColumn("Expires",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  72.0f);
    ImGui::TableSetupColumn("Ability",   ImGuiTableColumnFlags_WidthFixed,  60.0f);
    ImGui::TableSetupColumn("Potential", ImGuiTableColumnFlags_WidthFixed,  60.0f);
    ImGui::TableSetupColumn("CON",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  36.0f);
    ImGui::TableSetupColumn("SHP",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  36.0f);
    ImGui::TableSetupColumn("Morale",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  60.0f);
    ImGui::TableSetupColumn("Happiness", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  72.0f);
    ImGui::TableSetupColumn("L5",        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  60.0f);
    ImGui::TableSetupColumn("Apps",      ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  36.0f);
    PushMgrFont(g_ManagerFontSmall);
    ImGui::TableHeadersRow();
    PopMgrFont(g_ManagerFontSmall);

    // Build / update sorted index
    int nPlayers = (int)g_CareerHub.players.size();
    if (nPlayers != s_squadSortLastSize) {
      s_squadSortIdx.resize(nPlayers);
      for (int i = 0; i < nPlayers; i++) s_squadSortIdx[i] = i;
      s_squadSortLastSize = nPlayers;
    }
    if (ImGuiTableSortSpecs *specs = ImGui::TableGetSortSpecs()) {
      if (specs->SpecsDirty) {
        // Rebuild index then re-sort
        for (int i = 0; i < nPlayers; i++) s_squadSortIdx[i] = i;
        if (specs->SpecsCount > 0 &&
            specs->Specs[0].SortDirection != ImGuiSortDirection_None) {
          const ImGuiTableColumnSortSpecs &sp = specs->Specs[0];
          bool asc = (sp.SortDirection == ImGuiSortDirection_Ascending);
          std::sort(s_squadSortIdx.begin(), s_squadSortIdx.end(),
            [&](int a, int b) {
              const auto &pa = g_CareerHub.players[a];
              const auto &pb = g_CareerHub.players[b];
              float va = 0.0f, vb = 0.0f;
              std::string sa, sb;
              if (sp.ColumnIndex == kColAge) {
                va = (float)atoi(pa.age.c_str());
                vb = (float)atoi(pb.age.c_str());
              } else if (sp.ColumnIndex == kColFoot) {
                sa = pa.foot; sb = pb.foot;
                return asc ? (sa < sb) : (sa > sb);
              } else if (sp.ColumnIndex == kColAbility) {
                va = pa.baseStat; vb = pb.baseStat;
              } else if (sp.ColumnIndex == kColPotential) {
                va = (float)pa.potential; vb = (float)pb.potential;
              }
              return asc ? (va < vb) : (va > vb);
            });
        }
        specs->SpecsDirty = false;
      }
    }

    const ImU32 kGoldU = C32(kGold);
    const ImU32 kBlueU = IM_COL32(100, 160, 220, 220);
    const ImU32 kDimU  = C32(kTextDim);

    PushMgrFont(g_ManagerFontSmall);
    for (int si = 0; si < nPlayers; si++) {
      const auto &p = g_CareerHub.players[s_squadSortIdx[si]];
      ImGui::TableNextRow(0, 28.0f);

      // POS — dropdown button
      ImGui::TableSetColumnIndex(0);
      {
        const char *posLbl = PosLabel(p.formationOrder);
        bool isSub  = (p.formationOrder >= 11);
        bool isXI   = (p.formationOrder >= 0 && p.formationOrder <= 10);
        ImU32 badgeBg  = isXI  ? C32(kViolet)
                       : isSub ? IM_COL32(30,60,100,220)
                               : IM_COL32(25,35,58,180);
        ImU32 badgeTxt = IM_COL32(220,225,235,255);

        ImVec2 cp = ImGui::GetCursorScreenPos();
        const float bW = 42.0f, bH = 18.0f;
        float cy = cp.y + (28.0f - bH) * 0.5f;
        ImDrawList *dl = ImGui::GetWindowDrawList();

        // Badge background (clickable via InvisibleButton)
        char btnId[32]; snprintf(btnId, sizeof(btnId), "##pos_%d", p.id);
        ImGui::SetCursorScreenPos(ImVec2(cp.x, cy));
        bool clicked = ImGui::InvisibleButton(btnId, ImVec2(bW, bH));
        dl->AddRectFilled(ImVec2(cp.x, cy), ImVec2(cp.x+bW, cy+bH), badgeBg, 4.0f);
        ImVec2 tsz = g_ManagerFontSmall
            ? g_ManagerFontSmall->CalcTextSizeA(11.0f, FLT_MAX, 0.f, posLbl)
            : ImGui::CalcTextSize(posLbl);
        dl->AddText(g_ManagerFontSmall, 11.0f,
                    ImVec2(cp.x + (bW - tsz.x)*0.5f, cy + (bH - tsz.y)*0.5f),
                    badgeTxt, posLbl);

        if (clicked) {
          char popId[32]; snprintf(popId, sizeof(popId), "##posdd_%d", p.id);
          ImGui::OpenPopup(popId);
        }
        char popId2[32]; snprintf(popId2, sizeof(popId2), "##posdd_%d", p.id);
        if (ImGui::BeginPopup(popId2)) {
          PushMgrFont(g_ManagerFontSmall);
          ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
          ImGui::TextUnformatted("Assign position:");
          ImGui::PopStyleColor();
          ImGui::Separator();
          for (int si = 0; si < kNumSlots; si++) {
            bool isCurrent = (kPosSlots[si].fo == p.formationOrder);
            if (isCurrent) ImGui::PushStyleColor(ImGuiCol_Text, kGold);
            char selId[32];
            snprintf(selId, sizeof(selId), "%s##fo%d", kPosSlots[si].label, kPosSlots[si].fo);
            if (ImGui::Selectable(selId, isCurrent,
                                  0, ImVec2(60, 0))) {
              if (!isCurrent) {
                s_swapPlayerA = p.id;
                s_swapFoA     = p.formationOrder;
                s_swapFoB     = kPosSlots[si].fo;
              }
              ImGui::CloseCurrentPopup();
            }
            if (isCurrent) ImGui::PopStyleColor();
          }
          PopMgrFont(g_ManagerFontSmall);
          ImGui::EndPopup();
        }
      }

      // Name
      ImGui::TableSetColumnIndex(1);
      {
        std::string name = p.firstName.empty() ? p.lastName
                         : (p.lastName.empty() ? p.firstName
                         : p.firstName + " " + p.lastName);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
        ImGui::TextUnformatted(name.c_str());
        ImGui::PopStyleColor();
      }

      // Position (role)
      ImGui::TableSetColumnIndex(2);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted(p.role.c_str());
      ImGui::PopStyleColor();

      // Wage
      ImGui::TableSetColumnIndex(3);
      if (p.weeklywage > 0) {
        char wbuf[32];
        if (p.weeklywage >= 1000)
          snprintf(wbuf, sizeof(wbuf), "\xe2\x82\xac%d,%03d p/w",
                   p.weeklywage/1000, p.weeklywage%1000);
        else
          snprintf(wbuf, sizeof(wbuf), "\xe2\x82\xac%d p/w", p.weeklywage);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
        ImGui::TextUnformatted(wbuf);
        ImGui::PopStyleColor();
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kDimU);
        ImGui::TextUnformatted("\xe2\x80\x94");
        ImGui::PopStyleColor();
      }

      // Age (col 4)
      ImGui::TableSetColumnIndex(4);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted(p.age.c_str());
      ImGui::PopStyleColor();

      // Foot (col 5) — "L" → left foot badge, "R" → right foot badge
      ImGui::TableSetColumnIndex(5);
      {
        bool isLeft = (!p.foot.empty() && (p.foot[0] == 'L' || p.foot[0] == 'l'));
        bool isRight = (!p.foot.empty() && (p.foot[0] == 'R' || p.foot[0] == 'r'));
        if (isLeft || isRight) {
          ImU32 footCol = isLeft ? IM_COL32(80, 160, 255, 200) : IM_COL32(80, 200, 120, 200);
          ImVec2 cp = ImGui::GetCursorScreenPos();
          ImDrawList *fdl = ImGui::GetWindowDrawList();
          fdl->AddRectFilled(cp, ImVec2(cp.x + 22.0f, cp.y + 14.0f), footCol, 3.0f);
          const char *fl = isLeft ? "L" : "R";
          ImVec2 fts = ImGui::CalcTextSize(fl);
          fdl->AddText(ImVec2(cp.x + (22.0f - fts.x) * 0.5f, cp.y + (14.0f - fts.y) * 0.5f),
                       IM_COL32(255,255,255,230), fl);
          ImGui::Dummy(ImVec2(22.0f, 14.0f));
        } else {
          ImGui::PushStyleColor(ImGuiCol_Text, kDimU);
          ImGui::TextUnformatted("\xe2\x80\x94");
          ImGui::PopStyleColor();
        }
      }

      // Contract expiry (col 6) — format YYYY-MM-DD → D/M/YYYY
      ImGui::TableSetColumnIndex(6);
      {
        std::string exp = p.contractExpiry;
        if (exp.size() >= 10) {
          int yr = atoi(exp.substr(0,4).c_str());
          int mo = atoi(exp.substr(5,2).c_str());
          int dy = atoi(exp.substr(8,2).c_str());
          char ebuf[16]; snprintf(ebuf, sizeof(ebuf), "%d/%d/%d", dy, mo, yr);
          exp = ebuf;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
        ImGui::TextUnformatted(exp.empty() ? "\xe2\x80\x94" : exp.c_str());
        ImGui::PopStyleColor();
      }

      // Ability stars (col 7)
      ImGui::TableSetColumnIndex(7);
      DrawStars(p.baseStat, 1.0f, kGoldU);

      // Potential stars (col 8)
      ImGui::TableSetColumnIndex(8);
      DrawStars((float)p.potential, 200.0f, kBlueU);

      // Placeholders
      auto placeholder = [&](int col) {
        ImGui::TableSetColumnIndex(col);
        ImGui::PushStyleColor(ImGuiCol_Text, kDimU);
        ImGui::TextUnformatted("\xe2\x80\x94");
        ImGui::PopStyleColor();
      };
      placeholder(9);  // CON
      placeholder(10); // SHP
      placeholder(11); // Morale
      placeholder(12); // Happiness
      placeholder(13); // L5
      placeholder(14); // Apps
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

// ---- DrawCalendarPage ---------------------------------------------------
// Shows only the user's club fixtures in a month-grid calendar.

static int s_calYear  = 0;
static int s_calMonth = 0; // 1-12

static int DaysInMonth(int year, int month) {
  // Use day-0 of next month trick
  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon  = month;   // 0-based; month here is 1-based next month = 0-based current+1
  t.tm_mday = 0;
  mktime(&t);
  return t.tm_mday;
}

static void DrawCalendarPage(float w, float h) {
  // Always reset to current career month on entry (tracked via s_calInit cleared on page change)
  if (!s_calInit) {
    const std::string &cd = g_CareerHub.currentDate;
    if (cd.size() >= 7) {
      s_calYear  = atoi(cd.substr(0, 4).c_str());
      s_calMonth = atoi(cd.substr(5, 2).c_str());
    } else {
      s_calYear = 2026; s_calMonth = 7;
    }
    s_calInit = true;
  }

  // Build user-fixture map: "YYYY-MM-DD" → index into g_CareerHub.fixtures
  const std::string &myShort = g_CareerHub.club.shortName;
  std::map<std::string, int> calFixIdx;
  for (int i = 0; i < (int)g_CareerHub.fixtures.size(); i++) {
    const auto &f = g_CareerHub.fixtures[i];
    if (f.home == myShort || f.away == myShort)
      calFixIdx[f.fixtureDate] = i;
  }

  static const char *kMonths[] = {
    "January","February","March","April","May","June",
    "July","August","September","October","November","December"
  };
  static const char *kDays[] = { "MON","TUE","WED","THU","FRI","SAT","SUN" };

  const float kPad = 16.0f, kGap = 6.0f;
  ImDrawList *wdl  = ImGui::GetWindowDrawList();

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 20.0f;

  // ---- Month navigation header (custom-drawn, buttons vertically centred) --
  const float kHdrH  = 52.0f;
  const float kBtnW  = 90.0f, kBtnH = 32.0f;
  const float kBtnML = 16.0f; // left margin for Prev button

  ImVec2 hdrPos = ImGui::GetCursorScreenPos();
  // Background + border
  wdl->AddRectFilled(hdrPos, ImVec2(hdrPos.x+usW, hdrPos.y+kHdrH),
                     C32(kBgCard), 8.0f);
  wdl->AddRect(hdrPos, ImVec2(hdrPos.x+usW, hdrPos.y+kHdrH),
               C32(kBorder), 8.0f, 0, 1.0f);

  float btnY = hdrPos.y + (kHdrH - kBtnH) * 0.5f;

  // Prev button
  ImVec2 prevMin(hdrPos.x + kBtnML, btnY);
  ImVec2 prevMax(prevMin.x + kBtnW, prevMin.y + kBtnH);
  ImGui::SetCursorScreenPos(prevMin);
  bool prevClicked = ImGui::InvisibleButton("##cal_prev", ImVec2(kBtnW, kBtnH));
  bool prevHov     = ImGui::IsItemHovered();
  if (prevHov)
    wdl->AddRectFilled(prevMin, prevMax, IM_COL32(50,70,120,180), 6.0f);
  else
    wdl->AddRectFilled(prevMin, prevMax, IM_COL32(30,42,72,140), 6.0f);
  wdl->AddRect(prevMin, prevMax, IM_COL32(60,80,130,160), 6.0f, 0, 1.0f);
  {
    PushMgrFont(g_ManagerFontSmall);
    const char *lbl = "< Prev";
    ImVec2 ts = ImGui::CalcTextSize(lbl);
    wdl->AddText(ImVec2(prevMin.x + (kBtnW - ts.x)*0.5f,
                        prevMin.y + (kBtnH - ts.y)*0.5f),
                 prevHov ? IM_COL32(200,220,255,255) : IM_COL32(140,165,215,210), lbl);
    PopMgrFont(g_ManagerFontSmall);
  }
  if (prevClicked) {
    s_calMonth--;
    if (s_calMonth < 1) { s_calMonth = 12; s_calYear--; }
  }

  // Month + Year label (centred)
  {
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%s %d", kMonths[s_calMonth - 1], s_calYear);
    PushMgrFont(g_ManagerFontBold);
    ImVec2 ts = ImGui::CalcTextSize(hdr);
    wdl->AddText(ImVec2(hdrPos.x + (usW - ts.x)*0.5f,
                        hdrPos.y + (kHdrH - ts.y)*0.5f),
                 C32(kBlue), hdr);
    PopMgrFont(g_ManagerFontBold);
  }

  // Next button (right-aligned, mirrored margin)
  ImVec2 nextMin(hdrPos.x + usW - kBtnML - kBtnW, btnY);
  ImVec2 nextMax(nextMin.x + kBtnW, nextMin.y + kBtnH);
  ImGui::SetCursorScreenPos(nextMin);
  bool nextClicked = ImGui::InvisibleButton("##cal_next", ImVec2(kBtnW, kBtnH));
  bool nextHov     = ImGui::IsItemHovered();
  if (nextHov)
    wdl->AddRectFilled(nextMin, nextMax, IM_COL32(50,70,120,180), 6.0f);
  else
    wdl->AddRectFilled(nextMin, nextMax, IM_COL32(30,42,72,140), 6.0f);
  wdl->AddRect(nextMin, nextMax, IM_COL32(60,80,130,160), 6.0f, 0, 1.0f);
  {
    PushMgrFont(g_ManagerFontSmall);
    const char *lbl = "Next >";
    ImVec2 ts = ImGui::CalcTextSize(lbl);
    wdl->AddText(ImVec2(nextMin.x + (kBtnW - ts.x)*0.5f,
                        nextMin.y + (kBtnH - ts.y)*0.5f),
                 nextHov ? IM_COL32(200,220,255,255) : IM_COL32(140,165,215,210), lbl);
    PopMgrFont(g_ManagerFontSmall);
  }
  if (nextClicked) {
    s_calMonth++;
    if (s_calMonth > 12) { s_calMonth = 1; s_calYear++; }
  }

  // Advance cursor past header
  ImGui::SetCursorScreenPos(ImVec2(hdrPos.x, hdrPos.y + kHdrH));
  ImGui::Dummy(ImVec2(usW, kGap));

  // ---- Calendar grid --------------------------------------------------
  float gridH = usH - kHdrH - kGap * 2.0f;
  if (gridH < 120.0f) gridH = 120.0f;

  // Compute month layout
  struct tm t0 = {};
  t0.tm_year = s_calYear - 1900;
  t0.tm_mon  = s_calMonth - 1;
  t0.tm_mday = 1;
  mktime(&t0);
  int firstDow  = (t0.tm_wday + 6) % 7; // 0=Mon … 6=Sun
  int daysInMon = DaysInMonth(s_calYear, s_calMonth);
  int numWeeks  = (firstDow + daysInMon + 6) / 7;

  const float kDayHdrH = 24.0f;
  float cellW = usW / 7.0f;
  float cellH = (gridH - kDayHdrH) / (float)numWeeks;
  if (cellH < 100.0f) cellH = 100.0f;

  ImVec2 gridOrigin = ImGui::GetCursorScreenPos();

  // Day-of-week header row
  PushMgrFont(g_ManagerFontSmall);
  for (int d = 0; d < 7; d++) {
    float dx = gridOrigin.x + d * cellW;
    // Weekend columns slightly dimmer
    ImU32 dhCol = (d >= 5) ? IM_COL32(110,125,170,160) : IM_COL32(130,150,195,200);
    ImVec2 ts = ImGui::CalcTextSize(kDays[d]);
    wdl->AddText(ImVec2(dx + (cellW - ts.x)*0.5f,
                        gridOrigin.y + (kDayHdrH - ts.y)*0.5f),
                 dhCol, kDays[d]);
  }
  PopMgrFont(g_ManagerFontSmall);

  float rowY      = gridOrigin.y + kDayHdrH;
  std::string todayStr = g_CareerHub.currentDate;

  // Precompute small font line height once
  PushMgrFont(g_ManagerFontSmall);
  float smLineH = ImGui::CalcTextSize("X").y;
  PopMgrFont(g_ManagerFontSmall);
  PushMgrFont(g_ManagerFontBold);
  float bdLineH = ImGui::CalcTextSize("X").y;
  PopMgrFont(g_ManagerFontBold);

  for (int week = 0; week < numWeeks; week++) {
    for (int dow = 0; dow < 7; dow++) {
      int cell = week * 7 + dow;
      int day  = cell - firstDow + 1;
      float cx = gridOrigin.x + dow * cellW;
      float cy = rowY + week * cellH;
      const float kCellPad = 3.0f;
      ImVec2 cMin(cx + kCellPad, cy + kCellPad);
      ImVec2 cMax(cx + cellW - kCellPad, cy + cellH - kCellPad);
      float  cW = cMax.x - cMin.x;

      if (day < 1 || day > daysInMon) {
        wdl->AddRectFilled(cMin, cMax, IM_COL32(12,14,22,60), 7.0f);
        continue;
      }

      char dateBuf[16];
      snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", s_calYear, s_calMonth, day);
      std::string dateStr(dateBuf);

      bool isToday  = (dateStr == todayStr);
      bool hasMatch = (calFixIdx.count(dateStr) > 0);

      // Cell background
      ImU32 bgCol = hasMatch ? IM_COL32(18, 30, 60, 240)
                  : isToday  ? IM_COL32(24, 26, 50, 220)
                             : IM_COL32(14, 18, 32, 190);
      wdl->AddRectFilled(cMin, cMax, bgCol, 7.0f);

      // Border
      if (isToday)
        wdl->AddRect(cMin, cMax, IM_COL32(90,140,255,200), 7.0f, 0, 1.5f);
      else if (hasMatch)
        wdl->AddRect(cMin, cMax, IM_COL32(55,80,140,120), 7.0f, 0, 1.0f);
      else
        wdl->AddRect(cMin, cMax, IM_COL32(30,38,60,80), 7.0f, 0, 1.0f);

      // Day number (top-left)
      char dayBuf[8];
      snprintf(dayBuf, sizeof(dayBuf), "%d", day);
      PushMgrFont(g_ManagerFontSmall);
      ImU32 dayNumCol = isToday   ? IM_COL32(120,175,255,255)
                      : hasMatch  ? IM_COL32(200,215,245,220)
                                  : IM_COL32(100,115,155,180);
      wdl->AddText(ImVec2(cMin.x + 7.0f, cMin.y + 5.0f), dayNumCol, dayBuf);
      PopMgrFont(g_ManagerFontSmall);

      if (!hasMatch) continue;

      const auto &f  = g_CareerHub.fixtures[calFixIdx[dateStr]];
      bool isHome    = (f.home == myShort);
      bool played    = (f.status == "played");

      // Matchday label (top-right)
      if (!f.matchday.empty()) {
        char mdBuf[16];
        snprintf(mdBuf, sizeof(mdBuf), "MD %s", f.matchday.c_str());
        PushMgrFont(g_ManagerFontSmall);
        ImVec2 mds = ImGui::CalcTextSize(mdBuf);
        wdl->AddText(ImVec2(cMax.x - mds.x - 6.0f, cMin.y + 5.0f),
                     IM_COL32(80,100,150,160), mdBuf);
        PopMgrFont(g_ManagerFontSmall);
      }

      // Thin separator below day number
      float sepY = cMin.y + smLineH + 9.0f;
      wdl->AddLine(ImVec2(cMin.x + 6.0f, sepY), ImVec2(cMax.x - 6.0f, sepY),
                   IM_COL32(50,70,110,100), 1.0f);

      // ── H/A pill — left-aligned, immediately below separator ────────────
      float pillY;
      {
        PushMgrFont(g_ManagerFontSmall);
        const char *haPill = isHome ? "HOME" : "AWAY";
        ImU32 haColor = isHome ? IM_COL32(30,100,200,230) : IM_COL32(165,45,45,230);
        ImVec2 haTs = ImGui::CalcTextSize(haPill);
        float  haW  = haTs.x + 14.0f, haH = haTs.y + 5.0f;
        float  pillX = cMin.x + 7.0f;
        pillY = sepY + 7.0f;
        wdl->AddRectFilled(ImVec2(pillX, pillY),
                           ImVec2(pillX + haW, pillY + haH), haColor, 4.0f);
        wdl->AddText(ImVec2(pillX + 7.0f, pillY + 2.5f),
                     IM_COL32(255,255,255,240), haPill);
        pillY += haH;
        PopMgrFont(g_ManagerFontSmall);
      }

      // ── Badge + opponent name — centred horizontally ─────────────────────
      const float kBadgeS = 28.0f;
      float badgeY = pillY + 6.0f;

      const std::string &oppFull = isHome
        ? (f.awayFull.empty() ? f.away : f.awayFull)
        : (f.homeFull.empty() ? f.home : f.homeFull);
      const std::string &oppLogo = isHome ? f.awayLogo : f.homeLogo;
      GLuint badge = LoadBadgeTex(oppLogo);

      // Measure name to compute total block width for centering
      PushMgrFont(g_ManagerFontSmall);
      float nameMaxW = cW - kBadgeS - 10.0f - 8.0f; // badge + gap + padding

      // Build display string (split to two lines if needed)
      std::string nameLine1 = oppFull, nameLine2;
      bool twoLine = false;
      if (ImGui::CalcTextSize(nameLine1.c_str()).x > nameMaxW) {
        size_t sp = oppFull.rfind(' ');
        if (sp != std::string::npos) {
          nameLine1 = oppFull.substr(0, sp);
          nameLine2 = oppFull.substr(sp + 1);
          twoLine   = true;
          while (nameLine1.size() > 2 && ImGui::CalcTextSize(nameLine1.c_str()).x > nameMaxW)
            nameLine1.pop_back();
          while (nameLine2.size() > 2 && ImGui::CalcTextSize(nameLine2.c_str()).x > nameMaxW)
            nameLine2.pop_back();
        } else {
          while (nameLine1.size() > 2 && ImGui::CalcTextSize(nameLine1.c_str()).x > nameMaxW)
            nameLine1.pop_back();
        }
      }

      float nameBlockH = twoLine ? smLineH * 2.0f + 2.0f : smLineH;
      float blockH     = kBadgeS > nameBlockH ? kBadgeS : nameBlockH;

      // Total block width: badge + gap + widest name line
      float w1 = ImGui::CalcTextSize(nameLine1.c_str()).x;
      float w2 = twoLine ? ImGui::CalcTextSize(nameLine2.c_str()).x : 0.0f;
      float nameW  = w1 > w2 ? w1 : w2;
      float blockW = kBadgeS + 8.0f + nameW;
      if (blockW > cW - 8.0f) blockW = cW - 8.0f;

      float blockX = cMin.x + (cW - blockW) * 0.5f;

      // Badge
      float bx = blockX;
      float by = badgeY + (blockH - kBadgeS) * 0.5f;
      if (badge) {
        wdl->AddImage((ImTextureID)(intptr_t)badge,
                      ImVec2(bx, by), ImVec2(bx + kBadgeS, by + kBadgeS));
      } else {
        wdl->AddCircleFilled(ImVec2(bx + kBadgeS*0.5f, by + kBadgeS*0.5f),
                             kBadgeS * 0.47f, IM_COL32(40,55,95,200));
        wdl->AddCircle(ImVec2(bx + kBadgeS*0.5f, by + kBadgeS*0.5f),
                       kBadgeS * 0.47f, IM_COL32(60,80,130,180), 20, 1.0f);
      }

      // Name (right of badge, vertically centred to block)
      float nx  = blockX + kBadgeS + 8.0f;
      float ny1 = twoLine ? badgeY + (blockH - nameBlockH)*0.5f
                          : badgeY + (blockH - smLineH)*0.5f;
      wdl->AddText(ImVec2(nx, ny1), IM_COL32(220,230,255,230), nameLine1.c_str());
      if (twoLine)
        wdl->AddText(ImVec2(nx, ny1 + smLineH + 2.0f),
                     IM_COL32(175,190,225,185), nameLine2.c_str());
      PopMgrFont(g_ManagerFontSmall);

      // Score or "vs" centred on row
      float scorY = badgeY + blockH + 6.0f;
      if (scorY + bdLineH < cMax.y - 18.0f) {
        PushMgrFont(g_ManagerFontBold);
        const char *scoreTxt = played ? f.score.c_str() : "vs";
        ImVec2 sts = ImGui::CalcTextSize(scoreTxt);
        ImU32  stCol = played ? IM_COL32(255,220,60,240) : IM_COL32(90,110,155,190);
        wdl->AddText(ImVec2(cMin.x + (cW - sts.x)*0.5f, scorY), stCol, scoreTxt);
        PopMgrFont(g_ManagerFontBold);
      }

      // Competition name (bottom, centred, dimmed)
      if (!f.league.empty()) {
        PushMgrFont(g_ManagerFontSmall);
        std::string compDisp = f.league;
        float compMaxW = cW - 10.0f;
        while (compDisp.size() > 3 &&
               ImGui::CalcTextSize(compDisp.c_str()).x > compMaxW)
          compDisp.pop_back();
        ImVec2 cs = ImGui::CalcTextSize(compDisp.c_str());
        float compY = cMax.y - smLineH - 5.0f;
        wdl->AddText(ImVec2(cMin.x + (cW - cs.x)*0.5f, compY),
                     IM_COL32(70,90,135,150), compDisp.c_str());
        PopMgrFont(g_ManagerFontSmall);
      }
    }
  }

  // Reserve layout space for the grid
  ImGui::SetCursorScreenPos(ImVec2(gridOrigin.x, gridOrigin.y));
  ImGui::Dummy(ImVec2(usW, kDayHdrH + numWeeks * cellH));
}

// ---- DrawSchedulePage ---------------------------------------------------

static void DrawSchedulePage(float w, float h) {
  EnsureFilterCache();

  // Auto-select user's country + league on every fresh visit (reset by prevPage logic)
  if (!s_schedInit && !s_filterLeagues.empty()) {
    s_schedCountry = -1; s_schedLeague = -1; s_schedClub = -1;
    const std::string &myLeague = g_CareerHub.club.leagueName;
    if (!myLeague.empty()) {
      for (int i = 0; i < (int)s_filterLeagues.size(); i++) {
        if (s_filterLeagues[i].name == myLeague) {
          s_schedLeague = i;
          int cid = s_filterLeagues[i].countryId;
          for (int j = 0; j < (int)s_filterCountries.size(); j++) {
            if (s_filterCountries[j].id == cid) { s_schedCountry = j; break; }
          }
          break;
        }
      }
    }
    s_schedInit = true;
  }

  const float kPad = 16.0f, kGap = 8.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 20.0f;
  const std::string &sn = g_CareerHub.club.shortName;

  // Determine active league name filter
  std::string filterLeagueName;
  if (s_schedLeague >= 0) filterLeagueName = s_filterLeagues[s_schedLeague].name;

  // Collect unique full team names for club dropdown
  std::vector<std::string> clubNames;
  for (const auto &f : g_CareerHub.fixtures) {
    if (!filterLeagueName.empty() && f.league != filterLeagueName) continue;
    auto addIfNew = [&](const std::string &n) {
      if (n.empty()) return;
      for (const auto &c : clubNames) if (c == n) return;
      clubNames.push_back(n);
    };
    addIfNew(f.homeFull.empty() ? f.home : f.homeFull);
    addIfNew(f.awayFull.empty() ? f.away : f.awayFull);
  }
  if (s_schedClub >= (int)clubNames.size()) s_schedClub = -1;

  // Auto-select user's club now that clubNames is available for this league
  if (!s_schedClubInit && !clubNames.empty()) {
    const std::string &myClub = g_CareerHub.club.name;
    if (!myClub.empty()) {
      for (int i = 0; i < (int)clubNames.size(); i++) {
        if (clubNames[i] == myClub) { s_schedClub = i; break; }
      }
    }
    s_schedClubInit = true;
  }

  std::string filterClubName;
  if (s_schedClub >= 0 && s_schedClub < (int)clubNames.size())
    filterClubName = clubNames[s_schedClub];

  // ---- Filter bar (centred vertically) ------------------------------------
  const float fbarH = 56.0f;
  ImVec2 sfbarTop = ImGui::GetCursorScreenPos(); // capture before BeginChild
  BeginModernCard("##sched_fbar", ImVec2(usW, fbarH));
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
  {
    float comboH = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(sfbarTop.x + 22.0f,
                                     sfbarTop.y + (fbarH - comboH) * 0.5f));
  }

  // Country
  ImGui::SetNextItemWidth(130.0f);
  {
    std::string lbl = (s_schedCountry < 0) ? "Country"
                                           : s_filterCountries[s_schedCountry].name;
    if (ImGui::BeginCombo("##sch_country", lbl.c_str())) {
      if (ImGui::Selectable("All countries", s_schedCountry < 0))
        s_schedCountry = s_schedLeague = s_schedClub = -1;
      for (int i = 0; i < (int)s_filterCountries.size(); i++) {
        bool sel = (s_schedCountry == i);
        if (ImGui::Selectable(s_filterCountries[i].name.c_str(), sel))
          { s_schedCountry = i; s_schedLeague = s_schedClub = -1; }
      }
      ImGui::EndCombo();
    }
  }
  ImGui::SameLine();

  // League
  ImGui::SetNextItemWidth(180.0f);
  {
    bool hasCountry = (s_schedCountry >= 0);
    int  filterCid  = hasCountry ? s_filterCountries[s_schedCountry].id : -1;
    std::string lbl = (s_schedLeague < 0) ? "League"
                                          : s_filterLeagues[s_schedLeague].name;
    if (ImGui::BeginCombo("##sch_league", lbl.c_str())) {
      if (ImGui::Selectable("All leagues", s_schedLeague < 0))
        s_schedLeague = s_schedClub = -1;
      for (int i = 0; i < (int)s_filterLeagues.size(); i++) {
        if (hasCountry && s_filterLeagues[i].countryId != filterCid) continue;
        bool sel = (s_schedLeague == i);
        if (ImGui::Selectable(s_filterLeagues[i].name.c_str(), sel))
          { s_schedLeague = i; s_schedClub = -1; }
      }
      ImGui::EndCombo();
    }
  }
  ImGui::SameLine();

  // Club (only shown once a league is selected; always starts at "All clubs")
  if (s_schedLeague >= 0) {
    ImGui::SetNextItemWidth(170.0f);
    std::string lbl = (s_schedClub < 0) ? "All clubs" : filterClubName;
    if (ImGui::BeginCombo("##sch_club", lbl.c_str())) {
      if (ImGui::Selectable("All clubs", s_schedClub < 0)) s_schedClub = -1;
      for (int i = 0; i < (int)clubNames.size(); i++) {
        bool sel = (s_schedClub == i);
        if (ImGui::Selectable(clubNames[i].c_str(), sel)) s_schedClub = i;
      }
      ImGui::EndCombo();
    }
  }

  ImGui::PopStyleVar(2);
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();

  ImGui::Dummy(ImVec2(0, kGap));

  // Collect leagues to render (filtered)
  std::vector<std::string> leagues;
  for (const auto &f : g_CareerHub.fixtures) {
    if (!filterLeagueName.empty() && f.league != filterLeagueName) continue;
    bool found = false;
    for (const auto &l : leagues) if (l == f.league) { found = true; break; }
    if (!found) leagues.push_back(f.league);
  }

  float schedH = usH - fbarH - kGap - 4.0f;
  if (schedH < 60.0f) schedH = 60.0f;
  BeginModernCard("##sched_outer", ImVec2(usW, schedH));
  ImGui::BeginChild("##sc_scroll", ImVec2(0, schedH - 38.0f), false);

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

    // Count visible rows in this league
    int cnt = 0;
    for (const auto &f : g_CareerHub.fixtures) {
      if (f.league != lg) continue;
      if (!filterClubName.empty()) {
        const std::string &hf = f.homeFull.empty() ? f.home : f.homeFull;
        const std::string &af = f.awayFull.empty() ? f.away : f.awayFull;
        if (hf != filterClubName && af != filterClubName) continue;
      }
      cnt++;
    }
    if (cnt == 0) continue;

    float rH    = 30.0f;
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
      ImGui::TableSetupColumn("MD",    ImGuiTableColumnFlags_WidthFixed,  32.0f);
      ImGui::TableSetupColumn("Rd",    ImGuiTableColumnFlags_WidthFixed,  26.0f);
      ImGui::TableSetupColumn("Home",  ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Away",  ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Date",  ImGuiTableColumnFlags_WidthFixed,  60.0f);
      ImGui::TableSetupColumn("Score", ImGuiTableColumnFlags_WidthFixed,  52.0f);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::TableHeadersRow();

      for (const auto &f : g_CareerHub.fixtures) {
        if (f.league != lg) continue;
        const std::string &hf = f.homeFull.empty() ? f.home : f.homeFull;
        const std::string &af = f.awayFull.empty() ? f.away : f.awayFull;
        if (!filterClubName.empty() && hf != filterClubName && af != filterClubName)
          continue;
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
        DrawTeamLabel(f.homeLogo, hf, 20.0f);

        ImGui::TableSetColumnIndex(3);
        DrawTeamLabel(f.awayLogo, af, 20.0f);

        ImGui::TableSetColumnIndex(4);
        {
          ImU32 dc = (f.status == "played") ? C32(kTextDim) : C32(kTextSec);
          ImGui::PushStyleColor(ImGuiCol_Text, dc);
          ImGui::TextUnformatted(FormatFixtureDate(f.fixtureDate).c_str());
          ImGui::PopStyleColor();
        }

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
  EnsureFilterCache();

  // Auto-select user's league on first visit (or after career load)
  if (!s_compInit && !s_filterLeagues.empty()) {
    const std::string &myLeague = g_CareerHub.club.leagueName;
    if (!myLeague.empty()) {
      for (int i = 0; i < (int)s_filterLeagues.size(); i++) {
        if (s_filterLeagues[i].name == myLeague) {
          s_compLeague = i;
          // Also pre-select the matching country
          int cid = s_filterLeagues[i].countryId;
          for (int j = 0; j < (int)s_filterCountries.size(); j++) {
            if (s_filterCountries[j].id == cid) { s_compCountry = j; break; }
          }
          break;
        }
      }
    }
    s_compInit = true;
  }

  const float kPad = 16.0f, kGap = 8.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 20.0f;
  const std::string &sn = g_CareerHub.club.shortName;

  // ---- Filter bar ----
  const float fbarH = 56.0f;
  ImVec2 cfbarTop = ImGui::GetCursorScreenPos(); // capture before BeginChild
  BeginModernCard("##comp_fbar", ImVec2(usW, fbarH));
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8.0f, 4.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,  ImVec2(8.0f, 0.0f));
  {
    float comboH = ImGui::GetFrameHeight();
    ImGui::SetCursorScreenPos(ImVec2(cfbarTop.x + 22.0f,
                                     cfbarTop.y + (fbarH - comboH) * 0.5f));
  }
  ImGui::SetNextItemWidth(140.0f);
  {
    std::string cLabel = (s_compCountry < 0) ? "Country" : s_filterCountries[s_compCountry].name;
    if (ImGui::BeginCombo("##comp_country", cLabel.c_str())) {
      if (ImGui::Selectable("All countries", s_compCountry < 0))
        { s_compCountry = -1; s_compLeague = -1; }
      for (int i = 0; i < (int)s_filterCountries.size(); i++) {
        bool sel = (s_compCountry == i);
        if (ImGui::Selectable(s_filterCountries[i].name.c_str(), sel))
          { s_compCountry = i; s_compLeague = -1; }
      }
      ImGui::EndCombo();
    }
  }
  ImGui::SameLine();
  ImGui::SetNextItemWidth(200.0f);
  {
    bool hasCountry = (s_compCountry >= 0);
    int  filterCid  = hasCountry ? s_filterCountries[s_compCountry].id : -1;
    std::string lLabel = (s_compLeague < 0) ? "League" : s_filterLeagues[s_compLeague].name;
    if (ImGui::BeginCombo("##comp_league", lLabel.c_str())) {
      if (ImGui::Selectable("All leagues", s_compLeague < 0))
        s_compLeague = -1;
      for (int i = 0; i < (int)s_filterLeagues.size(); i++) {
        if (hasCountry && s_filterLeagues[i].countryId != filterCid) continue;
        bool sel = (s_compLeague == i);
        if (ImGui::Selectable(s_filterLeagues[i].name.c_str(), sel))
          s_compLeague = i;
      }
      ImGui::EndCombo();
    }
  }
  ImGui::PopStyleVar(2);
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();

  ImGui::Dummy(ImVec2(0, kGap));

  // Determine which league names pass the filter
  std::string filterLeagueName;
  if (s_compLeague >= 0) filterLeagueName = s_filterLeagues[s_compLeague].name;

  // Collect visible leagues from career standings
  std::vector<std::string> leagues;
  for (const auto &s : g_CareerHub.standings) {
    if (!filterLeagueName.empty() && s.league != filterLeagueName) continue;
    bool found = false;
    for (const auto &l : leagues) if (l == s.league) { found = true; break; }
    if (!found) leagues.push_back(s.league);
  }

  float standH = usH - fbarH - kGap - 4.0f;
  if (standH < 60.0f) standH = 60.0f;
  BeginModernCard("##comp_outer", ImVec2(usW, standH));
  ImGui::BeginChild("##cp_scroll", ImVec2(0, standH - 38.0f), false);

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
        if (mine)
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(75,18,140,42));
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
        // Use full name when available, fall back to shortname
        const std::string &displayName = s.teamFull.empty() ? s.team : s.teamFull;
        DrawTeamLabel(s.teamLogo, displayName, 18.0f);
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

// ---- Team Instructions helpers ------------------------------------------

static void SaveTacticsToDb(int clubId, const std::map<std::string, float> &tactics) {
  std::stringstream xml;
  for (const auto &kv : tactics)
    xml << "<" << kv.first << ">" << kv.second << "</" << kv.first << ">\n";
  // Escape single-quotes in the XML just in case values are ever strings
  std::string xmlStr = xml.str();
  std::string escaped;
  escaped.reserve(xmlStr.size());
  for (char c : xmlStr) {
    if (c == '\'') escaped += "''";
    else escaped += c;
  }
  std::stringstream q;
  q << "UPDATE teams SET tactics_xml='" << escaped << "' WHERE id=" << clubId << ";";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
}

struct TacInstruction {
  const char *key;
  const char *name;
  const char *category; // "Attacking" | "Defending" | "On the Ball"
  struct Preset { const char *label; float value; } presets[4];
};

static const TacInstruction kTacInstructions[] = {
  { "position_offense_depth_factor", "Attacking Depth", "Attacking",
    {{"Compact",0.25f},{"Balanced",0.5f},{"Expansive",0.75f},{"Total Attack",1.0f}} },
  { "position_offense_width_factor", "Attacking Width", "Attacking",
    {{"Narrow",0.3f},{"Balanced",0.6f},{"Wide",0.8f},{"Full Width",1.0f}} },
  { "position_offense_midfieldfocus", "Midfield in Attack", "Attacking",
    {{"Hold Shape",0.2f},{"Balanced",0.45f},{"Join Attack",0.7f},{"All Forward",0.9f}} },
  { "position_offense_sidefocus_strength", "Flank Play", "Attacking",
    {{"Central",0.1f},{"Mixed",0.35f},{"Wide Threat",0.6f},{"Wing Overloads",0.9f}} },
  { "position_offense_microfocus_strength", "Attacking Pressing", "Attacking",
    {{"Loose",0.2f},{"Balanced",0.5f},{"Tight",0.75f},{"Swarm",0.95f}} },
  { "position_defense_depth_factor", "Defensive Line", "Defending",
    {{"Deep Block",0.3f},{"Mid Block",0.55f},{"High Line",0.75f},{"Ultra High",0.95f}} },
  { "position_defense_width_factor", "Defensive Shape", "Defending",
    {{"Narrow Block",0.3f},{"Balanced",0.55f},{"Wide",0.8f},{"Spread",1.0f}} },
  { "position_defense_midfieldfocus", "Midfield Pressure", "Defending",
    {{"Drop Deep",0.2f},{"Compact",0.45f},{"Press High",0.7f},{"Extreme Press",0.9f}} },
  { "position_defense_sidefocus_strength", "Flank Coverage", "Defending",
    {{"Narrow",0.1f},{"Balanced",0.4f},{"Cover Wings",0.65f},{"Full Width",0.9f}} },
  { "position_defense_microfocus_strength", "Defensive Compactness", "Defending",
    {{"Loose",0.2f},{"Solid",0.5f},{"Compact",0.75f},{"Ultra Compact",0.95f}} },
  { "dribble_offensiveness", "Dribble Rate", "On the Ball",
    {{"Cautious",0.2f},{"Balanced",0.5f},{"Direct",0.7f},{"Expressive",0.9f}} },
  { "dribble_centermagnet", "Dribble Direction", "On the Ball",
    {{"Hug Flanks",0.1f},{"Mixed",0.4f},{"Through Middle",0.7f},{"Central Drive",0.9f}} },
};
static const int kNumTacInstructions = 12;

static void DrawTeamInstructionsPanel(float px, float py, float pw, float ph) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  // Dark overlay on top of the pitch
  dl->AddRectFilled(ImVec2(px, py), ImVec2(px + pw, py + ph),
                    IM_COL32(8, 14, 32, 230), 10.0f);

  // Category colours (bg tint for section headers)
  const ImU32 kCatAttack  = IM_COL32(18, 100, 42, 200);
  const ImU32 kCatDefend  = IM_COL32(16, 56, 130, 200);
  const ImU32 kCatBall    = IM_COL32(140, 80, 10, 200);
  const ImU32 kCatAttackT = IM_COL32(50, 200, 90, 255);
  const ImU32 kCatDefendT = IM_COL32(80, 160, 255, 255);
  const ImU32 kCatBallT   = IM_COL32(255, 190, 60, 255);

  const float kRowH    = 36.0f;
  const float kHdrH    = 22.0f;
  const float kPadX    = 10.0f;
  const float kPadTop  = 8.0f;
  const float kBtnGap  = 3.0f;

  // Scrollable child so long lists don't overflow the pitch area
  ImGui::SetCursorScreenPos(ImVec2(px + kPadX, py + kPadTop));
  ImGui::BeginChild("##tacInstr", ImVec2(pw - kPadX * 2.0f, ph - kPadTop * 2.0f),
                    false, ImGuiWindowFlags_None);
  ImDrawList *wdl = ImGui::GetWindowDrawList();

  const char *lastCat = nullptr;
  float innerW = pw - kPadX * 2.0f - 12.0f;

  for (int ti = 0; ti < kNumTacInstructions; ti++) {
    const TacInstruction &ins = kTacInstructions[ti];

    // Category header
    if (!lastCat || strcmp(lastCat, ins.category) != 0) {
      lastCat = ins.category;
      ImU32 catBg  = kCatAttack;
      ImU32 catTxt = kCatAttackT;
      if (strcmp(ins.category, "Defending") == 0)    { catBg = kCatDefend; catTxt = kCatDefendT; }
      else if (strcmp(ins.category, "On the Ball") == 0) { catBg = kCatBall; catTxt = kCatBallT; }

      ImVec2 hp = ImGui::GetCursorScreenPos();
      wdl->AddRectFilled(hp, ImVec2(hp.x + innerW, hp.y + kHdrH), catBg, 4.0f);
      wdl->AddRectFilled(hp, ImVec2(hp.x + 3.0f, hp.y + kHdrH), catTxt);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(catTxt));
      char catBuf[48]; snprintf(catBuf, sizeof(catBuf), "  %s", ins.category);
      ImGui::TextUnformatted(catBuf);
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);
    }

    // Row background
    ImVec2 rp = ImGui::GetCursorScreenPos();
    wdl->AddRectFilled(rp, ImVec2(rp.x + innerW, rp.y + kRowH),
                       IM_COL32(12, 20, 48, 180), 3.0f);

    // Tactic name (left 38%)
    float nameW = innerW * 0.38f;
    PushMgrFont(g_ManagerFontSmall);
    ImGui::SetCursorScreenPos(ImVec2(rp.x + 8.0f, rp.y + (kRowH - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.85f, 0.88f, 0.95f, 1.0f));
    ImGui::TextUnformatted(ins.name);
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);

    // Preset buttons (right 62%)
    float btnAreaX = rp.x + nameW;
    float btnAreaW = innerW - nameW - kBtnGap;
    float btnW = (btnAreaW - kBtnGap * 3.0f) / 4.0f;
    float btnH = kRowH - 8.0f;
    float btnY = rp.y + 4.0f;

    // Find current value to determine selected preset
    float curVal = 0.5f;
    auto it = g_CareerHub.tactics.find(ins.key);
    if (it != g_CareerHub.tactics.end()) curVal = it->second;

    int selPreset = 0;
    float bestDist = 9999.0f;
    for (int pi = 0; pi < 4; pi++) {
      float d = fabsf(ins.presets[pi].value - curVal);
      if (d < bestDist) { bestDist = d; selPreset = pi; }
    }

    ImGui::PushID(ins.key);
    for (int pi = 0; pi < 4; pi++) {
      float bx = btnAreaX + pi * (btnW + kBtnGap);
      bool selected = (pi == selPreset);

      ImGui::SetCursorScreenPos(ImVec2(bx, btnY));
      ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
      ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(2.0f, 2.0f));
      if (selected) {
        ImGui::PushStyleColor(ImGuiCol_Button,        C32(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C32(kAccentH));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  C32(kAccentA));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(20, 30, 60, 200));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 55, 110, 220));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(60, 80, 150, 255));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.65f, 0.75f, 1.0f));
      }

      PushMgrFont(g_ManagerFontSmall);
      char btnId[64]; snprintf(btnId, sizeof(btnId), "%s##p%d", ins.presets[pi].label, pi);
      if (ImGui::Button(btnId, ImVec2(btnW, btnH))) {
        g_CareerHub.tactics[ins.key] = ins.presets[pi].value;
        SaveTacticsToDb(g_CareerHub.clubId, g_CareerHub.tactics);
      }
      PopMgrFont(g_ManagerFontSmall);

      ImGui::PopStyleColor(4);
      ImGui::PopStyleVar(2);
    }
    ImGui::PopID();

    // Advance cursor past the row
    ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + kRowH + 2.0f));
    ImGui::Dummy(ImVec2(innerW, 0.0f));
  }

  // ---- Tactic Visualisation Mini Pitch ------------------------------------
  ImGui::Dummy(ImVec2(innerW, 8.0f));

  // Read tactic values (use defaults if missing)
  auto TV = [&](const char *k, float def) -> float {
    auto it = g_CareerHub.tactics.find(k);
    return it != g_CareerHub.tactics.end() ? it->second : def;
  };
  float offDepth   = TV("position_offense_depth_factor",      0.9f);
  float defDepth   = TV("position_defense_depth_factor",      0.75f);
  float offWidth   = TV("position_offense_width_factor",      0.9f); (void)offWidth;
  float defWidth   = TV("position_defense_width_factor",      0.8f);
  float offMid     = TV("position_offense_midfieldfocus",     0.6f);
  float defMid     = TV("position_defense_midfieldfocus",     0.5f);
  float offSide    = TV("position_offense_sidefocus_strength",  0.1f);
  float defSide    = TV("position_defense_sidefocus_strength",  0.4f);
  float offMicro   = TV("position_offense_microfocus_strength", 0.7f); (void)offMicro;
  float defMicro   = TV("position_defense_microfocus_strength", 0.8f);
  float dribOff    = TV("dribble_offensiveness",              0.5f);
  float dribCtr    = TV("dribble_centermagnet",               0.5f);

  // Pitch dimensions — landscape (wide), attack direction left→right
  // x=0=our goal, x=1=opponent goal  |  y=0=top touchline, y=1=bottom touchline
  float mpW  = innerW - 4.0f;
  float mpH  = mpW * 0.38f;   // compact landscape — fits below the tactic rows
  ImVec2 mpos = ImGui::GetCursorScreenPos();
  float mpX = mpos.x + 2.0f;
  float mpY = mpos.y;

  // Arrow helper
  auto Arrow = [&](float x1,float y1,float x2,float y2, ImU32 col, float thick, float hd) {
    wdl->AddLine(ImVec2(x1,y1), ImVec2(x2,y2), col, thick);
    float dx=x2-x1, dy=y2-y1, len=sqrtf(dx*dx+dy*dy);
    if (len < 2.0f) return;
    dx/=len; dy/=len;
    float nx=-dy, ny=dx;
    wdl->AddTriangleFilled(
      ImVec2(x2,y2),
      ImVec2(x2-dx*hd+nx*hd*0.45f, y2-dy*hd+ny*hd*0.45f),
      ImVec2(x2-dx*hd-nx*hd*0.45f, y2-dy*hd-ny*hd*0.45f), col);
  };
  // Normalised-coord helpers: x=0 our goal, x=1 opp goal; y=0 top, y=1 bottom
  auto PX = [&](float nx) { return mpX + nx * mpW; };
  auto PY = [&](float ny) { return mpY + ny * mpH; };

  // Pitch stripes (vertical — along the length of the pitch)
  {
    int ns = 10;
    float sh = mpH / ns;
    for (int i = 0; i < ns; i++) {
      ImU32 c = (i%2==0) ? IM_COL32(18,80,34,255) : IM_COL32(22,94,40,255);
      wdl->AddRectFilled(ImVec2(mpX, mpY+i*sh), ImVec2(mpX+mpW, mpY+(i+1)*sh), c);
    }
  }
  wdl->PushClipRect(ImVec2(mpX,mpY), ImVec2(mpX+mpW,mpY+mpH), true);
  {
    int ns = 10; float sh = mpH / ns;
    for (int i = 0; i < ns; i++) {
      ImU32 c = (i%2==0) ? IM_COL32(18,80,34,255) : IM_COL32(22,94,40,255);
      wdl->AddRectFilled(ImVec2(mpX, mpY+i*sh), ImVec2(mpX+mpW, mpY+(i+1)*sh), c);
    }
  }

  // Pitch markings
  ImU32 lc = IM_COL32(255,255,255,50);
  float lm=PX(0.05f), rm=PX(0.95f), tm=PY(0.06f), bm=PY(0.94f);
  float midX=PX(0.5f), midY=PY(0.5f);
  wdl->AddRect(ImVec2(lm,tm), ImVec2(rm,bm), lc, 0.0f, 0, 1.2f);
  wdl->AddLine(ImVec2(midX,tm), ImVec2(midX,bm), lc, 1.2f);        // halfway line
  wdl->AddCircle(ImVec2(midX,midY), mpH*0.18f, lc, 36, 1.2f);      // centre circle
  wdl->AddCircleFilled(ImVec2(midX,midY), 2.5f, lc);
  // Our penalty area (left)
  float paw=mpW*0.14f, pah=mpH*0.52f, pay=PY(0.5f)-pah*0.5f;
  wdl->AddRect(ImVec2(lm, pay), ImVec2(lm+paw, pay+pah), lc, 0.0f, 0, 1.2f);
  // Goal (left)
  float gw=mpW*0.02f, gh=mpH*0.25f, gy=PY(0.5f)-gh*0.5f;
  wdl->AddRect(ImVec2(mpX, gy), ImVec2(mpX+gw, gy+gh), lc, 0.0f, 0, 1.2f);
  // Opponent penalty area (right)
  wdl->AddRect(ImVec2(rm-paw, pay), ImVec2(rm, pay+pah), lc, 0.0f, 0, 1.2f);
  // Goal (right)
  wdl->AddRect(ImVec2(mpX+mpW-gw, gy), ImVec2(mpX+mpW, gy+gh), lc, 0.0f, 0, 1.2f);

  // ---- DEFENDING indicators (blue) ----------------------------------------
  // Defensive line — vertical bar in our half (x moves right as line goes higher)
  float defLineNX = 0.26f + defDepth * 0.24f;  // 0.3→0.332, 0.95→0.488
  {
    ImU32 defCol = IM_COL32(80, 150, 255, 230);
    wdl->AddLine(ImVec2(PX(defLineNX), tm+2), ImVec2(PX(defLineNX), bm-2), defCol, 2.0f);
    wdl->AddLine(ImVec2(PX(defLineNX)-4, tm+2), ImVec2(PX(defLineNX)+4, tm+2), defCol, 1.5f);
    wdl->AddLine(ImVec2(PX(defLineNX)-4, bm-2), ImVec2(PX(defLineNX)+4, bm-2), defCol, 1.5f);
  }

  // Defensive width — shaded bands at top and bottom in our half
  if (defWidth > 0.3f) {
    float alpha = (defWidth - 0.3f) / 0.7f * 90.0f;
    float bandH = mpH * defWidth * 0.14f;
    ImU32 bandCol = IM_COL32(50, 110, 210, (int)alpha);
    wdl->AddRectFilled(ImVec2(PX(0.05f), PY(0.0f)), ImVec2(PX(defLineNX), PY(0.0f)+bandH), bandCol);
    wdl->AddRectFilled(ImVec2(PX(0.05f), PY(1.0f)-bandH), ImVec2(PX(defLineNX), PY(1.0f)), bandCol);
  }

  // Defensive flank coverage — arrows running along top/bottom wings toward our goal
  if (defSide > 0.2f) {
    float alpha = (defSide - 0.2f) / 0.8f;
    ImU32 defArrow = IM_COL32(80, 160, 255, (int)(alpha * 200.0f + 40.0f));
    float thick = 1.0f + alpha * 1.5f;
    float hd    = 5.0f + alpha * 3.0f;
    float wingY = 0.08f + (1.0f - defSide) * 0.24f;  // 0.9→0.08 (near touchline), 0.1→0.32
    float fromX = defLineNX + 0.14f;
    float toX   = defLineNX - 0.02f;
    Arrow(PX(fromX), PY(wingY),        PX(toX), PY(wingY),        defArrow, thick, hd);
    Arrow(PX(fromX), PY(1.0f-wingY),   PX(toX), PY(1.0f-wingY),   defArrow, thick, hd);
  }

  // Pressing arrows — rightward arrows in opponent half when midfield presses high
  if (defMid > 0.5f) {
    float alpha = (defMid - 0.5f) / 0.5f;
    ImU32 pressCol = IM_COL32(100, 180, 255, (int)(alpha * 170.0f + 30.0f));
    float thick = 1.0f + alpha * 1.2f;
    float hd = 4.0f + alpha * 2.5f;
    float fromX = 0.52f, toX = 0.66f + alpha * 0.08f;
    Arrow(PX(fromX), PY(0.25f), PX(toX), PY(0.25f), pressCol, thick, hd);
    Arrow(PX(fromX), PY(0.50f), PX(toX), PY(0.50f), pressCol, thick, hd);
    Arrow(PX(fromX), PY(0.75f), PX(toX), PY(0.75f), pressCol, thick, hd);
  }

  // Defensive compactness — inward arrows from touchlines toward centre in our half
  if (defMicro > 0.5f) {
    float alpha = (defMicro - 0.5f) / 0.5f;
    ImU32 compCol = IM_COL32(60, 130, 220, (int)(alpha * 160.0f + 20.0f));
    float thick = 1.0f + alpha;
    float hd = 3.5f + alpha * 2.0f;
    float compX = defLineNX - 0.06f;
    Arrow(PX(compX), PY(0.12f), PX(compX), PY(0.30f), compCol, thick, hd); // top → center
    Arrow(PX(compX), PY(0.88f), PX(compX), PY(0.70f), compCol, thick, hd); // bottom → center
  }

  // ---- ATTACKING indicators (green) ----------------------------------------

  // Attacking zone shading — how far into opponent half the team pushes
  {
    float zoneLeftNX = 0.5f + offDepth * 0.40f;  // 0.25→0.6, 1.0→0.9
    wdl->AddRectFilled(ImVec2(PX(0.5f), PY(0.06f)),
                       ImVec2(PX(zoneLeftNX < 0.95f ? zoneLeftNX : 0.95f), PY(0.94f)),
                       IM_COL32(50, 200, 80, (int)(offDepth * 38.0f + 8.0f)));
  }

  // Attacking flank arrows — along top/bottom wings into opponent half
  if (offSide > 0.2f) {
    float alpha = (offSide - 0.2f) / 0.8f;
    ImU32 attArrow = IM_COL32(50, 220, 100, (int)(alpha * 210.0f + 40.0f));
    float thick = 1.2f + alpha * 1.8f;
    float hd    = 5.0f + alpha * 4.0f;
    float wingY = 0.07f + (1.0f - offSide) * 0.20f;
    float fromX = 0.48f, toX = 0.72f + alpha * 0.14f;
    Arrow(PX(fromX), PY(wingY),      PX(toX), PY(wingY),      attArrow, thick, hd);
    Arrow(PX(fromX), PY(1.0f-wingY), PX(toX), PY(1.0f-wingY), attArrow, thick, hd);
  }

  // Attacking depth — central forward arrow in opponent half
  if (offDepth > 0.4f) {
    float alpha = (offDepth - 0.4f) / 0.6f;
    ImU32 attCol = IM_COL32(80, 210, 110, (int)(alpha * 180.0f + 30.0f));
    float thick = 1.2f + alpha * 1.5f;
    float hd    = 5.0f + alpha * 3.5f;
    Arrow(PX(0.50f), PY(0.5f), PX(0.62f + alpha*0.20f), PY(0.5f), attCol, thick, hd);
  }

  // Midfield joining attack — arrows slightly off-center
  if (offMid > 0.5f) {
    float alpha = (offMid - 0.5f) / 0.5f;
    ImU32 midCol = IM_COL32(120, 230, 140, (int)(alpha * 160.0f + 20.0f));
    float thick = 1.0f + alpha;
    float hd    = 4.0f + alpha * 2.5f;
    float toX   = 0.68f + alpha * 0.10f;
    Arrow(PX(0.44f), PY(0.34f), PX(toX), PY(0.34f), midCol, thick, hd);
    Arrow(PX(0.44f), PY(0.66f), PX(toX), PY(0.66f), midCol, thick, hd);
  }

  // ---- ON THE BALL indicators (amber) -------------------------------------
  {
    float alpha = dribOff * 0.85f + 0.15f;
    float hd    = 4.5f + dribOff * 3.0f;
    float thick = 1.2f + dribOff * 1.2f;
    float fromX = 0.50f, toX = 0.66f + dribOff * 0.10f;
    if (dribCtr > 0.55f) {
      // Central drive — straight right through middle
      ImU32 aCol = IM_COL32(255, 190, 60, (int)(alpha * 200.0f));
      Arrow(PX(fromX), PY(0.5f), PX(toX), PY(0.5f), aCol, thick, hd);
    } else if (dribCtr < 0.35f) {
      // Hug flanks — angled toward touchlines
      ImU32 aCol = IM_COL32(255, 190, 60, (int)(alpha * 180.0f));
      Arrow(PX(fromX), PY(0.5f), PX(toX-0.04f), PY(0.18f), aCol, thick-0.3f, hd);
      Arrow(PX(fromX), PY(0.5f), PX(toX-0.04f), PY(0.82f), aCol, thick-0.3f, hd);
    } else {
      // Mixed — center + slight flank spread
      ImU32 aCol = IM_COL32(255, 190, 60, (int)(alpha * 160.0f));
      Arrow(PX(fromX),  PY(0.50f), PX(toX),        PY(0.50f), aCol, thick,       hd);
      Arrow(PX(fromX),  PY(0.50f), PX(toX-0.06f),  PY(0.28f), aCol, thick-0.4f, hd-1.0f);
      Arrow(PX(fromX),  PY(0.50f), PX(toX-0.06f),  PY(0.72f), aCol, thick-0.4f, hd-1.0f);
    }
  }

  wdl->PopClipRect();

  // Pitch border
  wdl->AddRect(ImVec2(mpX,mpY), ImVec2(mpX+mpW,mpY+mpH),
               IM_COL32(255,255,255,40), 6.0f, 0, 1.2f);

  // Attack direction label (right side)
  {
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f,0.44f,0.54f,1.0f));
    ImGui::SetCursorScreenPos(ImVec2(mpX, mpY + mpH + 3.0f));
    ImGui::TextUnformatted("OUR GOAL");
    float rw = ImGui::CalcTextSize("OPP GOAL").x;
    ImGui::SetCursorScreenPos(ImVec2(mpX + mpW - rw, mpY + mpH + 3.0f));
    ImGui::TextUnformatted("OPP GOAL");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
  }

  // Legend dots
  {
    float legY = mpY + mpH + 18.0f;
    float legX = mpX;
    auto LegDot = [&](float &lx, ImU32 col, const char *lbl) {
      wdl->AddCircleFilled(ImVec2(lx+5.0f, legY+6.0f), 4.0f, col);
      lx += 12.0f;
      PushMgrFont(g_ManagerFontSmall);
      ImGui::SetCursorScreenPos(ImVec2(lx, legY));
      ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(col));
      ImGui::TextUnformatted(lbl);
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);
      lx += ImGui::CalcTextSize(lbl).x + 10.0f;
    };
    LegDot(legX, IM_COL32(80,150,255,255), "Defending");
    LegDot(legX, IM_COL32(80,210,110,255), "Attacking");
    LegDot(legX, IM_COL32(255,190,60,255), "On the Ball");
  }

  // Reserve layout space
  ImGui::SetCursorScreenPos(ImVec2(mpX, mpY));
  ImGui::Dummy(ImVec2(mpW, mpH + 32.0f));

  ImGui::EndChild();
}

// ---- DrawTacticsPage ----------------------------------------------------

// Formation node layout for 4-3-3.
// Each entry: {fo, normalized x [0,1], normalized y [0,1] (0=GK end, 1=attack end)}
struct TacNode { int fo; float nx; float ny; };
static const TacNode kTacNodes[] = {
  {0,  0.50f, 0.08f},                                           // GK
  {1,  0.12f, 0.28f}, {2, 0.35f, 0.28f}, {3, 0.65f, 0.28f}, {4, 0.88f, 0.28f}, // DEF
  {5,  0.22f, 0.52f}, {6, 0.50f, 0.52f}, {7, 0.78f, 0.52f},   // MID
  {8,  0.16f, 0.78f}, {10, 0.50f, 0.78f}, {9, 0.84f, 0.78f},  // ATT
};
static const int kNumTacNodes = 11;

// Face texture loaded once from media/textures/faces/player.png (relative to cwd)
static GLuint s_FaceTex = 0;
static bool   s_FaceTexTried = false;

static GLuint GetFaceTex() {
  if (s_FaceTexTried) return s_FaceTex;
  s_FaceTexTried = true;
  const char *path = "media/textures/faces/player.png";
  SDL_Surface *surf = IMG_Load(path);
  if (!surf) return 0;
  SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(surf);
  if (!rgba) return 0;
  glGenTextures(1, &s_FaceTex);
  glBindTexture(GL_TEXTURE_2D, s_FaceTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
  SDL_FreeSurface(rgba);
  glBindTexture(GL_TEXTURE_2D, 0);
  return s_FaceTex;
}

// POS badge color per position type
static ImU32 PosBadgeColor(int fo) {
  if (fo == 0)                     return IM_COL32(220, 120, 20,  220); // GK  orange
  if (fo >= 1 && fo <= 4)          return IM_COL32(30,  110, 200, 220); // DEF blue
  if (fo >= 5 && fo <= 7)          return IM_COL32(30,  160, 80,  220); // MID green
  if (fo >= 8 && fo <= 10)         return IM_COL32(190, 40,  40,  220); // ATT red
  return                                  IM_COL32(80,  80,  80,  220); // bench
}

// Reuses s_swapPlayerA/FoA/FoB from FlushSquadSwap()

// Drag-drop payload: the player being dragged
struct TacDragPayload { int playerId; int fo; };

static void DrawTacPlayerRow(ImDrawList *wdl, float rowW, float rowH,
                             int playerId, int curFo,
                             const std::string &name, bool isSub,
                             GLuint faceTex, bool altRow, const char *rowId,
                             float baseStat = 0.0f, const std::string &role = "")
{
  ImVec2 rp = ImGui::GetCursorScreenPos();

  if (altRow)
    wdl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH),
                       IM_COL32(255,255,255,6));

  // Invisible button covers the whole row (needed for drag source activation)
  ImGui::InvisibleButton(rowId, ImVec2(rowW, rowH));
  bool hovered = ImGui::IsItemHovered();

  if (hovered)
    wdl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH),
                       IM_COL32(255,255,255,12));

  // ---- Drag source (real players only) ------------------------------------
  if (playerId >= 0 &&
      ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
    TacDragPayload payload{playerId, curFo};
    ImGui::SetDragDropPayload("TACTIC_PLAYER", &payload, sizeof(payload));

    // Drag tooltip
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::Text("%s  ", PosLabel(curFo));
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
    ImGui::TextUnformatted(name.c_str());
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);

    ImGui::EndDragDropSource();
  }

  // ---- Drop target (every row — including empty slots) -------------------
  if (ImGui::BeginDragDropTarget()) {
    // Highlight the drop zone
    ImVec2 tl = ImGui::GetItemRectMin();
    ImVec2 br = ImGui::GetItemRectMax();
    wdl->AddRect(tl, br, C32(kAccent), 4.0f, 0, 2.0f);

    if (const ImGuiPayload *payload =
            ImGui::AcceptDragDropPayload("TACTIC_PLAYER")) {
      auto *src = static_cast<const TacDragPayload*>(payload->Data);
      if (src->fo != curFo) {          // don't drop onto itself
        s_swapPlayerA = src->playerId;
        s_swapFoA     = src->fo;
        s_swapFoB     = curFo;
      }
    }
    ImGui::EndDragDropTarget();
  }

  // ---- Visuals (drawn after interaction so they render on top) -----------
  float faceS = rowH - 6.0f;
  float faceX = rp.x + 6.0f;
  float faceY = rp.y + 3.0f;

  if (faceTex && playerId >= 0) {
    wdl->AddRectFilled(ImVec2(faceX, faceY),
                       ImVec2(faceX + faceS, faceY + faceS),
                       IM_COL32(30, 40, 65, 255), 4.0f);
    wdl->AddImage((ImTextureID)(intptr_t)faceTex,
                  ImVec2(faceX, faceY),
                  ImVec2(faceX + faceS, faceY + faceS));
    float cr = faceS * 0.5f;
    wdl->AddCircle(ImVec2(faceX + cr, faceY + cr), cr,
                   IM_COL32(255,255,255,30), 32, 1.0f);
  } else {
    float cr = faceS * 0.5f;
    wdl->AddCircleFilled(ImVec2(faceX + cr, faceY + cr), cr,
                         IM_COL32(40, 50, 75, playerId >= 0 ? 200 : 80));
    wdl->AddCircle(ImVec2(faceX + cr, faceY + cr), cr,
                   IM_COL32(255,255,255,25), 32, 1.0f);
  }

  float badgeX = faceX + faceS + 6.0f;
  float badgeY = rp.y + (rowH - 16.0f) * 0.5f;
  float badgeW = 32.0f;
  wdl->AddRectFilled(ImVec2(badgeX, badgeY),
                     ImVec2(badgeX + badgeW, badgeY + 16.0f),
                     PosBadgeColor(curFo), 3.0f);
  {
    const char *lbl = isSub ? "SUB" : PosLabel(curFo);
    ImVec2 ts = ImGui::CalcTextSize(lbl);
    wdl->AddText(ImVec2(badgeX + (badgeW - ts.x) * 0.5f,
                        badgeY + (16.0f - ts.y) * 0.5f),
                 IM_COL32(255,255,255,230), lbl);
  }

  float nameX = badgeX + badgeW + 7.0f;
  float lineH = ImGui::GetTextLineHeight();
  bool hasSecondLine = (playerId >= 0 && (!role.empty() || baseStat > 0.0f));
  // Name: vertically centered if single line, offset up if two lines
  float nameY = hasSecondLine ? rp.y + rowH * 0.5f - lineH - 1.0f
                              : rp.y + (rowH - lineH) * 0.5f;
  ImU32 nameCol = (playerId < 0) ? IM_COL32(100,105,120,160) : IM_COL32(220,230,248,230);
  wdl->AddText(ImVec2(nameX, nameY), nameCol, name.c_str());

  // Second line: role text + small ability bar
  if (hasSecondLine) {
    float line2Y = rp.y + rowH * 0.5f + 1.0f;
    float curX   = nameX;

    // Role text (truncated to ~10 chars)
    if (!role.empty()) {
      std::string roleDisp = role;
      if (roleDisp.size() > 12) roleDisp = roleDisp.substr(0, 11) + ".";
      wdl->AddText(ImVec2(curX, line2Y), IM_COL32(140,155,180,180), roleDisp.c_str());
      ImVec2 rs = ImGui::CalcTextSize(roleDisp.c_str());
      curX += rs.x + 6.0f;
    }

    // Ability stars (5 small pills, matches Squad table style)
    if (baseStat > 0.0f && curX + 44.0f < rp.x + rowW - 20.0f) {
      float stars = baseStat * 5.0f;
      if (stars > 5.f) stars = 5.f;
      const float kW = 7.f, kH = 5.f, kGp = 1.5f;
      float sy = line2Y + (lineH - kH) * 0.5f;
      ImU32 emptyCol = IM_COL32(30, 44, 72, 200);
      ImU32 fillCol  = C32(kGold);
      for (int si = 0; si < 5; si++) {
        float x0   = curX + (float)si * (kW + kGp);
        float fill = std::max(0.f, std::min(1.f, stars - (float)si));
        wdl->AddRectFilled(ImVec2(x0, sy), ImVec2(x0+kW, sy+kH), emptyCol, 1.5f);
        if (fill > 0.02f)
          wdl->AddRectFilled(ImVec2(x0, sy), ImVec2(x0+kW*fill, sy+kH), fillCol, 1.5f);
      }
    }
  }

  // Drag handle icon on right edge (real players only)
  if (playerId >= 0) {
    float iconX  = rp.x + rowW - 13.0f;
    float iconCY = rp.y + rowH * 0.5f;
    ImU32 ic = hovered ? IM_COL32(190,210,255,180) : IM_COL32(80,110,160,80);
    for (int li = -1; li <= 1; li++) {
      float ly = iconCY + li * 4.0f;
      wdl->AddLine(ImVec2(iconX - 5.0f, ly), ImVec2(iconX + 5.0f, ly), ic, 1.5f);
    }
  }
}

static void DrawTacticsPage(float w, float h) {
  FlushSquadSwap();

  GLuint faceTex = GetFaceTex();

  const float kPad = 14.0f;
  const float kGap = 10.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 6.0f));

  float usW = w - kPad * 2.0f;
  float usH = h - 12.0f;

  float boardW = usW * 0.57f - kGap * 0.5f;
  float listW  = usW * 0.43f - kGap * 0.5f;

  static bool s_showTeamInstructions = false;

  // ===== Formation board ===================================================
  ImGui::BeginGroup();

  ImVec2 cardPos = ImGui::GetCursorScreenPos();
  ImDrawList *dl = ImGui::GetWindowDrawList();

  float pitchX = cardPos.x;
  float pitchW = boardW;

  // Header bar: two toggle buttons (32px tall)
  const float kHdrBarH = 32.0f;
  {
    const float kBtnGapHdr = 6.0f;
    float btnW  = boardW * 0.46f;
    float btnH  = 26.0f;
    float btnY  = cardPos.y + (kHdrBarH - btnH) * 0.5f;

    // --- Team Instructions ---
    ImGui::SetCursorScreenPos(ImVec2(pitchX, btnY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
    if (s_showTeamInstructions) {
      ImGui::PushStyleColor(ImGuiCol_Button,        C32(kAccent));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C32(kAccentH));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  C32(kAccentA));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(14, 22, 50, 220));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(30, 45, 90, 230));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(50, 70, 130, 255));
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.75f, 0.88f, 1.0f));
    }
    PushMgrFont(g_ManagerFontSmall);
    if (ImGui::Button("Team Instructions", ImVec2(btnW, btnH)))
      s_showTeamInstructions = !s_showTeamInstructions;
    PopMgrFont(g_ManagerFontSmall);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);

    // --- Player Instructions (placeholder, disabled) ---
    ImGui::SetCursorScreenPos(ImVec2(pitchX + btnW + kBtnGapHdr, btnY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 4.0f));
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(14, 22, 50, 120));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(14, 22, 50, 120));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(14, 22, 50, 120));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.44f, 0.54f, 1.0f));
    PushMgrFont(g_ManagerFontSmall);
    ImGui::BeginDisabled(true);
    ImGui::Button("Player Instructions", ImVec2(btnW, btnH));
    ImGui::EndDisabled();
    PopMgrFont(g_ManagerFontSmall);
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);
  }

  float pitchY = cardPos.y + kHdrBarH;
  float pitchH = usH - kHdrBarH;

  // Pitch outer card shadow
  dl->AddRectFilled(ImVec2(pitchX + 3, pitchY + 3),
                    ImVec2(pitchX + pitchW + 3, pitchY + pitchH + 3),
                    IM_COL32(0,0,0,60), 12.0f);

  // Pitch base — two-tone vertical stripes
  {
    int ns = 10;
    float sw = pitchW / ns;
    for (int i = 0; i < ns; i++) {
      ImU32 col = (i % 2 == 0) ? IM_COL32(20, 90, 38, 255)
                                : IM_COL32(24, 102, 44, 255);
      dl->AddRectFilled(ImVec2(pitchX + i * sw, pitchY),
                        ImVec2(pitchX + (i+1) * sw, pitchY + pitchH), col);
    }
    // round corners by overdrawing with transparent rects at corners
    dl->AddRectFilled(ImVec2(pitchX, pitchY),
                      ImVec2(pitchX + pitchW, pitchY + pitchH),
                      IM_COL32(0,0,0,0), 12.0f);
  }

  // Clip to rounded rect so stripes don't overflow
  dl->PushClipRect(ImVec2(pitchX, pitchY),
                   ImVec2(pitchX + pitchW, pitchY + pitchH), true);

  // Re-draw stripes inside clip
  {
    int ns = 10;
    float sw = pitchW / ns;
    for (int i = 0; i < ns; i++) {
      ImU32 col = (i % 2 == 0) ? IM_COL32(20, 90, 38, 255)
                                : IM_COL32(24, 102, 44, 255);
      dl->AddRectFilled(ImVec2(pitchX + i * sw, pitchY),
                        ImVec2(pitchX + (i+1) * sw, pitchY + pitchH), col);
    }
  }

  // Pitch markings
  ImU32 lc = IM_COL32(255, 255, 255, 60);
  float ml  = pitchX + pitchW * 0.07f;
  float mr  = pitchX + pitchW * 0.93f;
  float mt  = pitchY + pitchH * 0.05f;
  float mb  = pitchY + pitchH * 0.95f;
  float midY = pitchY + pitchH * 0.5f;

  dl->AddRect(ImVec2(ml, mt), ImVec2(mr, mb), lc, 0.0f, 0, 1.5f);
  dl->AddLine(ImVec2(ml, midY), ImVec2(mr, midY), lc, 1.5f);
  dl->AddCircle(ImVec2(pitchX + pitchW * 0.5f, midY), pitchH * 0.095f, lc, 48, 1.5f);
  dl->AddCircleFilled(ImVec2(pitchX + pitchW * 0.5f, midY), 3.0f, lc);

  // Top penalty area
  { float bw=pitchW*0.48f, bh=pitchH*0.13f, bx=pitchX+(pitchW-bw)*0.5f;
    dl->AddRect(ImVec2(bx, mb-bh), ImVec2(bx+bw, mb), lc, 0.0f, 0, 1.5f);
    float gw=pitchW*0.22f, gh=pitchH*0.055f, gx=pitchX+(pitchW-gw)*0.5f;
    dl->AddRect(ImVec2(gx, mb-gh), ImVec2(gx+gw, mb), lc, 0.0f, 0, 1.5f); }
  // Bottom penalty area
  { float bw=pitchW*0.48f, bh=pitchH*0.13f, bx=pitchX+(pitchW-bw)*0.5f;
    dl->AddRect(ImVec2(bx, mt), ImVec2(bx+bw, mt+bh), lc, 0.0f, 0, 1.5f);
    float gw=pitchW*0.22f, gh=pitchH*0.055f, gx=pitchX+(pitchW-gw)*0.5f;
    dl->AddRect(ImVec2(gx, mt), ImVec2(gx+gw, mt+gh), lc, 0.0f, 0, 1.5f); }

  if (s_showTeamInstructions) {
    DrawTeamInstructionsPanel(pitchX, pitchY, pitchW, pitchH);
  }

  if (!s_showTeamInstructions) {
  // Build node data — include all fields needed for display and tooltip
  struct NodeInfo {
    std::string name;
    bool    filled    = false;
    float   baseStat  = 0.0f;
    int     potential = 0;
    int     stamina   = 0;
    std::string foot;
  };
  NodeInfo nodes[kNumTacNodes];
  for (const auto &p : g_CareerHub.players) {
    for (int ni = 0; ni < kNumTacNodes; ni++) {
      if (kTacNodes[ni].fo == p.formationOrder) {
        std::string ln = p.lastName.empty() ? p.firstName : p.lastName;
        if (ln.size() > 9) ln = ln.substr(0, 8) + ".";
        nodes[ni].name      = ln;
        nodes[ni].filled    = true;
        nodes[ni].baseStat  = p.baseStat;
        nodes[ni].potential = p.potential;
        nodes[ni].stamina   = p.stamina;
        nodes[ni].foot      = p.foot;
        break;
      }
    }
  }

  // Node card dimensions
  const float kHdrH = 18.0f;
  float cardW = pitchW * 0.175f;
  if (cardW > 80.0f) cardW = 80.0f;
  if (cardW < 58.0f) cardW = 58.0f;
  float faceS = cardW - 8.0f;
  float cardH = kHdrH + faceS + 22.0f; // extra bottom margin so name doesn't overflow

  // Flip animation state — 0=front(face), 1=back(stats). Persists across frames.
  static float s_cardFlipT[kNumTacNodes] = {};
  const  float kFlipSpd = 4.0f;

  for (int ni = 0; ni < kNumTacNodes; ni++) {
    const TacNode &tn = kTacNodes[ni];
    float cx  = pitchX + tn.nx * pitchW;
    float cy  = pitchY + (1.0f - tn.ny) * pitchH;
    float cx0 = cx - cardW * 0.5f;
    float cy0 = cy - cardH * 0.5f;

    bool  has       = nodes[ni].filled;
    ImU32 posCol    = PosBadgeColor(tn.fo);
    ImU32 cardBg    = has ? IM_COL32(16, 24, 52, 230) : IM_COL32(18, 20, 30, 160);
    ImU32 borderCol = has ? IM_COL32(80, 120, 200, 140) : IM_COL32(55, 60, 80, 100);

    // Hover → drive flip
    bool hovered = has && !ImGui::IsMouseDragging(ImGuiMouseButton_Left) &&
                   ImGui::IsMouseHoveringRect(ImVec2(cx0, cy0),
                                              ImVec2(cx0+cardW, cy0+cardH), false);
    float &ft = s_cardFlipT[ni];
    if (hovered) ft = fminf(ft + ImGui::GetIO().DeltaTime * kFlipSpd, 1.0f);
    else         ft = fmaxf(ft - ImGui::GetIO().DeltaTime * kFlipSpd, 0.0f);

    // X-scale: cosine over [0,π] gives 1→-1; absScX goes 1→0→1
    float cosV   = cosf(ft * 3.14159f);
    float absScX = fabsf(cosV);
    bool  showBack = (ft >= 0.5f);

    // Scaled left/right edges (symmetric around cx)
    float lx = cx - cardW * 0.5f * absScX;
    float rx = cx + cardW * 0.5f * absScX;

    // Card background — single rounded rect, no double-background
    dl->AddRectFilled(ImVec2(lx, cy0), ImVec2(rx, cy0+cardH),
                      cardBg, 6.0f * absScX);

    // Header — RoundCornersTop so bottom edge is flush with card body
    dl->AddRectFilled(ImVec2(lx, cy0), ImVec2(rx, cy0+kHdrH),
                      posCol, 6.0f * absScX, ImDrawFlags_RoundCornersTop);

    // POS label in header
    if (absScX > 0.18f) {
      const char *lbl = PosLabel(tn.fo);
      ImVec2 ts = ImGui::CalcTextSize(lbl);
      dl->AddText(ImVec2(cx - ts.x * 0.5f, cy0 + (kHdrH - ts.y) * 0.5f),
                  IM_COL32(255,255,255,240), lbl);
    }

    // Card border
    dl->AddRect(ImVec2(lx, cy0), ImVec2(rx, cy0+cardH),
                borderCol, 6.0f * absScX, 0, 1.5f);

    if (absScX > 0.05f) {
      if (!showBack) {
        // ── FRONT: face image + player name ──────────────────────────────
        float fw = faceS * absScX;
        float fx = cx - fw * 0.5f;
        float fy = cy0 + kHdrH + 1.0f;
        if (faceTex && has) {
          dl->AddRectFilled(ImVec2(fx, fy), ImVec2(fx+fw, fy+faceS),
                            IM_COL32(30,40,65,255), 3.0f * absScX);
          dl->AddImage((ImTextureID)(intptr_t)faceTex,
                       ImVec2(fx, fy), ImVec2(fx+fw, fy+faceS));
        } else {
          dl->AddRectFilled(ImVec2(fx, fy), ImVec2(fx+fw, fy+faceS),
                            IM_COL32(28, 36, 58, has ? 190 : 80), 3.0f * absScX);
          if (has && absScX > 0.3f) {
            float cr = fw * 0.27f;
            dl->AddCircleFilled(ImVec2(fx+fw*0.5f, fy+faceS*0.36f), cr,
                                IM_COL32(55,65,95,210));
            dl->AddRectFilled(ImVec2(fx+fw*0.14f, fy+faceS*0.65f),
                              ImVec2(fx+fw*0.86f, fy+faceS),
                              IM_COL32(55,65,95,210), 3.0f);
          }
        }
        // Player name — with bottom margin (5px from face bottom to text top)
        if (absScX > 0.28f) {
          const char *nameLbl = has ? nodes[ni].name.c_str() : "TBD";
          ImVec2 ns2 = ImGui::CalcTextSize(nameLbl);
          float  ny2 = cy0 + kHdrH + faceS + 5.0f;
          ImU32  nc  = has ? IM_COL32(220,235,255,220) : IM_COL32(90,95,115,150);
          dl->AddText(ImVec2(cx - ns2.x * 0.5f, ny2), nc, nameLbl);
        }
      } else {
        // ── BACK: ability stars + potential stars + foot ──────────────────
        // Content uses fixed full-width coords; fade alpha hides the sliding artefact
        float backAlpha = (absScX - 0.35f) / 0.65f;
        if (backAlpha < 0.f) backAlpha = 0.f;
        if (backAlpha > 1.f) backAlpha = 1.f;

        if (backAlpha > 0.01f) {
          // Fixed position — never slides as card expands
          float bx = cx0 + 4.0f;
          float by = cy0 + kHdrH + 5.0f;

          // Star renderer: 5 small pill segments, drawn directly to dl
          auto DrawStarsDL = [&](float sx, float sy,
                                 float value, float maxVal, ImU32 filledCol) {
            float stars = (maxVal > 0.f) ? (value / maxVal) * 5.f : 0.f;
            if (stars < 0.f) stars = 0.f;
            if (stars > 5.f) stars = 5.f;
            const float kW = 8.f, kH = 6.f, kGp = 2.f;
            ImU32 emptyA = IM_COL32(30, 44, 72, (int)(160.f * backAlpha));
            int   origA  = (int)((filledCol >> 24) & 0xFF);
            ImU32 fillA  = (filledCol & 0x00FFFFFF) |
                           ((ImU32)((int)((float)origA * backAlpha)) << 24);
            for (int si2 = 0; si2 < 5; si2++) {
              float x0  = sx + (float)si2 * (kW + kGp);
              float fill = std::max(0.f, std::min(1.f, stars - (float)si2));
              dl->AddRectFilled(ImVec2(x0, sy), ImVec2(x0+kW, sy+kH), emptyA, 2.f);
              if (fill > 0.02f)
                dl->AddRectFilled(ImVec2(x0, sy),
                                  ImVec2(x0+kW*fill, sy+kH), fillA, 2.f);
            }
          };

          PushMgrFont(g_ManagerFontSmall);
          float lineH = ImGui::CalcTextSize("X").y;
          const float kStarH = 6.f;
          ImU32 lblCol = IM_COL32(180,190,220, (int)(200.f * backAlpha));

          // Ability label + stars
          dl->AddText(ImVec2(bx, by), lblCol, "Ability");
          by += lineH + 2.f;
          DrawStarsDL(bx, by + (kStarH == 0 ? 0 : 0), nodes[ni].baseStat, 1.0f, C32(kGold));
          by += kStarH + 7.f;

          // Potential label + stars
          dl->AddText(ImVec2(bx, by), lblCol, "Potential");
          by += lineH + 2.f;
          DrawStarsDL(bx, by, (float)nodes[ni].potential, 200.0f,
                      IM_COL32(100,160,220,220));
          by += kStarH + 8.f;

          // Foot
          bool isLeft  = (!nodes[ni].foot.empty() &&
                          (nodes[ni].foot[0]=='L' || nodes[ni].foot[0]=='l'));
          bool isRight = (!nodes[ni].foot.empty() &&
                          (nodes[ni].foot[0]=='R' || nodes[ni].foot[0]=='r'));
          const char *footStr = isLeft  ? "Left Footed"
                              : isRight ? "Right Footed" : "-";
          int fBaseA = isLeft ? 230 : isRight ? 230 : 180;
          ImU32 footCol = isLeft
            ? IM_COL32(100,170,255, (int)(fBaseA * backAlpha))
            : isRight
            ? IM_COL32(100,220,130, (int)(fBaseA * backAlpha))
            : IM_COL32(120,120,140, (int)(fBaseA * backAlpha));
          dl->AddText(ImVec2(bx, by), footCol, footStr);

          PopMgrFont(g_ManagerFontSmall);
        }
      }
    }

    // Stamina bar — below the card, always full width
    if (has) {
      float stVal = (float)nodes[ni].stamina;
      if (stVal < 0.0f) stVal = 0.0f;
      if (stVal > 100.0f) stVal = 100.0f;
      float ratio = stVal / 100.0f;
      ImU32 stCol = (stVal >= 70.0f) ? IM_COL32(60, 200, 80, 220)
                  : (stVal >= 40.0f) ? IM_COL32(220, 180, 40, 220)
                                     : IM_COL32(210, 60, 60, 220);
      float by = cy0 + cardH + 4.0f;
      dl->AddRectFilled(ImVec2(cx0, by), ImVec2(cx0+cardW, by+5.0f),
                        IM_COL32(0,0,0,100), 2.0f);
      if (ratio > 0.0f)
        dl->AddRectFilled(ImVec2(cx0, by), ImVec2(cx0+cardW*ratio, by+5.0f),
                          stCol, 2.0f);
    }
  }
  } // end if (!s_showTeamInstructions)

  dl->PopClipRect();

  // Pitch border on top
  dl->AddRect(ImVec2(pitchX, pitchY),
              ImVec2(pitchX + pitchW, pitchY + pitchH),
              IM_COL32(255, 255, 255, 30), 12.0f, 0, 1.5f);

  ImGui::Dummy(ImVec2(boardW, usH));

  ImGui::SetCursorScreenPos(ImVec2(pitchX, pitchY + pitchH));
  ImGui::EndGroup();

  // ===== Player list =======================================================
  ImGui::SameLine(0.0f, kGap);
  ImGui::BeginGroup();

  // Collect and sort XI
  std::vector<const CareerHubState::Player*> xi, subs;
  for (const auto &p : g_CareerHub.players) {
    if (p.formationOrder >= 0 && p.formationOrder <= 10)       xi.push_back(&p);
    else if (p.formationOrder >= 11 && p.formationOrder <= 19) subs.push_back(&p);
  }
  std::sort(xi.begin(), xi.end(),
    [](const CareerHubState::Player *a, const CareerHubState::Player *b){
      return a->formationOrder < b->formationOrder; });
  std::sort(subs.begin(), subs.end(),
    [](const CareerHubState::Player *a, const CareerHubState::Player *b){
      return a->formationOrder < b->formationOrder; });

  // Player list card
  BeginModernCard("##taclist", ImVec2(listW, usH));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##tacscroll", ImVec2(listW - 14.0f, usH - 14.0f), false);

  float rowW  = listW - 14.0f;
  float rowH  = 48.0f;
  float innerW = rowW - 2.0f;

  ImDrawList *wdl = ImGui::GetWindowDrawList();

  // Section header helper
  auto DrawSectionHeader = [&](const char *label, int count) {
    ImVec2 hp = ImGui::GetCursorScreenPos();
    wdl->AddRectFilled(hp, ImVec2(hp.x + innerW, hp.y + 22.0f),
                       IM_COL32(12, 20, 45, 200));
    wdl->AddRectFilled(hp, ImVec2(hp.x + 3.0f, hp.y + 22.0f),
                       C32(kAccent));
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    char buf[64]; snprintf(buf, sizeof(buf), "  %s  (%d)", label, count);
    ImGui::TextUnformatted(buf);
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
  };

  // ---- Starting XI ----
  DrawSectionHeader("Starting XI", (int)xi.size());

  bool slotFilled[11] = {};
  for (const auto *p : xi) slotFilled[p->formationOrder] = true;

  bool alt = false;
  for (const auto *p : xi) {
    alt = !alt;
    std::string dispName = p->firstName.empty() ? p->lastName :
                           (p->lastName.empty() ? p->firstName :
                            p->firstName.substr(0,1) + ". " + p->lastName);
    char popId[32]; snprintf(popId, sizeof(popId), "##tlrow_%d", p->id);
    DrawTacPlayerRow(wdl, innerW, rowH, p->id, p->formationOrder,
                     dispName, false, faceTex, alt, popId, p->baseStat, p->role);
  }
  // Empty XI slots
  for (int fo = 0; fo <= 10; fo++) {
    if (slotFilled[fo]) continue;
    alt = !alt;
    char dId[32]; snprintf(dId, sizeof(dId), "##tlempty_%d", fo);
    DrawTacPlayerRow(wdl, innerW, rowH, -1, fo, "TBD", false, 0, alt, dId);
  }

  ImGui::Spacing();

  // ---- Substitutes ----
  DrawSectionHeader("Substitutes", (int)subs.size());

  alt = false;
  for (const auto *p : subs) {
    alt = !alt;
    std::string dispName = p->firstName.empty() ? p->lastName :
                           (p->lastName.empty() ? p->firstName :
                            p->firstName.substr(0,1) + ". " + p->lastName);
    char popId[32]; snprintf(popId, sizeof(popId), "##tlsub_%d", p->id);
    DrawTacPlayerRow(wdl, innerW, rowH, p->id, p->formationOrder,
                     dispName, true, faceTex, alt, popId, p->baseStat, p->role);
  }
  if (subs.empty()) {
    ImVec2 ep = ImGui::GetCursorScreenPos();
    wdl->AddText(ImVec2(ep.x + 8.0f, ep.y + 6.0f),
                 C32(kTextDim), "No substitutes assigned");
    ImGui::Dummy(ImVec2(innerW, 22.0f));
  }

  ImGui::EndChild();
  ImGui::PopStyleColor();
  EndModernCard();

  ImGui::EndGroup();
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

  // Reset page-specific init flags whenever the user navigates away
  static e_ManagerPage s_prevPage = PAGE_HOME;
  if (s_prevPage != g_activePage) {
    if (s_prevPage == PAGE_CALENDAR)
      s_calInit = false;
    if (s_prevPage == PAGE_SCHEDULE) {
      s_schedInit     = false;
      s_schedClubInit = false;
    }
    if (s_prevPage == PAGE_COMPETITIONS)
      s_compInit = false;
  }
  s_prevPage = g_activePage;

  switch (g_activePage) {
    case PAGE_HOME:         DrawHomePage(contentW, workH);         break;
    case PAGE_SQUAD:        DrawSquadPage(contentW, workH);        break;
    case PAGE_TACTICS:      DrawTacticsPage(contentW, workH);      break;
    case PAGE_CALENDAR:     DrawCalendarPage(contentW, workH);     break;
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


  // ---- Speed selector ----------------------------------------------------
  // Rendered as small toggle buttons; click does NOT trigger match start.
  static bool s_speedClickConsumed = false;
  s_speedClickConsumed = false;
  if (!g_PreMatchLineup.continueRequested) {
    const int speeds[]      = { 1, 2, 4, 8 };
    const char *labels[]    = { "x1", "x2", "x4", "x8" };
    const float btnW        = 52.0f;
    const float btnH        = 28.0f;
    const float gap         = 8.0f;
    const float rowW        = 4 * btnW + 3 * gap;
    float bx = (cardW - rowW) * 0.5f;
    float by = cardH - 80.0f;

    ImDrawList *sdl = ImGui::GetWindowDrawList();
    ImVec2 wpos = ImGui::GetWindowPos();

    for (int i = 0; i < 4; i++) {
      bool active = (g_PreMatchLineup.matchSpeed == speeds[i]);
      ImU32 bgCol  = active ? IM_COL32(255, 200, 40, 255)  : IM_COL32(30, 42, 70, 220);
      ImU32 txtCol = active ? IM_COL32(15,  15,  15, 255)  : IM_COL32(180, 200, 240, 220);

      ImGui::SetCursorPos(ImVec2(bx, by));
      ImGui::PushID(i + 9000);
      if (ImGui::InvisibleButton("##spd", ImVec2(btnW, btnH))) {
        g_PreMatchLineup.matchSpeed = speeds[i];
        s_speedClickConsumed = true;
      }
      ImGui::PopID();

      ImVec2 bmin = ImVec2(wpos.x + bx, wpos.y + by);
      ImVec2 bmax = ImVec2(bmin.x + btnW, bmin.y + btnH);
      sdl->AddRectFilled(bmin, bmax, bgCol, 5.0f);
      sdl->AddRect(bmin, bmax, IM_COL32(80, 110, 180, 160), 5.0f, 0, 1.0f);
      {
        ImVec2 tsz = g_ManagerFontBold
            ? g_ManagerFontBold->CalcTextSizeA(15.0f, FLT_MAX, 0.f, labels[i])
            : ImGui::CalcTextSize(labels[i]);
        sdl->AddText(g_ManagerFontBold, 15.0f,
                     ImVec2(bmin.x + (btnW - tsz.x) * 0.5f,
                            bmin.y + (btnH - tsz.y) * 0.5f),
                     txtCol, labels[i]);
      }

      bx += btnW + gap;
    }

    // "Speed" label above buttons
    ImVec2 labelPos = ImVec2(wpos.x + (cardW - rowW) * 0.5f,
                             wpos.y + cardH - 80.0f - 20.0f);
    sdl->AddText(nullptr, 12.0f, labelPos,
                 IM_COL32(120, 140, 180, 180), "Match Speed");
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
    if (!s_speedClickConsumed && ImGui::IsMouseClicked(0)) doContinue = true;
    if (ImGui::IsKeyPressed(ImGuiKey_Enter)  ||
        ImGui::IsKeyPressed(ImGuiKey_Space)  ||
        ImGui::IsKeyPressed(ImGuiKey_Escape))   doContinue = true;

    if (doContinue) {
      printf("[PREMATCH] Continue requested (speed x%d)\n", g_PreMatchLineup.matchSpeed);
      g_PreMatchLineup.continueRequested = true;
    }
  }

  ImGui::End();
}
