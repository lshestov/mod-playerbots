/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfBotQuestCatalog.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "ConditionMgr.h"
#include "DBCStores.h"
#include "DisableMgr.h"
#include "GameEventMgr.h"
#include "GridDefines.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "MapMgr.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PoolMgr.h"
#include "SharedValueContext.h"
#include "SmartScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
using SourceMap = std::unordered_map<int32, SelfBotQuestSources>;
constexpr float GIVER_CELL_SIZE = 500.0f;
constexpr float CREATURE_CELL_SIZE = 100.0f;
constexpr size_t MAX_CORRIDOR_POINTS = 2048;
constexpr size_t MAX_CORRIDOR_STEPS = 8192;
constexpr size_t MAX_CORRIDOR_DISTANCE_CHECKS = 262144;
constexpr size_t MAX_ITEM_SPELL_NODES = 32;
constexpr uint32 MAX_ITEM_SPELL_DEPTH = 4;
constexpr size_t MAX_ITEM_TARGET_CONDITIONS = 64;
constexpr size_t MAX_ITEM_SMART_ROWS = 256;
constexpr float MAX_ITEM_NEARBY_RANGE = 100.0f;
constexpr float MAX_ITEM_SUMMON_RADIUS = 40.0f;
constexpr uint32 SUMMON_GUARDIAN_FLAG = 512;
constexpr size_t MAX_ITEM_LOCATION_POINTS = 64;
constexpr size_t MAX_ITEM_LOCATION_POLYGONS = 8;
constexpr size_t MAX_ITEM_LOCATION_CELLS = 256;
constexpr size_t MAX_ITEM_LOCATION_SPAWNS = 4096;
constexpr size_t MAX_ITEM_LOCATION_CHECKS = 8;
constexpr size_t MAX_ITEM_LOCATION_GRIDS = 32;
constexpr Targets NO_ITEM_TARGET = Targets(0);

struct ItemProgress
{
    uint32 Entry;
    bool ItemObjective;
    bool CasterEffect;
};

struct ItemTarget
{
    int32 Entry = 0;
    uint32 SpawnId = 0;
    bool RequiresDead = false;
    bool RequiresAlive = false;
    bool RequiresUnit = false;
    bool Supported = true;
    bool RejectScriptOverrides = false;
    float MinHealthPct = 0.0f;
    float MaxHealthPct = 100.0f;
    bool MinHealthExclusive = false;
    bool MaxHealthExclusive = false;
};

struct ItemSpellAnalysis
{
    std::vector<ItemProgress> Progress;
    std::vector<ItemTarget> Targets;
    bool Supported = true;
    bool UtilityOnly = false;
};

struct ItemUseDraft
{
    SelfBotQuestItemUse Use;
    std::vector<ItemTarget> Targets;
    uint32 AreaGroupId = 0;
};

struct ItemSmartIndex
{
    std::unordered_map<uint32, std::vector<SmartScriptHolder>> Hits;
    std::unordered_map<uint32, SmartAIEventList> Scripts;
};

struct QuestItemAnalysis
{
    std::vector<ItemUseDraft> Uses;
    std::array<bool, QUEST_OBJECTIVES_COUNT> Objectives{};
    std::array<bool, QUEST_ITEM_OBJECTIVES_COUNT> Items{};
    bool UnresolvedTool = false;
};

bool IsUtilityAura(AuraType aura)
{
    switch (aura)
    {
        case SPELL_AURA_WATER_BREATHING:
        case SPELL_AURA_MOD_WATER_BREATHING:
        case SPELL_AURA_MOD_INCREASE_SWIM_SPEED:
        case SPELL_AURA_WATER_WALK:
        case SPELL_AURA_SAFE_FALL:
        case SPELL_AURA_MOD_INCREASE_SPEED:
        case SPELL_AURA_PERIODIC_HEAL:
        case SPELL_AURA_PERIODIC_ENERGIZE:
        case SPELL_AURA_MOD_REGEN:
        case SPELL_AURA_MOD_POWER_REGEN:
        case SPELL_AURA_MOD_HEALTH_REGEN_PERCENT:
        case SPELL_AURA_MOD_POWER_REGEN_PERCENT:
        case SPELL_AURA_MOD_STAT:
        case SPELL_AURA_MOD_RESISTANCE:
        case SPELL_AURA_MOD_INCREASE_HEALTH:
        case SPELL_AURA_MOD_INCREASE_ENERGY:
            return true;
        default:
            return false;
    }
}

bool IsUtilityItemSpell(SpellInfo const* spell)
{
    auto const scripts = sObjectMgr->GetSpellScriptsBounds(spell->Id);
    if (scripts.first != scripts.second || spell->RequiresSpellFocus || !spell->IsPositive())
        return false;
    bool hasAura = false;
    for (SpellEffectInfo const& effect : spell->Effects)
    {
        if (!effect.Effect)
            continue;
        if (effect.Effect != SPELL_EFFECT_APPLY_AURA || !IsUtilityAura(effect.ApplyAuraName) || effect.TriggerSpell ||
            effect.TargetA.GetTarget() != TARGET_UNIT_CASTER || effect.TargetB.GetTarget() != NO_ITEM_TARGET ||
            (effect.ImplicitTargetConditions && !effect.ImplicitTargetConditions->empty()))
            return false;
        hasAura = true;
    }
    return hasAura;
}

bool IsNativeItemTrigger(uint32 effect)
{
    return effect == SPELL_EFFECT_TRIGGER_SPELL || effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE ||
           effect == SPELL_EFFECT_TRIGGER_SPELL_2;
}

bool IsSimpleItemTarget(SpellImplicitTargetInfo const& target)
{
    return target.GetTarget() == NO_ITEM_TARGET || target.GetTarget() == TARGET_UNIT_CASTER ||
           (target.GetSelectionCategory() == TARGET_SELECT_CATEGORY_DEFAULT &&
            target.GetReferenceType() == TARGET_REFERENCE_TYPE_TARGET &&
            (target.GetObjectType() == TARGET_OBJECT_TYPE_UNIT || target.GetObjectType() == TARGET_OBJECT_TYPE_GOBJ));
}

bool ValidItemTarget(ItemTarget const& target)
{
    return target.Supported && !(target.RequiresAlive && target.RequiresDead) &&
           !(target.Entry < 0 && target.RequiresUnit) &&
           (target.MinHealthPct < target.MaxHealthPct ||
            (target.MinHealthPct == target.MaxHealthPct && !target.MinHealthExclusive && !target.MaxHealthExclusive));
}

bool RequiresItemWeakening(ItemTarget const& target)
{
    return target.MaxHealthPct < 100.0f || target.MaxHealthExclusive;
}

void ApplyItemCondition(ItemTarget& branch, Condition const& condition, uint8 targetIndex, bool allowCaster)
{
    if (condition.ReferenceId || condition.ScriptId)
    {
        branch.Supported = false;
        return;
    }
    if (condition.ConditionTarget != targetIndex)
    {
        if (allowCaster && condition.ConditionTarget == 0)
            switch (condition.ConditionType)
            {
                case CONDITION_QUESTTAKEN:
                case CONDITION_QUESTSTATE:
                case CONDITION_QUEST_COMPLETE:
                case CONDITION_ITEM:
                case CONDITION_AURA:
                    return;
                default:
                    break;
            }
        branch.Supported = false;
        return;
    }
    if (condition.ConditionType == CONDITION_ALIVE)
    {
        branch.RequiresUnit = true;
        branch.RequiresDead |= condition.NegativeCondition;
        branch.RequiresAlive |= !condition.NegativeCondition;
        return;
    }
    if (condition.ConditionType == CONDITION_OBJECT_ENTRY_GUID && !condition.NegativeCondition &&
        condition.ConditionValue2 &&
        (condition.ConditionValue1 == TYPEID_UNIT || condition.ConditionValue1 == TYPEID_GAMEOBJECT))
    {
        int32 const entry = condition.ConditionValue1 == TYPEID_GAMEOBJECT ? -int32(condition.ConditionValue2)
                                                                           : int32(condition.ConditionValue2);
        uint32 const spawnId = condition.ConditionValue3 > 1 ? condition.ConditionValue3 : 0;
        if ((branch.Entry && branch.Entry != entry) || (branch.SpawnId && spawnId && branch.SpawnId != spawnId))
            branch.Supported = false;
        branch.Entry = entry;
        branch.SpawnId = std::max(branch.SpawnId, spawnId);
        return;
    }
    if (condition.ConditionType != CONDITION_HP_PCT || condition.ConditionValue1 > 100)
    {
        branch.Supported = false;
        return;
    }
    uint32 comparison = condition.ConditionValue2;
    branch.RequiresUnit = true;
    if (condition.NegativeCondition)
        switch (comparison)
        {
            case COMP_TYPE_HIGH:
                comparison = COMP_TYPE_LOW_EQ;
                break;
            case COMP_TYPE_LOW:
                comparison = COMP_TYPE_HIGH_EQ;
                break;
            case COMP_TYPE_HIGH_EQ:
                comparison = COMP_TYPE_LOW;
                break;
            case COMP_TYPE_LOW_EQ:
                comparison = COMP_TYPE_HIGH;
                break;
            default:
                branch.Supported = false;
                return;
        }
    float const percent = float(condition.ConditionValue1);
    if (comparison == COMP_TYPE_HIGH || comparison == COMP_TYPE_HIGH_EQ || comparison == COMP_TYPE_EQ)
    {
        if (percent > branch.MinHealthPct)
        {
            branch.MinHealthPct = percent;
            branch.MinHealthExclusive = comparison == COMP_TYPE_HIGH;
        }
        else if (percent == branch.MinHealthPct)
            branch.MinHealthExclusive |= comparison == COMP_TYPE_HIGH;
    }
    if (comparison == COMP_TYPE_LOW || comparison == COMP_TYPE_LOW_EQ || comparison == COMP_TYPE_EQ)
    {
        if (percent < branch.MaxHealthPct)
        {
            branch.MaxHealthPct = percent;
            branch.MaxHealthExclusive = comparison == COMP_TYPE_LOW;
        }
        else if (percent == branch.MaxHealthPct)
            branch.MaxHealthExclusive |= comparison == COMP_TYPE_LOW;
    }
    if (comparison >= COMP_TYPE_MAX)
        branch.Supported = false;
}

std::vector<ItemTarget> ConditionTargets(ConditionList const& conditions, ItemTarget const& initial, uint8 targetIndex,
                                         bool allowCaster)
{
    if (conditions.size() > MAX_ITEM_TARGET_CONDITIONS)
        return {};
    if (conditions.empty())
        return {initial};
    std::map<uint32, ItemTarget> branches;
    for (Condition const* condition : conditions)
    {
        auto found = branches.try_emplace(condition->ElseGroup, initial).first;
        ApplyItemCondition(found->second, *condition, targetIndex, allowCaster);
    }
    std::vector<ItemTarget> result;
    for (auto const& [group, branch] : branches)
        if (ValidItemTarget(branch))
            result.push_back(branch);
    return result;
}

