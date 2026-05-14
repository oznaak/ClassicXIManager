#include "managercareer.hpp"
#include "pagefactory.hpp"
#include "menutask.hpp"

#include "base/utils.hpp"
#include <boost/bind/bind.hpp>
#include <sstream>
#include <cstdlib>

using namespace boost::placeholders;

static std::string SqlEscape(const std::string &in) {
  std::string out;
  for (unsigned int i = 0; i < in.size(); i++) {
    if (in[i] == '\'') out += "''";
    else out += in[i];
  }
  return out;
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

  Gui2Caption *title = new Gui2Caption(wm, "mgr_create_title", 24, 10, 52, 6, "Create Manager Profile");
  this->AddView(title);
  title->Show();

  grid = new Gui2Grid(wm, "mgr_create_grid", 28, 24, 44, 50);

  Gui2Caption *nameLabel = new Gui2Caption(wm, "mgr_name_lbl", 0, 0, 20, 3, "Name");
  Gui2Caption *ageLabel  = new Gui2Caption(wm, "mgr_age_lbl",  0, 0, 20, 3, "Age");
  Gui2Caption *natLabel  = new Gui2Caption(wm, "mgr_nat_lbl",  0, 0, 20, 3, "Nationality");
  Gui2Caption *genLabel  = new Gui2Caption(wm, "mgr_gen_lbl",  0, 0, 20, 3, "Gender");

  nameInput = new Gui2EditLine(wm, "mgr_name_input", 0, 0, 24, 3, "Manager");
  ageInput  = new Gui2EditLine(wm, "mgr_age_input",  0, 0, 24, 3, "35");
  ageInput->SetAllowedChars("0123456789");
  ageInput->SetMaxLength(2);

  nationalityDropdown = new Gui2Pulldown(wm, "mgr_nat_pd", 0, 0, 24, 3);
  nationalityDropdown->AddEntry("Portugal",      "Portugal");
  nationalityDropdown->AddEntry("England",       "England");
  nationalityDropdown->AddEntry("Spain",         "Spain");
  nationalityDropdown->AddEntry("France",        "France");
  nationalityDropdown->AddEntry("Germany",       "Germany");
  nationalityDropdown->AddEntry("Italy",         "Italy");
  nationalityDropdown->AddEntry("Netherlands",   "Netherlands");
  nationalityDropdown->AddEntry("Brazil",        "Brazil");
  nationalityDropdown->AddEntry("Argentina",     "Argentina");
  nationalityDropdown->AddEntry("United States", "United States");
  nationalityDropdown->SetSelected(0);

  genderDropdown = new Gui2Pulldown(wm, "mgr_gen_pd", 0, 0, 24, 3);
  genderDropdown->AddEntry("Male",   "Male");
  genderDropdown->AddEntry("Female", "Female");
  genderDropdown->SetSelected(0);

  createButton = new Gui2Button(wm, "mgr_create_btn", 0, 0, 24, 3, "Create Profile");
  backButton   = new Gui2Button(wm, "mgr_back_btn",   0, 0, 24, 3, "Back");

  createButton->sig_OnClick.connect(boost::bind(&ManagerCreateProfilePage::CreateProfile, this));
  backButton->sig_OnClick.connect(boost::bind(&ManagerCreateProfilePage::Back, this));

  grid->AddView(nameLabel,            0, 0);
  grid->AddView(nameInput,            0, 1);
  grid->AddView(ageLabel,             1, 0);
  grid->AddView(ageInput,             1, 1);
  grid->AddView(natLabel,             2, 0);
  grid->AddView(nationalityDropdown,  2, 1);
  grid->AddView(genLabel,             3, 0);
  grid->AddView(genderDropdown,       3, 1);
  grid->AddView(createButton,         5, 1);
  grid->AddView(backButton,           6, 1);
  grid->UpdateLayout(0.25, 0.25, 0.25, 0.25);

  this->AddView(grid);
  grid->Show();
  nameInput->SetFocus();
  this->Show();
}

