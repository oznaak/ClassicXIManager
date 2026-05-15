// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#include "menutask.hpp"

#include "../onthepitch/match.hpp"

#include "pagefactory.hpp"

#include "mainmenu.hpp"
#include "ingame/ingame.hpp"
#include "visualoptions.hpp"
#include "ingame/replaymenu.hpp"
#include "ingame/phasemenu.hpp"
#include "ingame/gameover.hpp"

#include "gametask.hpp"

#include "main.hpp"

#include "framework/scheduler.hpp"
#include "managers/resourcemanagerpool.hpp"

using namespace blunted;

void SetActiveController(int side, bool keyboard) {
  bool keyboardActive = true;
  const std::vector<SideSelection> sides = GetMenuTask()->GetControllerSetup();
  int menuControllerID = -1;
  for (unsigned int i = 0; i < sides.size(); i++) {
    if (sides.at(i).side == side) {
      if (GetControllers().at(sides.at(i).controllerID)->GetDeviceType() == e_HIDeviceType_Gamepad) {
        menuControllerID = static_cast<HIDGamepad*>(GetControllers().at(sides.at(i).controllerID))->GetGamepadID();
        keyboardActive = false;
      }
      break;
    }
    if (i == sides.size() - 1) menuControllerID = 0; // AI opponent, so allow choosing their team with controller
  }

  GetMenuTask()->SetActiveJoystickID(menuControllerID);
  if (keyboard) {
    if (keyboardActive) {
      GetMenuTask()->EnableKeyboard();
    } else {
      GetMenuTask()->DisableKeyboard();
    }
  } else {
    GetMenuTask()->EnableKeyboard();
  }
}

MenuTask::MenuTask(float aspectRatio, float margin, TTF_Font *defaultFont, TTF_Font *defaultOutlineFont) : Gui2Task(GetScene2D(), aspectRatio, margin) {

  Gui2Style *style = windowManager->GetStyle();

  style->SetFont(e_TextType_Default, defaultFont);
  style->SetFont(e_TextType_DefaultOutline, defaultOutlineFont);
  style->SetFont(e_TextType_Caption, defaultFont);
  style->SetFont(e_TextType_Title, defaultFont);
  style->SetFont(e_TextType_ToolTip, defaultFont);

/* previous colorset
  style->SetColor(e_DecorationType_Dark1, Vector3(0, 0, 0));
  style->SetColor(e_DecorationType_Dark2, Vector3(63, 63, 63));
  style->SetColor(e_DecorationType_Bright1, Vector3(240, 255, 210));
  style->SetColor(e_DecorationType_Bright2, Vector3(214, 194, 154));
  style->SetColor(e_DecorationType_Toggled, Vector3(255, 20, 70));
*/

  // huisstijl:
  // blauw: 0, 100, 220
  // orange: 240, 100, 0
  style->SetColor(e_DecorationType_Dark1, Vector3(20, 35, 55));
  style->SetColor(e_DecorationType_Dark2, Vector3(60, 35, 20));
  style->SetColor(e_DecorationType_Bright1, Vector3(150, 180, 220));
  style->SetColor(e_DecorationType_Bright2, Vector3(240, 150, 100));
  style->SetColor(e_DecorationType_Toggled, Vector3(240, 60, 60));

  windowManager->SetTimeStep_ms(10);

  Gui2Root *root = windowManager->GetRoot();
  root->Show();

  PageFactory *pageFactory = new PageFactory();
  windowManager->SetPageFactory(pageFactory);

  queuedFixture->team1KitNum = 1;
  queuedFixture->team2KitNum = 2;

  managerMatchPending    = false;
  managerMainMenuPending = false;
  managerCareerPagePending = false;
  managerCareerPageId = 0;
  menuAction = e_MenuAction_Menu;

}

MenuTask::~MenuTask() {
  if (Verbose()) printf("exiting menutask.. ");

  delete windowManager->GetPageFactory();

  if (Verbose()) printf("done\n");
}

void MenuTask::ProcessPhase() {

  Gui2Task::ProcessPhase();

  // Manager match start: ManagerMainScreenPage already Exit()'d itself from the GL thread.
  // We create LoadingMatchPage here (main thread) so image loading via ObjectFactory is safe.
  if (managerMatchPending) {
    managerMatchPending = false;
    printf("[MENUTASK] Creating LoadingMatchPage from MenuTask\n");
    Properties properties;
    windowManager->GetPageFactory()->CreatePage((int)e_PageID_LoadingMatch, properties, 0);
    printf("[MENUTASK] LoadingMatchPage created\n");
    return;
  }

  if (managerMainMenuPending) {
    managerMainMenuPending = false;
    printf("[MENUTASK] Creating MainMenu page inside existing MenuScene\n");
    windowManager->GetPagePath()->Clear();
    Properties properties;
    windowManager->GetPageFactory()->CreatePage((int)e_PageID_MainMenu, properties, 0);
    printf("[MENUTASK] MainMenu page created\n");
    return;
  }

  if (managerCareerPagePending) {
    managerCareerPagePending = false;
    printf("[MENUTASK] Creating ManagerMainScreen page inside existing MenuScene for manager %d\n", managerCareerPageId);
    windowManager->GetPagePath()->Clear();
    Properties properties;
    properties.Set("managerId", managerCareerPageId);
    windowManager->GetPageFactory()->CreatePage((int)e_PageID_Manager_MainScreen, properties, 0);
    printf("[MENUTASK] ManagerMainScreen page created\n");
    return;
  }

  if (menuAction == e_MenuAction_Menu) {

    windowManager->GetPagePath()->Clear();

    GetGameTask()->Action(e_GameTaskMessage_StopMatch);
    GetGameTask()->Action(e_GameTaskMessage_StartMenuScene);

    Properties properties;
    if (!QuickStart()) {
      if (!IsReleaseVersion()) {
        windowManager->GetPageFactory()->CreatePage((int)e_PageID_MainMenu, properties, 0);
      } else {
        windowManager->GetPageFactory()->CreatePage((int)e_PageID_Intro, properties, 0);
      }
    } else {
      windowManager->GetPageFactory()->CreatePage((int)e_PageID_LoadingMatch, properties, 0);
    }

  } else if (menuAction == e_MenuAction_Game) {

    GetGameTask()->Action(e_GameTaskMessage_StopMenuScene);
    GetGameTask()->Action(e_GameTaskMessage_StartMatch);

  }

  menuAction = e_MenuAction_None;
}

bool MenuTask::QuickStart() {
  return false;
}

void MenuTask::QuitGame() {
  EnvironmentManager::GetInstance().SignalQuit();
}

void MenuTask::ReleaseAllButtons() {
  // when going back to game, depress all buttons, so we don't go around doing passes we don't want
  for (int joyID = 0; joyID < UserEventManager::GetInstance().GetJoystickCount(); joyID++) {
    for (unsigned int buttonID = 0; buttonID < blunted::_JOYSTICK_MAXBUTTONS; buttonID++) {
      UserEventManager::GetInstance().SetJoyButtonState(joyID, buttonID, false);
    }
  }
  UserEventManager::GetInstance().SetKeyboardState(SDLK_ESCAPE, false);
}
