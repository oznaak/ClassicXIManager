// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "team.hpp"

#include "match.hpp"

#include "../gamedefines.hpp"
#include "../utils.hpp"
#include "../main.hpp"

#include "AIsupport/AIfunctions.hpp"

#include "managers/resourcemanagerpool.hpp"

#include "base/sdl_surface.hpp"

static int ClampColor(float v) { int i = (int)v; return i < 0 ? 0 : (i > 255 ? 255 : i); }

// Creates a banded 1024x1024 kit texture: shirt (rows 0-539), shorts (540-761), socks (762-1023).
// Cached by the three colors so each unique combination is only created once.
static boost::intrusive_ptr<Resource<Surface>> MakeColorKit(
    const Vector3 &shirtColor,
    const Vector3 &shortsColor,
    const Vector3 &socksColor) {
  int sr = ClampColor(shirtColor.coords[0]),  sg = ClampColor(shirtColor.coords[1]),  sb = ClampColor(shirtColor.coords[2]);
  int kr = ClampColor(shortsColor.coords[0]), kg = ClampColor(shortsColor.coords[1]), kb = ClampColor(shortsColor.coords[2]);
  int pr = ClampColor(socksColor.coords[0]),  pg = ClampColor(socksColor.coords[1]),  pb = ClampColor(socksColor.coords[2]);
  char name[128];
  snprintf(name, sizeof(name), "__kit_s%d_%d_%d__k%d_%d_%d__p%d_%d_%d",
           sr, sg, sb, kr, kg, kb, pr, pg, pb);
  bool alreadyThere = false;
  auto res = ResourceManagerPool::GetInstance()
               .GetManager<Surface>(e_ResourceType_Surface)
               ->Fetch(std::string(name), false, alreadyThere, true);
  if (!alreadyThere) {
    const int W = 1024, H = 1024;
    SDL_Surface *sdl = CreateSDLSurface(W, H);
    Uint32 shirtPx  = SDL_MapRGB(sdl->format, (Uint8)sr, (Uint8)sg, (Uint8)sb);
    Uint32 shortsPx = SDL_MapRGB(sdl->format, (Uint8)kr, (Uint8)kg, (Uint8)kb);
    Uint32 socksPx  = SDL_MapRGB(sdl->format, (Uint8)pr, (Uint8)pg, (Uint8)pb);
    // UV layout (empirically): shirt top, socks middle, shorts bottom
    SDL_Rect shirtRect  = { 0,   0, W, 540 };
    SDL_Rect socksRect  = { 0, 540, W, 222 };
    SDL_Rect shortsRect = { 0, 762, W, 262 };
    SDL_FillRect(sdl, &shirtRect,  shirtPx);
    SDL_FillRect(sdl, &socksRect,  socksPx);
    SDL_FillRect(sdl, &shortsRect, shortsPx);
    res->GetResource()->SetData(sdl);
  }
  return res;
}

// Determines shirt/shorts/socks colors for this team, handling away color clashes.
static void GetKitColors(Team *team, Vector3 &shirtOut, Vector3 &shortsOut, Vector3 &socksOut) {
  Vector3 primary   = team->GetTeamData()->GetColor1();
  Vector3 secondary = team->GetTeamData()->GetColor2();
  shirtOut  = primary;
  shortsOut = secondary;
  socksOut  = primary;
  if (team->GetID() == 1) {
    Vector3 homePrimary = team->GetMatch()->GetTeam(0)->GetTeamData()->GetColor1();
    float dr = primary.coords[0] - homePrimary.coords[0];
    float dg = primary.coords[1] - homePrimary.coords[1];
    float db = primary.coords[2] - homePrimary.coords[2];
    if (dr*dr + dg*dg + db*db < 15000.0f) { // Euclidean distance < ~122 across all channels
      shirtOut  = secondary;
      shortsOut = primary;
      socksOut  = secondary;
    }
  }
}

