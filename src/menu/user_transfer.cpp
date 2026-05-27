// user_transfer.cpp — Phase 3+4 user-facing transfer system
#include "user_transfer.hpp"
#include "transfer_engine.hpp"
#include "managercareer.hpp"
#include "imgui_career.hpp"
#include <sstream>
#include <string>
#include <vector>
#include <algorithm>
#include <map>
#include <cstdlib>
#include <cstdio>
#include <cstring>

// ---- Local helpers ----------------------------------------------------------

static std::string UTCell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size()) return "";
  if (col >= r->data[row].size()) return "";
  return r->data[row][col];
}

static void UTExec(const std::string &sql) {
  DatabaseResult *r = GetDB()->Query(sql.c_str());
  if (r) delete r;
}

static std::string UTSql(const std::string &in) {
  std::string out;
  for (char c : in) { if (c == '\'') out += "''"; else out += c; }
  return out;
}

static int UTRandInt(unsigned int seed, int lo, int hi) {
  if (hi <= lo) return lo;
  return lo + (int)(seed % (unsigned int)(hi - lo + 1));
}

static int GetManagedClubId(int managerId) {
  std::stringstream q;
  q << "SELECT club_id FROM managers WHERE id=" << managerId << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str().c_str());
  int clubId = 0;
  if (r && r->data.size() > 0) clubId = atoi(UTCell(r,0,0).c_str());
  if (r) delete r;
  return clubId;
}

static int GetPlayerCurrentClub(int managerId, int playerId) {
  std::stringstream q;
  q << "SELECT COALESCE(pss.team_id,p.team_id)"
    << " FROM players p LEFT JOIN player_save_state pss"
    << " ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
    << " WHERE p.id=" << playerId << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str().c_str());
  int clubId = 0;
  if (r && r->data.size() > 0) clubId = atoi(UTCell(r,0,0).c_str());
  if (r) delete r;
  return clubId;
}

static long long GetPlayerCurrentWage(int managerId, int playerId) {
  std::stringstream q;
  q << "SELECT COALESCE(pss.weekly_wage,p.weekly_wage)"
    << " FROM players p LEFT JOIN player_save_state pss"
    << " ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
    << " WHERE p.id=" << playerId << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str().c_str());
  long long wage = 0;
  if (r && r->data.size() > 0) wage = atoll(UTCell(r,0,0).c_str());
  if (r) delete r;
  return wage;
}

static std::string GetPlayerNameUT(int playerId) {
  std::stringstream q;
  q << "SELECT firstname||' '||lastname FROM players WHERE id=" << playerId << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str().c_str());
  std::string name = "Player";
  if (r && r->data.size() > 0 && !UTCell(r,0,0).empty()) name = UTCell(r,0,0);
  if (r) delete r;
  return name;
}

static std::string GetTeamNameUT(int clubId) {
  std::stringstream q;
  q << "SELECT name FROM teams WHERE id=" << clubId << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str().c_str());
  std::string name = "Club";
  if (r && r->data.size() > 0 && !UTCell(r,0,0).empty()) name = UTCell(r,0,0);
  if (r) delete r;
  return name;
}

static std::string UTAddDaysSigned(const std::string &date, int days) {
  std::stringstream q;
  q << "SELECT date('" << UTSql(date) << "', '"
    << (days >= 0 ? "+" : "") << days << " days');";
  DatabaseResult *r = GetDB()->Query(q.str().c_str());
  std::string out = date;
  if (r && r->data.size() > 0 && !UTCell(r,0,0).empty()) out = UTCell(r,0,0);
  if (r) delete r;
  return out;
}

// League-id → home nationality mapping for pref_domestic evaluation.
static std::string LeagueNationality(int leagueId) {
  switch (leagueId) {
    case 1: return "England";
    case 2: return "Germany";
    case 3: return "Netherlands";
    case 4: return "Spain";
    case 9: return "Portugal";
    default: return "";
  }
}

// ---- SeedPlayerPreferences --------------------------------------------------

void SeedPlayerPreferences(int managerId) {
  // Collect per-player data joined with their current club's league
  DatabaseResult *pr = GetDB()->Query(
    "SELECT pt.player_id, pt.ambition, pt.greed, pt.ego, pt.adaptability,"
    " p.age, p.nationality, p.sofifaPotential, p.international_reputation,"
    " p.team_id, t.league_id"
    " FROM player_traits pt"
    " JOIN players p ON p.id = pt.player_id"
    " JOIN teams  t ON t.id = p.team_id"
    " WHERE pt.manager_id = ?;");
  // SQLite via GetDB() doesn't support parameterised queries — rebuild with managerId
  delete pr;
  std::stringstream sq;
  sq << "SELECT pt.player_id, pt.ambition, pt.greed, pt.ego, pt.adaptability,"
     << " p.age, p.nationality, p.sofifaPotential, p.international_reputation,"
     << " p.team_id, t.league_id"
     << " FROM player_traits pt"
     << " JOIN players p ON p.id = pt.player_id"
     << " JOIN teams  t ON t.id = p.team_id"
     << " WHERE pt.manager_id = " << managerId << ";";
  pr = GetDB()->Query(sq.str().c_str());
  if (!pr) return;

  // Collect club league membership to find derby pairs (same league → rival candidate)
  // Build list of (league_id, club_id) for derby assignment
  std::vector<std::pair<int,int>> leagueClubs; // (league_id, club_id)
  {
    DatabaseResult *cr = GetDB()->Query("SELECT id, league_id FROM teams;");
    if (cr) {
      for (unsigned int i = 0; i < cr->data.size(); i++) {
        int cid = atoi(UTCell(cr,i,0).c_str());
        int lid = atoi(UTCell(cr,i,1).c_str());
        leagueClubs.push_back({lid, cid});
      }
      delete cr;
    }
  }

  for (unsigned int i = 0; i < pr->data.size(); i++) {
    int pid      = atoi(UTCell(pr,i,0).c_str());
    int ambition = atoi(UTCell(pr,i,1).c_str());
    int greed    = atoi(UTCell(pr,i,2).c_str());
    int ego      = atoi(UTCell(pr,i,3).c_str());
    int adapt    = atoi(UTCell(pr,i,4).c_str());
    int age      = atoi(UTCell(pr,i,5).c_str());
    std::string nat = UTCell(pr,i,6);
    int pot      = atoi(UTCell(pr,i,7).c_str());
    int intlRep  = atoi(UTCell(pr,i,8).c_str());
    int teamId   = atoi(UTCell(pr,i,9).c_str());
    int leagueId = atoi(UTCell(pr,i,10).c_str());

    unsigned int seed = (unsigned int)(managerId * 7919u ^ pid * 31337u);

    int prefDomestic       = 0;
    int prefPrestige       = 0;
    int prefWages          = 0;
    int prefDevelopment    = 0;
    int prefGuaranteedStarts = 0;
    int hatesRivalClubId   = 0;

    // pref_domestic: nationality matches league home nation AND low adaptability
    if (!nat.empty() && adapt < 35 && LeagueNationality(leagueId) == nat)
      prefDomestic = 1;

    // pref_prestige: ego > 65 AND intl rep >= 3
    if (ego > 65 && intlRep >= 3)
      prefPrestige = 1;

    // pref_wages: high greed
    if (greed > 70)
      prefWages = 1;

    // pref_development: young wonderkid with ambition
    if (age <= 22 && pot >= 80 && ambition > 60)
      prefDevelopment = 1;

    // pref_guaranteed_starts: veteran ego
    if (ego > 60 && age >= 27)
      prefGuaranteedStarts = 1;

    // hates_rival_club_id: 15% chance for players at clubs sharing same league
    if ((seed % 100) < 15) {
      // Find another club in the same league
      std::vector<int> rivals;
      for (auto &lc : leagueClubs) {
        if (lc.first == leagueId && lc.second != teamId)
          rivals.push_back(lc.second);
      }
      if (!rivals.empty()) {
        hatesRivalClubId = rivals[(seed >> 4) % rivals.size()];
      }
    }

    // Only one dominant preference per player (priority order)
    // Multiple can co-exist but only first-tier flags are set
    std::stringstream uq;
    uq << "UPDATE player_traits SET"
       << " pref_domestic=" << prefDomestic
       << ",pref_prestige=" << prefPrestige
       << ",pref_wages=" << prefWages
       << ",pref_development=" << prefDevelopment
       << ",pref_guaranteed_starts=" << prefGuaranteedStarts
       << ",hates_rival_club_id=" << hatesRivalClubId
       << " WHERE manager_id=" << managerId << " AND player_id=" << pid << ";";
    UTExec(uq.str());
  }

  printf("[TRANSFER] SeedPlayerPreferences: manager=%d players=%u\n",
         managerId, (unsigned int)pr->data.size());
  delete pr;
}

// ---- SeedSpecialEvents ------------------------------------------------------

void SeedSpecialEvents(int managerId, int seasonYear, const std::string &currentDate) {
  // Clear any stale events for this manager first
  { std::stringstream q; q << "DELETE FROM transfer_special_events WHERE manager_id=" << managerId << ";";
    UTExec(q.str()); }

  int userClubId = GetManagedClubId(managerId);

  int month = currentDate.size() >= 7 ? atoi(currentDate.substr(5,2).c_str()) : 7;
  // Summer window trigger dates: July 20-28
  std::string summerBase = std::to_string(seasonYear) + "-07-";

  // ---- Galactico: prestige club (intl_prestige >= 8) targets elite player (intl_rep >= 4) ---
  {
    DatabaseResult *pr = GetDB()->Query(
      "SELECT p.id, p.team_id FROM players p"
      " JOIN teams t ON t.id = p.team_id"
      " WHERE p.international_reputation >= 4"
      " AND t.international_prestige <= 6"
      " ORDER BY p.international_reputation DESC, p.base_stat DESC LIMIT 5;");
    std::stringstream bq;
    bq << "SELECT id FROM teams WHERE international_prestige >= 8";
    if (userClubId > 0) bq << " AND id != " << userClubId;
    bq << " LIMIT 3;";
    DatabaseResult *br = GetDB()->Query(bq.str().c_str());
    if (pr && pr->data.size() > 0 && br && br->data.size() > 0) {
      int playerId = atoi(UTCell(pr,0,0).c_str());
      int buyerId  = atoi(UTCell(br,0,0).c_str());
      // Trigger day 20-28 of summer
      unsigned int rng = (unsigned int)(managerId * 13337u ^ playerId * 7u);
      int triggerDay = 20 + (int)(rng % 9);
      char dateBuf[12];
      snprintf(dateBuf, sizeof(dateBuf), "%04d-07-%02d", seasonYear, triggerDay);
      std::stringstream iq;
      iq << "INSERT INTO transfer_special_events(manager_id,event_type,player_id,club_id,trigger_date)"
         << " VALUES(" << managerId << ",'galactico'," << playerId << "," << buyerId << ",'" << dateBuf << "');";
      UTExec(iq.str());
    }
    if (pr) delete pr;
    if (br) delete br;
  }

  // ---- Financial collapse forced sale: high-debt club must sell best player ---
  {
    // Guard: club_finances may not exist yet on very first career seed
    DatabaseResult *te = GetDB()->Query(
      "SELECT name FROM sqlite_master WHERE type='table' AND name='club_finances';");
    bool cfExists = (te && te->data.size() > 0);
    if (te) delete te;
    DatabaseResult *cr = nullptr;
    if (cfExists) {
      std::stringstream cq;
      cq << "SELECT cf.club_id, p.id as player_id"
         << " FROM club_finances cf"
         << " JOIN players p ON p.team_id = cf.club_id"
         << " LEFT JOIN player_market_status pms"
         << "   ON pms.manager_id=" << managerId
         << "  AND pms.player_id=p.id"
         << " WHERE cf.debt_level > cf.transfer_budget * 2.5"
         << " AND COALESCE(pms.status,'normal') != 'transfer_listed'";
      if (userClubId > 0) cq << " AND cf.club_id != " << userClubId;
      cq << " ORDER BY p.base_stat DESC LIMIT 1;";
      cr = GetDB()->Query(cq.str().c_str());
    }
    if (cr && cr->data.size() > 0) {
      int clubId   = atoi(UTCell(cr,0,0).c_str());
      int playerId = atoi(UTCell(cr,0,1).c_str());
      char dateBuf[12];
      snprintf(dateBuf, sizeof(dateBuf), "%04d-07-03", seasonYear);
      std::stringstream iq;
      iq << "INSERT INTO transfer_special_events(manager_id,event_type,player_id,club_id,trigger_date)"
         << " VALUES(" << managerId << ",'collapse_sale'," << playerId << "," << clubId << ",'" << dateBuf << "');";
      UTExec(iq.str());
    }
    if (cr) delete cr;
  }

  // ---- Wonderkid explosion: young high-potential player ---
  {
    DatabaseResult *wr = GetDB()->Query(
      "SELECT p.id, p.team_id FROM players p"
      " WHERE p.age <= 21 AND p.sofifaPotential >= 88"
      " ORDER BY p.sofifaPotential DESC LIMIT 3;");
    if (wr && wr->data.size() > 0) {
      unsigned int rng = (unsigned int)(managerId * 99991u);
      int idx = (int)(rng % wr->data.size());
      int playerId = atoi(UTCell(wr,idx,0).c_str());
      int teamId   = atoi(UTCell(wr,idx,1).c_str());
      int triggerDay = 5 + (int)(rng % 11);
      char dateBuf[12];
      snprintf(dateBuf, sizeof(dateBuf), "%04d-07-%02d", seasonYear, triggerDay);
      std::stringstream iq;
      iq << "INSERT INTO transfer_special_events(manager_id,event_type,player_id,club_id,trigger_date)"
         << " VALUES(" << managerId << ",'wonderkid_explosion'," << playerId << "," << teamId << ",'" << dateBuf << "');";
      UTExec(iq.str());
    }
    if (wr) delete wr;
  }

  printf("[TRANSFER] SeedSpecialEvents: manager=%d season=%d\n", managerId, seasonYear);
}

