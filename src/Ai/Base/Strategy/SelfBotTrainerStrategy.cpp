/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfBotTrainerStrategy.h"

#include <algorithm>

#include "ChooseTargetActions.h"
#include "Creature.h"
#include "LootAction.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Multiplier.h"
#include "NPCPackets.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "SelfBotQuestStrategy.h"
#include "SellGrayStrategy.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Trainer.h"
#include "WorldSession.h"

namespace
{
constexpr float TrainerSearchDistance = 1200.0f;
constexpr float ProgressDistance = 3.0f;
constexpr float TrainerIdleRelevance = ACTION_DEFAULT + 0.5f;
constexpr time_t RetryDelay = 60;
constexpr time_t FailedTrainerDelay = 300;
constexpr time_t JourneyTimeout = 300;
constexpr time_t MaxVisitAge = 900;
constexpr time_t StuckTimeout = 30;
constexpr time_t PauseThreshold = 5;
constexpr size_t MaxPathAttempts = 4;

SelfBotTrainerState& TrainerState(AiObjectContext* context)
{
    return context->GetValue<SelfBotTrainerState>("selfbot trainer state")->RefGet();
}

bool Connected(Player* bot)
{
    WorldSession* session = bot->GetSession();
    return IsSelfBot(bot) && session && !session->IsSocketClosed() && !session->IsLoggingOut() && bot->IsAlive() &&
           !bot->IsInCombat() && !bot->IsInFlight() && !bot->GetVehicle() && !bot->IsBeingTeleported();
}

bool CanVisitTrainer(PlayerbotAI* botAI, AiObjectContext* context)
{
    Player* bot = botAI->GetBot();
    if (!Connected(bot) || bot->IsNonMeleeSpellCast(false) || bot->GetHealthPct() < 50.0f)
        return false;

    if (bot->IsSitState() &&
        (bot->GetHealthPct() < 99.0f || (bot->GetMaxPower(POWER_MANA) && bot->GetPowerPct(POWER_MANA) < 99.0f)))
        return false;

    GrayVendorState const& vendor = context->GetValue<GrayVendorState>("gray vendor state")->Get();
    return vendor.Phase == GrayVendorPhase::Idle &&
           (!botAI->HasStrategy("sell gray", BOT_STATE_NON_COMBAT) || time(nullptr) < vendor.RetryAfter ||
            !context->GetValue<bool>("gray vendor needed")->Get());
}

Trainer::Trainer* ClassTrainer(Player* bot, uint32 entry)
{
    Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry);
    return trainer && trainer->GetTrainerType() == Trainer::Type::Class && trainer->IsTrainerValidForPlayer(bot)
               ? trainer
               : nullptr;
}

Trainer::Spell const* AffordableSpell(Player* bot, Trainer::Trainer const* trainer, float discount,
                                      SelfBotTrainerState const& state)
{
    PlayerbotMgr* manager = GET_PLAYERBOT_MGR(bot);
    auto const settings = manager ? manager->GetSelfBotController().GetCachedSettings() : std::nullopt;
    if (!settings || !settings->AutoTrain)
        return nullptr;
    Trainer::Spell const* selected = nullptr;
    uint32 selectedCost = 0;
    for (Trainer::Spell const& spell : trainer->GetSpells())
    {
        if (state.AttemptedSpells.count(spell.SpellId) || !trainer->CanTeachSpell(bot, &spell))
            continue;

        uint32 const cost = static_cast<uint32>(spell.MoneyCost * discount);
        if (!settings->CanAffordTraining(bot->GetMoney(), cost))
            continue;

        if (!selected || cost < selectedCost || (cost == selectedCost && spell.SpellId < selected->SpellId))
        {
            selected = &spell;
            selectedCost = cost;
        }
    }
    return selected;
}

Creature* ResolveTrainer(Player* bot, SelfBotTrainerState const& state)
{
    auto const bounds = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(state.SpawnId);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
    {
        Creature* creature = itr->second;
        if (creature && creature->IsInWorld() && !creature->IsDuringRemoveFromWorld() && creature->IsAlive() &&
            creature->GetEntry() == state.Entry && creature->InSamePhase(bot) && creature->IsFriendlyTo(bot) &&
            creature->HasNpcFlag(UNIT_NPC_FLAG_TRAINER) && !creature->IsTrigger() &&
            !creature->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) && ClassTrainer(bot, creature->GetEntry()))
            return creature;
    }
    return nullptr;
}

void FinishVisit(Player* bot, SelfBotTrainerState& state, bool failed)
{
    if (failed)
        state.FailedSpawns[state.SpawnId] = time(nullptr) + FailedTrainerDelay;
    LOG_INFO("playerbots", "SelfBot {} class trainer {} visit ended: {}", bot->GetName(), state.SpawnId,
             failed ? "unreachable or unavailable" : "no more affordable abilities");
    state.Phase = SelfBotTrainerPhase::Idle;
    state.RetryAfter = time(nullptr) + RetryDelay;
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
}

