// fps_uncap: runs Way of the Samurai 4 at 60 logic updates (and frames) per second
// outside scripts, in memory only.
//
// The game waits for the next frame in its frame limiter (0x686bd0) and advances
// everything by one fixed step per frame. The step is shared constants: f64 at
// 0x99dca8 (limiter, game clock, timers, integration) and f32 at 0x97cdc4 and
// 0x9ddd64 (the dt passed to the managers, UI animation). This Mod writes 1/60
// to them, and 1/30 back while a script runs ([0x9eb5f8]+0x170), because scripts
// count frames in places not corrected yet. The limiter hook switches them once
// per call, before the game's own wait.
//
// Some reads of 0x99dca8 turn the game's 30 fps frame counts (motion data,
// effects, script arguments) into seconds or back; they are pointed at this
// Mod's own 1/30 so they stay right. Amounts the game applies once per update
// (UI layout animations and screen fades advance one 30 fps frame per update)
// are pointed at values this Mod scales to the step, and counts of updates
// written into instructions or held by the menu key repeat are rewritten with it.
// The cameras take no dt at all, so their smoothing weights, rotation speeds and
// counts are handled the same way; the weights a camera copies into itself are
// written again, and its options re-applied (vt+0x1c), when the rate changes.
#include <wots4/mod_api.h>
#include <MinHook.h>
#include <cstdio>
#include <cstring>
#include "step_values.h"

