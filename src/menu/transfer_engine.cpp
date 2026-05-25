#include "transfer_engine.hpp"
#include "imgui_career.hpp"
#include "managercareer.hpp"
#include "imgui_menu.hpp"
#include "base/utils.hpp"
#include <sstream>
#include <string>
#include <vector>
#include <map>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <algorithm>

// ---- Local DB helpers -------------------------------------------------------

static std::string TECell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size() || col >= r->data.at(row).size()) return "";
  return r->data.at(row).at(col);
}

static std::string TESql(const std::string &in) {
  std::string out;
  for (char c : in) { if (c == '\'') out += "''"; else out += c; }
  return out;
}

// Seeded pseudo-random float 0.0–1.0. index offsets within same seed.
static float TERand(unsigned int seed, int index) {
  unsigned int s = seed ^ (unsigned int)(index * 2654435761u);
  s = s ^ (s >> 16); s *= 0x45d9f3b; s = s ^ (s >> 16);
  return (float)(s & 0xFFFF) / 65535.0f;
}

// Returns integer in [lo, hi] from seed+index.
static int TERandInt(unsigned int seed, int index, int lo, int hi) {
  float f = TERand(seed, index);
  return lo + (int)(f * (float)(hi - lo + 1));
}

// Date helpers
static int DateToJulian(const std::string &d) {
  // Returns approximate days since epoch for ordering. Format: YYYY-MM-DD.
  if (d.size() < 10) return 0;
  int y = atoi(d.substr(0,4).c_str());
  int m = atoi(d.substr(5,2).c_str());
  int dy = atoi(d.substr(8,2).c_str());
  return y*365 + m*30 + dy;
}

static std::string AddDays(const std::string &date, int n) {
  // Simple add — delegates to SQLite for correctness.
  std::stringstream q;
  q << "SELECT date('" << date << "', '+" << n << " days');";
  DatabaseResult *r = GetDB()->Query(q.str());
  std::string out = (r && r->data.size() > 0) ? TECell(r, 0, 0) : date;
  delete r;
  return out;
}

static bool InTransferWindow(const std::string &date) {
  if (date.size() < 10) return false;
  int m  = atoi(date.substr(5,2).c_str());
  // Summer: Jul 1 – Aug 31
  if (m == 7 || m == 8) return true;
  // Winter: Jan 1 – Jan 31
  if (m == 1) return true;
  return false;
}

// Days remaining until window end (0 if not in window or on last day).
static int DaysToWindowEnd(const std::string &date) {
  if (!InTransferWindow(date)) return 0;
  int m  = atoi(date.substr(5,2).c_str());
  int dy = atoi(date.substr(8,2).c_str());
  if (m == 7) { return (31 - dy) + 31; } // days left in Jul + all Aug
  if (m == 8) { return 31 - dy; }
  if (m == 1) { return 31 - dy; }
  return 0;
}

static void InsertTransferNews(int managerId, const std::string &date,
                                const std::string &headline,
                                const std::string &category,
                                int playerId, int fromClub, int toClub) {
  std::stringstream q;
  q << "INSERT INTO transfer_news(manager_id,game_date,headline,category,"
    << "player_id,from_club_id,to_club_id) VALUES("
    << managerId << ",'" << date << "','" << TESql(headline) << "','"
    << category << "'," << playerId << "," << fromClub << "," << toClub << ");";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
}

static void AddUnhappiness(int managerId, int playerId, const std::string &reason,
                            int severity, const std::string &date) {
  std::stringstream q;
  q << "SELECT severity FROM player_unhappiness WHERE manager_id=" << managerId
    << " AND player_id=" << playerId << " AND reason='" << reason
    << "' AND resolved=0;";
  DatabaseResult *r = GetDB()->Query(q.str());
  bool exists = (r && r->data.size() > 0);
  delete r;

  if (exists) {
    std::stringstream uq;
    uq << "UPDATE player_unhappiness SET severity=MIN(100,severity+" << severity
       << ") WHERE manager_id=" << managerId << " AND player_id=" << playerId
       << " AND reason='" << reason << "' AND resolved=0;";
    DatabaseResult *ur = GetDB()->Query(uq.str());
    delete ur;
  } else {
    std::stringstream iq;
    iq << "INSERT INTO player_unhappiness(manager_id,player_id,reason,severity,created_date,resolved)"
       << " VALUES(" << managerId << "," << playerId << ",'" << reason << "',"
       << severity << ",'" << date << "',0);";
    DatabaseResult *ir = GetDB()->Query(iq.str());
    delete ir;
  }
}

