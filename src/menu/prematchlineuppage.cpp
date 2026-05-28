#include "prematchlineuppage.hpp"

#include "imgui_career.hpp"
#include "careermatchcontext.hpp"
#include "menutask.hpp"
#include "pagefactory.hpp"
#include "main.hpp"
#include "data/teamdata.hpp"
#include "utils/database.hpp"
#include "base/utils.hpp"

#include <sstream>
#include <cstdio>

using namespace blunted;

// ---------------------------------------------------------------------------

static std::string DBCell(DatabaseResult *r, unsigned int row, unsigned int col) {
  if (!r || row >= r->data.size() || col >= r->data.at(row).size()) return "";
  return r->data.at(row).at(col);
}

static std::string MapCompetitionName(const std::string &raw) {
  if (raw == "Portugal Primeira Liga") return "Liga Portugal BETCLIC";
  if (raw == "Portugal Segunda Liga")  return "Liga Portugal 2";
  return raw;
}

std::vector<PreMatchLineupPlayer>
PreMatchLineupPage::LoadXI(int managerId, int teamId, int limit, int offset) {
  std::vector<PreMatchLineupPlayer> out;
  (void)managerId;

  TeamData teamData(teamId);
  int total = teamData.GetPlayerNum();
  for (int i = 0; i < limit && offset + i < total; i++) {
    PreMatchLineupPlayer p;
    PlayerData *pd = teamData.GetPlayerData(offset + i);
    p.name = pd ? pd->GetDisplayName() : "Player";
    p.number = offset + i;

    // Bench players always show "SUB"; starters use formation XML role
    if (offset > 0)
      p.role = "SUB";
    else if (i >= 0 && i < playerNum)
      p.role = GetRoleName(teamData.GetFormationEntry(i).role);
    else
      p.role = pd ? pd->GetRoleRaw() : "";

    out.push_back(p);
  }

  // Fill missing rows with TBD only for starting XI (offset==0).
  if (offset == 0) {
    while ((int)out.size() < limit) {
      PreMatchLineupPlayer tbd;
      tbd.number = (int)out.size();
      tbd.name   = "TBD";
      out.push_back(tbd);
    }
  }
  return out;
}

// ---------------------------------------------------------------------------