bool ChooseTrainer(Player* bot, SelfBotTrainerState& state)
{
    time_t const now = time(nullptr);
    state.RetryAfter = now + RetryDelay;
    for (auto itr = state.FailedSpawns.begin(); itr != state.FailedSpawns.end();)
        if (itr->second <= now)
            itr = state.FailedSpawns.erase(itr);
        else
            ++itr;

    GridCoord const center = Acore::ComputeGridCoord(bot->GetPositionX(), bot->GetPositionY());
    int32 const gridRadius = static_cast<int32>(std::ceil(TrainerSearchDistance / SIZE_OF_GRIDS));
    std::vector<std::pair<float, CreatureData const*>> candidates;
    for (int32 x = std::max(0, static_cast<int32>(center.x_coord) - gridRadius);
         x <= std::min(static_cast<int32>(MAX_NUMBER_OF_GRIDS) - 1, static_cast<int32>(center.x_coord) + gridRadius);
         ++x)
        for (int32 y = std::max(0, static_cast<int32>(center.y_coord) - gridRadius);
             y <=
             std::min(static_cast<int32>(MAX_NUMBER_OF_GRIDS) - 1, static_cast<int32>(center.y_coord) + gridRadius);
             ++y)
        {
            auto const& spawns =
                sObjectMgr->GetGridObjectGuids(bot->GetMapId(), bot->GetMap()->GetSpawnMode(), GridCoord(x, y).GetId())
                    .creatures;
            for (ObjectGuid::LowType spawnId : spawns)
            {
                if (state.FailedSpawns.count(spawnId))
                    continue;
                CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
                if (!data || !(data->phaseMask & bot->GetPhaseMask()))
                    continue;

                float const distance = bot->GetExactDist(data->posX, data->posY, data->posZ);
                if (distance > TrainerSearchDistance)
                    continue;

                CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(data->id);
                if (!creatureTemplate || creatureTemplate->HasFlagsExtra(CREATURE_FLAG_EXTRA_TRIGGER) ||
                    !((data->npcflag ? data->npcflag : creatureTemplate->npcflag) & UNIT_NPC_FLAG_TRAINER) ||
                    ((data->unit_flags ? data->unit_flags : creatureTemplate->unit_flags) & UNIT_FLAG_NOT_SELECTABLE))
                    continue;

                FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creatureTemplate->faction);
                Trainer::Trainer* trainer = ClassTrainer(bot, data->id);
                if (!faction || Unit::GetFactionReactionTo(faction, bot->GetFactionTemplateEntry()) < REP_FRIENDLY ||
                    !trainer || !AffordableSpell(bot, trainer, bot->GetReputationPriceDiscount(faction), state))
                    continue;

                candidates.emplace_back(distance, data);
            }
        }

    std::sort(candidates.begin(), candidates.end(),
              [](auto const& left, auto const& right) { return left.first < right.first; });
    size_t attempts = 0;
    for (auto const& candidate : candidates)
    {
        if (++attempts > MaxPathAttempts)
            break;
        CreatureData const* data = candidate.second;
        PathGenerator path(bot);
        if (!path.CalculatePath(data->posX, data->posY, data->posZ) || path.GetPathType() != PATHFIND_NORMAL)
        {
            state.FailedSpawns[data->spawnId] = now + FailedTrainerDelay;
            continue;
        }

        state.SpawnId = data->spawnId;
        state.Entry = data->id;
        state.Destination = WorldPosition(data->mapid, data->posX, data->posY, data->posZ);
        state.LastProgress = WorldPosition(bot);
        state.ProgressAt = now;
        state.LastActive = now;
        state.Deadline = now + JourneyTimeout;
        state.HardDeadline = now + MaxVisitAge;
        state.Phase = SelfBotTrainerPhase::Traveling;
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
        LOG_INFO("playerbots", "SelfBot {} visiting class trainer {} spawn {} at {:.1f} yards with {} copper",
                 bot->GetName(), state.Entry, state.SpawnId, candidate.first, bot->GetMoney());
        return true;
    }
    return false;
}

bool SpellLearned(Player* bot, uint32 spellId)
{
    if (bot->HasSpell(spellId))
        return true;
    SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
    if (!spellInfo)
        return false;
    bool learnsSpell = false;
    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        if (effect.IsEffect(SPELL_EFFECT_LEARN_SPELL))
        {
            learnsSpell = true;
            if (!bot->HasSpell(effect.TriggerSpell))
                return false;
        }
    return learnsSpell;
}

