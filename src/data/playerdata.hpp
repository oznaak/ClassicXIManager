// written by bastiaan konings schuiling 2008 - 2015
// this work is public domain. the code is undocumented, scruffy, untested, and should generally not be used for anything important.
// i do not offer support, so don't ask. to be used for inspiration :)

#ifndef _HPP_PLAYERDATA
#define _HPP_PLAYERDATA

#include "defines.hpp"

#include "../gamedefines.hpp"
#include "../utils.hpp"

#include "base/properties.hpp"

class PlayerData {

  public:
    PlayerData(int playerDatabaseID);
    PlayerData();
    virtual ~PlayerData();

    std::string GetFirstName() const { return firstName; }
    std::string GetLastName() const { return lastName; }
    std::string GetNickname() const { return nickname; }
    std::string GetDisplayName() const;
    int GetDatabaseID() const { return databaseID; }
    const std::vector<e_PlayerRole> &GetRoles() const;
    const std::string &GetRoleRaw() const { return roleRaw; }
    const std::string &GetPreferredFootRaw() const { return preferredFootRaw; }
    bool IsPreferredFootLeft() const { return preferredFootLeft; }
    bool IsPreferredFootRight() const { return !preferredFootLeft; }
    int GetWeakFootRating() const { return weakFoot; }
    float GetWeakFootRating01() const;
    int GetSkillMovesRating() const { return skillMoves; }
    float GetSkillMovesRating01() const;

    float GetStat(const char *name);

    int GetSkinColor() { return skinColor; }
    std::string GetHairStyle() { return hairStyle; }
    std::string GetHairColor() { return hairColor; }
    float GetHeight() { return height; }
    int   GetJerseyNumber() const { return jerseyNumber; }

  protected:
    int databaseID;
    std::string firstName;
    std::string lastName;
    std::string nickname;
    std::vector<e_PlayerRole> roles;
    std::string roleRaw;
    std::string preferredFootRaw = "right";
    bool preferredFootLeft = false;
    int weakFoot = 3;
    int skillMoves = 2;

    Properties stats;

    int skinColor;
    std::string hairStyle;
    std::string hairColor;
    float height;
    int   jerseyNumber = 0;

};

#endif
