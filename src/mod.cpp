// fps_uncap: runs Way of the Samurai 4 at 30, 60, 90 or 120 logic updates (and frames)
// per second, events and cut-ins included, in memory only. The player picks the rate on
// an options page; it takes effect between two updates.
//
// The game waits for the next frame in its frame limiter (0x686bd0) and advances
// everything by one fixed step per frame. The step is shared constants: f64 at
// 0x99dca8 (game clock, timers, integration) and f32 at 0x97cdc4 and 0x9ddd64 (the
// dt passed to the managers, UI animation). This Mod writes 1/rate to them and
// replaces the limiter, which kept time in f32 seconds, with one on the
// performance counter.
//
// Some reads of 0x99dca8 turn the game's 30 fps frame counts (motion data,
// effects, script arguments) into seconds or back; they are pointed at this
// Mod's own 1/30 so they stay right. Amounts the game applies once per update
// (UI layout animations and screen fades advance one 30 fps frame per update)
// are pointed at values this Mod scales to the step, and counts of updates
// written into instructions are rewritten with it. The cameras take no dt at
// all, so their smoothing weights, rotation speeds and counts are handled the
// same way. Gameplay counts of updates (hit stop, action start invincibility,
// Rensatsu, AI timers) are scaled as immediates.
//
// Counts that come from data are scaled where they are used: the action script
// Timer interrupt (0x4185c0), the hang's seconds left (0x5a6fc0) and the torture
// press window (0x5a4cc0) are replaced, the script Wait start (0x590c20) rescales
// its count, and the event scene clock (0x58ded0) advances by a share of a 30 fps
// frame. Short pieces of code are replaced by jumps to this Mod's versions where
// a count or an amount goes through a register: action script command waits,
// balloon lives, the action end time-out, the water mill wheel, the physics kick
// threshold, the sound fade step, effect repeat intervals, the vitality charm and
// the motion of items and thrown weapons, and two counts compared as a sign-extended
// byte that do not fit one at 90 and 120.
#include <wots4/mod_api.h>
#include <MinHook.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include "settings.h"
#include "step_values.h"

namespace {

using fps_uncap::PerUpdate;

uintptr_t gameBase;
HMODULE self;
const wots4::HostApi* host;
fps_uncap::Settings settings;

uintptr_t va(uintptr_t address) { return gameBase+address-0x400000; }

// HostApi only grows at the end, so an older ModLoader still serves this Mod when it
// reaches onOptionsSaved, the last member used here.
bool hostHasUsedMembers(const wots4::HostApi* candidate) {
    return candidate->size>=offsetof(wots4::HostApi,onOptionsSaved)+sizeof(candidate->onOptionsSaved);
}

bool same(uintptr_t address, const unsigned char* bytes, size_t count) {
    return !memcmp(reinterpret_cast<const void*>(va(address)),bytes,count);
}

uintptr_t readGamePointer(uintptr_t address) { return *reinterpret_cast<const uintptr_t*>(va(address)); }

double readGameDouble(uintptr_t address) { return *reinterpret_cast<const double*>(va(address)); }

void log(const char* message) {
    if(host->log)
        host->log(self,message);
}

// The rate in use. The hooks read it for counts that come from data.
unsigned currentUpdatesPerSecond = fps_uncap::gameUpdatesPerSecond;

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
constexpr unsigned char fcomp[2] = {0xdc,0x1d};
constexpr unsigned char fcompFloat[2] = {0xd8,0x1d};

bool readsFloat(const unsigned char* opcode) { return opcode==fldFloat||opcode==fcompFloat; }

constexpr uintptr_t oneFrame = 0x99da28;  // f64 1.0, shared by 278 instructions
constexpr uintptr_t thirty = 0x99da78;    // f64 30.0, shared by 118 instructions
constexpr uintptr_t effectFrame = 0x99db90;  // f64 (double)(float)(1/30) * 30, an effect frame
constexpr double talkTurnDecay = 0.8999999761581421;  // k of the talk camera turn (0x445ad9)
constexpr double millInputDecay = 0.9950000047683716;  // k of the water mill stick input (0x5aaf4b)

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
    // HP and vitality gauges (MainGauge 0x5dcd80): after a change the shown values move by
    // the difference over 10 and over 30 updates (0x5da780)
    {0x5dcdf4,fld,0x99da50,10.0,PerUpdate::gain},
    {0x5dcdfa,fld,thirty,30.0,PerUpdate::gain},
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
    // Rensatsu kill slow motion (0x43e5f0): the player's speed returns by 1/8 per update
    {0x43e63d,fld,0x99d9e8,0.125,PerUpdate::amount},

    // Script durations in seconds turned into counts of updates (seconds * 30.0)
    // Script camera: joint lock, eased move, move (0x546b30), SetAnimRot
    {0x4481ba,fmul,thirty,30.0,PerUpdate::gain},
    {0x447e79,fmul,thirty,30.0,PerUpdate::gain},
    {0x447baa,fmul,thirty,30.0,PerUpdate::gain},
    {0x448204,fmul,thirty,30.0,PerUpdate::gain},
    // ButtonWait time-out (0x561648)
    {0x561648,fmul,thirty,30.0,PerUpdate::gain},
    // Face and mouth hold (SetFace, SetSelif, time table kind 29)
    {0x5506c7,fmul,thirty,30.0,PerUpdate::gain},
    // Event object, map part, physics object and character moves and rotations
    {0x55b2f9,fmul,thirty,30.0,PerUpdate::gain},
    {0x55b46b,fmul,thirty,30.0,PerUpdate::gain},
    {0x565e09,fmul,thirty,30.0,PerUpdate::gain},
    {0x565f89,fmul,thirty,30.0,PerUpdate::gain},
    {0x55d2a5,fmul,thirty,30.0,PerUpdate::gain},
    {0x54c44f,fmul,thirty,30.0,PerUpdate::gain},
    // Telop fades: lengths in updates and the alpha (out of 255) moved per update
    {0x58887f,fmul,thirty,30.0,PerUpdate::gain},
    {0x58892b,fmul,thirty,30.0,PerUpdate::gain},
    {0x5886a7,fadd,0x99da50,10.0,PerUpdate::amount},
    {0x588707,fsub,0x99da50,10.0,PerUpdate::amount},
    // Event battle and torture durations
    {0x545e9a,fmul,thirty,30.0,PerUpdate::gain},
    {0x56da31,fmul,thirty,30.0,PerUpdate::gain},
    {0x58c334,fmul,thirty,30.0,PerUpdate::gain},
    {0x56a8b2,fmul,thirty,30.0,PerUpdate::gain},

    // Torture commands: hang clear time, attacks, fire, stone throws, mill stop and speed change
    {0x56ac94,fmul,thirty,30.0,PerUpdate::gain},
    {0x56ad18,fmul,thirty,30.0,PerUpdate::gain},
    {0x56ade5,fmul,thirty,30.0,PerUpdate::gain},
    {0x56adfe,fmul,thirty,30.0,PerUpdate::gain},
    {0x56af70,fmul,thirty,30.0,PerUpdate::gain},
    {0x56aae6,fmul,thirty,30.0,PerUpdate::gain},
    {0x56aba8,fmul,thirty,30.0,PerUpdate::gain},
    // Torture, all kinds (0x5a4f30 and the end states): mashing penalty, end fade, gauge fill
    {0x5a5013,fsub,0x99dcc0,static_cast<double>(0.05f),PerUpdate::amount},
    {0x5a5247,fsub,0x99db08,static_cast<double>(1.0f/60.0f),PerUpdate::amount},
    {0x5a54e7,fadd,0x99daa8,2.0,PerUpdate::amount},
    // Water mill: speed smoothing, stick input, turn detection, life under water
    {0x5aaa02,fldFloat,0x97c028,static_cast<double>(0.08f),PerUpdate::weight},
    {0x5abc69,fldFloat,0x97c028,static_cast<double>(0.08f),PerUpdate::weight},
    {0x5abd4b,fldFloat,0x97c028,static_cast<double>(0.08f),PerUpdate::weight},
    {0x5aaf1c,fsub,0x99e7d8,0.001500000013038516,PerUpdate::amountUnderDecay,millInputDecay},
    {0x5aaf34,fadd,0x99e7d0,0.003000000026077032,PerUpdate::amountUnderDecay,millInputDecay},
    {0x5aaf4b,fmul,0x99e7c8,millInputDecay,PerUpdate::decay},
    {0x5aba0b,fldFloat,0x97bfc8,6.0,PerUpdate::amount},
    {0x5aba1f,fcompFloat,0x99484c,20.0,PerUpdate::amount},
    {0x5ab3bb,fsub,oneFrame,1.0,PerUpdate::amount},
    // Hang: rope power, height smoothing, life, fire scale, end state, node rise
    {0x5a8965,fsub,0x99dd40,0.6000000238418579,PerUpdate::amount},
    {0x5a89c5,fmul,0x99e640,0.07500000298023224,PerUpdate::weight},
    {0x5a89d1,fmul,0x99eb38,0.9249999970197678,PerUpdate::decay},
    {0x5a855a,fsub,0x99da98,0.5,PerUpdate::amount},
    {0x5a8682,fsub,0x99da98,0.5,PerUpdate::amount},
    {0x5a8a69,fmul,0x99eb30,0.02500000037252903,PerUpdate::weight},
    {0x5a8a75,fmul,0x99eb28,0.974999999627471,PerUpdate::decay},
    {0x5a8944,fadd,0x99e3a8,1.2000000476837158,PerUpdate::amount},
    {0x5a8b99,fadd,0x99e388,8.0,PerUpdate::amount},
    // Stone: pile height smoothing, thrown stone speed (3.0, toward 3.5) and fall
    {0x5aa02f,fmul,0x99db10,0.15000000596046448,PerUpdate::weight},
    {0x5aa03e,fmul,0x99dd30,0.8500000238418579,PerUpdate::decay},
    {0x5a64fd,fmul,0x99da40,0.10000000149011612,PerUpdate::weight},
    {0x5a6506,fmul,0x99e358,0.8999999761581421,PerUpdate::decay},
    {0x5a653a,fldFloat,0x99e7e0,2.933333396911621,PerUpdate::amount},
    {0x5a61c0,fldFloat,0x97c080,3.0,PerUpdate::amount},
    {0x5a64dd,fldFloat,0x97c080,3.0,PerUpdate::amount},
    {0x5a6659,fldFloat,0x97c080,3.0,PerUpdate::amount},
    {0x5a66e8,fldFloat,0x97c080,3.0,PerUpdate::amount},
    {0x5a6bbc,fldFloat,0x97c080,3.0,PerUpdate::amount},
    {0x5a6c82,fldFloat,0x97c080,3.0,PerUpdate::amount},
    {0x5a61ce,fldFloat,0x99daf8,3.5,PerUpdate::amount},
    {0x5a64e6,fldFloat,0x99daf8,3.5,PerUpdate::amount},
    {0x5a6665,fldFloat,0x99daf8,3.5,PerUpdate::amount},
    {0x5a66f5,fldFloat,0x99daf8,3.5,PerUpdate::amount},
    {0x5a6bcd,fldFloat,0x99daf8,3.5,PerUpdate::amount},
    {0x5a6c96,fldFloat,0x99daf8,3.5,PerUpdate::amount},

