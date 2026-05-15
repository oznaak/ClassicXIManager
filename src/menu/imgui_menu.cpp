#include "imgui_menu.hpp"
#include "imgui_manager_fonts.hpp"
#include "managercareer.hpp"

#include "../main.hpp"
#include "imgui.h"
#include <SDL2/SDL_image.h>
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl.h>
#endif
#include <sstream>
#include <string>
#include <map>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <cmath>

// ---- Global state -------------------------------------------------------

PreCareerState g_PreCareer;

void PreCareerState::Clear() {
  active        = false;
  screen        = PRECAREER_NONE;
  pendingAction = 0;
  pendingId     = 0;
  isTransitioning = false;

  onNewGame      = nullptr;
  onLoadGame     = nullptr;
  onSettings     = nullptr;
  onQuit         = nullptr;
  onCreateProfile = nullptr;
  onBack         = nullptr;
  onLoadManager  = nullptr;
  onSelectLeague = nullptr;
  onStartCareer  = nullptr;

  memset(nameBuffer, 0, sizeof(nameBuffer));
  memset(ageBuf, 0, sizeof(ageBuf));
  nationalityIdx = 0;
  genderIdx      = 0;

  currentManagerId = 0;
  currentLeagueId = 0;
  leagues.clear();
  clubs.clear();
  selectedClubId = 0;
  saves.clear();
}

static std::string SqlEscape(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    if (c == '\'') out += "''";
    else out += c;
  }
  return out;
}

static int LastInsertId() {
  DatabaseResult *r = GetDB()->Query("SELECT last_insert_rowid();");
  int id = 0;
  if (r->data.size() > 0 && r->data.at(0).size() > 0)
    id = atoi(r->data.at(0).at(0).c_str());
  delete r;
  return id;
}

static void LoadPreCareerSaves() {
  g_PreCareer.saves.clear();
  DatabaseResult *result = GetDB()->Query(
    "SELECT managers.id, managers.name, managers.age, managers.nationality, "
    "teams.name, teams.shortname, teams.logo_url "
    "FROM managers "
    "LEFT JOIN teams ON managers.club_id = teams.id "
    "ORDER BY managers.id DESC LIMIT 20;"
  );
  for (unsigned int i = 0; i < result->data.size(); i++) {
    PreCareerState::SaveEntry e;
    e.id          = atoi(result->data.at(i).at(0).c_str());
    e.name        = result->data.at(i).at(1);
    e.age         = atoi(result->data.at(i).at(2).c_str());
    e.nationality = result->data.at(i).at(3);
    e.clubName    = result->data.at(i).at(4);
    e.shortName   = result->data.at(i).at(5);
    e.logoUrl     = result->data.at(i).at(6);
    if (e.name.empty())     e.name     = "Unnamed Manager";
    if (e.clubName.empty()) e.clubName = "No Club";
    if (e.shortName.empty()) e.shortName = e.clubName.empty() ? "NC" : e.clubName.substr(0, 2);
    g_PreCareer.saves.push_back(e);
  }
  delete result;
}

static void LoadPreCareerLeagues() {
  g_PreCareer.leagues.clear();
  DatabaseResult *res = GetDB()->Query("SELECT id, name, logo_url FROM leagues ORDER BY name LIMIT 20;");
  for (unsigned int i = 0; i < res->data.size(); i++) {
    PreCareerState::LeagueItem item;
    item.id   = atoi(res->data.at(i).at(0).c_str());
    item.name = res->data.at(i).at(1);
    item.logoUrl = res->data.at(i).at(2);
    if (item.name.empty()) item.name = "League";
    g_PreCareer.leagues.push_back(item);
  }
  delete res;
}

static void LoadPreCareerClubs(int leagueId) {
  g_PreCareer.clubs.clear();
  std::stringstream q;
  q << "SELECT id, name, shortname, logo_url FROM teams WHERE league_id = "
    << leagueId << " ORDER BY name LIMIT 20;";
  DatabaseResult *res = GetDB()->Query(q.str());
  for (unsigned int i = 0; i < res->data.size(); i++) {
    PreCareerState::ClubItem item;
    item.id        = atoi(res->data.at(i).at(0).c_str());
    item.name      = res->data.at(i).at(1);
    item.shortName = res->data.at(i).at(2);
    item.logoPath  = res->data.at(i).at(3);
    if (item.name.empty()) item.name = "Club";
    g_PreCareer.clubs.push_back(item);
  }
  delete res;
}

static void EnterPreCareerMainMenu() {
  g_PreCareer.Clear();
  g_PreCareer.screen = PRECAREER_MAIN_MENU;
  g_PreCareer.active = true;
}

static void EnterPreCareerCreateProfile() {
  if (g_PreCareer.currentManagerId == 0) {
    strncpy(g_PreCareer.nameBuffer, "Manager", sizeof(g_PreCareer.nameBuffer) - 1);
    strncpy(g_PreCareer.ageBuf, "35", sizeof(g_PreCareer.ageBuf) - 1);
    g_PreCareer.nationalityIdx = 0;
    g_PreCareer.genderIdx = 0;
  }
  g_PreCareer.screen = PRECAREER_CREATE_PROFILE;
  g_PreCareer.active = true;
}

static void EnterPreCareerLoadGame() {
  g_PreCareer.Clear();
  LoadPreCareerSaves();
  g_PreCareer.screen = PRECAREER_LOAD_GAME;
  g_PreCareer.active = true;
}

static void EnterPreCareerSelectLeague(int managerId) {
  g_PreCareer.currentManagerId = managerId;
  g_PreCareer.currentLeagueId = 0;
  g_PreCareer.selectedClubId = 0;
  LoadPreCareerLeagues();
  g_PreCareer.screen = PRECAREER_SELECT_LEAGUE;
  g_PreCareer.active = true;
}

static void EnterPreCareerSelectClub(int managerId, int leagueId) {
  g_PreCareer.currentManagerId = managerId;
  g_PreCareer.currentLeagueId = leagueId;
  g_PreCareer.selectedClubId = 0;
  LoadPreCareerClubs(leagueId);
  g_PreCareer.screen = PRECAREER_SELECT_CLUB;
  g_PreCareer.active = true;
}

static void EnterPreCareerSettingsPlaceholder() {
  g_PreCareer.screen = PRECAREER_SETTINGS_PLACEHOLDER;
  g_PreCareer.active = true;
}

static void PreCareerStartCareer() {
  int managerId = g_PreCareer.currentManagerId;
  int clubId = g_PreCareer.selectedClubId;
  if (managerId == 0 || clubId == 0) return;

  std::stringstream q;
  q << "UPDATE managers SET club_id = " << clubId
    << " WHERE id = " << managerId << ";";
  DatabaseResult *r = GetDB()->Query(q.str());
  delete r;

  GenerateCareerSeason(managerId);
  GetMenuTask()->RequestManagerCareerPage(managerId);

  g_PreCareer.Clear();
  g_PreCareer.screen = PRECAREER_NONE;
}

