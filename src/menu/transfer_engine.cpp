#include "transfer_engine.hpp"
#include "user_transfer.hpp"
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
int DateToJulian(const std::string &d) {
  // Returns approximate days since epoch for ordering. Format: YYYY-MM-DD.
  if (d.size() < 10) return 0;
  int y = atoi(d.substr(0,4).c_str());
  int m = atoi(d.substr(5,2).c_str());
  int dy = atoi(d.substr(8,2).c_str());
  return y*365 + m*30 + dy;
}

std::string AddDays(const std::string &date, int n) {
  // Simple add — delegates to SQLite for correctness.
  std::stringstream q;
  q << "SELECT date('" << date << "', '+" << n << " days');";
  DatabaseResult *r = GetDB()->Query(q.str());
  std::string out = (r && r->data.size() > 0) ? TECell(r, 0, 0) : date;
  delete r;
  return out;
}

bool InTransferWindow(const std::string &date) {
  if (date.size() < 10) return false;
  int m  = atoi(date.substr(5,2).c_str());
  int dy = atoi(date.substr(8,2).c_str());
  // Summer: Jul 1 through Sep 1 deadline day
  if (m == 7 || m == 8) return true;
  // Deadline day spillover: Sep 1
  if (m == 9 && dy == 1) return true;
  // Winter: Jan 1 – Jan 31
  if (m == 1) return true;
  return false;
}

// Days remaining until window end (0 if not in window or on last day).
int DaysToWindowEnd(const std::string &date) {
  if (!InTransferWindow(date)) return 0;
  int m  = atoi(date.substr(5,2).c_str());
  int dy = atoi(date.substr(8,2).c_str());
  if (m == 7) { return (31 - dy) + 31; } // days left in Jul + all Aug
  if (m == 8) { return 31 - dy; }
  if (m == 9 && dy == 1) { return 0; }
  if (m == 1) { return 31 - dy; }
  return 0;
}