// ---- ProcessSpecialEvents ---------------------------------------------------

void ProcessSpecialEvents(int managerId, int userClubId,
                          const std::string &currentDate, int seasonYear) {
  std::stringstream sq;
  sq << "SELECT id, event_type, player_id, club_id"
     << " FROM transfer_special_events"
     << " WHERE manager_id=" << managerId
     << " AND fired=0 AND trigger_date<='" << currentDate << "';";
  DatabaseResult *er = GetDB()->Query(sq.str().c_str());
  if (!er) return;

  struct EvRow { int id, playerId, clubId; std::string type; };
  std::vector<EvRow> events;
  for (unsigned int i = 0; i < er->data.size(); i++) {
    EvRow e;
    e.id       = atoi(UTCell(er,i,0).c_str());
    e.type     = UTCell(er,i,1);
    e.playerId = atoi(UTCell(er,i,2).c_str());
    e.clubId   = atoi(UTCell(er,i,3).c_str());
    events.push_back(e);
  }
  delete er;

  for (auto &ev : events) {
    if (ev.type == "galactico") {
      // Get player value and selling club
      long long playerVal = 0;
      int sellerClubId = 0;
      int curWage = 0;
      {
        std::stringstream pq;
        pq << "SELECT playervalue, team_id, weekly_wage FROM players WHERE id=" << ev.playerId << ";";
        DatabaseResult *pr = GetDB()->Query(pq.str().c_str());
        if (pr && pr->data.size() > 0) {
          playerVal    = atoll(UTCell(pr,0,0).c_str());
          sellerClubId = atoi(UTCell(pr,0,1).c_str());
          curWage      = atoi(UTCell(pr,0,2).c_str());
        }
        if (pr) delete pr;
      }
      if (sellerClubId == 0 || ev.clubId == sellerClubId) { goto mark_fired; }
      if (ev.clubId == userClubId) { goto mark_fired; }

      // Inject forced negotiation with extreme values
      {
        long long galFee  = (long long)(playerVal * 2.2);
        int galWage = (int)(curWage * 2.5);
        std::stringstream iq;
        iq << "INSERT INTO transfer_negotiations("
           << "manager_id,buying_club_id,selling_club_id,player_id,state,"
           << "offered_fee,offered_wage,promised_role,days_in_state,initiated_date,"
           << "deadline_pressure,agent_pressure,irrationality_driven,tier,"
           << "is_user_bid,seller_patience_days,player_patience_days,negotiation_momentum)"
           << " VALUES(" << managerId << "," << ev.clubId << "," << sellerClubId << ","
           << ev.playerId << ",'initiated',"
           << galFee << "," << galWage << ",'star_player',0,'" << currentDate << "',"
           << "90,60,1,1,0,1,1,50);";
        UTExec(iq.str());
        // Temporarily boost seller patience and selling_pressure for this deal
        std::stringstream usq;
        usq << "UPDATE club_transfer_identity SET selling_pressure=85"
            << " WHERE manager_id=" << managerId << " AND club_id=" << sellerClubId << ";";
        UTExec(usq.str());
        std::string pname;
        { std::stringstream pnq; pnq << "SELECT firstname||' '||lastname FROM players WHERE id=" << ev.playerId << ";";
          DatabaseResult *pnr = GetDB()->Query(pnq.str().c_str());
          if (pnr && pnr->data.size() > 0) pname = UTCell(pnr,0,0);
          if (pnr) delete pnr; }
        std::string cname;
        { std::stringstream cnq; cnq << "SELECT name FROM teams WHERE id=" << ev.clubId << ";";
          DatabaseResult *cnr = GetDB()->Query(cnq.str().c_str());
          if (cnr && cnr->data.size() > 0) cname = UTCell(cnr,0,0);
          if (cnr) delete cnr; }
        InsertTransferNews(managerId, currentDate,
          cname + " make shock move for " + pname,
          "rumour", ev.playerId, sellerClubId, ev.clubId);
      }

    } else if (ev.type == "collapse_sale") {
      if (ev.clubId == userClubId) { goto mark_fired; }
      // Force-list the player at 70% value
      {
        std::stringstream pq;
        pq << "SELECT playervalue FROM players WHERE id=" << ev.playerId << ";";
        DatabaseResult *pr = GetDB()->Query(pq.str().c_str());
        long long pval = 0;
        if (pr && pr->data.size() > 0) pval = atoll(UTCell(pr,0,0).c_str());
        if (pr) delete pr;
        long long saleVal = (long long)(pval * 0.70);
        std::stringstream uq;
        uq << "INSERT OR REPLACE INTO player_market_status(manager_id,player_id,status,set_date,asking_price)"
           << " VALUES(" << managerId << "," << ev.playerId << ",'transfer_listed','"
           << currentDate << "'," << saleVal << ");";
        UTExec(uq.str());
        // Bump seller's selling_pressure
        std::stringstream usq;
        usq << "UPDATE club_transfer_identity SET selling_pressure=90"
            << " WHERE manager_id=" << managerId << " AND club_id=" << ev.clubId << ";";
        UTExec(usq.str());
        std::string pname;
        { std::stringstream pnq; pnq << "SELECT firstname||' '||lastname FROM players WHERE id=" << ev.playerId << ";";
          DatabaseResult *pnr = GetDB()->Query(pnq.str().c_str());
          if (pnr && pnr->data.size() > 0) pname = UTCell(pnr,0,0);
          if (pnr) delete pnr; }
        std::string cname;
        { std::stringstream cnq; cnq << "SELECT name FROM teams WHERE id=" << ev.clubId << ";";
          DatabaseResult *cnr = GetDB()->Query(cnq.str().c_str());
          if (cnr && cnr->data.size() > 0) cname = UTCell(cnr,0,0);
          if (cnr) delete cnr; }
        InsertTransferNews(managerId, currentDate,
          cname + " forced to sell " + pname + " amid financial crisis",
          "completed", ev.playerId, ev.clubId, 0);
      }

    } else if (ev.type == "wonderkid_explosion") {
      // Raise rep globally, but keep this save's valuation hype in the
      // manager-scoped market table so separate careers do not inherit it.
      {
        long long newValue = 0;
        {
          std::stringstream vq;
          vq << "SELECT CAST(playervalue * 1.4 AS INTEGER) FROM players WHERE id=" << ev.playerId << ";";
          DatabaseResult *vr = GetDB()->Query(vq.str().c_str());
          if (vr && vr->data.size() > 0) newValue = atoll(UTCell(vr,0,0).c_str());
          if (vr) delete vr;
        }
        std::stringstream uq;
        uq << "UPDATE players SET"
           << " international_reputation = MIN(5, international_reputation+1)"
           << " WHERE id=" << ev.playerId << ";";
        UTExec(uq.str());
        uq.str("");
        uq << "INSERT INTO player_market_status(manager_id,player_id,status,set_date,asking_price)"
           << " VALUES(" << managerId << "," << ev.playerId << ",'wonderkid','"
           << currentDate << "'," << newValue << ")"
           << " ON CONFLICT(manager_id,player_id) DO UPDATE SET"
           << " status='wonderkid',set_date=excluded.set_date,asking_price=excluded.asking_price;";
        UTExec(uq.str());
        // Bump knowledge for all clubs that know this player
        std::stringstream kq;
        kq << "UPDATE club_player_knowledge SET knowledge=MAX(knowledge,80)"
           << " WHERE manager_id=" << managerId << " AND player_id=" << ev.playerId << ";";
        UTExec(kq.str());
        std::string pname;
        { std::stringstream pnq; pnq << "SELECT firstname||' '||lastname, age FROM players WHERE id=" << ev.playerId << ";";
          DatabaseResult *pnr = GetDB()->Query(pnq.str().c_str());
          if (pnr && pnr->data.size() > 0) pname = UTCell(pnr,0,0);
          if (pnr) delete pnr; }
        InsertTransferNews(managerId, currentDate,
          pname + " named in national squad — clubs circle the young star",
          "rumour", ev.playerId, ev.clubId, 0);
      }
    }

    mark_fired:
    { std::stringstream fq; fq << "UPDATE transfer_special_events SET fired=1 WHERE id=" << ev.id << ";";
      UTExec(fq.str()); }
  }
}

// ---- TickUserNegotiations ---------------------------------------------------
// Runs once per day for all is_user_bid=1 negotiations. Mirrors AdvanceNegotiations
// but applies player preferences, counter-offer psychology, hijack logic, and momentum.