Team::Team(int id, Match *match, TeamData *teamData) : id(id), match(match), teamData(teamData) {
  assert(id == 0 || id == 1);
  assert(teamData->GetPlayerNum() >= playerNum); // does team have enough players?

  teamNode = boost::intrusive_ptr<Node>(new Node("team node #" + int_to_str(id)));
  teamNode->SetLocalMode(e_LocalMode_Absolute);
  match->GetDynamicNode()->AddNode(teamNode);

  teamController = new TeamAIController(this);

  timeNeededToGetToBall_ms = 100;
  hasPossession = false;

  teamPossessionAmount = 1.0;
  fadingTeamPossessionAmount = 1.0;

  for (unsigned int i = 0; i < e_TouchType_SIZE; i++) {
    lastTouchPlayers[i] = 0;
  }
  lastTouchPlayer = 0;
  lastTouchType = e_TouchType_None;
}

Team::~Team() {
}

void Team::Exit() {

  Hide2D();

  for (unsigned int i = 0; i < humanGamers.size(); i++) {
    delete humanGamers.at(i);
  }
  for (unsigned int i = 0; i < players.size(); i++) {
    delete players.at(i);
  }

  delete teamController;

  playerNode->Exit();
  playerNode.reset();

  match->GetDynamicNode()->DeleteNode(teamNode);
}

void Team::InitPlayers(boost::intrusive_ptr<Node> fullbodyNode, std::map<Vector3, Vector3> &colorCoords) {

  // first, load 1 instance of a player

  Log(e_Notice, "Team", "InitPlayers", "Loading player template instance");

  ObjectLoader loader;
  playerNode = loader.LoadObject(GetScene3D(), "media/objects/players/player.object");
  playerNode->SetName("player");
  playerNode->SetLocalMode(e_LocalMode_Absolute);

  activePlayerCount = playerNum;

  Log(e_Notice, "Team", "Team", "Creating players");

  // load all players in the team, even the players who sit on the bench. aww.
  for (int i = 0; i < (signed int)teamData->GetPlayerNum(); i++) {
    PlayerData *playerData = teamData->GetPlayerData(i);
    Player *player = new Player(this, playerData);
    players.push_back(player);

    if (i < activePlayerCount) {
      // activate playerCount players (the starting eleven, usually)
      std::string kitFilename;
      //printf("%i player id\n", player->GetID());
      if (GetFormationEntry(player->GetID()).role != e_PlayerRole_GK) {
        kitFilename = GetTeamData()->GetKitUrl() + "_kit_0" + int_to_str(GetMenuTask()->GetTeamKitNum(GetID())) + ".png";
        if (!boost::filesystem::exists(kitFilename)) {
          Vector3 shirtC, shortsC, socksC;
          GetKitColors(this, shirtC, shortsC, socksC);
          kit = MakeColorKit(shirtC, shortsC, socksC);
          player->Activate(playerNode, fullbodyNode, colorCoords, kit, match->GetAnimCollection());
          continue;
        }
      } else {
        kitFilename = "media/objects/players/textures/goalie_kit.png";
      }
      kit = ResourceManagerPool::GetInstance().GetManager<Surface>(e_ResourceType_Surface)->Fetch(kitFilename);
      player->Activate(playerNode, fullbodyNode, colorCoords, kit, match->GetAnimCollection());
    } else {
      // pre-create humanoid for bench player on GL thread; hides immediately
      // kit is whatever was set for the last starting player — acceptable for bench
      player->ActivateBench(playerNode, fullbodyNode, colorCoords, kit, match->GetAnimCollection());
    }
  }

  designatedTeamPossessionPlayer = players.at(0);

}

signed int Team::GetSide() {
  signed int side;
  if (id == 0) side = -1;
  if (id == 1) side = 1;

  // -1 == left, 1 == right
  e_MatchPhase phase = match->GetMatchPhase();
  if (phase == e_MatchPhase_2ndHalf || phase == e_MatchPhase_2ndExtraTime) side *= -1;

  return side;
}

Player *Team::GetPlayer(int player_id) {
  for (int i = 0; i < (signed int)players.size(); i++) {
    if (players.at(i)->GetID() == player_id) {
      return players.at(i);
    }
  }

  // id not found
  return 0;
}

PlayerData *Team::GetPlayerData(int playerID) {
  for (int i = 0; i < (signed int)players.size(); i++) {
    if (players.at(i)->GetID() == playerID) {
      return teamData->GetPlayerData(i);
    }
  }

  assert(1 == 2);
  return 0;
}

FormationEntry Team::GetFormationEntry(int playerID) {
  for (int i = 0; i < (signed int)players.size(); i++) {
    if (players.at(i)->GetID() == playerID) {
      return teamData->GetFormationEntry(i);
    }
  }

  assert(1 == 2);
  FormationEntry fail;
  return fail;
}