void InsertTransferNews(int managerId, const std::string &date,
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

void InsertInboxMessage(int managerId, const std::string &subject,
                               const std::string &body, const std::string &category,
                               const std::string &date) {
  std::stringstream q;
  q << "INSERT INTO manager_inbox"
    << "(manager_id,sender_type,sender_name,subject,body,category,game_date)"
    << " VALUES(" << managerId << ",'transfers','Transfer Desk',"
    << "'" << TESql(subject) << "','" << TESql(body) << "','"
    << TESql(category) << "','" << TESql(date) << "');";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
}

void InsertFinanceTransaction(int managerId, int clubId, const std::string &date,
                              const std::string &category,
                              const std::string &description, long long amount) {
  DatabaseResult *exists = GetDB()->Query(
    "SELECT name FROM sqlite_master WHERE type='table' AND name='finance_transactions';");
  bool hasTable = exists && exists->data.size() > 0;
  delete exists;
  if (!hasTable) return;

  std::stringstream q;
  q << "INSERT INTO finance_transactions(manager_id,club_id,date,category,description,amount)"
    << " VALUES(" << managerId << "," << clubId << ",'" << TESql(date) << "','"
    << TESql(category) << "','" << TESql(description) << "'," << amount << ");";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
}

void SetPlayerSaveState(int managerId, int playerId, int teamId,
                        long long weeklyWage, const std::string &contractExpiry) {
  std::stringstream q;
  q << "INSERT INTO player_save_state(manager_id,player_id,team_id,weekly_wage,contract_expiry)"
    << " VALUES(" << managerId << "," << playerId << "," << teamId << ","
    << weeklyWage << ",";
  if (contractExpiry.empty()) q << "NULL";
  else q << "'" << TESql(contractExpiry) << "'";
  q << ") ON CONFLICT(manager_id,player_id) DO UPDATE SET"
    << " team_id=excluded.team_id,"
    << " weekly_wage=excluded.weekly_wage,"
    << " contract_expiry=COALESCE(excluded.contract_expiry, player_save_state.contract_expiry);";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
}

void RemovePlayerScoutingRecords(int managerId, int playerId) {
  std::stringstream q;
  q << "DELETE FROM scout_queue WHERE manager_id=" << managerId
    << " AND player_id=" << playerId << ";";
  DatabaseResult *qr = GetDB()->Query(q.str());
  delete qr;

  q.str("");
  q << "DELETE FROM scout_reports WHERE manager_id=" << managerId
    << " AND player_id=" << playerId << ";";
  DatabaseResult *rr = GetDB()->Query(q.str());
  delete rr;
}

void AddUnhappiness(int managerId, int playerId, const std::string &reason,
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

// Maps player role strings to one of the 7 position groups.
std::string RoleToGroup(const std::string &role) {
  if (role == "GK") return "GK";
  if (role == "CB") return "CB";
  if (role == "LB" || role == "RB" || role == "LWB" || role == "RWB") return "FB_WB";
  if (role == "CDM") return "DM";
  if (role == "CM") return "CM";
  if (role == "CAM" || role == "LM" || role == "RM" || role == "LW" || role == "RW") return "AM_W";
  if (role == "CF" || role == "ST") return "ST";
  return "CM"; // fallback
}

std::map<std::string, int> EvaluateSquadNeeds(int managerId, int clubId,
                                               int seasonYear) {
  static const char *kGroups[] = {"GK","CB","FB_WB","DM","CM","AM_W","ST"};
  std::map<std::string, int> scores;
  for (const char *g : kGroups) scores[g] = 0;

  // Load club's players
  std::stringstream pq;
  pq << "SELECT p.role, p.base_stat, p.age, p.contract_expiry"
     << " FROM players p WHERE p.team_id=" << clubId << ";";
  DatabaseResult *pr = GetDB()->Query(pq.str());

  struct GroupData { int count=0; float statSum=0; int ageSum=0; int expiryCount=0; };
  std::map<std::string, GroupData> gd;

  std::string today;
  { DatabaseResult *dr = GetDB()->Query("SELECT date('now');");
    if (dr && dr->data.size()>0) today = TECell(dr,0,0); delete dr; }
  std::string sixMonths = AddDays(today, 180);

  if (pr) {
    for (unsigned int i = 0; i < pr->data.size(); i++) {
      std::string grp = RoleToGroup(TECell(pr,i,0));
      float stat = atof(TECell(pr,i,1).c_str());
      int age    = atoi(TECell(pr,i,2).c_str());
      std::string exp = TECell(pr,i,3);
      gd[grp].count++;
      gd[grp].statSum += stat;
      gd[grp].ageSum  += age;
      if (!exp.empty() && exp <= sixMonths) gd[grp].expiryCount++;
    }
    delete pr;
  }

  // League average base_stat per group
  std::map<std::string, float> leagueAvg;
  {
    std::stringstream lq;
    lq << "SELECT p.role, AVG(p.base_stat)"
       << " FROM players p JOIN teams t ON t.id=p.team_id"
       << " JOIN teams tc ON tc.id=" << clubId
       << " WHERE t.league_id=tc.league_id GROUP BY p.role;";
    DatabaseResult *lr = GetDB()->Query(lq.str());
    if (lr) {
      for (unsigned int i=0; i < lr->data.size(); i++) {
        std::string grp = RoleToGroup(TECell(lr,i,0));
        float avg = atof(TECell(lr,i,1).c_str());
        if (leagueAvg.find(grp)==leagueAvg.end()) leagueAvg[grp] = avg;
        else leagueAvg[grp] = (leagueAvg[grp] + avg) / 2.0f;
      }
      delete lr;
    }
  }

  // Club identity for age_preference
  int agePref = 0;
  {
    std::stringstream cq;
    cq << "SELECT age_preference FROM club_transfer_identity"
       << " WHERE manager_id=" << managerId << " AND club_id=" << clubId << ";";
    DatabaseResult *cr = GetDB()->Query(cq.str());
    if (cr && cr->data.size()>0) agePref = atoi(TECell(cr,0,0).c_str());
    delete cr;
  }

  for (const char *gName : kGroups) {
    std::string g(gName);
    auto &d = gd[g];
    float lAvg = (leagueAvg.count(g)) ? leagueAvg[g] : 65.0f;
    float myAvg = d.count > 0 ? d.statSum / d.count : 0.0f;
    float myAge = d.count > 0 ? (float)d.ageSum / d.count : 25.0f;

    // quality_gap: 0-1 normalised
    float qgap = std::max(0.0f, (lAvg - myAvg) / lAvg);
    // depth_gap: 1.0 if 0 players, 0.5 if 1 player, 0 if 2+
    float dgap = d.count == 0 ? 1.0f : d.count == 1 ? 0.5f : 0.0f;
    // age_problem: avg age > 30
    float agap = myAge > 30.0f ? std::min(1.0f, (myAge - 30.0f) / 5.0f) : 0.0f;
    // expiry_pressure: 2+ players expiring
    float epres = d.expiryCount >= 2 ? 1.0f : d.expiryCount == 1 ? 0.5f : 0.0f;
    // identity_fit: desired age vs actual
    float desiredAge = 25.0f + agePref * 0.1f; // agePref -50..+50
    float ifit = std::min(1.0f, std::abs(myAge - desiredAge) / 10.0f);

    int score = (int)(qgap*30 + dgap*25 + agap*20 + epres*15 + ifit*10);
    scores[g] = std::max(0, std::min(100, score));
  }
  return scores;
}

void TriggerTransferCascade(int managerId, int sellingClubId,
                             int affectedClubId, const std::string &currentDate,
                             int seasonYear, int depth) {
  if (depth > 3) return; // cap at depth 3

  if (sellingClubId > 0) {
    auto needs = EvaluateSquadNeeds(managerId, sellingClubId, seasonYear);
    for (auto &kv : needs) {
      if (kv.second > 50) {
        printf("[TRANSFER CASCADE] club=%d needs %s (score=%d depth=%d)\n",
               sellingClubId, kv.first.c_str(), kv.second, depth);
        break;
      }
    }
  }

  if (affectedClubId > 0) {
    auto needs = EvaluateSquadNeeds(managerId, affectedClubId, seasonYear);
    (void)needs;
  }
}

void ProcessContractRenewals(int managerId, const std::string &currentDate) {
  // Only run on 1st of month
  if (currentDate.size() < 10 || currentDate.substr(8,2) != "01") return;

  std::stringstream q;
  q << "SELECT p.id, COALESCE(pss.weekly_wage,p.weekly_wage),"
    << " COALESCE(pss.contract_expiry,p.contract_expiry), p.age,"
    << " pt.loyalty, pt.greed, pt.ambition,"
    << " cti.loyalty_to_players, cti.resale_focus,"
    << " COALESCE(pss.team_id,p.team_id)"
    << " FROM players p"
    << " LEFT JOIN player_save_state pss ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
    << " JOIN player_traits pt ON pt.player_id=p.id AND pt.manager_id=" << managerId
    << " JOIN club_transfer_identity cti ON cti.club_id=COALESCE(pss.team_id,p.team_id) AND cti.manager_id=" << managerId
    << " WHERE COALESCE(pss.contract_expiry,p.contract_expiry) IS NOT NULL"
    << " AND COALESCE(pss.contract_expiry,p.contract_expiry) != ''"
    << " AND COALESCE(pss.contract_expiry,p.contract_expiry) <= date('" << currentDate << "', '+12 months');";
  DatabaseResult *r = GetDB()->Query(q.str());
  if (!r) return;

  for (unsigned int i=0; i < r->data.size(); i++) {
    int   pid        = atoi(TECell(r,i,0).c_str());
    long long wage   = atoll(TECell(r,i,1).c_str());
    std::string exp  = TECell(r,i,2);
    int   age        = atoi(TECell(r,i,3).c_str());
    int   loyalty    = atoi(TECell(r,i,4).c_str());
    int   greed      = atoi(TECell(r,i,5).c_str());
    int   ambition   = atoi(TECell(r,i,6).c_str());
    int   clubLoyalty = atoi(TECell(r,i,7).c_str());
    int   resale     = atoi(TECell(r,i,8).c_str());
    int   clubId     = atoi(TECell(r,i,9).c_str());

    bool within6Mo = (!exp.empty() && exp <= AddDays(currentDate, 180));

    if (clubLoyalty > 50 && loyalty > 40) {
      float mult = 1.05f + greed / 500.0f;
      long long newWage = (long long)(wage * mult);
      std::string newExpiry = AddDays(currentDate, 365*3);
      SetPlayerSaveState(managerId, pid, clubId, newWage, newExpiry);
      std::stringstream sq;
      sq << "DELETE FROM player_market_status WHERE manager_id=" << managerId
         << " AND player_id=" << pid << " AND status='expiring_soon';";
      DatabaseResult *sr = GetDB()->Query(sq.str()); delete sr;
      continue;
    }

    if (ambition > 70 && within6Mo) {
      AddUnhappiness(managerId, pid, "contract_stall", 10, currentDate);
      continue;
    }

    if (resale > 60 && within6Mo) {
      std::stringstream lq;
      lq << "INSERT OR REPLACE INTO player_market_status(manager_id,player_id,status,set_date,asking_price)"
         << " VALUES(" << managerId << "," << pid << ",'transfer_listed','"
         << currentDate << "',0);";
      DatabaseResult *lr = GetDB()->Query(lq.str()); delete lr;
      InsertTransferNews(managerId, currentDate, "Player made available for transfer",
                         "player_listed", pid, clubId, 0);
    }

    if (age > 32 && clubLoyalty < 40 && within6Mo) {
      AddUnhappiness(managerId, pid, "contract_stall", 5, currentDate);
    }
  }
  delete r;
}

static void SeedPlayerTraits(int managerId, unsigned int careerSeed) {
  (void)careerSeed;
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

    unsigned int seed = (unsigned int)(pid * 7919u);
    auto clamp = [](int v){ return std::max(0, std::min(100, v)); };

    // ambition: potential gap + young age bonus (bstat is 0.0-1.0, pot is 0-100)
    int potGap = std::max(0, pot - (int)(bstat * 100));
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

static void SeedClubPlayerKnowledge(int managerId, unsigned int careerSeed) {
  // Load all clubs with their league_id
  DatabaseResult *cr = GetDB()->Query(
    "SELECT id, league_id FROM teams WHERE transfer_budget > 0;");
  if (!cr) return;

  // Load all players with their team's league_id and international_reputation
  DatabaseResult *pr = GetDB()->Query(
    "SELECT p.id, p.international_reputation, t.league_id"
    " FROM players p JOIN teams t ON t.id = p.team_id;");
  if (!pr) { delete cr; return; }

  for (unsigned int ci = 0; ci < cr->data.size(); ci++) {
    int clubId      = atoi(TECell(cr, ci, 0).c_str());
    int clubLeague  = atoi(TECell(cr, ci, 1).c_str());

    for (unsigned int pi = 0; pi < pr->data.size(); pi++) {
      int pid        = atoi(TECell(pr, pi, 0).c_str());
      int intlRep    = atoi(TECell(pr, pi, 1).c_str()); // 1-5
      int pLeague    = atoi(TECell(pr, pi, 2).c_str());

      unsigned int seed = careerSeed ^ (unsigned int)(clubId * 7919u ^ pid * 31337u);
      int k = TERandInt(seed, 0, 0, 14); // base noise 0-14

      if (pLeague == clubLeague)  k += 60; // same league
      if (intlRep >= 5)           k += 70; // global superstar
      else if (intlRep >= 4)      k += 40; // globally known
      else if (intlRep >= 3)      k += 15;

      k = std::min(k, 100);
      if (k < 10) continue; // don't store effectively-zero knowledge

      std::stringstream iq;
      iq << "INSERT OR IGNORE INTO club_player_knowledge"
         << "(manager_id,club_id,player_id,knowledge)"
         << " VALUES(" << managerId << "," << clubId << "," << pid << "," << k << ");";
      DatabaseResult *ir = GetDB()->Query(iq.str());
      delete ir;
    }
  }
  delete cr;
  delete pr;
  printf("[TRANSFER] SeedClubPlayerKnowledge: manager=%d\n", managerId);
}

static void SeedPlayerMarketStatus(int managerId, unsigned int careerSeed,
                                    const std::string &currentDate) {
  DatabaseResult *pr = GetDB()->Query(
    "SELECT p.id, p.age, p.sofifaPotential, p.international_reputation,"
    " p.contract_expiry, p.base_stat,"
    " t.international_prestige, t.domestic_prestige"
    " FROM players p JOIN teams t ON t.id = p.team_id;");
  if (!pr) return;

  for (unsigned int i = 0; i < pr->data.size(); i++) {
    int   pid      = atoi(TECell(pr, i, 0).c_str());
    int   age      = atoi(TECell(pr, i, 1).c_str());
    int   pot      = atoi(TECell(pr, i, 2).c_str());
    int   intlRep  = atoi(TECell(pr, i, 3).c_str());
    std::string expiry = TECell(pr, i, 4);
    float bstat    = atof(TECell(pr, i, 5).c_str());

    std::string status = "normal";

    // wonderkid: age <= 21, potential >= 85, rep >= 3
    if (age <= 21 && pot >= 85 && intlRep >= 3) { status = "wonderkid"; }
    // franchise_player: team's identity marker (high prestige + high stat + old enough)
    else if (intlRep >= 4 && bstat >= 0.80f && age >= 24) { status = "franchise_player"; }
    // expiring_soon: contract within 6 months
    else if (!expiry.empty() && expiry <= AddDays(currentDate, 180)) {
      status = "expiring_soon";
    }

    if (status == "normal") continue; // don't store boring rows

    std::stringstream iq;
    iq << "INSERT OR REPLACE INTO player_market_status(manager_id,player_id,status,set_date,asking_price)"
       << " VALUES(" << managerId << "," << pid << ",'" << status << "','" << currentDate << "',0);";
    DatabaseResult *ir = GetDB()->Query(iq.str());
    delete ir;
  }
  delete pr;
  printf("[TRANSFER] SeedPlayerMarketStatus: manager=%d\n", managerId);
}

void SeedTransferSystem(int managerId) {
  // Derive career seed from manager creation timestamp
  unsigned int careerSeed = 12345u;
  {
    std::stringstream q;
    q << "SELECT strftime('%s', created_at) FROM managers WHERE id=" << managerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size() > 0 && !TECell(r, 0, 0).empty())
      careerSeed = (unsigned int)atoll(TECell(r, 0, 0).c_str());
    delete r;
  }
  // Get current date for status seeding
  std::string currentDate;
  {
    std::stringstream q;
    q << "SELECT current_date FROM managers WHERE id=" << managerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size() > 0) currentDate = TECell(r, 0, 0);
    delete r;
  }

  SeedPlayerTraits(managerId, careerSeed);
  SeedClubTransferIdentity(managerId, careerSeed);
  SeedClubPlayerKnowledge(managerId, careerSeed);
  SeedPlayerMarketStatus(managerId, careerSeed, currentDate);

  printf("[TRANSFER] SeedTransferSystem complete: manager=%d\n", managerId);
}

long long CalculateContextualValue(int managerId, int playerId,
                                           int sellingClubId, int buyingClubId,
                                           int needScore, int deadlinePressure,
                                           int agentPressure) {
  long long base = 0;
  float contractFactor = 1.0f, statusFactor = 1.0f;
  int intlRep = 1;
  {
    std::stringstream q;
    q << "SELECT COALESCE(NULLIF(pms.asking_price,0), p.playervalue),"
      << " p.contract_expiry, p.international_reputation"
      << " FROM players p"
      << " LEFT JOIN player_market_status pms"
      << "   ON pms.manager_id=" << managerId << " AND pms.player_id=p.id"
      << " WHERE p.id=" << playerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size() > 0) {
      base       = atoll(TECell(r,0,0).c_str());
      intlRep    = atoi(TECell(r,0,2).c_str());
      std::string exp = TECell(r,0,1);
      if (!exp.empty()) {
        int yrs = std::max(0, atoi(exp.substr(0,4).c_str()) - 2026);
        if (yrs >= 3)      contractFactor = 1.3f;
        else if (yrs <= 1) contractFactor = 0.7f;
      }
    }
    delete r;
  }

  // market status factor
  {
    std::stringstream q;
    q << "SELECT status FROM player_market_status WHERE manager_id=" << managerId
      << " AND player_id=" << playerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size() > 0) {
      std::string st = TECell(r,0,0);
      if (st == "wonderkid")                  statusFactor = 1.4f;
      else if (st == "franchise_player")      statusFactor = 1.8f;
      else if (st == "transfer_listed")       statusFactor = 0.95f;
      else if (st == "surplus_to_requirements") statusFactor = 0.75f;
      else if (st == "expiring_soon")         statusFactor = 0.6f;
    }
    delete r;
  }

  // scarcity factor
  float scarcityFactor = 1.0f;
  {
    std::stringstream q;
    q << "SELECT role FROM players WHERE id=" << playerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    std::string grp;
    if (r && r->data.size()>0) grp = RoleToGroup(TECell(r,0,0));
    delete r;
    if (!grp.empty()) {
      std::stringstream sq;
      sq << "SELECT scarcity_score FROM market_scarcity WHERE manager_id=" << managerId
         << " AND position_group='" << grp << "';";
      DatabaseResult *sr = GetDB()->Query(sq.str());
      if (sr && sr->data.size()>0) {
        int sc = atoi(TECell(sr,0,0).c_str());
        if      (sc > 80) scarcityFactor = 1.5f;
        else if (sc > 60) scarcityFactor = 1.3f;
        else if (sc > 30) scarcityFactor = 1.15f;
      }
      delete sr;
    }
  }

  // selling club selling_pressure
  float sellerFactor = 1.0f;
  {
    std::stringstream q;
    q << "SELECT selling_pressure FROM club_transfer_identity"
      << " WHERE manager_id=" << managerId << " AND club_id=" << sellingClubId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) {
      int sp = atoi(TECell(r,0,0).c_str());
      sellerFactor = 1.0f - (float)sp / 333.0f; // max pressure → 0.7
      sellerFactor = std::max(0.7f, sellerFactor);
    }
    delete r;
  }
  {
    std::stringstream q;
    q << "SELECT club_id FROM managers WHERE id=" << managerId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size() > 0 && atoi(TECell(r,0,0).c_str()) == sellingClubId)
      sellerFactor = std::max(0.90f, sellerFactor);
    delete r;
  }

  float buyerUrgency   = 1.0f + (needScore > 70 ? 0.3f : needScore > 50 ? 0.15f : 0.0f);
  float deadlineFactor = 1.0f + (float)deadlinePressure / 200.0f; // up to 1.5x
  float hypeFactor     = 1.0f + (float)std::max(0, agentPressure - 30) / 230.0f; // up to 1.3x

  // previous rejected_offer adds 15%
  float rejFactor = 1.0f;
  {
    std::stringstream q;
    q << "SELECT COUNT(*) FROM club_player_relationship WHERE manager_id=" << managerId
      << " AND club_id=" << buyingClubId << " AND player_id=" << playerId
      << " AND relationship_type='rejected_offer';";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0 && atoi(TECell(r,0,0).c_str())>0) rejFactor = 1.15f;
    delete r;
  }

  long long val = (long long)(base
    * contractFactor * scarcityFactor * sellerFactor
    * buyerUrgency * deadlineFactor * hypeFactor
    * statusFactor * rejFactor);
  return std::max(50000LL, val);
}

