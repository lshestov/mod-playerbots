/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfBotQuestItemUse.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "CellImpl.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Item.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace SelfBotQuestItemCasting
{
namespace
{
constexpr uint32 RetryDelayMs = 500;
constexpr uint32 MaxRetryDelayMs = 5000;
constexpr uint8 CastCount = 1;
constexpr float MaxNearbyRange = 100.0f;
constexpr float NearestTieDistance = 0.01f;
constexpr size_t MaxNearbyCreatures = 256;
constexpr size_t MaxSafetyAuras = 128;
constexpr size_t MaxControlledUnits = 16;
constexpr size_t MaxTargetAttackers = 16;
constexpr float CriticalMultiplier = 2.0f;

Result Reject(SpellCastResult castResult, char const* reason)
{
    Result result;
    result.CastResult = castResult;
    result.Reason = reason;
    return result;
}

Result Wait(SpellCastResult castResult, char const* reason)
{
    Result result = Reject(castResult, reason);
    result.State = Status::Wait;
    result.RetryDelayMs = RetryDelayMs;
    return result;
}

bool SetTarget(SpellCastTargets& targets, WorldObject* target, TargetMode mode)
{
    if (mode == TargetMode::Self || mode == TargetMode::NearbyUnit)
        return true;

    // A dead NPC is still a Unit. CORPSE target flags address player corpses instead.
    if (target->IsCreature())
        targets.SetUnitTarget(target->ToUnit());
    else if (GameObject* gameObject = target->ToGameObject())
        targets.SetGOTarget(gameObject);
    else
        return false;

    return true;
}

struct NearbyTargetVisitor
{
    NearbyTargetVisitor(Player* player, SpellInfo const* info, ConditionList* conditions, float range)
        : PlayerCaster(player), Check(player, player, info, TARGET_CHECK_ENTRY, conditions), Range(range)
    {
    }

    Player* PlayerCaster;
    Acore::WorldObjectSpellTargetCheck Check;
    float Range;
    Creature* Nearest = nullptr;
    float Distance = std::numeric_limits<float>::max();
    size_t Examined = 0;
    bool Complete = true;
    bool Tied = false;

    void Visit(CreatureMapType& creatures)
    {
        if (!Complete)
            return;
        for (auto itr = creatures.begin(); itr != creatures.end(); ++itr)
        {
            if (++Examined > MaxNearbyCreatures)
            {
                Complete = false;
                return;
            }
            Creature* creature = itr->GetSource();
            if (!creature || !creature->IsInWorld() || !creature->InSamePhase(PlayerCaster))
                continue;
            float const distance = creature->GetDistance2d(PlayerCaster->GetPositionX(), PlayerCaster->GetPositionY());
            if (distance >= Range || !Check(creature))
                continue;
            if (distance + NearestTieDistance < Distance)
            {
                Distance = distance;
                Nearest = creature;
                Tied = false;
            }
            else if (std::abs(distance - Distance) <= NearestTieDistance && creature != Nearest)
                Tied = true;
        }
    }

    template <class T>
    void Visit(GridRefMgr<T>&)
    {
    }
};

bool MatchesNearbyTarget(Player* player, WorldObject* target, Spell& spell)
{
    SpellInfo const* info = spell.GetSpellInfo();
    bool hasNearbyTarget = false;
    for (auto const& effect : info->Effects)
    {
        if (!effect.IsEffect())
            continue;
        for (SpellImplicitTargetInfo const& selector : {effect.TargetA, effect.TargetB})
        {
            if (selector.GetSelectionCategory() != TARGET_SELECT_CATEGORY_NEARBY)
                continue;
            if (selector.GetReferenceType() != TARGET_REFERENCE_TYPE_CASTER ||
                selector.GetObjectType() != TARGET_OBJECT_TYPE_UNIT || selector.GetCheckType() != TARGET_CHECK_ENTRY ||
                !effect.ImplicitTargetConditions || effect.ImplicitTargetConditions->empty() ||
                spell.GetSearcherTypeMask(TARGET_OBJECT_TYPE_UNIT, effect.ImplicitTargetConditions) !=
                    GRID_MAP_TYPE_MASK_CREATURE)
                return false;

            float const range = info->GetMaxRange(info->IsPositive(), player, &spell);
            if (!std::isfinite(range) || range <= 0.0f || range > MaxNearbyRange)
                return false;
            NearbyTargetVisitor visitor(player, info, effect.ImplicitTargetConditions, range);
            Cell::VisitObjects(player, visitor, range);
            if (!visitor.Complete || visitor.Tied || visitor.Nearest != target)
                return false;
            hasNearbyTarget = true;
        }
    }
    return hasNearbyTarget;
}

bool HasAdditionalUseEnchantment(Item const* item)
{
    for (uint8 slot = 0; slot < MAX_ENCHANTMENT_SLOT; ++slot)
    {
        auto const* enchantment = sSpellItemEnchantmentStore.LookupEntry(item->GetEnchantmentId(EnchantmentSlot(slot)));
        if (!enchantment)
            continue;

        for (uint8 effect = 0; effect < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++effect)
            if (enchantment->type[effect] == ITEM_ENCHANTMENT_TYPE_USE_SPELL)
                return true;
    }
    return false;
}

bool IsAvailable(Player* player)
{
    return player && player->IsInWorld() && !player->IsDuringRemoveFromWorld() && player->IsAlive() &&
           player->GetSession() && !player->GetSession()->IsSocketClosed() && !player->GetSession()->IsLoggingOut() &&
           !player->GetSession()->PlayerLogout() && player->m_mover == player && !player->IsBeingTeleported() &&
           !player->IsInFlight() && !player->GetVehicle();
}

bool IsOffensive(SpellInfo const* spellInfo) { return !spellInfo || !spellInfo->IsPositive(); }

void StopOffensiveCasts(Unit* unit, uint32 preserveItemSpellId)
{
    unit->AttackStop();
    unit->InterruptSpell(CURRENT_MELEE_SPELL);
    for (CurrentSpellTypes type : {CURRENT_GENERIC_SPELL, CURRENT_CHANNELED_SPELL, CURRENT_AUTOREPEAT_SPELL})
    {
        Spell* spell = unit->GetCurrentSpell(type);
        if (spell && IsOffensive(spell->GetSpellInfo()) && spell->GetSpellInfo()->Id != preserveItemSpellId)
            unit->InterruptSpell(type, true, true, true);
    }
}

bool HasPeriodicHazard(Unit* target)
{
    for (AuraType type : {SPELL_AURA_PERIODIC_DAMAGE, SPELL_AURA_PERIODIC_DAMAGE_PERCENT, SPELL_AURA_PERIODIC_LEECH})
        if (target->HasAuraType(type))
            return true;

    for (AuraType type : {SPELL_AURA_PERIODIC_TRIGGER_SPELL, SPELL_AURA_PERIODIC_TRIGGER_SPELL_WITH_VALUE,
                          SPELL_AURA_PERIODIC_TRIGGER_SPELL_FROM_CLIENT})
    {
        auto const& effects = target->GetAuraEffectsByType(type);
        if (effects.size() > MaxSafetyAuras)
            return true;
        for (AuraEffect const* effect : effects)
            if (IsOffensive(
                    sSpellMgr->GetSpellInfo(effect->GetSpellInfo()->Effects[effect->GetEffIndex()].TriggerSpell)))
                return true;
    }
    return false;
}

bool HasMeleeProcHazard(Player* player)
{
    if (player->m_extraAttacks || player->HasAuraType(SPELL_AURA_DAMAGE_SHIELD))
        return true;
    auto const& auras = player->GetAppliedAuras();
    if (auras.size() > MaxSafetyAuras)
        return true;
    for (auto const& [spellId, application] : auras)
    {
        SpellInfo const* info = application->GetBase()->GetSpellInfo();
        if (info->ProcFlags || sSpellMgr->GetSpellProcEntry(spellId))
            return true;
    }

    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item const* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;
        for (auto const& spell : item->GetTemplate()->Spells)
            if (spell.SpellId && spell.SpellTrigger == ITEM_SPELLTRIGGER_CHANCE_ON_HIT)
                return true;
        for (uint8 enchantSlot = 0; enchantSlot < MAX_ENCHANTMENT_SLOT; ++enchantSlot)
        {
            auto const* enchantment =
                sSpellItemEnchantmentStore.LookupEntry(item->GetEnchantmentId(EnchantmentSlot(enchantSlot)));
            if (!enchantment)
                continue;
            for (uint8 effect = 0; effect < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++effect)
                if (enchantment->type[effect] == ITEM_ENCHANTMENT_TYPE_COMBAT_SPELL ||
                    enchantment->type[effect] == ITEM_ENCHANTMENT_TYPE_DAMAGE)
                    return true;
        }
    }
    return false;
}

uint32 MeleeDamageEstimate(Player* player, Creature* target)
{
    double total = 0.0;
    for (uint8 index = 0; index < MAX_ITEM_PROTO_DAMAGES; ++index)
    {
        SpellSchoolMask const school = player->GetMeleeDamageSchoolMask(BASE_ATTACK, index);
        float minimum = 0.0f;
        float maximum = 0.0f;
        player->CalculateMinMaxDamage(BASE_ATTACK, false, bool(school & SPELL_SCHOOL_MASK_NORMAL), minimum, maximum,
                                      index);
        maximum = std::max(minimum, maximum);
        if (!std::isfinite(maximum) || maximum >= float(std::numeric_limits<uint32>::max()))
            return std::numeric_limits<uint32>::max();
        uint32 damage = uint32(std::ceil(std::max(0.0f, maximum)));
        damage = player->MeleeDamageBonusDone(target, damage, BASE_ATTACK, nullptr, school);
        damage = target->MeleeDamageBonusTaken(player, damage, BASE_ATTACK, nullptr, school);
        float modifier = float(target->GetTotalAuraModifier(SPELL_AURA_MOD_ATTACKER_MELEE_CRIT_DAMAGE));
        modifier +=
            (player->GetTotalAuraMultiplierByMiscMask(SPELL_AURA_MOD_CRIT_DAMAGE_BONUS, school) - 1.0f) * 100.0f;
        modifier += float(
            player->GetTotalAuraModifierByMiscMask(SPELL_AURA_MOD_CRIT_PERCENT_VERSUS, target->GetCreatureTypeMask()));
        total += double(damage) * CriticalMultiplier * std::max(1.0f, 1.0f + modifier / 100.0f);
    }
    if (!std::isfinite(total) || total >= double(std::numeric_limits<uint32>::max()))
        return std::numeric_limits<uint32>::max();
    return uint32(std::ceil(total));
}
}  // namespace

