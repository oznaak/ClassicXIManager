#include "managercareer.hpp"
#include "imgui_career.hpp"
#include "imgui_menu.hpp"
#include "careermatchcontext.hpp"
#include "pagefactory.hpp"
#include "menutask.hpp"

#include "base/utils.hpp"
#include <boost/bind/bind.hpp>
#include <sstream>
#include <cstdlib>
#include <cstring>
#include <ctime>

using namespace boost::placeholders;

static std::string SqlEscape(const std::string &in) {
  std::string out;
  for (unsigned int i = 0; i < in.size(); i++) {
    if (in[i] == '\'') out += "''";
    else out += in[i];
  }
  return out;
}

static void EnsureCareerTables() {
  DatabaseResult *r0 = GetDB()->Query(
    "CREATE TABLE IF NOT EXISTS managers ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name VARCHAR(128),"
    "age INTEGER,"
    "nationality VARCHAR(64),"
    "gender VARCHAR(16),"
    "club_id INTEGER,"
    "current_date TEXT,"
    "season_year INTEGER,"
    "created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
    ");"
  );
  delete r0;

  DatabaseResult *r1 = GetDB()->Query(
    "CREATE TABLE IF NOT EXISTS fixtures ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "manager_id INTEGER NOT NULL,"
    "league_id INTEGER NOT NULL,"
    "season_year INTEGER NOT NULL,"
    "round INTEGER NOT NULL,"
    "matchday INTEGER NOT NULL,"
    "home_team_id INTEGER NOT NULL,"
    "away_team_id INTEGER NOT NULL,"
    "fixture_date TEXT,"
    "status VARCHAR(32) DEFAULT 'scheduled',"
    "home_score INTEGER,"
    "away_score INTEGER,"
    "stats_json TEXT DEFAULT '{}',"
    "scorers_json TEXT DEFAULT '[]',"
    "cards_json TEXT DEFAULT '[]',"
    "played_at DATETIME,"
    "created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
    ");"
  );
  delete r1;

  DatabaseResult *r2 = GetDB()->Query(
    "CREATE TABLE IF NOT EXISTS standings ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "manager_id INTEGER NOT NULL,"
    "league_id INTEGER NOT NULL,"
    "team_id INTEGER NOT NULL,"
    "played INTEGER DEFAULT 0,"
    "won INTEGER DEFAULT 0,"
    "drawn INTEGER DEFAULT 0,"
    "lost INTEGER DEFAULT 0,"
    "goals_for INTEGER DEFAULT 0,"
    "goals_against INTEGER DEFAULT 0,"
    "goal_difference INTEGER DEFAULT 0,"
    "points INTEGER DEFAULT 0,"
    "created_at DATETIME DEFAULT CURRENT_TIMESTAMP,"
    "UNIQUE(manager_id, league_id, team_id)"
    ");"
  );
  delete r2;

  // Safe migration: add columns if they don't already exist.
  {
    DatabaseResult *info = GetDB()->Query("PRAGMA table_info(managers);");
    bool hasCurrentDate = false, hasSeasonYear = false;
    for (unsigned int i = 0; i < info->data.size(); i++) {
      if (info->data.at(i).size() > 1) {
        if (info->data.at(i).at(1) == "current_date") hasCurrentDate = true;
        if (info->data.at(i).at(1) == "season_year")  hasSeasonYear  = true;
      }
    }
    delete info;
    if (!hasCurrentDate) {
      DatabaseResult *a = GetDB()->Query("ALTER TABLE managers ADD COLUMN current_date TEXT;");
      delete a;
    }
    if (!hasSeasonYear) {
      DatabaseResult *a = GetDB()->Query("ALTER TABLE managers ADD COLUMN season_year INTEGER;");
      delete a;
    }
  }
  {
    DatabaseResult *info = GetDB()->Query("PRAGMA table_info(fixtures);");
    bool hasFixtureDate = false;
    for (unsigned int i = 0; i < info->data.size(); i++) {
      if (info->data.at(i).size() > 1 && info->data.at(i).at(1) == "fixture_date")
        hasFixtureDate = true;
    }
    delete info;
    if (!hasFixtureDate) {
      DatabaseResult *a = GetDB()->Query("ALTER TABLE fixtures ADD COLUMN fixture_date TEXT;");
      delete a;
    }
  }
}

static void DeleteCareerSeason(int managerId) {
  std::stringstream q1;
  q1 << "DELETE FROM fixtures WHERE manager_id = " << managerId << ";";
  DatabaseResult *r1 = GetDB()->Query(q1.str());
  delete r1;

  std::stringstream q2;
  q2 << "DELETE FROM standings WHERE manager_id = " << managerId << ";";
  DatabaseResult *r2 = GetDB()->Query(q2.str());
  delete r2;
}

// Compute YYYY-MM-DD for season start (Aug 15) + (round-1)*7 days.
static std::string MakeFixtureDate(int seasonYear, int round) {
  const int kDIM[] = {0,31,28,31,30,31,30,31,31,30,31,30,31};
  int year  = seasonYear;
  int month = 8;
  int day   = 15;
  int addDays = (round - 1) * 7;
  day += addDays;
  while (day > kDIM[month]) {
    day -= kDIM[month];
    month++;
    if (month > 12) { month = 1; year++; }
  }
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
  return std::string(buf);
}