void SetItemHealthWindow(SelfBotQuestItemUse& use, ItemTarget const& target)
{
    use.RequiresDead = target.RequiresDead;
    use.MinHealthPct = target.MinHealthPct;
    use.MaxHealthPct = target.MaxHealthPct;
    use.MinHealthExclusive = target.MinHealthExclusive;
    use.MaxHealthExclusive = target.MaxHealthExclusive;
}

ItemTarget InitialItemTarget(SpellInfo const* spell)
{
    ItemTarget result;
    if (spell->TargetAuraState == AURA_STATE_HEALTHLESS_20_PERCENT ||
        spell->TargetAuraState == AURA_STATE_HEALTHLESS_35_PERCENT)
    {
        result.MaxHealthPct = spell->TargetAuraState == AURA_STATE_HEALTHLESS_20_PERCENT ? 20.0f : 35.0f;
        result.MaxHealthExclusive = true;
    }
    return result;
}

ItemSpellAnalysis AnalyzeItemSpell(uint32 spellId)
{
    ItemSpellAnalysis result;
    SpellInfo const* root = sSpellMgr->GetSpellInfo(spellId);
    if (!root)
    {
        result.Supported = false;
        return result;
    }
    result.UtilityOnly = IsUtilityItemSpell(root);

    std::vector<std::pair<uint32, uint32>> pending{{spellId, 0}};
    std::unordered_set<uint32> visited;
    while (!pending.empty())
    {
        auto const [id, depth] = pending.back();
        pending.pop_back();
        if (!visited.insert(id).second)
            continue;
        if (visited.size() > MAX_ITEM_SPELL_NODES || depth > MAX_ITEM_SPELL_DEPTH)
        {
            result.Supported = false;
            break;
        }
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(id);
        if (!spell)
        {
            result.Supported = false;
            continue;
        }
        if (depth && !sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_SPELL, id).empty())
            result.Supported = false;
        for (SpellEffectInfo const& effect : spell->Effects)
        {
            if (!effect.Effect)
                continue;
            bool const casterEffect =
                effect.TargetA.GetTarget() == TARGET_UNIT_CASTER && effect.TargetB.GetTarget() == NO_ITEM_TARGET;
            if (effect.Effect == SPELL_EFFECT_CREATE_ITEM && effect.ItemType)
                result.Progress.push_back({effect.ItemType, true, casterEffect});
            if ((effect.Effect == SPELL_EFFECT_KILL_CREDIT || effect.Effect == SPELL_EFFECT_KILL_CREDIT2) &&
                effect.MiscValue > 0)
                result.Progress.push_back({uint32(effect.MiscValue), false, casterEffect});
            if (IsNativeItemTrigger(effect.Effect) && effect.TriggerSpell)
                pending.emplace_back(effect.TriggerSpell, depth + 1);
            if (!IsSimpleItemTarget(effect.TargetA) || !IsSimpleItemTarget(effect.TargetB) ||
                (effect.ImplicitTargetConditions && !effect.ImplicitTargetConditions->empty()))
                result.Supported = false;
            switch (effect.Effect)
            {
                case SPELL_EFFECT_DUMMY:
                case SPELL_EFFECT_CREATE_ITEM:
                case SPELL_EFFECT_KILL_CREDIT:
                case SPELL_EFFECT_KILL_CREDIT2:
                case SPELL_EFFECT_APPLY_AURA:
                case SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE:
                    break;
                // The other trigger handlers contain special branches or discard the explicit target.
                case SPELL_EFFECT_TRIGGER_SPELL:
                case SPELL_EFFECT_TRIGGER_SPELL_2:
                default:
                    result.Supported = false;
                    break;
            }
        }
    }

    uint32 const mask = root->GetExplicitTargetMask();
    bool const unitTarget = (mask & TARGET_FLAG_UNIT_MASK) != 0;
    bool const objectTarget = (mask & TARGET_FLAG_GAMEOBJECT) != 0;
    float const minRange = std::max(root->GetMinRange(false), root->GetMinRange(true));
    float const maxRange = root->GetMaxRange(root->IsPositive());
    if (unitTarget == objectTarget || root->RequiresSpellFocus || root->IsRequiringDeadTarget() ||
        !std::isfinite(minRange) || minRange != 0.0f || !std::isfinite(maxRange) || maxRange <= 0.0f)
        result.Supported = false;
    ConditionList const conditions =
        sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_SPELL, spellId);
    if (conditions.empty() || conditions.size() > MAX_ITEM_TARGET_CONDITIONS)
    {
        result.Supported = false;
        return result;
    }

    for (ItemTarget const& branch : ConditionTargets(conditions, InitialItemTarget(root), 1, true))
        if (branch.Entry && ((unitTarget && branch.Entry > 0) || (objectTarget && branch.Entry < 0)) &&
            (!branch.RequiresDead || root->IsAllowingDeadTarget()))
            result.Targets.push_back(branch);
    return result;
}

ItemSmartIndex BuildItemSmartIndex()
{
    ItemSmartIndex result;
    for (auto const& [entry, creature] : *sObjectMgr->GetCreatureTemplates())
    {
        if (creature.AIName != "SmartAI" || creature.ScriptID)
            continue;
        SmartAIEventList const rows = sSmartScriptMgr->GetScript(int32(entry), SMART_SCRIPT_TYPE_CREATURE);
        if (rows.size() > MAX_ITEM_SMART_ROWS)
            continue;
        result.Scripts.emplace(entry, rows);
        for (SmartScriptHolder const& row : rows)
            if (row.event.type == SMART_EVENT_SPELLHIT && row.event.spellHit.spell)
                result.Hits[row.event.spellHit.spell].push_back(row);
    }
    return result;
}

bool IsBenignItemReaction(SmartScriptHolder const& row, SmartAIEventList const& script)
{
    switch (row.action.type)
    {
        case SMART_ACTION_TALK:
            return std::none_of(script.begin(), script.end(), [](SmartScriptHolder const& other)
                                { return other.event.type == SMART_EVENT_TEXT_OVER; });
        case SMART_ACTION_SIMPLE_TALK:
        case SMART_ACTION_SOUND:
        case SMART_ACTION_PLAY_EMOTE:
            return true;
        case SMART_ACTION_ATTACK_START:
            return row.target.type == SMART_TARGET_ACTION_INVOKER;
        default:
            return false;
    }
}

bool IsBenignItemReactionChain(SmartScriptHolder const& first, SmartAIEventList const& script)
{
    SmartScriptHolder const* row = &first;
    std::unordered_set<uint32> visited;
    while (row)
    {
        if (visited.size() >= MAX_ITEM_SPELL_NODES || !visited.insert(row->event_id).second ||
            !IsBenignItemReaction(*row, script))
            return false;
        if (!row->link)
            return true;
        auto const next =
            std::find_if(script.begin(), script.end(), [&](SmartScriptHolder const& candidate)
                         { return candidate.event_id == row->link && candidate.event.type == SMART_EVENT_LINK; });
        if (next == script.end())
            return false;
        row = &*next;
    }
    return true;
}

bool HasOnlyKnownItemReactions(SmartScriptHolder const& selected, SpellInfo const* spell, ItemSmartIndex const& smart)
{
    auto const found = smart.Scripts.find(uint32(selected.entryOrGuid));
    if (found == smart.Scripts.end())
        return false;
    for (SmartScriptHolder const& other : found->second)
        if (other.event.type == SMART_EVENT_SPELLHIT && other.event_id != selected.event_id &&
            (!other.event.spellHit.spell || other.event.spellHit.spell == spell->Id) &&
            (!other.event.spellHit.school || (spell->SchoolMask & other.event.spellHit.school)) &&
            !IsBenignItemReactionChain(other, found->second))
            return false;
    return true;
}

bool HasBenignItemSpellHit(uint32 entry, SpellInfo const* spell, ItemSmartIndex const& smart)
{
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
    if (!creature || creature->ScriptID)
        return false;
    if (creature->AIName != "SmartAI")
        return creature->AIName.empty() || creature->AIName == "AggressorAI" || creature->AIName == "PassiveAI";
    SmartScriptHolder noSelectedEvent;
    noSelectedEvent.entryOrGuid = int32(entry);
    noSelectedEvent.event_id = std::numeric_limits<uint32>::max();
    return HasOnlyKnownItemReactions(noSelectedEvent, spell, smart);
}

bool HasKnownItemResult(uint32 entry, bool summoned, ItemSmartIndex const& smart)
{
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
    if (!creature || creature->ScriptID || (creature->flags_extra & CREATURE_FLAG_EXTRA_TRIGGER) ||
        (creature->unit_flags & (UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_SELECTABLE | UNIT_FLAG_IMMUNE_TO_PC)))
        return false;
    if (!summoned)
        return true;
    if (creature->AIName != "SmartAI")
        return creature->AIName.empty() || creature->AIName == "AggressorAI" || creature->AIName == "PassiveAI";
    auto const found = smart.Scripts.find(entry);
    if (found == smart.Scripts.end())
        return false;
    for (SmartScriptHolder const& row : found->second)
        switch (row.event.type)
        {
            case SMART_EVENT_AI_INIT:
            case SMART_EVENT_JUST_CREATED:
            case SMART_EVENT_JUST_SUMMONED:
            case SMART_EVENT_RESPAWN:
            case SMART_EVENT_RESET:
                if (!IsBenignItemReactionChain(row, found->second))
                    return false;
                break;
            default:
                break;
        }
    return true;
}

bool HasItemSpellScript(uint32 spellId)
{
    auto const scripts = sObjectMgr->GetSpellScriptsBounds(spellId);
    return scripts.first != scripts.second;
}

bool IsNearbyItemTarget(SpellImplicitTargetInfo const& target)
{
    return target.GetSelectionCategory() == TARGET_SELECT_CATEGORY_NEARBY &&
           target.GetReferenceType() == TARGET_REFERENCE_TYPE_CASTER &&
           target.GetObjectType() == TARGET_OBJECT_TYPE_UNIT;
}

struct ItemTargetPlan
{
    SelfBotQuestItemTargetMode Mode = SelfBotQuestItemTargetMode::Explicit;
    std::vector<ItemTarget> Targets;
    bool Supported = false;
};

