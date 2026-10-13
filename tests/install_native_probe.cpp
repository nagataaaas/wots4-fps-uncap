// Installs mod.dll on a copy of the exe mapped into this process and calls the hooked frame
// limiter (0x686bd0) with and without a running script. The rest of the game's limiter needs the
// running game, so after installation it returns right after its first 6 bytes (the part MinHook
// moves). Never attaches to the game process or reads saves.
#include <cstdlib>
#include "mapped_exe.h"

using namespace mapped_exe;

constexpr unsigned stepSeconds = 0x99dca8;
constexpr unsigned managerStepSeconds = 0x97cdc4;
constexpr unsigned layoutStepSeconds = 0x9ddd64;
constexpr unsigned scriptRunningOffset = 0x170;
constexpr unsigned redirectCount = 100;
constexpr unsigned immediateCount = 14;

static unsigned char* scriptSystem;  // [0x9eb5f8]
static unsigned char* uiManager;     // [0x9ebb8c]
static unsigned char* cameraManager; // [0x9e964c]
static unsigned char* followCamera;  // [mgr+8]
static unsigned char* battleCamera;  // [mgr+0xc]
static unsigned char* gameOptions;   // [0x9e967c], read by the cameras' vt+0x1c
static unsigned char keyRepeat[0x14];
static unsigned char timer[16];

static double gameDouble(unsigned address) { return *(double*)va(address); }
static float gameFloat(unsigned address) { return *(float*)va(address); }
static unsigned operandOf(unsigned instruction) { return *(unsigned*)va(instruction+2); }
static double valueAtOperand(unsigned instruction) { return *(double*)operandOf(instruction); }
static unsigned immediateAt(unsigned address) { return *(unsigned*)va(address); }
static unsigned char byteAt(unsigned address) { return *(unsigned char*)va(address); }
static float floatAtOperand(unsigned instruction) { return *(float*)operandOf(instruction); }
static float cameraField(unsigned char* camera, unsigned offset) { return *(float*)(camera+offset); }
static bool closeTo(double a, double b) { return fabs(a-b)<=1e-6*fabs(b); }
static unsigned short repeatDelay() { return *(unsigned short*)(keyRepeat+0xc); }
static unsigned short repeatInterval() { return *(unsigned short*)(keyRepeat+0xe); }

// The limiter as the game calls it (0x4dd4fe): esi = timer, one stack argument, ret 4.
static int callFrameLimiter() {
    auto limiter=va(0x686bd0);
    auto timerAddress=timer;
    int result;
    __asm {
        push esi
        mov esi, timerAddress
        push 0
        call limiter
        mov result, eax
        pop esi
    }
    return result;
}

static bool modOwned(unsigned address, HMODULE mod) {
    MEMORY_BASIC_INFORMATION info;
    return VirtualQuery((void*)address,&info,sizeof(info)) && info.AllocationBase==mod;
}