void Team::SetFormationEntry(int playerID, FormationEntry entry) {
  for (int i = 0; i < (signed int)players.size(); i++) {
    if (players.at(i)->GetID() == playerID) {
      teamData->SetFormationEntry(i, entry);
    }
  }
}

void Team::GetActivePlayers(std::vector<Player*> &activePlayers) {
  for (auto player : players) {
    if (player->IsActive()) activePlayers.push_back(player);
  }
}

void Team::AddHumanGamer(IHIDevice *hid, e_PlayerColor color) {
  HumanGamer *humanGamer = new HumanGamer(this, hid, color);

  humanGamers.push_back(humanGamer);

  humanGamer->SetSelectedPlayerID(AI_GetClosestPlayer(this, match->GetBall()->Predict(0).Get2D(), true)->GetID());

  switchPriority.push_back(humanGamers.size() - 1);
  designatedTeamPossessionPlayer = AI_GetClosestPlayer(this, match->GetBall()->Predict(0).Get2D(), false);
}

void Team::DeleteHumanGamers() {
  for (unsigned int i = 0; i < humanGamers.size(); i++) {
    delete humanGamers.at(i);
  }
  humanGamers.clear();
  switchPriority.clear();
}

e_PlayerColor Team::GetPlayerColor(int playerID) {
  for (unsigned int h = 0; h < humanGamers.size(); h++) {
    if (humanGamers.at(h)->GetSelectedPlayerID() == playerID) return humanGamers.at(h)->GetPlayerColor();
  }
  return e_PlayerColor_Default;
}

bool Team::IsHumanControlled(int playerID) {
  for (unsigned int h = 0; h < humanGamers.size(); h++) {
    if (humanGamers.at(h)->GetSelectedPlayerID() == playerID) return true;
  }
  return false;
}

bool Team::HasPossession() const {
  return hasPossession;
}

bool Team::HasUniquePossession() const {
  return HasPossession() && !match->GetTeam(abs(id - 1))->HasPossession();
}

int Team::GetTimeNeededToGetToBall_ms() const {
  return timeNeededToGetToBall_ms;
}

signed int Team::GetBestPossessionPlayerID() {
  return GetBestPossessionPlayer()->GetID();
}

Player *Team::GetBestPossessionPlayer() {
  int bestTime_ms = 10000000;
  Player *bestPlayer = 0;
  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      int time_ms = players.at(i)->GetTimeNeededToGetToBall_ms();
      if (time_ms < bestTime_ms) {
        bestTime_ms = time_ms;
        bestPlayer = players.at(i);
      }
    }
  }

  assert(bestPlayer);

  return bestPlayer;
}

float Team::GetTeamPossessionAmount() const {
  return teamPossessionAmount;
}

float Team::GetFadingTeamPossessionAmount() const {
  return fadingTeamPossessionAmount;
}

void Team::SetFadingTeamPossessionAmount(float value) {
  fadingTeamPossessionAmount = clamp(value, 0.5, 1.5);
}

void Team::SetLastTouchPlayer(Player *player, e_TouchType touchType) {
  lastTouchPlayers[touchType] = player;
  lastTouchPlayer = player;
  lastTouchType = touchType;
  player->SetLastTouchTime_ms(match->GetActualTime_ms());
  player->SetLastTouchType(lastTouchType);
  match->SetLastTouchTeamID(GetID(), touchType);
}

void Team::ResetSituation(const Vector3 &focusPos) {
  timeNeededToGetToBall_ms = 100;
  hasPossession = false;

  teamPossessionAmount = 1.0f;
  fadingTeamPossessionAmount = 1.0f;

  for (unsigned int i = 0; i < e_TouchType_SIZE; i++) {
    lastTouchPlayers[i] = 0;
  }
  lastTouchPlayer = 0;
  lastTouchType = e_TouchType_None;

  designatedTeamPossessionPlayer = players.at(0);

  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      players.at(i)->ResetSituation(focusPos);
    }
  }

  GetController()->Reset();
}

