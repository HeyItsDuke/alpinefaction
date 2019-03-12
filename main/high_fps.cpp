#include "stdafx.h"
#include "high_fps.h"
#include "utils.h"
#include "rf.h"
#include "inline_asm.h"
#include "commands.h"
#include <FunHook2.h>
#include <RegsPatch.h>
#include <unordered_map>
#include <unordered_set>

constexpr auto reference_fps = 30.0f;
constexpr auto reference_framerate = 1.0f / reference_fps;
constexpr auto screen_shake_fps = 150.0f;

static float g_jump_threshold = 0.05f;
static float g_camera_shake_factor = 0.6f;

class FtolAccuracyFix {

    struct StateData {
        double remainder = 0.0;
        double last_val = 0.0;
        int num_calls_in_frame = 0;
        int age = 0;
    };

    uintptr_t m_ftol_call_addr;
    std::optional<AsmRegMem> m_key_loc_opt;
    std::unordered_map<void*, StateData> m_state_map;

public:
    FtolAccuracyFix(uintptr_t ftol_call_addr, std::optional<AsmRegMem> key_loc = {}) :
        m_ftol_call_addr(ftol_call_addr), m_key_loc_opt(key_loc) {}

    long ftol(double value, void *key)
    {
        auto& state = m_state_map[key];

        if (state.num_calls_in_frame > 0 && state.last_val != value)
            TRACE("Different ftol argument during a single frame in address %p", m_ftol_call_addr);

        value += state.remainder;
        long result = static_cast<long>(value);
        state.remainder = value - result;
        state.last_val = value;
        state.num_calls_in_frame++;
        state.age = 0;

        return result;
    }

    void NextFrame()
    {
        std::vector<void*> keys_to_erase;
        for (auto& p : m_state_map) {
            auto& state = p.second;
            state.num_calls_in_frame = 0;
            state.age++;
            if (state.age >= 100)
                keys_to_erase.push_back(p.first);
        }
        for (auto& k : keys_to_erase) {
            m_state_map.erase(k);
        }
    }

    void Install()
    {
        char *code_buf = new char[512];
        UnprotectMem(code_buf, 512);

        using namespace AsmRegs;
        AsmWritter(reinterpret_cast<unsigned>(code_buf))
            .mov(ECX, reinterpret_cast<int32_t>(this)) // thiscall
            .sub(ESP, 12)
            .mov(EAX, m_key_loc_opt.value_or(ECX))
            .mov(AsmMem(ESP + 8), EAX)
            .fstp<double>(AsmMem(ESP + 0))
            .callLong(reinterpret_cast<void*>(&ftol))
            .ret();

        AsmWritter(m_ftol_call_addr)
            .callLong(code_buf);
    }
};

std::array<FtolAccuracyFix, 5> g_ftol_accuracy_fixes{
    FtolAccuracyFix{0x00416426}, // hit screen
    FtolAccuracyFix{0x004D5214, AsmRegs::ESI}, // decal fade out
    FtolAccuracyFix{0x005096A7}, // timer
    FtolAccuracyFix{0x0050ABFB}, // console open/close
    FtolAccuracyFix{0x0051BAD7, AsmRegs::ESI}, // anim mesh
};

#ifdef DEBUG

struct FtolDebugInfo
{
    double val_sum = 0.0;
    int num_calls = 0;
};

int g_num_frames = 0;
bool g_ftol_issue_detection = false;
float g_ftol_issue_detection_fps;
std::unordered_map<uintptr_t, FtolDebugInfo> g_ftol_debug_info_map;
std::unordered_map<uintptr_t, FtolDebugInfo> g_ftol_debug_info_map_old;
std::unordered_set<uintptr_t> g_ftol_issues;

extern "C" long ftol2(double value, uintptr_t ret_addr)
{
    long result = static_cast<long>(value);
    g_ftol_debug_info_map[ret_addr].val_sum += value;
    g_ftol_debug_info_map[ret_addr].num_calls++;
    return result;
}

ASM_FUNC(ftol_Wrapper,
    ASM_I  mov ecx, [esp]
    ASM_I  sub esp, 12
    ASM_I  mov [esp + 8], ecx
    ASM_I  fstp qword ptr [esp + 0]
    ASM_I  call ASM_SYM(ftol2)
    ASM_I  add esp, 12
    ASM_I  ret
)