namespace {

using fps_uncap::PerUpdate;

uintptr_t gameBase;
HMODULE self;
const wots4::HostApi* host;

uintptr_t va(uintptr_t address) { return gameBase+address-0x400000; }

bool same(uintptr_t address, const unsigned char* bytes, size_t count) {
    return !memcmp(reinterpret_cast<const void*>(va(address)),bytes,count);
}

uintptr_t readGamePointer(uintptr_t address) { return *reinterpret_cast<const uintptr_t*>(va(address)); }

double readGameDouble(uintptr_t address) { return *reinterpret_cast<const double*>(va(address)); }

void log(const char* message) {
    if(host->log)
        host->log(self,message);
}

constexpr unsigned highUpdatesPerSecond = 60;

// ---- Shared step constants

constexpr uintptr_t stepSeconds = 0x99dca8;        // f64 (double)(float)(1/30), 61 reads
constexpr uintptr_t managerStepSeconds = 0x97cdc4;  // f32 1/30, dt for the managers (0x4c2f00)
constexpr uintptr_t layoutStepSeconds = 0x9ddd64;   // f32 1/30 in .data, UI layout animation
constexpr uint64_t originalStepBits = 0x3fa1111120000000;
constexpr uint32_t originalStepFloatBits = 0x3d088889;

// ---- Reads redirected to this Mod's values

// Each is a 6-byte x87 instruction with its absolute operand address at +2.
constexpr uint32_t operandOffset = 2;
constexpr uint32_t operandLength = 4;

// On a qword (f64) operand
constexpr unsigned char fdiv[2] = {0xdc,0x35};
constexpr unsigned char fld[2] = {0xdd,0x05};
constexpr unsigned char fmul[2] = {0xdc,0x0d};
constexpr unsigned char fsub[2] = {0xdc,0x25};
constexpr unsigned char fadd[2] = {0xdc,0x05};
// On a dword (f32) operand
constexpr unsigned char fldFloat[2] = {0xd9,0x05};
constexpr unsigned char fcompFloat[2] = {0xd8,0x1d};

bool readsFloat(const unsigned char* opcode) { return opcode==fldFloat||opcode==fcompFloat; }

constexpr uintptr_t oneFrame = 0x99da28;  // f64 1.0, shared by 278 instructions
constexpr double talkTurnDecay = 0.8999999761581421;  // k of the talk camera turn (0x445ad9)

struct OperandRedirect {
    uintptr_t instruction;
    const unsigned char* opcode;
    uintptr_t originalOperand;
    double valueAtGameRate;  // What the original operand holds in the Steam version
    PerUpdate kind;
    double decayOfAmount;    // Only for PerUpdate::amountUnderDecay
    double value;            // Read by the game in place of the original operand
    float valueFloat;        // The same, for a dword operand
};

OperandRedirect redirects[] = {
    // Seconds -> 30 fps frames: knockback spin and tilt (0x44c5d0)
    {0x44d571,fdiv,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x44d61b,fdiv,stepSeconds,0.03333333507180214,PerUpdate::keep},
    // 30 fps frames -> seconds: motion data (MotionDatabase u16 frame counts)
    {0x44e50a,fld,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x44ebb2,fld,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x44f4b5,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x44f519,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x450778,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x450cb8,fld,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x450dcb,fld,stepSeconds,0.03333333507180214,PerUpdate::keep},
    // 30 fps frames -> seconds: effects, game parameters, script characters, script arguments, events
    {0x4b97a9,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x505963,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x54f07d,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x54f3a1,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x54f3c9,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x54f4da,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x5648b2,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    {0x58b0d8,fmul,stepSeconds,0.03333333507180214,PerUpdate::keep},
    // Characters: half a motion advance before a motion event frame (0x44f860), Rensatsu gauge
    // drain (0x43d340), knockback spin and tilt decay, leftover spin decay (0x44c5d0)
    {0x44f882,fmul,0x99da98,0.5,PerUpdate::amount},
    {0x43d38a,fsub,0x99ed70,static_cast<double>(2.3f),PerUpdate::amount},
    {0x44d69c,fld,0x99e8f8,static_cast<double>(0.96825f),PerUpdate::decay},
    {0x44d4b9,fmul,0x99dcf0,static_cast<double>(0.7f),PerUpdate::decay},
    // One 30 fps frame of every UI layout animation, forward and reverse (0x5b8670)
    {0x5b8775,fadd,oneFrame,1.0,PerUpdate::amount},
    {0x5b86fe,fsub,oneFrame,1.0,PerUpdate::amount},
    // One 30 fps frame of the screen fade (0x543b20, every GamePart)
    {0x543b34,fadd,oneFrame,1.0,PerUpdate::amount},
    // Title menu: item opacity (0x67c090) and cursor glide (0x67e050)
    {0x67cb5e,fld,0x99e540,static_cast<double>(1.0f/9.0f),PerUpdate::amount},
    {0x67ce34,fld,0x99e540,static_cast<double>(1.0f/9.0f),PerUpdate::amount},
    {0x67e093,fadd,0x99da30,static_cast<double>(0.2f),PerUpdate::amount},
    // 0.033 s per update: text caret blinks, save wait, key mapping countdown
    {0x4f4ea1,fsub,0x99de90,static_cast<double>(0.033f),PerUpdate::amount},
    {0x5f705c,fsub,0x99de90,static_cast<double>(0.033f),PerUpdate::amount},
    {0x603522,fsub,0x99de90,static_cast<double>(0.033f),PerUpdate::amount},
    {0x64a5d5,fsub,0x99de90,static_cast<double>(0.033f),PerUpdate::amount},
    // Event black bars in and out (0x5d6450), gauge icon fade (0x5da6d0)
    {0x5d64d1,fadd,0x99dcc0,static_cast<double>(0.05f),PerUpdate::amount},
    {0x5d656f,fsub,0x99dcc0,static_cast<double>(0.05f),PerUpdate::amount},
    {0x5da6d7,fadd,0x99da48,0.25,PerUpdate::amount},
    // One 30 fps frame: torture UI (0x5e1f40) and the waits of UI 22 (0x5d4720)
    {0x5e1f95,fsub,oneFrame,1.0,PerUpdate::amount},
    {0x5d484c,fsub,oneFrame,1.0,PerUpdate::amount},
    {0x5d4962,fsub,oneFrame,1.0,PerUpdate::amount},
    {0x5d49e3,fsub,oneFrame,1.0,PerUpdate::amount},
    {0x5d4a2b,fsub,oneFrame,1.0,PerUpdate::amount},
    {0x5d4a8f,fsub,oneFrame,1.0,PerUpdate::amount},
    // Character model screen marker growth (0x5b1310, 0x5b2da0)
    {0x5b13b8,fadd,0x99e200,6.0,PerUpdate::amount},
    {0x5b2e4e,fadd,0x99e200,6.0,PerUpdate::amount},

    // Cameras take no dt: every move is a fixed share or amount per update (0x441000-0x448900).
    // Follow camera options (vt+0x1c 0x444f90): rotation speed (0.6*speed+2 degrees per update)
    // and the start of the eye weight
    {0x444fa9,fld,0x99dd40,0.6000000238418579,PerUpdate::amount},
    {0x444fbc,fadd,0x99dd38,0.034906577318906784,PerUpdate::amount},
    {0x44500c,fld,0x99da98,0.5,PerUpdate::amount},
    // Follow camera: mouse turn per update (25*dx/window width) and the wall check look-ahead
    {0x445288,fld,0x99dcd0,25.0,PerUpdate::gain},
    {0x4460da,fld,0x99e200,6.0,PerUpdate::gain},
    // Follow camera constructor (0x444b40) and activation (0x444d30): smoothing weights
    {0x444c82,fldFloat,0x97c064,0.6000000238418579,PerUpdate::weight},
    {0x444c2b,fldFloat,0x97e01c,0.5,PerUpdate::weight},
    {0x444c52,fldFloat,0x966c28,0.20000000298023224,PerUpdate::weight},
    {0x444cac,fldFloat,0x97df24,0.029999999329447746,PerUpdate::weight},
    {0x444eaf,fldFloat,0x99dab0,0.03999999910593033,PerUpdate::weight},
    {0x444ebb,fldFloat,0x97c448,0.009999999776482582,PerUpdate::weight},
    {0x4454f3,fldFloat,0x97c448,0.009999999776482582,PerUpdate::weight},
    // Follow camera: "the target moves" when the look-at moved more than 1.0 in one update
    {0x4453d1,fcompFloat,0x97e020,1.0,PerUpdate::amount},
    // Follow camera: automatic height, look-at / height / eye smoothing, field of view
    {0x4457c0,fmul,0x99e938,0.9599999785423279,PerUpdate::decay},
    {0x4457ca,fmul,0x99db80,0.03999999910593033,PerUpdate::weight},
    {0x446399,fmul,0x99e398,0.019999999552965164,PerUpdate::weight},
    {0x4463a5,fmul,0x99e390,0.9800000190734863,PerUpdate::decay},
    {0x4463b9,fmul,0x99dc58,0.009999999776482582,PerUpdate::weight},
    {0x4463c5,fmul,0x99e278,0.9900000095367432,PerUpdate::decay},
    {0x446545,fmul,0x99da40,0.10000000149011612,PerUpdate::weight},
    {0x446551,fmul,0x99e358,0.8999999761581421,PerUpdate::decay},
    {0x4465c0,fmul,0x99da40,0.10000000149011612,PerUpdate::weight},
    {0x4465c9,fmul,0x99e358,0.8999999761581421,PerUpdate::decay},
    // Follow camera talk turn (0x4459b0): d = 0.9*d +- 9 degrees
    {0x445ad9,fmul,0x99e358,0.8999999761581421,PerUpdate::decay},
    {0x445ae4,fadd,0x99e548,0.15707960362851026,PerUpdate::amountUnderDecay,talkTurnDecay},
    {0x445aec,fsub,0x99e548,0.15707960362851026,PerUpdate::amountUnderDecay,talkTurnDecay},
    // Battle camera: distance, look-at and field of view smoothing
    {0x4428f6,fldFloat,0x99dc48,0.07000000029802322,PerUpdate::weight},
    {0x4428fe,fldFloat,0x99dad8,0.05000000074505806,PerUpdate::weight},
    {0x443a33,fldFloat,0x96fbb4,0.75,PerUpdate::weight},
    {0x443b2e,fldFloat,0x96fbb4,0.75,PerUpdate::weight},
    {0x442dbc,fldFloat,0x99dc70,0.12999999523162842,PerUpdate::weight},
    {0x442dc4,fldFloat,0x99dad8,0.05000000074505806,PerUpdate::weight},
    {0x442d56,fmul,0x99da40,0.10000000149011612,PerUpdate::weight},
    {0x442d5f,fmul,0x99e560,0.8999999985098839,PerUpdate::decay},
    // Battle camera: automatic height and manual rotation (1 degree * speed per update)
    {0x4430ce,fmul,0x99da40,0.10000000149011612,PerUpdate::weight},
    {0x4430da,fmul,0x99e358,0.8999999761581421,PerUpdate::decay},
    {0x4430f3,fmul,0x99ea38,0.01745329238474369,PerUpdate::amount},
    {0x44315c,fmul,0x99ea38,0.01745329238474369,PerUpdate::amount},
    // Battle camera: angle weights and how fast they move to their targets
    {0x4433da,fmul,0x99dcc0,0.05000000074505806,PerUpdate::amount},
    {0x443392,fldFloat,0x969be4,0.3499999940395355,PerUpdate::weight},
    {0x444010,fldFloat,0x97df24,0.029999999329447746,PerUpdate::weight},
    {0x4440de,fldFloat,0x97df24,0.029999999329447746,PerUpdate::weight},
    {0x44349f,fmul,0x99da30,0.20000000298023224,PerUpdate::weight},
    {0x4434ce,fmul,0x99da30,0.20000000298023224,PerUpdate::weight},
    {0x44284e,fldFloat,0x99db78,0.25,PerUpdate::weight},
    {0x44285a,fldFloat,0x99dab0,0.03999999910593033,PerUpdate::weight},
    {0x442c92,fmul,0x99da48,0.25,PerUpdate::amount},
    {0x442cb0,fmul,0x99db80,0.03999999910593033,PerUpdate::amount},
    // Battle camera: yaw changes at most 2.5 degrees per update
    {0x442a8b,fcompFloat,0x97df48,0.04363323375582695,PerUpdate::amount},
    {0x442ace,fsub,0x99ea40,0.04363323375582695,PerUpdate::amount},
    {0x442ad6,fadd,0x99ea40,0.04363323375582695,PerUpdate::amount},
    // Camera manager stick smoothing (0x442400), shake progress (0x441570)
    {0x442474,fld,0x99e3b8,0.4000000059604645,PerUpdate::weight},
    {0x44247f,fld,0x99dd40,0.6000000238418579,PerUpdate::decay},
    {0x441584,fldFloat,0x9d3704,0.029999999329447746,PerUpdate::amount},
    // Equipment and body screen camera (0x444500): turn, height and zoom per update
    {0x4445ad,fld,0x99e558,0.05235987901687622,PerUpdate::amount},
    {0x4446e2,fmul,0x99da40,0.10000000149011612,PerUpdate::amount},
    {0x44473a,fadd,0x99dc90,5.0,PerUpdate::amount},
    {0x444775,fsub,0x99dc90,5.0,PerUpdate::amount},
};

double originalOperandValue(const OperandRedirect& redirect) {
    if(readsFloat(redirect.opcode))
        return *reinterpret_cast<const float*>(va(redirect.originalOperand));
    return readGameDouble(redirect.originalOperand);
}

bool redirectMatches(const OperandRedirect& redirect) {
    return same(redirect.instruction,redirect.opcode,2) &&
        readGamePointer(redirect.instruction+operandOffset)==va(redirect.originalOperand) &&
        originalOperandValue(redirect)==redirect.valueAtGameRate;
}

void setRedirectValue(OperandRedirect& redirect, unsigned updatesPerSecond) {
    redirect.value=fps_uncap::perUpdateValue(redirect.kind,redirect.valueAtGameRate,updatesPerSecond,
        redirect.decayOfAmount);
    redirect.valueFloat=static_cast<float>(redirect.value);
}

uintptr_t replacementOperand(const OperandRedirect& redirect) {
    if(readsFloat(redirect.opcode))
        return reinterpret_cast<uintptr_t>(&redirect.valueFloat);
    return reinterpret_cast<uintptr_t>(&redirect.value);
}

// ---- Counts of updates

// An immediate count of updates inside an instruction, 1 or 4 bytes. A second count in the upper
// 16 bits (pairedHighCount) packs two u16 counts into one 32-bit immediate.
struct UpdateCountImmediate {
    uintptr_t instruction;
    unsigned char bytes[10];  // The whole instruction, immediate included
    uint32_t length;
    uint32_t immediateOffset;
    uint32_t immediateSize;
    uint32_t countAtGameRate;
    uint32_t pairedHighCount;
};

constexpr uint32_t repeatDelay = 11;    // Updates before a held direction repeats
constexpr uint32_t repeatInterval = 3;  // Updates between repeats
constexpr uint32_t largestSignedByte = 127;  // An imm8 of cmp is sign-extended

UpdateCountImmediate updateCounts[] = {
    // TitleMenu: each background picture restarts its animations after 3500 updates (0x67d235),
    // the five start 700 apart (0x67c355)
    {0x67d262,{0xc7,0x06,0xac,0x0d,0x00,0x00},6,2,4,3500,0},
    {0x67c35f,{0xc7,0x83,0x60,0x01,0x00,0x00,0xbc,0x02,0x00,0x00},10,6,4,700,0},
    {0x67c369,{0xc7,0x83,0x64,0x01,0x00,0x00,0x78,0x05,0x00,0x00},10,6,4,1400,0},
    {0x67c373,{0xc7,0x83,0x68,0x01,0x00,0x00,0x34,0x08,0x00,0x00},10,6,4,2100,0},
    {0x67c37d,{0xc7,0x83,0x6c,0x01,0x00,0x00,0xf0,0x0a,0x00,0x00},10,6,4,2800,0},
    // TitleMenu state 6 waits 30 updates (0x67c47a); the cursor settles for 2 (0x67e050)
    {0x67c46b,{0xc7,0x83,0xa0,0x00,0x00,0x00,0x1e,0x00,0x00,0x00},10,6,4,30,0},
    {0x67e0c2,{0xb8,0x02,0x00,0x00,0x00},5,1,4,2,0},
    // CUIManager constructor (0x67e9c0): menu key repeat delay and interval (0x680d90)
    {0x67eadb,{0xc7,0x40,0x0c,0x0b,0x00,0x03,0x00},7,3,4,repeatDelay,repeatInterval},
    // Follow camera: "moving" after 20 updates of movement, "stopped" after 5 (0x445190)
    {0x4453e7,{0x83,0xbe,0x30,0x01,0x00,0x00,0x14},7,6,1,20,0},
    {0x445402,{0xc7,0x86,0x30,0x01,0x00,0x00,0x14,0x00,0x00,0x00},10,6,4,20,0},
    {0x4453ee,{0xc7,0x86,0x34,0x01,0x00,0x00,0x05,0x00,0x00,0x00},10,6,4,5,0},
    // Talk system (0x41f6fd): the camera turns toward the speaker for 5 updates
    {0x41f6fd,{0xc7,0x87,0xe4,0x00,0x00,0x00,0x05,0x00,0x00,0x00},10,6,4,5,0},
    // Battle camera: looks for a better angle every 10 updates (0x44320e), and keeps a new
    // angle for 10 updates (0x443d47)
    {0x44320e,{0x83,0xbf,0x14,0x01,0x00,0x00,0x0a},7,6,1,10,0},
    {0x443d47,{0xc6,0x86,0xcd,0x00,0x00,0x00,0x0a},7,6,1,10,0},
};

uint32_t immediateFor(const UpdateCountImmediate& count, unsigned updatesPerSecond) {
    uint32_t low=fps_uncap::scaledUpdateCount(count.countAtGameRate,updatesPerSecond);
    if(!count.pairedHighCount)
        return low;
    return low|fps_uncap::scaledUpdateCount(count.pairedHighCount,updatesPerSecond)<<16;
}

bool immediateFits(const UpdateCountImmediate& count, unsigned updatesPerSecond) {
    return count.immediateSize!=1 || immediateFor(count,updatesPerSecond)<=largestSignedByte;
}

bool writeImmediate(const UpdateCountImmediate& count, uint32_t value) {
    auto target=reinterpret_cast<void*>(va(count.instruction+count.immediateOffset));
    DWORD previous;
    if(!VirtualProtect(target,count.immediateSize,PAGE_EXECUTE_READWRITE,&previous))
        return false;
    if(count.immediateSize==1)
        *reinterpret_cast<uint8_t*>(target)=static_cast<uint8_t>(value);
    else
        *reinterpret_cast<uint32_t*>(target)=value;
    VirtualProtect(target,count.immediateSize,previous,&previous);
    FlushInstructionCache(GetCurrentProcess(),target,count.immediateSize);
    return true;
}

// The live key repeat state of the CUIManager, when it already exists.
constexpr uintptr_t uiManager = 0x9ebb8c;        // CUIManager*, set at 0x4f1686
constexpr uint32_t uiManagerKeyRepeat = 0xc8;    // Key repeat state, created at 0x67ead5
constexpr uint32_t keyRepeatDelay = 0xc;         // u16, read at 0x680df7
constexpr uint32_t keyRepeatInterval = 0xe;      // u16, read at 0x680dce

bool keyRepeatCodeMatches() {
    const unsigned char storeState[] = {0x89,0x86,0xc8,0x00,0x00,0x00};  // mov [esi+0xc8],eax
    const unsigned char readDelay[] = {0x0f,0xb7,0x50,0x0c};             // movzx edx,word [eax+0xc]
    const unsigned char readInterval[] = {0x0f,0xb7,0x48,0x0e};          // movzx ecx,word [eax+0xe]
    const unsigned char storeManager[] = {0xa3};                         // mov [0x9ebb8c],eax
    return same(0x67eaef,storeState,sizeof(storeState)) && same(0x680df7,readDelay,sizeof(readDelay)) &&
        same(0x680dce,readInterval,sizeof(readInterval)) && same(0x4f1686,storeManager,sizeof(storeManager)) &&
        readGamePointer(0x4f1687)==va(uiManager);
}

void writeLiveKeyRepeat(unsigned updatesPerSecond) {
    auto manager=readGamePointer(uiManager);
    if(!manager)
        return;
    auto state=*reinterpret_cast<const uintptr_t*>(manager+uiManagerKeyRepeat);
    if(!state)
        return;
    *reinterpret_cast<uint16_t*>(state+keyRepeatDelay)=
        static_cast<uint16_t>(fps_uncap::scaledUpdateCount(repeatDelay,updatesPerSecond));
    *reinterpret_cast<uint16_t*>(state+keyRepeatInterval)=
        static_cast<uint16_t>(fps_uncap::scaledUpdateCount(repeatInterval,updatesPerSecond));
}

// ---- Camera settings held in the cameras

// Some weights are copied into the cameras only when they are made, and the rotation speeds
// when the options are applied (vt+0x1c), so a rate change writes them again.
constexpr uintptr_t cameraManager = 0x9e964c;  // CCameraManager*, set at 0x4f1b81
constexpr uint32_t followCameraSlot = 0x8;     // Cameras by kind at +8+kind*4 (0x442072)
constexpr uint32_t battleCameraSlot = 0xc;
constexpr uintptr_t followCameraVtable = 0x980784;
constexpr uintptr_t battleCameraVtable = 0x9806ec;
constexpr uint32_t applyOptionsSlot = 7;       // vt+0x1c: reads the options at [0x9e967c]
constexpr uintptr_t gameOptions = 0x9e967c;

struct CameraWeightField {
    uint32_t vtable;
    uint32_t field;                  // f32 in the camera
    uintptr_t constructorRead;       // The redirected read that gives the field its value
};

const CameraWeightField cameraWeightFields[] = {
    {followCameraVtable,0xc8,0x444c82},  // Input smoothing (constructor 0x444c88)
    {followCameraVtable,0xac,0x444c2b},  // Look-at weight target (0x444c46)
    {followCameraVtable,0xb4,0x444c52},  // Height weight target (0x444c64)
    {followCameraVtable,0xe8,0x444cac},  // Distance smoothing (0x444cb2)
    {battleCameraVtable,0xb0,0x44284e},  // Pitch weight target (activation 0x442860)
};

bool cameraCodeMatches() {
    const unsigned char storeManager[] = {0xa3};                          // mov [0x9e964c],eax
    const unsigned char storeCamera[] = {0x89,0x44,0x99,0x08};            // mov [ecx+ebx*4+8],eax
    const unsigned char followFields[][6] = {{0xd9,0x9e,0xc8,0x00,0x00,0x00},{0xd9,0x9e,0xac,0x00,0x00,0x00},
        {0xd9,0x96,0xb4,0x00,0x00,0x00},{0xd9,0x9e,0xe8,0x00,0x00,0x00}};
    const unsigned char battleField[] = {0x89,0x86,0xb0,0x00,0x00,0x00};  // mov [esi+0xb0],eax
    auto followVtable=reinterpret_cast<const uintptr_t*>(va(followCameraVtable));
    auto battleVtable=reinterpret_cast<const uintptr_t*>(va(battleCameraVtable));
    return same(0x4f1b81,storeManager,sizeof(storeManager)) && readGamePointer(0x4f1b82)==va(cameraManager) &&
        same(0x442072,storeCamera,sizeof(storeCamera)) && same(0x444c88,followFields[0],6) &&
        same(0x444c46,followFields[1],6) && same(0x444c64,followFields[2],6) && same(0x444cb2,followFields[3],6) &&
        same(0x442860,battleField,sizeof(battleField)) &&
        followVtable[applyOptionsSlot]==va(0x444f90) && battleVtable[applyOptionsSlot]==va(0x442bf0);
}

const OperandRedirect* findRedirect(uintptr_t instruction) {
    for(auto& redirect:redirects)
        if(redirect.instruction==instruction)
            return &redirect;
    return nullptr;
}

uintptr_t cameraOfKind(uint32_t slot, uintptr_t vtable) {
    auto manager=readGamePointer(cameraManager);
    if(!manager)
        return 0;
    auto camera=*reinterpret_cast<const uintptr_t*>(manager+slot);
    if(!camera || *reinterpret_cast<const uintptr_t*>(camera)!=va(vtable))
        return 0;
    return camera;
}

void applyCameraOptions(uintptr_t camera) {
    if(!camera || !readGamePointer(gameOptions))
        return;
    auto apply=reinterpret_cast<void (__thiscall*)(uintptr_t)>(
        reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(camera))[applyOptionsSlot]);
    apply(camera);
}

// Needs the redirected values of the new rate already set.
void writeLiveCameraSettings() {
    auto follow=cameraOfKind(followCameraSlot,followCameraVtable);
    auto battle=cameraOfKind(battleCameraSlot,battleCameraVtable);
    applyCameraOptions(follow);
    applyCameraOptions(battle);
    for(auto& weight:cameraWeightFields) {
        auto camera=weight.vtable==followCameraVtable ? follow : battle;
        if(camera)
            *reinterpret_cast<float*>(camera+weight.field)=findRedirect(weight.constructorRead)->valueFloat;
    }
}

// ---- Frame limiter

// CFPSTimer wait: esi = timer, [esp+4] = force, ret 4. Called again and again while it
// waits, so the hook only compares and, when the scene changes, writes the values.
constexpr uintptr_t frameLimiter = 0x686bd0;
constexpr uint32_t frameLimiterHookLength = 6;  // push ebp; mov ebp,esp; sub esp,8
constexpr uintptr_t scriptEventSystem = 0x9eb5f8;  // CScriptEventSystem*
constexpr uint32_t scriptRunningOffset = 0x170;    // 1 while a script runs (0x5729b6, 0x573973)

bool frameLimiterCodeMatches() {
    const unsigned char prologue[] = {0x55,0x8b,0xec,0x83,0xec,0x08,0xe8};
    const unsigned char readStep[] = {0xdd,0x05};  // fld qword [0x99dca8]: next time = last + step
    // mov eax,[0x9eb5f8]; test eax,eax; je; cmp dword [eax+0x170],1 (vitality regeneration stops in scripts)
    const unsigned char scriptCheck[] = {0x85,0xc0,0x74,0x0d,0x83,0xb8,0x70,0x01,0x00,0x00,0x01};
    return same(frameLimiter,prologue,sizeof(prologue)) && same(0x686be4,readStep,sizeof(readStep)) &&
        readGamePointer(0x686be6)==va(stepSeconds) && same(0x484b0b,scriptCheck,sizeof(scriptCheck)) &&
        readGamePointer(0x484b07)==va(scriptEventSystem);
}

void* originalFrameLimiter;
unsigned appliedUpdatesPerSecond;  // 0 until the first frame

// Runs only when the rate changes (entering or leaving a script), not every frame.
void applyRate(unsigned updatesPerSecond) {
    auto step=fps_uncap::stepFor(updatesPerSecond);
    *reinterpret_cast<double*>(va(stepSeconds))=step.seconds;
    *reinterpret_cast<float*>(va(managerStepSeconds))=step.secondsFloat;
    *reinterpret_cast<float*>(va(layoutStepSeconds))=step.secondsFloat;
    for(auto& redirect:redirects)
        setRedirectValue(redirect,updatesPerSecond);
    // Installation already wrote every immediate once, so VirtualProtect is known to work here.
    for(auto& count:updateCounts)
        writeImmediate(count,immediateFor(count,updatesPerSecond));
    writeLiveKeyRepeat(updatesPerSecond);
    writeLiveCameraSettings();
}

bool scriptRunning() {
    auto system=readGamePointer(scriptEventSystem);
    return system && *reinterpret_cast<const int32_t*>(system+scriptRunningOffset)==1;
}

// The switch takes effect from this frame's wait; the update that follows uses the new step.
void __cdecl applyRateForScene() {
    auto wanted=fps_uncap::updatesPerSecondFor(scriptRunning(),highUpdatesPerSecond);
    if(wanted==appliedUpdatesPerSecond)
        return;
    applyRate(wanted);
    appliedUpdatesPerSecond=wanted;
}

__declspec(naked) void frameLimiterHook() {
    __asm {
        pushad
        pushfd
        call applyRateForScene
        popfd
        popad
        jmp dword ptr [originalFrameLimiter]
    }
}

// ---- Installation

// .rdata is read-only; the step constants stay writable so the scene switch is a plain store.
bool makeWritable(uintptr_t address, uint32_t length) {
    DWORD previous;
    return VirtualProtect(reinterpret_cast<void*>(va(address)),length,PAGE_READWRITE,&previous)!=FALSE;
}

bool writeOperand(uintptr_t instruction, uintptr_t operand) {
    auto target=reinterpret_cast<void*>(va(instruction+operandOffset));
    DWORD previous;
    if(!VirtualProtect(target,operandLength,PAGE_EXECUTE_READWRITE,&previous))
        return false;
    *reinterpret_cast<uintptr_t*>(target)=operand;
    VirtualProtect(target,operandLength,previous,&previous);
    FlushInstructionCache(GetCurrentProcess(),target,operandLength);
    return true;
}

void restoreOperands(size_t count) {
    for(size_t i=0;i<count;++i)
        writeOperand(redirects[i].instruction,va(redirects[i].originalOperand));
}

// Points every redirected read at this Mod's value, or none of them.
bool redirectOperands() {
    for(size_t i=0;i<ARRAYSIZE(redirects);++i) {
        if(!writeOperand(redirects[i].instruction,replacementOperand(redirects[i]))) {
            char line[160];
            sprintf_s(line,"failed to change the operand at 0x%x (VirtualProtect); nothing changed",
                static_cast<unsigned>(redirects[i].instruction));
            log(line);
            restoreOperands(i);
            return false;
        }
    }
    return true;
}

const char* codeMismatch() {
    if(!frameLimiterCodeMatches())
        return "the frame limiter at 0x686bd0";
    for(auto& redirect:redirects)
        if(!redirectMatches(redirect))
            return "an instruction that reads a time constant";
    for(auto& count:updateCounts)
        if(!same(count.instruction,count.bytes,count.length) || !immediateFits(count,highUpdatesPerSecond))
            return "an instruction that holds a count of updates";
    if(!keyRepeatCodeMatches())
        return "the menu key repeat";
    if(!cameraCodeMatches())
        return "the camera manager";
    if(*reinterpret_cast<const uint64_t*>(va(stepSeconds))!=originalStepBits ||
        *reinterpret_cast<const uint32_t*>(va(managerStepSeconds))!=originalStepFloatBits ||
        *reinterpret_cast<const uint32_t*>(va(layoutStepSeconds))!=originalStepFloatBits)
        return "the 1/30 s step constants";
    return nullptr;
}

bool claimAll() {
    bool claimed=host->claim(self,va(frameLimiter),frameLimiterHookLength) &&
        host->claim(self,va(stepSeconds),sizeof(double)) &&
        host->claim(self,va(managerStepSeconds),sizeof(float)) &&
        host->claim(self,va(layoutStepSeconds),sizeof(float));
    for(auto& redirect:redirects)
        claimed=claimed&&host->claim(self,va(redirect.instruction+operandOffset),operandLength);
    for(auto& count:updateCounts)
        claimed=claimed&&host->claim(self,va(count.instruction+count.immediateOffset),count.immediateSize);
    return claimed;
}

bool hookFrameLimiter() {
    auto target=reinterpret_cast<void*>(va(frameLimiter));
    if(MH_CreateHook(target,reinterpret_cast<void*>(&frameLimiterHook),&originalFrameLimiter)!=MH_OK)
        return false;
    if(MH_EnableHook(target)!=MH_OK) {
        MH_RemoveHook(target);
        return false;
    }
    return true;
}

bool installed;

}  // namespace