ManagerCreateProfilePage::~ManagerCreateProfilePage() {}

void ManagerCreateProfilePage::CreateProfile() {
  std::string name = nameInput->GetText();
  int age = atoi(ageInput->GetText().c_str());
  std::string nat = nationalityDropdown->GetSelected();
  std::string gen = genderDropdown->GetSelected();

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
  this->Exit();
  Properties props;
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_MainMenu, props, 0);
  delete this;
}

/* ---- Select League ---- */

ManagerSelectLeaguePage::ManagerSelectLeaguePage(
  Gui2WindowManager *wm, const Gui2PageData &pd
) : Gui2Page(wm, pd) {
  managerId = pd.properties->GetInt("managerId");

  Gui2Caption *title = new Gui2Caption(wm, "mgr_league_title", 20, 8, 60, 6, "Select League");
  this->AddView(title);
  title->Show();

  grid = new Gui2Grid(wm, "mgr_league_grid", 24, 18, 52, 70);

  DatabaseResult *res = GetDB()->Query(
    "SELECT id, name FROM leagues ORDER BY name LIMIT 12;"
  );

  for (unsigned int i = 0; i < res->data.size(); i++) {
    int lid = atoi(Cell(res, i, 0).c_str());
    std::string lname = Cell(res, i, 1);
    if (lname.empty()) lname = "League";

    Gui2Button *btn = new Gui2Button(wm,
      "mgr_league_btn_" + Cell(res, i, 0), 0, 0, 36, 3, lname);
    btn->sig_OnClick.connect(
      boost::bind(&ManagerSelectLeaguePage::SelectLeague, this, lid));
    leagueButtons.push_back(btn);
    grid->AddView(btn, (int)i, 0);
  }

  delete res;

  backButton = new Gui2Button(wm, "mgr_league_back", 0, 0, 36, 3, "Back");
  backButton->sig_OnClick.connect(
    boost::bind(&ManagerSelectLeaguePage::Back, this));
  grid->AddView(backButton, (int)leagueButtons.size() + 1, 0);

  grid->UpdateLayout(0.25, 0.25, 0.25, 0.25);
  this->AddView(grid);
  grid->Show();
  if (!leagueButtons.empty()) leagueButtons.at(0)->SetFocus();
  this->Show();
}

ManagerSelectLeaguePage::~ManagerSelectLeaguePage() {}

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
  managerId     = pd.properties->GetInt("managerId");
  leagueId      = pd.properties->GetInt("leagueId");
  selectedClubId = 0;

  Gui2Caption *title = new Gui2Caption(wm, "mgr_club_title", 20, 8, 60, 6, "Select Club");
  this->AddView(title);
  title->Show();

  grid = new Gui2Grid(wm, "mgr_club_grid", 24, 18, 52, 70);

  std::stringstream q;
  q << "SELECT id, name, shortname FROM teams WHERE league_id = "
    << leagueId << " ORDER BY name LIMIT 14;";

  DatabaseResult *res = GetDB()->Query(q.str());

  for (unsigned int i = 0; i < res->data.size(); i++) {
    int cid = atoi(Cell(res, i, 0).c_str());
    std::string cname = Cell(res, i, 1);
    std::string sname = Cell(res, i, 2);
    if (!sname.empty()) cname = sname + " - " + cname;

    Gui2Button *btn = new Gui2Button(wm,
      "mgr_club_btn_" + Cell(res, i, 0), 0, 0, 42, 3, cname);
    btn->sig_OnClick.connect(
      boost::bind(&ManagerSelectClubPage::SelectClub, this, cid));
    clubButtons.push_back(btn);
    grid->AddView(btn, (int)i, 0);
  }

  delete res;

  startButton = new Gui2Button(wm, "mgr_start_career", 0, 0, 42, 3, "Start Career");
  startButton->SetActive(false);
  startButton->sig_OnClick.connect(
    boost::bind(&ManagerSelectClubPage::StartCareer, this));

  backButton = new Gui2Button(wm, "mgr_club_back", 0, 0, 42, 3, "Back");
  backButton->sig_OnClick.connect(
    boost::bind(&ManagerSelectClubPage::Back, this));

  grid->AddView(startButton, (int)clubButtons.size() + 1, 0);
  grid->AddView(backButton,  (int)clubButtons.size() + 2, 0);

  grid->UpdateLayout(0.25, 0.25, 0.25, 0.25);
  this->AddView(grid);
  grid->Show();
  if (!clubButtons.empty()) clubButtons.at(0)->SetFocus();
  this->Show();
}

