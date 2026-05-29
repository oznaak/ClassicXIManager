// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "gameover.hpp"

#include "main.hpp"

#include "../pagefactory.hpp"
#include "../careermatchcontext.hpp"
#include "../imgui_match.hpp"
#include "utils/database.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <sstream>

using namespace blunted;

namespace {

float RoleStaminaLoad(e_PlayerRole role) {
  if (role == e_PlayerRole_GK) return 0.25f;
  if (role == e_PlayerRole_CB) return 0.82f;
  if (role == e_PlayerRole_LB || role == e_PlayerRole_RB) return 1.12f;
  if (role == e_PlayerRole_DM || role == e_PlayerRole_CM) return 1.08f;
  if (role == e_PlayerRole_LM || role == e_PlayerRole_RM) return 1.15f;
  if (role == e_PlayerRole_AM) return 1.05f;
  return 1.00f;
}

int ClampCondition(int value) {
  return clamp(value, 1, 100);
}

bool DBHasColumn(const std::string &table, const std::string &column) {
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

std::string SQLQuote(const std::string &value) {
  std::string out;
  for (unsigned int i = 0; i < value.size(); i++) {
    if (value[i] == '\'') out += "''";
    else out += value[i];
  }
  return out;
}

std::string DBCell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size() || col >= r->data.at(row).size()) return "";
  return r->data.at(row).at(col);
}

int CurrentFixtureSeasonYear(int managerId, int fixtureId) {
  std::stringstream q;
  q << "SELECT season_year FROM fixtures WHERE manager_id=" << managerId
    << " AND id=" << fixtureId << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str());
  int seasonYear = 0;
  if (r && !r->data.empty()) seasonYear = atoi(DBCell(r, 0, 0).c_str());
  delete r;
  return seasonYear;
}

std::string MatchStatsJson(Match *match) {
  MatchData *md = match->GetMatchData();
  unsigned long poss0 = md->GetPossessionTime_ms(0);
  unsigned long poss1 = md->GetPossessionTime_ms(1);
  unsigned long possTotal = std::max((unsigned long)1, poss0 + poss1);
  int passPct0 = md->GetPassesAttempted(0) > 0
    ? (int)round(md->GetPassesCompleted(0) * 100.0f / md->GetPassesAttempted(0))
    : 0;
  int passPct1 = md->GetPassesAttempted(1) > 0
    ? (int)round(md->GetPassesCompleted(1) * 100.0f / md->GetPassesAttempted(1))
    : 0;

  std::stringstream ss;
  ss << "{"
     << "\"home\":{\"possession\":" << (int)round(poss0 * 100.0f / possTotal)
     << ",\"shots\":" << md->GetShots(0)
     << ",\"shots_on_target\":" << md->GetShotsOnTarget(0)
     << ",\"corners\":" << md->GetCorners(0)
     << ",\"fouls\":" << md->GetFouls(0)
     << ",\"offsides\":" << md->GetOffsides(0)
     << ",\"passes\":" << md->GetPassesAttempted(0)
     << ",\"passes_completed\":" << md->GetPassesCompleted(0)
     << ",\"pass_accuracy\":" << passPct0
     << ",\"yellow_cards\":" << md->GetYellowCards(0)
     << ",\"red_cards\":" << md->GetRedCards(0) << "},"
     << "\"away\":{\"possession\":" << (int)round(poss1 * 100.0f / possTotal)
     << ",\"shots\":" << md->GetShots(1)
     << ",\"shots_on_target\":" << md->GetShotsOnTarget(1)
     << ",\"corners\":" << md->GetCorners(1)
     << ",\"fouls\":" << md->GetFouls(1)
     << ",\"offsides\":" << md->GetOffsides(1)
     << ",\"passes\":" << md->GetPassesAttempted(1)
     << ",\"passes_completed\":" << md->GetPassesCompleted(1)
     << ",\"pass_accuracy\":" << passPct1
     << ",\"yellow_cards\":" << md->GetYellowCards(1)
     << ",\"red_cards\":" << md->GetRedCards(1) << "}"
     << "}";
  return ss.str();
}