    // Effects: life in 30 fps frames (EffectListData) used up by speed * 1.0 each update (base
    // vt+0x14 0x4a4da0), effect tracks, blood, placed effects
    {0x4a4de6,fmul,effectFrame,1.0000000521540642,PerUpdate::amount},
    {0x4bb0c6,fld,effectFrame,1.0000000521540642,PerUpdate::amount},
    {0x4bb1f8,fld,effectFrame,1.0000000521540642,PerUpdate::amount},
    {0x4b75a0,fmul,effectFrame,1.0000000521540642,PerUpdate::amount},
    {0x4b9a78,fsub,oneFrame,1.0,PerUpdate::amount},
    // Effects: fade out (scale kept, vertex alpha moved) and track fade
    {0x4a4e19,fldFloat,0x99dbb4,static_cast<double>(0.95f),PerUpdate::decay},
    {0x4b9984,fldFloat,0x99dbb4,static_cast<double>(0.95f),PerUpdate::decay},
    {0x4a4e3f,fmul,0x99dcc8,static_cast<double>(0.03f),PerUpdate::amount},
    {0x4b99b0,fmul,0x99dcc8,static_cast<double>(0.03f),PerUpdate::amount},
    {0x4bb118,fldFloat,0x97dfc4,static_cast<double>(0.4f),PerUpdate::decay},
    {0x4bb26b,fldFloat,0x97dfc4,static_cast<double>(0.4f),PerUpdate::decay},
    // Rensatsu screen effect strength in and out
    {0x4a8a9f,fadd,0x99db80,static_cast<double>(0.04f),PerUpdate::amount},
    {0x4b8144,fsub,0x99db80,static_cast<double>(0.04f),PerUpdate::amount},

    // Items and dropped or thrown weapons (0x520a40): gravity added to the velocity each update
    // (normal and while kicked up, 0x51bb77), velocity damping, the thrown weapon's spin,
    // items pushing each other and the wall push, and the distance under which an item rests
    {0x5214ef,fldFloat,0x99dcf8,static_cast<double>(-0.08f),PerUpdate::amount},
    {0x4f6c36,fldFloat,0x99dcf8,static_cast<double>(-0.08f),PerUpdate::amount},
    {0x51bfc9,fldFloat,0x99dcf8,static_cast<double>(-0.08f),PerUpdate::amount},
    {0x51bb77,fldFloat,0x99dba8,-0.25,PerUpdate::amount},
    {0x5214f8,fldFloat,0x99e620,0.9699999690055847,PerUpdate::decay},  // One float below 0.97f
    {0x4f6c3f,fldFloat,0x99e620,0.9699999690055847,PerUpdate::decay},
    {0x521549,fldFloat,0x99e3e4,static_cast<double>(0.99f),PerUpdate::decay},
    {0x52158b,fldFloat,0x99e3e4,static_cast<double>(0.99f),PerUpdate::decay},
    {0x4f6c8f,fldFloat,0x99e3e4,static_cast<double>(0.99f),PerUpdate::decay},
    {0x521567,fldFloat,0x99e61c,static_cast<double>(0.98f),PerUpdate::decay},
    {0x521f3c,fldFloat,0x99e61c,static_cast<double>(0.98f),PerUpdate::decay},
    {0x52173b,fldFloat,0x99ed4c,-0.7155848145484924,PerUpdate::amount},
    {0x520e37,fld,0x99da98,0.5,PerUpdate::amount},
    {0x521b21,fld,0x99ed40,-0.05000000074505806,PerUpdate::amount},
    {0x520d07,fcomp,0x99e4f8,0.04999999701976776,PerUpdate::amount},

