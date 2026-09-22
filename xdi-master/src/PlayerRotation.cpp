#include "PlayerRotation.h"

#include <cmath>

#include "GameUtils.h"
#include "rva/RVA.h"

#include "f4se/GameSettings.h"

namespace
{
    //--------------------
    // Addresses [3]
    //--------------------

    // VR player turning. The game turns the player by rotating the VR world (room) transform rather than the
    // actor: the actor's heading follows the HMD. The three entries below are the get/set pair the engine's
    // own turn code uses plus the global it works on, all read off the VR turn worker (0x140FCF5E0 of
    // Fallout4VR.exe 1.2.72), which loads that same global into rbp and calls both functions.
    // Source: F4VR-CommonFramework src/f4vr/F4VROffsets.h ("VR player turning"); the world-space struct is
    // also in Modding-Reference/F4VR/Analysis/gold/Virtual-Reloads_RE_REFERENCE.md ("vrDataStruct").
    //
    // The VR addresses are given for the record but do not resolve these: F4SEVR reports the *flat* game's
    // runtime version to plugins (0x010A08A0 / v1.10.138, its RUNTIME_VERSION was never updated for VR), so
    // no address map keyed on a VR version is ever hit and the signature is what resolves - as it is for
    // every other address in this plugin. Each signature was checked to match exactly once in 1.2.72, and
    // IsResolved() below keeps a miss from turning into a null call.

    // The VR world-space data the game rotates when the player turns; other VR mods call it `vrDataStruct`.
    // The room rotation sits at +0x210 as a SIMD-padded 3x3 matrix (three rows of four floats). Resolved off
    // the `mov rbp, [rip + vrWorldData]` 16 bytes into the turn worker.
    RVA<void*> g_vrWorldData(0x59429C0, "48 8B C4 48 89 58 10 48 89 68 20 56 48 83 EC 70 48 8B 2D ? ? ? ?", 0x10, 3, 7);
    constexpr int VR_WORLD_DATA_ROTATION_OFFSET = 0x210;

    // Euler decomposition of the VR world rotation matrix (pass g_vrWorldData + 0x210): the yaw the game
    // turns with lands in yawOut, in radians. The two other out-params are the remaining Euler angles,
    // written in every path; the return says whether the matrix was outside the gimbal-lock branch.
    //
    // Pair it with VRWorld_SetYaw and nothing else: that is the pair the engine's own smooth turn uses, so a
    // get -> add -> set round trip is sign-convention safe. (A second decomposition exists at 0x141C0FED0,
    // used by the snap path, with a different Euler convention.)
    using _VRWorld_GetEulerAngles = bool (*)(const void* rotationMatrix, float* yawOut, float* outB, float* outC);
    RVA<_VRWorld_GetEulerAngles> VRWorld_GetEulerAngles(0x1C11B00,
        "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 50 F3 0F 10 41 18 0F 29 74 24 40 F3 0F 10 35 ? ? ? ? 0F 29 7C 24 30");

    // Rebuild the VR world rotation from a single yaw (radians, taken by pointer) and write it back - what
    // both the engine's smooth turn and its instant snap call to actually move the player.
    using _VRWorld_SetYaw = void (*)(void* vrWorldData, const float* yawRadians);
    RVA<_VRWorld_SetYaw> VRWorld_SetYaw(0x1BA7780,
        "48 8B C4 53 48 83 EC 70 F3 0F 10 12 48 8B D9 0F 29 70 E8 48 8D 50 10 48 8D 48 18 0F 29 78 D8 0F 57 15 ? ? ? ?");

    // NOT used on purpose: the engine's own snap entry points, PlayerCharacter "start smoothed snap"
    // (0x140EFA920) and "snap now, with the comfort fade" (0x140EFA980). Both only park a target angle in
    // PlayerCharacter +0x8C8 behind the latch flags at +0x12A4: bit 0x20 is handed to the per-frame applier
    // and bit 0x40 is cleared *only* by the vanilla thumbstick handler when the stick re-centres. Called
    // while that handler is out of the loop - which is exactly the case this turns the player for, a menu
    // that blocks turning - the first call latches 0x40 and every later one is silently dropped. So the
    // transform is driven here instead.

    //--------------------
    // Constants
    //--------------------

    constexpr float DEG_TO_RAD = 0.017453292f;
    constexpr float TWO_PI = 6.2831855f;

