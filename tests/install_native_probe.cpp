// Installs mod.dll on a copy of the exe mapped into this process and checks every value it
// writes, then calls the hooked functions through their entry points. The rest of each hooked
// function needs the running game, so after installation the code right after the part MinHook
// moved is replaced with a short return that records what the hook passed on. Then chooses 120,
// 90 and 30 on the options page as the player would and checks the values again. Never attaches
// to the game process or reads saves; config.ini is written in a temporary folder only.
#include <cstdlib>
#include "mapped_exe.h"

using namespace mapped_exe;

constexpr unsigned stepSeconds = 0x99dca8;
constexpr unsigned managerStepSeconds = 0x97cdc4;
constexpr unsigned layoutStepSeconds = 0x9ddd64;
constexpr unsigned redirectCount = 203;
constexpr unsigned immediateCount = 88;
// Replaced functions: frame limiter, action script timer, scene clock, script Wait, hang seconds,
// press window. Jumps: command wait, balloon life, action end time-out, mill step, kick threshold,
// sound fade, blood drops, placed effects (2), vitality charm, item motion, Rensatsu start wait,
// hang time-out sound. Branches: the hang cycle (2).
constexpr unsigned hookClaims = 6+13+2;
constexpr unsigned stepClaims = 3;
// The choices of the options page
constexpr uint32_t choice30 = 0, choice90 = 2, choice120 = 3;

static unsigned char* uiManager;     // [0x9ebb8c]
static unsigned char* saveDataManager;  // [0x9e96d4]
static unsigned char saveList[0x50];    // [manager+8], key repeats at +0x10/+0x20/+0x30/+0x40
static unsigned char keyRepeat[0x14];
static unsigned char* cameraManager; // [0x9e964c]
static unsigned char* followCamera;  // [mgr+8]
static unsigned char* battleCamera;  // [mgr+0xc]
static unsigned char* gameOptions;   // [0x9e967c], read by the cameras' vt+0x1c

static double gameDouble(unsigned address) { return *(double*)va(address); }
static float gameFloat(unsigned address) { return *(float*)va(address); }
static unsigned operandOf(unsigned instruction) { return *(unsigned*)va(instruction+2); }
static double valueAtOperand(unsigned instruction) { return *(double*)operandOf(instruction); }
static float floatAtOperand(unsigned instruction) { return *(float*)operandOf(instruction); }
static unsigned immediateAt(unsigned address) { return *(unsigned*)va(address); }
static unsigned char byteAt(unsigned address) { return *(unsigned char*)va(address); }
static float cameraField(unsigned char* camera, unsigned offset) { return *(float*)(camera+offset); }
static bool closeTo(double a, double b) { return fabs(a-b)<=1e-6*fabs(b); }
static unsigned short repeatDelay() { return *(unsigned short*)(keyRepeat+0xc); }
static unsigned short repeatInterval() { return *(unsigned short*)(keyRepeat+0xe); }

static bool modOwned(unsigned address, HMODULE mod) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery((void*)address,&info,sizeof(info)) && info.AllocationBase==mod;
}

static void writeBytes(unsigned address, const unsigned char* bytes, size_t length) {
    memcpy((void*)va(address),bytes,length);
}

// An action script Timer interrupt (0x4185c0) waiting waitFrames 30 fps frames: the number of
// updates it answers "keep running" (-1) before it jumps to its target.
static unsigned updatesBeforeTimerJumps(int waitFrames) {
    struct Timer { int unused[2]; int jumpTarget; int unused2; int waitFrames; int elapsed; } timer{};
    timer.jumpTarget=0x1234;
    timer.waitFrames=waitFrames;
    auto check=(int (__thiscall*)(Timer*, unsigned, unsigned))va(0x4185c0);
    unsigned updates=0;
    while(check(&timer,0,0)==-1 && updates<1000)
        ++updates;
    return check(&timer,0,0)==0x1234 ? updates : 9999;
}

// The event scene clock (0x58ded0) as PlayScene calls it, with 1.0 frame: what the game's
// function receives in [ebp+8]. After the moved prologue (push ebp; mov ebp,esp; fld [ebp+8])
// the copy stores the loaded value and returns.
static float sceneStepReceived;
static float advanceSceneByOneFrame() {
    unsigned char storeAndReturn[]={0xd9,0x1d,0,0,0,0,0x5d,0xc3};  // fstp [abs]; pop ebp; ret
    *(float**)(storeAndReturn+2)=&sceneStepReceived;
    writeBytes(0x58ded6,storeAndReturn,sizeof(storeAndReturn));
    auto advance=va(0x58ded0);
    float oneFrame=1.0f;
    __asm {
        push oneFrame
        call advance
        add esp, 4
    }
    return sceneStepReceived;
}

// The script Wait start (0x590c20) leaving framesLeft in [+0xc], as the game's own start does:
// the count the Wait holds the script for afterwards. After the moved prologue (push ebp;
// mov ebp,esp; push ecx; push ebx; push esi) the copy returns TRUE at once.
static int scriptWaitFramesAfterStart(int framesLeft) {
    const unsigned char returnTrue[]={0x5e,0x5b,0x8b,0xe5,0x5d,0xb0,0x01,0xc2,0x08,0x00};
    writeBytes(0x590c26,returnTrue,sizeof(returnTrue));
    struct Wait { int unused[3]; int frames; } wait{};
    wait.frames=framesLeft;
    auto start=(unsigned char (__thiscall*)(Wait*, unsigned, unsigned))va(0x590c20);
    start(&wait,0,0);
    return wait.frames;
}