int ExistingSeasonYellows(int managerId, int playerId, int seasonYear) {
  std::stringstream q;
  q << "SELECT yellow_cards FROM player_discipline WHERE manager_id=" << managerId
    << " AND player_id=" << playerId << " AND season_year=" << seasonYear
    << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str());
  int yellows = 0;
  if (r && !r->data.empty()) yellows = atoi(DBCell(r, 0, 0).c_str());
  delete r;
  return yellows;
}

int BestPhysioRating(int managerId) {
  std::stringstream q;
  q << "SELECT MAX(sl.rating) FROM career_staff cs"
    << " JOIN staff_list sl ON sl.staff_id=cs.staff_id"
    << " WHERE cs.manager_id=" << managerId
    << " AND sl.role='Physio';";
  DatabaseResult *r = GetDB()->Query(q.str());
  int rating = 0;
  if (r && !r->data.empty()) rating = atoi(DBCell(r, 0, 0).c_str());
  delete r;
  return clamp(rating, 0, 5);
}

float PhysioRecoveryFactor(int rating) {
  if (rating <= 0) return 1.0f;
  if (rating == 1) return 0.96f;
  if (rating == 2) return 0.91f;
  if (rating == 3) return 0.85f;
  if (rating == 4) return 0.78f;
  return 0.70f;
}

void PersistPlayerDiscipline(int managerId, int playerId, int seasonYear,
                             int yellows, int reds) {
  if (playerId <= 0 || (yellows <= 0 && reds <= 0)) return;

  int oldYellows = ExistingSeasonYellows(managerId, playerId, seasonYear);
  int suspensionAdd = 0;
  std::string reason;
  if (reds > 0) {
    suspensionAdd += 2;
    reason = "straight red";
  } else if (yellows >= 2) {
    suspensionAdd += 1;
    reason = "two yellows";
  }
  if (yellows > 0 && oldYellows / 5 < (oldYellows + yellows) / 5) {
    suspensionAdd += 1;
    reason = reason.empty() ? "yellow accumulation" : reason + " + accumulation";
  }

  std::stringstream q;
  q << "INSERT INTO player_discipline(manager_id,player_id,season_year,yellow_cards,"
    << "suspension_matches_remaining,suspension_reason) VALUES("
    << managerId << "," << playerId << "," << seasonYear << ","
    << yellows << "," << suspensionAdd << ",'" << SQLQuote(reason) << "') "
    << "ON CONFLICT(manager_id,player_id,season_year) DO UPDATE SET "
    << "yellow_cards=yellow_cards+" << yellows << ","
    << "suspension_matches_remaining=suspension_matches_remaining+" << suspensionAdd;
  if (!reason.empty()) q << ",suspension_reason='" << SQLQuote(reason) << "'";
  q << ";";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
}

void MaybePersistInjury(int managerId, int fixtureId, int playerId,
                        Player *player, int minutes, int aggressionBias,
                        float physioRecoveryFactor) {
  if (playerId <= 0 || minutes <= 0 || !player) return;

  float condition = player->GetFatigueFactorInv() * 100.0f;
  float stamina = player->GetStat("physical_stamina");
  float resilience = player->GetStat("mental_resilience");
  float risk = 1.4f;
  risk += std::max(0.0f, 70.0f - condition) * 0.10f;
  risk += std::max(0.0f, 0.62f - stamina) * 6.0f;
  risk += std::max(0.0f, 0.58f - resilience) * 4.0f;
  risk += std::max(0, aggressionBias) * 0.7f;
  if (minutes >= 80) risk += 0.8f;

  int seed = fixtureId * 1103515245 + playerId * 97 + minutes * 13;
  float roll = (float)(abs(seed) % 10000) / 100.0f;
  if (roll > risk) return;

  int days = 3 + (abs(seed / 17) % 8);
  std::string severity = "minor";
  std::string type = "knock";
  if (condition < 40.0f || roll < risk * 0.30f) {
    days += 7 + (abs(seed / 31) % 12);
    severity = "moderate";
    type = "muscle strain";
  }
  if (condition < 25.0f && roll < risk * 0.12f) {
    days += 14 + (abs(seed / 43) % 18);
    severity = "major";
    type = "muscle tear";
  }
  days = std::max(1, (int)round(days * physioRecoveryFactor));

  std::stringstream q;
  q << "INSERT INTO player_availability(manager_id,player_id,injury_days_remaining,"
    << "injury_type,injury_severity) VALUES("
    << managerId << "," << playerId << "," << days << ",'"
    << SQLQuote(type) << "','" << SQLQuote(severity) << "') "
    << "ON CONFLICT(manager_id,player_id) DO UPDATE SET "
    << "injury_days_remaining=MAX(injury_days_remaining," << days << "),"
    << "injury_type=CASE WHEN injury_days_remaining<" << days << " THEN '"
    << SQLQuote(type) << "' ELSE injury_type END,"
    << "injury_severity=CASE WHEN injury_days_remaining<" << days << " THEN '"
    << SQLQuote(severity) << "' ELSE injury_severity END;";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
}