Result Check(Player* player, ObjectGuid itemGuid, uint32 spellId, WorldObject* target, TargetMode mode)
{
    if (!player || !player->IsInWorld() || player->IsDuringRemoveFromWorld() || !player->IsAlive() ||
        !player->GetSession() || player->GetSession()->IsSocketClosed() || player->GetSession()->IsLoggingOut() ||
        player->GetSession()->PlayerLogout())
        return Reject(SPELL_FAILED_DONT_REPORT, "player-unavailable");

    if (player->m_mover != player || player->IsBeingTeleported() || player->IsInFlight() || player->GetVehicle())
        return Wait(SPELL_FAILED_CANT_DO_THAT_RIGHT_NOW, "player-transit-or-controlled");

    if (mode == TargetMode::Self)
    {
        if (target && target != player)
            return Reject(SPELL_FAILED_BAD_TARGETS, "self-mode-external-target");
        target = player;
    }
    if (!target || !target->IsInWorld() || player->GetMap() != target->GetMap() || !target->InSamePhase(player))
        return Reject(SPELL_FAILED_BAD_TARGETS, "target-unavailable");

    if (mode == TargetMode::NearbyUnit && !target->IsCreature())
        return Reject(SPELL_FAILED_BAD_TARGETS, "nearby-mode-requires-creature");

    if (GameObject* gameObject = target->ToGameObject(); gameObject && !gameObject->isSpawned())
        return Reject(SPELL_FAILED_BAD_TARGETS, "gameobject-not-spawned");

    SpellCastTargets targets;
    if (!SetTarget(targets, target, mode))
        return Reject(SPELL_FAILED_BAD_TARGETS, "unsupported-target-type");

    Item* item = player->GetItemByGuid(itemGuid);
    if (!item || item->GetOwnerGUID() != player->GetGUID() ||
        (!Player::IsInventoryPos(item->GetBagSlot(), item->GetSlot()) && !item->IsEquipped()) ||
        player->GetUseableItemByPos(item->GetBagSlot(), item->GetSlot()) != item)
        return Reject(SPELL_FAILED_ITEM_GONE, "exact-item-unavailable");

    if (item->IsInTrade())
        return Wait(SPELL_FAILED_ITEM_NOT_READY, "item-in-trade");

    ItemTemplate const* itemTemplate = item->GetTemplate();
    if (!itemTemplate || (itemTemplate->InventoryType != INVTYPE_NON_EQUIP && !item->IsEquipped()) ||
        player->CanUseItem(item) != EQUIP_ERR_OK)
        return Reject(SPELL_FAILED_EQUIPPED_ITEM, "item-not-usable");

    uint32 useSpellCount = 0;
    bool matchingSpell = false;
    for (auto const& itemSpell : itemTemplate->Spells)
    {
        if (!itemSpell.SpellId || itemSpell.SpellTrigger != ITEM_SPELLTRIGGER_ON_USE)
            continue;

        ++useSpellCount;
        matchingSpell = matchingSpell || uint32(itemSpell.SpellId) == spellId;
    }

    // The native handler casts every ON_USE spell, not just the ID supplied in the packet.
    if (useSpellCount != 1 || !matchingSpell || HasAdditionalUseEnchantment(item))
        return Reject(SPELL_FAILED_BAD_TARGETS, "item-use-spells-not-singleton");

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return Reject(SPELL_FAILED_BAD_TARGETS, "spell-unavailable");

    uint32 const targetMask = spellInfo->GetExplicitTargetMask();
    if (mode == TargetMode::Explicit &&
        ((target->IsCreature() && !(targetMask & (TARGET_FLAG_UNIT_MASK | TARGET_FLAG_CORPSE_MASK))) ||
         (target->IsGameObject() && !(targetMask & TARGET_FLAG_GAMEOBJECT_MASK))))
        return Reject(SPELL_FAILED_BAD_TARGETS, "spell-has-no-matching-explicit-target");

    if (mode != TargetMode::Explicit &&
        (targetMask & (TARGET_FLAG_UNIT_MASK | TARGET_FLAG_CORPSE_MASK | TARGET_FLAG_GAMEOBJECT_MASK)))
        return Reject(SPELL_FAILED_BAD_TARGETS, "implicit-mode-requires-explicit-target");

    if (!player->GetLootGUID().IsEmpty() || player->IsNonMeleeSpellCast(false) || !player->SpellQueue.empty())
        return Wait(SPELL_FAILED_SPELL_IN_PROGRESS, "player-busy");

    Result result;
    result.MinRange = player->GetSpellMinRangeForTarget(target->ToUnit(), spellInfo);
    result.MaxRange = player->GetSpellMaxRangeForTarget(target->ToUnit(), spellInfo);

    // This mirrors the native preflight; prepare() later loads scripts and repeats final checks.
    Spell spell(player, spellInfo, TRIGGERED_NONE);
    spell.m_CastItem = item;
    spell.InitExplicitTargets(targets);
    if (mode == TargetMode::Explicit && spell.m_targets.GetObjectTargetGUID() != target->GetGUID())
        return Reject(SPELL_FAILED_BAD_TARGETS, "native-target-changed");

    if (mode != TargetMode::Explicit && !spell.m_targets.GetObjectTargetGUID().IsEmpty())
        return Reject(SPELL_FAILED_BAD_TARGETS, "unexpected-native-object-target");

    if (mode == TargetMode::NearbyUnit && !MatchesNearbyTarget(player, target, spell))
        return Reject(SPELL_FAILED_BAD_TARGETS, "nearby-target-not-proven");

    result.CastResult = spell.CheckCast(true);
    switch (result.CastResult)
    {
        case SPELL_CAST_OK:
            if (player->isMoving() || !player->IsStopped())
            {
                result.State = Status::Wait;
                result.CastResult = SPELL_FAILED_MOVING;
                result.RetryDelayMs = RetryDelayMs;
                result.Reason = "stop-for-item-use";
            }
            else
            {
                result.State = Status::Ready;
                result.Reason = "native-check-passed";
            }
            break;
        case SPELL_FAILED_OUT_OF_RANGE:
        case SPELL_FAILED_TOO_CLOSE:
        case SPELL_FAILED_LINE_OF_SIGHT:
        case SPELL_FAILED_NOT_INFRONT:
            result.State = Status::Approach;
            result.Reason = "native-position-check";
            break;
        case SPELL_FAILED_NOT_READY:
        case SPELL_FAILED_ITEM_NOT_READY:
        case SPELL_FAILED_SPELL_IN_PROGRESS:
        case SPELL_FAILED_MOVING:
        case SPELL_FAILED_NOT_STANDING:
        case SPELL_FAILED_TRY_AGAIN:
        case SPELL_FAILED_AFFECTING_COMBAT:
        case SPELL_FAILED_TARGET_AFFECTING_COMBAT:
            result.State = Status::Wait;
            result.RetryDelayMs = std::clamp(player->GetSpellCooldownDelay(spellId), RetryDelayMs, MaxRetryDelayMs);
            result.Reason = "native-temporary-check";
            break;
        default:
            result.State = Status::Rejected;
            result.Reason = "native-check-rejected";
            break;
    }
    return result;
}