static int GetCurrentYear() {
  time_t now = time(nullptr);
  struct tm *t = localtime(&now);
  return 1900 + t->tm_year;
}

static std::vector<int> GetLeagueTeamIds(int leagueId) {
  std::vector<int> teamIds;
  std::stringstream q;
  q << "SELECT id FROM teams WHERE league_id = " << leagueId << " ORDER BY id;";
  DatabaseResult *r = GetDB()->Query(q.str());
  for (unsigned int i = 0; i < r->data.size(); i++) {
    teamIds.push_back(atoi(r->data.at(i).at(0).c_str()));
  }
  delete r;
  return teamIds;
}

static void InsertFixture(int managerId, int leagueId, int seasonYear,
                          int round, int matchday,
                          int homeTeamId, int awayTeamId,
                          const std::string &fixtureDate) {
  std::stringstream q;
  q << "INSERT INTO fixtures"
    << "(manager_id,league_id,season_year,round,matchday,home_team_id,away_team_id,fixture_date)"
    << " VALUES("
    << managerId << "," << leagueId << "," << seasonYear << ","
    << round << "," << matchday << "," << homeTeamId << "," << awayTeamId << ","
    << "'" << fixtureDate << "'"
    << ");";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;
  printf("[FIXTURES] manager=%d league=%d matchday=%d date=%s home=%d away=%d\n",
         managerId, leagueId, matchday, fixtureDate.c_str(), homeTeamId, awayTeamId);
}

static void GenerateFixturesForLeague(int managerId, int leagueId,
                                      int seasonYear,
                                      const std::vector<int> &teamIds) {
  printf("[CAREER] League %d teams: %lu\n", leagueId, (unsigned long)teamIds.size());
  // Each pair plays home and away. Round 1 = first leg, Round 2 = second leg.
  // Matchday numbers the pair within each round; round drives the fixture_date.
  int pairIndex = 0;
  for (unsigned int i = 0; i < teamIds.size(); i++) {
    for (unsigned int j = i + 1; j < teamIds.size(); j++) {
      int matchday = pairIndex + 1;
      // Round 1: season start Aug 15 + (pairIndex * 7) days (first leg of this pair)
      // Round 2: first-leg date + 7 days (return leg)
      std::string date1 = MakeFixtureDate(seasonYear, matchday);
      std::string date2 = MakeFixtureDate(seasonYear, matchday + 1);
      InsertFixture(managerId, leagueId, seasonYear,
                    1, matchday,
                    teamIds.at(i), teamIds.at(j), date1);
      InsertFixture(managerId, leagueId, seasonYear,
                    2, matchday + 1,
                    teamIds.at(j), teamIds.at(i), date2);
      pairIndex++;
    }
  }
}

static void GenerateStandingsForLeague(int managerId, int leagueId,
                                       const std::vector<int> &teamIds) {
  for (unsigned int i = 0; i < teamIds.size(); i++) {
    std::stringstream q;
    q << "INSERT OR IGNORE INTO standings(manager_id,league_id,team_id)"
      << " VALUES(" << managerId << "," << leagueId << "," << teamIds.at(i) << ");";
    DatabaseResult *r = GetDB()->Query(q.str());
    delete r;
  }
}

void GenerateCareerSeason(int managerId) {
  printf("[CAREER] Generating season for manager %d\n", managerId);
  EnsureCareerTables();
  DeleteCareerSeason(managerId);

  const int seasonYear = GetCurrentYear();

  DatabaseResult *lr = GetDB()->Query("SELECT id FROM leagues ORDER BY id;");
  for (unsigned int i = 0; i < lr->data.size(); i++) {
    int leagueId = atoi(lr->data.at(i).at(0).c_str());
    std::vector<int> teamIds = GetLeagueTeamIds(leagueId);
    GenerateFixturesForLeague(managerId, leagueId, seasonYear, teamIds);
    GenerateStandingsForLeague(managerId, leagueId, teamIds);
  }
  delete lr;

  // Set career start date to July 1 of season year.
  char careerDate[16];
  snprintf(careerDate, sizeof(careerDate), "%04d-07-01", seasonYear);
  std::stringstream uq;
  uq << "UPDATE managers SET current_date='" << careerDate
     << "', season_year=" << seasonYear
     << " WHERE id=" << managerId << ";";
  DatabaseResult *ur = GetDB()->Query(uq.str());
  delete ur;

  printf("[CAREER] Created manager save id=%d season_year=%d current_date=%s\n",
         managerId, seasonYear, careerDate);
  printf("[CAREER] Season generated for manager %d\n", managerId);
}

static void EnsureManagerTable() {
  DatabaseResult *r = GetDB()->Query(
    "CREATE TABLE IF NOT EXISTS managers ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "name VARCHAR(128),"
    "age INTEGER,"
    "nationality VARCHAR(64),"
    "gender VARCHAR(16),"
    "club_id INTEGER,"
    "created_at DATETIME DEFAULT CURRENT_TIMESTAMP"
    ");"
  );
  delete r;
}