void PersistWatchedPlayerStats(Match *match, int managerId, int fixtureId, int seasonYear) {
  if (!match || managerId <= 0 || fixtureId <= 0 || seasonYear <= 0) return;

  float physioRecoveryFactor = PhysioRecoveryFactor(BestPhysioRating(managerId));
  std::map<int, int> goalsByPlayer;
  std::map<int, int> assistsByPlayer;
  const std::vector<GoalEvent> &events = match->GetMatchData()->GetGoalEvents();
  for (unsigned int i = 0; i < events.size(); i++) {
    if (!events.at(i).ownGoal && events.at(i).scorerDatabaseID > 0) goalsByPlayer[events.at(i).scorerDatabaseID]++;
    if (events.at(i).assistDatabaseID > 0) assistsByPlayer[events.at(i).assistDatabaseID]++;
  }

  for (int teamId = 0; teamId < 2; teamId++) {
    Team *team = match->GetTeam(teamId);
    if (!team || !team->GetTeamData()) continue;
    int clubId = team->GetTeamData()->GetDatabaseID();
    const std::vector<Player*> &players = team->GetAllPlayers();
    for (unsigned int i = 0; i < players.size(); i++) {
      Player *player = players.at(i);
      if (!player || !player->GetPlayerData()) continue;

      int playerId = player->GetPlayerData()->GetDatabaseID();
      if (playerId <= 0) continue;
      int minutes = (int)round(player->GetPlayedMatchTime_ms() / 60000.0f);
      int cards = player->GetCards();
      int reds = cards >= 3 ? 1 : 0;
      int yellows = reds ? 0 : std::min(cards, 2);
      int goals = goalsByPlayer[playerId];
      int assists = assistsByPlayer[playerId];
      if (minutes <= 0 && yellows <= 0 && reds <= 0 && goals <= 0 && assists <= 0) continue;

      float rating = 6.45f + std::min(minutes, 90) / 90.0f * 0.12f
                   + goals * 1.05f + assists * 0.55f
                   - yellows * 0.22f - reds * 0.80f;
      if (rating < 3.0f) rating = 3.0f;
      if (rating > 10.0f) rating = 10.0f;

      std::stringstream q;
      q << "INSERT OR REPLACE INTO player_match_stats("
        << "manager_id,fixture_id,season_year,player_id,team_id,started,minutes,"
        << "goals,assists,yellow_cards,red_cards,rating) VALUES("
        << managerId << "," << fixtureId << "," << seasonYear << ","
        << playerId << "," << clubId << "," << (minutes >= 45 ? 1 : 0) << ","
        << minutes << "," << goals << "," << assists << ","
        << yellows << "," << reds << "," << rating << ");";
      DatabaseResult *r = GetDB()->Query(q.str());
      delete r;

      PersistPlayerDiscipline(managerId, playerId, seasonYear, yellows, reds);
      int aggr = (clubId == g_CareerMatchContext.userClubId) ? g_MatchPlanAggression : 0;
      MaybePersistInjury(managerId, fixtureId, playerId, player, minutes, aggr,
                         physioRecoveryFactor);
    }
  }
}