static void PreCareerLoadManager(int managerId) {
  GetMenuTask()->RequestManagerCareerPage(managerId);
  g_PreCareer.Clear();
  g_PreCareer.screen = PRECAREER_NONE;
}

static void ProcessPreCareerPendingAction() {
  if (g_PreCareer.pendingAction == 0) return;

  int action = g_PreCareer.pendingAction;
  int pid = g_PreCareer.pendingId;
  g_PreCareer.pendingAction = 0;
  g_PreCareer.pendingId = 0;

  switch (action) {
    case 1:
      EnterPreCareerCreateProfile();
      break;
    case 2:
      EnterPreCareerLoadGame();
      break;
    case 3:
      EnterPreCareerSettingsPlaceholder();
      break;
    case 4:
      GetMenuTask()->QuitGame();
      break;
    case 5: {
      std::string name = g_PreCareer.nameBuffer;
      int age = atoi(g_PreCareer.ageBuf);
      int natIdx = g_PreCareer.nationalityIdx;
      int genIdx = g_PreCareer.genderIdx;
      static const char *kNats[] = {
        "Portugal","England","Spain","France","Germany",
        "Italy","Netherlands","Brazil","Argentina","United States"
      };
      static const char *kGens[] = { "Male", "Female" };
      if (natIdx < 0 || natIdx > 9) natIdx = 0;
      if (genIdx < 0 || genIdx > 1) genIdx = 0;
      std::string nat = kNats[natIdx];
      std::string gen = kGens[genIdx];
      if (name.empty()) name = "Manager";
      if (age < 18) age = 18;
      if (age > 99) age = 99;

      std::stringstream q;
      q << "INSERT INTO managers(name,age,nationality,gender,club_id) VALUES(";
      q << "'" << SqlEscape(name) << "',";
      q << age << ",";
      q << "'" << SqlEscape(nat) << "',";
      q << "'" << SqlEscape(gen) << "',";
      q << "NULL);";
      DatabaseResult *r = GetDB()->Query(q.str());
      delete r;
      int managerId = LastInsertId();
      EnterPreCareerSelectLeague(managerId);
    } break;
    case 6:
      switch (g_PreCareer.screen) {
        case PRECAREER_CREATE_PROFILE:
        case PRECAREER_LOAD_GAME:
        case PRECAREER_SETTINGS_PLACEHOLDER:
          EnterPreCareerMainMenu();
          break;
        case PRECAREER_SELECT_LEAGUE:
          EnterPreCareerCreateProfile();
          break;
        case PRECAREER_SELECT_CLUB:
          EnterPreCareerSelectLeague(g_PreCareer.currentManagerId);
          break;
        default:
          EnterPreCareerMainMenu();
          break;
      }
      break;
    case 7:
      PreCareerLoadManager(pid);
      break;
    case 8:
      EnterPreCareerSelectClub(g_PreCareer.currentManagerId, pid);
      break;
    case 10:
      PreCareerStartCareer();
      break;
    default:
      break;
  }
}

// ---- Image texture cache ------------------------------------------------

static std::map<std::string, GLuint> s_ImageCache;

static GLuint TryLoadImage(const std::string &path) {
  printf("[IMG LOAD] trying: %s\n", path.c_str());
  SDL_Surface *surf = IMG_Load(path.c_str());
  if (!surf) {
    printf("[IMG LOAD] failed: %s (SDL_image: %s)\n", path.c_str(), IMG_GetError());
    return 0;
  }
  SDL_Surface *rgba = SDL_ConvertSurfaceFormat(surf, SDL_PIXELFORMAT_RGBA32, 0);
  SDL_FreeSurface(surf);
  if (!rgba) {
    printf("[IMG LOAD] failed: %s (RGBA convert)\n", path.c_str());
    return 0;
  }

  GLuint texID = 0;
  glGenTextures(1, &texID);
  glBindTexture(GL_TEXTURE_2D, texID);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, rgba->w, rgba->h, 0,
               GL_RGBA, GL_UNSIGNED_BYTE, rgba->pixels);
  glBindTexture(GL_TEXTURE_2D, 0);
  SDL_FreeSurface(rgba);

  printf("[IMG LOAD] success: %s texture=%u size=%dx%d\n",
         path.c_str(), texID, (int)rgba->w, (int)rgba->h);
  return texID;
}

static GLuint LoadImageTexture(const std::string &relPath) {
  if (relPath.empty()) return 0;

  std::string dbRelPath = "databases/default/" + relPath;

  auto it = s_ImageCache.find(dbRelPath);
  if (it != s_ImageCache.end()) return it->second;

  GLuint texID = 0;

  if (relPath.find("media/") == 0 || relPath.find("data/") == 0) {
    std::string p1 = relPath;
    std::string p2 = "data/" + relPath;
    std::string p3 = "../data/" + relPath;
    const char *aPaths[] = { p1.c_str(), p2.c_str(), p3.c_str(), nullptr };
    for (int i = 0; aPaths[i]; i++) {
      texID = TryLoadImage(aPaths[i]);
      if (texID) break;
    }
  } else {
    const char *basePaths[] = {
      "databases/default/",
      "data/databases/default/",
      "../data/databases/default/",
      nullptr
    };
    for (int i = 0; basePaths[i]; i++) {
      std::string fullPath = std::string(basePaths[i]) + relPath;
      texID = TryLoadImage(fullPath);
      if (texID) break;
    }
  }

  if (!texID) {
    printf("[IMG LOAD] failed: %s (all paths exhausted)\n", relPath.c_str());
  }

  s_ImageCache[dbRelPath] = texID;
  return texID;
}

static GLuint GetMainLogoTexture() {
  static GLuint s_logoTex = 0;
  static bool s_tried = false;
  if (s_tried) return s_logoTex;
  s_tried = true;

  const char *logoPaths[] = {
    "media/logos/logo.png",
    "data/media/logos/logo.png",
    "../data/media/logos/logo.png",
    nullptr
  };

  for (int i = 0; logoPaths[i]; i++) {
    s_logoTex = TryLoadImage(logoPaths[i]);
    if (s_logoTex) break;
  }

  s_ImageCache["__main_logo__"] = s_logoTex;
  return s_logoTex;
}

static void ClearImageCache() {
  for (auto &kv : s_ImageCache)
    if (kv.second) glDeleteTextures(1, &kv.second);
  s_ImageCache.clear();
}

// ---- Draw image badge (texture or fallback) -----------------------------