// ---- Function stubs (implemented in later tasks) ----------------------------

std::map<std::string, int> EvaluateSquadNeeds(int managerId, int clubId,
                                               int seasonYear) {
  return {};
}

void TriggerTransferCascade(int managerId, int sellingClubId,
                             int affectedClubId, const std::string &currentDate,
                             int seasonYear, int depth) {}

void ProcessContractRenewals(int managerId, const std::string &currentDate) {}

static void SeedPlayerTraits(int managerId, unsigned int careerSeed) {
  DatabaseResult *pr = GetDB()->Query(
    "SELECT id, age, base_stat, sofifaPotential, international_reputation,"
    " weekly_wage, playervalue, contract_expiry"
    " FROM players;");
  if (!pr) return;

  for (unsigned int i = 0; i < pr->data.size(); i++) {
    int   pid      = atoi(TECell(pr, i, 0).c_str());
    int   age      = atoi(TECell(pr, i, 1).c_str());
    float bstat    = atof(TECell(pr, i, 2).c_str());
    int   pot      = atoi(TECell(pr, i, 3).c_str());
    int   intlRep  = atoi(TECell(pr, i, 4).c_str()); // 1-5
    int   wage     = atoi(TECell(pr, i, 5).c_str());
    int   pval     = atoi(TECell(pr, i, 6).c_str());
    std::string expiry = TECell(pr, i, 7);

    unsigned int seed = careerSeed ^ (unsigned int)(pid * 7919u);
    auto clamp = [](int v){ return std::max(0, std::min(100, v)); };

    // ambition: potential gap + young age bonus
    int potGap = std::max(0, pot - (int)bstat);
    int ambition = clamp((int)(potGap * 2.5f)
                         + (age < 21 ? 25 : age < 24 ? 10 : 0)
                         + TERandInt(seed, 0, -10, 10));

    // loyalty: older + long contract + low rep = loyal
    int contractYears = expiry.empty() ? 1 :
      std::max(0, atoi(expiry.substr(0,4).c_str()) - 2026);
    int loyalty = clamp(30 + contractYears * 12 + (age > 30 ? 15 : 0)
                        - intlRep * 8
                        + TERandInt(seed, 1, -10, 10));

    // greed: wage + value + rep
    int greed = clamp((int)(std::min(wage / 2000, 40))
                      + intlRep * 10
                      + (pval > 20000000 ? 15 : pval > 5000000 ? 8 : 0)
                      + TERandInt(seed, 2, -8, 8));

    // ego: international_reputation is the dominant driver (1-5 -> 0-80)
    int ego = clamp((intlRep - 1) * 20
                    + (pval > 30000000 ? 10 : 0)
                    + TERandInt(seed, 3, -8, 8));

    // trophy_hunger: age 27-34 + high rep
    int trophy = clamp((age > 27 && age < 35 ? 30 : age > 34 ? 10 : 0)
                       + intlRep * 8
                       + TERandInt(seed, 4, -10, 10));

    // adaptability: young + low rep (big rep = settled)
    int adapt = clamp(50 - intlRep * 6
                      + (age < 26 ? 20 : 0)
                      + TERandInt(seed, 5, -10, 10));

    // professionalism: mature + consistent stats
    int prof = clamp(30 + (age > 25 ? 20 : 0)
                     + (int)(bstat / 2.0f)
                     - (potGap > 20 ? 10 : 0)
                     + TERandInt(seed, 6, -8, 8));

    std::stringstream iq;
    iq << "INSERT OR IGNORE INTO player_traits"
       << "(manager_id,player_id,ambition,loyalty,greed,ego,trophy_hunger,adaptability,professionalism)"
       << " VALUES(" << managerId << "," << pid << ","
       << ambition << "," << loyalty << "," << greed << ","
       << ego << "," << trophy << "," << adapt << "," << prof << ");";
    DatabaseResult *ir = GetDB()->Query(iq.str());
    delete ir;
  }

  printf("[TRANSFER] SeedPlayerTraits: manager=%d players=%u\n",
         managerId, (unsigned int)pr->data.size());
  delete pr;
}