void PersistWatchedMatchStamina(Match *match) {
  if (!match || GetConfiguration()->GetReal("manager_mode", 0.0f) <= 0.5f) return;
  bool hasSaveStamina = DBHasColumn("player_save_state", "player_stamina") &&
                        g_CareerMatchContext.managerId > 0;
  bool hasPlayerStamina = DBHasColumn("players", "player_stamina");
  if (!hasSaveStamina && !hasPlayerStamina) return;

  for (int teamId = 0; teamId < 2; teamId++) {
    Team *team = match->GetTeam(teamId);
    if (!team) continue;

    const std::vector<Player*> &players = team->GetAllPlayers();
    for (unsigned int i = 0; i < players.size(); i++) {
      Player *player = players.at(i);
      if (!player || !player->GetPlayerData()) continue;

      int playerId = player->GetPlayerData()->GetDatabaseID();
      if (playerId <= 0) continue;

      int startCondition = ClampCondition(player->GetPlayerData()->GetCurrentCondition());
      float playedMinutes = player->GetPlayedMatchTime_ms() / 60000.0f;
      int newCondition = startCondition;

      if (playedMinutes > 0.5f) {
        float staminaAttr = player->GetPlayerData()->GetStat("physical_stamina");
        float roleLoad = RoleStaminaLoad(player->GetDynamicFormationEntry().role);
        float minutesFactor = clamp(playedMinutes / 90.0f, 0.0f, 1.35f);
        float baseLoss = (11.0f + (1.0f - staminaAttr) * 6.0f) * roleLoad * minutesFactor;
        float liveDrain = std::min(8.0f, std::max(0.0f, player->GetStartingFatigueFactorInv() - player->GetFatigueFactorInv()) * 12.0f);
        float overuse = startCondition < 70 ? 1.0f + (70 - startCondition) * 0.01f : 1.0f;
        int loss = (int)round((baseLoss + liveDrain) * overuse);
        if (playedMinutes < 20.0f) loss = std::max(1, (int)round(loss * 0.55f));
        newCondition = ClampCondition(startCondition - loss);
      } else if (i >= 11) {
        newCondition = ClampCondition(startCondition + 2);
      }

      if (hasSaveStamina) {
        int clubId = team->GetTeamData() ? team->GetTeamData()->GetDatabaseID() : 0;
        std::stringstream iq;
        iq << "INSERT OR IGNORE INTO player_save_state(manager_id,player_id,team_id,player_stamina)"
           << " VALUES(" << g_CareerMatchContext.managerId << "," << playerId << ","
           << clubId << "," << newCondition << ");";
        DatabaseResult *ir = GetDB()->Query(iq.str());
        delete ir;

        std::stringstream uq;
        uq << "UPDATE player_save_state SET player_stamina=" << newCondition
           << " WHERE manager_id=" << g_CareerMatchContext.managerId
           << " AND player_id=" << playerId << ";";
        DatabaseResult *ur = GetDB()->Query(uq.str());
        delete ur;
      } else {
        std::stringstream uq;
        uq << "UPDATE players SET player_stamina=" << newCondition
           << " WHERE id=" << playerId << ";";
        DatabaseResult *ur = GetDB()->Query(uq.str());
        delete ur;
      }
    }
  }
}

}