static void DrawImageBadge(GLuint tex, const std::string &sn, float sz) {
  if (tex) {
    ImGui::Image((ImTextureID)(intptr_t)tex, ImVec2(sz, sz));
  } else {
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    unsigned int hash = 5381;
    for (char c : sn) hash = ((hash << 5) + hash) ^ (unsigned char)c;
    float hue = (float)(hash % 360) / 360.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.48f, r, g, b);
    dl->AddRectFilled(p, ImVec2(p.x+sz, p.y+sz),
      IM_COL32((int)(r*255),(int)(g*255),(int)(b*255),215), sz*0.22f);
    std::string ini;
    for (unsigned int i = 0; i < sn.size() && (int)ini.size() < 2; i++)
      if (isalpha((unsigned char)sn[i])) ini += (char)toupper((unsigned char)sn[i]);
    if (!ini.empty()) {
      ImVec2 tsz = ImGui::CalcTextSize(ini.c_str());
      dl->AddText(ImVec2(p.x+(sz-tsz.x)*0.5f, p.y+(sz-tsz.y)*0.5f),
                  IM_COL32(255,255,255,215), ini.c_str());
    }
    ImGui::Dummy(ImVec2(sz, sz));
  }
}

static void DrawImageBadgeAt(ImDrawList *dl, ImVec2 pos, GLuint tex,
                              const std::string &sn, float sz) {
  if (tex) {
    dl->AddImage((ImTextureID)(intptr_t)tex, pos, ImVec2(pos.x+sz, pos.y+sz));
  } else {
    unsigned int hash = 5381;
    for (char c : sn) hash = ((hash << 5) + hash) ^ (unsigned char)c;
    float hue = (float)(hash % 360) / 360.0f;
    float r, g, b;
    ImGui::ColorConvertHSVtoRGB(hue, 0.55f, 0.48f, r, g, b);
    dl->AddRectFilled(pos, ImVec2(pos.x+sz, pos.y+sz),
      IM_COL32((int)(r*255),(int)(g*255),(int)(b*255),215), sz*0.22f);
    std::string ini;
    for (unsigned int i = 0; i < sn.size() && (int)ini.size() < 2; i++)
      if (isalpha((unsigned char)sn[i])) ini += (char)toupper((unsigned char)sn[i]);
    if (!ini.empty()) {
      ImVec2 tsz = ImGui::CalcTextSize(ini.c_str());
      dl->AddText(ImVec2(pos.x+(sz-tsz.x)*0.5f, pos.y+(sz-tsz.y)*0.5f),
                  IM_COL32(255,255,255,215), ini.c_str());
    }
  }
}

// ---- Color palette ------------------------------------------------------

static const ImVec4 kBgApp     = ImVec4(0.027f, 0.043f, 0.086f, 1.0f);
static const ImVec4 kBgSidebar = ImVec4(0.043f, 0.063f, 0.125f, 1.0f);
static const ImVec4 kBgCard    = ImVec4(0.071f, 0.102f, 0.173f, 1.0f);
static const ImVec4 kBgCardAlt = ImVec4(0.094f, 0.129f, 0.212f, 1.0f);
static const ImVec4 kBorder    = ImVec4(0.149f, 0.196f, 0.290f, 0.80f);
static const ImVec4 kAccent    = ImVec4(0.741f, 0.102f, 0.788f, 1.0f);
static const ImVec4 kAccentH   = ImVec4(0.863f, 0.318f, 0.918f, 1.0f);
static const ImVec4 kAccentA   = ImVec4(0.576f, 0.047f, 0.620f, 1.0f);
static const ImVec4 kViolet    = ImVec4(0.482f, 0.231f, 0.929f, 1.0f);
static const ImVec4 kTextPri   = ImVec4(0.937f, 0.949f, 0.965f, 1.0f);
static const ImVec4 kTextSec   = ImVec4(0.612f, 0.655f, 0.729f, 1.0f);
static const ImVec4 kTextDim   = ImVec4(0.239f, 0.290f, 0.388f, 1.0f);
static const ImVec4 kSuccess   = ImVec4(0.133f, 0.773f, 0.369f, 1.0f);
static const ImVec4 kDanger    = ImVec4(0.937f, 0.267f, 0.267f, 1.0f);
static const ImVec4 kGold      = ImVec4(0.992f, 0.820f, 0.110f, 1.0f);

static inline ImU32 C32(const ImVec4 &v) { return ImGui::ColorConvertFloat4ToU32(v); }

// ---- Font helpers -------------------------------------------------------

static inline void PushMF(ImFont *f) { if (f) ImGui::PushFont(f); }
static inline void PopMF(ImFont *f)  { if (f) ImGui::PopFont(); }

// ---- Centering helpers --------------------------------------------------

static float CenterX(float parentW, float itemW) {
  return (parentW - itemW) * 0.5f;
}

static void SetCenteredTextX(const char *text, float parentW) {
  float textW = ImGui::CalcTextSize(text).x;
  ImGui::SetCursorPosX((parentW - textW) * 0.5f);
}

static ImVec2 CenteredCardPos(ImVec2 cardSize, float yOffset = 0.0f) {
  ImVec2 d = ImGui::GetIO().DisplaySize;
  return ImVec2((d.x - cardSize.x) * 0.5f, (d.y - cardSize.y) * 0.5f + yOffset);
}

static ImVec2 CenteredPos(ImVec2 size) {
  ImVec2 d = ImGui::GetIO().DisplaySize;
  return ImVec2((d.x - size.x) * 0.5f, (d.y - size.y) * 0.5f);
}

static ImVec2 CenteredPosWithYOffset(ImVec2 size, float yOffset) {
  ImVec2 d = ImGui::GetIO().DisplaySize;
  return ImVec2((d.x - size.x) * 0.5f, (d.y - size.y) * 0.5f + yOffset);
}

static void CenterNextItem(float itemWidth) {
  float avail = ImGui::GetContentRegionAvail().x;
  if (avail > itemWidth) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail - itemWidth) * 0.5f);
}

// ---- Theme --------------------------------------------------------------

static void ApplyPreCareerTheme() {
  ImGuiStyle &st = ImGui::GetStyle();
  st.WindowRounding    = 0.0f;
  st.ChildRounding     = 10.0f;
  st.FrameRounding     = 6.0f;
  st.ScrollbarRounding = 4.0f;
  st.GrabRounding      = 4.0f;
  st.WindowBorderSize  = 0.0f;
  st.ChildBorderSize   = 0.0f;
  st.FrameBorderSize   = 0.0f;
  st.WindowPadding     = ImVec2(0.0f, 0.0f);
  st.FramePadding      = ImVec2(10.0f, 6.0f);
  st.ItemSpacing       = ImVec2(8.0f, 6.0f);
  st.ScrollbarSize     = 6.0f;

  ImVec4 *c = st.Colors;
  c[ImGuiCol_WindowBg]             = kBgApp;
  c[ImGuiCol_ChildBg]              = kBgCard;
  c[ImGuiCol_PopupBg]              = kBgCard;
  c[ImGuiCol_Border]               = kBorder;
  c[ImGuiCol_BorderShadow]         = ImVec4(0,0,0,0);
  c[ImGuiCol_FrameBg]              = ImVec4(0.035f,0.055f,0.102f,1.0f);
  c[ImGuiCol_FrameBgHovered]       = ImVec4(0.055f,0.082f,0.153f,1.0f);
  c[ImGuiCol_FrameBgActive]        = ImVec4(0.082f,0.122f,0.220f,1.0f);
  c[ImGuiCol_Button]               = ImVec4(0.055f,0.082f,0.153f,1.0f);
  c[ImGuiCol_ButtonHovered]        = ImVec4(0.082f,0.122f,0.220f,1.0f);
  c[ImGuiCol_ButtonActive]         = ImVec4(0.110f,0.161f,0.282f,1.0f);
  c[ImGuiCol_Header]               = ImVec4(0.082f,0.122f,0.220f,0.60f);
  c[ImGuiCol_HeaderHovered]        = ImVec4(0.110f,0.161f,0.282f,0.80f);
  c[ImGuiCol_HeaderActive]         = ImVec4(0.176f,0.243f,0.396f,1.0f);
  c[ImGuiCol_Separator]            = kBorder;
  c[ImGuiCol_ScrollbarBg]          = ImVec4(0,0,0,0);
  c[ImGuiCol_ScrollbarGrab]        = ImVec4(0.110f,0.161f,0.282f,1.0f);
  c[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.176f,0.243f,0.396f,1.0f);
  c[ImGuiCol_ScrollbarGrabActive]  = kViolet;
  c[ImGuiCol_CheckMark]            = kAccent;
  c[ImGuiCol_Text]                 = kTextPri;
  c[ImGuiCol_TextDisabled]         = kTextDim;
}