// One sound fade step (0x5a06c3) on a sound with ebx = sound. The game continues at 0x5a06d5
// (fstp [ebx+0x6c]); the copy returns right after it.
struct Sound { unsigned char unused[0x3c]; float frames; float volumeStep; unsigned char unused2[0x28]; float volume; };
static void stepSoundFade(Sound* sound) {
    const unsigned char returnAfterStore[]={0xc3};
    writeBytes(0x5a06d8,returnAfterStore,sizeof(returnAfterStore));
    auto step=va(0x5a06c3);
    __asm {
        push ebx
        mov ebx, sound
        call step
        pop ebx
    }
}

// The action script command wait (0x40b8ff) with the handler's result in eax, on a script with
// esi = script: what the script then stays for in [+0x214]. The copy returns after the store.
static int commandWaitAfterStore(int framesReturned) {
    const unsigned char returnHere[]={0xc3};
    writeBytes(0x40b905,returnHere,sizeof(returnHere));
    static unsigned char script[0x220];
    auto site=va(0x40b8ff);
    auto scriptAddress=script;
    __asm {
        push esi
        mov esi, scriptAddress
        mov eax, framesReturned
        call site
        pop esi
    }
    return *(int*)(script+0x214);
}

// A balloon opening (0x5b088d) with edi = balloon, eax = its life and ecx = something the game
// reads after: the stored life, and whether ecx came back.
static unsigned ecxAfterBalloon;
static int balloonLifeAfterStore(int life) {
    unsigned char storeEcxAndReturn[]={0x89,0x0d,0,0,0,0,0xc3};  // mov [abs],ecx; ret
    *(unsigned**)(storeEcxAndReturn+2)=&ecxAfterBalloon;
    writeBytes(0x5b0893,storeEcxAndReturn,sizeof(storeEcxAndReturn));
    static unsigned char balloon[0x240];
    auto site=va(0x5b088d);
    auto balloonAddress=balloon;
    __asm {
        push edi
        mov edi, balloonAddress
        mov eax, life
        mov ecx, 0x12345678
        call site
        pop edi
    }
    return *(int*)(balloon+0x234);
}

// Waiting for an action to end (0x41974d) after waitedUpdates: 2 keeps waiting (0x41975d),
// 1 gives up (0x419753).
static int actionEndCheck(int waitedUpdates) {
    const unsigned char returnOne[]={0xb8,1,0,0,0,0xc3};
    const unsigned char returnTwo[]={0xb8,2,0,0,0,0xc3};
    writeBytes(0x419753,returnOne,sizeof(returnOne));
    writeBytes(0x41975d,returnTwo,sizeof(returnTwo));
    static unsigned char action[0x30];
    *(int*)(action+0x24)=waitedUpdates;
    auto site=va(0x41974d);
    auto actionAddress=action;
    int result;
    __asm {
        push edi
        mov edi, actionAddress
        call site
        mov result, eax
        pop edi
    }
    return result;
}

// One water mill wheel step (0x5ab012) with the speed on the x87 stack: the animation time the
// game computes next (time - speed), and the speed it stored.
static float millTimeAfterStep;
static float millStep(float time, float speed, float* storedSpeed) {
    unsigned char subtractAndStore[]={0xd8,0xe1,0xd9,0x1d,0,0,0,0,0xdd,0xd8,0xc3};  // fsub st,st(1); fstp [abs]; fstp st0; ret
    *(float**)(subtractAndStore+4)=&millTimeAfterStep;
    writeBytes(0x5ab01e,subtractAndStore,sizeof(subtractAndStore));
    static unsigned char mill[0x100];
    *(float*)(mill+0x84)=time;
    auto site=va(0x5ab012);
    auto millAddress=mill;
    __asm {
        push edi
        mov edi, millAddress
        fld speed
        call site
        pop edi
    }
    *storedSpeed=*(float*)(mill+0x90);
    return millTimeAfterStep;
}

// The strong kick threshold (0x51e664): the value the game compares the move of one update with.
static float kickThreshold, kickMoved;
static unsigned kickSite;
static unsigned char* kickFrameBase;

// Calls the site with ebp = kickFrameBase, the frame the game's code reads [ebp-0x1c] from.
static __declspec(naked) void callKickSite() {
    __asm {
        push ebp
        mov ebp, kickFrameBase
        call dword ptr [kickSite]
        pop ebp
        ret
    }
}

static float kickThresholdUsed(float moved) {
    unsigned char storeBoth[]={0xd9,0x1d,0,0,0,0,0xd9,0x1d,0,0,0,0,0xc3};  // fstp [moved]; fstp [threshold]; ret
    *(float**)(storeBoth+2)=&kickMoved;
    *(float**)(storeBoth+8)=&kickThreshold;
    writeBytes(0x51e669,storeBoth,sizeof(storeBoth));
    static float frame[8];
    frame[0]=moved;
    kickFrameBase=(unsigned char*)frame+0x1c;
    kickSite=va(0x51e664);
    callKickSite();
    return kickMoved==moved ? kickThreshold : -1.0f;
}

// One item motion step (0x520ab6) with esi = item, edi = velocity: the position afterwards.
struct Vector { float x, y, z; };
static Vector itemPositionAfterStep(Vector position, Vector velocity) {
    const unsigned char returnHere[]={0xc3};
    writeBytes(0x520ad0,returnHere,sizeof(returnHere));
    static unsigned char item[0x40];
    static Vector itemVelocity;
    *(Vector*)(item+8)=position;
    itemVelocity=velocity;
    auto site=va(0x520ab6);
    auto itemAddress=item;
    auto velocityAddress=&itemVelocity;
    __asm {
        push esi
        push edi
        mov esi, itemAddress
        mov edi, velocityAddress
        call site
        pop edi
        pop esi
    }
    return *(Vector*)(item+8);
}