GameOverPage::GameOverPage(Gui2WindowManager *windowManager, const Gui2PageData &pageData) : Gui2Page(windowManager, pageData) {

  match = GetGameTask()->GetMatch();
  match->Pause(true);

  // Reset speed multiplier so the next match starts at normal speed.
  GetConfiguration()->Set("match_speed_multiplier", 1.0f);

  int homeScore = match->GetMatchData()->GetGoalCount(0);
  int awayScore = match->GetMatchData()->GetGoalCount(1);
  PersistWatchedMatchStamina(match);

  // Capture result for scheduled career fixture.
  if (g_CareerMatchContext.active) {
    int seasonYear = CurrentFixtureSeasonYear(g_CareerMatchContext.managerId,
                                              g_CareerMatchContext.fixtureId);
    std::string statsJson = MatchStatsJson(match);
    printf("[CAREER MATCH] Finished fixture=%d score=%d-%d\n",
           g_CareerMatchContext.fixtureId, homeScore, awayScore);
    CompleteScheduledFixture(g_CareerMatchContext.managerId,
                             g_CareerMatchContext.fixtureId,
                             homeScore, awayScore,
                             statsJson);
    PersistWatchedPlayerStats(match,
                              g_CareerMatchContext.managerId,
                              g_CareerMatchContext.fixtureId,
                              seasonYear);
    // Context cleared in GoMainMenu after navigation is queued.
  } else {
    printf("[CAREER MATCH] No active fixture context; skipping result processing\n");
  }

  Gui2Image *bg1 = new Gui2Image(windowManager, "image_gameover_bg", 10, 10, 80, 80);
  this->AddView(bg1);
  bg1->LoadImage("media/menu/backgrounds/black.png");
  bg1->Show();

  std::string scoreStr = match->GetTeam(0)->GetTeamData()->GetName() + " " + int_to_str(homeScore) + " - " + int_to_str(awayScore) + " " + match->GetTeam(1)->GetTeamData()->GetName();
  Gui2Caption *header = new Gui2Caption(windowManager, "caption_gameover_header", 0, 15, 80, 4, scoreStr);
  header->SetPosition(50 - header->GetTextWidthPercent() / 2, 15);
  this->AddView(header);
  header->Show();

  buttonOkay = new Gui2Button(windowManager, "button_gameover_ok", 40, 82, 20, 3, "well then");
  this->AddView(buttonOkay);
  buttonOkay->Show();
  buttonOkay->sig_OnClick.connect(boost::bind(&GameOverPage::GoMainMenu, this));

  float possession1 = match->GetMatchData()->GetPossessionTime_ms(0);
  float possession2 = match->GetMatchData()->GetPossessionTime_ms(1);
  int shots1 = match->GetMatchData()->GetShots(0);
  int shots2 = match->GetMatchData()->GetShots(1);

  Gui2Grid *grid = new Gui2Grid(windowManager, "grid_gameover_stats", 15, 25, 70, 50);

  grid->AddView(new Gui2Caption(windowManager, "caption_possession_t1", 0, 0, 25, 3, int_to_str(round(possession1 / (possession1 + possession2) * 100)) + "%"), 0, 0);
  grid->AddView(new Gui2Caption(windowManager, "caption_possession_header", 0, 0, 35, 3, "possession"), 0, 1);
  grid->AddView(new Gui2Caption(windowManager, "caption_possession_t2", 0, 0, 10, 3, int_to_str(round(possession2 / (possession1 + possession2) * 100)) + "%"), 0, 2);

  grid->AddView(new Gui2Caption(windowManager, "caption_shots_t1", 0, 0, 25, 3, int_to_str(shots1)), 1, 0);
  grid->AddView(new Gui2Caption(windowManager, "caption_shots_header", 0, 0, 35, 3, "shots"), 1, 1);
  grid->AddView(new Gui2Caption(windowManager, "caption_shots_t2", 0, 0, 10, 3, int_to_str(shots2)), 1, 2);

  grid->UpdateLayout(0.5);

  this->AddView(grid);
  grid->Show();


  buttonOkay->SetFocus();

  this->Show();
}

GameOverPage::~GameOverPage() {
}

void GameOverPage::GoRematch() {
  windowManager->GetPagePath()->Clear();

  GetGameTask()->Action(e_GameTaskMessage_StopMatch);
  GetGameTask()->Action(e_GameTaskMessage_StartMatch);

  this->Exit();
  Properties properties;
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_Game, properties, 0);
  delete this;
}

void GameOverPage::GoMainMenu() {
  this->Exit();
  if (g_CareerMatchContext.active) {
    int managerId = g_CareerMatchContext.managerId;
    g_CareerMatchContext.Clear();
    // Queue career hub creation inside the menuAction stop-match block.
    GetMenuTask()->RequestReturnToCareerAfterMatch(managerId);
  }
  GetMenuTask()->SetMenuAction(e_MenuAction_Menu);
  delete this;
}
