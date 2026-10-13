#pragma once
// The values the game reads every logic update, for a given number of updates per second.
// The game was written for 30 updates per second; at 60, every update covers half the time.
#include <cmath>

namespace fps_uncap {

constexpr unsigned gameUpdatesPerSecond = 30;  // What the game's constants assume

// The logic step as the game stores it: float 1/N, also widened to double.
struct Step {
    unsigned updatesPerSecond;
    float secondsFloat;
    double seconds;
};

inline Step stepFor(unsigned updatesPerSecond) {
    const float seconds = 1.0f/static_cast<float>(updatesPerSecond);
    return {updatesPerSecond,seconds,seconds};
}

inline double gameUpdatesPerUpdate(unsigned updatesPerSecond) {
    return static_cast<double>(gameUpdatesPerSecond)/updatesPerSecond;
}

// How a value the game applies once per update must change so one second does the same.
enum class PerUpdate {
    keep,              // Not per update: frame counts of the game's 30 fps data, kept as they are
    amount,            // Added or subtracted each update: shrinks in proportion
    decay,             // Kept share k of x = k*x + ...: taken to the matching power
    weight,            // Moved share w of x = (1-w)*x + w*target: 1-(1-w)^r
    gain,              // Multiplies what moved during one update (mouse, prediction): grows in proportion
    amountUnderDecay,  // Added each update to x = k*x + c: keeps the limit c/(1-k) and the rate
};

// decayOfAmount is the k of amountUnderDecay and is not used by the other kinds.
inline double perUpdateValue(PerUpdate kind, double valueAtGameRate, unsigned updatesPerSecond,
    double decayOfAmount = 0) {
    const double share = gameUpdatesPerUpdate(updatesPerSecond);
    switch(kind) {
    case PerUpdate::amount:
        return valueAtGameRate*share;
    case PerUpdate::decay:
        return std::pow(valueAtGameRate,share);
    case PerUpdate::weight:
        return 1-std::pow(1-valueAtGameRate,share);
    case PerUpdate::gain:
        return valueAtGameRate/share;
    case PerUpdate::amountUnderDecay:
        return valueAtGameRate*(1-std::pow(decayOfAmount,share))/(1-decayOfAmount);
    default:
        return valueAtGameRate;
    }
}

// A count of updates written for 30 per second, for the same time at updatesPerSecond (rounded).
inline unsigned scaledUpdateCount(unsigned countAtGameRate, unsigned updatesPerSecond) {
    return (countAtGameRate*updatesPerSecond+gameUpdatesPerSecond/2)/gameUpdatesPerSecond;
}

// Many waits store a count n of 30 fps frames and hold for n + 1 updates (they end when a
// counter passes n). The holds are what must last as long: the stored count for updatesPerSecond.
// Negative counts mean "no wait" or "forever" and are kept.
inline int scaledHoldCount(int framesAtGameRate, unsigned updatesPerSecond) {
    if(framesAtGameRate<0)
        return framesAtGameRate;
    return static_cast<int>(scaledUpdateCount(static_cast<unsigned>(framesAtGameRate)+1,updatesPerSecond))-1;
}

}  // namespace fps_uncap