Result Execute(Player* player, ObjectGuid itemGuid, uint32 spellId, WorldObject* target, TargetMode mode)
{
    Result result = Check(player, itemGuid, spellId, target, mode);
    if (result.CastResult == SPELL_FAILED_MOVING && result.State == Status::Wait)
    {
        player->StopMoving();
        return result;
    }

    if (result.State != Status::Ready && result.CastResult != SPELL_FAILED_NOT_INFRONT &&
        result.CastResult != SPELL_FAILED_NOT_STANDING)
        return result;

    if (!player->IsStandState())
        player->SetStandState(UNIT_STAND_STATE_STAND);
    if (target && target != player)
        player->SetFacingToObject(target);

    result = Check(player, itemGuid, spellId, target, mode);
    if (result.State != Status::Ready)
        return result;

    Item* item = player->GetItemByGuid(itemGuid);
    if (!item)
        return Reject(SPELL_FAILED_ITEM_GONE, "exact-item-unavailable");

    SpellCastTargets targets;
    if (!SetTarget(targets, target, mode))
        return Reject(SPELL_FAILED_BAD_TARGETS, "unsupported-target-type");

    WorldPacket packet(CMSG_USE_ITEM);
    packet << uint8(item->GetBagSlot()) << uint8(item->GetSlot()) << CastCount << spellId << itemGuid << uint32(0)
           << uint8(0);
    targets.Write(packet);
    player->GetSession()->HandleUseItemOpcode(packet);

    result.State = Status::Sent;
    result.RetryDelayMs = RetryDelayMs;
    result.Reason = "native-item-use-dispatched";
    return result;
}

