// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "gametask.hpp"

#include "main.hpp"

#include "framework/scheduler.hpp"
#include "managers/taskmanager.hpp"
#include "managers/resourcemanagerpool.hpp"

#include "blunted.hpp"

#include "menu/imgui_match.hpp"
#include "menu/imgui_career.hpp"
#include "onthepitch/team.hpp"
#include "onthepitch/referee.hpp"

static bool MatchIsFinalOrEnded(Match *match) {
  if (!match) return true;
  if (match->IsGameOver()) return true;
  if (match->GetMatchPhase() == e_MatchPhase_Penalties) return true;
  return false;
}

static bool MatchIsPhaseTransitioning(Match *match) {
  if (!match) return true;
  const RefereeBuffer &buffer = match->GetRefereeBuffer();
  return buffer.active && buffer.endPhase;
}

static bool MatchIsPreparingKickOffRestart(Match *match) {
  if (!match) return true;
  const RefereeBuffer &buffer = match->GetRefereeBuffer();
  return buffer.active && buffer.desiredSetPiece == e_SetPiece_KickOff;
}

void UploadFullbodyModel::Update() {
  for (unsigned int i = 0; i < geometryToUpload.size(); i++) {
    geometryToUpload.at(i)->OnUpdateGeometryData(false);
  }
}

GameTask::GameTask() {

  match = 0;
  menuScene = 0;

  // prohibits deletion of the scene before this object is dead
  scene3D = GetScene3D();
}

GameTask::~GameTask() {
  if (Verbose()) printf("exiting gametask.. ");
  Exit();
  if (Verbose()) printf("done\n");
}

void GameTask::Exit() {

  Action(e_GameTaskMessage_StopMatch);
  Action(e_GameTaskMessage_StopMenuScene);

  ResourceManagerPool::GetInstance().CleanUp();

  scene3D.reset();
}

void GameTask::Action(e_GameTaskMessage message) {

  switch (message) {

    case e_GameTaskMessage_StartMatch:
      {
        if (Verbose()) printf("*gametaskmessage: starting match\n");

        GetGraphicsSystem()->getPhaseMutex.lock();
        MatchData *matchData = GetMenuTask()->GetMatchData();
        assert(matchData);
        Match *tmpMatch = new Match(matchData, GetControllers());

        matchLifetimeMutex.lock();
        matchPutBufferMutex.lock();
        assert(!match);
        match = tmpMatch;
        GetScheduler()->ResetTaskSequenceTime("game");
        matchPutBufferMutex.unlock();
        matchLifetimeMutex.unlock();
        GetGraphicsSystem()->getPhaseMutex.unlock();
      }
      break;

    case e_GameTaskMessage_StopMatch:
      if (Verbose()) printf("*gametaskmessage: stopping match\n");

      GetGraphicsSystem()->getPhaseMutex.lock();
      matchLifetimeMutex.lock();
      matchPutBufferMutex.lock();
      {
        // Null the match pointer under matchRenderMutex so the OpenGL render
        // thread sees null and skips match access on its next SwapBuffers.
        // We release matchRenderMutex BEFORE Exit()/delete so that OpenGL calls
        // inside Exit() don't deadlock with the render thread trying to lock it.
        Match *m = match;
        matchRenderMutex.lock();
        match = 0;
        ResetMatchOverlayState(); // clear stale match pointer before renderer sees it
        matchRenderMutex.unlock();
        if (m) {
          m->Exit();
          delete m;
        }
      }
      matchPutBufferMutex.unlock();
      matchLifetimeMutex.unlock();
      GetGraphicsSystem()->getPhaseMutex.unlock();
      break;

    case e_GameTaskMessage_StartMenuScene:
      if (Verbose()) printf("*gametaskmessage: starting menu scene\n");

      GetGraphicsSystem()->getPhaseMutex.lock();
      menuSceneLifetimeMutex.lock();
      assert(!menuScene);
      menuScene = new MenuScene();
      GetScheduler()->ResetTaskSequenceTime("game");
      menuSceneLifetimeMutex.unlock();
      GetGraphicsSystem()->getPhaseMutex.unlock();
      break;

    case e_GameTaskMessage_StopMenuScene:
      if (Verbose()) printf("*gametaskmessage: stopping menu scene\n");

      GetGraphicsSystem()->getPhaseMutex.lock();
      menuSceneLifetimeMutex.lock();
      //assert(menuScene);
      if (menuScene) {
        delete menuScene;
        menuScene = 0;
      }
      menuSceneLifetimeMutex.unlock();
      GetGraphicsSystem()->getPhaseMutex.unlock();
      break;

    default:
      break;

  }
}

