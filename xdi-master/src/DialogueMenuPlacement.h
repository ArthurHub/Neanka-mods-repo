#pragma once

/**
 * Keep the VR dialogue UI pointed at the NPC being talked to.
 *
 * The world-space UI is anchored to a yaw the game keeps on the player (+ 0x890). Every frame it drifts that
 * yaw toward wherever the head is looking, holds off for fHmdRotationLagDuration:VRUI, then accelerates after
 * it, and snaps it whenever the head gets further than fHmdRotationLagMaxDistance:VRUI away - which is the
 * drift-then-jump the dialogue menu is subject to, since the menu hangs off the root built from that yaw.
 *
 * So the menu is aimed by writing that yaw: point it at the NPC instead of at the head, every frame, and the
 * engine builds the UI where we want it with its own code. The menu's own node is not touched - it has no
 * offset or rotation of its own to work with, since fDialogueInputX/Y/Z/Pitch:VRUI are all 0 by default and
 * it sits exactly on its parent's origin, which is why placing the menu is not what to hook.
 *
 * The aim eases onto the NPC rather than snapping to them, since the bearing to someone standing close by
 * swings quickly when the player steps sideways. Turning is unaffected either way: the aim is kept as a
 * bearing in the world, so moving the head, or the whole room, leaves it where it is.
 *
 * The engine would otherwise clamp the result to fHmdRotationLagMaxDistance:VRUI, 45 degrees by default, and
 * an NPC further round than that is ordinary, so that setting is raised in memory while a conversation is up
 * and put back when it ends.
 *
 * All of this is only done while the dialogue menu is open. The yaw is shared by every world-space menu, so
 * when the dialogue menu closes the setting is put back and the yaw is handed back to the engine in front of
 * the head, leaving the Pip-Boy, the pause menu and the rest to the engine's own placement.
 */
namespace DialogueMenuPlacement
{
    // Installs the hook. Call after RVAManager::UpdateAddresses().
    void Init();

    // Tells the placement the dialogue menu opened or closed, from the menu open/close event.
    void OnDialogueMenuOpenClose(bool open);
}