void Team::HumanGamersSelectAnyone() {
  // make sure all human gamers have a player selected

  if (match->IsInPlay()) {
    for (unsigned int i = 0; i < humanGamers.size(); i++) {
      if (humanGamers.at(i)->GetSelectedPlayerID() == -1) {
        int playerID = AI_GetClosestPlayer(this, match->GetBall()->Predict(0).Get2D(), true)->GetID();
        humanGamers.at(i)->SetSelectedPlayerID(playerID);
      }
    }
  }
}

void Team::SelectPlayer(Player *player) {
  //printf("trying to switch to %s\n", player->GetPlayerData()->GetLastName().c_str());
  if (!IsHumanControlled(player->GetID()) && humanGamers.size() != 0) { // already selected
    humanGamers.at(*switchPriority.begin())->SetSelectedPlayerID(player->GetID());
    switchPriority.push_back(*switchPriority.begin());
    switchPriority.pop_front();
    if (Verbose()) printf("switched player to %s\n", player->GetPlayerData()->GetLastName().c_str());
  }
  designatedTeamPossessionPlayer = player;
}

void Team::DeselectPlayer(Player *player) {
  for (unsigned int i = 0; i < humanGamers.size(); i++) {
    int selectedPlayerID = humanGamers.at(i)->GetSelectedPlayerID();
    if (selectedPlayerID == player->GetID()) {
      Player *somePlayer = AI_GetClosestPlayer(this, player->GetPosition(), true, player);
      if (somePlayer) {
        humanGamers.at(i)->SetSelectedPlayerID(somePlayer->GetID());
      } else {
        humanGamers.at(i)->SetSelectedPlayerID(-1);
      }
    }
  }
}

void Team::RelaxFatigue(float howMuch) {
  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      players.at(i)->RelaxFatigue(howMuch);
    }
  }
}