// ---- Button helpers -----------------------------------------------------

static bool CTABtn(const char *lbl, ImVec2 sz = ImVec2(0,0)) {
  ImGui::PushStyleColor(ImGuiCol_Button,        kAccent);
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentH);
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  kAccentA);
  bool r = ImGui::Button(lbl, sz);
  ImGui::PopStyleColor(3);
  return r;
}

static bool SecBtn(const char *lbl, ImVec2 sz = ImVec2(0,0)) {
  ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.055f,0.082f,0.153f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.082f,0.122f,0.220f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.110f,0.161f,0.282f,1.0f));
  bool r = ImGui::Button(lbl, sz);
  ImGui::PopStyleColor(3);
  return r;
}

static bool DangerBtn(const char *lbl, ImVec2 sz = ImVec2(0,0)) {
  ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.10f,0.04f,0.04f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f,0.07f,0.07f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.26f,0.10f,0.10f,1.0f));
  ImGui::PushStyleColor(ImGuiCol_Text,          kDanger);
  bool r = ImGui::Button(lbl, sz);
  ImGui::PopStyleColor(4);
  return r;
}

// ---- Decorative background (no purple circles) --------------------------

static void DrawPreCareerBackground(float w, float h) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  dl->AddRectFilled(wp, ImVec2(wp.x+w, wp.y+h), C32(kBgApp));
  dl->AddRectFilledMultiColor(wp, ImVec2(wp.x+w, wp.y+90.0f),
    IM_COL32(16,26,62,30), IM_COL32(16,26,62,30),
    IM_COL32(0,0,0,0), IM_COL32(0,0,0,0));
}

// ---- Card drawing helpers -----------------------------------------------

static void DrawCardRect(ImVec2 pos, ImVec2 size) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 p0 = ImVec2(wp.x + pos.x, wp.y + pos.y);
  ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
  dl->AddRectFilled(p0, p1, C32(kBgCard), 14.0f);
  dl->AddRect(p0, p1, C32(kBorder), 14.0f, 0, 1.0f);
  dl->AddLine(ImVec2(p0.x+16,p0.y+1), ImVec2(p1.x-16,p0.y+1),
              IM_COL32(255,255,255,6), 1.0f);
}

static void BeginCardContent(ImVec2 pos, ImVec2 size, const char *id) {
  ImGui::SetCursorPos(pos);
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(40.0f, 28.0f));
  ImGui::BeginChild(id, size, false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor();
}

static void EndCardContent() { ImGui::EndChild(); }

// ---- Shared footer helper -----------------------------------------------

static void DrawPreCareerFooter(float winW, float winH) {
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 fp = ImVec2(wp.x + winW * 0.5f, wp.y + winH - 28.0f);
  ImVec2 fsz = ImGui::CalcTextSize("Classic Manager - Alpha v0.0.01");
  ImDrawList *dl = ImGui::GetWindowDrawList();
  PushMF(g_ManagerFontSmall);
  dl->AddText(ImVec2(fp.x - fsz.x * 0.5f, fp.y), C32(kTextDim), "Classic Manager - Alpha v0.0.01");
  PopMF(g_ManagerFontSmall);
}

// =========================================================================
// Screen: Main Menu
// =========================================================================

static void DrawMainMenuScreen(float winW, float winH) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 display = ImGui::GetIO().DisplaySize;

  const float kLeftW = winW * 0.50f;

  // ---- Left region: logo + text ---------------------------------------
  ImGui::SetCursorPos(ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0,0,0,0));
  ImGui::BeginChild("##mm_left", ImVec2(kLeftW, winH), false,
                    ImGuiWindowFlags_NoScrollbar);
  ImGui::PopStyleColor();

  GLuint logoTex = GetMainLogoTexture();
  const float kLogoSz = 280.0f;

  float logoX = (kLeftW - kLogoSz) * 0.5f;
  float logoY = (winH - kLogoSz) * 0.5f - 20.0f;
  if (logoY < 20.0f) logoY = 20.0f;

  if (logoTex) {
    dl->AddImage((ImTextureID)(intptr_t)logoTex,
                 ImVec2(wp.x + logoX, wp.y + logoY),
                 ImVec2(wp.x + logoX + kLogoSz, wp.y + logoY + kLogoSz));
  }

  const char *sub1 = "Build your career. Shape your club.";
  const char *sub2 = "Watch football unfold.";
  ImVec2 sz1 = ImGui::CalcTextSize(sub1);
  ImVec2 sz2 = ImGui::CalcTextSize(sub2);
  float textY = logoY + kLogoSz + 16.0f;

  PushMF(g_ManagerFontRegular);
  dl->AddText(ImVec2(wp.x + (kLeftW - sz1.x) * 0.5f, wp.y + textY),
              C32(kTextSec), sub1);
  dl->AddText(ImVec2(wp.x + (kLeftW - sz2.x) * 0.5f, wp.y + textY + 22.0f),
              C32(kTextSec), sub2);
  PopMF(g_ManagerFontRegular);

  ImGui::EndChild();

  // ---- Vertical accent line -------------------------------------------
  dl->AddLine(
    ImVec2(wp.x + kLeftW, wp.y + winH * 0.15f),
    ImVec2(wp.x + kLeftW, wp.y + winH * 0.85f),
    C32(kBorder), 1.0f);

  // ---- Right action card ----------------------------------------------
  const float kCardW = 520.0f;
  const float kCardH = 360.0f;
  ImVec2 cardPos = CenteredCardPos(ImVec2(kCardW, kCardH), 0.0f);
  cardPos.x = kLeftW + (winW - kLeftW - kCardW) * 0.5f;

  DrawCardRect(cardPos, ImVec2(kCardW, kCardH));
  BeginCardContent(cardPos, ImVec2(kCardW, kCardH), "##mm_card");

  float buttonW = 380.0f;
  float buttonH = 52.0f;
  float gap = 18.0f;
  float totalH = buttonH * 4.0f + gap * 3.0f;
  float startY = (kCardH - totalH) * 0.5f;
  float buttonX = (kCardW - buttonW) * 0.5f;

  PushMF(g_ManagerFontBold);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));

  const char *btnLabels[] = { "New Career", "Load Career", "Settings", "Quit" };
  for (int i = 0; i < 4; i++) {
    float btnY = startY + i * (buttonH + gap);
    ImGui::SetCursorPos(ImVec2(buttonX, btnY));

    if (i == 0) {
      if (CTABtn(btnLabels[i], ImVec2(buttonW, buttonH))) {
        printf("[IMGUI MAIN MENU] New Game requested\n");
        g_PreCareer.pendingAction = 1;
      }
    } else if (i == 3) {
      if (DangerBtn(btnLabels[i], ImVec2(buttonW, buttonH))) {
        printf("[IMGUI MAIN MENU] Quit requested\n");
        g_PreCareer.pendingAction = 4;
      }
    } else {
      if (SecBtn(btnLabels[i], ImVec2(buttonW, buttonH))) {
        printf("[IMGUI MAIN MENU] %s requested\n", btnLabels[i]);
        g_PreCareer.pendingAction = i + 1;
      }
    }
  }

  ImGui::PopStyleVar();
  PopMF(g_ManagerFontBold);

  EndCardContent();

  DrawPreCareerFooter(winW, winH);
}