static int LastInsertId() {
  DatabaseResult *r = GetDB()->Query("SELECT last_insert_rowid();");
  int id = 0;
  if (r->data.size() > 0 && r->data.at(0).size() > 0)
    id = atoi(r->data.at(0).at(0).c_str());
  delete r;
  return id;
}

static std::string Cell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size() || col >= r->data.at(row).size()) return "";
  return r->data.at(row).at(col);
}

/* ---- Create Profile ---- */

ManagerCreateProfilePage::ManagerCreateProfilePage(
  Gui2WindowManager *wm, const Gui2PageData &pd
) : Gui2Page(wm, pd) {
  EnsureManagerTable();

  // ImGui renders all UI — no Gui2 widgets needed.
  grid = nullptr; nameInput = nullptr; ageInput = nullptr;
  nationalityDropdown = nullptr; genderDropdown = nullptr;
  createButton = nullptr; backButton = nullptr;
  this->Show();

  printf("[IMGUI MAIN MENU] Creating Manager Create Profile page\n");

  g_PreCareer.Clear();
  strncpy(g_PreCareer.nameBuffer, "Manager", sizeof(g_PreCareer.nameBuffer) - 1);
  strncpy(g_PreCareer.ageBuf, "35", sizeof(g_PreCareer.ageBuf) - 1);
  g_PreCareer.nationalityIdx = 0;
  g_PreCareer.genderIdx      = 0;
  g_PreCareer.screen         = PRECAREER_CREATE_PROFILE;
  g_PreCareer.onCreateProfile = boost::bind(&ManagerCreateProfilePage::CreateProfile, this);
  g_PreCareer.onBack          = boost::bind(&ManagerCreateProfilePage::Back, this);
  g_PreCareer.active          = true;

  printf("[IMGUI MAIN MENU] Create Profile page ready\n");
}

ManagerCreateProfilePage::~ManagerCreateProfilePage() {
  if (g_PreCareer.screen == PRECAREER_CREATE_PROFILE)
    g_PreCareer.active = false;
}

void ManagerCreateProfilePage::CreateProfile() {
  static const char *kNats[] = {
    "Portugal","England","Spain","France","Germany",
    "Italy","Netherlands","Brazil","Argentina","United States"
  };
  static const char *kGens[] = { "Male", "Female" };

  std::string name = g_PreCareer.nameBuffer;
  int age = atoi(g_PreCareer.ageBuf);
  int natIdx = g_PreCareer.nationalityIdx;
  int genIdx = g_PreCareer.genderIdx;
  if (natIdx < 0 || natIdx > 9)  natIdx = 0;
  if (genIdx < 0 || genIdx > 1)  genIdx = 0;
  std::string nat = kNats[natIdx];
  std::string gen = kGens[genIdx];

  if (name.empty()) name = "Manager";
  if (age < 18) age = 18;
  if (age > 99) age = 99;

  std::stringstream q;
  q << "INSERT INTO managers(name,age,nationality,gender,club_id) VALUES("
    << "'" << SqlEscape(name) << "',"
    << age << ","
    << "'" << SqlEscape(nat) << "',"
    << "'" << SqlEscape(gen) << "',"
    << "NULL);";

  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;

  int mid = LastInsertId();

  Properties props;
  props.Set("managerId", mid);

  this->Exit();
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_Manager_SelectLeague, props, 0);
  delete this;
}

void ManagerCreateProfilePage::Back() {
  printf("[IMGUI CREATE MANAGER] Processing Back action\n");
  printf("[IMGUI CREATE MANAGER] Returning to Main Menu page\n");
  this->Exit();
  Properties props;
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_MainMenu, props, 0);
  printf("[IMGUI CREATE MANAGER] Main Menu page created\n");
  delete this;
}

/* ---- Select League ---- */

ManagerSelectLeaguePage::ManagerSelectLeaguePage(
  Gui2WindowManager *wm, const Gui2PageData &pd
) : Gui2Page(wm, pd) {
  managerId = pd.properties->GetInt("managerId");

  // ImGui renders all UI — no Gui2 widgets needed.
  grid = nullptr; backButton = nullptr;
  this->Show();

  // Populate league list and activate ImGui.
  g_PreCareer.Clear();
  DatabaseResult *res = GetDB()->Query("SELECT id, name FROM leagues ORDER BY name LIMIT 20;");
  for (unsigned int i = 0; i < res->data.size(); i++) {
    PreCareerState::LeagueItem item;
    item.id   = atoi(Cell(res, i, 0).c_str());
    item.name = Cell(res, i, 1);
    if (item.name.empty()) item.name = "League";
    g_PreCareer.leagues.push_back(item);
  }
  delete res;

  g_PreCareer.screen         = PRECAREER_SELECT_LEAGUE;
  g_PreCareer.onSelectLeague = boost::bind(&ManagerSelectLeaguePage::SelectLeague, this, _1);
  g_PreCareer.onBack         = boost::bind(&ManagerSelectLeaguePage::Back, this);
  g_PreCareer.active         = true;
}

ManagerSelectLeaguePage::~ManagerSelectLeaguePage() {
  if (g_PreCareer.screen == PRECAREER_SELECT_LEAGUE)
    g_PreCareer.active = false;
}