void Team::Process() {

  if (!match->GetPause()) {

    teamPossessionAmount = (float)(match->GetTeam(abs(GetID() - 1))->GetTimeNeededToGetToBall_ms() + 1500) / (float)(GetTimeNeededToGetToBall_ms() + 1500);
    float tmpFadingTeamPossessionAmount = fadingTeamPossessionAmount * 0.995f + clamp(teamPossessionAmount, 0.5f, 1.5f) * 0.005f;
    fadingTeamPossessionAmount += clamp(tmpFadingTeamPossessionAmount - fadingTeamPossessionAmount, -0.005f, 0.005f); // maximum change per 10ms

    if (!match->IsInPlay() || match->IsInSetPiece() || match->GetBallRetainer() != 0) {
      if (match->GetBallRetainer() != 0) {
        fadingTeamPossessionAmount = teamPossessionAmount = (match->GetBallRetainer()->GetTeamID() == GetID()) ? 1.5f : 0.5f;
      } else {
        fadingTeamPossessionAmount = teamPossessionAmount = (match->GetBestPossessionTeamID() == GetID()) ? 1.5f : 0.5f;
      }
    }

    HumanGamersSelectAnyone();

    if (match->IsInPlay() && !match->IsInSetPiece()) {
      teamController->Process();

      if ((match->GetActualTime_ms() + 200 * id) % 400 == 0) {
        teamController->CalculateDynamicRoles();
        //printf("dynamic roles calc team %i\n", id);
      }

      if ((match->GetActualTime_ms() + 200 * id + 100) % 400 == 0) {
        teamController->CalculateManMarking();
        //printf("man marking calc team %i\n", id);
      }
    }

    for (unsigned int i = 0; i < players.size(); i++) {
      if (players.at(i)->IsActive()) {
        players.at(i)->Process();
      }
    }

    if (match->IsInPlay()) {

      for (unsigned int i = 0; i < humanGamers.size(); i++) {

        // switch button
        int selectedPlayerID = humanGamers.at(i)->GetSelectedPlayerID();
        Player *selectedPlayer = 0;
        selectedPlayer = GetPlayer(selectedPlayerID);
        assert(selectedPlayer);

        if (humanGamers.at(i)->GetHIDevice()->GetButton(e_ButtonFunction_Switch) &&
            !humanGamers.at(i)->GetHIDevice()->GetPreviousButtonState(e_ButtonFunction_Switch) &&
            // don't switch if we are both best AND designated possession player. unless opponent team has ball.
            (!(selectedPlayerID == GetBestPossessionPlayerID() && selectedPlayerID == designatedTeamPossessionPlayer->GetID()) || GetTeamPossessionAmount() < 1.0f) &&
            !selectedPlayer->HasUniquePossession()) {

          int targetPlayerID = -1;
          Player *targetPlayer = 0;

          if (!IsHumanControlled(designatedTeamPossessionPlayer->GetID()) && match->GetBestPossessionTeamID() == GetID()) {
            targetPlayer = designatedTeamPossessionPlayer;
          } else if (!IsHumanControlled(GetBestPossessionPlayer()->GetID()) && match->GetBestPossessionTeamID() == GetID()) {
            targetPlayer = GetBestPossessionPlayer();
          } else {
            targetPlayer = AI_GetBestSwitchTargetPlayer(match, this, humanGamers.at(i)->GetHIDevice()->GetDirection());
            if (targetPlayer)
              if (IsHumanControlled(targetPlayer->GetID())) targetPlayer = 0;
          }
          if (targetPlayer == GetGoalie()) targetPlayer = 0; // can not be goalie in current version, at least not during play, unless being directly passed to by teammate

          if (targetPlayer) {
            targetPlayerID = targetPlayer->GetID();
          }
          if (targetPlayerID != -1) humanGamers.at(i)->SetSelectedPlayerID(targetPlayerID);
        }

      }

    } else {

      // make sure all human gamers don't have a player selected

      for (unsigned int i = 0; i < humanGamers.size(); i++) {
        if (humanGamers.at(i)->GetSelectedPlayerID() != -1) {
          humanGamers.at(i)->SetSelectedPlayerID(-1);
        }
      }

    }

    int designatedPlayerTime_ms = designatedTeamPossessionPlayer->GetTimeNeededToGetToBall_ms();
    Player *bestPlayer = GetBestPossessionPlayer();
    int oppTime_ms = match->GetTeam(abs(GetID() - 1))->GetTimeNeededToGetToBall_ms();
    if (designatedTeamPossessionPlayer != bestPlayer) {
      // switch only if other player is somewhat better, to overcome possession-chaos
      int bestPlayerTime_ms = bestPlayer->GetTimeNeededToGetToBall_ms();
      float timeRating = (float)(bestPlayerTime_ms + 500) / (float)(designatedPlayerTime_ms + 500);

      if (bestPlayer->HasPossession()) timeRating *= 0.5f;
      if (designatedTeamPossessionPlayer->HasPossession()) timeRating /= 0.5f;

      if (IsHumanControlled(bestPlayer->GetID())) timeRating *= 0.8f;
      if (IsHumanControlled(designatedTeamPossessionPlayer->GetID())) timeRating /= 0.8f;

      // current player can get to the ball before the closest opponent: less need to switch
      //if (GetID() == 0) printf("opptime: %i, designated time: %i\n", oppTime_ms, designatedPlayerTime_ms);
      if (IsHumanControlled(bestPlayer->GetID()) == false && designatedPlayerTime_ms < oppTime_ms - 100) {
        timeRating += 0.2f;
        timeRating *= 1.2f;
      }

      if (timeRating < 0.8f) {
        designatedTeamPossessionPlayer = bestPlayer;
      }
    }

    //printf("team id: %i, time: %i, other team id: %i, time: %i\n", GetID(), GetTimeNeededToGetToBall_ms(), match->GetTeam(abs(GetID() - 1))->GetID(), match->GetTeam(abs(GetID() - 1))->GetTimeNeededToGetToBall_ms());

  /*
    if (id == 0) {
      GetSmallDebugCircle1()->SetPosition(designatedTeamPossessionPlayer->GetPosition());
    } else {
      GetSmallDebugCircle2()->SetPosition(designatedTeamPossessionPlayer->GetPosition());
    }
  */

  }

}

void Team::PreparePutBuffers(unsigned long snapshotTime_ms) {
  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      players.at(i)->PreparePutBuffers(snapshotTime_ms);
    }
  }
}

void Team::FetchPutBuffers(unsigned long putTime_ms) {
  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      players.at(i)->FetchPutBuffers(putTime_ms);
    }
  }
}

void Team::Put() {
  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      players.at(i)->Put();
    }
  }
}

void Team::Put2D() {
  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      players.at(i)->Put2D();
    }
  }
}

void Team::Hide2D() {
  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      players.at(i)->Hide2D();
    }
  }
}