int CalculateAcceptanceScore(int managerId, int playerId,
                                     int buyingClubId, int sellingClubId,
                                     long long offeredWage, const std::string &promisedRole,
                                     int agentPressure) {
  // Load player traits
  int ambition=50, loyalty=50, greed=50, ego=50, trophy=50;
  {
    std::stringstream q;
    q << "SELECT ambition,loyalty,greed,ego,trophy_hunger"
      << " FROM player_traits WHERE manager_id=" << managerId
      << " AND player_id=" << playerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) {
      ambition = atoi(TECell(r,0,0).c_str());
      loyalty  = atoi(TECell(r,0,1).c_str());
      greed    = atoi(TECell(r,0,2).c_str());
      ego      = atoi(TECell(r,0,3).c_str());
      trophy   = atoi(TECell(r,0,4).c_str());
    }
    delete r;
  }

  long long curWage = 0;
  int age = 25;
  {
    std::stringstream q;
    q << "SELECT COALESCE(pss.weekly_wage,p.weekly_wage), p.age"
      << " FROM players p LEFT JOIN player_save_state pss"
      << " ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
      << " WHERE p.id=" << playerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) {
      curWage = atoll(TECell(r,0,0).c_str());
      age     = atoi(TECell(r,0,1).c_str());
    }
    delete r;
  }

  int buyingIntl=5, sellingIntl=5;
  int buyingDom=5;
  {
    std::stringstream q;
    q << "SELECT international_prestige,domestic_prestige FROM teams WHERE id=" << buyingClubId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) {
      buyingIntl = atoi(TECell(r,0,0).c_str());
      buyingDom = atoi(TECell(r,0,1).c_str());
    }
    delete r;
    q.str("");
    q << "SELECT international_prestige,domestic_prestige FROM teams WHERE id=" << sellingClubId << ";";
    r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) {
      sellingIntl = atoi(TECell(r,0,0).c_str());
    }
    delete r;
  }

  // Wage fit: once the offer clears the player's demand, extra money should not
  // endlessly inflate the same decision. Very low current wages still get a sane
  // demand floor so free/academy records do not accept anything by default.
  long long wageBase = std::max(1000LL, curWage);
  long long expected = (long long)((float)wageBase * (1.0f + greed / 200.0f));
  float wageFit = expected > 0
    ? std::min(1.0f, (float)offeredWage / (float)expected) : 0.5f;

  // Role fit
  float roleFit = 0.5f;
  if (promisedRole == "star_player") roleFit = ego > 50 ? 1.0f : 0.6f;
  else if (promisedRole == "important") roleFit = ego > 70 ? 0.85f : 0.78f;
  else if (promisedRole == "rotation")  roleFit = ego < 40 ? 0.4f : (ego > 65 ? 0.1f : 0.35f);
  else if (promisedRole == "prospect")  roleFit = (age < 21 && ambition > 60) ? 0.5f : (ego > 50 ? 0.0f : 0.3f);

  // Destination prestige is the club's raw pull. International prestige is
  // deliberately dominant here: domestic dominance in a smaller league should
  // not flatten a move to a bigger global club.
  float destinationPull = std::min(1.0f,
    ((float)buyingIntl * 0.95f + (float)buyingDom * 0.05f) / 10.0f);

  // Step-up pull is separate from destination prestige. A move from Braga to
  // Man United should be attractive even for a low-ambition player, while a
  // sideways elite-to-elite move should not get this bonus for free.
  float intlGap = std::max(0, buyingIntl - sellingIntl) / 7.0f;
  float stepUpPull = std::min(1.0f, intlGap);
  float ambitionMultiplier = 0.75f + (ambition / 400.0f) + (trophy / 500.0f);
  stepUpPull = std::min(1.0f, stepUpPull * ambitionMultiplier);

  // Playtime fit
  std::string role;
  { std::stringstream q; q << "SELECT role FROM players WHERE id=" << playerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) role = TECell(r,0,0);
    delete r; }
  int samePos = 0;
  if (!role.empty()) {
    std::stringstream q;
    q << "SELECT COUNT(*) FROM players p"
      << " LEFT JOIN player_save_state pss"
      << " ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
      << " WHERE COALESCE(pss.team_id,p.team_id)=" << buyingClubId
      << " AND role='" << TESql(role) << "';";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) samePos = atoi(TECell(r,0,0).c_str());
    delete r;
  }
  float playtimeFit = std::max(0.0f, 1.0f - samePos * 0.20f);
  if (promisedRole == "star_player") playtimeFit = std::max(playtimeFit, 0.80f);
  else if (promisedRole == "important") playtimeFit = std::max(playtimeFit, 0.60f);

  float loyaltyDrag = loyalty / 100.0f;

  float score = wageFit         * 30.0f
              + roleFit         * 15.0f
              + destinationPull * 15.0f
              + stepUpPull      * 20.0f
              + playtimeFit     * 10.0f
              - loyaltyDrag     * 8.0f;

  // agent pressure modifier
  score += (float)(agentPressure - 50) * 0.08f;

  // relationship modifiers
  {
    std::stringstream q;
    q << "SELECT relationship_type FROM club_player_relationship"
      << " WHERE manager_id=" << managerId << " AND club_id=" << buyingClubId
      << " AND player_id=" << playerId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r) {
      for (unsigned int i=0; i<r->data.size(); i++) {
        std::string rt = TECell(r,i,0);
        if (rt == "former_player")   score += 8.0f;
        if (rt == "rejected_offer")  score -= 10.0f;
        if (rt == "unhappy_at_club") score += 15.0f;
      }
      delete r;
    }
  }

  return std::max(0, std::min(100, (int)score));
}

