#include "DialogueMenuPlacement.h"

#include <atomic>
#include <cmath>

#include <xbyak/xbyak.h>

#include "DialogueEx.h"
#include "GameUtils.h"
#include "Globals.h"
#include "Settings.h"
#include "rva/RVA.h"

#include "f4se_common/BranchTrampoline.h"

#include "f4se/GameReferences.h"
#include "f4se/GameSettings.h"

namespace DialogueMenuPlacement
{
    namespace
    {
        //--------------------
        // Addresses [1]
        //--------------------

        // PlayerCharacter VR UI update - VR 0x140EF7180, called from the player update every frame and on the
        // game's main thread. It works out where the head is looking, keeps the yaw the world-space UI is
        // anchored to (PlayerCharacter + 0x890) trailing it under the fHmdRotationLag*:VRUI settings, and then
        // rebuilds and updates the UI roots (PlayerCharacter + 0x7E0 and + 0x7F0) from it.
        //
        // Found from the RTTI of WSDialogueInputModel, whose placement method (VR 0x140C81010) turned out to be
        // the wrong thing to hook: it writes the menu node's whole local transform from fDialogueInputX/Y/Z/
        // Pitch/Scale:VRUI, but those default to 0 (scale 1), so the node sits on its parent's origin with
        // nothing of its own to aim - and it only runs about three times a second. All of the menu's placement
        // comes from the roots this function builds, so this is where the aiming belongs.
        using _PlayerCharacter_UpdateVRUI = void (*)(void* player, float deltaSeconds, bool unk);
        RVA<_PlayerCharacter_UpdateVRUI> PlayerCharacter_UpdateVRUI(0xEF7180,
            "48 8B C4 55 57 41 56 48 8D 68 A1 48 81 EC ? ? ? ? 48 83 B9 ? ? ? ? 00");
        _PlayerCharacter_UpdateVRUI PlayerCharacter_UpdateVRUI_original = nullptr;

        // The yaw the world-space UI is anchored to, and the yaw of the head it trails, both in radians and
        // both written by the function above. Writing the first after it has run aims the UI for the next
        // frame; the second is what the first is set to whenever the UI recenters on the head.
        constexpr int PLAYER_UI_ANCHOR_YAW_OFFSET = 0x890;
        constexpr int PLAYER_UI_HEAD_YAW_OFFSET = 0x894;

        //--------------------
        // Constants
        //--------------------

        constexpr float PI = 3.14159265f;
        constexpr float TWO_PI = 6.28318531f;
        constexpr float RAD_TO_DEG = 57.29578f;

        // An NPC closer than this has no meaningful bearing from the player.
        constexpr float MIN_TARGET_DISTANCE = 1.0f;

        // What fHmdRotationLagMaxDistance:VRUI is set to while a conversation is up. The engine clamps the
        // anchor yaw to within that many degrees of the head before it builds the UI, and its default of 45
        // would stop the menu reaching an NPC the player has turned away from - measured deltas of over 80
        // degrees are ordinary while looking around mid-conversation. Restored when the dialogue ends.
        constexpr float AIM_LAG_MAX_DISTANCE_DEGREES = 180.0f;

        // How quickly the aim closes on the NPC, as a proportion of the remaining angle per second, which
        // settles in about a fifth of a second. The easing is there because the bearing to an NPC standing
        // close by swings quickly when the player steps sideways, and following it exactly frame for frame
        // makes the menu look like it is swinging around the player.
        constexpr float AIM_SMOOTHING_PER_SECOND = 5.0f;

        // The frame time to fall back on when the engine hands over one that can't be right.
        constexpr float FALLBACK_FRAME_SECONDS = 1.0f / 60.0f;
        constexpr float MAX_FRAME_SECONDS = 0.1f;

        // Set from the menu open/close event, which is only recorded there: everything it leads to is done on
        // the next VR UI update, the one place the anchor yaw is written.
        std::atomic<bool> s_dialogueMenuOpen{false};
        bool s_wasDialogueMenuOpen = false;

        // Whether the UI is being aimed for the dialogue that is open, and so there is something to put back
        // when it closes.
        bool s_aiming = false;

        // Where the menu is currently aimed, as a bearing in the world rather than relative to the head, so
        // that turning the head - or the whole room, when the player turns - doesn't drag it along.
        float s_aimBearing = 0;
        bool s_haveAimBearing = false;

        Setting* s_lagMaxDistance = nullptr;
        float s_savedLagMaxDistance = 0;
        bool s_lagMaxDistanceRaised = false;