void Team::UpdatePossessionStats() {
  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      players.at(i)->UpdatePossessionStats();
    }
  }


  // possession?

  hasPossession = false;
  timeNeededToGetToBall_ms = 100000;
  for (int i = 0; i < (signed int)players.size(); i++) {
    if (players.at(i)->IsActive()) {
      if (players.at(i)->HasPossession()) hasPossession = true;
      if (players.at(i)->GetTimeNeededToGetToBall_ms() < timeNeededToGetToBall_ms) timeNeededToGetToBall_ms = players.at(i)->GetTimeNeededToGetToBall_ms();
    }
  }
}

void Team::UpdateSwitch() {

  // lose turn on ball possession

  if (match->IsInPlay() && humanGamers.size() > 1) {
    int myTurn = *switchPriority.begin();
    if (humanGamers.at(myTurn)->GetSelectedPlayerID() == match->GetDesignatedPossessionPlayer()->GetID()) {
      switchPriority.pop_front();
      switchPriority.push_back(myTurn);
    }
  }


  // autoswitch on proximity

/* recently disabled
  if (match->IsInPlay() && humanGamers.size() > 0) {
    if (!IsHumanControlled(designatedTeamPossessionPlayer->GetID()) &&
        designatedTeamPossessionPlayer->GetTimeNeededToGetToBall_ms() < 2000 && // proximity
        designatedTeamPossessionPlayer->GetTimeNeededToGetToBall_ms() <= this->GetTimeNeededToGetToBall_ms() && // sometimes, the designated team possession player is not the player quickest to ball. don't autoswitch then
        GetTeamPossessionAmount() > 1.3f) SelectPlayer(designatedTeamPossessionPlayer);
  }
*/

  //if (GetID() == 0) printf("teamposs %f\n", GetTeamPossessionAmount());


  // team player in possession is not human selected

  if (match->IsInPlay() && humanGamers.size() > 0) {
    if (!IsHumanControlled(designatedTeamPossessionPlayer->GetID()) && (designatedTeamPossessionPlayer->HasUniquePossession() || match->IsInSetPiece())) {
      if (designatedTeamPossessionPlayer != GetGoalie()) {
        SelectPlayer(designatedTeamPossessionPlayer);
      }
    }
  }

}

Player *Team::GetGoalie() {
  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      if (players.at(i)->GetFormationEntry().role == e_PlayerRole_GK) return players.at(i);
    }
  }

  return 0;
}

void Team::SetKitNumber(int num) {
  std::string kitNumberString = int_to_str(num);
  if (kitNumberString.size() < 2) kitNumberString = "0" + kitNumberString;
  std::string kitFilename = GetTeamData()->GetKitUrl() + "_kit_" + kitNumberString + ".png";
  boost::intrusive_ptr<Resource<Surface>> newKit;
  if (!boost::filesystem::exists(kitFilename)) {
    Vector3 shirtC, shortsC, socksC;
    GetKitColors(this, shirtC, shortsC, socksC);
    newKit = MakeColorKit(shirtC, shortsC, socksC);
  } else {
    newKit = ResourceManagerPool::GetInstance().GetManager<Surface>(e_ResourceType_Surface)->Fetch(kitFilename);
  }

  for (unsigned int i = 0; i < players.size(); i++) {
    if (players.at(i)->IsActive()) {
      if (players.at(i)->GetFormationEntry().role != e_PlayerRole_GK) players.at(i)->SetKit(newKit);
    }
  }

  kit = newKit;
}

bool Team::SubstitutePlayer(int offIdx, int onIdx) {
  if (offIdx < 0 || offIdx >= (int)players.size()) return false;
  if (onIdx  < 0 || onIdx  >= (int)players.size()) return false;
  if (!players[offIdx]->IsActive())  return false;
  if ( players[onIdx ]->IsActive())  return false;

  // SetBench() is safe to call from the game thread — no GUI widget allocation/deletion.
  // Deactivate() (which calls MarkForDeletion on captions) is NOT safe mid-match from this thread.
  players[offIdx]->SetBench();

  // Swap so the incoming player sits at offIdx — a valid formation-entry index (0–10).
  std::swap(players[offIdx], players[onIdx]);

  players[offIdx]->SetActive();

  subsMade++;
  printf("[SUB] Team %d: players[%d] off → players[%d] on (sub #%d)\n",
         id, offIdx, onIdx, subsMade);
  return true;
}