void ManagerSelectLeaguePage::SelectLeague(int leagueId) {
  Properties props;
  props.Set("managerId", managerId);
  props.Set("leagueId", leagueId);

  this->Exit();
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_Manager_SelectClub, props, 0);
  delete this;
}

void ManagerSelectLeaguePage::Back() {
  Properties props;
  props.Set("managerId", managerId);
  this->Exit();
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_Manager_CreateProfile, props, 0);
  delete this;
}

/* ---- Select Club ---- */

ManagerSelectClubPage::ManagerSelectClubPage(
  Gui2WindowManager *wm, const Gui2PageData &pd
) : Gui2Page(wm, pd) {
  managerId      = pd.properties->GetInt("managerId");
  leagueId       = pd.properties->GetInt("leagueId");
  selectedClubId = 0;

  // ImGui renders all UI — no Gui2 widgets needed.
  grid = nullptr; startButton = nullptr; backButton = nullptr;
  this->Show();

  // Populate club list and activate ImGui.
  g_PreCareer.Clear();
  std::stringstream q;
  q << "SELECT id, name, shortname, logo_url FROM teams WHERE league_id = "
    << leagueId << " ORDER BY name LIMIT 20;";
  DatabaseResult *res = GetDB()->Query(q.str());
  for (unsigned int i = 0; i < res->data.size(); i++) {
    PreCareerState::ClubItem item;
    item.id        = atoi(Cell(res, i, 0).c_str());
    item.name      = Cell(res, i, 1);
    item.shortName = Cell(res, i, 2);
    item.logoPath  = Cell(res, i, 3);
    if (item.name.empty()) item.name = "Club";
    g_PreCareer.clubs.push_back(item);
  }
  delete res;

  g_PreCareer.selectedClubId = 0;
  g_PreCareer.screen         = PRECAREER_SELECT_CLUB;
  g_PreCareer.onStartCareer  = boost::bind(&ManagerSelectClubPage::StartCareer, this);
  g_PreCareer.onBack         = boost::bind(&ManagerSelectClubPage::Back, this);
  g_PreCareer.active         = true;
}

ManagerSelectClubPage::~ManagerSelectClubPage() {
  if (g_PreCareer.screen == PRECAREER_SELECT_CLUB)
    g_PreCareer.active = false;
}

void ManagerSelectClubPage::SelectClub(int clubId) {
  selectedClubId = clubId;
  // ImGui reads g_PreCareer.selectedClubId directly for highlight; sync it.
  g_PreCareer.selectedClubId = clubId;
}

void ManagerSelectClubPage::StartCareer() {
  // Read the selection from g_PreCareer (set by ImGui) or the legacy field.
  int clubId = g_PreCareer.selectedClubId > 0
                 ? g_PreCareer.selectedClubId
                 : selectedClubId;
  if (clubId == 0) return;
  selectedClubId = clubId;

  std::stringstream q;
  q << "UPDATE managers SET club_id = " << clubId
    << " WHERE id = " << managerId << ";";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;

  GenerateCareerSeason(managerId);

  Properties props;
  props.Set("managerId", managerId);
  this->Exit();
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_Manager_MainScreen, props, 0);
  delete this;
}

void ManagerSelectClubPage::Back() {
  Properties props;
  props.Set("managerId", managerId);
  this->Exit();
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_Manager_SelectLeague, props, 0);
  delete this;
}

/* ---- Main Career Screen ---- */

ManagerMainScreenPage::ManagerMainScreenPage(
  Gui2WindowManager *wm, const Gui2PageData &pd
) : Gui2Page(wm, pd),
    navGrid(nullptr), managerGrid(nullptr), clubGrid(nullptr),
    matchesGrid(nullptr), standingsGrid(nullptr),
    managerButton(nullptr), clubButton(nullptr),
    matchesButton(nullptr), standingsButton(nullptr),
    playMatchButton(nullptr), mainMenuButton(nullptr)
{
  managerId = pd.properties->GetInt("managerId");
  clubId    = 0;
  activeTab = 0;

  std::stringstream mq;
  mq << "SELECT managers.club_id FROM managers WHERE id = " << managerId << " LIMIT 1;";
  DatabaseResult *mr = GetDB()->Query(mq.str());
  clubId = atoi(Cell(mr, 0, 0).c_str());
  delete mr;

  static const bool useImGuiCareerHub = true;

  if (useImGuiCareerHub) {
    // Wire ImGui action callbacks before loading so they are ready when active=true.
    g_CareerHub.onPlayMatch   = boost::bind(&ManagerMainScreenPage::PlayMatch,   this);
    g_CareerHub.onMainMenu    = boost::bind(&ManagerMainScreenPage::BackToMainMenu, this);
    g_CareerHub.onAdvance     = boost::bind(&ManagerMainScreenPage::AdvanceDay,  this);
    g_CareerHub.onPlayFixture = boost::bind(&ManagerMainScreenPage::PlayFixture, this);
    g_CareerHub.LoadFromDB(managerId, clubId);
    this->Show();
  } else {
    BuildNavigation();
    BuildManagerView();
    BuildClubView();
    BuildMatchesView();
    BuildStandingsView();
    ShowActiveView();
    this->Show();
    g_CareerHub.LoadFromDB(managerId, clubId);
  }
}

