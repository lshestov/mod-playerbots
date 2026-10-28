/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfBotDefenseActions.h"

#include <typeinfo>

#include "AiObjectContext.h"
#include "AttackAction.h"
#include "Engine.h"
#include "Event.h"
#include "GenericSpellActions.h"
#include "Log.h"
#include "MovementActions.h"
#include "PaladinActions.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "ReachTargetActions.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
bool IsDirectTarget(SpellImplicitTargetInfo const& target)
{
    if (!target.GetTarget())
        return true;

    switch (target.GetTarget())
    {
        case TARGET_UNIT_CASTER:
        case TARGET_UNIT_TARGET_ENEMY:
        case TARGET_UNIT_TARGET_ALLY:
        case TARGET_UNIT_TARGET_ANY:
            return true;
        default:
            return false;
    }
}

bool IsHostileDefenseEffect(SpellEffectInfo const& effect)
{
    switch (effect.Effect)
    {
        case SPELL_EFFECT_SCHOOL_DAMAGE:
        case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
        case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
        case SPELL_EFFECT_WEAPON_DAMAGE:
        case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
        case SPELL_EFFECT_HEALTH_LEECH:
        case SPELL_EFFECT_POWER_BURN:
        case SPELL_EFFECT_INTERRUPT_CAST:
            return true;
        case SPELL_EFFECT_APPLY_AURA:
            switch (effect.ApplyAuraName)
            {
                case SPELL_AURA_PERIODIC_DAMAGE:
                case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                case SPELL_AURA_PERIODIC_LEECH:
                case SPELL_AURA_MOD_STUN:
                case SPELL_AURA_MOD_ROOT:
                case SPELL_AURA_MOD_SILENCE:
                case SPELL_AURA_MOD_DECREASE_SPEED:
                    return true;
                default:
                    return false;
            }
        default:
            return false;
    }
}

bool HasNativeSpellExecution(Action const* action)
{
    // These concrete classes inherit the direct CastSpellAction execution without extra side effects.
    std::type_info const& actionType = typeid(*action);
    return actionType == typeid(CastSpellAction) || actionType == typeid(CastMeleeSpellAction) ||
           actionType == typeid(CastDebuffSpellAction) || actionType == typeid(CastMeleeDebuffSpellAction) ||
           actionType == typeid(CastHealingSpellAction) || actionType == typeid(CastHolyLightAction) ||
           actionType == typeid(CastFlashOfLightAction) || actionType == typeid(CastLayOnHandsAction) ||
           actionType == typeid(CastHammerOfWrathAction) || actionType == typeid(CastCrusaderStrikeAction) ||
           actionType == typeid(CastHammerOfJusticeAction) || actionType == typeid(CastExorcismAction) ||
           actionType == typeid(ShieldOfRighteousnessAction);
}

class SelfBotDefenseListener final : public ActionExecutionListener
{
public:
    explicit SelfBotDefenseListener(PlayerbotAI* botAI) : _botAI(botAI) {}

    bool Before(Action* action, Event /*event*/) override
    {
        _defenseAction = false;
        _targetGuid.Clear();
        if (!_botAI->IsSelfBotSessionInactive())
            return true;

        if (!IsSelfBotDefenseAction(_botAI, action))
            return false;

        Unit* target = typeid(*action) == typeid(SetFacingTargetAction)
                           ? _botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get()
                           : action->GetTarget();
        if (target)
            _targetGuid = target->GetGUID();

        _defenseAction = true;
        return true;
    }

    bool AllowExecution(Action* /*action*/, Event /*event*/) override { return true; }

    void After(Action* action, bool executed, Event /*event*/) override
    {
        if (!_defenseAction || !executed || !_botAI->IsSelfBotSessionInactive())
            return;

        Player* bot = _botAI->GetBot();
        LOG_INFO("playerbots", "SelfBot {} defense action: action={}, target={}, health={}, xp={}",
                 bot->GetGUID().ToString(), action->getName(), _targetGuid.ToString(), bot->GetHealth(),
                 bot->GetUInt32Value(PLAYER_XP));
    }

    bool OverrideResult(Action* /*action*/, bool executed, Event /*event*/) override { return executed; }

private:
    PlayerbotAI* _botAI;
    bool _defenseAction = false;
    ObjectGuid _targetGuid;
};
}  // namespace

bool IsSelfBotDefenseAction(PlayerbotAI* botAI, Action* action)
{
    if (!botAI || !action)
        return false;

    if (!botAI->IsSelfBotSessionInactive())
        return true;

    if (!botAI->IsSelfBotDefending())
        return false;

    std::type_info const& actionType = typeid(*action);
    if (actionType == typeid(MeleeAction) || actionType == typeid(ReachMeleeAction) ||
        actionType == typeid(ReachSpellAction) || actionType == typeid(SetFacingTargetAction))
    {
        Unit* target = botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get();
        return botAI->IsSelfBotDefenseTarget(target);
    }

    if (!HasNativeSpellExecution(action))
        return false;

    CastSpellAction* spellAction = static_cast<CastSpellAction*>(action);
    uint32 const spellId = botAI->GetAiObjectContext()->GetValue<uint32>("spell id", spellAction->getSpell())->Get();
    return IsSelfBotDefenseSpell(botAI, spellId, spellAction->GetTarget());
}

bool IsSelfBotDefenseSpell(PlayerbotAI* botAI, uint32 spellId, Unit const* target)
{
    if (!botAI || !botAI->IsSelfBotDefending() || !target)
        return false;

    Player* bot = botAI->GetBot();
    if (!bot || !bot->HasSpell(spellId))
        return false;

    bool const selfHeal = target == bot;
    if (!selfHeal && !botAI->IsSelfBotDefenseTarget(target))
        return false;

    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo || spellInfo->IsAffectingArea() || spellInfo->IsPassive())
        return false;

    bool hasEffect = false;
    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
    {
        if (!effect.IsEffect())
            continue;

        hasEffect = true;
        if (effect.ChainTarget > 1 || effect.TriggerSpell || !IsDirectTarget(effect.TargetA) ||
            !IsDirectTarget(effect.TargetB))
            return false;

        if (selfHeal)
        {
            if (effect.Effect != SPELL_EFFECT_HEAL && effect.Effect != SPELL_EFFECT_HEAL_MAX_HEALTH &&
                effect.Effect != SPELL_EFFECT_HEAL_PCT)
                return false;
        }
        else if (spellInfo->IsPositiveEffect(effect.EffectIndex) || !IsHostileDefenseEffect(effect))
            return false;
    }

    return hasEffect;
}

ActionExecutionListener* CreateSelfBotDefenseListener(PlayerbotAI* botAI) { return new SelfBotDefenseListener(botAI); }