ManagerSelectClubPage::~ManagerSelectClubPage() {}

void ManagerSelectClubPage::SelectClub(int clubId) {
  selectedClubId = clubId;
  startButton->SetActive(true);
  startButton->SetFocus();
}

void ManagerSelectClubPage::StartCareer() {
  if (selectedClubId == 0) return;

  std::stringstream q;
  q << "UPDATE managers SET club_id = " << selectedClubId
    << " WHERE id = " << managerId << ";";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;

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
) : Gui2Page(wm, pd) {
  managerId = pd.properties->GetInt("managerId");
  clubId    = 0;

  std::stringstream mq;
  mq << "SELECT managers.name, managers.age, managers.nationality, managers.gender,"
     << " managers.club_id, teams.name"
     << " FROM managers LEFT JOIN teams ON managers.club_id = teams.id"
     << " WHERE managers.id = " << managerId << " LIMIT 1;";

  DatabaseResult *mr = GetDB()->Query(mq.str());
  std::string mgrName   = Cell(mr, 0, 0);
  std::string mgrAge    = Cell(mr, 0, 1);
  std::string mgrNat    = Cell(mr, 0, 2);
  std::string mgrGender = Cell(mr, 0, 3);
  clubId                = atoi(Cell(mr, 0, 4).c_str());
  std::string clubName  = Cell(mr, 0, 5);
  delete mr;

  Gui2Caption *title = new Gui2Caption(wm, "mgr_main_title", 8, 7, 84, 5, "Career");
  this->AddView(title);
  title->Show();

  std::stringstream hdr;
  hdr << mgrName << "  |  Age " << mgrAge
      << "  |  " << mgrNat << "  |  " << mgrGender
      << "  |  Club: " << clubName;
  Gui2Caption *header = new Gui2Caption(wm, "mgr_main_hdr", 8, 14, 84, 4, hdr.str());
  this->AddView(header);
  header->Show();

  grid = new Gui2Grid(wm, "mgr_main_grid", 8, 22, 84, 60);

  Gui2Caption *pTitle = new Gui2Caption(wm, "mgr_players_title", 0, 0, 60, 3, "Squad");
  grid->AddView(pTitle, 0, 0);

  std::stringstream pq;
  pq << "SELECT firstname, lastname, role, age, base_stat"
     << " FROM players WHERE team_id = " << clubId
     << " ORDER BY formationorder ASC, base_stat DESC LIMIT 18;";

  DatabaseResult *pr = GetDB()->Query(pq.str());
  for (unsigned int i = 0; i < pr->data.size(); i++) {
    std::stringstream line;
    line << Cell(pr, i, 0) << " " << Cell(pr, i, 1)
         << "  " << Cell(pr, i, 2)
         << "  Age " << Cell(pr, i, 3)
         << "  " << Cell(pr, i, 4);
    Gui2Caption *pl = new Gui2Caption(wm,
      "mgr_player_" + int_to_str(i), 0, 0, 60, 3, line.str());
    grid->AddView(pl, (int)i + 1, 0);
  }
  delete pr;

  playMatchButton = new Gui2Button(wm, "mgr_play_match",  0, 0, 24, 3, "Play Match");
  backButton      = new Gui2Button(wm, "mgr_main_back",   0, 0, 24, 3, "Main Menu");

  playMatchButton->sig_OnClick.connect(
    boost::bind(&ManagerMainScreenPage::PlayMatch, this));
  backButton->sig_OnClick.connect(
    boost::bind(&ManagerMainScreenPage::BackToMainMenu, this));

  grid->AddView(playMatchButton, 0, 1);
  grid->AddView(backButton,      1, 1);

  grid->UpdateLayout(0.25, 0.25, 0.25, 0.25);
  this->AddView(grid);
  grid->Show();
  playMatchButton->SetFocus();
  this->Show();
}