ManagerMainScreenPage::~ManagerMainScreenPage() {
  g_CareerHub.Clear();
}

void ManagerMainScreenPage::BuildNavigation() {
  navGrid = new Gui2Grid(windowManager, "mgr_nav_grid", 2, 2, 96, 6);

  managerButton   = new Gui2Button(windowManager, "mgr_nav_manager",    0, 0, 14, 4, "Manager");
  clubButton      = new Gui2Button(windowManager, "mgr_nav_club",       0, 0, 14, 4, "Club");
  matchesButton   = new Gui2Button(windowManager, "mgr_nav_matches",    0, 0, 14, 4, "Matches");
  standingsButton = new Gui2Button(windowManager, "mgr_nav_standings",  0, 0, 14, 4, "Standings");
  playMatchButton = new Gui2Button(windowManager, "mgr_nav_playmatch",  0, 0, 14, 4, "Play Match");
  mainMenuButton  = new Gui2Button(windowManager, "mgr_nav_mainmenu",   0, 0, 14, 4, "Main Menu");

  managerButton->sig_OnClick.connect(
    boost::bind(&ManagerMainScreenPage::OpenTab, this, 0));
  clubButton->sig_OnClick.connect(
    boost::bind(&ManagerMainScreenPage::OpenTab, this, 1));
  matchesButton->sig_OnClick.connect(
    boost::bind(&ManagerMainScreenPage::OpenTab, this, 2));
  standingsButton->sig_OnClick.connect(
    boost::bind(&ManagerMainScreenPage::OpenTab, this, 3));
  playMatchButton->sig_OnClick.connect(
    boost::bind(&ManagerMainScreenPage::PlayMatch, this));
  mainMenuButton->sig_OnClick.connect(
    boost::bind(&ManagerMainScreenPage::BackToMainMenu, this));

  navGrid->AddView(managerButton,   0, 0);
  navGrid->AddView(clubButton,      0, 1);
  navGrid->AddView(matchesButton,   0, 2);
  navGrid->AddView(standingsButton, 0, 3);
  navGrid->AddView(playMatchButton, 0, 4);
  navGrid->AddView(mainMenuButton,  0, 5);

  navGrid->UpdateLayout(0.25, 0.25, 0.25, 0.25);
  this->AddView(navGrid);
  navGrid->Show();
}

void ManagerMainScreenPage::BuildManagerView() {
  managerGrid = new Gui2Grid(windowManager, "mgr_view_manager", 2, 10, 96, 80);

  std::stringstream q;
  q << "SELECT managers.name, managers.age, managers.nationality, managers.gender, teams.name"
    << " FROM managers LEFT JOIN teams ON managers.club_id = teams.id"
    << " WHERE managers.id = " << managerId << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str());

  std::string mgrName   = Cell(r, 0, 0);
  std::string mgrAge    = Cell(r, 0, 1);
  std::string mgrNat    = Cell(r, 0, 2);
  std::string mgrGender = Cell(r, 0, 3);
  std::string clubName  = Cell(r, 0, 4);
  delete r;

  int row = 0;
  managerGrid->AddView(new Gui2Caption(windowManager, "mgr_v_title",  0, 0, 60, 4, "Manager Profile"), row++, 0);
  managerGrid->AddView(new Gui2Caption(windowManager, "mgr_v_name",   0, 0, 60, 3, "Name: " + mgrName), row++, 0);
  managerGrid->AddView(new Gui2Caption(windowManager, "mgr_v_age",    0, 0, 60, 3, "Age: " + mgrAge), row++, 0);
  managerGrid->AddView(new Gui2Caption(windowManager, "mgr_v_nat",    0, 0, 60, 3, "Nationality: " + mgrNat), row++, 0);
  managerGrid->AddView(new Gui2Caption(windowManager, "mgr_v_gender", 0, 0, 60, 3, "Gender: " + mgrGender), row++, 0);
  managerGrid->AddView(new Gui2Caption(windowManager, "mgr_v_club",   0, 0, 60, 3, "Club: " + clubName), row++, 0);

  managerGrid->UpdateLayout(0.25, 0.25, 0.25, 0.25);
  this->AddView(managerGrid);
  managerGrid->Hide();
}

void ManagerMainScreenPage::BuildClubView() {
  clubGrid = new Gui2Grid(windowManager, "mgr_view_club", 2, 10, 96, 80);

  std::stringstream cq;
  cq << "SELECT name, shortname FROM teams WHERE id = " << clubId << " LIMIT 1;";
  DatabaseResult *cr = GetDB()->Query(cq.str());
  std::string clubName  = Cell(cr, 0, 0);
  std::string clubShort = Cell(cr, 0, 1);
  delete cr;

  int row = 0;
  std::string clubHeader = clubShort.empty() ? clubName : clubShort + " - " + clubName;
  clubGrid->AddView(new Gui2Caption(windowManager, "mgr_c_title", 0, 0, 60, 4, clubHeader), row++, 0);
  clubGrid->AddView(new Gui2Caption(windowManager, "mgr_c_squad", 0, 0, 60, 3, "Squad"), row++, 0);

  std::stringstream pq;
  pq << "SELECT firstname, lastname, role, age, base_stat"
     << " FROM players WHERE team_id = " << clubId
     << " ORDER BY formationorder ASC, base_stat DESC LIMIT 22;";
  DatabaseResult *pr = GetDB()->Query(pq.str());
  for (unsigned int i = 0; i < pr->data.size(); i++) {
    std::stringstream line;
    line << Cell(pr, i, 0) << " " << Cell(pr, i, 1)
         << "  " << Cell(pr, i, 2)
         << "  Age " << Cell(pr, i, 3)
         << "  " << Cell(pr, i, 4);
    clubGrid->AddView(new Gui2Caption(windowManager,
      "mgr_c_player_" + int_to_str(i), 0, 0, 60, 3, line.str()), row++, 0);
  }
  delete pr;

  clubGrid->UpdateLayout(0.25, 0.25, 0.25, 0.25);
  this->AddView(clubGrid);
  clubGrid->Hide();
}