ItemTargetPlan ItemSpellTargetPlan(SpellInfo const* spell)
{
    ItemTargetPlan result;
    if (spell->RequiresSpellFocus || spell->IsRequiringDeadTarget() ||
        std::max(spell->GetMinRange(false), spell->GetMinRange(true)) != 0.0f)
        return result;
    ConditionList const conditions =
        sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_SPELL, spell->Id);
    uint32 const mask = spell->GetExplicitTargetMask();
    bool const unitTarget = mask & TARGET_FLAG_UNIT_MASK;
    bool const objectTarget = mask & TARGET_FLAG_GAMEOBJECT;
    if (objectTarget)
        return result;
    if (unitTarget || objectTarget)
    {
        float const range = spell->GetMaxRange(spell->IsPositive());
        if (!std::isfinite(range) || range <= 0.0f)
            return result;
        result.Targets = ConditionTargets(conditions, InitialItemTarget(spell), 1, true);
        result.Targets.erase(std::remove_if(result.Targets.begin(), result.Targets.end(),
                                            [&](ItemTarget const& target)
                                            {
                                                return (target.Entry && ((target.Entry > 0) != unitTarget)) ||
                                                       (target.RequiresDead && !spell->IsAllowingDeadTarget());
                                            }),
                             result.Targets.end());
        result.Supported = !result.Targets.empty();
        return result;
    }
    result.Mode = SelfBotQuestItemTargetMode::Self;
    auto const areaRules = sSpellMgr->GetSpellAreaMapBounds(spell->Id);
    if (areaRules.first != areaRules.second)
        return result;
    result.Targets = ConditionTargets(conditions, InitialItemTarget(spell), 1, true);
    if (result.Targets.empty())
        return result;
    ConditionList const* implicit = nullptr;
    for (SpellEffectInfo const& effect : spell->Effects)
        if (effect.Effect && (IsNearbyItemTarget(effect.TargetA) || IsNearbyItemTarget(effect.TargetB)))
        {
            float const range = spell->GetMaxRange(spell->IsPositive());
            if (!std::isfinite(range) || range <= 0.0f || range > MAX_ITEM_NEARBY_RANGE ||
                !effect.ImplicitTargetConditions || effect.ImplicitTargetConditions->empty() ||
                (implicit && implicit != effect.ImplicitTargetConditions))
                return result;
            implicit = effect.ImplicitTargetConditions;
        }
    if (implicit)
    {
        if (spell->AreaGroupId > 0)
            return result;
        result.Mode = SelfBotQuestItemTargetMode::NearbyUnit;
        std::vector<ItemTarget> selected;
        for (ItemTarget const& initial : result.Targets)
            for (ItemTarget const& target : ConditionTargets(*implicit, initial, 0, false))
                if (target.Entry > 0 && (!target.RequiresDead || spell->IsAllowingDeadTarget()))
                    selected.push_back(target);
        result.Targets = std::move(selected);
    }
    else if (std::any_of(result.Targets.begin(), result.Targets.end(),
                         [](ItemTarget const& target)
                         {
                             return target.Entry || target.RequiresUnit || target.RequiresDead || target.MinHealthPct ||
                                    target.MinHealthExclusive || RequiresItemWeakening(target);
                         }))
        return result;
    result.Supported = !result.Targets.empty();
    return result;
}

bool IsStagedItemTarget(SpellImplicitTargetInfo const& target)
{
    return IsSimpleItemTarget(target) || IsNearbyItemTarget(target) || target.GetTarget() == TARGET_DEST_CASTER ||
           target.GetTarget() == TARGET_DEST_TARGET_ENEMY || target.GetTarget() == TARGET_DEST_TARGET_ANY ||
           target.GetTarget() == TARGET_SRC_CASTER || target.GetTarget() == TARGET_DEST_CASTER_FRONT;
}

bool IsSingleWildSummon(SpellEffectInfo const& effect);

bool IsKnownWeakeningAura(SpellEffectInfo const& effect)
{
    if (effect.TriggerSpell ||
        (effect.TargetA.GetReferenceType() != TARGET_REFERENCE_TYPE_TARGET && !IsNearbyItemTarget(effect.TargetA)))
        return false;
    switch (effect.ApplyAuraName)
    {
        case SPELL_AURA_MOD_STUN:
        case SPELL_AURA_MOD_ROOT:
            return true;
        case SPELL_AURA_MOD_DECREASE_SPEED:
        case SPELL_AURA_MOD_STAT:
        case SPELL_AURA_MOD_RESISTANCE:
        case SPELL_AURA_MOD_RESISTANCE_PCT:
        case SPELL_AURA_MOD_DAMAGE_DONE:
        case SPELL_AURA_MOD_DAMAGE_PERCENT_DONE:
            return effect.RealPointsPerLevel == 0.0f && effect.PointsPerComboPoint == 0.0f &&
                   int64(effect.BasePoints) + std::max(effect.DieSides, int32(1)) < 0;
        default:
            return false;
    }
}

bool IsStagedSpellSafe(SpellInfo const* spell)
{
    if (HasItemSpellScript(spell->Id) || spell->RequiresSpellFocus || spell->IsChanneled())
        return false;
    for (SpellEffectInfo const& effect : spell->Effects)
    {
        if (!effect.Effect)
            continue;
        if (!IsStagedItemTarget(effect.TargetA) || !IsStagedItemTarget(effect.TargetB))
            return false;
        switch (effect.Effect)
        {
            case SPELL_EFFECT_DUMMY:
                if (sSpellScripts.count(spell->Id | (uint32(effect.EffectIndex) << 24)) ||
                    sSpellMgr->GetPetAura(spell->Id, effect.EffectIndex))
                    return false;
                break;
            case SPELL_EFFECT_APPLY_AURA:
                if (!IsKnownWeakeningAura(effect))
                    return false;
                break;
            case SPELL_EFFECT_CREATE_ITEM:
            case SPELL_EFFECT_KILL_CREDIT:
            case SPELL_EFFECT_KILL_CREDIT2:
            case SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE:
                break;
            case SPELL_EFFECT_SUMMON:
                if (!IsSingleWildSummon(effect))
                    return false;
                break;
            default:
                return false;
        }
    }
    return true;
}

bool IsSingleWildSummon(SpellEffectInfo const& effect)
{
    SummonPropertiesEntry const* properties = sSummonPropertiesStore.LookupEntry(effect.MiscValueB);
    float const radius = effect.RadiusEntry ? effect.RadiusEntry->RadiusMax : 0.0f;
    return effect.MiscValue > 0 && properties && properties->Category == SUMMON_CATEGORY_WILD &&
           !(properties->Flags & SUMMON_GUARDIAN_FLAG) &&
           (properties->Type == SUMMON_TYPE_NONE || properties->Type == SUMMON_TYPE_WILD2 ||
            properties->Type == SUMMON_TYPE_WILD3) &&
           effect.RealPointsPerLevel == 0.0f && effect.PointsPerComboPoint == 0.0f &&
           (effect.DieSides == 0 || effect.DieSides == 1) && int64(effect.BasePoints) + effect.DieSides <= 1 &&
           std::isfinite(radius) && radius >= 0.0f && radius <= MAX_ITEM_SUMMON_RADIUS;
}

std::vector<ItemUseDraft> AnalyzeStagedItemSpell(uint32 spellId, ItemSmartIndex const& smart)
{
    std::vector<ItemUseDraft> result;
    SpellInfo const* root = sSpellMgr->GetSpellInfo(spellId);
    if (!root)
        return result;
    ItemTargetPlan const plan = ItemSpellTargetPlan(root);
    if (!plan.Supported)
        return result;
    std::vector<SpellInfo const*> chain;
    size_t summonEffects = 0;
    std::vector<std::pair<uint32, uint32>> pending{{spellId, 0}};
    std::unordered_set<uint32> visited;
    while (!pending.empty())
    {
        auto const [id, depth] = pending.back();
        pending.pop_back();
        if (!visited.insert(id).second)
            continue;
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(id);
        if (!spell || visited.size() > MAX_ITEM_SPELL_NODES || depth > MAX_ITEM_SPELL_DEPTH ||
            !IsStagedSpellSafe(spell) ||
            (depth && !sConditionMgr->GetConditionsForNotGroupedEntry(CONDITION_SOURCE_TYPE_SPELL, id).empty()))
            return {};
        chain.push_back(spell);
        for (SpellEffectInfo const& effect : spell->Effects)
        {
            // Value triggers replace child aura amounts and may otherwise retarget through child selectors.
            if (depth && (effect.Effect == SPELL_EFFECT_APPLY_AURA || IsNearbyItemTarget(effect.TargetA) ||
                          IsNearbyItemTarget(effect.TargetB) ||
                          (effect.ImplicitTargetConditions && !effect.ImplicitTargetConditions->empty())))
                return {};
            if (effect.Effect == SPELL_EFFECT_SUMMON && (depth || ++summonEffects > 1))
                return {};
            if (effect.Effect == SPELL_EFFECT_TRIGGER_SPELL_WITH_VALUE && effect.TriggerSpell)
            {
                // A child may reuse the selected unit only when the native trigger forwards that unit.
                if (effect.TargetA.GetReferenceType() != TARGET_REFERENCE_TYPE_TARGET ||
                    effect.TargetB.GetTarget() != NO_ITEM_TARGET)
                    return {};
                pending.emplace_back(effect.TriggerSpell, depth + 1);
            }
        }
    }
    auto append = [&](SelfBotQuestItemUse use, ItemTarget target)
    {
        if (!ValidItemTarget(target) || target.RequiresDead ||
            (RequiresItemWeakening(target) && !root->CanBeUsedInCombat()))
            return;
        use.SpellId = spellId;
        use.TargetMode = plan.Mode;
        target.RejectScriptOverrides = plan.Mode != SelfBotQuestItemTargetMode::Self;
        SetItemHealthWindow(use, target);
        for (ItemUseDraft const& previous : result)
            if (previous.Use.Kind == use.Kind && previous.Use.ResultEntry == use.ResultEntry &&
                previous.Use.ResultAura == use.ResultAura && previous.Targets.front().Entry == target.Entry &&
                previous.Targets.front().SpawnId == target.SpawnId && previous.Use.MinHealthPct == use.MinHealthPct &&
                previous.Use.MaxHealthPct == use.MaxHealthPct &&
                previous.Use.MinHealthExclusive == use.MinHealthExclusive &&
                previous.Use.MaxHealthExclusive == use.MaxHealthExclusive)
                return;
        result.push_back(
            {use,
             {target},
             plan.Mode == SelfBotQuestItemTargetMode::Self && root->AreaGroupId > 0 ? uint32(root->AreaGroupId) : 0});
    };
    for (SpellInfo const* spell : chain)
    {
        for (SpellEffectInfo const& effect : spell->Effects)
        {
            bool const physicalTarget =
                effect.TargetA.GetReferenceType() == TARGET_REFERENCE_TYPE_TARGET || IsNearbyItemTarget(effect.TargetA);
            if (effect.Effect == SPELL_EFFECT_APPLY_AURA && physicalTarget && !spell->IsPositive() &&
                effect.ApplyAuraName != SPELL_AURA_DUMMY)
                for (ItemTarget const& target : plan.Targets)
                    if (target.Entry > 0 && HasBenignItemSpellHit(uint32(target.Entry), spell, smart) &&
                        HasKnownItemResult(uint32(target.Entry), false, smart))
                    {
                        SelfBotQuestItemUse use;
                        use.Kind = SelfBotQuestItemKind::Weaken;
                        use.ResultEntry = uint32(target.Entry);
                        use.ResultAura = spell->Id;
                        append(use, target);
                    }
            if (effect.Effect == SPELL_EFFECT_SUMMON && IsSingleWildSummon(effect))
                for (ItemTarget const& target : plan.Targets)
                {
                    if (plan.Mode != SelfBotQuestItemTargetMode::Self &&
                        (target.Entry <= 0 || !HasBenignItemSpellHit(uint32(target.Entry), spell, smart)))
                        continue;
                    if (plan.Mode == SelfBotQuestItemTargetMode::Self &&
                        effect.TargetA.GetTarget() != TARGET_DEST_CASTER &&
                        effect.TargetB.GetTarget() != TARGET_DEST_CASTER &&
                        effect.TargetA.GetTarget() != TARGET_DEST_CASTER_FRONT &&
                        effect.TargetB.GetTarget() != TARGET_DEST_CASTER_FRONT)
                        continue;
                    float const radius = effect.RadiusEntry ? effect.RadiusEntry->RadiusMax : 0.0f;
                    if (!std::isfinite(radius) || radius < 0.0f || radius > MAX_ITEM_SUMMON_RADIUS)
                        continue;
                    SelfBotQuestItemUse use;
                    use.Kind = SelfBotQuestItemKind::Summon;
                    use.ResultEntry = uint32(effect.MiscValue);
                    if (HasKnownItemResult(use.ResultEntry, true, smart))
                        append(use, target);
                }
        }
        auto const found = smart.Hits.find(spell->Id);
        bool const hitsPhysicalTarget = std::any_of(
            std::begin(spell->Effects), std::end(spell->Effects),
            [](SpellEffectInfo const& effect)
            {
                return effect.Effect && (effect.TargetA.GetReferenceType() == TARGET_REFERENCE_TYPE_TARGET ||
                                         IsNearbyItemTarget(effect.TargetA));
            });
        if (found == smart.Hits.end() || !hitsPhysicalTarget || plan.Mode == SelfBotQuestItemTargetMode::Self ||
            (plan.Mode == SelfBotQuestItemTargetMode::Explicit &&
             !(root->GetExplicitTargetMask() & TARGET_FLAG_UNIT_MASK)) ||
            found->second.size() > MAX_ITEM_SMART_ROWS)
            continue;
        for (SmartScriptHolder const& row : found->second)
        {
            if (row.link || row.event.event_phase_mask || row.event.event_chance != 100 ||
                (row.event.event_flags & ~(SMART_EVENT_FLAG_WHILE_CHARMED | SMART_EVENT_FLAG_DONT_RESET)) ||
                row.target.type != SMART_TARGET_SELF ||
                (row.event.spellHit.school && !(spell->SchoolMask & row.event.spellHit.school)) ||
                !HasOnlyKnownItemReactions(row, spell, smart))
                continue;
            SelfBotQuestItemUse use;
            if (row.action.type == SMART_ACTION_UPDATE_TEMPLATE)
            {
                use.Kind = SelfBotQuestItemKind::Transform;
                use.ResultEntry = row.action.updateTemplate.creature;
            }
            else if (row.action.type == SMART_ACTION_SUMMON_CREATURE &&
                     (row.action.summonCreature.flags & uint32(SmartActionSummonCreatureFlags::PreferUnit)) &&
                     row.target.x == 0.0f && row.target.y == 0.0f && row.target.z == 0.0f)
            {
                use.Kind = SelfBotQuestItemKind::Summon;
                use.ResultEntry = row.action.summonCreature.creature;
            }
            else
                continue;
            if (!use.ResultEntry ||
                !HasKnownItemResult(use.ResultEntry, use.Kind == SelfBotQuestItemKind::Summon, smart))
                continue;
            for (ItemTarget target : plan.Targets)
            {
                if (target.Entry && target.Entry != row.entryOrGuid)
                    continue;
                target.Entry = row.entryOrGuid;
                target.RejectScriptOverrides = true;
                ConditionList const conditions = sConditionMgr->GetConditionsForSmartEvent(
                    row.entryOrGuid, row.event_id, SMART_SCRIPT_TYPE_CREATURE);
                for (ItemTarget const& branch : ConditionTargets(conditions, target, 1, true))
                    append(use, branch);
            }
        }
    }
    return result;
}