void TickUserNegotiations(int managerId, int userClubId,
                           const std::string &currentDate, int seasonYear) {
  // Load user bids
  std::stringstream nq;
  nq << "SELECT id,buying_club_id,selling_club_id,player_id,state,"
     << "offered_fee,offered_wage,promised_role,days_in_state,"
     << "deadline_pressure,acceptance_score,counter_offer_count,"
     << "user_pending_action,negotiation_momentum,"
     << "seller_patience_days,player_patience_days,tier"
     << " FROM transfer_negotiations"
     << " WHERE manager_id=" << managerId
     << " AND is_user_bid=1"
     << " AND state NOT IN ('completed','collapsed');";
  DatabaseResult *nr = GetDB()->Query(nq.str().c_str());
  if (!nr) return;

  struct UNeg {
    int id, buyer, seller, player, days, dlPressure, acceptScore, coCount;
    int momentum, sellerPat, playerPat, tier;
    std::string state, offFee, offWage, role, pendingAction;
  };
  std::vector<UNeg> rows;
  for (unsigned int i = 0; i < nr->data.size(); i++) {
    UNeg n;
    n.id           = atoi(UTCell(nr,i,0).c_str());
    n.buyer        = atoi(UTCell(nr,i,1).c_str());
    n.seller       = atoi(UTCell(nr,i,2).c_str());
    n.player       = atoi(UTCell(nr,i,3).c_str());
    n.state        = UTCell(nr,i,4);
    n.offFee       = UTCell(nr,i,5);
    n.offWage      = UTCell(nr,i,6);
    n.role         = UTCell(nr,i,7);
    n.days         = atoi(UTCell(nr,i,8).c_str());
    n.dlPressure   = atoi(UTCell(nr,i,9).c_str());
    n.acceptScore  = atoi(UTCell(nr,i,10).c_str());
    n.coCount      = atoi(UTCell(nr,i,11).c_str());
    n.pendingAction= UTCell(nr,i,12);
    n.momentum     = atoi(UTCell(nr,i,13).c_str());
    n.sellerPat    = atoi(UTCell(nr,i,14).c_str());
    n.playerPat    = atoi(UTCell(nr,i,15).c_str());
    n.tier         = atoi(UTCell(nr,i,16).c_str());
    rows.push_back(n);
  }
  delete nr;

  bool deadlineDay = DaysToWindowEnd(currentDate) <= 2 && InTransferWindow(currentDate);

  for (auto &n : rows) {
    unsigned int rng = (unsigned int)(managerId*7919u ^ n.buyer*31337u ^ n.player*1999u
                                      ^ (unsigned int)DateToJulian(currentDate));
    std::string newState = n.state;
    std::string collapseReason;
    int newMomentum = n.momentum;
    int newDays = n.days + 1;

    // ---- Pending user action from UI (set by RespondToOffer) ----------------
    if (!n.pendingAction.empty() && n.pendingAction != "") {
      if (n.pendingAction == "accept_counter") {
        // User accepted the counter — advance to player_talks
        newState = "player_talks";
        newMomentum += 10;
      } else if (n.pendingAction == "reject") {
        newState = "collapsed";
        collapseReason = "user_rejected";
      } else if (n.pendingAction == "withdraw") {
        newState = "collapsed";
        collapseReason = "user_withdrew";
      }
      // Clear the action
      { std::stringstream cq;
        cq << "UPDATE transfer_negotiations SET user_pending_action='' WHERE id=" << n.id << ";";
        UTExec(cq.str()); }
      if (!newState.empty() && newState != n.state) goto write_state;
    }

    // ---- State machine ------------------------------------------------------

    if (n.state == "initiated") {
      // Day 1-2: seller evaluates the bid
      if (n.days < 1) { goto write_state; } // wait one day

      long long fee  = atoll(n.offFee.c_str());
      long long wage = atoll(n.offWage.c_str());

      // Get contextual value and seller selling_pressure
      long long ctxVal = CalculateContextualValue(managerId, n.player, n.seller, n.buyer,
                                                    50, n.dlPressure, 0);
      int sellingPressure = 20;
      std::string negotiationPersonality = "patient";
      {
        std::stringstream idq;
        idq << "SELECT selling_pressure, negotiation_personality FROM club_transfer_identity"
            << " WHERE manager_id=" << managerId << " AND club_id=" << n.seller << ";";
        DatabaseResult *idr = GetDB()->Query(idq.str().c_str());
        if (idr && idr->data.size() > 0) {
          sellingPressure = atoi(UTCell(idr,0,0).c_str());
          negotiationPersonality = UTCell(idr,0,1);
        }
        if (idr) delete idr;
      }

      // Check if player is franchise (untouchable)
      {
        std::stringstream fq;
        fq << "SELECT status FROM player_market_status WHERE manager_id=" << managerId
           << " AND player_id=" << n.player << ";";
        DatabaseResult *fr = GetDB()->Query(fq.str().c_str());
        bool isFranchise = (fr && fr->data.size() > 0 && UTCell(fr,0,0) == "franchise_player");
        if (fr) delete fr;
        if (isFranchise && sellingPressure < 30) {
          newState = "collapsed";
          collapseReason = "not_for_sale";
          goto write_state;
        }
      }

      // Evaluate bid strength
      double feeRatio = ctxVal > 0 ? (double)fee / (double)ctxVal : 0.0;
      if (feeRatio >= 0.90 || sellingPressure > 60) {
        newState = "player_talks";
        newMomentum += 10;
        InsertInboxMessage(managerId, "Talks progressing over bid",
          "Your bid is under consideration. The selling club is open to discussing terms.",
          "transfer", currentDate);
      } else if (feeRatio >= 0.60) {
        // Counter-offer — apply personality
        long long counterFee = ctxVal; // ask for full value by default
        if (negotiationPersonality == "desperate" && sellingPressure > 70)
          counterFee = (long long)(ctxVal * 0.88);
        else if (negotiationPersonality == "fast_closer")
          counterFee = (long long)(ctxVal * 0.90);
        else if (negotiationPersonality == "hard_negotiator")
          counterFee = (long long)(ctxVal * 1.02);
        newState = "counter_offer";
        newMomentum -= 15;
        // Store counter fee as agent_pressure field temporarily (repurposed as counter_fee_store)
        // and fire inbox via transfer_news
        std::stringstream clfq;
        clfq << "UPDATE transfer_negotiations SET"
             << " offered_fee=" << counterFee  // store counter as new "offered" so UI can read it
             << ",counter_offer_count=" << (n.coCount+1)
             << " WHERE id=" << n.id << ";";
        UTExec(clfq.str());
        InsertInboxMessage(managerId, "Counter-offer received",
          "The selling club has responded with a counter-offer. Review it in your Transfers screen.",
          "transfer", currentDate);
      } else {
        newState = "collapsed";
        collapseReason = "bid_rejected";
      }

    } else if (n.state == "player_talks") {
      // Personal terms: compute acceptance score with preference modifiers
      long long wage = atoll(n.offWage.c_str());

      // Player patience deadline
      if (n.days >= n.playerPat) {
        newState = "collapsed";
        collapseReason = "player_walked";
        goto write_state;
      }

      int score = CalculateAcceptanceScore(managerId, n.player, n.buyer,
                                            n.seller, (long long)atoll(n.offWage.c_str()),
                                            n.role, 50);

      // Apply preference profile modifiers
      {
        std::stringstream pq;
        pq << "SELECT pref_domestic, pref_prestige, pref_wages, pref_development,"
           << " pref_guaranteed_starts, hates_rival_club_id"
           << " FROM player_traits WHERE manager_id=" << managerId << " AND player_id=" << n.player << ";";
        DatabaseResult *pfr = GetDB()->Query(pq.str().c_str());
        if (pfr && pfr->data.size() > 0) {
          int pDom   = atoi(UTCell(pfr,0,0).c_str());
          int pPres  = atoi(UTCell(pfr,0,1).c_str());
          int pWages = atoi(UTCell(pfr,0,2).c_str());
          int pDev   = atoi(UTCell(pfr,0,3).c_str());
          int pGuar  = atoi(UTCell(pfr,0,4).c_str());
          int hates  = atoi(UTCell(pfr,0,5).c_str());

          // pref_domestic: get buying club's league nationality vs player nationality
          if (pDom) {
            int buyLeague = 0;
            { std::stringstream lq; lq << "SELECT league_id FROM teams WHERE id=" << n.buyer << ";";
              DatabaseResult *lr = GetDB()->Query(lq.str().c_str());
              if (lr && lr->data.size() > 0) buyLeague = atoi(UTCell(lr,0,0).c_str());
              if (lr) delete lr; }
            std::string pnat;
            { std::stringstream natq; natq << "SELECT nationality FROM players WHERE id=" << n.player << ";";
              DatabaseResult *natr = GetDB()->Query(natq.str().c_str());
              if (natr && natr->data.size() > 0) pnat = UTCell(natr,0,0);
              if (natr) delete natr; }
            if (!pnat.empty() && LeagueNationality(buyLeague) != pnat)
              score -= 20;
          }
          if (pPres) score = (int)(score * 0.85); // prestige weight doubled internally — reduce if poor fit
          if (pWages) score = score; // wage weight already in base score
          if (pGuar && (n.role == "rotation" || n.role == "prospect")) score = 0;
          if (hates == n.buyer) score -= 30;
          score = std::max(0, std::min(100, score));
        }
        if (pfr) delete pfr;
      }

      // Momentum bonus
      if (n.momentum > 50) score += 10;
      score = std::max(0, std::min(100, score));

      printf("[TRANSFER] player_talks score=%d nid=%d coCount=%d\n", score, n.id, n.coCount);
      if (score > 50) {
        newState = "medical_pending";
        newMomentum += 8;
        { std::stringstream uq;
          uq << "UPDATE transfer_negotiations SET acceptance_score=" << score << " WHERE id=" << n.id << ";";
          UTExec(uq.str()); }
        InsertInboxMessage(managerId, "Player happy to join — medical arranged",
          "The player has agreed personal terms. A medical is being arranged to complete the deal.",
          "transfer", currentDate);
      } else if (score >= 33 && n.coCount < 2) {
        // Player wants improved terms — wait for user to improve via UI
        // Don't collapse yet; increment counter so UI knows to show "improve" prompt
        { std::stringstream uq;
          uq << "UPDATE transfer_negotiations SET acceptance_score=" << score
             << ",counter_offer_count=" << (n.coCount+1) << " WHERE id=" << n.id << ";";
          UTExec(uq.str()); }
        InsertInboxMessage(managerId, "Player requesting improved terms",
          "The player is not satisfied with the current offer. Consider increasing the wage or improving the promised role.",
          "transfer", currentDate);
      } else {
        newState = "collapsed";
        collapseReason = "player_rejected";
      }

    } else if (n.state == "counter_offer") {
      // User has received a counter — waiting for their response (set via RespondToOffer)
      // Check seller patience deadline
      if (n.days >= n.sellerPat) {
        newState = "collapsed";
        collapseReason = "seller_withdrew";
        InsertInboxMessage(managerId, "Selling club withdrew from negotiations",
          "The seller has pulled out of talks after your bid did not meet their deadline.",
          "transfer", currentDate);
      } else {
        // Check fast_closer personality deadline (day 3 = final offer warning)
        std::string personality = "patient";
        { std::stringstream pq; pq << "SELECT negotiation_personality FROM club_transfer_identity"
            << " WHERE manager_id=" << managerId << " AND club_id=" << n.seller << ";";
          DatabaseResult *pr = GetDB()->Query(pq.str().c_str());
          if (pr && pr->data.size() > 0) personality = UTCell(pr,0,0);
          if (pr) delete pr; }
        if (personality == "fast_closer" && n.days == 3) {
          InsertInboxMessage(managerId, "Seller issues final offer deadline",
            "The selling club has set a deadline. Accept or improve the offer soon or talks will collapse.",
            "transfer", currentDate);
        }
        newMomentum -= 8; // stalling
      }

    } else if (n.state == "medical_pending") {
      // 2% normal fail, 5% deadline day
      int failChance = deadlineDay ? 5 : 2;
      if ((int)(rng % 100) < failChance) {
        newState = "collapsed";
        collapseReason = "medical_failed";
        newMomentum -= 50;
        InsertInboxMessage(managerId, "Deal collapsed — player failed medical",
          "The transfer has fallen through after the player did not pass their medical examination.",
          "transfer", currentDate);
      } else {
        // Complete the deal
        newState = "completed";
        SetPlayerSaveState(managerId, n.player, n.buyer, atoll(n.offWage.c_str()));
        if (n.buyer == userClubId) RemovePlayerScoutingRecords(managerId, n.player);
        // Deduct fee from buyer budget
        { std::stringstream uq;
          uq << "UPDATE club_finances SET transfer_budget=MAX(0,transfer_budget-" << n.offFee << ")"
             << " WHERE manager_id=" << managerId << " AND club_id=" << n.buyer << ";";
          UTExec(uq.str()); }
        // Add fee to seller income
        { std::stringstream uq;
          uq << "UPDATE club_finances SET cash_balance=cash_balance+" << n.offFee
             << " WHERE manager_id=" << managerId << " AND club_id=" << n.seller << ";";
          UTExec(uq.str()); }

        std::string pname, buyerName, sellerName;
        { std::stringstream pnq; pnq << "SELECT firstname||' '||lastname FROM players WHERE id=" << n.player << ";";
          DatabaseResult *pnr = GetDB()->Query(pnq.str().c_str());
          if (pnr && pnr->data.size() > 0) pname = UTCell(pnr,0,0);
          if (pnr) delete pnr; }
        { std::stringstream tq; tq << "SELECT name FROM teams WHERE id=" << n.buyer << ";";
          DatabaseResult *tr = GetDB()->Query(tq.str().c_str());
          if (tr && tr->data.size() > 0) buyerName = UTCell(tr,0,0);
          if (tr) delete tr; }
        { std::stringstream tq; tq << "SELECT name FROM teams WHERE id=" << n.seller << ";";
          DatabaseResult *tr = GetDB()->Query(tq.str().c_str());
          if (tr && tr->data.size() > 0) sellerName = UTCell(tr,0,0);
          if (tr) delete tr; }
        InsertFinanceTransaction(managerId, n.buyer, currentDate, "transfer",
          "Transfer fee paid: " + pname + " from " + sellerName, -atoll(n.offFee.c_str()));
        InsertFinanceTransaction(managerId, n.seller, currentDate, "transfer",
          "Transfer fee received: " + pname + " to " + buyerName, atoll(n.offFee.c_str()));
        InsertTransferNews(managerId, currentDate,
          pname + " joins " + buyerName + " from " + sellerName, "completed", n.player, n.seller, n.buyer);
        InsertInboxMessage(managerId, pname + " transfer complete",
          "The deal is done. " + pname + " has joined " + buyerName + ".",
          "transfer", currentDate);

        // If user is buyer, handle squad harmony boost
        if (n.buyer == userClubId) {
          ProcessSquadHarmonyOnBuy(managerId, n.player, currentDate);
        }
        // If user is seller, handle squad harmony on sale
        if (n.seller == userClubId) {
          ProcessSquadHarmonyOnSale(managerId, n.player, userClubId, currentDate);
          // Set succession_role
          std::string pRole;
          { std::stringstream rq; rq << "SELECT role FROM players WHERE id=" << n.player << ";";
            DatabaseResult *rr = GetDB()->Query(rq.str().c_str());
            if (rr && rr->data.size() > 0) pRole = RoleToGroup(UTCell(rr,0,0));
            if (rr) delete rr; }
          int pStat = 0;
          { std::stringstream sq2; sq2 << "SELECT base_stat FROM players WHERE id=" << n.player << ";";
            DatabaseResult *sr = GetDB()->Query(sq2.str().c_str());
            if (sr && sr->data.size() > 0) pStat = atoi(UTCell(sr,0,0).c_str());
            if (sr) delete sr; }
          if (pStat >= 75) {
            std::stringstream sucq;
            sucq << "UPDATE club_transfer_identity SET succession_role='" << pRole << "'"
                 << " WHERE manager_id=" << managerId << " AND club_id=" << n.seller << ";";
            UTExec(sucq.str());
          }
        }
      }
    }

    // ---- Check hijack opportunity during counter_offer / medical_pending ----
    if (newState == "counter_offer" || newState == "medical_pending") {
      int hijackChance = (newState == "medical_pending") ? 25 : 15;
      if (deadlineDay) hijackChance = 40;
      if ((int)(rng % 100) < hijackChance) {
        // Find a competing club with knowledge >= 50
        std::stringstream hq;
        hq << "SELECT cpk.club_id FROM club_player_knowledge cpk"
           << " WHERE cpk.manager_id=" << managerId
           << " AND cpk.player_id=" << n.player
           << " AND cpk.knowledge >= 50"
           << " AND cpk.club_id != " << n.buyer
           << " AND cpk.club_id != " << n.seller
           << " LIMIT 5;";
        DatabaseResult *hr = GetDB()->Query(hq.str().c_str());
        if (hr && hr->data.size() > 0) {
          int hIdx = (int)(rng % hr->data.size());
          int competitor = atoi(UTCell(hr,hIdx,0).c_str());
          // Check cooldown
          bool onCooldown = false;
          { std::stringstream cq; cq << "SELECT id FROM negotiation_cooldowns"
              << " WHERE manager_id=" << managerId << " AND buying_club_id=" << competitor
              << " AND player_id=" << n.player << " AND cooldown_until>'" << currentDate << "';";
            DatabaseResult *cr = GetDB()->Query(cq.str().c_str());
            onCooldown = (cr && cr->data.size() > 0);
            if (cr) delete cr; }
          if (!onCooldown) {
            // Fire competing bid event
            std::stringstream uq;
            uq << "UPDATE transfer_negotiations SET competing_bid_club_id=" << competitor << " WHERE id=" << n.id << ";";
            UTExec(uq.str());
            newMomentum -= 25;
            std::string cname;
            { std::stringstream cnq; cnq << "SELECT name FROM teams WHERE id=" << competitor << ";";
              DatabaseResult *cnr = GetDB()->Query(cnq.str().c_str());
              if (cnr && cnr->data.size() > 0) cname = UTCell(cnr,0,0);
              if (cnr) delete cnr; }
            InsertInboxMessage(managerId, cname + " enter race for your target",
              cname + " are showing interest in the same player. You may need to improve your offer if they make a formal bid.",
              "transfer", currentDate);
          }
        }
        if (hr) delete hr;
      }
    }

    // ---- Collapse from momentum < -60 ---------------------------------------
    if (newMomentum < -60 && newState != "completed" && newState != "collapsed") {
      if ((int)(rng % 100) < 30) {
        newState = "collapsed";
        collapseReason = "deal_collapsed";
      }
    }

    write_state:
    // Clamp momentum
    newMomentum = std::max(-100, std::min(100, newMomentum));

    // Fire inbox for collapse cases not already individually handled
    if (newState == "collapsed" && n.state != "collapsed") {
      if (collapseReason == "bid_rejected") {
        InsertInboxMessage(managerId, "Bid rejected — offer too low",
          "The selling club rejected your bid as insufficient. Consider raising the offer or moving on.",
          "transfer", currentDate);
      } else if (collapseReason == "not_for_sale") {
        InsertInboxMessage(managerId, "Player not for sale",
          "The club has no intention of selling this player at this time.",
          "transfer", currentDate);
      } else if (collapseReason == "player_walked") {
        InsertInboxMessage(managerId, "Player ended negotiations",
          "The player chose not to wait any longer and has walked away from the deal.",
          "transfer", currentDate);
      } else if (collapseReason == "player_rejected") {
        InsertInboxMessage(managerId, "Player rejected your terms",
          "The player was not satisfied with your wage offer or the promised role.",
          "transfer", currentDate);
      } else if (collapseReason == "deal_collapsed") {
        InsertInboxMessage(managerId, "Transfer deal collapsed",
          "Negotiations broke down. The deal has fallen apart due to deteriorating momentum.",
          "transfer", currentDate);
      } else if (collapseReason == "user_rejected") {
        InsertInboxMessage(managerId, "You rejected the counter-offer",
          "You have rejected the selling club's counter-offer. The deal is now off.",
          "transfer", currentDate);
      }
    }

    std::stringstream wq;
    wq << "UPDATE transfer_negotiations SET"
       << " state='" << newState << "'"
       << ",days_in_state=" << newDays
       << ",negotiation_momentum=" << newMomentum;
    if (!collapseReason.empty())
      wq << ",collapse_reason='" << collapseReason << "'";
    wq << " WHERE id=" << n.id << ";";
    UTExec(wq.str());
  }
}

