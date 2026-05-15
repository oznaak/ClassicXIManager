#pragma once

// Session-scoped context for a scheduled career fixture.
// Set by PlayFixture(), cleared by Test Engine and after result is stored.
// Read by GameOverPage to capture the final score and update the DB.

struct CareerMatchContext {
  bool active    = false;
  int managerId  = 0;
  int fixtureId  = 0;
  int leagueId   = 0;
  int homeTeamId = 0;
  int awayTeamId = 0;

  void Clear() { *this = CareerMatchContext(); }
};

extern CareerMatchContext g_CareerMatchContext;

// Store match result and update standings.
// No-op if fixture is already 'played'.
void CompleteScheduledFixture(int managerId, int fixtureId,
                              int homeScore, int awayScore);