// =========================================================================
// Screen: Create Manager Profile
// =========================================================================

static const char *kNationalities[] = {
  "Portugal", "England", "Spain", "France", "Germany",
  "Italy", "Netherlands", "Brazil", "Argentina", "United States"
};
static const int kNationalityCount = 10;

static const char *kGenders[] = { "Male", "Female" };
static const int kGenderCount  = 2;

static void DrawCreateProfileScreen(float winW, float winH) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 display = ImGui::GetIO().DisplaySize;

  const float kLogoW = 110.0f;
  const float kLogoH = 110.0f;
  const float kLogoGap = 24.0f;
  const float kCardW = 620.0f;
  const float kCardH = 560.0f;
  const float kPad = 40.0f;

  GLuint logoTex = GetMainLogoTexture();

  float combinedH = kLogoH + kLogoGap + kCardH;
  float logoX = (display.x - kLogoW) * 0.5f;
  float logoY = (display.y - combinedH) * 0.5f;
  if (logoY < 10.0f) logoY = 10.0f;

  float cardX = (display.x - kCardW) * 0.5f;
  float cardY = logoY + kLogoH + kLogoGap;

  if (logoTex) {
    dl->AddImage((ImTextureID)(intptr_t)logoTex,
                 ImVec2(wp.x + logoX, wp.y + logoY),
                 ImVec2(wp.x + logoX + kLogoW, wp.y + logoY + kLogoH));
  }

  DrawCardRect(ImVec2(cardX, cardY), ImVec2(kCardW, kCardH));
  BeginCardContent(ImVec2(cardX, cardY), ImVec2(kCardW, kCardH), "##cp_card");

  PushMF(g_ManagerFontTitle);
  float titleW = ImGui::CalcTextSize("Create Manager").x;
  ImGui::SetCursorPosX((kCardW - titleW) * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Create Manager");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontTitle);

  const char *desc = "Define your identity before the season begins.";
  PushMF(g_ManagerFontSmall);
  float descW = ImGui::CalcTextSize(desc).x;
  ImGui::SetCursorPosX((kCardW - descW) * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted(desc);
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);

  ImGui::Dummy(ImVec2(0, 20.0f));

  float formW = kCardW - kPad * 2.0f;
  float inputH = 34.0f;

  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("MANAGER NAME");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::SetNextItemWidth(formW);
  PushMF(g_ManagerFontRegular);
  ImGui::InputText("##mgr_name", g_PreCareer.nameBuffer,
                   sizeof(g_PreCareer.nameBuffer));
  PopMF(g_ManagerFontRegular);

  ImGui::Dummy(ImVec2(0, 12.0f));

  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("AGE  (18 \xe2\x80\x93 99)");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::SetNextItemWidth(formW);
  PushMF(g_ManagerFontRegular);
  ImGui::InputText("##mgr_age", g_PreCareer.ageBuf,
                   sizeof(g_PreCareer.ageBuf),
                   ImGuiInputTextFlags_CharsDecimal);
  PopMF(g_ManagerFontRegular);

  ImGui::Dummy(ImVec2(0, 12.0f));

  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("NATIONALITY");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::SetNextItemWidth(formW);
  PushMF(g_ManagerFontRegular);
  ImGui::Combo("##mgr_nat", &g_PreCareer.nationalityIdx,
               kNationalities, kNationalityCount);
  PopMF(g_ManagerFontRegular);

  ImGui::Dummy(ImVec2(0, 12.0f));

  PushMF(g_ManagerFontSmall);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted("GENDER");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);
  ImGui::SetNextItemWidth(formW);
  PushMF(g_ManagerFontRegular);
  ImGui::Combo("##mgr_gen", &g_PreCareer.genderIdx,
               kGenders, kGenderCount);
  PopMF(g_ManagerFontRegular);

  ImGui::Dummy(ImVec2(0, 24.0f));

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
  PushMF(g_ManagerFontBold);

  if (CTABtn("Create Manager Profile", ImVec2(formW, 52.0f)))
    g_PreCareer.pendingAction = 5;

  ImGui::Dummy(ImVec2(0, 6.0f));

  if (SecBtn("Back", ImVec2(formW, 40.0f))) {
    printf("[IMGUI CREATE MANAGER] Back requested\n");
    g_PreCareer.pendingAction = 6;
  }

  PopMF(g_ManagerFontBold);
  ImGui::PopStyleVar();

  EndCardContent();

  DrawPreCareerFooter(winW, winH);
}

// =========================================================================
// Screen: Settings
// =========================================================================

