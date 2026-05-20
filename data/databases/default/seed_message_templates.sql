-- Message templates for the ClassicManager inbox system.
-- Run once against data/databases/default/database.sqlite:
--   sqlite3 data/databases/default/database.sqlite < data/databases/default/seed_message_templates.sql

CREATE TABLE IF NOT EXISTS message_templates (
  id               INTEGER PRIMARY KEY AUTOINCREMENT,
  category         VARCHAR(32) NOT NULL,
  subcategory      VARCHAR(64),
  sender_type      VARCHAR(32) NOT NULL DEFAULT 'board',
  sender_name      VARCHAR(64) NOT NULL DEFAULT 'The Board',
  subject_template TEXT NOT NULL,
  body_template    TEXT NOT NULL,
  has_task         INTEGER DEFAULT 0
);

DELETE FROM message_templates;

-- Board
INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('board','welcome','board','The Board',
 'Welcome to %ClubName%, %ManagerName%',
 'Dear %ManagerName%,

On behalf of everyone at %ClubName%, we are delighted to welcome you as our new manager.

We have full confidence in your abilities and look forward to an exciting partnership. The squad is ready and the fans are eager to see your vision come to life.

Your first priority will be to review the squad and prepare for the upcoming season.

Welcome aboard.

The Board',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('board','objectives','board','The Board',
 'Season %SeasonYear% - Pre-Season Objectives',
 'Dear %ManagerName%,

With the %SeasonYear% season approaching, the board has outlined the following objectives:

- Achieve a competitive league position
- Show progress in cup competitions
- Develop young talent within the squad
- Maintain financial sustainability

We believe you have the tools to succeed. Your transfer budget has been confirmed separately.

Good luck this season.

The Board',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('board','transfer_budget','board','The Board',
 'Transfer Budget Confirmed - %SeasonYear%',
 'Dear %ManagerName%,

Your transfer budget for the %SeasonYear% season has been finalised. Please use these resources wisely to strengthen the squad while remaining within wage guidelines.

Any transfer activity must align with the long-term strategy and financial health of %ClubName%.

Kind regards,
The Board',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('board','confidence_good','board','The Board',
 'Board Confidence: The Club Is Behind You',
 'Dear %ManagerName%,

We wanted to take a moment to express the board''s confidence in your management. The results have been encouraging and the performances have been positive.

Keep up the good work and we look forward to continued success.

The Board',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('board','confidence_bad','board','The Board',
 'Board Concerns Over Recent Results',
 'Dear %ManagerName%,

The recent run of results has given the board cause for concern. We remain supportive but expect to see an improvement in both performances and results.

We trust you will address the issues with the squad and get the team back on track.

The Board',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('board','dismissal_warning','board','The Board',
 'Your Position Is Under Review',
 'Dear %ManagerName%,

Following recent performances and league position, the board has reviewed the situation and formally notifies you that your position is under serious consideration.

We require an immediate improvement in results. Failure to do so may result in further action.

The Board',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('board','season_review','board','The Board',
 'Season %SeasonYear% - End of Season Review',
 'Dear %ManagerName%,

Now that the %SeasonYear% season has concluded, the board has reviewed the campaign.

We are committed to moving forward constructively and will discuss objectives for next season shortly.

Thank you for your efforts throughout the campaign.

The Board',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('board','contract_renewal','board','The Board',
 'Contract Renewal Offer',
 'Dear %ManagerName%,

The board would like to offer you a contract renewal at %ClubName%. We value your contributions greatly and would like to secure your future with the club.

Please respond at your earliest convenience.

The Board',
 1);

-- Staff
INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('staff','fitness_report','staff','Fitness Coach',
 'Weekly Fitness Report',
 'Manager,

This week''s fitness report is now ready. The squad is generally in good condition heading into the next fixture.

Highlights:
- Overall squad fitness: 85-92%
- No new injury concerns reported
- Two players on individual conditioning programmes

We will continue monitoring workload to prevent overtraining.

Fitness Coach',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('staff','fitness_assessment','staff','Fitness Coach',
 'Pre-Season Fitness Assessment',
 'Manager,

Pre-season fitness testing is complete. The full squad has been assessed and individual programmes have been distributed.

Overall the group is in good shape and ready for the demands of the upcoming season.

Fitness Coach',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('staff','injury_report','staff','Medical Staff',
 'Injury Update - %PlayerName%',
 'Manager,

%PlayerName% sustained an injury in training today. Our medical team has assessed the situation and will provide a full report shortly.

They will be unavailable for selection for the coming matches. Full recovery details will follow.

Medical Staff',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('staff','player_return','staff','Medical Staff',
 '%PlayerName% Returns to Training',
 'Manager,

We are pleased to confirm that %PlayerName% has returned to full training and is available for selection.

The player is in good condition and ready to contribute to the team.

Medical Staff',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('staff','tactical_suggestion','staff','Assistant Manager',
 'Tactical Suggestion for Upcoming Fixture',
 'Manager,

Having reviewed the upcoming opposition, I have a few tactical observations to share.

The opponent has shown vulnerability on the flanks and tends to press high in the first 20 minutes. I recommend we exploit the space behind their defensive line early.

Happy to discuss this further at your convenience.

Your Assistant',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('staff','youth_promotion','staff','Youth Director',
 '%PlayerName% Ready for First Team Consideration',
 'Manager,

%PlayerName% from the youth academy has shown exceptional progress this season. Our coaching staff believes they are ready to be considered for the first team squad.

I would recommend integrating them into first team training to assess their readiness.

Youth Director',
 1);

