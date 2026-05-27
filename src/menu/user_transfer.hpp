#pragma once
#include <string>

// ---- Seeding (called once at career start) -----------------------------------
void SeedPlayerPreferences(int managerId);
void SeedSpecialEvents(int managerId, int seasonYear, const std::string &currentDate);

// ---- Daily processing (called from ProcessDailyTransfers) -------------------
void ProcessSpecialEvents(int managerId, int userClubId, const std::string &currentDate, int seasonYear);
void TickUserNegotiations(int managerId, int userClubId, const std::string &currentDate, int seasonYear);
void ProcessAILoanDecision(int managerId, const std::string &currentDate);
void ProcessLoanClauses(int managerId, const std::string &currentDate, int seasonYear);
void ProcessMediaPressure(int managerId, const std::string &currentDate);

// ---- User actions (called from imgui_career.cpp UI) -------------------------
void InitiateUserBid(int managerId, int userClubId, int playerId, int sellingClubId,
                      int offeredFee, int offeredWage, const std::string &promisedRole,
                      int sellOnPct, int loanBackMonths,
                      const std::string &currentDate, int seasonYear);
void RespondToOffer(int managerId, int negotiationId, const std::string &action,
                     int counterFee, int counterWage);
void OfferLoan(int managerId, int userClubId, int playerId, int receivingClubId,
               int loanFee, int wageSplitPct, int recallAfterMonth,
               int optionFee, const std::string &optionDeadline,
               int buyBackFee, const std::string &buyBackExpiry,
               int seasonYear, const std::string &currentDate);

// ---- Side effects (called from within sell-side flow) -----------------------
void ProcessSquadHarmonyOnSale(int managerId, int playerId, int userClubId,
                                const std::string &currentDate);
void ProcessSquadHarmonyOnBuy(int managerId, int playerId,
                               const std::string &currentDate);

// ---- Appearance + promise tracking ------------------------------------------
void TrackPlayerAppearances(int managerId, int fixtureId, int seasonYear);
void EvaluatePromiseFulfillment(int managerId, const std::string &currentDate, int seasonYear);

// ---- Season-end (called from AdvanceDay when all fixtures complete) ----------
void ProcessPoachingEscalation(int managerId, int userClubId,
                                 const std::string &currentDate, int seasonYear);