static void DrawSettingsPlaceholderScreen(float winW, float winH) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 display = ImGui::GetIO().DisplaySize;

  const float kLogoW = 100.0f;
  const float kLogoH = 100.0f;
  const float kLogoGap = 24.0f;
  const float kCardW = 520.0f;
  const float kCardH = 320.0f;

  GLuint logoTex = GetMainLogoTexture();

  float combinedH = kLogoH + kLogoGap + kCardH;
  float logoX = (display.x - kLogoW) * 0.5f;
  float logoY = (display.y - combinedH) * 0.5f;
  if (logoY < 10.0f) logoY = 10.0f;
  float cardX = (display.x - kCardW) * 0.5f;
  float cardY = logoY + kLogoH + kLogoGap;

  if (logoTex) {
    dl->AddImage((ImTextureID)(intptr_t)logoTex,
                 ImVec2(wp.x + logoX, wp.y + logoY),
                 ImVec2(wp.x + logoX + kLogoW, wp.y + logoY + kLogoH));
  }

  DrawCardRect(ImVec2(cardX, cardY), ImVec2(kCardW, kCardH));
  BeginCardContent(ImVec2(cardX, cardY), ImVec2(kCardW, kCardH), "##settings_card");

  PushMF(g_ManagerFontTitle);
  float titleW = ImGui::CalcTextSize("Settings").x;
  ImGui::SetCursorPosX((kCardW - titleW) * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Settings");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontTitle);

  ImGui::Dummy(ImVec2(0, 14.0f));

  const char *desc = "Settings screen coming soon.";
  float descW = ImGui::CalcTextSize(desc).x;
  ImGui::SetCursorPosX((kCardW - descW) * 0.5f);
  PushMF(g_ManagerFontRegular);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
  ImGui::TextUnformatted(desc);
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontRegular);

  ImGui::Dummy(ImVec2(0, 24.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
  PushMF(g_ManagerFontBold);

  float backW = 180.0f;
  float backX = (kCardW - backW) * 0.5f;
  ImGui::SetCursorPosX(backX);
  if (SecBtn("Back", ImVec2(backW, 40.0f))) {
    printf("[IMGUI MAIN MENU] Settings Back requested\n");
    g_PreCareer.pendingAction = 6;
  }
  PopMF(g_ManagerFontBold);
  ImGui::PopStyleVar();

  EndCardContent();

  DrawPreCareerFooter(winW, winH);
}

// =========================================================================
// Screen: Load Game
// =========================================================================

static void DrawLoadGameScreen(float winW, float winH) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 display = ImGui::GetIO().DisplaySize;

  const float kLogoW = 100.0f;
  const float kLogoH = 100.0f;
  const float kLogoGap = 24.0f;
  const float kCardW = 760.0f;
  const float kCardH = 640.0f;

  GLuint logoTex = GetMainLogoTexture();

  float combinedH = kLogoH + kLogoGap + kCardH;
  float logoX = (display.x - kLogoW) * 0.5f;
  float logoY = (display.y - combinedH) * 0.5f;
  if (logoY < 10.0f) logoY = 10.0f;
  float cardX = (display.x - kCardW) * 0.5f;
  float cardY = logoY + kLogoH + kLogoGap;

  if (logoTex) {
    dl->AddImage((ImTextureID)(intptr_t)logoTex,
                 ImVec2(wp.x + logoX, wp.y + logoY),
                 ImVec2(wp.x + logoX + kLogoW, wp.y + logoY + kLogoH));
  }

  DrawCardRect(ImVec2(cardX, cardY), ImVec2(kCardW, kCardH));
  BeginCardContent(ImVec2(cardX, cardY), ImVec2(kCardW, kCardH), "##lg_card");

  PushMF(g_ManagerFontTitle);
  float titleW = ImGui::CalcTextSize("Load Career").x;
  ImGui::SetCursorPosX((kCardW - titleW) * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Load Career");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontTitle);

  ImGui::Dummy(ImVec2(0, 4.0f));

  char subBuf[64];
  snprintf(subBuf, sizeof(subBuf), "%d career%s found", (int)g_PreCareer.saves.size(),
           g_PreCareer.saves.size() == 1 ? "" : "s");
  PushMF(g_ManagerFontSmall);
  float subW = ImGui::CalcTextSize(subBuf).x;
  ImGui::SetCursorPosX((kCardW - subW) * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted(subBuf);
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);

  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, 6.0f));

  float rowW = kCardW - 80.0f;
  float rowH = 86.0f;
  float rowX = (kCardW - rowW) * 0.5f;
  float rowGap = 16.0f;
  float listStartY = 120.0f;
  float badgeSize = 48.0f;
  float badgeX = 20.0f;
  float textX = badgeX + badgeSize + 18.0f;
  float textGap = 6.0f;

  if (g_PreCareer.saves.empty()) {
    ImGui::Dummy(ImVec2(0, 24.0f));
    PushMF(g_ManagerFontBold);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No careers found.");
    ImGui::PopStyleColor();
    PopMF(g_ManagerFontBold);
    ImGui::Dummy(ImVec2(0, 6.0f));
    PushMF(g_ManagerFontSmall);
    ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
    ImGui::TextUnformatted("Start a new career to begin.");
    ImGui::PopStyleColor();
    PopMF(g_ManagerFontSmall);
  }

  for (unsigned int i = 0; i < g_PreCareer.saves.size(); i++) {
    const auto &s = g_PreCareer.saves.at(i);
    float rowY = listStartY + i * (rowH + rowGap);

    ImVec2 rp0 = ImVec2(wp.x + cardX + rowX, wp.y + cardY + rowY);
    ImVec2 rp1 = ImVec2(rp0.x + rowW, rp0.y + rowH);

    char rowId[32]; snprintf(rowId, sizeof(rowId), "##lg_row_%d", s.id);
    ImGui::SetCursorPos(ImVec2(rowX, rowY));
    bool clicked = ImGui::InvisibleButton(rowId, ImVec2(rowW, rowH));
    bool hov = ImGui::IsItemHovered();

    if (hov)
      dl->AddRectFilled(rp0, rp1, C32(kBgCardAlt), 8.0f);
    else
      dl->AddRectFilled(rp0, rp1, IM_COL32(18,28,52,120), 8.0f);
    dl->AddRect(rp0, rp1, C32(kBorder), 8.0f, 0, 0.8f);

    GLuint badgeTex = LoadImageTexture(s.logoUrl);
    float badgeY = (rowH - badgeSize) * 0.5f;
    DrawImageBadgeAt(dl, ImVec2(rp0.x + badgeX, rp0.y + badgeY), badgeTex, s.shortName, badgeSize);

    char sub[160];
    snprintf(sub, sizeof(sub), "%s%s%s%s Age %d",
             s.clubName.empty() ? "" : s.clubName.c_str(),
             s.clubName.empty() ? "" : " \xe2\x80\xa2 ",
             s.nationality.empty() ? "" : s.nationality.c_str(),
             s.nationality.empty() ? "" : " \xe2\x80\xa2 ",
             s.age);

    float titleTextH = ImGui::CalcTextSize(s.name.c_str()).y;
    float subTextH = ImGui::CalcTextSize(sub).y;
    float textBlockH = titleTextH + textGap + subTextH;
    float textY = (rowH - textBlockH) * 0.5f;

    PushMF(g_ManagerFontBold);
    dl->AddText(ImVec2(rp0.x + textX, rp0.y + textY),
                C32(hov ? kAccent : kTextPri), s.name.c_str());
    PopMF(g_ManagerFontBold);

    PushMF(g_ManagerFontSmall);
    dl->AddText(ImVec2(rp0.x + textX, rp0.y + textY + titleTextH + textGap),
                C32(kTextSec), sub);
    PopMF(g_ManagerFontSmall);

    if (clicked) {
      g_PreCareer.pendingAction = 7;
      g_PreCareer.pendingId     = s.id;
    }
  }

  float backW = rowW;
  float backH = 48.0f;
  float backY = kCardH - backH - 24.0f;

  ImGui::SetCursorPos(ImVec2(rowX, backY));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
  PushMF(g_ManagerFontBold);
  if (SecBtn("Back", ImVec2(backW, backH)))
    g_PreCareer.pendingAction = 6;
  PopMF(g_ManagerFontBold);
  ImGui::PopStyleVar();

  EndCardContent();

  DrawPreCareerFooter(winW, winH);
}