void ManagerMainScreenPage::BuildMatchesView() {
  matchesGrid = new Gui2Grid(windowManager, "mgr_view_matches", 2, 10, 96, 80);

  std::stringstream q;
  q << "SELECT leagues.name, fixtures.matchday, fixtures.round,"
    << " home.shortname, away.shortname, fixtures.status,"
    << " fixtures.home_score, fixtures.away_score"
    << " FROM fixtures"
    << " JOIN leagues ON fixtures.league_id = leagues.id"
    << " JOIN teams home ON fixtures.home_team_id = home.id"
    << " JOIN teams away ON fixtures.away_team_id = away.id"
    << " WHERE fixtures.manager_id = " << managerId
    << " ORDER BY leagues.id ASC, fixtures.matchday ASC, fixtures.round ASC;";
  DatabaseResult *r = GetDB()->Query(q.str());

  int row = 0;
  std::string lastLeague = "";
  for (unsigned int i = 0; i < r->data.size(); i++) {
    std::string league   = Cell(r, i, 0);
    std::string matchday = Cell(r, i, 1);
    std::string round    = Cell(r, i, 2);
    std::string home     = Cell(r, i, 3);
    std::string away     = Cell(r, i, 4);
    std::string status   = Cell(r, i, 5);
    std::string hscore   = Cell(r, i, 6);
    std::string ascore   = Cell(r, i, 7);

    if (league != lastLeague) {
      matchesGrid->AddView(new Gui2Caption(windowManager,
        "mgr_m_league_" + int_to_str(row), 0, 0, 90, 4, league), row++, 0);
      lastLeague = league;
    }

    std::stringstream line;
    line << "MD " << matchday << " R" << round
         << "  " << home << " vs " << away
         << "  " << status;
    if (status != "scheduled" && !hscore.empty()) {
      line << "  " << hscore << " - " << ascore;
    }
    matchesGrid->AddView(new Gui2Caption(windowManager,
      "mgr_m_fix_" + int_to_str(row), 0, 0, 90, 3, line.str()), row++, 0);
  }
  delete r;

  if (row == 0) {
    matchesGrid->AddView(new Gui2Caption(windowManager,
      "mgr_m_empty", 0, 0, 90, 3, "No fixtures generated yet."), 0, 0);
  }

  matchesGrid->UpdateLayout(0.25, 0.25, 0.25, 0.25);
  this->AddView(matchesGrid);
  matchesGrid->Hide();
}

void ManagerMainScreenPage::BuildStandingsView() {
  standingsGrid = new Gui2Grid(windowManager, "mgr_view_standings", 2, 10, 96, 80);

  std::stringstream q;
  q << "SELECT leagues.name, teams.shortname,"
    << " standings.played, standings.won, standings.drawn, standings.lost,"
    << " standings.goals_for, standings.goals_against,"
    << " standings.goal_difference, standings.points"
    << " FROM standings"
    << " JOIN leagues ON standings.league_id = leagues.id"
    << " JOIN teams ON standings.team_id = teams.id"
    << " WHERE standings.manager_id = " << managerId
    << " ORDER BY leagues.id ASC, standings.points DESC,"
    << " standings.goal_difference DESC, standings.goals_for DESC;";
  DatabaseResult *r = GetDB()->Query(q.str());

  int row = 0;
  std::string lastLeague = "";
  for (unsigned int i = 0; i < r->data.size(); i++) {
    std::string league = Cell(r, i, 0);
    std::string team   = Cell(r, i, 1);
    std::string played = Cell(r, i, 2);
    std::string won    = Cell(r, i, 3);
    std::string drawn  = Cell(r, i, 4);
    std::string lost   = Cell(r, i, 5);
    std::string gf     = Cell(r, i, 6);
    std::string ga     = Cell(r, i, 7);
    std::string gd     = Cell(r, i, 8);
    std::string pts    = Cell(r, i, 9);

    if (league != lastLeague) {
      standingsGrid->AddView(new Gui2Caption(windowManager,
        "mgr_s_league_" + int_to_str(row), 0, 0, 90, 4, league), row++, 0);
      lastLeague = league;
    }

    std::stringstream line;
    line << team
         << "  P " << played
         << "  W " << won
         << "  D " << drawn
         << "  L " << lost
         << "  GF " << gf
         << "  GA " << ga
         << "  GD " << gd
         << "  Pts " << pts;
    standingsGrid->AddView(new Gui2Caption(windowManager,
      "mgr_s_row_" + int_to_str(row), 0, 0, 90, 3, line.str()), row++, 0);
  }
  delete r;

  if (row == 0) {
    standingsGrid->AddView(new Gui2Caption(windowManager,
      "mgr_s_empty", 0, 0, 90, 3, "No standings generated yet."), 0, 0);
  }

  standingsGrid->UpdateLayout(0.25, 0.25, 0.25, 0.25);
  this->AddView(standingsGrid);
  standingsGrid->Hide();
}