void FtolIssuesDetectionStart()
{
    g_ftol_issues.clear();
    g_ftol_issue_detection = true;
}

void FtolIssuesDetectionDoFrame()
{
    if (!g_ftol_issue_detection)
        return;
    if (g_num_frames == 0) {
        for (auto p : g_ftol_debug_info_map) {
            auto& info_60 = p.second;
            auto& info_30 = g_ftol_debug_info_map_old[p.first];
            float avg_60 = info_60.val_sum / info_60.num_calls;
            float avg_30 = info_30.val_sum / info_30.num_calls;
            float value_ratio = avg_60 / avg_30;
            float framerate_ratio = 30.0f / 60.0f;
            float ratio = value_ratio / framerate_ratio;
            bool is_fps_dependent = fabs(ratio - 1.0f) < 0.1f;
            float avg_high_fps = fabs(avg_60 * 60.0f / g_ftol_issue_detection_fps);
            bool is_significant = avg_high_fps < 1.0f;
            if (is_fps_dependent && is_significant) {
                bool is_new = g_ftol_issues.insert(p.first).second;
                if (is_new)
                    rf::DcPrintf("ftol issue detected: address %p ratio %.2f estimated value %.4f",
                        p.first - 5, ratio, avg_high_fps);
            }
        }

        rf::g_fMinFramerate = 1.0f / 30.0f;
        g_ftol_debug_info_map.clear();
        g_ftol_debug_info_map_old.clear();
    }
    else if (g_num_frames == 5) {
        g_ftol_debug_info_map_old = std::move(g_ftol_debug_info_map);
        g_ftol_debug_info_map.clear();
        rf::g_fMinFramerate = 1.0f / 60.0f;
    }

    g_num_frames = (g_num_frames + 1) % 10;
}

DcCommand2 detect_ftol_issues_cmd{
    "detect_ftol_issues",
    [](std::optional<float> fps_opt) {
        if (!g_ftol_issue_detection) {
            g_ftol_issue_detection_fps = fps_opt.value_or(240);
            FtolIssuesDetectionStart();
        }
        else {
            g_ftol_issue_detection = false;
        }
        rf::DcPrintf("ftol issues detection is %s", g_ftol_issue_detection ? "enabled" : "disabled");
    },
    "detect_ftol_issues <fps>"
};

#endif // DEBUG

void STDCALL EntityWaterDecelerateFix(rf::EntityObj* entity)
{
    float vel_factor = 1.0f - (rf::g_fFramerate * 4.5f);
    entity->_Super.PhysInfo.vVel.x *= vel_factor;
    entity->_Super.PhysInfo.vVel.y *= vel_factor;
    entity->_Super.PhysInfo.vVel.z *= vel_factor;
}

extern "C" void WaterAnimateWaves_UpdatePos(rf::Vector3* result)
{
    constexpr float flt_5A3BF4 = 12.8f;
    constexpr float flt_5A3C00 = 3.878788f;
    constexpr float flt_5A3C0C = 4.2666669f;
    result->x += flt_5A3BF4 * (rf::g_fFramerate) / reference_framerate;
    result->y += flt_5A3C0C * (rf::g_fFramerate) / reference_framerate;
    result->z += flt_5A3C00 * (rf::g_fFramerate) / reference_framerate;
}

ASM_FUNC(WaterAnimateWaves_004E68A0,
    ASM_I  mov eax, esi
    ASM_I  add eax, 0x2C
    ASM_I  push eax
    ASM_I  call ASM_SYM(WaterAnimateWaves_UpdatePos)
    ASM_I  add esp, 4
    ASM_I  mov ecx, [esi + 0x24]
    ASM_I  lea eax, [esp + 0x6C - 0x20] // var_20
    ASM_I  mov eax, 0x004E68D1
    ASM_I  jmp eax
)