extern "C" BOOL WINAPI ModInitialize(const wots4::ModContext* context) {
    if(!context||context->size<sizeof(*context)||context->version!=wots4::modApiVersion||!context->host||
        context->host->size<sizeof(wots4::HostApi)||!context->host->claim||!context->host->releaseClaims)
        return FALSE;
    if(installed)
        return TRUE;
    host=context->host;
    self=context->module;
    gameBase=reinterpret_cast<uintptr_t>(context->game);

    if(auto mismatch=codeMismatch()) {
        char line[200];
        sprintf_s(line,"%s is not the known version (Steam, PE time stamp 683d42eb); nothing changed",mismatch);
        log(line);
        return FALSE;
    }
    if(!claimAll()) {
        log("the frame limiter or a time constant is used by another Mod; nothing changed");
        host->releaseClaims(self);
        return FALSE;
    }
    for(auto& redirect:redirects)
        setRedirectValue(redirect,fps_uncap::gameUpdatesPerSecond);
    bool countsWritable=true;
    for(auto& count:updateCounts)
        countsWritable=countsWritable&&writeImmediate(count,immediateFor(count,fps_uncap::gameUpdatesPerSecond));
    if(!makeWritable(stepSeconds,sizeof(double))||!makeWritable(managerStepSeconds,sizeof(float))||!countsWritable) {
        log("failed to make the step constants or the update counts writable (VirtualProtect); nothing changed");
        host->releaseClaims(self);
        return FALSE;
    }
    if(!redirectOperands()) {
        host->releaseClaims(self);
        return FALSE;
    }
    if(MH_Initialize()!=MH_OK) {
        log("MinHook failed to initialize; nothing changed");
        restoreOperands(ARRAYSIZE(redirects));
        host->releaseClaims(self);
        return FALSE;
    }
    if(!hookFrameLimiter()) {
        log("failed to hook the frame limiter at 0x686bd0 (MinHook); nothing changed");
        MH_Uninitialize();
        restoreOperands(ARRAYSIZE(redirects));
        host->releaseClaims(self);
        return FALSE;
    }
    installed=true;
    char line[200];
    sprintf_s(line,"installed: %u updates per second outside scripts, 30 in scripts; "
        "%u reads redirected, %u update counts scaled",highUpdatesPerSecond,
        static_cast<unsigned>(ARRAYSIZE(redirects)),static_cast<unsigned>(ARRAYSIZE(updateCounts)));
    log(line);
    return TRUE;
}