// ---- RespondToOffer ---------------------------------------------------------
// Called by UI when user presses Accept / Reject / Counter on an incoming bid.
// For user buy-side: action = "accept_counter", "reject", "counter", "withdraw"
// For user sell-side: action = "accept", "reject", "counter", "block"

void RespondToOffer(int managerId, int negotiationId, const std::string &action,
                     int counterFee, int counterWage) {
  int isUserBid = 1;
  int buyerId = 0;
  int sellerId = 0;
  int playerId = 0;
  std::string currentDate;
  {
    std::stringstream q;
    q << "SELECT is_user_bid,buying_club_id,selling_club_id,player_id"
      << " FROM transfer_negotiations"
      << " WHERE id=" << negotiationId << " AND manager_id=" << managerId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str().c_str());
    if (r && r->data.size() > 0) {
      isUserBid = atoi(UTCell(r,0,0).c_str());
      buyerId   = atoi(UTCell(r,0,1).c_str());
      sellerId  = atoi(UTCell(r,0,2).c_str());
      playerId  = atoi(UTCell(r,0,3).c_str());
    }
    if (r) delete r;
  }
  {
    std::stringstream dq;
    dq << "SELECT current_date FROM managers WHERE id=" << managerId << " LIMIT 1;";
    DatabaseResult *dr = GetDB()->Query(dq.str().c_str());
    if (dr && dr->data.size() > 0) currentDate = UTCell(dr,0,0);
    if (dr) delete dr;
  }

  if (!isUserBid) {
    if (action == "reject") {
      std::stringstream uq;
      uq << "UPDATE transfer_negotiations SET state='collapsed',"
         << "collapse_reason='user_rejected',days_in_state=0"
         << " WHERE id=" << negotiationId << " AND manager_id=" << managerId << ";";
      UTExec(uq.str());
      InsertTransferNews(managerId, currentDate, "Incoming transfer offer rejected",
                         "incoming_rejected", playerId, sellerId, buyerId);
      return;
    }

    if (action == "counter" && counterFee > 0) {
      std::stringstream uq;
      uq << "UPDATE transfer_negotiations SET offered_fee=" << counterFee
         << ",state='negotiating',seller_approved=1,days_in_state=0,"
         << "counter_offer_count=counter_offer_count+1"
         << " WHERE id=" << negotiationId << " AND manager_id=" << managerId << ";";
      UTExec(uq.str());
      return;
    }
  }

  if (action == "counter" && counterFee > 0) {
    // User sends a counter: update the offered_fee and clear counter state back to negotiating
    std::stringstream uq;
    uq << "UPDATE transfer_negotiations SET"
       << " offered_fee=" << counterFee;
    if (counterWage > 0) uq << ",offered_wage=" << counterWage;
    uq << ",state='negotiating'"
       << ",user_pending_action=''"
       << ",days_in_state=0"
       << " WHERE id=" << negotiationId << " AND manager_id=" << managerId << ";";
    UTExec(uq.str());
  } else {
    // Store action for TickUserNegotiations to process next day
    std::stringstream uq;
    uq << "UPDATE transfer_negotiations SET user_pending_action='" << action << "'"
       << " WHERE id=" << negotiationId << " AND manager_id=" << managerId << ";";
    UTExec(uq.str());
  }
}

// ---- InitiateUserBid --------------------------------------------------------

void InitiateUserBid(int managerId, int userClubId, int playerId, int sellingClubId,
                      int offeredFee, int offeredWage, const std::string &promisedRole,
                      int sellOnPct, int loanBackMonths,
                      const std::string &currentDate, int seasonYear) {
  // Check for existing active bid on this player
  {
    std::stringstream eq;
    eq << "SELECT id FROM transfer_negotiations"
       << " WHERE manager_id=" << managerId
       << " AND buying_club_id=" << userClubId
       << " AND player_id=" << playerId
       << " AND is_user_bid=1"
       << " AND state NOT IN ('completed','collapsed') LIMIT 1;";
    DatabaseResult *er = GetDB()->Query(eq.str().c_str());
    bool exists = (er && er->data.size() > 0);
    if (er) delete er;
    if (exists) return; // Already bidding
  }

  // Check budget
  long long budget = 0;
  { std::stringstream bq; bq << "SELECT transfer_budget FROM club_finances WHERE manager_id=" << managerId
      << " AND club_id=" << userClubId << ";";
    DatabaseResult *br = GetDB()->Query(bq.str().c_str());
    if (br && br->data.size() > 0) budget = atoll(UTCell(br,0,0).c_str());
    if (br) delete br; }
  if (offeredFee > budget) {
    InsertInboxMessage(managerId, "Bid exceeds available budget",
      "Your transfer offer exceeds the current transfer budget. The bid has been submitted but funds may need to be reviewed.",
      "transfer", currentDate);
  }

  int sellerPat = 7;
  // Modulate seller patience by deadline
  int daysLeft = DaysToWindowEnd(currentDate);
  if (daysLeft <= 2) sellerPat = 2;
  else if (daysLeft <= 7) sellerPat = 4;

  std::stringstream iq;
  iq << "INSERT INTO transfer_negotiations("
     << "manager_id,buying_club_id,selling_club_id,player_id,state,"
     << "offered_fee,offered_wage,promised_role,days_in_state,initiated_date,"
     << "deadline_pressure,is_user_bid,seller_patience_days,player_patience_days,"
     << "negotiation_momentum,tier)"
     << " VALUES(" << managerId << "," << userClubId << "," << sellingClubId << ","
     << playerId << ",'initiated',"
     << offeredFee << "," << offeredWage << ",'" << promisedRole << "',0,'"
     << currentDate << "',"
     << (InTransferWindow(currentDate) ? std::max(0,100-daysLeft*5) : 0)
     << ",1," << sellerPat << ",5,0,2);";
  UTExec(iq.str());

  std::string pname;
  { std::stringstream pnq; pnq << "SELECT firstname||' '||lastname FROM players WHERE id=" << playerId << ";";
    DatabaseResult *pnr = GetDB()->Query(pnq.str().c_str());
    if (pnr && pnr->data.size() > 0) pname = UTCell(pnr,0,0);
    if (pnr) delete pnr; }
  InsertInboxMessage(managerId, "Bid submitted for " + pname,
    "Your transfer bid has been sent to the selling club. Check My Bids for updates.",
    "transfer", currentDate);
}

// ---- OfferLoan --------------------------------------------------------------

static void ActivateLoanDeal(int managerId, int loanDealId,
                             const std::string &currentDate) {
  std::stringstream sq;
  sq << "SELECT loaning_club_id,receiving_club_id,player_id,loan_fee,"
     << " monthly_wage_receiving_pct,end_date"
     << " FROM loan_deals WHERE manager_id=" << managerId
     << " AND id=" << loanDealId << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(sq.str().c_str());
  if (!r || r->data.empty()) { if (r) delete r; return; }

  int loaningClub = atoi(UTCell(r,0,0).c_str());
  int receivingClub = atoi(UTCell(r,0,1).c_str());
  int playerId = atoi(UTCell(r,0,2).c_str());
  long long loanFee = atoll(UTCell(r,0,3).c_str());
  int wagePct = atoi(UTCell(r,0,4).c_str());
  std::string endDate = UTCell(r,0,5);
  delete r;

  if (loaningClub <= 0 || receivingClub <= 0 || playerId <= 0) return;
  if (GetPlayerCurrentClub(managerId, playerId) != loaningClub) {
    std::stringstream cq;
    cq << "UPDATE loan_deals SET status='collapsed',collapse_reason='player_not_at_parent'"
       << " WHERE id=" << loanDealId << ";";
    UTExec(cq.str());
    return;
  }

  long long wage = GetPlayerCurrentWage(managerId, playerId);
  SetPlayerSaveState(managerId, playerId, receivingClub, wage);

  if (loanFee > 0) {
    std::stringstream bq;
    bq << "UPDATE club_finances SET transfer_budget=MAX(0,transfer_budget-" << loanFee << ")"
       << " WHERE manager_id=" << managerId << " AND club_id=" << receivingClub << ";";
    UTExec(bq.str());
    std::stringstream sq2;
    sq2 << "UPDATE club_finances SET cash_balance=cash_balance+" << loanFee
        << " WHERE manager_id=" << managerId << " AND club_id=" << loaningClub << ";";
    UTExec(sq2.str());
    InsertFinanceTransaction(managerId, receivingClub, currentDate, "transfer",
      "Loan fee paid: " + GetPlayerNameUT(playerId), -loanFee);
    InsertFinanceTransaction(managerId, loaningClub, currentDate, "transfer",
      "Loan fee received: " + GetPlayerNameUT(playerId), loanFee);
  }

  std::stringstream uq;
  uq << "UPDATE loan_deals SET status='active',user_pending_action='',"
     << " monthly_wage_receiving_pct=" << std::max(0, std::min(100, wagePct))
     << ",monthly_wage_parent_pct=" << (100 - std::max(0, std::min(100, wagePct)))
     << " WHERE id=" << loanDealId << ";";
  UTExec(uq.str());

  std::stringstream oq;
  oq << "UPDATE loan_deals SET status='collapsed',collapse_reason='accepted_other_loan'"
     << " WHERE manager_id=" << managerId
     << " AND player_id=" << playerId
     << " AND id!=" << loanDealId
     << " AND status IN ('offered','negotiating','accepted_pending_player');";
  UTExec(oq.str());

  std::string pname = GetPlayerNameUT(playerId);
  InsertTransferNews(managerId, currentDate,
    pname + " joins " + GetTeamNameUT(receivingClub) + " on loan",
    "completed", playerId, loaningClub, receivingClub);
  InsertInboxMessage(managerId, "Loan confirmed: " + pname,
    pname + " has joined " + GetTeamNameUT(receivingClub) + " on loan"
    + (endDate.empty() ? "." : " until " + endDate + "."),
    "transfer", currentDate);
}

