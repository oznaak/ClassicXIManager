// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#ifndef _HPP_MATCHDATA
#define _HPP_MATCHDATA

#include "defines.hpp"

#include "../gamedefines.hpp"

#include "teamdata.hpp"

class MatchData {

  public:
    MatchData(int team1DatabaseID, int team2DatabaseID);
    virtual ~MatchData();

    TeamData *GetTeamData(int id) { return teamData[id]; }
    int GetGoalCount(int id) { return goalCount[id]; }
    void SetGoalCount(int id, int amount) { goalCount[id] = amount; }
    void AddPossessionTime_10ms(int teamID);
    unsigned long GetPossessionTime_ms(int teamID) { return possessionTime_ms[teamID]; }
    float GetPossessionFactor_60seconds() { return possession60seconds / 60.0f * 0.5f + 0.5f; } // REMEMBER THESE ARE IRL INGAME SECONDS (because, I guess the tactics should be based on irl possession time instead of gametime? not sure yet, think about this)
    void AddShot(int teamID) { shots[teamID] += 1; }
    int GetShots(int teamID) { return shots[teamID]; }
    void AddShotOnTarget(int teamID) { shotsOnTarget[teamID]++; }
    int GetShotsOnTarget(int teamID) { return shotsOnTarget[teamID]; }

    void AddCorner(int teamID) { corners[teamID]++; }
    int GetCorners(int teamID) { return corners[teamID]; }
    void AddFoul(int teamID) { fouls[teamID]++; }
    int GetFouls(int teamID) { return fouls[teamID]; }
    void AddOffside(int teamID) { offsides[teamID]++; }
    int GetOffsides(int teamID) { return offsides[teamID]; }
    void AddYellowCard(int teamID) { yellowCards[teamID]++; }
    int GetYellowCards(int teamID) { return yellowCards[teamID]; }
    void AddRedCard(int teamID) { redCards[teamID]++; }
    int GetRedCards(int teamID) { return redCards[teamID]; }

    // Ball-touch notification for pass-completion tracking.
    // Call before any ball touch; call RecordPassAttempt after a pass fires.
    void RecordBallTouch(int teamID) {
      if (pendingPassTeamID >= 0) {
        if (teamID == pendingPassTeamID) passesCompleted[pendingPassTeamID]++;
        pendingPassTeamID = -1;
      }
    }
    void RecordPassAttempt(int teamID) {
      passesAttempted[teamID]++;
      pendingPassTeamID = teamID;
    }
    int GetPassesAttempted(int teamID) { return passesAttempted[teamID]; }
    int GetPassesCompleted(int teamID) { return passesCompleted[teamID]; }

  protected:
    TeamData *teamData[2];

    int goalCount[2];

    unsigned long possessionTime_ms[2];
    float possession60seconds; // -600 to 600 for possession of team 1 / 2 respectively
    int shots[2];
    int shotsOnTarget[2];
    int corners[2];
    int fouls[2];
    int offsides[2];
    int yellowCards[2];
    int redCards[2];
    int passesAttempted[2];
    int passesCompleted[2];
    int pendingPassTeamID;

};

#endif