        //--------------------
        // Helpers
        //--------------------

        // Wraps an angle into [-pi, pi], so a difference of two angles either side of the wrap reads as small.
        float NormalizeAngle(float radians)
        {
            radians = std::fmod(radians + PI, TWO_PI);
            if (radians < 0) {
                radians += TWO_PI;
            }
            return radians - PI;
        }

        /**
         * A reference resolved from a handle, given back as soon as it goes out of scope.
         *
         * The lookup hands back a counted reference - it does a locked increment of the reference's handle
         * refcount - and that count is only the low ten bits of the field (BSHandleRefObject::kMask_RefCount).
         * Leaking one per frame carries into the handle state kept in the bits above it within about ten
         * seconds, and the actor then crashes the game when the engine next destroys it.
         */
        class BorrowedRef
        {
        public:
            explicit BorrowedRef(const UInt32 handle)
            {
                if (handle) {
                    UInt32 lookupHandle = handle;
                    LookupREFRByHandle(&lookupHandle, &m_ref);
                }
            }

            ~BorrowedRef()
            {
                if (m_ref) {
                    m_ref->handleRefObject.DecRefHandle();
                }
            }

            BorrowedRef(const BorrowedRef&) = delete;
            BorrowedRef& operator=(const BorrowedRef&) = delete;

            TESObjectREFR* Get() const { return m_ref; }

        private:
            TESObjectREFR* m_ref = nullptr;
        };

        /**
         * Lets the anchor yaw sit as far from the head as the NPC needs it to, for as long as the dialogue is
         * open. RestoreLagClamp() puts back the value this found. Only the parsed setting in memory is touched,
         * never the ini on disk.
         */
        void RaiseLagClamp()
        {
            if (!s_lagMaxDistance) {
                s_lagMaxDistance = GameUtils::GetINISetting("fHmdRotationLagMaxDistance:VRUI");
                if (!s_lagMaxDistance) {
                    _WARNING("fHmdRotationLagMaxDistance:VRUI not found, the dialogue UI will not reach past it.");
                    return;
                }
            }

            s_savedLagMaxDistance = s_lagMaxDistance->data.f32;
            s_lagMaxDistance->data.f32 = AIM_LAG_MAX_DISTANCE_DEGREES;
            s_lagMaxDistanceRaised = true;
        }

        void RestoreLagClamp()
        {
            if (s_lagMaxDistanceRaised) {
                s_lagMaxDistance->data.f32 = s_savedLagMaxDistance;
                s_lagMaxDistanceRaised = false;
            }
        }

        /**
         * The dialogue menu has opened: read the MCM switch once for this dialogue and, if it is on, start
         * aiming - on the NPC straight away rather than sweeping onto them from wherever the last one was.
         */
        void StartAiming()
        {
            if (!Settings::GetBool("bEnableDialogueUIFacingNpc:VR", true)) {
                return;
            }

            RaiseLagClamp();
            s_haveAimBearing = false;
            s_aiming = true;
        }