bool ResultGivesObjective(uint32 entry, int32 objective)
{
    if (int32(entry) == objective)
        return true;
    CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry);
    return creature && objective > 0 &&
           std::find(std::begin(creature->KillCredit), std::end(creature->KillCredit), uint32(objective)) !=
               std::end(creature->KillCredit);
}

QuestItemAnalysis AnalyzeQuestItems(Quest const* quest, std::unordered_map<uint32, ItemSpellAnalysis>& spells,
                                    std::unordered_map<uint32, std::vector<ItemUseDraft>>& staged,
                                    ItemSmartIndex const& smart, DropMap const* drops)
{
    QuestItemAnalysis result;
    std::set<uint32> tools{quest->GetSrcItemId()};
    tools.insert(std::begin(quest->ItemDrop), std::end(quest->ItemDrop));
    std::set<uint32> items = tools;
    items.insert(std::begin(quest->RequiredItemId), std::end(quest->RequiredItemId));
    items.erase(0);
    for (uint32 itemId : items)
    {
        ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemId);
        if (!item)
            continue;
        std::set<uint32> uses;
        size_t useSlots = 0;
        for (auto const& spell : item->Spells)
            if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_USE)
            {
                uses.insert(uint32(spell.SpellId));
                ++useSlots;
            }
        for (uint32 spellId : uses)
        {
            size_t const previousBindings = result.Uses.size();
            auto found = spells.find(spellId);
            if (found == spells.end())
                found = spells.emplace(spellId, AnalyzeItemSpell(spellId)).first;
            ItemSpellAnalysis const& analysis = found->second;
            for (ItemProgress const& progress : analysis.Progress)
            {
                uint32 const count = progress.ItemObjective ? QUEST_ITEM_OBJECTIVES_COUNT : QUEST_OBJECTIVES_COUNT;
                for (uint32 index = 0; index < count; ++index)
                {
                    bool const matches =
                        progress.ItemObjective
                            ? quest->RequiredItemCount[index] && quest->RequiredItemId[index] == progress.Entry
                            : quest->RequiredNpcOrGoCount[index] &&
                                  quest->RequiredNpcOrGo[index] == int32(progress.Entry);
                    if (!matches)
                        continue;
                    if (progress.ItemObjective)
                        result.Items[index] = true;
                    else
                        result.Objectives[index] = true;
                    if (!analysis.Supported || !progress.CasterEffect || useSlots != 1)
                        continue;
                    for (ItemTarget const& target : analysis.Targets)
                    {
                        if (RequiresItemWeakening(target) && !sSpellMgr->GetSpellInfo(spellId)->CanBeUsedInCombat())
                            continue;
                        ItemUseDraft draft;
                        draft.Use.ItemId = itemId;
                        draft.Use.SpellId = spellId;
                        draft.Use.Objective = uint8(index);
                        draft.Use.ItemObjective = progress.ItemObjective;
                        SetItemHealthWindow(draft.Use, target);
                        if (RequiresItemWeakening(target))
                            draft.Use.Kind = SelfBotQuestItemKind::Weaken;
                        draft.Targets.push_back(target);
                        result.Uses.push_back(std::move(draft));
                    }
                }
            }
            if (tools.count(itemId) && useSlots == 1 && !item->ScriptId)
            {
                auto outcomes = staged.find(spellId);
                if (outcomes == staged.end())
                    outcomes = staged.emplace(spellId, AnalyzeStagedItemSpell(spellId, smart)).first;
                for (ItemUseDraft const& outcome : outcomes->second)
                {
                    for (uint32 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
                        if (quest->RequiredNpcOrGoCount[index] &&
                            ResultGivesObjective(outcome.Use.ResultEntry, quest->RequiredNpcOrGo[index]))
                        {
                            ItemUseDraft draft = outcome;
                            draft.Use.ItemId = itemId;
                            draft.Use.Objective = uint8(index);
                            draft.Use.Continuation = SelfBotQuestItemContinuation::Kill;
                            result.Objectives[index] = true;
                            result.Uses.push_back(std::move(draft));
                        }
                    for (uint32 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
                    {
                        if (!quest->RequiredItemCount[index])
                            continue;
                        auto const range = drops->equal_range(quest->RequiredItemId[index]);
                        bool const dropsItem = std::any_of(range.first, range.second, [&](auto const& drop)
                                                           { return drop.second == int32(outcome.Use.ResultEntry); });
                        if (!dropsItem)
                            continue;
                        ItemUseDraft draft = outcome;
                        draft.Use.ItemId = itemId;
                        draft.Use.Objective = uint8(index);
                        draft.Use.ItemObjective = true;
                        draft.Use.Continuation = SelfBotQuestItemContinuation::Loot;
                        result.Items[index] = true;
                        result.Uses.push_back(std::move(draft));
                    }
                }
            }
            // A supplied tool with unknown behavior cannot safely become an ordinary kill objective.
            if (tools.count(itemId) && (!analysis.UtilityOnly || item->ScriptId) &&
                result.Uses.size() == previousBindings)
                result.UnresolvedTool = true;
        }
    }
    return result;
}

int32 GiverCell(float coordinate) { return int32(std::floor(coordinate / GIVER_CELL_SIZE)); }

std::set<uint32> ItemLocationAreas(uint32 groupId)
{
    std::set<uint32> result;
    std::unordered_set<uint32> visited;
    while (groupId)
    {
        if (visited.size() >= MAX_ITEM_SPELL_NODES || !visited.insert(groupId).second)
            return {};
        AreaGroupEntry const* group = sAreaGroupStore.LookupEntry(groupId);
        if (!group)
            return {};
        for (uint32 areaId : group->AreaId)
            if (areaId)
            {
                if (!sAreaTableStore.LookupEntry(areaId))
                    return {};
                result.insert(areaId);
            }
        groupId = group->nextGroup;
    }
    return result;
}

struct ItemLocationPolygon
{
    QuestPOI const* Poi;
    float MinX;
    float MaxX;
    float MinY;
    float MaxY;
    double CenterX;
    double CenterY;
};

bool ItemPolygonEdgesIntersect(QuestPOIPoint const& a, QuestPOIPoint const& b, QuestPOIPoint const& c,
                               QuestPOIPoint const& d)
{
    auto cross = [](QuestPOIPoint const& first, QuestPOIPoint const& second, QuestPOIPoint const& third)
    {
        return (double(second.x) - first.x) * (double(third.y) - first.y) -
               (double(second.y) - first.y) * (double(third.x) - first.x);
    };
    auto onEdge = [](QuestPOIPoint const& first, QuestPOIPoint const& second, QuestPOIPoint const& point)
    {
        return point.x >= std::min(first.x, second.x) && point.x <= std::max(first.x, second.x) &&
               point.y >= std::min(first.y, second.y) && point.y <= std::max(first.y, second.y);
    };
    double const first = cross(a, b, c);
    double const second = cross(a, b, d);
    double const third = cross(c, d, a);
    double const fourth = cross(c, d, b);
    if ((!first && onEdge(a, b, c)) || (!second && onEdge(a, b, d)) || (!third && onEdge(c, d, a)) ||
        (!fourth && onEdge(c, d, b)))
        return true;
    return ((first < 0.0) != (second < 0.0)) && ((third < 0.0) != (fourth < 0.0));
}