void PauseDamage(Player* player, uint32 preserveItemSpellId, DamagePause& pause)
{
    if (!IsAvailable(player))
        return;
    StopOffensiveCasts(player, preserveItemSpellId);
    std::erase_if(player->SpellQueue,
                  [preserveItemSpellId](PendingSpellCastRequest const& request)
                  {
                      return !(request.isItem && request.spellId == preserveItemSpellId) &&
                             IsOffensive(sSpellMgr->GetSpellInfo(request.spellId));
                  });

    Pet* pet = player->GetPet();
    if (!pet || !pet->IsInWorld() || !pet->IsAlive() || (pause.Active && pause.PetGuid != pet->GetGUID()))
        return;
    if (!pause.Active)
    {
        pause.PetGuid = pet->GetGUID();
        pause.PetReactState = uint8(pet->GetReactState());
        pause.Active = true;
    }
    pet->SetReactState(REACT_PASSIVE);
    StopOffensiveCasts(pet, 0);
    if (CharmInfo* charm = pet->GetCharmInfo())
    {
        charm->SetIsCommandAttack(false);
        if (charm->GetForcedSpell() && IsOffensive(sSpellMgr->GetSpellInfo(charm->GetForcedSpell())))
        {
            charm->SetForcedSpell(0);
            charm->SetForcedTargetGUID();
        }
    }
}