class SelfBotTrainerMultiplier : public Multiplier
{
public:
    SelfBotTrainerMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "selfbot trainer") {}

    float GetValue(Action* action) override
    {
        if (!action || !IsSelfBot(bot) || TrainerState(context).Phase == SelfBotTrainerPhase::Idle ||
            botAI->IsSelfBotDefending())
            return 1.0f;

        if (dynamic_cast<VisitSelfBotTrainerAction*>(action))
            return ACTION_MOVE / TrainerIdleRelevance;

        std::string const name = action->getName();
        if (dynamic_cast<ChooseSelfBotQuestAction*>(action) || dynamic_cast<MoveToSelfBotQuestAction*>(action) ||
            dynamic_cast<InteractSelfBotQuestAction*>(action) || dynamic_cast<AttackAnythingAction*>(action) ||
            dynamic_cast<MoveRandomAction*>(action) || dynamic_cast<LootAction*>(action) ||
            dynamic_cast<MoveToLootAction*>(action) || name.find("rpg") != std::string::npos ||
            name.find("travel") != std::string::npos || name == "accept all quests" || name == "talk to quest giver")
            return 0.0f;

        return 1.0f;
    }
};
}  // namespace

bool SelfBotTrainerTrigger::IsActive()
{
    SelfBotTrainerState const& state = TrainerState(context);
    return Connected(bot) && (state.Phase != SelfBotTrainerPhase::Idle || time(nullptr) >= state.RetryAfter);
}

void SelfBotTrainerStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);
    triggers.push_back(new TriggerNode("selfbot trainer", {NextAction("visit selfbot trainer", TrainerIdleRelevance)}));
}

void SelfBotTrainerStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    Strategy::InitMultipliers(multipliers);
    multipliers.push_back(new SelfBotTrainerMultiplier(botAI));
}

bool VisitSelfBotTrainerAction::isUseful()
{
    SelfBotTrainerState const& state = TrainerState(context);
    return CanVisitTrainer(botAI, context) &&
           (state.Phase != SelfBotTrainerPhase::Idle || time(nullptr) >= state.RetryAfter);
}

bool VisitSelfBotTrainerAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;

    SelfBotTrainerState& state = TrainerState(context);
    if (state.Phase == SelfBotTrainerPhase::Idle)
        return ChooseTrainer(bot, state);

    time_t const now = time(nullptr);
    if (state.LastActive && now - state.LastActive > PauseThreshold)
    {
        state.Deadline += now - state.LastActive;
        state.ProgressAt = now;
        state.LastProgress = WorldPosition(bot);
    }
    state.LastActive = now;
    if (bot->GetExactDist(state.LastProgress) >= ProgressDistance)
    {
        state.LastProgress = WorldPosition(bot);
        state.ProgressAt = now;
    }
    if (bot->GetMapId() != state.Destination.GetMapId() || now >= state.Deadline || now >= state.HardDeadline ||
        (state.Phase == SelfBotTrainerPhase::Traveling && now - state.ProgressAt >= StuckTimeout))
    {
        FinishVisit(bot, state, true);
        return true;
    }

    Creature* creature = ResolveTrainer(bot, state);
    if (!creature)
    {
        if (bot->GetExactDist(state.Destination) <= INTERACTION_DISTANCE)
        {
            FinishVisit(bot, state, true);
            return true;
        }
    }
    else if (bot->GetNPCIfCanInteractWith(creature->GetGUID(), UNIT_NPC_FLAG_TRAINER) &&
             bot->IsWithinLOSInMap(creature))
    {
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
        state.Phase = SelfBotTrainerPhase::Learning;
        Trainer::Trainer* trainer = ClassTrainer(bot, creature->GetEntry());
        Trainer::Spell const* spell =
            trainer ? AffordableSpell(bot, trainer, bot->GetReputationPriceDiscount(creature), state) : nullptr;
        if (!spell)
        {
            FinishVisit(bot, state, false);
            return true;
        }

        uint32 const spellId = spell->SpellId;
        uint32 const moneyBefore = bot->GetMoney();
        // Trainer hooks can refuse learning after payment; never attempt the same spell twice in this session.
        state.AttemptedSpells.insert(spellId);
        creature->PauseMovementForInteraction();
        WorldPacket packet(CMSG_TRAINER_BUY_SPELL);
        packet << creature->GetGUID() << spellId;
        WorldPackets::NPC::TrainerBuySpell buyPacket(std::move(packet));
        buyPacket.Read();
        bot->GetSession()->HandleTrainerBuySpellOpcode(buyPacket);
        LOG_INFO("playerbots", "SelfBot {} class trainer {} spell {} learned={} money {} -> {} copper", bot->GetName(),
                 state.Entry, spellId, SpellLearned(bot, spellId), moneyBefore, bot->GetMoney());
        state.ProgressAt = now;
        return true;
    }

    state.Phase = SelfBotTrainerPhase::Traveling;
    if (bot->isMoving())
        return true;
    WorldPosition const destination = creature ? WorldPosition(creature) : state.Destination;
    // Movement can be waiting for an earlier action; the progress timeout bounds retries.
    return MoveTo(destination.GetMapId(), destination.GetPositionX(), destination.GetPositionY(),
                  destination.GetPositionZ(), false, false, true);
}