bool ItemPolygon(QuestPOI const& poi, ItemLocationPolygon& polygon)
{
    if (poi.points.size() < 3 || poi.points.size() > MAX_ITEM_LOCATION_POINTS)
        return false;
    polygon = {&poi,
               float(poi.points.front().x),
               float(poi.points.front().x),
               float(poi.points.front().y),
               float(poi.points.front().y),
               0.0,
               0.0};
    double area = 0.0;
    for (size_t index = 0; index < poi.points.size(); ++index)
    {
        QuestPOIPoint const& point = poi.points[index];
        QuestPOIPoint const& next = poi.points[(index + 1) % poi.points.size()];
        if (!Acore::IsValidMapCoord(float(point.x), float(point.y), 0.0f, 0.0f) ||
            (point.x == next.x && point.y == next.y))
            return false;
        for (size_t other = index + 1; other < poi.points.size(); ++other)
        {
            if (other == index + 1 || (index == 0 && other == poi.points.size() - 1))
                continue;
            if (ItemPolygonEdgesIntersect(point, next, poi.points[other], poi.points[(other + 1) % poi.points.size()]))
                return false;
        }
        polygon.MinX = std::min(polygon.MinX, float(point.x));
        polygon.MaxX = std::max(polygon.MaxX, float(point.x));
        polygon.MinY = std::min(polygon.MinY, float(point.y));
        polygon.MaxY = std::max(polygon.MaxY, float(point.y));
        polygon.CenterX += point.x;
        polygon.CenterY += point.y;
        area += double(point.x) * next.y - double(point.y) * next.x;
    }
    polygon.CenterX /= poi.points.size();
    polygon.CenterY /= poi.points.size();
    return std::abs(area) > 0.0;
}

bool InsideItemPolygon(WorldPosition const& position, ItemLocationPolygon const& polygon)
{
    double const x = position.GetPositionX();
    double const y = position.GetPositionY();
    if (x < polygon.MinX || x > polygon.MaxX || y < polygon.MinY || y > polygon.MaxY)
        return false;
    bool inside = false;
    auto const& points = polygon.Poi->points;
    for (size_t index = 0, previous = points.size() - 1; index < points.size(); previous = index++)
    {
        double const ax = points[previous].x;
        double const ay = points[previous].y;
        double const bx = points[index].x;
        double const by = points[index].y;
        double const cross = (x - ax) * (by - ay) - (y - ay) * (bx - ax);
        if (cross == 0.0 && x >= std::min(ax, bx) && x <= std::max(ax, bx) && y >= std::min(ay, by) &&
            y <= std::max(ay, by))
            return true;
        if ((ay > y) != (by > y) && x < ax + (y - ay) * (bx - ax) / (by - ay))
            inside = !inside;
    }
    return inside;
}

int32 CreatureCell(float coordinate) { return int32(std::floor(coordinate / CREATURE_CELL_SIZE)); }

float SegmentDistanceSquared(G3D::Vector3 const& point, G3D::Vector3 const& start, G3D::Vector3 const& end)
{
    G3D::Vector3 const segment = end - start;
    float const lengthSquared = segment.squaredLength();
    float const fraction =
        lengthSquared > 0.0f ? std::clamp((point - start).dot(segment) / lengthSquared, 0.0f, 1.0f) : 0.0f;
    return (point - (start + segment * fraction)).squaredLength();
}

bool ValidPosition(WorldPosition const& position)
{
    return Acore::IsValidMapCoord(position.GetPositionX(), position.GetPositionY(), position.GetPositionZ(),
                                  position.GetOrientation());
}

void AppendSources(SelfBotQuestSources& target, SourceMap const& sources, int32 entry)
{
    auto const found = sources.find(entry);
    if (found != sources.end())
        target.insert(target.end(), found->second.begin(), found->second.end());
}

bool IsSeasonalQuest(Quest const* quest)
{
    if (quest->GetEventIdForQuest())
        return true;
    // The native IsSeasonal() is a reward-reset predicate and omits repeatable holiday quests.
    switch (quest->GetZoneOrSort())
    {
        case -QUEST_SORT_SEASONAL:
        case -QUEST_SORT_DARKMOON_FAIRE:
        case -QUEST_SORT_AHN_QIRAJ_WAR:
        case -QUEST_SORT_LUNAR_FESTIVAL:
        case -QUEST_SORT_INVASION:
        case -QUEST_SORT_MIDSUMMER:
        case -QUEST_SORT_BREWFEST:
        case -QUEST_SORT_NOBLEGARDEN:
        case -QUEST_SORT_PILGRIMS_BOUNTY:
        case -QUEST_SORT_LOVE_IS_IN_THE_AIR:
            return true;
        default:
            return false;
    }
}

bool HasItemUse(SelfBotQuestDefinition const& definition, uint32 objective, bool itemObjective)
{
    return std::any_of(
        definition.ItemUses.begin(), definition.ItemUses.end(), [&](SelfBotQuestItemUse const& use)
        { return use.Objective == objective && use.ItemObjective == itemObjective && !use.Sources.empty(); });
}

std::string UnsupportedReason(Quest const* quest, SelfBotQuestDefinition const& definition)
{
    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) && definition.ExplorationSources.empty())
        return "scripted event, escort or exploration has no supported native trigger on open-world maps";
    if (quest->GetPlayersSlain())
        return "player-kill objectives are outside this executor";
    if (quest->GetRepObjectiveFaction() || quest->GetRepObjectiveFaction2())
        return "reputation objective requires a separate task";
    if (quest->GetTimeAllowed())
        return "timed objective scheduling is not implemented";
    if (definition.Takers.empty())
        return "no quest turn-in spawn on supported open-world maps";
    if (definition.Givers.empty())
        return "no quest giver spawn on supported open-world maps";

    for (uint32 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
    {
        if (!quest->RequiredNpcOrGoCount[index])
            continue;
        if (definition.ObjectiveSources[index].empty() && !HasItemUse(definition, index, false))
            return "required creature or object has no known spawn on supported open-world maps";
    }

    return {};
}

std::string TakeQuestReason(Player* player, Quest const* quest)
{
    if (sDisableMgr->IsDisabledFor(DISABLE_TYPE_QUEST, quest->GetQuestId(), player))
        return "disabled by server";
    if (!player->SatisfyQuestStatus(quest, false))
        return "already in quest journal or previously rewarded";
    if (!player->SatisfyQuestClass(quest, false))
        return "class requirement";
    if (!player->SatisfyQuestRace(quest, false))
        return "race requirement";
    if (!player->SatisfyQuestLevel(quest, false))
        return "level requirement";
    if (!player->SatisfyQuestSkill(quest, false))
        return "skill requirement";
    if (!player->SatisfyQuestReputation(quest, false))
        return "reputation requirement";
    if (!player->SatisfyQuestPreviousQuest(quest, false))
        return "previous quest requirement";
    if (!player->SatisfyQuestExclusiveGroup(quest, false))
        return "exclusive quest branch";
    if (!player->SatisfyQuestNextChain(quest, false) || !player->SatisfyQuestPrevChain(quest, false) ||
        !player->SatisfyQuestBreadcrumb(quest, false))
        return "quest chain or breadcrumb requirement";
    if (!player->SatisfyQuestTimed(quest, false))
        return "another timed quest is active";
    if (!player->SatisfyQuestDay(quest, false) || !player->SatisfyQuestWeek(quest, false) ||
        !player->SatisfyQuestMonth(quest, false) || !player->SatisfyQuestSeasonal(quest, false))
        return "daily, weekly, monthly or seasonal limit";
    if (!player->SatisfyQuestConditions(quest, false))
        return "server quest conditions";
    return "CanTakeQuest rejected the quest";
}

bool CanAddQuietly(Player* player, Quest const* quest, std::string& reason)
{
    if (!player->SatisfyQuestLog(false))
    {
        reason = "quest journal is full";
        return false;
    }
    if (quest->GetSrcItemId())
    {
        ItemPosCountVec destinations;
        InventoryResult result =
            player->CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, quest->GetSrcItemId(), quest->GetSrcItemCount());
        // CanAddQuest sends an inventory error even with msg=false; avoid repeating it during list refreshes.
        if (result != EQUIP_ERR_OK && result != EQUIP_ERR_CANT_CARRY_MORE_OF_THIS)
        {
            reason = "cannot store the quest source item";
            return false;
        }
    }
    if (player->CanAddQuest(quest, false))
        return true;
    reason = "CanAddQuest rejected journal or source item requirements";
    return false;
}

void HashValue(uint64& hash, uint64 value)
{
    constexpr uint64 HASH_PRIME = 1099511628211ULL;
    hash = (hash ^ value) * HASH_PRIME;
}
}  // namespace

SelfBotQuestCatalog& SelfBotQuestCatalog::Instance()
{
    static SelfBotQuestCatalog instance;
    return instance;
}

bool SelfBotQuestCatalog::IsAreaInRegion(uint32 areaId) { return Instance()._areas.count(areaId) != 0; }

bool SelfBotQuestCatalog::IsMapSupported(uint32 mapId) { return Instance()._maps.count(mapId) != 0; }

bool SelfBotQuestCatalog::IsInRegion(WorldPosition const& position)
{
    return IsMapSupported(position.GetMapId()) && ValidPosition(position);
}