// The vitality charm (0x484c2e) adding level [ebp-8] to vitality [ebp-4]: the vitality afterwards.
static unsigned charmSite;
static unsigned char* charmFrameBase;
static __declspec(naked) void callCharmSite() {
    __asm {
        push ebp
        mov ebp, charmFrameBase
        call dword ptr [charmSite]
        pop ebp
        ret
    }
}
static float vitalityAfterCharm(int level, float vitality) {
    const unsigned char returnHere[]={0xc3};
    writeBytes(0x484c37,returnHere,sizeof(returnHere));
    static unsigned char frame[0x10];
    *(int*)(frame+8)=level;      // [ebp-8] with ebp = frame+0x10
    *(float*)(frame+0xc)=vitality;  // [ebp-4]
    charmFrameBase=frame+0x10;
    charmSite=va(0x484c2e);
    callCharmSite();
    return *(float*)(frame+0xc);
}

// A placed effect starting (0x4b98bc) with edi = effect, interval [+0x20] and a random part in
// edx: the count stored in [+0x28], and edx afterwards.
static unsigned edxAfterPlacedEffect;
static int placedEffectRepeat(int interval, int randomPart) {
    unsigned char storeEdxAndReturn[]={0x89,0x15,0,0,0,0,0xc3};  // mov [abs],edx; ret
    *(unsigned**)(storeEdxAndReturn+2)=&edxAfterPlacedEffect;
    writeBytes(0x4b98c4,storeEdxAndReturn,sizeof(storeEdxAndReturn));
    static unsigned char effect[0x30];
    *(int*)(effect+0x20)=interval;
    auto site=va(0x4b98bc);
    auto effectAddress=effect;
    __asm {
        push edi
        mov edi, effectAddress
        mov edx, randomPart
        call site
        pop edi
    }
    return *(int*)(effect+0x28);
}

// The Rensatsu start with no script system (0x43cdce) after waitedUpdates in [ebx+0x24]: 2 keeps
// waiting (0x43cdf7), 1 goes on (0x43cdd4).
static int rensatsuStartCheck(int waitedUpdates) {
    const unsigned char returnOne[]={0xb8,1,0,0,0,0xc3};
    const unsigned char returnTwo[]={0xb8,2,0,0,0,0xc3};
    writeBytes(0x43cdd4,returnOne,sizeof(returnOne));
    writeBytes(0x43cdf7,returnTwo,sizeof(returnTwo));
    static unsigned char rensatsu[0x30];
    *(int*)(rensatsu+0x24)=waitedUpdates;
    auto site=va(0x43cdce);
    auto rensatsuAddress=rensatsu;
    int result;
    __asm {
        push ebx
        mov ebx, rensatsuAddress
        call site
        mov result, eax
        pop ebx
    }
    return result;
}

// The hang time-out sound (0x5a7e8f) with the word [esi+0xb6] = updates: 1 plays it (0x5a7e99),
// 2 does not (0x5a7eb2).
static int hangTimeoutSoundCheck(short updates) {
    const unsigned char returnOne[]={0xb8,1,0,0,0,0xc3};
    const unsigned char returnTwo[]={0xb8,2,0,0,0,0xc3};
    writeBytes(0x5a7e99,returnOne,sizeof(returnOne));
    writeBytes(0x5a7eb2,returnTwo,sizeof(returnTwo));
    static unsigned char hang[0xc0];
    *(short*)(hang+0xb6)=updates;
    auto site=va(0x5a7e8f);
    auto hangAddress=hang;
    int result;
    __asm {
        push esi
        mov esi, hangAddress
        call site
        mov result, eax
        pop esi
    }
    return result;
}

// The frame limiter (0x686bd0) as the main loop calls it: esi = timer, a force byte pushed.
struct FrameTimer { float nextUpdate, realigned, gameSeconds; unsigned char gameTimeRuns; };
// The limiter writes the timer inside inline assembly, which the optimizer does not see, so the
// clock is read through volatile right where it is needed.
static float gameClock(const FrameTimer& timer) { return *(const volatile float*)&timer.gameSeconds; }
// The clock before a limiter call, kept in memory for the same reason.
static volatile float secondsBeforeUpdate;
static bool limiterRunsUpdate(FrameTimer* timer, bool force) {
    auto limiter=va(0x686bd0);
    int forceArgument=force;
    unsigned char due;
    __asm {
        push esi
        mov esi, timer
        push forceArgument
        call limiter
        mov due, al
        pop esi
    }
    return due!=0;
}

static double secondsSince(const LARGE_INTEGER& start) {
    LARGE_INTEGER now, frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return double(now.QuadPart-start.QuadPart)/double(frequency.QuadPart);
}

// Calls the limiter until it runs an update, like the main loop: the seconds that took.
static double secondsUntilNextUpdate(FrameTimer* timer) {
    LARGE_INTEGER start;
    QueryPerformanceCounter(&start);
    while(!limiterRunsUpdate(timer,false) && secondsSince(start)<1.0) {}
    return secondsSince(start);
}

// The player picks a rate on the options page, then the game finishes its update.
static void chooseOnOptionsPage(uint32_t choice) {
    pageCallbacks.changed(pageCallbacks.context,0,choice);
    frameCallback(nullptr);
}

static std::wstring configPath;
static std::wstring savedFps() {
    wchar_t text[16];
    GetPrivateProfileStringW(L"frame_rate",L"fps",L"",text,16,configPath.c_str());
    return text;
}

// The torture press window (0x5a4cc0) on a torture with [+0x50] = updates of the swing.
static int swinging;
static int __fastcall fakeIsSwinging(void*) { return swinging; }
static bool pressInWindow(int updates, bool isSwinging, int animation) {
    static void* vtable[8];
    vtable[7]=(void*)&fakeIsSwinging;
    static unsigned char torture[0x60];
    *(void***)torture=vtable;
    *(int*)(torture+0x50)=updates;
    swinging=isSwinging;
    auto check=(unsigned char (__thiscall*)(void*, int))va(0x5a4cc0);
    return check(torture,animation)!=0;
}