static void AttemptInitiateNegotiations(int managerId, int clubId,
                                         int userClubId,
                                         const std::string &currentDate,
                                         int seasonYear,
                                         const std::map<std::string,int> &needs,
                                         int deadlinePressure,
                                         unsigned int rng) {
  if (clubId == userClubId) return;

  int aggression=50, irrat=25, agePref=0;
  int youthFocus=40, domesticBias=40, prestige=40, wageWill=50;
  int deadlinePanic=30;
  {
    std::stringstream q;
    q << "SELECT aggression,irrationality,age_preference,youth_focus,"
      << "domestic_bias,prestige_bias,wage_willingness,"
      << "deadline_panic FROM club_transfer_identity"
      << " WHERE manager_id=" << managerId << " AND club_id=" << clubId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) {
      aggression   = atoi(TECell(r,0,0).c_str());
      irrat        = atoi(TECell(r,0,1).c_str());
      agePref      = atoi(TECell(r,0,2).c_str());
      youthFocus   = atoi(TECell(r,0,3).c_str());
      domesticBias = atoi(TECell(r,0,4).c_str());
      prestige     = atoi(TECell(r,0,5).c_str());
      wageWill     = atoi(TECell(r,0,6).c_str());
      deadlinePanic= atoi(TECell(r,0,7).c_str());
    }
    delete r;
  }

  // Squad size check: suppress if over limit
  int squadSize = 0;
  { std::stringstream q; q << "SELECT COUNT(*) FROM players WHERE team_id=" << clubId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) squadSize = atoi(TECell(r,0,0).c_str()); delete r; }
  if (squadSize > 32) return;

  // Active negotiations: don't run more than 2 unless very aggressive
  int activeDeals = 0;
  { std::stringstream q;
    q << "SELECT COUNT(*) FROM transfer_negotiations WHERE manager_id=" << managerId
      << " AND buying_club_id=" << clubId
      << " AND state NOT IN ('completed','collapsed');";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) activeDeals = atoi(TECell(r,0,0).c_str()); delete r; }
  if (activeDeals >= 2 && aggression < 75) return;
  if (activeDeals >= 3) return;

  // Find position group with highest need
  std::string bestGroup; int bestNeed = 0;
  for (auto &kv : needs) { if (kv.second > bestNeed) { bestNeed=kv.second; bestGroup=kv.first; } }

  // Irrationality roll: may override need entirely
  bool irrational = (int)(rng % 100) < irrat;
  int daysLeft = DaysToWindowEnd(currentDate);
  int marketChance = 15 + (daysLeft <= 21 ? 10 : 0) + (daysLeft <= 14 ? 10 : 0);
  bool opportunisticMarket = !irrational && ((int)((rng >> 16) % 100) < marketChance);
  int effectiveNeed = irrational ? 100 : bestNeed;

  // Deadline panic boost
  if (deadlinePressure > 80 && deadlinePanic > 50) effectiveNeed += 30;
  if (opportunisticMarket && effectiveNeed < 65) effectiveNeed = 45;

  if (effectiveNeed < 40) return;

  // Determine target tier based on effectiveNeed
  int tier = (effectiveNeed > 65) ? 1 : 2;
  if (deadlinePressure > 80 && bestNeed < 40) tier = 3;

  // Map group back to role list for SQL IN clause
  static const struct { const char *group; const char *roles; } kGroupRoles[] = {
    {"GK",    "'GK'"},
    {"CB",    "'CB'"},
    {"FB_WB", "'LB','RB','LWB','RWB'"},
    {"DM",    "'CDM'"},
    {"CM",    "'CM'"},
    {"AM_W",  "'CAM','LM','RM','LW','RW'"},
    {"ST",    "'CF','ST'"},
    {nullptr, nullptr}
  };
  std::string roleIn;
  if (opportunisticMarket && bestNeed < 65) {
    roleIn = "'GK','CB','LB','RB','LWB','RWB','CDM','CM','CAM','LM','RM','LW','RW','CF','ST'";
  } else {
    for (int i=0; kGroupRoles[i].group; i++) {
      if (bestGroup == kGroupRoles[i].group) { roleIn = kGroupRoles[i].roles; break; }
    }
  }
  if (roleIn.empty()) return;

  // Age preference filter
  std::string ageClause;
  if (!irrational) {
    if (agePref < -20) ageClause = " AND p.age <= 23";
    else if (agePref > 20) ageClause = " AND p.age >= 27";
  }

  // base_stat is stored as 0.0-1.0 in the current database.
  float qualMin = (tier == 1) ? 0.75f : (tier == 2) ? 0.60f : 0.40f;

  // Select target from knowledge pool. Transfer-listed and expiring players are
  // public market targets, so they do not require pre-existing scout knowledge.
  std::stringstream tq;
  tq << "SELECT p.id, p.base_stat, COALESCE(pss.weekly_wage,p.weekly_wage), t.id as cur_club,"
     << " p.international_reputation, COALESCE(cpk.knowledge,0),"
     << " COALESCE(pms.status,'normal')"
     << " FROM players p"
     << " LEFT JOIN player_save_state pss"
     << "   ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
     << " JOIN teams t ON t.id=COALESCE(pss.team_id,p.team_id)"
     << " LEFT JOIN club_player_knowledge cpk"
     << "   ON cpk.player_id=p.id AND cpk.manager_id=" << managerId
     << "   AND cpk.club_id=" << clubId
     << " LEFT JOIN player_market_status pms"
     << "   ON pms.player_id=p.id AND pms.manager_id=" << managerId
     << " WHERE p.role IN (" << roleIn << ")"
     << " AND t.id != " << clubId
     << " AND p.base_stat >= " << qualMin
     << " AND (COALESCE(cpk.knowledge,0) >= 25"
     << "      OR pms.status IN ('transfer_listed','surplus_to_requirements','expiring_soon'))"
     << (opportunisticMarket && bestNeed < 65
          ? " AND pms.status IN ('transfer_listed','surplus_to_requirements','expiring_soon')"
          : "")
     << ageClause
     << " AND NOT EXISTS ("
     <<   "SELECT 1 FROM negotiation_cooldowns nc"
     <<   " WHERE nc.manager_id=" << managerId
     <<   " AND nc.buying_club_id=" << clubId
     <<   " AND nc.player_id=p.id"
     <<   " AND nc.cooldown_until >= '" << currentDate << "'"
     << ")"
     << " AND NOT EXISTS ("
     <<   "SELECT 1 FROM transfer_negotiations tn"
     <<   " WHERE tn.manager_id=" << managerId
     <<   " AND tn.buying_club_id=" << clubId
     <<   " AND tn.player_id=p.id"
     <<   " AND tn.state NOT IN ('completed','collapsed')"
     << ")"
     << " ORDER BY"
     << " (CASE WHEN pms.status='transfer_listed' THEN 80"
     << "         WHEN pms.status='surplus_to_requirements' THEN 65"
     << "         WHEN pms.status='expiring_soon' THEN 45"
     << "         ELSE 0 END)"
     << " + COALESCE(cpk.knowledge,0) DESC,"
     << " p.base_stat DESC LIMIT 25;";
  DatabaseResult *tr = GetDB()->Query(tq.str());
  if (!tr || tr->data.size() == 0) { delete tr; return; }

  int pick = (int)(rng % std::min((unsigned int)tr->data.size(), 5u));
  int targetId    = atoi(TECell(tr, pick, 0).c_str());
  int sellingClub = atoi(TECell(tr, pick, 3).c_str());
  long long wage  = atoll(TECell(tr, pick, 2).c_str());
  delete tr;

  long long ctxVal = CalculateContextualValue(managerId, targetId, sellingClub,
                                               clubId, effectiveNeed, deadlinePressure, 0);
  float openingPct = 0.82f;
  if (sellingClub == userClubId) openingPct = 0.92f;
  if (opportunisticMarket) openingPct += 0.04f;
  if (irrational || deadlinePressure > 80) openingPct += 0.06f;
  openingPct = std::min(1.10f, openingPct);
  long long offerFee  = (long long)(ctxVal * openingPct);
  long long offerWage = (long long)(wage * (1.0f + wageWill / 200.0f));

  int targetEgo = 50;
  { std::stringstream q; q << "SELECT ego FROM player_traits WHERE manager_id=" << managerId
      << " AND player_id=" << targetId << ";";
    DatabaseResult *r = GetDB()->Query(q.str());
    if (r && r->data.size()>0) targetEgo = atoi(TECell(r,0,0).c_str()); delete r; }
  std::string promised = "rotation";
  if (effectiveNeed > 70 || targetEgo > 65) promised = "important";
  if (targetEgo > 80 || irrational) promised = "star_player";

  std::stringstream iq;
  iq << "INSERT INTO transfer_negotiations"
     << "(manager_id,buying_club_id,selling_club_id,player_id,state,"
     << "offered_fee,offered_wage,promised_role,days_in_state,initiated_date,"
     << "deadline_pressure,agent_pressure,irrationality_driven,tier)"
     << " VALUES(" << managerId << "," << clubId << "," << sellingClub << ","
     << targetId << ",'initiated'," << offerFee << "," << offerWage << ",'"
     << promised << "',0,'" << currentDate << "',"
     << deadlinePressure << "," << std::min(targetEgo/2, 40) << ","
     << (irrational?1:0) << "," << tier << ");";
  DatabaseResult *ir = GetDB()->Query(iq.str());
  delete ir;

  std::string pName;
  { std::stringstream q; q << "SELECT firstname||' '||lastname FROM players WHERE id=" << targetId << ";";
    DatabaseResult *r = GetDB()->Query(q.str()); if(r&&r->data.size()>0) pName=TECell(r,0,0); delete r; }
  std::string cName;
  { std::stringstream q; q << "SELECT name FROM teams WHERE id=" << clubId << ";";
    DatabaseResult *r = GetDB()->Query(q.str()); if(r&&r->data.size()>0) cName=TECell(r,0,0); delete r; }

  if (sellingClub == userClubId) {
    // Incoming bid on the user's own player — notify inbox, wait for user to approve/reject
    std::string feeStr;
    { long long f = offerFee; char buf[32];
      if (f >= 1000000) snprintf(buf, sizeof(buf), "£%.1fM", f/1000000.0);
      else snprintf(buf, sizeof(buf), "£%.0fK", f/1000.0);
      feeStr = buf; }
    InsertTransferNews(managerId, currentDate,
      cName + " submit bid for " + pName,
      "incoming_bid", targetId, sellingClub, clubId);
    InsertInboxMessage(managerId,
      "Incoming bid: " + cName + " want " + pName,
      cName + " have submitted a bid of " + feeStr + " for " + pName
      + ". Review the offer in Transfers → Incoming Bids.",
      "transfer", currentDate);
    // If the career currently loaded belongs to this manager, reload inbox immediately
    if (g_CareerHub.managerId == managerId) {
      g_CareerHub.LoadFromDB(managerId, g_CareerHub.clubId);
    }
  } else {
    InsertTransferNews(managerId, currentDate,
      pName + " linked with move to " + cName,
      "rumour", targetId, sellingClub, clubId);
  }

  printf("[TRANSFER] Initiate: manager=%d buyer=%d seller=%d player=%d tier=%d irrational=%d\n",
         managerId, clubId, sellingClub, targetId, tier, irrational?1:0);
}