    // Physics kicks (0x52a040, on each touch): the velocity change pushed into a hit object
    {0x52a4b5,fld,0x99db58,-0.20000000298023224,PerUpdate::amount},
    {0x52a4d9,fldFloat,0x97dfc4,0.4000000059604645,PerUpdate::amount},
    {0x52a4e1,fldFloat,0x966c28,0.20000000298023224,PerUpdate::amount},
    // Physics kicks: how far a character moved in one update decides the kick (0x51e4f0)
    {0x51e597,fldFloat,0x99e67c,0.29999998211860657,PerUpdate::amount},
    {0x51e67a,fldFloat,0x99e678,0.14999999105930328,PerUpdate::amount},
    {0x51e812,fldFloat,0x99e67c,0.29999998211860657,PerUpdate::amount},
    {0x51ee6b,fldFloat,0x99e67c,0.29999998211860657,PerUpdate::amount},
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
// 16 bits (pairedHighCount) packs two u16 counts into one 32-bit immediate. An instruction that
// also holds an absolute address (relocated, so not compared as bytes) gives its offset. A count
// that holds for one update more than its value (holdsOneMore) is scaled by those holds.
struct UpdateCountImmediate {
    uintptr_t instruction;
    unsigned char bytes[10];  // The whole instruction as in the exe, immediate included
    uint32_t length;
    uint32_t immediateOffset;
    uint32_t immediateSize;
    uint32_t countAtGameRate;
    uint32_t pairedHighCount;
    uint32_t addressOffset;   // 0 when the instruction holds no absolute address
    uintptr_t address;
    bool holdsOneMore;
};

constexpr uint32_t repeatDelay = 11;    // Updates before a held direction repeats
constexpr uint32_t repeatInterval = 3;  // Updates between repeats
constexpr uint32_t largestSignedByte = 127;  // An imm8 of cmp is sign-extended
constexpr uint32_t largestUnsignedByte = 255;
constexpr uint32_t saveListRepeatDelay = 5;     // Updates before a held direction repeats, minus one
constexpr uint32_t saveListRepeatInterval = 1;  // Up and down: every 2 updates
constexpr uint32_t saveListSideInterval = 2;    // Left and right: every 3 updates

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
    // AI perception runs when the frame number % N equals the character's % N (0x40a7b0):
    // every 6 updates, every 15 in some game modes
    {0x40a7c4,{0xbf,0x06,0x00,0x00,0x00},5,1,4,6,0},
    {0x40a7e7,{0xbf,0x0f,0x00,0x00,0x00},5,1,4,15,0},
    // Hit stop of 2 updates: hits (0x487d7c, 0x49d638) and Rensatsu kills (0x43eb80)
    {0x487d7c,{0xb8,0x02,0x00,0x00,0x00},5,1,4,2,0},
    {0x49d638,{0xb9,0x02,0x00,0x00,0x00},5,1,4,2,0},
    {0x43eb80,{0xb9,0x02,0x00,0x00,0x00},5,1,4,2,0},
    // Invincibility for the first 4 updates of an action (0x44a0c9, read by the hit check 0x487334)
    {0x44a0c9,{0xc7,0x81,0xf0,0x00,0x00,0x00,0x04,0x00,0x00,0x00},10,6,4,4,0},
    // Rensatsu: kill slow motion over 8 updates (0x43e5f0), ending 30 and 10 updates (0x43d410).
    // The start wait (0x43cdce) is a code jump.
    {0x43e8c1,{0xc7,0x43,0x4c,0x08,0x00,0x00,0x00},7,3,4,8,0},
    {0x43e630,{0x83,0xf8,0x08},3,2,1,8,0},
    {0x43e638,{0xb9,0x08,0x00,0x00,0x00},5,1,4,8,0},
    {0x43d594,{0xc7,0x05,0xbc,0xe1,0x9d,0x00,0x1e,0x00,0x00,0x00},10,6,4,30,0,2,0x9de1bc},
    {0x43d59e,{0xc7,0x05,0xb8,0xe1,0x9d,0x00,0x0a,0x00,0x00,0x00},10,6,4,10,0,2,0x9de1b8},
    // AI: battle stance kept 15 updates (0x42b465), move arrival 3 and give-up 30 updates (0x42f540)
    {0x42b465,{0x83,0xbf,0x8c,0x00,0x00,0x00,0x0f},7,6,1,15,0},
    {0x42f611,{0x3c,0x03},2,1,1,3,0},
    {0x42f68a,{0x83,0x7f,0x78,0x1e},4,3,1,30,0},
    // Standing still 15 updates changes the neutral action ([+0x130])
    {0x448c16,{0x83,0xbe,0x30,0x01,0x00,0x00,0x0f},7,6,1,15,0},
    {0x4498b2,{0x83,0xb8,0x30,0x01,0x00,0x00,0x0f},7,6,1,15,0},
    {0x44a3cc,{0x83,0xbe,0x30,0x01,0x00,0x00,0x0f},7,6,1,15,0},
    // Event battle: waits 30 updates ([+0x444])
    {0x56d27f,{0x83,0xbe,0x44,0x04,0x00,0x00,0x1e},7,6,1,30,0},
    {0x56d384,{0x83,0xbe,0x44,0x04,0x00,0x00,0x1e},7,6,1,30,0},
    {0x56d3ae,{0x83,0xbe,0x44,0x04,0x00,0x00,0x1e},7,6,1,30,0},
    {0x56d58f,{0xc7,0x86,0x44,0x04,0x00,0x00,0x1e,0x00,0x00,0x00},10,6,4,30,0},
    // Scripts: the UI hides 3 updates after a script ends (0x5670e0) or SetEventMode(1) (0x58e42b),
    // and refreshes every 16 updates (0x5619e4)
    {0x5670e0,{0xc7,0x05,0x28,0x2e,0x9d,0x00,0x03,0x00,0x00,0x00},10,6,4,3,0,2,0x9d2e28},
    {0x58e42b,{0xc7,0x05,0x28,0x2e,0x9d,0x00,0x03,0x00,0x00,0x00},10,6,4,3,0,2,0x9d2e28},
    {0x5619e4,{0xc7,0x05,0x2c,0x2e,0x9d,0x00,0x0f,0x00,0x00,0x00},10,6,4,15,0,2,0x9d2e2c},
    // Action scripts: op165 counts down 60 updates (0x411de1); AI commands retry after 30
    {0x411de1,{0xc7,0x47,0x10,0x3c,0x00,0x00,0x00},7,3,4,60,0},
    {0x419147,{0x83,0x7e,0x30,0x1e},4,3,1,30,0},
    {0x4192b5,{0x83,0x7f,0x30,0x1e},4,3,1,30,0},
    {0x4193dd,{0x83,0x7f,0x30,0x1e},4,3,1,30,0},
    {0x4194cd,{0x83,0x7b,0x30,0x1e},4,3,1,30,0},
    {0x419618,{0x83,0x7b,0x30,0x1e},4,3,1,30,0},
    // Talk choices (0x4198e0, updated at 0x419a50): left alone for 150 updates, the talk takes
    // its time-out branch. The count goes down to 0 and times out on the next update
    {0x419950,{0xb8,0x96,0x00,0x00,0x00},5,1,4,150,0,0,0,true},
    // Balloons open over 6 updates (0x5b09c9)
    {0x5b0415,{0xc7,0x86,0x38,0x02,0x00,0x00,0x06,0x00,0x00,0x00},10,6,4,6,0},
    // Torture, all kinds: success state 30 updates, press window 7, voice every 15 + rand % 11,
    // a held button counts after 20 updates (0x4e6560)
    {0x5a5083,{0xc6,0x43,0x58,0x1e},4,3,1,30,0},
    {0x5a4b8a,{0xc6,0x46,0x57,0x07},4,3,1,7,0},
    {0x5a5344,{0xb9,0x0b,0x00,0x00,0x00},5,1,4,11,0},
    {0x5a534b,{0x80,0xc2,0x0f},3,2,1,15,0},
    {0x4e65d8,{0x83,0x3d,0x48,0x18,0x9f,0x00,0x14},7,6,1,20,0,2,0x9f1848},
    // Water mill: voice interval, press window 10 updates (the high byte of an imm16)
    {0x5ab35f,{0xb9,0x0b,0x00,0x00,0x00},5,1,4,11,0},
    {0x5ab366,{0x80,0xc2,0x0f},3,2,1,15,0},
    {0x5ab921,{0x66,0xc7,0x86,0xe5,0x00,0x00,0x00,0x0a,0x00},9,7,1,10,0},
    {0x5ab932,{0x66,0xc7,0x86,0xe4,0x00,0x00,0x00,0x01,0x0a},9,8,1,10,0},
    {0x5aa415,{0x66,0xc7,0x80,0xe4,0x00,0x00,0x00,0x00,0x0a},9,8,1,10,0},
    // Hang: end state cycle 60 and 30 (a byte compared unsigned, see unsignedBranches), time-out
    // 100 (its sound at 50 is a code jump), input blocks 20 and 30
    {0x5a8925,{0x80,0xbb,0xc4,0x00,0x00,0x00,0x3c},7,6,1,60,0},
    {0x5a8935,{0x80,0xbb,0xc4,0x00,0x00,0x00,0x1e},7,6,1,30,0},
    {0x5a7e67,{0xba,0x64,0x00,0x00,0x00},5,1,4,100,0},
    {0x5a850f,{0xc7,0x83,0x98,0x00,0x00,0x00,0x14,0x00,0x00,0x00},10,6,4,20,0},
    {0x5a8580,{0xc7,0x83,0xb0,0x00,0x00,0x00,0x1e,0x00,0x00,0x00},10,6,4,30,0},
    // Stone: level input lock 7 and 3, random level moves every 30 (first 40), landing 30, press 4.
    // The level move byte [+0x510] only counts down to 0 (0x5a9e0b), so 160 at 120 works unsigned.
    {0x5a9af9,{0xb2,0x07},2,1,1,7,0},
    {0x5a9b62,{0x80,0xf9,0x03},3,2,1,3,0},
    {0x5a9e63,{0xc6,0x86,0x10,0x05,0x00,0x00,0x1e},7,6,1,30,0},
    {0x5a8e7f,{0xc6,0x87,0x10,0x05,0x00,0x00,0x28},7,6,1,40,0},
    {0x5a67fc,{0x66,0x83,0x7e,0x30,0x1e},5,4,1,30,0},
    {0x5a688b,{0x83,0xf9,0x04},3,2,1,4,0},
    // Effects: fade out over 30 updates ([+0x19]; 0x4b819d's kind does not use it), a 5-update wait
    {0x4a4a83,{0xc6,0x46,0x19,0x1e},4,3,1,30,0},
    {0x4a9cad,{0xc6,0x40,0x19,0x1e},4,3,1,30,0},
    {0x4b7182,{0xc7,0x43,0x19,0x1e,0x01,0x00,0x00},7,3,1,30,0},
    {0x4b734f,{0xc7,0x43,0x19,0x1e,0x01,0x00,0x00},7,3,1,30,0},
    {0x4b8e1d,{0xc7,0x46,0x19,0x1e,0x01,0x00,0x00},7,3,1,30,0},
    {0x4b9c40,{0xc7,0x46,0x19,0x1e,0x01,0x00,0x00},7,3,1,30,0},
    {0x4ba4af,{0xc7,0x46,0x19,0x1e,0x01,0x00,0x00},7,3,1,30,0},
    {0x4bbe20,{0xc7,0x46,0x19,0x1e,0x01,0x00,0x00},7,3,1,30,0},
    {0x4a7e50,{0xb9,0x05,0x00,0x00,0x00},5,1,4,5,0},
    // Items: kicked up for 12 updates (0x51bb4b), rest after 5 still updates (0x520d32), checked
    // again every 5 while resting (0x520a60), not picked up for 5 after appearing (0x521608)
    {0x51bb4b,{0xc7,0x46,0x48,0x0c,0x00,0x00,0x00},7,3,4,12,0},
    {0x520d32,{0x83,0x7e,0x40,0x05},4,3,1,5,0},
    {0x520a60,{0xb9,0x05,0x00,0x00,0x00},5,1,4,5,0},
    {0x521608,{0xc7,0x47,0x18,0x05,0x00,0x00,0x00},7,3,4,5,0},
    // Physics objects: hit sound intervals 20 and 6, invincible for 30 after appearing
    {0x52ff6b,{0xc7,0x46,0x20,0x14,0x00,0x00,0x00},7,3,4,20,0},
    {0x5322df,{0xc7,0x47,0x20,0x06,0x00,0x00,0x00},7,3,4,6,0},
    {0x53238e,{0xc7,0x47,0x20,0x06,0x00,0x00,0x00},7,3,4,6,0},
    {0x52ffad,{0xc7,0x86,0x84,0x00,0x00,0x00,0x1e,0x00,0x00,0x00},10,6,4,30,0},
    // Save data list (SaveDataWin32Manager [0x9e96d4] +8, made at 0x53ebe0): its own key repeat
    // (0x5400b0, fires every n + 1 updates) for up, down, left and right: first after 5, then every
    // 1 or 2
    {0x53ec20,{0xc7,0x46,0x14,0x05,0x00,0x01,0x00},7,3,4,saveListRepeatDelay,saveListRepeatInterval,0,0,true},
    {0x53ec35,{0xc7,0x46,0x24,0x05,0x00,0x01,0x00},7,3,4,saveListRepeatDelay,saveListRepeatInterval,0,0,true},
    {0x53ec4a,{0xc7,0x46,0x34,0x05,0x00,0x02,0x00},7,3,4,saveListRepeatDelay,saveListSideInterval,0,0,true},
    {0x53ec63,{0xc7,0x46,0x44,0x05,0x00,0x02,0x00},7,3,4,saveListRepeatDelay,saveListSideInterval,0,0,true},
};

