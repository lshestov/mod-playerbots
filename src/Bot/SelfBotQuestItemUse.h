/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTQUESTITEMUSE_H
#define PLAYERBOTS_SELFBOTQUESTITEMUSE_H

#include "ObjectGuid.h"
#include "SharedDefines.h"

class Player;
class Creature;
class WorldObject;

namespace SelfBotQuestItemCasting
{
enum class Status : uint8
{
    Ready,
    Approach,
    Wait,
    Rejected,
    Sent
};

enum class TargetMode : uint8
{
    Explicit,
    NearbyUnit,
    Self
};

struct Result
{
    Status State = Status::Rejected;
    SpellCastResult CastResult = SPELL_FAILED_DONT_REPORT;
    uint32 RetryDelayMs = 0;
    // Movement hints; native CheckCast also accounts for reach, geometry and spell modifiers.
    float MinRange = 0.0f;
    float MaxRange = 0.0f;
    char const* Reason = "invalid-request";
};

struct DamagePause
{
    ObjectGuid PetGuid;
    uint8 PetReactState = 0;
    bool Active = false;
};

struct WeakeningResult
{
    bool Allowed = false;
    // Native maximum damage with critical modifiers, excluding arbitrary script damage hooks.
    uint32 MaxMeleeDamage = 0;
    char const* Reason = "invalid-target";
};

Result Check(Player* player, ObjectGuid itemGuid, uint32 spellId, WorldObject* target,
             TargetMode mode = TargetMode::Explicit);
// Sent confirms dispatch only. The caller must verify native quest or inventory progress.
Result Execute(Player* player, ObjectGuid itemGuid, uint32 spellId, WorldObject* target,
               TargetMode mode = TargetMode::Explicit);

void PauseDamage(Player* player, uint32 preserveItemSpellId, DamagePause& pause);
// Teardown only restores the saved pet reaction; it never resumes attacks or movement.
void RestoreDamage(Player* player, DamagePause& pause, bool teardown = false);
WeakeningResult WeakeningCheck(Player* player, Creature* target);
Result WeakenOnce(Player* player, Creature* target);
}  // namespace SelfBotQuestItemCasting

#endif
