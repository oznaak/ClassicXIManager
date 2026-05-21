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
    int GetDatabaseID() const { return databaseID; }
    const std::vector<e_PlayerRole> &GetRoles() const;
    const std::string &GetRoleRaw() const { return roleRaw; }

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
    std::vector<e_PlayerRole> roles;
    std::string roleRaw;

    Properties stats;

    int skinColor;
    std::string hairStyle;
    std::string hairColor;
    float height;
    int   jerseyNumber = 0;

};

#endif