-- Media
INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('media','press_conference','media','Media Department',
 'Press Conference Scheduled',
 'Manager,

A pre-match press conference has been scheduled ahead of our next fixture. The media are expected to ask about team selection, recent form, and tactical approach.

Please confirm your availability.

Media Department',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('media','post_match_interview','media','Media Department',
 'Post-Match Interview Request',
 'Manager,

Following today''s match, several media outlets have requested an interview. The press are interested in your reaction to the performance and result.

Please attend the post-match media room at your earliest convenience.

Media Department',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('media','manager_of_month','media','Football Association',
 'Manager of the Month - Nomination',
 'Dear %ManagerName%,

We are pleased to inform you that you have been nominated for the Manager of the Month award.

This recognition reflects your team''s excellent performances during the nomination period.

Congratulations and good luck!

The Football Association',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('media','transfer_rumor','media','Press Office',
 'Media Transfer Speculation',
 'Manager,

The media has been reporting transfer speculation linking %ClubName% with several players. We wanted to make you aware so you can prepare for press questions on the matter.

No official approach has been made at this stage.

Press Office',
 0);

-- Competition
INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('competition','season_begins','competition','League Administration',
 'Season %SeasonYear% Officially Begins',
 'Dear %ManagerName%,

We are pleased to confirm that the %SeasonYear% season is now officially underway. Fixtures have been confirmed and the schedule has been distributed to all clubs.

We wish %ClubName% the best of luck this season.

League Administration',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('competition','prize_money','competition','League Administration',
 'Season Prize Money Notification',
 'Dear %ManagerName%,

We are pleased to confirm that your end-of-season prize money has been transferred to %ClubName% based on your final league position.

This award reflects your club''s competitive performance throughout the season.

League Administration',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('competition','cup_draw','competition','Cup Committee',
 'Cup Draw: %ClubName% vs %OpponentName%',
 'Dear %ManagerName%,

The cup draw has been completed. %ClubName% have been drawn against %OpponentName% in the next round.

Fixture details including date, time, and venue will be confirmed shortly.

Cup Committee',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('competition','fixture_rescheduled','competition','League Administration',
 'Fixture Rescheduled',
 'Dear %ManagerName%,

Please be advised that an upcoming fixture has been rescheduled. The new date and time will be communicated as soon as it is confirmed.

We apologise for any inconvenience caused.

League Administration',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('competition','title_race','competition','Sports Media',
 'Title Race Update - Final Stretch',
 'The title race is entering its final and most crucial phase. Several clubs remain in contention and every match now carries enormous significance.

%ClubName% are well positioned but there is still work to be done.

Sports Media',
 0);

-- Transfers
INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('transfers','bid_received','transfers','Transfer Committee',
 'Transfer Offer Received for %PlayerName%',
 'Manager,

We have received a formal transfer offer for %PlayerName% from an external club. The offer is currently under review by the board.

Please advise whether you wish to accept, reject, or negotiate on this offer.

Transfer Committee',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('transfers','bid_accepted','transfers','Transfer Committee',
 'Bid Accepted - %PlayerName% from %ClubName%',
 'Manager,

We are pleased to confirm that our bid for %PlayerName% has been accepted by %ClubName%.

Personal terms will now need to be agreed with the player. Please proceed with contract negotiations at your earliest convenience.

Transfer Committee',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('transfers','bid_rejected','transfers','Transfer Committee',
 'Bid Rejected - %PlayerName%',
 'Manager,

Unfortunately our bid for %PlayerName% has been rejected by the selling club. They have indicated the player is not for sale at this time.

We can either submit an improved offer or look for alternative targets.

Transfer Committee',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('transfers','contract_expiry','transfers','Contract Manager',
 '%PlayerName% Contract Expiry Warning',
 'Manager,

This is a formal notice that %PlayerName%''s contract is due to expire within the next six months.

If you wish to retain the player, contract renewal talks should begin immediately. Failure to act may result in the player leaving on a free transfer.

Contract Manager',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('transfers','free_agent','transfers','Scouting Department',
 'Free Agent Available - %PlayerName%',
 'Manager,

We have identified %PlayerName% as a free agent currently available for signing. This player has recently become available following the expiry of their previous contract.

This could represent excellent value for the club.

Scouting Department',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('transfers','loan_offer','transfers','Transfer Committee',
 'Loan Request Received for %PlayerName%',
 'Manager,

We have received a loan request for %PlayerName% from another club. This player currently has limited first team opportunities.

A loan move could benefit their development. Please advise how you wish to proceed.

Transfer Committee',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('transfers','deadline_day','transfers','Transfer Committee',
 'Transfer Deadline Day - Final Hours',
 'Manager,

The transfer window closes tonight at midnight. If you wish to make any final signings or departures, time is running out.

Please confirm any pending transfer activity immediately to ensure it can be completed in time.

Transfer Committee',
 1);