uint32_t scaledCount(const UpdateCountImmediate& count, uint32_t countAtGameRate, unsigned updatesPerSecond) {
    if(count.holdsOneMore)
        return static_cast<uint32_t>(fps_uncap::scaledHoldCount(static_cast<int>(countAtGameRate),updatesPerSecond));
    return fps_uncap::scaledUpdateCount(countAtGameRate,updatesPerSecond);
}

uint32_t immediateFor(const UpdateCountImmediate& count, unsigned updatesPerSecond) {
    uint32_t low=scaledCount(count,count.countAtGameRate,updatesPerSecond);
    if(!count.pairedHighCount)
        return low;
    return low|scaledCount(count,count.pairedHighCount,updatesPerSecond)<<16;
}

bool updateCountMatches(const UpdateCountImmediate& count) {
    if(!count.addressOffset)
        return same(count.instruction,count.bytes,count.length);
    const uint32_t afterAddress=count.addressOffset+sizeof(uint32_t);
    return same(count.instruction,count.bytes,count.addressOffset) &&
        readGamePointer(count.instruction+count.addressOffset)==va(count.address) &&
        same(count.instruction+afterAddress,count.bytes+afterAddress,count.length-afterAddress);
}

// cmp r/m32 (or r/m16), imm8 sign-extends the byte; the other 1-byte immediates are byte operands
// (mov, cmp or add on a byte), whose fields are read as signed only where they stay below 128.
bool signExtendsImmediate(const UpdateCountImmediate& count) {
    constexpr unsigned char compareSignExtended = 0x83;
    constexpr unsigned char operandSize16 = 0x66;
    return count.bytes[0]==compareSignExtended ||
        (count.bytes[0]==operandSize16 && count.bytes[1]==compareSignExtended);
}