int main(int argc, char** argv) {
    setvbuf(stdout,nullptr,_IONBF,0);
    if(argc!=3) return 2;
    if(!mapImage(argv[1])) return 3;
    // The real .rdata is read-only; map it so here too, so the Mod has to ask for writing.
    DWORD previous;
    VirtualProtect((void*)va(0x905000),0xb55f7,PAGE_READONLY,&previous);
    scriptSystem=(unsigned char*)calloc(1,0x200);
    *(void**)va(0x9eb5f8)=scriptSystem;
    const double originalStep=gameDouble(stepSeconds);

    auto mod=LoadLibraryA(argv[2]);
    auto initialize=mod?(wots4::Initialize)GetProcAddress(mod,"Wots4ModInitialize"):nullptr;
    wots4::HostApi host{sizeof(host),wots4::modApiVersion,hostLog,hostClaim,hostRelease};
    wots4::ModContext context{sizeof(context),wots4::modApiVersion,nullptr,mod,(HMODULE)imageBase,L".\\",&host};
    check("mod.dll exports Wots4ModInitialize",initialize!=nullptr);
    if(!initialize) { printf("RESULT tests=%u failures=%u\n",tests,failures+1); return 1; }

    // A changed instruction: nothing is installed or claimed.
    auto changed=(unsigned char*)va(0x44f4b5);
    changed[1]=0x05;
    check("with a time read changed, the Mod refuses",initialize(&context)==FALSE);
    check("nothing is claimed or redirected",claims.empty()&&operandOf(0x44f4b5)==va(stepSeconds)&&
        *(unsigned char*)va(0x686bd0)==0x55);
    check("the refusal is logged with the reason",loggedText("is not the known version"));
    changed[1]=0x0d;

    check("with the known code, the Mod initializes",initialize(&context)!=FALSE);
    check("the limiter (6 bytes) and the three step constants are claimed",
        claimed(0x686bd0,6)&&claimed(stepSeconds,8)&&claimed(managerStepSeconds,4)&&claimed(layoutStepSeconds,4));
    check("100 operands (4 bytes) and 14 immediates (4 or 1 bytes) are claimed",
        claims.size()==4+redirectCount+immediateCount&&claimed(0x44d571+2,4)&&claimed(0x5b8775+2,4)&&
        claimed(0x444775+2,4)&&claimed(0x67d262+2,4)&&claimed(0x67eadb+3,4)&&claimed(0x67e0c2+1,4)&&
        claimed(0x4453e7+6,1)&&claimed(0x443d47+6,1)&&claimed(0x41f6fd+6,4));
    check("the hook is written at 0x686bd0",*(unsigned char*)va(0x686bd0)==0xe9);

    const unsigned conversions[]={0x44d571,0x44d61b,0x44e50a,0x44ebb2,0x44f4b5,0x44f519,0x450778,0x450cb8,
        0x450dcb,0x4b97a9,0x505963,0x54f07d,0x54f3a1,0x54f3c9,0x54f4da,0x5648b2,0x58b0d8};
    bool convertedToMod=true;
    for(auto instruction:conversions)
        convertedToMod=convertedToMod&&modOwned(operandOf(instruction),mod)&&valueAtOperand(instruction)==originalStep;
    check("the 17 frame-count conversions read the Mod's 1/30 (same bits as the game's)",convertedToMod);
    check("before the first frame, every redirected read sees the game's value",
        valueAtOperand(0x5b8775)==1.0&&valueAtOperand(0x44f882)==0.5&&valueAtOperand(0x5b13b8)==6.0);
    check("0x99dca8 is untouched until the limiter runs",gameDouble(stepSeconds)==originalStep);

    // mov esp,ebp; pop ebp; xor eax,eax; ret 4: undoes the moved prologue and reports "keep waiting".
    const unsigned char limiterReturn[]={0x8b,0xe5,0x5d,0x33,0xc0,0xc2,0x04,0x00};
    memcpy((void*)va(0x686bd6),limiterReturn,sizeof(limiterReturn));

    // The CUIManager and its key repeat state as the constructor leaves them (0x67ead5).
    uiManager=(unsigned char*)calloc(1,0x200);
    *(unsigned*)(keyRepeat+0xc)=0x3000b;
    *(unsigned char**)(uiManager+0xc8)=keyRepeat;
    *(void**)va(0x9ebb8c)=uiManager;

    // The follow and battle cameras with the game's vtables, and options with a camera speed of 5
    // and the normal rotation direction, so the cameras' own vt+0x1c compute their rotation speeds.
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

    check("the hooked limiter returns the game's result",callFrameLimiter()==0);
    check("outside scripts: 0x99dca8 is float 1/60 widened",gameDouble(stepSeconds)==(double)(1.0f/60.0f));
    check("outside scripts: 0x97cdc4 and 0x9ddd64 are 1/60",
        gameFloat(managerStepSeconds)==1.0f/60.0f&&gameFloat(layoutStepSeconds)==1.0f/60.0f);
    check("outside scripts: character values are scaled (0.25, 1.15, square-root decays)",
        valueAtOperand(0x44f882)==0.25&&valueAtOperand(0x43d38a)==(double)2.3f/2&&
        fabs(valueAtOperand(0x44d69c)*valueAtOperand(0x44d69c)-(double)0.96825f)<1e-12&&
        fabs(valueAtOperand(0x44d4b9)*valueAtOperand(0x44d4b9)-(double)0.7f)<1e-12);
    check("the conversions keep 1/30 at 60 updates",valueAtOperand(0x44e50a)==originalStep);
    check("outside scripts: UI animations, screen fade and UI waits advance 0.5 frames per update",
        valueAtOperand(0x5b8775)==0.5&&valueAtOperand(0x5b86fe)==0.5&&valueAtOperand(0x543b34)==0.5&&
        valueAtOperand(0x5e1f95)==0.5&&valueAtOperand(0x5d4a8f)==0.5);
    check("outside scripts: UI amounts are halved (title opacity, cursor glide, caret, belts, icon, marker)",
        valueAtOperand(0x67cb5e)==(double)(1.0f/9.0f)/2&&valueAtOperand(0x67e093)==(double)0.2f/2&&
        valueAtOperand(0x4f4ea1)==(double)0.033f/2&&valueAtOperand(0x5d656f)==(double)0.05f/2&&
        valueAtOperand(0x5da6d7)==0.125&&valueAtOperand(0x5b2e4e)==3.0);
    check("outside scripts: title counts are doubled (7000, 1400..5600, 60, 4)",
        immediateAt(0x67d262+2)==7000&&immediateAt(0x67c35f+6)==1400&&immediateAt(0x67c37d+6)==5600&&
        immediateAt(0x67c46b+6)==60&&immediateAt(0x67e0c2+1)==4);
    check("outside scripts: the key repeat constructor makes 22 and 6",immediateAt(0x67eadb+3)==(22|6<<16));
    check("outside scripts: the live key repeat waits 22 then repeats every 6",
        repeatDelay()==22&&repeatInterval()==6);
    const float followTurnAt60=cameraField(followCamera,0xd4);
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
    check("cameras: the follow camera's copied weights are rewritten",
        closeTo(cameraField(followCamera,0xc8),1-sqrt(1-(double)0.6f))&&
        closeTo(cameraField(followCamera,0xe8),1-sqrt(1-(double)0.03f)));
    check("cameras: the battle camera's pitch weight target is rewritten",
        closeTo(cameraField(battleCamera,0xb0),1-sqrt(1-0.25)));
    check("cameras: the follow camera's options were re-applied (a rotation speed is set)",followTurnAt60>0);

    *(int*)(scriptSystem+scriptRunningOffset)=1;
    callFrameLimiter();
    check("in a script: 0x99dca8 is the game's 1/30 again",gameDouble(stepSeconds)==originalStep);
    check("in a script: 0x97cdc4 and 0x9ddd64 are 1/30",
        gameFloat(managerStepSeconds)==1.0f/30.0f&&gameFloat(layoutStepSeconds)==1.0f/30.0f);
    check("in a script: redirected reads see the game's own values",
        valueAtOperand(0x44f882)==0.5&&valueAtOperand(0x43d38a)==(double)2.3f&&
        valueAtOperand(0x44d69c)==(double)0.96825f&&valueAtOperand(0x44d4b9)==(double)0.7f&&
        valueAtOperand(0x5b8775)==1.0&&valueAtOperand(0x67cb5e)==(double)(1.0f/9.0f)&&
        valueAtOperand(0x5b13b8)==6.0);
    check("in a script: counts are the game's own (3500, 30, key repeat 11 and 3)",
        immediateAt(0x67d262+2)==3500&&immediateAt(0x67c46b+6)==30&&immediateAt(0x67eadb+3)==0x3000b&&
        repeatDelay()==11&&repeatInterval()==3);
    check("in a script: camera reads and counts are the game's own",
        floatAtOperand(0x444c82)==0.6f&&valueAtOperand(0x446551)==(double)0.9f&&valueAtOperand(0x445288)==25.0&&
        byteAt(0x4453e7+6)==20&&immediateAt(0x41f6fd+6)==5&&cameraField(followCamera,0xc8)==0.6f&&
        cameraField(battleCamera,0xb0)==0.25f);
    check("in a script: the follow camera turns twice as far per update as at 60",
        closeTo(cameraField(followCamera,0xd4),followTurnAt60*2.0));

    *(int*)(scriptSystem+scriptRunningOffset)=0;
    callFrameLimiter();
    check("after the script: back to 1/60",gameDouble(stepSeconds)==(double)(1.0f/60.0f));
    *(void**)va(0x9eb5f8)=nullptr;
    *(void**)va(0x9ebb8c)=nullptr;
    callFrameLimiter();
    check("no script system or UI manager yet (boot): 1/60, nothing read through null",
        gameDouble(stepSeconds)==(double)(1.0f/60.0f));
    check("the installation is logged",loggedText("installed: 60 updates per second outside scripts"));

    printf("RESULT tests=%u failures=%u; mapped exe copy in this process only\n",tests,failures);
    return failures?1:0;
}
