#pragma once

#include <string>

// Session-scoped context for a scheduled career fixture.
// Set by PlayFixture() and cleared after result is stored.
// Read by GameOverPage to capture the final score and update the DB.

struct CareerMatchContext {
  bool active    = false;
  int managerId  = 0;
  int fixtureId  = 0;
  int leagueId   = 0;
  int homeTeamId = 0;
  int awayTeamId = 0;
  int userClubId = 0; // persists after g_CareerHub is cleared when page exits

  void Clear() { *this = CareerMatchContext(); }
};

extern CareerMatchContext g_CareerMatchContext;

// Store match result and update standings.
// No-op if fixture is already 'played'.
// statsJson is stored verbatim in fixtures.stats_json (defaults to "{}").
void CompleteScheduledFixture(int managerId, int fixtureId,
                              int homeScore, int awayScore,
                              const std::string &statsJson = "{}");