void ManagerMainScreenPage::OpenTab(int tab) {
  if (tab < 0 || tab > 3) return;
  activeTab = tab;
  ShowActiveView();
}

void ManagerMainScreenPage::ShowActiveView() {
  managerGrid->Hide();
  clubGrid->Hide();
  matchesGrid->Hide();
  standingsGrid->Hide();

  switch (activeTab) {
    case 0: managerGrid->Show();   managerButton->SetFocus();   break;
    case 1: clubGrid->Show();      clubButton->SetFocus();      break;
    case 2: matchesGrid->Show();   matchesButton->SetFocus();   break;
    case 3: standingsGrid->Show(); standingsButton->SetFocus(); break;
    default: managerGrid->Show();  managerButton->SetFocus();   break;
  }
}

void ManagerMainScreenPage::PlayMatch() {
  if (clubId == 0) return;

  // Test Engine: hardcoded teams, no fixture context.
  g_CareerMatchContext.Clear();
  printf("[CAREER MATCH] Test engine match; no fixture context\n");

  printf("[IMGUI MANAGER] Setting up controller sides\n");
  std::vector<SideSelection> sides;
  GetMenuTask()->SetControllerSetup(sides);

  std::string team1 = int_to_str(clubId);
  std::string team2 = (clubId == 8) ? "3" : "8";
  printf("[IMGUI MANAGER] Setting teams: %s vs %s\n", team1.c_str(), team2.c_str());
  GetMenuTask()->SetTeamIDs(team1, team2);

  GetConfiguration()->Set("manager_mode",        1.0f);
  GetConfiguration()->Set("manager_ai_difficulty", 1.0f);
  GetConfiguration()->Set("match_difficulty",    1.0f);
  GetConfiguration()->Set("match_duration",      0.0f); // shortest: 5-minute halves
  printf("[MANAGER MODE] Match duration forced to shortest: match_duration=0.0 (5 min halves)\n");

  // Do NOT call CreatePage(LoadingMatch) here — this runs from the GL thread.
  // LoadingMatchPage constructor calls LoadImage which needs the main-thread ObjectFactory.
  // Calling it from the GL thread crashes. Queue it for MenuTask::ProcessPhase (main thread).
  printf("[IMGUI MANAGER] Queued match start in MenuTask\n");
  GetMenuTask()->RequestManagerMatchStart();

  this->Exit();
  delete this;
}

void ManagerMainScreenPage::BackToMainMenu() {
  printf("[IMGUI MANAGER] Returning to main menu\n");
  GetMenuTask()->RequestManagerMainMenuPage();
  this->Exit();
  delete this;
}

