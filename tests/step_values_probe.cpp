// Checks, with the game's per-update constants as plain numbers, the values used at 30 and 60
// updates per second. Needs neither the exe nor the game.
#include <cmath>
#include <cstdio>
#include "step_values.h"

using fps_uncap::PerUpdate;

static unsigned tests, failures;
static void check(const char* label, bool passed) {
    ++tests; if(!passed) ++failures;
    printf("%s %s\n",passed?"PASS":"FAIL",label);
}

static bool near(double a, double b) { return std::fabs(a-b)<=1e-12*std::fabs(b); }

int main() {
    auto at30=fps_uncap::stepFor(30);
    check("30 updates: the step has the same bits as the game's 1/30 (0x3fa1111120000000)",
        at30.seconds==0.03333333507180214);
    check("30 updates: the float step is the game's 1/30 (0x3d088889)",at30.secondsFloat==1.0f/30.0f);
    auto at60=fps_uncap::stepFor(60);
    check("60 updates: the step is float 1/60 widened",at60.seconds==static_cast<double>(1.0f/60.0f));

    const double rensatsuDrain=static_cast<double>(2.3f), spinDecay=static_cast<double>(0.96825f);
    check("30 updates: every kind keeps the game's own value",
        fps_uncap::perUpdateValue(PerUpdate::amount,rensatsuDrain,30)==rensatsuDrain &&
        fps_uncap::perUpdateValue(PerUpdate::decay,spinDecay,30)==spinDecay &&
        fps_uncap::perUpdateValue(PerUpdate::keep,at30.seconds,30)==at30.seconds);
    check("60 updates: amounts per update are halved (UI frame 1.0 -> 0.5, motion window 0.5 -> 0.25)",
        fps_uncap::perUpdateValue(PerUpdate::amount,1.0,60)==0.5 &&
        fps_uncap::perUpdateValue(PerUpdate::amount,0.5,60)==0.25);
    check("60 updates: one second drains as much Rensatsu gauge as at 30",
        near(fps_uncap::perUpdateValue(PerUpdate::amount,rensatsuDrain,60)*60,rensatsuDrain*30));
    auto decay60=fps_uncap::perUpdateValue(PerUpdate::decay,spinDecay,60);
    check("60 updates: two decays give one decay at 30",near(decay60*decay60,spinDecay));
    check("60 updates: frame-count conversions keep 1/30",
        fps_uncap::perUpdateValue(PerUpdate::keep,at30.seconds,60)==at30.seconds);

    check("update counts: 3500 at 30 is 7000 at 60, and stays 3500 at 30",
        fps_uncap::scaledUpdateCount(3500,60)==7000&&fps_uncap::scaledUpdateCount(3500,30)==3500);
    check("update counts: key repeat 11 and 3 become 22 and 6 at 60",
        fps_uncap::scaledUpdateCount(11,60)==22&&fps_uncap::scaledUpdateCount(3,60)==6);
    check("update counts round to the nearest update (700 at 45 is 1050, 1 at 45 is 2)",
        fps_uncap::scaledUpdateCount(700,45)==1050&&fps_uncap::scaledUpdateCount(1,45)==2);

    check("a running script keeps 30 updates",fps_uncap::updatesPerSecondFor(true,60)==30);
    check("outside scripts the high rate is used",fps_uncap::updatesPerSecondFor(false,60)==60);

    printf("RESULT tests=%u failures=%u\n",tests,failures);
    return failures?1:0;
}
