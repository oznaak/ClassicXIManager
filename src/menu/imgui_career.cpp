#include "imgui_career.hpp"
#include "imgui_manager_fonts.hpp"
#include "user_transfer.hpp"
#include "transfer_engine.hpp"

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
#include <set>
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
#include "menu/imgui_menu.hpp"

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

  // Glyph range: Latin, Latin Extended, Greek, Cyrillic, punctuation, currency.
  static const ImWchar kGlyphRanges[] = {
    0x0020, 0x024F,  // Latin + Latin-1 + Latin Extended-A/B
    0x0370, 0x03FF,  // Greek
    0x0400, 0x052F,  // Cyrillic + supplement
    0x1E00, 0x1EFF,  // Latin Extended Additional
    0x2000, 0x206F,  // General punctuation
    0x20A0, 0x20CF,  // Currency symbols
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

static bool DBHasColumn(const std::string &table, const std::string &column) {
  std::stringstream q;
  q << "PRAGMA table_info(" << table << ");";
  DatabaseResult *r = GetDB()->Query(q.str());
  bool found = false;
  if (r) {
    for (unsigned int i = 0; i < r->data.size(); i++) {
      if (r->data.at(i).size() > 1 && r->data.at(i).at(1) == column) {
        found = true;
        break;
      }
    }
  }
  delete r;
  return found;
}

static std::string SqlEsc(const std::string &in) {
  std::string out;
  for (char c : in) { if (c == '\'') out += "''"; else out += c; }
  return out;
}

// Returns the currency symbol for the league that owns the given club.
static std::string ClubCurrency(int clubId) {
  std::stringstream q;
  q << "SELECT l.currency FROM leagues l"
    << " JOIN teams t ON t.league_id = l.id"
    << " WHERE t.id = " << clubId << ";";
  DatabaseResult *r = GetDB()->Query(q.str());
  std::string cur = (r && r->data.size() > 0) ? DBCell(r, 0, 0) : "";
  delete r;
  if (cur.empty()) cur = "\xE2\x82\xAC"; // fallback: €
  return cur;
}

// Looks up a message_templates row by subcategory, fills placeholders, writes
// subject/body by reference, and returns the template id (0 if not found).
static int FillSponsorTemplate(const std::string &subcategory,
                                const std::string &mgrName,
                                const std::string &clubNameStr,
                                const std::string &sponsorName,
                                const std::string &currency,
                                long long          weeklyValue,
                                std::string &outSubject,
                                std::string &outBody) {
  std::stringstream q;
  q << "SELECT id, subject_template, body_template FROM message_templates"
    << " WHERE subcategory='" << SqlEsc(subcategory) << "' LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str());
  int tid = 0;
  if (r && r->data.size() > 0) {
    tid         = atoi(DBCell(r, 0, 0).c_str());
    outSubject  = DBCell(r, 0, 1);
    outBody     = DBCell(r, 0, 2);
  }
  delete r;

  char valBuf[32];
  snprintf(valBuf, sizeof(valBuf), "%lld", weeklyValue);

  auto replace = [](std::string &s, const std::string &from, const std::string &to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    }
  };

  replace(outSubject, "%ManagerName%",  mgrName);
  replace(outSubject, "%ClubName%",     clubNameStr);
  replace(outSubject, "%SponsorName%",  sponsorName);
  replace(outSubject, "%WeeklyValue%",  valBuf);
  replace(outSubject, "%Currency%",     currency);
  replace(outBody,    "%ManagerName%",  mgrName);
  replace(outBody,    "%ClubName%",     clubNameStr);
  replace(outBody,    "%SponsorName%",  sponsorName);
  replace(outBody,    "%WeeklyValue%",  valBuf);
  replace(outBody,    "%Currency%",     currency);

  return tid;
}

// ---- Badge texture cache ------------------------------------------------

static std::map<std::string, GLuint> s_BadgeCache;
static std::map<GLuint, std::pair<int,int>> s_BadgeDims;

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
  s_BadgeDims[texID] = { rgba->w, rgba->h };
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
  s_BadgeDims.clear();
}

static ImVec2 BadgeFitInSquare(GLuint tex, float sz) {
  auto it = s_BadgeDims.find(tex);
  if (it == s_BadgeDims.end() || it->second.first <= 0 || it->second.second <= 0)
    return ImVec2(sz, sz);
  float iw = (float)it->second.first;
  float ih = (float)it->second.second;
  float aspect = iw / ih;
  return aspect >= 1.0f ? ImVec2(sz, sz / aspect) : ImVec2(sz * aspect, sz);
}

// ---- State management ---------------------------------------------------

// forward declaration
static void ResetNavState();

void CareerHubState::Clear() {
  active        = false;
  primaryTab    = 0;
  activeTab     = 0;
  pendingAction = 0;
  onMainMenu    = nullptr;
  onAdvance     = nullptr;
  onPlayFixture = nullptr;
  onAdvanceUntilMatch = nullptr;
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
  advanceMode          = ADVANCE_MODE_NEXT_DAY;
  onStartNextSeason    = nullptr;
  manager = {};
  club    = {};
  players.clear();
  fixtures.clear();
  standings.clear();
  tactics.clear();
  staff.clear();
  scoutQueue.clear();
  scoutReports.clear();
  inbox.clear();
  activeSponsors.clear();
  pendingOffers.clear();
  finances = {};
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

// Mutable accent — updated from club color1 in LoadFromDB
static ImVec4 kAccent  = ImVec4(0.741f, 0.102f, 0.788f, 1.0f);
static ImVec4 kAccentH = ImVec4(0.863f, 0.318f, 0.918f, 1.0f);
static ImVec4 kAccentA = ImVec4(0.576f, 0.047f, 0.620f, 1.0f);

// ---- League finance parameters ------------------------------------------
// Starting balance scales between minBalance (worst club, rep≈5) and
// maxBalance (best club, rep≈19) using the club's average player reputation.
// Weekly TV also gets a small merit uplift for stronger clubs (+/- 15%).
// Everything else (operating, matchday, prizes) stays league-fixed.
struct LeagueFP {
  int       id;
  long long minBalance;    // £ — weakest club in this league tier
  long long maxBalance;    // £ — strongest club in this league tier
  long long weeklyTV;      // base weekly TV rights (merit-scaled ±15% at runtime)
  long long weeklyOperating;
  long long matchdayHome;  // per home game (bigger clubs attract bigger gates)
  long long prize[6];      // by finishing position, index 0=1st
};
//
// Tier calibration (real-world rough anchors):
//   PL:          bottom club (e.g. Luton) ~£15M, top (Man City) ~£200M
//   Bundesliga:  Bochum ~£8M, Bayern ~£120M
//   Eredivisie:  Go Ahead Eagles ~£3M, Ajax/PSV ~£20M
//   La Liga:     Getafe ~£10M, Real Madrid ~£180M
//   Unknown:     treated as a mid-tier domestic league (Liga Portugal tier)
//
static const LeagueFP kLeagueFinanceParams[] = {
  // Premier League
  { 1,  15000000LL, 200000000LL, 1500000LL, 150000LL, 3000000LL,
    { 120000000LL, 90000000LL, 75000000LL, 65000000LL, 58000000LL, 52000000LL } },
  // Bundesliga
  { 2,   8000000LL, 120000000LL,  900000LL, 100000LL, 2000000LL,
    {  50000000LL, 35000000LL, 28000000LL, 22000000LL, 18000000LL, 15000000LL } },
  // Eredivisie
  { 3,   1500000LL,  20000000LL,  200000LL,  50000LL,  500000LL,
    {   5000000LL,  3000000LL,  2000000LL,  1500000LL,  1000000LL,   800000LL } },
  // La Liga
  { 4,  10000000LL, 180000000LL, 1200000LL, 120000LL, 2500000LL,
    {  80000000LL, 55000000LL, 42000000LL, 33000000LL, 27000000LL, 22000000LL } },
};
static const int kLFPCount = 4;

// Fallback for leagues not in the table (Liga Portugal, Championship, etc.)
// Calibrated as a mid-lower domestic league: Benfica-tier top ~£18M, AVS-tier ~£1.5M
static const LeagueFP kLeagueFPDefault = {
  0,   1500000LL,  18000000LL,  150000LL,  40000LL,  300000LL,
  {   4000000LL,  2500000LL,  1800000LL,  1200000LL,   800000LL,   500000LL }
};

static const LeagueFP *GetLeagueFP(int leagueId) {
  for (int i = 0; i < kLFPCount; i++)
    if (kLeagueFinanceParams[i].id == leagueId) return &kLeagueFinanceParams[i];
  return &kLeagueFPDefault;
}

// Returns 0.0–1.0 quality factor from average reputation of the club's top-11 players.
// reputation is on a 1–20 scale; we normalise against a practical range of 4–19.
static float CalcClubQualityFactor(const std::vector<CareerHubState::Player> &squad) {
  if (squad.empty()) return 0.35f; // mid-table default
  std::vector<float> reps;
  reps.reserve(squad.size());
  for (const auto &p : squad) reps.push_back(p.reputation);
  std::sort(reps.rbegin(), reps.rend());
  int n = std::min((int)reps.size(), 11);
  float sum = 0.0f;
  for (int i = 0; i < n; i++) sum += reps[i];
  float avg = sum / (float)n;
  // Map practical rep range [4, 19] → [0.0, 1.0]
  float factor = (avg - 4.0f) / (19.0f - 4.0f);
  if (factor < 0.0f) factor = 0.0f;
  if (factor > 1.0f) factor = 1.0f;
  return factor;
}

// ---- Multi-club finance system -------------------------------------------

struct ClubFinances {
  int       club_id               = 0;
  long long cash_balance          = 0;
  long long wage_budget           = 0;
  long long transfer_budget       = 0;
  int       board_confidence      = 50;
  float     commercial_strength   = 0.5f;
  float     institutional_power   = 0.3f;
  int       style_seed            = 0;
  long long debt_level            = 0;
  int       shock_cooldown        = 0;
  float     commercial_shock_mult = 1.0f;
  int       bad_contract_weeks    = 0;
  int       transfer_budget_frozen = 0;
  int       emergency_credit_used  = 0;
  int       wage_overrun_weeks    = 0;
  std::string last_weekly_date;
  int       season_budget_processed = 0;
  int       season_prize_paid     = 0;
};

struct FinancialStyle {
  float wage_tolerance;
  float transfer_spend_bias;
  float commercial_variance;
  float selling_bias;
  float risk_appetite;
  float saving_rate;
  float shock_vulnerability;
  float shock_opportunity;
  int   sell_threshold;
};

// Deterministic float in [0,1) from a seed + index.
static float SeededRand(unsigned int seed, int index) {
  unsigned int s = seed + (unsigned int)(index * 2654435769u);
  s ^= s >> 16; s *= 0x45d9f3bu; s ^= s >> 16;
  return (float)(s & 0x7FFFFFFFu) / (float)0x7FFFFFFFu;
}

// Archetype is derived at runtime — never stored as a label.
static FinancialStyle DeriveStyle(int style_seed) {
  switch (((unsigned int)style_seed) % 6) {
    case 0: return { 0.52f, 0.55f, 0.08f, 0.10f, 0.05f, 0.70f, 0.60f, 1.20f, 25 }; // Conservative
    case 1: return { 0.68f, 0.85f, 0.15f, 0.15f, 0.35f, 0.25f, 1.10f, 0.90f, 20 }; // Aggressive
    case 2: return { 0.80f, 1.00f, 0.25f, 0.10f, 0.60f, 0.05f, 1.80f, 0.70f, 15 }; // Gambling
    case 3: return { 0.45f, 0.40f, 0.10f, 0.35f, 0.08f, 0.55f, 0.75f, 1.40f, 30 }; // Youth-focused
    case 4: return { 0.48f, 0.25f, 0.08f, 0.65f, 0.05f, 0.60f, 0.50f, 1.10f, 40 }; // Selling
    default: return { 0.72f, 0.70f, 0.18f, 0.20f, 0.30f, 0.20f, 1.20f, 0.85f, 22 }; // Star-focused
  }
}

static void InitAllClubFinances(int managerId);
static void ProcessWeeklyAllClubs(int managerId, const std::string &currentDate,
                                  int clubId, const LeagueFP *playerLfp,
                                  long long playerWageBill);
static long long ApplyLoanWageSplitsToClub(int managerId, int clubId,
                                           long long baseWageBill) {
  std::stringstream lq;
  lq << "SELECT ld.loaning_club_id,ld.receiving_club_id,"
     << " COALESCE(pss.weekly_wage,p.weekly_wage),"
     << " ld.monthly_wage_parent_pct,ld.monthly_wage_receiving_pct,"
     << " COALESCE(pss.team_id,p.team_id)"
     << " FROM loan_deals ld JOIN players p ON p.id=ld.player_id"
     << " LEFT JOIN player_save_state pss"
     << " ON pss.manager_id=ld.manager_id AND pss.player_id=p.id"
     << " WHERE ld.manager_id=" << managerId
     << " AND ld.status='active'"
     << " AND (ld.loaning_club_id=" << clubId
     << " OR ld.receiving_club_id=" << clubId << ");";
  DatabaseResult *lr = GetDB()->Query(lq.str());
  long long adjusted = baseWageBill;
  if (lr) {
    for (unsigned int i = 0; i < lr->data.size(); i++) {
      int parent = atoi(DBCell(lr,i,0).c_str());
      int receiving = atoi(DBCell(lr,i,1).c_str());
      long long wage = atoll(DBCell(lr,i,2).c_str());
      int parentPct = atoi(DBCell(lr,i,3).c_str());
      int receivingPct = atoi(DBCell(lr,i,4).c_str());
      int currentClub = atoi(DBCell(lr,i,5).c_str());
      if (currentClub == clubId) adjusted -= wage;
      if (parent == clubId) adjusted += (wage * parentPct) / 100;
      if (receiving == clubId) adjusted += (wage * receivingPct) / 100;
    }
    delete lr;
  }
  return std::max(0LL, adjusted);
}
static void TriggerDebtCrisis(int managerId, int clubId, ClubFinances &cf,
                               const LeagueFP *lfp);
static bool TryFireShock(int managerId, int clubId, ClubFinances &cf,
                          const LeagueFP *lfp, int seasonYear,
                          long long estAnnualIncome, int domPrestige, int intlPrestige);
static void ProcessAnnualCycle(int managerId, int clubId, ClubFinances &cf,
                                const LeagueFP *lfp, int seasonYear,
                                int leagueId, float clubFactor,
                                int domPrestige, int intlPrestige);
static void CheckSponsorOffers(int managerId, int clubId, const std::string &currentDate,
                                int seasonYear);
static void ProcessSponsorRenewals(int managerId, int clubId, int seasonYear,
                                   int position, int totalClubs, int expectedPos,
                                   const std::string &mgrName, const std::string &clubName);

// Forward declarations (implementations follow in the player-detail helpers section)
static void ParsePlayerAttrsFromRow(CareerHubState::Player &p,
                                    DatabaseResult *r, int row, int col0);
static std::string DisplayName(const CareerHubState::Player &p);
static std::string FormatContractExpiry(const std::string &exp);
static std::vector<std::string> ParseAltPositions(const std::string &raw);
static void LoadPlayerFullDetail(int playerId, CareerHubState::Player &out);

// Shared column list for all full player SELECT queries (appended after base columns)
static const char *kPlayerAttrCols =
  " nickname, alternative_pos, skillMoves, weakFoot,"
  " nationality, weight, playervalue, jersey_number, international_reputation,"
  " Acceleration, SprintSpeed, Agility, Balance, Jumping, Strength, Reactions,"
  " Aggression, Composure, Interceptions, Positioning, Vision,"
  " BallControl, Crossing, Dribbling, Finishing, FkAccuracy, HeadingAccuracy,"
  " LongPassing, ShortPassing, DefensiveAwareness, ShotPower, LongShots,"
  " StandingTackle, SlidingTackle, Volleys, Curve, Penalties,"
  " GkDiving, GkHandling, GkKicking, GkReflexes, GkPositioning";

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
    q << "SELECT teams.name, teams.shortname, teams.logo_url, leagues.name, leagues.id, teams.color1, leagues.currency"
      << " FROM teams JOIN leagues ON teams.league_id = leagues.id"
      << " WHERE teams.id = " << clubId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str());
    club.name       = DBCell(r, 0, 0);
    club.shortName  = DBCell(r, 0, 1);
    club.logoPath   = DBCell(r, 0, 2);
    club.leagueName = DBCell(r, 0, 3);
    club.leagueId   = atoi(DBCell(r, 0, 4).c_str());
    {
      std::string cur = DBCell(r, 0, 6);
      club.currency = cur.empty() ? "\xE2\x82\xAC" : cur;
    }
    {
      std::string c1 = DBCell(r, 0, 5);
      int ri = 189, gi = 26, bi = 201; // fallback magenta
      if (!c1.empty()) sscanf(c1.c_str(), "%d , %d , %d", &ri, &gi, &bi);
      float fr = ri / 255.0f, fg = gi / 255.0f, fb = bi / 255.0f;
      kAccent  = ImVec4(fr,                   fg,                   fb,                   1.0f);
      kAccentH = ImVec4(fr*0.88f + 0.12f,     fg*0.88f + 0.12f,     fb*0.88f + 0.12f,     1.0f);
      kAccentA = ImVec4(fr*0.70f,              fg*0.70f,             fb*0.70f,             1.0f);
    }
    delete r;
  }

  players.clear();
  {
    const bool hasPlayerStamina = DBHasColumn("players", "player_stamina");
    std::stringstream q;
    q << "SELECT id, firstname, lastname, role, age, base_stat,"
      << " formationorder, COALESCE(pss.weekly_wage, players.weekly_wage),"
      << " COALESCE(pss.contract_expiry, players.contract_expiry), player_potential,"
      << " foot, stamina,"
      << (hasPlayerStamina ? " COALESCE(players.player_stamina,100)," : " 100,")
      << " height, reputation,"
      << " COALESCE(pa.injury_days_remaining,0), COALESCE(pa.injury_type,''),"
      << " COALESCE(pd.suspension_matches_remaining,0), COALESCE(pd.suspension_reason,''),"
      << " COALESCE(pms.apps,0), COALESCE(pms.goals,0), COALESCE(pms.assists,0), COALESCE(pms.avg_rating,0),"
      << kPlayerAttrCols
      << " FROM players LEFT JOIN player_save_state pss"
      << " ON pss.manager_id=" << mgrId << " AND pss.player_id=players.id"
      << " LEFT JOIN player_availability pa ON pa.manager_id=" << mgrId << " AND pa.player_id=players.id"
      << " LEFT JOIN player_discipline pd ON pd.manager_id=" << mgrId
      << " AND pd.player_id=players.id AND pd.season_year=" << seasonYear
      << " LEFT JOIN (SELECT player_id, COUNT(*) apps, SUM(goals) goals,"
      << " SUM(assists) assists, AVG(rating) avg_rating"
      << " FROM player_match_stats WHERE manager_id=" << mgrId
      << " AND season_year=" << seasonYear << " GROUP BY player_id) pms"
      << " ON pms.player_id=players.id"
      << " WHERE COALESCE(pss.team_id, players.team_id) = " << clubId
      << " ORDER BY"
      << "  CASE WHEN formationorder IS NULL OR formationorder < 0 THEN 999"
      << "       ELSE formationorder END ASC,"
      << "  players.base_stat DESC LIMIT 50;";
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
      std::string csStr  = DBCell(r, i, 12);
      p.currentStamina   = csStr.empty() ? 100 : atoi(csStr.c_str());
      std::string htStr  = DBCell(r, i, 13);
      p.height           = htStr.empty() ? 0.0f : (float)atof(htStr.c_str());
      std::string repStr = DBCell(r, i, 14);
      p.reputation       = repStr.empty() ? 0.0f : (float)atof(repStr.c_str());
      p.injuryDays       = atoi(DBCell(r, i, 15).c_str());
      p.injuryType       = DBCell(r, i, 16);
      p.suspensionMatches = atoi(DBCell(r, i, 17).c_str());
      p.suspensionReason = DBCell(r, i, 18);
      p.matchesPlayed    = atoi(DBCell(r, i, 19).c_str());
      p.goals            = atoi(DBCell(r, i, 20).c_str());
      p.assists          = atoi(DBCell(r, i, 21).c_str());
      p.avgRating        = (float)atof(DBCell(r, i, 22).c_str());
      ParsePlayerAttrsFromRow(p, r, i, 23);
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
    tactics["position_offense_depth_factor"]      = 0.5f;
    tactics["position_defense_depth_factor"]      = 0.5f;
    tactics["position_offense_width_factor"]      = 0.5f;
    tactics["position_defense_width_factor"]      = 0.5f;
    tactics["position_offense_midfieldfocus"]     = 0.5f;
    tactics["position_defense_midfieldfocus"]     = 0.5f;
    tactics["position_offense_sidefocus_strength"]  = 0.5f;
    tactics["position_defense_sidefocus_strength"]  = 0.5f;
    tactics["position_offense_microfocus_strength"] = 0.5f;
    tactics["position_defense_microfocus_strength"] = 0.5f;
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

  // Ensure career_staff table exists (created once per save)
  {
    DatabaseResult *r = GetDB()->Query(
      "CREATE TABLE IF NOT EXISTS career_staff ("
      "  manager_id INTEGER NOT NULL,"
      "  staff_id   INTEGER NOT NULL,"
      "  PRIMARY KEY (manager_id, staff_id),"
      "  FOREIGN KEY (staff_id) REFERENCES staff_list(staff_id)"
      ");");
    delete r;
  }

  // Load hired staff for this manager
  staff.clear();
  {
    std::stringstream q;
    q << "SELECT sl.staff_id, sl.firstname, sl.lastname, sl.nationality, sl.role,"
      << " (CAST(strftime('%Y', 'now') AS INTEGER) - CAST(strftime('%Y', sl.\"date-of-birth\") AS INTEGER)) as age,"
      << " sl.rating, sl.weekly_wage"
      << " FROM career_staff cs"
      << " JOIN staff_list sl ON cs.staff_id = sl.staff_id"
      << " WHERE cs.manager_id = " << managerId
      << " ORDER BY sl.role ASC;";
    DatabaseResult *r = GetDB()->Query(q.str());
    for (unsigned int i = 0; i < r->data.size(); i++) {
      StaffMember sm;
      sm.id          = atoi(DBCell(r, i, 0).c_str());
      sm.firstName   = DBCell(r, i, 1);
      sm.lastName    = DBCell(r, i, 2);
      sm.nationality = DBCell(r, i, 3);
      sm.role        = DBCell(r, i, 4);
      std::string ag = DBCell(r, i, 5);
      sm.age         = ag.empty() ? 0 : atoi(ag.c_str());
      sm.rating      = atoi(DBCell(r, i, 6).c_str());
      sm.weeklywage  = atoi(DBCell(r, i, 7).c_str());
      staff.push_back(sm);
    }
    delete r;
  }

  // ---- Finance system ---------------------------------------------------

  const LeagueFP *lfp = GetLeagueFP(club.leagueId);

  // ---- Finance schema (create / migrate) ---------------------------------
  {
    // club_finances (replaces career_finances)
    DatabaseResult *r = GetDB()->Query(
      "CREATE TABLE IF NOT EXISTS club_finances ("
      "  manager_id              INTEGER NOT NULL,"
      "  club_id                 INTEGER NOT NULL,"
      "  cash_balance            INTEGER DEFAULT 0,"
      "  wage_budget             INTEGER DEFAULT 0,"
      "  transfer_budget         INTEGER DEFAULT 0,"
      "  board_confidence        INTEGER DEFAULT 50,"
      "  commercial_strength     REAL    DEFAULT 0.5,"
      "  institutional_power     REAL    DEFAULT 0.3,"
      "  style_seed              INTEGER DEFAULT 0,"
      "  debt_level              INTEGER DEFAULT 0,"
      "  shock_cooldown          INTEGER DEFAULT 0,"
      "  commercial_shock_mult   REAL    DEFAULT 1.0,"
      "  bad_contract_weeks      INTEGER DEFAULT 0,"
      "  transfer_budget_frozen  INTEGER DEFAULT 0,"
      "  emergency_credit_used   INTEGER DEFAULT 0,"
      "  wage_overrun_weeks      INTEGER DEFAULT 0,"
      "  last_weekly_date        TEXT,"
      "  season_budget_processed INTEGER DEFAULT 0,"
      "  season_prize_paid       INTEGER DEFAULT 0,"
      "  PRIMARY KEY (manager_id, club_id)"
      ");");
    delete r;

    // finance_transactions — ensure table exists with club_id column
    r = GetDB()->Query(
      "CREATE TABLE IF NOT EXISTS finance_transactions ("
      "  id          INTEGER PRIMARY KEY AUTOINCREMENT,"
      "  manager_id  INTEGER NOT NULL,"
      "  club_id     INTEGER NOT NULL DEFAULT 0,"
      "  date        TEXT    NOT NULL,"
      "  category    TEXT    NOT NULL,"
      "  description TEXT,"
      "  amount      INTEGER NOT NULL"
      ");");
    delete r;

    // Add club_id to finance_transactions if it's an existing table without it
    r = GetDB()->Query("PRAGMA table_info(finance_transactions);");
    bool hasTxClubId = false;
    for (unsigned int i = 0; i < r->data.size(); i++)
      if (DBCell(r, i, 1) == "club_id") { hasTxClubId = true; break; }
    delete r;
    if (!hasTxClubId) {
      r = GetDB()->Query(
        "ALTER TABLE finance_transactions ADD COLUMN club_id INTEGER NOT NULL DEFAULT 0;");
      delete r;
    }

    // Migrate existing career_finances row → club_finances (safe no-op if table absent or already migrated)
    {
      DatabaseResult *te = GetDB()->Query(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='career_finances';");
      bool hasOldTable = (te->data.size() > 0);
      delete te;
      if (hasOldTable) {
        std::stringstream mq;
        mq << "SELECT balance FROM career_finances WHERE manager_id=" << mgrId << ";";
        DatabaseResult *mr = GetDB()->Query(mq.str());
        if (mr->data.size() > 0 && !DBCell(mr, 0, 0).empty()) {
          long long oldBal = atoll(DBCell(mr, 0, 0).c_str());
          std::stringstream ins;
          ins << "INSERT OR IGNORE INTO club_finances (manager_id, club_id, cash_balance)"
              << " VALUES (" << mgrId << "," << cId << "," << oldBal << ");";
          DatabaseResult *ir = GetDB()->Query(ins.str());
          delete ir;
        }
        delete mr;
      }
    }

    // Per-career sponsor tables (sponsors reference data lives in the main database.sqlite)
    DatabaseResult *sp2 = GetDB()->Query(
      "CREATE TABLE IF NOT EXISTS club_sponsors ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT, manager_id INTEGER NOT NULL,"
      "  club_id INTEGER NOT NULL, sponsor_id INTEGER NOT NULL,"
      "  sponsor_name TEXT NOT NULL, weekly_value INTEGER NOT NULL DEFAULT 0,"
      "  season_year INTEGER NOT NULL,"
      "  UNIQUE(manager_id, club_id, sponsor_id, season_year));");
    delete sp2;
    DatabaseResult *sp3 = GetDB()->Query(
      "CREATE TABLE IF NOT EXISTS pending_sponsor_offers ("
      "  id INTEGER PRIMARY KEY AUTOINCREMENT, manager_id INTEGER NOT NULL,"
      "  sponsor_id INTEGER NOT NULL, sponsor_name TEXT NOT NULL,"
      "  weekly_value INTEGER NOT NULL DEFAULT 0, offered_date TEXT NOT NULL,"
      "  status TEXT NOT NULL DEFAULT 'pending');");
    delete sp3;
    DatabaseResult *sp4 = GetDB()->Query(
      "CREATE TABLE IF NOT EXISTS sponsor_blacklist ("
      "  manager_id INTEGER NOT NULL, sponsor_id INTEGER NOT NULL,"
      "  blacklisted_season INTEGER NOT NULL,"
      "  PRIMARY KEY (manager_id, sponsor_id, blacklisted_season));");
    delete sp4;
  }

  // Init all clubs on first load for this manager
  {
    std::stringstream ck;
    ck << "SELECT COUNT(*) FROM club_finances WHERE manager_id=" << mgrId << ";";
    DatabaseResult *cr = GetDB()->Query(ck.str());
    int rowCount = (cr->data.size() > 0 && !DBCell(cr, 0, 0).empty())
                  ? atoi(DBCell(cr, 0, 0).c_str()) : 0;
    delete cr;
    if (rowCount == 0) InitAllClubFinances(mgrId);
  }

  // Helper: insert a finance transaction and update club_finances.cash_balance atomically
  auto InsertTx = [&](const std::string &date, const std::string &cat,
                      const std::string &desc, long long amount) {
    std::stringstream tx;
    tx << "INSERT INTO finance_transactions (manager_id, club_id, date, category, description, amount)"
       << " VALUES (" << mgrId << "," << cId << ",'" << date << "','"
       << cat << "','" << desc << "'," << amount << ");";
    DatabaseResult *r = GetDB()->Query(tx.str());
    delete r;
    std::stringstream bq;
    bq << "UPDATE club_finances SET cash_balance = cash_balance + " << amount
       << " WHERE manager_id=" << mgrId << " AND club_id=" << cId << ";";
    r = GetDB()->Query(bq.str());
    delete r;
  };

  // Calculate weekly wage bill from loaded players + staff
  long long wageBill = 0;
  for (const auto &p : players) wageBill += p.weeklywage;
  for (const auto &sm : staff)  wageBill += sm.weeklywage;
  long long adjustedWageBill = ApplyLoanWageSplitsToClub(mgrId, cId, wageBill);

  // Process weekly finances if at least 7 game-days have passed
  if (!currentDate.empty()) {
    std::stringstream lwq;
    lwq << "SELECT last_weekly_date FROM club_finances"
        << " WHERE manager_id=" << mgrId << " AND club_id=" << cId << ";";
    DatabaseResult *lwr = GetDB()->Query(lwq.str());
    std::string lastWeekly = (lwr->data.size() > 0) ? DBCell(lwr, 0, 0) : "";
    delete lwr;

    bool doWeekly = lastWeekly.empty();
    if (!doWeekly && !currentDate.empty()) {
      std::stringstream dq;
      dq << "SELECT julianday('" << currentDate << "') - julianday('" << lastWeekly << "') >= 7;";
      DatabaseResult *dr = GetDB()->Query(dq.str());
      if (dr->data.size() > 0) doWeekly = (DBCell(dr, 0, 0) == "1");
      delete dr;
    }

    if (doWeekly) {
      // Player's club: full transaction logging
      float qual     = CalcClubQualityFactor(players);
      long long tv   = (long long)(lfp->weeklyTV * (0.85f + qual * 0.30f));
      InsertTx(currentDate, "tv_rights", "Weekly TV rights distribution", tv);
      if (adjustedWageBill > 0)
        InsertTx(currentDate, "wages",    "Weekly player & staff wages",   -adjustedWageBill);
      InsertTx(currentDate, "operating", "Weekly club operating costs",    -lfp->weeklyOperating);

      // Sponsor income: sum all active sponsorships for this week
      if (seasonYear > 0) {
        std::stringstream spq;
        spq << "SELECT SUM(weekly_value) FROM club_sponsors"
            << " WHERE manager_id=" << mgrId << " AND club_id=" << cId
            << " AND season_year=" << seasonYear << ";";
        DatabaseResult *spr = GetDB()->Query(spq.str());
        if (spr->data.size() > 0 && !DBCell(spr, 0, 0).empty()) {
          long long sponsorIncome = atoll(DBCell(spr, 0, 0).c_str());
          if (sponsorIncome > 0)
            InsertTx(currentDate, "sponsorship", "Weekly sponsor income", sponsorIncome);
        }
        delete spr;
      }

      std::stringstream upd;
      upd << "UPDATE club_finances SET last_weekly_date='" << currentDate
          << "' WHERE manager_id=" << mgrId << " AND club_id=" << cId << ";";
      DatabaseResult *ur = GetDB()->Query(upd.str());
      delete ur;
      printf("[FINANCE] Weekly player_club=%d date=%s wages=-%lld TV=+%lld op=-%lld\n",
             cId, currentDate.c_str(), adjustedWageBill, tv, lfp->weeklyOperating);

      // Check for new sponsor offers (player's club only)
      if (seasonYear > 0)
        CheckSponsorOffers(mgrId, cId, currentDate, seasonYear);

      // All other clubs: balance-only tick
      ProcessWeeklyAllClubs(mgrId, currentDate, cId, lfp, adjustedWageBill);
    }

    // Matchday income: scan all played fixtures involving this club (home or away).
    // Home games: full gate receipts.  Away games: 30% allocation (league revenue share).
    if (seasonYear > 0 && cId > 0) {
      float qual2 = CalcClubQualityFactor(players);
      // Dedup subquery: find fixture IDs already paid (stored as "H:<id>" or "A:<id>")
      std::string alreadyPaid =
        "SELECT CAST(SUBSTR(description,3) AS INTEGER) FROM finance_transactions"
        " WHERE manager_id=" + std::to_string(mgrId) + " AND category='matchday'"
        " AND (description GLOB 'H:[0-9]*' OR description GLOB 'A:[0-9]*')";

      // Helper: get team quality factor (avg reputation of top-11 players, normalised 0-1)
      auto TeamQuality = [&](int teamId) -> float {
        std::stringstream tq;
        tq << "SELECT reputation FROM players WHERE team_id=" << teamId
           << " ORDER BY reputation DESC LIMIT 11;";
        DatabaseResult *tr = GetDB()->Query(tq.str());
        float sum = 0.0f; int n = 0;
        for (unsigned int k = 0; k < tr->data.size(); k++) {
          float r = atof(DBCell(tr, k, 0).c_str()); sum += r; n++;
        }
        delete tr;
        if (n == 0) return 0.35f;
        float avg = sum / (float)n;
        float f = (avg - 4.0f) / (19.0f - 4.0f);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        return f;
      };

      // Helper: league position factor for a team (0=best/1st, 1=worst/last)
      auto LeaguePosFactor = [&](int teamId) -> float {
        std::stringstream pq;
        pq << "SELECT team_id FROM standings WHERE manager_id=" << mgrId
           << " AND league_id=(SELECT league_id FROM teams WHERE id=" << teamId << " LIMIT 1)"
           << " ORDER BY points DESC, goal_difference DESC, goals_for DESC;";
        DatabaseResult *pr = GetDB()->Query(pq.str());
        int pos = 1, total = (int)pr->data.size();
        for (int k = 0; k < total; k++) {
          if (atoi(DBCell(pr, k, 0).c_str()) == teamId) { pos = k + 1; break; }
        }
        delete pr;
        if (total <= 1) return 0.5f;
        return (float)(pos - 1) / (float)(total - 1); // 0=1st, 1=last
      };

      // Matchday income = lerp between min and max based on combined attractiveness
      // attractiveness: 60% from opponent quality, 20% from user's league position (top=good), 20% user quality
      auto CalcMatchdayAmt = [&](int opponentId, bool isHome) -> long long {
        float oppQ   = TeamQuality(opponentId);
        float oppPos = 1.0f - LeaguePosFactor(opponentId); // 1=1st, 0=last
        float usrPos = 1.0f - LeaguePosFactor(cId);
        float attract = oppQ * 0.50f + oppPos * 0.25f + usrPos * 0.15f + qual2 * 0.10f;
        if (attract < 0.0f) attract = 0.0f;
        if (attract > 1.0f) attract = 1.0f;
        long long mdMin = (long long)(lfp->matchdayHome * 0.25f);
        long long mdMax = (long long)(lfp->matchdayHome * (0.80f + qual2 * 1.20f));
        long long md    = mdMin + (long long)((double)(mdMax - mdMin) * attract);
        if (!isHome) md = (long long)(md * 0.30);
        return md;
      };

      // Home fixtures
      {
        std::stringstream mq;
        mq << "SELECT id, fixture_date, away_team_id FROM fixtures"
           << " WHERE manager_id=" << mgrId
           << " AND season_year=" << seasonYear
           << " AND home_team_id=" << cId
           << " AND status='played'"
           << " AND id NOT IN (" << alreadyPaid << ");";
        DatabaseResult *mr = GetDB()->Query(mq.str());
        for (unsigned int i = 0; i < mr->data.size(); i++) {
          int fxId       = atoi(DBCell(mr, i, 0).c_str());
          std::string fxDate = DBCell(mr, i, 1);
          int oppId      = atoi(DBCell(mr, i, 2).c_str());
          long long md   = CalcMatchdayAmt(oppId, true);
          std::stringstream tx2;
          tx2 << "INSERT INTO finance_transactions (manager_id, club_id, date, category, description, amount)"
              << " VALUES (" << mgrId << "," << cId << ",'" << fxDate << "',"
              << "'matchday','H:" << fxId << "'," << md << ");";
          DatabaseResult *tr2 = GetDB()->Query(tx2.str()); delete tr2;
          std::stringstream bq2;
          bq2 << "UPDATE club_finances SET cash_balance = cash_balance + " << md
              << " WHERE manager_id=" << mgrId << " AND club_id=" << cId << ";";
          DatabaseResult *br2 = GetDB()->Query(bq2.str()); delete br2;
          printf("[FINANCE] Matchday home fixture=%d opp=%d date=%s amount=%lld\n", fxId, oppId, fxDate.c_str(), md);
        }
        delete mr;
      }

      // Away fixtures: 30% of attractiveness-based rate
      {
        std::stringstream mq;
        mq << "SELECT id, fixture_date, home_team_id FROM fixtures"
           << " WHERE manager_id=" << mgrId
           << " AND season_year=" << seasonYear
           << " AND away_team_id=" << cId
           << " AND status='played'"
           << " AND id NOT IN (" << alreadyPaid << ");";
        DatabaseResult *mr = GetDB()->Query(mq.str());
        for (unsigned int i = 0; i < mr->data.size(); i++) {
          int fxId       = atoi(DBCell(mr, i, 0).c_str());
          std::string fxDate = DBCell(mr, i, 1);
          int oppId      = atoi(DBCell(mr, i, 2).c_str());
          long long md   = CalcMatchdayAmt(oppId, false);
          std::stringstream tx2;
          tx2 << "INSERT INTO finance_transactions (manager_id, club_id, date, category, description, amount)"
              << " VALUES (" << mgrId << "," << cId << ",'" << fxDate << "',"
              << "'matchday','A:" << fxId << "'," << md << ");";
          DatabaseResult *tr2 = GetDB()->Query(tx2.str()); delete tr2;
          std::stringstream bq2;
          bq2 << "UPDATE club_finances SET cash_balance = cash_balance + " << md
              << " WHERE manager_id=" << mgrId << " AND club_id=" << cId << ";";
          DatabaseResult *br2 = GetDB()->Query(bq2.str()); delete br2;
          printf("[FINANCE] Matchday away fixture=%d opp=%d date=%s amount=%lld\n", fxId, oppId, fxDate.c_str(), md);
        }
        delete mr;
      }
    }

    // Season-end prize money for player's club (pay once per season_year)
    if (hasSeasonEnded && seasonYear > 0) {
      std::stringstream ppq;
      ppq << "SELECT season_prize_paid FROM club_finances"
          << " WHERE manager_id=" << mgrId << " AND club_id=" << cId << ";";
      DatabaseResult *ppr = GetDB()->Query(ppq.str());
      int prizePaid = (ppr->data.size() > 0 && !DBCell(ppr, 0, 0).empty())
                      ? atoi(DBCell(ppr, 0, 0).c_str()) : 0;
      delete ppr;

      if (prizePaid != seasonYear) {
        // Determine user club's finishing position in their league
        std::stringstream posq;
        posq << "SELECT team_id FROM standings"
             << " WHERE manager_id=" << mgrId << " AND league_id=" << club.leagueId
             << " ORDER BY points DESC, goal_difference DESC, goals_for DESC;";
        DatabaseResult *posr = GetDB()->Query(posq.str());
        int position = 1;
        for (unsigned int i = 0; i < posr->data.size(); i++) {
          if (atoi(DBCell(posr, i, 0).c_str()) == cId) { position = (int)i + 1; break; }
        }
        delete posr;

        int prizeIdx = (position - 1);
        if (prizeIdx < 0) prizeIdx = 0;
        if (prizeIdx > 5) prizeIdx = 5;
        long long prizeAmt = lfp->prize[prizeIdx];
        char prizeDesc[64];
        snprintf(prizeDesc, sizeof(prizeDesc), "Season %d prize - %d%s place",
                 seasonYear, position,
                 position == 1 ? "st" : position == 2 ? "nd" : position == 3 ? "rd" : "th");
        InsertTx(currentDate, "prize", prizeDesc, prizeAmt);

        std::stringstream ppu;
        ppu << "UPDATE club_finances SET season_prize_paid=" << seasonYear
            << " WHERE manager_id=" << mgrId << " AND club_id=" << cId << ";";
        DatabaseResult *ppud = GetDB()->Query(ppu.str());
        delete ppud;
        printf("[FINANCE] Prize paid position=%d amount=%lld season=%d\n",
               position, prizeAmt, seasonYear);
      }

      // Annual budget cycle for ALL clubs (once per season)
      {
        std::stringstream acq;
        acq << "SELECT cf.club_id, cf.season_budget_processed,"
            << " cf.cash_balance, cf.wage_budget, cf.transfer_budget, cf.board_confidence,"
            << " cf.commercial_strength, cf.institutional_power, cf.style_seed,"
            << " cf.debt_level, cf.shock_cooldown, cf.commercial_shock_mult,"
            << " cf.bad_contract_weeks, cf.transfer_budget_frozen, cf.emergency_credit_used,"
            << " cf.wage_overrun_weeks,"
            << " t.league_id, t.domestic_prestige, t.international_prestige,"
            << " t.transfer_budget as team_tbud"
            << " FROM club_finances cf JOIN teams t ON cf.club_id=t.id"
            << " WHERE cf.manager_id=" << mgrId << ";";
        DatabaseResult *acr = GetDB()->Query(acq.str());

        // Need max transfer_budget once for club_factor calculation
        DatabaseResult *mxr = GetDB()->Query(
          "SELECT MAX(transfer_budget) FROM teams WHERE transfer_budget > 0;");
        long long maxB = 1LL;
        if (mxr->data.size() > 0 && !DBCell(mxr, 0, 0).empty())
          maxB = std::max(1LL, atoll(DBCell(mxr, 0, 0).c_str()));
        delete mxr;

        for (unsigned int i = 0; i < acr->data.size(); i++) {
          int acClubId   = atoi(DBCell(acr, i,  0).c_str());
          int seasonDone = atoi(DBCell(acr, i,  1).c_str());
          if (seasonDone == seasonYear) continue;

          ClubFinances cf;
          cf.club_id               = acClubId;
          cf.cash_balance          = atoll(DBCell(acr, i, 2).c_str());
          cf.wage_budget           = atoll(DBCell(acr, i, 3).c_str());
          cf.transfer_budget       = atoll(DBCell(acr, i, 4).c_str());
          cf.board_confidence      = atoi(DBCell(acr, i,  5).c_str());
          cf.commercial_strength   = (float)atof(DBCell(acr, i, 6).c_str());
          cf.institutional_power   = (float)atof(DBCell(acr, i, 7).c_str());
          cf.style_seed            = atoi(DBCell(acr, i,  8).c_str());
          cf.debt_level            = atoll(DBCell(acr, i, 9).c_str());
          cf.shock_cooldown        = atoi(DBCell(acr, i, 10).c_str());
          cf.commercial_shock_mult = (float)atof(DBCell(acr, i, 11).c_str());
          cf.bad_contract_weeks    = atoi(DBCell(acr, i, 12).c_str());
          cf.transfer_budget_frozen = atoi(DBCell(acr, i, 13).c_str());
          cf.emergency_credit_used  = atoi(DBCell(acr, i, 14).c_str());
          cf.wage_overrun_weeks    = atoi(DBCell(acr, i, 15).c_str());
          int acLeagueId  = atoi(DBCell(acr, i, 16).c_str());
          int acDomPres   = atoi(DBCell(acr, i, 17).c_str());
          int acIntlPres  = atoi(DBCell(acr, i, 18).c_str());
          long long acTbud = atoll(DBCell(acr, i, 19).c_str());

          const LeagueFP *acLfp = GetLeagueFP(acLeagueId);
          float bFactor = maxB > 0 ? (float)acTbud / (float)maxB : 0.0f;
          float pFactor = (acIntlPres * 0.6f + acDomPres * 0.4f) / 10.0f;
          float clubFac = bFactor * 0.70f + pFactor * 0.30f;

          ProcessAnnualCycle(mgrId, acClubId, cf, acLfp, seasonYear,
                             acLeagueId, clubFac, acDomPres, acIntlPres);

          // Sponsor renewals — only for the player's club
          if (acClubId == cId) {
            int pos = 6, nClubs = 10;
            {
              std::stringstream spq;
              spq << "SELECT team_id FROM standings"
                  << " WHERE manager_id=" << mgrId << " AND league_id=" << acLeagueId
                  << " ORDER BY points DESC, goal_difference DESC, goals_for DESC;";
              DatabaseResult *spr = GetDB()->Query(spq.str());
              nClubs = (int)spr->data.size();
              if (nClubs < 1) nClubs = 1;
              for (unsigned int k = 0; k < spr->data.size(); k++) {
                if (atoi(DBCell(spr, k, 0).c_str()) == cId) { pos = (int)k + 1; break; }
              }
              delete spr;
            }
            int expectedPos = (int)((1.0f - clubFac) * (float)nClubs) + 1;
            ProcessSponsorRenewals(mgrId, cId, seasonYear, pos, nClubs, expectedPos,
                                   manager.name, club.name);
          }
        }
        delete acr;
      }
    }
  }

  // Load finance state into CareerHubState
  {
    std::stringstream bq;
    bq << "SELECT cash_balance, wage_budget, transfer_budget, board_confidence, debt_level"
       << " FROM club_finances WHERE manager_id=" << mgrId << " AND club_id=" << cId << ";";
    DatabaseResult *br = GetDB()->Query(bq.str());
    if (br->data.size() > 0) {
      finances.balance         = atoll(DBCell(br, 0, 0).c_str());
      finances.wageBudget      = atoll(DBCell(br, 0, 1).c_str());
      finances.transferBudget  = atoll(DBCell(br, 0, 2).c_str());
      finances.boardConfidence = atoi(DBCell(br, 0, 3).c_str());
      finances.debtLevel       = atoll(DBCell(br, 0, 4).c_str());
    }
    delete br;

    float qualityForDisplay  = CalcClubQualityFactor(players);
    finances.weeklyTV        = (long long)(lfp->weeklyTV * (0.85f + qualityForDisplay * 0.30f));
    finances.weeklyWages     = adjustedWageBill;
    finances.weeklyOperating = lfp->weeklyOperating;
    finances.matchdayMin     = (long long)(lfp->matchdayHome * 0.25f);
    finances.matchdayMax     = (long long)(lfp->matchdayHome * (0.80f + qualityForDisplay * 1.20f));
    finances.seasonPrize1st  = lfp->prize[0];
    finances.seasonPrize2nd  = lfp->prize[1];

    finances.recent.clear();
    std::stringstream rq;
    rq << "SELECT date, category, description, amount"
       << " FROM finance_transactions WHERE manager_id=" << mgrId
       << " AND club_id=" << cId
       << " ORDER BY id DESC LIMIT 40;";
    DatabaseResult *rr = GetDB()->Query(rq.str());
    for (unsigned int i = 0; i < rr->data.size(); i++) {
      FinanceTransaction ft;
      ft.date        = DBCell(rr, i, 0);
      ft.category    = DBCell(rr, i, 1);
      ft.description = DBCell(rr, i, 2);
      std::string am = DBCell(rr, i, 3);
      ft.amount      = am.empty() ? 0LL : atoll(am.c_str());
      finances.recent.push_back(ft);
    }
    delete rr;

    // Weekly sponsor income total for display
    finances.weeklySponsors = 0;
    if (seasonYear > 0) {
      std::stringstream spq;
      spq << "SELECT SUM(weekly_value) FROM club_sponsors"
          << " WHERE manager_id=" << mgrId << " AND club_id=" << cId
          << " AND season_year=" << seasonYear << ";";
      DatabaseResult *spr = GetDB()->Query(spq.str());
      if (spr->data.size() > 0 && !DBCell(spr, 0, 0).empty())
        finances.weeklySponsors = atoll(DBCell(spr, 0, 0).c_str());
      delete spr;
    }
  }

  // Load active sponsor contracts
  activeSponsors.clear();
  if (seasonYear > 0) {
    std::stringstream q;
    q << "SELECT id, sponsor_id, sponsor_name, weekly_value, season_year"
      << " FROM club_sponsors WHERE manager_id=" << mgrId << " AND club_id=" << cId
      << " AND season_year=" << seasonYear << " ORDER BY id ASC;";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r) {
      for (unsigned int i = 0; i < r->data.size(); i++) {
        SponsorContract sc;
        sc.id          = atoi(DBCell(r, i, 0).c_str());
        sc.sponsorId   = atoi(DBCell(r, i, 1).c_str());
        sc.sponsorName = DBCell(r, i, 2);
        sc.weeklyValue = atoll(DBCell(r, i, 3).c_str());
        sc.seasonYear  = atoi(DBCell(r, i, 4).c_str());
        activeSponsors.push_back(sc);
      }
      delete r;
    }
  }

  // Load pending sponsor offers
  pendingOffers.clear();
  {
    std::stringstream q;
    q << "SELECT id, sponsor_id, sponsor_name, weekly_value"
      << " FROM pending_sponsor_offers WHERE manager_id=" << mgrId
      << " AND status='pending' ORDER BY id ASC;";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r) {
      for (unsigned int i = 0; i < r->data.size(); i++) {
        SponsorOffer so;
        so.id          = atoi(DBCell(r, i, 0).c_str());
        so.sponsorId   = atoi(DBCell(r, i, 1).c_str());
        so.sponsorName = DBCell(r, i, 2);
        so.weeklyValue = atoll(DBCell(r, i, 3).c_str());
        pendingOffers.push_back(so);
      }
      delete r;
    }
  }

  // Load scout queue (in-progress scouting) — join players + teams for richer display
  scoutQueue.clear();
  {
    std::stringstream q;
    q << "SELECT sq.player_id, sq.firstname, sq.lastname, COALESCE(t.name,sq.club_name), sq.due_date, sq.scout_rating,"
      << " p.role, p.age, t.logo_url, t.shortname"
      << " FROM scout_queue sq"
      << " LEFT JOIN players p ON p.id = sq.player_id"
      << " LEFT JOIN player_save_state pss ON pss.manager_id=" << mgrId << " AND pss.player_id=p.id"
      << " LEFT JOIN teams   t ON t.id = COALESCE(pss.team_id,p.team_id)"
      << " WHERE sq.manager_id=" << mgrId
      << " ORDER BY sq.due_date ASC;";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r) {
      for (unsigned int i = 0; i < r->data.size(); i++) {
        auto &row = r->data[i];
        ScoutQueueEntry e;
        e.playerId      = atoi(row[0].c_str());
        e.firstName     = row.size() > 1 ? row[1] : "";
        e.lastName      = row.size() > 2 ? row[2] : "";
        e.clubName      = row.size() > 3 ? row[3] : "";
        e.dueDate       = row.size() > 4 ? row[4] : "";
        e.scoutRating   = row.size() > 5 ? atoi(row[5].c_str()) : 1;
        e.role          = row.size() > 6 ? row[6] : "";
        e.age           = row.size() > 7 ? row[7] : "";
        e.clubLogoPath  = row.size() > 8 ? row[8] : "";
        e.clubShortName = row.size() > 9 ? row[9] : e.clubName;
        scoutQueue.push_back(e);
      }
      delete r;
    }
  }

  // Load completed scout reports — join players + teams for richer display
  scoutReports.clear();
  {
    std::stringstream q;
    q << "SELECT sr.player_id, sr.firstname, sr.lastname, COALESCE(t.name,sr.club_name), sr.reveal_pct,"
      << " p.role, p.age, t.logo_url, t.shortname"
      << " FROM scout_reports sr"
      << " LEFT JOIN players p ON p.id = sr.player_id"
      << " LEFT JOIN player_save_state pss ON pss.manager_id=" << mgrId << " AND pss.player_id=p.id"
      << " LEFT JOIN teams   t ON t.id = COALESCE(pss.team_id,p.team_id)"
      << " WHERE sr.manager_id=" << mgrId
      << " AND COALESCE(pss.team_id,p.team_id) != " << cId
      << " ORDER BY sr.id DESC;";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r) {
      for (unsigned int i = 0; i < r->data.size(); i++) {
        auto &row = r->data[i];
        ScoutReport sr;
        sr.playerId      = atoi(row[0].c_str());
        sr.firstName     = row.size() > 1 ? row[1] : "";
        sr.lastName      = row.size() > 2 ? row[2] : "";
        sr.clubName      = row.size() > 3 ? row[3] : "";
        sr.revealPct     = row.size() > 4 ? (float)atof(row[4].c_str()) : 0.0f;
        sr.role          = row.size() > 5 ? row[5] : "";
        sr.age           = row.size() > 6 ? row[6] : "";
        sr.clubLogoPath  = row.size() > 7 ? row[7] : "";
        sr.clubShortName = row.size() > 8 ? row[8] : sr.clubName;
        scoutReports.push_back(sr);
      }
      delete r;
    }
  }

  // Load manager inbox (newest first, up to 200 messages)
  inbox.clear();
  {
    std::stringstream q;
    q << "SELECT id,template_id,sender_type,sender_name,subject,body,category,"
      << "game_date,is_read,is_starred,has_task,task_done"
      << " FROM manager_inbox WHERE manager_id=" << mgrId
      << " ORDER BY id DESC LIMIT 200;";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r) {
      for (unsigned int i = 0; i < r->data.size(); i++) {
        InboxMessage msg;
        msg.id         = atoi(DBCell(r, i,  0).c_str());
        msg.templateId = atoi(DBCell(r, i,  1).c_str());
        msg.senderType = DBCell(r, i,  2);
        msg.senderName = DBCell(r, i,  3);
        msg.subject    = DBCell(r, i,  4);
        msg.body       = DBCell(r, i,  5);
        msg.category   = DBCell(r, i,  6);
        msg.gameDate   = DBCell(r, i,  7);
        msg.isRead     = DBCell(r, i,  8) == "1";
        msg.isStarred  = DBCell(r, i,  9) == "1";
        msg.hasTask    = DBCell(r, i, 10) == "1";
        msg.taskDone   = DBCell(r, i, 11) == "1";
        inbox.push_back(msg);
      }
      delete r;
    }
  }

  active = true;
}

// ---- Finance function implementations -----------------------------------

static void InitAllClubFinances(int managerId) {
  // Find the largest transfer_budget across all teams (normalisation denominator)
  DatabaseResult *mr = GetDB()->Query(
    "SELECT MAX(transfer_budget) FROM teams WHERE transfer_budget > 0;");
  long long maxBudget = 1LL;
  if (mr->data.size() > 0 && !DBCell(mr, 0, 0).empty())
    maxBudget = std::max(1LL, atoll(DBCell(mr, 0, 0).c_str()));
  delete mr;

  // Get manager creation time as the career-stable seed base
  std::stringstream tsq;
  tsq << "SELECT strftime('%s', created_at) FROM managers WHERE id=" << managerId << ";";
  DatabaseResult *tsr = GetDB()->Query(tsq.str());
  unsigned int careerSeed = 12345u;
  if (tsr->data.size() > 0 && !DBCell(tsr, 0, 0).empty())
    careerSeed = (unsigned int)atoll(DBCell(tsr, 0, 0).c_str());
  delete tsr;

  // Load all clubs with financial inputs
  DatabaseResult *tr = GetDB()->Query(
    "SELECT id, league_id, transfer_budget, international_prestige, domestic_prestige"
    " FROM teams WHERE transfer_budget > 0;");

  for (unsigned int i = 0; i < tr->data.size(); i++) {
    int   clubId   = atoi(DBCell(tr, i, 0).c_str());
    int   leagueId = atoi(DBCell(tr, i, 1).c_str());
    long long tbud = atoll(DBCell(tr, i, 2).c_str());
    int   intlPres = atoi(DBCell(tr, i, 3).c_str());
    int   domPres  = atoi(DBCell(tr, i, 4).c_str());
    const LeagueFP *lfp = GetLeagueFP(leagueId);

    float budget_factor   = (float)tbud / (float)maxBudget;
    float prestige_factor = (intlPres * 0.6f + domPres * 0.4f) / 10.0f;
    float club_factor     = budget_factor * 0.70f + prestige_factor * 0.30f;

    int style_seed = (int)((unsigned int)(clubId * 31337u) ^ careerSeed);
    int archetype  = ((unsigned int)style_seed) % 6;

    // commercial_strength — seeded at career start, evolves slowly each season via ProcessAnnualCycle
    float comm_var = ((float)(((unsigned int)style_seed >> 8) % 30) / 100.0f) - 0.15f;
    float commercial_strength = std::max(0.05f, std::min(1.0f, club_factor * 0.70f + comm_var));

    // institutional_power (permanent)
    float prestige_score    = (intlPres * 0.5f + domPres * 0.3f) / 8.0f;
    float institutional_power = std::max(0.05f, std::min(1.0f,
                                           prestige_score * 0.6f + commercial_strength * 0.4f));

    // cash_balance
    long long range    = lfp->maxBalance - lfp->minBalance;
    long long baseCash = lfp->minBalance + (long long)((double)range * club_factor);
    float cash_mults[] = { 1.25f, 0.85f, 0.65f, 1.00f, 1.10f, 0.75f };
    float var_pct = (float)((style_seed >> 4) % 21 - 10) / 100.0f;
    long long cash_balance = (long long)(baseCash * cash_mults[archetype] * (1.0f + var_pct));

    // wage_budget: anchored to actual squad wage bill + a prestige-scaled headroom.
    // This keeps the budget grounded in reality — the board covers what you already
    // have, plus a small margin for 1-3 signings depending on club size.
    long long squadWages = 0;
    {
      std::stringstream wq;
      wq << "SELECT SUM(weekly_wage) FROM players WHERE team_id=" << clubId << ";";
      DatabaseResult *wr = GetDB()->Query(wq.str());
      if (wr && wr->data.size() > 0 && !DBCell(wr, 0, 0).empty())
        squadWages = atoll(DBCell(wr, 0, 0).c_str());
      delete wr;
    }
    long long wage_budget;
    if (squadWages > 0) {
      // headroom: 5% (small/poor clubs) to 20% (elite clubs)
      float headroom = 1.05f + prestige_factor * 0.15f;
      wage_budget = (long long)((float)squadWages * headroom);
    } else {
      // fallback for clubs with no players in DB
      wage_budget = (long long)((float)tbud * 0.008f); // ~0.8% of transfer budget per week
    }

    // board_confidence
    int prestige_bonus = (int)(prestige_factor * 15.0f);
    int conf_var       = (style_seed % 11) - 5;
    int board_confidence = std::max(35, std::min(75, 50 + prestige_bonus + conf_var));

    // Insert (OR IGNORE — existing saves keep their already-migrated row for manager's club)
    std::stringstream ins;
    ins << "INSERT OR IGNORE INTO club_finances"
        << " (manager_id, club_id, cash_balance, wage_budget, transfer_budget,"
        << "  board_confidence, commercial_strength, institutional_power, style_seed)"
        << " VALUES ("
        << managerId  << "," << clubId        << "," << cash_balance  << ","
        << wage_budget << "," << tbud          << "," << board_confidence << ","
        << commercial_strength << "," << institutional_power << "," << style_seed << ");";
    DatabaseResult *ir = GetDB()->Query(ins.str());
    delete ir;

    printf("[FINANCE INIT] club=%d factor=%.2f archetype=%d cash=%lld wage_bud=%lld tbud=%lld conf=%d\n",
           clubId, club_factor, archetype, cash_balance, wage_budget, tbud, board_confidence);
  }
  delete tr;
}

static void TriggerDebtCrisis(int managerId, int clubId, ClubFinances &cf,
                               const LeagueFP *lfp) {
  cf.transfer_budget_frozen = 1;

  // Board confidence penalty, softened for large clubs
  int penalty = (int)(25.0f * (1.0f - cf.institutional_power * 0.40f));
  cf.board_confidence = std::max(0, cf.board_confidence - penalty);

  if (!cf.emergency_credit_used) {
    // One-time emergency board loan — size scales with institutional_power
    long long credit = (long long)(lfp->minBalance
                     * (0.5f + cf.institutional_power * 1.0f));
    cf.cash_balance        += credit;
    cf.debt_level          += credit;
    cf.emergency_credit_used = 1;
    cf.shock_cooldown        = 2;
    printf("[FINANCE CRISIS] club=%d emergency_credit=%lld conf=%d\n",
           clubId, credit, cf.board_confidence);
  } else {
    // Second crisis: no safety net — pin debt at ceiling, drain cash weekly
    printf("[FINANCE CRISIS] club=%d no_safety_net conf=%d\n",
           clubId, cf.board_confidence);
  }

  // Write crisis state back immediately
  std::stringstream uq;
  uq << "UPDATE club_finances SET"
     << "  transfer_budget_frozen=" << cf.transfer_budget_frozen << ","
     << "  board_confidence="       << cf.board_confidence       << ","
     << "  emergency_credit_used="  << cf.emergency_credit_used  << ","
     << "  cash_balance="           << cf.cash_balance           << ","
     << "  debt_level="             << cf.debt_level             << ","
     << "  shock_cooldown="         << cf.shock_cooldown
     << " WHERE manager_id=" << managerId << " AND club_id=" << clubId << ";";
  DatabaseResult *ur = GetDB()->Query(uq.str());
  delete ur;
}

static bool TryFireShock(int managerId, int clubId, ClubFinances &cf,
                          const LeagueFP *lfp, int seasonYear,
                          long long estAnnualIncome, int domPrestige, int intlPrestige) {
  if (cf.shock_cooldown > 0) return false;

  FinancialStyle style = DeriveStyle(cf.style_seed);
  unsigned int rseed   = (unsigned int)(cf.style_seed) ^ (unsigned int)(seasonYear * 1031);
  float roll           = SeededRand(rseed, 5);
  float base_prob      = 0.15f;

  if (roll >= base_prob * style.shock_vulnerability) return false;

  // Bad vs good
  float bvg       = SeededRand(rseed, 6);
  float bad_thr   = 0.62f / style.shock_opportunity;
  bool  is_bad    = (bvg < bad_thr);
  bool  did_shock = false;

  if (is_bad) {
    int archetype = ((unsigned int)cf.style_seed) % 6;
    int bad_id    = (int)(SeededRand(rseed, 7) * 5.99f); // 0-5

    // Only bad contracts fire for aggressive/gambling/star archetypes
    if (bad_id == 1 && archetype != 1 && archetype != 2 && archetype != 5)
      bad_id = 0;
    // Financial investigation only for gambling archetype
    if (bad_id == 5 && archetype != 2)
      bad_id = 3;

    switch (bad_id) {
      case 0: { // Sponsorship collapse
        float sev = 0.35f + SeededRand(rseed, 8) * 0.20f;
        sev *= (1.0f - cf.institutional_power * 0.35f);
        cf.commercial_shock_mult = 1.0f - sev;
        cf.shock_cooldown = 2;
        printf("[SHOCK BAD] club=%d sponsorship_collapse mult=%.2f\n", clubId, cf.commercial_shock_mult);
        did_shock = true; break;
      }
      case 1: { // Bad contract
        long long contract_cost = (long long)(estAnnualIncome
                                * (0.15f + SeededRand(rseed, 8) * 0.10f));
        contract_cost = (long long)(contract_cost * (1.0f - cf.institutional_power * 0.25f));
        cf.debt_level         += contract_cost;
        cf.bad_contract_weeks  = 104;
        cf.shock_cooldown      = 3;
        printf("[SHOCK BAD] club=%d bad_contract cost=%lld\n", clubId, contract_cost);
        did_shock = true; break;
      }
      case 2: { // Ownership crisis
        if (cf.board_confidence < 45) {
          int pen = (int)(20.0f * (1.0f - cf.institutional_power * 0.40f));
          cf.board_confidence = std::max(0, cf.board_confidence - pen);
          cf.transfer_budget /= 2;
          cf.shock_cooldown   = 2;
          printf("[SHOCK BAD] club=%d ownership_crisis conf=%d tbud=%lld\n",
                 clubId, cf.board_confidence, cf.transfer_budget);
          did_shock = true;
        }
        break;
      }
      case 3: { // Stadium emergency (small clubs only)
        if (domPrestige < 5) {
          float drain = 0.08f + SeededRand(rseed, 8) * 0.07f;
          drain *= (1.0f - cf.institutional_power * 0.35f);
          long long loss = (long long)(cf.cash_balance * drain);
          cf.cash_balance -= loss;
          cf.shock_cooldown = 2;
          printf("[SHOCK BAD] club=%d stadium_emergency loss=%lld\n", clubId, loss);
          did_shock = true;
        }
        break;
      }
      case 4: { // Wage revolt
        if (cf.wage_overrun_weeks > 12) {
          int pen = (int)(25.0f * (1.0f - cf.institutional_power * 0.40f));
          cf.board_confidence = std::max(0, cf.board_confidence - pen);
          cf.shock_cooldown   = 2;
          printf("[SHOCK BAD] club=%d wage_revolt conf=%d\n", clubId, cf.board_confidence);
          did_shock = true;
        }
        break;
      }
      case 5: { // Financial investigation (gambling only)
        cf.transfer_budget_frozen = 1;
        long long fine = (long long)(cf.cash_balance * 0.08f);
        cf.debt_level   += fine;
        cf.cash_balance -= fine;
        cf.shock_cooldown = 3;
        printf("[SHOCK BAD] club=%d financial_investigation fine=%lld\n", clubId, fine);
        did_shock = true; break;
      }
    }
  } else {
    // Good shocks
    int good_id = (int)(SeededRand(rseed, 7) * 4.99f); // 0-4

    switch (good_id) {
      case 0: { // Sponsorship windfall
        if (cf.commercial_strength > 0.5f) {
          cf.commercial_shock_mult = 1.30f + SeededRand(rseed, 8) * 0.20f;
          cf.shock_cooldown = 2;
          printf("[SHOCK GOOD] club=%d sponsorship_windfall mult=%.2f\n",
                 clubId, cf.commercial_shock_mult);
          did_shock = true;
        }
        break;
      }
      case 1: { // Ownership injection
        if (cf.board_confidence > 65) {
          long long inj = (long long)(cf.transfer_budget
                        * (0.15f + SeededRand(rseed, 8) * 0.25f));
          cf.cash_balance  += inj;
          cf.shock_cooldown = 3;
          printf("[SHOCK GOOD] club=%d ownership_injection inj=%lld\n", clubId, inj);
          did_shock = true;
        }
        break;
      }
      case 2: { // Naming rights
        if (domPrestige >= 7) {
          cf.cash_balance  += lfp->prize[2];
          cf.shock_cooldown = 4;
          printf("[SHOCK GOOD] club=%d naming_rights cash+=%lld\n", clubId, lfp->prize[2]);
          did_shock = true;
        }
        break;
      }
      case 3: { // Youth breakthrough (youth-focused archetype)
        if (((unsigned int)cf.style_seed) % 6 == 3) {
          long long youth_val = (long long)(cf.transfer_budget
                              * (0.05f + SeededRand(rseed, 8) * 0.15f));
          cf.transfer_budget += youth_val;
          cf.shock_cooldown   = 2;
          printf("[SHOCK GOOD] club=%d youth_breakthrough tbud+=%lld\n", clubId, youth_val);
          did_shock = true;
        }
        break;
      }
      case 4: { // European windfall
        if (intlPrestige >= 7) {
          cf.cash_balance  += lfp->prize[3];
          cf.shock_cooldown = 2;
          printf("[SHOCK GOOD] club=%d european_windfall cash+=%lld\n", clubId, lfp->prize[3]);
          did_shock = true;
        }
        break;
      }
    }
  }

  return did_shock;
}

static void ProcessAnnualCycle(int managerId, int clubId, ClubFinances &cf,
                                const LeagueFP *lfp, int seasonYear,
                                int leagueId, float clubFactor,
                                int domPrestige, int intlPrestige) {
  FinancialStyle style  = DeriveStyle(cf.style_seed);
  int archetype         = ((unsigned int)cf.style_seed) % 6;
  unsigned int rng_seed = (unsigned int)cf.style_seed ^ (unsigned int)seasonYear;

  // --- Estimated annual income (used for debt ceiling and wage_budget)
  long long est_annual = (long long)(lfp->weeklyTV * 52.0f * (0.85f + clubFactor * 0.30f))
                       + (long long)(lfp->weeklyTV * 12.0f * cf.commercial_strength);

  // --- Hard debt ceiling check
  long long max_debt = (long long)(est_annual * 2.5f);
  if (cf.debt_level > max_debt) {
    cf.debt_level = max_debt;
    TriggerDebtCrisis(managerId, clubId, cf, lfp);
  }

  // --- Season prize money (lookup actual position from standings)
  long long prize_money = 0LL;
  int total_clubs = 10;
  int position    = 6; // default mid-table if no standings found
  {
    std::stringstream pq;
    pq << "SELECT (SELECT COUNT(*)+1 FROM standings s2"
       << " WHERE s2.manager_id=" << managerId
       << " AND s2.league_id=" << leagueId
       << " AND s2.season_year=" << seasonYear
       << " AND s2.points > s1.points)"
       << " FROM standings s1"
       << " WHERE s1.manager_id=" << managerId
       << " AND s1.league_id=" << leagueId
       << " AND s1.team_id=" << clubId
       << " AND s1.season_year=" << seasonYear << ";";
    DatabaseResult *pr = GetDB()->Query(pq.str());
    if (pr->data.size() > 0 && !DBCell(pr, 0, 0).empty())
      position = atoi(DBCell(pr, 0, 0).c_str());
    delete pr;
    int prizeIdx = std::max(0, std::min(5, position - 1));
    prize_money  = lfp->prize[prizeIdx];
    cf.cash_balance += prize_money;

    std::stringstream cq;
    cq << "SELECT COUNT(*) FROM standings WHERE manager_id=" << managerId
       << " AND league_id=" << leagueId << " AND season_year=" << seasonYear << ";";
    DatabaseResult *cr = GetDB()->Query(cq.str());
    if (cr->data.size() > 0 && !DBCell(cr, 0, 0).empty())
      total_clubs = atoi(DBCell(cr, 0, 0).c_str());
    delete cr;

    // Board confidence: performance component
    int expected_pos  = (int)((1.0f - clubFactor) * (float)total_clubs) + 1;
    int pos_delta     = expected_pos - position;

    int perf_delta;
    if      (pos_delta >=  3) perf_delta = +12;
    else if (pos_delta >=  1) perf_delta = +6;
    else if (pos_delta ==  0) perf_delta = +2;
    else if (pos_delta >= -2) perf_delta = -8;
    else                      perf_delta = -18;

    if (perf_delta < 0)
      perf_delta = (int)(perf_delta * (1.0f - cf.institutional_power * 0.40f));

    cf.board_confidence = std::max(0, std::min(100, cf.board_confidence + perf_delta));
  }

  // --- commercial_strength annual evolution (slow brand memory drift)
  {
    // position_score: 1.0 = finished 1st, 0.0 = finished last
    float position_score = (total_clubs <= 1)
        ? 0.5f
        : 1.0f - ((float)(position - 1) / (float)(total_clubs - 1));

    // trophies/UCL not tracked yet — using 0 for both terms, reducing to position only
    float performance_factor = position_score * 0.5f; // range [0, 0.5]

    // drift: positive when finishing top-half, negative when bottom-half
    float drift = (performance_factor - 0.25f) * 0.06f; // ±0.015 typical, ±0.03 max

    // Soft saturation: large clubs near ceiling gain very slowly
    if (drift > 0.0f && cf.commercial_strength > 0.80f)
      drift *= (1.0f - cf.commercial_strength) * 5.0f; // tapers to 0 at 1.0

    // Soft floor protection: small clubs lose brand very slowly at the bottom
    if (drift < 0.0f && cf.commercial_strength < 0.20f)
      drift *= cf.commercial_strength * 5.0f; // tapers to 0 at 0.0

    const float inertia = 0.97f;
    cf.commercial_strength = std::max(0.05f, std::min(1.0f,
        cf.commercial_strength * inertia + drift));
  }

  // --- Annual revenue streams
  float rng01 = SeededRand(rng_seed, 0);
  float comm_var = 1.0f + (rng01 * 2.0f - 1.0f) * style.commercial_variance;
  comm_var *= cf.commercial_shock_mult;
  cf.commercial_shock_mult = 1.0f; // consume for this season

  long long commercial = (long long)(lfp->weeklyTV * 12.0f
                       * cf.commercial_strength * comm_var);

  float conf_factor_r = 0.70f + (cf.board_confidence / 100.0f) * 0.60f;
  float rng02 = SeededRand(rng_seed, 1);
  long long sponsorship = (long long)(lfp->weeklyTV * 8.0f
                        * cf.commercial_strength * conf_factor_r
                        * (0.90f + rng02 * 0.20f));

  float merch_base = (domPrestige / 10.0f) * 0.8f + 0.2f;
  float rng03 = SeededRand(rng_seed, 2);
  long long merchandise = (long long)(lfp->weeklyTV * 4.0f
                         * merch_base * (0.85f + rng03 * 0.30f));

  long long player_sales = 0LL;
  float rng04 = SeededRand(rng_seed, 3);
  if (rng04 < style.selling_bias * 0.6f)
    player_sales = (long long)(cf.transfer_budget * (0.20f + rng04 * 0.40f));

  long long annual_income = commercial + sponsorship + merchandise
                          + player_sales + prize_money;
  cf.cash_balance += commercial + sponsorship + merchandise + player_sales;

  // Annual non-weekly expenses
  long long facilities = (long long)(lfp->weeklyOperating * 8.0f);
  long long youth_cost = (long long)(lfp->weeklyOperating * (archetype == 3 ? 6.0f : 2.0f));
  long long season_profit = annual_income - facilities - youth_cost;
  cf.cash_balance -= (facilities + youth_cost);

  // --- Board confidence: financial health component
  int fin_delta = 0;
  if (season_profit > 0)                                     fin_delta += 5;
  if (cf.cash_balance > lfp->minBalance)                     fin_delta += 3;
  if (cf.debt_level > (long long)(est_annual * 0.40f))       fin_delta -= 12;
  if (cf.wage_overrun_weeks > 8)                             fin_delta -= 10;
  if (cf.cash_balance < 0)                                   fin_delta -= 15;
  fin_delta = (int)(fin_delta * (1.0f - cf.institutional_power * 0.20f));

  int decay = -1;
  cf.board_confidence = std::max(0, std::min(100, cf.board_confidence + fin_delta + decay));

  // --- Cash retention
  if (season_profit > 0) {
    long long banked = (long long)(season_profit * style.saving_rate);
    cf.cash_balance += banked;
  }

  // --- Next season transfer_budget
  if (!cf.transfer_budget_frozen) {
    long long rollover    = (long long)(cf.transfer_budget * 0.50f);
    float conf_factor_t   = (cf.board_confidence - 50) / 50.0f;
    long long injection   = (long long)(std::max(0LL, cf.cash_balance)
                          * (0.15f + conf_factor_t * 0.20f));
    long long prize_slice = (long long)(prize_money * 0.25f);
    float style_mult = 0.70f + style.transfer_spend_bias * 0.60f;
    float conf_mult  = 1.00f + conf_factor_t * 0.35f;
    float rng_t      = SeededRand(rng_seed, 4);
    float variance   = 0.90f + rng_t * 0.20f;

    long long raw = (long long)((rollover + injection + prize_slice)
                  * style_mult * conf_mult * variance);
    long long floor_b = lfp->minBalance / 4LL;
    long long ceil_b  = (long long)(lfp->maxBalance * 1.20f);
    cf.transfer_budget = std::max(floor_b, std::min(ceil_b, raw));
  } else {
    cf.transfer_budget        = 0LL;
    cf.transfer_budget_frozen = 0;
  }

  // --- Next season wage_budget: re-anchor to current squad wage bill
  {
    long long squadWages = 0;
    std::stringstream wq;
    wq << "SELECT SUM(weekly_wage) FROM players WHERE team_id=" << clubId << ";";
    DatabaseResult *wr = GetDB()->Query(wq.str());
    if (wr && wr->data.size() > 0 && !DBCell(wr, 0, 0).empty())
      squadWages = atoll(DBCell(wr, 0, 0).c_str());
    delete wr;

    float prestige_factor = (intlPrestige * 0.6f + domPrestige * 0.4f) / 10.0f;
    if (squadWages > 0) {
      float headroom = 1.05f + prestige_factor * 0.15f;
      cf.wage_budget = (long long)((float)squadWages * headroom);
    } else {
      cf.wage_budget = (long long)((float)cf.transfer_budget * 0.008f);
    }
  }

  // --- Try to fire a shock event
  TryFireShock(managerId, clubId, cf, lfp, seasonYear, est_annual, domPrestige, intlPrestige);

  // --- Tick shock cooldown and reset seasonal counters
  if (cf.shock_cooldown > 0) cf.shock_cooldown--;
  cf.wage_overrun_weeks      = 0;
  cf.season_budget_processed = seasonYear;

  printf("[ANNUAL CYCLE] club=%d season=%d tbud=%lld conf=%d cash=%lld debt=%lld\n",
         clubId, seasonYear, cf.transfer_budget, cf.board_confidence,
         cf.cash_balance, cf.debt_level);

  // --- Write back all fields
  std::stringstream uq;
  uq << "UPDATE club_finances SET"
     << "  cash_balance="           << cf.cash_balance           << ","
     << "  wage_budget="            << cf.wage_budget            << ","
     << "  transfer_budget="        << cf.transfer_budget        << ","
     << "  board_confidence="       << cf.board_confidence       << ","
     << "  commercial_strength="    << cf.commercial_strength    << ","
     << "  debt_level="             << cf.debt_level             << ","
     << "  shock_cooldown="         << cf.shock_cooldown         << ","
     << "  commercial_shock_mult="  << cf.commercial_shock_mult  << ","
     << "  bad_contract_weeks="     << cf.bad_contract_weeks     << ","
     << "  transfer_budget_frozen=" << cf.transfer_budget_frozen << ","
     << "  emergency_credit_used="  << cf.emergency_credit_used  << ","
     << "  wage_overrun_weeks=0,"
     << "  season_budget_processed=" << cf.season_budget_processed
     << " WHERE manager_id=" << managerId << " AND club_id=" << clubId << ";";
  DatabaseResult *ur = GetDB()->Query(uq.str());
  delete ur;
}

static void ProcessWeeklyAllClubs(int managerId, const std::string &currentDate,
                                   int playerClubId, const LeagueFP *playerLfp,
                                   long long playerWageBill) {
  // Batch 1: wage bill per team
  std::stringstream wq;
  wq << "SELECT team_id, SUM(weekly_wage) FROM players"
     << " WHERE team_id IN (SELECT club_id FROM club_finances WHERE manager_id="
     << managerId << ") GROUP BY team_id;";
  DatabaseResult *wr = GetDB()->Query(wq.str());
  std::map<int,long long> wagemap;
  for (unsigned int i = 0; i < wr->data.size(); i++) {
    int       tid  = atoi(DBCell(wr, i, 0).c_str());
    long long bill = atoll(DBCell(wr, i, 1).c_str());
    wagemap[tid]   = bill;
  }
  delete wr;

  {
    std::stringstream lq;
    lq << "SELECT ld.loaning_club_id,ld.receiving_club_id,"
       << " COALESCE(pss.weekly_wage,p.weekly_wage),"
       << " ld.monthly_wage_parent_pct,ld.monthly_wage_receiving_pct"
       << " FROM loan_deals ld JOIN players p ON p.id=ld.player_id"
       << " LEFT JOIN player_save_state pss"
       << " ON pss.manager_id=ld.manager_id AND pss.player_id=p.id"
       << " WHERE ld.manager_id=" << managerId
       << " AND ld.status='active';";
    DatabaseResult *lr = GetDB()->Query(lq.str());
    if (lr) {
      for (unsigned int i = 0; i < lr->data.size(); i++) {
        int parent = atoi(DBCell(lr,i,0).c_str());
        int receiving = atoi(DBCell(lr,i,1).c_str());
        long long wage = atoll(DBCell(lr,i,2).c_str());
        int parentPct = atoi(DBCell(lr,i,3).c_str());
        int receivingPct = atoi(DBCell(lr,i,4).c_str());
        wagemap[parent] -= wage;
        wagemap[parent] += (wage * parentPct) / 100;
        wagemap[receiving] += (wage * receivingPct) / 100;
      }
      delete lr;
    }
  }

  // Batch 2: avg base_stat per team (quality approximation for AI clubs)
  std::stringstream qq;
  qq << "SELECT team_id, AVG(base_stat) FROM players"
     << " WHERE team_id IN (SELECT club_id FROM club_finances WHERE manager_id="
     << managerId << ") GROUP BY team_id;";
  DatabaseResult *qr = GetDB()->Query(qq.str());
  std::map<int,float> qualmap;
  for (unsigned int i = 0; i < qr->data.size(); i++) {
    int   tid = atoi(DBCell(qr, i, 0).c_str());
    float avg = DBCell(qr, i, 1).empty() ? 60.0f : (float)atof(DBCell(qr, i, 1).c_str());
    qualmap[tid] = std::max(0.0f, std::min(1.0f, (avg - 50.0f) / 40.0f));
  }
  delete qr;

  // Batch 3: home fixtures played this week (to award matchday income)
  std::stringstream fq;
  fq << "SELECT home_team_id FROM fixtures"
     << " WHERE manager_id=" << managerId
     << " AND status='played'"
     << " AND julianday('" << currentDate << "') - julianday(fixture_date) BETWEEN 0 AND 6;";
  DatabaseResult *fr = GetDB()->Query(fq.str());
  std::set<int> homeThisWeek;
  for (unsigned int i = 0; i < fr->data.size(); i++)
    homeThisWeek.insert(atoi(DBCell(fr, i, 0).c_str()));
  delete fr;

  // Load all club_finances rows for this manager
  std::stringstream cfq;
  cfq << "SELECT club_id, cash_balance, wage_budget, transfer_budget, board_confidence,"
      << " commercial_strength, institutional_power, style_seed, debt_level,"
      << " shock_cooldown, bad_contract_weeks, transfer_budget_frozen,"
      << " emergency_credit_used, wage_overrun_weeks"
      << " FROM club_finances WHERE manager_id=" << managerId << ";";
  DatabaseResult *cfr = GetDB()->Query(cfq.str());

  for (unsigned int i = 0; i < cfr->data.size(); i++) {
    ClubFinances cf;
    cf.club_id               = atoi(DBCell(cfr, i,  0).c_str());
    cf.cash_balance          = atoll(DBCell(cfr, i, 1).c_str());
    cf.wage_budget           = atoll(DBCell(cfr, i, 2).c_str());
    cf.transfer_budget       = atoll(DBCell(cfr, i, 3).c_str());
    cf.board_confidence      = atoi(DBCell(cfr, i,  4).c_str());
    cf.commercial_strength   = (float)atof(DBCell(cfr, i, 5).c_str());
    cf.institutional_power   = (float)atof(DBCell(cfr, i, 6).c_str());
    cf.style_seed            = atoi(DBCell(cfr, i,  7).c_str());
    cf.debt_level            = atoll(DBCell(cfr, i, 8).c_str());
    cf.shock_cooldown        = atoi(DBCell(cfr, i,  9).c_str());
    cf.bad_contract_weeks    = atoi(DBCell(cfr, i, 10).c_str());
    cf.transfer_budget_frozen = atoi(DBCell(cfr, i, 11).c_str());
    cf.emergency_credit_used  = atoi(DBCell(cfr, i, 12).c_str());
    cf.wage_overrun_weeks    = atoi(DBCell(cfr, i, 13).c_str());

    // Get league info for this club
    std::stringstream lq;
    lq << "SELECT league_id FROM teams WHERE id=" << cf.club_id << ";";
    DatabaseResult *lr = GetDB()->Query(lq.str());
    int leagueId = 0;
    if (lr->data.size() > 0) leagueId = atoi(DBCell(lr, 0, 0).c_str());
    delete lr;
    const LeagueFP *lfp = GetLeagueFP(leagueId);

    float qual  = qualmap.count(cf.club_id) ? qualmap[cf.club_id] : 0.35f;
    long long wages = wagemap.count(cf.club_id) ? wagemap[cf.club_id] : 0LL;

    // TV income
    long long tv = (long long)(lfp->weeklyTV
                 * (0.85f + qual * 0.30f)
                 * (0.80f + cf.commercial_strength * 0.40f));

    // Operating
    long long operating = lfp->weeklyOperating;

    // Matchday
    long long matchday = 0LL;
    if (homeThisWeek.count(cf.club_id)) {
      float md_qual = 0.50f + qual * 0.50f;
      matchday = (long long)(lfp->matchdayHome
                * (0.25f + md_qual * 1.00f)
                * (0.70f + cf.commercial_strength * 0.60f));
    }

    long long net = tv - wages - operating + matchday;
    cf.cash_balance += net;

    // Debt servicing
    if (cf.debt_level > 0) {
      long long installment = std::max(1000LL, cf.debt_level / 50LL);
      cf.cash_balance -= installment;
      cf.debt_level   -= installment;
      if (cf.debt_level < 0) cf.debt_level = 0;
    }

    // Insolvency
    if (cf.cash_balance < 0) {
      long long est_annual = (long long)(lfp->weeklyTV * 52.0f * (0.85f + qual * 0.30f));
      long long max_debt   = (long long)(est_annual * 2.5f);
      long long shortfall  = -cf.cash_balance;
      if (cf.debt_level + shortfall > max_debt) {
        cf.debt_level   = max_debt;
        cf.cash_balance = 0;
        TriggerDebtCrisis(managerId, cf.club_id, cf, lfp);
      } else {
        cf.debt_level   += shortfall;
        cf.cash_balance  = 0;
        cf.board_confidence = std::max(0, cf.board_confidence - 3);
      }
    }

    // Wage overrun
    if (wages > cf.wage_budget) {
      cf.wage_overrun_weeks++;
      int overrun_pct = (int)(((float)(wages - cf.wage_budget) / cf.wage_budget) * 100.0f);
      int penalty     = (int)((1 + overrun_pct / 15) * (1.0f - cf.institutional_power * 0.30f));
      cf.board_confidence = std::max(0, cf.board_confidence - penalty);
    }

    // Bad contract countdown
    if (cf.bad_contract_weeks > 0) cf.bad_contract_weeks--;

    // Write back — AI clubs only (player's club handled by InsertTx path)
    if (cf.club_id != playerClubId) {
      std::stringstream uq;
      uq << "UPDATE club_finances SET"
         << "  cash_balance="        << cf.cash_balance        << ","
         << "  board_confidence="    << cf.board_confidence    << ","
         << "  debt_level="          << cf.debt_level          << ","
         << "  bad_contract_weeks="  << cf.bad_contract_weeks  << ","
         << "  wage_overrun_weeks="  << cf.wage_overrun_weeks  << ","
         << "  last_weekly_date='"   << currentDate << "'"
         << " WHERE manager_id=" << managerId << " AND club_id=" << cf.club_id << ";";
      DatabaseResult *ur = GetDB()->Query(uq.str());
      delete ur;
    }
  }
  delete cfr;
}

// ---- Sponsor system -------------------------------------------------------

static void CheckSponsorOffers(int managerId, int clubId,
                                const std::string &currentDate, int seasonYear) {
  // Get club prestige
  int intlPres = 0, domPres = 0;
  {
    std::stringstream q;
    q << "SELECT international_prestige, domestic_prestige FROM teams WHERE id=" << clubId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r->data.size() > 0) {
      intlPres = atoi(DBCell(r, 0, 0).c_str());
      domPres  = atoi(DBCell(r, 0, 1).c_str());
    }
    delete r;
  }

  // Count active sponsors this season
  int sponsorCount = 0;
  {
    std::stringstream q;
    q << "SELECT COUNT(*) FROM club_sponsors WHERE manager_id=" << managerId
      << " AND club_id=" << clubId << " AND season_year=" << seasonYear << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r->data.size() > 0 && !DBCell(r, 0, 0).empty())
      sponsorCount = atoi(DBCell(r, 0, 0).c_str());
    delete r;
  }
  if (sponsorCount >= 4) return;

  // Already has a pending offer?
  {
    std::stringstream q;
    q << "SELECT COUNT(*) FROM pending_sponsor_offers WHERE manager_id=" << managerId
      << " AND status='pending';";
    DatabaseResult *r = GetDB()->Query(q.str());
    bool hasPending = r->data.size() > 0 && !DBCell(r, 0, 0).empty()
                      && atoi(DBCell(r, 0, 0).c_str()) > 0;
    delete r;
    if (hasPending) return;
  }

  // Respect the 3-day grace period after career start
  {
    std::stringstream q;
    q << "SELECT julianday('" << currentDate
      << "') - julianday(created_at) FROM managers WHERE id=" << managerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    double daysOld = 0.0;
    if (r->data.size() > 0 && !DBCell(r, 0, 0).empty())
      daysOld = atof(DBCell(r, 0, 0).c_str());
    delete r;
    if (daysOld < 3.0) return;
  }

  // Determine eligible sponsor rating range from club prestige
  int presTotal = intlPres + domPres;
  int maxRating;
  if      (presTotal >= 15) maxRating = 5;
  else if (presTotal >= 11) maxRating = 4;
  else if (presTotal >=  7) maxRating = 3;
  else if (presTotal >=  4) maxRating = 2;
  else                      maxRating = 1;
  int minRating = std::max(1, maxRating - 1);

  // Build exclusion list (already contracted or blacklisted this season)
  std::string excl = "0";
  {
    std::stringstream eq;
    eq << "SELECT sponsor_id FROM club_sponsors"
       << " WHERE manager_id=" << managerId << " AND club_id=" << clubId
       << " AND season_year=" << seasonYear
       << " UNION SELECT sponsor_id FROM sponsor_blacklist"
       << " WHERE manager_id=" << managerId << " AND blacklisted_season=" << seasonYear;
    DatabaseResult *er = GetDB()->Query(eq.str());
    for (unsigned int i = 0; i < er->data.size(); i++)
      excl += "," + DBCell(er, i, 0);
    delete er;
  }

  // Pick a random eligible sponsor
  std::stringstream sq;
  sq << "SELECT id, name, rangelow, rangehigh FROM sponsors"
     << " WHERE rating BETWEEN " << minRating << " AND " << maxRating
     << " AND id NOT IN (" << excl << ") ORDER BY RANDOM() LIMIT 1;";
  DatabaseResult *sr = GetDB()->Query(sq.str());
  if (!sr || sr->data.size() == 0) { delete sr; return; }

  int         sponsorId   = atoi(DBCell(sr, 0, 0).c_str());
  std::string sponsorName = DBCell(sr, 0, 1);
  long long   rangeLow    = atoll(DBCell(sr, 0, 2).c_str());
  long long   rangeHigh   = atoll(DBCell(sr, 0, 3).c_str());
  delete sr;

  // Generate offer value with prestige-weighted bias toward higher end
  float bias = (presTotal >= 14) ? 0.60f : (presTotal >= 9) ? 0.45f : 0.30f;
  unsigned int rng = (unsigned int)(managerId * 7919u ^ clubId * 31337u) ^ (unsigned int)time(nullptr);
  float t = bias + ((float)(rng % 1000) / 1000.0f) * (1.0f - bias);
  if ((rng >> 16) % 3 == 0) t *= 0.75f; // occasional below-midrange offer
  t = std::max(0.0f, std::min(1.0f, t));
  long long offerValue = rangeLow + (long long)((rangeHigh - rangeLow) * t);
  offerValue = (offerValue / 2500) * 2500; // round to nearest £2500
  if (offerValue < rangeLow)  offerValue = rangeLow;
  if (offerValue > rangeHigh) offerValue = rangeHigh;

  // Insert pending offer
  {
    std::stringstream iq;
    iq << "INSERT INTO pending_sponsor_offers"
       << " (manager_id, sponsor_id, sponsor_name, weekly_value, offered_date)"
       << " VALUES (" << managerId << "," << sponsorId
       << ",'" << SqlEsc(sponsorName) << "'," << offerValue << ",'" << currentDate << "');";
    DatabaseResult *ir = GetDB()->Query(iq.str());
    delete ir;
  }

  // Deliver inbox message using template
  {
    // Look up manager name and club name for placeholder substitution
    std::string mgrName, clubName;
    {
      std::stringstream mq;
      mq << "SELECT m.name, t.name FROM managers m"
         << " JOIN teams t ON t.id = m.club_id"
         << " WHERE m.id = " << managerId << ";";
      DatabaseResult *mr = GetDB()->Query(mq.str());
      if (mr && mr->data.size() > 0) {
        mgrName  = DBCell(mr, 0, 0);
        clubName = DBCell(mr, 0, 1);
      }
      delete mr;
    }
    std::string currency = ClubCurrency(clubId);
    std::string subject, body;
    int tid = FillSponsorTemplate("sponsor_offer", mgrName, clubName,
                                  sponsorName, currency, offerValue, subject, body);
    std::stringstream iq;
    iq << "INSERT INTO manager_inbox"
       << " (manager_id,template_id,sender_type,sender_name,subject,body,category,game_date)"
       << " VALUES (" << managerId << "," << tid << ",'finance','Commercial Director',"
       << "'" << SqlEsc(subject) << "','" << SqlEsc(body) << "','finance','" << currentDate << "');";
    DatabaseResult *ir = GetDB()->Query(iq.str());
    delete ir;
  }

  printf("[SPONSOR] Offer generated: manager=%d sponsor='%s' value=%lld presTotal=%d\n",
         managerId, sponsorName.c_str(), offerValue, presTotal);
}

static void ProcessSponsorRenewals(int managerId, int clubId, int seasonYear,
                                   int position, int totalClubs, int expectedPos,
                                   const std::string &mgrName, const std::string &clubNameStr) {
  // Get all active contracts expiring this season
  std::stringstream cq;
  cq << "SELECT id, sponsor_id, sponsor_name, weekly_value FROM club_sponsors"
     << " WHERE manager_id=" << managerId << " AND club_id=" << clubId
     << " AND season_year=" << seasonYear << ";";
  DatabaseResult *cr = GetDB()->Query(cq.str());
  if (!cr || cr->data.size() == 0) { delete cr; return; }

  int posDelta = expectedPos - position; // positive = overperformed
  std::string nextSeasonDate = std::to_string(seasonYear + 1) + "-07-01";

  unsigned int rng = (unsigned int)(managerId * 7919u) ^ (unsigned int)(seasonYear * 31337u);
  auto NextRng = [&]() -> unsigned int { return rng = rng * 1664525u + 1013904223u; };

  for (unsigned int i = 0; i < cr->data.size(); i++) {
    int         cid      = atoi(DBCell(cr, i, 0).c_str());
    int         spId     = atoi(DBCell(cr, i, 1).c_str());
    std::string spName   = DBCell(cr, i, 2);
    long long   currVal  = atoll(DBCell(cr, i, 3).c_str());

    int roll = (int)(NextRng() % 100);
    bool willRenew  = true;
    float valueMult = 1.0f;

    if      (posDelta >= 3)  { valueMult = 1.10f + (float)(NextRng() % 21) / 100.0f; }
    else if (posDelta >= 1)  { valueMult = 1.00f + (float)(NextRng() % 11) / 100.0f; }
    else if (posDelta == 0)  { valueMult = 0.97f + (float)(NextRng() %  7) / 100.0f; }
    else if (posDelta >= -2) { if (roll < 25) willRenew = false;
                               else valueMult = 0.80f + (float)(NextRng() % 16) / 100.0f; }
    else                     { if (roll < 60) willRenew = false;
                               else valueMult = 0.65f + (float)(NextRng() % 21) / 100.0f; }

    // Remove the expiring contract
    {
      std::stringstream dq;
      dq << "DELETE FROM club_sponsors WHERE id=" << cid << ";";
      DatabaseResult *dr = GetDB()->Query(dq.str());
      delete dr;
    }

    if (!willRenew) {
      // Blacklist for next season
      std::stringstream blq;
      blq << "INSERT OR IGNORE INTO sponsor_blacklist"
          << " (manager_id, sponsor_id, blacklisted_season)"
          << " VALUES (" << managerId << "," << spId << "," << (seasonYear + 1) << ");";
      DatabaseResult *blr = GetDB()->Query(blq.str());
      delete blr;

      std::string currency = ClubCurrency(clubId);
      std::string subject, body;
      int tid = FillSponsorTemplate("sponsor_not_renewing", mgrName, clubNameStr,
                                    spName, currency, 0LL, subject, body);
      std::stringstream iq;
      iq << "INSERT INTO manager_inbox"
         << " (manager_id,template_id,sender_type,sender_name,subject,body,category,game_date)"
         << " VALUES (" << managerId << "," << tid << ",'finance','Commercial Director',"
         << "'" << SqlEsc(subject) << "','" << SqlEsc(body) << "','finance','" << nextSeasonDate << "');";
      DatabaseResult *ir = GetDB()->Query(iq.str());
      delete ir;
      printf("[SPONSOR] Renewal declined: manager=%d sponsor='%s' (blacklisted s%d)\n",
             managerId, spName.c_str(), seasonYear + 1);
    } else {
      long long newVal = (long long)((float)currVal * valueMult);
      newVal = (newVal / 2500) * 2500;
      if (newVal < 87500) newVal = 87500;

      std::stringstream iq;
      iq << "INSERT INTO pending_sponsor_offers"
         << " (manager_id, sponsor_id, sponsor_name, weekly_value, offered_date)"
         << " VALUES (" << managerId << "," << spId
         << ",'" << SqlEsc(spName) << "'," << newVal << ",'" << nextSeasonDate << "');";
      DatabaseResult *ir = GetDB()->Query(iq.str());
      delete ir;

      std::string currency = ClubCurrency(clubId);
      std::string subject, body;
      int tid = FillSponsorTemplate("sponsor_renewal", mgrName, clubNameStr,
                                    spName, currency, newVal, subject, body);
      std::stringstream iq2;
      iq2 << "INSERT INTO manager_inbox"
          << " (manager_id,template_id,sender_type,sender_name,subject,body,category,game_date)"
          << " VALUES (" << managerId << "," << tid << ",'finance','Commercial Director',"
          << "'" << SqlEsc(subject) << "','" << SqlEsc(body) << "','finance','" << nextSeasonDate << "');";
      DatabaseResult *ir2 = GetDB()->Query(iq2.str());
      delete ir2;
      printf("[SPONSOR] Renewal offer: manager=%d sponsor='%s' old=%lld new=%lld\n",
             managerId, spName.c_str(), currVal, newVal);
    }
  }
  delete cr;
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
  PAGE_PLAYER_DETAIL,
  PAGE_STAFF_MARKET,
  PAGE_CLUB_DETAIL,
  PAGE_COUNT
};

static const char *kPageNames[PAGE_COUNT] = {
  "Home", "Inbox", "News", "Schedule",
  "Squad", "Tactics", "Training", "Staff", "Scouting", "Finances", "Transfers",
  "Competitions", "Fixtures", "Players", "Teams", "Settings", "Player", "Staff Market", "Club"
};

static e_ManagerPage g_activePage = PAGE_HOME;
static std::vector<e_ManagerPage> s_navBack;
static std::vector<e_ManagerPage> s_navFwd;
static bool s_escMenuOpen       = false;
static bool s_escMenuJustOpened = false; // suppress ESC-close on the frame it was opened
static int  s_playerDetailId     = -1;
static int  s_playerDetailLastId = -1;
static int  s_plTab              = 0;

// ---- Bid popup shared state (used by DrawTransfersPage + DrawPlayerDetailPage) ----
static int  s_bidPlayerId_g     = 0;
static int  s_bidSellerClubId_g = 0;
static int  s_bidFee_g          = 0;
static int  s_bidWage_g         = 0;
static int  s_bidRoleIdx_g      = 1;
static char s_bidFeeStr_g[32]   = "";
static char s_bidWageStr_g[32]  = "";
static char s_bidPlayerName_g[128] = "";
static bool s_openBidPopup_g    = false;

static int  s_reviewNegotiationId_g = 0;
static char s_reviewCounterFeeStr_g[32] = "";
static char s_reviewPlayerName_g[128] = "";
static char s_reviewBuyerName_g[128] = "";
static bool s_openReviewPopup_g = false;

static int  s_listPlayerId_g = 0;
static char s_listPlayerName_g[128] = "";
static char s_listAskingValueStr_g[32] = "";
static bool s_openListPlayerPopup_g = false;

static int  s_loanPlayerId_g = 0;
static int  s_loanParentClubId_g = 0;
static int  s_loanReceivingClubId_g = 0;
static char s_loanPlayerName_g[128] = "";
static char s_loanFeeStr_g[32] = "0";
static char s_loanWagePctStr_g[32] = "70";
static char s_loanEndDateStr_g[32] = "";
static char s_loanOptionFeeStr_g[32] = "0";
static char s_loanMandatoryFeeStr_g[32] = "0";
static char s_loanMandatoryAppsStr_g[32] = "0";
static char s_loanClubSearchStr_g[64] = "";
static int  s_loanPlayingTimeIdx_g = 1;
static int  s_loanMandatoryModeIdx_g = 0;
static bool s_loanIsLoanIn_g = true;
static bool s_openLoanPopup_g = false;

static int  s_loanReviewDealId_g = 0;
static char s_loanReviewFeeStr_g[32] = "";
static char s_loanReviewWagePctStr_g[32] = "";
static char s_loanReviewOptionFeeStr_g[32] = "0";
static char s_loanReviewEndDateStr_g[32] = "";
static char s_loanReviewMandatoryFeeStr_g[32] = "0";
static char s_loanReviewMandatoryAppsStr_g[32] = "0";
static char s_loanReviewPlayerName_g[128] = "";
static char s_loanReviewClubName_g[128] = "";
static int  s_loanReviewMandatoryModeIdx_g = 0;
static bool s_openLoanReviewPopup_g = false;
static std::map<int, bool> s_incomingOfferGroupOpen_g;
static std::map<int, bool> s_incomingLoanGroupOpen_g;

// ---- Live player search --------------------------------------------------
struct SearchPlayerResult {
  int         id            = 0;
  std::string firstName, lastName, nickname, role, age, clubName, clubShortName, clubLogoPath;
  float       baseStat      = 0.0f;
  float       height        = 0.0f;
  float       reputation    = 0.0f;
  int         weeklywage    = 0;
  std::string contractExpiry, foot;
  int         stamina       = 0;
  int         potential     = 0;
  int         formationOrder = -1;
};
static char   s_searchBuf[128]              = "";
static std::string  s_searchLastTerm;
static std::vector<SearchPlayerResult> s_searchResults;
static bool   s_searchActive                = false;
static ImVec2 s_searchDropdownPos;
static float  s_searchDropdownW             = 360.0f;
static bool   s_detailOverrideActive        = false;
static CareerHubState::Player s_detailPlayerOverride;
static std::string s_detailClubName;
static std::string s_detailClubLogo;
static std::string s_detailClubShortName;

// ---- Club detail page state ----------------------------------------------
struct ClubDetailData {
  int         id              = -1;
  std::string name, shortName, logoPath;
  std::string color1, color2;
  std::string homeStadium;
  int         intPrestige     = 0;
  int         domPrestige     = 0;
  struct ClubPlayer {
    int         id            = 0;
    int         jerseyNumber  = 0;
    std::string name, role, age;
    float       ability = 0.0f;
  };
  std::vector<ClubPlayer> squad;
};
static int            s_clubDetailId     = -1;
static int            s_clubDetailLastId = -1;
static ClubDetailData s_clubDetail;

static void LoadClubDetail(int teamId) {
  s_clubDetail = ClubDetailData();
  s_clubDetail.id = teamId;
  {
    std::stringstream q;
    q << "SELECT name, shortname, logo_url, color1, color2, home_stadium,"
      << " international_prestige, domestic_prestige"
      << " FROM teams WHERE id=" << teamId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (!r->data.empty()) {
      s_clubDetail.name         = DBCell(r, 0, 0);
      s_clubDetail.shortName    = DBCell(r, 0, 1);
      s_clubDetail.logoPath     = DBCell(r, 0, 2);
      s_clubDetail.color1       = DBCell(r, 0, 3);
      s_clubDetail.color2       = DBCell(r, 0, 4);
      s_clubDetail.homeStadium  = DBCell(r, 0, 5);
      s_clubDetail.intPrestige  = atoi(DBCell(r, 0, 6).c_str());
      s_clubDetail.domPrestige  = atoi(DBCell(r, 0, 7).c_str());
    }
    delete r;
  }
  {
    std::stringstream q;
    q << "SELECT id, COALESCE(nickname,''), firstname, lastname, role, age, base_stat,"
      << " COALESCE(jersey_number,0)"
      << " FROM players"
      << " LEFT JOIN player_save_state pss"
      << "   ON pss.manager_id=" << g_CareerHub.managerId << " AND pss.player_id=players.id"
      << " WHERE COALESCE(pss.team_id, players.team_id)=" << teamId
      << " ORDER BY COALESCE(jersey_number,999) ASC, base_stat DESC LIMIT 50;";
    DatabaseResult *r = GetDB()->Query(q.str());
    for (unsigned int i = 0; i < r->data.size(); i++) {
      ClubDetailData::ClubPlayer cp;
      cp.id      = atoi(DBCell(r, i, 0).c_str());
      std::string nick  = DBCell(r, i, 1);
      std::string first = DBCell(r, i, 2);
      std::string last  = DBCell(r, i, 3);
      if (!nick.empty())
        cp.name = nick;
      else if (!first.empty() && !last.empty())
        cp.name = first + " " + last;
      else
        cp.name = last.empty() ? first : last;
      cp.role          = DBCell(r, i, 4);
      cp.age           = DBCell(r, i, 5);
      cp.ability       = (float)atof(DBCell(r, i, 6).c_str());
      cp.jerseyNumber  = atoi(DBCell(r, i, 7).c_str());
      s_clubDetail.squad.push_back(cp);
    }
    delete r;
  }
}

// ---- Staff market state --------------------------------------------------
struct StaffMarketEntry {
  int         id         = 0;
  std::string firstName, lastName, nationality, role;
  int         age        = 0;
  int         rating     = 0;
  int         weeklywage = 0;
};
static std::string              s_staffMarketRole;
static std::vector<StaffMarketEntry> s_staffMarketList;
static bool                     s_staffMarketLoaded = false;

// Navigate to a new page — pushes current to back stack, clears forward stack.
static void NavPush(e_ManagerPage page) {
  if (page == g_activePage) return;
  s_navBack.push_back(g_activePage);
  s_navFwd.clear();
  g_activePage = page;
}
static void NavBack() {
  if (s_navBack.empty()) return;
  s_navFwd.push_back(g_activePage);
  g_activePage = s_navBack.back();
  s_navBack.pop_back();
}
static void NavForward() {
  if (s_navFwd.empty()) return;
  s_navBack.push_back(g_activePage);
  g_activePage = s_navFwd.back();
  s_navFwd.pop_back();
}
static void NavToClubDetail(int teamId) {
  if (teamId <= 0) return;
  s_clubDetailId = teamId;
  s_clubDetailLastId = -1;
  NavPush(PAGE_CLUB_DETAIL);
}
static int LookupTeamIdByName(const std::string &name) {
  if (name.empty()) return -1;
  std::string escaped;
  for (char c : name) { if (c == '\'') escaped += "''"; else escaped += c; }
  std::stringstream q;
  q << "SELECT id FROM teams WHERE name='" << escaped << "' LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str());
  int id = -1;
  if (!r->data.empty()) id = atoi(DBCell(r, 0, 0).c_str());
  delete r;
  return id;
}
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
  s_navBack.clear();
  s_navFwd.clear();
  s_escMenuOpen = false;
  s_calInit       = false;
  s_compInit      = false;
  s_compCountry   = -1;
  s_compLeague    = -1;
  s_schedInit     = false;
  s_schedClubInit = false;
  s_schedCountry      = -1;
  s_schedLeague       = -1;
  s_schedClub         = -1;
  s_staffMarketLoaded = false;
}

// ---- Color palette ------------------------------------------------------

static const ImVec4 kBgApp     = ImVec4(0.027f, 0.043f, 0.086f, 1.0f); // #070B16
static const ImVec4 kBgSidebar = ImVec4(0.043f, 0.063f, 0.125f, 1.0f); // #0B1020
static const ImVec4 kBgHeader  = ImVec4(0.051f, 0.075f, 0.141f, 1.0f); // #0D1324
static const ImVec4 kBgCard    = ImVec4(0.071f, 0.102f, 0.173f, 1.0f); // #121A2C
static const ImVec4 kBgCardAlt = ImVec4(0.094f, 0.129f, 0.212f, 1.0f); // #182136
static const ImVec4 kBorder    = ImVec4(0.149f, 0.196f, 0.290f, 0.80f); // #26324A
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

static ImU32 SenderTypeColor(const std::string &t) {
  if (t == "board")       return IM_COL32( 59,130,246,255);
  if (t == "staff")       return IM_COL32( 34,197, 94,255);
  if (t == "media")       return IM_COL32(148,163,184,255);
  if (t == "fans")        return IM_COL32(251,191, 36,255);
  if (t == "players")     return IM_COL32(167,139,250,255);
  if (t == "competition") return IM_COL32( 34,211,238,255);
  if (t == "transfers")   return IM_COL32(251,146, 60,255);
  if (t == "finance")     return IM_COL32( 52,211,153,255);
  return IM_COL32(100,120,160,255);
}

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
  dl->AddCircleFilled(br, w*0.45f, IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),11), 48);
  dl->AddCircleFilled(br, w*0.22f, IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),15), 48);
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
  if (tex) {
    ImVec2 fit = BadgeFitInSquare(tex, sz);
    float ox = (sz - fit.x) * 0.5f;
    float oy = (sz - fit.y) * 0.5f;
    ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddImage(
      (ImTextureID)(intptr_t)tex,
      ImVec2(cur.x + ox, cur.y + oy),
      ImVec2(cur.x + ox + fit.x, cur.y + oy + fit.y));
    ImGui::Dummy(ImVec2(sz, sz));
  } else {
    DrawFallbackBadge(sn, sz);
  }
}

static void DrawTeamLabel(const std::string &logoPath, const std::string &sn, float badgeSz = 18.0f) {
  float lh   = ImGui::GetTextLineHeight();
  float offY = (lh - badgeSz) * 0.5f;
  if (offY > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offY);
  GLuint tex = LoadBadgeTex(logoPath);
  if (tex) {
    ImVec2 fit = BadgeFitInSquare(tex, badgeSz);
    float ox = (badgeSz - fit.x) * 0.5f;
    float oy = (badgeSz - fit.y) * 0.5f;
    ImVec2 cur = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddImage(
      (ImTextureID)(intptr_t)tex,
      ImVec2(cur.x + ox, cur.y + oy),
      ImVec2(cur.x + ox + fit.x, cur.y + oy + fit.y));
    ImGui::Dummy(ImVec2(badgeSz, badgeSz));
  } else {
    DrawFallbackBadge(sn, badgeSz);
  }
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
  snprintf(btnId, sizeof(btnId), "##navbtn_%d_%s", (int)page, label);
  bool clicked = ImGui::InvisibleButton(btnId, ImVec2(w, kH));
  bool hov     = ImGui::IsItemHovered();

  ImDrawList *dl = ImGui::GetWindowDrawList();
  if (active) {
    dl->AddRectFilled(p0, p1, IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),75), 6.0f);
    dl->AddRectFilled(p0, ImVec2(p0.x + 3.0f, p1.y), C32(kAccent), 1.5f);
  } else if (hov) {
    dl->AddRectFilled(p0, p1, IM_COL32(22, 38, 76, 175), 6.0f);
  }

  // Text via DrawList so we control screen position exactly
  ImFont *font = active ? g_ManagerFontBold : g_ManagerFontRegular;
  if (font) ImGui::PushFont(font);
  float lh = ImGui::GetTextLineHeight();
  ImVec2 tp(p0.x + 16.0f, p0.y + (kH - lh) * 0.5f);
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

  if (clicked) NavPush(page);
}

// ---- Action flags (deferred, consumed after Handle()) -------------------

static bool s_advanceClicked     = false; // advance day or play fixture
static bool s_startSeasonClicked = false; // start next season
static bool s_menuClicked        = false;
static bool s_advanceModePopupWasOpen = false;
// Returns count of players with formationOrder 0-19 (active squad slots)
static int CountActiveSquad() {
  int n = 0;
  for (const auto &p : g_CareerHub.players)
    if (p.formationOrder >= 0 && p.formationOrder <= 19 &&
        p.injuryDays <= 0 && p.suspensionMatches <= 0) n++;
  return n;
}

// ---- DrawSidebar --------------------------------------------------------

static void DrawSidebar(float sideW, float winH) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSidebar);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##sidebar", ImVec2(sideW, winH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  ImDrawList *dl = ImGui::GetWindowDrawList();

  // ---- Brand (game logo) ----------------------------------------------
  {
    GLuint logoTex = GetMainLogoTexture();
    if (logoTex) {
      const float logoW = sideW - 32.0f;
      const float logoH = logoW / kMainLogoAspect;
      float lx = ImGui::GetWindowPos().x + 16.0f;
      float ly = ImGui::GetWindowPos().y + 14.0f;
      dl->AddImage((ImTextureID)(intptr_t)logoTex,
                   ImVec2(lx, ly), ImVec2(lx + logoW, ly + logoH));
      ImGui::Dummy(ImVec2(0, logoH + 14.0f));
    } else {
      ImGui::SetCursorPos(ImVec2(16.0f, 16.0f));
      PushMgrFont(g_ManagerFontBold);
      ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
      ImGui::TextUnformatted("Classic XI Manager");
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontBold);
    }
  }

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
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  {
    const std::string &mgrName = g_CareerHub.manager.name;
    if (!mgrName.empty()) ImGui::TextUnformatted(mgrName.c_str());
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
  // Reserve space for bottom section: separator + version text + bottom breathing room
  const float kBottomH = 1.0f + 8.0f + 20.0f + 20.0f;
  float navH = ImGui::GetContentRegionAvail().y - kBottomH;
  if (navH < 40.0f) navH = 40.0f;

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##nav_area", ImVec2(0, navH), false);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  DrawNavSectionHeader("MAIN");
  DrawNavItem("Home",     PAGE_HOME);
  {
    int unread = 0;
    for (const auto &m : g_CareerHub.inbox) if (!m.isRead) unread++;
    DrawNavItem("Inbox", PAGE_INBOX, unread);
  }
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
  {
    // Compute pending transfer actions (incoming bids needing user response)
    int pendingTransfers = 0;
    for (const auto &tn : g_CareerHub.inbox) {
      // inbox entries with category 'transfer' count as notifications already
      if (!tn.isRead && tn.category == "transfer") pendingTransfers++;
    }
    // Additionally count unresolved incoming transfer_negotiations where user must act
    std::stringstream pq; pq << "SELECT COUNT(*) FROM transfer_negotiations tn"
                           << " WHERE tn.manager_id=" << g_CareerHub.managerId
                           << " AND tn.selling_club_id=" << g_CareerHub.clubId
                           << " AND tn.is_user_bid=0"
                           << " AND tn.seller_approved=0"
                           << " AND tn.state IN ('initiated','offer_made','negotiating','counter_offer')";
    DatabaseResult *pr = GetDB()->Query(pq.str().c_str());
    if (pr && pr->data.size() > 0 && !pr->data[0].empty()) pendingTransfers += atoi(pr->data[0][0].c_str());
    if (pr) delete pr;
    DrawNavItem("Transfers", PAGE_TRANSFERS, pendingTransfers);
  }
  ImGui::Dummy(ImVec2(0, 6.0f));

  DrawNavSectionHeader("LEAGUE");
  DrawNavItem("Fixtures",   PAGE_SCHEDULE);
  DrawNavItem("Standings",  PAGE_COMPETITIONS);
  ImGui::Dummy(ImVec2(0, 6.0f));
  DrawNavSectionHeader("CUPS");
  DrawNavItem("Domestic",   PAGE_COUNT); // placeholder
  DrawNavItem("European",   PAGE_COUNT); // placeholder

  ImGui::EndChild(); // nav_area

  // ---- Bottom ---------------------------------------------------------
  {
    ImVec2 sp = ImGui::GetCursorScreenPos();
    dl->AddLine(sp, ImVec2(sp.x + sideW, sp.y), C32(kBorder), 1.0f);
    ImGui::Dummy(ImVec2(0, 8.0f));
  }

  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##nav_bottom", ImVec2(0, kBottomH - 8.0f), false,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  // Centered version text
  {
    ImDrawList *bdl = ImGui::GetWindowDrawList();
    ImVec2 bp = ImGui::GetCursorScreenPos();
    static const char *kVerText = "Classic XI Manager - Alpha v0.0.01";
    PushMgrFont(g_ManagerFontSmall);
    ImVec2 tsz = ImGui::CalcTextSize(kVerText);
    float childH = kBottomH - 8.0f;
    float tx = bp.x + (sideW - tsz.x) * 0.5f;
    float ty = bp.y + (childH - tsz.y) * 0.5f;
    bdl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                 ImVec2(tx, ty), IM_COL32(120, 130, 160, 180), kVerText);
    PopMgrFont(g_ManagerFontSmall);
    ImGui::Dummy(ImVec2(0, childH));
  }

  ImGui::EndChild(); // nav_bottom
  ImGui::EndChild(); // sidebar
}

static void RunPlayerSearch(const char *raw);  // forward declaration
static void DrawSearchDropdown();              // forward declaration

// ---- DrawTopHeader ------------------------------------------------------

static void DrawTopHeader(float contentX, float contentW) {
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgHeader);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##topheader", ImVec2(contentW, kTopHdrH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();

  ImDrawList *dl = ImGui::GetWindowDrawList();

  // ---- Left: back / forward navigation arrows ----------------------------
  {
    const float kArrowSz = 28.0f;
    const float kArrowGap = 4.0f;
    const float kLeftMar = 16.0f;
    float ay = (kTopHdrH - kArrowSz) * 0.5f;

    bool canBack = !s_navBack.empty();
    bool canFwd  = !s_navFwd.empty();

    auto DrawNavArrow = [&](const char *id, const char *symbol, float ax, bool enabled) {
      ImGui::SetCursorPos(ImVec2(ax, ay));
      ImVec2 p0 = ImGui::GetCursorScreenPos();
      ImVec2 p1 = ImVec2(p0.x + kArrowSz, p0.y + kArrowSz);

      bool clicked = false;
      if (enabled) {
        clicked = ImGui::InvisibleButton(id, ImVec2(kArrowSz, kArrowSz));
        bool hov = ImGui::IsItemHovered();
        int ar = (int)(kAccent.x*255), ag = (int)(kAccent.y*255), ab = (int)(kAccent.z*255);
        ImU32 bg = hov ? IM_COL32(ar, ag, ab, 80) : IM_COL32(ar, ag, ab, 40);
        dl->AddRectFilled(p0, p1, bg, 6.0f);
        dl->AddRect(p0, p1, IM_COL32(ar, ag, ab, 180), 6.0f, 0, 1.0f);
        PushMgrFont(g_ManagerFontBold);
        ImVec2 tsz = ImGui::CalcTextSize(symbol);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(p0.x + (kArrowSz - tsz.x) * 0.5f,
                           p0.y + (kArrowSz - tsz.y) * 0.5f),
                    IM_COL32(ar, ag, ab, 255), symbol);
        PopMgrFont(g_ManagerFontBold);
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
      } else {
        ImGui::InvisibleButton(id, ImVec2(kArrowSz, kArrowSz)); // consume space
        dl->AddRectFilled(p0, p1, IM_COL32(15, 22, 48, 120), 6.0f);
        dl->AddRect(p0, p1, IM_COL32(40, 55, 90, 80), 6.0f, 0, 1.0f);
        PushMgrFont(g_ManagerFontBold);
        ImVec2 tsz = ImGui::CalcTextSize(symbol);
        dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
                    ImVec2(p0.x + (kArrowSz - tsz.x) * 0.5f,
                           p0.y + (kArrowSz - tsz.y) * 0.5f),
                    IM_COL32(80, 90, 120, 140), symbol);
        PopMgrFont(g_ManagerFontBold);
      }
      return clicked;
    };

    if (DrawNavArrow("##nav_back", "<", kLeftMar, canBack)) NavBack();
    if (DrawNavArrow("##nav_fwd",  ">", kLeftMar + kArrowSz + kArrowGap, canFwd)) NavForward();
  }

  // ---- Right: search + date + Advance/PlayMatch controls -----------------
  const float kBtnW    = 112.0f;  // each button width
  const float kDropW   = 32.0f;
  const float kDateW   = 88.0f;   // wider for "31 Aug 2026"
  const float kSearchW = 210.0f;
  const float kElemH   = 30.0f;
  const float kGap     = 8.0f;
  float elemY     = (kTopHdrH - kElemH) * 0.5f;
  float rightEdge = contentW - 16.0f;

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 5.0f));
  PushMgrFont(g_ManagerFontBold);

  // Advance / Play Match / Start Season button — priority:
  // 1. isAdvancing -> disabled "Advancing..."
  // 2. hasTodayFixture -> "Play Match"
  // 3. hasSeasonEnded -> "Start Season"
  // 4. else -> "Advance"
  float dropBtnX = rightEdge - kDropW;
  float advBtnX = dropBtnX - kGap - kBtnW;
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

  ImGui::SetCursorPos(ImVec2(dropBtnX, elemY));
  ImVec2 dropScreen = ImGui::GetCursorScreenPos();
  bool advanceModeSelected = (g_CareerHub.advanceMode == ADVANCE_MODE_NEXT_MATCH);
  if (g_CareerHub.isAdvancing || g_CareerHub.hasTodayFixture || g_CareerHub.hasSeasonEnded) {
    ImGui::BeginDisabled();
    SecBtn("##advance_mode_btn", ImVec2(kDropW, kElemH));
    ImGui::EndDisabled();
  } else {
    if (advanceModeSelected) {
      ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(25, 92, 160, 255));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(32, 112, 190, 255));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(18, 76, 140, 255));
    }
    if (SecBtn("##advance_mode_btn", ImVec2(kDropW, kElemH))) ImGui::OpenPopup("##advance_mode_popup");
    if (advanceModeSelected) ImGui::PopStyleColor(3);
  }

  ImDrawList *fg = ImGui::GetWindowDrawList();
  ImU32 chevCol = advanceModeSelected ? IM_COL32(255, 255, 255, 245) : IM_COL32(180, 190, 215, 230);
  ImVec2 c(dropScreen.x + kDropW * 0.5f, dropScreen.y + kElemH * 0.5f + 1.0f);
  fg->AddTriangleFilled(ImVec2(c.x - 4.5f, c.y - 2.0f),
                        ImVec2(c.x + 4.5f, c.y - 2.0f),
                        ImVec2(c.x,        c.y + 3.5f),
                        chevCol);

  ImGui::SetNextWindowPos(ImVec2(dropScreen.x + kDropW - 218.0f, dropScreen.y + kElemH + 6.0f),
                          ImGuiCond_Appearing);
  ImGui::SetNextWindowSize(ImVec2(218.0f, 48.0f), ImGuiCond_Appearing);
  bool advanceModePopupOpen = false;
  if (ImGui::BeginPopup("##advance_mode_popup")) {
    advanceModePopupOpen = true;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(6.0f, 6.0f));
    PushMgrFont(g_ManagerFontSmall);
    bool selected = advanceModeSelected;
    const ImVec2 rowSize(210.0f, 32.0f);
    ImGui::PushID("advance_until_matchday");
    if (selected) {
      ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(24, 92, 165, 255));
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(30, 105, 185, 255));
      ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(18, 76, 145, 255));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(15, 24, 46, 255));
      ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(24, 36, 68, 255));
      ImGui::PushStyleColor(ImGuiCol_HeaderActive,  IM_COL32(30, 46, 82, 255));
    }
    bool clicked = ImGui::Selectable("##until_next_matchday", selected, 0, rowSize);
    ImGui::PopStyleColor(3);
    ImVec2 r0 = ImGui::GetItemRectMin();
    ImVec2 r1 = ImGui::GetItemRectMax();
    ImDrawList *pdl = ImGui::GetWindowDrawList();
    if (selected) {
      pdl->AddCircleFilled(ImVec2(r0.x + 17.0f, (r0.y + r1.y) * 0.5f), 8.0f,
                           IM_COL32(255, 255, 255, 245), 16);
      ImVec2 checkA(r0.x + 12.5f, r0.y + 16.5f);
      ImVec2 checkB(r0.x + 15.8f, r0.y + 19.5f);
      ImVec2 checkC(r0.x + 21.8f, r0.y + 12.5f);
      pdl->AddLine(checkA, checkB, IM_COL32(18, 76, 145, 255), 2.0f);
      pdl->AddLine(checkB, checkC, IM_COL32(18, 76, 145, 255), 2.0f);
    } else {
      pdl->AddCircle(ImVec2(r0.x + 17.0f, (r0.y + r1.y) * 0.5f), 8.0f,
                     IM_COL32(95, 115, 150, 200), 16, 1.5f);
    }
    pdl->AddText(g_ManagerFontSmall, 13.0f,
                 ImVec2(r0.x + 36.0f, r0.y + 8.0f),
                 selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(205, 215, 235, 245),
                 "Until Next Match Day");
    if (clicked) {
      g_CareerHub.advanceMode = selected ? ADVANCE_MODE_NEXT_DAY : ADVANCE_MODE_NEXT_MATCH;
    }
    ImGui::PopID();
    PopMgrFont(g_ManagerFontSmall);
    ImGui::PopStyleVar(2);
    ImGui::EndPopup();
  }
  s_advanceModePopupWasOpen = advanceModePopupOpen;

  PopMgrFont(g_ManagerFontBold);
  ImGui::PopStyleVar();

  // Current date display
  PushMgrFont(g_ManagerFontSmall);
  float dateH = ImGui::GetTextLineHeight();
  PopMgrFont(g_ManagerFontSmall);
  float dateX = advBtnX - kGap - kDateW;
  ImGui::SetCursorPos(ImVec2(dateX, elemY + (kElemH - dateH) * 0.5f));
  PushMgrFont(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  const std::string &dateStr = g_CareerHub.currentDateDisplay.empty()
                                 ? g_CareerHub.currentDate
                                 : g_CareerHub.currentDateDisplay;
  ImGui::TextUnformatted(dateStr.empty() ? "--" : dateStr.c_str());
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontSmall);

  // Live search bar — centered in the top header
  float searchX = (contentW - kSearchW) * 0.5f;
  ImGui::SetCursorPos(ImVec2(searchX, elemY));
  ImVec2 scr = ImGui::GetCursorScreenPos();

  // Style the InputText to match the dark UI
  {
    int ar2 = (int)(kAccent.x*255), ag2c = (int)(kAccent.y*255), ab2c = (int)(kAccent.z*255);
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        C32(ImVec4(0.055f,0.082f,0.153f,1.0f)));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(22, 34, 68, 255));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  IM_COL32(18, 28, 58, 255));
    ImGui::PushStyleColor(ImGuiCol_Border,         IM_COL32(ar2, ag2c, ab2c, 140));
  }
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,  ImVec2(32.0f, 6.0f)); // leave room for icon
  PushMgrFont(g_ManagerFontSmall);
  ImGui::SetNextItemWidth(kSearchW);
  bool changed = ImGui::InputText("##search_bar", s_searchBuf, sizeof(s_searchBuf),
                                   ImGuiInputTextFlags_AutoSelectAll);
  bool focused = ImGui::IsItemFocused() || ImGui::IsItemActive();
  PopMgrFont(g_ManagerFontSmall);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(4);

  // Hint text when empty
  if (s_searchBuf[0] == '\0') {
    PushMgrFont(g_ManagerFontSmall);
    dl->AddText(g_ManagerFontSmall, 12.0f,
                ImVec2(scr.x + 32.0f, scr.y + (kElemH - 12.0f) * 0.5f),
                IM_COL32(90, 105, 140, 180), "Search players...");
    PopMgrFont(g_ManagerFontSmall);
  }

  // Search icon (magnifying glass — always in accent color)
  {
    float ic = scr.x + 14.0f, iy = scr.y + kElemH * 0.5f;
    ImU32 iconCol = focused ? C32(kAccent) : IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),180);
    dl->AddCircle(ImVec2(ic, iy - 1.0f), 5.5f, iconCol, 12, 1.4f);
    dl->AddLine(ImVec2(ic + 3.8f, iy + 2.8f), ImVec2(ic + 7.5f, iy + 6.5f), iconCol, 1.8f);
  }

  // Accent border glow — always visible, brighter when focused
  {
    int ar = (int)(kAccent.x*255), ag2 = (int)(kAccent.y*255), ab2 = (int)(kAccent.z*255);
    dl->AddRect(scr, ImVec2(scr.x + kSearchW, scr.y + kElemH),
                IM_COL32(ar, ag2, ab2, focused ? 200 : 120), 6.0f, 0, focused ? 2.0f : 1.2f);
  }

  // X clear button when there's text
  if (s_searchBuf[0] != '\0') {
    float xBtnX = scr.x + kSearchW - 22.0f;
    float xBtnY = scr.y + (kElemH - 16.0f) * 0.5f;
    ImVec2 mp   = ImGui::GetMousePos();
    bool xHov   = mp.x >= xBtnX && mp.x <= xBtnX+16.0f &&
                  mp.y >= xBtnY && mp.y <= xBtnY+16.0f;
    ImU32 xCol  = xHov ? IM_COL32(220, 80, 80, 255) : IM_COL32(100, 115, 150, 180);
    dl->AddLine(ImVec2(xBtnX+3, xBtnY+3), ImVec2(xBtnX+13, xBtnY+13), xCol, 1.8f);
    dl->AddLine(ImVec2(xBtnX+13, xBtnY+3), ImVec2(xBtnX+3, xBtnY+13), xCol, 1.8f);
    if (xHov && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      memset(s_searchBuf, 0, sizeof(s_searchBuf));
      s_searchResults.clear();
      s_searchActive   = false;
      s_searchLastTerm = "";
      ImGui::SetKeyboardFocusHere(-1); // release focus from InputText
    }
  }

  // Store dropdown anchor
  s_searchDropdownPos = ImVec2(scr.x + (kSearchW - s_searchDropdownW) * 0.5f,
                                scr.y + kElemH + 4.0f);

  // Trigger search on change
  std::string curTerm = s_searchBuf;
  if (changed || curTerm != s_searchLastTerm) {
    s_searchLastTerm = curTerm;
    RunPlayerSearch(s_searchBuf);
    s_searchActive = !curTerm.empty();
  }

  // Dropdown is rendered by RenderImGuiCareerHub just before End() so it sits on top.

  // Bottom border
  ImVec2 hp = ImGui::GetWindowPos();
  dl->AddLine(ImVec2(hp.x, hp.y + kTopHdrH - 1),
              ImVec2(hp.x + contentW, hp.y + kTopHdrH - 1),
              C32(kBorder), 1.0f);

  ImGui::EndChild();
}

// ---- Home page cards ----------------------------------------------------

static void DrawMessagesCard(ImVec2 sz) {
  const auto &inbox = g_CareerHub.inbox;
  const int kN = (int)inbox.size() < 8 ? (int)inbox.size() : 8;
  const bool hasReal = kN > 0;

  // Fallback mock rows when inbox not yet loaded
  static const struct { const char *from, *subject, *time; } kMock[] = {
    { "Board",  "Pre-season objectives confirmed", "Jul 1"    },
    { "Board",  "Transfer budget confirmed",       "Jul 1"    },
    { "Staff",  "Pre-season fitness assessment",   "Jul 1"    },
    { "Comp.",  "Season begins",                   "Jul 1"    },
  };
  const int kMockN = 4;

  BeginModernCard("##msgs_ov", sz);
  {
    int rows = hasReal ? kN : kMockN;
    PushMgrFont(g_ManagerFontSmall);
    float fh = ImGui::GetTextLineHeight();
    PopMgrFont(g_ManagerFontSmall);
    float tableH = rows * (fh + 14.0f);
    float innerH = sz.y - 24.0f;
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
    if (hasReal) {
      for (int i = 0; i < kN; i++) {
        const auto &m = inbox[i];
        // Shorten sender name to first word or 8 chars
        std::string sn = m.senderName;
        size_t sp = sn.find(' ');
        if (sp != std::string::npos) sn = sn.substr(0, sp);
        if (sn.size() > 8) sn = sn.substr(0, 7) + ".";
        // Shorten date to "D Mon"
        std::string ds = FormatDateDisplay(m.gameDate);
        size_t ls = ds.rfind(' ');
        if (ls != std::string::npos) ds = ds.substr(0, ls);

        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImU32 snCol = m.isRead ? C32(kTextSec) : C32(kAccent);
        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(snCol));
        ImGui::TextUnformatted(sn.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        if (!m.isRead) ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
        else           ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
        ImGui::TextUnformatted(m.subject.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::TextUnformatted(ds.c_str());
        ImGui::PopStyleColor();
      }
    } else {
      for (int i = 0; i < kMockN; i++) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
        ImGui::TextUnformatted(kMock[i].from);
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextUnformatted(kMock[i].subject);
        ImGui::TableSetColumnIndex(2);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::TextUnformatted(kMock[i].time);
        ImGui::PopStyleColor();
      }
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
  if (highlight) {
    int ar = (int)(kAccent.x*255), ag = (int)(kAccent.y*255), ab = (int)(kAccent.z*255);
    ImGui::GetWindowDrawList()->AddRect(p0, ImVec2(p0.x+teamW, p0.y+boxH),
                                        IM_COL32(ar, ag, ab, 200), 8.0f, 0, 1.5f);
  }
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
  ImGui::PushStyleColor(ImGuiCol_Text, highlight ? ImVec4(1,1,1,1) : kTextPri);
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
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
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
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
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
      bool isUserRow = (isUserHome || isUserAway);
      ImGui::TableNextRow();
      if (isUserRow) {
        int ar = (int)(kAccent.x * 255), ag = (int)(kAccent.y * 255), ab = (int)(kAccent.z * 255);
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(ar, ag, ab, 45));
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(ar, ag, ab, 45));
      }
      ImGui::TableSetColumnIndex(0);
      if (isUserHome) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
      DrawTeamLabel(f.homeLogo, hDisp, 20.0f);
      if (isUserHome) ImGui::PopStyleColor();
      ImGui::TableSetColumnIndex(1);
      if (!f.score.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
        ImGui::TextUnformatted(f.score.c_str());
        ImGui::PopStyleColor();
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
        ImGui::TextUnformatted("vs");
        ImGui::PopStyleColor();
      }
      ImGui::TableSetColumnIndex(2);
      if (isUserAway) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1,1,1,1));
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
  ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
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
      if (mine) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),42));
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

// ---- Tactic instruction data (shared by home overview + tactics page) ------

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

// ---- Placeholder home panels -----------------------------------------------

static void DrawPlaceholderHeader(const char *title) {
  PushMgrFont(g_ManagerFontBold);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted(title);
  ImGui::PopStyleColor();
  PopMgrFont(g_ManagerFontBold);
  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Spacing();
}

static void DrawTrainingScheduleCard(ImVec2 sz) {
  BeginModernCard("##training_ph", sz);

  static const struct { const char *abbr; const char *focus; } kWeek[] = {
    { "Mon", "Tactical"  },
    { "Tue", "Physical"  },
    { "Wed", "Rest"      },
    { "Thu", "Set Pieces"},
    { "Fri", "Match Prep"},
    { "Sat", "Recovery"  },
    { "Sun", "Day Off"   },
  };

  ImDrawList *dl = ImGui::GetWindowDrawList();
  float avW = ImGui::GetContentRegionAvail().x;
  float avH = ImGui::GetContentRegionAvail().y;
  const int kN = 7;
  float cellW = avW / (float)kN;
  float cellH = avH - 4.0f;
  cellH = cellH < 40.0f ? 40.0f : cellH;
  float startX = ImGui::GetCursorScreenPos().x;
  float startY = ImGui::GetCursorScreenPos().y + 2.0f;

  PushMgrFont(g_ManagerFontSmall);
  float lh = ImGui::GetTextLineHeight();
  int todayDow = -1;
  {
    const std::string &cd = g_CareerHub.currentDate;
    if (cd.size() >= 10) {
      // weekday from career date: quick Zeller's congruence
      int y = atoi(cd.substr(0,4).c_str());
      int m = atoi(cd.substr(5,2).c_str());
      int d = atoi(cd.substr(8,2).c_str());
      if (m < 3) { m += 12; y--; }
      int k = y % 100, j = y / 100;
      int h = (d + (13*(m+1))/5 + k + k/4 + j/4 + 5*j) % 7;
      // h: 0=Sat,1=Sun,2=Mon,...,6=Fri → Mon=2→0 ... Sun=1→6
      int dow = (h + 5) % 7; // 0=Mon ... 6=Sun
      todayDow = dow;
    }
  }

  for (int i = 0; i < kN; i++) {
    float cx = startX + i * cellW;
    float cy = startY;
    bool isToday = (i == todayDow);
    ImVec2 cMin(cx + 2.0f, cy);
    ImVec2 cMax(cx + cellW - 2.0f, cy + cellH);

    if (isToday)
      dl->AddRectFilled(cMin, cMax, IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),40), 5.0f);

    // Day abbreviation: fixed near top
    float dayW = ImGui::CalcTextSize(kWeek[i].abbr).x;
    float dayX = cx + (cellW - dayW) * 0.5f;
    ImU32 dayCol = isToday ? IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),255)
                           : IM_COL32(255,255,255,160);
    const float kDayTopOff = 5.0f;
    dl->AddText(ImGui::GetFont(), lh, ImVec2(dayX, cy + kDayTopOff), dayCol, kWeek[i].abbr);

    // Focus text: vertically centered in the remaining space below the day abbreviation
    float focScale = 0.82f;
    float remainY  = cy + kDayTopOff + lh;   // top of remaining space
    float remainH  = cellH - kDayTopOff - lh; // height of remaining space
    float focH     = lh * focScale;
    float focTopY  = remainY + (remainH - focH) * 0.5f;
    if (focTopY < remainY) focTopY = remainY;
    float focW = ImGui::CalcTextSize(kWeek[i].focus).x;
    float scale = focScale;
    if (focW * scale > cellW - 4.0f) scale = (cellW - 4.0f) / focW;
    float focX = cx + (cellW - focW * scale) * 0.5f;
    dl->AddText(ImGui::GetFont(), lh * scale, ImVec2(focX, focTopY),
                IM_COL32(255,255,255, isToday ? 210 : 100), kWeek[i].focus);

    if (isToday)
      dl->AddRect(cMin, cMax, IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),160), 5.0f, 0, 1.0f);
  }
  // Advance cursor past the drawn region so EndModernCard clips correctly
  ImGui::Dummy(ImVec2(avW, cellH + 2.0f));
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();
}

static void DrawBoardObjectivesCard(ImVec2 sz) {
  BeginModernCard("##boardobj_ph", sz);

  static const struct { const char *text; ImU32 dotCol; } kObjs[] = {
    { "Avoid relegation",        IM_COL32( 34,197, 94,220) }, // green  – achieved
    { "Reach knockout rounds",   IM_COL32(248,158, 11,220) }, // amber  – in progress
    { "Develop 2 youth players", IM_COL32( 90, 95,110,220) }, // dim    – not started
  };
  ImDrawList *dl = ImGui::GetWindowDrawList();
  PushMgrFont(g_ManagerFontSmall);
  float lh = ImGui::GetTextLineHeight();
  float cr = 4.0f;

  ImGui::Dummy(ImVec2(0, 4.0f));
  for (const auto &o : kObjs) {
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8.0f);
    ImVec2 p = ImGui::GetCursorScreenPos();
    dl->AddCircleFilled(ImVec2(p.x + cr, p.y + lh * 0.5f), cr, o.dotCol);
    ImGui::Dummy(ImVec2(cr * 2.0f + 8.0f, lh));
    ImGui::SameLine(0, 0);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
    ImGui::TextUnformatted(o.text);
    ImGui::PopStyleColor();
    ImGui::Spacing();
  }
  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();
}

static void DrawMedicalCentreCard(ImVec2 sz) {
  BeginModernCard("##medical_live", sz);

  struct MedRow {
    std::string name;
    std::string meta;
    int severity = 0; // 2 injured, 1 risk
    int condition = 100;
  };
  std::vector<MedRow> injured;
  std::vector<MedRow> risk;

  for (const auto &p : g_CareerHub.players) {
    std::string name = DisplayName(p);
    if (name.empty()) name = "Unknown player";
    if (p.injuryDays > 0) {
      MedRow row;
      row.name = name;
      row.severity = 2;
      row.condition = p.currentStamina;
      row.meta = (p.injuryType.empty() ? "Injured" : p.injuryType) + " - " + int_to_str(p.injuryDays) + "d";
      injured.push_back(row);
    } else if (p.currentStamina > 0 && p.currentStamina <= 68) {
      MedRow row;
      row.name = name;
      row.severity = 1;
      row.condition = p.currentStamina;
      row.meta = int_to_str(p.currentStamina) + "% condition";
      risk.push_back(row);
    }
  }

  std::sort(injured.begin(), injured.end(), [](const MedRow &a, const MedRow &b) {
    return a.meta < b.meta;
  });
  std::sort(risk.begin(), risk.end(), [](const MedRow &a, const MedRow &b) {
    return a.condition < b.condition;
  });

  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 origin = ImGui::GetCursorScreenPos();
  float avW = ImGui::GetContentRegionAvail().x;
  const float padX = 8.0f;
  float y = origin.y + 2.0f;

  auto DrawHeader = [&](const char *label, int count, ImU32 col) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%s %d", label, count);
    dl->AddCircleFilled(ImVec2(origin.x + padX + 5.0f, y + 8.0f), 5.0f, col, 16);
    dl->AddText(g_ManagerFontBold ? g_ManagerFontBold : ImGui::GetFont(), 13.0f,
                ImVec2(origin.x + padX + 18.0f, y),
                IM_COL32(220, 232, 255, 235), buf);
    y += 20.0f;
  };

  auto DrawRow = [&](const MedRow &row) {
    ImU32 col = row.severity == 2 ? IM_COL32(230, 76, 86, 235)
                                  : IM_COL32(235, 170, 55, 235);
    ImVec2 r0(origin.x + padX, y);
    ImVec2 r1(origin.x + avW - padX, y + 24.0f);
    dl->AddRectFilled(r0, r1, IM_COL32(14, 23, 48, 205), 5.0f);
    dl->AddRectFilled(r0, ImVec2(r0.x + 3.0f, r1.y), col, 2.0f);
    dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(r0.x + 9.0f, r0.y + 2.0f),
                IM_COL32(232, 238, 252, 238), row.name.c_str());
    ImVec2 msz = g_ManagerFontSmall
      ? g_ManagerFontSmall->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, row.meta.c_str())
      : ImGui::CalcTextSize(row.meta.c_str());
    dl->AddText(g_ManagerFontSmall, 12.0f,
                ImVec2(r1.x - msz.x - 8.0f, r0.y + 4.0f),
                row.severity == 2 ? IM_COL32(255, 145, 150, 230)
                                  : IM_COL32(255, 210, 120, 230),
                row.meta.c_str());
    y += 28.0f;
  };

  DrawHeader("Injured", (int)injured.size(), IM_COL32(230, 76, 86, 235));
  if (injured.empty()) {
    dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(origin.x + padX + 2.0f, y),
                IM_COL32(130, 148, 184, 220), "No players injured");
    y += 22.0f;
  } else {
    int limit = std::min(2, (int)injured.size());
    for (int i = 0; i < limit; i++) DrawRow(injured[i]);
  }

  y += 3.0f;
  dl->AddLine(ImVec2(origin.x + padX, y), ImVec2(origin.x + avW - padX, y),
              IM_COL32(255, 255, 255, 18), 1.0f);
  y += 8.0f;

  DrawHeader("At risk", (int)risk.size(), IM_COL32(235, 170, 55, 235));
  if (risk.empty()) {
    dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(origin.x + padX + 2.0f, y),
                IM_COL32(130, 148, 184, 220), "No high-risk players");
    y += 22.0f;
  } else {
    int limit = std::min(2, (int)risk.size());
    for (int i = 0; i < limit; i++) DrawRow(risk[i]);
  }

  ImGui::Dummy(ImVec2(avW, std::max(0.0f, y - origin.y)));
  EndModernCard();
}

static void DrawTacticsOverviewCard(ImVec2 sz) {
  BeginModernCard("##tactics_ph", sz);

  ImDrawList *dl = ImGui::GetWindowDrawList();
  float avW = ImGui::GetContentRegionAvail().x;
  float avH = ImGui::GetContentRegionAvail().y - 2.0f;

  // Split: left 30% pitch, right 70% tactic list
  const float kSplit = 0.30f;
  float pitchColW = avW * kSplit - 4.0f;
  float listColW  = avW - pitchColW - 8.0f;

  ImVec2 origin = ImGui::GetCursorScreenPos();

  // ---- Left: vertical mini pitch (with top/bottom margin) ----
  const float kPitchMargin = 8.0f;
  const float kAspect = 0.62f;
  float ptW = pitchColW;
  float ptH = ptW / kAspect;
  float maxPtH = avH - kPitchMargin * 2.0f;
  if (ptH > maxPtH) { ptH = maxPtH; ptW = ptH * kAspect; }
  float pitchOffX = (pitchColW - ptW) * 0.5f;
  ImVec2 ptMin(origin.x + pitchOffX, origin.y + kPitchMargin);
  ImVec2 ptMax(ptMin.x + ptW, ptMin.y + ptH);

  dl->AddRectFilled(ptMin, ptMax, IM_COL32(30, 90, 45, 220), 6.0f);
  dl->AddRect(ptMin, ptMax, IM_COL32(255,255,255,25), 6.0f, 0, 1.0f);
  float mx = (ptMin.x + ptMax.x) * 0.5f;
  float my = (ptMin.y + ptMax.y) * 0.5f;
  dl->AddLine(ImVec2(ptMin.x+4, my), ImVec2(ptMax.x-4, my), IM_COL32(255,255,255,30), 1.0f);
  dl->AddCircle(ImVec2(mx, my), ptW * 0.20f, IM_COL32(255,255,255,25), 32, 1.0f);
  float bw = ptW * 0.55f, bh = ptH * 0.14f;
  float bx = ptMin.x + (ptW - bw) * 0.5f;
  dl->AddRect(ImVec2(bx, ptMin.y),     ImVec2(bx+bw, ptMin.y+bh),  IM_COL32(255,255,255,22), 0.0f, 0, 1.0f);
  dl->AddRect(ImVec2(bx, ptMax.y-bh),  ImVec2(bx+bw, ptMax.y),     IM_COL32(255,255,255,22), 0.0f, 0, 1.0f);

  auto roleCol = [](int fo) -> ImU32 {
    if (fo == 0)  return IM_COL32(220,160, 30,210);
    if (fo <= 4)  return IM_COL32( 80,150,255,210);
    if (fo <= 7)  return IM_COL32( 80,210,110,210);
    return              IM_COL32(255, 90, 70,210);
  };
  static const float kNX[] = { 0.50f, 0.15f, 0.38f, 0.62f, 0.85f,
                                0.22f, 0.50f, 0.78f, 0.22f, 0.50f, 0.78f };
  static const float kNY[] = { 0.90f, 0.72f, 0.72f, 0.72f, 0.72f,
                                0.50f, 0.50f, 0.50f, 0.22f, 0.22f, 0.22f };
  float pw2 = ptMax.x - ptMin.x, ph2 = ptMax.y - ptMin.y;
  for (const auto &pl : g_CareerHub.players) {
    int fo = pl.formationOrder;
    if (fo < 0 || fo > 10) continue;
    float dx = ptMin.x + kNX[fo] * pw2;
    float dy = ptMin.y + kNY[fo] * ph2;
    dl->AddCircleFilled(ImVec2(dx, dy), 5.0f, roleCol(fo));
    dl->AddCircle(ImVec2(dx, dy), 5.0f, IM_COL32(0,0,0,140), 12, 1.0f);
  }

  // ---- Right: read-only tactic instructions — 2-column layout ----
  // Shift left slightly: reduce gap between pitch and list from 8 to 2
  ImVec2 listPos(origin.x + pitchColW + 2.0f, origin.y);
  ImGui::PushClipRect(listPos, ImVec2(listPos.x + listColW, listPos.y + avH), true);

  PushMgrFont(g_ManagerFontSmall);
  float lh   = ImGui::GetTextLineHeight();
  float rowH = lh + 4.0f;
  float catH = lh * 0.78f + 3.0f;

  // Pre-compute total block height for vertical centering
  // Attacking/Defending columns (same height): catH + 5*rowH
  // Gap between sections: 4px
  // On the Ball: catH + 1*rowH
  float colSectionH = catH + 5.0f * rowH;
  float totalBlockH = colSectionH + 4.0f + catH + rowH;
  float listStartY  = listPos.y + (avH - totalBlockH) * 0.5f;
  if (listStartY < listPos.y) listStartY = listPos.y;

  auto bestPreset = [](const TacInstruction &ins, float val) -> const char * {
    int best = 0; float bestD = 999.0f;
    for (int p = 0; p < 4; p++) {
      float d = fabsf(ins.presets[p].value - val);
      if (d < bestD) { bestD = d; best = p; }
    }
    return ins.presets[best].label;
  };

  const ImU32 kCatAtt = IM_COL32( 50,200, 90,255);
  const ImU32 kCatDef = IM_COL32( 80,160,255,255);
  const ImU32 kCatBal = IM_COL32(255,190, 60,255);
  const ImU32 kDim    = IM_COL32(170,170,185,200);
  const ImU32 kAcc    = IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),220);

  // Separate tactics into three buckets
  std::vector<const TacInstruction *> att, def, bal;
  for (int ti = 0; ti < kNumTacInstructions; ti++) {
    const TacInstruction &ins = kTacInstructions[ti];
    if      (strcmp(ins.category, "Attacking")   == 0) att.push_back(&ins);
    else if (strcmp(ins.category, "Defending")   == 0) def.push_back(&ins);
    else                                               bal.push_back(&ins);
  }

  // Draw one column of tactics starting at (cx, cy), width colW
  auto drawCol = [&](float cx, float cy, ImU32 catColor, const char *catName,
                     const std::vector<const TacInstruction *> &rows, float colW) {
    float dotR = 2.5f;
    dl->AddCircleFilled(ImVec2(cx + dotR, cy + lh * 0.78f * 0.5f), dotR, catColor);
    dl->AddText(ImGui::GetFont(), lh * 0.78f,
                ImVec2(cx + dotR * 2.0f + 3.0f, cy), catColor, catName);
    cy += catH;
    for (const TacInstruction *ins : rows) {
      float val = 0.5f;
      auto it = g_CareerHub.tactics.find(ins->key);
      if (it != g_CareerHub.tactics.end()) val = it->second;
      const char *pl = bestPreset(*ins, val);
      // Name left-aligned, value right-aligned within the column
      dl->AddText(ImGui::GetFont(), lh, ImVec2(cx + 2.0f, cy), kDim, ins->name);
      float plW = ImGui::CalcTextSize(pl).x;
      float plX = cx + colW - plW - 2.0f;
      if (plX < cx + 2.0f) plX = cx + 2.0f;
      dl->AddText(ImGui::GetFont(), lh, ImVec2(plX, cy), kAcc, pl);
      cy += rowH;
    }
    return cy;
  };

  // Center the content block horizontally within listColW
  const float kListMargin = 3.0f;
  float halfW  = (listColW - 2.0f * kListMargin - 8.0f) * 0.5f;
  float colAX  = listPos.x + kListMargin;
  float colDX  = colAX + halfW + 8.0f;

  float bottomAtt = drawCol(colAX, listStartY, kCatAtt, "Attacking", att, halfW);
  float bottomDef = drawCol(colDX, listStartY, kCatDef, "Defending", def, halfW);
  float bottomRow = std::max(bottomAtt, bottomDef) + 4.0f;

  // On the Ball — shared header, then one item per column
  {
    float dotR = 2.5f;
    dl->AddCircleFilled(ImVec2(colAX + dotR, bottomRow + lh * 0.78f * 0.5f), dotR, kCatBal);
    dl->AddText(ImGui::GetFont(), lh * 0.78f,
                ImVec2(colAX + dotR * 2.0f + 3.0f, bottomRow), kCatBal, "On the Ball");
    bottomRow += catH;
    // Dribble Rate → left column, Dribble Direction → right column
    for (int i = 0; i < (int)bal.size(); i++) {
      const TacInstruction *ins = bal[i];
      float val = 0.5f;
      auto it = g_CareerHub.tactics.find(ins->key);
      if (it != g_CareerHub.tactics.end()) val = it->second;
      const char *pl = bestPreset(*ins, val);
      float cx = (i == 0) ? colAX : colDX;
      dl->AddText(ImGui::GetFont(), lh, ImVec2(cx + 2.0f, bottomRow), kDim, ins->name);
      float plW = ImGui::CalcTextSize(pl).x;
      float plX = cx + halfW - plW - 2.0f;
      if (plX < cx + 2.0f) plX = cx + 2.0f;
      dl->AddText(ImGui::GetFont(), lh, ImVec2(plX, bottomRow), kAcc, pl);
    }
  }

  PopMgrFont(g_ManagerFontSmall);
  ImGui::PopClipRect();

  // Advance cursor
  float totalH = (bottomRow - listPos.y) + rowH;
  ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y + std::max(ptH, totalH)));
  ImGui::Dummy(ImVec2(avW, 1.0f));

  EndModernCard();
}

static GLuint s_DefaultFaceTex     = 0;
static bool   s_DefaultFaceTexTried = false;
static GLuint GetDefaultFaceTex() {
  if (s_DefaultFaceTexTried) return s_DefaultFaceTex;
  s_DefaultFaceTexTried = true;
  SDL_Surface *surf = IMG_Load("media/textures/faces/Men Default faces /Zet /male.png");
  if (!surf) return 0;
  SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(surf);
  if (!rgba) return 0;
  glGenTextures(1, &s_DefaultFaceTex);
  glBindTexture(GL_TEXTURE_2D, s_DefaultFaceTex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
  SDL_FreeSurface(rgba);
  glBindTexture(GL_TEXTURE_2D, 0);
  return s_DefaultFaceTex;
}

static std::string FmtMoney(long long v); // forward declaration

static void DrawTransfersCard(ImVec2 sz) {
  BeginModernCard("##transfers_card", sz);
  ImDrawList *dl = ImGui::GetWindowDrawList();
  int mid = g_CareerHub.managerId;

  // Window open/closed status
  bool windowOpen = InTransferWindow(g_CareerHub.currentDate);
  int daysLeft    = DaysToWindowEnd(g_CareerHub.currentDate);

  // Active bids placed by user
  int activeBids = 0;
  { std::stringstream q; q << "SELECT COUNT(*) FROM transfer_negotiations"
      << " WHERE manager_id=" << mid << " AND is_user_bid=1"
      << " AND state NOT IN ('completed','collapsed');";
    DatabaseResult *r = GetDB()->Query(q.str().c_str());
    if (r) { if (r->data.size()>0) activeBids=atoi(DBCell(r,0,0).c_str()); delete r; } }

  // Incoming bids on user's players
  int incomingBids = 0;
  { std::stringstream q; q << "SELECT COUNT(*) FROM transfer_negotiations tn"
      << " JOIN managers m ON m.id=" << mid
      << " WHERE tn.manager_id=" << mid << " AND tn.selling_club_id=m.club_id"
      << " AND tn.is_user_bid=0 AND tn.state NOT IN ('completed','collapsed');";
    DatabaseResult *r = GetDB()->Query(q.str().c_str());
    if (r) { if (r->data.size()>0) incomingBids=atoi(DBCell(r,0,0).c_str()); delete r; } }

  // Transfer budget
  long long budget = 0;
  { std::stringstream q; q << "SELECT transfer_budget FROM club_finances"
      << " WHERE manager_id=" << mid << " AND club_id=(SELECT club_id FROM managers WHERE id=" << mid << ");";
    DatabaseResult *r = GetDB()->Query(q.str().c_str());
    if (r) { if (r->data.size()>0) budget=atoll(DBCell(r,0,0).c_str()); delete r; } }

  ImVec2 origin = ImGui::GetCursorScreenPos();
  float avW = ImGui::GetContentRegionAvail().x;
  float avH = sz.y - 24.0f;
  const float padX = 10.0f;
  const float cardW = avW - padX * 2.0f;
  float y = origin.y + 2.0f;

  ImU32 statusCol = windowOpen ? IM_COL32(45, 205, 115, 245)
                               : IM_COL32(220, 92, 92, 235);
  std::string statusText = windowOpen
    ? ("Window open - " + std::to_string(std::max(0, daysLeft)) + " days left")
    : "Transfer window closed";

  ImVec2 banner0(origin.x + padX, y);
  ImVec2 banner1(origin.x + padX + cardW, y + 34.0f);
  dl->AddRectFilled(banner0, banner1, IM_COL32(14, 24, 50, 230), 7.0f);
  dl->AddRect(banner0, banner1, IM_COL32(64, 86, 140, 120), 7.0f);
  dl->AddCircleFilled(ImVec2(banner0.x + 17.0f, banner0.y + 17.0f), 5.0f, statusCol, 16);
  dl->AddText(g_ManagerFontBold ? g_ManagerFontBold : ImGui::GetFont(), 14.0f,
              ImVec2(banner0.x + 30.0f, banner0.y + 8.0f),
              IM_COL32(226, 236, 255, 245), statusText.c_str());
  y += 42.0f;

  auto DrawMetric = [&](float x, float w, const char *label, const std::string &value,
                        ImU32 valueCol) {
    ImVec2 r0(x, y);
    ImVec2 r1(x + w, y + 44.0f);
    dl->AddRectFilled(r0, r1, IM_COL32(12, 20, 43, 220), 7.0f);
    dl->AddRect(r0, r1, IM_COL32(50, 70, 118, 110), 7.0f);
    dl->AddText(g_ManagerFontSmall, 12.0f, ImVec2(r0.x + 10.0f, r0.y + 7.0f),
                IM_COL32(130, 148, 184, 230), label);
    ImVec2 vs = g_ManagerFontBold
      ? g_ManagerFontBold->CalcTextSizeA(18.0f, FLT_MAX, 0.0f, value.c_str())
      : ImGui::CalcTextSize(value.c_str());
    dl->AddText(g_ManagerFontBold ? g_ManagerFontBold : ImGui::GetFont(), 18.0f,
                ImVec2(r1.x - vs.x - 10.0f, r0.y + 18.0f),
                valueCol, value.c_str());
  };

  float half = (cardW - 8.0f) * 0.5f;
  DrawMetric(origin.x + padX, half, "My bids", int_to_str(activeBids),
             activeBids > 0 ? IM_COL32(130, 170, 255, 245) : IM_COL32(95, 112, 150, 230));
  DrawMetric(origin.x + padX + half + 8.0f, half, "Incoming", int_to_str(incomingBids),
             incomingBids > 0 ? IM_COL32(255, 203, 85, 245) : IM_COL32(95, 112, 150, 230));
  y += 54.0f;

  ImVec2 budget0(origin.x + padX, y);
  ImVec2 budget1(origin.x + padX + cardW, y + 30.0f);
  dl->AddRectFilled(budget0, budget1, IM_COL32(10, 18, 38, 210), 6.0f);
  dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(budget0.x + 10.0f, budget0.y + 7.0f),
              IM_COL32(146, 164, 200, 230), "Budget");
  std::string bstr = FmtMoney(budget);
  ImVec2 bsz = g_ManagerFontBold
    ? g_ManagerFontBold->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, bstr.c_str())
    : ImGui::CalcTextSize(bstr.c_str());
  dl->AddText(g_ManagerFontBold ? g_ManagerFontBold : ImGui::GetFont(), 15.0f,
              ImVec2(budget1.x - bsz.x - 10.0f, budget0.y + 6.0f),
              IM_COL32(220, 230, 248, 240), bstr.c_str());

  ImGui::Dummy(ImVec2(avW, std::max(avH, budget1.y - origin.y)));
  EndModernCard();
}

static const char *PosLabel(int fo); // forward declaration — defined with Squad page below

static void DrawSquadSnapshotCard(ImVec2 sz) {
  BeginModernCard("##squad_snap", sz);
  // Capture inner origin right after BeginModernCard so we can anchor the
  // full-width notice bar to the card edges, not to the padded cursor.
  ImVec2 cardInner = ImGui::GetCursorScreenPos(); // (card_left+14, card_top+12)

  GLuint faceTex = GetDefaultFaceTex();

  // Only formationOrder 0-19: 0-10 = Starting XI, 11-19 = Subs
  std::vector<const CareerHubState::Player *> xi, subs;
  for (const auto &p : g_CareerHub.players) {
    if      (p.formationOrder >= 0  && p.formationOrder <= 10) xi.push_back(&p);
    else if (p.formationOrder >= 11 && p.formationOrder <= 19) subs.push_back(&p);
  }
  std::sort(xi.begin(),   xi.end(),   [](const CareerHubState::Player *a, const CareerHubState::Player *b){ return a->formationOrder < b->formationOrder; });
  std::sort(subs.begin(), subs.end(), [](const CareerHubState::Player *a, const CareerHubState::Player *b){ return a->formationOrder < b->formationOrder; });

  PushMgrFont(g_ManagerFontSmall);
  float lh = ImGui::GetTextLineHeight();
  PopMgrFont(g_ManagerFontSmall);

  // Compact row height to fit 20 rows: face + 1px top/bottom padding
  const float kFaceH  = lh + 2.0f;   // tiny face thumbnail
  const float kRowH   = kFaceH + 3.0f;
  const float kHdrH   = lh + 4.0f;   // column header row

  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(3.0f, 1.0f));
  if (ImGui::BeginTable("##sq_snap", 7,
                        ImGuiTableFlags_RowBg | ImGuiTableFlags_PadOuterX,
                        ImVec2(0, 0))) {
    ImGui::TableSetupColumn("POS",  ImGuiTableColumnFlags_WidthFixed,   28.0f);
    ImGui::TableSetupColumn("##fc", ImGuiTableColumnFlags_WidthFixed,   kFaceH);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn("MP",   ImGuiTableColumnFlags_WidthFixed,   22.0f);
    ImGui::TableSetupColumn("G",    ImGuiTableColumnFlags_WidthFixed,   18.0f);
    ImGui::TableSetupColumn("A",    ImGuiTableColumnFlags_WidthFixed,   18.0f);
    ImGui::TableSetupColumn("R",    ImGuiTableColumnFlags_WidthFixed,   32.0f);

    // Header row
    ImGui::TableNextRow(0, kHdrH);
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    for (int c = 0; c < 7; c++) {
      ImGui::TableSetColumnIndex(c);
      static const char *kHdrs[] = {"POS","","Player","MP","G","A","R"};
      ImGui::TextUnformatted(kHdrs[c]);
    }
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);

    // Helper to draw one player row
    auto drawRow = [&](const CareerHubState::Player *p, bool isXI) {
      ImGui::TableNextRow(0, kRowH);

      // POS badge
      ImGui::TableSetColumnIndex(0);
      {
        PushMgrFont(g_ManagerFontSmall);
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImU32 badgeBg;
        int fo = p->formationOrder;
        if      (!isXI)   badgeBg = IM_COL32(60, 65, 85,200);  // subs — neutral
        else if (fo == 0) badgeBg = IM_COL32(220,160, 30,200); // GK gold
        else if (fo <= 4) badgeBg = IM_COL32( 80,150,255,200); // DEF blue
        else if (fo <= 7) badgeBg = IM_COL32( 80,210,110,200); // MID green
        else              badgeBg = IM_COL32(255, 90, 70,200); // FWD red
        const char *posStr = PosLabel(fo);
        float tw = ImGui::CalcTextSize(posStr).x;
        float bw = std::max(tw + 6.0f, 24.0f);
        float bh = lh + 2.0f;
        dl->AddRectFilled(ImVec2(pos.x, pos.y), ImVec2(pos.x+bw, pos.y+bh), badgeBg, 3.0f);
        dl->AddText(ImGui::GetFont(), lh, ImVec2(pos.x+(bw-tw)*0.5f, pos.y+1.0f),
                    IM_COL32(255,255,255,230), posStr);
        ImGui::Dummy(ImVec2(bw, bh));
        PopMgrFont(g_ManagerFontSmall);
      }

      // Face
      ImGui::TableSetColumnIndex(1);
      if (faceTex) {
        ImGui::Image((ImTextureID)(intptr_t)faceTex, ImVec2(kFaceH, kFaceH));
      } else {
        ImGui::Dummy(ImVec2(kFaceH, kFaceH));
      }

      // Name (nickname if available)
      ImGui::TableSetColumnIndex(2);
      PushMgrFont(g_ManagerFontSmall);
      std::string full = DisplayName(*p);
      ImGui::PushStyleColor(ImGuiCol_Text, isXI ? kTextPri : kTextSec);
      ImGui::TextUnformatted(full.c_str());
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);

      // MP / G / A / R — all placeholder dashes
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted("-");
      ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted("-");
      ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted("-");
      ImGui::PopStyleColor();
      // Rating badge
      ImGui::TableSetColumnIndex(6);
      {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 rp = ImGui::GetCursorScreenPos();
        const float rw = 28.0f, rh = lh + 1.0f;
        dl->AddRectFilled(rp, ImVec2(rp.x+rw, rp.y+rh),
                          IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),50), 3.0f);
        dl->AddText(ImGui::GetFont(), lh,
                    ImVec2(rp.x + (rw - ImGui::CalcTextSize("-").x)*0.5f, rp.y),
                    IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),220), "-");
        ImGui::Dummy(ImVec2(rw, rh));
      }
      PopMgrFont(g_ManagerFontSmall);
    };

    for (const auto *p : xi)   drawRow(p, true);
    for (const auto *p : subs) drawRow(p, false);

    ImGui::EndTable();
  }
  ImGui::PopStyleVar();

  // Incomplete squad notice — shown whenever fewer than 20 players assigned
  int activeCount = (int)(xi.size() + subs.size());
  if (activeCount < 20) {
    ImGui::Dummy(ImVec2(0, 4.0f));
    float curY = ImGui::GetCursorScreenPos().y;
    // Anchor to card edges by undoing WindowPadding (14px left)
    ImVec2 np  = ImVec2(cardInner.x - 14.0f, curY);
    float  nw  = sz.x; // full card width
    const float kNH = 32.0f;
    ImDrawList *ndl = ImGui::GetWindowDrawList();
    ndl->AddRectFilled(np, ImVec2(np.x + nw, np.y + kNH),
                       IM_COL32(130, 30, 30, 180), 4.0f);
    ndl->AddRectFilled(np, ImVec2(np.x + 3.0f, np.y + kNH),
                       IM_COL32(240, 70, 70, 255), 4.0f);
    char noticeMsg[80];
    snprintf(noticeMsg, sizeof(noticeMsg),
             "Select 20 players for active squad (%d/20 selected)", activeCount);
    PushMgrFont(g_ManagerFontSmall);
    const float kNFs = 16.0f;
    ImVec2 tsz = g_ManagerFontSmall
        ? g_ManagerFontSmall->CalcTextSizeA(kNFs, FLT_MAX, 0, noticeMsg)
        : ImGui::CalcTextSize(noticeMsg);
    float tx = np.x + (nw - tsz.x) * 0.5f;
    ndl->AddText(g_ManagerFontSmall, kNFs,
                 ImVec2(tx, np.y + (kNH - kNFs) * 0.5f),
                 IM_COL32(255, 190, 190, 240), noticeMsg);
    PopMgrFont(g_ManagerFontSmall);
    ImGui::Dummy(ImVec2(nw, kNH));
  }

  EndModernCard();
}

// Forward declarations for finance helpers used in DrawHomePage
static std::string FmtMoney(long long v);

// ---- DrawHomePage -------------------------------------------------------
// Layout: Left 28% (Messages + Training + Board Objectives) | Center 42% (Story + Fixture + Agenda + Tactics) | Right 30% (Schedule + Snapshot + Medical)

static void DrawHomePage(float w, float h) {
  const bool kCanClick = !s_escMenuOpen && !s_advanceModePopupWasOpen;
  const float kPad = 16.0f, kGap = 10.0f;

  // Shared hover-glow helper — draws club-accent ring and sets hand cursor.
  // Call AFTER drawing each card. Returns true if hovered (for click handling).
  const int kGlowR = (int)(kAccent.x*255), kGlowG = (int)(kAccent.y*255), kGlowB = (int)(kAccent.z*255);
  auto HoverGlow = [&](ImVec2 p, float cw, float ch) -> bool {
    if (!kCanClick) return false;
    if (!ImGui::IsMouseHoveringRect(p, ImVec2(p.x+cw, p.y+ch), false)) return false;
    ImGui::GetWindowDrawList()->AddRect(p, ImVec2(p.x+cw, p.y+ch),
      IM_COL32(kGlowR, kGlowG, kGlowB, 58), 10.0f, 0, 1.6f);
    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    return true;
  };
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

  const float kTrainH  = 5.0f * msgFontH + 6.0f;
  const float kBoardH  = 3.0f * (msgFontH + 10.0f) + 36.0f;
  const float kMedH    = 176.0f;
  const float kTransH  = 2.0f * (msgFontH * 2.4f + 12.0f) + 58.0f;
  const float kTacH    = 220.0f;
  // Squad snapshot: header + 20 compact rows. rowH = (msgFontH+2)+3 = msgFontH+5
  const float kSquadH  = (msgFontH + 4.0f) + 20.0f * (msgFontH + 5.0f) + 24.0f;

  // Left: Messages + Training Schedule + Board Objectives + Squad Snapshot
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_l", ImVec2(leftW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  { ImVec2 p = ImGui::GetCursorScreenPos();
    DrawMessagesCard(ImVec2(leftW, kMsgH));
    if (HoverGlow(p, leftW, kMsgH) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      NavPush(PAGE_INBOX);
  }
  ImGui::Dummy(ImVec2(0, kGap));
  { ImVec2 p = ImGui::GetCursorScreenPos();
    DrawTrainingScheduleCard(ImVec2(leftW, kTrainH));
    if (HoverGlow(p, leftW, kTrainH) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      NavPush(PAGE_TRAINING);
  }
  ImGui::Dummy(ImVec2(0, kGap));
  DrawBoardObjectivesCard(ImVec2(leftW, kBoardH));
  ImGui::Dummy(ImVec2(0, kGap));
  { ImVec2 p = ImGui::GetCursorScreenPos();
    DrawSquadSnapshotCard(ImVec2(leftW, kSquadH));
    if (HoverGlow(p, leftW, kSquadH) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      NavPush(PAGE_SQUAD);
  }
  ImGui::EndChild();

  ImGui::SameLine(0, kGap);

  // Center: Story + NextFixture + Upcoming
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_c", ImVec2(centerW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  { ImVec2 p = ImGui::GetCursorScreenPos();
    DrawTopStoryCard(ImVec2(centerW, kStoryH));
    if (HoverGlow(p, centerW, kStoryH) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      NavPush(PAGE_NEWS);
  }
  ImGui::Dummy(ImVec2(0, kGap));
  { ImVec2 p = ImGui::GetCursorScreenPos();
    DrawNextFixtureCard(ImVec2(centerW, kFixtH));
    HoverGlow(p, centerW, kFixtH);
  }
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
  ImGui::Dummy(ImVec2(0, kGap));
  { ImVec2 p = ImGui::GetCursorScreenPos();
    DrawTacticsOverviewCard(ImVec2(centerW, kTacH));
    if (HoverGlow(p, centerW, kTacH) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      NavPush(PAGE_TACTICS);
  }
  ImGui::Dummy(ImVec2(0, kGap));
  // ---- Finance + Scouting quick-view panels (side by side, pure DrawList) -
  {
    const float kPH   = 138.0f;  // panel height (extra room for 15px font)
    const float halfC = (centerW - kGap) * 0.5f;
    const float kPX   = 15.0f;
    const float kRad  = 10.0f;
    const ImU32 kLbl  = IM_COL32(215,228,250,225); // white label text
    const ImU32 kStripe = IM_COL32(kGlowR, kGlowG, kGlowB, 210); // club-color stripe
    ImDrawList *pdl   = ImGui::GetWindowDrawList();

    // ===== FINANCE CARD =====
    {
      ImVec2 fp = ImGui::GetCursorScreenPos();
      const float fw = halfC, fh = kPH;
      const auto &fi = g_CareerHub.finances;
      long long bal     = fi.balance;
      long long weekNet = fi.weeklyTV - fi.weeklyWages - fi.weeklyOperating;
      long long proj    = bal + weekNet * 38LL;

      ImU32 balCol  = bal     >= 0 ? IM_COL32(50,210,105,255) : IM_COL32(215,72,72,255);
      ImU32 flowCol = weekNet >= 0 ? IM_COL32(50,210,105,255) : IM_COL32(215,72,72,255);
      ImU32 projCol = proj    >= 0 ? IM_COL32(50,210,105,255) : IM_COL32(215,72,72,255);

      // Card shell
      pdl->AddRectFilled(fp, ImVec2(fp.x+fw, fp.y+fh), C32(kBgCard), kRad);
      pdl->AddRect      (fp, ImVec2(fp.x+fw, fp.y+fh), C32(kBorder), kRad, 0, 1.0f);
      pdl->AddLine(ImVec2(fp.x+kRad, fp.y+1), ImVec2(fp.x+fw-kRad, fp.y+1),
                   IM_COL32(255,255,255,8), 1.0f);
      // Club-color left stripe
      pdl->AddRectFilled(ImVec2(fp.x,     fp.y+kRad),
                         ImVec2(fp.x+3.5f, fp.y+fh-kRad), kStripe, 2.0f);

      // "Balance" label
      float lY = fp.y + 14.0f;
      if (g_ManagerFontSmall)
        pdl->AddText(g_ManagerFontSmall, 15.0f, ImVec2(fp.x+kPX, lY), kLbl, "Balance");

      // Balance value — large, right-aligned
      std::string balStr = FmtMoney(bal);
      if (g_ManagerFontBold) {
        ImVec2 bvSz = g_ManagerFontBold->CalcTextSizeA(22.0f, FLT_MAX, 0.0f, balStr.c_str());
        pdl->AddText(g_ManagerFontBold, 22.0f,
                     ImVec2(fp.x + fw - bvSz.x - kPX, lY - 4.0f),
                     balCol, balStr.c_str());
      }

      // Divider
      float sepY = fp.y + 52.0f;
      pdl->AddLine(ImVec2(fp.x+kPX, sepY), ImVec2(fp.x+fw-kPX, sepY),
                   IM_COL32(255,255,255,12), 1.0f);

      // Weekly Cashflow row
      float r1Y = sepY + 11.0f;
      std::string flowStr = (weekNet >= 0 ? "+" : "") + FmtMoney(weekNet) + "/wk";
      if (g_ManagerFontSmall) {
        pdl->AddText(g_ManagerFontSmall, 15.0f, ImVec2(fp.x+kPX, r1Y), kLbl, "Weekly Cashflow");
        ImVec2 fSz = g_ManagerFontSmall->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, flowStr.c_str());
        pdl->AddText(g_ManagerFontSmall, 15.0f,
                     ImVec2(fp.x+fw - fSz.x - kPX, r1Y), flowCol, flowStr.c_str());
      }

      // Season Projection row
      float r2Y = r1Y + 27.0f;
      std::string projStr = (proj >= 0 ? "+" : "") + FmtMoney(proj);
      if (g_ManagerFontSmall) {
        pdl->AddText(g_ManagerFontSmall, 15.0f, ImVec2(fp.x+kPX, r2Y), kLbl, "Season Projection");
        ImVec2 pSz = g_ManagerFontSmall->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, projStr.c_str());
        pdl->AddText(g_ManagerFontSmall, 15.0f,
                     ImVec2(fp.x+fw - pSz.x - kPX, r2Y), projCol, projStr.c_str());
      }

      if (HoverGlow(fp, fw, fh) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        NavPush(PAGE_FINANCES);
      ImGui::Dummy(ImVec2(fw, fh));
    }

    ImGui::SameLine(0, kGap);

    // ===== SCOUTING CARD =====
    {
      ImVec2 sp = ImGui::GetCursorScreenPos();
      const float sw = halfC, sh = kPH;
      int queueCnt  = (int)g_CareerHub.scoutQueue.size();
      int reportCnt = (int)g_CareerHub.scoutReports.size();

      // Card shell
      pdl->AddRectFilled(sp, ImVec2(sp.x+sw, sp.y+sh), C32(kBgCard), kRad);
      pdl->AddRect      (sp, ImVec2(sp.x+sw, sp.y+sh), C32(kBorder), kRad, 0, 1.0f);
      pdl->AddLine(ImVec2(sp.x+kRad, sp.y+1), ImVec2(sp.x+sw-kRad, sp.y+1),
                   IM_COL32(255,255,255,8), 1.0f);
      // Club-color left stripe
      pdl->AddRectFilled(ImVec2(sp.x,     sp.y+kRad),
                         ImVec2(sp.x+3.5f, sp.y+sh-kRad), kStripe, 2.0f);

      // Two large stat blocks
      float b1X   = sp.x + kPX + 6.0f;
      float b2X   = sp.x + sw * 0.5f + 10.0f;
      float statY = sp.y + 13.0f;

      // Awaiting count
      {
        char ab[8]; snprintf(ab, sizeof(ab), "%d", queueCnt);
        ImU32 nC = (queueCnt > 0) ? IM_COL32(225,178,42,255) : IM_COL32(60,75,128,230);
        if (g_ManagerFontBold)
          pdl->AddText(g_ManagerFontBold, 22.0f, ImVec2(b1X, statY), nC, ab);
        if (g_ManagerFontSmall)
          pdl->AddText(g_ManagerFontSmall, 15.0f, ImVec2(b1X, statY + 27.0f), kLbl, "Awaiting");
      }

      // Vertical divider between stat blocks
      float midX = sp.x + sw * 0.5f;
      pdl->AddLine(ImVec2(midX, sp.y + 12.0f), ImVec2(midX, sp.y + 12.0f + 48.0f),
                   IM_COL32(255,255,255,14), 1.0f);

      // Reports ready count
      {
        char rb[8]; snprintf(rb, sizeof(rb), "%d", reportCnt);
        ImU32 nC = (reportCnt > 0) ? IM_COL32(50,210,105,255) : IM_COL32(60,75,128,230);
        if (g_ManagerFontBold)
          pdl->AddText(g_ManagerFontBold, 22.0f, ImVec2(b2X, statY), nC, rb);
        if (g_ManagerFontSmall)
          pdl->AddText(g_ManagerFontSmall, 15.0f, ImVec2(b2X, statY + 27.0f), kLbl, "Reports Ready");
      }

      // Divider
      float sSepY = sp.y + 74.0f;
      pdl->AddLine(ImVec2(sp.x+kPX, sSepY), ImVec2(sp.x+sw-kPX, sSepY),
                   IM_COL32(255,255,255,12), 1.0f);

      // Bottom info row
      float btmY = sSepY + 11.0f;
      if (g_ManagerFontSmall) {
        if (!g_CareerHub.scoutQueue.empty()) {
          std::string soonest = g_CareerHub.scoutQueue[0].dueDate;
          for (const auto &sq : g_CareerHub.scoutQueue)
            if (!sq.dueDate.empty() && (soonest.empty() || sq.dueDate < soonest))
              soonest = sq.dueDate;
          if (!soonest.empty()) {
            std::string lbl = "Next report: " + FormatDateDisplay(soonest);
            pdl->AddText(g_ManagerFontSmall, 15.0f, ImVec2(sp.x+kPX, btmY), kLbl, lbl.c_str());
          }
        } else if (reportCnt > 0) {
          const auto &sr = g_CareerHub.scoutReports.back();
          std::string lbl = "Latest: " + sr.firstName + " " + sr.lastName;
          pdl->AddText(g_ManagerFontSmall, 15.0f, ImVec2(sp.x+kPX, btmY), kLbl, lbl.c_str());
        } else {
          pdl->AddText(g_ManagerFontSmall, 15.0f, ImVec2(sp.x+kPX, btmY),
                       IM_COL32(60,75,128,200), "No active scouting missions");
        }
      }

      if (HoverGlow(sp, sw, sh) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        NavPush(PAGE_SCOUTING);
      ImGui::Dummy(ImVec2(sw, sh));
    }
  }
  ImGui::EndChild();

  ImGui::SameLine(0, kGap);

  // Right: Fixture Schedule + League Snapshot + Medical Centre
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##ov_r", ImVec2(rightW, colH), false, ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();
  { ImVec2 p = ImGui::GetCursorScreenPos();
    DrawFixtureScheduleCard(ImVec2(rightW, schedH));
    if (HoverGlow(p, rightW, schedH) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      s_schedInit = false;
      s_schedClubInit = true;
      s_schedClub = -1;
      NavPush(PAGE_SCHEDULE);
    }
  }
  ImGui::Dummy(ImVec2(0, kGap));
  { ImVec2 p = ImGui::GetCursorScreenPos();
    DrawLeagueSnapshotCard(ImVec2(rightW, kSnapH));
    if (HoverGlow(p, rightW, kSnapH) && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
      NavPush(PAGE_COMPETITIONS);
  }
  ImGui::Dummy(ImVec2(0, kGap));
  DrawMedicalCentreCard(ImVec2(rightW, kMedH));
  ImGui::Dummy(ImVec2(0, kGap));
  DrawTransfersCard(ImVec2(rightW, kTransH));
  ImGui::EndChild();
}

// ---- Star rating renderer -----------------------------------------------

static void DrawStars(float value, float maxValue, ImU32 filledCol, float scale = 1.0f) {
  float stars = (maxValue > 0.0f) ? (value / maxValue) * 5.0f : 0.0f;
  stars = std::max(0.0f, std::min(5.0f, stars));
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 p = ImGui::GetCursorScreenPos();
  const float kW = 9.0f * scale, kH = 7.0f * scale, kGap = 2.0f * scale;
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

// ---- Player detail helpers -----------------------------------------------

// val = real DB attribute (0-99); hiddenIdx = unique index for scouting fog hash
struct StatDef { const char *label; int val; int hiddenIdx; };

static ImU32 StatValueColor(int v) {
  // v is 0-99 scale
  if (v >= 80) return IM_COL32( 80, 215, 105, 255); // green  — excellent
  if (v >= 65) return IM_COL32(155, 215,  80, 255); // lime   — good
  if (v >= 50) return IM_COL32(215, 195,  55, 255); // gold   — average
  if (v >= 35) return IM_COL32(215, 130,  45, 255); // orange — below avg
  return             IM_COL32(210,  60,  55, 255); // red    — poor
}

// ---- Search helpers -------------------------------------------------------

static ImU32 QualityColor(float baseStat) {
  if (baseStat >= 0.80f) return IM_COL32( 80, 215, 105, 255);
  if (baseStat >= 0.65f) return IM_COL32(155, 215,  80, 255);
  if (baseStat >= 0.50f) return IM_COL32(215, 195,  55, 255);
  if (baseStat >= 0.35f) return IM_COL32(215, 130,  45, 255);
  return                        IM_COL32(210,  60,  55, 255);
}

// Returns the display name: nickname if available, else firstName + " " + lastName
static std::string DisplayName(const CareerHubState::Player &p) {
  if (!p.nickname.empty()) return p.nickname;
  if (p.firstName.empty()) return p.lastName;
  if (p.lastName.empty())  return p.firstName;
  return p.firstName + " " + p.lastName;
}

// Format contract_expiry from DB (handles DD/MM/YYYY or YYYY-MM-DD) → "Jul 2028"
static std::string FormatContractExpiry(const std::string &exp) {
  if (exp.size() < 8) return exp.empty() ? "-" : exp;
  static const char *kMo[] = {"","Jan","Feb","Mar","Apr","May","Jun",
                               "Jul","Aug","Sep","Oct","Nov","Dec"};
  int dy = 0, mo = 0, yr = 0;
  if (exp.size() >= 10 && exp[2] == '/') {
    // DD/MM/YYYY
    dy = atoi(exp.substr(0,2).c_str());
    mo = atoi(exp.substr(3,2).c_str());
    yr = atoi(exp.substr(6,4).c_str());
  } else if (exp.size() >= 10 && exp[4] == '-') {
    // YYYY-MM-DD
    yr = atoi(exp.substr(0,4).c_str());
    mo = atoi(exp.substr(5,2).c_str());
    dy = atoi(exp.substr(8,2).c_str());
  } else {
    return exp;
  }
  (void)dy;
  char buf[16];
  if (mo >= 1 && mo <= 12)
    snprintf(buf, sizeof(buf), "%s %d", kMo[mo], yr);
  else
    snprintf(buf, sizeof(buf), "%d", yr);
  return buf;
}

// Parse alternative_pos from DB (handles JSON array ["RM","LW"] or comma-separated "RB,CM")
static std::vector<std::string> ParseAltPositions(const std::string &raw) {
  std::vector<std::string> result;
  if (raw.empty() || raw == "[]" || raw == "null" || raw == "NULL") return result;
  if (raw[0] == '[') {
    // JSON array: extract quoted tokens
    bool inQ = false;
    std::string tok;
    for (char c : raw) {
      if (c == '"') {
        inQ = !inQ;
        if (!inQ && !tok.empty()) { result.push_back(tok); tok.clear(); }
      } else if (inQ) {
        tok += c;
      }
    }
  } else {
    // Comma-separated
    std::stringstream ss(raw);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
      while (!tok.empty() && isspace((unsigned char)tok.front())) tok.erase(tok.begin());
      while (!tok.empty() && isspace((unsigned char)tok.back())) tok.pop_back();
      if (!tok.empty()) result.push_back(tok);
    }
  }
  return result;
}

// Parse all player attribute columns into a Player struct from a DB row.
// col0 is the offset of the first new column (nickname) in the row.
static void ParsePlayerAttrsFromRow(CareerHubState::Player &p,
                                    DatabaseResult *r, int row, int col0) {
  // col0+0: nickname
  p.nickname        = DBCell(r, row, col0+0);
  // col0+1: alternative_pos
  p.alternativePos  = DBCell(r, row, col0+1);
  // col0+2: skillMoves
  { std::string s = DBCell(r, row, col0+2); p.skillMoves = s.empty() ? 0 : atoi(s.c_str()); }
  // col0+3: weakFoot
  { std::string s = DBCell(r, row, col0+3); p.weakFoot = s.empty() ? 0 : atoi(s.c_str()); }
  // col0+4: nationality
  p.nationality     = DBCell(r, row, col0+4);
  // col0+5: weight
  { std::string s = DBCell(r, row, col0+5); p.weight = s.empty() ? 0.0f : (float)atof(s.c_str()); }
  // col0+6: playervalue
  { std::string s = DBCell(r, row, col0+6); p.playerValue = s.empty() ? 0 : atoi(s.c_str()); }
  // col0+7: jersey_number
  { std::string s = DBCell(r, row, col0+7); p.jerseyNumber = s.empty() ? 0 : atoi(s.c_str()); }
  // col0+8: international_reputation
  { std::string s = DBCell(r, row, col0+8); p.intlReputation = s.empty() ? 0 : atoi(s.c_str()); }
  // Outfield attributes col0+9 .. col0+36
  auto gi = [&](int off) -> int {
    std::string s = DBCell(r, row, col0+off); return s.empty() ? 0 : atoi(s.c_str());
  };
  p.atAcceleration      = gi(9);
  p.atSprintSpeed       = gi(10);
  p.atAgility           = gi(11);
  p.atBalance           = gi(12);
  p.atJumping           = gi(13);
  p.atStrength          = gi(14);
  p.atReactions         = gi(15);
  p.atAggression        = gi(16);
  p.atComposure         = gi(17);
  p.atInterceptions     = gi(18);
  p.atPositioning       = gi(19);
  p.atVision            = gi(20);
  p.atBallControl       = gi(21);
  p.atCrossing          = gi(22);
  p.atDribbling         = gi(23);
  p.atFinishing         = gi(24);
  p.atFkAccuracy        = gi(25);
  p.atHeadingAccuracy   = gi(26);
  p.atLongPassing       = gi(27);
  p.atShortPassing      = gi(28);
  p.atDefensiveAwareness= gi(29);
  p.atShotPower         = gi(30);
  p.atLongShots         = gi(31);
  p.atStandingTackle    = gi(32);
  p.atSlidingTackle     = gi(33);
  p.atVolleys           = gi(34);
  p.atCurve             = gi(35);
  p.atPenalties         = gi(36);
  // GK attributes col0+37..col0+41
  p.atGkDiving          = gi(37);
  p.atGkHandling        = gi(38);
  p.atGkKicking         = gi(39);
  p.atGkReflexes        = gi(40);
  p.atGkPositioning     = gi(41);
}

// Load a full player detail from DB by id into a Player struct
static void LoadPlayerFullDetail(int playerId, CareerHubState::Player &out) {
  const bool hasPlayerStamina = DBHasColumn("players", "player_stamina");
  std::stringstream q;
  q << "SELECT id, firstname, lastname, role, age, base_stat,"
    << " formationorder, COALESCE(pss.weekly_wage,players.weekly_wage),"
    << " COALESCE(pss.contract_expiry,players.contract_expiry), player_potential,"
    << " foot, stamina,"
    << (hasPlayerStamina ? " COALESCE(players.player_stamina,100)," : " 100,")
    << " height, reputation,"
    << kPlayerAttrCols
    << " FROM players LEFT JOIN player_save_state pss"
    << " ON pss.manager_id=" << g_CareerHub.managerId << " AND pss.player_id=players.id"
    << " WHERE players.id=" << playerId << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str());
  if (!r || r->data.empty()) { if (r) delete r; return; }
  out.id             = atoi(DBCell(r,0,0).c_str());
  out.firstName      = DBCell(r,0,1);
  out.lastName       = DBCell(r,0,2);
  out.role           = DBCell(r,0,3);
  out.age            = DBCell(r,0,4);
  { std::string s = DBCell(r,0,5); out.baseStat = s.empty() ? 0.0f : (float)atof(s.c_str()); }
  { std::string s = DBCell(r,0,6); out.formationOrder = s.empty() ? -1 : atoi(s.c_str()); }
  { std::string s = DBCell(r,0,7); out.weeklywage = s.empty() ? 0 : atoi(s.c_str()); }
  out.contractExpiry = DBCell(r,0,8);
  { std::string s = DBCell(r,0,9); out.potential = s.empty() ? 0 : atoi(s.c_str()); }
  out.foot           = DBCell(r,0,10);
  { std::string s = DBCell(r,0,11); out.stamina = s.empty() ? 0 : atoi(s.c_str()); }
  { std::string s = DBCell(r,0,12); out.currentStamina = s.empty() ? 100 : atoi(s.c_str()); }
  { std::string s = DBCell(r,0,13); out.height = s.empty() ? 0.0f : (float)atof(s.c_str()); }
  { std::string s = DBCell(r,0,14); out.reputation = s.empty() ? 0.0f : (float)atof(s.c_str()); }
  ParsePlayerAttrsFromRow(out, r, 0, 15);
  delete r;
}

static void RunPlayerSearch(const char *raw) {
  s_searchResults.clear();
  std::string term = raw;
  if (term.empty()) return;

  // lower-case + escape single quotes for LIKE
  std::string lo;
  lo.reserve(term.size() * 2);
  for (unsigned char c : term) {
    lo += (char)tolower(c);
    if (c == '\'') lo += '\''; // double the quote
  }

  std::stringstream q;
  q << "SELECT p.id, p.firstname, p.lastname, p.role, p.age, p.base_stat,"
    << " p.player_potential, p.foot, p.stamina, p.height, p.reputation,"
    << " COALESCE(pss.weekly_wage,p.weekly_wage), COALESCE(pss.contract_expiry,p.contract_expiry), p.formationorder,"
    << " COALESCE(t.name,'') as club_name,"
    << " COALESCE(t.logo_url,'') as club_logo,"
    << " COALESCE(t.shortname,'') as club_short,"
    << " COALESCE(p.nickname,'') as nickname"
    << " FROM players p"
    << " LEFT JOIN player_save_state pss ON pss.manager_id=" << g_CareerHub.managerId << " AND pss.player_id=p.id"
    << " LEFT JOIN teams t ON COALESCE(pss.team_id,p.team_id) = t.id"
    << " WHERE LOWER(p.firstname)  LIKE '%" << lo << "%'"
    << " OR LOWER(p.lastname)      LIKE '%" << lo << "%'"
    << " OR LOWER(COALESCE(p.nickname,'')) LIKE '%" << lo << "%'"
    << " ORDER BY p.base_stat DESC LIMIT 12;";

  DatabaseResult *r = GetDB()->Query(q.str());
  for (unsigned int i = 0; i < r->data.size(); i++) {
    SearchPlayerResult sr;
    sr.id             = atoi(DBCell(r, i, 0).c_str());
    sr.firstName      = DBCell(r, i, 1);
    sr.lastName       = DBCell(r, i, 2);
    sr.role           = DBCell(r, i, 3);
    sr.age            = DBCell(r, i, 4);
    std::string bs    = DBCell(r, i, 5);
    sr.baseStat       = bs.empty() ? 0.0f : (float)atof(bs.c_str());
    std::string pts   = DBCell(r, i, 6);
    sr.potential      = pts.empty() ? 0 : atoi(pts.c_str());
    sr.foot           = DBCell(r, i, 7);
    std::string sts   = DBCell(r, i, 8);
    sr.stamina        = sts.empty() ? 0 : atoi(sts.c_str());
    std::string hs    = DBCell(r, i, 9);
    sr.height         = hs.empty() ? 0.0f : (float)atof(hs.c_str());
    std::string rps   = DBCell(r, i, 10);
    sr.reputation     = rps.empty() ? 0.0f : (float)atof(rps.c_str());
    std::string ws    = DBCell(r, i, 11);
    sr.weeklywage     = ws.empty() ? 0 : atoi(ws.c_str());
    sr.contractExpiry = DBCell(r, i, 12);
    std::string fos   = DBCell(r, i, 13);
    sr.formationOrder = fos.empty() ? -1 : atoi(fos.c_str());
    sr.clubName       = DBCell(r, i, 14);
    sr.clubLogoPath   = DBCell(r, i, 15);
    sr.clubShortName  = DBCell(r, i, 16);
    sr.nickname       = DBCell(r, i, 17);
    s_searchResults.push_back(sr);
  }
  delete r;
}

// Returns true if this stat should be hidden for a non-owned player.
//
// Visibility rule (consistent direction):
//   A stat is VISIBLE when  bucket >= hideThreshold
//   A stat is HIDDEN  when  bucket <  hideThreshold
//
// No report:          hideThreshold = 90  → 10 % visible  (buckets 90-99)
// Report revealPct R: hideThreshold = 100 - round(R*100)
//   e.g. 40% reveal  → hideThreshold = 60  → 40 % visible  (buckets 60-99)
//   e.g. 50% reveal  → hideThreshold = 50  → 50 % visible  (buckets 50-99)
//
// Because we lower the threshold as scouting improves, every stat that was
// already visible (bucket ≥ 90) stays visible after scouting (bucket ≥ 60, etc.).
static bool IsHiddenStat(int playerId, int statIdx) {
  unsigned int h = (unsigned int)playerId * 2654435761u ^ (unsigned int)statIdx * 2246822519u;
  h ^= h >> 16;
  int bucket = (int)(h % 100); // 0-99, deterministic per (playerId, statIdx)

  int hideThreshold = 90; // default: 10% visible
  for (const auto &sr : g_CareerHub.scoutReports) {
    if (sr.playerId == playerId) {
      hideThreshold = 100 - (int)(sr.revealPct * 100.0f);
      if (hideThreshold < 0)   hideThreshold = 0;
      if (hideThreshold > 100) hideThreshold = 100;
      break;
    }
  }
  return bucket < hideThreshold;
}

// Draws a section title + bar-based stat rows, all via DrawList (no ImGui tables).
// ownPlayer=true → show all stats; false → apply scouting fog of war.
static void DrawStatSection(const char *title, ImU32 titleColor,
                            const StatDef *stats, int count,
                            int playerId, bool ownPlayer = true) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  float secW     = ImGui::GetContentRegionAvail().x;
  const float kFs   = 15.0f;
  const float kTFs  = 13.0f;
  const float kRowH = 26.0f;

  // Section title
  ImVec2 tp = ImGui::GetCursorScreenPos();
  PushMgrFont(g_ManagerFontSmall);
  dl->AddText(g_ManagerFontSmall, kTFs,
              ImVec2(tp.x + 4.0f, tp.y + 2.0f), titleColor, title);
  PopMgrFont(g_ManagerFontSmall);
  int tcR = titleColor & 0xFF, tcG = (titleColor >> 8) & 0xFF, tcB = (titleColor >> 16) & 0xFF;
  dl->AddLine(ImVec2(tp.x, tp.y + 19.0f), ImVec2(tp.x + secW, tp.y + 19.0f),
              IM_COL32(tcR, tcG, tcB, 60), 0.5f);
  ImGui::Dummy(ImVec2(secW, 23.0f));

  // Stat rows
  for (int i = 0; i < count; i++) {
    bool  hidden = !ownPlayer && IsHiddenStat(playerId, stats[i].hiddenIdx);
    int   val    = stats[i].val; // real 0-99 attribute value
    ImU32 valCol = hidden ? IM_COL32(60, 68, 90, 180) : StatValueColor(val);
    ImVec2 rp    = ImGui::GetCursorScreenPos();

    const float kLM = 6.0f; // left margin for stroke + label
    ImU32 rowBg = (i % 2 == 0) ? IM_COL32(15, 22, 46, 130) : IM_COL32(10, 16, 34, 60);
    dl->AddRectFilled(ImVec2(rp.x, rp.y), ImVec2(rp.x + secW, rp.y + kRowH), rowBg);

    // Colored accent edge always visible — color matches stat quality
    if (!hidden)
      dl->AddRectFilled(ImVec2(rp.x + kLM, rp.y), ImVec2(rp.x + kLM + 3.0f, rp.y + kRowH),
                        valCol);

    PushMgrFont(g_ManagerFontSmall);
    ImU32 labelCol = hidden ? IM_COL32(110, 118, 145, 160) : IM_COL32(192, 202, 226, 240);
    dl->AddText(g_ManagerFontSmall, kFs,
                ImVec2(rp.x + kLM + 8.0f, rp.y + (kRowH - kFs) * 0.5f),
                labelCol, stats[i].label);
    PopMgrFont(g_ManagerFontSmall);

    // Fill bar (0-99 scale)
    float bX0 = rp.x + secW * 0.60f;
    float bX1 = rp.x + secW - 24.0f;
    if (bX1 > bX0 + 4.0f) {
      const float bH = 9.0f;
      const float bY = rp.y + (kRowH - bH) * 0.5f;
      dl->AddRectFilled(ImVec2(bX0, bY), ImVec2(bX1, bY + bH),
                        IM_COL32(22, 30, 58, 210), 3.0f);
      if (!hidden) {
        const float fill = (bX1 - bX0) * (val / 99.0f);
        int vr = valCol & 0xFF, vg = (valCol >> 8) & 0xFF, vb = (valCol >> 16) & 0xFF;
        if (fill > 0.5f)
          dl->AddRectFilled(ImVec2(bX0, bY), ImVec2(bX0 + fill, bY + bH),
                            IM_COL32(vr, vg, vb, 175), 3.0f);
      } else {
        dl->AddRectFilled(ImVec2(bX0, bY), ImVec2(bX1, bY + bH),
                          IM_COL32(35, 42, 65, 140), 3.0f);
      }
    }

    // Number — "?" when hidden
    const char *vbuf = hidden ? "?" : nullptr;
    char numbuf[4];
    if (!hidden) { snprintf(numbuf, sizeof(numbuf), "%d", val); vbuf = numbuf; }
    PushMgrFont(g_ManagerFontSmall);
    ImVec2 vsz = g_ManagerFontSmall
        ? g_ManagerFontSmall->CalcTextSizeA(kFs, FLT_MAX, 0, vbuf)
        : ImGui::CalcTextSize(vbuf);
    dl->AddText(g_ManagerFontSmall, kFs,
                ImVec2(rp.x + secW - vsz.x - 4.0f, rp.y + (kRowH - kFs) * 0.5f),
                hidden ? IM_COL32(60, 68, 90, 160) : valCol, vbuf);
    PopMgrFont(g_ManagerFontSmall);

    ImGui::Dummy(ImVec2(secW, kRowH));
  }
  ImGui::Dummy(ImVec2(0, 14.0f));
}

static bool IsPlayerLoanedOutFromUserClub(int playerId) {
  std::stringstream q;
  q << "SELECT 1 FROM loan_deals"
    << " WHERE manager_id=" << g_CareerHub.managerId
    << " AND player_id=" << playerId
    << " AND loaning_club_id=" << g_CareerHub.clubId
    << " AND status='active'"
    << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str().c_str());
  bool loanedOut = r && !r->data.empty();
  if (r) delete r;
  return loanedOut;
}

// Forward declarations for scouting helpers (defined later in this file)
static void StartScouting(int managerId, int playerId,
                          const std::string &fn, const std::string &ln,
                          const std::string &club, int scoutRating,
                          const std::string &currentDate);
static void CancelScouting(int managerId, int playerId);

// ---- DrawClubDetailPage --------------------------------------------------

static ImVec4 ParseColorStr(const std::string &s, ImVec4 fallback) {
  int ri = (int)(fallback.x*255), gi = (int)(fallback.y*255), bi = (int)(fallback.z*255);
  if (!s.empty()) sscanf(s.c_str(), "%d , %d , %d", &ri, &gi, &bi);
  return ImVec4(ri/255.0f, gi/255.0f, bi/255.0f, 1.0f);
}

static void DrawPrestigeDots(ImDrawList *dl, ImVec2 pos, int val, int maxVal,
                             ImU32 filledCol, ImU32 emptyCol) {
  const float r = 5.0f, gap = 4.0f;
  for (int i = 0; i < maxVal; i++) {
    float cx = pos.x + i * (r*2.0f + gap) + r;
    dl->AddCircleFilled(ImVec2(cx, pos.y), r, i < val ? filledCol : emptyCol);
    if (i >= val)
      dl->AddCircle(ImVec2(cx, pos.y), r, emptyCol, 12, 1.0f);
  }
}

static void DrawClubDetailPage(float w, float h) {
  if (s_clubDetailId != s_clubDetailLastId) {
    LoadClubDetail(s_clubDetailId);
    s_clubDetailLastId = s_clubDetailId;
  }
  const ClubDetailData &cd = s_clubDetail;
  if (cd.id < 0) {
    ImGui::SetCursorPos(ImVec2(16.0f, 8.0f));
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted("Club not found.");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    return;
  }

  ImVec4 col1 = ParseColorStr(cd.color1, kAccent);
  ImVec4 col2 = ParseColorStr(cd.color2, ImVec4(0.9f,0.9f,0.9f,1.0f));
  ImU32 c1u   = IM_COL32((int)(col1.x*255),(int)(col1.y*255),(int)(col1.z*255),255);
  ImU32 c2u   = IM_COL32((int)(col2.x*255),(int)(col2.y*255),(int)(col2.z*255),255);

  const float kPad = 16.0f, kGap = 8.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 14.0f;

  // ===========================================================
  // Header card
  // ===========================================================
  const float kHdrH = 150.0f;
  ImVec2 hdrOrg = ImGui::GetCursorScreenPos();
  BeginModernCard("##clbhdr", ImVec2(usW, kHdrH));
  ImDrawList *hdl = ImGui::GetWindowDrawList();

  // Color swatch strip at very top of card
  hdl->AddRectFilled(hdrOrg, ImVec2(hdrOrg.x + usW, hdrOrg.y + 5.0f), c1u, 8.0f);
  hdl->AddRectFilled(ImVec2(hdrOrg.x + usW * 0.5f, hdrOrg.y),
                     ImVec2(hdrOrg.x + usW, hdrOrg.y + 5.0f), c2u, 0.0f);

  // Club badge (large)
  const float kBadgeS = 96.0f;
  float bx = hdrOrg.x + 20.0f;
  float by = hdrOrg.y + (kHdrH - kBadgeS) * 0.5f;
  ImGui::SetCursorScreenPos(ImVec2(bx, by));
  DrawTeamBadge(cd.logoPath, cd.shortName, kBadgeS);

  // Name block to the right of badge
  float infoX = bx + kBadgeS + 18.0f;
  float infoY = hdrOrg.y + 18.0f;

  PushMgrFont(g_ManagerFontTitle);
  hdl->AddText(g_ManagerFontTitle, 30.0f, ImVec2(infoX, infoY),
               IM_COL32(225, 232, 252, 255), cd.name.c_str());
  PopMgrFont(g_ManagerFontTitle);

  // Shortname pill
  if (!cd.shortName.empty()) {
    PushMgrFont(g_ManagerFontSmall);
    ImVec2 snSz = g_ManagerFontSmall
        ? g_ManagerFontSmall->CalcTextSizeA(14.0f, FLT_MAX, 0, cd.shortName.c_str())
        : ImGui::CalcTextSize(cd.shortName.c_str());
    float pillW = snSz.x + 14.0f, pillH = 20.0f;
    float pillX = infoX, pillY = infoY + 36.0f;
    hdl->AddRectFilled(ImVec2(pillX, pillY), ImVec2(pillX+pillW, pillY+pillH),
                       IM_COL32((int)(col1.x*255),(int)(col1.y*255),(int)(col1.z*255),180), 4.0f);
    hdl->AddText(g_ManagerFontSmall, 14.0f, ImVec2(pillX+7.0f, pillY+(pillH-14.0f)*0.5f),
                 IM_COL32(255,255,255,230), cd.shortName.c_str());
    PopMgrFont(g_ManagerFontSmall);
  }

  // Stadium
  if (!cd.homeStadium.empty()) {
    PushMgrFont(g_ManagerFontSmall);
    char stadBuf[128]; snprintf(stadBuf, sizeof(stadBuf), "Stadium: %s", cd.homeStadium.c_str());
    hdl->AddText(g_ManagerFontSmall, 15.0f, ImVec2(infoX, infoY + 64.0f),
                 IM_COL32(155,168,202,220), stadBuf);
    PopMgrFont(g_ManagerFontSmall);
  }

  // Prestige — right side
  float presX = hdrOrg.x + usW * 0.58f;
  float presY = hdrOrg.y + 30.0f;
  {
    PushMgrFont(g_ManagerFontSmall);
    hdl->AddText(g_ManagerFontSmall, 14.0f, ImVec2(presX, presY),
                 IM_COL32(138,152,185,200), "International");
    DrawPrestigeDots(hdl, ImVec2(presX + 2.0f, presY + 26.0f),
                     cd.intPrestige > 10 ? 10 : cd.intPrestige, 10,
                     IM_COL32((int)(col1.x*255),(int)(col1.y*255),(int)(col1.z*255),230),
                     IM_COL32(40,55,90,200));
    hdl->AddText(g_ManagerFontSmall, 14.0f, ImVec2(presX, presY + 44.0f),
                 IM_COL32(138,152,185,200), "Domestic");
    DrawPrestigeDots(hdl, ImVec2(presX + 2.0f, presY + 70.0f),
                     cd.domPrestige > 10 ? 10 : cd.domPrestige, 10,
                     c2u, IM_COL32(40,55,90,200));
    PopMgrFont(g_ManagerFontSmall);
  }

  // Color swatches (small circles, far right)
  float swX = hdrOrg.x + usW - 80.0f;
  float swY = hdrOrg.y + 30.0f;
  hdl->AddCircleFilled(ImVec2(swX, swY + 12.0f), 12.0f, c1u);
  hdl->AddCircle(ImVec2(swX, swY + 12.0f), 12.0f, IM_COL32(255,255,255,30));
  hdl->AddCircleFilled(ImVec2(swX + 40.0f, swY + 12.0f), 12.0f, c2u);
  hdl->AddCircle(ImVec2(swX + 40.0f, swY + 12.0f), 12.0f, IM_COL32(255,255,255,30));
  {
    PushMgrFont(g_ManagerFontSmall);
    ImVec2 p1sz = g_ManagerFontSmall
        ? g_ManagerFontSmall->CalcTextSizeA(11.0f, FLT_MAX, 0, "Primary")
        : ImGui::CalcTextSize("Primary");
    ImVec2 s1sz = g_ManagerFontSmall
        ? g_ManagerFontSmall->CalcTextSizeA(11.0f, FLT_MAX, 0, "Secondary")
        : ImGui::CalcTextSize("Secondary");
    hdl->AddText(g_ManagerFontSmall, 11.0f,
                 ImVec2(swX - p1sz.x * 0.5f + 0.0f, swY + 28.0f),
                 IM_COL32(100,120,160,180), "Primary");
    hdl->AddText(g_ManagerFontSmall, 11.0f,
                 ImVec2(swX + 40.0f - s1sz.x * 0.5f, swY + 28.0f),
                 IM_COL32(100,120,160,180), "Secondary");
    PopMgrFont(g_ManagerFontSmall);
  }

  EndModernCard();
  ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
  ImGui::Dummy(ImVec2(0, kGap));

  // ===========================================================
  // Squad list card
  // ===========================================================
  float listH = usH - kHdrH - kGap * 2.0f - 14.0f;
  if (listH < 80.0f) listH = 80.0f;
  ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
  BeginModernCard("##clb_squad", ImVec2(usW, listH));

  // Section header
  ImVec2 sqHdrOrg = ImGui::GetCursorScreenPos();
  ImDrawList *sdl = ImGui::GetWindowDrawList();
  const float kSqHdrH = 36.0f;
  sdl->AddRectFilled(sqHdrOrg, ImVec2(sqHdrOrg.x + usW, sqHdrOrg.y + kSqHdrH),
                     IM_COL32(14,22,46,220), 0.0f);
  {
    char sqTitle[32]; snprintf(sqTitle, sizeof(sqTitle), "Squad  (%d)", (int)cd.squad.size());
    PushMgrFont(g_ManagerFontBold);
    sdl->AddText(g_ManagerFontBold, 16.0f,
                 ImVec2(sqHdrOrg.x + 14.0f, sqHdrOrg.y + (kSqHdrH - 16.0f) * 0.5f),
                 C32(kAccent), sqTitle);
    PopMgrFont(g_ManagerFontBold);
  }
  ImGui::Dummy(ImVec2(0, kSqHdrH));

  const float kRowH = 34.0f;
  ImGui::BeginChild("##clb_sq_scroll", ImVec2(0, listH - kSqHdrH - 20.0f), false);
  PushMgrFont(g_ManagerFontSmall);

  for (int i = 0; i < (int)cd.squad.size(); i++) {
    const ClubDetailData::ClubPlayer &cp = cd.squad[i];
    ImVec2 rp = ImGui::GetCursorScreenPos();
    ImDrawList *rdl = ImGui::GetWindowDrawList();

    bool hovered = ImGui::IsMouseHoveringRect(rp, ImVec2(rp.x + usW - 28.0f, rp.y + kRowH));
    if (hovered)
      rdl->AddRectFilled(rp, ImVec2(rp.x + usW - 28.0f, rp.y + kRowH),
                         IM_COL32(255,255,255,12), 0.0f);

    // Accent left bar
    rdl->AddRectFilled(ImVec2(rp.x + 6.0f, rp.y + 6.0f),
                       ImVec2(rp.x + 9.0f, rp.y + kRowH - 6.0f), c1u);

    // Jersey number
    char nb[8];
    if (cp.jerseyNumber > 0)
      snprintf(nb, sizeof(nb), "#%d", cp.jerseyNumber);
    else
      nb[0] = '\0';
    if (nb[0]) {
      rdl->AddText(g_ManagerFontSmall, 15.0f,
                   ImVec2(rp.x + 14.0f, rp.y + (kRowH - 15.0f) * 0.5f),
                   IM_COL32(255, 255, 255, 220), nb);
    }

    // Position badge
    if (!cp.role.empty()) {
      ImVec2 bsz = g_ManagerFontSmall
          ? g_ManagerFontSmall->CalcTextSizeA(12.0f, FLT_MAX, 0, cp.role.c_str())
          : ImGui::CalcTextSize(cp.role.c_str());
      float bw = bsz.x + 10.0f, bh = 18.0f;
      float bx2 = rp.x + 36.0f, by2 = rp.y + (kRowH - bh) * 0.5f;
      rdl->AddRectFilled(ImVec2(bx2, by2), ImVec2(bx2+bw, by2+bh),
                         IM_COL32((int)(col1.x*255),(int)(col1.y*255),(int)(col1.z*255),160), 3.0f);
      rdl->AddText(g_ManagerFontSmall, 12.0f,
                   ImVec2(bx2 + 5.0f, by2 + (bh - 12.0f) * 0.5f),
                   IM_COL32(255,255,255,220), cp.role.c_str());
    }

    // Player name
    float nameX = rp.x + 90.0f;
    rdl->AddText(g_ManagerFontSmall, 16.0f,
                 ImVec2(nameX, rp.y + (kRowH - 16.0f) * 0.5f),
                 C32(kTextPri), cp.name.c_str());

    // Age
    if (!cp.age.empty()) {
      char ageBuf[12]; snprintf(ageBuf, sizeof(ageBuf), "Age %s", cp.age.c_str());
      rdl->AddText(g_ManagerFontSmall, 13.0f,
                   ImVec2(rp.x + usW - 180.0f, rp.y + (kRowH - 13.0f) * 0.5f),
                   C32(kTextSec), ageBuf);
    }

    // Ability stars
    ImGui::SetCursorScreenPos(ImVec2(rp.x + usW - 120.0f, rp.y + (kRowH - 14.0f) * 0.5f));
    DrawStars(cp.ability, 1.0f, C32(kGold), 1.2f);

    // Invisible click target for navigation to player detail
    ImGui::SetCursorScreenPos(rp);
    char btnId[32]; snprintf(btnId, sizeof(btnId), "##clbp_%d", cp.id);
    if (ImGui::InvisibleButton(btnId, ImVec2(usW - 28.0f, kRowH))) {
      // Navigate to player detail (read-only / scouting view since it's another club)
      bool isOwnSquad = false;
      for (const auto &p : g_CareerHub.players)
        if (p.id == cp.id) { isOwnSquad = true; break; }
      s_playerDetailId = cp.id;
      if (isOwnSquad) {
        s_detailOverrideActive = false;
        s_detailClubName = s_detailClubLogo = s_detailClubShortName = "";
      } else {
        CareerHubState::Player op;
        LoadPlayerFullDetail(cp.id, op);
        s_detailPlayerOverride = op;
        s_detailOverrideActive = true;
        s_detailClubName      = cd.name;
        s_detailClubLogo      = cd.logoPath;
        s_detailClubShortName = cd.shortName;
      }
      NavPush(PAGE_PLAYER_DETAIL);
    }
    if (ImGui::IsItemHovered())
      ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);

    // Separator
    rdl->AddLine(ImVec2(rp.x + 6.0f, rp.y + kRowH - 1.0f),
                 ImVec2(rp.x + usW - 34.0f, rp.y + kRowH - 1.0f),
                 IM_COL32(255,255,255,10));
  }

  if (cd.squad.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted("No players found for this club.");
    ImGui::PopStyleColor();
  }

  PopMgrFont(g_ManagerFontSmall);
  ImGui::EndChild();
  EndModernCard();
}

// ---- DrawPlayerDetailPage ------------------------------------------------

static void DrawPlayerDetailPage(float w, float h) {
  const CareerHubState::Player *pPlayer = nullptr;
  for (const auto &p : g_CareerHub.players)
    if (p.id == s_playerDetailId) { pPlayer = &p; break; }
  // Fall back to override (player found via search, not in current squad)
  if (!pPlayer && s_detailOverrideActive && s_detailPlayerOverride.id == s_playerDetailId)
    pPlayer = &s_detailPlayerOverride;
  if (!pPlayer) {
    ImGui::SetCursorPos(ImVec2(16.0f, 8.0f));
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted("Player not found.");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    return;
  }
  const auto &pl      = *pPlayer;
  const bool ownPlayer = !s_detailOverrideActive;
  const bool loanedOutOwnedPlayer = !ownPlayer && IsPlayerLoanedOutFromUserClub(pl.id);
  const bool fullAttributeAccess = ownPlayer || loanedOutOwnedPlayer;

  if (s_playerDetailLastId != s_playerDetailId) {
    s_plTab              = 0;
    s_playerDetailLastId = s_playerDetailId;
  }

  std::string dispName = DisplayName(pl);
  std::string fullName = pl.firstName.empty() ? pl.lastName
                       : (pl.lastName.empty() ? pl.firstName
                       : pl.firstName + " " + pl.lastName);

  const float kPad = 16.0f, kGap = 8.0f;
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW = w - kPad * 2.0f;
  float usH = h - 14.0f;

  int accR = (int)(kAccent.x * 255);
  int accG = (int)(kAccent.y * 255);
  int accB = (int)(kAccent.z * 255);

  // ===========================================================
  // Header card (taller to fit more info)
  // ===========================================================
  const float kHdrH = 150.0f;
  ImVec2 hdrOrg = ImGui::GetCursorScreenPos();
  BeginModernCard("##pldhdr", ImVec2(usW, kHdrH));
  ImDrawList *hdl = ImGui::GetWindowDrawList();

  // Face placeholder box (jersey number inside)
  const float kFaceS = 90.0f;
  float fx = hdrOrg.x + 16.0f;
  float fy = hdrOrg.y + (kHdrH - kFaceS) * 0.5f;
  hdl->AddRectFilled(ImVec2(fx, fy), ImVec2(fx + kFaceS, fy + kFaceS),
                     IM_COL32(22, 32, 56, 255), 12.0f);
  hdl->AddRect(ImVec2(fx, fy), ImVec2(fx + kFaceS, fy + kFaceS),
               IM_COL32(accR, accG, accB, 80), 12.0f, 0, 1.5f);
  // Jersey number badge
  if (pl.jerseyNumber > 0) {
    char jnBuf[8]; snprintf(jnBuf, sizeof(jnBuf), "#%d", pl.jerseyNumber);
    PushMgrFont(g_ManagerFontSmall);
    ImVec2 jnSz = g_ManagerFontSmall
        ? g_ManagerFontSmall->CalcTextSizeA(13.0f, FLT_MAX, 0, jnBuf)
        : ImGui::CalcTextSize(jnBuf);
    hdl->AddText(g_ManagerFontSmall, 13.0f,
                 ImVec2(fx + (kFaceS - jnSz.x) * 0.5f, fy + 6.0f),
                 IM_COL32(accR, accG, accB, 200), jnBuf);
    PopMgrFont(g_ManagerFontSmall);
  }
  // Initial letter
  char init[2] = { dispName.empty() ? '?' : (unsigned char)dispName[0], '\0' };
  PushMgrFont(g_ManagerFontTitle);
  ImVec2 initSz = g_ManagerFontTitle
      ? g_ManagerFontTitle->CalcTextSizeA(28.0f, FLT_MAX, 0, init)
      : ImGui::CalcTextSize(init);
  hdl->AddText(g_ManagerFontTitle, 28.0f,
               ImVec2(fx + (kFaceS - initSz.x) * 0.5f, fy + (kFaceS - initSz.y) * 0.5f),
               IM_COL32(accR, accG, accB, 160), init);
  PopMgrFont(g_ManagerFontTitle);

  // Name + info block — vertically centered within header
  float tx = fx + kFaceS + 18.0f;
  bool showFullName2 = !pl.nickname.empty() && fullName != dispName && !fullName.empty();
  float contentH2 = 34.0f + (showFullName2 ? 21.0f : 0.0f) + 22.0f + 20.0f + 26.0f;
  float ty = hdrOrg.y + (kHdrH - contentH2) * 0.5f;
  if (ty < hdrOrg.y + 8.0f) ty = hdrOrg.y + 8.0f;

  // Nickname / display name (title font)
  PushMgrFont(g_ManagerFontTitle);
  float titleLineH = g_ManagerFontTitle ? g_ManagerFontTitle->FontSize : 20.0f;
  hdl->AddText(g_ManagerFontTitle, 31.0f,
               ImVec2(tx, ty), IM_COL32(232, 238, 252, 255), dispName.c_str());
  PopMgrFont(g_ManagerFontTitle);

  // Full name (smaller, dimmer) — only if nickname differs from full name
  float row2y = ty + titleLineH + 3.0f;
  if (showFullName2) {
    PushMgrFont(g_ManagerFontSmall);
    hdl->AddText(g_ManagerFontSmall, 17.0f, ImVec2(tx, row2y),
                 IM_COL32(148, 162, 196, 200), fullName.c_str());
    PopMgrFont(g_ManagerFontSmall);
    row2y += 21.0f;
  }

  // Age | Nationality row
  {
    char ageLine[96];
    const char *natStr = pl.nationality.empty() ? "-" : pl.nationality.c_str();
    snprintf(ageLine, sizeof(ageLine), "Age: %s  |  %s",
             pl.age.empty() ? "-" : pl.age.c_str(), natStr);
    PushMgrFont(g_ManagerFontSmall);
    hdl->AddText(g_ManagerFontSmall, 18.0f, ImVec2(tx, row2y),
                 IM_COL32(148, 162, 196, 215), ageLine);
    PopMgrFont(g_ManagerFontSmall);
  }

  // Position badge + alt positions
  float row3y = row2y + 20.0f;
  {
    float badgeX = tx;
    // Main position
    if (!pl.role.empty()) {
      PushMgrFont(g_ManagerFontSmall);
      ImVec2 rSz = g_ManagerFontSmall
          ? g_ManagerFontSmall->CalcTextSizeA(16.0f, FLT_MAX, 0, pl.role.c_str())
          : ImGui::CalcTextSize(pl.role.c_str());
      float bW = rSz.x + 18.0f, bH = 26.0f;
      hdl->AddRectFilled(ImVec2(badgeX, row3y), ImVec2(badgeX + bW, row3y + bH),
                         IM_COL32(accR, accG, accB, 70), 4.0f);
      hdl->AddRect(ImVec2(badgeX, row3y), ImVec2(badgeX + bW, row3y + bH),
                   IM_COL32(accR, accG, accB, 180), 4.0f, 0, 1.2f);
      hdl->AddText(g_ManagerFontSmall, 16.0f,
                   ImVec2(badgeX + 9.0f, row3y + (bH - 16.0f) * 0.5f),
                   IM_COL32(222, 230, 248, 235), pl.role.c_str());
      PopMgrFont(g_ManagerFontSmall);
      badgeX += bW + 5.0f;
    }
    // Alternative positions
    auto altPos = ParseAltPositions(pl.alternativePos);
    for (const auto &ap : altPos) {
      PushMgrFont(g_ManagerFontSmall);
      ImVec2 aSz = g_ManagerFontSmall
          ? g_ManagerFontSmall->CalcTextSizeA(15.0f, FLT_MAX, 0, ap.c_str())
          : ImGui::CalcTextSize(ap.c_str());
      float bW = aSz.x + 16.0f, bH = 26.0f;
      if (badgeX + bW > hdrOrg.x + usW * 0.46f) { PopMgrFont(g_ManagerFontSmall); break; }
      hdl->AddRectFilled(ImVec2(badgeX, row3y), ImVec2(badgeX + bW, row3y + bH),
                         IM_COL32(45, 58, 95, 120), 4.0f);
      hdl->AddRect(ImVec2(badgeX, row3y), ImVec2(badgeX + bW, row3y + bH),
                   IM_COL32(80, 100, 150, 130), 4.0f, 0, 0.8f);
      hdl->AddText(g_ManagerFontSmall, 15.0f,
                   ImVec2(badgeX + 8.0f, row3y + (bH - 15.0f) * 0.5f),
                   IM_COL32(160, 175, 210, 210), ap.c_str());
      PopMgrFont(g_ManagerFontSmall);
      badgeX += bW + 4.0f;
    }
  }

  // Right section: club badge + info
  const std::string &dispClubLogo  = ownPlayer ? g_CareerHub.club.logoPath  : s_detailClubLogo;
  const std::string &dispClubShort = ownPlayer ? g_CareerHub.club.shortName : s_detailClubShortName;
  const std::string &dispClubName  = ownPlayer ? g_CareerHub.club.name      : s_detailClubName;

  float rx = hdrOrg.x + usW * 0.50f;
  {
    ImVec2 badgePos = ImVec2(rx, hdrOrg.y + (kHdrH - 60.0f) * 0.5f);
    ImGui::SetCursorScreenPos(badgePos);
    DrawTeamBadge(dispClubLogo, dispClubShort, 60.0f);
    // Click on badge → navigate to club detail page
    ImGui::SetCursorScreenPos(badgePos);
    if (ImGui::InvisibleButton("##pld_clubbadge", ImVec2(60.0f, 60.0f))) {
      int tid = ownPlayer ? g_CareerHub.clubId : LookupTeamIdByName(dispClubName);
      if (tid > 0) NavToClubDetail(tid);
    }
    if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
  }

  float infoX = rx + 70.0f, infoY = hdrOrg.y + 10.0f;
  PushMgrFont(g_ManagerFontSmall);
  hdl->AddText(g_ManagerFontSmall, 21.0f, ImVec2(infoX, infoY),
               IM_COL32(205, 215, 238, 240), dispClubName.c_str());
  {
    char wb[48];
    if (pl.weeklywage >= 1000)
      snprintf(wb, sizeof(wb), "Wage: \xe2\x82\xac%d,%03d p/w", pl.weeklywage/1000, pl.weeklywage%1000);
    else if (pl.weeklywage > 0)
      snprintf(wb, sizeof(wb), "Wage: \xe2\x82\xac%d p/w", pl.weeklywage);
    else
      snprintf(wb, sizeof(wb), "Wage: -");
    hdl->AddText(g_ManagerFontSmall, 20.0f, ImVec2(infoX, infoY + 24.0f),
                 IM_COL32(155, 168, 202, 220), wb);
  }
  {
    std::string expFmt = FormatContractExpiry(pl.contractExpiry);
    char cb[48];
    snprintf(cb, sizeof(cb), "Contract: %s", expFmt.c_str());
    hdl->AddText(g_ManagerFontSmall, 20.0f, ImVec2(infoX, infoY + 48.0f),
                 IM_COL32(155, 168, 202, 220), cb);
  }
  // Estimated player value
  if (pl.playerValue > 0) {
    char vb[40];
    if (pl.playerValue >= 1000000)
      snprintf(vb, sizeof(vb), "Est. Value: \xe2\x82\xac%.1fM", pl.playerValue / 1000000.0f);
    else if (pl.playerValue >= 1000)
      snprintf(vb, sizeof(vb), "Est. Value: \xe2\x82\xac%.0fK", pl.playerValue / 1000.0f);
    else
      snprintf(vb, sizeof(vb), "Est. Value: \xe2\x82\xac%d", pl.playerValue);
    hdl->AddText(g_ManagerFontSmall, 20.0f, ImVec2(infoX, infoY + 72.0f),
                 IM_COL32(155, 168, 202, 220), vb);
  }
  PopMgrFont(g_ManagerFontSmall);

  // CA / PA stars + jersey number (far right)
  float starX = hdrOrg.x + usW - 175.0f;
  PushMgrFont(g_ManagerFontSmall);
  hdl->AddText(g_ManagerFontSmall, 16.0f, ImVec2(starX, hdrOrg.y + 20.0f),
               IM_COL32(138, 152, 185, 210), "Ability");
  hdl->AddText(g_ManagerFontSmall, 16.0f, ImVec2(starX, hdrOrg.y + 52.0f),
               IM_COL32(138, 152, 185, 210), "Potential");
  // Jersey number — just the number, larger, white
  if (pl.jerseyNumber > 0) {
    char jnHdr[16]; snprintf(jnHdr, sizeof(jnHdr), "#%d", pl.jerseyNumber);
    hdl->AddText(g_ManagerFontSmall, 24.0f, ImVec2(starX + 80.0f, hdrOrg.y + 80.0f),
                 IM_COL32(255, 255, 255, 240), jnHdr);
  }
  PopMgrFont(g_ManagerFontSmall);
  ImGui::SetCursorScreenPos(ImVec2(starX + 80.0f, hdrOrg.y + 17.0f));
  DrawStars(pl.baseStat, 1.0f, C32(kGold), 1.5f);
  ImGui::SetCursorScreenPos(ImVec2(starX + 80.0f, hdrOrg.y + 49.0f));
  DrawStars((float)pl.potential, 200.0f, IM_COL32(100, 160, 220, 220), 1.5f);

  EndModernCard();
  ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
  ImGui::Dummy(ImVec2(0, kGap));

  // ===========================================================
  // Action row (for opposition players only)
  // ===========================================================
  if (!ownPlayer && !loanedOutOwnedPlayer) {
    // Check if player has a scout report or is transfer-listed
    bool hasScouted = false;
    bool isListed   = false;
    {
      std::stringstream scq;
      scq << "SELECT COUNT(*) FROM scout_reports WHERE manager_id=" << g_CareerHub.managerId
          << " AND player_id=" << pl.id << ";";
      DatabaseResult *scr = GetDB()->Query(scq.str().c_str());
      if (scr && scr->data.size() > 0) hasScouted = (atoi(DBCell(scr,0,0).c_str()) > 0);
      if (scr) delete scr;
    }
    {
      std::stringstream lq;
      lq << "SELECT status FROM player_market_status"
         << " WHERE manager_id=" << g_CareerHub.managerId
         << " AND player_id=" << pl.id
         << " AND status IN ('transfer_listed','surplus_to_requirements','expiring_soon')"
         << " LIMIT 1;";
      DatabaseResult *lr = GetDB()->Query(lq.str().c_str());
      if (lr && lr->data.size() > 0) isListed = true;
      if (lr) delete lr;
    }
    bool canBid = (hasScouted || isListed);
    ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
    if (!canBid) {
      ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.4f);
      ImGui::Button("Bid", ImVec2(80.0f, 28.0f));
      ImGui::PopStyleVar();
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
        ImGui::TextUnformatted("Board requests you scout this player at least once before making a bid.");
        ImGui::PopStyleColor();
        ImGui::EndTooltip();
      }
    } else {
      if (ImGui::Button("Bid", ImVec2(80.0f, 28.0f))) {
        // Find this save's current club for the player.
        int sellerClubId = 0;
        { std::stringstream tq;
          tq << "SELECT COALESCE(pss.team_id,p.team_id)"
             << " FROM players p LEFT JOIN player_save_state pss"
             << " ON pss.manager_id=" << g_CareerHub.managerId << " AND pss.player_id=p.id"
             << " WHERE p.id=" << pl.id << ";";
          DatabaseResult *tr = GetDB()->Query(tq.str().c_str());
          if (tr && tr->data.size() > 0) sellerClubId = atoi(DBCell(tr,0,0).c_str());
          if (tr) delete tr; }
        s_bidPlayerId_g     = pl.id;
        s_bidSellerClubId_g = sellerClubId;
        s_bidFee_g          = (int)(pl.playerValue * 0.80f);
        s_bidWage_g         = (int)(pl.weeklywage  * 1.10f);
        snprintf(s_bidFeeStr_g,    sizeof(s_bidFeeStr_g),    "%d", s_bidFee_g);
        snprintf(s_bidWageStr_g,   sizeof(s_bidWageStr_g),   "%d", s_bidWage_g);
        std::string pn = pl.firstName + " " + pl.lastName;
        snprintf(s_bidPlayerName_g, sizeof(s_bidPlayerName_g), "%s", pn.c_str());
        s_openBidPopup_g = true;
      }
      ImGui::SameLine(0.0f, 8.0f);
      if (ImGui::Button("Loan", ImVec2(80.0f, 28.0f))) {
        int parentClubId = 0;
        { std::stringstream tq;
          tq << "SELECT COALESCE(pss.team_id,p.team_id)"
             << " FROM players p LEFT JOIN player_save_state pss"
             << " ON pss.manager_id=" << g_CareerHub.managerId << " AND pss.player_id=p.id"
             << " WHERE p.id=" << pl.id << ";";
          DatabaseResult *tr = GetDB()->Query(tq.str().c_str());
          if (tr && tr->data.size() > 0) parentClubId = atoi(DBCell(tr,0,0).c_str());
          if (tr) delete tr; }
        s_loanPlayerId_g = pl.id;
        s_loanParentClubId_g = parentClubId;
        s_loanReceivingClubId_g = g_CareerHub.clubId;
        s_loanIsLoanIn_g = true;
        snprintf(s_loanPlayerName_g, sizeof(s_loanPlayerName_g), "%s %s", pl.firstName.c_str(), pl.lastName.c_str());
        snprintf(s_loanFeeStr_g, sizeof(s_loanFeeStr_g), "%d", 0);
        snprintf(s_loanWagePctStr_g, sizeof(s_loanWagePctStr_g), "%d", 70);
        snprintf(s_loanOptionFeeStr_g, sizeof(s_loanOptionFeeStr_g), "%d", 0);
        snprintf(s_loanMandatoryFeeStr_g, sizeof(s_loanMandatoryFeeStr_g), "%d", 0);
        snprintf(s_loanMandatoryAppsStr_g, sizeof(s_loanMandatoryAppsStr_g), "%d", 0);
        s_loanMandatoryModeIdx_g = 0;
        snprintf(s_loanEndDateStr_g, sizeof(s_loanEndDateStr_g), "%s", AddDays(g_CareerHub.currentDate, 180).c_str());
        s_openLoanPopup_g = true;
      }
    }
    ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
    ImGui::Dummy(ImVec2(0, kGap));
  } else if (loanedOutOwnedPlayer) {
    ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("On loan from your club");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    ImGui::Dummy(ImVec2(0, kGap));
  } else {
    bool listed = false;
    long long askVal = pl.playerValue > 0 ? pl.playerValue : 0;
    {
      std::stringstream mq;
      mq << "SELECT status, COALESCE(NULLIF(asking_price,0), " << askVal << ")"
         << " FROM player_market_status"
         << " WHERE manager_id=" << g_CareerHub.managerId
         << " AND player_id=" << pl.id
         << " AND status='transfer_listed'"
         << " LIMIT 1;";
      DatabaseResult *mr = GetDB()->Query(mq.str().c_str());
      if (mr && !mr->data.empty()) {
        listed = true;
        askVal = atoll(DBCell(mr,0,1).c_str());
      }
      if (mr) delete mr;
    }

    ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
    if (listed) {
      if (ImGui::Button("Remove from Transfer Market", ImVec2(206.0f, 28.0f))) {
        std::stringstream dq;
        dq << "DELETE FROM player_market_status WHERE manager_id=" << g_CareerHub.managerId
           << " AND player_id=" << pl.id
           << " AND status='transfer_listed';";
        delete GetDB()->Query(dq.str().c_str());
      }
    } else {
      if (ImGui::Button("List on Transfer Market", ImVec2(178.0f, 28.0f))) {
        s_listPlayerId_g = pl.id;
        std::string pn = pl.firstName + " " + pl.lastName;
        snprintf(s_listPlayerName_g, sizeof(s_listPlayerName_g), "%s", pn.c_str());
        snprintf(s_listAskingValueStr_g, sizeof(s_listAskingValueStr_g), "%lld", askVal);
        s_openListPlayerPopup_g = true;
      }
    }
    ImGui::SameLine(0.0f, 8.0f);
    bool loanListed = false;
    {
      std::stringstream lq;
      lq << "SELECT 1 FROM player_market_status WHERE manager_id=" << g_CareerHub.managerId
         << " AND player_id=" << pl.id << " AND status='loan_listed' LIMIT 1;";
      DatabaseResult *lr = GetDB()->Query(lq.str().c_str());
      loanListed = lr && !lr->data.empty();
      if (lr) delete lr;
    }
    if (loanListed) {
      if (ImGui::Button("Remove from Loan List", ImVec2(172.0f, 28.0f))) {
        std::stringstream dq;
        dq << "DELETE FROM player_market_status WHERE manager_id=" << g_CareerHub.managerId
           << " AND player_id=" << pl.id << " AND status='loan_listed';";
        delete GetDB()->Query(dq.str().c_str());
      }
    } else if (ImGui::Button("Loan List", ImVec2(94.0f, 28.0f))) {
      std::stringstream iq;
      iq << "INSERT OR REPLACE INTO player_market_status(manager_id,player_id,status,set_date,asking_price)"
         << " VALUES(" << g_CareerHub.managerId << "," << pl.id << ",'loan_listed','"
         << g_CareerHub.currentDate << "',0);";
      delete GetDB()->Query(iq.str().c_str());
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (ImGui::Button("Offer Loan", ImVec2(104.0f, 28.0f))) {
      s_loanPlayerId_g = pl.id;
      s_loanParentClubId_g = g_CareerHub.clubId;
      s_loanReceivingClubId_g = 0;
      s_loanIsLoanIn_g = false;
      s_loanClubSearchStr_g[0] = '\0';
      snprintf(s_loanPlayerName_g, sizeof(s_loanPlayerName_g), "%s %s", pl.firstName.c_str(), pl.lastName.c_str());
      snprintf(s_loanFeeStr_g, sizeof(s_loanFeeStr_g), "%d", 0);
      snprintf(s_loanWagePctStr_g, sizeof(s_loanWagePctStr_g), "%d", 70);
      snprintf(s_loanOptionFeeStr_g, sizeof(s_loanOptionFeeStr_g), "%d", 0);
      snprintf(s_loanMandatoryFeeStr_g, sizeof(s_loanMandatoryFeeStr_g), "%d", 0);
      snprintf(s_loanMandatoryAppsStr_g, sizeof(s_loanMandatoryAppsStr_g), "%d", 0);
      s_loanMandatoryModeIdx_g = 0;
      snprintf(s_loanEndDateStr_g, sizeof(s_loanEndDateStr_g), "%s", AddDays(g_CareerHub.currentDate, 180).c_str());
      s_openLoanPopup_g = true;
    }
    ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
    ImGui::Dummy(ImVec2(0, kGap));
  }

  // ===========================================================
  // Tab row
  // ===========================================================
  static const char *kDetailTabs[] = {"Overview", "Personal", "Performance", "Career"};
  const float kTabH = 34.0f;
  ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
  ImVec2 tabOrg = ImGui::GetCursorScreenPos();
  ImDrawList *tdl = ImGui::GetWindowDrawList();
  {
    float tx2 = tabOrg.x;
    for (int ti = 0; ti < 4; ti++) {
      const float tw = 110.0f;
      bool sel = (s_plTab == ti);
      ImGui::SetCursorScreenPos(ImVec2(tx2, tabOrg.y));
      char tbId[16]; snprintf(tbId, sizeof(tbId), "##plt_%d", ti);
      bool clicked = ImGui::InvisibleButton(tbId, ImVec2(tw, kTabH));
      bool hov = ImGui::IsItemHovered();
      if (sel)
        tdl->AddRectFilled(ImVec2(tx2, tabOrg.y), ImVec2(tx2 + tw, tabOrg.y + kTabH),
                           IM_COL32(accR, accG, accB, 40));
      else if (hov)
        tdl->AddRectFilled(ImVec2(tx2, tabOrg.y), ImVec2(tx2 + tw, tabOrg.y + kTabH),
                           IM_COL32(accR, accG, accB, 20));
      if (sel)
        tdl->AddLine(ImVec2(tx2 + 2, tabOrg.y + kTabH - 1),
                     ImVec2(tx2 + tw - 2, tabOrg.y + kTabH - 1),
                     IM_COL32(accR, accG, accB, 220), 2.0f);
      PushMgrFont(g_ManagerFontSmall);
      ImVec2 tsz = g_ManagerFontSmall
          ? g_ManagerFontSmall->CalcTextSizeA(16.0f, FLT_MAX, 0, kDetailTabs[ti])
          : ImGui::CalcTextSize(kDetailTabs[ti]);
      tdl->AddText(g_ManagerFontSmall, 16.0f,
                   ImVec2(tx2 + (tw - tsz.x) * 0.5f, tabOrg.y + (kTabH - 16.0f) * 0.5f),
                   sel ? IM_COL32(accR, accG, accB, 255) : IM_COL32(140, 155, 185, 200),
                   kDetailTabs[ti]);
      PopMgrFont(g_ManagerFontSmall);
      if (clicked) s_plTab = ti;
      tx2 += tw;
    }
  }
  // Scout button — right side of tab row, non-squad players only
  if (!ownPlayer && !loanedOutOwnedPlayer) {
    const CareerHubState::StaffMember *scout = nullptr;
    for (const auto &sm : g_CareerHub.staff)
      if (sm.role == "Scout") { scout = &sm; break; }

    bool inQueue = false;
    for (const auto &sq : g_CareerHub.scoutQueue)
      if (sq.playerId == pl.id) { inQueue = true; break; }

    bool hasPriorReport = false;
    for (const auto &sr : g_CareerHub.scoutReports)
      if (sr.playerId == pl.id) { hasPriorReport = true; break; }

    const float kBtnW = 148.0f, kBtnH = 26.0f;
    float btnX = tabOrg.x + usW - kBtnW;
    float btnY2 = tabOrg.y + (kTabH - kBtnH) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(btnX, btnY2));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    PushMgrFont(g_ManagerFontSmall);

    if (inQueue) {
      ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(30, 55, 100, 200));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(30, 55, 100, 200));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(30, 55, 100, 200));
      ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(130, 160, 210, 200));
      ImGui::BeginDisabled(true);
      ImGui::Button("Waiting on Report", ImVec2(kBtnW, kBtnH));
      ImGui::EndDisabled();
      ImGui::PopStyleColor(4);
    } else if (!scout) {
      ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(30, 38, 65, 160));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 50, 80, 180));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(40, 50, 80, 180));
      ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(100, 110, 140, 160));
      ImGui::BeginDisabled(true);
      ImGui::Button("Scout Player", ImVec2(kBtnW, kBtnH));
      ImGui::EndDisabled();
      ImGui::PopStyleColor(4);
      if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("You need to hire a Scout first");
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button,        C32(kAccent));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C32(kAccentH));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  C32(kAccentA));
      const char *lbl = hasPriorReport ? "Re-Scout Player" : "Scout Player";
      if (ImGui::Button(lbl, ImVec2(kBtnW, kBtnH)))
        StartScouting(g_CareerHub.managerId, pl.id,
                      pl.firstName, pl.lastName, s_detailClubName,
                      scout->rating, g_CareerHub.currentDate);
      ImGui::PopStyleColor(3);
    }

    PopMgrFont(g_ManagerFontSmall);
    ImGui::PopStyleVar();
  }

  ImGui::SetCursorScreenPos(ImVec2(tabOrg.x, tabOrg.y + kTabH));
  ImGui::Dummy(ImVec2(0, kGap));

  // ===========================================================
  // Stats area
  // ===========================================================
  float statsH = usH - kHdrH - kGap - kTabH - kGap * 2.0f - 8.0f;
  if (statsH < 80.0f) statsH = 80.0f;
  ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
  BeginModernCard("##pldstats", ImVec2(usW, statsH));

  if (s_plTab != 0) {
    ImGui::Dummy(ImVec2(0, 20.0f));
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    float phW = ImGui::CalcTextSize("Coming Soon").x;
    ImGui::SetCursorPosX((usW - 28.0f - phW) * 0.5f);
    ImGui::TextUnformatted("Coming Soon");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);
    EndModernCard();
    return;
  }

  // Determine if this player is a goalkeeper
  bool isGK = (pl.role == "GK");

  // Build real attribute arrays from DB values
  // hiddenIdx values: unique per stat across all sections for scouting fog
  const StatDef kAttackStats[] = {
    {"Finishing",          pl.atFinishing,          0},
    {"Shot Power",         pl.atShotPower,           1},
    {"Long Shots",         pl.atLongShots,           2},
    {"Volleys",            pl.atVolleys,             3},
    {"Penalties",          pl.atPenalties,           4},
    {"Curve",              pl.atCurve,               5},
    {"FK Accuracy",        pl.atFkAccuracy,          6},
    {"Heading Accuracy",   pl.atHeadingAccuracy,     7},
  };
  const StatDef kTechStats[] = {
    {"Ball Control",       pl.atBallControl,         8},
    {"Dribbling",          pl.atDribbling,           9},
    {"Crossing",           pl.atCrossing,           10},
    {"Short Passing",      pl.atShortPassing,       11},
    {"Long Passing",       pl.atLongPassing,        12},
    {"Vision",             pl.atVision,             13},
  };
  const StatDef kDefStats[] = {
    {"Def. Awareness",     pl.atDefensiveAwareness, 14},
    {"Standing Tackle",    pl.atStandingTackle,     15},
    {"Sliding Tackle",     pl.atSlidingTackle,      16},
    {"Interceptions",      pl.atInterceptions,      17},
  };
  const StatDef kPhysStats[] = {
    {"Acceleration",       pl.atAcceleration,       18},
    {"Sprint Speed",       pl.atSprintSpeed,        19},
    {"Agility",            pl.atAgility,            20},
    {"Balance",            pl.atBalance,            21},
    {"Jumping",            pl.atJumping,            22},
    {"Strength",           pl.atStrength,           23},
    {"Reactions",          pl.atReactions,          24},
    {"Stamina",            pl.stamina,              25},
  };
  const StatDef kMentStats[] = {
    {"Aggression",         pl.atAggression,         26},
    {"Composure",          pl.atComposure,          27},
    {"Positioning",        pl.atPositioning,        28},
  };
  const StatDef kGKStats[] = {
    {"GK Diving",          pl.atGkDiving,           29},
    {"GK Handling",        pl.atGkHandling,         30},
    {"GK Kicking",         pl.atGkKicking,          31},
    {"GK Reflexes",        pl.atGkReflexes,         32},
    {"GK Positioning",     pl.atGkPositioning,      33},
  };

  float availW = ImGui::GetContentRegionAvail().x;
  float colH   = statsH - 28.0f;
  if (colH < 40.0f) colH = 40.0f;

  const float c0W = availW * 0.25f;
  const float c1W = availW * 0.25f;
  const float c2W = availW * 0.22f;
  const float c3W = availW - c0W - c1W - c2W - 8.0f;

  // Column 0: Attacking + Technical
  ImGui::BeginChild("##pdc0", ImVec2(c0W, colH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  DrawStatSection("Attacking", IM_COL32(220,100,100,230),
                  kAttackStats, 8, pl.id, fullAttributeAccess);
  DrawStatSection("Technical", IM_COL32(130,185,130,230),
                  kTechStats,  6, pl.id, fullAttributeAccess);
  ImGui::EndChild();
  ImGui::SameLine(0, 2.0f);

  // Column 1: Defending + Mental
  ImGui::BeginChild("##pdc1", ImVec2(c1W, colH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  DrawStatSection("Defending", IM_COL32(80,140,220,230),
                  kDefStats, 4, pl.id, fullAttributeAccess);
  DrawStatSection("Mental", IM_COL32(130,150,220,230),
                  kMentStats, 3, pl.id, fullAttributeAccess);
  ImGui::EndChild();
  ImGui::SameLine(0, 2.0f);

  // Column 2: Physical + Goalkeeping (GK stats only shown for GKs)
  ImGui::BeginChild("##pdc2", ImVec2(c2W, colH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  DrawStatSection("Physical", IM_COL32(220,140,100,230),
                  kPhysStats, 8, pl.id, fullAttributeAccess);
  if (isGK)
    DrawStatSection("Goalkeeping", IM_COL32(60, 220, 200, 255),
                    kGKStats, 5, pl.id, fullAttributeAccess);
  ImGui::EndChild();
  ImGui::SameLine(0, 2.0f);

  // Column 3: Info + Foot + Spider + Pros/Cons
  ImGui::BeginChild("##pdc3", ImVec2(c3W, colH), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  {
    ImDrawList *c3dl = ImGui::GetWindowDrawList();
    float c3avail = ImGui::GetContentRegionAvail().x;
    const float kFs = 15.0f, kRowH = 26.0f;
    const float kTFs = 13.0f; // section title font size

    // ---- Info section -------------------------------------------------------
    ImVec2 cp = ImGui::GetCursorScreenPos();
    PushMgrFont(g_ManagerFontSmall);
    c3dl->AddText(g_ManagerFontSmall, kTFs, ImVec2(cp.x + 4.0f, cp.y + 2.0f),
                  IM_COL32(180,190,215,220), "Info");
    PopMgrFont(g_ManagerFontSmall);
    c3dl->AddLine(ImVec2(cp.x, cp.y + 19.0f), ImVec2(cp.x + c3avail, cp.y + 19.0f),
                  IM_COL32(60, 80, 130, 55), 0.5f);
    ImGui::Dummy(ImVec2(c3avail, 23.0f));

    // Build info rows
    char heightBuf[16] = "-";
    if (pPlayer->height > 0.5f) {
      int hcm = (int)roundf(pPlayer->height * 100.0f);
      snprintf(heightBuf, sizeof(heightBuf), "%d cm", hcm);
    }
    char weightBuf[16] = "-";
    if (pPlayer->weight > 0.5f)
      snprintf(weightBuf, sizeof(weightBuf), "%.0f kg", pPlayer->weight);

    // International reputation (1-5 scale)
    const char *repLabel = "-";
    ImU32 repCol = IM_COL32(212,220,238,235);
    switch (pPlayer->intlReputation) {
      case 5: repLabel = "GOAT Status"; repCol = IM_COL32(255,215, 40,255); break;
      case 4: repLabel = "World Star";  repCol = IM_COL32(120,200,255,255); break;
      case 3: repLabel = "Respected";   repCol = IM_COL32( 80,215,105,255); break;
      case 2: repLabel = "Barely Known";repCol = IM_COL32(215,195, 55,255); break;
      case 1: repLabel = "No Namer";    repCol = IM_COL32(160,170,195,200); break;
    }

    struct InfoRow { const char *lbl; const char *val; ImU32 col; };
    InfoRow infoRows[] = {
      {"Height",     heightBuf,                      IM_COL32(212,220,238,235)},
      {"Weight",     weightBuf,                      IM_COL32(212,220,238,235)},
      {"Nationality",pPlayer->nationality.empty() ? "-" : pPlayer->nationality.c_str(),
                                                     IM_COL32(212,220,238,235)},
      {"Reputation", repLabel,                        repCol},
    };
    for (int i = 0; i < 4; i++) {
      cp = ImGui::GetCursorScreenPos();
      ImU32 rowBg = (i%2==0) ? IM_COL32(15,22,46,130) : IM_COL32(10,16,34,60);
      c3dl->AddRectFilled(cp, ImVec2(cp.x+c3avail, cp.y+kRowH), rowBg);
      PushMgrFont(g_ManagerFontSmall);
      c3dl->AddText(g_ManagerFontSmall, kFs,
                    ImVec2(cp.x+6.0f, cp.y+(kRowH-kFs)*0.5f),
                    IM_COL32(172,182,212,215), infoRows[i].lbl);
      c3dl->AddText(g_ManagerFontSmall, kFs,
                    ImVec2(cp.x + c3avail*0.52f, cp.y+(kRowH-kFs)*0.5f),
                    infoRows[i].col, infoRows[i].val);
      PopMgrFont(g_ManagerFontSmall);
      ImGui::Dummy(ImVec2(c3avail, kRowH));
    }
    ImGui::Dummy(ImVec2(0, 6.0f));

    // ---- Skill Moves section ------------------------------------------------
    cp = ImGui::GetCursorScreenPos();
    PushMgrFont(g_ManagerFontSmall);
    c3dl->AddText(g_ManagerFontSmall, kTFs, ImVec2(cp.x+4.0f, cp.y+2.0f),
                  IM_COL32(180,190,215,220), "Skill Moves");
    PopMgrFont(g_ManagerFontSmall);
    c3dl->AddLine(ImVec2(cp.x, cp.y+19.0f), ImVec2(cp.x+c3avail, cp.y+19.0f),
                  IM_COL32(60,80,130,55), 0.5f);
    ImGui::Dummy(ImVec2(c3avail, 23.0f));
    {
      cp = ImGui::GetCursorScreenPos();
      // Draw 5 stars, filled = skillMoves count, gold if filled, dim if not
      const float kSW = 16.0f, kSH = 12.0f, kSG = 3.0f;
      float sx = cp.x + 6.0f, sy = cp.y + 4.0f;
      int sm = pPlayer->skillMoves;
      for (int i = 0; i < 5; i++) {
        bool filled = (i < sm);
        c3dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx+kSW, sy+kSH),
                            filled ? IM_COL32(220,175,30,235) : IM_COL32(30,38,65,200), 3.0f);
        if (filled)
          c3dl->AddRect(ImVec2(sx, sy), ImVec2(sx+kSW, sy+kSH),
                        IM_COL32(255,210,50,120), 3.0f, 0, 0.8f);
        sx += kSW + kSG;
      }
      ImGui::Dummy(ImVec2(c3avail, kRowH));
    }
    ImGui::Dummy(ImVec2(0, 4.0f));

    // ---- Preferred Foot section ---------------------------------------------
    cp = ImGui::GetCursorScreenPos();
    PushMgrFont(g_ManagerFontSmall);
    c3dl->AddText(g_ManagerFontSmall, kTFs, ImVec2(cp.x + 4.0f, cp.y + 2.0f),
                  IM_COL32(180,190,215,220), "Preferred Foot");
    PopMgrFont(g_ManagerFontSmall);
    c3dl->AddLine(ImVec2(cp.x, cp.y + 19.0f), ImVec2(cp.x + c3avail, cp.y + 19.0f),
                  IM_COL32(60,80,130,55), 0.5f);
    ImGui::Dummy(ImVec2(c3avail, 23.0f));

    bool isLeft  = (!pl.foot.empty() && (pl.foot[0]=='L'||pl.foot[0]=='l'));
    bool isRight = (!pl.foot.empty() && (pl.foot[0]=='R'||pl.foot[0]=='r'));
    int wf = pPlayer->weakFoot; // 1-5; weak foot star count

    // Draw foot label + 5 stars side by side: Left | Right
    // Strong foot = 5 gold stars; weak foot = weakFoot stars
    auto drawFootStars = [&](float bx, float by, bool isStrong, const char *side) {
      PushMgrFont(g_ManagerFontSmall);
      c3dl->AddText(g_ManagerFontSmall, kFs, ImVec2(bx, by),
                    IM_COL32(168,180,212,230), side);
      PopMgrFont(g_ManagerFontSmall);
      int filled = isStrong ? 5 : (wf > 0 ? wf : 1);
      const float kSW = 13.0f, kSH = 10.0f, kSG = 2.0f;
      float sx = bx, sy = by + 18.0f;
      for (int si = 0; si < 5; si++) {
        bool on = (si < filled);
        c3dl->AddRectFilled(ImVec2(sx, sy), ImVec2(sx+kSW, sy+kSH),
                            on ? IM_COL32(220,175,30,235) : IM_COL32(30,38,65,200), 2.0f);
        if (on)
          c3dl->AddRect(ImVec2(sx, sy), ImVec2(sx+kSW, sy+kSH),
                        IM_COL32(255,210,50,100), 2.0f, 0, 0.7f);
        sx += kSW + kSG;
      }
    };
    const float kFootLM = 8.0f;
    float footColW = c3avail * 0.5f - 4.0f - kFootLM * 0.5f;
    float lfx2 = ImGui::GetCursorScreenPos().x + kFootLM;
    float fby   = ImGui::GetCursorScreenPos().y;
    drawFootStars(lfx2,                    fby, isLeft,  "Left");
    drawFootStars(lfx2 + footColW + 8.0f,  fby, isRight, "Right");
    ImGui::Dummy(ImVec2(0, 34.0f));
    ImGui::Dummy(ImVec2(0, 10.0f));

    // ---- Spider / Radar chart -----------------------------------------------
    cp = ImGui::GetCursorScreenPos();
    float spR  = std::min(c3avail * 0.38f, 64.0f);
    float spCx = cp.x + c3avail * 0.5f;
    float spCy = cp.y + spR + 14.0f;

    // Filled concentric rings (subtle)
    for (int ring = 3; ring >= 1; ring--) {
      ImVec2 pts[6];
      for (int ai = 0; ai < 6; ai++) {
        float ang = ai * (2.0f * 3.14159f / 6.0f) - 3.14159f * 0.5f;
        float r   = spR * (ring / 3.0f);
        pts[ai]   = ImVec2(spCx + cosf(ang)*r, spCy + sinf(ang)*r);
      }
      // Ring fill (very subtle gradient from dark to darker)
      c3dl->AddConvexPolyFilled(pts, 6,
          ring == 3 ? IM_COL32(18,25,50,100) :
          ring == 2 ? IM_COL32(16,22,44,100) :
                      IM_COL32(14,19,38,100));
      // Ring outline
      for (int ai = 0; ai < 6; ai++)
        c3dl->AddLine(pts[ai], pts[(ai+1)%6],
                      IM_COL32(50, 68, 115, 110), 0.5f);
    }

    // Axis lines — use real attribute values (0-99 scale)
    static const char *kAxes[] = {"Def", "Phy", "Men", "Tec", "Att", "Spd"};
    int kRadarVals[6] = {
      pl.atDefensiveAwareness,  // Def
      pl.atStrength,             // Phy
      pl.atComposure,            // Men
      pl.atBallControl,          // Tec
      pl.atFinishing,            // Att
      pl.atSprintSpeed,          // Spd
    };
    ImVec2 webPts[6];
    for (int ai = 0; ai < 6; ai++) {
      float ang = ai * (2.0f * 3.14159f / 6.0f) - 3.14159f * 0.5f;
      c3dl->AddLine(ImVec2(spCx, spCy),
                    ImVec2(spCx + cosf(ang)*spR, spCy + sinf(ang)*spR),
                    IM_COL32(55, 72, 120, 150), 0.8f);

      PushMgrFont(g_ManagerFontSmall);
      ImVec2 lsz = g_ManagerFontSmall
          ? g_ManagerFontSmall->CalcTextSizeA(12.0f, FLT_MAX, 0, kAxes[ai])
          : ImGui::CalcTextSize(kAxes[ai]);
      float lblDist = spR + 14.0f;
      c3dl->AddText(g_ManagerFontSmall, 12.0f,
                    ImVec2(spCx + cosf(ang)*lblDist - lsz.x*0.5f,
                           spCy + sinf(ang)*lblDist - lsz.y*0.5f),
                    IM_COL32(145, 158, 192, 215), kAxes[ai]);
      PopMgrFont(g_ManagerFontSmall);

      float r = (kRadarVals[ai] / 99.0f) * spR;
      webPts[ai] = ImVec2(spCx + cosf(ang)*r, spCy + sinf(ang)*r);
    }

    // Data polygon fill + outline
    c3dl->AddConvexPolyFilled(webPts, 6, IM_COL32(accR, accG, accB, 55));
    for (int ai = 0; ai < 6; ai++) {
      c3dl->AddLine(webPts[ai], webPts[(ai+1)%6],
                    IM_COL32(accR, accG, accB, 210), 1.8f);
      c3dl->AddCircleFilled(webPts[ai], 2.5f, IM_COL32(accR, accG, accB, 240));
    }
    ImGui::Dummy(ImVec2(0, spR * 2.0f + 32.0f));

    // ---- Pros / Cons --------------------------------------------------------
    // Pros title
    cp = ImGui::GetCursorScreenPos();
    PushMgrFont(g_ManagerFontSmall);
    c3dl->AddText(g_ManagerFontSmall, kFs, ImVec2(cp.x + 4.0f, cp.y + 2.0f),
                  IM_COL32(80,200,110,230), "Pros");
    PopMgrFont(g_ManagerFontSmall);
    c3dl->AddLine(ImVec2(cp.x, cp.y+15.0f), ImVec2(cp.x+c3avail, cp.y+15.0f),
                  IM_COL32(80,200,110,45), 0.5f);
    ImGui::Dummy(ImVec2(c3avail, 17.0f));
    cp = ImGui::GetCursorScreenPos();
    PushMgrFont(g_ManagerFontSmall);
    c3dl->AddText(g_ManagerFontSmall, kFs, ImVec2(cp.x+6.0f, cp.y+2.0f),
                  IM_COL32(140,155,192,180), "+ (Coming soon)");
    PopMgrFont(g_ManagerFontSmall);
    ImGui::Dummy(ImVec2(c3avail, 16.0f));
    ImGui::Dummy(ImVec2(0, 6.0f));

    // Cons title
    cp = ImGui::GetCursorScreenPos();
    PushMgrFont(g_ManagerFontSmall);
    c3dl->AddText(g_ManagerFontSmall, kFs, ImVec2(cp.x + 4.0f, cp.y + 2.0f),
                  IM_COL32(215,75,75,230), "Cons");
    PopMgrFont(g_ManagerFontSmall);
    c3dl->AddLine(ImVec2(cp.x, cp.y+15.0f), ImVec2(cp.x+c3avail, cp.y+15.0f),
                  IM_COL32(215,75,75,45), 0.5f);
    ImGui::Dummy(ImVec2(c3avail, 17.0f));
    cp = ImGui::GetCursorScreenPos();
    PushMgrFont(g_ManagerFontSmall);
    c3dl->AddText(g_ManagerFontSmall, kFs, ImVec2(cp.x+6.0f, cp.y+2.0f),
                  IM_COL32(140,155,192,180), "- (Coming soon)");
    PopMgrFont(g_ManagerFontSmall);
    ImGui::Dummy(ImVec2(c3avail, 16.0f));
  }
  ImGui::EndChild(); // ##pdc3

  EndModernCard(); // ##pldstats
}

// ---- DrawInboxPage helpers -----------------------------------------------

// Returns an image path to use as the message avatar, or "" for the fallback circle.
static std::string GetInboxAvatarPath(const std::string &senderType) {
  if (senderType == "board")
    return g_CareerHub.club.logoPath;
  if (senderType == "competition") {
    int lid = g_CareerHub.club.leagueId;
    for (const auto &f : g_CareerHub.fixtures)
      if (f.leagueId == lid && !f.leagueLogo.empty())
        return f.leagueLogo;
  }
  return "";
}

// Draw a square avatar: image if available, coloured circle+initial otherwise.
// cx/cy = centre, r = half-size (image drawn as 2r x 2r square).
static void DrawInboxAvatar(ImDrawList *dl, float cx, float cy, float r,
                            const std::string &senderType, ImFont *font, float fsIni) {
  std::string path = GetInboxAvatarPath(senderType);
  GLuint tex = path.empty() ? 0 : LoadBadgeTex(path);
  if (tex) {
    dl->AddImageRounded((ImTextureID)(intptr_t)tex,
                        ImVec2(cx - r, cy - r), ImVec2(cx + r, cy + r),
                        ImVec2(0,0), ImVec2(1,1),
                        IM_COL32(255,255,255,255), r * 0.25f);
  } else {
    dl->AddCircleFilled(ImVec2(cx, cy), r, SenderTypeColor(senderType));
    if (font && !senderType.empty()) {
      char ini[2] = { (char)::toupper((unsigned char)senderType[0]), 0 };
      ImVec2 isz = font->CalcTextSizeA(fsIni, FLT_MAX, 0.0f, ini);
      dl->AddText(font, fsIni,
                  ImVec2(cx - isz.x*0.5f, cy - isz.y*0.5f),
                  IM_COL32(255,255,255,230), ini);
    }
  }

}

// ---- DrawInboxPage ------------------------------------------------------

static void DrawInboxPage(float w, float h) {
  static int s_sel = -1;
  static int s_tab =  0; // 0=All 1=New 2=Unread

  const auto &msgs = g_CareerHub.inbox;
  const int total  = (int)msgs.size();

  if (s_sel >= total) s_sel = -1;

  int cntNew = 0;
  for (const auto &m : msgs) if (!m.isRead) cntNew++;

  // Filter: tab 1=New(unread), tab 2=Unread (same filter)
  std::vector<int> filt;
  filt.reserve(total);
  for (int i = 0; i < total; i++) {
    const auto &m = msgs[i];
    if ((s_tab == 1 || s_tab == 2) && m.isRead) continue;
    filt.push_back(i);
  }

  const float kListW = floorf(w * 0.36f);
  const float kDetW  = w - kListW - 1.0f;
  const int   kAR    = (int)(kAccent.x * 255);
  const int   kAG    = (int)(kAccent.y * 255);
  const int   kAB    = (int)(kAccent.z * 255);

  // Font sizes (+4px from original)
  const float kFsTab  = 18.0f;
  const float kFsName = 18.0f;
  const float kFsSubj = 16.0f;
  const float kFsDate = 16.0f;
  const float kFsIni  = 18.0f;  // avatar initial in list
  const float kFsIni2 = 21.0f;  // avatar initial in detail

  // ---- LEFT PANEL: message list ----------------------------------------
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgSidebar);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##inbx_L", ImVec2(kListW, h), false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor();
  {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2      wp = ImGui::GetWindowPos();
    const float kTabH = 46.0f;

    dl->AddRectFilled(wp, ImVec2(wp.x + kListW, wp.y + kTabH),
                      IM_COL32(10, 16, 38, 255));

    // 3 tabs: All / New / Unread
    struct TabDef { const char *label; int count; };
    TabDef tabs[3] = { {"All",total},{"New",cntNew},{"Unread",cntNew} };
    float tabX = 12.0f;
    for (int t = 0; t < 3; t++) {
      bool act = (s_tab == t);
      char buf[28];
      if (tabs[t].count > 0) snprintf(buf, sizeof(buf), "%s (%d)", tabs[t].label, tabs[t].count);
      else                   snprintf(buf, sizeof(buf), "%s",       tabs[t].label);

      float bw = g_ManagerFontSmall
        ? g_ManagerFontSmall->CalcTextSizeA(kFsTab, FLT_MAX, 0.0f, buf).x + 16.0f
        : ImGui::CalcTextSize(buf).x + 16.0f;

      if (act)
        dl->AddRectFilled(ImVec2(wp.x + tabX, wp.y + kTabH - 2.0f),
                          ImVec2(wp.x + tabX + bw, wp.y + kTabH),
                          IM_COL32(kAR, kAG, kAB, 255));

      ImVec2 tp(wp.x + tabX + 8.0f, wp.y + (kTabH - kFsTab) * 0.5f);
      ImU32  tc = act ? C32(kTextPri) : C32(kTextSec);
      if (g_ManagerFontSmall) dl->AddText(g_ManagerFontSmall, kFsTab, tp, tc, buf);
      else                    dl->AddText(tp, tc, buf);

      ImGui::SetCursorScreenPos(ImVec2(wp.x + tabX, wp.y));
      char bid[12]; snprintf(bid, sizeof(bid), "##itab%d", t);
      if (ImGui::InvisibleButton(bid, ImVec2(bw, kTabH))) s_tab = t;
      tabX += bw;
    }
    dl->AddLine(ImVec2(wp.x, wp.y + kTabH), ImVec2(wp.x + kListW, wp.y + kTabH),
                C32(kBorder), 1.0f);

    // Scrollable list
    ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + kTabH + 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
    ImGui::BeginChild("##inbx_rows", ImVec2(kListW, h - kTabH - 1.0f), false, 0);
    ImGui::PopStyleColor();
    ImDrawList *ldl   = ImGui::GetWindowDrawList();
    const float kRowH = 76.0f;  // taller rows for bigger fonts
    const float kAvR  = 18.0f;  // slightly larger avatar
    const float kLPad = 14.0f;  // left margin inside row

    if (filt.empty()) {
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      const char *empty = s_tab == 0 ? "No messages yet" : "No messages";
      float tw = ImGui::CalcTextSize(empty).x;
      ImGui::SetCursorPos(ImVec2((kListW - tw) * 0.5f, ImGui::GetContentRegionAvail().y * 0.35f));
      ImGui::TextUnformatted(empty);
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);
    }

    for (int fi = 0; fi < (int)filt.size(); fi++) {
      int idx = filt[fi];
      const auto &msg = msgs[idx];
      bool sel = (s_sel == idx);

      ImVec2 rP  = ImGui::GetCursorScreenPos();
      ImVec2 rPx = ImVec2(rP.x + kListW, rP.y + kRowH);
      bool   hov = ImGui::IsMouseHoveringRect(rP, rPx, false);

      if (sel)      ldl->AddRectFilled(rP, rPx, IM_COL32(kAR, kAG, kAB, 28));
      else if (hov) ldl->AddRectFilled(rP, rPx, IM_COL32(255,255,255, 7));
      ldl->AddLine(ImVec2(rP.x+12.0f, rPx.y-1.0f), ImVec2(rPx.x-4.0f, rPx.y-1.0f),
                   C32(kBorder), 1.0f);

      // Unread dot (left edge)
      if (!msg.isRead)
        ldl->AddCircleFilled(ImVec2(rP.x + 5.0f, rP.y + kRowH * 0.5f),
                             5.0f, IM_COL32(kAR, kAG, kAB, 240));

      // Avatar
      float avCX = rP.x + kLPad + kAvR, avCY = rP.y + kRowH * 0.5f;
      DrawInboxAvatar(ldl, avCX, avCY, kAvR, msg.senderType, g_ManagerFontSmall, kFsIni);

      // Text area (sender name + subject)
      float tx  = rP.x + kLPad + kAvR*2.0f + 12.0f;
      float txW = kListW - (tx - rP.x) - 60.0f; // reserve right margin for date
      float topY  = rP.y + kRowH * 0.5f - kFsName - 2.0f;
      float subjY = rP.y + kRowH * 0.5f + 4.0f;

      // Sender name (bold if unread)
      if (g_ManagerFontSmall) {
        ImFont *nf  = msg.isRead ? g_ManagerFontSmall : g_ManagerFontBold;
        ImU32   nc  = msg.isRead ? C32(kTextSec) : C32(kTextPri);
        std::string sn = msg.senderName;
        if (nf) {
          while (sn.size() > 3 &&
                 nf->CalcTextSizeA(kFsName, FLT_MAX, 0.0f, sn.c_str()).x > txW)
            sn.resize(sn.size() - 1);
        }
        ldl->AddText(nf ? nf : g_ManagerFontSmall, kFsName,
                     ImVec2(tx, topY), nc, sn.c_str());
      }

      // Subject (truncated) — white for unread, dim for read
      if (g_ManagerFontSmall) {
        std::string subj = msg.subject;
        while (subj.size() > 3 &&
               g_ManagerFontSmall->CalcTextSizeA(kFsSubj, FLT_MAX, 0.0f, subj.c_str()).x > txW)
          subj.resize(subj.size() - 1);
        ImU32 subjCol = msg.isRead ? C32(kTextDim) : C32(kTextPri);
        ldl->AddText(g_ManagerFontSmall, kFsSubj,
                     ImVec2(tx, subjY), subjCol, subj.c_str());
      }

      // Date top-right (shortened "D Mon")
      if (g_ManagerFontSmall) {
        std::string ds = FormatDateDisplay(msg.gameDate);
        size_t ls = ds.rfind(' ');
        if (ls != std::string::npos) ds = ds.substr(0, ls);
        ImVec2 dsz = g_ManagerFontSmall->CalcTextSizeA(kFsDate, FLT_MAX, 0.0f, ds.c_str());
        ldl->AddText(g_ManagerFontSmall, kFsDate,
                     ImVec2(rPx.x - dsz.x - 8.0f, topY),
                     C32(kTextDim), ds.c_str());
      }

      // Invisible button
      ImGui::SetCursorScreenPos(rP);
      char rid[20]; snprintf(rid, sizeof(rid), "##irow%d", idx);
      if (ImGui::InvisibleButton(rid, ImVec2(kListW, kRowH))) {
        s_sel = idx;
        if (!msgs[idx].isRead) {
          std::stringstream uq;
          uq << "UPDATE manager_inbox SET is_read=1 WHERE id=" << msgs[idx].id << ";";
          DatabaseResult *ur = GetDB()->Query(uq.str()); delete ur;
          g_CareerHub.inbox[idx].isRead = true;
        }
      }
      if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
    }
    ImGui::EndChild(); // inbx_rows
  }
  ImGui::EndChild(); // inbx_L

  // Vertical separator
  ImGui::SameLine(0.0f, 0.0f);
  {
    ImVec2 sp = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(sp, ImVec2(sp.x, sp.y + h), C32(kBorder), 1.0f);
  }
  ImGui::SameLine(0.0f, 1.0f);

  // ---- RIGHT PANEL: message detail -------------------------------------
  const float kP     = 28.0f; // left/right margin
  const float kPTop  = 24.0f; // top margin
  const float kInnerW = kDetW - kP * 2.0f;
  ImGui::PushStyleColor(ImGuiCol_ChildBg, kBgApp);
  ImGui::BeginChild("##inbx_R", ImVec2(kDetW, h), false, 0);
  ImGui::PopStyleColor();
  {
    if (s_sel < 0 || s_sel >= total) {
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      const char *hint = "Select a message to read it";
      float tw = ImGui::CalcTextSize(hint).x;
      ImGui::SetCursorPos(ImVec2((kDetW - tw) * 0.5f, h * 0.42f));
      ImGui::TextUnformatted(hint);
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);
    } else {
      const auto &msg = msgs[s_sel];
      ImDrawList *rdl = ImGui::GetWindowDrawList();

      // Explicit top+left margin via cursor position
      ImGui::SetCursorPos(ImVec2(kP, kPTop));

      // Sender header: avatar circle + name/date vertically centered beside it
      {
        const float kAvR2  = 24.0f;
        const float kAvDiam = kAvR2 * 2.0f;
        ImVec2 avP = ImGui::GetCursorScreenPos();

        DrawInboxAvatar(rdl, avP.x + kAvR2, avP.y + kAvR2, kAvR2,
                        msg.senderType, g_ManagerFontBold, kFsIni2);

        // Reserve avatar footprint then place text beside it
        ImGui::Dummy(ImVec2(kAvDiam + 14.0f, kAvDiam));
        ImGui::SameLine(0.0f, 0.0f);

        // Vertically center name+date block against the avatar diameter
        float nameH = g_ManagerFontBold  ? g_ManagerFontBold->FontSize  : 20.0f;
        float dateH = g_ManagerFontSmall ? g_ManagerFontSmall->FontSize : 15.0f;
        float blockH = nameH + 4.0f + dateH;
        float vOff   = floorf((kAvDiam - blockH) * 0.5f);
        if (vOff < 0.0f) vOff = 0.0f;
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + vOff);

        ImGui::BeginGroup();
        PushMgrFont(g_ManagerFontBold);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
        ImGui::TextUnformatted(msg.senderName.c_str());
        ImGui::PopStyleColor();
        PopMgrFont(g_ManagerFontBold);
        PushMgrFont(g_ManagerFontSmall);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::TextUnformatted(FormatDateDisplay(msg.gameDate).c_str());
        ImGui::PopStyleColor();
        PopMgrFont(g_ManagerFontSmall);
        ImGui::EndGroup();
      }

      ImGui::Dummy(ImVec2(0.0f, 10.0f));
      ImGui::SetCursorPosX(kP);

      // Subject
      PushMgrFont(g_ManagerFontBold);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
      ImGui::PushTextWrapPos(kP + kInnerW);
      ImGui::TextUnformatted(msg.subject.c_str());
      ImGui::PopTextWrapPos();
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontBold);

      ImGui::Dummy(ImVec2(0.0f, 14.0f));
      ImGui::SetCursorPosX(kP);

      // Divider
      {
        ImVec2 dp = ImGui::GetCursorScreenPos();
        rdl->AddLine(dp, ImVec2(dp.x + kInnerW, dp.y), C32(kBorder), 1.0f);
        ImGui::Dummy(ImVec2(0.0f, 14.0f));
      }

      // Body — scrollable, 17px font
      {
        ImGui::SetCursorPosX(kP);
        float bodyH = ImGui::GetContentRegionAvail().y;
        if (bodyH < 40.0f) bodyH = 40.0f;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("##inbx_body", ImVec2(kInnerW, bodyH), false, 0);
        ImGui::PopStyleColor();
        PushMgrFont(g_ManagerFontRegular);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
        ImGui::PushTextWrapPos(kInnerW);
        ImGui::TextUnformatted(msg.body.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
        PopMgrFont(g_ManagerFontRegular);
        ImGui::EndChild();
      }
    }
  }
  ImGui::EndChild(); // inbx_R
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
  ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
  BeginModernCard("##sqtbl", ImVec2(usW, squadH));
  float tblH = squadH - 32.0f;
  if (tblH < 30.0f) tblH = 30.0f;

  // Sort state — persists across frames, reset when player list changes size
  static std::vector<int> s_squadSortIdx;
  static int  s_squadSortLastSize = -1;

  ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(5.0f, 4.0f));
  // Col indices: 0=POS 1=Name 2=Position 3=AltPos 4=Wage 5=Age 6=Foot 7=Expires 8=Ability 9=Potential
  //              10=Stamina 11=Status 12=Morale 13=Happiness 14=L5 15=Season
  static const int kColAge = 5, kColFoot = 6, kColAbility = 8, kColPotential = 9;
  if (ImGui::BeginTable("##sqfm", 16,
        ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_PadOuterX |
        ImGuiTableFlags_ScrollX |
        ImGuiTableFlags_Sortable | ImGuiTableFlags_SortTristate,
        ImVec2(0, tblH))) {
    ImGui::TableSetupScrollFreeze(2, 1);
    ImGui::TableSetupColumn("POS",       ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  54.0f);
    ImGui::TableSetupColumn("Name",      ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 220.0f);
    ImGui::TableSetupColumn("Position",  ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  90.0f);
    ImGui::TableSetupColumn("Alt Pos",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 110.0f);
    ImGui::TableSetupColumn("Wage",      ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  88.0f);
    ImGui::TableSetupColumn("Age",       ImGuiTableColumnFlags_WidthFixed,  36.0f);
    ImGui::TableSetupColumn("Foot",      ImGuiTableColumnFlags_WidthFixed,  36.0f);
    ImGui::TableSetupColumn("Expires",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  72.0f);
    ImGui::TableSetupColumn("Ability",   ImGuiTableColumnFlags_WidthFixed,  60.0f);
    ImGui::TableSetupColumn("Potential", ImGuiTableColumnFlags_WidthFixed,  60.0f);
    ImGui::TableSetupColumn("Stamina",   ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  72.0f);
    ImGui::TableSetupColumn("Status",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  56.0f);
    ImGui::TableSetupColumn("Morale",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  60.0f);
    ImGui::TableSetupColumn("Happiness", ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  72.0f);
    ImGui::TableSetupColumn("L5",        ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort,  60.0f);
    ImGui::TableSetupColumn("Season",    ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 104.0f);
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
      bool unavailable = (p.injuryDays > 0 || p.suspensionMatches > 0);
      ImGui::TableNextRow(0, unavailable ? 34.0f : 28.0f);

      // POS — dropdown button
      ImGui::TableSetColumnIndex(0);
      {
        const char *posLbl = PosLabel(p.formationOrder);
        bool isSub  = (p.formationOrder >= 11);
        bool isXI   = (p.formationOrder >= 0 && p.formationOrder <= 10);
        ImU32 badgeBg;
        if (unavailable) {
          badgeBg = p.injuryDays > 0 ? IM_COL32(170, 55, 65, 225)
                                     : IM_COL32(205, 150, 45, 225);
        } else if (isXI) {
          int fo = p.formationOrder;
          if      (fo == 0)  badgeBg = IM_COL32(220, 160,  30, 220); // GK  gold
          else if (fo <= 4)  badgeBg = IM_COL32( 80, 150, 255, 220); // DEF blue
          else if (fo <= 7)  badgeBg = IM_COL32( 80, 210, 110, 220); // MID green
          else               badgeBg = IM_COL32(255,  90,  70, 220); // FWD red
        } else if (isSub) {
          badgeBg = IM_COL32(30, 60, 100, 220);
        } else {
          badgeBg = IM_COL32(25, 35, 58, 180);
        }
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

        if (clicked && !unavailable) {
          char popId[32]; snprintf(popId, sizeof(popId), "##posdd_%d", p.id);
          ImGui::OpenPopup(popId);
        }
        if (ImGui::IsItemHovered() && unavailable) {
          std::string tip = p.injuryDays > 0
            ? ("Injured: " + p.injuryType + " (" + int_to_str(p.injuryDays) + "d)")
            : ("Suspended: " + int_to_str(p.suspensionMatches) + " match");
          ImGui::SetTooltip("%s", tip.c_str());
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

      // Name — click opens player detail page
      ImGui::TableSetColumnIndex(1);
      {
        std::string name = DisplayName(p);
        ImVec2 cp2 = ImGui::GetCursorScreenPos();
        ImDrawList *ndl = ImGui::GetWindowDrawList();
        char nbtnId[32]; snprintf(nbtnId, sizeof(nbtnId), "##plnm_%d", p.id);
        ImGui::InvisibleButton(nbtnId, ImVec2(170.0f, 22.0f));
        bool nhov     = ImGui::IsItemHovered();
        bool nclicked = ImGui::IsItemClicked();
        int ar2 = (int)(kAccent.x*255), ag2 = (int)(kAccent.y*255), ab2 = (int)(kAccent.z*255);
        ImU32 nCol = unavailable ? IM_COL32(170, 180, 205, 210)
                                  : (nhov ? IM_COL32(ar2, ag2, ab2, 255) : IM_COL32(220, 230, 248, 230));
        ndl->AddText(g_ManagerFontSmall, 17.0f, ImVec2(cp2.x, cp2.y + 2.0f), nCol, name.c_str());
        if (unavailable) {
          std::string status = p.injuryDays > 0
            ? ("Injured " + int_to_str(p.injuryDays) + "d")
            : ("Suspended " + int_to_str(p.suspensionMatches));
          ndl->AddText(g_ManagerFontSmall, 11.0f, ImVec2(cp2.x, cp2.y + 20.0f),
                       p.injuryDays > 0 ? IM_COL32(245, 105, 115, 230)
                                        : IM_COL32(245, 190, 85, 230),
                       status.c_str());
        }
        if (nhov) {
          ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          float nsz = g_ManagerFontSmall
              ? g_ManagerFontSmall->CalcTextSizeA(17.0f, FLT_MAX, 0, name.c_str()).x
              : ImGui::CalcTextSize(name.c_str()).x;
          ndl->AddLine(ImVec2(cp2.x, cp2.y + 19.0f),
                       ImVec2(cp2.x + nsz, cp2.y + 19.0f),
                       IM_COL32(ar2, ag2, ab2, 180), 1.0f);
        }
        if (nclicked && !s_escMenuOpen) {
          s_playerDetailId       = p.id;
          s_detailOverrideActive = false; // squad player — no override needed
          s_detailClubName = s_detailClubLogo = s_detailClubShortName = "";
          NavPush(PAGE_PLAYER_DETAIL);
        }
      }

      // Position (role) — col 2
      ImGui::TableSetColumnIndex(2);
      {
        ImVec2 rcp = ImGui::GetCursorScreenPos();
        ImDrawList *rdl = ImGui::GetWindowDrawList();
        rdl->AddText(g_ManagerFontSmall, 17.0f, ImVec2(rcp.x, rcp.y + 4.0f),
                     IM_COL32(148,162,196,220), p.role.c_str());
        ImGui::Dummy(ImVec2(80.0f, 20.0f));
      }

      // Alternative positions — col 3
      ImGui::TableSetColumnIndex(3);
      {
        auto altPosVec = ParseAltPositions(p.alternativePos);
        ImVec2 acp = ImGui::GetCursorScreenPos();
        ImDrawList *adl = ImGui::GetWindowDrawList();
        float ax = acp.x;
        for (const auto &ap : altPosVec) {
          ImVec2 aSz = g_ManagerFontSmall
              ? g_ManagerFontSmall->CalcTextSizeA(13.0f, FLT_MAX, 0, ap.c_str())
              : ImGui::CalcTextSize(ap.c_str());
          float bW = aSz.x + 10.0f, bH = 16.0f;
          if (ax + bW > acp.x + 106.0f) break;
          float by = acp.y + (28.0f - bH) * 0.5f;
          adl->AddRectFilled(ImVec2(ax, by), ImVec2(ax+bW, by+bH),
                             IM_COL32(50,65,110,180), 3.0f);
          adl->AddRect(ImVec2(ax, by), ImVec2(ax+bW, by+bH),
                       IM_COL32(80,105,165,160), 3.0f, 0, 0.7f);
          adl->AddText(g_ManagerFontSmall, 13.0f,
                       ImVec2(ax+5.0f, by+(bH-13.0f)*0.5f),
                       IM_COL32(160,175,210,220), ap.c_str());
          ax += bW + 3.0f;
        }
        ImGui::Dummy(ImVec2(106.0f, 20.0f));
      }

      // Wage — col 4
      ImGui::TableSetColumnIndex(4);
      if (p.weeklywage > 0) {
        char wbuf[32];
        if (p.weeklywage >= 1000)
          snprintf(wbuf, sizeof(wbuf), "\xe2\x82\xac%d,%03d p/w",
                   p.weeklywage/1000, p.weeklywage%1000);
        else
          snprintf(wbuf, sizeof(wbuf), "\xe2\x82\xac%d p/w", p.weeklywage);
        ImVec2 wcp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(g_ManagerFontSmall, 17.0f,
            ImVec2(wcp.x, wcp.y + 4.0f), IM_COL32(148,162,196,220), wbuf);
        ImGui::Dummy(ImVec2(84.0f, 20.0f));
      } else {
        ImGui::PushStyleColor(ImGuiCol_Text, kDimU);
        ImGui::TextUnformatted("\xe2\x80\x94");
        ImGui::PopStyleColor();
      }

      // Age — col 5
      ImGui::TableSetColumnIndex(5);
      {
        ImVec2 acp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(g_ManagerFontSmall, 17.0f,
            ImVec2(acp.x, acp.y + 4.0f), IM_COL32(148,162,196,220), p.age.c_str());
        ImGui::Dummy(ImVec2(32.0f, 20.0f));
      }

      // Foot (col 6) — "L" → left foot badge, "R" → right foot badge
      ImGui::TableSetColumnIndex(6);
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

      // Contract expiry (col 7)
      ImGui::TableSetColumnIndex(7);
      {
        std::string exp = FormatContractExpiry(p.contractExpiry);
        const char *expStr = (exp == "-") ? "\xe2\x80\x94" : exp.c_str();
        ImVec2 ecp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(g_ManagerFontSmall, 17.0f,
            ImVec2(ecp.x, ecp.y + 4.0f), IM_COL32(148,162,196,220), expStr);
        ImGui::Dummy(ImVec2(68.0f, 20.0f));
      }

      // Ability stars (col 8)
      ImGui::TableSetColumnIndex(8);
      DrawStars(p.baseStat, 1.0f, kGoldU);

      // Potential stars (col 9)
      ImGui::TableSetColumnIndex(9);
      DrawStars((float)p.potential, 200.0f, kBlueU);

      // Placeholders
      auto placeholder = [&](int col) {
        ImGui::TableSetColumnIndex(col);
        ImGui::PushStyleColor(ImGuiCol_Text, kDimU);
        ImGui::TextUnformatted("\xe2\x80\x94");
        ImGui::PopStyleColor();
      };
      // Stamina bar (col 10) — based on current condition.
      ImGui::TableSetColumnIndex(10);
      {
        int cs = p.currentStamina; // 0-100
        if (cs < 0) cs = 0; if (cs > 100) cs = 100;
        ImVec2 bcp = ImGui::GetCursorScreenPos();
        ImDrawList *sdl = ImGui::GetWindowDrawList();
        const float bW = 60.0f, bH = 8.0f;
        float bY = bcp.y + 5.0f;
        sdl->AddRectFilled(ImVec2(bcp.x, bY), ImVec2(bcp.x+bW, bY+bH),
                           IM_COL32(22,30,58,210), 3.0f);
        if (cs > 0) {
          ImU32 sCol = cs >= 70 ? IM_COL32(80,215,105,200)
                    : cs >= 40 ? IM_COL32(215,195,55,215)
                               : IM_COL32(215,80,80,200);
          sdl->AddRectFilled(ImVec2(bcp.x, bY),
                             ImVec2(bcp.x + bW*(cs/100.0f), bY+bH),
                             sCol, 3.0f);
        }
        ImGui::Dummy(ImVec2(bW, bH + 10.0f));
      }
      // Availability status (col 11)
      ImGui::TableSetColumnIndex(11);
      if (p.injuryDays > 0 || p.suspensionMatches > 0) {
        std::string status = p.injuryDays > 0
          ? ("INJ " + int_to_str(p.injuryDays) + "d")
          : ("SUS " + int_to_str(p.suspensionMatches));
        ImVec2 scp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(g_ManagerFontSmall, 14.0f,
            ImVec2(scp.x, scp.y + 4.0f),
            p.injuryDays > 0 ? IM_COL32(245,105,115,230)
                             : IM_COL32(245,190,85,230),
            status.c_str());
        ImGui::Dummy(ImVec2(54.0f, 18.0f));
      } else {
        placeholder(11);
      }
      placeholder(12); // Morale
      placeholder(13); // Happiness
      placeholder(14); // L5
      // Season stats (col 15): appearances, goals/assists and average rating.
      ImGui::TableSetColumnIndex(15);
      if (p.matchesPlayed > 0) {
        char sbuf[64];
        snprintf(sbuf, sizeof(sbuf), "%d  %dG/%dA  %.2f",
                 p.matchesPlayed, p.goals, p.assists, p.avgRating);
        ImVec2 stp = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddText(g_ManagerFontSmall, 14.0f,
            ImVec2(stp.x, stp.y + 4.0f), IM_COL32(170,190,225,230), sbuf);
        ImGui::Dummy(ImVec2(98.0f, 18.0f));
      } else {
        placeholder(15);
      }
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
static std::string s_calLastCareerYM; // "YYYY-MM" of career date when display was last synced

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
  // Reset to current career month on entry; also live-follow if career crosses a month boundary.
  const std::string &cd = g_CareerHub.currentDate;
  if (!s_calInit) {
    if (cd.size() >= 7) {
      s_calYear  = atoi(cd.substr(0, 4).c_str());
      s_calMonth = atoi(cd.substr(5, 2).c_str());
      s_calLastCareerYM = cd.substr(0, 7);
    } else {
      s_calYear = 2026; s_calMonth = 7;
      s_calLastCareerYM.clear();
    }
    s_calInit = true;
  } else if (cd.size() >= 7) {
    // If the career date has crossed into a new month and the user is still
    // viewing the old career month (not a manually navigated month), follow it.
    std::string careerYM = cd.substr(0, 7);
    if (careerYM != s_calLastCareerYM) {
      char lastBuf[8]; snprintf(lastBuf, sizeof(lastBuf), "%04d-%02d", s_calYear, s_calMonth);
      if (s_calLastCareerYM == std::string(lastBuf)) {
        s_calYear  = atoi(cd.substr(0, 4).c_str());
        s_calMonth = atoi(cd.substr(5, 2).c_str());
      }
      s_calLastCareerYM = careerYM;
    }
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
                 C32(kTextPri), hdr);
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

  ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
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
      int ar = (int)(kAccent.x*255), ag = (int)(kAccent.y*255), ab = (int)(kAccent.z*255);
      ImU32 bgCol = hasMatch ? IM_COL32(18, 30, 60, 240)
                  : isToday  ? IM_COL32(ar/4, ag/4, ab/4, 220)
                             : IM_COL32(14, 18, 32, 190);
      wdl->AddRectFilled(cMin, cMax, bgCol, 7.0f);

      // Border
      if (isToday)
        wdl->AddRect(cMin, cMax, IM_COL32(ar, ag, ab, 200), 7.0f, 0, 1.5f);
      else if (hasMatch)
        wdl->AddRect(cMin, cMax, IM_COL32(55,80,140,120), 7.0f, 0, 1.0f);
      else
        wdl->AddRect(cMin, cMax, IM_COL32(30,38,60,80), 7.0f, 0, 1.0f);

      // Day number (top-left)
      char dayBuf[8];
      snprintf(dayBuf, sizeof(dayBuf), "%d", day);
      PushMgrFont(g_ManagerFontSmall);
      ImU32 dayNumCol = isToday   ? IM_COL32(ar, ag, ab, 255)
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
  ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
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
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
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
        bool myGame  = (f.home == sn || f.away == sn);
        bool isToday = (f.fixtureDate == g_CareerHub.currentDate);
        ImGui::TableNextRow(0, rH);
        if (isToday || myGame) {
          int a = myGame && isToday ? 75 : isToday ? 55 : 42;
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),a));
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
        {
          ImVec2 hp0 = ImGui::GetCursorScreenPos();
          DrawTeamLabel(f.homeLogo, hf, 20.0f);
          ImVec2 hp1 = ImGui::GetCursorScreenPos();
          ImGui::SetCursorScreenPos(hp0);
          char hbid[48]; snprintf(hbid, sizeof(hbid), "##sch_h_%s_%s", f.home.c_str(), f.matchday.c_str());
          float hbw = ImGui::GetContentRegionAvail().x;
          if (ImGui::InvisibleButton(hbid, ImVec2(hbw > 4.0f ? hbw : 120.0f, rH - 4.0f))) {
            int tid = LookupTeamIdByName(hf);
            if (tid > 0) NavToClubDetail(tid);
          }
          if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          ImGui::SetCursorScreenPos(hp1);
        }

        ImGui::TableSetColumnIndex(3);
        {
          ImVec2 ap0 = ImGui::GetCursorScreenPos();
          DrawTeamLabel(f.awayLogo, af, 20.0f);
          ImVec2 ap1 = ImGui::GetCursorScreenPos();
          ImGui::SetCursorScreenPos(ap0);
          char abid[48]; snprintf(abid, sizeof(abid), "##sch_a_%s_%s", f.away.c_str(), f.matchday.c_str());
          float abw = ImGui::GetContentRegionAvail().x;
          if (ImGui::InvisibleButton(abid, ImVec2(abw > 4.0f ? abw : 120.0f, rH - 4.0f))) {
            int tid = LookupTeamIdByName(af);
            if (tid > 0) NavToClubDetail(tid);
          }
          if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          ImGui::SetCursorScreenPos(ap1);
        }

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
  ImGui::SetCursorPos(ImVec2(kPad, ImGui::GetCursorPos().y));
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
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 6.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
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
          ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),42));
        ImGui::TableSetColumnIndex(0);
        {
          ImVec2 ps = ImGui::GetCursorScreenPos();
          const float cr = 8.5f;
          float cy = ps.y + (rH - 10.0f) * 0.5f + cr * 0.5f;
          ImDrawList *dl2 = ImGui::GetWindowDrawList();
          dl2->AddCircleFilled(ImVec2(ps.x + cr, cy), cr,
            mine ? C32(kAccent) : IM_COL32(18,30,56,255));
          char nb[4]; snprintf(nb, sizeof(nb), "%d", pos);
          ImVec2 ts = ImGui::CalcTextSize(nb);
          dl2->AddText(ImVec2(ps.x + cr - ts.x*0.5f, cy - ts.y*0.5f),
                       mine ? IM_COL32(255,255,255,230) : C32(kTextDim), nb);
          ImGui::Dummy(ImVec2(cr*2, rH - 12.0f));
        }
        ImGui::TableSetColumnIndex(1);
        // Use full name when available, fall back to shortname
        const std::string &displayName = s.teamFull.empty() ? s.team : s.teamFull;
        {
          ImVec2 lblPos = ImGui::GetCursorScreenPos();
          DrawTeamLabel(s.teamLogo, displayName, 18.0f);
          ImVec2 lblEnd = ImGui::GetCursorScreenPos();
          // Invisible click target over the team label
          float lblW = ImGui::GetContentRegionAvail().x;
          ImGui::SetCursorScreenPos(lblPos);
          char cpBtnId[48]; snprintf(cpBtnId, sizeof(cpBtnId), "##cp_team_%s", s.team.c_str());
          if (ImGui::InvisibleButton(cpBtnId, ImVec2(lblW > 4.0f ? lblW : 120.0f, rH - 4.0f))) {
            int tid = LookupTeamIdByName(displayName);
            if (tid > 0) NavToClubDetail(tid);
          }
          if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
          ImGui::SetCursorScreenPos(lblEnd);
        }
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

// ---- Formation layout (shared by tactics board and mini pitch) ----------

// Portrait coords: nx=horizontal [0=left,1=right], ny=depth [0=GK end,1=attack end]
struct TacNode { int fo; float nx; float ny; };
static const TacNode kTacNodes[] = {
  {0,  0.50f, 0.08f},
  {1,  0.12f, 0.28f}, {2, 0.35f, 0.28f}, {3, 0.65f, 0.28f}, {4, 0.88f, 0.28f},
  {5,  0.22f, 0.52f}, {6, 0.50f, 0.52f}, {7, 0.78f, 0.52f},
  {8,  0.16f, 0.78f}, {10, 0.50f, 0.78f}, {9, 0.84f, 0.78f},
};
static const int kNumTacNodes = 11;

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
  const float kPadTop  = 8.0f;
  const float kBtnGap  = 3.0f;

  // Horizontally centre the content: cap content width and pad symmetrically
  const float kContentMaxW = 880.0f;
  float innerW = (pw > kContentMaxW) ? kContentMaxW : pw * 0.92f;
  float kPadX  = (pw - innerW) * 0.5f;
  if (kPadX < 8.0f) kPadX = 8.0f;

  // Scrollable child so long lists don't overflow the pitch area
  ImGui::SetCursorScreenPos(ImVec2(px + kPadX, py + kPadTop));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
  ImGui::BeginChild("##tacInstr", ImVec2(innerW, ph - kPadTop * 2.0f),
                    false, ImGuiWindowFlags_None);
  ImGui::PopStyleVar();
  ImDrawList *wdl = ImGui::GetWindowDrawList();
  innerW = ImGui::GetContentRegionAvail().x; // true usable width (handles scrollbar, padding)

  const char *lastCat = nullptr;

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
    float fieldH = mpH * (0.94f - 0.06f);
    float bandH  = fieldH * defWidth * 0.30f;
    if (bandH > fieldH * 0.46f) bandH = fieldH * 0.46f; // never overlap in center
    ImU32 bandCol = IM_COL32(50, 110, 210, (int)alpha);
    float ftop = PY(0.06f), fbot = PY(0.94f);
    wdl->AddRectFilled(ImVec2(PX(0.06f), ftop),          ImVec2(PX(defLineNX), ftop + bandH), bandCol);
    wdl->AddRectFilled(ImVec2(PX(0.06f), fbot - bandH),  ImVec2(PX(defLineNX), fbot),         bandCol);
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

  // ---- Player dots (positions react to tactic values) --------------------
  // Landscape mapping: portrait ny (depth) → landscape x, portrait nx (width) → landscape y
  for (int ni = 0; ni < kNumTacNodes; ni++) {
    const TacNode &tn = kTacNodes[ni];
    bool isGK  = (tn.fo == 0);
    bool isDef = (tn.fo >= 1 && tn.fo <= 4);
    bool isMid = (tn.fo >= 5 && tn.fo <= 7);
    // isAtt = everything else (fo 8,9,10)

    float adjX, adjY;

    if (isGK) {
      adjX = 0.06f;
      adjY = 0.50f;
    } else if (isDef) {
      // Line up just behind the defensive line; width scaled by defWidth
      adjX = defLineNX - 0.025f;
      float ws = 0.50f + defWidth * 0.70f;
      adjY = 0.5f + (tn.nx - 0.5f) * ws;
    } else if (isMid) {
      // Base midfield depth shifts forward with offMid
      float fwd = (offMid - 0.5f) * 0.10f;
      adjX = 0.50f + fwd;
      float ws = 0.45f + offWidth * 0.65f;
      adjY = 0.5f + (tn.nx - 0.5f) * ws;
    } else {
      // Attackers: depth driven by offDepth
      adjX = 0.52f + offDepth * 0.34f;
      float ws = 0.45f + offWidth * 0.65f;
      adjY = 0.5f + (tn.nx - 0.5f) * ws;
    }

    // Clamp inside pitch bounds
    if (adjX < 0.06f) adjX = 0.06f;
    if (adjX > 0.94f) adjX = 0.94f;
    if (adjY < 0.07f) adjY = 0.07f;
    if (adjY > 0.93f) adjY = 0.93f;

    float sx = PX(adjX), sy = PY(adjY);
    float r = 4.0f;

    // Role colours: GK=gold, DEF=blue, MID=green, ATT=red-orange
    ImU32 fill;
    if      (isGK)  fill = IM_COL32(255, 200,  30, 245);
    else if (isDef) fill = IM_COL32( 80, 150, 255, 245);
    else if (isMid) fill = IM_COL32( 80, 210, 110, 245);
    else            fill = IM_COL32(255,  90,  70, 245);

    wdl->AddCircleFilled(ImVec2(sx+1.0f, sy+1.0f), r, IM_COL32(0,0,0,120)); // drop shadow
    wdl->AddCircleFilled(ImVec2(sx, sy), r, fill);
    wdl->AddCircle(ImVec2(sx, sy), r, IM_COL32(255,255,255,170), 14, 1.0f);
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

  // ---- Active squad incomplete warning --------------------------------
  {
    int active = CountActiveSquad();
    if (active < 20) {
      ImVec2 wpos  = ImGui::GetCursorScreenPos();
      float  banW  = w - kPad * 2.0f;
      const float kBanH = 36.0f;
      ImDrawList *bdl = ImGui::GetWindowDrawList();
      bdl->AddRectFilled(wpos, ImVec2(wpos.x + banW, wpos.y + kBanH),
                         IM_COL32(140, 35, 35, 200), 6.0f);
      bdl->AddRect(wpos, ImVec2(wpos.x + banW, wpos.y + kBanH),
                   IM_COL32(220, 70, 70, 160), 6.0f, 0, 1.0f);
      // Left warning stripe
      bdl->AddRectFilled(wpos, ImVec2(wpos.x + 4.0f, wpos.y + kBanH),
                         IM_COL32(240, 80, 80, 255), 6.0f);
      char banMsg[96];
      snprintf(banMsg, sizeof(banMsg),
               "Active squad has %d/20 players ! Assign 20 players in Squad.",
               active);
      PushMgrFont(g_ManagerFontSmall);
      const float kBFs = 17.0f;
      ImVec2 tsz = g_ManagerFontSmall
          ? g_ManagerFontSmall->CalcTextSizeA(kBFs, FLT_MAX, 0, banMsg)
          : ImGui::CalcTextSize(banMsg);
      float btx = wpos.x + (banW - tsz.x) * 0.5f;
      bdl->AddText(g_ManagerFontSmall, kBFs,
                   ImVec2(btx, wpos.y + (kBanH - kBFs) * 0.5f),
                   IM_COL32(255, 200, 200, 245), banMsg);
      PopMgrFont(g_ManagerFontSmall);
      ImGui::Dummy(ImVec2(banW, kBanH));
      ImGui::Dummy(ImVec2(0, 6.0f));
    }
  }

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
        std::string ln = DisplayName(p);
        if (ln.size() > 9) ln = ln.substr(0, 8) + ".";
        nodes[ni].name      = ln;
        nodes[ni].filled    = true;
        nodes[ni].baseStat  = p.baseStat;
        nodes[ni].potential = p.potential;
        nodes[ni].stamina   = p.currentStamina; // use current fitness bar
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

  // Formation dropdown — top-left corner of pitch (hidden when Team Instructions active)
  if (!s_showTeamInstructions) {
    static int s_formationIdx = 0;
    static const char *kFormations[] = { "4-3-3" };
    const float fddW = 80.0f, fddH = 24.0f;
    const float fddX = pitchX + 8.0f;
    const float fddY = pitchY + 8.0f;
    ImGui::SetCursorScreenPos(ImVec2(fddX, fddY));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 3.0f));
    ImGui::PushStyleColor(ImGuiCol_FrameBg,        IM_COL32(10, 18, 42, 200));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(20, 32, 72, 220));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,  IM_COL32(30, 48, 100, 240));
    ImGui::PushStyleColor(ImGuiCol_Text,           IM_COL32(220, 225, 240, 255));
    ImGui::PushStyleColor(ImGuiCol_Button,         IM_COL32(10, 18, 42, 200));
    PushMgrFont(g_ManagerFontSmall);
    ImGui::SetNextItemWidth(fddW);
    if (ImGui::BeginCombo("##formation", kFormations[s_formationIdx],
                          ImGuiComboFlags_HeightSmall)) {
      for (int i = 0; i < 1; i++) {
        bool sel = (s_formationIdx == i);
        if (ImGui::Selectable(kFormations[i], sel)) s_formationIdx = i;
        if (sel) ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    PopMgrFont(g_ManagerFontSmall);
    ImGui::PopStyleColor(5);
    ImGui::PopStyleVar(2);
  }

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
    std::string dispName = DisplayName(*p);
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
    std::string dispName = DisplayName(*p);
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

// ---- Media texture loader (path relative to build/, no prefix) -----------

static GLuint LoadMediaTex(const std::string &relPath) {
  if (relPath.empty()) return 0;
  auto it = s_BadgeCache.find(relPath);
  if (it != s_BadgeCache.end()) return it->second;
  SDL_Surface *surf = IMG_Load(relPath.c_str());
  if (!surf) { s_BadgeCache[relPath] = 0; return 0; }
  SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(surf);
  if (!rgba) { s_BadgeCache[relPath] = 0; return 0; }
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
  glBindTexture(GL_TEXTURE_2D, 0);
  SDL_FreeSurface(rgba);
  s_BadgeCache[relPath] = tex;
  return tex;
}

static GLuint GetStaffPlaceholderTex(const std::string &role) {
  if (role == "Psychologist" || role == "Scout")
    return LoadMediaTex("media/textures/faces/Men Default faces /Extra persons - New colorways/Extra_person13.png");
  if (role == "Physio")
    return LoadMediaTex("media/textures/faces/Men Default faces /None - New colorways/None - No brand/none-grey.png");
  // Fitness Coach, Youth Coach
  return LoadMediaTex("media/textures/faces/Men Default faces /Manager - New colorways/Manager11-Grey.png");
}

// ---- Scouting DB helpers ------------------------------------------------

static int ComputeScoutDays(int rating) {
  switch (rating) {
    case 1: return 10 + (rand() % 6); // 10-15 days
    case 2: return 9  + (rand() % 5); // 9-13 days
    case 3: return 8  + (rand() % 4); // 8-11 days
    case 4: return 7  + (rand() % 3); // 7-9 days
    case 5: return 4  + (rand() % 2); // 4-5 days
    default: return 10;
  }
}

static void StartScouting(int managerId, int playerId,
                          const std::string &fn, const std::string &ln,
                          const std::string &club, int scoutRating,
                          const std::string &currentDate) {
  int days = ComputeScoutDays(scoutRating);
  // Compute due date in C++ (mirrors AddNDays in managercareer.cpp)
  std::string dueDate = currentDate;
  for (int i = 0; i < days; i++) {
    if (dueDate.size() < 10) break;
    int year  = atoi(dueDate.substr(0,4).c_str());
    int month = atoi(dueDate.substr(5,2).c_str());
    int day   = atoi(dueDate.substr(8,2).c_str());
    bool leap = (year%4==0 && (year%100!=0 || year%400==0));
    const int kDIM[] = {0,31,leap?29:28,31,30,31,30,31,31,30,31,30,31};
    day++;
    if (day > kDIM[month]) { day = 1; month++; }
    if (month > 12)        { month = 1; year++; }
    char buf[16]; snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
    dueDate = buf;
  }
  std::stringstream q;
  q << "INSERT OR IGNORE INTO scout_queue"
    << " (manager_id, player_id, firstname, lastname, club_name, scout_rating, due_date)"
    << " VALUES (" << managerId << "," << playerId
    << ",'" << fn << "','" << ln << "','" << club << "',"
    << scoutRating << ",'" << dueDate << "');";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
  g_CareerHub.LoadFromDB(managerId, g_CareerHub.clubId);
}

static void CancelScouting(int managerId, int playerId) {
  std::stringstream q;
  q << "DELETE FROM scout_queue WHERE manager_id=" << managerId
    << " AND player_id=" << playerId << ";";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
  g_CareerHub.LoadFromDB(managerId, g_CareerHub.clubId);
}

// ---- Staff market filter state ------------------------------------------
static int s_mktFilterRating  = 0;  // 0 = any, 1-5 = min stars
static int s_mktFilterMaxWage = 0;  // 0 = unlimited

// ---- Staff DB helpers ---------------------------------------------------

static void HireStaff(int managerId, int staffId) {
  std::stringstream q;
  q << "INSERT OR IGNORE INTO career_staff (manager_id, staff_id) VALUES ("
    << managerId << ", " << staffId << ");";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
  // Reload hired staff list
  g_CareerHub.LoadFromDB(managerId, g_CareerHub.clubId);
}

static void FireStaff(int managerId, int staffId) {
  std::stringstream q;
  q << "DELETE FROM career_staff WHERE manager_id=" << managerId
    << " AND staff_id=" << staffId << ";";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
  g_CareerHub.LoadFromDB(managerId, g_CareerHub.clubId);
}

static void LoadStaffMarket(int managerId, const std::string &role) {
  s_staffMarketList.clear();
  std::stringstream q;
  q << "SELECT sl.staff_id, sl.firstname, sl.lastname, sl.nationality, sl.role,"
    << " (CAST(strftime('%Y', 'now') AS INTEGER) - CAST(strftime('%Y', sl.\"date-of-birth\") AS INTEGER)) as age,"
    << " sl.rating, sl.weekly_wage"
    << " FROM staff_list sl"
    << " WHERE sl.role='" << role << "'"
    << " AND sl.staff_id NOT IN (SELECT staff_id FROM career_staff WHERE manager_id=" << managerId << ")"
    << " ORDER BY sl.rating DESC, sl.weekly_wage ASC;";
  DatabaseResult *r = GetDB()->Query(q.str());
  for (unsigned int i = 0; i < r->data.size(); i++) {
    StaffMarketEntry e;
    e.id          = atoi(r->data[i][0].c_str());
    e.firstName   = r->data[i][1];
    e.lastName    = r->data[i][2];
    e.nationality = r->data[i][3];
    e.role        = r->data[i][4];
    std::string ag = r->data[i][5];
    e.age         = ag.empty() ? 0 : atoi(ag.c_str());
    e.rating      = atoi(r->data[i][6].c_str());
    e.weeklywage  = atoi(r->data[i][7].c_str());
    s_staffMarketList.push_back(e);
  }
  delete r;
  s_staffMarketLoaded = true;
}

// ---- Staff card shared helper -------------------------------------------
// Draws a single staff card (hired or empty) into dl at (ox,oy) with size (cw,ch).
// Returns true if "Fire" was clicked (hired cards) or "Hire" was clicked (market cards).
// mode: 0=staff-page hired, 1=staff-page empty, 2=market card

static void DrawStaffCard_Hired(ImDrawList *dl, float ox, float oy, float cw, float ch,
                                const CareerHubState::StaffMember &sm, int uid) {
  const ImU32 kColWhite = IM_COL32(255, 255, 255, 230);
  const ImU32 kColSub   = IM_COL32(160, 175, 210, 180);
  const ImU32 kColWage  = IM_COL32(100, 215, 120, 230);
  const float kRad      = 10.0f;

  // Card bg + border
  dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox+cw, oy+ch), C32(kBgCard), kRad);
  dl->AddRect(ImVec2(ox, oy), ImVec2(ox+cw, oy+ch), C32(kBorder), kRad, 0, 1.0f);

  // Face image (top-center)
  const float kImgSz = 80.0f;
  float imgX = ox + (cw - kImgSz) * 0.5f;
  float imgY = oy + 18.0f;
  GLuint face = GetStaffPlaceholderTex(sm.role);
  if (face) {
    ImGui::SetCursorScreenPos(ImVec2(imgX, imgY));
    ImGui::Image((ImTextureID)(intptr_t)face, ImVec2(kImgSz, kImgSz));
  } else {
    dl->AddRectFilled(ImVec2(imgX, imgY), ImVec2(imgX+kImgSz, imgY+kImgSz),
                      IM_COL32(25,35,65,220), 8.0f);
  }

  float textY = imgY + kImgSz + 10.0f;

  // Role pill
  PushMgrFont(g_ManagerFontSmall);
  ImVec2 roleSize = ImGui::CalcTextSize(sm.role.c_str());
  float pillW = roleSize.x + 12.0f, pillH = roleSize.y + 4.0f;
  float pillX = ox + (cw - pillW) * 0.5f;
  dl->AddRectFilled(ImVec2(pillX, textY), ImVec2(pillX+pillW, textY+pillH),
                    IM_COL32((int)(kAccent.x*255*0.25f),(int)(kAccent.y*255*0.25f),(int)(kAccent.z*255*0.25f),200), 5.0f);
  dl->AddText(ImVec2(pillX+6.0f, textY+2.0f), C32(kAccent), sm.role.c_str());
  PopMgrFont(g_ManagerFontSmall);
  textY += pillH + 8.0f;

  // Name (centered)
  std::string fullName = sm.firstName + " " + sm.lastName;
  PushMgrFont(g_ManagerFontBold);
  ImVec2 nameSize = ImGui::CalcTextSize(fullName.c_str());
  float nameX = ox + (cw - nameSize.x) * 0.5f;
  if (nameX < ox + 4.0f) nameX = ox + 4.0f;
  dl->AddText(ImVec2(nameX, textY), kColWhite, fullName.c_str());
  PopMgrFont(g_ManagerFontBold);
  textY += nameSize.y + 4.0f;

  // Age · Nationality (centered)
  char subBuf[64];
  snprintf(subBuf, sizeof(subBuf), "%d  ·  %s", sm.age, sm.nationality.c_str());
  PushMgrFont(g_ManagerFontSmall);
  ImVec2 subSize = ImGui::CalcTextSize(subBuf);
  dl->AddText(ImVec2(ox + (cw - subSize.x)*0.5f, textY), kColSub, subBuf);
  PopMgrFont(g_ManagerFontSmall);
  textY += subSize.y + 6.0f;

  // Stars (centered)
  const float kStarW = 9.0f, kGap = 2.0f;
  float starsRowW = 5.0f * (kStarW + kGap) - kGap;
  ImGui::SetCursorScreenPos(ImVec2(ox + (cw - starsRowW) * 0.5f, textY));
  DrawStars((float)sm.rating, 5.0f, C32(kAccent));
  textY += ImGui::GetTextLineHeight() + 4.0f;

  // Wage (centered)
  char wageBuf[32];
  snprintf(wageBuf, sizeof(wageBuf), "%s%d / wk", g_CareerHub.club.currency.empty() ? "\xE2\x82\xAC" : g_CareerHub.club.currency.c_str(), sm.weeklywage);
  PushMgrFont(g_ManagerFontSmall);
  ImVec2 wageSize = ImGui::CalcTextSize(wageBuf);
  dl->AddText(ImVec2(ox + (cw - wageSize.x)*0.5f, textY), kColWage, wageBuf);
  PopMgrFont(g_ManagerFontSmall);

  // Fire button (bottom)
  const float kBtnH = 26.0f, kBtnMargin = 10.0f;
  float btnY = oy + ch - kBtnH - kBtnMargin;
  float btnW = cw - kBtnMargin * 2.0f;
  char fireBtnId[32];
  snprintf(fireBtnId, sizeof(fireBtnId), "Release##sf_%d", uid);
  ImGui::SetCursorScreenPos(ImVec2(ox + kBtnMargin, btnY));
  ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(140, 35, 35, 210));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(190, 55, 55, 230));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(110, 20, 20, 255));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
  if (ImGui::Button(fireBtnId, ImVec2(btnW, kBtnH)))
    FireStaff(g_CareerHub.managerId, sm.id);
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
}

static void DrawStaffCard_Empty(ImDrawList *dl, float ox, float oy, float cw, float ch,
                                const char *role, int ri) {
  const float kRad = 10.0f;
  // Darker dashed-looking bg
  dl->AddRectFilled(ImVec2(ox,oy), ImVec2(ox+cw,oy+ch),
                    IM_COL32(12,18,36,220), kRad);
  dl->AddRect(ImVec2(ox,oy), ImVec2(ox+cw,oy+ch),
              IM_COL32(50,65,100,130), kRad, 0, 1.0f);

  // Role label (centered, muted)
  float midY = oy + ch * 0.32f;
  PushMgrFont(g_ManagerFontSmall);
  ImVec2 rlSz = ImGui::CalcTextSize(role);
  dl->AddText(ImVec2(ox+(cw-rlSz.x)*0.5f, midY), C32(kTextDim), role);
  PopMgrFont(g_ManagerFontSmall);

  // + icon
  float plusY = midY + rlSz.y + 6.0f;
  PushMgrFont(g_ManagerFontBold);
  ImVec2 plusSz = ImGui::CalcTextSize("+");
  dl->AddText(ImVec2(ox+(cw-plusSz.x)*0.5f, plusY),
              IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),160), "+");
  PopMgrFont(g_ManagerFontBold);

  // Hire button
  const float kBtnH = 28.0f, kBtnMargin = 10.0f;
  float btnY = oy + ch - kBtnH - kBtnMargin;
  float btnW = cw - kBtnMargin * 2.0f;
  char hireBtnId[64];
  snprintf(hireBtnId, sizeof(hireBtnId), "Hire##se_%d", ri);
  ImGui::SetCursorScreenPos(ImVec2(ox + kBtnMargin, btnY));
  ImGui::PushStyleColor(ImGuiCol_Button,        C32(kAccent));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C32(kAccentH));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  C32(kAccentA));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
  if (ImGui::Button(hireBtnId, ImVec2(btnW, kBtnH))) {
    s_staffMarketRole   = role;
    s_staffMarketLoaded = false;
    NavPush(PAGE_STAFF_MARKET);
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
}

// ---- DrawStaffPage ------------------------------------------------------

static const char *kStaffRoles[] = {
  "Scout", "Physio", "Psychologist", "Fitness Coach", "Youth Coach"
};
static const int kNumStaffRoles = 5;

static void DrawStaffPage(float w, float h) {
  const float kPad     = 14.0f;
  const float kGap     = 14.0f;
  const float kCardH   = 290.0f;
  const int   kNumCols = 3;

  float cw   = w - kPad * 2.0f;
  float colW = (cw - kGap * (kNumCols - 1)) / kNumCols;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  ImGui::BeginChild("##staff_grid", ImVec2(cw, h - 16.0f), false,
                    ImGuiWindowFlags_NoScrollbar);

  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 origin  = ImGui::GetCursorScreenPos();

  for (int ri = 0; ri < kNumStaffRoles; ri++) {
    const char *role = kStaffRoles[ri];
    int row = ri / kNumCols;
    int col = ri % kNumCols;

    // Center the last partial row
    int rolesInLastRow = kNumStaffRoles % kNumCols;
    float rowOffsetX = 0.0f;
    if (rolesInLastRow != 0 && row == kNumStaffRoles / kNumCols)
      rowOffsetX = (kNumCols - rolesInLastRow) * (colW + kGap) * 0.5f;

    float ox = origin.x + rowOffsetX + col * (colW + kGap);
    float oy = origin.y + 4.0f + row * (kCardH + kGap);

    const CareerHubState::StaffMember *hired = nullptr;
    for (const auto &sm : g_CareerHub.staff)
      if (sm.role == role) { hired = &sm; break; }

    if (hired)
      DrawStaffCard_Hired(dl, ox, oy, colW, kCardH, *hired, hired->id);
    else
      DrawStaffCard_Empty(dl, ox, oy, colW, kCardH, role, ri);
  }

  int numRows = (kNumStaffRoles + kNumCols - 1) / kNumCols;
  ImGui::Dummy(ImVec2(cw, numRows * (kCardH + kGap) + 8.0f));
  ImGui::EndChild();
}

// ---- DrawStaffMarketPage ------------------------------------------------

static void DrawStaffMarketCard(ImDrawList *dl, float ox, float oy, float cw, float ch,
                                const StaffMarketEntry &e) {
  const ImU32 kColWhite = IM_COL32(255, 255, 255, 230);
  const ImU32 kColSub   = IM_COL32(160, 175, 210, 180);
  const ImU32 kColWage  = IM_COL32(100, 215, 120, 230);
  const float kRad      = 10.0f;

  dl->AddRectFilled(ImVec2(ox,oy), ImVec2(ox+cw,oy+ch), C32(kBgCard), kRad);
  dl->AddRect(ImVec2(ox,oy), ImVec2(ox+cw,oy+ch), C32(kBorder), kRad, 0, 1.0f);

  // Face image
  const float kImgSz = 52.0f;
  float imgX = ox + (cw - kImgSz) * 0.5f, imgY = oy + 12.0f;
  GLuint face = GetStaffPlaceholderTex(e.role);
  if (face) {
    float cx = imgX + kImgSz*0.5f, cy2 = imgY + kImgSz*0.5f;
    dl->AddCircleFilled(ImVec2(cx,cy2), kImgSz*0.5f+2.0f,
                        IM_COL32((int)(kAccent.x*255*0.35f),(int)(kAccent.y*255*0.35f),(int)(kAccent.z*255*0.35f),160), 32);
    ImGui::SetCursorScreenPos(ImVec2(imgX, imgY));
    ImGui::Image((ImTextureID)(intptr_t)face, ImVec2(kImgSz,kImgSz));
  } else {
    float cx=imgX+kImgSz*0.5f,cy2=imgY+kImgSz*0.5f;
    dl->AddCircleFilled(ImVec2(cx,cy2),kImgSz*0.5f,IM_COL32(40,55,90,200),32);
  }

  float textY = imgY + kImgSz + 8.0f;

  // Name
  std::string fullName = e.firstName + " " + e.lastName;
  PushMgrFont(g_ManagerFontBold);
  ImVec2 nSz = ImGui::CalcTextSize(fullName.c_str());
  float nameX = ox + (cw - nSz.x)*0.5f;
  if (nameX < ox+3.0f) nameX = ox+3.0f;
  dl->AddText(ImVec2(nameX, textY), kColWhite, fullName.c_str());
  PopMgrFont(g_ManagerFontBold);
  textY += nSz.y + 3.0f;

  // Age · Nationality
  char subBuf[48];
  snprintf(subBuf, sizeof(subBuf), "%d  ·  %s", e.age, e.nationality.c_str());
  PushMgrFont(g_ManagerFontSmall);
  ImVec2 sSz = ImGui::CalcTextSize(subBuf);
  dl->AddText(ImVec2(ox+(cw-sSz.x)*0.5f, textY), kColSub, subBuf);
  PopMgrFont(g_ManagerFontSmall);
  textY += sSz.y + 5.0f;

  // Stars
  const float kStarW = 9.0f, kGapS = 2.0f;
  float starsW = 5.0f*(kStarW+kGapS)-kGapS;
  ImGui::SetCursorScreenPos(ImVec2(ox+(cw-starsW)*0.5f, textY));
  DrawStars((float)e.rating, 5.0f, C32(kAccent));
  textY += ImGui::GetTextLineHeight() + 4.0f;

  // Wage
  char wageBuf[32];
  snprintf(wageBuf, sizeof(wageBuf), "%s%d / wk", g_CareerHub.club.currency.empty() ? "\xE2\x82\xAC" : g_CareerHub.club.currency.c_str(), e.weeklywage);
  PushMgrFont(g_ManagerFontSmall);
  ImVec2 wSz = ImGui::CalcTextSize(wageBuf);
  dl->AddText(ImVec2(ox+(cw-wSz.x)*0.5f, textY), kColWage, wageBuf);
  PopMgrFont(g_ManagerFontSmall);

  // Hire button
  const float kBtnH = 26.0f, kBtnMg = 8.0f;
  float btnY = oy + ch - kBtnH - kBtnMg;
  float btnW = cw - kBtnMg * 2.0f;
  char btnId[32];
  snprintf(btnId, sizeof(btnId), "Hire##mk_%d", e.id);
  ImGui::SetCursorScreenPos(ImVec2(ox+kBtnMg, btnY));
  ImGui::PushStyleColor(ImGuiCol_Button,        C32(kAccent));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C32(kAccentH));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  C32(kAccentA));
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
  if (ImGui::Button(btnId, ImVec2(btnW, kBtnH))) {
    HireStaff(g_CareerHub.managerId, e.id);
    s_staffMarketLoaded = false;
    NavBack();
  }
  ImGui::PopStyleVar();
  ImGui::PopStyleColor(3);
}

static void DrawStaffMarketPage(float w, float h) {
  const float kPad  = 14.0f;
  const float kGap  = 10.0f;
  const float kCardH = 230.0f;

  if (!s_staffMarketLoaded)
    LoadStaffMarket(g_CareerHub.managerId, s_staffMarketRole);

  float cw   = w - kPad * 2.0f;
  float colW = (cw - kGap * 4.0f) / 5.0f;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));

  // Title
  PushMgrFont(g_ManagerFontBold);
  std::string title = s_staffMarketRole + " Market";
  ImGui::TextUnformatted(title.c_str());
  PopMgrFont(g_ManagerFontBold);
  ImGui::Spacing();

  // ---- Filter bar -------------------------------------------------------
  // Rating filter: "Any" + star buttons 1-5
  ImGui::SetCursorPosX(kPad);
  ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,   ImVec2(4.0f, 4.0f));

  // "Rating:" label
  ImGui::AlignTextToFramePadding();
  PushMgrFont(g_ManagerFontSmall);
  ImGui::TextUnformatted("Rating:");
  PopMgrFont(g_ManagerFontSmall);
  ImGui::SameLine();

  auto ratingBtn = [&](const char *label, int val) {
    bool active = (s_mktFilterRating == val);
    if (active) {
      ImGui::PushStyleColor(ImGuiCol_Button,        C32(kAccent));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C32(kAccentH));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  C32(kAccentA));
    } else {
      ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(25,35,60,200));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40,55,90,230));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(20,28,50,255));
    }
    if (ImGui::Button(label, ImVec2(38.0f, 24.0f)))
      s_mktFilterRating = val;
    ImGui::PopStyleColor(3);
    ImGui::SameLine();
  };
  ratingBtn("Any", 0);
  ratingBtn("1+",  1);
  ratingBtn("2+",  2);
  ratingBtn("3+",  3);
  ratingBtn("4+",  4);
  ratingBtn("5",   5);

  // Wage filter
  ImGui::SameLine(0.0f, 20.0f);
  ImGui::AlignTextToFramePadding();
  PushMgrFont(g_ManagerFontSmall);
  ImGui::TextUnformatted("Max wage:");
  PopMgrFont(g_ManagerFontSmall);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90.0f);
  ImGui::InputInt("##maxwage", &s_mktFilterMaxWage, 500, 1000);
  if (s_mktFilterMaxWage < 0) s_mktFilterMaxWage = 0;
  ImGui::SameLine();
  if (s_mktFilterMaxWage > 0) {
    PushMgrFont(g_ManagerFontSmall);
    char capBuf[32]; snprintf(capBuf, sizeof(capBuf), "%s%d/wk", g_CareerHub.club.currency.empty() ? "\xE2\x82\xAC" : g_CareerHub.club.currency.c_str(), s_mktFilterMaxWage);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(capBuf);
    PopMgrFont(g_ManagerFontSmall);
    ImGui::SameLine();
  }
  // Clear wage button
  if (s_mktFilterMaxWage > 0) {
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(25,35,60,200));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40,55,90,230));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(20,28,50,255));
    if (ImGui::Button("Clear##wclr", ImVec2(44.0f, 24.0f)))
      s_mktFilterMaxWage = 0;
    ImGui::PopStyleColor(3);
  }

  ImGui::PopStyleVar(2);
  ImGui::Spacing();

  // ---- Build filtered list ----------------------------------------------
  std::vector<const StaffMarketEntry*> filtered;
  for (const auto &e : s_staffMarketList) {
    if (s_mktFilterRating > 0 && e.rating < s_mktFilterRating) continue;
    if (s_mktFilterMaxWage > 0 && e.weeklywage > s_mktFilterMaxWage) continue;
    filtered.push_back(&e);
  }

  if (filtered.empty()) {
    ImGui::TextUnformatted("No staff match your filters.");
    return;
  }

  // ---- Grid -------------------------------------------------------------
  float scrollH = h - ImGui::GetCursorPos().y - 8.0f;
  ImGui::BeginChild("##mkt_grid", ImVec2(cw, scrollH), false, 0);

  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 origin  = ImGui::GetCursorScreenPos();

  int cols   = 5;
  int rows   = ((int)filtered.size() + cols - 1) / cols;
  float rowGap = kGap;

  for (int i = 0; i < (int)filtered.size(); i++) {
    int col = i % cols;
    int row = i / cols;
    float ox = origin.x + col * (colW + kGap);
    float oy = origin.y + row * (kCardH + rowGap);
    DrawStaffMarketCard(dl, ox, oy, colW, kCardH, *filtered[i]);
  }

  ImGui::Dummy(ImVec2(cw, rows * (kCardH + rowGap) + 8.0f));
  ImGui::EndChild();
}

// ---- Finance page helpers -----------------------------------------------

static std::string FmtMoney(long long v) {
  bool neg = v < 0;
  long long abs = neg ? -v : v;
  const char *cur = g_CareerHub.club.currency.empty() ? "\xE2\x82\xAC" : g_CareerHub.club.currency.c_str();
  char buf[40];
  if      (abs >= 1000000000LL) snprintf(buf, sizeof(buf), "%s%s%.2fB", neg?"-":"", cur, abs/1e9);
  else if (abs >= 1000000LL)    snprintf(buf, sizeof(buf), "%s%s%.1fM", neg?"-":"", cur, abs/1e6);
  else if (abs >= 1000LL)       snprintf(buf, sizeof(buf), "%s%s%.0fK", neg?"-":"", cur, abs/1e3);
  else                          snprintf(buf, sizeof(buf), "%s%s%lld",  neg?"-":"", cur, abs);
  return buf;
}

static std::string FmtMoneyFull(long long v) {
  bool neg = v < 0;
  long long abs = neg ? -v : v;
  const char *cur = g_CareerHub.club.currency.empty() ? "\xE2\x82\xAC" : g_CareerHub.club.currency.c_str();
  char buf[56];
  char plain[24]; snprintf(plain, sizeof(plain), "%lld", abs);
  std::string s(plain);
  int ins = (int)s.size() - 3;
  while (ins > 0) { s.insert(ins, ","); ins -= 3; }
  snprintf(buf, sizeof(buf), "%s%s%s", neg ? "-" : "", cur, s.c_str());
  return buf;
}

static std::string FinanceCatLabel(const std::string &cat) {
  if (cat == "tv_rights")  return "TV Rights";
  if (cat == "matchday")   return "Match Day";
  if (cat == "wages")      return "Wages";
  if (cat == "operating")  return "Operating";
  if (cat == "prize")      return "Prize Money";
  if (cat == "balance")    return "Starting Budget";
  if (cat == "transfer_in")  return "Transfer In";
  if (cat == "transfer_out") return "Transfer Out";
  return cat;
}

static ImU32 FinanceCatColor(const std::string &cat) {
  if (cat == "tv_rights")    return IM_COL32( 80, 180, 240, 230);
  if (cat == "matchday")     return IM_COL32(100, 220, 140, 230);
  if (cat == "prize")        return IM_COL32(255, 200,  50, 230);
  if (cat == "balance")      return IM_COL32(160, 160, 200, 200);
  if (cat == "wages")        return IM_COL32(230,  80,  80, 230);
  if (cat == "operating")    return IM_COL32(200, 120,  60, 230);
  if (cat == "transfer_out") return IM_COL32(230,  80,  80, 230);
  if (cat == "transfer_in")  return IM_COL32(100, 220, 140, 230);
  return IM_COL32(180, 180, 180, 200);
}


static void DrawFinancesPage(float w, float h) {
  const float kPad  = 16.0f;
  const float kGap  = 12.0f;
  float cw          = w - kPad * 2.0f;

  const CareerHubState::FinanceState &fi = g_CareerHub.finances;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  ImDrawList *dl   = ImGui::GetWindowDrawList();
  ImVec2 win0      = ImGui::GetWindowPos();
  float scrollY    = ImGui::GetScrollY();

  float headerTop  = ImGui::GetCursorPos().y;

  // ── Row 1: three summary cards ──────────────────────────────────────────
  float cardH1  = 88.0f;
  float card3W  = (cw - kGap * 2.0f) / 3.0f;
  float baseY   = win0.y - scrollY + headerTop;
  ImVec2 origin = ImVec2(win0.x + kPad, baseY);

  // Card helper lambda — ox/sy are absolute screen coords, cardW defaults to card3W
  auto SummaryCard = [&](float ox, float sy, const char *title, const std::string &value,
                          ImU32 valCol, float cardW, const char *sub = nullptr, const char *sub2 = nullptr) {
    ImVec2 p0(ox, sy), p1(ox+cardW, sy+cardH1);
    dl->AddRectFilled(p0, p1, C32(kBgCard), 10.0f);
    dl->AddRect(p0, p1, C32(kBorder), 10.0f, 0, 1.0f);
    PushMgrFont(g_ManagerFontSmall);
    dl->AddText(ImVec2(ox+12.0f, sy+10.0f), C32(kTextDim), title);
    PopMgrFont(g_ManagerFontSmall);
    PushMgrFont(g_ManagerFontTitle);
    dl->AddText(ImVec2(ox+12.0f, sy+26.0f), valCol, value.c_str());
    PopMgrFont(g_ManagerFontTitle);
    float subY = sy + 58.0f;
    if (sub) {
      PushMgrFont(g_ManagerFontSmall);
      dl->AddText(ImVec2(ox+12.0f, subY), C32(kTextDim), sub);
      PopMgrFont(g_ManagerFontSmall);
      subY += ImGui::GetTextLineHeight() + 2.0f;
    }
    if (sub2) {
      PushMgrFont(g_ManagerFontSmall);
      dl->AddText(ImVec2(ox+12.0f, subY), C32(kTextDim), sub2);
      PopMgrFont(g_ManagerFontSmall);
    }
  };

  // Balance card
  long long bal = fi.balance;
  ImU32 balCol  = bal >= 0 ? IM_COL32(80, 215, 115, 255) : IM_COL32(220, 70, 70, 255);
  SummaryCard(origin.x, origin.y, "CURRENT BALANCE", FmtMoney(bal), balCol,
              card3W, FmtMoneyFull(bal).c_str());

  // Weekly net card (TV + sponsors - wages - operating)
  long long weekIncome  = fi.weeklyTV + fi.weeklySponsors;
  long long weekExpense = fi.weeklyWages + fi.weeklyOperating;
  long long weekNet     = weekIncome - weekExpense;
  ImU32 netCol = weekNet >= 0 ? IM_COL32(80, 215, 115, 255) : IM_COL32(220, 70, 70, 255);
  std::string netStr = (weekNet >= 0 ? "+" : "") + FmtMoney(weekNet) + " / wk";
  std::string netSub = fi.weeklySponsors > 0
    ? ("incl. " + FmtMoney(fi.weeklySponsors) + "/wk sponsors") : "";
  SummaryCard(origin.x + card3W + kGap, origin.y, "WEEKLY CASHFLOW", netStr, netCol,
              card3W, netSub.empty() ? nullptr : netSub.c_str());

  // Season projection: remaining weeks cashflow + remaining matchday estimate
  long long remainingWeeks = 30;
  if (!g_CareerHub.currentDate.empty() && g_CareerHub.seasonYear > 0) {
    int curM = atoi(g_CareerHub.currentDate.substr(5,2).c_str());
    int curY = atoi(g_CareerHub.currentDate.substr(0,4).c_str());
    int endM = 6, endY = g_CareerHub.seasonYear + 1;
    int months = (endY - curY) * 12 + (endM - curM);
    if (months < 0) months = 0;
    remainingWeeks = (long long)months * 4;
  }
  // Count remaining home and away fixtures for matchday estimate
  int remHome = 0, remAway = 0;
  for (const auto &f : g_CareerHub.fixtures) {
    if (f.status != "scheduled") continue;
    if (f.home == g_CareerHub.club.shortName) remHome++;
    else if (f.away == g_CareerHub.club.shortName) remAway++;
  }
  long long mdAvg    = (fi.matchdayMin + fi.matchdayMax) / 2;
  long long mdEstimate = (long long)remHome * mdAvg + (long long)remAway * mdAvg * 30 / 100;
  long long projBal  = bal + remainingWeeks * weekNet + mdEstimate;
  ImU32 projCol = projBal >= 0 ? IM_COL32(80, 215, 115, 220) : IM_COL32(220, 70, 70, 220);
  SummaryCard(origin.x + (card3W + kGap)*2.0f, origin.y, "SEASON PROJECTION",
              FmtMoney(projBal), projCol,
              card3W, "approx. end-of-season (excl. competitions prizes)");

  // ── Row 1b: Budget cards (transfer + wage) ──────────────────────────────
  float card2W  = (cw - kGap) / 2.0f;
  float row1bY  = headerTop + cardH1 + kGap;
  ImVec2 orig1b = ImVec2(win0.x + kPad, win0.y - scrollY + row1bY);

  // Transfer budget card
  {
    ImU32 tbCol = IM_COL32(147, 197, 253, 255); // sky blue — board allocation
    SummaryCard(orig1b.x, orig1b.y, "TRANSFER BUDGET",
                FmtMoney(fi.transferBudget), tbCol, card2W,
                "season allocation");
  }

  // Wage budget card
  {
    bool overBudget = fi.weeklyWages > fi.wageBudget;
    ImU32 wbCol = overBudget ? IM_COL32(220, 70, 70, 255) : IM_COL32(147, 197, 253, 255);
    std::string wbStr = FmtMoney(fi.wageBudget) + " / wk";
    std::string wbSub = overBudget ? "OVER BUDGET" : "board wage ceiling";
    SummaryCard(orig1b.x + card2W + kGap, orig1b.y, "WAGE BUDGET",
                wbStr, wbCol, card2W, wbSub.c_str());
  }

  // ── Row 2: Income / Expenses breakdown ──────────────────────────────────
  float row2Y  = row1bY + cardH1 + kGap;
  float col2W  = card2W;
  float row2H  = 180.0f;
  ImVec2 scr2  = ImVec2(win0.x + kPad, win0.y - scrollY + row2Y);

  // Income panel
  {
    ImVec2 p0(scr2.x, scr2.y), p1(scr2.x+col2W, scr2.y+row2H);
    dl->AddRectFilled(p0, p1, C32(kBgCard), 8.0f);
    dl->AddRect(p0, p1, C32(kBorder), 8.0f, 0, 1.0f);
    float ty = scr2.y + 10.0f;
    const float ix = scr2.x + 12.0f;
    const float iRight = scr2.x + col2W - 12.0f;
    const float kBarW = col2W - 24.0f, kBarH = 6.0f;

    PushMgrFont(g_ManagerFontBold);
    dl->AddText(ImVec2(ix, ty), IM_COL32(80,215,115,230), "INCOME");
    PopMgrFont(g_ManagerFontBold);
    ty += ImGui::GetTextLineHeight() + 8.0f;

    // TV Rights — weekly recurring
    {
      PushMgrFont(g_ManagerFontSmall);
      dl->AddText(ImVec2(ix, ty), IM_COL32(200,215,230,210), "TV Rights");
      std::string vs = FmtMoney(fi.weeklyTV) + "/wk";
      ImVec2 vsz = ImGui::CalcTextSize(vs.c_str());
      dl->AddText(ImVec2(iRight - vsz.x, ty), IM_COL32(80,215,115,230), vs.c_str());
      PopMgrFont(g_ManagerFontSmall);
      float barY = ty + ImGui::GetTextLineHeight() + 2.0f;
      float fillF = 1.0f; // TV rights is the baseline
      dl->AddRectFilled(ImVec2(ix,barY), ImVec2(ix+kBarW,barY+kBarH), IM_COL32(20,28,50,200), 3.0f);
      dl->AddRectFilled(ImVec2(ix,barY), ImVec2(ix+kBarW*fillF,barY+kBarH), IM_COL32(80,200,110,200), 3.0f);
      ty += ImGui::GetTextLineHeight() + kBarH + 10.0f;
    }

    // Match Day — per-game range (min–max per match)
    {
      PushMgrFont(g_ManagerFontSmall);
      dl->AddText(ImVec2(ix, ty), IM_COL32(200,215,230,210), "Match Day");
      std::string vs = FmtMoney(fi.matchdayMin) + " - " + FmtMoney(fi.matchdayMax) + "/game";
      ImVec2 vsz = ImGui::CalcTextSize(vs.c_str());
      dl->AddText(ImVec2(iRight - vsz.x, ty), IM_COL32(80,215,115,230), vs.c_str());
      PopMgrFont(g_ManagerFontSmall);
      float barY = ty + ImGui::GetTextLineHeight() + 2.0f;
      long long midVal = (fi.matchdayMin + fi.matchdayMax) / 2;
      long long refVal = fi.weeklyTV > 0 ? fi.weeklyTV : 1;
      float fillF = std::min(1.0f, (float)midVal / (float)refVal);
      dl->AddRectFilled(ImVec2(ix,barY), ImVec2(ix+kBarW,barY+kBarH), IM_COL32(20,28,50,200), 3.0f);
      dl->AddRectFilled(ImVec2(ix,barY), ImVec2(ix+kBarW*fillF,barY+kBarH), IM_COL32(80,200,110,200), 3.0f);
      ty += ImGui::GetTextLineHeight() + kBarH + 10.0f;
    }

    // Sponsorship income (if any active)
    if (fi.weeklySponsors > 0) {
      PushMgrFont(g_ManagerFontSmall);
      dl->AddText(ImVec2(ix, ty), IM_COL32(200,215,230,210), "Sponsorships");
      std::string vs = FmtMoney(fi.weeklySponsors) + "/wk";
      ImVec2 vsz = ImGui::CalcTextSize(vs.c_str());
      dl->AddText(ImVec2(iRight - vsz.x, ty), IM_COL32(147,197,253,230), vs.c_str());
      PopMgrFont(g_ManagerFontSmall);
      float barY = ty + ImGui::GetTextLineHeight() + 2.0f;
      float fillF = std::min(1.0f, (float)fi.weeklySponsors / (float)(fi.weeklyTV > 0 ? fi.weeklyTV : 1));
      dl->AddRectFilled(ImVec2(ix,barY), ImVec2(ix+kBarW,barY+kBarH), IM_COL32(20,28,50,200), 3.0f);
      dl->AddRectFilled(ImVec2(ix,barY), ImVec2(ix+kBarW*fillF,barY+kBarH), IM_COL32(100,160,240,200), 3.0f);
      ty += ImGui::GetTextLineHeight() + kBarH + 10.0f;
    }
  }

  // Expenses panel
  {
    float ex = scr2.x + col2W + kGap;
    ImVec2 p0(ex, scr2.y), p1(ex+col2W, scr2.y+row2H);
    dl->AddRectFilled(p0, p1, C32(kBgCard), 8.0f);
    dl->AddRect(p0, p1, C32(kBorder), 8.0f, 0, 1.0f);
    float ty = scr2.y + 10.0f;
    PushMgrFont(g_ManagerFontBold);
    dl->AddText(ImVec2(ex+12.0f, ty), IM_COL32(220,80,80,230), "EXPENSES");
    PopMgrFont(g_ManagerFontBold);
    ty += ImGui::GetTextLineHeight() + 6.0f;

    struct ExpRow { const char *label; long long weekly; };
    ExpRow rows[] = {
      { "Player & Staff Wages", fi.weeklyWages     },
      { "Club Operating Costs", fi.weeklyOperating },
    };
    long long maxExp = (fi.weeklyWages + fi.weeklyOperating) > 0
                       ? (fi.weeklyWages + fi.weeklyOperating) : 1;
    const float kBarW = col2W * 0.3f, kBarH = 7.0f;
    for (const auto &row : rows) {
      float fillF = std::min(1.0f, (float)row.weekly / (float)maxExp);
      PushMgrFont(g_ManagerFontSmall);
      dl->AddText(ImVec2(ex+12.0f, ty), IM_COL32(200,215,230,210), row.label);
      std::string vs = FmtMoney(row.weekly) + "/wk";
      ImVec2 vsz = ImGui::CalcTextSize(vs.c_str());
      dl->AddText(ImVec2(ex+col2W-vsz.x-12.0f, ty), IM_COL32(220,80,80,230), vs.c_str());
      PopMgrFont(g_ManagerFontSmall);
      float barX = ex + 12.0f;
      float barY = ty + ImGui::GetTextLineHeight() + 2.0f;
      dl->AddRectFilled(ImVec2(barX,barY), ImVec2(barX+kBarW,barY+kBarH),
                        IM_COL32(20,28,50,200), 3.0f);
      dl->AddRectFilled(ImVec2(barX,barY), ImVec2(barX+kBarW*fillF,barY+kBarH),
                        IM_COL32(200,80,80,200), 3.0f);
      ty += ImGui::GetTextLineHeight() + kBarH + 8.0f;
    }

    // Annual wage vs TV rights comparison bar
    ty += 4.0f;
    PushMgrFont(g_ManagerFontSmall);
    long long annualWages = fi.weeklyWages * 52;
    long long annualTV    = fi.weeklyTV    * 52;
    long long annualOp    = fi.weeklyOperating * 52;
    std::string wageStr = "Annual wage bill: " + FmtMoney(annualWages);
    dl->AddText(ImVec2(ex+12.0f, ty), IM_COL32(180,180,200,180), wageStr.c_str());
    PopMgrFont(g_ManagerFontSmall);
    (void)annualTV; (void)annualOp; // suppress unused warning
  }

  // ── Board Status ──────────────────────────────────────────────────────
  float boardRowH  = 44.0f;
  float boardRowY  = row2Y + row2H + kGap;
  float boardRowW  = cw;
  ImVec2 scrB = ImVec2(win0.x + kPad, win0.y - scrollY + boardRowY);
  {
    ImVec2 p0(scrB.x, scrB.y), p1(scrB.x + boardRowW, scrB.y + boardRowH);
    dl->AddRectFilled(p0, p1, C32(kBgCard), 8.0f);
    dl->AddRect(p0, p1, C32(kBorder), 8.0f, 0, 1.0f);

    float bx = scrB.x + 14.0f;
    float by = scrB.y + 13.0f;

    int conf = fi.boardConfidence;
    const char *confLabel;
    ImU32       confCol;
    if      (conf > 80) { confLabel = "Thriving";  confCol = IM_COL32(147,197,253,240); }
    else if (conf > 60) { confLabel = "Satisfied"; confCol = IM_COL32( 74,222,128,230); }
    else if (conf > 40) { confLabel = "Stable";    confCol = IM_COL32(250,204, 21,230); }
    else if (conf > 20) { confLabel = "Concerned"; confCol = IM_COL32(251,146, 60,230); }
    else                { confLabel = "Crisis";    confCol = IM_COL32(239, 68, 68,230); }

    PushMgrFont(g_ManagerFontSmall);
    dl->AddText(ImVec2(bx, by), C32(kTextDim), "Board Confidence:");
    ImVec2 labelSz = ImGui::CalcTextSize("Board Confidence:");
    dl->AddText(ImVec2(bx + labelSz.x + 8.0f, by), confCol, confLabel);
    PopMgrFont(g_ManagerFontSmall);

    if (fi.debtLevel > 0) {
      std::string debtHint = "Financial obligations outstanding: " + FmtMoney(fi.debtLevel);
      PushMgrFont(g_ManagerFontSmall);
      ImVec2 debtSz = ImGui::CalcTextSize(debtHint.c_str());
      dl->AddText(ImVec2(scrB.x + boardRowW - debtSz.x - 14.0f, by),
                  IM_COL32(239, 68, 68, 210), debtHint.c_str());
      PopMgrFont(g_ManagerFontSmall);
    }
  }

  // ── Sponsorship section ───────────────────────────────────────────────
  float sponsorY     = boardRowY + boardRowH + kGap;
  const int kMaxSlots = 4;
  float slotW        = (cw - kGap * 3.0f) / 4.0f;
  float sponsorCardH = 68.0f;
  float sponsorHdrH  = 26.0f;
  bool  hasPending   = !g_CareerHub.pendingOffers.empty();
  float pendingH     = hasPending ? (44.0f + kGap) : 0.0f;
  float sponsorPanelH = sponsorHdrH + sponsorCardH + kGap * 2.0f + pendingH;

  ImVec2 scrSp = ImVec2(win0.x + kPad, win0.y - scrollY + sponsorY);
  dl->AddRectFilled(scrSp, ImVec2(scrSp.x + cw, scrSp.y + sponsorPanelH), C32(kBgCard), 8.0f);
  dl->AddRect(scrSp,       ImVec2(scrSp.x + cw, scrSp.y + sponsorPanelH), C32(kBorder), 8.0f, 0, 1.0f);

  // Section header
  {
    PushMgrFont(g_ManagerFontBold);
    dl->AddText(ImVec2(scrSp.x + 12.0f, scrSp.y + 5.0f),
                IM_COL32(200, 215, 240, 230), "SPONSORSHIPS");
    PopMgrFont(g_ManagerFontBold);
    char slotTxt[32];
    snprintf(slotTxt, sizeof(slotTxt), "%d / 4 active",
             (int)g_CareerHub.activeSponsors.size());
    PushMgrFont(g_ManagerFontSmall);
    ImVec2 stSz = ImGui::CalcTextSize(slotTxt);
    dl->AddText(ImVec2(scrSp.x + cw - stSz.x - 14.0f, scrSp.y + 7.0f),
                IM_COL32(140, 160, 190, 200), slotTxt);
    PopMgrFont(g_ManagerFontSmall);
  }

  // Sponsor slot cards
  float slotRowY = sponsorY + sponsorHdrH + 4.0f;
  for (int s = 0; s < kMaxSlots; s++) {
    float sx = win0.x + kPad + s * (slotW + kGap);
    float sy = win0.y - scrollY + slotRowY;
    ImVec2 sp0(sx, sy), sp1(sx + slotW, sy + sponsorCardH);
    bool active = s < (int)g_CareerHub.activeSponsors.size();
    ImU32 bgC = active ? IM_COL32(22, 38, 72, 220) : IM_COL32(14, 20, 40, 160);
    ImU32 bdC = active ? IM_COL32(60, 100, 180, 180) : IM_COL32(40, 50, 80, 120);
    dl->AddRectFilled(sp0, sp1, bgC, 6.0f);
    dl->AddRect(sp0, sp1, bdC, 6.0f, 0, 1.0f);
    if (active) {
      const auto &sc = g_CareerHub.activeSponsors[s];
      std::string nm = sc.sponsorName;
      if (nm.size() > 15) nm = nm.substr(0, 14) + ".";
      PushMgrFont(g_ManagerFontSmall);
      dl->AddText(ImVec2(sx + 8.0f, sy + 8.0f), IM_COL32(200, 220, 255, 220), nm.c_str());
      PopMgrFont(g_ManagerFontSmall);
      PushMgrFont(g_ManagerFontMedium);
      std::string vs = FmtMoney(sc.weeklyValue) + "/wk";
      dl->AddText(ImVec2(sx + 8.0f, sy + 28.0f), IM_COL32(80, 215, 115, 230), vs.c_str());
      PopMgrFont(g_ManagerFontMedium);
      PushMgrFont(g_ManagerFontSmall);
      dl->AddText(ImVec2(sx + 8.0f, sy + 52.0f), IM_COL32(100, 130, 170, 180), "Active");
      PopMgrFont(g_ManagerFontSmall);
    } else {
      PushMgrFont(g_ManagerFontSmall);
      dl->AddText(ImVec2(sx + 8.0f, sy + 28.0f), IM_COL32(70, 90, 130, 150), "Open slot");
      PopMgrFont(g_ManagerFontSmall);
    }
  }

  // Pending offer strip
  if (hasPending) {
    const auto &offer = g_CareerHub.pendingOffers[0];
    float offerRowY   = slotRowY + sponsorCardH + kGap;
    float offerScrY   = win0.y - scrollY + offerRowY;
    float offerH      = 44.0f;
    float btnW        = 88.0f, btnH = 28.0f;

    dl->AddRectFilled(ImVec2(scrSp.x + 6.0f, offerScrY),
                      ImVec2(scrSp.x + cw - 6.0f, offerScrY + offerH),
                      IM_COL32(18, 32, 68, 230), 6.0f);
    dl->AddRect(ImVec2(scrSp.x + 6.0f, offerScrY),
                ImVec2(scrSp.x + cw - 6.0f, offerScrY + offerH),
                IM_COL32(80, 130, 220, 160), 6.0f, 0, 1.0f);

    PushMgrFont(g_ManagerFontSmall);
    std::string offerLine = "Offer: " + offer.sponsorName;
    dl->AddText(ImVec2(scrSp.x + 16.0f, offerScrY + 5.0f),
                IM_COL32(200, 220, 255, 230), offerLine.c_str());
    std::string valLine = FmtMoney(offer.weeklyValue) + " / week";
    dl->AddText(ImVec2(scrSp.x + 16.0f, offerScrY + 23.0f),
                IM_COL32(80, 215, 115, 230), valLine.c_str());
    PopMgrFont(g_ManagerFontSmall);

    float bx = scrSp.x + cw - btnW * 2.0f - kGap * 2.5f;
    float by = offerScrY + (offerH - btnH) * 0.5f;

    ImGui::SetCursorScreenPos(ImVec2(bx, by));
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(30, 120, 60, 220));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(40, 160, 80, 240));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(20, 100, 50, 255));
    if (ImGui::Button("Accept##spA", ImVec2(btnW, btnH))) {
      int sYear = g_CareerHub.seasonYear;
      std::stringstream aiq;
      aiq << "INSERT OR IGNORE INTO club_sponsors"
          << " (manager_id,club_id,sponsor_id,sponsor_name,weekly_value,season_year)"
          << " VALUES (" << g_CareerHub.managerId << "," << g_CareerHub.clubId << ","
          << offer.sponsorId << ",'" << SqlEsc(offer.sponsorName) << "',"
          << offer.weeklyValue << "," << sYear << ");";
      DatabaseResult *air = GetDB()->Query(aiq.str());
      delete air;
      std::stringstream adq;
      adq << "UPDATE pending_sponsor_offers SET status='accepted' WHERE id=" << offer.id << ";";
      DatabaseResult *adr = GetDB()->Query(adq.str());
      delete adr;
      CareerHubState::SponsorContract nsc;
      nsc.id = 0; nsc.sponsorId = offer.sponsorId;
      nsc.sponsorName = offer.sponsorName; nsc.weeklyValue = offer.weeklyValue;
      nsc.seasonYear  = sYear;
      g_CareerHub.activeSponsors.push_back(nsc);
      g_CareerHub.finances.weeklySponsors += offer.weeklyValue;
      g_CareerHub.pendingOffers.erase(g_CareerHub.pendingOffers.begin());
    }
    ImGui::PopStyleColor(3);

    ImGui::SetCursorScreenPos(ImVec2(bx + btnW + kGap, by));
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(100, 30, 30, 200));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(150, 45, 45, 230));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(80, 20, 20, 255));
    if (ImGui::Button("Decline##spD", ImVec2(btnW, btnH))) {
      std::stringstream ddq;
      ddq << "UPDATE pending_sponsor_offers SET status='declined' WHERE id=" << offer.id << ";";
      DatabaseResult *ddr = GetDB()->Query(ddq.str());
      delete ddr;
      g_CareerHub.pendingOffers.erase(g_CareerHub.pendingOffers.begin());
    }
    ImGui::PopStyleColor(3);
  }

  // ── Ledger ────────────────────────────────────────────────────────────
  float ledgerTop = sponsorY + sponsorPanelH + kGap;
  float ledgerH   = h - ledgerTop - 8.0f;
  if (ledgerH < 60.0f) ledgerH = 60.0f;

  // Header row
  float hdrH = 22.0f;
  ImVec2 scr4 = ImVec2(win0.x + kPad, win0.y - scrollY + ledgerTop);
  {
    dl->AddRectFilled(scr4, ImVec2(scr4.x+cw, scr4.y+hdrH),
                      IM_COL32(15,22,42,220), 0.0f);
    PushMgrFont(g_ManagerFontSmall);
    float dateW = 90.0f, catW = 110.0f;
    dl->AddText(ImVec2(scr4.x+6.0f,        scr4.y+4.0f), C32(kTextDim), "Date");
    dl->AddText(ImVec2(scr4.x+dateW,        scr4.y+4.0f), C32(kTextDim), "Category");
    dl->AddText(ImVec2(scr4.x+dateW+catW,   scr4.y+4.0f), C32(kTextDim), "Description");
    ImVec2 amtLbl = ImGui::CalcTextSize("Amount");
    dl->AddText(ImVec2(scr4.x+cw-amtLbl.x-6.0f, scr4.y+4.0f), C32(kTextDim), "Amount");
    PopMgrFont(g_ManagerFontSmall);
    ledgerH -= hdrH;
  }
  ImGui::SetCursorPos(ImVec2(kPad, ledgerTop + hdrH));

  ImGui::BeginChild("##fin_ledger", ImVec2(cw, ledgerH), false, 0);
  ImDrawList *ldl = ImGui::GetWindowDrawList();
  float lx = ImGui::GetCursorScreenPos().x;
  float ly = ImGui::GetCursorScreenPos().y;
  float rowH = 22.0f;
  float dateW = 90.0f, catW = 110.0f, amtW = 100.0f;

  for (int i = 0; i < (int)fi.recent.size(); i++) {
    const auto &tx = fi.recent[i];
    ImVec2 r0(lx, ly), r1(lx+cw, ly+rowH);
    ImU32 rowBg = (i%2==0) ? IM_COL32(12,18,36,200) : IM_COL32(18,26,50,200);
    ldl->AddRectFilled(r0, r1, rowBg, 0.0f);

    // Highlight income/expense with left-edge accent strip
    ImU32 stripCol = tx.amount >= 0 ? IM_COL32(60,180,90,200) : IM_COL32(180,60,60,200);
    ldl->AddRectFilled(ImVec2(lx,ly), ImVec2(lx+3.0f,ly+rowH), stripCol, 0.0f);

    std::string desc = tx.description;
    if (tx.category == "matchday") {
      if (desc.size() > 2 && desc[0] == 'H' && desc[1] == ':') desc = "Home gate receipts";
      else if (desc.size() > 2 && desc[0] == 'A' && desc[1] == ':') desc = "Away allocation";
      else { // legacy numeric-only descriptions
        bool allDigits = !desc.empty();
        for (char c : desc) if (!isdigit((unsigned char)c)) { allDigits = false; break; }
        if (allDigits) desc = "Home gate receipts";
      }
    }

    PushMgrFont(g_ManagerFontSmall);
    // Date
    std::string dispDate = tx.date.size() >= 10 ? FormatDateDisplay(tx.date) : tx.date;
    ldl->AddText(ImVec2(lx+6.0f, ly+4.0f), IM_COL32(160,175,210,200), dispDate.c_str());
    // Category pill
    ImU32 catCol = FinanceCatColor(tx.category);
    std::string catLbl = FinanceCatLabel(tx.category);
    ldl->AddText(ImVec2(lx+dateW, ly+4.0f), catCol, catLbl.c_str());
    // Description
    ldl->AddText(ImVec2(lx+dateW+catW, ly+4.0f),
                 IM_COL32(200,210,230,200), desc.c_str());
    // Amount
    std::string amtStr = (tx.amount >= 0 ? "+" : "") + FmtMoney(tx.amount);
    ImU32 amtCol = tx.amount >= 0 ? IM_COL32(80,215,110,230) : IM_COL32(215,80,80,230);
    ImVec2 amtSz = ImGui::CalcTextSize(amtStr.c_str());
    ldl->AddText(ImVec2(lx+cw-amtSz.x-6.0f, ly+4.0f), amtCol, amtStr.c_str());
    PopMgrFont(g_ManagerFontSmall);

    ly += rowH;
    ImGui::SetCursorScreenPos(ImVec2(lx, ly));
  }

  if (fi.recent.empty()) {
    PushMgrFont(g_ManagerFontSmall);
    ImGui::SetCursorScreenPos(ImVec2(lx+12.0f, ly+8.0f));
    ImGui::TextUnformatted("No transactions yet.");
    PopMgrFont(g_ManagerFontSmall);
  }

  ImGui::Dummy(ImVec2(cw, 8.0f));
  ImGui::EndChild();
}

// ---- DrawScoutingPage ---------------------------------------------------

// Shared helper: draws one scout card (report or queue).
// Returns true if the "View" button was clicked (only for report cards).
static bool DrawScoutCard(ImDrawList *dl, float ox, float oy, float cw, float ch,
                          const std::string &firstName, const std::string &lastName,
                          const std::string &role,      const std::string &age,
                          const std::string &clubName,  const std::string &clubLogoPath,
                          const std::string &clubShortName,
                          int uid,
                          // report-only fields (revealPct >= 0 means it's a report card)
                          float revealPct,
                          // queue-only
                          const std::string &dueDate,
                          bool *cancelClicked) {
  const float kRad  = 10.0f;
  const float kFaceW = 66.0f;
  const float kFaceH = ch - 16.0f; // fill most of the card height

  // ---- Card background ----
  dl->AddRectFilled(ImVec2(ox, oy), ImVec2(ox+cw, oy+ch), C32(kBgCard), kRad);
  dl->AddRect(ImVec2(ox, oy), ImVec2(ox+cw, oy+ch), C32(kBorder), kRad, 0, 1.0f);
  // Subtle top highlight line
  dl->AddLine(ImVec2(ox+kRad, oy+1), ImVec2(ox+cw-kRad, oy+1),
              IM_COL32(255,255,255,8), 1.0f);

  // ---- Facecard (left) ----
  float fx = ox + 12.0f;
  float fy = oy + (ch - kFaceH) * 0.5f;
  GLuint face = GetDefaultFaceTex();
  if (face) {
    dl->AddImageRounded((ImTextureID)(intptr_t)face,
                        ImVec2(fx, fy), ImVec2(fx+kFaceW, fy+kFaceH),
                        ImVec2(0,0), ImVec2(1,1),
                        IM_COL32(255,255,255,220), kRad);
  } else {
    dl->AddRectFilled(ImVec2(fx, fy), ImVec2(fx+kFaceW, fy+kFaceH),
                      IM_COL32(22,32,62,240), kRad);
  }
  // Face border tinted with accent
  dl->AddRect(ImVec2(fx, fy), ImVec2(fx+kFaceW, fy+kFaceH),
              IM_COL32((int)(kAccent.x*255*0.7f),(int)(kAccent.y*255*0.7f),(int)(kAccent.z*255*0.7f),180),
              kRad, 0, 1.2f);

  // ---- Content area ----
  float cx2  = fx + kFaceW + 12.0f;
  float textY = oy + 11.0f;

  // Full name
  std::string fullName = firstName.empty() ? lastName
                       : (lastName.empty() ? firstName : firstName + " " + lastName);
  PushMgrFont(g_ManagerFontBold);
  dl->AddText(ImVec2(cx2, textY), IM_COL32(230,238,255,245), fullName.c_str());
  PopMgrFont(g_ManagerFontBold);
  textY += 19.0f;

  // Position pill + Age
  if (!role.empty()) {
    PushMgrFont(g_ManagerFontSmall);
    ImVec2 rSz = ImGui::CalcTextSize(role.c_str());
    float pW = rSz.x + 10.0f, pH = rSz.y + 3.0f;
    dl->AddRectFilled(ImVec2(cx2, textY), ImVec2(cx2+pW, textY+pH),
                      IM_COL32((int)(kAccent.x*255*0.3f),(int)(kAccent.y*255*0.3f),(int)(kAccent.z*255*0.3f),200),
                      4.0f);
    dl->AddRect(ImVec2(cx2, textY), ImVec2(cx2+pW, textY+pH),
                IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),120),
                4.0f, 0, 0.8f);
    dl->AddText(g_ManagerFontSmall, 13.0f,
                ImVec2(cx2+5.0f, textY+1.5f), C32(kAccent), role.c_str());
    PopMgrFont(g_ManagerFontSmall);

    if (!age.empty()) {
      char ageBuf[16]; snprintf(ageBuf, sizeof(ageBuf), "Age %s", age.c_str());
      PushMgrFont(g_ManagerFontSmall);
      dl->AddText(g_ManagerFontSmall, 13.0f,
                  ImVec2(cx2 + pW + 8.0f, textY + 1.0f),
                  IM_COL32(150,162,195,190), ageBuf);
      PopMgrFont(g_ManagerFontSmall);
    }
    textY += pH + 7.0f;
  }

  // Club badge + name
  {
    GLuint badgeTex = clubLogoPath.empty() ? 0 : LoadBadgeTex(clubLogoPath);
    const float kBdgSz = 16.0f;
    if (badgeTex) {
      dl->AddImage((ImTextureID)(intptr_t)badgeTex,
                   ImVec2(cx2, textY), ImVec2(cx2+kBdgSz, textY+kBdgSz));
    } else {
      // text fallback initials
      dl->AddRectFilled(ImVec2(cx2, textY), ImVec2(cx2+kBdgSz, textY+kBdgSz),
                        IM_COL32(30,42,80,220), 3.0f);
      if (!clubShortName.empty()) {
        char ini[3] = {clubShortName[0], 0, 0};
        PushMgrFont(g_ManagerFontSmall);
        dl->AddText(g_ManagerFontSmall, 10.0f,
                    ImVec2(cx2+3.0f, textY+2.0f), IM_COL32(200,215,240,220), ini);
        PopMgrFont(g_ManagerFontSmall);
      }
    }
    PushMgrFont(g_ManagerFontSmall);
    dl->AddText(g_ManagerFontSmall, 13.0f,
                ImVec2(cx2 + kBdgSz + 5.0f, textY + 1.0f),
                IM_COL32(145,160,200,210), clubName.c_str());
    PopMgrFont(g_ManagerFontSmall);
  }

  // ---- Right side: reveal bar (report) or due date (queue) ----
  const float kRightW = 130.0f;
  float rx2 = ox + cw - kRightW - 10.0f;

  bool viewClicked = false;

  if (revealPct >= 0.0f) {
    // === REPORT card ===
    int knownPct = (int)(revealPct * 100.0f);
    ImU32 barCol = knownPct >= 40 ? IM_COL32(80,215,110,255) :
                   knownPct >= 20 ? IM_COL32(230,185,50,255)  :
                                    IM_COL32(200,90,90,255);

    // "X% known" label
    char pctBuf[20]; snprintf(pctBuf, sizeof(pctBuf), "%d%% known", knownPct);
    PushMgrFont(g_ManagerFontSmall);
    ImVec2 pSz = ImGui::CalcTextSize(pctBuf);
    dl->AddText(ImVec2(rx2 + (kRightW - pSz.x) * 0.5f, oy + 12.0f), barCol, pctBuf);
    PopMgrFont(g_ManagerFontSmall);

    // Progress bar
    float bY = oy + 30.0f;
    float bH = 8.0f;
    dl->AddRectFilled(ImVec2(rx2, bY), ImVec2(rx2+kRightW, bY+bH),
                      IM_COL32(18,24,52,220), 4.0f);
    float fill = kRightW * revealPct;
    if (fill > 1.0f)
      dl->AddRectFilled(ImVec2(rx2, bY), ImVec2(rx2+fill, bY+bH), barCol, 4.0f);

    // View button
    float vBtnW = kRightW, vBtnH = 24.0f;
    float vBtnY = oy + ch - vBtnH - 10.0f;
    char viewId[32]; snprintf(viewId, sizeof(viewId), "View##scv_%d", uid);
    ImGui::SetCursorScreenPos(ImVec2(rx2, vBtnY));
    ImGui::PushStyleColor(ImGuiCol_Button,        C32(kAccent));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, C32(kAccentH));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  C32(kAccentA));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    PushMgrFont(g_ManagerFontSmall);
    viewClicked = ImGui::Button(viewId, ImVec2(vBtnW, vBtnH));
    PopMgrFont(g_ManagerFontSmall);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);

  } else {
    // === QUEUE card ===
    // Spinning dots / "Scouting..." label
    PushMgrFont(g_ManagerFontSmall);
    const char *scoutingLbl = "Scouting...";
    ImVec2 slSz = ImGui::CalcTextSize(scoutingLbl);
    dl->AddText(ImVec2(rx2 + (kRightW - slSz.x)*0.5f, oy + 11.0f),
                IM_COL32(160,185,230,210), scoutingLbl);
    PopMgrFont(g_ManagerFontSmall);

    // Due date
    std::string dispDue = dueDate.size() >= 10 ? FormatDateDisplay(dueDate) : dueDate;
    char dueBuf[32]; snprintf(dueBuf, sizeof(dueBuf), "Due %s", dispDue.c_str());
    PushMgrFont(g_ManagerFontSmall);
    ImVec2 dSz = ImGui::CalcTextSize(dueBuf);
    dl->AddText(ImVec2(rx2 + (kRightW - dSz.x)*0.5f, oy + 29.0f),
                IM_COL32(130,155,200,190), dueBuf);
    PopMgrFont(g_ManagerFontSmall);

    // Scout rating stars
    float starsY = oy + 50.0f;
    ImGui::SetCursorScreenPos(ImVec2(rx2 + (kRightW - 60.0f)*0.5f, starsY));

    // Cancel button
    float cBtnW = kRightW, cBtnH = 24.0f;
    float cBtnY = oy + ch - cBtnH - 10.0f;
    char cancelId[32]; snprintf(cancelId, sizeof(cancelId), "Cancel##scq_%d", uid);
    ImGui::SetCursorScreenPos(ImVec2(rx2, cBtnY));
    ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(120,30,30,200));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(170,45,45,230));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(90,20,20,255));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 5.0f);
    PushMgrFont(g_ManagerFontSmall);
    if (ImGui::Button(cancelId, ImVec2(cBtnW, cBtnH)) && cancelClicked)
      *cancelClicked = true;
    PopMgrFont(g_ManagerFontSmall);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
  }

  // Vertical divider between content and right panel
  float divX = rx2 - 8.0f;
  dl->AddLine(ImVec2(divX, oy+12.0f), ImVec2(divX, oy+ch-12.0f),
              IM_COL32(255,255,255,18), 1.0f);

  return viewClicked;
}

static void DrawScoutingPage(float w, float h) {
  const float kPad  = 14.0f;
  const float kGap  = 14.0f;
  const float kCardH = 108.0f;
  const float kCardGap = 8.0f;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  float usW  = w - kPad * 2.0f;
  float halfW = (usW - kGap) * 0.5f;
  float colH  = h - 16.0f;

  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 origin  = ImGui::GetCursorScreenPos();

  // Helper: draw a column header bar
  auto DrawColHeader = [&](float px, float py, const char *label, int count) {
    dl->AddRectFilled(ImVec2(px, py), ImVec2(px+halfW, py+34.0f),
                      IM_COL32(14,22,52,240), 8.0f);
    dl->AddLine(ImVec2(px+10, py+33), ImVec2(px+halfW-10, py+33),
                C32(kAccent), 1.5f);
    PushMgrFont(g_ManagerFontSmall);
    dl->AddText(ImVec2(px+14.0f, py+9.0f),
                IM_COL32(200,215,245,240), label);
    if (count > 0) {
      char cntBuf[8]; snprintf(cntBuf, sizeof(cntBuf), "%d", count);
      ImVec2 cSz = ImGui::CalcTextSize(cntBuf);
      float pillX = px + halfW - cSz.x - 20.0f;
      dl->AddRectFilled(ImVec2(pillX-4, py+8), ImVec2(pillX+cSz.x+4, py+8+cSz.y+2),
                        IM_COL32((int)(kAccent.x*255*0.35f),(int)(kAccent.y*255*0.35f),(int)(kAccent.z*255*0.35f),200), 5.0f);
      dl->AddText(ImVec2(pillX, py+9.0f), C32(kAccent), cntBuf);
    }
    PopMgrFont(g_ManagerFontSmall);
  };

  // ---- Left: completed scout reports ------------------------------------
  {
    float px = origin.x, py = origin.y;
    DrawColHeader(px, py, "Scouted Players", (int)g_CareerHub.scoutReports.size());

    ImGui::SetCursorScreenPos(ImVec2(px, py + 38.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::BeginChild("##sc_done", ImVec2(halfW, colH - 40.0f), false,
                      ImGuiWindowFlags_None);
    ImGui::PopStyleVar();
    ImDrawList *wdl = ImGui::GetWindowDrawList();

    if (g_CareerHub.scoutReports.empty()) {
      float avH = ImGui::GetContentRegionAvail().y;
      ImGui::Dummy(ImVec2(halfW, avH * 0.35f));
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      float tw = ImGui::CalcTextSize("No scouted players yet.").x;
      ImGui::SetCursorPosX((halfW - tw) * 0.5f);
      ImGui::TextUnformatted("No scouted players yet.");
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);
    } else {
      float cardX = ImGui::GetCursorScreenPos().x + 6.0f;
      float cardW = ImGui::GetContentRegionAvail().x - 12.0f;

      for (unsigned int i = 0; i < g_CareerHub.scoutReports.size(); i++) {
        const auto &sr = g_CareerHub.scoutReports[i];
        float cardY = ImGui::GetCursorScreenPos().y + 4.0f;

        bool viewClicked = DrawScoutCard(wdl, cardX, cardY, cardW, kCardH,
          sr.firstName, sr.lastName, sr.role, sr.age,
          sr.clubName, sr.clubLogoPath, sr.clubShortName,
          sr.playerId, sr.revealPct, "", nullptr);

        if (viewClicked) {
          CareerHubState::Player op;
          LoadPlayerFullDetail(sr.playerId, op);
          if (op.id > 0) {
            s_detailPlayerOverride = op;
            s_detailOverrideActive = true;
            s_playerDetailId       = op.id;
            s_detailClubName       = sr.clubName;
            s_detailClubLogo       = sr.clubLogoPath;
            s_detailClubShortName  = sr.clubShortName;
            NavPush(PAGE_PLAYER_DETAIL);
          }
        }

        ImGui::SetCursorScreenPos(ImVec2(cardX, cardY + kCardH + kCardGap));
        ImGui::Dummy(ImVec2(cardW, 0.0f));
      }
      ImGui::Dummy(ImVec2(cardW, 8.0f));
    }
    ImGui::EndChild();
  }

  // ---- Right: in-progress queue -----------------------------------------
  {
    float px = origin.x + halfW + kGap, py = origin.y;
    DrawColHeader(px, py, "Awaiting Reports", (int)g_CareerHub.scoutQueue.size());

    ImGui::SetCursorScreenPos(ImVec2(px, py + 38.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0,0));
    ImGui::BeginChild("##sc_queue", ImVec2(halfW, colH - 40.0f), false,
                      ImGuiWindowFlags_None);
    ImGui::PopStyleVar();
    ImDrawList *wdl = ImGui::GetWindowDrawList();

    if (g_CareerHub.scoutQueue.empty()) {
      float avH = ImGui::GetContentRegionAvail().y;
      ImGui::Dummy(ImVec2(halfW, avH * 0.35f));
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      float tw = ImGui::CalcTextSize("No active scouting missions.").x;
      ImGui::SetCursorPosX((halfW - tw) * 0.5f);
      ImGui::TextUnformatted("No active scouting missions.");
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);
    } else {
      float cardX = ImGui::GetCursorScreenPos().x + 6.0f;
      float cardW = ImGui::GetContentRegionAvail().x - 12.0f;

      for (unsigned int i = 0; i < g_CareerHub.scoutQueue.size(); i++) {
        const auto &sq = g_CareerHub.scoutQueue[i];
        float cardY = ImGui::GetCursorScreenPos().y + 4.0f;

        bool cancelled = false;
        DrawScoutCard(wdl, cardX, cardY, cardW, kCardH,
          sq.firstName, sq.lastName, sq.role, sq.age,
          sq.clubName, sq.clubLogoPath, sq.clubShortName,
          sq.playerId, -1.0f, sq.dueDate, &cancelled);

        if (cancelled)
          CancelScouting(g_CareerHub.managerId, sq.playerId);

        ImGui::SetCursorScreenPos(ImVec2(cardX, cardY + kCardH + kCardGap));
        ImGui::Dummy(ImVec2(cardW, 0.0f));
      }
      ImGui::Dummy(ImVec2(cardW, 8.0f));
    }
    ImGui::EndChild();
  }

  // Reserve layout space
  ImGui::SetCursorPos(ImVec2(kPad, 8.0f + colH));
  ImGui::Dummy(ImVec2(usW, 1.0f));
}

// ---- DrawTransfersPage --------------------------------------------------

static ImU32 TransferRoleColor(const std::string &role) {
  if (role == "GK")
    return IM_COL32(220, 170, 45, 225);
  if (role == "CB" || role == "LB" || role == "RB" || role == "LWB" ||
      role == "RWB" || role == "FB" || role == "WB" || role == "SW")
    return IM_COL32(70, 145, 235, 225);
  if (role == "CDM" || role == "CM" || role == "CAM" || role == "LM" ||
      role == "RM" || role == "DM" || role == "AM")
    return IM_COL32(72, 190, 118, 225);
  return IM_COL32(238, 86, 74, 225);
}

static std::string SqlLikeEscNoQuotes(const char *raw) {
  std::string out;
  if (!raw) return out;
  for (const char *p = raw; *p; ++p)
    if (*p != '\'') out += (char)tolower((unsigned char)*p);
  return out;
}

static void OpenPlayerDetailFromTransfer(int playerId, int clubId,
                                         const std::string &clubName,
                                         const std::string &clubLogo,
                                         const std::string &clubShort) {
  s_playerDetailId = playerId;
  if (clubId == g_CareerHub.clubId) {
    s_detailOverrideActive = false;
    s_detailClubName = s_detailClubLogo = s_detailClubShortName = "";
  } else {
    CareerHubState::Player op;
    LoadPlayerFullDetail(playerId, op);
    if (op.id <= 0) return;
    s_detailPlayerOverride = op;
    s_detailOverrideActive = true;
    s_detailClubName = clubName;
    s_detailClubLogo = clubLogo;
    s_detailClubShortName = clubShort;
  }
  NavPush(PAGE_PLAYER_DETAIL);
}

static float DrawTransferPosChips(ImDrawList *dl, float x, float y,
                                  const std::string &role,
                                  const std::string &altRaw) {
  PushMgrFont(g_ManagerFontSmall);
  auto drawChip = [&](const std::string &txt, ImU32 bg, ImU32 fg) {
    if (txt.empty()) return;
    ImVec2 ts = g_ManagerFontSmall
        ? g_ManagerFontSmall->CalcTextSizeA(12.0f, FLT_MAX, 0.0f, txt.c_str())
        : ImGui::CalcTextSize(txt.c_str());
    float w = ts.x + 12.0f;
    dl->AddRectFilled(ImVec2(x, y), ImVec2(x + w, y + 20.0f), bg, 4.0f);
    dl->AddText(g_ManagerFontSmall, 12.0f, ImVec2(x + 6.0f, y + 4.0f), fg, txt.c_str());
    x += w + 5.0f;
  };

  ImU32 roleBg = TransferRoleColor(role);
  drawChip(role, roleBg, IM_COL32(255,255,255,240));
  auto alt = ParseAltPositions(altRaw);
  for (int i = 0; i < (int)alt.size() && i < 3; i++)
    drawChip(alt[i], IM_COL32(45,58,95,150), IM_COL32(190,205,235,230));
  PopMgrFont(g_ManagerFontSmall);
  return x;
}

static std::string TransferStateLabel(const std::string &state, bool sellerApproved) {
  if (state == "initiated" || state == "offer_made")
    return sellerApproved ? "Accepted - awaiting talks" : "Awaiting review";
  if (state == "negotiating")
    return sellerApproved ? "Accepted - player talks" : "Negotiating";
  if (state == "counter_offer")
    return sellerApproved ? "Accepted - terms discussion" : "Counter offer";
  if (state == "player_waiting")
    return "Accepted - player deciding";
  if (state == "medical_pending")
    return "Accepted - medical pending";
  if (state == "stalled")
    return sellerApproved ? "Accepted - talks stalled" : "Talks stalled";
  if (state == "competing_bid")
    return sellerApproved ? "Accepted - competing interest" : "Competing bid";
  if (state == "completed")
    return "Completed";
  if (state == "collapsed")
    return "Collapsed";
  return state;
}

static void DrawLoanTransfersPanel(int panel, float cw, float tableH,
                                   int managerId, int userClubId) {
  if (panel == 3) {
    std::stringstream sq;
    sq << "SELECT p.id,p.firstname||' '||p.lastname,p.role,p.age,"
       << " COALESCE(pss.team_id,p.team_id),t.name,COALESCE(t.logo_url,''),"
       << " COALESCE(t.shortname,''),COALESCE(p.alternative_pos,''),p.weekly_wage"
       << " FROM player_market_status pms"
       << " JOIN players p ON p.id=pms.player_id"
       << " LEFT JOIN player_save_state pss ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
       << " JOIN teams t ON t.id=COALESCE(pss.team_id,p.team_id)"
       << " WHERE pms.manager_id=" << managerId
       << " AND pms.status='loan_listed'"
       << " AND NOT EXISTS (SELECT 1 FROM loan_deals ld WHERE ld.manager_id=" << managerId
       << "   AND ld.player_id=p.id AND ld.status IN ('accepted_pending_player','active'))"
       << " ORDER BY p.base_stat DESC LIMIT 80;";
    DatabaseResult *r = GetDB()->Query(sq.str().c_str());
    BeginModernCard("##loan_market", ImVec2(cw, tableH));
    float cardW = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
    ImGui::BeginChild("##loan_market_scroll", ImVec2(cardW, tableH - 18.0f), false);
    ImGui::PopStyleColor();
    PushMgrFont(g_ManagerFontSmall);
    if (!r || r->data.empty()) {
      ImGui::TextColored(kTextDim, "No loan-listed players.");
    } else {
      const float rowH = 62.0f;
      if (r) for (unsigned int i = 0; i < r->data.size(); i++) {
        int pid = atoi(DBCell(r,i,0).c_str());
        int parent = atoi(DBCell(r,i,4).c_str());
        long long wg = atoll(DBCell(r,i,9).c_str());
        ImVec2 rp = ImGui::GetCursorScreenPos();
        float rowW = ImGui::GetContentRegionAvail().x;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        bool hovered = ImGui::IsMouseHoveringRect(rp, ImVec2(rp.x + rowW, rp.y + rowH));
        dl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH),
                          hovered ? IM_COL32(25,36,64,235) : IM_COL32(16,24,46,220), 7.0f);
        dl->AddRect(rp, ImVec2(rp.x + rowW, rp.y + rowH), IM_COL32(255,255,255,18), 7.0f);

        ImGui::SetCursorScreenPos(ImVec2(rp.x + 12.0f, rp.y + 12.0f));
        DrawTeamBadge(DBCell(r,i,6), DBCell(r,i,7), 38.0f);

        ImGui::SetCursorScreenPos(ImVec2(rp.x + 64.0f, rp.y + 10.0f));
        ImGui::PushID(pid + 70000);
        if (ImGui::SmallButton(DBCell(r,i,1).c_str()))
          OpenPlayerDetailFromTransfer(pid, parent, DBCell(r,i,5), DBCell(r,i,6), DBCell(r,i,7));
        ImGui::PopID();
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + 66.0f, rp.y + 36.0f),
                    C32(kTextPri), ("Age " + DBCell(r,i,3)).c_str());
        DrawTransferPosChips(dl, rp.x + rowW * 0.40f, rp.y + 21.0f, DBCell(r,i,2), DBCell(r,i,8));
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + rowW - 260.0f, rp.y + 15.0f),
                    C32(kTextSec), DBCell(r,i,5).c_str());
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + rowW - 260.0f, rp.y + 37.0f),
                    C32(kTextPri), ("Wage " + FmtMoney(wg)).c_str());

        ImGui::SetCursorScreenPos(ImVec2(rp.x + rowW - 82.0f, rp.y + 17.0f));
        if (parent == userClubId) ImGui::TextColored(kTextDim, "Your club");
        else if (ImGui::SmallButton("Loan")) {
          s_loanPlayerId_g = pid;
          s_loanParentClubId_g = parent;
          s_loanReceivingClubId_g = userClubId;
          s_loanIsLoanIn_g = true;
          snprintf(s_loanPlayerName_g, sizeof(s_loanPlayerName_g), "%s", DBCell(r,i,1).c_str());
          snprintf(s_loanFeeStr_g, sizeof(s_loanFeeStr_g), "%d", 0);
          snprintf(s_loanWagePctStr_g, sizeof(s_loanWagePctStr_g), "%d", 70);
          snprintf(s_loanOptionFeeStr_g, sizeof(s_loanOptionFeeStr_g), "%d", 0);
          snprintf(s_loanMandatoryFeeStr_g, sizeof(s_loanMandatoryFeeStr_g), "%d", 0);
          snprintf(s_loanMandatoryAppsStr_g, sizeof(s_loanMandatoryAppsStr_g), "%d", 0);
          s_loanMandatoryModeIdx_g = 0;
          snprintf(s_loanEndDateStr_g, sizeof(s_loanEndDateStr_g), "%s", AddDays(g_CareerHub.currentDate, 180).c_str());
          s_openLoanPopup_g = true;
        }
        ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH + 8.0f));
        ImGui::Dummy(ImVec2(rowW, 0.0f));
      }
    }
    PopMgrFont(g_ManagerFontSmall);
    ImGui::EndChild();
    EndModernCard();
    if (r) delete r;
    return;
  }

  if (panel == 4) {
    std::stringstream sq;
    sq << "SELECT ld.id,p.firstname||' '||p.lastname,lp.name,rp.name,ld.loan_fee,"
       << "ld.monthly_wage_receiving_pct,ld.status,ld.end_date,"
       << "ld.option_to_buy_fee,ld.mandatory_buy_fee"
       << " FROM loan_deals ld JOIN players p ON p.id=ld.player_id"
       << " JOIN teams lp ON lp.id=ld.loaning_club_id"
       << " JOIN teams rp ON rp.id=ld.receiving_club_id"
       << " WHERE ld.manager_id=" << managerId
       << " AND ld.initiating_club_id=" << userClubId
       << " ORDER BY ld.id DESC LIMIT 80;";
    DatabaseResult *r = GetDB()->Query(sq.str().c_str());
    BeginModernCard("##my_loan_offers", ImVec2(cw, tableH));
    float cardW = ImGui::GetContentRegionAvail().x;
    if (ImGui::BeginTable("##my_loan_table", 8, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY,
                          ImVec2(cardW, tableH - 18.0f))) {
      ImGui::TableSetupColumn("Player"); ImGui::TableSetupColumn("Parent"); ImGui::TableSetupColumn("Loan club");
      ImGui::TableSetupColumn("Loan fee"); ImGui::TableSetupColumn("Borrower wage %"); ImGui::TableSetupColumn("End");
      ImGui::TableSetupColumn("Clauses"); ImGui::TableSetupColumn("Status"); ImGui::TableHeadersRow();
      if (r) for (unsigned int i = 0; i < r->data.size(); i++) {
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(DBCell(r,i,1).c_str());
        ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(DBCell(r,i,2).c_str());
        ImGui::TableSetColumnIndex(2); ImGui::TextUnformatted(DBCell(r,i,3).c_str());
        ImGui::TableSetColumnIndex(3); ImGui::Text("%s", FmtMoney(atoll(DBCell(r,i,4).c_str())).c_str());
        ImGui::TableSetColumnIndex(4); ImGui::Text("%s%%", DBCell(r,i,5).c_str());
        ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(DBCell(r,i,7).c_str());
        ImGui::TableSetColumnIndex(6); ImGui::Text("Opt %s / Mand %s",
          FmtMoney(atoll(DBCell(r,i,8).c_str())).c_str(), FmtMoney(atoll(DBCell(r,i,9).c_str())).c_str());
        ImGui::TableSetColumnIndex(7); ImGui::TextUnformatted(DBCell(r,i,6).c_str());
      }
      ImGui::EndTable();
    }
    EndModernCard();
    if (r) delete r;
    return;
  }

  if (panel == 5) {
    std::stringstream sq;
    sq << "SELECT ld.id,p.firstname||' '||p.lastname,rp.name,ld.loan_fee,"
       << "ld.monthly_wage_receiving_pct,ld.playing_time_promise,ld.end_date,"
       << "ld.player_id,COALESCE(rp.logo_url,''),COALESCE(rp.shortname,''),p.role,p.age,COALESCE(p.alternative_pos,''),"
       << "ld.option_to_buy_fee,ld.mandatory_buy_fee,ld.mandatory_buy_trigger,ld.mandatory_buy_appearances,"
       << "ld.status,ld.initiating_club_id"
       << " FROM loan_deals ld JOIN players p ON p.id=ld.player_id"
       << " JOIN teams rp ON rp.id=ld.receiving_club_id"
       << " WHERE ld.manager_id=" << managerId
       << " AND ld.loaning_club_id=" << userClubId
       << " AND ld.direction='incoming_loan_out'"
       << " AND ld.status IN ('offered','negotiating','accepted_pending_player')"
       << " ORDER BY p.lastname ASC, ld.loan_fee DESC;";
    DatabaseResult *r = GetDB()->Query(sq.str().c_str());
    BeginModernCard("##incoming_loan_offers", ImVec2(cw, tableH));
    float cardW = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##incoming_loan_scroll", ImVec2(cardW, tableH - 18.0f), false);
    PushMgrFont(g_ManagerFontSmall);
    if (!r || r->data.empty()) {
      ImGui::TextColored(kTextDim, "No incoming loan offers.");
    } else {
      struct LoanIncomingRow {
        int id = 0, pid = 0;
        long long fee = 0;
        int wagePct = 0;
        std::string player, club, promise, endDate, logo, shortName, role, age, alt;
        long long optionFee = 0, mandatoryFee = 0;
        int mandatoryApps = 0;
        int initiatingClubId = 0;
        std::string mandatoryTrigger, status;
      };
      std::vector<int> playerOrder;
      std::map<int, std::vector<LoanIncomingRow> > grouped;
      for (unsigned int i = 0; i < r->data.size(); i++) {
        LoanIncomingRow row;
        row.id = atoi(DBCell(r,i,0).c_str());
        row.player = DBCell(r,i,1);
        row.club = DBCell(r,i,2);
        row.fee = atoll(DBCell(r,i,3).c_str());
        row.wagePct = atoi(DBCell(r,i,4).c_str());
        row.promise = DBCell(r,i,5);
        row.endDate = DBCell(r,i,6);
        row.pid = atoi(DBCell(r,i,7).c_str());
        row.logo = DBCell(r,i,8);
        row.shortName = DBCell(r,i,9);
        row.role = DBCell(r,i,10);
        row.age = DBCell(r,i,11);
        row.alt = DBCell(r,i,12);
        row.optionFee = atoll(DBCell(r,i,13).c_str());
        row.mandatoryFee = atoll(DBCell(r,i,14).c_str());
        row.mandatoryTrigger = DBCell(r,i,15);
        row.mandatoryApps = atoi(DBCell(r,i,16).c_str());
        row.status = DBCell(r,i,17);
        row.initiatingClubId = atoi(DBCell(r,i,18).c_str());
        if (grouped.find(row.pid) == grouped.end()) playerOrder.push_back(row.pid);
        grouped[row.pid].push_back(row);
      }

      for (unsigned int gi = 0; gi < playerOrder.size(); gi++) {
        std::vector<LoanIncomingRow> &offers = grouped[playerOrder[gi]];
        if (offers.empty()) continue;
        const LoanIncomingRow &head = offers[0];
        if (s_incomingLoanGroupOpen_g.find(head.pid) == s_incomingLoanGroupOpen_g.end())
          s_incomingLoanGroupOpen_g[head.pid] = true;
        bool groupOpen = s_incomingLoanGroupOpen_g[head.pid];
        float groupH = 54.0f + (groupOpen ? (float)offers.size() * 42.0f : 0.0f);
        ImVec2 rp = ImGui::GetCursorScreenPos();
        float rowW = ImGui::GetContentRegionAvail().x;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + groupH), IM_COL32(16,24,46,220), 7.0f);
        dl->AddRect(rp, ImVec2(rp.x + rowW, rp.y + groupH), IM_COL32(255,255,255,18), 7.0f);

        ImGui::SetCursorScreenPos(ImVec2(rp.x + 12.0f, rp.y + 12.0f));
        ImGui::PushID(head.pid + 84000);
        if (ImGui::SmallButton(groupOpen ? "v" : ">"))
          s_incomingLoanGroupOpen_g[head.pid] = !groupOpen;
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(rp.x + 40.0f, rp.y + 12.0f));
        ImGui::PushID(head.pid + 83000);
        if (ImGui::SmallButton(head.player.c_str()))
          OpenPlayerDetailFromTransfer(head.pid, userClubId, g_CareerHub.club.name,
                                       g_CareerHub.club.logoPath, g_CareerHub.club.shortName);
        ImGui::PopID();
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + 14.0f, rp.y + 34.0f),
                    C32(kTextPri), ("Age " + head.age).c_str());
        DrawTransferPosChips(dl, rp.x + rowW * 0.34f, rp.y + 17.0f, head.role, head.alt);
        int activeCount = 0, waitingCount = 0, pendingCount = 0;
        for (unsigned int oi = 0; oi < offers.size(); oi++) {
          if (offers[oi].status == "accepted_pending_player") pendingCount++;
          else if (offers[oi].initiatingClubId == userClubId) waitingCount++;
          else activeCount++;
        }
        std::string countText = std::to_string(activeCount) + " active loan offer";
        if (activeCount != 1) countText += "s";
        if (waitingCount > 0) countText += " / " + std::to_string(waitingCount) + " waiting";
        if (pendingCount > 0) countText += " / " + std::to_string(pendingCount) + " pending";
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + rowW - 170.0f, rp.y + 18.0f),
                    C32(kTextSec), countText.c_str());

        float y = rp.y + 54.0f;
        if (groupOpen) for (unsigned int oi = 0; oi < offers.size(); oi++) {
          LoanIncomingRow &offer = offers[oi];
          ImU32 lineBg = (oi % 2 == 0) ? IM_COL32(22,31,56,170) : IM_COL32(18,27,50,130);
          dl->AddRectFilled(ImVec2(rp.x + 8.0f, y - 4.0f),
                            ImVec2(rp.x + rowW - 8.0f, y + 34.0f), lineBg, 5.0f);
          ImGui::SetCursorScreenPos(ImVec2(rp.x + 16.0f, y + 3.0f));
          DrawTeamBadge(offer.logo, offer.shortName, 24.0f);
          ImGui::SetCursorScreenPos(ImVec2(rp.x + 44.0f, y + 3.0f));
          ImGui::TextUnformatted(offer.club.c_str());
          dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + rowW * 0.34f, y + 6.0f),
                      C32(kTextPri), ("Loan fee " + FmtMoney(offer.fee)).c_str());
          char wageBuf[64];
          snprintf(wageBuf, sizeof(wageBuf), "They pay %d%% wage", offer.wagePct);
          dl->AddText(g_ManagerFontSmall, 12.0f, ImVec2(rp.x + rowW * 0.50f, y + 7.0f),
                      C32(kTextSec), wageBuf);
          dl->AddText(g_ManagerFontSmall, 12.0f, ImVec2(rp.x + rowW * 0.64f, y + 7.0f),
                      C32(kTextSec), offer.promise.c_str());
          std::string clause = "";
          if (offer.optionFee > 0) clause += "Opt " + FmtMoney(offer.optionFee);
          if (offer.mandatoryFee > 0) {
            if (!clause.empty()) clause += " / ";
            clause += "Mand " + FmtMoney(offer.mandatoryFee);
          }
          if (!clause.empty())
            dl->AddText(g_ManagerFontSmall, 12.0f, ImVec2(rp.x + rowW * 0.74f, y + 7.0f),
                        C32(kWarning), clause.c_str());

          ImGui::PushID(offer.id + 81000);
          ImGui::SetCursorScreenPos(ImVec2(rp.x + rowW - 190.0f, y + 1.0f));
          if (offer.status == "accepted_pending_player") {
            ImGui::TextColored(kTextDim, "Final Checks");
          } else if (offer.initiatingClubId == userClubId) {
            ImGui::TextColored(kTextDim, "Waiting Decision");
          } else {
            if (ImGui::SmallButton("Accept")) RespondToLoanOffer(managerId, offer.id, "accept", 0, 0);
            ImGui::SameLine(0.0f, 5.0f);
            if (ImGui::SmallButton("Review")) {
              s_loanReviewDealId_g = offer.id;
              snprintf(s_loanReviewFeeStr_g, sizeof(s_loanReviewFeeStr_g), "%lld", offer.fee);
              snprintf(s_loanReviewWagePctStr_g, sizeof(s_loanReviewWagePctStr_g), "%d", offer.wagePct);
              snprintf(s_loanReviewOptionFeeStr_g, sizeof(s_loanReviewOptionFeeStr_g), "%lld", offer.optionFee);
              snprintf(s_loanReviewMandatoryFeeStr_g, sizeof(s_loanReviewMandatoryFeeStr_g), "%lld", offer.mandatoryFee);
              snprintf(s_loanReviewMandatoryAppsStr_g, sizeof(s_loanReviewMandatoryAppsStr_g), "%d", offer.mandatoryApps);
              snprintf(s_loanReviewEndDateStr_g, sizeof(s_loanReviewEndDateStr_g), "%s", offer.endDate.c_str());
              s_loanReviewMandatoryModeIdx_g =
                offer.mandatoryTrigger == "appearances" ? 2 :
                (offer.mandatoryTrigger == "end_date" ? 1 : 0);
              snprintf(s_loanReviewPlayerName_g, sizeof(s_loanReviewPlayerName_g), "%s", offer.player.c_str());
              snprintf(s_loanReviewClubName_g, sizeof(s_loanReviewClubName_g), "%s", offer.club.c_str());
              s_openLoanReviewPopup_g = true;
            }
            ImGui::SameLine(0.0f, 5.0f);
            if (ImGui::SmallButton("Reject")) RespondToLoanOffer(managerId, offer.id, "reject", 0, 0);
          }
          ImGui::PopID();
          y += 42.0f;
        }
        ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + groupH + 8.0f));
        ImGui::Dummy(ImVec2(rowW, 0.0f));
      }
    }
    PopMgrFont(g_ManagerFontSmall);
    ImGui::EndChild();
    EndModernCard();
    if (r) delete r;
    return;
  }

  if (panel == 6) {
    std::stringstream sq;
    sq << "SELECT ld.id,p.firstname||' '||p.lastname,lp.name,rp.name,ld.end_date,"
       << "ld.monthly_wage_receiving_pct,ld.playing_time_promise,ld.appearances_so_far,"
       << "ld.option_to_buy_fee,ld.loaning_club_id,ld.receiving_club_id,"
       << "COALESCE(rp.logo_url,''),COALESCE(rp.shortname,''),p.role,p.age,COALESCE(p.alternative_pos,''),"
       << "ld.loan_fee,ld.mandatory_buy_fee,ld.player_id"
       << " FROM loan_deals ld JOIN players p ON p.id=ld.player_id"
       << " JOIN teams lp ON lp.id=ld.loaning_club_id"
       << " JOIN teams rp ON rp.id=ld.receiving_club_id"
       << " WHERE ld.manager_id=" << managerId
       << " AND ld.status='active'"
       << " AND (ld.loaning_club_id=" << userClubId << " OR ld.receiving_club_id=" << userClubId << ")"
       << " ORDER BY ld.end_date ASC;";
    DatabaseResult *r = GetDB()->Query(sq.str().c_str());
    BeginModernCard("##active_loans", ImVec2(cw, tableH));
    float cardW = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
    ImGui::BeginChild("##active_loan_scroll", ImVec2(cardW, tableH - 18.0f), false);
    ImGui::PopStyleColor();
    PushMgrFont(g_ManagerFontSmall);
    if (!r || r->data.empty()) {
      ImGui::TextColored(kTextDim, "No active loans.");
    } else {
      const float rowH = 70.0f;
      if (r) for (unsigned int i = 0; i < r->data.size(); i++) {
        int id = atoi(DBCell(r,i,0).c_str());
        int parent = atoi(DBCell(r,i,9).c_str());
        int receiving = atoi(DBCell(r,i,10).c_str());
        long long loanFee = atoll(DBCell(r,i,16).c_str());
        long long mandatoryFee = atoll(DBCell(r,i,17).c_str());
        int pid = atoi(DBCell(r,i,18).c_str());
        ImVec2 rp = ImGui::GetCursorScreenPos();
        float rowW = ImGui::GetContentRegionAvail().x;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH), IM_COL32(16,24,46,220), 7.0f);
        dl->AddRect(rp, ImVec2(rp.x + rowW, rp.y + rowH), IM_COL32(255,255,255,18), 7.0f);

        ImGui::SetCursorScreenPos(ImVec2(rp.x + 12.0f, rp.y + 16.0f));
        DrawTeamBadge(DBCell(r,i,11), DBCell(r,i,12), 38.0f);
        ImGui::SetCursorScreenPos(ImVec2(rp.x + 64.0f, rp.y + 10.0f));
        ImGui::PushID(id + 83000);
        if (ImGui::SmallButton(DBCell(r,i,1).c_str()))
          OpenPlayerDetailFromTransfer(pid, receiving, DBCell(r,i,3), DBCell(r,i,11), DBCell(r,i,12));
        ImGui::PopID();
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + 66.0f, rp.y + 34.0f),
                    C32(kTextPri), (DBCell(r,i,2) + " to " + DBCell(r,i,3)).c_str());
        DrawTransferPosChips(dl, rp.x + rowW * 0.36f, rp.y + 19.0f, DBCell(r,i,13), DBCell(r,i,15));
        char detail[160];
        snprintf(detail, sizeof(detail), "End %s  |  Borrower pays %s%%  |  Apps %s",
                 DBCell(r,i,4).c_str(), DBCell(r,i,5).c_str(), DBCell(r,i,7).c_str());
        dl->AddText(g_ManagerFontSmall, 12.0f, ImVec2(rp.x + rowW * 0.55f, rp.y + 13.0f),
                    C32(kTextSec), detail);
        std::string clauses;
        if (loanFee > 0) clauses += "Fee " + FmtMoney(loanFee);
        if (atoll(DBCell(r,i,8).c_str()) > 0) {
          if (!clauses.empty()) clauses += " / ";
          clauses += "Opt " + FmtMoney(atoll(DBCell(r,i,8).c_str()));
        }
        if (mandatoryFee > 0) {
          if (!clauses.empty()) clauses += " / ";
          clauses += "Mand " + FmtMoney(mandatoryFee);
        }
        dl->AddText(g_ManagerFontSmall, 12.0f, ImVec2(rp.x + rowW * 0.55f, rp.y + 38.0f),
                    C32(kTextPri), clauses.empty() ? DBCell(r,i,6).c_str() : clauses.c_str());

        ImGui::SetCursorScreenPos(ImVec2(rp.x + rowW - 92.0f, rp.y + 20.0f));
        ImGui::PushID(id + 82000);
        if (parent == userClubId && ImGui::SmallButton("Recall"))
          RespondToLoanOffer(managerId, id, "recall", 0, 0);
        if (receiving == userClubId && atoll(DBCell(r,i,8).c_str()) > 0) {
          if (parent == userClubId) ImGui::SameLine();
          if (ImGui::SmallButton("Buy")) RespondToLoanOffer(managerId, id, "exercise_option", 0, 0);
        }
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH + 8.0f));
        ImGui::Dummy(ImVec2(rowW, 0.0f));
      }
    }
    PopMgrFont(g_ManagerFontSmall);
    ImGui::EndChild();
    EndModernCard();
    if (r) delete r;
  }
}

static void DrawTransfersPage(float w, float h) {
  const float kPad = 16.0f;
  float cw = w - kPad * 2.0f;
  int managerId = g_CareerHub.managerId;
  int userClubId = g_CareerHub.clubId;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));

  // Sub-panel selector
  static int s_transferPanel = 0;
  static const char *kPanels[] = {
    "Market List", "My Bids", "Incoming",
    "Loan Market", "My Loan Offers", "Incoming Loans", "Active Loans"
  };
  if (s_transferPanel > 6) s_transferPanel = 0;
  ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.1f, 0.15f, 0.25f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.22f, 0.35f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.8f));
  const float tabGap = 4.0f;
  const float tabW = (cw - tabGap * 6.0f) / 7.0f;
  for (int i = 0; i < 7; i++) {
    if (i > 0) ImGui::SameLine(0.0f, tabGap);
    bool sel = (s_transferPanel == i);
    if (sel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccent.x*0.8f, kAccent.y*0.8f, kAccent.z*0.8f, 1.0f));
    if (ImGui::Button(kPanels[i], ImVec2(tabW, 28.0f))) s_transferPanel = i;
    if (sel) ImGui::PopStyleColor();
  }
  ImGui::PopStyleColor(3);
  ImGui::Spacing();

  // ---- Bid popup — rendered globally in DrawWorkspace, state via file-scope vars ----
  // Convenience aliases so the panel code below can write s_bidPlayerId etc.
  int  &s_bidPlayerId     = s_bidPlayerId_g;
  int  &s_bidSellerClubId = s_bidSellerClubId_g;
  int  &s_bidFee          = s_bidFee_g;
  int  &s_bidWage         = s_bidWage_g;
  char *s_bidFeeStr       = s_bidFeeStr_g;
  char *s_bidWageStr      = s_bidWageStr_g;
  char *s_bidPlayerName   = s_bidPlayerName_g;

  float tableH = h - 100.0f;

  // ---- Panel: Market List -------------------------------------------------
  if (s_transferPanel == 0) {
    static char s_availSearch[64] = "";
    static int  s_availRoleFilter = 0; // 0=All, 1=GK, 2=DEF, 3=MID, 4=FWD
    static int  s_marketPage = 0;
    static const char *kRoleFilters[] = {"All Pos", "GK", "DEF", "MID", "FWD"};

    if (!InTransferWindow(g_CareerHub.currentDate)) {
      BeginModernCard("##market_closed", ImVec2(cw, 110.0f));
      PushMgrFont(g_ManagerFontBold);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
      ImGui::TextUnformatted("Transfer market closed");
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontBold);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
      ImGui::TextUnformatted("The market list is available only while the transfer window is open.");
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);
      EndModernCard();
      return;
    }

    ImGui::SetNextItemWidth(180.0f);
    bool filterChanged = ImGui::InputText("##avail_search", s_availSearch, sizeof(s_availSearch));
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::SetNextItemWidth(90.0f);
    filterChanged = ImGui::Combo("##avail_role", &s_availRoleFilter, kRoleFilters, 5) || filterChanged;
    if (filterChanged) s_marketPage = 0;
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("Players listed by clubs in this save.");
    ImGui::PopStyleColor();
    ImGui::Spacing();
    tableH -= 30.0f;

    std::stringstream where;
    where << " FROM players p"
          << " LEFT JOIN player_save_state pss ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
          << " JOIN player_market_status pms ON pms.manager_id=" << managerId << " AND pms.player_id=p.id"
          << " JOIN teams t ON t.id=COALESCE(pss.team_id,p.team_id)"
          << " WHERE pms.status='transfer_listed'";
    if (s_availSearch[0]) {
      std::string srch = SqlLikeEscNoQuotes(s_availSearch);
      where << " AND (LOWER(p.firstname) LIKE '%" << srch << "%'"
            << " OR LOWER(p.lastname) LIKE '%" << srch << "%'"
            << " OR LOWER(COALESCE(p.nickname,'')) LIKE '%" << srch << "%')";
    }
    switch (s_availRoleFilter) {
      case 1: where << " AND p.role IN ('GK')"; break;
      case 2: where << " AND p.role IN ('CB','LB','RB','LWB','RWB','FB','WB','SW')"; break;
      case 3: where << " AND p.role IN ('CDM','CM','CAM','LM','RM','DM','AM')"; break;
      case 4: where << " AND p.role IN ('ST','CF','LW','RW','SS','W','WF','IF')"; break;
      default: break;
    }

    std::stringstream countQ;
    countQ << "SELECT COUNT(*)" << where.str() << ";";
    int totalRows = 0;
    if (DatabaseResult *cr = GetDB()->Query(countQ.str().c_str())) {
      if (!cr->data.empty()) totalRows = atoi(DBCell(cr,0,0).c_str());
      delete cr;
    }

    const int pageSize = 12;
    int totalPages = totalRows > 0 ? (totalRows + pageSize - 1) / pageSize : 1;
    if (s_marketPage >= totalPages) s_marketPage = totalPages - 1;
    if (s_marketPage < 0) s_marketPage = 0;

    ImGui::TextColored(kTextDim, "%d listed", totalRows);
    ImGui::SameLine(0.0f, 12.0f);
    if (ImGui::Button("<", ImVec2(28.0f, 24.0f)) && s_marketPage > 0) s_marketPage--;
    ImGui::SameLine(0.0f, 4.0f);
    ImGui::TextColored(kTextSec, "%d / %d", s_marketPage + 1, totalPages);
    ImGui::SameLine(0.0f, 4.0f);
    if (ImGui::Button(">", ImVec2(28.0f, 24.0f)) && s_marketPage < totalPages - 1) s_marketPage++;
    ImGui::Spacing();

    std::stringstream sq;
    sq << "SELECT p.id, p.firstname||' '||p.lastname, p.role, p.age,"
       << " t.name, COALESCE(NULLIF(pms.asking_price,0), p.playervalue),"
       << " COALESCE(pss.weekly_wage,p.weekly_wage), COALESCE(pss.team_id,p.team_id),"
       << " p.base_stat, COALESCE(t.logo_url,''), COALESCE(t.shortname,''), COALESCE(p.alternative_pos,'')"
       << where.str()
       << " ORDER BY p.base_stat DESC, p.playervalue DESC"
       << " LIMIT " << pageSize << " OFFSET " << (s_marketPage * pageSize) << ";";
    DatabaseResult *pr = GetDB()->Query(sq.str().c_str());

    BeginModernCard("##market_list", ImVec2(cw, tableH));
    float cardW = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
    ImGui::BeginChild("##market_scroll", ImVec2(cardW, tableH - 18.0f), false);
    ImGui::PopStyleColor();
    PushMgrFont(g_ManagerFontSmall);
    if (!pr || pr->data.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
      ImGui::TextUnformatted("No listed players match these filters.");
      ImGui::PopStyleColor();
    } else {
      const float rowH = 62.0f;
      for (unsigned int i = 0; i < pr->data.size(); i++) {
        int pid = atoi(DBCell(pr,i,0).c_str());
        int sellerClubId = atoi(DBCell(pr,i,7).c_str());
        long long val = atoll(DBCell(pr,i,5).c_str());
        long long wg  = atoll(DBCell(pr,i,6).c_str());
        std::string role = DBCell(pr,i,2);
        ImVec2 rp = ImGui::GetCursorScreenPos();
        float rowW = ImGui::GetContentRegionAvail().x;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        bool hovered = ImGui::IsMouseHoveringRect(rp, ImVec2(rp.x + rowW, rp.y + rowH));
        dl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH),
                          hovered ? IM_COL32(25, 36, 64, 235) : IM_COL32(16, 24, 46, 220), 7.0f);
        dl->AddRect(rp, ImVec2(rp.x + rowW, rp.y + rowH), IM_COL32(255,255,255,18), 7.0f);

        ImGui::SetCursorScreenPos(ImVec2(rp.x + 12.0f, rp.y + 12.0f));
        DrawTeamBadge(DBCell(pr,i,9), DBCell(pr,i,10), 38.0f);

        ImGui::SetCursorScreenPos(ImVec2(rp.x + 64.0f, rp.y + 10.0f));
        ImGui::PushID(pid + 10000);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
        if (ImGui::SmallButton(DBCell(pr,i,1).c_str())) {
          OpenPlayerDetailFromTransfer(pid, sellerClubId, DBCell(pr,i,4), DBCell(pr,i,9), DBCell(pr,i,10));
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + 66.0f, rp.y + 36.0f),
                    C32(kTextPri), ("Age " + DBCell(pr,i,3)).c_str());

        DrawTransferPosChips(dl, rp.x + rowW * 0.43f, rp.y + 21.0f, role, DBCell(pr,i,11));

        dl->AddText(g_ManagerFontSmall, 14.0f, ImVec2(rp.x + rowW - 250.0f, rp.y + 14.0f),
                    C32(kTextSec), ("Value " + FmtMoney(val)).c_str());
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + rowW - 250.0f, rp.y + 37.0f),
                    C32(kTextPri), ("Wage " + FmtMoney(wg)).c_str());

        ImGui::SetCursorScreenPos(ImVec2(rp.x + rowW - 82.0f, rp.y + 17.0f));
        ImGui::PushID(pid);
        bool ownMarketPlayer = (sellerClubId == userClubId);
        if (ownMarketPlayer) {
          ImGui::TextColored(kTextDim, "Your club");
        } else if (ImGui::Button("Bid", ImVec2(56.0f, 28.0f))) {
          s_bidPlayerId     = pid;
          s_bidSellerClubId = sellerClubId;
          s_bidFee          = (int)(val * 0.90);
          s_bidWage         = (int)(wg  * 1.10);
          snprintf(s_bidFeeStr,  32, "%d", s_bidFee);
          snprintf(s_bidWageStr, 32, "%d", s_bidWage);
          snprintf(s_bidPlayerName, 128, "%s", DBCell(pr,i,1).c_str());
          s_openBidPopup_g = true;
        }
        ImGui::PopID();

        ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH + 8.0f));
        ImGui::Dummy(ImVec2(rowW, 0.0f));
      }
    }
    PopMgrFont(g_ManagerFontSmall);
    ImGui::EndChild();
    EndModernCard();
    if (pr) delete pr;
  }


  // ---- Panel: My Bids -----------------------------------------------------
  else if (s_transferPanel == 1) {
    // Active bids
    std::stringstream sq;
    sq << "SELECT tn.id, p.firstname||' '||p.lastname, t.name,"
       << " tn.offered_fee, tn.state, tn.days_in_state, tn.negotiation_momentum,"
       << " tn.user_pending_action, tn.player_id, tn.selling_club_id,"
       << " p.role, p.age, COALESCE(p.alternative_pos,''), COALESCE(t.logo_url,''), COALESCE(t.shortname,'')"
       << " FROM transfer_negotiations tn"
       << " JOIN players p ON p.id=tn.player_id"
       << " JOIN teams t ON t.id=tn.selling_club_id"
       << " WHERE tn.manager_id=" << managerId
       << " AND tn.buying_club_id=" << userClubId
       << " AND tn.is_user_bid=1"
       << " AND tn.state NOT IN ('completed','collapsed')"
       << " ORDER BY p.lastname ASC, p.firstname ASC, tn.offered_fee DESC, tn.id DESC;";
    DatabaseResult *nr = GetDB()->Query(sq.str().c_str());

    float activeH = tableH * 0.55f;
    float histH   = tableH * 0.40f;

    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::TextUnformatted("Active Bids");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);

    BeginModernCard("##mybids_active_cards", ImVec2(cw, activeH));
    float activeCardW = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##mybids_active_scroll", ImVec2(activeCardW, activeH - 18.0f), false);
    PushMgrFont(g_ManagerFontSmall);
    if (!nr || nr->data.empty()) {
      ImGui::TextColored(kTextDim, "No active bids.");
    } else {
      const float rowH = 62.0f;
      for (unsigned int i = 0; i < nr->data.size(); i++) {
        int nid = atoi(DBCell(nr,i,0).c_str());
        int pid = atoi(DBCell(nr,i,8).c_str());
        int sellerClubId = atoi(DBCell(nr,i,9).c_str());
        std::string state = DBCell(nr,i,4);
        long long fee = atoll(DBCell(nr,i,3).c_str());
        int mom = atoi(DBCell(nr,i,6).c_str());
        ImVec2 rp = ImGui::GetCursorScreenPos();
        float rowW = ImGui::GetContentRegionAvail().x;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + rowH), IM_COL32(16,24,46,220), 7.0f);
        dl->AddRect(rp, ImVec2(rp.x + rowW, rp.y + rowH), IM_COL32(255,255,255,18), 7.0f);
        ImGui::SetCursorScreenPos(ImVec2(rp.x + 12.0f, rp.y + 12.0f));
        DrawTeamBadge(DBCell(nr,i,13), DBCell(nr,i,14), 38.0f);
        ImGui::SetCursorScreenPos(ImVec2(rp.x + 64.0f, rp.y + 10.0f));
        ImGui::PushID(pid + 50000);
        if (ImGui::SmallButton(DBCell(nr,i,1).c_str()))
          OpenPlayerDetailFromTransfer(pid, sellerClubId, DBCell(nr,i,2), DBCell(nr,i,13), DBCell(nr,i,14));
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + 66.0f, rp.y + 36.0f),
                    C32(kTextPri), ("Age " + DBCell(nr,i,11)).c_str());
        DrawTransferPosChips(dl, rp.x + rowW * 0.43f, rp.y + 21.0f, DBCell(nr,i,10), DBCell(nr,i,12));
        ImVec4 stateCol = (state == "counter_offer") ? kWarning : kTextSec;
        if (state == "player_talks" || state == "medical_pending") stateCol = kSuccess;
        dl->AddText(g_ManagerFontSmall, 14.0f, ImVec2(rp.x + rowW - 285.0f, rp.y + 14.0f),
                    C32(stateCol), state.c_str());
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + rowW - 285.0f, rp.y + 37.0f),
                    C32(kTextPri), ("Offer " + FmtMoney(fee) + "  Mom " + (mom >= 0 ? "+" : "") + std::to_string(mom)).c_str());
        if (state == "counter_offer") {
          ImGui::SetCursorScreenPos(ImVec2(rp.x + rowW - 180.0f, rp.y + 17.0f));
          ImGui::PushID(nid);
          if (ImGui::SmallButton("Accept")) RespondToOffer(managerId, nid, "accept_counter", 0, 0);
          ImGui::SameLine(0.0f, 5.0f);
          if (ImGui::SmallButton("Reject")) RespondToOffer(managerId, nid, "reject", 0, 0);
          ImGui::SameLine(0.0f, 5.0f);
          if (ImGui::SmallButton("Withdraw")) RespondToOffer(managerId, nid, "withdraw", 0, 0);
          ImGui::PopID();
        }
        ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + rowH + 8.0f));
        ImGui::Dummy(ImVec2(rowW, 0.0f));
      }
      delete nr;
    }
    PopMgrFont(g_ManagerFontSmall);
    ImGui::EndChild();
    EndModernCard();

    // Transfer history (completed + collapsed)
    ImGui::Spacing();
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("History");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);

    std::stringstream hsq;
    // cols: 0=name, 1=seller, 2=fee, 3=wage, 4=state, 5=date, 6=player_id, 7=selling_club_id
    hsq << "SELECT p.firstname||' '||p.lastname, t.name,"
        << " tn.offered_fee, tn.offered_wage, tn.state, tn.initiated_date,"
        << " tn.player_id, tn.selling_club_id"
        << " FROM transfer_negotiations tn"
        << " JOIN players p ON p.id=tn.player_id"
        << " JOIN teams t ON t.id=tn.selling_club_id"
        << " WHERE tn.manager_id=" << managerId
        << " AND tn.buying_club_id=" << userClubId
        << " AND tn.is_user_bid=1"
        << " AND tn.state IN ('completed','collapsed')"
        << " ORDER BY tn.id DESC LIMIT 40;";
    DatabaseResult *hr = GetDB()->Query(hsq.str().c_str());
    if (ImGui::BeginTable("##mybids_hist", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                           ImVec2(cw, histH))) {
      ImGui::TableSetupScrollFreeze(0,1);
      ImGui::TableSetupColumn("Player",  ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Seller",  ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Offer",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Result",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableSetupColumn("Date",    ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 50.0f);
      ImGui::TableHeadersRow();
      if (hr) {
        for (unsigned int i = 0; i < hr->data.size(); i++) {
          std::string state = DBCell(hr,i,4);
          ImVec4 stateCol = (state == "completed") ? kSuccess : kDanger;
          int hpid   = atoi(DBCell(hr,i,6).c_str());
          int hscid  = atoi(DBCell(hr,i,7).c_str());
          long long hfee  = atoll(DBCell(hr,i,2).c_str());
          long long hwage = atoll(DBCell(hr,i,3).c_str());
          ImGui::TableNextRow(0, 22.0f);
          ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(DBCell(hr,i,0).c_str());
          ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(DBCell(hr,i,1).c_str());
          ImGui::TableSetColumnIndex(2); ImGui::Text("%s", FmtMoney(hfee).c_str());
          ImGui::TableSetColumnIndex(3); ImGui::TextColored(stateCol, "%s", state.c_str());
          std::string dt = DBCell(hr,i,5);
          ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(dt.size()>=10?dt.substr(0,10).c_str():dt.c_str());
          ImGui::TableSetColumnIndex(5);
          if (state == "collapsed") {
            ImGui::PushID(hpid + 30000 + (int)i);
            if (ImGui::SmallButton("Bid")) {
              s_bidPlayerId     = hpid;
              s_bidSellerClubId = hscid;
              s_bidFee          = (int)hfee;
              s_bidWage         = (int)hwage;
              snprintf(s_bidFeeStr,    32,  "%d", s_bidFee);
              snprintf(s_bidWageStr,   32,  "%d", s_bidWage);
              snprintf(s_bidPlayerName,128, "%s", DBCell(hr,i,0).c_str());
              s_openBidPopup_g = true;
            }
            ImGui::PopID();
          }
        }
        delete hr;
      }
      ImGui::EndTable();
    }
  }

  // ---- Panel: Incoming Bids -----------------------------------------------
  else if (s_transferPanel == 2) {
    float activeH = tableH * 0.58f;
    float histH   = tableH * 0.36f;
    std::stringstream sq;
    sq << "SELECT tn.id, p.firstname||' '||p.lastname, t.name,"
       << " tn.offered_fee, tn.state, tn.days_in_state,"
       << " tn.player_id, tn.buying_club_id, p.role, p.age, COALESCE(p.alternative_pos,''),"
       << " COALESCE(t.logo_url,''), COALESCE(t.shortname,''), tn.seller_approved"
       << " FROM transfer_negotiations tn"
       << " JOIN players p ON p.id=tn.player_id"
       << " LEFT JOIN player_save_state pss"
       << "   ON pss.manager_id=tn.manager_id AND pss.player_id=p.id"
       << " JOIN teams t ON t.id=tn.buying_club_id"
       << " WHERE tn.manager_id=" << managerId
       << " AND tn.selling_club_id=" << userClubId
       << " AND COALESCE(pss.team_id,p.team_id)=" << userClubId
       << " AND tn.is_user_bid=0"
       << " AND tn.state NOT IN ('completed','collapsed')"
       << " ORDER BY p.lastname ASC, p.firstname ASC, tn.offered_fee DESC, tn.id DESC;";
    DatabaseResult *nr = GetDB()->Query(sq.str().c_str());
    BeginModernCard("##incoming_active_cards", ImVec2(cw, activeH));
    float activeCardW = ImGui::GetContentRegionAvail().x;
    ImGui::BeginChild("##incoming_active_scroll", ImVec2(activeCardW, activeH - 18.0f), false);
    PushMgrFont(g_ManagerFontSmall);
    if (!nr || nr->data.empty()) {
      ImGui::TextColored(kTextDim, "No incoming bids.");
    } else {
      struct IncomingOfferRow {
        int nid = 0, pid = 0, buyerId = 0;
        long long fee = 0;
        std::string player, buyer, state, role, age, alt, logo, shortName;
        bool sellerApproved = false;
      };
      std::vector<int> playerOrder;
      std::map<int, std::vector<IncomingOfferRow> > grouped;
      for (unsigned int i = 0; i < nr->data.size(); i++) {
        IncomingOfferRow row;
        row.nid = atoi(DBCell(nr,i,0).c_str());
        row.player = DBCell(nr,i,1);
        row.buyer = DBCell(nr,i,2);
        row.fee = atoll(DBCell(nr,i,3).c_str());
        row.state = DBCell(nr,i,4);
        row.pid = atoi(DBCell(nr,i,6).c_str());
        row.buyerId = atoi(DBCell(nr,i,7).c_str());
        row.role = DBCell(nr,i,8);
        row.age = DBCell(nr,i,9);
        row.alt = DBCell(nr,i,10);
        row.logo = DBCell(nr,i,11);
        row.shortName = DBCell(nr,i,12);
        row.sellerApproved = atoi(DBCell(nr,i,13).c_str()) != 0;
        if (grouped.find(row.pid) == grouped.end()) playerOrder.push_back(row.pid);
        grouped[row.pid].push_back(row);
      }

      for (unsigned int gi = 0; gi < playerOrder.size(); gi++) {
        std::vector<IncomingOfferRow> &offers = grouped[playerOrder[gi]];
        if (offers.empty()) continue;
        const IncomingOfferRow &head = offers[0];
        if (s_incomingOfferGroupOpen_g.find(head.pid) == s_incomingOfferGroupOpen_g.end())
          s_incomingOfferGroupOpen_g[head.pid] = true;
        bool groupOpen = s_incomingOfferGroupOpen_g[head.pid];
        float groupH = 54.0f + (groupOpen ? (float)offers.size() * 38.0f : 0.0f);
        ImVec2 rp = ImGui::GetCursorScreenPos();
        float rowW = ImGui::GetContentRegionAvail().x;
        ImDrawList *dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(rp, ImVec2(rp.x + rowW, rp.y + groupH), IM_COL32(16,24,46,220), 7.0f);
        dl->AddRect(rp, ImVec2(rp.x + rowW, rp.y + groupH), IM_COL32(255,255,255,18), 7.0f);

        ImGui::SetCursorScreenPos(ImVec2(rp.x + 12.0f, rp.y + 12.0f));
        ImGui::PushID(head.pid + 61000);
        if (ImGui::SmallButton(groupOpen ? "v" : ">"))
          s_incomingOfferGroupOpen_g[head.pid] = !groupOpen;
        ImGui::PopID();
        ImGui::SetCursorScreenPos(ImVec2(rp.x + 40.0f, rp.y + 12.0f));
        ImGui::PushID(head.pid + 60000);
        if (ImGui::SmallButton(head.player.c_str()))
          OpenPlayerDetailFromTransfer(head.pid, userClubId, g_CareerHub.club.name,
                                       g_CareerHub.club.logoPath, g_CareerHub.club.shortName);
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PopID();
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + 14.0f, rp.y + 34.0f),
                    C32(kTextPri), ("Age " + head.age).c_str());
        DrawTransferPosChips(dl, rp.x + rowW * 0.34f, rp.y + 17.0f, head.role, head.alt);
        std::string countText = std::to_string((int)offers.size()) + " active offer";
        if (offers.size() != 1) countText += "s";
        dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + rowW - 145.0f, rp.y + 18.0f),
                    C32(kTextSec), countText.c_str());

        float y = rp.y + 54.0f;
        if (groupOpen) for (unsigned int oi = 0; oi < offers.size(); oi++) {
          IncomingOfferRow &offer = offers[oi];
          ImU32 lineBg = (oi % 2 == 0) ? IM_COL32(22,31,56,170) : IM_COL32(18,27,50,130);
          dl->AddRectFilled(ImVec2(rp.x + 8.0f, y - 4.0f),
                            ImVec2(rp.x + rowW - 8.0f, y + 30.0f), lineBg, 5.0f);
          ImGui::SetCursorScreenPos(ImVec2(rp.x + 16.0f, y + 1.0f));
          DrawTeamBadge(offer.logo, offer.shortName, 24.0f);
          ImGui::SetCursorScreenPos(ImVec2(rp.x + 44.0f, y + 1.0f));
          ImGui::TextUnformatted(offer.buyer.c_str());
          dl->AddText(g_ManagerFontSmall, 13.0f, ImVec2(rp.x + rowW * 0.42f, y + 5.0f),
                      C32(kTextPri), FmtMoney(offer.fee).c_str());
          ImVec4 stateCol = offer.sellerApproved ? kSuccess : kTextSec;
          dl->AddText(g_ManagerFontSmall, 12.0f, ImVec2(rp.x + rowW * 0.56f, y + 6.0f),
                      C32(stateCol), TransferStateLabel(offer.state, offer.sellerApproved).c_str());

          ImGui::PushID(offer.nid + 20000);
          if (!offer.sellerApproved) {
            ImGui::SetCursorScreenPos(ImVec2(rp.x + rowW - 190.0f, y - 1.0f));
            if (ImGui::SmallButton("Accept")) {
              std::stringstream uq;
              uq << "UPDATE transfer_negotiations SET state='negotiating',seller_approved=1,days_in_state=0 WHERE id=" << offer.nid << ";";
              delete GetDB()->Query(uq.str().c_str());
              std::stringstream oq;
              oq << "UPDATE transfer_negotiations SET state='collapsed',"
                 << "collapse_reason='accepted_other_offer',days_in_state=0"
                 << " WHERE manager_id=" << managerId
                 << " AND selling_club_id=" << userClubId
                 << " AND player_id=" << offer.pid
                 << " AND id!=" << offer.nid
                 << " AND is_user_bid=0"
                 << " AND seller_approved=0"
                 << " AND state NOT IN ('completed','collapsed');";
              delete GetDB()->Query(oq.str().c_str());
              InsertInboxMessage(managerId, "Transfer offer accepted: " + offer.player,
                                 "You accepted " + offer.buyer + "'s offer. The deal is now moving to player talks.",
                                 "transfer", g_CareerHub.currentDate);
            }
            ImGui::SameLine(0.0f, 5.0f);
            if (ImGui::SmallButton("Review")) {
              s_reviewNegotiationId_g = offer.nid;
              snprintf(s_reviewCounterFeeStr_g, sizeof(s_reviewCounterFeeStr_g), "%lld", offer.fee);
              snprintf(s_reviewPlayerName_g, sizeof(s_reviewPlayerName_g), "%s", offer.player.c_str());
              snprintf(s_reviewBuyerName_g, sizeof(s_reviewBuyerName_g), "%s", offer.buyer.c_str());
              s_openReviewPopup_g = true;
            }
            ImGui::SameLine(0.0f, 5.0f);
            if (ImGui::SmallButton("Block")) {
              std::stringstream cq;
              cq << "INSERT INTO negotiation_cooldowns(manager_id,buying_club_id,player_id,cooldown_until,reason)"
                 << " SELECT " << managerId << ",buying_club_id,player_id,"
                 << "date('" << g_CareerHub.currentDate << "','+90 days'),'blocked'"
                 << " FROM transfer_negotiations WHERE id=" << offer.nid << ";";
              delete GetDB()->Query(cq.str().c_str());
              std::stringstream sq2;
              sq2 << "UPDATE transfer_negotiations SET state='collapsed',collapse_reason='blocked' WHERE id=" << offer.nid << ";";
              delete GetDB()->Query(sq2.str().c_str());
            }
          } else {
            dl->AddText(g_ManagerFontSmall, 12.0f, ImVec2(rp.x + rowW - 170.0f, y + 6.0f),
                        C32(kTextDim), "Waiting for final confirmation");
          }
          ImGui::PopID();
          y += 38.0f;
        }

        ImGui::SetCursorScreenPos(ImVec2(rp.x, rp.y + groupH + 8.0f));
        ImGui::Dummy(ImVec2(rowW, 0.0f));
      }
      delete nr;
    }
    PopMgrFont(g_ManagerFontSmall);
    ImGui::EndChild();
    EndModernCard();

    // ---- Incoming history (completed + collapsed) ------------------------
    ImGui::Spacing();
    PushMgrFont(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("History");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontSmall);

    std::stringstream hsq2;
    hsq2 << "SELECT p.firstname||' '||p.lastname, t.name,"
         << " tn.offered_fee, tn.offered_wage, tn.state, tn.initiated_date,"
         << " tn.player_id, tn.buying_club_id"
         << " FROM transfer_negotiations tn"
         << " JOIN players p ON p.id=tn.player_id"
         << " JOIN teams t ON t.id=tn.buying_club_id"
         << " WHERE tn.manager_id=" << managerId
         << " AND tn.selling_club_id=" << userClubId
         << " AND tn.is_user_bid=0"
         << " AND tn.state IN ('completed','collapsed')"
         << " ORDER BY tn.id DESC LIMIT 40;";
    DatabaseResult *hr2 = GetDB()->Query(hsq2.str().c_str());
    if (ImGui::BeginTable("##incoming_hist", 6, ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
                           ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp,
                           ImVec2(cw, histH))) {
      ImGui::TableSetupScrollFreeze(0,1);
      ImGui::TableSetupColumn("Player",  ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("From",    ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableSetupColumn("Offer",   ImGuiTableColumnFlags_WidthFixed, 80.0f);
      ImGui::TableSetupColumn("Result",  ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableSetupColumn("Date",    ImGuiTableColumnFlags_WidthFixed, 90.0f);
      ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 50.0f);
      ImGui::TableHeadersRow();
      if (hr2) {
        for (unsigned int i = 0; i < hr2->data.size(); i++) {
          std::string state = DBCell(hr2,i,4);
          ImVec4 stateCol = (state == "completed") ? kSuccess : kDanger;
          int hpid   = atoi(DBCell(hr2,i,6).c_str());
          int hbcid  = atoi(DBCell(hr2,i,7).c_str());
          long long hfee  = atoll(DBCell(hr2,i,2).c_str());
          long long hwage = atoll(DBCell(hr2,i,3).c_str());
          ImGui::TableNextRow(0, 22.0f);
          ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(DBCell(hr2,i,0).c_str());
          ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(DBCell(hr2,i,1).c_str());
          ImGui::TableSetColumnIndex(2); ImGui::Text("%s", FmtMoney(hfee).c_str());
          ImGui::TableSetColumnIndex(3); ImGui::TextColored(stateCol, "%s", state.c_str());
          std::string dt = DBCell(hr2,i,5);
          ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted(dt.size()>=10?dt.substr(0,10).c_str():dt.c_str());
          ImGui::TableSetColumnIndex(5);
          if (state == "collapsed") {
            ImGui::PushID(hpid + 30000 + (int)i);
            if (ImGui::SmallButton("Bid")) {
              s_bidPlayerId     = hpid;
              s_bidSellerClubId = hbcid;
              s_bidFee          = (int)hfee;
              s_bidWage         = (int)hwage;
              snprintf(s_bidFeeStr_g,    32, "%d", s_bidFee);
              snprintf(s_bidWageStr_g,   32, "%d", s_bidWage);
              snprintf(s_bidPlayerName_g,128, "%s", DBCell(hr2,i,0).c_str());
              s_openBidPopup_g = true;
            }
            ImGui::PopID();
          }
        }
        delete hr2;
      }
      ImGui::EndTable();
    }
  }
  else if (s_transferPanel >= 3 && s_transferPanel <= 6) {
    DrawLoanTransfersPanel(s_transferPanel, cw, tableH, managerId, userClubId);
  }
}

// ---- DrawComingSoonPage -------------------------------------------------

// ---- DrawNewsPage -------------------------------------------------------

static void DrawNewsPage(float w, float h) {
  const float kPad = 16.0f;
  float cw = w - kPad * 2.0f;
  float tableH = h - 60.0f;
  if (tableH < 80.0f) tableH = 80.0f;

  ImGui::SetCursorPos(ImVec2(kPad, 8.0f));
  BeginModernCard("##news_page", ImVec2(cw, h - 16.0f));

  PushMgrFont(g_ManagerFontSmall);
  ImGui::Spacing();

  int mid = g_CareerHub.managerId;
  std::stringstream sq;
  sq << "SELECT headline, category, game_date FROM transfer_news"
     << " WHERE manager_id=" << mid
     << " AND category NOT IN ('contract_renewal')"
     << " ORDER BY id DESC LIMIT 80;";
  DatabaseResult *nr = GetDB()->Query(sq.str().c_str());

  if (!nr || nr->data.size() == 0) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No news yet. News events will appear as the season progresses.");
    ImGui::PopStyleColor();
  } else {
    if (ImGui::BeginTable("##news_table", 3,
          ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY |
          ImGuiTableFlags_RowBg   | ImGuiTableFlags_SizingStretchProp,
          ImVec2(cw - 8.0f, tableH))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      ImGui::TableSetupColumn("Date",     ImGuiTableColumnFlags_WidthFixed,   90.0f);
      ImGui::TableSetupColumn("Category", ImGuiTableColumnFlags_WidthFixed,   80.0f);
      ImGui::TableSetupColumn("Headline", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableHeadersRow();

      for (unsigned int i = 0; i < nr->data.size(); i++) {
        std::string hl  = DBCell(nr, i, 0);
        std::string cat = DBCell(nr, i, 1);
        std::string dt  = DBCell(nr, i, 2);

        ImVec4 catCol = kTextSec;
        if      (cat == "completed") catCol = kSuccess;
        else if (cat == "collapsed") catCol = kDanger;
        else if (cat == "rumour")    catCol = ImVec4(0.9f, 0.85f, 0.4f, 1.0f);
        else if (cat == "warning")   catCol = kWarning;

        ImGui::TableNextRow(0, 22.0f);
        ImGui::TableSetColumnIndex(0);
        ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
        ImGui::TextUnformatted(dt.size() >= 10 ? dt.substr(0,10).c_str() : dt.c_str());
        ImGui::PopStyleColor();
        ImGui::TableSetColumnIndex(1);
        ImGui::TextColored(catCol, "%s", cat.c_str());
        ImGui::TableSetColumnIndex(2);
        ImGui::TextUnformatted(hl.c_str());
      }
      ImGui::EndTable();
    }
  }
  if (nr) delete nr;

  PopMgrFont(g_ManagerFontSmall);
  EndModernCard();
}

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
    if (s_prevPage == PAGE_STAFF_MARKET)
      s_staffMarketLoaded = false;
  }
  s_prevPage = g_activePage;

  switch (g_activePage) {
    case PAGE_HOME:          DrawHomePage(contentW, workH);          break;
    case PAGE_INBOX:         DrawInboxPage(contentW, workH);         break;
    case PAGE_SQUAD:         DrawSquadPage(contentW, workH);         break;
    case PAGE_TACTICS:       DrawTacticsPage(contentW, workH);       break;
    case PAGE_CALENDAR:      DrawCalendarPage(contentW, workH);      break;
    case PAGE_SCHEDULE:      DrawSchedulePage(contentW, workH);      break;
    case PAGE_COMPETITIONS:  DrawCompetitionsPage(contentW, workH);  break;
    case PAGE_PLAYER_DETAIL: DrawPlayerDetailPage(contentW, workH);  break;
    case PAGE_CLUB_DETAIL:   DrawClubDetailPage(contentW, workH);    break;
    case PAGE_STAFF:         DrawStaffPage(contentW, workH);         break;
    case PAGE_STAFF_MARKET:  DrawStaffMarketPage(contentW, workH);   break;
    case PAGE_SCOUTING:      DrawScoutingPage(contentW, workH);      break;
    case PAGE_FINANCES:      DrawFinancesPage(contentW, workH);      break;
    case PAGE_TRANSFERS:    DrawTransfersPage(contentW, workH);     break;
    case PAGE_NEWS:         DrawNewsPage(contentW, workH);          break;
    default:
      DrawComingSoonPage(contentW, workH, kPageNames[g_activePage]);
      break;
  }

  ImGui::EndChild();

  // ---- Global bid popup (triggered from any page including player detail) --
  if (s_openBidPopup_g) {
    ImGui::OpenPopup("##bid_modal_g");
    s_openBidPopup_g = false;
  }
  ImGui::SetNextWindowSize(ImVec2(500.0f, 300.0f), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                                 ImGui::GetIO().DisplaySize.y * 0.5f),
                           ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 26, 44, 245));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
  if (ImGui::BeginPopupModal("##bid_modal_g", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    int  managerId  = g_CareerHub.managerId;
    int  userClubId = g_CareerHub.clubId;
    static const char *kBidRolesG[] = { "rotation", "important", "star_player", "prospect" };
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float innerW = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p0, ImVec2(p0.x + innerW, p0.y + 54.0f), IM_COL32(14,22,46,230), 8.0f);
    PushMgrFont(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 10.0f));
    ImGui::TextUnformatted("Transfer Bid");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontBold);
    PushMgrFont(g_ManagerFontSmall);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 31.0f));
    ImGui::TextColored(kTextSec, "%s", s_bidPlayerName_g[0] ? s_bidPlayerName_g : "Selected player");
    PopMgrFont(g_ManagerFontSmall);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 68.0f));
    BeginModernCard("##bid_terms", ImVec2(innerW, 132.0f));
    PushMgrFont(g_ManagerFontSmall);
    ImGui::TextColored(kTextDim, "Offer terms");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("Transfer fee", s_bidFeeStr_g, sizeof(s_bidFeeStr_g));
    ImGui::SameLine(0.0f, 16.0f);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputText("Weekly wage", s_bidWageStr_g, sizeof(s_bidWageStr_g));
    ImGui::SetNextItemWidth(220.0f);
    ImGui::Combo("Promised Role", &s_bidRoleIdx_g, kBidRolesG, 4);
    PopMgrFont(g_ManagerFontSmall);
    EndModernCard();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + innerW - 202.0f, p0.y + 226.0f));
    if (CTAButton("Submit Bid", ImVec2(120.0f, 32.0f))) {
      int fee  = atoi(s_bidFeeStr_g);
      int wage = atoi(s_bidWageStr_g);
      if (fee > 0 && wage > 0 && s_bidPlayerId_g > 0) {
        InitiateUserBid(managerId, userClubId, s_bidPlayerId_g, s_bidSellerClubId_g,
                        fee, wage, kBidRolesG[s_bidRoleIdx_g], 0, 0,
                        g_CareerHub.currentDate, g_CareerHub.seasonYear);
        s_bidPlayerId_g = 0;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (SecBtn("Cancel", ImVec2(74.0f, 32.0f)))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (s_openLoanPopup_g) {
    ImGui::OpenPopup("##loan_modal_g");
    s_openLoanPopup_g = false;
  }
  ImGui::SetNextWindowSize(ImVec2(560.0f, 450.0f), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                                 ImGui::GetIO().DisplaySize.y * 0.5f),
                           ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 26, 44, 245));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
  if (ImGui::BeginPopupModal("##loan_modal_g", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    static const char *kLoanTime[] = {"rotation", "squad_player", "regular_starter", "important"};
    static const char *kMandatoryModes[] = {"none", "end_date", "appearances"};
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float innerW = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p0, ImVec2(p0.x + innerW, p0.y + 58.0f), IM_COL32(14,22,46,230), 8.0f);
    PushMgrFont(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 10.0f));
    ImGui::TextUnformatted(s_loanIsLoanIn_g ? "Loan Offer" : "Offer Player On Loan");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontBold);
    PushMgrFont(g_ManagerFontSmall);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 31.0f));
    ImGui::TextColored(kTextSec, "%s", s_loanPlayerName_g[0] ? s_loanPlayerName_g : "Selected player");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 74.0f));
    BeginModernCard("##loan_terms", ImVec2(innerW, 278.0f));
    if (!s_loanIsLoanIn_g) {
      ImGui::SetNextItemWidth(260.0f);
      ImGui::InputText("Find receiving club", s_loanClubSearchStr_g, sizeof(s_loanClubSearchStr_g));
      std::stringstream tq;
      tq << "SELECT id,name FROM teams WHERE transfer_budget > 0"
         << " AND id!=" << g_CareerHub.clubId;
      if (s_loanClubSearchStr_g[0]) {
        std::string srch = SqlLikeEscNoQuotes(s_loanClubSearchStr_g);
        tq << " AND LOWER(name) LIKE '%" << srch << "%'";
      }
      tq << " ORDER BY name LIMIT 6;";
      DatabaseResult *tr = GetDB()->Query(tq.str().c_str());
      if (tr) {
        ImGui::BeginChild("##loan_club_search_results", ImVec2(innerW - 12.0f, 48.0f), true);
        for (unsigned int i = 0; i < tr->data.size(); i++) {
          int id = atoi(DBCell(tr,i,0).c_str());
          bool sel = id == s_loanReceivingClubId_g;
          if (ImGui::Selectable(DBCell(tr,i,1).c_str(), sel))
            s_loanReceivingClubId_g = id;
        }
        ImGui::EndChild();
        delete tr;
      }
    }
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputText("Loan fee paid to parent", s_loanFeeStr_g, sizeof(s_loanFeeStr_g));
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SetNextItemWidth(130.0f);
    ImGui::InputText("Borrower wage %", s_loanWagePctStr_g, sizeof(s_loanWagePctStr_g));
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("End date", s_loanEndDateStr_g, sizeof(s_loanEndDateStr_g));
    ImGui::SetNextItemWidth(190.0f);
    ImGui::Combo("Playing time", &s_loanPlayingTimeIdx_g, kLoanTime, 4);
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputText("Option to buy fee", s_loanOptionFeeStr_g, sizeof(s_loanOptionFeeStr_g));
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SetNextItemWidth(150.0f);
    ImGui::InputText("Mandatory fee", s_loanMandatoryFeeStr_g, sizeof(s_loanMandatoryFeeStr_g));
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("Mandatory trigger", &s_loanMandatoryModeIdx_g, kMandatoryModes, 3);
    if (s_loanMandatoryModeIdx_g == 2) {
      ImGui::SameLine(0.0f, 12.0f);
      ImGui::SetNextItemWidth(100.0f);
      ImGui::InputText("Apps", s_loanMandatoryAppsStr_g, sizeof(s_loanMandatoryAppsStr_g));
    }
    PopMgrFont(g_ManagerFontSmall);
    EndModernCard();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + innerW - 216.0f, p0.y + 382.0f));
    if (CTAButton("Submit Loan", ImVec2(128.0f, 32.0f))) {
      int fee = atoi(s_loanFeeStr_g);
      int wagePct = atoi(s_loanWagePctStr_g);
      int optFee = atoi(s_loanOptionFeeStr_g);
      int mandatoryFee = atoi(s_loanMandatoryFeeStr_g);
      int mandatoryApps = atoi(s_loanMandatoryAppsStr_g);
      int parent = s_loanParentClubId_g;
      int receiving = s_loanReceivingClubId_g;
      if (s_loanIsLoanIn_g) receiving = g_CareerHub.clubId;
      std::string direction = s_loanIsLoanIn_g ? "loan_in" : "loan_out";
      if (s_loanPlayerId_g > 0 && parent > 0 && receiving > 0 && parent != receiving) {
        InitiateLoanOffer(g_CareerHub.managerId, g_CareerHub.clubId, s_loanPlayerId_g,
                          parent, receiving, fee, wagePct, kLoanTime[s_loanPlayingTimeIdx_g],
                          1, optFee, s_loanEndDateStr_g, mandatoryFee,
                          kMandatoryModes[s_loanMandatoryModeIdx_g], mandatoryApps,
                          s_loanEndDateStr_g, direction,
                          g_CareerHub.seasonYear, g_CareerHub.currentDate);
        s_loanPlayerId_g = 0;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (SecBtn("Cancel", ImVec2(80.0f, 32.0f)))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (s_openLoanReviewPopup_g) {
    ImGui::OpenPopup("##loan_review_modal");
    s_openLoanReviewPopup_g = false;
  }
  ImGui::SetNextWindowSize(ImVec2(520.0f, 360.0f), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                                 ImGui::GetIO().DisplaySize.y * 0.5f),
                           ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 26, 44, 245));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
  if (ImGui::BeginPopupModal("##loan_review_modal", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float innerW = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p0, ImVec2(p0.x + innerW, p0.y + 58.0f), IM_COL32(14,22,46,230), 8.0f);
    PushMgrFont(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 10.0f));
    ImGui::TextUnformatted("Review Loan Offer");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontBold);
    PushMgrFont(g_ManagerFontSmall);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 31.0f));
    ImGui::TextColored(kTextSec, "%s from %s", s_loanReviewPlayerName_g, s_loanReviewClubName_g);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 76.0f));
    static const char *kMandatoryModesReview[] = {"none", "end_date", "appearances"};
    BeginModernCard("##loan_review_terms", ImVec2(innerW, 172.0f));
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("Loan fee", s_loanReviewFeeStr_g, sizeof(s_loanReviewFeeStr_g));
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("Borrower wage %", s_loanReviewWagePctStr_g, sizeof(s_loanReviewWagePctStr_g));
    ImGui::SetNextItemWidth(160.0f);
    ImGui::InputText("Option to buy fee", s_loanReviewOptionFeeStr_g, sizeof(s_loanReviewOptionFeeStr_g));
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::SetNextItemWidth(140.0f);
    ImGui::InputText("Mandatory fee", s_loanReviewMandatoryFeeStr_g, sizeof(s_loanReviewMandatoryFeeStr_g));
    ImGui::SetNextItemWidth(180.0f);
    ImGui::Combo("Mandatory trigger", &s_loanReviewMandatoryModeIdx_g, kMandatoryModesReview, 3);
    if (s_loanReviewMandatoryModeIdx_g == 2) {
      ImGui::SameLine(0.0f, 12.0f);
      ImGui::SetNextItemWidth(90.0f);
      ImGui::InputText("Apps", s_loanReviewMandatoryAppsStr_g, sizeof(s_loanReviewMandatoryAppsStr_g));
    }
    PopMgrFont(g_ManagerFontSmall);
    EndModernCard();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + innerW - 294.0f, p0.y + 288.0f));
    if (CTAButton("Counter", ImVec2(94.0f, 32.0f))) {
      RespondToLoanOffer(g_CareerHub.managerId, s_loanReviewDealId_g, "counter",
                         atoi(s_loanReviewFeeStr_g), atoi(s_loanReviewWagePctStr_g),
                         atoi(s_loanReviewOptionFeeStr_g),
                         s_loanReviewEndDateStr_g,
                         atoi(s_loanReviewMandatoryFeeStr_g),
                         kMandatoryModesReview[s_loanReviewMandatoryModeIdx_g],
                         atoi(s_loanReviewMandatoryAppsStr_g));
      s_loanReviewDealId_g = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (SecBtn("Reject", ImVec2(82.0f, 32.0f))) {
      RespondToLoanOffer(g_CareerHub.managerId, s_loanReviewDealId_g, "reject", 0, 0);
      s_loanReviewDealId_g = 0;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (SecBtn("Cancel", ImVec2(74.0f, 32.0f)))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (s_openListPlayerPopup_g) {
    ImGui::OpenPopup("##list_player_modal");
    s_openListPlayerPopup_g = false;
  }
  ImGui::SetNextWindowSize(ImVec2(460.0f, 240.0f), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                                 ImGui::GetIO().DisplaySize.y * 0.5f),
                           ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 26, 44, 245));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
  if (ImGui::BeginPopupModal("##list_player_modal", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float innerW = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p0, ImVec2(p0.x + innerW, p0.y + 58.0f), IM_COL32(14,22,46,230), 8.0f);
    PushMgrFont(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 10.0f));
    ImGui::TextUnformatted("List on Transfer Market");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontBold);
    PushMgrFont(g_ManagerFontSmall);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 31.0f));
    ImGui::TextColored(kTextSec, "%s", s_listPlayerName_g[0] ? s_listPlayerName_g : "Selected player");
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 76.0f));
    BeginModernCard("##list_player_terms", ImVec2(innerW, 78.0f));
    ImGui::TextColored(kTextDim, "Asking value");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputText("##list_asking_value", s_listAskingValueStr_g, sizeof(s_listAskingValueStr_g));
    PopMgrFont(g_ManagerFontSmall);
    EndModernCard();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + innerW - 204.0f, p0.y + 174.0f));
    if (CTAButton("Confirm", ImVec2(104.0f, 32.0f))) {
      long long askVal = atoll(s_listAskingValueStr_g);
      if (askVal > 0 && s_listPlayerId_g > 0) {
        std::stringstream uq;
        uq << "INSERT INTO player_market_status(manager_id,player_id,status,set_date,asking_price)"
           << " VALUES(" << g_CareerHub.managerId << "," << s_listPlayerId_g << ",'transfer_listed','"
           << g_CareerHub.currentDate << "'," << askVal << ")"
           << " ON CONFLICT(manager_id,player_id) DO UPDATE SET"
           << " status='transfer_listed',set_date=excluded.set_date,asking_price=excluded.asking_price;";
        delete GetDB()->Query(uq.str().c_str());
        s_listPlayerId_g = 0;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (SecBtn("Cancel", ImVec2(84.0f, 32.0f)))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();

  if (s_openReviewPopup_g) {
    ImGui::OpenPopup("##incoming_review_modal");
    s_openReviewPopup_g = false;
  }
  ImGui::SetNextWindowSize(ImVec2(500.0f, 282.0f), ImGuiCond_Always);
  ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                                 ImGui::GetIO().DisplaySize.y * 0.5f),
                           ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, IM_COL32(18, 26, 44, 245));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 16.0f));
  if (ImGui::BeginPopupModal("##incoming_review_modal", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove)) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    float innerW = ImGui::GetContentRegionAvail().x;
    dl->AddRectFilled(p0, ImVec2(p0.x + innerW, p0.y + 58.0f), IM_COL32(14,22,46,230), 8.0f);
    PushMgrFont(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 10.0f));
    ImGui::TextUnformatted("Review Offer");
    ImGui::PopStyleColor();
    PopMgrFont(g_ManagerFontBold);
    PushMgrFont(g_ManagerFontSmall);
    ImGui::SetCursorScreenPos(ImVec2(p0.x + 14.0f, p0.y + 31.0f));
    ImGui::TextColored(kTextSec, "%s", s_reviewPlayerName_g[0] ? s_reviewPlayerName_g : "Incoming offer");
    if (s_reviewBuyerName_g[0]) {
      ImGui::SameLine(0.0f, 8.0f);
      ImGui::TextColored(kTextDim, "from %s", s_reviewBuyerName_g);
    }
    PopMgrFont(g_ManagerFontSmall);
    ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + 72.0f));
    BeginModernCard("##review_terms", ImVec2(innerW, 96.0f));
    PushMgrFont(g_ManagerFontSmall);
    ImGui::TextColored(kTextDim, "Counter proposal");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(210.0f);
    ImGui::InputText("Counter fee", s_reviewCounterFeeStr_g, sizeof(s_reviewCounterFeeStr_g));
    ImGui::SameLine(0.0f, 12.0f);
    ImGui::TextColored(kTextDim, "Reject leaves a history entry and ends the negotiation.");
    PopMgrFont(g_ManagerFontSmall);
    EndModernCard();
    ImGui::SetCursorScreenPos(ImVec2(p0.x + innerW - 310.0f, p0.y + 205.0f));
    if (CTAButton("Send Counter", ImVec2(124.0f, 32.0f))) {
      int counterFee = atoi(s_reviewCounterFeeStr_g);
      if (counterFee > 0 && s_reviewNegotiationId_g > 0) {
        RespondToOffer(g_CareerHub.managerId, s_reviewNegotiationId_g, "counter", counterFee, 0);
        s_reviewNegotiationId_g = 0;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (SecBtn("Reject", ImVec2(82.0f, 32.0f))) {
      if (s_reviewNegotiationId_g > 0) {
        RespondToOffer(g_CareerHub.managerId, s_reviewNegotiationId_g, "reject", 0, 0);
        s_reviewNegotiationId_g = 0;
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine(0.0f, 8.0f);
    if (SecBtn("Cancel", ImVec2(74.0f, 32.0f)))
      ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
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
  fg->AddRect(cardPos, cardEnd, IM_COL32((int)(kAccent.x*255),(int)(kAccent.y*255),(int)(kAccent.z*255),180), kRounding, 0, 1.5f);

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

// ---- DrawEscMenu --------------------------------------------------------
// Full-screen pause menu triggered by ESC. Drawn as a floating ImGui window
// with a dimmed foreground backdrop so it sits above everything.

// (ESC menu is rendered as a BeginPopupModal inside ##mgr_root — see RenderImGuiCareerHub)

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

// ---- Search results dropdown overlay ------------------------------------
// Called from RenderImGuiCareerHub just before ImGui::End() so it renders on top.
static void DrawSearchDropdown() {
  if (!s_searchActive || s_searchResults.empty()) return;

  const float kDropW   = s_searchDropdownW;
  const float kCardH   = 58.0f;
  int  numR   = (int)s_searchResults.size();
  float totalH = numR * kCardH;

  ImGui::SetCursorScreenPos(s_searchDropdownPos);

  ImGui::PushStyleColor(ImGuiCol_ChildBg,  IM_COL32(10, 15, 30, 252));
  ImGui::PushStyleColor(ImGuiCol_Border,   IM_COL32(40, 58, 100, 200));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding,   8.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,   ImVec2(0, 0));
  ImGui::BeginChild("##srch_drop", ImVec2(kDropW, totalH), true,
    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoNav);

  ImDrawList *dl  = ImGui::GetWindowDrawList();
  ImVec2     wpos = ImGui::GetWindowPos();
  int ar = (int)(kAccent.x*255), ag2 = (int)(kAccent.y*255), ab2 = (int)(kAccent.z*255);

  // Result cards
  bool clicked = false;
  int  clickedId = -1;
  int  clickedIdx = -1;

  float ry = wpos.y;
  int maxVisible = numR;
  for (int i = 0; i < maxVisible; i++) {
    const SearchPlayerResult &sr = s_searchResults[i];
    ImU32 qCol = QualityColor(sr.baseStat);
    int qr = qCol & 0xFF, qg = (qCol>>8)&0xFF, qb = (qCol>>16)&0xFF;

    // Row background
    ImGui::SetCursorScreenPos(ImVec2(wpos.x, ry));
    char rowId[32]; snprintf(rowId, sizeof(rowId), "##sr_%d", i);
    ImGui::InvisibleButton(rowId, ImVec2(kDropW, kCardH));
    bool hov = ImGui::IsItemHovered();
    bool clk = ImGui::IsItemClicked();

    ImU32 rowBg = hov ? IM_COL32(ar, ag2, ab2, 22) : IM_COL32(8, 13, 28, 200);
    dl->AddRectFilled(ImVec2(wpos.x, ry), ImVec2(wpos.x+kDropW, ry+kCardH), rowBg);

    // Quality left stripe
    dl->AddRectFilled(ImVec2(wpos.x, ry+4), ImVec2(wpos.x+3, ry+kCardH-4),
                      IM_COL32(qr, qg, qb, hov?255:160), 2.0f);

    // Club badge (36x36), centered vertically in the card
    const float kBadgeSz = 36.0f;
    float badgeLeft = wpos.x + 12.0f;
    float badgeTop  = ry + (kCardH - kBadgeSz) * 0.5f;
    ImGui::SetCursorScreenPos(ImVec2(badgeLeft, badgeTop));
    DrawTeamBadge(sr.clubLogoPath, sr.clubShortName, kBadgeSz);

    // Name (use nickname if available)
    float textX = wpos.x + 58.0f; // badge(12+36=48) + 10px gap
    std::string fullName = sr.nickname.empty()
        ? (sr.firstName + " " + sr.lastName) : sr.nickname;
    PushMgrFont(g_ManagerFontSmall);
    dl->AddText(g_ManagerFontSmall, 18.0f,
                ImVec2(textX, ry + 8.0f),
                hov ? IM_COL32(ar,ag2,ab2,255) : IM_COL32(225, 232, 250, 245),
                fullName.c_str());

    // Club | Role | Age sub-line
    char subBuf[96];
    snprintf(subBuf, sizeof(subBuf), "%s  |  %s  |  Age %s",
             sr.clubName.empty() ? "-" : sr.clubName.c_str(),
             sr.role.empty()     ? "-" : sr.role.c_str(),
             sr.age.empty()      ? "-" : sr.age.c_str());
    dl->AddText(g_ManagerFontSmall, 15.0f,
                ImVec2(textX, ry + 31.0f),
                IM_COL32(108, 122, 160, 200), subBuf);
    PopMgrFont(g_ManagerFontSmall);

    // Quality rating badge (right side)
    int ovr = (int)(sr.baseStat * 100.0f);
    char ovrBuf[8]; snprintf(ovrBuf, sizeof(ovrBuf), "%d", ovr);
    float badgeX = wpos.x + kDropW - 44.0f;
    float badgeY = ry + (kCardH - 28.0f) * 0.5f;
    dl->AddRectFilled(ImVec2(badgeX, badgeY), ImVec2(badgeX+38, badgeY+28),
                      IM_COL32(qr/5, qg/5, qb/5+6, 200), 6.0f);
    dl->AddRect(ImVec2(badgeX, badgeY), ImVec2(badgeX+38, badgeY+28),
                IM_COL32(qr, qg, qb, 80), 6.0f, 0, 1.0f);
    PushMgrFont(g_ManagerFontSmall);
    ImVec2 oSz = g_ManagerFontSmall
        ? g_ManagerFontSmall->CalcTextSizeA(19.0f, FLT_MAX, 0, ovrBuf)
        : ImGui::CalcTextSize(ovrBuf);
    dl->AddText(g_ManagerFontSmall, 19.0f,
                ImVec2(badgeX + (38.0f-oSz.x)*0.5f, badgeY + (28.0f-19.0f)*0.5f),
                IM_COL32(qr, qg, qb, 235), ovrBuf);
    PopMgrFont(g_ManagerFontSmall);

    // Row separator
    if (i < maxVisible - 1)
      dl->AddLine(ImVec2(wpos.x+8, ry+kCardH-0.5f), ImVec2(wpos.x+kDropW-8, ry+kCardH-0.5f),
                  IM_COL32(30, 45, 85, 120), 0.5f);

    if (clk) { clicked = true; clickedId = sr.id; clickedIdx = i; }
    ry += kCardH;
  }


  ImGui::EndChild();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor(2);

  // Close when clicking outside the dropdown
  if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
    ImVec2 mp = ImGui::GetMousePos();
    ImVec2 p1 = ImVec2(s_searchDropdownPos.x + kDropW, s_searchDropdownPos.y + totalH);
    if (mp.x < s_searchDropdownPos.x || mp.x > p1.x ||
        mp.y < s_searchDropdownPos.y || mp.y > p1.y) {
      s_searchBuf[0]   = '\0';
      s_searchActive   = false;
      s_searchLastTerm = "";
      s_searchResults.clear();
    }
  }

  // Handle click — load full player data and navigate
  if (clicked && clickedIdx >= 0) {
    const SearchPlayerResult &sr = s_searchResults[clickedIdx];
    // If the player is already in your squad, treat as own player (no fog)
    bool isOwnSquad = false;
    for (const auto &p : g_CareerHub.players)
      if (p.id == sr.id) { isOwnSquad = true; break; }

    s_playerDetailId = sr.id;
    if (isOwnSquad) {
      s_detailOverrideActive = false;
      s_detailClubName = s_detailClubLogo = s_detailClubShortName = "";
    } else {
      CareerHubState::Player op;
      LoadPlayerFullDetail(sr.id, op);
      s_detailPlayerOverride = op;
      s_detailOverrideActive = true;
      s_detailClubName       = sr.clubName;
      s_detailClubLogo       = sr.clubLogoPath;
      s_detailClubShortName  = sr.clubShortName;
    }
    // Clear search
    s_searchBuf[0]   = '\0';
    s_searchActive   = false;
    s_searchLastTerm = "";
    s_searchResults.clear();
    NavPush(PAGE_PLAYER_DETAIL);
  }
}

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

  s_advanceClicked     = false;
  s_startSeasonClicked = false;
  s_menuClicked        = false;
  // ESC: open pause menu (only when not advancing and not already open)
  // Closing is handled by BeginPopupModal via &s_escMenuOpen (p_open).
  if (!g_CareerHub.isAdvancing && !s_escMenuOpen &&
      ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    s_escMenuOpen = true;
    s_escMenuJustOpened = true;
    ImGui::OpenPopup("##esc_modal");
  }
  if (g_CareerHub.isAdvancing) {
    s_escMenuOpen = false;
  }

  // Always draw the normal hub behind any overlay.
  DrawAppBackground(winW, winH);
  DrawManagerShell(winW, winH);

  // ---- Squad incomplete alert modal ---------------------------------------
  {
    int ar = (int)(kAccent.x*255), ag = (int)(kAccent.y*255), ab = (int)(kAccent.z*255);
    const float kPad = 28.0f;
    const float kCardW = 380.0f;
    ImGui::SetNextWindowPos(ImVec2(winW * 0.5f, winH * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(kCardW, 0), ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(kPad, kPad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,  6.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,        IM_COL32(12, 18, 38, 252));
    ImGui::PushStyleColor(ImGuiCol_Border,          IM_COL32(200, 70, 70, 200));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,IM_COL32(0, 0, 0, 165));
    if (ImGui::BeginPopupModal("##squad_alert", nullptr,
          ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
          ImGuiWindowFlags_NoMove     | ImGuiWindowFlags_NoScrollbar)) {
      int active = CountActiveSquad();
      float btnW = kCardW - kPad * 2.0f;

      // Warning icon + title
      PushMgrFont(g_ManagerFontBold);
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(240, 80, 80, 255));
      ImGui::TextUnformatted("\xe2\x9a\xa0  Incomplete Lineup");
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontBold);

      ImGui::Dummy(ImVec2(0, 8.0f));

      // Body message
      char msg[128];
      snprintf(msg, sizeof(msg),
               "Only %d players in lineup.\nYou need exactly 20 players\n(11 starters + 9 subs) to play a match.",
               active);
      PushMgrFont(g_ManagerFontSmall);
      ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(180, 190, 215, 240));
      ImGui::TextWrapped("%s", msg);
      ImGui::PopStyleColor();
      PopMgrFont(g_ManagerFontSmall);

      ImGui::Dummy(ImVec2(0, 14.0f));

      // Go to Squad button
      ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(ar, ag, ab, 180));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(ar, ag, ab, 230));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(ar, ag, ab, 255));
      ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(255, 255, 255, 255));
      PushMgrFont(g_ManagerFontRegular);
      if (ImGui::Button("Go to Squad", ImVec2(btnW, 38.0f))) {
        NavPush(PAGE_SQUAD);
        ImGui::CloseCurrentPopup();
      }
      PopMgrFont(g_ManagerFontRegular);
      ImGui::PopStyleColor(4);

      ImGui::Dummy(ImVec2(0, 6.0f));

      // Dismiss
      ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(30, 40, 70, 200));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(45, 58, 100, 230));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(60, 75, 120, 255));
      ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(160, 170, 200, 240));
      PushMgrFont(g_ManagerFontRegular);
      if (ImGui::Button("Dismiss", ImVec2(btnW, 32.0f)))
        ImGui::CloseCurrentPopup();
      PopMgrFont(g_ManagerFontRegular);
      ImGui::PopStyleColor(4);

      ImGui::EndPopup();
    }
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar(3);
  }

  // ---- ESC pause menu (BeginPopupModal blocks background clicks and dims) ----
  if (s_escMenuOpen) {
    int ar = (int)(kAccent.x*255), ag = (int)(kAccent.y*255), ab = (int)(kAccent.z*255);
    const float kBtnH = 40.0f, kBtnGap = 8.0f, kPad = 24.0f;
    const float kCardW = 300.0f;

    ImGui::SetNextWindowPos(ImVec2(winW * 0.5f, winH * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(kCardW, 0), ImGuiCond_Always); // 0 height = auto

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,  ImVec2(kPad, kPad));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 12.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,    ImVec2(0.0f, kBtnGap));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding,  6.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg,        IM_COL32(12, 18, 38, 252));
    ImGui::PushStyleColor(ImGuiCol_Border,          IM_COL32(ar, ag, ab, 160));
    ImGui::PushStyleColor(ImGuiCol_BorderShadow,    IM_COL32(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg,IM_COL32(0, 0, 0, 165));

    if (ImGui::BeginPopupModal("##esc_modal", nullptr,
          ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize  |
          ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoScrollbar)) {
      // Close on ESC — skip the frame it was opened (same ESC press would re-close it)
      if (!s_escMenuJustOpened && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        s_escMenuOpen = false;
        ImGui::CloseCurrentPopup();
      }
      s_escMenuJustOpened = false;
      float btnW = kCardW - kPad * 2.0f;

      // Main Screen — accent fill
      ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(ar, ag, ab, 180));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(ar, ag, ab, 230));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(ar, ag, ab, 255));
      ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(255, 255, 255, 255));
      PushMgrFont(g_ManagerFontRegular);
      if (ImGui::Button("Main Screen", ImVec2(btnW, kBtnH))) {
        s_menuClicked = true;
        s_escMenuOpen = false;
        ImGui::CloseCurrentPopup();
      }
      PopMgrFont(g_ManagerFontRegular);
      ImGui::PopStyleColor(4);

      // Settings — subtle tint
      ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(ar, ag, ab, 30));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(ar, ag, ab, 70));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(ar, ag, ab, 100));
      ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(200, 210, 230, 255));
      PushMgrFont(g_ManagerFontRegular);
      if (ImGui::Button("Settings", ImVec2(btnW, kBtnH))) {
        NavPush(PAGE_SETTINGS);
        s_escMenuOpen = false;
        ImGui::CloseCurrentPopup();
      }
      PopMgrFont(g_ManagerFontRegular);
      ImGui::PopStyleColor(4);

      // Quit — danger red
      ImGui::PushStyleColor(ImGuiCol_Button,        IM_COL32(140, 28, 28, 180));
      ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(180, 40, 40, 220));
      ImGui::PushStyleColor(ImGuiCol_ButtonActive,  IM_COL32(210, 55, 55, 255));
      ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(255, 255, 255, 255));
      PushMgrFont(g_ManagerFontRegular);
      if (ImGui::Button("Quit", ImVec2(btnW, kBtnH))) exit(0);
      PopMgrFont(g_ManagerFontRegular);
      ImGui::PopStyleColor(4);

      ImGui::EndPopup();
    } else {
      // Closed by ImGui (ESC key or click outside)
      s_escMenuOpen = false;
    }

    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(4);
  }

  // Search dropdown — rendered last so it draws on top of all child windows.
  if (s_searchActive && !s_searchResults.empty())
    DrawSearchDropdown();

  ImGui::End();

  // Advancing overlay sits above everything via foreground drawlist.
  if (g_CareerHub.isAdvancing) {
    DrawAdvancingModalOverlay();
  }

  // ---- Deferred action dispatch ----------------------------------------
  // Immediate actions (Main Menu, Play Fixture) fire right away.
  // Advance/Start Season set isAdvancing=true and defer work by one frame so
  // the overlay is guaranteed to appear before processing begins.

  if (g_CareerHub.pendingAction == 0) {
    if (s_menuClicked) {
      g_CareerHub.pendingAction = 2;
      printf("[IMGUI MANAGER] Main Menu requested\n");

    } else if (s_startSeasonClicked && !g_CareerHub.isAdvancing) {
      g_CareerHub.isAdvancing          = true;
      g_CareerHub.pendingAdvanceAction = ADVANCE_START_SEASON;
      g_CareerHub.advanceFramesWaited  = 0;
      printf("[IMGUI MANAGER] Start Season: overlay shown, work deferred one frame\n");

    } else if (s_advanceClicked && !g_CareerHub.isAdvancing) {
      if (g_CareerHub.hasTodayFixture) {
        // Block if active squad not full
        if (CountActiveSquad() < 20) {
          ImGui::OpenPopup("##squad_alert");
          printf("[IMGUI MANAGER] Match blocked — active squad incomplete (%d/20)\n",
                 CountActiveSquad());
        } else {
          g_CareerHub.pendingAction = 4; // Play Fixture — no overlay needed
          printf("[IMGUI MANAGER] Play Fixture requested fixture=%d\n",
                 g_CareerHub.todayFixture.id);
        }
      } else {
        g_CareerHub.isAdvancing          = true;
        g_CareerHub.pendingAdvanceAction = (g_CareerHub.advanceMode == ADVANCE_MODE_NEXT_MATCH)
                                             ? ADVANCE_UNTIL_MATCH
                                             : ADVANCE_NEXT_DAY;
        g_CareerHub.advanceFramesWaited  = 0;
        printf("[IMGUI MANAGER] Advance: mode=%d overlay shown, work deferred one frame\n",
               (int)g_CareerHub.pendingAdvanceAction);
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
        } else if (g_CareerHub.pendingAdvanceAction == ADVANCE_UNTIL_MATCH) {
          g_CareerHub.pendingAction = 6;
          printf("[IMGUI MANAGER] Firing AdvanceUntilNextMatchDay after overlay frame\n");
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


  static bool s_speedClickConsumed = false;
  s_speedClickConsumed = false;

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