bool immediateFits(const UpdateCountImmediate& count, unsigned updatesPerSecond) {
    if(count.immediateSize!=1)
        return true;
    const uint32_t largest=signExtendsImmediate(count) ? largestSignedByte : largestUnsignedByte;
    return immediateFor(count,updatesPerSecond)<=largest;
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

// The live key repeat of the save data list, when it already exists.
constexpr uintptr_t saveDataManager = 0x9e96d4;  // SaveDataWin32Manager*, set at 0x53e2ff
constexpr uint32_t saveDataManagerList = 0x8;     // The list, set at 0x53e4cc
constexpr uint32_t saveListRepeats[] = {0x10,0x20,0x30,0x40};  // Up, down, left, right
constexpr uint32_t saveListRepeatDelayOffset = 4;     // u16, read at 0x5400d5
constexpr uint32_t saveListRepeatIntervalOffset = 6;  // u16, read at 0x5400dc

bool saveListCodeMatches() {
    const unsigned char storeManager[] = {0xa3};                 // mov [0x9e96d4],eax
    const unsigned char storeList[] = {0x89,0x46,0x08};          // mov [esi+8],eax
    const unsigned char readDelay[] = {0x0f,0xb7,0x48,0x04};     // movzx ecx,word [eax+4]
    const unsigned char readInterval[] = {0x0f,0xb7,0x48,0x06};  // movzx ecx,word [eax+6]
    return same(0x53e2ff,storeManager,sizeof(storeManager)) && readGamePointer(0x53e300)==va(saveDataManager) &&
        same(0x53e4cc,storeList,sizeof(storeList)) && same(0x5400d5,readDelay,sizeof(readDelay)) &&
        same(0x5400dc,readInterval,sizeof(readInterval));
}

void writeLiveSaveListRepeat(unsigned updatesPerSecond) {
    auto manager=readGamePointer(saveDataManager);
    if(!manager)
        return;
    auto list=*reinterpret_cast<const uintptr_t*>(manager+saveDataManagerList);
    if(!list)
        return;
    const auto delay=static_cast<uint16_t>(fps_uncap::scaledHoldCount(saveListRepeatDelay,updatesPerSecond));
    for(size_t i=0;i<ARRAYSIZE(saveListRepeats);++i) {
        const bool upOrDown=i<2;
        const auto interval=static_cast<uint16_t>(fps_uncap::scaledHoldCount(
            upOrDown ? saveListRepeatInterval : saveListSideInterval,updatesPerSecond));
        *reinterpret_cast<uint16_t*>(list+saveListRepeats[i]+saveListRepeatDelayOffset)=delay;
        *reinterpret_cast<uint16_t*>(list+saveListRepeats[i]+saveListRepeatIntervalOffset)=interval;
    }
}

// ---- ActionScript timer interrupt

// The AI and NPC action scripts' Timer interrupt (0x4185c0, vtable 0x97f220): counts one per
// update in [+0x14] and jumps to [+8] once it reaches [+0x10], a 30 fps frame count that
// Jump_Timer draws from the script's minFrame..maxFrame. The count comes from data, so the
// whole check is replaced to compare with the count at this Mod's rate.
constexpr uintptr_t actionScriptTimer = 0x4185c0;
constexpr uint32_t actionScriptTimerHookLength = 6;  // mov eax,[ecx+0x14]; cmp eax,[ecx+0x10]
constexpr uint32_t timerJumpTarget = 0x8;
constexpr uint32_t timerWaitFrames = 0x10;
constexpr uint32_t timerElapsedUpdates = 0x14;
constexpr int32_t timerKeepRunning = -1;

bool actionScriptTimerCodeMatches() {
    // mov eax,[ecx+0x14]; cmp eax,[ecx+0x10]; jl; mov eax,[ecx+8]; ret 8; inc eax;
    // mov [ecx+0x14],eax; or eax,-1; ret 8
    const unsigned char timer[] = {0x8b,0x41,0x14,0x3b,0x41,0x10,0x7c,0x06,0x8b,0x41,0x08,0xc2,0x08,0x00,
        0x40,0x89,0x41,0x14,0x83,0xc8,0xff,0xc2,0x08,0x00};
    return same(actionScriptTimer,timer,sizeof(timer)) &&
        *reinterpret_cast<const uintptr_t*>(va(0x97f220))==va(actionScriptTimer);
}

// thiscall with two stack arguments and ret 8: fastcall with an unused edx has the same layout.
// Runs for every waiting timer of every scripted character each update: arithmetic only.
int32_t __fastcall actionScriptTimerHook(uint8_t* timer, void*, uint32_t, uint32_t) {
    auto& elapsed=*reinterpret_cast<int32_t*>(timer+timerElapsedUpdates);
    const int32_t waitFrames=*reinterpret_cast<const int32_t*>(timer+timerWaitFrames);
    if(elapsed>=fps_uncap::scaledHoldCount(waitFrames,currentUpdatesPerSecond))
        return *reinterpret_cast<const int32_t*>(timer+timerJumpTarget);
    ++elapsed;
    return timerKeepRunning;
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

// The game's wait (CFPSTimer, 0x686bd0, called only by the main loop at 0x4dd4fe) runs an update
// once the last update time + 0x99dca8 has passed. It keeps the times in f32 seconds since the
// first call, which after a few hours of play are only a few milliseconds fine and round every
// 1/60 or 1/120 step. This Mod's wait does the same on the performance counter.
constexpr uintptr_t frameLimiter = 0x686bd0;
constexpr uint32_t frameLimiterHookLength = 6;  // push ebp; mov ebp,esp; sub esp,8
constexpr uint32_t timerGameSeconds = 0x8;      // f32, read by scripts and others
constexpr uint32_t timerGameTimeRuns = 0xc;     // byte, cleared by menus
constexpr int64_t gameSpinMilliseconds = 30;    // The game sleeps only what is left beyond 30 ms

bool frameLimiterCodeMatches() {
    const unsigned char prologue[] = {0x55,0x8b,0xec,0x83,0xec,0x08};
    const unsigned char readStep[] = {0xdd,0x05};  // fld qword [0x99dca8]: next time = last + step
    const unsigned char callLimiter[] = {0xe8};
    const auto callNext=va(0x4dd503);
    return same(frameLimiter,prologue,sizeof(prologue)) && same(0x686be4,readStep,sizeof(readStep)) &&
        readGamePointer(0x686be6)==va(stepSeconds) && same(0x4dd4fe,callLimiter,sizeof(callLimiter)) &&
        callNext+*reinterpret_cast<const int32_t*>(va(0x4dd4ff))==va(frameLimiter);
}

int64_t countsPerSecond;  // QueryPerformanceFrequency, set at installation
int64_t countsPerUpdate;
double gameStepSeconds;   // What the game adds to its clock per update, the value in 0x99dca8
int64_t lastUpdateCount;
int64_t lastRealignCount;

int64_t performanceCount() {
    LARGE_INTEGER count;
    QueryPerformanceCounter(&count);
    return count.QuadPart;
}

// The game sleeps only what is left beyond 30 ms and spins the rest (Sleep may oversleep by a
// timer tick); at 30 updates per second and above that is at most 3 ms, as in the game.
void sleepWhileFarFromDue(int64_t countsLeft) {
    const int64_t millisecondsToSleep=countsLeft*1000/countsPerSecond-gameSpinMilliseconds;
    if(millisecondsToSleep>0)
        Sleep(static_cast<DWORD>(millisecondsToSleep));
}

// Called by the main loop thousands of times per update while it waits: arithmetic and QPC only.
// force runs an update now. Like the game, a late update runs right away and the next ones catch
// up step by step, but about every second the schedule is moved to now, so at most a second
// is caught up.
bool __cdecl updateIsDue(uint8_t* timer, uint32_t force) {
    constexpr int64_t realignAfterMilliseconds = 999;  // 0x99dca0
    if(!force) {
        const int64_t now=performanceCount();
        const int64_t due=lastUpdateCount+countsPerUpdate;
        if(now<due) {
            sleepWhileFarFromDue(due-now);
            return false;
        }
        if((now-lastRealignCount)*1000>=realignAfterMilliseconds*countsPerSecond)
            lastUpdateCount=lastRealignCount=now;
        else
            lastUpdateCount=due;
    }
    // The game also sets the clock to 0 when it equals DBL_MAX (0x99dc98), which a float never does.
    if(timer[timerGameTimeRuns]) {
        auto& gameSeconds=*reinterpret_cast<float*>(timer+timerGameSeconds);
        gameSeconds=static_cast<float>(gameSeconds+gameStepSeconds);
    }
    return true;
}

// The game's call: esi = the timer, [esp+4] = force (a byte), ret 4, al = 1 when an update is due.
__declspec(naked) void frameLimiterHook() {
    __asm {
        movzx eax, byte ptr [esp+4]
        push eax
        push esi
        call updateIsDue
        add esp, 8
        ret 4
    }
}

// ---- Event scenes, script waits and sound fades

// What advances one 30 fps frame per update now advances this share of one.
double thirtyFpsFramesPerUpdate;

// Event scene clock (0x58ded0): [esp+4] is the f32 count of 30 fps frames to advance, 1.0 from
// both callers (PlayScene 0x560bff, PlaySceneEmpty 0x560fd5); eax is the scene.
constexpr uintptr_t advanceScene = 0x58ded0;
constexpr uint32_t advanceSceneHookLength = 6;  // push ebp; mov ebp,esp; fld dword [ebp+8]
void* originalAdvanceScene;

bool advanceSceneCodeMatches() {
    const unsigned char prologue[] = {0x55,0x8b,0xec,0xd9,0x45,0x08};
    const unsigned char playSceneStep[] = {0xd9,0xe8};  // fld1 before the call
    return same(advanceScene,prologue,sizeof(prologue)) && same(0x560bf6,playSceneStep,sizeof(playSceneStep)) &&
        same(0x560fcc,playSceneStep,sizeof(playSceneStep));
}

// Once per playing scene each update.
__declspec(naked) void advanceSceneHook() {
    __asm {
        fld dword ptr [esp+4]
        fmul qword ptr [thirtyFpsFramesPerUpdate]
        fstp dword ptr [esp+4]
        jmp dword ptr [originalAdvanceScene]
    }
}

// Script Wait start (0x590c20, the inner vtable 0x98e66c +0x10): thiscall, two arguments, ret 8.
// It leaves [+0xc] = n, a 30 fps frame count (seconds * 30 at 0x590d6d, or frames); the Wait then
// holds the script for n + 1 updates (0x590db0), so the count of holds is what gets scaled.
constexpr uintptr_t scriptWaitStart = 0x590c20;
constexpr uint32_t scriptWaitStartHookLength = 6;  // push ebp; mov ebp,esp; push ecx; push ebx; push esi
constexpr uint32_t scriptWaitFrames = 0xc;

using ScriptWaitStart = uint32_t (__fastcall*)(uint8_t* wait, void* unused, uint32_t, uint32_t);
ScriptWaitStart originalScriptWaitStart;

bool scriptWaitCodeMatches() {
    const unsigned char prologue[] = {0x55,0x8b,0xec,0x51,0x53,0x56};
    // mov eax,[ecx+0xc]; lea edx,[eax-1]; test eax,eax; mov [ecx+0xc],edx
    const unsigned char countDown[] = {0x8b,0x41,0x0c,0x8d,0x50,0xff,0x85,0xc0,0x89,0x51,0x0c};
    return same(scriptWaitStart,prologue,sizeof(prologue)) && same(0x590db0,countDown,sizeof(countDown)) &&
        *reinterpret_cast<const uintptr_t*>(va(0x98e66c+0x10))==va(scriptWaitStart);
}

// Once when a script starts a Wait.
uint32_t __fastcall scriptWaitStartHook(uint8_t* wait, void* unused, uint32_t first, uint32_t second) {
    uint32_t result=originalScriptWaitStart(wait,unused,first,second);
    auto& frames=*reinterpret_cast<int32_t*>(wait+scriptWaitFrames);
    frames=fps_uncap::scaledHoldCount(frames,currentUpdatesPerSecond);
    return result;
}

// ---- Short pieces of code replaced by a jump to this Mod's version

// Where the game keeps a count or an amount in a register on its way to memory, the
// instructions are replaced by a jmp to a stub that does the same with the scaled value and
// jumps back. Nothing jumps into the middle of a replaced range.

int __cdecl commandHoldFor(int frames) {  // 0 means "next command now" and is kept
    return frames>0 ? fps_uncap::scaledHoldCount(frames,currentUpdatesPerSecond) : frames;
}

int __cdecl holdFor(int frames) {
    return fps_uncap::scaledHoldCount(frames,currentUpdatesPerSecond);
}

// The jmp stubs return here, set at installation.
uintptr_t commandWaitResume, balloonLifetimeResume, actionEndKeepWaiting, actionEndTimedOut,
    millStepResume, kickThresholdResume, soundFadeResume, bloodDropResume, placedEffectResume,
    placedEffectRestartResume, charmVitalityResume, itemMoveResume, rensatsuStartKeepWaiting,
    rensatsuStartWaited, hangTimeoutSoundNotYet, hangTimeoutSoundNow;
constexpr uint32_t actionEndTimeoutAtGameRate = 90;
int32_t actionEndTimeoutUpdates;
// Compared as a sign-extended imm8 by the game, so 180 and 240 (at 90 and 120) need a stub.
constexpr uint32_t rensatsuStartWaitAtGameRate = 60;
constexpr uint32_t hangTimeoutSoundAtGameRate = 50;
int32_t rensatsuStartWaitUpdates;
int32_t hangTimeoutSoundUpdates;

// Action script command wait (0x40b8ff): the handler returned a count of 30 fps frames in eax;
// the script stays on the command until [+0x218] passes it (0x40b970). Once per command.
__declspec(naked) void commandWaitStub() {
    __asm {
        push eax
        call commandHoldFor
        add esp, 4
        mov dword ptr [esi+0x214], eax
        jmp dword ptr [commandWaitResume]
    }
}

// Balloon lifetime (0x5b088d): [+0x234] counts down each update (0x5b0a02). Once per balloon;
// ecx is read after the store.
__declspec(naked) void balloonLifetimeStub() {
    __asm {
        push ecx
        push edx
        push eax
        call holdFor
        add esp, 4
        pop edx
        pop ecx
        mov dword ptr [edi+0x234], eax
        jmp dword ptr [balloonLifetimeResume]
    }
}

// Vitality regeneration charm (0x484c2e, in 0x4848f0): the charm's level adds a whole number
// [ebp-8] to the vitality [ebp-4] each update while standing; only the share of it is added.
__declspec(naked) void charmVitalityStub() {
    __asm {
        fild dword ptr [ebp-8]
        fmul qword ptr [thirtyFpsFramesPerUpdate]
        fadd dword ptr [ebp-4]
        fstp dword ptr [ebp-4]
        jmp dword ptr [charmVitalityResume]
    }
}

// Item motion (0x520ab6, in 0x520a40, once per item each update): items, dropped and thrown
// weapons are the game's own objects, not PhysX actors; their velocity [edi] is in units per
// 30 fps frame and is added to the position [esi+8] each update. Only the share is added.
__declspec(naked) void itemMoveStub() {
    __asm {
        fld dword ptr [edi]
        fmul qword ptr [thirtyFpsFramesPerUpdate]
        fadd dword ptr [esi+8]
        fstp dword ptr [esi+8]
        fld dword ptr [edi+4]
        fmul qword ptr [thirtyFpsFramesPerUpdate]
        fadd dword ptr [esi+0xc]
        fstp dword ptr [esi+0xc]
        fld dword ptr [edi+8]
        fmul qword ptr [thirtyFpsFramesPerUpdate]
        fadd dword ptr [esi+0x10]
        fstp dword ptr [esi+0x10]
        jmp dword ptr [itemMoveResume]
    }
}

// Blood drops (EffectBlood, 0x4b7876): the next drop comes after [+0x2c] updates.
__declspec(naked) void bloodDropStub() {
    __asm {
        dec dword ptr [esi+0x30]
        pop edi
        pop ebx
        push ecx
        push edx
        push eax
        call holdFor
        add esp, 4
        pop edx
        pop ecx
        mov dword ptr [esi+0x2c], eax
        jmp dword ptr [bloodDropResume]
    }
}

// Placed effects repeat after [+0x28] = interval [+0x20] + a random part (edx, read after) updates:
// at their start (0x4b98bc, edi = effect) and when they start again (0x4b9abc, esi = effect).
__declspec(naked) void placedEffectStub() {
    __asm {
        mov eax, dword ptr [edi+0x20]
        add eax, edx
        push ecx
        push edx
        push eax
        call holdFor
        add esp, 4
        pop edx
        pop ecx
        mov dword ptr [edi+0x28], eax
        jmp dword ptr [placedEffectResume]
    }
}

__declspec(naked) void placedEffectRestartStub() {
    __asm {
        mov eax, dword ptr [esi+0x20]
        add eax, edx
        push ecx
        push edx
        push eax
        call holdFor
        add esp, 4
        pop edx
        pop ecx
        mov dword ptr [esi+0x28], eax
        jmp dword ptr [placedEffectRestartResume]
    }
}

// Waiting for an action to end gives up after 90 updates (0x41974d); 180 does not fit the imm8.
__declspec(naked) void actionEndTimeoutStub() {
    __asm {
        push eax
        mov eax, actionEndTimeoutUpdates
        cmp dword ptr [edi+0x24], eax
        pop eax
        jle keepWaiting
        jmp dword ptr [actionEndTimedOut]
    keepWaiting:
        jmp dword ptr [actionEndKeepWaiting]
    }
}

// Rensatsu start (0x43cdce): with no script system it waits while [ebx+0x24] <= 60 updates.
__declspec(naked) void rensatsuStartWaitStub() {
    __asm {
        push eax
        mov eax, rensatsuStartWaitUpdates
        cmp dword ptr [ebx+0x24], eax
        pop eax
        jle keepWaiting
        jmp dword ptr [rensatsuStartWaited]
    keepWaiting:
        jmp dword ptr [rensatsuStartKeepWaiting]
    }
}

// Hang time-out (0x5a7e8f, every update): the sound plays when the word [esi+0xb6] reaches 50.
__declspec(naked) void hangTimeoutSoundStub() {
    __asm {
        push eax
        movsx eax, word ptr [esi+0xb6]
        cmp eax, hangTimeoutSoundUpdates
        pop eax
        jne notYet
        jmp dword ptr [hangTimeoutSoundNow]
    notYet:
        jmp dword ptr [hangTimeoutSoundNotYet]
    }
}

// Water mill wheel (0x5ab012, every update): the animation time [+0x84] moves back by the
// speed, kept on the x87 stack and used again after; the moved share of it is what is kept.
__declspec(naked) void millStepStub() {
    __asm {
        fst dword ptr [edi+0x90]
        fmul qword ptr [thirtyFpsFramesPerUpdate]
        fld dword ptr [edi+0x84]
        jmp dword ptr [millStepResume]
    }
}

// Physics kick (0x51e664): a strong kick when a character moved at least 1.0 in one update.
__declspec(naked) void kickThresholdStub() {
    __asm {
        fld qword ptr [thirtyFpsFramesPerUpdate]
        fld dword ptr [ebp-0x1c]
        jmp dword ptr [kickThresholdResume]
    }
}

// Sound fade step (0x5a06c3, in the sound update 0x5a0640): frames [ebx+0x3c] -= 1.0 and
// volume [ebx+0x6c] += [ebx+0x40] (the difference over the frames). Every fade of BGM, sound
// effects and SetVolume passes here: both move by the share. Once per fading sound each update.
__declspec(naked) void soundFadeStub() {
    __asm {
        fld dword ptr [ebx+0x3c]
        fsub qword ptr [thirtyFpsFramesPerUpdate]
        fstp dword ptr [ebx+0x3c]
        fld dword ptr [ebx+0x40]
        fmul qword ptr [thirtyFpsFramesPerUpdate]
        fadd dword ptr [ebx+0x6c]
        jmp dword ptr [soundFadeResume]
    }
}

constexpr uint32_t jumpLength = 5;

struct CodeJump {
    uintptr_t site;
    unsigned char bytes[26];  // The replaced instructions as in the exe
    uint32_t length;
    void (*stub)();
    uintptr_t* resume;
    uintptr_t resumeAt;       // The instruction after the replaced range, or where the stub goes
    uintptr_t* otherResume;   // A second way back, or nullptr
    uintptr_t otherResumeAt;
    uint32_t addressOffset;   // 0 when the replaced bytes hold no absolute address
    uintptr_t address;
    unsigned char saved[jumpLength];
};

CodeJump codeJumps[] = {
    {0x40b8ff,{0x89,0x86,0x14,0x02,0x00,0x00},6,commandWaitStub,&commandWaitResume,0x40b905},
    {0x5b088d,{0x89,0x87,0x34,0x02,0x00,0x00},6,balloonLifetimeStub,&balloonLifetimeResume,0x5b0893},
    // cmp dword [edi+0x24],0x5a; jle 0x41975d
    {0x41974d,{0x83,0x7f,0x24,0x5a,0x7e,0x0a},6,actionEndTimeoutStub,&actionEndTimedOut,0x419753,
        &actionEndKeepWaiting,0x41975d},
    // fst [edi+0x90]; fld [edi+0x84]
    {0x5ab012,{0xd9,0x97,0x90,0x00,0x00,0x00,0xd9,0x87,0x84,0x00,0x00,0x00},12,millStepStub,
        &millStepResume,0x5ab01e},
    // fld1; fld [ebp-0x1c]
    {0x51e664,{0xd9,0xe8,0xd9,0x45,0xe4},5,kickThresholdStub,&kickThresholdResume,0x51e669},
    // fld [ebx+0x3c]; fsub qword [0x99da28]; fstp [ebx+0x3c]; fld [ebx+0x40]; fadd [ebx+0x6c]
    {0x5a06c3,{0xd9,0x43,0x3c,0xdc,0x25,0x28,0xda,0x99,0x00,0xd9,0x5b,0x3c,0xd9,0x43,0x40,0xd8,0x43,0x6c},
        18,soundFadeStub,&soundFadeResume,0x5a06d5,nullptr,0,5,oneFrame},
    // dec [esi+0x30]; pop edi; pop ebx; mov [esi+0x2c],eax
    {0x4b7876,{0xff,0x4e,0x30,0x5f,0x5b,0x89,0x46,0x2c},8,bloodDropStub,&bloodDropResume,0x4b787e},
    // mov eax,[edi+0x20]; add eax,edx; mov [edi+0x28],eax (and the same on esi)
    {0x4b98bc,{0x8b,0x47,0x20,0x03,0xc2,0x89,0x47,0x28},8,placedEffectStub,&placedEffectResume,0x4b98c4},
    {0x4b9abc,{0x8b,0x46,0x20,0x03,0xc2,0x89,0x46,0x28},8,placedEffectRestartStub,&placedEffectRestartResume,
        0x4b9ac4},
    // fild [ebp-8]; fadd [ebp-4]; fstp [ebp-4]
    {0x484c2e,{0xdb,0x45,0xf8,0xd8,0x45,0xfc,0xd9,0x5d,0xfc},9,charmVitalityStub,&charmVitalityResume,0x484c37},
    // fld [esi+8]; fadd [edi]; fstp [esi+8], and the same for +4 / +0xc and +8 / +0x10
    {0x520ab6,{0xd9,0x46,0x08,0xd8,0x07,0xd9,0x5e,0x08,0xd9,0x47,0x04,0xd8,0x46,0x0c,0xd9,0x5e,0x0c,0xd9,0x47,
        0x08,0xd8,0x46,0x10,0xd9,0x5e,0x10},26,itemMoveStub,&itemMoveResume,0x520ad0},
    // cmp dword [ebx+0x24],0x3c; jle 0x43cdf7
    {0x43cdce,{0x83,0x7b,0x24,0x3c,0x7e,0x23},6,rensatsuStartWaitStub,&rensatsuStartWaited,0x43cdd4,
        &rensatsuStartKeepWaiting,0x43cdf7},
    // cmp word [esi+0xb6],0x32; jne 0x5a7eb2
    {0x5a7e8f,{0x66,0x83,0xbe,0xb6,0x00,0x00,0x00,0x32,0x75,0x19},10,hangTimeoutSoundStub,&hangTimeoutSoundNow,
        0x5a7e99,&hangTimeoutSoundNotYet,0x5a7eb2},
};

// ---- Branches made unsigned

// The hang end state cycle (0x5a8910) counts a byte [ebx+0xc4] up to 60 and back to 0, and acts
// while it is below 30. At 90 and 120 the counts pass 127, which the game's signed jge reads as
// negative; unsigned branches give the same result at every rate. Restarting at "reached" instead
// of "equal" also keeps a count left above a lower rate's cycle from running past it.
struct BranchOpcode {
    uintptr_t site;
    unsigned char original;
    unsigned char replacement;
};

const BranchOpcode unsignedBranches[] = {
    {0x5a892c,0x75,0x72},  // jne -> jb: restart the cycle once it reached its length
    {0x5a893c,0x7d,0x73},  // jge -> jae: the second half of the cycle
};

bool unsignedBranchesMatch() {
    for(auto& branch:unsignedBranches)
        if(!same(branch.site,&branch.original,1))
            return false;
    return true;
}

bool codeJumpMatches(const CodeJump& jump) {
    if(!jump.addressOffset)
        return same(jump.site,jump.bytes,jump.length);
    const uint32_t afterAddress=jump.addressOffset+sizeof(uint32_t);
    return same(jump.site,jump.bytes,jump.addressOffset) &&
        readGamePointer(jump.site+jump.addressOffset)==va(jump.address) &&
        same(jump.site+afterAddress,jump.bytes+afterAddress,jump.length-afterAddress);
}

bool writeCode(uintptr_t address, const void* bytes, uint32_t length) {
    auto target=reinterpret_cast<void*>(va(address));
    DWORD previous;
    if(!VirtualProtect(target,length,PAGE_EXECUTE_READWRITE,&previous))
        return false;
    memcpy(target,bytes,length);
    VirtualProtect(target,length,previous,&previous);
    FlushInstructionCache(GetCurrentProcess(),target,length);
    return true;
}

void restoreCodeJumps(size_t count) {
    for(size_t i=0;i<count;++i)
        writeCode(codeJumps[i].site,codeJumps[i].saved,jumpLength);
}

// Writes every jump, or none of them.
bool writeCodeJumps() {
    for(size_t i=0;i<ARRAYSIZE(codeJumps);++i) {
        auto& jump=codeJumps[i];
        *jump.resume=va(jump.resumeAt);
        if(jump.otherResume)
            *jump.otherResume=va(jump.otherResumeAt);
        memcpy(jump.saved,reinterpret_cast<const void*>(va(jump.site)),jumpLength);
        unsigned char code[jumpLength]={0xe9};
        const auto next=va(jump.site)+jumpLength;
        *reinterpret_cast<int32_t*>(code+1)=static_cast<int32_t>(reinterpret_cast<uintptr_t>(jump.stub)-next);
        if(!writeCode(jump.site,code,sizeof(code))) {
            restoreCodeJumps(i);
            return false;
        }
    }
    return true;
}

void restoreUnsignedBranches(size_t count) {
    for(size_t i=0;i<count;++i)
        writeCode(unsignedBranches[i].site,&unsignedBranches[i].original,1);
}

// Writes every branch, or none of them.
bool writeUnsignedBranches() {
    for(size_t i=0;i<ARRAYSIZE(unsignedBranches);++i) {
        if(!writeCode(unsignedBranches[i].site,&unsignedBranches[i].replacement,1)) {
            restoreUnsignedBranches(i);
            return false;
        }
    }
    return true;
}

// ---- Torture functions replaced whole

// Hang (vt+0x30 0x5a6fc0, read by UITorture 0x5e10a5): the seconds left are [+0xb8], a count
// of updates, over 30. A leaf with no stack arguments.
constexpr uintptr_t hangSecondsLeft = 0x5a6fc0;
constexpr uint32_t hangSecondsLeftHookLength = 6;  // mov ecx,[ecx+0xb8]
constexpr uint32_t hangClearUpdatesLeft = 0xb8;

bool hangSecondsLeftCodeMatches() {
    // mov ecx,[ecx+0xb8]; mov eax,0x88888889 (division by 30); ... sar edx,4 ... ret
    const unsigned char body[] = {0x8b,0x89,0xb8,0x00,0x00,0x00,0xb8,0x89,0x88,0x88,0x88,0xf7,0xe9,0x03,0xd1,
        0xc1,0xfa,0x04,0x8b,0xc2,0xc1,0xe8,0x1f,0x03,0xc2,0xc3};
    return same(hangSecondsLeft,body,sizeof(body));
}

int32_t __fastcall hangSecondsLeftHook(uint8_t* hang) {
    return *reinterpret_cast<const int32_t*>(hang+hangClearUpdatesLeft)/static_cast<int32_t>(currentUpdatesPerSecond);
}

// The press window of all three tortures (vt+0x2c 0x5a4cc0): thiscall with the torturer's
// animation number, ret 4, al. [+0x50] counts updates while the torturer swings (0x5a51de) and
// is compared with 30 fps frames of that swing; the upper bounds do not fit the imm8 when
// doubled, so the check is replaced with the same ranges on the count turned into frames.
constexpr uintptr_t pressWindow = 0x5a4cc0;
constexpr uint32_t pressWindowHookLength = 5;  // push ebp; mov ebp,esp; push ebx; push esi
constexpr uint32_t swingUpdates = 0x50;
constexpr uint32_t isSwingingSlot = 7;         // vt+0x1c: 0 before the swing animation starts
constexpr int32_t swingAnimation = 0x7e2;

struct FrameRange { int32_t after, before; };  // Exclusive bounds, as the game compares them
constexpr FrameRange beforeSwingWindow = {0x24,0x29};
constexpr FrameRange swingWindows[] = {{0x2a,0x30},{0x36,0x3c},{0x4b,0x51}};

bool pressWindowCodeMatches() {
    const unsigned char prologue[] = {0x55,0x8b,0xec,0x53,0x56,0x8b,0xf1,0x8b,0x06,0x8b,0x50,0x1c};
    const unsigned char beforeSwing[] = {0x8b,0x76,0x50,0x83,0xfe,0x24,0x7e,0x3d,0x83,0xfe,0x29};
    const unsigned char swing[] = {0x81,0x7d,0x08,0xe2,0x07,0x00,0x00};
    const unsigned char lastWindow[] = {0x83,0xfe,0x4b,0x7e,0x07,0xb0,0x01,0x83,0xfe,0x51};
    return same(pressWindow,prologue,sizeof(prologue)) && same(0x5a4cd4,beforeSwing,sizeof(beforeSwing)) &&
        same(0x5a4ce9,swing,sizeof(swing)) && same(0x5a4d0d,lastWindow,sizeof(lastWindow));
}

bool inside(int32_t frame, FrameRange range) { return frame>range.after && frame<range.before; }

// Called by the torture each update while a press is checked.
uint32_t __fastcall pressWindowHook(uint8_t* torture, void*, int32_t animation) {
    auto vtable=*reinterpret_cast<const uintptr_t* const*>(torture);
    auto isSwinging=reinterpret_cast<int32_t (__thiscall*)(void*)>(vtable[isSwingingSlot]);
    const int32_t frame=*reinterpret_cast<const int32_t*>(torture+swingUpdates)*
        static_cast<int32_t>(fps_uncap::gameUpdatesPerSecond)/static_cast<int32_t>(currentUpdatesPerSecond);
    if(!isSwinging(torture))
        return inside(frame,beforeSwingWindow);
    if(animation!=swingAnimation)
        return false;
    for(auto& window:swingWindows)
        if(inside(frame,window))
            return true;
    return false;
}

// ---- The rate

// Writes every value for updatesPerSecond. FALSE if an immediate could not be written.
bool applyRate(unsigned updatesPerSecond) {
    currentUpdatesPerSecond=updatesPerSecond;
    auto step=fps_uncap::stepFor(updatesPerSecond);
    *reinterpret_cast<double*>(va(stepSeconds))=step.seconds;
    *reinterpret_cast<float*>(va(managerStepSeconds))=step.secondsFloat;
    *reinterpret_cast<float*>(va(layoutStepSeconds))=step.secondsFloat;
    gameStepSeconds=step.seconds;
    countsPerUpdate=countsPerSecond/updatesPerSecond;
    thirtyFpsFramesPerUpdate=fps_uncap::perUpdateValue(PerUpdate::amount,1.0,updatesPerSecond);
    actionEndTimeoutUpdates=static_cast<int32_t>(fps_uncap::scaledUpdateCount(actionEndTimeoutAtGameRate,updatesPerSecond));
    rensatsuStartWaitUpdates=static_cast<int32_t>(fps_uncap::scaledUpdateCount(rensatsuStartWaitAtGameRate,updatesPerSecond));
    hangTimeoutSoundUpdates=static_cast<int32_t>(fps_uncap::scaledUpdateCount(hangTimeoutSoundAtGameRate,updatesPerSecond));
    for(auto& redirect:redirects)
        setRedirectValue(redirect,updatesPerSecond);
    bool written=true;
    for(auto& count:updateCounts)
        written=writeImmediate(count,immediateFor(count,updatesPerSecond))&&written;
    return written;
}

// For a key repeat and cameras the game already made, which copied the old values. Usually the
// Mod starts first and they are made from the values above. Needs the reads redirected.
void rewriteExistingObjects(unsigned updatesPerSecond) {
    writeLiveKeyRepeat(updatesPerSecond);
    writeLiveSaveListRepeat(updatesPerSecond);
    writeLiveCameraSettings();
}

// ---- Installation

// .rdata is read-only; the step constants stay writable so a rate change is a plain store.
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
    for(auto& count:updateCounts) {
        if(!updateCountMatches(count))
            return "an instruction that holds a count of updates";
        for(auto rate:fps_uncap::selectableUpdatesPerSecond)
            if(!immediateFits(count,rate))
                return "an instruction that holds a count of updates too large for its immediate";
    }
    if(!keyRepeatCodeMatches())
        return "the menu key repeat";
    if(!saveListCodeMatches())
        return "the save data list";
    if(!cameraCodeMatches())
        return "the camera manager";
    if(!actionScriptTimerCodeMatches())
        return "the action script timer at 0x4185c0";
    if(!advanceSceneCodeMatches())
        return "the event scene clock at 0x58ded0";
    if(!scriptWaitCodeMatches())
        return "the script Wait at 0x590c20";
    for(auto& jump:codeJumps)
        if(!codeJumpMatches(jump))
            return "an instruction replaced by a jump";
    if(!hangSecondsLeftCodeMatches())
        return "the hang seconds at 0x5a6fc0";
    if(!pressWindowCodeMatches())
        return "the torture press window at 0x5a4cc0";
    if(!unsignedBranchesMatch())
        return "the hang end state cycle at 0x5a8910";
    if(*reinterpret_cast<const uint64_t*>(va(stepSeconds))!=originalStepBits ||
        *reinterpret_cast<const uint32_t*>(va(managerStepSeconds))!=originalStepFloatBits ||
        *reinterpret_cast<const uint32_t*>(va(layoutStepSeconds))!=originalStepFloatBits)
        return "the 1/30 s step constants";
    return nullptr;
}

bool claimAll() {
    bool claimed=host->claim(self,va(frameLimiter),frameLimiterHookLength) &&
        host->claim(self,va(actionScriptTimer),actionScriptTimerHookLength) &&
        host->claim(self,va(advanceScene),advanceSceneHookLength) &&
        host->claim(self,va(scriptWaitStart),scriptWaitStartHookLength) &&
        host->claim(self,va(hangSecondsLeft),hangSecondsLeftHookLength) &&
        host->claim(self,va(pressWindow),pressWindowHookLength) &&
        host->claim(self,va(stepSeconds),sizeof(double)) &&
        host->claim(self,va(managerStepSeconds),sizeof(float)) &&
        host->claim(self,va(layoutStepSeconds),sizeof(float));
    for(auto& redirect:redirects)
        claimed=claimed&&host->claim(self,va(redirect.instruction+operandOffset),operandLength);
    for(auto& count:updateCounts)
        claimed=claimed&&host->claim(self,va(count.instruction+count.immediateOffset),count.immediateSize);
    for(auto& jump:codeJumps)
        claimed=claimed&&host->claim(self,va(jump.site),jump.length);
    for(auto& branch:unsignedBranches)
        claimed=claimed&&host->claim(self,va(branch.site),1);
    return claimed;
}

// MinHook's trampolines of the replaced functions; their hooks never call them
void* replacedFrameLimiter;
void* replacedActionScriptTimer;
void* replacedHangSecondsLeft;
void* replacedPressWindow;

struct Hook {
    uintptr_t target;
    void* detour;
    void** original;
};

const Hook hooks[] = {
    {frameLimiter,reinterpret_cast<void*>(&frameLimiterHook),&replacedFrameLimiter},
    {actionScriptTimer,reinterpret_cast<void*>(&actionScriptTimerHook),&replacedActionScriptTimer},
    {advanceScene,reinterpret_cast<void*>(&advanceSceneHook),&originalAdvanceScene},
    {scriptWaitStart,reinterpret_cast<void*>(&scriptWaitStartHook),reinterpret_cast<void**>(&originalScriptWaitStart)},
    {hangSecondsLeft,reinterpret_cast<void*>(&hangSecondsLeftHook),&replacedHangSecondsLeft},
    {pressWindow,reinterpret_cast<void*>(&pressWindowHook),&replacedPressWindow},
};

void removeHooks(size_t count) {
    for(size_t i=0;i<count;++i)
        MH_RemoveHook(reinterpret_cast<void*>(va(hooks[i].target)));
}

// Creates and enables every hook, code jump and branch, or none of them. Needs MinHook initialized.
bool hookGame() {
    for(size_t i=0;i<ARRAYSIZE(hooks);++i) {
        if(MH_CreateHook(reinterpret_cast<void*>(va(hooks[i].target)),hooks[i].detour,hooks[i].original)!=MH_OK) {
            removeHooks(i);
            return false;
        }
    }
    if(!writeCodeJumps()) {
        removeHooks(ARRAYSIZE(hooks));
        return false;
    }
    if(!writeUnsignedBranches()) {
        restoreCodeJumps(ARRAYSIZE(codeJumps));
        removeHooks(ARRAYSIZE(hooks));
        return false;
    }
    if(MH_EnableHook(MH_ALL_HOOKS)!=MH_OK) {
        restoreUnsignedBranches(ARRAYSIZE(unsignedBranches));
        restoreCodeJumps(ARRAYSIZE(codeJumps));
        removeHooks(ARRAYSIZE(hooks));
        return false;
    }
    return true;
}

bool installed;

// ---- The rate chosen on the options page

// After the game's update, so every system sees one rate for a whole update. Rare: only when the
// player chose another rate.
void WINAPI applyChosenRate(void*) {
    if(!settings.takeRateChange())
        return;
    const unsigned updatesPerSecond=settings.updatesPerSecond();
    const bool written=applyRate(updatesPerSecond);
    rewriteExistingObjects(updatesPerSecond);
    char line[200];
    if(written)
        sprintf_s(line,"frame rate changed to %u updates per second",updatesPerSecond);
    else
        sprintf_s(line,"frame rate changed to %u updates per second, but some counts of updates could not be "
            "written (VirtualProtect) and keep the previous rate",updatesPerSecond);
    log(line);
}

void WINAPI saveSettings(void*) { settings.save(); }

// Without the frame callback a chosen rate could not be applied, so the page is left out then.
void addSettingsPage(const wots4::ModContext& context) {
    if(!host->onFrame(self,applyChosenRate,nullptr)) {
        log("the ModLoader refused the frame callback; no options page, the rate can be changed in config.ini");
        return;
    }
    if(!host->onOptionsSaved(self,saveSettings,nullptr))
        log("the ModLoader refused the options save callback; a rate chosen on the options page is not saved");
    if(!settings.registerPage(context))
        log("the ModLoader refused the options page; the rate can be changed in config.ini");
}

}  // namespace