void RestoreDamage(Player* player, DamagePause& pause, bool teardown)
{
    if (!pause.Active || !player || (!teardown && !IsAvailable(player)))
        return;
    if (player->FindMap())
        if (Pet* pet = player->GetPet(); pet && pet->GetGUID() == pause.PetGuid && pet->IsInWorld() &&
                                         !pet->IsDuringRemoveFromWorld() && pet->GetMap() == player->FindMap() &&
                                         pet->GetOwnerGUID() == player->GetGUID())
            if (pet->HasReactState(REACT_PASSIVE))
                pet->SetReactState(ReactStates(pause.PetReactState));
    pause = {};
}

WeakeningResult WeakeningCheck(Player* player, Creature* target)
{
    WeakeningResult result;
    if (!IsAvailable(player) || !target || !target->IsInWorld() || !target->IsAlive() ||
        player->GetMap() != target->GetMap() || !target->InSamePhase(player) || !player->IsValidAttackTarget(target))
        return result;
    if (HasPeriodicHazard(target))
    {
        result.Reason = "target-periodic-damage";
        return result;
    }
    if (target->HasAuraType(SPELL_AURA_ADD_CASTER_HIT_TRIGGER))
    {
        result.Reason = "melee-target-may-redirect";
        return result;
    }
    if (target->getAttackers().size() > MaxTargetAttackers || player->m_Controlled.size() > MaxControlledUnits)
    {
        result.Reason = "damage-owner-check-budget";
        return result;
    }
    for (Unit* attacker : target->getAttackers())
        if (attacker != player)
        {
            result.Reason = "other-target-attacker";
            return result;
        }
    Pet* pet = player->GetPet();
    if (pet && (!pet->HasReactState(REACT_PASSIVE) || pet->GetVictim() ||
                (pet->GetCharmInfo() && pet->GetCharmInfo()->IsCommandAttack())))
    {
        result.Reason = "unpaused-pet";
        return result;
    }
    for (Unit* controlled : player->m_Controlled)
        if (controlled != pet)
        {
            result.Reason = "unpaused-controlled-unit";
            return result;
        }
    if (player->IsNonMeleeSpellCast(true) || player->GetCurrentSpell(CURRENT_MELEE_SPELL) ||
        !player->SpellQueue.empty() || (pet && pet->IsNonMeleeSpellCast(true)))
    {
        result.Reason = "cast-still-active";
        return result;
    }
    if (HasMeleeProcHazard(player))
    {
        result.Reason = "unbounded-melee-proc";
        return result;
    }
    result.MaxMeleeDamage = MeleeDamageEstimate(player, target);
    if (!result.MaxMeleeDamage || result.MaxMeleeDamage >= target->GetHealth())
    {
        result.Reason = "melee-could-kill-target";
        return result;
    }
    result.Allowed = true;
    result.Reason = "single-melee-feasible";
    return result;
}

