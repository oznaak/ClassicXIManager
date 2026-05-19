#pragma once
#include <string>
#include <vector>
#include <functional>

enum e_PreCareerScreen {
  PRECAREER_NONE             = 0,
  PRECAREER_MAIN_MENU        = 1,
  PRECAREER_CREATE_PROFILE   = 2,
  PRECAREER_LOAD_GAME        = 3,
  PRECAREER_SELECT_LEAGUE    = 4,
  PRECAREER_SELECT_CLUB      = 5,
  PRECAREER_SETTINGS_PLACEHOLDER = 6,
  PRECAREER_SELECT_COUNTRY   = 7,
};

// pendingAction values set by ImGui buttons and consumed by RenderImGuiPreCareer() in imgui_menu.cpp:
//  1 = new game        (enter create profile)
//  2 = load game       (enter load game)
//  3 = settings        (enter settings placeholder)
//  4 = quit            (quit game)
//  5 = create profile  (validate profile and enter select country)
//  6 = back            (navigate back)
//  7 = load manager    (load manager by pendingId)
//  8 = select league   (enter club selection for pendingId league)
//  9 = select country  (selectedCountryId/Name already set; enter filtered league)
// 10 = start career    (persist club and enter career hub)

struct PreCareerState {
  bool              active        = false;
  e_PreCareerScreen screen        = PRECAREER_NONE;
  int               pendingAction = 0;
  int               pendingId     = 0;
  bool              isTransitioning = false;

  // Callbacks wired by each hosting Gui2Page
  std::function<void()>    onNewGame;
  std::function<void()>    onLoadGame;
  std::function<void()>    onSettings;
  std::function<void()>    onQuit;
  std::function<void()>    onCreateProfile;
  std::function<void()>    onBack;
  std::function<void(int)> onLoadManager;
  std::function<void(int)> onSelectLeague;
  std::function<void()>    onStartCareer;

  // Create-profile form — persistent buffers, initialised once per page open
  char nameBuffer[128];
  char ageBuf[8];
  int  nationalityIdx;
  int  genderIdx;

  // Data lists for selection screens
  struct CountryItem { int id; std::string name; std::string flag; };
  std::vector<CountryItem> countries;
  int    selectedCountryId   = 0;
  std::string selectedCountryName;

  struct LeagueItem { int id; std::string name; std::string logoUrl; };
  std::vector<LeagueItem> leagues;

  struct ClubItem { int id; std::string name; std::string shortName; std::string logoPath; };
  std::vector<ClubItem> clubs;
  int selectedClubId;   // updated directly by ImGui, no deferred action needed
  int currentManagerId = 0;
  int currentLeagueId = 0;

  // Save list for Load Game
  struct SaveEntry { int id; std::string name, clubName, nationality, shortName, logoUrl; int age; };
  std::vector<SaveEntry> saves;

  // Pagination
  int selectCountryPage = 0;
  int selectLeaguePage  = 0;
  int selectClubPage    = 0;

  void Clear();
};

extern PreCareerState g_PreCareer;

// Called from OpenGLRenderer3D::SwapBuffers() between NewFrame / Render.
void RenderImGuiPreCareer();

// Logo texture — shared with career UI. Aspect ratio: 1672x941 (landscape).
// Returns a GL texture ID (unsigned int); include GL headers before use.
unsigned int GetMainLogoTexture();
static const float kMainLogoAspect = 1672.0f / 941.0f;
