#ifndef _HPP_MENU_MANAGER_CAREER
#define _HPP_MENU_MANAGER_CAREER

#include "utils/gui2/page.hpp"
#include "utils/gui2/widgets/button.hpp"
#include "utils/gui2/widgets/caption.hpp"
#include "utils/gui2/widgets/editline.hpp"
#include "utils/gui2/widgets/grid.hpp"
#include "utils/gui2/widgets/pulldown.hpp"
#include "../utils.hpp"
#include "../main.hpp"

using namespace blunted;

class ManagerCreateProfilePage : public Gui2Page {
 public:
  ManagerCreateProfilePage(Gui2WindowManager *windowManager, const Gui2PageData &pageData);
  virtual ~ManagerCreateProfilePage();

  void CreateProfile();
  void Back();

 protected:
  Gui2Grid *grid;
  Gui2EditLine *nameInput;
  Gui2EditLine *ageInput;
  Gui2Pulldown *nationalityDropdown;
  Gui2Pulldown *genderDropdown;
  Gui2Button *createButton;
  Gui2Button *backButton;
};

class ManagerSelectLeaguePage : public Gui2Page {
 public:
  ManagerSelectLeaguePage(Gui2WindowManager *windowManager, const Gui2PageData &pageData);
  virtual ~ManagerSelectLeaguePage();

  void SelectLeague(int leagueId);
  void Back();

 protected:
  int managerId;
  Gui2Grid *grid;
  std::vector<Gui2Button *> leagueButtons;
  Gui2Button *backButton;
};

class ManagerSelectClubPage : public Gui2Page {
 public:
  ManagerSelectClubPage(Gui2WindowManager *windowManager, const Gui2PageData &pageData);
  virtual ~ManagerSelectClubPage();

  void SelectClub(int clubId);
  void StartCareer();
  void Back();

 protected:
  int managerId;
  int leagueId;
  int selectedClubId;

  Gui2Grid *grid;
  std::vector<Gui2Button *> clubButtons;
  Gui2Button *startButton;
  Gui2Button *backButton;
};

class ManagerLoadGamePage : public Gui2Page {
 public:
  ManagerLoadGamePage(Gui2WindowManager *windowManager, const Gui2PageData &pageData);
  virtual ~ManagerLoadGamePage();

  void LoadManager(int managerId);
  void Back();

 protected:
  Gui2Grid *grid;
  std::vector<Gui2Button *> managerButtons;
  Gui2Button *backButton;
};

class ManagerMainScreenPage : public Gui2Page {
 public:
  ManagerMainScreenPage(Gui2WindowManager *windowManager, const Gui2PageData &pageData);
  virtual ~ManagerMainScreenPage();

  void OpenTab(int tab);
  void PlayMatch();
  void BackToMainMenu();

 protected:
  int managerId;
  int clubId;
  int activeTab;

  Gui2Grid *navGrid;
  Gui2Grid *managerGrid;
  Gui2Grid *clubGrid;
  Gui2Grid *matchesGrid;
  Gui2Grid *standingsGrid;

  Gui2Button *managerButton;
  Gui2Button *clubButton;
  Gui2Button *matchesButton;
  Gui2Button *standingsButton;
  Gui2Button *playMatchButton;
  Gui2Button *mainMenuButton;

  void BuildNavigation();
  void BuildManagerView();
  void BuildClubView();
  void BuildMatchesView();
  void BuildStandingsView();
  void ShowActiveView();
};

#endif