Result WeakenOnce(Player* player, Creature* target)
{
    WeakeningResult const safety = WeakeningCheck(player, target);
    if (!safety.Allowed)
        return Reject(SPELL_FAILED_BAD_TARGETS, safety.Reason);
    if (!player->isAttackReady(BASE_ATTACK) || player->IsActionPreventedByCasting() ||
        player->HasUnitState(UNIT_STATE_LOST_CONTROL) || player->HasUnitFlag(UNIT_FLAG_PACIFIED) ||
        !player->CanUseAttackType(BASE_ATTACK))
        return Wait(SPELL_FAILED_NOT_READY, "native-melee-not-ready");
    if (!player->IsWithinMeleeRange(target) || !player->IsWithinLOSInMap(target))
    {
        Result result = Reject(SPELL_FAILED_OUT_OF_RANGE, "native-melee-position");
        result.State = Status::Approach;
        return result;
    }
    player->AttackStop();
    if (!player->IsStandState())
        player->SetStandState(UNIT_STAND_STATE_STAND);
    player->SetFacingToObject(target);
    player->AttackerStateUpdate(target, BASE_ATTACK, false, false);
    player->resetAttackTimer(BASE_ATTACK);
    player->AttackStop();
    Result result;
    result.State = Status::Sent;
    result.CastResult = SPELL_CAST_OK;
    result.RetryDelayMs = RetryDelayMs;
    result.Reason = "native-single-melee-swing";
    return result;
}
}  // namespace SelfBotQuestItemCasting