    // A smoothed snap is done once this little of it is left, the same epsilon the engine's applier uses.
    constexpr float SNAP_ARRIVED_EPSILON = 0.0001f;

    // Ceiling on a frame delta, so a hitch or a loading screen doesn't spin the player.
    constexpr float MAX_FRAME_DELTA_SEC = 0.1f;

    // The directions the game puts on a thumbstick event once the player's deadzones are applied.
    constexpr UInt32 THUMBSTICK_DIRECTION_RIGHT = 2;
    constexpr UInt32 THUMBSTICK_DIRECTION_LEFT = 4;

    /**
     * One Fallout4Prefs.ini [VR] turn setting, looked up by name on first use and then held onto: the lookup
     * walks the INI collection's linked list and these are read every frame while the stick is held. Reading
     * the Setting itself keeps the value live, and the in-game settings menu writes these very objects, so a
     * change there is picked up without a restart.
     */
    class VRSetting
    {
    public:
        VRSetting(const char* name, double fallback)
            : m_name(name)
            , m_fallback(fallback) {}

        double Get()
        {
            if (!m_lookedUp) {
                m_lookedUp = true;
                m_setting = GameUtils::GetINISetting(m_name);
                if (!m_setting) {
                    _WARNING("Rotation: VR setting '%s' not found, using %f.", m_name, m_fallback);
                }
            }

            double value;
            return m_setting && m_setting->GetDouble(&value) ? value : m_fallback;
        }

    private:
        const char* m_name;
        double m_fallback;
        Setting* m_setting = nullptr;
        bool m_lookedUp = false;
    };

    // Fallbacks are the game's own Fallout4Prefs.ini [VR] defaults.
    VRSetting s_rotationType("iRotationType:VR", 1);
    VRSetting s_rotationAngle("fRotationAngle:VR", 45);
    VRSetting s_rotationSpeed("fRotationSpeed:VR", 80);
    VRSetting s_angleSnapSmoothingSpeed("fAngleSnapSmoothingSpeed:VR", 360);

    /**
     * The VR world-space data to rotate, or null if the addresses above didn't resolve (a game version other
     * than 1.2.72) or the data isn't up yet. Turning is then simply skipped.
     */
    void* GetVRWorldData()
    {
        if (!g_vrWorldData.IsResolved() || !VRWorld_GetEulerAngles.IsResolved() || !VRWorld_SetYaw.IsResolved()) {
            return nullptr;
        }
        return *g_vrWorldData;
    }
}

float PlayerRotation::s_pendingSnap = 0.0f;
std::chrono::steady_clock::time_point PlayerRotation::s_lastTurnTime;
std::chrono::steady_clock::time_point PlayerRotation::s_lastSnapStepTime;

VRRotationType PlayerRotation::GetRotationType()
{
    const int type = static_cast<int>(s_rotationType.Get());
    return type >= static_cast<int>(VRRotationType::None) && type <= static_cast<int>(VRRotationType::Smooth)
        ? static_cast<VRRotationType>(type)
        : VRRotationType::None;
}

float PlayerRotation::GetSnapAngleDegrees()
{
    return static_cast<float>(s_rotationAngle.Get());
}

float PlayerRotation::GetSmoothSpeedDegreesPerSec()
{
    return static_cast<float>(s_rotationSpeed.Get());
}

float PlayerRotation::GetSnapSmoothingSpeedDegreesPerSec()
{
    return static_cast<float>(s_angleSnapSmoothingSpeed.Get());
}

/**
 * Turn from the thumbstick the way the vanilla VR turn handler does (0x140FC8730): only the horizontal
 * directions turn, a direction the stick wasn't already in is one snap for the snap styles, and holding it
 * there is a continuous turn for the smooth style. A smoothed snap already running is advanced first, so
 * this is the only per-frame call while the stick is in use.
 */