ManagerMainScreenPage::~ManagerMainScreenPage() {}

void ManagerMainScreenPage::PlayMatch() {
  if (clubId == 0) return;

  std::vector<SideSelection> sides;
  GetMenuTask()->SetControllerSetup(sides);

  std::string team1 = int_to_str(clubId);
  std::string team2 = (clubId == 8) ? "3" : "8";
  GetMenuTask()->SetTeamIDs(team1, team2);

  GetConfiguration()->Set("manager_mode",        1.0f);
  GetConfiguration()->Set("manager_ai_difficulty", 1.0f);
  GetConfiguration()->Set("match_difficulty",    1.0f);

  Properties props;
  this->Exit();
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_LoadingMatch, props, 0);
  delete this;
}

void ManagerMainScreenPage::BackToMainMenu() {
  this->Exit();
  Properties props;
  windowManager->GetPageFactory()->CreatePage((int)e_PageID_MainMenu, props, 0);
  delete this;
}

/* ---- Load Game ---- */

ManagerLoadGamePage::ManagerLoadGamePage(
  Gui2WindowManager *windowManager,
  const Gui2PageData &pageData
) : Gui2Page(windowManager, pageData) {
  EnsureManagerTable();

  Gui2Caption *title = new Gui2Caption(windowManager, "manager_load_title", 20, 8, 60, 6, "Load Game");
  this->AddView(title);
  title->Show();

  grid = new Gui2Grid(windowManager, "manager_load_grid", 22, 18, 56, 66);

  DatabaseResult *result = GetDB()->Query(
    "SELECT managers.id, managers.name, managers.age, managers.nationality, managers.gender, "
    "teams.name "
    "FROM managers "
    "LEFT JOIN teams ON managers.club_id = teams.id "
    "ORDER BY managers.id DESC "
    "LIMIT 20;"
  );

  if (result->data.size() == 0) {
    Gui2Caption *emptyText = new Gui2Caption(windowManager, "manager_load_empty", 0, 0, 54, 3, "No saved managers found.");
    grid->AddView(emptyText, 0, 0);
  }

  for (unsigned int i = 0; i < result->data.size(); i++) {
    int managerId = atoi(Cell(result, i, 0).c_str());
    std::string managerName = Cell(result, i, 1);
    std::string age = Cell(result, i, 2);
    std::string nationality = Cell(result, i, 3);
    std::string gender = Cell(result, i, 4);
    std::string clubName = Cell(result, i, 5);

    if (managerName.empty()) managerName = "Unnamed Manager";
    if (clubName.empty()) clubName = "No Club";

    std::stringstream label;
    label << managerName << " | " << clubName << " | Age " << age << " | " << nationality << " | " << gender;

    Gui2Button *button = new Gui2Button(windowManager, "manager_load_button_" + Cell(result, i, 0), 0, 0, 54, 3, label.str());
    button->sig_OnClick.connect(boost::bind(&ManagerLoadGamePage::LoadManager, this, managerId));
    managerButtons.push_back(button);
    grid->AddView(button, i, 0);
  }

  delete result;

  backButton = new Gui2Button(windowManager, "manager_load_back", 0, 0, 54, 3, "Back");
  backButton->sig_OnClick.connect(boost::bind(&ManagerLoadGamePage::Back, this));
  grid->AddView(backButton, managerButtons.size() + 2, 0);

  grid->UpdateLayout(0.25, 0.25, 0.25, 0.25);
  this->AddView(grid);
  grid->Show();

  if (managerButtons.size() > 0) {
    managerButtons.at(0)->SetFocus();
  } else {
    backButton->SetFocus();
  }

  this->Show();
}

ManagerLoadGamePage::~ManagerLoadGamePage() {}

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