static void AdvanceNegotiations(int managerId, int userClubId,
                                 const std::string &currentDate,
                                 int seasonYear, int deadlinePressure) {
  std::stringstream nq;
  nq << "SELECT id,buying_club_id,selling_club_id,player_id,state,"
     << "offered_fee,offered_wage,promised_role,days_in_state,agent_pressure,"
     << "acceptance_score,counter_offer_count,tier,competing_bid_club_id"
     << " FROM transfer_negotiations"
     << " WHERE manager_id=" << managerId
     << " AND is_user_bid=0"
     << " AND buying_club_id!=" << userClubId
     << " AND NOT (selling_club_id=" << userClubId << " AND seller_approved=0)"
     << " AND state NOT IN ('completed','collapsed');";
  DatabaseResult *nr = GetDB()->Query(nq.str());
  if (!nr) return;

  struct NegRow {
    int id, buyer, seller, player, days, agentP, acceptScore, coCount, tier, competingClub;
    std::string state, offWage, offFee, role;
  };
  std::vector<NegRow> rows;
  for (unsigned int i=0; i < nr->data.size(); i++) {
    NegRow n;
    n.id           = atoi(TECell(nr,i,0).c_str());
    n.buyer        = atoi(TECell(nr,i,1).c_str());
    n.seller       = atoi(TECell(nr,i,2).c_str());
    n.player       = atoi(TECell(nr,i,3).c_str());
    n.state        = TECell(nr,i,4);
    n.offFee       = TECell(nr,i,5);
    n.offWage      = TECell(nr,i,6);
    n.role         = TECell(nr,i,7);
    n.days         = atoi(TECell(nr,i,8).c_str());
    n.agentP       = atoi(TECell(nr,i,9).c_str());
    n.acceptScore  = atoi(TECell(nr,i,10).c_str());
    n.coCount      = atoi(TECell(nr,i,11).c_str());
    n.tier         = atoi(TECell(nr,i,12).c_str());
    n.competingClub= atoi(TECell(nr,i,13).c_str());
    rows.push_back(n);
  }
  delete nr;

  unsigned int dateSeed = (unsigned int)DateToJulian(currentDate);

  for (auto &n : rows) {
    unsigned int rng = (unsigned int)(managerId*7919u ^ n.buyer*31337u ^ n.player*1999u ^ dateSeed);
    std::string newState = n.state;
    std::string collapseReason;
    bool didComplete = false;
    long long fee  = atoll(n.offFee.c_str());
    long long wage = atoll(n.offWage.c_str());

    int newAgentP = n.agentP;
    if (n.state == "negotiating" || n.state == "stalled" || n.state == "player_waiting")
      newAgentP = std::min(100, n.agentP + 5);

    if (n.state == "initiated") {
      int waitDays = 3;
      { std::stringstream q;
        q << "SELECT aggression FROM club_transfer_identity WHERE manager_id=" << managerId
          << " AND club_id=" << n.buyer << ";";
        DatabaseResult *r = GetDB()->Query(q.str());
        if (r && r->data.size()>0) {
          int agg = atoi(TECell(r,0,0).c_str());
          waitDays = 3 - (agg / 50);
          waitDays = std::max(1, std::min(3, waitDays));
        }
        delete r; }
      if (n.days >= waitDays) newState = "offer_made";

    } else if (n.state == "offer_made") {
      int sellPressure=20, loyalty=50;
      { std::stringstream q;
        q << "SELECT selling_pressure,loyalty_to_players FROM club_transfer_identity"
          << " WHERE manager_id=" << managerId << " AND club_id=" << n.seller << ";";
        DatabaseResult *r = GetDB()->Query(q.str());
        if (r && r->data.size()>0) {
          sellPressure = atoi(TECell(r,0,0).c_str());
          loyalty      = atoi(TECell(r,0,1).c_str());
        }
        delete r; }
      long long ctxVal = CalculateContextualValue(managerId, n.player, n.seller,
                                                   n.buyer, 50, deadlinePressure, n.agentP);
      bool untouchable = false;
      { std::stringstream q;
        q << "SELECT status FROM player_market_status WHERE manager_id=" << managerId
          << " AND player_id=" << n.player << ";";
        DatabaseResult *r = GetDB()->Query(q.str());
        if (r && r->data.size()>0 && TECell(r,0,0)=="franchise_player") untouchable=true;
        delete r; }
      if (untouchable && sellPressure < 70) {
        newState = "collapsed"; collapseReason = "untouchable";
      } else if (n.days >= 2) {
        if (fee < (long long)(ctxVal * 0.60f) && sellPressure < 40)
          { newState = "collapsed"; collapseReason = "fee_too_low"; }
        else
          newState = "negotiating";
      }

    } else if (n.state == "negotiating") {
      int score = CalculateAcceptanceScore(managerId, n.player, n.buyer, n.seller,
                                            wage, n.role, n.agentP);
      if (n.competingClub == 0 && (int)(rng % 100) < 15) {
        std::stringstream cq;
        cq << "SELECT club_id FROM club_player_knowledge"
           << " WHERE manager_id=" << managerId << " AND player_id=" << n.player
           << " AND knowledge >= 60 AND club_id != " << n.buyer
           << " AND club_id != " << n.seller
           << " AND club_id != " << userClubId
           << " ORDER BY RANDOM() LIMIT 1;";
        DatabaseResult *cr = GetDB()->Query(cq.str());
        if (cr && cr->data.size()>0) {
          int cBid = atoi(TECell(cr,0,0).c_str());
          std::stringstream uq;
          uq << "UPDATE transfer_negotiations SET competing_bid_club_id=" << cBid
             << ",state='competing_bid' WHERE id=" << n.id << ";";
          DatabaseResult *ur = GetDB()->Query(uq.str()); delete ur;
          std::string pName, cName;
          { std::stringstream q; q << "SELECT firstname||' '||lastname FROM players WHERE id=" << n.player << ";";
            DatabaseResult *r = GetDB()->Query(q.str()); if(r&&r->data.size()>0) pName=TECell(r,0,0); delete r; }
          { std::stringstream q; q << "SELECT name FROM teams WHERE id=" << cBid << ";";
            DatabaseResult *r = GetDB()->Query(q.str()); if(r&&r->data.size()>0) cName=TECell(r,0,0); delete r; }
          InsertTransferNews(managerId, currentDate, cName + " show interest in " + pName,
                             "rumour", n.player, n.seller, cBid);
          newAgentP = std::min(100, newAgentP + 20);
        }
        delete cr;
        goto update_row;
      }
      { std::stringstream uq;
        uq << "UPDATE transfer_negotiations SET acceptance_score=" << score
           << " WHERE id=" << n.id << ";";
        DatabaseResult *ur = GetDB()->Query(uq.str()); delete ur; }
      if (score > 50)
        newState = (n.role == "star_player" && score < 65) ? "player_waiting" : "medical_pending";
      else if (score >= 33) newState = "counter_offer";
      else if (n.days > 12) { newState = "collapsed"; collapseReason = "player_rejected"; }
      if (n.state == "negotiating" && n.days > 5 && newState == "negotiating")
        newState = "stalled";

    } else if (n.state == "counter_offer") {
      int wageWill = 50;
      { std::stringstream q;
        q << "SELECT wage_willingness,negotiation_personality FROM club_transfer_identity"
          << " WHERE manager_id=" << managerId << " AND club_id=" << n.buyer << ";";
        DatabaseResult *r = GetDB()->Query(q.str());
        if (r && r->data.size()>0) {
          wageWill = atoi(TECell(r,0,0).c_str());
          if (TECell(r,0,1) == "fast_closer") wageWill = std::min(100, wageWill+20);
        }
        delete r; }
      float feePct  = 0.05f + (wageWill / 200.0f);
      float wagePct = 0.03f + (wageWill / 500.0f);
      fee  = (long long)(fee  * (1.0f + feePct));
      wage = (long long)(wage * (1.0f + wagePct));
      { std::stringstream uq;
        uq << "UPDATE transfer_negotiations SET offered_fee=" << fee
           << ",offered_wage=" << wage
           << ",counter_offer_count=" << (n.coCount+1)
           << " WHERE id=" << n.id << ";";
        DatabaseResult *ur = GetDB()->Query(uq.str()); delete ur; }
      if (n.coCount > 3) {
        int score = CalculateAcceptanceScore(managerId, n.player, n.buyer, n.seller,
                                              wage, n.role, n.agentP);
        if (score < 38) { newState = "collapsed"; collapseReason = "negotiation_failed"; }
        else newState = "negotiating";
      } else {
        newState = "negotiating";
      }

    } else if (n.state == "stalled") {
      int reviveChance = (deadlinePressure > 80) ? 60 : 30;
      if ((int)(rng % 100) < reviveChance) newState = "negotiating";
      else if (n.days > 10) { newState = "collapsed"; collapseReason = "stalled_too_long"; }

    } else if (n.state == "competing_bid") {
      int agg = 50;
      { std::stringstream q;
        q << "SELECT aggression FROM club_transfer_identity WHERE manager_id=" << managerId
          << " AND club_id=" << n.buyer << ";";
        DatabaseResult *r = GetDB()->Query(q.str());
        if (r && r->data.size()>0) agg = atoi(TECell(r,0,0).c_str()); delete r; }
      if ((int)(rng % 100) < agg) {
        fee = (long long)(fee * 1.12f);
        std::stringstream uq;
        uq << "UPDATE transfer_negotiations SET offered_fee=" << fee
           << ",competing_bid_club_id=0,state='negotiating' WHERE id=" << n.id << ";";
        DatabaseResult *ur = GetDB()->Query(uq.str()); delete ur;
        goto update_row;
      } else {
        newState = "collapsed"; collapseReason = "lost_to_competitor";
      }

    } else if (n.state == "player_waiting") {
      if (n.days >= 3) newState = "medical_pending";

    } else if (n.state == "medical_pending") {
      int failChance = (deadlinePressure > 80) ? 5 : 2;
      if ((int)(rng % 100) < failChance) {
        newState = "collapsed"; collapseReason = "failed_medical";
        AddUnhappiness(managerId, n.player, "failed_medical_stress", 10, currentDate);
        std::stringstream rq;
        rq << "INSERT INTO club_player_relationship(manager_id,club_id,player_id,relationship_type,created_date)"
           << " VALUES(" << managerId << "," << n.buyer << "," << n.player
           << ",'failed_medical','" << currentDate << "');";
        DatabaseResult *rr = GetDB()->Query(rq.str()); delete rr;
        std::stringstream cq;
        cq << "INSERT INTO negotiation_cooldowns(manager_id,buying_club_id,player_id,cooldown_until,reason)"
           << " VALUES(" << managerId << "," << n.buyer << "," << n.player
           << ",'" << AddDays(currentDate,30) << "','failed_medical');";
        DatabaseResult *cr = GetDB()->Query(cq.str()); delete cr;
      } else {
        SetPlayerSaveState(managerId, n.player, n.buyer, wage);
        if (n.buyer == userClubId) RemovePlayerScoutingRecords(managerId, n.player);
        std::stringstream fq;
        fq << "UPDATE club_finances SET transfer_budget=MAX(0,transfer_budget-" << fee << ")"
           << " WHERE manager_id=" << managerId << " AND club_id=" << n.buyer << ";";
        DatabaseResult *fr = GetDB()->Query(fq.str()); delete fr;
        std::stringstream rlq;
        rlq << "INSERT INTO club_player_relationship(manager_id,club_id,player_id,relationship_type,created_date)"
            << " VALUES(" << managerId << "," << n.seller << "," << n.player
            << ",'former_player','" << currentDate << "');";
        DatabaseResult *rlr = GetDB()->Query(rlq.str()); delete rlr;
        newState = "completed";
        didComplete = true;
        std::string pName, bName, sName;
        { std::stringstream q; q << "SELECT firstname||' '||lastname FROM players WHERE id=" << n.player << ";";
          DatabaseResult *r = GetDB()->Query(q.str()); if(r&&r->data.size()>0) pName=TECell(r,0,0); delete r; }
        { std::stringstream q; q << "SELECT name FROM teams WHERE id=" << n.buyer << ";";
          DatabaseResult *r = GetDB()->Query(q.str()); if(r&&r->data.size()>0) bName=TECell(r,0,0); delete r; }
        { std::stringstream q; q << "SELECT name FROM teams WHERE id=" << n.seller << ";";
          DatabaseResult *r = GetDB()->Query(q.str()); if(r&&r->data.size()>0) sName=TECell(r,0,0); delete r; }
        InsertTransferNews(managerId, currentDate,
          pName + " joins " + bName + " from " + sName,
          "completed", n.player, n.seller, n.buyer);
        InsertFinanceTransaction(managerId, n.buyer, currentDate, "transfer",
          "Transfer fee paid: " + pName + " from " + sName, -fee);
        InsertFinanceTransaction(managerId, n.seller, currentDate, "transfer",
          "Transfer fee received: " + pName + " to " + bName, fee);
        if (n.buyer == userClubId || n.seller == userClubId) {
          InsertInboxMessage(managerId, pName + " transfer complete",
            "The deal is done. " + pName + " has joined " + bName + " from " + sName + ".",
            "transfer", currentDate);
        }
        printf("[TRANSFER] Completed: player=%d buyer=%d seller=%d fee=%lld\n",
               n.player, n.buyer, n.seller, fee);
      }
    }

    if (newState == "collapsed") {
      std::string pName, cName;
      { std::stringstream q; q << "SELECT firstname||' '||lastname FROM players WHERE id=" << n.player << ";";
        DatabaseResult *r = GetDB()->Query(q.str()); if(r&&r->data.size()>0) pName=TECell(r,0,0); delete r; }
      { std::stringstream q; q << "SELECT name FROM teams WHERE id=" << n.buyer << ";";
        DatabaseResult *r = GetDB()->Query(q.str()); if(r&&r->data.size()>0) cName=TECell(r,0,0); delete r; }
      InsertTransferNews(managerId, currentDate,
        "Deal collapses — " + pName + " stays at current club",
        "collapsed", n.player, n.seller, n.buyer);
      unsigned int cd = 7 + (rng % 8);
      std::stringstream cdq;
      cdq << "INSERT INTO negotiation_cooldowns(manager_id,buying_club_id,player_id,cooldown_until,reason)"
          << " VALUES(" << managerId << "," << n.buyer << "," << n.player
          << ",'" << AddDays(currentDate, (int)cd) << "','" << TESql(collapseReason) << "');";
      DatabaseResult *cdr = GetDB()->Query(cdq.str()); delete cdr;
      std::stringstream rrq;
      rrq << "INSERT OR IGNORE INTO club_player_relationship(manager_id,club_id,player_id,relationship_type,created_date)"
          << " VALUES(" << managerId << "," << n.buyer << "," << n.player
          << ",'rejected_offer','" << currentDate << "');";
      DatabaseResult *rrr = GetDB()->Query(rrq.str()); delete rrr;
      { std::stringstream q;
        q << "SELECT COUNT(*) FROM transfer_negotiations WHERE manager_id=" << managerId
          << " AND player_id=" << n.player
          << " AND selling_club_id=" << n.seller << " AND state='collapsed';";
        DatabaseResult *r = GetDB()->Query(q.str());
        if (r && r->data.size()>0 && atoi(TECell(r,0,0).c_str()) >= 3)
          AddUnhappiness(managerId, n.player, "blocked_move", 25, currentDate);
        delete r; }
      TriggerTransferCascade(managerId, n.seller, n.buyer, currentDate, seasonYear, 1);
    }

    update_row:
    {
      bool stateChanged = newState != n.state;
      std::stringstream uq;
      uq << "UPDATE transfer_negotiations SET"
         << " state='" << newState << "'"
         << ",days_in_state=" << (stateChanged ? 0 : n.days+1)
         << ",agent_pressure=" << newAgentP
         << ",deadline_pressure=" << deadlinePressure
         << ",collapse_reason='" << TESql(collapseReason) << "'"
         << " WHERE id=" << n.id << ";";
      DatabaseResult *ur = GetDB()->Query(uq.str());
      delete ur;
    }

    if (didComplete)
      TriggerTransferCascade(managerId, n.seller, 0, currentDate, seasonYear, 1);
  }
}