bool PreMatchLineupPage::BuildPresentation() {
  const CareerMatchContext &ctx = g_CareerMatchContext;

  if (!ctx.active || ctx.fixtureId == 0 || ctx.homeTeamId == 0 || ctx.awayTeamId == 0) {
    printf("[PREMATCH] Failed to build presentation; falling back to LoadingMatchPage\n");
    return false;
  }

  printf("[PREMATCH] Building presentation manager=%d fixture=%d\n",
         ctx.managerId, ctx.fixtureId);

  std::stringstream q;
  q << "SELECT f.id, f.league_id, f.home_team_id, f.away_team_id,"
    << " l.name, l.logo_url,"
    << " ht.name, ht.logo_url,"
    << " at.name, at.logo_url"
    << " FROM fixtures f"
    << " JOIN leagues l  ON l.id  = f.league_id"
    << " JOIN teams   ht ON ht.id = f.home_team_id"
    << " JOIN teams   at ON at.id = f.away_team_id"
    << " WHERE f.id = " << ctx.fixtureId
    << " AND f.manager_id = " << ctx.managerId << " LIMIT 1;";
  DatabaseResult *r = GetDB()->Query(q.str());
  if (!r || r->data.empty()) {
    if (r) delete r;
    printf("[PREMATCH] Failed to build presentation; falling back to LoadingMatchPage\n");
    return false;
  }

  g_PreMatchLineup.fixtureId          = atoi(DBCell(r, 0, 0).c_str());
  g_PreMatchLineup.leagueId           = atoi(DBCell(r, 0, 1).c_str());
  g_PreMatchLineup.homeTeamId         = atoi(DBCell(r, 0, 2).c_str());
  g_PreMatchLineup.awayTeamId         = atoi(DBCell(r, 0, 3).c_str());
  g_PreMatchLineup.competitionName    = MapCompetitionName(DBCell(r, 0, 4));
  g_PreMatchLineup.competitionLogoPath = DBCell(r, 0, 5);
  g_PreMatchLineup.homeTeamName       = DBCell(r, 0, 6);
  g_PreMatchLineup.homeBadgePath      = DBCell(r, 0, 7);
  g_PreMatchLineup.awayTeamName       = DBCell(r, 0, 8);
  g_PreMatchLineup.awayBadgePath      = DBCell(r, 0, 9);
  g_PreMatchLineup.managerId          = ctx.managerId;
  delete r;

  printf("[PREMATCH] Home=%d %s Away=%d %s League=%s\n",
         g_PreMatchLineup.homeTeamId, g_PreMatchLineup.homeTeamName.c_str(),
         g_PreMatchLineup.awayTeamId, g_PreMatchLineup.awayTeamName.c_str(),
         g_PreMatchLineup.competitionName.c_str());

  g_PreMatchLineup.homeStartingXI = LoadXI(ctx.managerId, g_PreMatchLineup.homeTeamId, 11, 0);
  g_PreMatchLineup.awayStartingXI = LoadXI(ctx.managerId, g_PreMatchLineup.awayTeamId, 11, 0);
  g_PreMatchLineup.homeBench      = LoadXI(ctx.managerId, g_PreMatchLineup.homeTeamId, 9, 11);
  g_PreMatchLineup.awayBench      = LoadXI(ctx.managerId, g_PreMatchLineup.awayTeamId, 9, 11);
  g_PreMatchLineup.hasBench       = (!g_PreMatchLineup.homeBench.empty() ||
                                     !g_PreMatchLineup.awayBench.empty());

  printf("[PREMATCH] Loaded XI home=%d away=%d bench home=%d away=%d\n",
         (int)g_PreMatchLineup.homeStartingXI.size(),
         (int)g_PreMatchLineup.awayStartingXI.size(),
         (int)g_PreMatchLineup.homeBench.size(),
         (int)g_PreMatchLineup.awayBench.size());

  g_PreMatchLineup.startedAt         = 0.0;
  g_PreMatchLineup.continueRequested = false;
  g_PreMatchLineup.valid             = true;
  g_PreMatchLineup.active            = true;
  return true;
}

// ---------------------------------------------------------------------------

PreMatchLineupPage::PreMatchLineupPage(Gui2WindowManager *windowManager,
                                       const Gui2PageData &pageData)
  : Gui2Page(windowManager, pageData), m_fallback(false) {
  this->Show();
  if (!BuildPresentation()) {
    m_fallback = true;
  }
}

PreMatchLineupPage::~PreMatchLineupPage() {
  // Only clear if we did NOT continue to match — in that case GamePage clears it.
  if (g_PreMatchLineup.active && !g_PreMatchLineup.continueRequested)
    g_PreMatchLineup.Clear();
}

void PreMatchLineupPage::Process() {
  Gui2Page::Process();

  if (m_fallback) {
    // Presentation could not be built — go straight to LoadingMatchPage.
    GetConfiguration()->Set("match_speed_multiplier", (float)g_PreMatchLineup.matchSpeed);
    GetMenuTask()->RequestManagerMatchStart();
    this->Exit();
    delete this;
    return;
  }

  if (!g_PreMatchLineup.continueRequested) return;

  // Keep g_PreMatchLineup.active = true — the card keeps rendering and covers the stadium
  // background while Match::Match() constructs. GamePage clears it when ready.
  printf("[PREMATCH] Starting match load; lineup stays visible as loading cover (speed x%d)\n",
         g_PreMatchLineup.matchSpeed);
  GetConfiguration()->Set("match_speed_multiplier", (float)g_PreMatchLineup.matchSpeed);
  GetMenuTask()->RequestManagerMatchStart(/*skipLoadingVisual=*/true);
  this->Exit();
  delete this;
}
