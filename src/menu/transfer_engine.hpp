#pragma once
#include <string>
#include <map>

// ---- Date helpers -----------------------------------------------------------
int         DateToJulian(const std::string &d);
std::string AddDays(const std::string &date, int n);
bool        InTransferWindow(const std::string &date);
int         DaysToWindowEnd(const std::string &date);

// ---- Side-effect helpers ----------------------------------------------------
void InsertTransferNews(int managerId, const std::string &date,
                         const std::string &headline, const std::string &category,
                         int playerId, int fromClubId, int toClubId);
void InsertInboxMessage(int managerId, const std::string &subject,
                        const std::string &body, const std::string &category,
                        const std::string &date);
void InsertFinanceTransaction(int managerId, int clubId, const std::string &date,
                              const std::string &category,
                              const std::string &description, long long amount);
void SetPlayerSaveState(int managerId, int playerId, int teamId,
                        long long weeklyWage, const std::string &contractExpiry = "");
void RemovePlayerScoutingRecords(int managerId, int playerId);
void AddUnhappiness(int managerId, int playerId, const std::string &reason,
                     int severity, const std::string &date);

// ---- Valuation + scoring ----------------------------------------------------
long long CalculateContextualValue(int managerId, int playerId, int sellingClubId,
                                    int buyingClubId, int needScore,
                                    int deadlinePressure, int agentPressure);
int CalculateAcceptanceScore(int managerId, int playerId, int buyingClubId,
                              int sellingClubId, long long offeredWage,
                              const std::string &promisedRole, int agentPressure);

// ---- Utility ----------------------------------------------------------------
std::string RoleToGroup(const std::string &role);

// ---- Main API ---------------------------------------------------------------
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