static void CalculateMarketScarcity(int managerId, const std::string &currentDate) {
  static const struct { const char *group; const char *roles; } kGroupRoles[] = {
    {"GK",    "'GK'"},
    {"CB",    "'CB'"},
    {"FB_WB", "'LB','RB','LWB','RWB'"},
    {"DM",    "'CDM'"},
    {"CM",    "'CM'"},
    {"AM_W",  "'CAM','LM','RM','LW','RW'"},
    {"ST",    "'CF','ST'"},
    {nullptr, nullptr}
  };

  std::map<std::string,float> leagueAvg;
  { DatabaseResult *r = GetDB()->Query(
      "SELECT role, AVG(base_stat) FROM players GROUP BY role;");
    if (r) { for (unsigned int i=0; i<r->data.size(); i++)
      leagueAvg[RoleToGroup(TECell(r,i,0))] = atof(TECell(r,i,1).c_str());
    delete r; } }

  for (int gi=0; kGroupRoles[gi].group; gi++) {
    std::string grp(kGroupRoles[gi].group);
    float avg = leagueAvg.count(grp) ? leagueAvg[grp] : 65.0f;
    std::stringstream q;
    q << "SELECT COUNT(*) FROM players p"
      << " LEFT JOIN player_market_status pms ON pms.player_id=p.id AND pms.manager_id=" << managerId
      << " WHERE p.role IN (" << kGroupRoles[gi].roles << ")"
      << " AND p.base_stat >= " << avg
      << " AND pms.status IN ('expiring_soon','surplus_to_requirements','transfer_listed');";
    DatabaseResult *r = GetDB()->Query(q.str());
    int supply = 0;
    if (r && r->data.size()>0) supply = atoi(TECell(r,0,0).c_str());
    delete r;
    int scarcity = std::max(0, std::min(100, 100 - supply * 8));
    std::stringstream uq;
    uq << "INSERT OR REPLACE INTO market_scarcity(manager_id,position_group,scarcity_score,last_updated)"
       << " VALUES(" << managerId << ",'" << grp << "'," << scarcity << ",'" << currentDate << "');";
    DatabaseResult *ur = GetDB()->Query(uq.str()); delete ur;
  }
}