        /**
         * The dialogue menu has closed: put back what aiming changed, so the Pip-Boy, the pause menu and every
         * other world-space menu get the engine's own placement. The setting goes back to the value it had
         * before the dialogue, and the anchor yaw to the head yaw - the value the engine itself recentres it on,
         * where without the aiming it would have been trailing close behind - so the next menu opens in front
         * of the player rather than wherever the NPC was.
         */
        void StopAiming(void* player)
        {
            if (!s_aiming) {
                return;
            }

            RestoreLagClamp();
            const auto anchorYaw = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(player) + PLAYER_UI_ANCHOR_YAW_OFFSET);
            *anchorYaw = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(player) + PLAYER_UI_HEAD_YAW_OFFSET);
            s_aiming = false;
            _MESSAGE("Dialogue menu closed, the UI placement is handed back to the engine.");
        }

        /**
         * Point the world-space UI at the NPC instead of at the head, for the frame after this one.
         *
         * The engine has just set the anchor yaw to trail the head; this moves it on by the angle between the
         * head and the NPC, so the UI ends up facing the NPC and stays there while the head moves - the drift
         * and the recentre jump both being expressed in the value this overwrites.
         *
         * The head yaw the engine keeps at + 0x894 was measured to be the player's own heading (rot.z) to
         * three decimal places, in radians and clockwise from +Y, give or take whole turns - which is the
         * convention a bearing read with atan2(dx, dy) is in, so the two subtract directly.
         */
        void AimUIAtDialogueTarget(void* player, float deltaSeconds)
        {
            const auto playerRef = static_cast<TESObjectREFR*>(player);
            const auto anchorYaw = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(player) + PLAYER_UI_ANCHOR_YAW_OFFSET);
            const float headYaw = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(player) + PLAYER_UI_HEAD_YAW_OFFSET);
            const float heading = playerRef->rot.z;

            // The topic manager holds the speaker for the whole conversation, where the player dialogue action
            // only answers while one is running - hardly ever, at the rate this is asked. Without a target the
            // frame is left to the engine.
            const BorrowedRef target(DialogueEx::GetDialogueTargetHandle());
            if (!target.Get()) {
                return;
            }

            const float dx = target.Get()->pos.x - playerRef->pos.x;
            const float dy = target.Get()->pos.y - playerRef->pos.y;
            if (std::sqrt(dx * dx + dy * dy) < MIN_TARGET_DISTANCE) {
                return;
            }

            const float bearing = std::atan2(dx, dy);
            if (deltaSeconds <= 0 || deltaSeconds > MAX_FRAME_SECONDS) {
                deltaSeconds = FALLBACK_FRAME_SECONDS;
            }

            if (!s_haveAimBearing) {
                s_haveAimBearing = true;
                s_aimBearing = bearing;
            } else {
                // Close the remaining angle by a fixed proportion of what is left each second, which settles
                // quickly without ever arriving with a jolt.
                const float remaining = NormalizeAngle(bearing - s_aimBearing);
                s_aimBearing = NormalizeAngle(s_aimBearing + remaining * (1.0f - std::exp(-AIM_SMOOTHING_PER_SECOND * deltaSeconds)));
            }

            const float delta = NormalizeAngle(s_aimBearing - heading);
            *anchorYaw = headYaw + delta;

            static bool loggedFirstAim = false;
            if (!loggedFirstAim) {
                loggedFirstAim = true;
                _MESSAGE("Dialogue UI aimed at the NPC, %.1f degrees from where the player is looking.", delta * RAD_TO_DEG);
            }
        }

        void OnPlayerCharacter_UpdateVRUI_Hook(void* player, const float deltaSeconds, const bool unk)
        {
            PlayerCharacter_UpdateVRUI_original(player, deltaSeconds, unk);

            if (player != *G::player) {
                return;
            }

            const bool dialogueMenuOpen = s_dialogueMenuOpen;
            if (dialogueMenuOpen != s_wasDialogueMenuOpen) {
                s_wasDialogueMenuOpen = dialogueMenuOpen;
                if (dialogueMenuOpen) {
                    StartAiming();
                } else {
                    StopAiming(player);
                }
            }

            if (s_aiming) {
                AimUIAtDialogueTarget(player, deltaSeconds);
            }
        }
    }

    void OnDialogueMenuOpenClose(const bool open)
    {
        s_dialogueMenuOpen = open;
    }

    void Init()
    {
        if (!PlayerCharacter_UpdateVRUI.IsResolved()) {
            _WARNING("VR UI update not found, the dialogue UI will not face the NPC.");
            return;
        }

        struct PlayerCharacter_UpdateVRUI_Code : Xbyak::CodeGenerator
        {
            PlayerCharacter_UpdateVRUI_Code(void* buf, const uintptr_t funcAddr)
                : CodeGenerator(1024, buf)
            {
                Xbyak::Label retnLabel;

                // Stolen bytes from 140EF7180. The frame pointer it takes from rsp here is its own, so running
                // these from the trampoline is as good as running them in place.
                // mov rax, rsp
                mov(rax, rsp);

                // push rbp
                push(rbp);

                // push rdi
                push(rdi);

                // Jump back to the original after the stolen bytes
                jmp(ptr[rip + retnLabel]);

                L(retnLabel);
                dq(funcAddr + 0x5);
            }
        };

        void* codeBuf = g_localTrampoline.StartAlloc();
        PlayerCharacter_UpdateVRUI_Code code(codeBuf, PlayerCharacter_UpdateVRUI.GetUIntPtr());
        g_localTrampoline.EndAlloc(code.getCurr());

        PlayerCharacter_UpdateVRUI_original = (_PlayerCharacter_UpdateVRUI)codeBuf;
        g_branchTrampoline.Write5Branch(PlayerCharacter_UpdateVRUI.GetUIntPtr(), (uintptr_t)OnPlayerCharacter_UpdateVRUI_Hook);

        _MESSAGE("Dialogue UI aiming hooked at %p.", PlayerCharacter_UpdateVRUI.GetUIntPtr());
    }
}