FunHook2<int(rf::String&, rf::String&, char*)> RflLoad_Hook{
    0x0045C540,
    [](rf::String& level_filename, rf::String& a2, char* error_desc) {
        int ret = RflLoad_Hook.CallTarget(level_filename, a2, error_desc);
        if (ret == 0 && strstr(level_filename, "L5S3")) {
            // Fix submarine exploding - change delay of two events to make submarine physics enabled later
            //INFO("Fixing Submarine exploding bug...");
            rf::Object* obj = rf::ObjGetFromUid(4679);
            if (obj && obj->Type == rf::OT_EVENT) {
                rf::EventObj* event = reinterpret_cast<rf::EventObj*>(reinterpret_cast<uintptr_t>(obj) - 4);
                event->fDelay += 1.5f;
            }
            obj = rf::ObjGetFromUid(4680);
            if (obj && obj->Type == rf::OT_EVENT) {
                rf::EventObj* event = reinterpret_cast<rf::EventObj*>(reinterpret_cast<uintptr_t>(obj) - 4);
                event->fDelay += 1.5f;
            }
        }
        return ret;
    }
};

RegsPatch CutsceneShotSyncFix{
    0x0045B43B,
    [](X86Regs& regs) {
        auto &current_shot_idx = StructFieldRef<int>(rf::g_active_cutscene, 0x808);
        void *current_shot_timer = reinterpret_cast<char*>(rf::g_active_cutscene) + 0x810;
        if (current_shot_idx > 1)
        {
            // decrease time for next shot using current shot timer value
            int shot_time_left_ms = rf::Timer__GetTimeLeftMs(current_shot_timer);
            if (shot_time_left_ms > 0 || shot_time_left_ms < -100)
                WARN("invalid shot_time_left_ms %d", shot_time_left_ms);
            regs.eax += shot_time_left_ms;
        }
    }
};

void HighFpsInit()
{
    // Fix animations broken on high FPS because of ignored ftol remainder
    for (auto& ftol_fix : g_ftol_accuracy_fixes) {
        ftol_fix.Install();
    }

#ifdef DEBUG
    AsmWritter(0x00573528).jmpLong(ftol_Wrapper);
    detect_ftol_issues_cmd.Register();
#endif

    // Fix jumping on high FPS
    WriteMemPtr(0x004A09A6 + 2, &g_jump_threshold);

    // Fix water deceleration on high FPS
    WriteMemUInt8(0x0049D816, ASM_NOP, 5);
    WriteMemUInt8(0x0049D82A, ASM_NOP, 5);
    WriteMemUInt8(0x0049D82A + 5, ASM_PUSH_ESI);
    AsmWritter(0x0049D830).callLong(EntityWaterDecelerateFix);

    // Fix water waves animation on high FPS
    WriteMemUInt8(0x004E68A0, ASM_NOP, 9);
    WriteMemUInt8(0x004E68B6, ASM_LONG_JMP_REL);
    AsmWritter(0x004E68B6).jmpLong(WaterAnimateWaves_004E68A0);

    // Fix incorrect frame time calculation
    WriteMemUInt8(0x00509595, ASM_NOP, 2);
    WriteMemUInt8(0x00509532, ASM_SHORT_JMP_REL);

    // Fix submarine exploding on high FPS
    RflLoad_Hook.Install();

    // Fix screen shake caused by some weapons (eg. Assault Rifle)
    WriteMemPtr(0x0040DBCC + 2, &g_camera_shake_factor);

    // Remove cutscene sync RF hackfix
    WriteMemFloat(0x005897B4, 1000.0f);
    WriteMemFloat(0x005897B8, 1.0f);
    static float zero = 0.0f;
    WriteMemPtr(0x0045B42A + 2, &zero);

    // Fix cutscene shot timer sync on high fps
    CutsceneShotSyncFix.Install();
}

void HighFpsUpdate()
{
    float frame_time = rf::g_fFramerate;
    if (frame_time > 0.0001f) {
        // Make jump fix framerate dependent to fix bouncing on small FPS
        g_jump_threshold = 0.025f + 0.075f * frame_time / (1 / 60.0f);

        // Fix screen shake caused by some weapons (eg. Assault Rifle)
        g_camera_shake_factor = pow(0.6f, frame_time / (1 / screen_shake_fps));
    }

#if DEBUG
    FtolIssuesDetectionDoFrame();
#endif

    for (auto& ftol_fix : g_ftol_accuracy_fixes) {
        ftol_fix.NextFrame();
    }
}
