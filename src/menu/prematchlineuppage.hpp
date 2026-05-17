#pragma once

#include "imgui_career.hpp"
#include "utils/gui2/windowmanager.hpp"

using namespace blunted;

// Shown between PlayFixture() and LoadingMatchPage.
// Queries DB from g_CareerMatchContext, fills g_PreMatchLineup, and renders
// the lineup card via RenderImGuiPreMatchLineup() on the GL thread.
// On continue (click / key / 3-second timeout) it queues LoadingMatchPage
// via RequestManagerMatchStart() and destroys itself.

class PreMatchLineupPage : public Gui2Page {
  public:
    PreMatchLineupPage(Gui2WindowManager *windowManager,
                       const Gui2PageData &pageData);
    virtual ~PreMatchLineupPage();

    virtual void Process();

  private:
    bool m_fallback;

    bool BuildPresentation();

    static std::vector<PreMatchLineupPlayer>
      LoadXI(int teamId, int limit, int offset);
};