extern "C" BOOL WINAPI ModInitialize(const wots4::ModContext* context) {
    if(!context||context->size<sizeof(*context)||context->version!=wots4::modApiVersion||!context->host||
        !hostHasUsedMembers(context->host)||!context->host->claim||!context->host->releaseClaims||
        !context->host->addOptionsPage||!context->host->onFrame||!context->host->onOptionsSaved||
        !context->directory)
        return FALSE;
    if(installed)
        return TRUE;
    host=context->host;
    self=context->module;
    gameBase=reinterpret_cast<uintptr_t>(context->game);
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    countsPerSecond=frequency.QuadPart;

    if(auto mismatch=codeMismatch()) {
        char line[200];
        sprintf_s(line,"%s is not the known version (Steam, PE time stamp 683d42eb); nothing changed",mismatch);
        log(line);
        return FALSE;
    }
    if(!claimAll()) {
        log("a hooked function, a time constant or a count is used by another Mod; nothing changed");
        host->releaseClaims(self);
        return FALSE;
    }
    if(!makeWritable(stepSeconds,sizeof(double))||!makeWritable(managerStepSeconds,sizeof(float))) {
        log("failed to make the step constants writable (VirtualProtect); nothing changed");
        host->releaseClaims(self);
        return FALSE;
    }
    settings.load(context->directory,log);
    const unsigned chosenUpdatesPerSecond=settings.updatesPerSecond();
    if(!applyRate(chosenUpdatesPerSecond)) {
        log("failed to write the counts of updates (VirtualProtect); nothing changed");
        applyRate(fps_uncap::gameUpdatesPerSecond);
        host->releaseClaims(self);
        return FALSE;
    }
    if(!redirectOperands()) {
        applyRate(fps_uncap::gameUpdatesPerSecond);
        host->releaseClaims(self);
        return FALSE;
    }
    rewriteExistingObjects(chosenUpdatesPerSecond);
    if(MH_Initialize()!=MH_OK||!hookGame()) {
        log("failed to hook the frame limiter, the action script timer, the event scene clock, the script Wait "
            "or the code jumps (MinHook or VirtualProtect); nothing changed");
        MH_Uninitialize();
        restoreOperands(ARRAYSIZE(redirects));
        applyRate(fps_uncap::gameUpdatesPerSecond);
        rewriteExistingObjects(fps_uncap::gameUpdatesPerSecond);
        host->releaseClaims(self);
        return FALSE;
    }
    installed=true;
    char line[200];
    sprintf_s(line,"installed: %u updates per second, scripts included; %u reads redirected, "
        "%u update counts scaled, %u hooks",chosenUpdatesPerSecond,static_cast<unsigned>(ARRAYSIZE(redirects)),
        static_cast<unsigned>(ARRAYSIZE(updateCounts)),static_cast<unsigned>(ARRAYSIZE(hooks)+ARRAYSIZE(codeJumps)));
    log(line);
    addSettingsPage(*context);
    return TRUE;
}