bool PlayerRotation::TurnByThumbstick(const UInt32 direction, const UInt32 previousDirection)
{
    // Time the turn from the previous event of this stick - one frame's worth - and leave the snap
    // interpolation its own clock, which whatever per-frame tick calls OnFrameUpdate() shares.
    const float deltaSeconds = ElapsedSeconds(s_lastTurnTime);
    StepPendingSnap(ElapsedSeconds(s_lastSnapStepTime));

    const VRRotationType type = GetRotationType();
    if (type == VRRotationType::None) {
        return false;
    }

    if (direction != THUMBSTICK_DIRECTION_RIGHT && direction != THUMBSTICK_DIRECTION_LEFT) {
        return false;
    }

    const bool right = direction == THUMBSTICK_DIRECTION_RIGHT;
    const bool flicked = direction != previousDirection;
    if (type == VRRotationType::Smooth) {
        if (!flicked) {
            SmoothTurn(right, deltaSeconds);
        }
    } else if (flicked) {
        SnapTurn(right);
    }
    return true;
}

void PlayerRotation::OnFrameUpdate()
{
    StepPendingSnap(ElapsedSeconds(s_lastSnapStepTime));
}

/**
 * Queue (or apply) one snap of the configured angle. The smoothed style adds onto whatever a snap still in
 * flight owes, so flicking again mid-turn stacks the way the engine's moving target does.
 */
void PlayerRotation::SnapTurn(const bool right)
{
    const float angle = GetSnapAngleDegrees() * DEG_TO_RAD * (right ? 1.0f : -1.0f);
    if (GetRotationType() == VRRotationType::SnapInstant) {
        RotateBy(angle);
    } else {
        s_pendingSnap += angle;
    }
}

void PlayerRotation::SmoothTurn(const bool right, const float deltaSeconds)
{
    RotateBy(GetSmoothSpeedDegreesPerSec() * DEG_TO_RAD * deltaSeconds * (right ? 1.0f : -1.0f));
}

float PlayerRotation::GetYaw()
{
    void* vrWorldData = GetVRWorldData();
    if (!vrWorldData) {
        return 0;
    }
    float yaw = 0, unusedB = 0, unusedC = 0;
    VRWorld_GetEulerAngles(static_cast<UInt8*>(vrWorldData) + VR_WORLD_DATA_ROTATION_OFFSET, &yaw, &unusedB, &unusedC);
    return yaw;
}

/**
 * Write the VR world yaw, normalized into [0, 2pi) as the engine's own rotation code keeps it so a long
 * session of turning one way can't drift the value off into imprecision.
 */
void PlayerRotation::SetYaw(const float yawRadians)
{
    void* vrWorldData = GetVRWorldData();
    if (!vrWorldData) {
        return;
    }
    float yaw = std::fmod(yawRadians, TWO_PI);
    if (yaw < 0) {
        yaw += TWO_PI;
    }
    VRWorld_SetYaw(vrWorldData, &yaw);
}

void PlayerRotation::RotateBy(const float deltaRadians)
{
    if (deltaRadians != 0) {
        SetYaw(GetYaw() + deltaRadians);
    }
}

bool PlayerRotation::IsSnapInProgress()
{
    return s_pendingSnap != 0;
}

void PlayerRotation::Cancel()
{
    s_pendingSnap = 0;
}

/**
 * Move a smoothed snap along by one frame's worth of fAngleSnapSmoothingSpeed, never past what it owes.
 */
void PlayerRotation::StepPendingSnap(const float deltaSeconds)
{
    if (s_pendingSnap == 0) {
        return;
    }

    // std::clamp of the owed angle to one frame's step (Windows.h makes std::min / std::max unusable here).
    const float step = GetSnapSmoothingSpeedDegreesPerSec() * DEG_TO_RAD * deltaSeconds;
    const float applied = s_pendingSnap > step ? step : (s_pendingSnap < -step ? -step : s_pendingSnap);
    s_pendingSnap = std::abs(s_pendingSnap - applied) < SNAP_ARRIVED_EPSILON ? 0 : s_pendingSnap - applied;
    RotateBy(applied);
}

/**
 * Seconds since the given clock last ran, re-arming it. Measured here rather than taken from the engine: the
 * global the engine's own smooth turn multiplies by is unidentified, and a wrong guess would come out as a
 * wrong turn speed. The first call and any hitch yield no turning rather than a jump.
 */
float PlayerRotation::ElapsedSeconds(std::chrono::steady_clock::time_point& lastTime)
{
    const auto now = std::chrono::steady_clock::now();
    if (lastTime == std::chrono::steady_clock::time_point{}) {
        lastTime = now;
        return 0;
    }
    const float delta = std::chrono::duration<float>(now - lastTime).count();
    lastTime = now;
    return delta > MAX_FRAME_DELTA_SEC ? 0 : delta;
}
