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

void SeedTransferSystem(int managerId) {}

void ProcessDailyTransfers(int managerId, int userClubId,
                            const std::string &currentDate, int seasonYear) {}
