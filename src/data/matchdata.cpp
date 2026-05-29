// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "matchdata.hpp"

MatchData::MatchData(int team1DatabaseID, int team2DatabaseID) {
  teamData[0] = new TeamData(team1DatabaseID);
  teamData[1] = new TeamData(team2DatabaseID);

  goalCount[0] = 0;
  goalCount[1] = 0;

  possessionTime_ms[0] = 0;
  possessionTime_ms[1] = 0;

  shots[0] = 0;
  shots[1] = 0;
  shotsOnTarget[0] = 0; shotsOnTarget[1] = 0;
  corners[0] = 0;       corners[1] = 0;
  fouls[0] = 0;         fouls[1] = 0;
  offsides[0] = 0;      offsides[1] = 0;
  yellowCards[0] = 0;   yellowCards[1] = 0;
  redCards[0] = 0;      redCards[1] = 0;
  passesAttempted[0] = 0; passesAttempted[1] = 0;
  passesCompleted[0] = 0; passesCompleted[1] = 0;
  pendingPassTeamID = -1;
  lastIntentionalTouch.teamID = -1;
  lastIntentionalTouch.playerDatabaseID = 0;
  lastIntentionalTouch.time_ms = 0;
  previousIntentionalTouch.teamID = -1;
  previousIntentionalTouch.playerDatabaseID = 0;
  previousIntentionalTouch.time_ms = 0;

  possession60seconds = 0.0f;
}

MatchData::~MatchData() {
  delete teamData[0];
  delete teamData[1];
}

void MatchData::AddGoalEvent(int teamID, int scorerDatabaseID, int assistDatabaseID, bool ownGoal) {
  GoalEvent event;
  event.teamID = teamID;
  event.scorerDatabaseID = scorerDatabaseID;
  event.assistDatabaseID = assistDatabaseID;
  event.ownGoal = ownGoal;
  goalEvents.push_back(event);
}

int MatchData::GetAssistCandidate(int teamID, int scorerDatabaseID, unsigned long goalTime_ms) const {
  if (previousIntentionalTouch.teamID != teamID) return 0;
  if (previousIntentionalTouch.playerDatabaseID <= 0) return 0;
  if (previousIntentionalTouch.playerDatabaseID == scorerDatabaseID) return 0;
  if (goalTime_ms < previousIntentionalTouch.time_ms) return 0;
  if (goalTime_ms - previousIntentionalTouch.time_ms > 10000) return 0;
  return previousIntentionalTouch.playerDatabaseID;
}

void MatchData::AddPossessionTime_10ms(int teamID) {
  possessionTime_ms[teamID] += 10;
  if (teamID == 0) possession60seconds = std::max(possession60seconds - 0.01f, -60.0f);
  else if (teamID == 1) possession60seconds = std::min(possession60seconds + 0.01f, 60.0f);
  //printf("pos60s: %f\n", possession60seconds);
}
