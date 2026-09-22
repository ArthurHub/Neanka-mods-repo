#pragma once

#include <chrono>

/**
 * The turning style the player picked in the VR comfort settings ("iRotationType:VR").
 */
enum class VRRotationType : UInt8
{
    None = 0, // turning is off
    SnapSmoothed = 1, // snap by a fixed angle, interpolated there at fAngleSnapSmoothingSpeed:VR
    SnapInstant = 2, // snap by a fixed angle in one frame (vanilla also plays a comfort fade)
    Smooth = 3, // continuous turn at fRotationSpeed:VR while the stick is held
};

/**
 * Turn the player the way the game itself would, including where the game refuses to: the vanilla turn is
 * driven by a player-controls input handler, so anything that takes the controls away (dialogue, most menus)
 * also takes turning away, while thumbstick events keep reaching the menu-controls handler this plugin
 * registers.
 *
 * Turning in VR rotates the VR world (room) transform, not the actor - the actor's heading follows the HMD -
 * so this writes the same transform the engine's own turn code writes, and reads the style / angle / speed
 * straight off the engine's parsed Fallout4Prefs.ini [VR] settings, so a change made in the in-game settings
 * menu applies without a restart.
 *
 * The interpolation of a smoothed snap is run here rather than handed to the engine's per-frame applier,
 * which is gated behind a latch only the vanilla input handler clears - see the note in PlayerRotation.cpp.
 * The one vanilla behaviour not reproduced is the comfort fade of the instant-snap style.
 *
 * Main thread only, and OnFrameUpdate() must be called every frame for a smoothed snap to finish.
 */
class PlayerRotation
{
public:
    // The player's configured turning style and its tuning, read live.
    static VRRotationType GetRotationType();
    static float GetSnapAngleDegrees();
    static float GetSmoothSpeedDegreesPerSec();
    static float GetSnapSmoothingSpeedDegreesPerSec();

    /**
     * Feed every thumbstick event of the turning stick to turn exactly as the player's settings say: a snap
     * per flick for the snap styles, a continuous turn while held for the smooth style, and nothing at all
     * when turning is off. Takes the direction the game already put on the event rather than the raw axis,
     * which is what the vanilla turn handler works off too, so the deadzones the player configured apply.
     * Returns true if the flick was used to turn, so the caller can keep it from also reaching whatever else
     * reads that stick.
     * Also advances an in-progress smoothed snap, so this is the only per-frame call a caller needs while the
     * stick is in use.
     */
    static bool TurnByThumbstick(UInt32 direction, UInt32 previousDirection);

    /**
     * Advance an in-progress smoothed snap. Needed on frames without a thumbstick event, which is every frame
     * after the player lets the stick go, so call it from whatever per-frame tick the caller has.
     */
    static void OnFrameUpdate();

    // One configured snap, honouring the smoothed / instant style. Ignores the stick direction handling.
    static void SnapTurn(bool right);

    // Continuous turn for one frame of deltaSeconds at the configured smooth speed.
    static void SmoothTurn(bool right, float deltaSeconds);

    // Raw access to the VR world yaw in radians, bypassing the configured style entirely.
    static float GetYaw();
    static void SetYaw(float yawRadians);
    static void RotateBy(float deltaRadians);

    static bool IsSnapInProgress();

    /**
     * Drop an in-progress smoothed snap where it is. Worth calling when whatever was driving the turn goes
     * away mid-snap (the dialogue menu closed, a save was loaded).
     */
    static void Cancel();

private:
    static void StepPendingSnap(float deltaSeconds);
    static float ElapsedSeconds(std::chrono::steady_clock::time_point& lastTime);

    // Radians still owed by a smoothed snap, signed; 0 = none in progress.
    static float s_pendingSnap;

    // The turn and the snap interpolation keep their own clocks on purpose: they are driven from different
    // places (thumbstick events / any per-frame tick) and a shared clock would hand each of them only the
    // slice of the frame since the other one last ran, which comes out as a turn slower than the vanilla one.
    static std::chrono::steady_clock::time_point s_lastTurnTime;
    static std::chrono::steady_clock::time_point s_lastSnapStepTime;
};