static void BlockUnauthorizedUserClubBuyers(int managerId, int userClubId) {
  if (userClubId <= 0) return;

  std::stringstream rq;
  rq << "SELECT id,player_id,selling_club_id,offered_fee"
     << " FROM transfer_negotiations"
     << " WHERE manager_id=" << managerId
     << " AND buying_club_id=" << userClubId
     << " AND is_user_bid=0"
     << " AND state='completed';";
  DatabaseResult *rr = GetDB()->Query(rq.str());
  if (rr) {
    for (unsigned int i = 0; i < rr->data.size(); i++) {
      int nid = atoi(TECell(rr,i,0).c_str());
      int playerId = atoi(TECell(rr,i,1).c_str());
      int sellerId = atoi(TECell(rr,i,2).c_str());
      long long fee = atoll(TECell(rr,i,3).c_str());

      long long originalWage = 0;
      std::string originalExpiry;
      if (playerId > 0) {
        std::stringstream wq;
        wq << "SELECT weekly_wage, contract_expiry FROM players WHERE id=" << playerId << ";";
        DatabaseResult *wr = GetDB()->Query(wq.str());
        if (wr && wr->data.size() > 0) {
          originalWage = atoll(TECell(wr,0,0).c_str());
          originalExpiry = TECell(wr,0,1);
        }
        delete wr;
      }

      if (playerId > 0 && sellerId > 0) {
        SetPlayerSaveState(managerId, playerId, sellerId, originalWage, originalExpiry);
      }
      if (fee > 0) {
        std::stringstream fq;
        fq << "UPDATE club_finances SET transfer_budget=transfer_budget+" << fee
           << " WHERE manager_id=" << managerId << " AND club_id=" << userClubId << ";";
        DatabaseResult *fr = GetDB()->Query(fq.str()); delete fr;

        std::stringstream sq;
        sq << "UPDATE club_finances SET cash_balance=MAX(0,cash_balance-" << fee << ")"
           << " WHERE manager_id=" << managerId << " AND club_id=" << sellerId << ";";
        DatabaseResult *sr = GetDB()->Query(sq.str()); delete sr;
      }

      std::stringstream uq;
      uq << "UPDATE transfer_negotiations SET state='collapsed',"
         << "collapse_reason='user_club_auto_buyer_blocked'"
         << " WHERE id=" << nid << ";";
      DatabaseResult *ur = GetDB()->Query(uq.str()); delete ur;
    }
    delete rr;
  }

  std::stringstream bq;
  bq << "UPDATE transfer_negotiations SET state='collapsed',"
     << "collapse_reason='user_club_auto_buyer_blocked'"
     << " WHERE manager_id=" << managerId
     << " AND buying_club_id=" << userClubId
     << " AND is_user_bid=0"
     << " AND state NOT IN ('completed','collapsed');";
  DatabaseResult *br = GetDB()->Query(bq.str()); delete br;
}