// =========================================================================
// Screen: Select League
// =========================================================================

static void DrawSelectLeagueScreen(float winW, float winH) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 display = ImGui::GetIO().DisplaySize;

  const float kLogoW = 100.0f;
  const float kLogoH = 100.0f;
  const float kLogoGap = 24.0f;
  const float kCardW = 760.0f;
  const float kCardH = 520.0f;

  GLuint logoTex = GetMainLogoTexture();

  float combinedH = kLogoH + kLogoGap + kCardH;
  float logoX = (display.x - kLogoW) * 0.5f;
  float logoY = (display.y - combinedH) * 0.5f;
  if (logoY < 10.0f) logoY = 10.0f;
  float cardX = (display.x - kCardW) * 0.5f;
  float cardY = logoY + kLogoH + kLogoGap;

  if (logoTex) {
    dl->AddImage((ImTextureID)(intptr_t)logoTex,
                 ImVec2(wp.x + logoX, wp.y + logoY),
                 ImVec2(wp.x + logoX + kLogoW, wp.y + logoY + kLogoH));
  }

  DrawCardRect(ImVec2(cardX, cardY), ImVec2(kCardW, kCardH));
  BeginCardContent(ImVec2(cardX, cardY), ImVec2(kCardW, kCardH), "##sl_card");

  PushMF(g_ManagerFontTitle);
  float titleW = ImGui::CalcTextSize("Select League").x;
  ImGui::SetCursorPosX((kCardW - titleW) * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Select League");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontTitle);

  ImGui::Dummy(ImVec2(0, 4.0f));

  const char *desc = "Choose the competition you will manage in.";
  PushMF(g_ManagerFontSmall);
  float descW = ImGui::CalcTextSize(desc).x;
  ImGui::SetCursorPosX((kCardW - descW) * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted(desc);
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);

  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, 8.0f));

  if (g_PreCareer.leagues.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No leagues found in database.");
    ImGui::PopStyleColor();
  }

  float pad = 36.0f;
  float gapX = 20.0f;
  float gapY = 18.0f;
  float tileW = (kCardW - pad * 2.0f - gapX) * 0.5f;
  float tileH = 86.0f;
  float gridX = pad;
  float gridY = 120.0f;

  for (unsigned int i = 0; i < g_PreCareer.leagues.size(); i++) {
    const auto &lg = g_PreCareer.leagues.at(i);
    int col = i % 2;
    int row = i / 2;
    float x = gridX + col * (tileW + gapX);
    float y = gridY + row * (tileH + gapY);

    ImVec2 tp0 = ImVec2(wp.x + cardX + x, wp.y + cardY + y);
    ImVec2 tp1 = ImVec2(tp0.x + tileW, tp0.y + tileH);

    char bid[32]; snprintf(bid, sizeof(bid), "##sl_lg_%d", lg.id);
    ImGui::SetCursorPos(ImVec2(x, y));
    bool clicked = ImGui::InvisibleButton(bid, ImVec2(tileW, tileH));
    bool hov = ImGui::IsItemHovered();

    dl->AddRectFilled(tp0, tp1,
      hov ? C32(kBgCardAlt) : IM_COL32(18,28,52,100), 8.0f);
    dl->AddRect(tp0, tp1, C32(kBorder), 8.0f, 0, 0.8f);
    if (hov) dl->AddRectFilled(tp0, ImVec2(tp0.x+3.0f, tp1.y), C32(kAccent), 2.0f);

    GLuint leagueTex = LoadImageTexture(lg.logoUrl);
    float logoBadgeSz = 48.0f;
    float badgeY = y + (tileH - logoBadgeSz) * 0.5f;
    DrawImageBadgeAt(dl, ImVec2(tp0.x + 10.0f, tp0.y + (tileH - logoBadgeSz) * 0.5f), leagueTex, lg.name, logoBadgeSz);

    float textX = x + 68.0f;
    float nameH = ImGui::CalcTextSize(lg.name.c_str()).y;
    float textY = y + (tileH - nameH) * 0.5f;

    ImFont *bf = hov ? g_ManagerFontBold : g_ManagerFontRegular;
    if (bf) ImGui::PushFont(bf);
    dl->AddText(ImVec2(wp.x + cardX + textX, wp.y + cardY + textY),
                C32(hov ? kTextPri : kTextSec), lg.name.c_str());
    if (bf) ImGui::PopFont();

    if (clicked) {
      g_PreCareer.pendingAction = 8;
      g_PreCareer.pendingId     = lg.id;
    }
  }

  float backW = kCardW - pad * 2.0f;
  float backH = 48.0f;
  float backY = kCardH - backH - 28.0f;

  ImGui::SetCursorPos(ImVec2(pad, backY));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
  PushMF(g_ManagerFontBold);
  if (SecBtn("Back", ImVec2(backW, backH)))
    g_PreCareer.pendingAction = 6;
  PopMF(g_ManagerFontBold);
  ImGui::PopStyleVar();

  EndCardContent();

  DrawPreCareerFooter(winW, winH);
}

// =========================================================================
// Screen: Select Club
// =========================================================================