static void SeedClubTransferIdentity(int managerId, unsigned int careerSeed) {
  DatabaseResult *cr = GetDB()->Query(
    "SELECT id, international_prestige, domestic_prestige,"
    " transfer_budget FROM teams WHERE transfer_budget > 0;");
  if (!cr) return;

  // Max budget for normalisation
  long long maxBud = 1;
  for (unsigned int i = 0; i < cr->data.size(); i++) {
    long long b = atoll(TECell(cr, i, 3).c_str());
    if (b > maxBud) maxBud = b;
  }

  static const char *kPersonalities[] = {
    "patient","fast_closer","hard_negotiator","media_manipulator","desperate","patient"
  };

  for (unsigned int i = 0; i < cr->data.size(); i++) {
    int   clubId   = atoi(TECell(cr, i, 0).c_str());
    int   intlPres = atoi(TECell(cr, i, 1).c_str());
    int   domPres  = atoi(TECell(cr, i, 2).c_str());
    long long tbud = atoll(TECell(cr, i, 3).c_str());

    unsigned int seed = careerSeed ^ (unsigned int)(clubId * 31337u);
    float pf = (intlPres * 0.6f + domPres * 0.4f) / 10.0f;     // 0-1
    float bf = (float)tbud / (float)maxBud;                      // 0-1
    float cf = bf * 0.7f + pf * 0.3f;                           // combined factor
    auto clamp = [](int v){ return std::max(0, std::min(100, v)); };

    int aggression  = clamp((int)(cf * 70) + TERandInt(seed, 0, -15, 15));
    int wage_will   = clamp((int)(bf * 60) + TERandInt(seed, 1, -15, 20));
    int age_pref    = TERandInt(seed, 2, -50, 50); // signed, stored as-is
    int panic       = clamp(80 - (int)(pf * 50) + TERandInt(seed, 3, -10, 10));
    int loyalty     = clamp((int)(pf * 60) + TERandInt(seed, 4, -15, 15));
    int risk_tol    = clamp((int)(bf * 55) + TERandInt(seed, 5, -15, 15));
    int sell_press  = clamp(10 + TERandInt(seed, 6, 0, 20)); // starts low
    int youth       = clamp(30 + TERandInt(seed, 7, -20, 30));
    int domestic    = clamp(30 + TERandInt(seed, 8, -15, 25));
    int resale      = clamp(25 + TERandInt(seed, 9, -15, 25));
    int prestige    = clamp((int)(pf * 70) + TERandInt(seed, 10, -10, 20));
    int irrat       = clamp(15 + TERandInt(seed, 11, 0, 35));

    int pIdx        = ((unsigned int)(seed >> 8)) % 6;
    const char *personality = kPersonalities[pIdx];

    std::stringstream iq;
    iq << "INSERT OR IGNORE INTO club_transfer_identity"
       << "(manager_id,club_id,aggression,wage_willingness,age_preference,"
       << "deadline_panic,loyalty_to_players,financial_risk_tolerance,selling_pressure,"
       << "youth_focus,domestic_bias,resale_focus,prestige_bias,irrationality,"
       << "negotiation_personality) VALUES("
       << managerId << "," << clubId << ","
       << aggression << "," << wage_will << "," << age_pref << ","
       << panic << "," << loyalty << "," << risk_tol << "," << sell_press << ","
       << youth << "," << domestic << "," << resale << "," << prestige << ","
       << irrat << ",'" << personality << "');";
    DatabaseResult *ir = GetDB()->Query(iq.str());
    delete ir;
  }
  delete cr;
  printf("[TRANSFER] SeedClubTransferIdentity: manager=%d\n", managerId);
}

void SeedTransferSystem(int managerId) {}

void ProcessDailyTransfers(int managerId, int userClubId,
                            const std::string &currentDate, int seasonYear) {}
