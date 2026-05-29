// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "gameover.hpp"

#include "main.hpp"

#include "../pagefactory.hpp"
#include "../careermatchcontext.hpp"
#include "utils/database.hpp"

#include <algorithm>
#include <cmath>
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

void PersistWatchedMatchStamina(Match *match) {
  if (!match || GetConfiguration()->GetReal("manager_mode", 0.0f) <= 0.5f) return;
  if (!DBHasColumn("players", "player_stamina")) return;

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

      std::stringstream uq;
      uq << "UPDATE players SET player_stamina=" << newCondition
         << " WHERE id=" << playerId << ";";
      DatabaseResult *ur = GetDB()->Query(uq.str());
      delete ur;
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
    printf("[CAREER MATCH] Finished fixture=%d score=%d-%d\n",
           g_CareerMatchContext.fixtureId, homeScore, awayScore);
    CompleteScheduledFixture(g_CareerMatchContext.managerId,
                             g_CareerMatchContext.fixtureId,
                             homeScore, awayScore);
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
