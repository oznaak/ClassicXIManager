#include "careermatchcontext.hpp"

#include "main.hpp"
#include "utils/database.hpp"

#include <sstream>
#include <cstdio>
#include <cstdlib>

CareerMatchContext g_CareerMatchContext;

static std::string DBCell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size() || col >= r->data.at(row).size()) return "";
  return r->data.at(row).at(col);
}

void CompleteScheduledFixture(int managerId, int fixtureId,
                              int homeScore, int awayScore,
                              const std::string &statsJson) {
  // Guard: verify fixture exists and is not already played.
  std::stringstream chk;
  chk << "SELECT id, league_id, home_team_id, away_team_id, status, season_year"
      << " FROM fixtures WHERE id=" << fixtureId
      << " AND manager_id=" << managerId << " LIMIT 1;";
  DatabaseResult *cr = GetDB()->Query(chk.str());
  if (cr->data.size() == 0) {
    printf("[FIXTURE] Fixture %d not found for manager %d; skipping\n",
           fixtureId, managerId);
    delete cr;
    return;
  }
  std::string status   = DBCell(cr, 0, 4);
  int leagueId   = atoi(DBCell(cr, 0, 1).c_str());
  int homeTeamId = atoi(DBCell(cr, 0, 2).c_str());
  int awayTeamId = atoi(DBCell(cr, 0, 3).c_str());
  int seasonYear = atoi(DBCell(cr, 0, 5).c_str());
  delete cr;

  if (status == "played") {
    printf("[FIXTURE] Fixture %d already played; skipping standings update\n", fixtureId);
    return;
  }

  // Escape single quotes in statsJson for safe embedding.
  std::string safeStats;
  for (unsigned int si = 0; si < statsJson.size(); si++) {
    if (statsJson[si] == '\'') safeStats += "''";
    else safeStats += statsJson[si];
  }

  // Mark fixture played.
  std::stringstream uq;
  uq << "UPDATE fixtures SET status='played',"
     << " home_score=" << homeScore << ","
     << " away_score=" << awayScore << ","
     << " stats_json='" << safeStats << "',"
     << " played_at=datetime('now')"
     << " WHERE id=" << fixtureId << " AND manager_id=" << managerId << ";";
  DatabaseResult *ur = GetDB()->Query(uq.str());
  delete ur;
  printf("[FIXTURE] Marked played fixture=%d score=%d-%d\n",
         fixtureId, homeScore, awayScore);

  // Compute outcome flags.
  int homeWin  = (homeScore > awayScore) ? 1 : 0;
  int awayWin  = (awayScore > homeScore) ? 1 : 0;
  int isDraw   = (homeScore == awayScore) ? 1 : 0;
  int homeGD   = homeScore - awayScore;
  int awayGD   = awayScore - homeScore;
  int homePts  = homeWin ? 3 : (isDraw ? 1 : 0);
  int awayPts  = awayWin ? 3 : (isDraw ? 1 : 0);

  // Update home team standings.
  std::stringstream hq;
  hq << "UPDATE standings SET"
     << " played=played+1,"
     << " won=won+"       << homeWin  << ","
     << " drawn=drawn+"   << isDraw   << ","
     << " lost=lost+"     << awayWin  << ","
     << " goals_for=goals_for+"         << homeScore << ","
     << " goals_against=goals_against+" << awayScore << ","
     << " goal_difference=goal_difference+" << homeGD << ","
     << " points=points+" << homePts
     << " WHERE manager_id=" << managerId
     << " AND league_id="   << leagueId
     << " AND team_id="     << homeTeamId
     << " AND season_year=" << seasonYear << ";";
  DatabaseResult *hr = GetDB()->Query(hq.str());
  delete hr;

  // Update away team standings.
  std::stringstream aq;
  aq << "UPDATE standings SET"
     << " played=played+1,"
     << " won=won+"       << awayWin  << ","
     << " drawn=drawn+"   << isDraw   << ","
     << " lost=lost+"     << homeWin  << ","
     << " goals_for=goals_for+"         << awayScore << ","
     << " goals_against=goals_against+" << homeScore << ","
     << " goal_difference=goal_difference+" << awayGD << ","
     << " points=points+" << awayPts
     << " WHERE manager_id=" << managerId
     << " AND league_id="   << leagueId
     << " AND team_id="     << awayTeamId
     << " AND season_year=" << seasonYear << ";";
  DatabaseResult *ar = GetDB()->Query(aq.str());
  delete ar;

  printf("[STANDINGS] Updated manager=%d league=%d home=%d away=%d score=%d-%d\n",
         managerId, leagueId, homeTeamId, awayTeamId, homeScore, awayScore);
}