void GameTask::GetPhase() {

  // process messageQueue
  if (match) match->Get();
  if (menuScene) menuScene->Get();
}

void GameTask::ProcessPhase() {

  for (unsigned int i = 0; i < GetControllers().size(); i++) {
    GetControllers().at(i)->Process();
  }

  if (match) {
    int mult = 0;
    if (!g_TopBarSoftPause) {
      mult = GetConfiguration()->GetInt("match_speed_multiplier", 1);
      if (mult < 1) mult = 1;
      if (mult > 8) mult = 8;
    }
    for (int s = 0; s < mult; s++) {
      match->Process();
    }

    // Track window lifecycle: close the open window when play resumes.
    {
      static bool s_wasInPlay = false;
      bool nowInPlay = match->IsInPlay();
      if (nowInPlay && !s_wasInPlay && g_SubWindowOpen)
        g_SubWindowOpen = false;
      s_wasInPlay = nowInPlay;
    }

    // Apply pending tactic changes from the tactics panel (every frame, immediate effect).
    if (!g_PendingTacticsChanges.empty()) {
      matchPutBufferMutex.lock();
      for (const auto &tc : g_PendingTacticsChanges) {
        Team *team = match->GetTeam(tc.teamIdx);
        if (team && team->GetTeamData())
          team->GetTeamData()->GetTacticsWritable().userProperties.Set(tc.key.c_str(), tc.value);
      }
      g_PendingTacticsChanges.clear();
      matchPutBufferMutex.unlock();
    }

    // Execute queued substitutions one per frame on dead ball.
    // Rules: max 5 subs total, max 3 distinct windows; multiple subs in same stoppage = 1 window.
    if (!QueueSubEmpty() && MatchIsFinalOrEnded(match)) {
      printf("[SUB] Cleared %zu queued substitution(s): match is ending or ended phase=%d time=%lu\n",
             QueueSubSize(), (int)match->GetMatchPhase(), match->GetMatchTime_ms());
      QueueSubClear();
    }

    if (!QueueSubEmpty() && !match->GetPause() && !match->IsInPlay() &&
        !match->IsGoalScored() && !MatchIsFinalOrEnded(match) &&
        !MatchIsPhaseTransitioning(match) && !MatchIsPreparingKickOffRestart(match)) {
      QueuedSub nextSub;
      bool hasQueuedSub = QueueSubPeekFront(nextSub);
      bool canSub = false;
      if (hasQueuedSub) {
        if (nextSub.aiControlled) {
          Team *team = (nextSub.teamIdx >= 0 && nextSub.teamIdx < 2) ? match->GetTeam(nextSub.teamIdx) : 0;
          canSub = team && team->GetSubsMade() < 5;
        } else {
          canSub = (g_SubsUsed < 5) && (g_WindowsUsed < 3 || g_SubWindowOpen);
        }
      }
      if (hasQueuedSub && canSub) {
        QueuedSub sub;
        if (!QueueSubPopFront(sub)) sub.pending = false;

        if (sub.pending) {
          printf("[SUB] Executing queued substitution team=%d offDb=%d onDb=%d offIdx=%d onIdx=%d phase=%d time=%lu used=%d windows=%d open=%d\n",
               sub.teamIdx, sub.offPlayerDbId, sub.onPlayerDbId, sub.offIdx, sub.onIdx,
               (int)match->GetMatchPhase(), match->GetMatchTime_ms(),
               g_SubsUsed, g_WindowsUsed, g_SubWindowOpen ? 1 : 0);

          matchLifetimeMutex.lock();
          matchPutBufferMutex.lock();
          matchRenderMutex.lock();
          Team *team = (sub.teamIdx >= 0 && sub.teamIdx < 2) ? match->GetTeam(sub.teamIdx) : 0;
          bool subDone = team && team->SubstitutePlayerByDatabaseID(sub.offPlayerDbId, sub.onPlayerDbId,
                                                                    sub.offIdx, sub.onIdx);
          matchRenderMutex.unlock();
          matchPutBufferMutex.unlock();
          matchLifetimeMutex.unlock();

          if (subDone) {
            if (!sub.aiControlled) {
              if (!g_SubWindowOpen) { g_WindowsUsed++; g_SubWindowOpen = true; }
              g_SubsUsed++;
            }

            SubGraphic sg;
            sg.active         = true;
            sg.nameOut        = sub.nameOut;
            sg.nameIn         = sub.nameIn;
            sg.teamBadgePath  = sub.teamBadgePath;
            sg.leagueLogoPath = sub.leagueLogoPath;
            if (sg.leagueLogoPath.empty()) sg.leagueLogoPath = g_MatchCompetitionLogoPath;
            sg.teamColor      = sub.teamColor;
            sg.startTime      = -1.0;
            SubGraphicPush(sg);
          } else {
            printf("[SUB] Ignored stale substitution request team=%d offDb=%d onDb=%d offIdx=%d onIdx=%d\n",
                   sub.teamIdx, sub.offPlayerDbId, sub.onPlayerDbId, sub.offIdx, sub.onIdx);
          }
        }
      } else if (hasQueuedSub) {
        // Budget exhausted — drop remaining queued subs
        printf("[SUB] Cleared %zu queued substitution(s): budget exhausted used=%d windows=%d open=%d\n",
               QueueSubSize(), g_SubsUsed, g_WindowsUsed, g_SubWindowOpen ? 1 : 0);
        QueueSubClear();
      }
    }

    matchPutBufferMutex.lock();
    match->PreparePutBuffers();
    matchPutBufferMutex.unlock();
  }

  if (menuScene) {
    menuScene->Process();
  }

}