-- Players
INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('players','wants_contract','players','Player Agent',
 '%PlayerName% Requests Contract Renewal',
 'Manager,

%PlayerName%''s agent has been in touch to request that renewal talks begin. The player is keen to stay at %ClubName% but wants assurances about their future.

We recommend beginning discussions to avoid any unsettlement.

Player Agent',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('players','unhappy_role','players','Player Liaison',
 '%PlayerName% Has Requested a Meeting',
 'Manager,

%PlayerName% has requested a meeting to discuss their role in the squad. They feel they deserve more first team opportunities and are currently unsettled.

It may be worth considering their position in your plans.

Player Liaison',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('players','wants_transfer','players','Player Agent',
 '%PlayerName% Has Requested a Transfer',
 'Manager,

%PlayerName% has formally submitted a transfer request. The player is keen to seek a new challenge and has indicated their intention to leave at the earliest opportunity.

Please discuss how you would like to handle this situation.

Player Agent',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('players','excellent_form','players','Coaching Staff',
 '%PlayerName% - Outstanding Form',
 'Manager,

%PlayerName% has been in exceptional form recently. Their performances have been a major factor in our results and their attitude in training has been exemplary.

The staff and squad are fully behind them.

Coaching Staff',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('players','suspension','players','Disciplinary Committee',
 '%PlayerName% - Suspension Notice',
 'Manager,

Following their recent red card or accumulation of bookings, %PlayerName% is now subject to an automatic suspension.

They will be unavailable for selection for the next match. Please plan your lineup accordingly.

Disciplinary Committee',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('players','injury_recovery','players','Medical Staff',
 '%PlayerName% - Positive Recovery Update',
 'Manager,

We wanted to update you on %PlayerName%''s recovery. Progress has been positive and we are cautiously optimistic about their return to fitness ahead of schedule.

We will continue to monitor closely and update you with any developments.

Medical Staff',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('players','captain_message','players','Team Captain',
 'Message from the Dressing Room',
 'Manager,

I wanted to reach out on behalf of the squad. The players are fully committed to the cause and we have full belief in the direction you are taking this club.

We are ready to fight for every result this season.

The Captain',
 0);

-- Fans
INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('fans','satisfaction_high','fans','Supporters Trust',
 'Supporters Trust - Positive Message',
 'Dear %ManagerName%,

On behalf of the %ClubName% supporters, we wanted to express our appreciation for the performances this season.

The fans are enjoying the football and the atmosphere at home matches has been fantastic. We are fully behind you.

Supporters Trust',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('fans','protest','fans','Supporters Trust',
 'Growing Supporter Concern',
 'Dear %ManagerName%,

We must be candid - a section of the supporter base is growing concerned about recent results and performances.

While we remain supportive of the club, we urge the management to address the current issues and restore confidence among the fanbase.

Supporters Trust',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('fans','season_tickets','fans','Ticketing Office',
 'Season Ticket Renewals Now Open',
 'Dear %ManagerName%,

Season ticket renewals for the %SeasonYear% campaign are now open. We are pleased to report that early renewal figures are strong, reflecting the enthusiasm of our fanbase.

Ticketing Office',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('fans','player_of_year','fans','Supporters Trust',
 'Supporters'' Player of the Year - Voting Open',
 'Dear %ManagerName%,

Voting for the Supporters'' Player of the Year award is now open. Fans have been asked to nominate the player who has impressed them most this season.

Results will be announced at the end of season dinner.

Supporters Trust',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('fans','away_support','fans','Supporters Trust',
 'Outstanding Away Support',
 'Dear %ManagerName%,

We wanted to pass on a message of thanks for your efforts in recent away fixtures. The supporters who made the journey showed tremendous loyalty.

That kind of backing can only help the players perform to their best.

Supporters Trust',
 0);

-- Finance
INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('finance','weekly_summary','finance','Finance Department',
 'Weekly Finance Summary',
 'Manager,

Please find this week''s financial overview:

- Wages processed: on schedule
- TV rights payment: received
- Operating costs: within budget
- Transfer fund balance: updated

Full details are available in the Finance section.

Finance Department',
 0);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('finance','budget_reminder','finance','Finance Department',
 'Mid-Window Budget Reminder',
 'Manager,

We are now halfway through the transfer window and wanted to remind you of your remaining budget.

Please ensure any planned signings are communicated to the finance team to allow sufficient time to process the necessary paperwork.

Finance Department',
 1);

INSERT INTO message_templates (category,subcategory,sender_type,sender_name,subject_template,body_template,has_task) VALUES
('finance','wage_concern','finance','Finance Department',
 'Wage Bill Review - Action Required',
 'Manager,

Following a review of current contracts, we wish to flag that the wage bill is approaching the level agreed with the board.

Please bear this in mind when considering any new signings or contract renewals to ensure we remain within agreed parameters.

Finance Department',
 1);
