#pragma once
// The frame rate the player chose: kept in config.ini and changed on the options page.
#include <wots4/mod_api.h>
#include <string>

namespace fps_uncap {

// Whole multiples of the game's 30, so every count of updates scales to a whole count. Higher
// rates would overflow the byte counters of some systems (torture, effect fades).
constexpr unsigned selectableUpdatesPerSecond[] = {30,60,90,120};
constexpr unsigned selectableRateCount = sizeof(selectableUpdatesPerSecond)/sizeof(selectableUpdatesPerSecond[0]);
constexpr unsigned highestUpdatesPerSecond = 120;
constexpr unsigned defaultRateChoice = 1;  // 60

using LogFunction = void (*)(const char*);

class Settings {
public:
    void load(const wchar_t* directory, LogFunction logger);
    bool registerPage(const wots4::ModContext& context);
    // Writes config.ini when the player changed the rate since the last save.
    void save();
    unsigned updatesPerSecond() const { return selectableUpdatesPerSecond[rateChoice]; }
    // TRUE once after the player chose another rate on the options page.
    bool takeRateChange();
private:
    static void WINAPI readPage(void* context, uint32_t* values, uint32_t count);
    static void WINAPI changePage(void* context, uint32_t item, uint32_t value);
    uint32_t rateChoice = defaultRateChoice;
    bool rateChanged = false;
    bool dirty = false;
    std::wstring path;
    LogFunction log = nullptr;
};

}  // namespace fps_uncap