void GameTask::PutPhase() {

  std::vector < boost::intrusive_ptr<UpdateFullbodyModel> > updateFullbodyModels;
  std::vector < boost::intrusive_ptr<UploadFullbodyModel> > uploadFullbodyModels;
  std::vector<PlayerBase*> playersToProcess;

  matchLifetimeMutex.lock();

  if (match) {

    matchPutBufferMutex.lock();
    match->FetchPutBuffers();
    matchPutBufferMutex.unlock();

    match->Put();

    std::vector<Player*> players;
    match->GetActiveTeamPlayers(0, players);
    match->GetActiveTeamPlayers(1, players);
    std::vector<PlayerBase*> officials;
    match->GetOfficialPlayers(officials);

    for (unsigned int i = 0; i < players.size(); i++) {
      if (match->GetPause() || players.at(i)->NeedsModelUpdate()) playersToProcess.push_back(players.at(i));
    }
    for (unsigned int i = 0; i < officials.size(); i++) {
      playersToProcess.push_back(officials.at(i));
    }

    //printf("%i players, %i threads.\n", playersToProcess.size(), threadCount);
    unsigned int playersPerThread = 7;
    unsigned int playerStartIndex = 0;
    while (playerStartIndex < playersToProcess.size()) {
      std::vector<PlayerBase*> playersToProcessInThread;
      for (unsigned int p = 0; p < playersPerThread; p++) {
        if (playerStartIndex + p >= playersToProcess.size()) break;
        playersToProcessInThread.push_back(playersToProcess.at(playerStartIndex + p));
        //printf("adding player %i\n", playerStartIndex + p);
        // unthreaded version: playersToProcess.at(playerStartIndex + p)->UpdateFullbodyModel();
      }
      playerStartIndex += playersPerThread;

      boost::intrusive_ptr<UpdateFullbodyModel> updateFullbodyModel(new UpdateFullbodyModel(playersToProcessInThread));
      updateFullbodyModels.push_back(updateFullbodyModel);
      TaskManager::GetInstance().EnqueueWork(updateFullbodyModel, true);
    }

    match->UploadGoalNetting(); // won't this block the whole process thing too? (opengl busy == wait, while mutex locked == no process)

  }


  for (unsigned int t = 0; t < updateFullbodyModels.size(); t++) {
    updateFullbodyModels.at(t)->Wait();
  }

  if (match) {

    unsigned int playersPerThread = 7;
    unsigned int playerStartIndex = 0;
    while (playerStartIndex < playersToProcess.size()) {
      std::vector < boost::intrusive_ptr<Geometry> > geometryToUploadInThread;
      for (unsigned int p = 0; p < playersPerThread; p++) {
        if (playerStartIndex + p >= playersToProcess.size()) break;
        geometryToUploadInThread.push_back(boost::static_pointer_cast<Geometry>(playersToProcess.at(playerStartIndex + p)->GetFullbodyNode()->GetObject("fullbody")));
      }
      playerStartIndex += playersPerThread;

      boost::intrusive_ptr<UploadFullbodyModel> uploadFullbodyModel(new UploadFullbodyModel(geometryToUploadInThread));
      uploadFullbodyModels.push_back(uploadFullbodyModel);
      TaskManager::GetInstance().EnqueueWork(uploadFullbodyModel, true);

      //working on: maybe we need to use the gfx system get pointer somewhere here? too tired to analyse this now :p
    }

  } // !match

  matchLifetimeMutex.unlock();

  menuSceneLifetimeMutex.lock();
  if (menuScene) menuScene->Put();
  menuSceneLifetimeMutex.unlock();

}