static void DrawSelectClubScreen(float winW, float winH) {
  ImDrawList *dl = ImGui::GetWindowDrawList();
  ImVec2 wp = ImGui::GetWindowPos();
  ImVec2 display = ImGui::GetIO().DisplaySize;

  const float kLogoW = 100.0f;
  const float kLogoH = 100.0f;
  const float kLogoGap = 24.0f;
  const float kCardW = 760.0f;
  const float kCardH = 520.0f;

  GLuint logoTex = GetMainLogoTexture();

  float combinedH = kLogoH + kLogoGap + kCardH;
  float logoX = (display.x - kLogoW) * 0.5f;
  float logoY = (display.y - combinedH) * 0.5f;
  if (logoY < 10.0f) logoY = 10.0f;
  float cardX = (display.x - kCardW) * 0.5f;
  float cardY = logoY + kLogoH + kLogoGap;

  if (logoTex) {
    dl->AddImage((ImTextureID)(intptr_t)logoTex,
                 ImVec2(wp.x + logoX, wp.y + logoY),
                 ImVec2(wp.x + logoX + kLogoW, wp.y + logoY + kLogoH));
  }

  DrawCardRect(ImVec2(cardX, cardY), ImVec2(kCardW, kCardH));
  BeginCardContent(ImVec2(cardX, cardY), ImVec2(kCardW, kCardH), "##sc_card");

  PushMF(g_ManagerFontTitle);
  float titleW = ImGui::CalcTextSize("Select Club").x;
  ImGui::SetCursorPosX((kCardW - titleW) * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextPri);
  ImGui::TextUnformatted("Select Club");
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontTitle);

  ImGui::Dummy(ImVec2(0, 4.0f));

  const char *desc = "Click a club to select it, then press Start Career.";
  PushMF(g_ManagerFontSmall);
  float descW = ImGui::CalcTextSize(desc).x;
  ImGui::SetCursorPosX((kCardW - descW) * 0.5f);
  ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
  ImGui::TextUnformatted(desc);
  ImGui::PopStyleColor();
  PopMF(g_ManagerFontSmall);

  ImGui::PushStyleColor(ImGuiCol_Separator, kBorder);
  ImGui::Separator();
  ImGui::PopStyleColor();
  ImGui::Dummy(ImVec2(0, 8.0f));

  if (g_PreCareer.clubs.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, kTextSec);
    ImGui::TextUnformatted("No clubs found in this league.");
    ImGui::PopStyleColor();
  }

  float pad = 36.0f;
  float gapX = 20.0f;
  float gapY = 18.0f;
  float tileW = (kCardW - pad * 2.0f - gapX) * 0.5f;
  float tileH = 96.0f;
  float gridX = pad;
  float gridY = 120.0f;
  float badgeSz = 48.0f;

  for (unsigned int i = 0; i < g_PreCareer.clubs.size(); i++) {
    const auto &cl = g_PreCareer.clubs.at(i);
    bool sel = (g_PreCareer.selectedClubId == cl.id);
    int col = i % 2;
    int row = i / 2;
    float x = gridX + col * (tileW + gapX);
    float y = gridY + row * (tileH + gapY);

    ImVec2 tp0 = ImVec2(wp.x + cardX + x, wp.y + cardY + y);
    ImVec2 tp1 = ImVec2(tp0.x + tileW, tp0.y + tileH);

    char bid[32]; snprintf(bid, sizeof(bid), "##sc_cl_%d", cl.id);
    ImGui::SetCursorPos(ImVec2(x, y));
    bool clicked = ImGui::InvisibleButton(bid, ImVec2(tileW, tileH));
    bool hov = ImGui::IsItemHovered();

    if (sel)
      dl->AddRectFilled(tp0, tp1, IM_COL32(80,20,145,185), 8.0f);
    else if (hov)
      dl->AddRectFilled(tp0, tp1, C32(kBgCardAlt), 8.0f);
    else
      dl->AddRectFilled(tp0, tp1, IM_COL32(18,28,52,100), 8.0f);
    dl->AddRect(tp0, tp1, sel ? C32(kAccent) : C32(kBorder), 8.0f, 0, sel ? 1.5f : 0.8f);

    const std::string &sn = cl.shortName.empty() ? cl.name : cl.shortName;
    GLuint badgeTex = LoadImageTexture(cl.logoPath);
    DrawImageBadgeAt(dl, ImVec2(tp0.x + 10.0f, tp0.y + (tileH - badgeSz) * 0.5f), badgeTex, sn, badgeSz);

    float nameH = ImGui::CalcTextSize(cl.name.c_str()).y;
    float textY = y + (tileH - nameH) * 0.5f;

    ImFont *bf = sel ? g_ManagerFontBold : (hov ? g_ManagerFontBold : g_ManagerFontRegular);
    if (bf) ImGui::PushFont(bf);
    ImVec4 nc = sel ? kAccent : (hov ? kTextPri : kTextSec);
    dl->AddText(ImVec2(wp.x + cardX + x + 68.0f, wp.y + cardY + textY),
                C32(nc), cl.name.c_str());
    if (bf) ImGui::PopFont();

    if (sel) {
      PushMF(g_ManagerFontSmall);
      ImVec2 tsz = ImGui::CalcTextSize("Selected");
      dl->AddText(ImVec2(wp.x + cardX + x + tileW - tsz.x - 12.0f, wp.y + cardY + textY),
                  C32(kGold), "Selected");
      PopMF(g_ManagerFontSmall);
    }

    if (clicked) g_PreCareer.selectedClubId = cl.id;
  }

  ImGui::Dummy(ImVec2(0, 12.0f));

  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(12.0f, 8.0f));
  PushMF(g_ManagerFontBold);

  float btnW = kCardW - pad * 2.0f;
  float btnX = pad;

  bool canStart = (g_PreCareer.selectedClubId > 0);
  ImGui::SetCursorPosX(btnX);
  if (!canStart) {
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.40f);
    SecBtn("Start Career", ImVec2(btnW, 52.0f));
    ImGui::PopStyleVar();
  } else {
    if (CTABtn("Start Career", ImVec2(btnW, 52.0f)))
      g_PreCareer.pendingAction = 10;
  }

  ImGui::Dummy(ImVec2(0, 6.0f));

  ImGui::SetCursorPosX(btnX);
  if (SecBtn("Back", ImVec2(btnW, 40.0f)))
    g_PreCareer.pendingAction = 6;

  PopMF(g_ManagerFontBold);
  ImGui::PopStyleVar();

  EndCardContent();

  DrawPreCareerFooter(winW, winH);
}

// =========================================================================
// Entry point
// =========================================================================

void RenderImGuiPreCareer() {
  if (!g_PreCareer.active || g_PreCareer.isTransitioning) return;

  ApplyPreCareerTheme();

  ImGuiIO &io = ImGui::GetIO();
  float winW = io.DisplaySize.x;
  float winH = io.DisplaySize.y;

  ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(winW, winH), ImGuiCond_Always);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##precareer_root", nullptr,
               ImGuiWindowFlags_NoTitleBar          |
               ImGuiWindowFlags_NoResize            |
               ImGuiWindowFlags_NoMove              |
               ImGuiWindowFlags_NoCollapse          |
               ImGuiWindowFlags_NoBringToFrontOnFocus |
               ImGuiWindowFlags_NoScrollbar         |
               ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleVar();

  DrawPreCareerBackground(winW, winH);

  switch (g_PreCareer.screen) {
    case PRECAREER_MAIN_MENU:
      DrawMainMenuScreen(winW, winH);
      break;
    case PRECAREER_CREATE_PROFILE:
      DrawCreateProfileScreen(winW, winH);
      break;
    case PRECAREER_LOAD_GAME:
      DrawLoadGameScreen(winW, winH);
      break;
    case PRECAREER_SELECT_LEAGUE:
      DrawSelectLeagueScreen(winW, winH);
      break;
    case PRECAREER_SELECT_CLUB:
      DrawSelectClubScreen(winW, winH);
      break;
    case PRECAREER_SETTINGS_PLACEHOLDER:
      DrawSettingsPlaceholderScreen(winW, winH);
      break;
    default:
      break;
  }

  DrawPreCareerFooter(winW, winH);

  ImGui::End();
  ProcessPreCareerPendingAction();
}
