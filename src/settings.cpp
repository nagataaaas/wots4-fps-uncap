#include "settings.h"
#include <cstdio>
#include <cwchar>

namespace fps_uncap {
namespace {

const wots4::LocalizedText rateChoices[] = {{L"30 FPS",L"30 FPS"},{L"60 FPS",L"60 FPS"},{L"90 FPS",L"90 FPS"},
    {L"120 FPS",L"120 FPS"}};
static_assert(sizeof(rateChoices)/sizeof(rateChoices[0])==selectableRateCount);
constexpr uint32_t rateItem = 0;
const wots4::OptionItem rateOption{wots4::optionChoice,{L"フレームレート",L"Frame Rate"},rateChoices,
    selectableRateCount,defaultRateChoice};
// Shown under the rate for the rates above 60, which fewer PCs keep.
const wots4::LocalizedText slowdownNotice{L"この FPS を保てない PC では、ゲームの進みが遅くなります",
    L"On a PC that cannot keep this frame rate, the game runs slower"};
constexpr uint32_t pageItemCount = 2;

// config.ini [frame_rate] fps holds the number itself, one of selectableUpdatesPerSecond.
constexpr wchar_t configSection[] = L"frame_rate";
constexpr wchar_t configKey[] = L"fps";
// Longer than any accepted value, so a truncated read is never mistaken for one.
constexpr DWORD valueBufferLength = 16;

bool findRateChoice(const wchar_t* text, uint32_t& choice) {
    wchar_t* end = nullptr;
    const unsigned long number = wcstoul(text,&end,10);
    if(end==text || *end)
        return false;
    for(uint32_t candidate=0;candidate<selectableRateCount;++candidate) {
        if(selectableUpdatesPerSecond[candidate]!=number)
            continue;
        choice=candidate;
        return true;
    }
    return false;
}

}  // namespace

void Settings::load(const wchar_t* directory, LogFunction logger) {
    path=std::wstring(directory)+L"config.ini";
    log=logger;
    rateChoice=defaultRateChoice;
    wchar_t text[valueBufferLength];
    const DWORD length=GetPrivateProfileStringW(configSection,configKey,L"",text,valueBufferLength,path.c_str());
    if(!length)
        return;
    const bool truncated=length>=valueBufferLength-1;
    if(!truncated && findRateChoice(text,rateChoice))
        return;
    if(!log)
        return;
    char message[256];
    sprintf_s(message,"config.ini [%ls] %ls=%ls%ls is not one of 30, 60, 90, 120; using the default %u",
        configSection,configKey,text,truncated ? L"..." : L"",updatesPerSecond());
    log(message);
}

bool Settings::takeRateChange() {
    if(!rateChanged)
        return false;
    rateChanged=false;
    return true;
}

void WINAPI Settings::readPage(void* context, uint32_t* values, uint32_t count) {
    if(!values || !count)
        return;
    values[rateItem]=static_cast<Settings*>(context)->rateChoice;
}

void WINAPI Settings::changePage(void* context, uint32_t item, uint32_t value) {
    auto& settings=*static_cast<Settings*>(context);
    if(item!=rateItem || value>=selectableRateCount || value==settings.rateChoice)
        return;
    settings.rateChoice=value;
    settings.rateChanged=true;
    settings.dirty=true;
}

bool Settings::registerPage(const wots4::ModContext& context) {
    const wots4::LocalizedText title{L"フレームレート",L"Frame Rate"};
    const wots4::OptionsPageCallbacks callbacks{this,readPage,changePage};
    const wots4::OptionItem page[pageItemCount]={rateOption,
        wots4::optionNoticeItem(slowdownNotice,rateItem,{2,3})};
    if(context.host->addOptionsPage(context.module,&title,page,pageItemCount,&callbacks))
        return true;
    if(log)
        log("the ModLoader did not accept the options page with the slowdown notice; adding it without the "
            "notice (a ModLoader without notices refuses them)");
    return context.host->addOptionsPage(context.module,&title,&rateOption,1,&callbacks)!=FALSE;
}

void Settings::save() {
    if(!dirty)
        return;
    wchar_t text[valueBufferLength];
    swprintf_s(text,L"%u",updatesPerSecond());
    // Updates the key in place, keeping the comments of the bundled config.ini.
    if(!WritePrivateProfileStringW(configSection,configKey,text,path.c_str())) {
        if(log) {
            char message[256];
            sprintf_s(message,"could not save config.ini [%ls] %ls (Windows error %lu); the rate stays in use "
                "and is saved again with the next options save",configSection,configKey,GetLastError());
            log(message);
        }
        return;
    }
    // A cache-flush call returns zero even on success; only the write above decides a retry.
    WritePrivateProfileStringW(nullptr,nullptr,nullptr,path.c_str());
    dirty=false;
}

}  // namespace fps_uncap