// Add exactly one calendar day to an ISO date string YYYY-MM-DD.
static std::string AddOneDay(const std::string &iso) {
  if (iso.size() < 10) return iso;
  int year  = atoi(iso.substr(0, 4).c_str());
  int month = atoi(iso.substr(5, 2).c_str());
  int day   = atoi(iso.substr(8, 2).c_str());
  bool leap = (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
  const int kDIM[] = {0,31,leap?29:28,31,30,31,30,31,31,30,31,30,31};
  day++;
  if (day > kDIM[month]) { day = 1; month++; }
  if (month > 12)        { month = 1; year++; }
  char buf[16];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d", year, month, day);
  return std::string(buf);
}

void ManagerMainScreenPage::AdvanceDay() {
  // Read the stored date from in-memory state — do NOT use SQLite date('now')
  // or any real-world clock; that was the original bug (current_date is also
  // a SQLite keyword for the real date, so date(current_date,'+1 day') read
  // today's real date instead of the column).
  std::string fromDate = g_CareerHub.currentDate;
  if (fromDate.empty()) {
    printf("[CAREER] AdvanceDay: no current_date in state, aborting\n");
    return;
  }

  // Verify what the DB actually holds before touching it.
  {
    std::stringstream rq;
    rq << "SELECT current_date FROM managers WHERE id=" << managerId << ";";
    DatabaseResult *rr = GetDB()->Query(rq.str());
    std::string dbDate = (rr->data.size() > 0 && rr->data.at(0).size() > 0)
                           ? rr->data.at(0).at(0) : "";
    delete rr;
    printf("[CAREER] AdvanceDay before DB current_date=%s\n", dbDate.c_str());
  }

  // Compute next day entirely in C++ — no SQLite date() call to avoid the
  // CURRENT_DATE keyword collision.
  std::string newDate = AddOneDay(fromDate);

  // Write the literal new date string into the DB.
  std::stringstream uq;
  uq << "UPDATE managers SET current_date='" << newDate << "' WHERE id=" << managerId << ";";
  DatabaseResult *ur = GetDB()->Query(uq.str());
  delete ur;

  // Verify the write landed.
  {
    std::stringstream rq;
    rq << "SELECT current_date FROM managers WHERE id=" << managerId << ";";
    DatabaseResult *rr = GetDB()->Query(rq.str());
    std::string dbDate = (rr->data.size() > 0 && rr->data.at(0).size() > 0)
                           ? rr->data.at(0).at(0) : "";
    delete rr;
    printf("[CAREER] AdvanceDay after DB current_date=%s\n", dbDate.c_str());
    if (dbDate.empty()) {
      printf("[CAREER] AdvanceDay ERROR: DB write failed, aborting state reload\n");
      return;
    }
  }

  // Reload career state so the UI reflects the new date and fixture status.
  g_CareerHub.LoadFromDB(managerId, clubId);

  printf("[CAREER] Advance day manager=%d from=%s to=%s\n",
         managerId, fromDate.c_str(), g_CareerHub.currentDate.c_str());
}

void ManagerMainScreenPage::PlayFixture() {
  int homeId    = g_CareerHub.todayFixture.homeTeamId;
  int awayId    = g_CareerHub.todayFixture.awayTeamId;
  int fixtureId = g_CareerHub.todayFixture.id;
  int leagueId  = g_CareerHub.todayFixture.leagueId;

  // Set context so GameOverPage can capture score and update DB.
  g_CareerMatchContext.active     = true;
  g_CareerMatchContext.managerId  = managerId;
  g_CareerMatchContext.fixtureId  = fixtureId;
  g_CareerMatchContext.leagueId   = leagueId;
  g_CareerMatchContext.homeTeamId = homeId;
  g_CareerMatchContext.awayTeamId = awayId;
  printf("[CAREER MATCH] Context set fixture=%d manager=%d league=%d home=%d away=%d\n",
         fixtureId, managerId, leagueId, homeId, awayId);

  printf("[CAREER MATCH] Playing scheduled fixture id=%d date=%s home=%d away=%d\n",
         fixtureId,
         g_CareerHub.todayFixture.fixtureDate.c_str(),
         homeId, awayId);

  std::vector<SideSelection> sides;
  GetMenuTask()->SetControllerSetup(sides);
  GetMenuTask()->SetTeamIDs(int_to_str(homeId), int_to_str(awayId));

  GetConfiguration()->Set("manager_mode",         1.0f);
  GetConfiguration()->Set("manager_ai_difficulty", 1.0f);
  GetConfiguration()->Set("match_difficulty",      1.0f);
  GetConfiguration()->Set("match_duration",        0.0f); // shortest: 5-minute halves
  printf("[MANAGER MODE] Match duration forced to shortest: match_duration=0.0 (5 min halves)\n");

  GetMenuTask()->RequestManagerMatchStart();
  this->Exit();
  delete this;
}

/* ---- Load Game ---- */

ManagerLoadGamePage::ManagerLoadGamePage(
  Gui2WindowManager *windowManager,
  const Gui2PageData &pageData
) : Gui2Page(windowManager, pageData) {
  EnsureManagerTable();

  // ImGui renders all UI — no Gui2 widgets needed.
  grid = nullptr; backButton = nullptr;
  this->Show();

  // Populate save list and activate ImGui.
  g_PreCareer.Clear();
  DatabaseResult *result = GetDB()->Query(
    "SELECT managers.id, managers.name, managers.age, managers.nationality, "
    "teams.name "
    "FROM managers "
    "LEFT JOIN teams ON managers.club_id = teams.id "
    "ORDER BY managers.id DESC LIMIT 20;"
  );
  for (unsigned int i = 0; i < result->data.size(); i++) {
    PreCareerState::SaveEntry e;
    e.id          = atoi(Cell(result, i, 0).c_str());
    e.name        = Cell(result, i, 1);
    e.age         = atoi(Cell(result, i, 2).c_str());
    e.nationality = Cell(result, i, 3);
    e.clubName    = Cell(result, i, 4);
    if (e.name.empty())     e.name     = "Unnamed Manager";
    if (e.clubName.empty()) e.clubName = "No Club";
    g_PreCareer.saves.push_back(e);
  }
  delete result;

  g_PreCareer.screen        = PRECAREER_LOAD_GAME;
  g_PreCareer.onLoadManager = boost::bind(&ManagerLoadGamePage::LoadManager, this, _1);
  g_PreCareer.onBack        = boost::bind(&ManagerLoadGamePage::Back, this);
  g_PreCareer.active        = true;
}

ManagerLoadGamePage::~ManagerLoadGamePage() {
  if (g_PreCareer.screen == PRECAREER_LOAD_GAME)
    g_PreCareer.active = false;
}

void ManagerLoadGamePage::LoadManager(int managerId) {
  Properties properties;
  properties.Set("managerId", managerId);
  this->Exit();
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_Manager_MainScreen, properties, 0);
  delete this;
}

void ManagerLoadGamePage::Back() {
  Properties properties;
  properties.Set("selectedButtonID", 1);
  this->Exit();
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_MainMenu, properties, 0);
  delete this;
}