void InitiateLoanOffer(int managerId, int userClubId, int playerId,
                       int loaningClubId, int receivingClubId,
                       int loanFee, int wageReceivingPct,
                       const std::string &playingTimePromise,
                       int recallAfterMonth,
                       int optionFee, const std::string &optionDeadline,
                       int mandatoryFee, const std::string &mandatoryTrigger,
                       int mandatoryAppearances,
                       const std::string &endDate,
                       const std::string &direction,
                       int seasonYear, const std::string &currentDate) {
  if (!InTransferWindow(currentDate)) return;
  if (playerId <= 0 || loaningClubId <= 0 || receivingClubId <= 0) return;
  if (loaningClubId == receivingClubId) return;
  if (GetPlayerCurrentClub(managerId, playerId) != loaningClubId) return;

  std::stringstream dup;
  dup << "SELECT COUNT(*) FROM loan_deals WHERE manager_id=" << managerId
      << " AND player_id=" << playerId
      << " AND (status IN ('accepted_pending_player','active')"
      << " OR (receiving_club_id=" << receivingClubId
      << " AND status IN ('offered','negotiating')));";
  DatabaseResult *dr = GetDB()->Query(dup.str().c_str());
  int dupCount = (dr && dr->data.size() > 0) ? atoi(UTCell(dr,0,0).c_str()) : 0;
  if (dr) delete dr;
  if (dupCount > 0) return;

  int wagePct = std::max(0, std::min(100, wageReceivingPct));
  std::string status = (loaningClubId == userClubId && receivingClubId != userClubId)
    ? "offered" : "negotiating";
  int initiatingClub = (direction == "loan_in") ? receivingClubId : loaningClubId;
  if (direction == "incoming_loan_out") initiatingClub = receivingClubId;

  std::stringstream iq;
  iq << "INSERT INTO loan_deals("
     << "manager_id,loaning_club_id,receiving_club_id,player_id,"
     << "loan_fee,wage_split_pct,recall_clause_after_month,"
     << "option_to_buy_fee,option_to_buy_deadline,buy_back_fee,buy_back_expiry,"
     << "season_year,status,created_date,end_date,initiating_club_id,direction,"
     << "monthly_wage_parent_pct,monthly_wage_receiving_pct,playing_time_promise,"
     << "mandatory_buy_fee,mandatory_buy_trigger,mandatory_buy_appearances,"
     << "appearances_so_far,user_pending_action,collapse_reason)"
     << " VALUES(" << managerId << "," << loaningClubId << "," << receivingClubId
     << "," << playerId << "," << loanFee << "," << wagePct << ","
     << recallAfterMonth << "," << optionFee << ",'" << UTSql(optionDeadline)
     << "',0,''," << seasonYear << ",'" << status << "','" << currentDate
     << "','" << UTSql(endDate) << "'," << initiatingClub << ",'"
     << UTSql(direction) << "'," << (100 - wagePct) << "," << wagePct
     << ",'" << UTSql(playingTimePromise.empty() ? "rotation" : playingTimePromise)
     << "'," << mandatoryFee << ",'" << UTSql(mandatoryTrigger) << "',"
     << mandatoryAppearances << ",0,'','');";
  UTExec(iq.str());

  std::string pname = GetPlayerNameUT(playerId);
  InsertInboxMessage(managerId, "Loan offer submitted: " + pname,
    "The loan proposal is now being reviewed.", "transfer", currentDate);
}

void OfferLoan(int managerId, int userClubId, int playerId, int receivingClubId,
               int loanFee, int wageSplitPct, int recallAfterMonth,
               int optionFee, const std::string &optionDeadline,
               int buyBackFee, const std::string &buyBackExpiry,
               int seasonYear, const std::string &currentDate) {
  (void)buyBackFee;
  (void)buyBackExpiry;
  InitiateLoanOffer(managerId, userClubId, playerId, userClubId, receivingClubId,
                    loanFee, wageSplitPct, "rotation", recallAfterMonth,
                    optionFee, optionDeadline, 0, "", 0,
                    AddDays(currentDate, 180), "loan_out",
                    seasonYear, currentDate);
}

// ---- ProcessAILoanDecision --------------------------------------------------
// AI clubs evaluate loan proposals against intelligence filters.

static int LoanFitScore(int managerId, int receivingClubId, int playerId,
                        int wagePct, const std::string &promise,
                        int seasonYear) {
  std::string role;
  int age = 24, pot = 70;
  long long wage = GetPlayerCurrentWage(managerId, playerId);
  {
    std::stringstream pq;
    pq << "SELECT role,age,sofifaPotential FROM players WHERE id=" << playerId << " LIMIT 1;";
    DatabaseResult *pr = GetDB()->Query(pq.str().c_str());
    if (pr && pr->data.size() > 0) {
      role = UTCell(pr,0,0);
      age = atoi(UTCell(pr,0,1).c_str());
      pot = atoi(UTCell(pr,0,2).c_str());
    }
    if (pr) delete pr;
  }
  std::string grp = RoleToGroup(role);
  auto needs = EvaluateSquadNeeds(managerId, receivingClubId, seasonYear);
  int need = needs.count(grp) ? needs[grp] : 25;

  int sameRole = 0;
  {
    std::stringstream cq;
    cq << "SELECT COUNT(*) FROM players p LEFT JOIN player_save_state pss"
       << " ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
       << " WHERE COALESCE(pss.team_id,p.team_id)=" << receivingClubId
       << " AND p.role='" << UTSql(role) << "';";
    DatabaseResult *cr = GetDB()->Query(cq.str().c_str());
    if (cr && cr->data.size() > 0) sameRole = atoi(UTCell(cr,0,0).c_str());
    if (cr) delete cr;
  }
  int score = 35 + need / 2 + wagePct / 4 - sameRole * 8;
  if (promise == "regular_starter") score += need > 50 ? 12 : -10;
  if (promise == "squad_player") score += 4;
  if (age <= 22 && pot >= 80) score += 8;
  if (wage > 0 && wagePct < 30) score -= 10;
  return std::max(0, std::min(100, score));
}

static void GenerateAILoanActivity(int managerId, int userClubId,
                                   const std::string &currentDate,
                                   int seasonYear) {
  if (!InTransferWindow(currentDate)) return;
  unsigned int dateSeed = (unsigned int)DateToJulian(currentDate);

  // AI clubs mark realistic loan candidates as available.
  DatabaseResult *clubs = GetDB()->Query("SELECT id FROM teams WHERE transfer_budget > 0;");
  if (!clubs) return;
  for (unsigned int ci = 0; ci < clubs->data.size(); ci++) {
    int clubId = atoi(UTCell(clubs,ci,0).c_str());
    if (clubId == userClubId) continue;
    if (((dateSeed ^ (unsigned int)(clubId * 31)) % 100) > 18) continue;

    std::stringstream pq;
    pq << "SELECT p.id, p.age, p.sofifaPotential, p.base_stat"
       << " FROM players p LEFT JOIN player_save_state pss"
       << " ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
       << " LEFT JOIN player_market_status pms"
       << " ON pms.manager_id=" << managerId << " AND pms.player_id=p.id"
       << " WHERE COALESCE(pss.team_id,p.team_id)=" << clubId
       << " AND COALESCE(pms.status,'')=''"
       << " AND NOT EXISTS (SELECT 1 FROM loan_deals ld"
       << "   WHERE ld.manager_id=" << managerId
       << "   AND ld.player_id=p.id"
       << "   AND ld.status IN ('accepted_pending_player','active'))"
       << " AND (p.age<=23 OR p.base_stat<0.62)"
       << " ORDER BY (p.sofifaPotential-p.base_stat*100) DESC, p.age ASC LIMIT 1;";
    DatabaseResult *pr = GetDB()->Query(pq.str().c_str());
    if (pr && pr->data.size() > 0) {
      int pid = atoi(UTCell(pr,0,0).c_str());
      std::stringstream iq;
      iq << "INSERT OR REPLACE INTO player_market_status"
         << "(manager_id,player_id,status,set_date,asking_price)"
         << " VALUES(" << managerId << "," << pid << ",'loan_listed','"
         << currentDate << "',0);";
      UTExec(iq.str());
    }
    if (pr) delete pr;
  }
  delete clubs;

  // AI clubs seek listed players, and occasionally sensible unlisted user prospects.
  DatabaseResult *buyers = GetDB()->Query("SELECT id FROM teams WHERE transfer_budget > 0;");
  if (!buyers) return;
  for (unsigned int bi = 0; bi < buyers->data.size(); bi++) {
    int buyerId = atoi(UTCell(buyers,bi,0).c_str());
    if (buyerId == userClubId) continue;
    if (((dateSeed ^ (unsigned int)(buyerId * 911)) % 100) > 22) continue;

    std::stringstream tq;
    tq << "SELECT p.id, COALESCE(pss.team_id,p.team_id), p.weekly_wage"
       << " FROM players p"
       << " LEFT JOIN player_market_status pms"
       << " ON pms.manager_id=" << managerId << " AND pms.player_id=p.id"
       << " LEFT JOIN player_save_state pss"
       << " ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
       << " WHERE (pms.status='loan_listed'"
       << " OR (COALESCE(pss.team_id,p.team_id)=" << userClubId
       << " AND p.age<=23 AND p.base_stat<0.72))"
       << " AND COALESCE(pss.team_id,p.team_id)!=" << buyerId
       << " AND NOT EXISTS (SELECT 1 FROM loan_deals ld"
       << "   WHERE ld.manager_id=" << managerId
       << "   AND ld.player_id=p.id"
       << "   AND (ld.status IN ('accepted_pending_player','active')"
       << "   OR (ld.receiving_club_id=" << buyerId
       << "   AND ld.status IN ('offered','negotiating'))))"
       << " ORDER BY p.base_stat DESC LIMIT 8;";
    DatabaseResult *tr = GetDB()->Query(tq.str().c_str());
    if (!tr || tr->data.empty()) { if (tr) delete tr; continue; }
    int pick = (int)((dateSeed ^ (unsigned int)buyerId) % tr->data.size());
    int playerId = atoi(UTCell(tr,pick,0).c_str());
    int parentId = atoi(UTCell(tr,pick,1).c_str());
    long long wage = atoll(UTCell(tr,pick,2).c_str());
    delete tr;
    if (parentId <= 0 || parentId == buyerId) continue;

    int wagePct = 50 + (int)((dateSeed + buyerId + playerId) % 6) * 10;
    wagePct = std::min(100, wagePct);
    int loanFee = (int)std::max(0LL, wage * (long long)(wagePct / 10));
    std::string direction = parentId == userClubId ? "incoming_loan_out" : "ai_loan";
    InitiateLoanOffer(managerId, userClubId, playerId, parentId, buyerId,
      loanFee, wagePct, "squad_player", 1, 0, "", 0, "", 0,
      AddDays(currentDate, 180), direction, seasonYear, currentDate);
  }
  delete buyers;
}