int main(int argc, char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc!=3) return 2;
    if(!mapImage(argv[1])) return 3;
    // The real .rdata is read-only; map it so here too, so the Mod has to ask for writing.
    DWORD previous;
    VirtualProtect((void*)va(0x905000),0xb55f7,PAGE_READONLY,&previous);
    const double originalStep=gameDouble(stepSeconds);

    // The CUIManager with its key repeat state as the constructor leaves it (0x67ead5), and the
    // follow and battle cameras with options of camera speed 5, as when the Mod starts late.
    uiManager=(unsigned char*)calloc(1,0x200);
    *(unsigned*)(keyRepeat+0xc)=0x3000b;
    *(unsigned char**)(uiManager+0xc8)=keyRepeat;
    *(void**)va(0x9ebb8c)=uiManager;
    saveDataManager=(unsigned char*)calloc(1,0x20);
    for(unsigned repeat=0x10;repeat<=0x40;repeat+=0x10)
        *(unsigned*)(saveList+repeat+4)=repeat<0x30 ? 0x10005 : 0x20005;
    *(unsigned char**)(saveDataManager+8)=saveList;
    *(void**)va(0x9e96d4)=saveDataManager;
    cameraManager=(unsigned char*)calloc(1,0x40);
    followCamera=(unsigned char*)calloc(1,0x200);
    battleCamera=(unsigned char*)calloc(1,0x200);
    gameOptions=(unsigned char*)calloc(1,0x40);
    *(unsigned*)followCamera=va(0x980784);
    *(unsigned*)battleCamera=va(0x9806ec);
    *(unsigned char**)(cameraManager+8)=followCamera;
    *(unsigned char**)(cameraManager+0xc)=battleCamera;
    gameOptions[0]=1; gameOptions[1]=1; gameOptions[2]=5; gameOptions[3]=5;
    *(void**)va(0x9e964c)=cameraManager;
    *(void**)va(0x9e967c)=gameOptions;

    // A config.ini with a rate that cannot be chosen, in a folder of its own
    wchar_t temporary[MAX_PATH];
    GetTempPathW(MAX_PATH,temporary);
    std::wstring directory=std::wstring(temporary)+L"fps_uncap_probe_"+std::to_wstring(GetCurrentProcessId())+L"\\";
    CreateDirectoryW(directory.c_str(),nullptr);
    configPath=directory+L"config.ini";
    WritePrivateProfileStringW(L"frame_rate",L"fps",L"75",configPath.c_str());

    auto mod=LoadLibraryA(argv[2]);
    auto initialize=mod?(wots4::Initialize)GetProcAddress(mod,"Wots4ModInitialize"):nullptr;
    wots4::HostApi host{sizeof(host),wots4::modApiVersion,hostLog,hostClaim,hostRelease,hostAddOptionsPage,
        hostOnFrame,hostOnOptionsSaved};
    wots4::ModContext context{sizeof(context),wots4::modApiVersion,nullptr,mod,(HMODULE)imageBase,
        directory.c_str(),&host};
    check("mod.dll exports Wots4ModInitialize",initialize!=nullptr);
    if(!initialize) { printf("RESULT tests=%u failures=%u\n",tests,failures+1); return 1; }

    // A changed instruction: nothing is installed or claimed.
    auto changed=(unsigned char*)va(0x44f4b5);
    changed[1]=0x05;
    check("with a time read changed, the Mod refuses",initialize(&context)==FALSE);
    check("nothing is claimed, redirected or written",claims.empty()&&operandOf(0x44f4b5)==va(stepSeconds)&&
        gameDouble(stepSeconds)==originalStep&&*(unsigned char*)va(0x58ded0)==0x55&&
        *(unsigned char*)va(0x5a06c3)==0xd9);
    check("the refusal is logged with the reason",loggedText("is not the known version"));
    changed[1]=0x0d;

    check("with the known code, the Mod initializes",initialize(&context)!=FALSE);
    check("config.ini fps=75 is refused with a warning and the default 60 is used",
        loggedText("[frame_rate] fps=75 is not one of 30, 60, 90, 120; using the default 60"));
    check("the installation is logged",loggedText("installed: 60 updates per second, scripts included"));
    check("the replaced functions are claimed (6, 6, 6, 6, 6 and 5 bytes)",
        claimed(0x686bd0,6)&&claimed(0x4185c0,6)&&claimed(0x58ded0,6)&&claimed(0x590c20,6)&&claimed(0x5a6fc0,6)&&
        claimed(0x5a4cc0,5));
    check("the code jumps are claimed (6, 6, 6, 12, 5, 18, 6 and 10 bytes) and the 2 hang cycle branches",
        claimed(0x40b8ff,6)&&claimed(0x5b088d,6)&&claimed(0x41974d,6)&&claimed(0x5ab012,12)&&
        claimed(0x51e664,5)&&claimed(0x5a06c3,18)&&claimed(0x43cdce,6)&&claimed(0x5a7e8f,10)&&
        claimed(0x5a892c,1)&&claimed(0x5a893c,1));
    check("the three step constants are claimed",
        claimed(stepSeconds,8)&&claimed(managerStepSeconds,4)&&claimed(layoutStepSeconds,4));
    check("203 operands (4 bytes) and 88 immediates (4 or 1 bytes) are claimed",
        claims.size()==hookClaims+stepClaims+redirectCount+immediateCount&&claimed(0x44d571+2,4)&&
        claimed(0x5b8775+2,4)&&claimed(0x56a8b2+2,4)&&claimed(0x51ee6b+2,4)&&claimed(0x67eadb+3,4)&&
        claimed(0x4453e7+6,1)&&claimed(0x5619e4+6,4)&&claimed(0x5ab932+8,1)&&claimed(0x4e65d8+6,1));
    const unsigned jumpSites[]={0x686bd0,0x4185c0,0x58ded0,0x590c20,0x5a6fc0,0x5a4cc0,0x40b8ff,0x5b088d,0x41974d,
        0x5ab012,0x51e664,0x5a06c3,0x4b7876,0x4b98bc,0x4b9abc,0x484c2e,0x520ab6,0x43cdce,0x5a7e8f};
    bool allJump=true;
    for(auto site:jumpSites)
        allJump=allJump&&byteAt(site)==0xe9;
    check("every hook and code jump starts with a jmp, the frame limiter included",allJump);
    check("the options page has the rate (4 choices, default 60) and the slowdown notice for 90 and 120",
        pageItems.size()==2&&pageItems[0].kind==wots4::optionChoice&&pageItems[0].choiceCount==4&&
        pageItems[0].defaultValue==1&&pageItems[1].kind==wots4::optionNotice&&pageItems[1].choiceCount==(4u|8u));
    uint32_t shownChoice=99;
    pageCallbacks.read(pageCallbacks.context,&shownChoice,2);
    check("the page shows 60",shownChoice==1);
    check("the frame and options save callbacks are registered",frameCallback&&optionsSavedCallback);

    // The frame limiter
    FrameTimer timer{};
    timer.gameTimeRuns=1;
    check("limiter: forced, an update runs and the game clock moves 1/60 s",
        limiterRunsUpdate(&timer,true)&&gameClock(timer)==1.0f/60.0f);
    check("limiter: the first call runs an update at once",limiterRunsUpdate(&timer,false));
    check("limiter: right after an update, the next is not due",!limiterRunsUpdate(&timer,false));
    const double waited60=secondsUntilNextUpdate(&timer);
    check("limiter: the next update runs 1/60 s after the last",waited60>=1.0/60-0.0005&&waited60<1.0/60+0.01);
    timer.gameTimeRuns=0;
    secondsBeforeUpdate=gameClock(timer);
    check("limiter: with the game clock stopped (menus), updates run and the clock stays",
        limiterRunsUpdate(&timer,true)&&gameClock(timer)==secondsBeforeUpdate);
    timer.gameTimeRuns=1;

    // The step
    check("0x99dca8 is float 1/60 widened",gameDouble(stepSeconds)==(double)(1.0f/60.0f));
    check("0x97cdc4 and 0x9ddd64 are 1/60",
        gameFloat(managerStepSeconds)==1.0f/60.0f&&gameFloat(layoutStepSeconds)==1.0f/60.0f);
    const unsigned conversions[]={0x44d571,0x44d61b,0x44e50a,0x44ebb2,0x44f4b5,0x44f519,0x450778,0x450cb8,
        0x450dcb,0x4b97a9,0x505963,0x54f07d,0x54f3a1,0x54f3c9,0x54f4da,0x5648b2,0x58b0d8};
    bool convertedToMod=true;
    for(auto instruction:conversions)
        convertedToMod=convertedToMod&&modOwned(operandOf(instruction),mod)&&valueAtOperand(instruction)==originalStep;
    check("the 17 frame-count conversions read the Mod's 1/30 (same bits as the game's)",convertedToMod);

    // Characters, UI, title
    check("characters: 0.25, 1.15, square-root decays",
        valueAtOperand(0x44f882)==0.25&&valueAtOperand(0x43d38a)==(double)2.3f/2&&
        closeTo(valueAtOperand(0x44d69c)*valueAtOperand(0x44d69c),(double)0.96825f)&&
        closeTo(valueAtOperand(0x44d4b9)*valueAtOperand(0x44d4b9),(double)0.7f));
    check("UI animations, screen fade and UI waits advance 0.5 frames per update",
        valueAtOperand(0x5b8775)==0.5&&valueAtOperand(0x5b86fe)==0.5&&valueAtOperand(0x543b34)==0.5&&
        valueAtOperand(0x5e1f95)==0.5&&valueAtOperand(0x5d4a8f)==0.5);
    check("UI amounts are halved (title opacity, cursor glide, caret, belts, icon, marker)",
        valueAtOperand(0x67cb5e)==(double)(1.0f/9.0f)/2&&valueAtOperand(0x67e093)==(double)0.2f/2&&
        valueAtOperand(0x4f4ea1)==(double)0.033f/2&&valueAtOperand(0x5d656f)==(double)0.05f/2&&
        valueAtOperand(0x5da6d7)==0.125&&valueAtOperand(0x5b2e4e)==3.0);
    check("title counts are doubled (7000, 1400..5600, 60, 4)",
        immediateAt(0x67d262+2)==7000&&immediateAt(0x67c35f+6)==1400&&immediateAt(0x67c37d+6)==5600&&
        immediateAt(0x67c46b+6)==60&&immediateAt(0x67e0c2+1)==4);
    check("the key repeat constructor makes 22 and 6",immediateAt(0x67eadb+3)==(22|6<<16));
    check("an existing key repeat waits 22 then repeats every 6",repeatDelay()==22&&repeatInterval()==6);
    check("the save list repeat (fires every n + 1) makes up/down 11 and 3, left/right 11 and 5",
        immediateAt(0x53ec20+3)==(11|3<<16)&&immediateAt(0x53ec35+3)==(11|3<<16)&&
        immediateAt(0x53ec4a+3)==(11|5<<16)&&immediateAt(0x53ec63+3)==(11|5<<16));
    check("an existing save list gets the same repeats",
        *(unsigned*)(saveList+0x14)==(11|3<<16)&&*(unsigned*)(saveList+0x24)==(11|3<<16)&&
        *(unsigned*)(saveList+0x34)==(11|5<<16)&&*(unsigned*)(saveList+0x44)==(11|5<<16));

    // Cameras
    check("cameras: f32 smoothing weights read the Mod's float (0.6 -> 1-0.4^0.5)",
        modOwned(operandOf(0x444c82),mod)&&closeTo(floatAtOperand(0x444c82),1-sqrt(1-(double)0.6f)));
    check("cameras: smoothing keeps are square roots, their weights 1-sqrt(k)",
        closeTo(valueAtOperand(0x446551),sqrt((double)0.9f))&&closeTo(valueAtOperand(0x446545),1-sqrt(1-(double)0.1f)));
    check("cameras: mouse turn and wall look-ahead gains are doubled (50, 12)",
        valueAtOperand(0x445288)==50.0&&valueAtOperand(0x4460da)==12.0);
    check("cameras: the talk turn step keeps its limit (9deg*(1-sqrt 0.9)/(1-0.9))",
        closeTo(valueAtOperand(0x445ae4),0.15707960362851026*(1-sqrt(0.8999999761581421))/(1-0.8999999761581421)));
    check("cameras: update counts are doubled (cmp imm8 40, 20 -> 40, 5 -> 10, 10 -> 20)",
        byteAt(0x4453e7+6)==40&&immediateAt(0x445402+6)==40&&immediateAt(0x4453ee+6)==10&&
        byteAt(0x44320e+6)==20&&byteAt(0x443d47+6)==20&&immediateAt(0x41f6fd+6)==10);
    check("cameras: existing cameras get their copied weights rewritten",
        closeTo(cameraField(followCamera,0xc8),1-sqrt(1-(double)0.6f))&&
        closeTo(cameraField(followCamera,0xe8),1-sqrt(1-(double)0.03f))&&
        closeTo(cameraField(battleCamera,0xb0),1-sqrt(1-0.25)));
    const float followTurnAt60=cameraField(followCamera,0xd4);
    *(float*)(followCamera+0xd4)=0;
    // The game applying the options again computes the same speed, from the redirected reads.
    auto applyOptions=(void (__thiscall*)(void*))va(0x444f90);
    applyOptions(followCamera);
    check("cameras: existing cameras had their options re-applied (rotation speed set and unchanged)",
        followTurnAt60>0&&cameraField(followCamera,0xd4)==followTurnAt60);

    // Gameplay
    check("gameplay: AI perception every 12 or 30 updates, hit stop 4, action start invincibility 8",
        immediateAt(0x40a7c4+1)==12&&immediateAt(0x40a7e7+1)==30&&immediateAt(0x487d7c+1)==4&&
        immediateAt(0x49d638+1)==4&&immediateAt(0x43eb80+1)==4&&immediateAt(0x44a0c9+6)==8);
    check("gameplay: Rensatsu slow motion over 16 updates at 1/16 each, ending 60 and 20",
        immediateAt(0x43e8c1+3)==16&&byteAt(0x43e630+2)==16&&immediateAt(0x43e638+1)==16&&
        valueAtOperand(0x43e63d)==0.0625&&immediateAt(0x43d594+6)==60&&immediateAt(0x43d59e+6)==20);
    check("gameplay: the Rensatsu start keeps waiting through 120 updates and goes on at 121",
        rensatsuStartCheck(120)==2&&rensatsuStartCheck(121)==1);
    check("gameplay: AI stance 30, move arrival 6 and give-up 60, neutral action after 30",
        byteAt(0x42b465+6)==30&&byteAt(0x42f611+1)==6&&byteAt(0x42f68a+3)==60&&byteAt(0x448c16+6)==30&&
        byteAt(0x4498b2+6)==30&&byteAt(0x44a3cc+6)==30);
    check("gameplay: an action script timer of 10 frames (11 updates) waits 21 updates (22)",
        updatesBeforeTimerJumps(10)==21);
    check("gameplay: a timer of 0 frames (1 update) waits 1 update (2)",updatesBeforeTimerJumps(0)==1);

    // Action scripts and balloons
    check("action scripts: a command wait of 5 frames (6 updates) stays 11 (12), 0 and -1 are kept",
        commandWaitAfterStore(5)==11&&commandWaitAfterStore(0)==0&&commandWaitAfterStore(-1)==-1);
    check("action scripts: waiting for an action to end gives up after 180 updates instead of 90",
        actionEndCheck(100)==2&&actionEndCheck(180)==2&&actionEndCheck(181)==1);
    check("action scripts: op165 counts 120, AI commands retry after 60",
        immediateAt(0x411de1+3)==120&&byteAt(0x419147+3)==60&&byteAt(0x419618+3)==60);
    check("action scripts: talk choices left alone for 150 frames (151 updates) time out after 301 (302)",
        immediateAt(0x419950+1)==301);
    const int balloonLife=balloonLifeAfterStore(5);
    check("balloons: a life of 5 (6 updates) becomes 11 (12), ecx comes back",
        balloonLife==11&&ecxAfterBalloon==0x12345678);
    check("balloons: a life of 0 (1 update) becomes 1 (2), opening takes 12 updates",
        balloonLifeAfterStore(0)==1&&immediateAt(0x5b0415+6)==12);

    // Torture
    check("torture: commands in seconds read 60 (hang clear time, attacks, fire, stones, mill)",
        valueAtOperand(0x56ac94)==60.0&&valueAtOperand(0x56af70)==60.0&&valueAtOperand(0x56aba8)==60.0);
    struct Hang { unsigned char unused[0xb8]; int updatesLeft; } hang{};
    hang.updatesLeft=125;
    auto hangSeconds=(int (__fastcall*)(Hang*))va(0x5a6fc0);
    check("torture: the hang shows 125 updates left as 2 seconds",hangSeconds(&hang)==2);
    check("torture: before the swing, updates 76 (frame 38) press in time, 72 (36) and 82 (41) not",
        pressInWindow(76,false,0)&&!pressInWindow(72,false,0)&&!pressInWindow(82,false,0));
    check("torture: during swing 0x7e2, updates 90 (45) and 155 (77) in time, 100 (50) not",
        pressInWindow(90,true,0x7e2)&&pressInWindow(155,true,0x7e2)&&!pressInWindow(100,true,0x7e2));
    check("torture: another animation never presses in time",!pressInWindow(90,true,0x7e1));
    float storedSpeed=0;
    check("torture: the mill wheel moves back half its speed per update and keeps the speed",
        millStep(10.0f,2.0f,&storedSpeed)==9.0f&&storedSpeed==2.0f);
    check("torture: hang rope power 0.3, life 0.25 per update, stone speed 1.5 toward 1.75",
        closeTo(valueAtOperand(0x5a8965),0.3)&&valueAtOperand(0x5a855a)==0.25&&
        floatAtOperand(0x5a61c0)==1.5f&&floatAtOperand(0x5a6c96)==1.75f);
    check("torture: counts doubled (success 60, press window 14, held button 40, mill window 20)",
        byteAt(0x5a5083+3)==60&&byteAt(0x5a4b8a+3)==14&&byteAt(0x4e65d8+6)==40&&byteAt(0x5ab932+8)==20&&
        byteAt(0x5ab921+7)==20);
    check("torture: the hang time-out sound plays at 100 updates left, not at 50 or 99",
        hangTimeoutSoundCheck(100)==1&&hangTimeoutSoundCheck(50)==2&&hangTimeoutSoundCheck(99)==2);
    check("torture: the hang cycle 120 and 60 compares unsigned (jb, jae)",
        byteAt(0x5a8925+6)==120&&byteAt(0x5a8935+6)==60&&byteAt(0x5a892c)==0x72&&byteAt(0x5a893c)==0x73);

    // Physics
    check("physics: kick velocity changes halved (-0.1, 0.2, 0.1)",
        closeTo(valueAtOperand(0x52a4b5),-0.1)&&closeTo(floatAtOperand(0x52a4d9),0.2)&&
        closeTo(floatAtOperand(0x52a4e1),0.1));
    check("physics: kick distances per update halved (0.15, 0.075)",
        closeTo(floatAtOperand(0x51e597),0.15)&&closeTo(floatAtOperand(0x51e67a),0.075));
    check("physics: a strong kick needs 0.5 moved in one update instead of 1.0",kickThresholdUsed(0.7f)==0.5f);

    // Items and dropped or thrown weapons
    const Vector moved=itemPositionAfterStep({10.0f,20.0f,30.0f},{2.0f,4.0f,6.0f});
    check("items: one step moves half the velocity (10,20,30 + 0.5*(2,4,6))",
        moved.x==11.0f&&moved.y==22.0f&&moved.z==33.0f);
    check("items: gravity -0.04 (-0.125 kicked up), damping sqrt(0.99), spin halved, kicked up 24 updates",
        closeTo(floatAtOperand(0x5214ef),-0.04)&&floatAtOperand(0x51bb77)==-0.125f&&
        closeTo(floatAtOperand(0x521549),sqrt((double)0.99f))&&closeTo(floatAtOperand(0x52173b),-0.7155848145484924/2)&&
        immediateAt(0x51bb4b+3)==24);

    // Effects
    check("effects: life used up half a frame per update, fades over 60 updates",
        closeTo(valueAtOperand(0x4a4de6),1.0000000521540642/2)&&byteAt(0x4a4a83+3)==60&&byteAt(0x4ba4af+3)==60&&
        byteAt(0x4ba4af+4)==0x01);
    check("effects: a placed effect repeating after 4 + 1 updates (6 holds) waits 11 (12), edx kept",
        placedEffectRepeat(4,1)==11&&edxAfterPlacedEffect==1);

    // Recovery
    check("recovery: the vitality charm level 6 adds 3 per update",vitalityAfterCharm(6,100.0f)==103.0f);
    check("recovery: the HP and vitality gauges catch up over 20 and 60 updates",
        valueAtOperand(0x5dcdf4)==20.0&&valueAtOperand(0x5dcdfa)==60.0);
    check("recovery: the regeneration itself keeps the step (0x484d75 reads 0x99dca8)",
        operandOf(0x484d75)==va(stepSeconds)&&operandOf(0x484b8f)==va(stepSeconds));

    // Scripts and events
    check("scripts: seconds -> updates read 60 (camera, ButtonWait, face, moves, telops, event battle, torture)",
        valueAtOperand(0x4481ba)==60.0&&valueAtOperand(0x561648)==60.0&&valueAtOperand(0x5506c7)==60.0&&
        valueAtOperand(0x55d2a5)==60.0&&valueAtOperand(0x58892b)==60.0&&valueAtOperand(0x56da31)==60.0&&
        valueAtOperand(0x56a8b2)==60.0);
    check("scripts: telop alpha moves 5 per update",valueAtOperand(0x5886a7)==5.0&&valueAtOperand(0x588707)==5.0);
    check("scripts: event battle waits 60, UI hide delays 6, UI refresh 30",
        byteAt(0x56d27f+6)==60&&byteAt(0x56d3ae+6)==60&&immediateAt(0x56d58f+6)==60&&
        immediateAt(0x5670e0+6)==6&&immediateAt(0x58e42b+6)==6&&immediateAt(0x5619e4+6)==30);
    check("scripts: the scene frames stay in 30 fps units (0x560e0c and 0x58b0d8 untouched or 1/30)",
        operandOf(0x560e0c)==va(0x99da78)&&valueAtOperand(0x58b0d8)==originalStep);
    check("scripts: the event scene clock advances half a frame per update",advanceSceneByOneFrame()==0.5f);
    check("scripts: a Wait of 10 frames (11 holds) holds 22 updates",scriptWaitFramesAfterStart(10)==21);
    check("scripts: a Wait of 0 frames (1 hold) holds 2 updates, -1 is left alone",
        scriptWaitFramesAfterStart(0)==1&&scriptWaitFramesAfterStart(-1)==-1);
    Sound sound{};
    sound.frames=10.0f; sound.volumeStep=2.0f; sound.volume=0.0f;
    stepSoundFade(&sound);
    check("sounds: a fade step moves half a frame and half the volume step",
        sound.frames==9.5f&&sound.volume==1.0f);

    // 120 chosen on the options page
    chooseOnOptionsPage(choice120);
    check("120: the change is logged",loggedText("frame rate changed to 120 updates per second"));
    check("120: the steps are float 1/120",gameDouble(stepSeconds)==(double)(1.0f/120.0f)&&
        gameFloat(managerStepSeconds)==1.0f/120.0f&&gameFloat(layoutStepSeconds)==1.0f/120.0f);
    check("120: amounts are a quarter, decays fourth roots, gains 4x",
        valueAtOperand(0x5b8775)==0.25&&closeTo(pow(valueAtOperand(0x446551),4),(double)0.9f)&&
        valueAtOperand(0x445288)==100.0&&valueAtOperand(0x4481ba)==120.0);
    check("120: counts 4x (title 14000, camera 80, Rensatsu end 120, effect fade 120, torture success 120)",
        immediateAt(0x67d262+2)==14000&&byteAt(0x4453e7+6)==80&&immediateAt(0x43d594+6)==120&&
        byteAt(0x4a4a83+3)==120&&byteAt(0x5a5083+3)==120);
    check("120: byte counts past 127 (hang cycle 240, stone first move 160)",
        byteAt(0x5a8925+6)==240&&byteAt(0x5a8935+6)==120&&byteAt(0x5a8e7f+6)==160);
    check("120: the Rensatsu start waits through 240, the hang sound plays at 200",
        rensatsuStartCheck(240)==2&&rensatsuStartCheck(241)==1&&hangTimeoutSoundCheck(200)==1&&
        hangTimeoutSoundCheck(100)==2);
    check("120: an existing key repeat waits 44 then repeats every 12",repeatDelay()==44&&repeatInterval()==12);
    check("120: an existing camera's copied weight is 1-0.4^(1/4)",
        closeTo(cameraField(followCamera,0xc8),1-pow(1-(double)0.6f,0.25)));
    check("120: a timer of 10 frames (11 updates) waits 43 updates (44), a Wait of 10 frames holds 44",
        updatesBeforeTimerJumps(10)==43&&scriptWaitFramesAfterStart(10)==43);
    check("120: the event scene clock advances a quarter frame per update",advanceSceneByOneFrame()==0.25f);
    hang.updatesLeft=250;
    check("120: the hang shows 250 updates left as 2 seconds",hangSeconds(&hang)==2);
    check("120: before the swing, updates 152 (frame 38) press in time, 144 (36) not",
        pressInWindow(152,false,0)&&!pressInWindow(144,false,0));
    secondsBeforeUpdate=gameClock(timer);
    check("120: forced, the game clock moves 1/120 s",
        limiterRunsUpdate(&timer,true)&&gameClock(timer)==(float)(secondsBeforeUpdate+(double)(1.0f/120.0f)));
    secondsUntilNextUpdate(&timer);
    const double waited120=secondsUntilNextUpdate(&timer);
    check("120: the next update runs 1/120 s after the last",waited120>=1.0/120-0.0005&&waited120<1.0/120+0.01);
    optionsSavedCallback(nullptr);
    check("120: the options save writes fps=120 to config.ini",savedFps()==L"120");

    // 90
    chooseOnOptionsPage(choice90);
    check("90: the step is float 1/90, title 10500, key repeat 33 and 9, timer of 10 frames waits 32",
        gameDouble(stepSeconds)==(double)(1.0f/90.0f)&&immediateAt(0x67d262+2)==10500&&repeatDelay()==33&&
        repeatInterval()==9&&updatesBeforeTimerJumps(10)==32);
    check("90: hang cycle 180 and 90, Rensatsu start waits through 180",
        byteAt(0x5a8925+6)==180&&byteAt(0x5a8935+6)==90&&rensatsuStartCheck(180)==2&&rensatsuStartCheck(181)==1);

    // 30: the game's own values
    chooseOnOptionsPage(choice30);
    check("30: the steps are the game's 1/30 again",gameDouble(stepSeconds)==originalStep&&
        gameFloat(managerStepSeconds)==1.0f/30.0f&&gameFloat(layoutStepSeconds)==1.0f/30.0f);
    check("30: amounts, decays and gains are the game's (UI 1.0, camera keep 0.9, mouse 25)",
        valueAtOperand(0x5b8775)==1.0&&valueAtOperand(0x446551)==(double)0.9f&&valueAtOperand(0x445288)==25.0);
    check("30: counts are the game's (title 3500, camera 20, hang cycle 60 and 30, stone 40, key repeat 11 and 3)",
        immediateAt(0x67d262+2)==3500&&byteAt(0x4453e7+6)==20&&byteAt(0x5a8925+6)==60&&byteAt(0x5a8935+6)==30&&
        byteAt(0x5a8e7f+6)==40&&repeatDelay()==11&&repeatInterval()==3);
    check("30: timers, Waits, the scene clock and the Rensatsu start are the game's",
        updatesBeforeTimerJumps(10)==10&&scriptWaitFramesAfterStart(10)==10&&advanceSceneByOneFrame()==1.0f&&
        rensatsuStartCheck(60)==2&&rensatsuStartCheck(61)==1&&hangTimeoutSoundCheck(50)==1);
    secondsUntilNextUpdate(&timer);
    const double waited30=secondsUntilNextUpdate(&timer);
    check("30: the next update runs 1/30 s after the last",waited30>=1.0/30-0.0005&&waited30<1.0/30+0.01);
    optionsSavedCallback(nullptr);
    check("30: the options save writes fps=30",savedFps()==L"30");
    shownChoice=99;
    pageCallbacks.read(pageCallbacks.context,&shownChoice,2);
    check("30: the page shows 30",shownChoice==choice30);

    DeleteFileW(configPath.c_str());
    RemoveDirectoryW(directory.c_str());
    printf("RESULT tests=%u failures=%u; mapped exe copy in this process only\n",tests,failures);
    return failures?1:0;
}
