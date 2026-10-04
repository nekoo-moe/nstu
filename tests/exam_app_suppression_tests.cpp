#include "nstu/exam_app_suppression.hpp"

#include <cassert>

int main() {
    using nstu::client::ExamAppSuppression;

    // Disarmed: nothing is ever suppressed.
    {
        ExamAppSuppression suppression({"game.exe"});
        assert(!suppression.active());
        assert(!suppression.should_suppress("game.exe"));
        assert(!suppression.should_suppress("notepad.exe"));
    }

    // Armed: non-allowlisted, non-protected apps are suppressed; an empty image
    // name never is; disarming stops suppression at once.
    {
        ExamAppSuppression suppression({});
        suppression.arm();
        assert(suppression.active());
        assert(suppression.should_suppress("game.exe"));
        assert(suppression.should_suppress("Discord.exe"));
        assert(!suppression.should_suppress(""));
        suppression.disarm();
        assert(!suppression.should_suppress("game.exe"));
    }

    // Protected processes are NEVER suppressed, even armed with an empty exam
    // allowlist -- killing any of these would crash the box, lock out the
    // desktop, or take down NSTU / the exam kiosk itself.
    {
        ExamAppSuppression suppression({});
        suppression.arm();
        for (const char* keep :
             {"csrss.exe", "winlogon.exe", "svchost.exe", "lsass.exe",
              "explorer.exe", "dwm.exe", "nstu-agent.exe", "nstu-service.exe",
              "nstu-server.exe", "msedgewebview2.exe"}) {
            assert(ExamAppSuppression::is_protected(keep));
            assert(!suppression.should_suppress(keep));
        }
    }

    // Case-insensitive: protected and allowlisted names match in any case.
    {
        ExamAppSuppression suppression({"Calc.EXE"});
        suppression.arm();
        assert(!suppression.should_suppress("CSRSS.EXE"));  // protected
        assert(!suppression.should_suppress("calc.exe"));   // allowlisted
        assert(suppression.should_suppress("Cheat.exe"));   // neither
    }

    // The exam's own allowlist spares exactly the named app, nothing else.
    {
        ExamAppSuppression suppression({"safeexam.exe"});
        suppression.arm();
        assert(!suppression.should_suppress("safeexam.exe"));
        assert(suppression.should_suppress("chrome.exe"));
    }

    // is_protected is a static floor, independent of arm state and allowlist.
    assert(ExamAppSuppression::is_protected("winlogon.exe"));
    assert(!ExamAppSuppression::is_protected("game.exe"));

    return 0;
}