void ProcessAILoanDecision(int managerId, int userClubId,
                           const std::string &currentDate, int seasonYear) {
  GenerateAILoanActivity(managerId, userClubId, currentDate, seasonYear);

  std::stringstream sq;
  sq << "SELECT id,loaning_club_id,receiving_club_id,player_id,status,"
     << "created_date,initiating_club_id,monthly_wage_receiving_pct,"
     << "playing_time_promise,direction"
     << " FROM loan_deals WHERE manager_id=" << managerId
     << " AND status IN ('offered','negotiating','accepted_pending_player');";
  DatabaseResult *lr = GetDB()->Query(sq.str().c_str());
  if (!lr) return;

  struct LoanRow {
    int id, loanClub, rcvClub, playerId, initClub, wagePct;
    std::string status, created, promise, direction;
  };
  std::vector<LoanRow> loans;
  for (unsigned int i = 0; i < lr->data.size(); i++) {
    LoanRow l;
    l.id = atoi(UTCell(lr,i,0).c_str());
    l.loanClub = atoi(UTCell(lr,i,1).c_str());
    l.rcvClub = atoi(UTCell(lr,i,2).c_str());
    l.playerId = atoi(UTCell(lr,i,3).c_str());
    l.status = UTCell(lr,i,4);
    l.created = UTCell(lr,i,5);
    l.initClub = atoi(UTCell(lr,i,6).c_str());
    l.wagePct = atoi(UTCell(lr,i,7).c_str());
    l.promise = UTCell(lr,i,8);
    l.direction = UTCell(lr,i,9);
    loans.push_back(l);
  }
  delete lr;

  for (auto &l : loans) {
    if (GetPlayerCurrentClub(managerId, l.playerId) != l.loanClub) {
      UTExec("UPDATE loan_deals SET status='collapsed',collapse_reason='player_not_at_parent' WHERE id=" + std::to_string(l.id) + ";");
      continue;
    }

    int ageDays = DateToJulian(currentDate) - DateToJulian(l.created);
    if (l.status == "accepted_pending_player") {
      if (ageDays >= 2) ActivateLoanDeal(managerId, l.id, currentDate);
      continue;
    }

    bool needsUserDecision =
      (l.loanClub == userClubId && l.initClub != userClubId) ||
      (l.rcvClub == userClubId && l.initClub != userClubId);
    if (needsUserDecision) continue;
    if (ageDays < 1) continue;

    int fit = LoanFitScore(managerId, l.rcvClub, l.playerId,
                           l.wagePct, l.promise, seasonYear);
    if (fit >= 52) {
      std::stringstream uq;
      uq << "UPDATE loan_deals SET status='accepted_pending_player',"
         << "created_date='" << currentDate << "' WHERE id=" << l.id << ";";
      UTExec(uq.str());
    } else if (fit >= 38) {
      int newPct = std::min(100, l.wagePct + 20);
      std::stringstream uq;
      uq << "UPDATE loan_deals SET status='negotiating',"
         << "monthly_wage_receiving_pct=" << newPct
         << ",monthly_wage_parent_pct=" << (100 - newPct)
         << ",wage_split_pct=" << newPct
         << ",created_date='" << currentDate << "' WHERE id=" << l.id << ";";
      UTExec(uq.str());
    } else {
      std::stringstream uq;
      uq << "UPDATE loan_deals SET status='rejected',collapse_reason='poor_loan_fit'"
         << " WHERE id=" << l.id << ";";
      UTExec(uq.str());
    }
  }
}

void RespondToLoanOffer(int managerId, int loanDealId,
                        const std::string &action, int counterFee,
                        int counterWagePct) {
  std::string currentDate;
  {
    std::stringstream dq;
    dq << "SELECT current_date FROM managers WHERE id=" << managerId << " LIMIT 1;";
    DatabaseResult *dr = GetDB()->Query(dq.str().c_str());
    if (dr && dr->data.size() > 0) currentDate = UTCell(dr,0,0);
    if (dr) delete dr;
  }
  if (currentDate.empty()) currentDate = "2026-07-01";

  int userClubId = GetManagedClubId(managerId);
  int playerId = 0, loanClub = 0, rcvClub = 0;
  {
    std::stringstream q;
    q << "SELECT player_id,loaning_club_id,receiving_club_id"
      << " FROM loan_deals WHERE manager_id=" << managerId
      << " AND id=" << loanDealId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str().c_str());
    if (r && r->data.size() > 0) {
      playerId = atoi(UTCell(r,0,0).c_str());
      loanClub = atoi(UTCell(r,0,1).c_str());
      rcvClub = atoi(UTCell(r,0,2).c_str());
    }
    if (r) delete r;
  }

  if (action == "accept") {
    std::stringstream uq;
    uq << "UPDATE loan_deals SET status='accepted_pending_player',"
       << "created_date='" << currentDate << "',user_pending_action=''"
       << " WHERE manager_id=" << managerId << " AND id=" << loanDealId << ";";
    UTExec(uq.str());
    std::stringstream oq;
    oq << "UPDATE loan_deals SET status='collapsed',collapse_reason='accepted_other_loan'"
       << " WHERE manager_id=" << managerId
       << " AND player_id=" << playerId
       << " AND id!=" << loanDealId
       << " AND status IN ('offered','negotiating','accepted_pending_player');";
    UTExec(oq.str());
    InsertTransferNews(managerId, currentDate,
      GetTeamNameUT(rcvClub) + " loan offer accepted for " + GetPlayerNameUT(playerId),
      "incoming_accepted", playerId, loanClub, rcvClub);
    return;
  }

  if (action == "counter") {
    int pct = std::max(0, std::min(100, counterWagePct));
    std::stringstream uq;
    uq << "UPDATE loan_deals SET status='negotiating',loan_fee=" << counterFee
       << ",wage_split_pct=" << pct
       << ",monthly_wage_receiving_pct=" << pct
       << ",monthly_wage_parent_pct=" << (100 - pct)
       << ",initiating_club_id=" << userClubId
       << ",created_date='" << currentDate << "'"
       << " WHERE manager_id=" << managerId << " AND id=" << loanDealId << ";";
    UTExec(uq.str());
    return;
  }

  if (action == "reject" || action == "block") {
    std::stringstream uq;
    uq << "UPDATE loan_deals SET status='"
       << (action == "block" ? "collapsed" : "rejected")
       << "',collapse_reason='" << (action == "block" ? "blocked" : "user_rejected")
       << "' WHERE manager_id=" << managerId << " AND id=" << loanDealId << ";";
    UTExec(uq.str());
    return;
  }

  if (action == "recall" && loanClub == userClubId) {
    long long wage = GetPlayerCurrentWage(managerId, playerId);
    SetPlayerSaveState(managerId, playerId, loanClub, wage);
    std::stringstream uq;
    uq << "UPDATE loan_deals SET status='recalled',collapse_reason='parent_recall'"
       << " WHERE manager_id=" << managerId << " AND id=" << loanDealId << ";";
    UTExec(uq.str());
    InsertTransferNews(managerId, currentDate,
      GetPlayerNameUT(playerId) + " recalled from loan",
      "completed", playerId, rcvClub, loanClub);
    return;
  }

  if (action == "exercise_option") {
    std::stringstream q;
    q << "SELECT option_to_buy_fee FROM loan_deals WHERE id=" << loanDealId
      << " AND manager_id=" << managerId << " LIMIT 1;";
    DatabaseResult *r = GetDB()->Query(q.str().c_str());
    long long fee = (r && r->data.size() > 0) ? atoll(UTCell(r,0,0).c_str()) : 0;
    if (r) delete r;
    if (fee > 0 && rcvClub == userClubId) {
      std::stringstream bq;
      bq << "UPDATE club_finances SET transfer_budget=MAX(0,transfer_budget-" << fee << ")"
         << " WHERE manager_id=" << managerId << " AND club_id=" << rcvClub << ";";
      UTExec(bq.str());
      std::stringstream sq2;
      sq2 << "UPDATE club_finances SET cash_balance=cash_balance+" << fee
          << " WHERE manager_id=" << managerId << " AND club_id=" << loanClub << ";";
      UTExec(sq2.str());
      UTExec("UPDATE loan_deals SET status='option_exercised' WHERE id=" + std::to_string(loanDealId) + ";");
      InsertFinanceTransaction(managerId, rcvClub, currentDate, "transfer",
        "Loan option fee paid: " + GetPlayerNameUT(playerId), -fee);
      InsertFinanceTransaction(managerId, loanClub, currentDate, "transfer",
        "Loan option fee received: " + GetPlayerNameUT(playerId), fee);
      InsertTransferNews(managerId, currentDate,
        "Option to buy exercised for " + GetPlayerNameUT(playerId),
        "completed", playerId, loanClub, rcvClub);
    }
  }
}

// ---- ProcessLoanClauses -----------------------------------------------------