void ProcessDailyTransfers(int managerId, int userClubId,
                            const std::string &currentDate, int seasonYear) {
  bool inWindow = InTransferWindow(currentDate);
  int  daysLeft  = DaysToWindowEnd(currentDate);
  int  deadline  = inWindow ? std::max(0, 100 - daysLeft * 5) : 0;

  // Update market scarcity if stale
  if (inWindow) {
    std::stringstream sq;
    sq << "SELECT last_updated FROM market_scarcity WHERE manager_id=" << managerId
       << " AND position_group='ST' LIMIT 1;";
    DatabaseResult *sr = GetDB()->Query(sq.str());
    bool needsUpdate = true;
    if (sr && sr->data.size()>0 && TECell(sr,0,0) == currentDate) needsUpdate = false;
    delete sr;
    if (needsUpdate) CalculateMarketScarcity(managerId, currentDate);
  }

  // Contract renewals (monthly)
  ProcessContractRenewals(managerId, currentDate);

  // The managed club must never act as an AI buyer. This also repairs saves
  // where older transfer logic already created or completed such a deal.
  BlockUnauthorizedUserClubBuyers(managerId, userClubId);

  // Deadline day chaos scaling (final 48h of window)
  if (inWindow && daysLeft <= 2) {
    // Boost deadline_panic for all clubs by 40 (capped at 100)
    std::stringstream dpq;
    dpq << "UPDATE club_transfer_identity SET deadline_panic=MIN(100,deadline_panic+40)"
        << " WHERE manager_id=" << managerId << ";";
    DatabaseResult *dpr = GetDB()->Query(dpq.str()); delete dpr;
    // Revive stalled negotiations: 70% chance to re-enter negotiating
    std::stringstream revq;
    revq << "SELECT id FROM transfer_negotiations WHERE manager_id=" << managerId
         << " AND state='stalled' AND is_user_bid=0"
         << " AND buying_club_id!=" << userClubId << ";";
    DatabaseResult *revr = GetDB()->Query(revq.str());
    if (revr) {
      unsigned int dseed = (unsigned int)DateToJulian(currentDate);
      for (unsigned int i = 0; i < revr->data.size(); i++) {
        int nid = atoi(TECell(revr,i,0).c_str());
        if ((int)((dseed ^ (unsigned int)nid) % 100) < 70) {
          std::stringstream uq; uq << "UPDATE transfer_negotiations SET state='negotiating',days_in_state=0 WHERE id=" << nid << ";";
          DatabaseResult *ur = GetDB()->Query(uq.str()); delete ur;
        }
      }
      delete revr;
    }
  }

  // Advance all in-flight AI negotiations (user bids handled by TickUserNegotiations)
  AdvanceNegotiations(managerId, userClubId, currentDate, seasonYear, deadline);

  // User negotiations (buy-side async state machine)
  TickUserNegotiations(managerId, userClubId, currentDate, seasonYear);

  // Special events (galactico, collapse sale, wonderkid explosion)
  ProcessSpecialEvents(managerId, userClubId, currentDate, seasonYear);

  // Media pressure resolution and trigger checks
  ProcessMediaPressure(managerId, currentDate);

  // Loan clause daily checks (recall, buy-back warnings, option deadlines)
  ProcessLoanClauses(managerId, currentDate, seasonYear);

  if (!inWindow) return;

  // For each club, attempt to initiate new negotiations
  DatabaseResult *cr = GetDB()->Query(
    "SELECT id FROM teams WHERE transfer_budget > 0;");
  if (!cr) return;

  unsigned int dateSeed = (unsigned int)DateToJulian(currentDate);

  for (unsigned int i=0; i < cr->data.size(); i++) {
    int clubId = atoi(TECell(cr,i,0).c_str());

    // Skip user's managed club — no automatic transfers for the player
    if (clubId == userClubId) continue;

    // Financial guard: skip if in debt panic
    bool debtPanic = false;
    { std::stringstream q;
      q << "SELECT debt_level, transfer_budget FROM club_finances"
        << " WHERE manager_id=" << managerId << " AND club_id=" << clubId << ";";
      DatabaseResult *r = GetDB()->Query(q.str());
      if (r && r->data.size()>0) {
        long long debt = atoll(TECell(r,0,0).c_str());
        long long tbud = atoll(TECell(r,0,1).c_str());
        if (debt > tbud * 2) debtPanic = true;
      }
      delete r; }
    if (debtPanic) continue;

    int agg = 40;
    { std::stringstream q;
      q << "SELECT aggression FROM club_transfer_identity WHERE manager_id=" << managerId
        << " AND club_id=" << clubId << ";";
      DatabaseResult *r = GetDB()->Query(q.str());
      if (r && r->data.size()>0) agg = atoi(TECell(r,0,0).c_str()); delete r; }

    unsigned int rng = (unsigned int)(managerId*7919u ^ clubId*31337u ^ dateSeed);
    int actChance = 10 + agg/4;
    if (daysLeft <= 21) actChance += 8;
    if (daysLeft <= 14) actChance += 8;
    if (deadline > 80) actChance += 20;
    if ((int)(rng % 100) >= actChance) continue;

    auto needs = EvaluateSquadNeeds(managerId, clubId, seasonYear);
    AttemptInitiateNegotiations(managerId, clubId, userClubId, currentDate, seasonYear,
                                 needs, deadline, rng);
  }
  delete cr;
}