void SelfBotQuestCatalog::Initialize()
{
    if (_initialized)
        return;

    for (uint32 index = 0; index < sMapStore.GetNumRows(); ++index)
        if (MapEntry const* map = sMapStore.LookupEntry(index);
            map && map->IsWorldMap() && !sObjectMgr->IsTransportMap(map->MapID))
            _maps.insert(map->MapID);
    for (uint32 index = 0; index < sAreaTableStore.GetNumRows(); ++index)
        if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(index); area && IsMapSupported(area->mapid))
            _areas.insert(area->ID);

    std::unordered_set<uint32> eventCreatures;
    std::unordered_set<uint32> eventGameObjects;
    for (auto const& spawns : sGameEventMgr->GameEventCreatureGuids)
        eventCreatures.insert(spawns.begin(), spawns.end());
    for (auto const& spawns : sGameEventMgr->GameEventGameobjectGuids)
        eventGameObjects.insert(spawns.begin(), spawns.end());

    std::unordered_set<uint32> eventQuests;
    auto collectEventQuests = [&](auto const& events)
    {
        for (auto const& relations : events)
            for (auto const& [entry, questId] : relations)
                eventQuests.insert(questId);
    };
    collectEventQuests(sGameEventMgr->GetEventCreatureQuestMap());
    collectEventQuests(sGameEventMgr->GetEventGameObjectQuestMap());

    std::unordered_set<uint32> visitedEventPools;
    std::vector<uint32> eventPools;
    auto queueEventPool = [&](uint32 poolId)
    {
        if (sPoolMgr->GetPoolTemplate(poolId) && visitedEventPools.insert(poolId).second)
            eventPools.push_back(poolId);
    };
    for (auto const& pools : sGameEventMgr->GetEventPoolIds())
        for (uint32 poolId : pools)
            queueEventPool(poolId);
    auto visitPoolObjects = [](auto const* group, auto&& visit)
    {
        if (!group)
            return;
        for (PoolObject const& object : group->GetExplicitlyChanced())
            visit(object.guid);
        for (PoolObject const& object : group->GetEqualChanced())
            visit(object.guid);
    };
    // Each loaded pool is visited once; nested pools and cycles cannot expand this traversal indefinitely.
    for (size_t index = 0; index < eventPools.size(); ++index)
    {
        uint32 const poolId = eventPools[index];
        visitPoolObjects(sPoolMgr->GetPoolCreatureGroup(poolId),
                         [&](uint32 spawnId) { eventCreatures.insert(spawnId); });
        visitPoolObjects(sPoolMgr->GetPoolGameObjectGroup(poolId),
                         [&](uint32 spawnId) { eventGameObjects.insert(spawnId); });
        visitPoolObjects(sPoolMgr->GetPoolPoolGroup(poolId), queueEventPool);
    }

    std::unordered_set<int32> requiredEntries;
    auto collectRelations = [&](QuestRelations const* relations, bool gameObjects)
    {
        for (auto const& [entry, questId] : *relations)
            requiredEntries.insert(gameObjects ? -int32(entry) : int32(entry));
    };
    collectRelations(sObjectMgr->GetCreatureQuestRelationMap(), false);
    collectRelations(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), false);
    collectRelations(sObjectMgr->GetGOQuestRelationMap(), true);
    collectRelations(sObjectMgr->GetGOQuestInvolvedRelationMap(), true);

    std::unordered_map<uint32, std::vector<int32>> itemEntries;
    std::unordered_map<uint32, ItemSpellAnalysis> itemSpells;
    std::unordered_map<uint32, std::vector<ItemUseDraft>> stagedItems;
    ItemSmartIndex const smartItems = BuildItemSmartIndex();
    std::unordered_map<uint32, QuestItemAnalysis> questItems;
    DropMap const* drops = sSharedValueContext.getGlobalValue<DropMap*>("drop map")->Get();
    ObjectMgr::QuestMap const& quests = sObjectMgr->GetQuestTemplates();
    std::vector<uint32> questIds;
    questIds.reserve(quests.size());
    for (auto const& [questId, quest] : quests)
    {
        questIds.push_back(questId);
        for (uint32 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
            if (quest->RequiredNpcOrGoCount[index] && quest->RequiredNpcOrGo[index])
                requiredEntries.insert(quest->RequiredNpcOrGo[index]);
        for (uint32 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
        {
            uint32 const itemId = quest->RequiredItemId[index];
            if (!quest->RequiredItemCount[index] || !itemId || itemEntries.count(itemId))
                continue;
            std::vector<int32>& entries = itemEntries[itemId];
            auto const range = drops->equal_range(itemId);
            for (auto drop = range.first; drop != range.second; ++drop)
                entries.push_back(drop->second);
            std::sort(entries.begin(), entries.end());
            entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
            requiredEntries.insert(entries.begin(), entries.end());
        }
        QuestItemAnalysis analysis = AnalyzeQuestItems(quest, itemSpells, stagedItems, smartItems, drops);
        for (ItemUseDraft const& draft : analysis.Uses)
            for (ItemTarget const& target : draft.Targets)
                if (target.Entry)
                    requiredEntries.insert(target.Entry);
        questItems.emplace(questId, std::move(analysis));
    }
    std::sort(questIds.begin(), questIds.end());
    _definitions.reserve(questIds.size());
    for (uint32 questId : questIds)
    {
        _definitionIndex.emplace(questId, _definitions.size());
        SelfBotQuestDefinition definition;
        definition.Id = questId;
        definition.Seasonal = IsSeasonalQuest(quests.at(questId));
        if (definition.Seasonal)
            definition.ExclusionReason = "seasonal or game-event quest excluded by policy";
        else if (eventQuests.count(questId))
            definition.ExclusionReason = "event-bound quest relation excluded by policy";
        _definitions.push_back(std::move(definition));
    }

    auto storeSource = [&](SelfBotQuestSource source, bool questSource)
    {
        _sources.push_back(std::move(source));
        SelfBotQuestSource const* stored = &_sources.back();
        if (!questSource)
            return stored;
        WorldPosition const& position = stored->Position;
        MapIndex& map = _mapIndex[position.GetMapId()];
        float const x = position.GetPositionX();
        float const y = position.GetPositionY();
        float const z = position.GetPositionZ();
        if (!map.HasSources)
        {
            map.MinX = map.MaxX = x;
            map.MinY = map.MaxY = y;
            map.MinZ = map.MaxZ = z;
            map.HasSources = true;
        }
        else
        {
            map.MinX = std::min(map.MinX, x);
            map.MaxX = std::max(map.MaxX, x);
            map.MinY = std::min(map.MinY, y);
            map.MaxY = std::max(map.MaxY, y);
            map.MinZ = std::min(map.MinZ, z);
            map.MaxZ = std::max(map.MaxZ, z);
        }
        return stored;
    };
    SourceMap sources;
    std::unordered_set<int32> eventEntries;
    auto addSource = [&](int32 entry, GuidPosition const& position, uint32 phaseMask, CreatureData const* data)
    {
        bool const eventBound = (entry > 0 ? eventCreatures : eventGameObjects).count(position.GetCounter()) != 0;
        bool const questSource = requiredEntries.count(entry) != 0 && !eventBound;
        if (eventBound)
            eventEntries.insert(entry);
        if ((!questSource && entry <= 0) || !IsInRegion(position))
            return;
        SelfBotQuestSource source;
        source.Entry = entry;
        source.Position = position;
        source.PhaseMask = phaseMask;
        source.EventBound = eventBound;
        if (entry > 0)
            if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(uint32(entry)))
            {
                source.CreatureMinLevel = creature->minlevel;
                source.CreatureMaxLevel = creature->maxlevel;
                source.CreatureRank = creature->rank;
                source.FactionTemplateId = creature->faction;
                source.DetectionRange = creature->detection_range;
                source.Civilian = (creature->flags_extra & CREATURE_FLAG_EXTRA_CIVILIAN) != 0;
                source.MovementType = data ? data->movementType : creature->MovementType;
                source.WanderDistance = data ? data->wander_distance : 0.0f;
                uint32 dynamicFlags = 0;
                ObjectMgr::ChooseCreatureFlags(creature, source.NpcFlags, source.UnitFlags, dynamicFlags, data);
            }
        SelfBotQuestSource const* stored = storeSource(std::move(source), questSource);
        if (questSource)
            sources[entry].push_back(stored);
        if (entry > 0)
            _creatureCells[position.GetMapId()]
                          [{CreatureCell(position.GetPositionX()), CreatureCell(position.GetPositionY())}]
                              .push_back(stored);
    };
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (!IsMapSupported(data.mapid))
            continue;
        std::array<uint32, 3> const entries{data.id, data.id2, data.id3};
        for (size_t index = 0; index < entries.size(); ++index)
            if (entries[index] &&
                std::find(entries.begin(), entries.begin() + index, entries[index]) == entries.begin() + index)
            {
                GuidPosition position(data);
                static_cast<ObjectGuid&>(position) = ObjectGuid::Create<HighGuid::Unit>(entries[index], data.spawnId);
                addSource(int32(entries[index]), position, data.phaseMask, &data);
            }
    }
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
        if (IsMapSupported(data.mapid))
            addSource(-int32(data.id), GuidPosition(data), data.phaseMask, nullptr);

    auto appendRelations = [&](QuestRelations const* relations, bool gameObjects, bool giver)
    {
        for (auto const& [entry, questId] : *relations)
        {
            auto const definition = _definitionIndex.find(questId);
            if (definition == _definitionIndex.end())
                continue;
            SelfBotQuestDefinition& target = _definitions[definition->second];
            int32 const sourceEntry = gameObjects ? -int32(entry) : int32(entry);
            AppendSources(giver ? target.Givers : target.Takers, sources, sourceEntry);
            if (giver && eventEntries.count(sourceEntry) && target.ExclusionReason.empty())
                target.ExclusionReason = "quest giver is event-bound";
        }
    };
    appendRelations(sObjectMgr->GetCreatureQuestRelationMap(), false, true);
    appendRelations(sObjectMgr->GetCreatureQuestInvolvedRelationMap(), false, false);
    appendRelations(sObjectMgr->GetGOQuestRelationMap(), true, true);
    appendRelations(sObjectMgr->GetGOQuestInvolvedRelationMap(), true, false);
    for (SelfBotQuestDefinition& definition : _definitions)
        if (!definition.Seasonal && !eventQuests.count(definition.Id) && !definition.Givers.empty())
            definition.ExclusionReason.clear();

    for (auto const& [triggerId, trigger] : sObjectMgr->GetAllAreaTriggers())
    {
        auto const definition = _definitionIndex.find(sObjectMgr->GetQuestForAreaTrigger(triggerId));
        if (definition == _definitionIndex.end() || sObjectMgr->GetAreaTriggerTeleport(triggerId) ||
            sObjectMgr->GetAreaTriggerScriptId(triggerId))
            continue;
        WorldPosition const position(trigger.map, trigger.x, trigger.y, trigger.z, trigger.orientation);
        if (!IsInRegion(position))
            continue;
        SelfBotQuestSource source;
        source.Position.set(position);
        source.AreaTriggerId = triggerId;
        _definitions[definition->second].ExplorationSources.push_back(storeSource(std::move(source), true));
    }

    struct LocationBudget
    {
        size_t Cells = 0;
        size_t Examined = 0;
        size_t Checks = 0;
    };
    struct LocationCandidate
    {
        SelfBotQuestSource const* Source;
        double DistanceSquared;
    };
    using LocationKey = std::pair<uint32, uint32>;
    std::unordered_map<uint32, LocationBudget> locationBudgets;
    std::set<std::tuple<uint32, uint32, uint32>> locationGrids;
    std::map<LocationKey, std::pair<uint32, uint32>> locationAreas;
    std::map<LocationKey, SelfBotQuestSource const*> locations;
    std::map<std::tuple<uint32, uint32, uint32>, SelfBotQuestSources> locationPlans;
    uint32 nextLocationId = 1;
    auto locationSources = [&](uint32 questId, ItemUseDraft const& draft) -> SelfBotQuestSources
    {
        uint32 const objective = uint32(draft.Use.Objective) + (draft.Use.ItemObjective ? QUEST_OBJECTIVES_COUNT : 0);
        auto const planKey = std::make_tuple(questId, objective, draft.AreaGroupId);
        if (auto const cached = locationPlans.find(planKey); cached != locationPlans.end())
            return cached->second;
        SelfBotQuestSources& result = locationPlans[planKey];
        if (draft.Use.TargetMode != SelfBotQuestItemTargetMode::Self ||
            draft.Use.Kind != SelfBotQuestItemKind::Summon || !draft.AreaGroupId)
            return result;
        std::set<uint32> const areas = ItemLocationAreas(draft.AreaGroupId);
        QuestPOIVector const* pois = sObjectMgr->GetQuestPOIVector(questId);
        if (areas.empty() || !pois)
            return result;
        std::vector<QuestPOI const*> matching;
        for (QuestPOI const& poi : *pois)
            if (poi.ObjectiveIndex == int32(objective) && IsMapSupported(poi.MapId) &&
                std::any_of(areas.begin(), areas.end(),
                            [&](uint32 area) { return sAreaTableStore.LookupEntry(area)->mapid == poi.MapId; }))
                matching.push_back(&poi);
        if (matching.size() > MAX_ITEM_LOCATION_POLYGONS)
            return result;
        std::sort(matching.begin(), matching.end(), [](QuestPOI const* left, QuestPOI const* right)
                  { return std::tie(left->MapId, left->Id) < std::tie(right->MapId, right->Id); });
        LocationBudget& budget = locationBudgets[questId];
        std::map<LocationKey, LocationCandidate> candidates;
        for (QuestPOI const* poi : matching)
        {
            ItemLocationPolygon polygon;
            auto const map = _creatureCells.find(poi->MapId);
            if (map == _creatureCells.end() || !ItemPolygon(*poi, polygon))
                continue;
            int32 const minX = CreatureCell(polygon.MinX);
            int32 const maxX = CreatureCell(polygon.MaxX);
            int32 const minY = CreatureCell(polygon.MinY);
            int32 const maxY = CreatureCell(polygon.MaxY);
            uint64 const cells = uint64(maxX - minX + 1) * uint64(maxY - minY + 1);
            if (cells > MAX_ITEM_LOCATION_CELLS - budget.Cells)
                return result;
            for (int32 x = minX; x <= maxX; ++x)
                for (int32 y = minY; y <= maxY; ++y)
                {
                    ++budget.Cells;
                    auto const cell = map->second.find({x, y});
                    if (cell == map->second.end())
                        continue;
                    for (SelfBotQuestSource const* source : cell->second)
                    {
                        if (++budget.Examined > MAX_ITEM_LOCATION_SPAWNS)
                            return result;
                        if (source->EventBound || !source->PhaseMask || !InsideItemPolygon(source->Position, polygon))
                            continue;
                        double const dx = source->Position.GetPositionX() - polygon.CenterX;
                        double const dy = source->Position.GetPositionY() - polygon.CenterY;
                        LocationKey const key{poi->MapId, source->Position.GetCounter()};
                        auto found = candidates.find(key);
                        double const distance = dx * dx + dy * dy;
                        if (found == candidates.end() || distance < found->second.DistanceSquared)
                            candidates[key] = {source, distance};
                    }
                }
        }
        std::vector<LocationCandidate> ordered;
        for (auto const& [key, candidate] : candidates)
            ordered.push_back(candidate);
        std::sort(ordered.begin(), ordered.end(),
                  [](LocationCandidate const& left, LocationCandidate const& right)
                  {
                      return std::make_tuple(left.DistanceSquared, left.Source->Position.GetMapId(),
                                             left.Source->Position.GetCounter()) <
                             std::make_tuple(right.DistanceSquared, right.Source->Position.GetMapId(),
                                             right.Source->Position.GetCounter());
                  });
        for (LocationCandidate const& candidate : ordered)
        {
            if (budget.Checks >= MAX_ITEM_LOCATION_CHECKS)
                break;
            ++budget.Checks;
            SelfBotQuestSource const& original = *candidate.Source;
            WorldPosition const& position = original.Position;
            LocationKey const key{position.GetMapId(), original.Position.GetCounter()};
            auto resolved = locationAreas.find(key);
            if (resolved == locationAreas.end())
            {
                GridCoord const grid = Acore::ComputeGridCoord(position.GetPositionX(), position.GetPositionY());
                auto const gridKey = std::make_tuple(position.GetMapId(), grid.x_coord, grid.y_coord);
                if (!locationGrids.count(gridKey) && locationGrids.size() >= MAX_ITEM_LOCATION_GRIDS)
                    continue;
                locationGrids.insert(gridKey);
                Map* map = sMapMgr->CreateBaseMap(position.GetMapId());
                if (!map)
                    continue;
                // Load terrain/VMap before the native area's initial WMO lookup; do not activate grid objects.
                map->EnsureGridCreated(grid);
                uint32 zoneId = 0;
                uint32 areaId = 0;
                map->GetZoneAndAreaId(original.PhaseMask, zoneId, areaId, position.GetPositionX(),
                                      position.GetPositionY(), position.GetPositionZ());
                resolved = locationAreas.emplace(key, std::make_pair(zoneId, areaId)).first;
            }
            if (!areas.count(resolved->second.first) && !areas.count(resolved->second.second))
                continue;
            auto location = locations.find(key);
            if (location == locations.end())
            {
                SelfBotQuestSource source;
                source.Position.set(position);
                source.LocationId = nextLocationId++;
                source.AreaId = resolved->second.second;
                source.PhaseMask = original.PhaseMask;
                location = locations.emplace(key, storeSource(std::move(source), true)).first;
            }
            result.push_back(location->second);
        }
        LOG_DEBUG("playerbots",
                  "SelfBot quest item locations quest={} objective={} areaGroup={} locations={} "
                  "cells={} spawns={} areaChecks={}",
                  questId, objective, draft.AreaGroupId, result.size(), budget.Cells, budget.Examined, budget.Checks);
        return result;
    };

    size_t supported = 0;
    size_t excluded = 0;
    size_t sourceReferences = 0;
    size_t giverReferences = 0;
    size_t itemUseBindings = 0;
    std::array<size_t, 4> itemKinds{};
    std::unordered_map<uint32, bool> smartSpawnOverrides;
    for (size_t definitionIndex = 0; definitionIndex < _definitions.size(); ++definitionIndex)
    {
        SelfBotQuestDefinition& definition = _definitions[definitionIndex];
        Quest const* quest = quests.at(definition.Id);
        definition.Title = quest->GetTitle();
        definition.QuestLevel = quest->GetQuestLevel();
        definition.MinLevel = quest->GetMinLevel();
        definition.SuggestedPlayers = quest->GetSuggestedPlayers();
        for (uint32 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
            if (quest->RequiredNpcOrGoCount[index])
                AppendSources(definition.ObjectiveSources[index], sources, quest->RequiredNpcOrGo[index]);
        for (uint32 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
            if (quest->RequiredItemCount[index])
                for (int32 entry : itemEntries[quest->RequiredItemId[index]])
                    AppendSources(definition.ItemSources[index], sources, entry);
        QuestItemAnalysis const& analysis = questItems.at(definition.Id);
        for (ItemUseDraft const& draft : analysis.Uses)
        {
            SelfBotQuestItemUse use = draft.Use;
            std::unordered_set<SelfBotQuestSource const*> selected;
            if (use.TargetMode == SelfBotQuestItemTargetMode::Self)
                use.Sources = draft.AreaGroupId ? locationSources(definition.Id, draft) : definition.Givers;
            for (ItemTarget const& target : draft.Targets)
                if (auto const found = sources.find(target.Entry); found != sources.end())
                    for (SelfBotQuestSource const* source : found->second)
                    {
                        uint32 const spawnId = source->Position.GetCounter();
                        if (target.SpawnId && spawnId != target.SpawnId)
                            continue;
                        if (target.RejectScriptOverrides)
                        {
                            CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
                            if (data && data->ScriptId && data->id == uint32(target.Entry))
                                continue;
                            auto overridden = smartSpawnOverrides.find(spawnId);
                            if (overridden == smartSpawnOverrides.end())
                                overridden =
                                    smartSpawnOverrides
                                        .emplace(spawnId, !sSmartScriptMgr
                                                               ->GetScript(-int32(spawnId), SMART_SCRIPT_TYPE_CREATURE)
                                                               .empty())
                                        .first;
                            if (overridden->second)
                                continue;
                        }
                        if (selected.insert(source).second)
                            use.Sources.push_back(source);
                    }
            if (!use.Sources.empty())
            {
                ++itemKinds[size_t(use.Kind)];
                LOG_DEBUG("playerbots",
                          "SelfBot quest item plan quest={} item={} spell={} kind={} targetMode={} continuation={} "
                          "objective={} itemObjective={} resultEntry={} resultAura={} health={}:{} sources={}",
                          definition.Id, use.ItemId, use.SpellId, uint32(use.Kind), uint32(use.TargetMode),
                          uint32(use.Continuation), uint32(use.Objective), use.ItemObjective, use.ResultEntry,
                          use.ResultAura, use.MinHealthPct, use.MaxHealthPct, use.Sources.size());
                sourceReferences += use.Sources.size();
                definition.ItemUses.push_back(std::move(use));
            }
        }
        for (uint32 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
            if (analysis.Objectives[index])
            {
                // A spell's virtual credit must not become an ordinary kill task.
                definition.ObjectiveSources[index].clear();
                if (!HasItemUse(definition, index, false))
                    definition.UnsupportedReason =
                        "quest item credit has unsupported target conditions or no open-world source";
            }
        for (uint32 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
            if (analysis.Items[index] && !HasItemUse(definition, index, true))
                definition.UnsupportedReason =
                    "quest item creation has unsupported target conditions or no open-world source";
        if (analysis.UnresolvedTool)
        {
            for (SelfBotQuestSources& objective : definition.ObjectiveSources)
                objective.clear();
            definition.UnsupportedReason =
                "quest source item has unresolved on-use behavior; ordinary kill fallback disabled";
        }
        if (definition.UnsupportedReason.empty())
            definition.UnsupportedReason = UnsupportedReason(quest, definition);
        definition.Supported = definition.UnsupportedReason.empty();
        supported += definition.Supported;
        excluded += !definition.ExclusionReason.empty();
        itemUseBindings += definition.ItemUses.size();
        sourceReferences += definition.Givers.size() + definition.Takers.size() + definition.ExplorationSources.size();
        for (auto const& objective : definition.ObjectiveSources)
            sourceReferences += objective.size();
        for (auto const& item : definition.ItemSources)
            sourceReferences += item.size();
        for (SelfBotQuestSource const* giver : definition.Givers)
        {
            if (!definition.ExclusionReason.empty())
                continue;
            WorldPosition const& position = giver->Position;
            _mapIndex[position.GetMapId()]
                .Cells[GiverCell(position.GetPositionX())][GiverCell(position.GetPositionY())]
                .push_back({definitionIndex, giver});
            ++giverReferences;
        }
        LOG_DEBUG("playerbots",
                  "SelfBot quest catalog quest={} title={} supported={} reason={} givers={} takers={} exploration={} "
                  "itemUses={} seasonal={} exclusion={}",
                  definition.Id, definition.Title, definition.Supported, definition.UnsupportedReason,
                  definition.Givers.size(), definition.Takers.size(), definition.ExplorationSources.size(),
                  definition.ItemUses.size(), definition.Seasonal, definition.ExclusionReason);
    }
    for (auto& [mapId, map] : _mapIndex)
        for (auto& [cellX, columns] : map.Cells)
            for (auto& [cellY, references] : columns)
                std::sort(references.begin(), references.end(),
                          [&](GiverReference const& left, GiverReference const& right)
                          {
                              SelfBotQuestDefinition const& leftQuest = _definitions[left.DefinitionIndex];
                              SelfBotQuestDefinition const& rightQuest = _definitions[right.DefinitionIndex];
                              return leftQuest.MinLevel != rightQuest.MinLevel
                                         ? leftQuest.MinLevel < rightQuest.MinLevel
                                         : leftQuest.Id < rightQuest.Id;
                          });
    _initialized = true;
    LOG_INFO("playerbots",
             "SelfBot quest catalog initialized maps={} areas={} quests={} supported={} sources={} references={} "
             "giverReferences={} itemUses={} itemProgress={} itemWeaken={} itemTransform={} itemSummon={} "
             "itemLocations={} itemLocationGrids={} itemLocationAreaChecks={} excluded={} eventCreatures={} "
             "eventGameObjects={} eventQuests={} eventPools={}",
             _maps.size(), _areas.size(), _definitions.size(), supported, _sources.size(), sourceReferences,
             giverReferences, itemUseBindings, itemKinds[0], itemKinds[1], itemKinds[2], itemKinds[3], locations.size(),
             locationGrids.size(), locationAreas.size(), excluded, eventCreatures.size(), eventGameObjects.size(),
             eventQuests.size(), visitedEventPools.size());
}

SelfBotQuestDefinition const* SelfBotQuestCatalog::Find(uint32 questId) const
{
    auto const found = _definitionIndex.find(questId);
    return found != _definitionIndex.end() ? &_definitions[found->second] : nullptr;
}

float SelfBotQuestCatalog::MaxSearchRadius(WorldPosition const& origin) const
{
    auto const found = _mapIndex.find(origin.GetMapId());
    if (!ValidPosition(origin) || found == _mapIndex.end() || !found->second.HasSources)
        return 0.0f;
    MapIndex const& map = found->second;
    float const x = std::max(std::abs(origin.GetPositionX() - map.MinX), std::abs(origin.GetPositionX() - map.MaxX));
    float const y = std::max(std::abs(origin.GetPositionY() - map.MinY), std::abs(origin.GetPositionY() - map.MaxY));
    float const z = std::max(std::abs(origin.GetPositionZ() - map.MinZ), std::abs(origin.GetPositionZ() - map.MaxZ));
    return std::ceil(std::sqrt(x * x + y * y + z * z)) + 1.0f;
}

SelfBotQuestCorridor SelfBotQuestCatalog::QueryCreatureCorridor(uint32 mapId, Movement::PointsArray const& points,
                                                                float radius, size_t maxCells, size_t maxSpawns) const
{
    SelfBotQuestCorridor result;
    if (!_initialized || !IsMapSupported(mapId) || points.empty() || points.size() > MAX_CORRIDOR_POINTS ||
        !std::isfinite(radius) || radius < 0.0f || radius > MAP_SIZE || !maxCells || !maxSpawns)
        return result;
    for (G3D::Vector3 const& point : points)
        if (!Acore::IsValidMapCoord(point.x, point.y, point.z))
            return result;

    using Cell = std::pair<int32, int32>;
    std::set<Cell> centers;
    std::set<Cell> cells;
    int32 const padding = int32(std::ceil(radius / CREATURE_CELL_SIZE));
    size_t traversed = 0;
    auto addCell = [&](int32 cellX, int32 cellY)
    {
        if (++traversed > MAX_CORRIDOR_STEPS)
            return false;
        if (!centers.emplace(cellX, cellY).second)
            return true;
        for (int32 x = cellX - padding; x <= cellX + padding; ++x)
            for (int32 y = cellY - padding; y <= cellY + padding; ++y)
            {
                Cell const cell{x, y};
                if (cells.count(cell))
                    continue;
                if (cells.size() >= maxCells)
                    return false;
                cells.insert(cell);
                result.VisitedCells = cells.size();
            }
        return true;
    };
    if (!addCell(CreatureCell(points.front().x), CreatureCell(points.front().y)))
        return result;
    for (size_t index = 1; index < points.size(); ++index)
    {
        G3D::Vector3 const& start = points[index - 1];
        G3D::Vector3 const& end = points[index];
        int32 cellX = CreatureCell(start.x);
        int32 cellY = CreatureCell(start.y);
        int32 const endX = CreatureCell(end.x);
        int32 const endY = CreatureCell(end.y);
        int32 const stepX = (end.x > start.x) - (end.x < start.x);
        int32 const stepY = (end.y > start.y) - (end.y < start.y);
        double const dx = double(end.x) - start.x;
        double const dy = double(end.y) - start.y;
        double const infinity = std::numeric_limits<double>::infinity();
        double const deltaX = stepX ? std::abs(CREATURE_CELL_SIZE / dx) : infinity;
        double const deltaY = stepY ? std::abs(CREATURE_CELL_SIZE / dy) : infinity;
        double nextX = stepX ? ((cellX + (stepX > 0)) * CREATURE_CELL_SIZE - start.x) / dx : infinity;
        double nextY = stepY ? ((cellY + (stepY > 0)) * CREATURE_CELL_SIZE - start.y) / dy : infinity;
        while (cellX != endX || cellY != endY)
        {
            if (nextX < nextY)
            {
                cellX += stepX;
                nextX += deltaX;
            }
            else if (nextY < nextX)
            {
                cellY += stepY;
                nextY += deltaY;
            }
            else
            {
                // Include both cells touched at a corner, including zero-radius boundary queries.
                if (stepX && stepY && (!addCell(cellX + stepX, cellY) || !addCell(cellX, cellY + stepY)))
                    return result;
                cellX += stepX;
                cellY += stepY;
                nextX += deltaX;
                nextY += deltaY;
            }
            if (!addCell(cellX, cellY))
                return result;
        }
    }

    auto const map = _creatureCells.find(mapId);
    if (map == _creatureCells.end())
    {
        result.Complete = true;
        return result;
    }
    float const radiusSquared = radius * radius;
    size_t distanceChecks = 0;
    for (Cell const& cell : cells)
    {
        auto const found = map->second.find(cell);
        if (found == map->second.end())
            continue;
        for (SelfBotQuestSource const* source : found->second)
        {
            if (result.Examined >= maxSpawns)
                return result;
            ++result.Examined;
            G3D::Vector3 const position(source->Position.GetPositionX(), source->Position.GetPositionY(),
                                        source->Position.GetPositionZ());
            for (size_t index = points.size() == 1 ? 0 : 1; index < points.size(); ++index)
            {
                if (++distanceChecks > MAX_CORRIDOR_DISTANCE_CHECKS)
                    return result;
                G3D::Vector3 const& start = points[index ? index - 1 : 0];
                if (SegmentDistanceSquared(position, start, points[index]) <= radiusSquared)
                {
                    result.Sources.push_back(source);
                    break;
                }
            }
        }
    }
    result.Complete = true;
    return result;
}

std::vector<SelfBotQuestRow> SelfBotQuestCatalog::Evaluate(Player* player, WorldPosition const& origin,
                                                           float radius) const
{
    std::vector<SelfBotQuestRow> rows;
    if (!player || !_initialized)
        return rows;
    std::unordered_set<size_t> selected;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
    {
        auto const definition = _definitionIndex.find(player->GetQuestSlotQuestId(slot));
        if (definition != _definitionIndex.end())
            selected.insert(definition->second);
    }
    auto const map = _mapIndex.find(player->GetMapId());
    if (origin.GetMapId() == player->GetMapId() && ValidPosition(origin) && std::isfinite(radius) && radius >= 0.0f &&
        map != _mapIndex.end() && map->second.HasSources)
    {
        MapIndex const& index = map->second;
        radius = std::min(radius, MaxSearchRadius(origin));
        float const minX = std::max(origin.GetPositionX() - radius, index.MinX);
        float const maxX = std::min(origin.GetPositionX() + radius, index.MaxX);
        float const minY = std::max(origin.GetPositionY() - radius, index.MinY);
        float const maxY = std::min(origin.GetPositionY() + radius, index.MaxY);
        if (minX <= maxX && minY <= maxY)
            for (auto x = index.Cells.lower_bound(GiverCell(minX));
                 x != index.Cells.end() && x->first <= GiverCell(maxX); ++x)
                for (auto y = x->second.lower_bound(GiverCell(minY));
                     y != x->second.end() && y->first <= GiverCell(maxY); ++y)
                    for (GiverReference const& giver : y->second)
                    {
                        SelfBotQuestDefinition const& definition = _definitions[giver.DefinitionIndex];
                        if (definition.MinLevel > player->GetLevel())
                            break;
                        if (origin.GetExactDist(giver.Source->Position) <= radius)
                            selected.insert(giver.DefinitionIndex);
                    }
    }
    std::vector<size_t> definitions(selected.begin(), selected.end());
    std::sort(definitions.begin(), definitions.end());
    rows.reserve(definitions.size());
    for (size_t index : definitions)
    {
        SelfBotQuestRow row = EvaluateQuest(player, _definitions[index].Id);
        if (row.Quest)
            rows.push_back(std::move(row));
    }
    return rows;
}

SelfBotQuestRow SelfBotQuestCatalog::EvaluateQuest(Player* player, uint32 questId) const
{
    SelfBotQuestRow row;
    SelfBotQuestDefinition const* definition = Find(questId);
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!player || !_initialized || !definition || !quest)
        return row;
    row.Quest = definition;
    row.Status = player->GetQuestStatus(questId);
    row.Rewarded = player->IsQuestRewarded(questId);
    row.CanTake = definition->ExclusionReason.empty() && player->CanTakeQuest(quest, false);
    if (!definition->ExclusionReason.empty())
        row.EligibilityReason = definition->ExclusionReason;
    else if (row.Rewarded)
        row.EligibilityReason = "already rewarded";
    else if (row.Status == QUEST_STATUS_COMPLETE)
        row.EligibilityReason = "complete; awaiting turn-in";
    else if (row.Status == QUEST_STATUS_INCOMPLETE)
        row.EligibilityReason = "active; objectives pending";
    else if (row.Status == QUEST_STATUS_FAILED)
        row.EligibilityReason = "failed; manual recovery required";
    else if (!row.CanTake)
        row.EligibilityReason = TakeQuestReason(player, quest);
    else if ((row.CanAdd = CanAddQuietly(player, quest, row.EligibilityReason)))
        row.EligibilityReason = "available";
    return row;
}

uint64 SelfBotQuestCatalog::Fingerprint(Player* player)
{
    if (!player)
        return 0;
    constexpr uint64 HASH_OFFSET = 14695981039346656037ULL;
    uint64 hash = HASH_OFFSET;
    HashValue(hash, player->GetLevel());
    HashValue(hash, player->GetMoney());
    HashValue(hash, player->GetFreeInventorySpace());
    HashValue(hash, player->GetRewardedQuestCount());
    for (auto const& [questId, status] : player->getQuestStatusMap())
    {
        HashValue(hash, questId);
        HashValue(hash, status.Status);
        HashValue(hash, status.Explored);
        for (uint32 count : status.CreatureOrGOCount)
            HashValue(hash, count);
        for (uint32 count : status.ItemCount)
            HashValue(hash, count);
    }
    return hash;
}
