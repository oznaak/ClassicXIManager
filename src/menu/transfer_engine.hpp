#pragma once
#include <string>
#include <map>

// Called once at career start (inside GenerateCareerSeason).
void SeedTransferSystem(int managerId);

// Called every day advance (inside AdvanceDay), before LoadFromDB.
void ProcessDailyTransfers(int managerId, int userClubId,
                            const std::string &currentDate, int seasonYear);

// Called on the 1st of each month (inside ProcessDailyTransfers).
void ProcessContractRenewals(int managerId, const std::string &currentDate);

// Called after every completed or collapsed deal.
void TriggerTransferCascade(int managerId, int playerTeamId,
                             int affectedClubId, const std::string &currentDate,
                             int seasonYear, int depth);

// Returns need scores per position group for a club (0-100).
// Groups: "GK","CB","FB_WB","DM","CM","AM_W","ST"
std::map<std::string, int> EvaluateSquadNeeds(int managerId, int clubId,
                                               int seasonYear);