void ProcessLoanClauses(int managerId, const std::string &currentDate, int seasonYear) {
  std::stringstream sq;
  sq << "SELECT id, loaning_club_id, receiving_club_id, player_id,"
     << " recall_clause_after_month, option_to_buy_fee, option_to_buy_deadline,"
     << " buy_back_fee, buy_back_expiry, end_date, mandatory_buy_fee,"
     << " mandatory_buy_trigger, mandatory_buy_appearances, playing_time_promise,"
     << " appearances_so_far, last_drama_date, created_date"
     << " FROM loan_deals WHERE manager_id=" << managerId << " AND status='active';";
  DatabaseResult *lr = GetDB()->Query(sq.str().c_str());
  if (!lr) return;

  int curMonth = currentDate.size() >= 7 ? atoi(currentDate.substr(5,2).c_str()) : 0;
  struct LDeal {
    int id, loanClub, rcvClub, playerId;
    int recallMonth, optFee, bbFee, mandatoryFee, mandatoryApps, apps;
    std::string optDeadline, bbExpiry, endDate, mandatoryTrigger, promise, lastDramaDate, createdDate;
  };
  std::vector<LDeal> deals;
  for (unsigned int i = 0; i < lr->data.size(); i++) {
    LDeal d;
    d.id         = atoi(UTCell(lr,i,0).c_str());
    d.loanClub   = atoi(UTCell(lr,i,1).c_str());
    d.rcvClub    = atoi(UTCell(lr,i,2).c_str());
    d.playerId   = atoi(UTCell(lr,i,3).c_str());
    d.recallMonth= atoi(UTCell(lr,i,4).c_str());
    d.optFee     = atoi(UTCell(lr,i,5).c_str());
    d.optDeadline= UTCell(lr,i,6);
    d.bbFee      = atoi(UTCell(lr,i,7).c_str());
    d.bbExpiry   = UTCell(lr,i,8);
    d.endDate    = UTCell(lr,i,9);
    d.mandatoryFee = atoi(UTCell(lr,i,10).c_str());
    d.mandatoryTrigger = UTCell(lr,i,11);
    d.mandatoryApps = atoi(UTCell(lr,i,12).c_str());
    d.promise = UTCell(lr,i,13);
    d.apps = atoi(UTCell(lr,i,14).c_str());
    d.lastDramaDate = UTCell(lr,i,15);
    d.createdDate = UTCell(lr,i,16);
    deals.push_back(d);
  }
  delete lr;

  for (auto &d : deals) {
    int starts = 0, subs = 0;
    {
      std::stringstream aq;
      aq << "SELECT starts,sub_appearances FROM player_appearances"
         << " WHERE manager_id=" << managerId
         << " AND player_id=" << d.playerId
         << " AND season_year=" << seasonYear << " LIMIT 1;";
      DatabaseResult *ar = GetDB()->Query(aq.str().c_str());
      if (ar && ar->data.size() > 0) {
        starts = atoi(UTCell(ar,0,0).c_str());
        subs = atoi(UTCell(ar,0,1).c_str());
      }
      if (ar) delete ar;
    }
    int totalApps = starts + subs;
    if (totalApps != d.apps) {
      std::stringstream au;
      au << "UPDATE loan_deals SET appearances_so_far=" << totalApps
         << " WHERE id=" << d.id << ";";
      UTExec(au.str());
      d.apps = totalApps;
    }

    bool mandatoryDue = false;
    if (d.mandatoryFee > 0) {
      if (d.mandatoryTrigger == "appearances" && d.mandatoryApps > 0 && d.apps >= d.mandatoryApps)
        mandatoryDue = true;
      if ((d.mandatoryTrigger.empty() || d.mandatoryTrigger == "end_date")
          && !d.endDate.empty() && currentDate >= d.endDate)
        mandatoryDue = true;
    }
    if (mandatoryDue) {
      std::stringstream bq;
      bq << "UPDATE club_finances SET transfer_budget=MAX(0,transfer_budget-" << d.mandatoryFee << ")"
         << " WHERE manager_id=" << managerId << " AND club_id=" << d.rcvClub << ";";
      UTExec(bq.str());
      std::stringstream sq2;
      sq2 << "UPDATE club_finances SET cash_balance=cash_balance+" << d.mandatoryFee
          << " WHERE manager_id=" << managerId << " AND club_id=" << d.loanClub << ";";
      UTExec(sq2.str());
      UTExec("UPDATE loan_deals SET status='mandatory_exercised' WHERE id=" + std::to_string(d.id) + ";");
      InsertFinanceTransaction(managerId, d.rcvClub, currentDate, "transfer",
        "Mandatory loan buy fee paid: " + GetPlayerNameUT(d.playerId), -d.mandatoryFee);
      InsertFinanceTransaction(managerId, d.loanClub, currentDate, "transfer",
        "Mandatory loan buy fee received: " + GetPlayerNameUT(d.playerId), d.mandatoryFee);
      InsertTransferNews(managerId, currentDate, "Mandatory buy clause triggered for " + GetPlayerNameUT(d.playerId),
                          "completed", d.playerId, d.loanClub, d.rcvClub);
      continue;
    }

    if ((d.promise == "regular_starter" || d.promise == "important")
        && DateToJulian(currentDate) - DateToJulian(d.lastDramaDate) >= 30) {
      int daysElapsed = std::max(1, DateToJulian(currentDate) - DateToJulian(d.createdDate));
      int expectedApps = d.promise == "regular_starter" ? daysElapsed / 10 : daysElapsed / 16;
      if (expectedApps >= 2 && d.apps + 1 < expectedApps) {
        InsertInboxMessage(managerId, "Loan playing time concern",
          GetPlayerNameUT(d.playerId) + " is not receiving the promised playing time on loan.",
          "transfer", currentDate);
        AddUnhappiness(managerId, d.playerId, "loan_playing_time", 8, currentDate);
        std::stringstream du;
        du << "UPDATE loan_deals SET last_drama_date='" << currentDate
           << "' WHERE id=" << d.id << ";";
        UTExec(du.str());
        if (d.recallMonth > 0 && curMonth >= d.recallMonth && d.loanClub == GetManagedClubId(managerId)) {
          long long wage = GetPlayerCurrentWage(managerId, d.playerId);
          SetPlayerSaveState(managerId, d.playerId, d.loanClub, wage);
          UTExec("UPDATE loan_deals SET status='recalled',collapse_reason='missed_playing_time' WHERE id=" + std::to_string(d.id) + ";");
          InsertTransferNews(managerId, currentDate, GetPlayerNameUT(d.playerId) + " recalled after loan concerns",
                              "completed", d.playerId, d.rcvClub, d.loanClub);
          continue;
        }
      }
    }

    // Season end: expire loan
    if (!d.endDate.empty() && currentDate >= d.endDate) {
      long long wage = 0;
      { std::stringstream wq; wq << "SELECT COALESCE(pss.weekly_wage,p.weekly_wage)"
          << " FROM players p LEFT JOIN player_save_state pss"
          << " ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
          << " WHERE p.id=" << d.playerId << ";";
        DatabaseResult *wr = GetDB()->Query(wq.str().c_str());
        if (wr && wr->data.size() > 0) wage = atoll(UTCell(wr,0,0).c_str());
        if (wr) delete wr; }
      SetPlayerSaveState(managerId, d.playerId, d.loanClub, wage);
      UTExec("UPDATE loan_deals SET status='expired' WHERE id=" + std::to_string(d.id) + ";");
      InsertTransferNews(managerId, currentDate, "Player returned from loan",
                          "completed", d.playerId, d.rcvClub, d.loanClub);
      continue;
    }

    // Buy-back: warn user 30 days before expiry if value has grown
    if (d.bbFee > 0 && !d.bbExpiry.empty() && d.loanClub > 0) {
      std::string warnDate = UTAddDaysSigned(d.bbExpiry, -30);
      if (currentDate >= warnDate && currentDate < d.bbExpiry) {
        long long curVal = 0;
        { std::stringstream vq; vq << "SELECT playervalue FROM players WHERE id=" << d.playerId << ";";
          DatabaseResult *vr = GetDB()->Query(vq.str().c_str());
          if (vr && vr->data.size() > 0) curVal = atoll(UTCell(vr,0,0).c_str());
          if (vr) delete vr; }
        if (curVal >= (long long)(d.bbFee * 1.5)) {
          InsertInboxMessage(managerId, "Buy-back window closing soon",
            "A player's buy-back window is expiring and their value has risen significantly. Review your loan deals.",
            "transfer", currentDate);
        }
      }
    }

    // Option to buy: AI automatically exercises if squad needs it and budget allows
    if (d.optFee > 0 && !d.optDeadline.empty() && currentDate >= d.optDeadline) {
      int userClub = GetManagedClubId(managerId);
      if (d.loanClub == userClub || d.rcvClub == userClub) {
        if (currentDate == d.optDeadline) {
          InsertInboxMessage(managerId, "Loan option deadline",
            "The option to buy for " + GetPlayerNameUT(d.playerId) + " is due. Review Active Loans.",
            "transfer", currentDate);
        }
        continue;
      }
      auto needs = EvaluateSquadNeeds(managerId, d.rcvClub, seasonYear);
      std::string pRole;
      { std::stringstream rq; rq << "SELECT role FROM players WHERE id=" << d.playerId << ";";
        DatabaseResult *rr = GetDB()->Query(rq.str().c_str());
        if (rr && rr->data.size() > 0) pRole = RoleToGroup(UTCell(rr,0,0));
        if (rr) delete rr; }
      int needScore = needs.count(pRole) ? needs[pRole] : 0;
      long long budget = 0;
      { std::stringstream bq; bq << "SELECT transfer_budget FROM club_finances WHERE manager_id=" << managerId
          << " AND club_id=" << d.rcvClub << ";";
        DatabaseResult *br = GetDB()->Query(bq.str().c_str());
        if (br && br->data.size() > 0) budget = atoll(UTCell(br,0,0).c_str());
        if (br) delete br; }
      if (needScore > 60 && budget >= d.optFee) {
        // Exercise option
        { std::stringstream uq; uq << "UPDATE club_finances SET transfer_budget=transfer_budget-" << d.optFee
            << " WHERE manager_id=" << managerId << " AND club_id=" << d.rcvClub << ";"; UTExec(uq.str()); }
        { std::stringstream uq; uq << "UPDATE club_finances SET cash_balance=cash_balance+" << d.optFee
            << " WHERE manager_id=" << managerId << " AND club_id=" << d.loanClub << ";"; UTExec(uq.str()); }
        UTExec("UPDATE loan_deals SET status='option_exercised' WHERE id=" + std::to_string(d.id) + ";");
        InsertFinanceTransaction(managerId, d.rcvClub, currentDate, "transfer",
          "Loan option fee paid: " + GetPlayerNameUT(d.playerId), -d.optFee);
        InsertFinanceTransaction(managerId, d.loanClub, currentDate, "transfer",
          "Loan option fee received: " + GetPlayerNameUT(d.playerId), d.optFee);
        InsertTransferNews(managerId, currentDate, "Option to buy exercised on loan player",
                            "completed", d.playerId, d.loanClub, d.rcvClub);
      } else {
        // Option expires — return player
        long long wage = 0;
        { std::stringstream wq; wq << "SELECT COALESCE(pss.weekly_wage,p.weekly_wage)"
            << " FROM players p LEFT JOIN player_save_state pss"
            << " ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
            << " WHERE p.id=" << d.playerId << ";";
          DatabaseResult *wr = GetDB()->Query(wq.str().c_str());
          if (wr && wr->data.size() > 0) wage = atoll(UTCell(wr,0,0).c_str());
          if (wr) delete wr; }
        SetPlayerSaveState(managerId, d.playerId, d.loanClub, wage);
        UTExec("UPDATE loan_deals SET status='expired' WHERE id=" + std::to_string(d.id) + ";");
      }
    }
  }
}

// ---- Squad harmony ----------------------------------------------------------

void ProcessSquadHarmonyOnSale(int managerId, int playerId, int userClubId,
                                const std::string &currentDate) {
  // Ensure harmony row exists
  { std::stringstream iq; iq << "INSERT OR IGNORE INTO squad_harmony(manager_id) VALUES(" << managerId << ");";
    UTExec(iq.str()); }

  int intlRep = 0, bStat = 0;
  { std::stringstream pq; pq << "SELECT international_reputation, base_stat FROM players WHERE id=" << playerId << ";";
    DatabaseResult *pr = GetDB()->Query(pq.str().c_str());
    if (pr && pr->data.size() > 0) {
      intlRep = atoi(UTCell(pr,0,0).c_str());
      bStat   = atoi(UTCell(pr,0,1).c_str());
    }
    if (pr) delete pr; }

  // Captain check (formation order 0 — proxy for captain)
  bool isCaptain = false;
  { std::stringstream cq; cq << "SELECT id FROM fixtures WHERE manager_id=" << managerId
      << " AND (home_team_id=" << userClubId << " OR away_team_id=" << userClubId << ")"
      << " AND status='completed' LIMIT 1;";
    // Simplified: treat international_reputation >= 4 at club >= 3 seasons as captain proxy
    if (intlRep >= 4) isCaptain = true; }

  int moraleDelta = 0, stabilityDelta = 0;

  if (isCaptain) {
    moraleDelta    -= 15;
    stabilityDelta -= 20;
    // Fire media pressure event
    { std::stringstream iq; iq << "INSERT INTO media_pressure_events(manager_id,event_type,severity,game_date,expires_date)"
        << " VALUES(" << managerId << ",'fans_angry_sale',2,'" << currentDate << "','"
        << AddDays(currentDate, 30) << "');"; UTExec(iq.str()); }
  } else if (intlRep >= 3) {
    moraleDelta -= 10;
    { std::stringstream iq; iq << "INSERT INTO media_pressure_events(manager_id,event_type,severity,game_date,expires_date)"
        << " VALUES(" << managerId << ",'fans_angry_sale',1,'" << currentDate << "','"
        << AddDays(currentDate, 30) << "');"; UTExec(iq.str()); }
  }

  if (bStat >= 80) {
    // Elite performer — board confidence hit + succession role
    { std::stringstream uq; uq << "UPDATE club_finances SET board_confidence=MAX(0,board_confidence-10)"
        << " WHERE manager_id=" << managerId << " AND club_id=" << userClubId << ";";
      UTExec(uq.str()); }
  }

  if (moraleDelta != 0 || stabilityDelta != 0) {
    std::stringstream uq;
    uq << "UPDATE squad_harmony SET"
       << " morale=MAX(0,MIN(100,morale+" << moraleDelta << "))"
       << ",dressing_room_stability=MAX(0,MIN(100,dressing_room_stability+" << stabilityDelta << "))"
       << ",last_updated='" << currentDate << "'"
       << " WHERE manager_id=" << managerId << ";";
    UTExec(uq.str());
  }
}

void ProcessSquadHarmonyOnBuy(int managerId, int playerId, const std::string &currentDate) {
  { std::stringstream iq; iq << "INSERT OR IGNORE INTO squad_harmony(manager_id) VALUES(" << managerId << ");";
    UTExec(iq.str()); }

  int intlRep = 0;
  { std::stringstream pq; pq << "SELECT international_reputation FROM players WHERE id=" << playerId << ";";
    DatabaseResult *pr = GetDB()->Query(pq.str().c_str());
    if (pr && pr->data.size() > 0) intlRep = atoi(UTCell(pr,0,0).c_str());
    if (pr) delete pr; }

  if (intlRep >= 4) {
    std::stringstream uq;
    uq << "UPDATE squad_harmony SET"
       << " morale=MIN(100,morale+15)"
       << ",dressing_room_stability=MIN(100,dressing_room_stability+5)"
       << ",last_updated='" << currentDate << "'"
       << " WHERE manager_id=" << managerId << ";";
    UTExec(uq.str());
  }
}

// ---- TrackPlayerAppearances -------------------------------------------------

void TrackPlayerAppearances(int managerId, int fixtureId, int seasonYear) {
  // Get which clubs played in this fixture
  std::stringstream fq;
  fq << "SELECT home_team_id, away_team_id, status FROM fixtures WHERE id=" << fixtureId
     << " AND manager_id=" << managerId << ";";
  DatabaseResult *fr = GetDB()->Query(fq.str().c_str());
  if (!fr || fr->data.size() == 0) { if (fr) delete fr; return; }
  int homeId = atoi(UTCell(fr,0,0).c_str());
  int awayId = atoi(UTCell(fr,0,1).c_str());
  std::string status = UTCell(fr,0,2);
  delete fr;
  if (status != "completed") return;

  // For both clubs, count all their players as starters (simplified — no formation data)
  // Count players on each team as having started
  for (int clubId : {homeId, awayId}) {
    std::stringstream pq;
    pq << "SELECT p.id FROM players p LEFT JOIN player_save_state pss"
       << " ON pss.manager_id=" << managerId << " AND pss.player_id=p.id"
       << " WHERE COALESCE(pss.team_id,p.team_id)=" << clubId << ";";
    DatabaseResult *pr = GetDB()->Query(pq.str().c_str());
    if (!pr) continue;
    for (unsigned int i = 0; i < pr->data.size(); i++) {
      int pid = atoi(UTCell(pr,i,0).c_str());
      std::stringstream uq;
      uq << "INSERT INTO player_appearances(manager_id,player_id,season_year,starts,sub_appearances)"
         << " VALUES(" << managerId << "," << pid << "," << seasonYear << ",1,0)"
         << " ON CONFLICT(manager_id,player_id,season_year) DO UPDATE SET starts=starts+1;";
      UTExec(uq.str());
    }
    delete pr;
  }
}

// ---- EvaluatePromiseFulfillment ---------------------------------------------

void EvaluatePromiseFulfillment(int managerId, const std::string &currentDate, int seasonYear) {
  // Only run on 1st of month
  if (currentDate.size() < 10 || currentDate.substr(8,2) != "01") return;

  // Count completed fixtures so far this season
  int totalFixtures = 0;
  { std::stringstream fq; fq << "SELECT COUNT(*) FROM fixtures WHERE manager_id=" << managerId
      << " AND season_year=" << seasonYear << " AND status='completed';";
    DatabaseResult *fr = GetDB()->Query(fq.str().c_str());
    if (fr && fr->data.size() > 0) totalFixtures = atoi(UTCell(fr,0,0).c_str());
    if (fr) delete fr; }
  if (totalFixtures == 0) return;

  // Get all players with a promised role (from any negotiation that completed this season)
  std::stringstream nq;
  nq << "SELECT tn.player_id, tn.promised_role, tn.buying_club_id"
     << " FROM transfer_negotiations tn"
     << " WHERE tn.manager_id=" << managerId
     << " AND tn.state='completed'"
     << " AND tn.buying_club_id > 0;";
  DatabaseResult *nr = GetDB()->Query(nq.str().c_str());
  if (!nr) return;

  for (unsigned int i = 0; i < nr->data.size(); i++) {
    int pid       = atoi(UTCell(nr,i,0).c_str());
    std::string role = UTCell(nr,i,1);

    // Get appearance counts
    int starts = 0, subs = 0;
    { std::stringstream aq; aq << "SELECT starts, sub_appearances FROM player_appearances"
        << " WHERE manager_id=" << managerId << " AND player_id=" << pid
        << " AND season_year=" << seasonYear << ";";
      DatabaseResult *ar = GetDB()->Query(aq.str().c_str());
      if (ar && ar->data.size() > 0) {
        starts = atoi(UTCell(ar,0,0).c_str());
        subs   = atoi(UTCell(ar,0,1).c_str());
      }
      if (ar) delete ar; }

    bool violated = false;
    bool kept     = false;

    if (role == "star_player") {
      kept     = starts >= (int)(totalFixtures * 0.75);
      violated = starts <  (int)(totalFixtures * 0.50);
    } else if (role == "important") {
      kept     = starts >= (int)(totalFixtures * 0.50);
      violated = starts <  (int)(totalFixtures * 0.30);
    } else if (role == "rotation") {
      kept     = (starts + subs) >= (int)(totalFixtures * 0.40);
      violated = (starts + subs) <  (int)(totalFixtures * 0.20);
    } else if (role == "prospect") {
      kept     = (starts + subs) > 0;
      // Only penalise after 3 completed months (approx 3 fixtures)
      violated = (starts + subs) == 0 && totalFixtures > 3;
    }

    if (violated) {
      AddUnhappiness(managerId, pid, "promise_broken", 20, currentDate);
      InsertInboxMessage(managerId, "Player unhappy with playing time",
        "A player you promised a starting role to is not getting sufficient game time and is becoming unsettled.",
        "squad", currentDate);
      // If severity > 80: auto transfer list
      { std::stringstream sq2; sq2 << "SELECT severity FROM player_unhappiness WHERE manager_id=" << managerId
          << " AND player_id=" << pid << " AND reason='promise_broken' AND resolved=0;";
        DatabaseResult *sr = GetDB()->Query(sq2.str().c_str());
        if (sr && sr->data.size() > 0 && atoi(UTCell(sr,0,0).c_str()) > 80) {
          std::stringstream tq;
          tq << "INSERT OR REPLACE INTO player_market_status(manager_id,player_id,status,set_date,asking_price)"
             << " VALUES(" << managerId << "," << pid << ",'transfer_listed','"
             << currentDate << "',0);";
          UTExec(tq.str());
        }
        if (sr) delete sr; }
    }

    // Decay: if promise being kept, reduce severity by 5
    if (kept) {
      std::stringstream dq;
      dq << "UPDATE player_unhappiness SET severity=MAX(0,severity-5)"
         << " WHERE manager_id=" << managerId << " AND player_id=" << pid
         << " AND reason='promise_broken' AND resolved=0;";
      UTExec(dq.str());
      // Mark resolved if reaches 0
      std::stringstream rq;
      rq << "UPDATE player_unhappiness SET resolved=1"
         << " WHERE manager_id=" << managerId << " AND player_id=" << pid
         << " AND reason='promise_broken' AND severity=0 AND resolved=0;";
      UTExec(rq.str());
    }
  }
  delete nr;
}

// ---- ProcessMediaPressure ---------------------------------------------------

void ProcessMediaPressure(int managerId, const std::string &currentDate) {
  // Fire pending events
  std::stringstream sq;
  sq << "SELECT id, event_type, severity, expires_date FROM media_pressure_events"
     << " WHERE manager_id=" << managerId << " AND fired=0;";
  DatabaseResult *mr = GetDB()->Query(sq.str().c_str());
  if (!mr) return;

  struct MPEvent { int id, severity; std::string type, expires; };
  std::vector<MPEvent> events;
  for (unsigned int i = 0; i < mr->data.size(); i++) {
    MPEvent e;
    e.id       = atoi(UTCell(mr,i,0).c_str());
    e.type     = UTCell(mr,i,1);
    e.severity = atoi(UTCell(mr,i,2).c_str());
    e.expires  = UTCell(mr,i,3);
    events.push_back(e);
  }
  delete mr;

  for (auto &e : events) {
    if (e.type == "fans_angry_sale") {
      int moraleDrop = (e.severity == 1) ? 10 : (e.severity == 2) ? 15 : 20;
      std::stringstream uq;
      uq << "UPDATE squad_harmony SET morale=MAX(0,morale-" << moraleDrop << ")"
         << ",last_updated='" << currentDate << "'"
         << " WHERE manager_id=" << managerId << ";";
      UTExec(uq.str());
    } else if (e.type == "board_demands_signing") {
      std::stringstream uq;
      uq << "UPDATE club_finances SET board_confidence=MAX(0,board_confidence-5)"
         << " WHERE manager_id=" << managerId << ";";
      UTExec(uq.str());
    } else if (e.type == "failed_negotiations_criticism") {
      std::stringstream uq;
      uq << "UPDATE club_finances SET board_confidence=MAX(0,board_confidence-8)"
         << " WHERE manager_id=" << managerId << ";";
      UTExec(uq.str());
      InsertInboxMessage(managerId, "Board concerned over failed negotiations",
        "The board has noted your recent failed transfer dealings and is growing impatient.",
        "board", currentDate);
    } else if (e.type == "player_public_push") {
      InsertTransferNews(managerId, currentDate,
        "Player pushing for transfer", "warning", 0, 0, 0);
    }
    UTExec("UPDATE media_pressure_events SET fired=1 WHERE id=" + std::to_string(e.id) + ";");
  }

  // Check board_demands_signing trigger: window >= 50% done, 0 user signings, board_conf < 50
  if (InTransferWindow(currentDate)) {
    int daysLeft = DaysToWindowEnd(currentDate);
    int windowLen = (currentDate.size() >= 7 && currentDate.substr(5,2) == "01") ? 31 : 62;
    if (daysLeft < windowLen / 2) {
      // Check if user has any completed signings this window
      int signings = 0;
      { std::stringstream sq2;
        sq2 << "SELECT COUNT(*) FROM transfer_negotiations"
            << " WHERE manager_id=" << managerId
            << " AND buying_club_id IN (SELECT club_id FROM managers WHERE id=" << managerId << ")"
            << " AND state='completed';";
        DatabaseResult *sr = GetDB()->Query(sq2.str().c_str());
        if (sr && sr->data.size() > 0) signings = atoi(UTCell(sr,0,0).c_str());
        if (sr) delete sr; }
      int boardConf = 70;
      { std::stringstream bq; bq << "SELECT board_confidence FROM club_finances WHERE manager_id=" << managerId << " LIMIT 1;";
        DatabaseResult *br = GetDB()->Query(bq.str().c_str());
        if (br && br->data.size() > 0) boardConf = atoi(UTCell(br,0,0).c_str());
        if (br) delete br; }
      if (signings == 0 && boardConf < 50) {
        // Check if event already active
        std::stringstream eq2;
        eq2 << "SELECT id FROM media_pressure_events WHERE manager_id=" << managerId
            << " AND event_type='board_demands_signing' AND fired=0 LIMIT 1;";
        DatabaseResult *er2 = GetDB()->Query(eq2.str().c_str());
        bool exists = (er2 && er2->data.size() > 0);
        if (er2) delete er2;
        if (!exists) {
          std::stringstream iq;
          iq << "INSERT INTO media_pressure_events(manager_id,event_type,severity,game_date,expires_date)"
             << " VALUES(" << managerId << ",'board_demands_signing',1,'" << currentDate << "','"
             << AddDays(currentDate, daysLeft) << "');";
          UTExec(iq.str());
          InsertInboxMessage(managerId, "Board expects a signing",
            "The board is monitoring the transfer window and expects at least one signing before it closes.",
            "board", currentDate);
        }
      }
    }
  }

  // Decay expired events (restore morale/confidence at +2/day)
  { std::stringstream eq;
    eq << "SELECT id, event_type, severity FROM media_pressure_events"
       << " WHERE manager_id=" << managerId
       << " AND fired=1 AND expires_date<='" << currentDate << "';";
    DatabaseResult *er = GetDB()->Query(eq.str().c_str());
    if (er) {
      for (unsigned int i = 0; i < er->data.size(); i++) {
        std::string etype = UTCell(er,i,1);
        if (etype == "fans_angry_sale") {
          std::stringstream dq;
          dq << "UPDATE squad_harmony SET morale=MIN(70,morale+2),last_updated='" << currentDate << "'"
             << " WHERE manager_id=" << managerId << ";";
          UTExec(dq.str());
        }
      }
      delete er;
    }
  }
}

// ---- ProcessPoachingEscalation ----------------------------------------------

void ProcessPoachingEscalation(int managerId, int userClubId,
                                 const std::string &currentDate, int seasonYear) {
  // Only fires for smaller clubs (domestic_prestige <= 6)
  int userPres = 0;
  { std::stringstream pq; pq << "SELECT domestic_prestige FROM teams WHERE id=" << userClubId << ";";
    DatabaseResult *pr = GetDB()->Query(pq.str().c_str());
    if (pr && pr->data.size() > 0) userPres = atoi(UTCell(pr,0,0).c_str());
    if (pr) delete pr; }
  if (userPres > 6) return;

  // Check triggers
  bool triggered = false;

  // Trigger 1: finished top 2
  { std::stringstream sq;
    sq << "SELECT COUNT(*) FROM standings"
       << " WHERE manager_id=" << managerId << " AND team_id=" << userClubId
       << " AND season_year=" << seasonYear << " AND points >= ALL("
       << " SELECT points FROM standings WHERE manager_id=" << managerId
       << " AND season_year=" << seasonYear
       << " AND league_id = (SELECT league_id FROM standings WHERE manager_id=" << managerId
       << " AND team_id=" << userClubId << " AND season_year=" << seasonYear << " LIMIT 1)"
       << " ORDER BY points DESC LIMIT 2);";
    // Simplified: just check if user's wins were > 60% of fixtures
    std::stringstream wq;
    wq << "SELECT won, played FROM standings WHERE manager_id=" << managerId
       << " AND team_id=" << userClubId << " AND season_year=" << seasonYear << " LIMIT 1;";
    DatabaseResult *wr = GetDB()->Query(wq.str().c_str());
    if (wr && wr->data.size() > 0) {
      int won    = atoi(UTCell(wr,0,0).c_str());
      int played = atoi(UTCell(wr,0,1).c_str());
      if (played > 0 && (float)won / (float)played >= 0.60f) triggered = true;
    }
    if (wr) delete wr; }

  // Trigger 2: best player has high rep
  { std::stringstream bq;
    bq << "SELECT international_reputation FROM players WHERE team_id=" << userClubId
       << " ORDER BY base_stat DESC LIMIT 1;";
    DatabaseResult *br = GetDB()->Query(bq.str().c_str());
    if (br && br->data.size() > 0 && atoi(UTCell(br,0,0).c_str()) >= 4) triggered = true;
    if (br) delete br; }

  if (!triggered) return;

  // Get top 3 players by base_stat
  std::stringstream tq;
  tq << "SELECT id FROM players WHERE team_id=" << userClubId << " ORDER BY base_stat DESC LIMIT 3;";
  DatabaseResult *tr = GetDB()->Query(tq.str().c_str());
  if (!tr) return;

  for (unsigned int i = 0; i < tr->data.size(); i++) {
    int pid = atoi(UTCell(tr,i,0).c_str());

    // Bump knowledge to 90 for elite clubs
    std::stringstream uq;
    uq << "UPDATE club_player_knowledge SET knowledge=90"
       << " WHERE manager_id=" << managerId << " AND player_id=" << pid
       << " AND club_id IN (SELECT id FROM teams WHERE international_prestige >= 7);";
    UTExec(uq.str());

    // Temporarily boost aggression for those clubs
    uq.str("");
    uq << "UPDATE club_transfer_identity SET aggression=MIN(100,aggression+25)"
       << " WHERE manager_id=" << managerId
       << " AND club_id IN (SELECT id FROM teams WHERE international_prestige >= 7);";
    UTExec(uq.str());

    std::string pname;
    { std::stringstream pnq; pnq << "SELECT firstname||' '||lastname FROM players WHERE id=" << pid << ";";
      DatabaseResult *pnr = GetDB()->Query(pnq.str().c_str());
      if (pnr && pnr->data.size() > 0) pname = UTCell(pnr,0,0);
      if (pnr) delete pnr; }
    InsertInboxMessage(managerId, "Elite clubs circling " + pname,
      pname + " has attracted serious interest from top clubs after an impressive season. Expect approaches.",
      "transfer", currentDate);
  }
  delete tr;

  printf("[TRANSFER] ProcessPoachingEscalation: manager=%d userClub=%d triggered\n",
         managerId, userClubId);
}
