/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfBotQuestStrategy.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <sstream>
#include <unordered_set>

#include "CellImpl.h"
#include "ChooseTargetActions.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GenericSpellActions.h"
#include "Item.h"
#include "ItemUsageValue.h"
#include "LootAction.h"
#include "LootObjectStack.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Multiplier.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "ReputationMgr.h"
#include "SelfBotQuestItemUse.h"
#include "SelfBotTrainerStrategy.h"
#include "SellGrayStrategy.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TemporarySummon.h"
#include "World.h"
#include "WorldSession.h"

namespace
{
constexpr time_t RefreshDelay = 5;
constexpr time_t EmptyQueueDelay = 10;
constexpr time_t FailedSourceDelay = 120;
constexpr time_t DeadSourceDelay = 30;
constexpr time_t ItemUseTimeout = 120;
constexpr time_t ItemHealthStabilityDelay = 2;
constexpr float ItemResultRadius = 40.0f;
constexpr size_t MaxItemResultCandidates = 64;
constexpr time_t JourneyTimeout = 240;
constexpr time_t MaxJourneyTimeout = 1800;
constexpr time_t StuckTimeout = 30;
constexpr uint8 MaxAttempts = 3;
constexpr size_t MaxPathAttempts = 4;
constexpr float ProgressDistance = 2.0f;
constexpr float ArrivalDistance = 3.0f;
constexpr float TravelStepDistance = 200.0f;
constexpr float RouteSampleDistance = 10.0f;
constexpr size_t MaxRouteSamples = 2048;
constexpr float RouteThreatQueryRadius = 100.0f;
constexpr float MaxThreatWanderDistance = 30.0f;
constexpr size_t MaxNearbyRouteCreatures = 256;
constexpr size_t MaxNewRouteGrids = 4;
constexpr size_t MaxRouteGrids = 64;
constexpr uint64 ExplorationSourceMask = uint64(0x8000) << 48;
constexpr uint64 ItemLocationSourceMask = uint64(0x4000) << 48;

uint64 SourceKey(SelfBotQuestSource const& source)
{
    if (source.LocationId)
        return ItemLocationSourceMask | source.LocationId;
    // Area triggers have no object GUID; keep their retry keys outside creature/GO GUID ranges.
    return source.AreaTriggerId ? ExplorationSourceMask | source.AreaTriggerId : source.Position.GetRawValue();
}

SelfBotQuestState& QuestState(AiObjectContext* context)
{
    return context->GetValue<SelfBotQuestState>("selfbot quest state")->RefGet();
}

void ResetOpportunity(WorldPosition const& anchor, SelfBotQuestState& state)
{
    state.OpportunityAnchor = anchor;
    state.OpportunityRadius = SelfBotQuestPlanning::InitialOpportunityRadius;
    state.OpportunityAnchored = true;
    state.RefreshAt = 0;
}

SelfBotActionQueue* ActionQueue(Player* bot)
{
    PlayerbotMgr* manager = GET_PLAYERBOT_MGR(bot);
    return manager ? &manager->GetSelfBotController().GetActionQueue() : nullptr;
}

bool QueueAllowsQuest(Player* bot, uint32 questId = 0)
{
    SelfBotActionQueue const* queue = ActionQueue(bot);
    return !queue || !queue->Selected() ||
           (queue->Running() && queue->Current() && (!questId || queue->Current()->QuestId == questId));
}

bool Connected(Player* bot)
{
    WorldSession* session = bot->GetSession();
    return sPlayerbotAIConfig.SelfBotQuesting && SelfBotQuestCatalog::Instance().IsInitialized() && IsSelfBot(bot) &&
           QueueAllowsQuest(bot) && session && !session->IsSocketClosed() && !session->IsLoggingOut() &&
           bot->IsAlive() && !bot->IsInCombat() && !bot->IsInFlight() && !bot->GetVehicle() &&
           !bot->IsBeingTeleported();
}

bool CanWork(Player* bot, AiObjectContext* context)
{
    if (!Connected(bot))
        return false;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    GrayVendorState const& vendor = context->GetValue<GrayVendorState>("gray vendor state")->Get();
    bool const selling = botAI && botAI->HasStrategy("sell gray", BOT_STATE_NON_COMBAT) &&
                         (vendor.Phase != GrayVendorPhase::Idle ||
                          (time(nullptr) >= vendor.RetryAfter && context->GetValue<bool>("gray vendor needed")->Get()));
    SelfBotQuestState& state = QuestState(context);
    bool const training =
        botAI && botAI->HasStrategy("selfbot trainer", BOT_STATE_NON_COMBAT) &&
        context->GetValue<SelfBotTrainerState>("selfbot trainer state")->RefGet().Phase != SelfBotTrainerPhase::Idle;
    if (selling || training || bot->IsNonMeleeSpellCast(false) || bot->GetHealthPct() < 50.0f)
    {
        if (!state.WasPaused && state.QuestId)
            LOG_INFO("playerbots", "SelfBot {} quest {} paused: {}", bot->GetName(), state.QuestId,
                     selling ? "gray vendor" : (training ? "trainer" : "recovery"));
        state.WasPaused = true;
        return false;
    }
    return true;
}

char const* TaskName(SelfBotQuestTask task)
{
    switch (task)
    {
        case SelfBotQuestTask::TurnIn:
            return "turn-in";
        case SelfBotQuestTask::Objective:
            return "objective";
        case SelfBotQuestTask::Item:
            return "item";
        case SelfBotQuestTask::Accept:
            return "accept";
        case SelfBotQuestTask::Explore:
            return "explore";
        case SelfBotQuestTask::UseItem:
            return "use-item";
    }
    return "unknown";
}

void Resume(Player* bot, SelfBotQuestState& state)
{
    time_t const now = time(nullptr);
    if (state.WasPaused || (state.LastActive && now - state.LastActive > RefreshDelay))
    {
        state.HasTravelStep = false;
        state.LastTravelPosition = WorldPosition(bot);
    }
    if (state.LastActive && now - state.LastActive > RefreshDelay)
    {
        state.Deadline += now - state.LastActive;
        state.ProgressAt = now;
        state.LastProgress = WorldPosition(bot);
    }
    if (state.WasPaused && state.QuestId)
        LOG_INFO("playerbots", "SelfBot {} quest {} resumed from real journal: {}", bot->GetName(), state.QuestId,
                 TaskName(state.Task));
    state.WasPaused = false;
    state.LastActive = now;
}

uint32 CurrentObjectiveProgress(Player* bot, SelfBotQuestState const& state)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(state.QuestId);
    if (!quest)
        return 0;
    if (state.Task == SelfBotQuestTask::Explore)
    {
        auto const& statuses = bot->getQuestStatusMap();
        auto const status = statuses.find(state.QuestId);
        return status != statuses.end() && status->second.Explored ? 1 : 0;
    }
    if (state.Task == SelfBotQuestTask::Item || (state.Task == SelfBotQuestTask::UseItem && state.UseItemObjective))
        return bot->GetItemCount(quest->RequiredItemId[state.Objective]);
    uint16 const slot = bot->FindQuestSlot(state.QuestId);
    return (state.Task == SelfBotQuestTask::Objective || state.Task == SelfBotQuestTask::UseItem) &&
                   slot < MAX_QUEST_LOG_SIZE
               ? bot->GetQuestSlotCounter(slot, state.Objective)
               : 0;
}

void ClearTask(Player* bot, SelfBotQuestState& state)
{
    SelfBotQuestItemCasting::RestoreDamage(bot, state.UseDamagePause);
    if (state.Phase != SelfBotQuestPhase::Idle && SelfBotQuestPlanning::IsWorkTask(state.Task))
    {
        if (state.WorkActive)
            ++state.WorkSteps;
        if (CurrentObjectiveProgress(bot, state) > state.TaskProgress)
            state.PickupAllowed = true;
    }
    state.Phase = SelfBotQuestPhase::Idle;
    state.QuestId = 0;
    state.Attempts = 0;
    state.RetryAt = 0;
    state.HasTravelStep = false;
    state.UseTarget.Clear();
    state.UseResultTarget.Clear();
    state.UseEffectObserved = false;
    state.UseCombatControl = false;
    state.UseExistingSummons.clear();
    state.UseObservedHealth = 0;
    state.UseHealthStableAt = 0;
    state.UseItemId = 0;
    state.UseSpellId = 0;
    state.UseWaitUntil = 0;
    state.UseStartedAt = 0;
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
}

void FailSource(Player* bot, SelfBotQuestState& state, char const* reason, time_t delay = FailedSourceDelay)
{
    if (state.OrderedQuestId)
        state.RouteFailure = reason;
    LOG_INFO("playerbots", "SelfBot {} quest {} {} source {} spawn {} deferred {}s: {} trigger={}", bot->GetName(),
             state.QuestId, TaskName(state.Task), state.Source.Entry, state.Source.Position.GetCounter(), delay, reason,
             state.Source.AreaTriggerId);
    uint64 const key = SourceKey(state.Source);
    if (delay == DeadSourceDelay)
        state.DeadSources[key] = time(nullptr) + delay;
    else
        state.FailedSources[key] = time(nullptr) + delay;
    if (delay != DeadSourceDelay && ++state.SourceFailures[key] >= MaxAttempts)
    {
        state.BlockedSources[key] = state.Fingerprint;
        LOG_INFO("playerbots",
                 "SelfBot {} source {} blocked after repeated failures until journal or conditions change",
                 bot->GetName(), state.Source.Position.GetCounter());
    }
    ClearTask(bot, state);
}

WorldObject* ResolveSource(Player* bot, SelfBotQuestSource const& source)
{
    if (source.AreaTriggerId || source.LocationId || bot->GetMapId() != source.Position.GetMapId())
        return nullptr;

    WorldObject* loaded = nullptr;
    if (source.Entry > 0)
    {
        auto const bounds = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(source.Position.GetCounter());
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            Creature* creature = itr->second;
            if (creature && creature->IsInWorld() && creature->GetEntry() == uint32(source.Entry))
            {
                if (creature->InSamePhase(bot))
                    return creature;
                loaded = creature;
            }
        }
    }
    else
    {
        auto const bounds = bot->GetMap()->GetGameObjectBySpawnIdStore().equal_range(source.Position.GetCounter());
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            GameObject* object = itr->second;
            if (object && object->IsInWorld() && object->isSpawned() && object->GetEntry() == uint32(-source.Entry))
            {
                if (object->InSamePhase(bot))
                    return object;
                loaded = object;
            }
        }
    }
    return loaded;
}

WorldObject* ResolveTaskSource(Player* bot, SelfBotQuestState const& state)
{
    if (state.Task != SelfBotQuestTask::UseItem || (!state.UseTarget && !state.UseResultTarget))
        return ResolveSource(bot, state.Source);
    if (state.Source.Position.GetMapId() != bot->GetMapId())
        return nullptr;
    ObjectGuid const guid = state.UseEffectObserved && state.UseResultTarget ? state.UseResultTarget : state.UseTarget;
    return guid.IsCreature() ? static_cast<WorldObject*>(bot->GetMap()->GetCreature(guid))
                             : static_cast<WorldObject*>(bot->GetMap()->GetGameObject(guid));
}

SelfBotQuestItemUse const* CurrentItemUse(SelfBotQuestState const& state)
{
    SelfBotQuestDefinition const* definition = SelfBotQuestCatalog::Instance().Find(state.QuestId);
    if (state.Task != SelfBotQuestTask::UseItem || !definition)
        return nullptr;
    if (state.UsePlanIndex >= definition->ItemUses.size())
        return nullptr;
    SelfBotQuestItemUse const& use = definition->ItemUses[state.UsePlanIndex];
    return use.ItemId == state.UseItemId && use.SpellId == state.UseSpellId && use.Objective == state.Objective &&
                   use.ItemObjective == state.UseItemObjective && use.RequiresDead == state.UseRequiresDead
               ? &use
               : nullptr;
}

bool ItemCombatConnected(Player* bot)
{
    WorldSession* session = bot->GetSession();
    return sPlayerbotAIConfig.SelfBotQuesting && IsSelfBot(bot) && QueueAllowsQuest(bot) && bot->IsInWorld() &&
           !bot->IsDuringRemoveFromWorld() && bot->m_mover == bot && bot->IsAlive() && session &&
           !session->IsSocketClosed() && !session->IsLoggingOut() && !bot->IsInFlight() && !bot->GetVehicle() &&
           !bot->IsBeingTeleported();
}

bool ItemCombatActive(PlayerbotAI* botAI, SelfBotQuestState const& state)
{
    return state.Phase != SelfBotQuestPhase::Idle && state.Task == SelfBotQuestTask::UseItem &&
           QueueAllowsQuest(botAI->GetBot(), state.QuestId) && state.UseCombatControl &&
           ItemCombatConnected(botAI->GetBot()) && botAI->HasStrategy("selfbot quest", BOT_STATE_NON_COMBAT);
}

SelfBotQuestPlanning::ItemUseHealthRange ItemHealthRange(SelfBotQuestItemUse const& use)
{
    return {use.MinHealthPct, use.MaxHealthPct, !use.MinHealthExclusive, !use.MaxHealthExclusive};
}

bool ResultSafeToAttack(Player* bot, Creature* creature)
{
    if (!creature || !creature->IsInWorld() || !creature->InSamePhase(bot) || !creature->IsAlive() ||
        !bot->IsValidAttackTarget(creature) || creature->GetCharmerOrOwnerGUID())
        return false;
    CreatureTemplate const* creatureTemplate = creature->GetCreatureTemplate();
    return creatureTemplate &&
           SelfBotQuestPlanning::AttackSourceAllowed(
               creatureTemplate->maxlevel, creature->GetLevel(),
               creatureTemplate->rank != CREATURE_ELITE_NORMAL && creatureTemplate->rank != CREATURE_ELITE_RARE,
               creature->isElite(), bot->GetLevel());
}

struct ItemSummonVisitor
{
    Player* Bot;
    uint32 Entry;
    std::vector<ObjectGuid>& Result;
    size_t Examined = 0;
    bool Complete = true;

    void Visit(CreatureMapType& creatures)
    {
        if (!Complete)
            return;
        for (auto itr = creatures.begin(); itr != creatures.end(); ++itr)
        {
            if (++Examined > MaxItemResultCandidates)
            {
                Complete = false;
                return;
            }
            Creature* creature = itr->GetSource();
            if (creature && creature->GetEntry() == Entry && creature->IsInWorld() && creature->InSamePhase(Bot) &&
                Bot->GetDistance(creature) <= ItemResultRadius && creature->IsSummon() &&
                creature->ToTempSummon()->GetSummonerGUID() == Bot->GetGUID())
                Result.push_back(creature->GetGUID());
        }
    }

    template <class T>
    void Visit(GridRefMgr<T>&)
    {
    }
};

bool CollectItemSummons(Player* bot, SelfBotQuestItemUse const& use, std::vector<ObjectGuid>& result)
{
    result.clear();
    ItemSummonVisitor visitor{bot, use.ResultEntry, result};
    Cell::VisitObjects(bot, visitor, ItemResultRadius);
    return visitor.Complete;
}

bool ObserveItemMechanic(Player* bot, SelfBotQuestState& state, SelfBotQuestItemUse const& use)
{
    if (state.UseEffectObserved || !state.Attempts)
        return state.UseEffectObserved;
    ObjectGuid result;
    if (use.Kind == SelfBotQuestItemKind::Summon)
    {
        std::vector<ObjectGuid> summons;
        if (use.SummonerIsTarget || !CollectItemSummons(bot, use, summons))
            return false;
        for (ObjectGuid guid : summons)
            if (std::find(state.UseExistingSummons.begin(), state.UseExistingSummons.end(), guid) ==
                state.UseExistingSummons.end())
            {
                if (result)
                    return false;
                result = guid;
            }
    }
    else if (state.UseTarget.IsCreature())
    {
        Creature* creature = bot->GetMap()->GetCreature(state.UseTarget);
        if (!creature || !creature->IsInWorld() || !creature->InSamePhase(bot))
            return false;
        if (use.Kind == SelfBotQuestItemKind::Transform && creature->GetEntry() == use.ResultEntry &&
            creature->GetEntry() != uint32(state.Source.Entry))
            result = state.UseTarget;
        else if (use.ResultAura &&
                 (use.ResultAuraRemoved ? state.UseHadResultAura && !creature->HasAura(use.ResultAura)
                                        : !state.UseHadResultAura && creature->HasAura(use.ResultAura, bot->GetGUID())))
            result = state.UseTarget;
    }
    if (!result)
        return false;
    state.UseResultTarget = result;
    state.UseEffectObserved = true;
    state.UseWaitUntil = 0;
    state.RetryAt = 0;
    state.Phase = SelfBotQuestPhase::Interacting;
    SelfBotQuestItemCasting::RestoreDamage(bot, state.UseDamagePause);
    LOG_INFO("playerbots", "SelfBot {} quest {} item effect observed on {}; objective still needs native progress",
             bot->GetName(), state.QuestId, result.ToString());
    return true;
}

bool HasItemUse(SelfBotQuestDefinition const& definition, bool itemObjective, uint8 objective)
{
    return std::any_of(definition.ItemUses.begin(), definition.ItemUses.end(), [&](SelfBotQuestItemUse const& use)
                       { return use.ItemObjective == itemObjective && use.Objective == objective; });
}

bool ItemUseAvailable(Player* bot, Quest const* quest, SelfBotQuestItemUse const& use, bool prospective)
{
    if (Item* item = bot->GetItemByEntry(use.ItemId))
        return bot->CanUseItem(item) == EQUIP_ERR_OK;
    return prospective && quest->GetSrcItemId() == use.ItemId;
}

auto ItemUseKey(uint32 questId, uint32 itemId, uint32 spellId, SelfBotQuestSource const& source, ObjectGuid target)
{
    return std::make_tuple(questId, itemId, spellId, target ? target.GetRawValue() : SourceKey(source));
}

void DeferItemUse(Player* bot, SelfBotQuestState& state, char const* reason)
{
    state
        .ItemUseDeferrals[ItemUseKey(state.QuestId, state.UseItemId, state.UseSpellId, state.Source, state.UseTarget)] =
        time(nullptr) + FailedSourceDelay;
    LOG_INFO("playerbots", "SelfBot {} quest {} item {} spell {} target {}: {}", bot->GetName(), state.QuestId,
             state.UseItemId, state.UseSpellId, state.UseTarget.ToString(), reason);
    ClearTask(bot, state);
    state.RefreshAt = 0;
}

bool ReseedObjectiveLoot(Player* bot, AiObjectContext* context, SelfBotQuestState& state)
{
    if (state.Phase == SelfBotQuestPhase::Idle || state.LootReseeded ||
        (state.Task != SelfBotQuestTask::Objective && state.Task != SelfBotQuestTask::Item))
        return false;

    WorldObject* source = ResolveSource(bot, state.Source);
    Creature* corpse = source ? source->ToCreature() : nullptr;
    if (!corpse || !corpse->InSamePhase(bot) || corpse->IsAlive() ||
        !corpse->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE) || !bot->isAllowedToLoot(corpse))
        return false;

    LootObjectStack* available = context->GetValue<LootObjectStack*>("available loot")->Get();
    if (available->IsLootPending() || bot->GetLootGUID())
        return true;

    LootObject loot(bot, corpse->GetGUID());
    if (!loot.IsLootPossible(bot))
        return false;

    // Pull-time loot entries expire after 30s; refresh this corpse once after a long fight.
    state.LootReseeded = corpse->GetGUID();
    available->Remove(state.LootReseeded);
    available->Add(state.LootReseeded);
    context->GetValue<LootObject>("loot target")->Set(loot);
    return true;
}

uint8 ActiveQuestCount(Player* bot)
{
    uint8 count = 0;
    for (uint16 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (uint32 questId = bot->GetQuestSlotQuestId(slot); questId && !bot->GetQuestRewardStatus(questId))
            ++count;
    return count;
}

bool QuestAutomationAllowed(Quest const* quest)
{
    SelfBotQuestDefinition const* definition =
        quest ? SelfBotQuestCatalog::Instance().Find(quest->GetQuestId()) : nullptr;
    return definition && definition->ExclusionReason.empty();
}

bool QuestSafeForWork(Player* bot, Quest const* quest)
{
    return QuestAutomationAllowed(quest) &&
           SelfBotQuestPlanning::QuestLevelAllowed(quest->GetQuestLevel(), bot->GetLevel()) &&
           quest->GetSuggestedPlayers() <= 1;
}

bool WouldAcceptExcludedQuest(Player* bot, Quest const* quest)
{
    return quest && quest->IsAutoAccept() && !QuestAutomationAllowed(quest) && bot->CanTakeQuest(quest, false) &&
           bot->CanAddQuest(quest, false);
}

bool OffersExcludedAutomaticQuest(Player* bot, WorldObject* object)
{
    auto const bounds = object->IsCreature() ? sObjectMgr->GetCreatureQuestRelationBounds(object->GetEntry())
                                             : sObjectMgr->GetGOQuestRelationBounds(object->GetEntry());
    for (auto relation = bounds.first; relation != bounds.second; ++relation)
        if (WouldAcceptExcludedQuest(bot, sObjectMgr->GetQuestTemplate(relation->second)))
            return true;
    return false;
}

ReputationRank SourceReaction(Player* bot, SelfBotQuestSource const& source, Creature const* creature)
{
    if (creature)
        return creature->GetReactionTo(bot);
    FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(source.FactionTemplateId);
    if (!faction)
        return REP_HOSTILE;
    if ((faction->factionFlags & FACTION_TEMPLATE_FLAG_ATTACK_PVP_ACTIVE_PLAYERS) &&
        bot->HasPlayerFlag(PLAYER_FLAGS_CONTESTED_PVP))
        return REP_HOSTILE;
    if (ReputationRank const* forced = bot->GetReputationMgr().GetForcedRankIfAny(faction))
        return *forced;
    if (!bot->HasUnitFlag2(UNIT_FLAG2_IGNORE_REPUTATION))
        if (FactionEntry const* entry = sFactionStore.LookupEntry(faction->faction);
            entry && entry->CanHaveReputation())
        {
            ReputationRank const rank = bot->GetReputationMgr().GetRank(entry);
            return bot->GetReputationMgr().IsAtWar(entry) ? std::min(REP_NEUTRAL, rank) : rank;
        }
    return Unit::GetFactionReactionTo(faction, bot->GetFactionTemplateEntry());
}

bool SourceSafeForWork(Player* bot, SelfBotQuestSource const& source, SelfBotQuestTask task, WorldObject* object,
                       SelfBotQuestItemUse const* use = nullptr)
{
    if (source.EventBound || source.Position.GetMapId() != bot->GetMapId() ||
        (object ? !object->InSamePhase(bot) : !(source.PhaseMask & bot->GetPhaseMask())))
        return false;
    if (task == SelfBotQuestTask::UseItem && use && use->ResultEntry &&
        use->Continuation != SelfBotQuestItemContinuation::None)
    {
        CreatureTemplate const* result = sObjectMgr->GetCreatureTemplate(use->ResultEntry);
        if (!result ||
            !SelfBotQuestPlanning::AttackSourceAllowed(
                result->maxlevel, 0, result->rank != CREATURE_ELITE_NORMAL && result->rank != CREATURE_ELITE_RARE,
                false, bot->GetLevel()))
            return false;
    }
    if (source.Entry <= 0)
        return true;
    Creature* creature = object ? object->ToCreature() : nullptr;
    ReputationRank const reaction = SourceReaction(bot, source, creature);
    if (task == SelfBotQuestTask::Accept || task == SelfBotQuestTask::TurnIn)
        return reaction > REP_UNFRIENDLY &&
               (creature ? creature->IsAlive() && creature->HasNpcFlag(UNIT_NPC_FLAG_QUESTGIVER) &&
                               !creature->GetCharmerGUID()
                         : (source.NpcFlags & UNIT_NPC_FLAG_QUESTGIVER) != 0);
    if (task == SelfBotQuestTask::UseItem)
    {
        if (!use)
            return false;
        if (creature && !creature->IsAlive())
            return use->RequiresDead;
        bool const elite = source.CreatureRank != CREATURE_ELITE_NORMAL && source.CreatureRank != CREATURE_ELITE_RARE;
        if (use->RequiresDead)
            return SelfBotQuestPlanning::AttackSourceAllowed(source.CreatureMaxLevel,
                                                             creature ? creature->GetLevel() : 0, elite,
                                                             creature && creature->isElite(), bot->GetLevel()) &&
                   reaction <= REP_NEUTRAL && (!creature || bot->IsValidAttackTarget(creature));
        return reaction > REP_NEUTRAL ||
               SelfBotQuestPlanning::AttackSourceAllowed(source.CreatureMaxLevel, creature ? creature->GetLevel() : 0,
                                                         elite, creature && creature->isElite(), bot->GetLevel());
    }
    if (task != SelfBotQuestTask::Objective && task != SelfBotQuestTask::Item)
        return true;
    if (task == SelfBotQuestTask::Objective && reaction >= REP_FRIENDLY &&
        (creature ? creature->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) : (source.NpcFlags & UNIT_NPC_FLAG_GOSSIP) != 0))
        return true;
    bool const templateElite =
        source.CreatureRank != CREATURE_ELITE_NORMAL && source.CreatureRank != CREATURE_ELITE_RARE;
    return SelfBotQuestPlanning::AttackSourceAllowed(source.CreatureMaxLevel, creature ? creature->GetLevel() : 0,
                                                     templateElite, creature && creature->isElite(), bot->GetLevel()) &&
           reaction <= REP_NEUTRAL && (!creature || (creature->IsAlive() && bot->IsValidAttackTarget(creature)));
}

SelfBotQuestPlanning::Candidate QuestValue(Player* bot, Quest const* quest, SelfBotQuestTask task)
{
    SelfBotQuestPlanning::Candidate value;
    value.QuestId = quest->GetQuestId();
    value.Kind = task;
    value.QuestLevel = quest->GetQuestLevel();
    value.NativeQuestXp = quest->XPValue(bot->GetLevel());
    value.HasFollowUp = quest->GetNextQuestId() || quest->GetNextQuestInChain();
    uint64 required = 0;
    uint64 completed = 0;
    uint16 const slot = bot->FindQuestSlot(quest->GetQuestId());
    for (uint8 index = 0; index < QUEST_OBJECTIVES_COUNT; ++index)
    {
        required += quest->RequiredNpcOrGoCount[index];
        if (slot < MAX_QUEST_LOG_SIZE)
            completed += std::min(uint32(bot->GetQuestSlotCounter(slot, index)), quest->RequiredNpcOrGoCount[index]);
    }
    for (uint8 index = 0; index < QUEST_ITEM_OBJECTIVES_COUNT; ++index)
    {
        required += quest->RequiredItemCount[index];
        if (quest->RequiredItemId[index])
            completed += std::min(bot->GetItemCount(quest->RequiredItemId[index]), quest->RequiredItemCount[index]);
    }
    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT))
    {
        ++required;
        auto const status = bot->getQuestStatusMap().find(quest->GetQuestId());
        if (status != bot->getQuestStatusMap().end() && status->second.Explored)
            ++completed;
    }
    value.ProgressPercent = required ? uint32(100 * completed / required) : 0;
    return value;
}

char const* PickupBlockedReason(Player* bot, SelfBotQuestDefinition const& definition, SelfBotQuestState const& state,
                                WorldPosition const& giver, float searchRadius, float* continuationDistance = nullptr)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(definition.Id);
    if (!definition.Supported || !QuestSafeForWork(bot, quest))
        return "quest mechanism or difficulty is not supported";

    SelfBotQuestPlanning::State valueState;
    valueState.OrderedQuestId = state.OrderedQuestId;
    valueState.HeroLevel = bot->GetLevel();
    valueState.LevelXp = sObjectMgr->GetXPForLevel(bot->GetLevel());
    float const sourceRadius =
        std::min(SelfBotQuestPlanning::FutureSourceRadius(searchRadius),
                 SelfBotQuestPlanning::TravelLimit(QuestValue(bot, quest, SelfBotQuestTask::Accept), valueState));
    if (continuationDistance)
        *continuationDistance = 0.0f;
    auto hasSafeSource =
        [&](SelfBotQuestSources const& sources, SelfBotQuestTask task, SelfBotQuestItemUse const* use = nullptr)
    {
        float closest = std::numeric_limits<float>::max();
        for (SelfBotQuestSource const* known : sources)
        {
            SelfBotQuestSource const& source = *known;
            auto const blocked = state.BlockedSources.find(SourceKey(source));
            float const distance = giver.GetExactDist(source.Position);
            if (source.Position.GetMapId() != bot->GetMapId() || distance > sourceRadius || distance >= closest ||
                !SelfBotQuestCatalog::IsMapSupported(source.Position.GetMapId()) ||
                (blocked != state.BlockedSources.end() && blocked->second == state.Fingerprint))
                continue;
            WorldObject* object = ResolveSource(bot, source);
            // A respawning creature or short cooldown does not make its quest permanently infeasible.
            if (Creature* creature = object ? object->ToCreature() : nullptr; creature && !creature->IsAlive())
                object = nullptr;
            if (SourceSafeForWork(bot, source, task, object, use))
                closest = distance;
        }
        if (closest == std::numeric_limits<float>::max())
            return false;
        if (continuationDistance)
            *continuationDistance = std::max(*continuationDistance, closest);
        return true;
    };
    auto hasSafeItemUse = [&](bool itemObjective, uint8 objective)
    {
        for (SelfBotQuestItemUse const& use : definition.ItemUses)
            if (use.ItemObjective == itemObjective && use.Objective == objective &&
                ItemUseAvailable(bot, quest, use, bot->GetQuestStatus(definition.Id) == QUEST_STATUS_NONE) &&
                hasSafeSource(use.Sources, SelfBotQuestTask::UseItem, &use))
                return true;
        return false;
    };

    if (!hasSafeSource(definition.Takers, SelfBotQuestTask::TurnIn))
        return "quest turn-in has no usable known source on this map";

    uint16 const slot = bot->FindQuestSlot(definition.Id);
    for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
    {
        uint32 const required = quest->RequiredNpcOrGoCount[i];
        uint32 const current = slot < MAX_QUEST_LOG_SIZE ? bot->GetQuestSlotCounter(slot, i) : 0;
        if (current < required &&
            !SelfBotQuestPlanning::RequirementFeasible(
                required, current, 0,
                hasSafeItemUse(false, i) || hasSafeSource(definition.ObjectiveSources[i], SelfBotQuestTask::Objective)))
            return "required creature or object has no safe known source";
    }
    for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
    {
        uint32 const required = quest->RequiredItemCount[i];
        if (!required)
            continue;
        uint32 const current = bot->GetItemCount(quest->RequiredItemId[i]);
        if (current >= required)
            continue;
        uint32 provided = 0;
        if (slot >= MAX_QUEST_LOG_SIZE && quest->RequiredItemId[i] == quest->GetSrcItemId())
        {
            uint32 const count = std::max(uint32(1), quest->GetSrcItemCount());
            ItemPosCountVec destinations;
            if (bot->CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, quest->GetSrcItemId(), count) == EQUIP_ERR_OK)
                provided = count;
        }
        if (!SelfBotQuestPlanning::RequirementFeasible(required, current, provided, false) &&
            !hasSafeItemUse(true, i) && !hasSafeSource(definition.ItemSources[i], SelfBotQuestTask::Item))
            return "required item has no safe known source";
    }
    auto const& statuses = bot->getQuestStatusMap();
    auto const status = statuses.find(definition.Id);
    bool const explored = slot < MAX_QUEST_LOG_SIZE && status != statuses.end() && status->second.Explored;
    if (quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_EXPLORATION_OR_EVENT) && !explored &&
        !hasSafeSource(definition.ExplorationSources, SelfBotQuestTask::Explore))
        return "exploration has no safe known source";
    return nullptr;
}

bool TaskNeeded(Player* bot, SelfBotQuestState const& state)
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(state.QuestId);
    if (!QuestAutomationAllowed(quest) || bot->GetQuestRewardStatus(state.QuestId))
        return false;

    if (state.Task == SelfBotQuestTask::Accept)
        return ActiveQuestCount(bot) < SelfBotQuestPlanning::MaxActiveQuests && QuestSafeForWork(bot, quest) &&
               bot->CanTakeQuest(quest, false) && bot->CanAddQuest(quest, false);

    QuestStatus const status = bot->GetQuestStatus(state.QuestId);
    if (state.Task == SelfBotQuestTask::TurnIn)
        return status == QUEST_STATUS_COMPLETE ||
               (status == QUEST_STATUS_INCOMPLETE && bot->CanCompleteQuest(state.QuestId));

    if (status != QUEST_STATUS_INCOMPLETE)
        return false;
    if (!QuestSafeForWork(bot, quest))
        return false;

    if (state.Task == SelfBotQuestTask::Explore)
    {
        return !CurrentObjectiveProgress(bot, state) && state.Source.AreaTriggerId &&
               sObjectMgr->GetQuestForAreaTrigger(state.Source.AreaTriggerId) == state.QuestId;
    }

    if (state.Task == SelfBotQuestTask::UseItem)
    {
        if (!CurrentItemUse(state))
            return false;
        uint32 const required = state.UseItemObjective ? quest->RequiredItemCount[state.Objective]
                                                       : quest->RequiredNpcOrGoCount[state.Objective];
        return CurrentObjectiveProgress(bot, state) < required;
    }
    if (state.Task == SelfBotQuestTask::Item)
        return bot->GetItemCount(quest->RequiredItemId[state.Objective]) < quest->RequiredItemCount[state.Objective];

    uint16 const slot = bot->FindQuestSlot(state.QuestId);
    return slot < MAX_QUEST_LOG_SIZE &&
           bot->GetQuestSlotCounter(slot, state.Objective) < quest->RequiredNpcOrGoCount[state.Objective];
}

bool ObserveItemUse(Player* bot, SelfBotQuestState& state)
{
    if (state.Task != SelfBotQuestTask::UseItem || state.Phase == SelfBotQuestPhase::Idle)
        return false;
    if (CurrentObjectiveProgress(bot, state) > state.TaskProgress || !TaskNeeded(bot, state))
    {
        DeferItemUse(bot, state, "native quest progress observed or objective no longer needed");
        return true;
    }
    if (state.UseStartedAt && time(nullptr) - state.UseStartedAt >= ItemUseTimeout)
    {
        DeferItemUse(bot, state, "item mechanic timed out without native quest progress");
        return true;
    }
    if (SelfBotQuestItemUse const* use = CurrentItemUse(state); use && ObserveItemMechanic(bot, state, *use))
        return false;
    if (!state.UseWaitUntil)
        return false;
    if (time(nullptr) < state.UseWaitUntil)
        return true;
    state.UseWaitUntil = 0;
    if (state.Attempts >= SelfBotQuestPlanning::MaxItemUseAttempts)
    {
        DeferItemUse(bot, state, "bounded item-use attempts produced no quest progress");
        return true;
    }
    return false;
}

bool PathInsideRegion(uint32 mapId, Movement::PointsArray const& points)
{
    if (points.size() < 2)
        return false;
    size_t samples = 0;
    for (size_t i = 0; i < points.size(); ++i)
    {
        auto const& point = points[i];
        if (++samples > MaxRouteSamples ||
            !SelfBotQuestCatalog::IsInRegion(WorldPosition(mapId, point.x, point.y, point.z)))
            return false;
        if (i == 0)
            continue;
        auto const& previous = points[i - 1];
        G3D::Vector3 const delta = point - previous;
        float const length = delta.length();
        for (float distance = RouteSampleDistance; distance < length; distance += RouteSampleDistance)
        {
            G3D::Vector3 const sample = previous + delta * (distance / length);
            if (++samples > MaxRouteSamples ||
                !SelfBotQuestCatalog::IsInRegion(WorldPosition(mapId, sample.x, sample.y, sample.z)))
                return false;
        }
    }
    return true;
}

float DistanceToRoute(WorldPosition const& position, Movement::PointsArray const& points)
{
    G3D::Vector3 const point(position.GetPositionX(), position.GetPositionY(), position.GetPositionZ());
    float closest = std::numeric_limits<float>::max();
    for (size_t index = 1; index < points.size(); ++index)
    {
        G3D::Vector3 const delta = points[index] - points[index - 1];
        float const squared = delta.squaredLength();
        float const fraction =
            squared > 0.0f ? std::clamp((point - points[index - 1]).dot(delta) / squared, 0.0f, 1.0f) : 0.0f;
        closest = std::min(closest, (point - (points[index - 1] + delta * fraction)).length());
    }
    return closest;
}

struct RouteCreatureVisitor
{
    explicit RouteCreatureVisitor(Player* bot) : Bot(bot) {}

    Player* Bot;
    std::vector<Creature*> Creatures;
    size_t Examined = 0;
    bool Complete = true;

    void Visit(CreatureMapType& creatures)
    {
        if (!Complete)
            return;
        for (auto itr = creatures.begin(); itr != creatures.end(); ++itr)
        {
            if (++Examined > MaxNearbyRouteCreatures)
            {
                Complete = false;
                return;
            }
            Creature* creature = itr->GetSource();
            if (creature && creature->IsInWorld() && creature->InSamePhase(Bot) &&
                Bot->IsWithinDistInMap(creature, RouteThreatQueryRadius))
                Creatures.push_back(creature);
        }
    }

    template <class T>
    void Visit(GridRefMgr<T>&)
    {
    }
};

bool RouteRisk(Player* bot, Movement::PointsArray const& points, float& cost)
{
    cost = 0.0f;
    SelfBotQuestCorridor const corridor =
        SelfBotQuestCatalog::Instance().QueryCreatureCorridor(bot->GetMapId(), points, RouteThreatQueryRadius);
    if (!corridor.Complete)
        return false;
    float const aggroRate = sWorld->getRate(RATE_CREATURE_AGGRO);
    constexpr float maxAggroRange = 45.0f;
    if (!std::isfinite(aggroRate) || aggroRate < 0.0f ||
        maxAggroRange * aggroRate + MaxThreatWanderDistance + SelfBotQuestPlanning::RouteAggroBuffer >
            RouteThreatQueryRadius)
        return false;
    std::unordered_set<uint64> loaded;
    auto assess = [&](SelfBotQuestSource const* source, Creature* creature)
    {
        if (creature)
        {
            if (!loaded.insert(creature->GetGUID().GetRawValue()).second || !creature->IsAlive() ||
                !creature->InSamePhase(bot) || creature->IsCivilian() || creature->IsImmuneToPC())
                return true;
        }
        else if (!source || !(source->PhaseMask & bot->GetPhaseMask()) || source->Civilian ||
                 (source->UnitFlags & UNIT_FLAG_IMMUNE_TO_PC))
            return true;
        bool const hostile = creature ? creature->IsHostileTo(bot) || creature->GetVictim() == bot
                                      : SourceReaction(bot, *source, nullptr) <= REP_HOSTILE;
        if (!hostile)
            return true;
        float aggro = 0.0f;
        float wander = 0.0f;
        if (creature)
            aggro = creature->GetAggroRange(bot);
        else
        {
            if (source->DetectionRange < 1.0f || aggroRate == 0.0f)
                return true;
            int32 const difference = int32(source->CreatureMaxLevel) - int32(bot->GetLevel());
            aggro = std::clamp(source->DetectionRange + std::min(difference, 25) +
                                   bot->GetTotalAuraModifier(SPELL_AURA_MOD_DETECTED_RANGE),
                               5.0f, maxAggroRange) *
                    aggroRate;
            if (source->MovementType)
                wander = std::min(MaxThreatWanderDistance, std::max(0.0f, source->WanderDistance));
        }
        if (aggro <= 0.0f)
            return true;
        if (!std::isfinite(aggro) || aggro + wander + SelfBotQuestPlanning::RouteAggroBuffer > RouteThreatQueryRadius)
            return false;
        WorldPosition const position = creature ? WorldPosition(creature) : WorldPosition(source->Position);
        unsigned const level = creature ? creature->GetLevel() : source->CreatureMaxLevel;
        bool const elite =
            creature ? creature->isElite()
                     : source->CreatureRank != CREATURE_ELITE_NORMAL && source->CreatureRank != CREATURE_ELITE_RARE;
        float const risk = SelfBotQuestPlanning::RouteThreatCost(bot->GetLevel(), level, elite, hostile,
                                                                 DistanceToRoute(position, points), aggro + wander);
        if (risk == SelfBotQuestPlanning::UnsafeRouteThreat)
            return false;
        cost += risk;
        return true;
    };
    for (SelfBotQuestSource const* source : corridor.Sources)
    {
        WorldObject* object = ResolveSource(bot, *source);
        if (!assess(source, object ? object->ToCreature() : nullptr))
            return false;
    }

    // Include nearby moved and summoned creatures that have no static spawn inside the corridor.
    RouteCreatureVisitor nearby(bot);
    Cell::VisitObjects(bot, nearby, RouteThreatQueryRadius);
    if (!nearby.Complete)
        return false;
    for (Creature* creature : nearby.Creatures)
        if (!assess(nullptr, creature))
            return false;
    return true;
}

struct QuestRoute
{
    WorldPosition Step;
    float Distance = 0.0f;
    float RiskCost = 0.0f;
    bool Checked = false;
    bool PendingTerrain = false;
    char const* Failure = "No safe reachable source for the current quest; waiting to retry.";
    uint32 PathFlags = 0;
};

enum class RouteTerrain
{
    Ready,
    Pending,
    Invalid
};

RouteTerrain PrepareRouteTerrain(Player* bot, WorldPosition const& destination, float travelLimit, size_t& gridBudget)
{
    // Every route within the length budget lies within this midpoint-centred square.
    float const middleX = (bot->GetPositionX() + destination.GetPositionX()) / 2.0f;
    float const middleY = (bot->GetPositionY() + destination.GetPositionY()) / 2.0f;
    float const radius = travelLimit / 2.0f;
    auto clampCoordinate = [](float value) { return std::clamp(value, -MAP_HALFSIZE + 1.0f, MAP_HALFSIZE - 1.0f); };
    GridCoord const first =
        Acore::ComputeGridCoord(clampCoordinate(middleX - radius), clampCoordinate(middleY - radius));
    GridCoord const last =
        Acore::ComputeGridCoord(clampCoordinate(middleX + radius), clampCoordinate(middleY + radius));
    if (!first.IsCoordValid() || !last.IsCoordValid())
        return RouteTerrain::Invalid;
    uint32 const minX = std::min(first.x_coord, last.x_coord);
    uint32 const maxX = std::max(first.x_coord, last.x_coord);
    uint32 const minY = std::min(first.y_coord, last.y_coord);
    uint32 const maxY = std::max(first.y_coord, last.y_coord);
    if ((maxX - minX + 1) * (maxY - minY + 1) > MaxRouteGrids)
        return RouteTerrain::Invalid;
    for (uint32 x = minX; x <= maxX; ++x)
        for (uint32 y = minY; y <= maxY; ++y)
        {
            GridCoord const grid(x, y);
            if (bot->GetMap()->IsGridCreated(grid))
                continue;
            if (!gridBudget)
                return RouteTerrain::Pending;
            --gridBudget;
            // Creation loads terrain/navigation only; LoadGrid would also activate creatures and objects.
            bot->GetMap()->EnsureGridCreated(grid);
        }
    return RouteTerrain::Ready;
}

bool RegionStep(Player* bot, WorldPosition const& destination, float travelLimit, size_t& gridBudget,
                QuestRoute& result)
{
    auto reject = [&](char const* reason)
    {
        result.Failure = reason;
        return false;
    };
    if (bot->GetMapId() != destination.GetMapId() || !SelfBotQuestCatalog::IsInRegion(destination) ||
        !std::isfinite(travelLimit) || travelLimit < 0.0f || bot->GetExactDist(destination) > travelLimit)
        return reject("Quest source is outside the remaining travel budget or supported map.");

    // Prepare first: PathGenerator's shortcut fallback can otherwise load endpoint terrain without our budget.
    RouteTerrain const terrain = PrepareRouteTerrain(bot, destination, travelLimit, gridBudget);
    if (terrain != RouteTerrain::Ready)
    {
        result.PendingTerrain = terrain == RouteTerrain::Pending;
        return reject("Navigation terrain is not ready for the complete quest route.");
    }
    // A partial route cannot establish either its total cost or the danger beyond its last point.
    PathGenerator route(bot, PathGenerator::SearchMode::LongDistance);
    route.SetUseStraightPath(true);
    bool const calculated =
        route.CalculatePath(destination.GetPositionX(), destination.GetPositionY(), destination.GetPositionZ());
    result.PathFlags = route.GetPathType();
    if (!calculated || route.GetPathType() != PATHFIND_NORMAL)
        return reject("No complete navigation path to the quest source.");
    if (!PathInsideRegion(bot->GetMapId(), route.GetPath()))
        return reject("Quest route leaves supported terrain or exceeds the route sampling budget.");
    auto const& points = route.GetPath();
    auto const& end = points.back();
    if (destination.GetExactDist(end.x, end.y, end.z) > ArrivalDistance)
        return reject("Navigation path stops short of the quest source.");
    result.Distance = route.getPathLength();
    if (!std::isfinite(result.Distance) || result.Distance > travelLimit)
        return reject("Complete quest route exceeds the remaining travel budget.");
    if (!RouteRisk(bot, points, result.RiskCost))
        return reject("Quest route is unsafe or its creature corridor could not be fully checked.");

    G3D::Vector3 waypoint = points.front();
    float remaining = TravelStepDistance;
    for (size_t i = 1; i < points.size(); ++i)
    {
        G3D::Vector3 const delta = points[i] - points[i - 1];
        float const length = delta.length();
        if (length > remaining)
        {
            waypoint = points[i - 1] + delta * (remaining / length);
            break;
        }
        waypoint = points[i];
        remaining -= length;
    }
    if (bot->GetExactDist(waypoint.x, waypoint.y, waypoint.z) < ProgressDistance)
        return reject("Navigation path does not provide a usable next travel step.");

    PathGenerator path(bot);
    if (!path.CalculatePath(waypoint.x, waypoint.y, waypoint.z) || path.GetPathType() != PATHFIND_NORMAL ||
        !PathInsideRegion(bot->GetMapId(), path.GetPath()))
        return reject("The next quest travel step has no complete navigation path.");
    result.Distance += std::max(0.0f, path.getPathLength() - std::min(TravelStepDistance, result.Distance));
    float stepRisk = 0.0f;
    if (result.Distance > travelLimit || !RouteRisk(bot, path.GetPath(), stepRisk))
        return reject("The next quest travel step exceeds the travel budget or is unsafe.");
    result.RiskCost = std::max(result.RiskCost, stepRisk);
    auto const& actual = path.GetActualEndPosition();
    result.Step = WorldPosition(bot->GetMapId(), actual.x, actual.y, actual.z);
    result.Checked = true;
    return true;
}

bool ReachedExploration(Player* bot, SelfBotQuestState const& state)
{
    return state.Task == SelfBotQuestTask::Explore &&
           bot->IsInAreaTriggerRadius(sObjectMgr->GetAreaTrigger(state.Source.AreaTriggerId));
}

bool Arrived(Player* bot, WorldObject* object, SelfBotQuestTask task, uint32 useSpellId = 0)
{
    if (!object || !object->InSamePhase(bot))
        return false;
    if (task == SelfBotQuestTask::UseItem)
    {
        SpellInfo const* spell = sSpellMgr->GetSpellInfo(useSpellId);
        if (!spell || !bot->IsWithinLOSInMap(object))
            return false;
        float const range = std::max(spell->GetMaxRange(false), spell->GetMaxRange(true));
        return bot->GetDistance(object) <= std::clamp(range - 1.0f, 1.0f, 10.0f);
    }
    if (SelfBotQuestPlanning::IsWorkTask(task))
        if (GameObject* gameObject = object->ToGameObject())
            // A quest object's origin can be inside its own collision model.
            return gameObject->isSpawned() && gameObject->IsWithinDistInMap(bot) &&
                   bot->GetDistance(gameObject) <= ArrivalDistance;
    if (!bot->IsWithinLOSInMap(object))
        return false;
    if (task == SelfBotQuestTask::Accept || task == SelfBotQuestTask::TurnIn)
        return bot->CanInteractWithQuestGiver(object);
    return bot->GetDistance(object) <= ArrivalDistance;
}

void RefreshRows(Player* bot, SelfBotQuestState& state)
{
    std::vector<SelfBotQuestRow> rows;
    if (state.OrderedQuestId)
    {
        SelfBotQuestRow row = SelfBotQuestCatalog::Instance().EvaluateQuest(bot, state.OrderedQuestId);
        if (row.Quest)
            rows.push_back(std::move(row));
    }
    else
        rows = SelfBotQuestCatalog::Instance().Evaluate(bot, state.OpportunityAnchor, state.OpportunityRadius);
    uint64 fingerprint = SelfBotQuestCatalog::Fingerprint(bot);
    constexpr uint64 HASH_COMBINE = 0x9e3779b97f4a7c15ULL;
    auto combine = [&](uint64 value) { fingerprint ^= value + HASH_COMBINE + (fingerprint << 6) + (fingerprint >> 2); };
    combine(bot->GetPhaseMask());
    combine(bot->GetMapId());
    for (auto const& [id, faction] : bot->GetReputationMgr().GetStateList())
    {
        combine(id);
        combine(uint64(faction.Standing));
        combine(faction.Flags);
    }
    bool const rowsChanged = rows.size() != state.Rows.size() ||
                             !std::equal(rows.begin(), rows.end(), state.Rows.begin(),
                                         [](auto const& left, auto const& right)
                                         {
                                             return left.Quest->Id == right.Quest->Id && left.Status == right.Status &&
                                                    left.Rewarded == right.Rewarded && left.CanTake == right.CanTake &&
                                                    left.CanAdd == right.CanAdd &&
                                                    left.EligibilityReason == right.EligibilityReason;
                                         });
    // Spatial query membership is not player progress and must not release failed-source suppression.
    bool const changed = rowsChanged || state.Fingerprint != fingerprint;
    state.Rows = std::move(rows);
    state.Fingerprint = fingerprint;
    state.RefreshAt = time(nullptr) + RefreshDelay;
    if (!changed)
        return;

    LOG_INFO(
        "playerbots",
        "SelfBot {} quest list refreshed: level {} region {} catalog {} selected {} {} pickup={} work={} horizon={}",
        bot->GetName(), bot->GetLevel(), bot->GetAreaId(), state.Rows.size(), state.QuestId, TaskName(state.Task),
        state.PickupActive, state.WorkActive, state.OpportunityRadius);
    for (SelfBotQuestRow const& row : state.Rows)
    {
        std::ostringstream progress;
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(row.Quest->Id))
        {
            uint16 const slot = bot->FindQuestSlot(row.Quest->Id);
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredNpcOrGo[i] && slot < MAX_QUEST_LOG_SIZE)
                    progress << " objective[" << uint32(i) << "]=" << bot->GetQuestSlotCounter(slot, i) << "/"
                             << quest->RequiredNpcOrGoCount[i];
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] && slot < MAX_QUEST_LOG_SIZE)
                    progress << " item[" << quest->RequiredItemId[i]
                             << "]=" << bot->GetItemCount(quest->RequiredItemId[i]) << "/"
                             << quest->RequiredItemCount[i];
        }
        LOG_INFO("playerbots",
                 "SelfBot {} quest {} [{}] status={} rewarded={} canTake={} canAdd={} supported={} "
                 "eligibility={} mechanism={}{} questLevel={} executor={}",
                 bot->GetName(), row.Quest->Id, row.Quest->Title, uint32(row.Status), row.Rewarded, row.CanTake,
                 row.CanAdd, row.Quest->Supported, row.EligibilityReason, row.Quest->UnsupportedReason, progress.str(),
                 row.Quest->QuestLevel,
                 row.Status == QUEST_STATUS_COMPLETE ? "earned reward visit allowed"
                 : !SelfBotQuestPlanning::QuestLevelAllowed(row.Quest->QuestLevel, bot->GetLevel())
                     ? "deferred: quest level exceeds hero+3"
                 : row.Quest->SuggestedPlayers > 1 ? "deferred: group quest"
                                                   : "difficulty allowed");
    }
}

uint32 ChooseReward(Player* bot, AiObjectContext* context, Quest const* quest)
{
    uint32 selected = 0;
    int bestUpgrade = -1;
    uint64 bestPrice = 0;
    for (uint32 i = 0; i < quest->GetRewChoiceItemsCount(); ++i)
    {
        ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest->RewardChoiceItemId[i]);
        if (!item || !bot->CanRewardQuest(quest, i, false))
            continue;
        ItemUsage const usage = context->GetValue<ItemUsage>("item usage", item->ItemId)->Get();
        int const upgrade = usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE ? 1 : 0;
        uint64 const price = uint64(item->SellPrice) * quest->RewardChoiceItemCount[i];
        if (upgrade > bestUpgrade || (upgrade == bestUpgrade && price > bestPrice))
        {
            selected = i;
            bestUpgrade = upgrade;
            bestPrice = price;
        }
    }
    return selected;
}

class SelfBotQuestMultiplier : public Multiplier
{
public:
    SelfBotQuestMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "selfbot quest") {}

    float GetValue(Action* action) override
    {
        if (!action || !sPlayerbotAIConfig.SelfBotQuesting || !IsSelfBot(bot))
            return 1.0f;

        std::string const name = action->getName();
        SelfBotQuestState const& state = QuestState(context);
        if (state.Task == SelfBotQuestTask::UseItem && state.Phase != SelfBotQuestPhase::Idle &&
            !state.UseEffectObserved && state.UseRequiresDead && state.UseTarget &&
            name.find("loot") != std::string::npos)
            return 0.0f;
        if (dynamic_cast<AttackAnythingAction*>(action) || dynamic_cast<MoveRandomAction*>(action) ||
            name.find("rpg") != std::string::npos || name.find("travel") != std::string::npos ||
            name == "accept all quests" || name == "talk to quest giver")
            return 0.0f;

        return 1.0f;
    }
};

class SelfBotQuestItemCombatMultiplier : public Multiplier
{
public:
    SelfBotQuestItemCombatMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "selfbot quest item combat") {}

    float GetValue(Action* action) override
    {
        SelfBotQuestState const& state = QuestState(context);
        if (!action || !ItemCombatActive(botAI, state) || state.UseEffectObserved || bot->GetHealthPct() < 50.0f)
            return 1.0f;
        if (dynamic_cast<SelfBotQuestItemCombatAction*>(action) || dynamic_cast<CastHealingSpellAction*>(action) ||
            dynamic_cast<UseHealingPotion*>(action) || dynamic_cast<FleeAction*>(action) ||
            dynamic_cast<FleeWithPetAction*>(action) || dynamic_cast<RunAwayAction*>(action))
            return 1.0f;
        return 0.0f;
    }
};
}  // namespace

SelfBotQuestStateValue::~SelfBotQuestStateValue()
{
    SelfBotQuestItemCasting::RestoreDamage(bot, value.UseDamagePause, true);
}

void SelfBotQuestStateValue::Reset()
{
    SelfBotQuestItemCasting::RestoreDamage(bot, value.UseDamagePause);
    auto const pending = value.UseDamagePause;
    ManualSetValue<SelfBotQuestState>::Reset();
    value.UseDamagePause = pending;
}

void SelfBotQuestStateValue::Update()
{
    if (value.UseDamagePause.Active &&
        (value.Phase == SelfBotQuestPhase::Idle || value.UseEffectObserved || !sPlayerbotAIConfig.SelfBotQuesting ||
         !botAI->HasStrategy("selfbot quest", BOT_STATE_NON_COMBAT)))
        SelfBotQuestItemCasting::RestoreDamage(bot, value.UseDamagePause);
}

void PreserveSelfBotQuestPetState(PlayerbotAI* botAI)
{
    if (!botAI || !botAI->GetBot() || botAI->GetMaster() != botAI->GetBot())
        return;
    auto pause = QuestState(botAI->GetAiObjectContext()).UseDamagePause;
    SelfBotQuestItemCasting::RestoreDamage(botAI->GetBot(), pause, true);
}

void UpdateSelfBotQuestState(PlayerbotAI* botAI)
{
    if (!botAI || !botAI->GetBot() || botAI->GetMaster() != botAI->GetBot())
        return;
    if (UntypedValue* state = botAI->GetAiObjectContext()->GetUntypedValue("selfbot quest state"))
        state->Update();
}

bool SelfBotQuestTrigger::IsActive() { return Connected(bot); }

void SelfBotQuestStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);
    // Loot actions (6..8) run before quest travel so required drops are collected first.
    triggers.push_back(new TriggerNode("selfbot quest", {NextAction("choose selfbot quest", ACTION_DEFAULT + 0.4f),
                                                         NextAction("move to selfbot quest", ACTION_DEFAULT + 0.1f),
                                                         NextAction("interact selfbot quest", ACTION_DEFAULT + 0.2f)}));
}

void SelfBotQuestStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    Strategy::InitMultipliers(multipliers);
    multipliers.push_back(new SelfBotQuestMultiplier(botAI));
}

bool SelfBotQuestItemCombatTrigger::IsActive()
{
    SelfBotQuestState const& state = QuestState(context);
    return ItemCombatActive(botAI, state);
}

void SelfBotQuestItemCombatStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);
    triggers.push_back(
        new TriggerNode("selfbot quest item combat", {NextAction("selfbot quest item combat", ACTION_RAID + 1.0f)}));
}

void SelfBotQuestItemCombatStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    Strategy::InitMultipliers(multipliers);
    multipliers.push_back(new SelfBotQuestItemCombatMultiplier(botAI));
}

bool SelfBotQuestItemCombatAction::isUseful()
{
    SelfBotQuestState const& state = QuestState(context);
    if (!ItemCombatActive(botAI, state))
        return false;
    if (!state.UseEffectObserved)
        return true;
    if ((state.UseStartedAt && time(nullptr) - state.UseStartedAt >= ItemUseTimeout) ||
        time(nullptr) >= state.Deadline || bot->GetHealthPct() < 50.0f || !TaskNeeded(bot, state) ||
        CurrentObjectiveProgress(bot, state) > state.TaskProgress)
        return true;
    WorldObject* object = ResolveTaskSource(bot, state);
    SelfBotQuestItemUse const* use = CurrentItemUse(state);
    if (use && use->Kind == SelfBotQuestItemKind::Weaken && use->ResultAura && object && object->ToCreature() &&
        (use->ResultAuraRemoved ? object->ToCreature()->HasAura(use->ResultAura)
                                : !object->ToCreature()->HasAura(use->ResultAura)))
        return true;
    return !object || !object->ToCreature() || !object->ToCreature()->IsAlive() || bot->GetVictim() != object ||
           !bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING);
}

bool SelfBotQuestItemCombatAction::Execute(Event /*event*/) { return isUseful() && ExecuteItemUse(); }

bool ChooseSelfBotQuestAction::isUseful()
{
    SelfBotQuestState const& state = QuestState(context);
    return CanWork(bot, context) && (time(nullptr) >= state.RefreshAt ||
                                     (state.Phase == SelfBotQuestPhase::Idle && time(nullptr) >= state.RetryAt));
}

bool ChooseSelfBotQuestAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;

    SelfBotQuestState& state = QuestState(context);
    SelfBotActionQueue* queue = ActionQueue(bot);
    if (queue && queue->Selected())
    {
        while (queue->Running() && queue->Current() && bot->IsQuestRewarded(queue->Current()->QuestId))
            queue->Complete(queue->Current()->Id);
        if (!queue->Running() || !queue->Current())
        {
            ClearTask(bot, state);
            state.OrderedQuestId = 0;
            return true;
        }
        if (state.OrderedQuestId != queue->Current()->QuestId)
        {
            ClearTask(bot, state);
            state.OrderedQuestId = queue->Current()->QuestId;
            state.RouteFailure.clear();
            state.PickupActive = false;
            state.WorkActive = false;
            ResetOpportunity(WorldPosition(bot), state);
        }
    }
    Resume(bot, state);
    if (!state.OpportunityAnchored || state.OpportunityAnchor.GetMapId() != bot->GetMapId())
    {
        state.PickupActive = false;
        state.WorkActive = false;
        state.PickupAllowed = true;
        ResetOpportunity(WorldPosition(bot), state);
    }
    if (ObserveItemUse(bot, state) || ReseedObjectiveLoot(bot, context, state))
        return true;
    time_t const now = time(nullptr);
    if (state.Task == SelfBotQuestTask::UseItem && state.Phase != SelfBotQuestPhase::Idle &&
        (state.UseStartedAt || state.UseEffectObserved))
    {
        state.RefreshAt = now + RefreshDelay;
        return false;
    }
    if (now >= state.RefreshAt)
        RefreshRows(bot, state);

    if (state.OrderedQuestId)
    {
        std::string reason;
        if (state.Rows.empty())
            reason = "Quest is not in the server quest catalog.";
        else
        {
            SelfBotQuestRow const& row = state.Rows.front();
            if (!row.Quest->ExclusionReason.empty())
                reason = row.Quest->ExclusionReason;
            else if (row.Status == QUEST_STATUS_FAILED ||
                     (row.Status == QUEST_STATUS_NONE && (!row.CanTake || !row.CanAdd)))
                reason = row.EligibilityReason;
            else if (row.Status != QUEST_STATUS_COMPLETE && !row.Quest->Supported)
                reason = row.Quest->UnsupportedReason;
            else if (row.Status != QUEST_STATUS_COMPLETE &&
                     (!SelfBotQuestPlanning::QuestLevelAllowed(row.Quest->QuestLevel, bot->GetLevel()) ||
                      row.Quest->SuggestedPlayers > 1))
                reason = "Quest difficulty exceeds the solo executor limits.";
        }
        if (!reason.empty())
        {
            queue->ReportBlocked(std::move(reason));
            ClearTask(bot, state);
            state.RetryAt = now + EmptyQueueDelay;
            return true;
        }
    }

    SelfBotQuestPlanning::State planning;
    planning.OrderedQuestId = state.OrderedQuestId;
    planning.HeroLevel = bot->GetLevel();
    planning.LevelXp = sObjectMgr->GetXPForLevel(bot->GetLevel());
    planning.ActiveCount = ActiveQuestCount(bot);
    planning.HasCurrent = state.Phase != SelfBotQuestPhase::Idle;
    planning.CurrentUsable =
        planning.HasCurrent && TaskNeeded(bot, state) &&
        SourceSafeForWork(bot, state.Source, state.Task, ResolveTaskSource(bot, state), CurrentItemUse(state));
    planning.CurrentTask = state.Task;
    if (Quest const* current = sObjectMgr->GetQuestTemplate(state.QuestId))
    {
        planning.CurrentQuestLevel = current->GetQuestLevel();
        SelfBotQuestPlanning::Candidate currentValue = QuestValue(bot, current, state.Task);
        WorldObject* object = ResolveTaskSource(bot, state);
        WorldPosition const currentPosition = object ? WorldPosition(object) : WorldPosition(state.Source.Position);
        currentValue.Distance = bot->GetExactDist(currentPosition);
        currentValue.RouteDistance = std::max(currentValue.Distance, state.TaskRouteDistance - state.TravelledDistance);
        currentValue.RiskCost = state.TaskRiskCost;
        currentValue.ContinuationDistance = state.TaskContinuationDistance;
        if (state.PickupActive)
            currentValue.PickupDistance = currentPosition.GetExactDist(state.PickupAnchor);
        if (state.WorkActive)
            currentValue.WorkDistance = currentPosition.GetExactDist(state.WorkAnchor);
        planning.CurrentUtility = SelfBotQuestPlanning::CandidateUtility(currentValue, planning);
        planning.CurrentUsable = planning.CurrentUsable &&
                                 currentValue.RouteDistance + state.TravelledDistance <= state.TaskTravelLimit &&
                                 SelfBotQuestPlanning::Usable(currentValue, planning);
    }
    if (planning.HasCurrent && !planning.CurrentUsable)
    {
        ClearTask(bot, state);
        planning.HasCurrent = false;
    }
    if (now < state.RetryAt)
        return false;
    if (planning.CurrentUsable && state.Task == SelfBotQuestTask::UseItem && state.UseRequiresDead && state.UseTarget)
        if (WorldObject* object = ResolveTaskSource(bot, state);
            object && object->ToCreature() && !object->ToCreature()->IsAlive())
        {
            // Finish this expiring corpse before considering a different quest cluster.
            state.RefreshAt = now + RefreshDelay;
            return true;
        }

    for (auto itr = state.FailedSources.begin(); itr != state.FailedSources.end();)
        if (itr->second <= now)
            itr = state.FailedSources.erase(itr);
        else
            ++itr;
    for (auto itr = state.DeadSources.begin(); itr != state.DeadSources.end();)
        if (itr->second <= now)
            itr = state.DeadSources.erase(itr);
        else
            ++itr;
    for (auto itr = state.CostDeferrals.begin(); itr != state.CostDeferrals.end();)
        if (itr->second <= now)
            itr = state.CostDeferrals.erase(itr);
        else
            ++itr;
    for (auto itr = state.ItemUseDeferrals.begin(); itr != state.ItemUseDeferrals.end();)
        if (itr->second <= now)
            itr = state.ItemUseDeferrals.erase(itr);
        else
            ++itr;
    for (auto itr = state.BlockedSources.begin(); itr != state.BlockedSources.end();)
        if (itr->second != state.Fingerprint)
        {
            state.SourceFailures.erase(itr->first);
            itr = state.BlockedSources.erase(itr);
        }
        else
            ++itr;

    planning.PickupActive = state.PickupActive;
    planning.PickupAllowed = state.PickupAllowed || planning.ActiveCount == 0;
    planning.WorkActive = state.WorkActive;
    planning.WorkSteps = state.WorkSteps;
    planning.WorkAgeSeconds = state.WorkStarted && now > state.WorkStarted ? uint32(now - state.WorkStarted) : 0;
    planning.OpportunityRadius = state.OpportunityRadius;

    std::vector<SelfBotQuestPlanning::Candidate> candidates;
    SelfBotQuestSources sources;
    std::vector<SelfBotQuestItemUse const*> itemUses;
    std::map<uint32, std::string> pickupDeferrals;
    uint32 unsafeSources = 0;
    auto add = [&](SelfBotQuestRow const& row, SelfBotQuestTask task, uint8 objective,
                   SelfBotQuestSources const& questSources, SelfBotQuestItemUse const* use = nullptr)
    {
        if (task != SelfBotQuestTask::TurnIn &&
            (!SelfBotQuestPlanning::QuestLevelAllowed(row.Quest->QuestLevel, planning.HeroLevel) ||
             row.Quest->SuggestedPlayers > 1))
        {
            unsafeSources += uint32(questSources.size());
            return;
        }
        bool feasibleGiver = false;
        char const* pickupReason = nullptr;
        Quest const* quest = sObjectMgr->GetQuestTemplate(row.Quest->Id);
        if (!quest)
            return;
        if (use && !ItemUseAvailable(bot, quest, *use, false))
            return;
        SelfBotQuestPlanning::Candidate const value = QuestValue(bot, quest, task);
        for (SelfBotQuestSource const* known : questSources)
        {
            SelfBotQuestSource const& source = *known;
            uint64 const key = SourceKey(source);
            if (source.Position.GetMapId() != bot->GetMapId() ||
                !SelfBotQuestCatalog::IsMapSupported(source.Position.GetMapId()) || state.BlockedSources.count(key))
                continue;
            bool const pickupSource = state.PickupActive && source.Position.GetExactDist(state.PickupAnchor) <=
                                                                SelfBotQuestPlanning::PickupRadius;
            if (source.Position.GetExactDist(state.OpportunityAnchor) > state.OpportunityRadius && !pickupSource)
                continue;
            WorldObject* object = ResolveSource(bot, source);
            Creature* creature = object ? object->ToCreature() : nullptr;
            bool const dead = creature && !creature->IsAlive();
            bool const unusableDead = dead && (!use || !use->RequiresDead);
            WorldPosition const position = object ? WorldPosition(object) : WorldPosition(source.Position);
            SelfBotQuestPlanning::Candidate candidate = value;
            candidate.QuestId = row.Quest->Id;
            candidate.SourceKey = key;
            candidate.Kind = task;
            candidate.Objective = objective;
            candidate.QuestLevel = row.Quest->QuestLevel;
            candidate.Distance = bot->GetExactDist(position);
            candidate.OpportunityDistance = position.GetExactDist(state.OpportunityAnchor);
            candidate.PickupDistance =
                state.PickupActive ? position.GetExactDist(state.PickupAnchor) : std::numeric_limits<float>::max();
            candidate.WorkDistance =
                state.WorkActive ? position.GetExactDist(state.WorkAnchor) : std::numeric_limits<float>::max();
            candidate.Safe = SourceSafeForWork(bot, source, task, unusableDead ? nullptr : object, use);
            candidate.Deferred =
                unusableDead || state.FailedSources.count(key) || state.CostDeferrals.count({row.Quest->Id, key});
            if ((!use || !use->RequiresDead) && state.DeadSources.count(key))
                candidate.Deferred = true;
            if (use && state.ItemUseDeferrals.count(ItemUseKey(row.Quest->Id, use->ItemId, use->SpellId, source,
                                                               object ? object->GetGUID() : ObjectGuid::Empty)))
                candidate.Deferred = true;
            if (!candidate.Safe)
            {
                ++unsafeSources;
                continue;
            }
            if (task == SelfBotQuestTask::Accept)
            {
                float searchRadius = state.OpportunityRadius;
                if (state.PickupActive && candidate.PickupDistance <= SelfBotQuestPlanning::PickupRadius)
                    searchRadius = std::max(searchRadius, state.PickupSearchRadius);
                if (char const* reason = PickupBlockedReason(bot, *row.Quest, state, position, searchRadius,
                                                             &candidate.ContinuationDistance))
                {
                    pickupReason = reason;
                    ++unsafeSources;
                    continue;
                }
                feasibleGiver = true;
            }
            if (unusableDead && !use && !state.DeadSources.count(key))
                state.DeadSources[key] = now + DeadSourceDelay;
            candidates.push_back(candidate);
            sources.push_back(known);
            itemUses.push_back(use);
        }
        if (task == SelfBotQuestTask::Accept && !feasibleGiver && pickupReason)
            pickupDeferrals.emplace(row.Quest->Id, pickupReason);
    };

    for (SelfBotQuestRow const& row : state.Rows)
    {
        if (row.Rewarded || !row.Quest->ExclusionReason.empty())
            continue;
        Quest const* quest = sObjectMgr->GetQuestTemplate(row.Quest->Id);
        if (!quest)
            continue;
        QuestStatus const status = bot->GetQuestStatus(row.Quest->Id);
        if (status == QUEST_STATUS_COMPLETE ||
            (status == QUEST_STATUS_INCOMPLETE && bot->CanCompleteQuest(row.Quest->Id)))
            add(row, SelfBotQuestTask::TurnIn, 0, row.Quest->Takers);
        else if (!row.Quest->Supported)
            continue;
        else if (status == QUEST_STATUS_INCOMPLETE)
        {
            auto const& statuses = bot->getQuestStatusMap();
            auto const data = statuses.find(row.Quest->Id);
            if (data != statuses.end() && !data->second.Explored)
                add(row, SelfBotQuestTask::Explore, 0, row.Quest->ExplorationSources);
            uint16 const slot = bot->FindQuestSlot(row.Quest->Id);
            for (uint8 i = 0; i < QUEST_OBJECTIVES_COUNT; ++i)
                if (!HasItemUse(*row.Quest, false, i) && quest->RequiredNpcOrGo[i] && slot < MAX_QUEST_LOG_SIZE &&
                    bot->GetQuestSlotCounter(slot, i) < quest->RequiredNpcOrGoCount[i])
                    add(row, SelfBotQuestTask::Objective, i, row.Quest->ObjectiveSources[i]);
            for (uint8 i = 0; i < QUEST_ITEM_OBJECTIVES_COUNT; ++i)
                if (quest->RequiredItemId[i] &&
                    bot->GetItemCount(quest->RequiredItemId[i]) < quest->RequiredItemCount[i])
                {
                    SelfBotQuestSources directSources;
                    for (SelfBotQuestSource const* source : row.Quest->ItemSources[i])
                    {
                        bool const prepare = std::any_of(
                            row.Quest->ItemUses.begin(), row.Quest->ItemUses.end(),
                            [&](SelfBotQuestItemUse const& use)
                            {
                                return use.ItemObjective && use.Objective == i &&
                                       use.Continuation == SelfBotQuestItemContinuation::Loot &&
                                       ItemUseAvailable(bot, quest, use, false) &&
                                       std::find(use.Sources.begin(), use.Sources.end(), source) != use.Sources.end();
                            });
                        if (!prepare)
                            directSources.push_back(source);
                    }
                    add(row, SelfBotQuestTask::Item, i, directSources);
                }
            for (SelfBotQuestItemUse const& use : row.Quest->ItemUses)
            {
                bool const needed = use.ItemObjective
                                        ? bot->GetItemCount(quest->RequiredItemId[use.Objective]) <
                                              quest->RequiredItemCount[use.Objective]
                                        : slot < MAX_QUEST_LOG_SIZE && bot->GetQuestSlotCounter(slot, use.Objective) <
                                                                           quest->RequiredNpcOrGoCount[use.Objective];
                if (needed)
                    add(row, SelfBotQuestTask::UseItem, use.Objective, use.Sources, &use);
            }
        }
        else if (row.CanTake && row.CanAdd)
            add(row, SelfBotQuestTask::Accept, 0, row.Quest->Givers);
    }

    for (auto const& [questId, reason] : pickupDeferrals)
    {
        auto const previous = state.PickupDeferrals.find(questId);
        if (previous == state.PickupDeferrals.end() || previous->second != reason)
            LOG_INFO("playerbots", "SelfBot {} quest {} pickup deferred: {}", bot->GetName(), questId, reason);
    }
    state.PickupDeferrals = std::move(pickupDeferrals);
    if (planning.HasCurrent && state.Task == SelfBotQuestTask::Accept)
    {
        SelfBotQuestDefinition const* definition = SelfBotQuestCatalog::Instance().Find(state.QuestId);
        WorldObject* giver = ResolveSource(bot, state.Source);
        WorldPosition const position = giver ? WorldPosition(giver) : WorldPosition(state.Source.Position);
        float continuation = 0.0f;
        bool unusable = !definition ||
                        PickupBlockedReason(bot, *definition, state, position, state.TaskSearchRadius, &continuation);
        if (!unusable)
        {
            SelfBotQuestPlanning::Candidate value =
                QuestValue(bot, sObjectMgr->GetQuestTemplate(state.QuestId), SelfBotQuestTask::Accept);
            value.Distance = bot->GetExactDist(position);
            value.ContinuationDistance = continuation;
            unusable = !SelfBotQuestPlanning::Usable(value, planning);
        }
        if (unusable)
        {
            ClearTask(bot, state);
            planning.HasCurrent = false;
            planning.CurrentUsable = false;
        }
    }

    auto closeBatches = [&](SelfBotQuestPlanning::Decision const& decision)
    {
        if (decision.ClosePickup)
        {
            state.PickupActive = false;
            state.PickupAllowed = false;
        }
        if (decision.CloseWork)
            state.WorkActive = false;
    };
    SelfBotQuestPlanning::Reason waitingReason = SelfBotQuestPlanning::Reason::NoSafeTask;
    std::vector<QuestRoute> routes(candidates.size());
    size_t pathAttempts = 0;
    size_t gridBudget = MaxNewRouteGrids;
    bool pathBudgetExhausted = false;
    bool navigationPending = false;
    for (;;)
    {
        SelfBotQuestPlanning::Decision const decision = SelfBotQuestPlanning::Choose(candidates, planning);
        if (decision.KeepCurrent)
        {
            if (state.OrderedQuestId)
                queue->ReportRunning();
            return true;
        }
        if (decision.Index == SelfBotQuestPlanning::NoCandidate)
        {
            waitingReason = decision.Why;
            closeBatches(decision);
            break;
        }
        SelfBotQuestPlanning::Candidate& candidate = candidates[decision.Index];
        SelfBotQuestSource const& source = *sources[decision.Index];
        SelfBotQuestItemUse const* use = itemUses[decision.Index];
        WorldObject* object = ResolveSource(bot, source);
        WorldPosition const destination = object ? WorldPosition(object) : WorldPosition(source.Position);
        QuestRoute& route = routes[decision.Index];
        if (!route.Checked)
        {
            if (pathAttempts >= MaxPathAttempts)
            {
                pathBudgetExhausted = true;
                for (size_t index = 0; index < candidates.size(); ++index)
                    if (!routes[index].Checked)
                        candidates[index].Deferred = true;
                continue;
            }
            ++pathAttempts;
            bool const reached = source.LocationId ? bot->GetExactDist(source.Position) <= ArrivalDistance
                                 : candidate.Kind == SelfBotQuestTask::Explore
                                     ? bot->IsInAreaTriggerRadius(sObjectMgr->GetAreaTrigger(source.AreaTriggerId))
                                     : Arrived(bot, object, candidate.Kind, use ? use->SpellId : 0);
            if (reached)
            {
                route.Step = WorldPosition(bot);
                route.Checked = true;
            }
            else if (!RegionStep(bot, destination, SelfBotQuestPlanning::MaxTravelDistance, gridBudget, route))
            {
                if (route.PendingTerrain)
                {
                    navigationPending = true;
                    candidate.Deferred = true;
                    continue;
                }
                state.FailedSources[candidate.SourceKey] = now + FailedSourceDelay;
                if (++state.SourceFailures[candidate.SourceKey] >= MaxAttempts)
                    state.BlockedSources[candidate.SourceKey] = state.Fingerprint;
                for (SelfBotQuestPlanning::Candidate& alternative : candidates)
                    if (alternative.SourceKey == candidate.SourceKey)
                        alternative.Deferred = true;
                candidate.PathAvailable = false;
                candidate.Blocked = state.BlockedSources.count(candidate.SourceKey) != 0;
                if (state.OrderedQuestId)
                    state.RouteFailure = route.Failure;
                LOG_INFO(
                    "playerbots",
                    "SelfBot {} quest {} source {} spawn {} deferred: complete safe route within {} yards unavailable; "
                    "reason={} pathFlags={} routeDistance={}",
                    bot->GetName(), candidate.QuestId, source.Entry, source.Position.GetCounter(),
                    SelfBotQuestPlanning::MaxTravelDistance, route.Failure, route.PathFlags, route.Distance);
                continue;
            }
            candidate.RouteDistance = route.Distance;
            candidate.RiskCost = route.RiskCost;
            if (SelfBotQuestPlanning::TravelCost(candidate) > SelfBotQuestPlanning::TravelLimit(candidate, planning))
            {
                state.CostDeferrals[{candidate.QuestId, candidate.SourceKey}] = now + FailedSourceDelay;
                candidate.Deferred = true;
                LOG_INFO("playerbots", "SelfBot {} quest {} deferred: route cost {} exceeds value travel limit {}",
                         bot->GetName(), candidate.QuestId, SelfBotQuestPlanning::TravelCost(candidate),
                         SelfBotQuestPlanning::TravelLimit(candidate, planning));
            }
            // The measured detour and threats can change the winner of the optimistic shortlist.
            continue;
        }
        if (planning.HasCurrent)
            ClearTask(bot, state);
        closeBatches(decision);
        if (decision.StartPickup)
        {
            state.PickupActive = true;
            state.PickupAnchor = decision.PickupAtSource ? destination : WorldPosition(bot);
            state.PickupSearchRadius = planning.OpportunityRadius;
            LOG_INFO("playerbots", "SelfBot {} pickup hub anchored at {:.1f},{:.1f} radius={} active={}/{}",
                     bot->GetName(), state.PickupAnchor.GetPositionX(), state.PickupAnchor.GetPositionY(),
                     SelfBotQuestPlanning::PickupRadius, planning.ActiveCount, SelfBotQuestPlanning::MaxActiveQuests);
        }
        if (decision.StartWork)
        {
            state.WorkActive = true;
            state.WorkAnchor = destination;
            state.WorkSteps = 0;
            state.WorkStarted = now;
        }
        state.TaskSearchRadius = planning.OpportunityRadius;
        if (candidate.Kind == SelfBotQuestTask::Accept && state.PickupActive &&
            destination.GetExactDist(state.PickupAnchor) <= SelfBotQuestPlanning::PickupRadius)
            state.TaskSearchRadius = std::max(state.TaskSearchRadius, state.PickupSearchRadius);
        if (decision.StartPickup || decision.StartWork)
            ResetOpportunity(destination, state);
        state.QuestId = candidate.QuestId;
        state.Task = candidate.Kind;
        state.Objective = candidate.Objective;
        state.Source = source;
        state.UseItemId = use ? use->ItemId : 0;
        SelfBotQuestDefinition const* useQuest =
            use ? SelfBotQuestCatalog::Instance().Find(candidate.QuestId) : nullptr;
        state.UsePlanIndex = use && useQuest ? size_t(use - useQuest->ItemUses.data()) : 0;
        state.UseSpellId = use ? use->SpellId : 0;
        state.UseItemObjective = use && use->ItemObjective;
        state.UseRequiresDead = use && use->RequiresDead;
        state.UseTarget = use && object ? object->GetGUID() : ObjectGuid::Empty;
        state.UseWaitUntil = 0;
        state.UseStartedAt = 0;
        state.UseResultTarget.Clear();
        state.UseEffectObserved = false;
        state.UseHadResultAura = false;
        state.UseCombatControl =
            use && !use->RequiresDead && (source.Entry > 0 || use->Kind == SelfBotQuestItemKind::Summon);
        state.UseExistingSummons.clear();
        state.UseObservedHealth = 0;
        state.UseHealthStableAt = 0;
        state.TravelStep = route.Step;
        state.HasTravelStep = true;
        state.TaskUtility = SelfBotQuestPlanning::CandidateUtility(candidate, planning);
        state.TaskRouteDistance = route.Distance;
        state.TaskRiskCost = route.RiskCost;
        state.TaskContinuationDistance = candidate.ContinuationDistance;
        state.TaskTravelLimit = SelfBotQuestPlanning::TravelLimit(candidate, planning) -
                                (candidate.Kind == SelfBotQuestTask::Accept ? candidate.ContinuationDistance : 0.0f);
        state.TravelledDistance = 0.0f;
        state.LastTravelPosition = WorldPosition(bot);
        state.RiskCheckAt = now + RefreshDelay;
        state.TaskProgress = CurrentObjectiveProgress(bot, state);
        state.LootReseeded.Clear();
        state.Phase = SelfBotQuestPhase::Traveling;
        state.Attempts = 0;
        state.LastProgress = WorldPosition(bot);
        state.ProgressAt = now;
        float const speed = std::max(1.0f, bot->GetSpeed(MOVE_RUN));
        time_t const travelBudget = time_t(route.Distance * 3.0f / speed) + 60;
        state.Deadline = now + std::clamp(travelBudget, JourneyTimeout, MaxJourneyTimeout);
        LOG_INFO("playerbots",
                 "SelfBot {} selected quest {} {} source {} spawn {} distance {:.1f} questLevel={} "
                 "reason={} pickup={} work={} active={}/{} area={} currentArea={} trigger={} location={} "
                 "stepDistance={:.1f} "
                 "searchRadius={} futureSourceRadius={} route={} riskCost={} utility={} xp={} futureDistance={}",
                 bot->GetName(), state.QuestId, TaskName(state.Task), state.Source.Entry,
                 state.Source.Position.GetCounter(), candidate.Distance, candidate.QuestLevel,
                 SelfBotQuestPlanning::ReasonName(decision.Why), state.PickupActive, state.WorkActive,
                 planning.ActiveCount, SelfBotQuestPlanning::MaxActiveQuests, source.AreaId, bot->GetAreaId(),
                 source.AreaTriggerId, source.LocationId, bot->GetExactDist(route.Step), state.TaskSearchRadius,
                 SelfBotQuestPlanning::FutureSourceRadius(state.TaskSearchRadius), route.Distance, route.RiskCost,
                 state.TaskUtility, candidate.NativeQuestXp, candidate.ContinuationDistance);
        if (state.OrderedQuestId)
        {
            state.RouteFailure.clear();
            queue->ReportRunning();
        }
        return true;
    }

    if (planning.HasCurrent)
    {
        if (SelfBotQuestPlanning::Choose(candidates, planning).KeepCurrent)
            return true;
        ClearTask(bot, state);
        planning.HasCurrent = false;
    }
    SelfBotQuestPlanning::Decision const remaining = SelfBotQuestPlanning::Choose(candidates, planning);
    if (!pathBudgetExhausted && !navigationPending && remaining.Index == SelfBotQuestPlanning::NoCandidate)
    {
        float const maximum = SelfBotQuestCatalog::Instance().MaxSearchRadius(state.OpportunityAnchor);
        float const expanded = SelfBotQuestPlanning::NextOpportunityRadius(state.OpportunityRadius, maximum);
        if (expanded > state.OpportunityRadius)
        {
            state.OpportunityRadius = expanded;
            state.RefreshAt = 0;
            state.RetryAt = now + RefreshDelay;
            LOG_INFO("playerbots",
                     "SelfBot {} quest opportunity search expanded to {} at {:.1f},{:.1f}; local tasks exhausted",
                     bot->GetName(), expanded, state.OpportunityAnchor.GetPositionX(),
                     state.OpportunityAnchor.GetPositionY());
            return true;
        }
    }
    if (navigationPending)
    {
        state.RefreshAt = now + RefreshDelay;
        state.RetryAt = now + RefreshDelay;
        LOG_DEBUG("playerbots", "SelfBot {} preparing bounded route terrain; retry in {}s", bot->GetName(),
                  RefreshDelay);
        return true;
    }
    state.RetryAt = now + EmptyQueueDelay;
    if (state.OrderedQuestId)
    {
        auto const deferred = state.PickupDeferrals.find(state.OrderedQuestId);
        queue->ReportBlocked(deferred != state.PickupDeferrals.end()
                                 ? deferred->second
                                 : !state.RouteFailure.empty()
                                     ? state.RouteFailure
                                     : "No safe reachable source for the current quest; waiting to retry.");
    }
    LOG_INFO("playerbots",
             "SelfBot {} quest queue waiting: no safe reachable task; active={}/{} candidates={} "
             "unsafe={} deferred={} pickup={} work={} retry={}s reason={}",
             bot->GetName(), planning.ActiveCount, SelfBotQuestPlanning::MaxActiveQuests, candidates.size(),
             unsafeSources, state.FailedSources.size(), state.PickupActive, state.WorkActive, EmptyQueueDelay,
             SelfBotQuestPlanning::ReasonName(waitingReason));
    return true;
}

bool MoveToSelfBotQuestAction::isUseful()
{
    SelfBotQuestState const& state = QuestState(context);
    return CanWork(bot, context) && state.Phase == SelfBotQuestPhase::Traveling && time(nullptr) >= state.RetryAt;
}

bool MoveToSelfBotQuestAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;
    SelfBotQuestState& state = QuestState(context);
    Resume(bot, state);
    if (ObserveItemUse(bot, state) || ReseedObjectiveLoot(bot, context, state))
        return true;
    if (state.Task == SelfBotQuestTask::UseItem && state.UseEffectObserved)
    {
        state.Phase = SelfBotQuestPhase::Interacting;
        return true;
    }
    if (!TaskNeeded(bot, state))
    {
        ClearTask(bot, state);
        return true;
    }

    WorldObject* object = ResolveTaskSource(bot, state);
    if (state.Task == SelfBotQuestTask::UseItem)
    {
        if (!bot->GetItemByEntry(state.UseItemId) || (state.UseTarget && !object))
        {
            DeferItemUse(bot, state, "quest item or selected target disappeared");
            return true;
        }
        if (object && !state.UseTarget)
            state.UseTarget = object->GetGUID();
    }
    if (Creature* creature = object ? object->ToCreature() : nullptr)
        if (!creature->IsAlive() && !(state.Task == SelfBotQuestTask::UseItem && state.UseRequiresDead))
        {
            if (state.Task == SelfBotQuestTask::UseItem)
                DeferItemUse(bot, state, "item requires a living target; selected creature died");
            else
                FailSource(bot, state, "creature dead; selecting another spawn", DeadSourceDelay);
            return true;
        }
    if (!SourceSafeForWork(bot, state.Source, state.Task, object, CurrentItemUse(state)))
    {
        if (state.Task == SelfBotQuestTask::UseItem)
            DeferItemUse(bot, state, "item target phase, reaction, or difficulty changed");
        else
            FailSource(bot, state, "source phase, reaction, or difficulty changed");
        return true;
    }
    state.TravelledDistance += bot->GetExactDist(state.LastTravelPosition);
    state.LastTravelPosition = WorldPosition(bot);
    if (state.TravelledDistance > state.TaskTravelLimit)
    {
        FailSource(bot, state, "quest travel budget exhausted");
        return true;
    }

    bool const atItemLocation = state.Task == SelfBotQuestTask::UseItem && state.Source.LocationId &&
                                bot->GetExactDist(state.Source.Position) <= ArrivalDistance;
    if (atItemLocation || ReachedExploration(bot, state) || Arrived(bot, object, state.Task, state.UseSpellId))
    {
        if (GameObject* gameObject = object ? object->ToGameObject() : nullptr)
            LOG_INFO("playerbots",
                     "SelfBot {} quest {} reached object {} spawn {} distance {:.2f} originLOS={} nativeRange={}",
                     bot->GetName(), state.QuestId, gameObject->GetEntry(), state.Source.Position.GetCounter(),
                     bot->GetDistance(gameObject), bot->IsWithinLOSInMap(gameObject),
                     gameObject->IsWithinDistInMap(bot));
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
        state.Phase = SelfBotQuestPhase::Interacting;
        return true;
    }

    time_t const now = time(nullptr);
    if (bot->GetExactDist(state.LastProgress) >= ProgressDistance)
    {
        state.LastProgress = WorldPosition(bot);
        state.ProgressAt = now;
    }
    if (now >= state.Deadline || now - state.ProgressAt >= StuckTimeout)
    {
        FailSource(bot, state, "journey or no-progress timeout");
        return true;
    }

    if (state.Task != SelfBotQuestTask::Explore && !state.Source.LocationId && !object &&
        bot->GetExactDist(state.Source.Position) <= INTERACTION_DISTANCE)
    {
        FailSource(bot, state, "spawn missing, despawned, or wrong phase");
        return true;
    }
    if (state.HasTravelStep && now >= state.RiskCheckAt)
    {
        state.RiskCheckAt = now + RefreshDelay;
        PathGenerator remaining(bot);
        float risk = 0.0f;
        if (!remaining.CalculatePath(state.TravelStep.GetPositionX(), state.TravelStep.GetPositionY(),
                                     state.TravelStep.GetPositionZ()) ||
            remaining.GetPathType() != PATHFIND_NORMAL || !RouteRisk(bot, remaining.GetPath(), risk))
        {
            FailSource(bot, state, "upcoming route danger changed or could not be checked");
            return true;
        }
    }
    if (bot->isMoving())
        return true;

    if (IsWaitingForLastMove(MovementPriority::MOVEMENT_NORMAL))
        return true;
    WorldPosition const destination = object ? WorldPosition(object) : WorldPosition(state.Source.Position);
    if (!state.HasTravelStep || bot->GetExactDist(state.TravelStep) <= ArrivalDistance)
    {
        QuestRoute route;
        size_t gridBudget = MaxNewRouteGrids;
        if (!RegionStep(bot, destination, state.TaskTravelLimit - state.TravelledDistance, gridBudget, route))
        {
            if (route.PendingTerrain)
            {
                state.HasTravelStep = false;
                state.RetryAt = now + RefreshDelay;
                state.ProgressAt = now;
                state.Deadline += RefreshDelay;
                return true;
            }
            FailSource(bot, state, route.Failure);
            return true;
        }
        state.TravelStep = route.Step;
        state.HasTravelStep = true;
        state.TaskRouteDistance = state.TravelledDistance + route.Distance;
        state.TaskRiskCost = route.RiskCost;
        state.RiskCheckAt = now + RefreshDelay;
    }
    WorldPosition const step = state.TravelStep;
    if (IsDuplicateMove(step.GetPositionX(), step.GetPositionY(), step.GetPositionZ()))
        return true;
    PathGenerator path(bot);
    float risk = 0.0f;
    if (!path.CalculatePath(step.GetPositionX(), step.GetPositionY(), step.GetPositionZ()) ||
        path.GetPathType() != PATHFIND_NORMAL || !PathInsideRegion(bot->GetMapId(), path.GetPath()) ||
        path.getPathLength() + state.TravelledDistance > state.TaskTravelLimit || !RouteRisk(bot, path.GetPath(), risk))
    {
        FailSource(bot, state, "path unavailable or leaves region");
        return true;
    }
    if (MoveTo(step.GetMapId(), step.GetPositionX(), step.GetPositionY(), step.GetPositionZ(), false, false, true,
               true))
        return true;
    if (++state.Attempts >= MaxAttempts)
        FailSource(bot, state, "movement rejected repeatedly");
    return false;
}

bool InteractSelfBotQuestAction::isUseful()
{
    SelfBotQuestState const& state = QuestState(context);
    return CanWork(bot, context) && state.Phase == SelfBotQuestPhase::Interacting && time(nullptr) >= state.RetryAt &&
           (!SelfBotQuestPlanning::IsWorkTask(state.Task) || time(nullptr) < state.RefreshAt);
}

bool InteractSelfBotQuestAction::ExecuteItemUse()
{
    SelfBotQuestState& state = QuestState(context);
    if (!ItemCombatConnected(bot) || state.Task != SelfBotQuestTask::UseItem || state.Phase == SelfBotQuestPhase::Idle)
        return false;
    if (state.UseCombatControl && !state.UseEffectObserved)
        SelfBotQuestItemCasting::PauseDamage(bot, state.UseSpellId, state.UseDamagePause);
    if (ObserveItemUse(bot, state))
        return true;
    SelfBotQuestItemUse const* use = CurrentItemUse(state);
    if (!use || bot->GetHealthPct() < 50.0f || !botAI->HasStrategy("selfbot quest", BOT_STATE_NON_COMBAT))
    {
        DeferItemUse(bot, state, "item mechanic interrupted for recovery or disabled questing");
        return true;
    }
    time_t const now = time(nullptr);
    if (now < state.RetryAt)
        return false;
    WorldObject* object = ResolveTaskSource(bot, state);
    Creature* creature = object ? object->ToCreature() : nullptr;
    if (!state.UseTarget && object)
        state.UseTarget = object->GetGUID();
    if (bot->IsInCombat())
    {
        ObjectGuid const expected = state.UseEffectObserved ? state.UseResultTarget : state.UseTarget;
        auto const& combat = bot->GetCombatManager().GetPvECombatRefs();
        if (!bot->GetCombatManager().GetPvPCombatRefs().empty() || combat.size() > 1 ||
            (!combat.empty() && !combat.count(expected)))
        {
            DeferItemUse(bot, state, "unrelated combat interrupted the item mechanic");
            return true;
        }
    }
    auto approach = [&](WorldObject* target, float distance)
    {
        if (!target || bot->GetDistance(target) > ItemResultRadius)
            return false;
        PathGenerator path(bot);
        float risk = 0.0f;
        if (!path.CalculatePath(target->GetPositionX(), target->GetPositionY(), target->GetPositionZ()) ||
            path.GetPathType() != PATHFIND_NORMAL || !PathInsideRegion(bot->GetMapId(), path.GetPath()) ||
            !RouteRisk(bot, path.GetPath(), risk))
            return false;
        return bot->isMoving() || MoveNear(target, distance);
    };
    if (!state.UseEffectObserved && state.Source.LocationId &&
        bot->GetExactDist(state.Source.Position) > ArrivalDistance)
    {
        if (bot->IsInCombat())
        {
            DeferItemUse(bot, state, "combat interrupted travel to the item location");
            return true;
        }
        state.Phase = SelfBotQuestPhase::Traveling;
        return false;
    }
    if (state.UseEffectObserved)
    {
        SelfBotQuestPlanning::ItemUseContinuationObservation continuation;
        continuation.Needed = TaskNeeded(bot, state);
        continuation.Progressed = CurrentObjectiveProgress(bot, state) > state.TaskProgress;
        continuation.MechanicObserved = true;
        continuation.Kind =
            use->Continuation == SelfBotQuestItemContinuation::Loot   ? SelfBotQuestPlanning::ItemUseContinuation::Loot
            : use->Continuation == SelfBotQuestItemContinuation::Kill ? SelfBotQuestPlanning::ItemUseContinuation::Kill
                                                                      : SelfBotQuestPlanning::ItemUseContinuation::None;
        continuation.Attributed = creature && creature->GetGUID() == state.UseResultTarget &&
                                  (!use->ResultEntry || creature->GetEntry() == use->ResultEntry);
        continuation.TargetPresent = creature && creature->IsInWorld();
        continuation.TargetInPhase = creature && creature->InSamePhase(bot);
        continuation.TargetAlive = creature && creature->IsAlive();
        continuation.AttackAllowed = ResultSafeToAttack(bot, creature);
        auto const next = SelfBotQuestPlanning::NextItemUseContinuationStep(continuation);
        if (next == SelfBotQuestPlanning::ItemUseContinuationStep::Completed ||
            next == SelfBotQuestPlanning::ItemUseContinuationStep::Defer)
        {
            DeferItemUse(bot, state, "item continuation completed or no longer suitable");
            return true;
        }
        if (next == SelfBotQuestPlanning::ItemUseContinuationStep::Kill)
        {
            if (use->Kind == SelfBotQuestItemKind::Weaken && use->ResultAura &&
                (use->ResultAuraRemoved ? creature->HasAura(use->ResultAura) : !creature->HasAura(use->ResultAura)))
            {
                DeferItemUse(bot, state, "required weakening effect is no longer present");
                return true;
            }
            if (!bot->IsWithinLOSInMap(creature))
            {
                if (!approach(creature, ArrivalDistance))
                    DeferItemUse(bot, state, "result target has no safe local approach");
                return true;
            }
            if (bot->GetVictim() == creature && bot->HasUnitState(UNIT_STATE_MELEE_ATTACKING))
                return false;
            return Attack(creature);
        }
        if (next == SelfBotQuestPlanning::ItemUseContinuationStep::Loot)
        {
            if (bot->IsInCombat())
                return false;
            if (!bot->isAllowedToLoot(creature) || !creature->HasFlag(UNIT_DYNAMIC_FLAGS, UNIT_DYNFLAG_LOOTABLE))
            {
                DeferItemUse(bot, state, "result creature has no accessible loot");
                return true;
            }
            LootObjectStack* available = context->GetValue<LootObjectStack*>("available loot")->Get();
            if (!state.LootReseeded)
            {
                state.LootReseeded = creature->GetGUID();
                available->Remove(state.LootReseeded);
                available->Add(state.LootReseeded);
                context->GetValue<LootObject>("loot target")->Set(LootObject(bot, state.LootReseeded));
            }
        }
        state.RetryAt = now + 1;
        return false;
    }
    Item* item = bot->GetItemByEntry(state.UseItemId);
    bool const selfTarget = use->TargetMode == SelfBotQuestItemTargetMode::Self;
    WorldObject* castTarget = selfTarget ? bot : object;
    SelfBotQuestPlanning::ItemUseObservation observation;
    observation.Needed = TaskNeeded(bot, state);
    observation.Progressed = CurrentObjectiveProgress(bot, state) > state.TaskProgress;
    observation.Pending = now < state.UseWaitUntil;
    observation.HasItem = item != nullptr;
    observation.TargetPresent = castTarget && castTarget->IsInWorld();
    observation.TargetInPhase = castTarget && castTarget->InSamePhase(bot);
    observation.RequiresDead = use->RequiresDead;
    observation.TargetAlive = selfTarget || (object && (!creature || creature->IsAlive()));
    observation.AttackAllowed = !selfTarget && ResultSafeToAttack(bot, creature) &&
                                SourceSafeForWork(bot, state.Source, state.Task, object, use);
    observation.Attempts = state.Attempts;
    observation.HealthRange = ItemHealthRange(*use);
    observation.TargetHealthPercent = creature && !selfTarget ? creature->GetHealthPct() : 100.0f;
    auto const step = SelfBotQuestPlanning::NextItemUseStep(observation);
    if (step == SelfBotQuestPlanning::ItemUseStep::Completed)
    {
        DeferItemUse(bot, state, "native objective progress verified");
        return true;
    }
    if (step == SelfBotQuestPlanning::ItemUseStep::Wait)
        return true;
    if (step == SelfBotQuestPlanning::ItemUseStep::Defer || now >= state.Deadline)
    {
        DeferItemUse(bot, state, "item-use target, health, item, or retry budget is no longer suitable");
        return true;
    }
    if (!selfTarget)
        state.UseTarget = object->GetGUID();
    if (step == SelfBotQuestPlanning::ItemUseStep::PrepareKill)
    {
        if (Attack(creature))
        {
            state.Phase = SelfBotQuestPhase::Traveling;
            return true;
        }
        DeferItemUse(bot, state, "cannot prepare the required corpse");
        return true;
    }
    if (!state.UseStartedAt)
        state.UseStartedAt = now;
    if (use->Kind == SelfBotQuestItemKind::Summon)
    {
        state.UseCombatControl = true;
        SelfBotQuestItemCasting::PauseDamage(bot, state.UseSpellId, state.UseDamagePause);
    }
    if (creature && creature->IsAlive() && !selfTarget)
    {
        state.UseCombatControl = true;
        SelfBotQuestItemCasting::PauseDamage(bot, state.UseSpellId, state.UseDamagePause);
        if (state.UseObservedHealth != creature->GetHealth() || !state.UseHealthStableAt)
        {
            state.UseObservedHealth = creature->GetHealth();
            state.UseHealthStableAt = now;
        }
        if (now - state.UseHealthStableAt < ItemHealthStabilityDelay)
        {
            state.RetryAt = now + 1;
            return true;
        }
    }
    if (step == SelfBotQuestPlanning::ItemUseStep::PrepareWeaken)
    {
        auto const safety = SelfBotQuestItemCasting::WeakeningCheck(bot, creature);
        float const minimumHealth = creature->GetMaxHealth() * use->MinHealthPct / 100.0f;
        if (!safety.Allowed || float(creature->GetHealth()) - float(safety.MaxMeleeDamage) <= minimumHealth)
        {
            DeferItemUse(bot, state, safety.Allowed ? "a weakening hit could cross the minimum health" : safety.Reason);
            return true;
        }
        if (!bot->IsWithinMeleeRange(creature) || !bot->IsWithinLOSInMap(creature))
        {
            if (!approach(creature, 0.0f))
                DeferItemUse(bot, state, "cannot reach the weakening target");
            return true;
        }
        bot->SetFacingToObject(creature);
        auto const result = SelfBotQuestItemCasting::WeakenOnce(bot, creature);
        if (result.State == SelfBotQuestItemCasting::Status::Rejected)
            DeferItemUse(bot, state, result.Reason);
        else
            state.RetryAt = now + 1;
        return true;
    }
    if (!selfTarget && !Arrived(bot, object, SelfBotQuestTask::UseItem, state.UseSpellId))
    {
        if (bot->IsInCombat())
        {
            if (!approach(object, 1.0f))
                DeferItemUse(bot, state, "cannot reach the item target during combat");
        }
        else
            state.Phase = SelfBotQuestPhase::Traveling;
        return true;
    }
    if (use->Kind == SelfBotQuestItemKind::Summon &&
        (use->SummonerIsTarget || !CollectItemSummons(bot, *use, state.UseExistingSummons)))
    {
        DeferItemUse(bot, state, "cannot attribute the expected summoned creature");
        return true;
    }
    state.UseHadResultAura = creature && use->ResultAura && creature->HasAura(use->ResultAura);
    auto const mode = selfTarget ? SelfBotQuestItemCasting::TargetMode::Self
                      : use->TargetMode == SelfBotQuestItemTargetMode::NearbyUnit
                          ? SelfBotQuestItemCasting::TargetMode::NearbyUnit
                          : SelfBotQuestItemCasting::TargetMode::Explicit;
    SelfBotQuestItemCasting::Result const result =
        SelfBotQuestItemCasting::Execute(bot, item->GetGUID(), state.UseSpellId, castTarget, mode);
    if (result.State == SelfBotQuestItemCasting::Status::Sent)
    {
        ++state.Attempts;
        state.UseWaitUntil = now + std::max(time_t(3), time_t(result.RetryDelayMs / 1000) + 2);
        state.RetryAt = state.UseWaitUntil;
        LOG_INFO("playerbots", "SelfBot {} quest {} using item {} spell {} on {}; waiting for native result",
                 bot->GetName(), state.QuestId, state.UseItemId, state.UseSpellId, state.UseTarget.ToString());
    }
    else if (result.State == SelfBotQuestItemCasting::Status::Wait)
        state.RetryAt = now + std::max(time_t(1), time_t(result.RetryDelayMs / 1000));
    else
        DeferItemUse(bot, state, result.Reason);
    return true;
}

bool InteractSelfBotQuestAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;
    SelfBotQuestState& state = QuestState(context);
    Resume(bot, state);
    if (ObserveItemUse(bot, state))
        return true;
    if (!TaskNeeded(bot, state))
    {
        ClearTask(bot, state);
        return true;
    }

    if (state.Task == SelfBotQuestTask::Explore)
    {
        if (!ReachedExploration(bot, state))
        {
            state.Phase = SelfBotQuestPhase::Traveling;
            return true;
        }
        if (++state.Attempts > MaxAttempts)
        {
            FailSource(bot, state, "area trigger produced no journal progress");
            return true;
        }
        WorldPacket packet(CMSG_AREATRIGGER);
        packet << state.Source.AreaTriggerId;
        if (!Connected(bot))
            return false;
        bot->GetSession()->HandleAreaTriggerOpcode(packet);
        bool const explored = CurrentObjectiveProgress(bot, state) != 0;
        LOG_INFO("playerbots", "SelfBot {} quest {} visited area trigger {} via session; explored={}", bot->GetName(),
                 state.QuestId, state.Source.AreaTriggerId, explored);
        state.RefreshAt = 0;
        if (explored)
            ClearTask(bot, state);
        return explored;
    }

    if (state.Task == SelfBotQuestTask::UseItem)
        return ExecuteItemUse();

    WorldObject* object = ResolveTaskSource(bot, state);
    if (!Arrived(bot, object, state.Task, state.UseSpellId))
    {
        state.Phase = SelfBotQuestPhase::Traveling;
        return true;
    }
    Quest const* quest = sObjectMgr->GetQuestTemplate(state.QuestId);
    if (!quest || !object)
        return false;
    if (++state.Attempts > MaxAttempts)
    {
        FailSource(bot, state, "interaction produced no journal progress");
        return true;
    }

    ObjectGuid const guid = object->GetGUID();
    bot->SetFacingToObject(object);
    bot->SetTarget(guid);
    if (state.Task == SelfBotQuestTask::Accept)
    {
        uint32 accepted = 0;
        unsigned const heroLevel = bot->GetLevel();
        unsigned const pickupLevel = SelfBotQuestPlanning::EffectiveQuestLevel(quest->GetQuestLevel(), heroLevel);
        std::vector<SelfBotQuestRow const*> pickupRows;
        for (SelfBotQuestRow const& row : state.Rows)
            if ((!state.OrderedQuestId || row.Quest->Id == state.OrderedQuestId) && row.Quest->Supported &&
                SelfBotQuestPlanning::EffectiveQuestLevel(row.Quest->QuestLevel, heroLevel) <= pickupLevel)
                pickupRows.push_back(&row);
        std::sort(pickupRows.begin(), pickupRows.end(),
                  [heroLevel](auto const* left, auto const* right)
                  {
                      unsigned const leftLevel =
                          SelfBotQuestPlanning::EffectiveQuestLevel(left->Quest->QuestLevel, heroLevel);
                      unsigned const rightLevel =
                          SelfBotQuestPlanning::EffectiveQuestLevel(right->Quest->QuestLevel, heroLevel);
                      return leftLevel != rightLevel ? leftLevel < rightLevel : left->Quest->Id < right->Quest->Id;
                  });
        for (SelfBotQuestRow const* row : pickupRows)
        {
            Quest const* next = sObjectMgr->GetQuestTemplate(row->Quest->Id);
            if (!Connected(bot) || ActiveQuestCount(bot) >= SelfBotQuestPlanning::MaxActiveQuests)
                break;
            if (!QuestSafeForWork(bot, next) || !object->hasQuest(next->GetQuestId()) ||
                bot->GetQuestRewardStatus(next->GetQuestId()) || !bot->CanTakeQuest(next, false) ||
                !bot->CanAddQuest(next, false) || !bot->CanInteractWithQuestGiver(object))
                continue;
            if (char const* reason =
                    PickupBlockedReason(bot, *row->Quest, state, WorldPosition(object), state.TaskSearchRadius))
            {
                LOG_INFO("playerbots", "SelfBot {} quest {} pickup deferred before acceptance: {}", bot->GetName(),
                         next->GetQuestId(), reason);
                continue;
            }
            WorldPacket packet(CMSG_QUESTGIVER_ACCEPT_QUEST);
            packet << guid << next->GetQuestId() << uint32(0);
            bot->GetSession()->HandleQuestgiverAcceptQuestOpcode(packet);
            QuestStatus const status = bot->GetQuestStatus(next->GetQuestId());
            if (status == QUEST_STATUS_INCOMPLETE || status == QUEST_STATUS_COMPLETE)
            {
                ++accepted;
                LOG_INFO("playerbots", "SelfBot {} accepted quest {} [{}] at {} distance {:.2f} via session",
                         bot->GetName(), next->GetQuestId(), next->GetTitle(), guid.ToString(),
                         bot->GetDistance(object));
            }
        }
        if (accepted)
        {
            state.RefreshAt = 0;
            ClearTask(bot, state);
            return true;
        }
        return false;
    }

    if (state.Task == SelfBotQuestTask::TurnIn)
    {
        // Native rewards can auto-accept the next quest without another bot action.
        if (Quest const* next = bot->GetNextQuest(guid, quest);
            next && next->IsAutoAccept() && !QuestAutomationAllowed(next))
        {
            FailSource(bot, state, "reward would automatically accept an excluded event quest");
            return true;
        }
        WorldPacket request(CMSG_QUESTGIVER_REQUEST_REWARD);
        request << guid << state.QuestId;
        bot->GetSession()->HandleQuestgiverRequestRewardOpcode(request);
        uint32 const reward = ChooseReward(bot, context, quest);
        uint32 const moneyBefore = bot->GetMoney();
        uint32 const levelBefore = bot->GetLevel();
        uint32 const xpBefore = bot->GetUInt32Value(PLAYER_XP);
        if (!Connected(bot) || !bot->CanRewardQuest(quest, reward, false))
            return false;
        WorldPacket packet(CMSG_QUESTGIVER_CHOOSE_REWARD);
        packet << guid << state.QuestId << reward;
        bot->GetSession()->HandleQuestgiverChooseRewardOpcode(packet);
        if (bot->GetQuestRewardStatus(state.QuestId))
        {
            LOG_INFO("playerbots",
                     "SelfBot {} rewarded quest {} [{}] at {} distance {:.2f} choice={} item={} "
                     "money={}->{} level={}->{} xp={}->{} via session",
                     bot->GetName(), state.QuestId, quest->GetTitle(), guid.ToString(), bot->GetDistance(object),
                     reward, quest->RewardChoiceItemId[reward], moneyBefore, bot->GetMoney(), levelBefore,
                     bot->GetLevel(), xpBefore, bot->GetUInt32Value(PLAYER_XP));
            context->GetValue<uint8>("bag space")->Reset();
            state.PickupAllowed = true;
            state.RefreshAt = 0;
            ResetOpportunity(WorldPosition(bot), state);
            ClearTask(bot, state);
            return true;
        }
        return false;
    }

    if (Creature* creature = object->ToCreature())
    {
        if (!QuestSafeForWork(bot, quest) || !SourceSafeForWork(bot, state.Source, state.Task, object))
        {
            FailSource(bot, state, "quest or attack source exceeds hero+3, is elite, or is no longer a valid target");
            return true;
        }
        if (state.Task == SelfBotQuestTask::Objective && creature->IsFriendlyTo(bot) &&
            bot->GetNPCIfCanInteractWith(guid, UNIT_NPC_FLAG_GOSSIP))
        {
            if (OffersExcludedAutomaticQuest(bot, creature))
            {
                FailSource(bot, state, "gossip could automatically accept an excluded event quest");
                return true;
            }
            WorldPacket packet(CMSG_GOSSIP_HELLO);
            packet << guid;
            bot->GetSession()->HandleGossipHelloOpcode(packet);
            state.RefreshAt = 0;
            LOG_INFO("playerbots", "SelfBot {} quest {} spoke to {} spawn {} via session; verifying counters",
                     bot->GetName(), state.QuestId, creature->GetEntry(), state.Source.Position.GetCounter());
            return true;
        }
        if (!creature->IsAlive() || !bot->IsValidAttackTarget(creature) ||
            !SelfBotQuestCatalog::IsInRegion(WorldPosition(creature)))
        {
            FailSource(bot, state, "objective creature is no longer a valid regional attack target", DeadSourceDelay);
            return true;
        }
        bool const attacked = Attack(creature);
        if (attacked)
        {
            state.Phase = SelfBotQuestPhase::Traveling;
            state.Attempts = 0;
            LOG_INFO("playerbots", "SelfBot {} quest {} {} attacking {} spawn {}", bot->GetName(), state.QuestId,
                     TaskName(state.Task), creature->GetEntry(), state.Source.Position.GetCounter());
        }
        return attacked;
    }

    if (GameObject* gameObject = object->ToGameObject())
    {
        if (gameObject->GetGoType() == GAMEOBJECT_TYPE_QUESTGIVER && OffersExcludedAutomaticQuest(bot, gameObject))
        {
            FailSource(bot, state, "object could automatically accept an excluded event quest");
            return true;
        }
        if (gameObject->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_NOT_SELECTABLE) ||
            (gameObject->HasFlag(GAMEOBJECT_FLAGS, GO_FLAG_INTERACT_COND) && !gameObject->ActivateToQuest(bot)))
        {
            FailSource(bot, state, "gameobject cannot currently be used for this character");
            return true;
        }
        if (gameObject->GetGoType() == GAMEOBJECT_TYPE_CHEST)
        {
            LootObject loot(bot, guid);
            if (loot.IsEmpty() || !loot.IsLootPossible(bot))
            {
                FailSource(bot, state, "quest object lock, skill, key, or loot conditions not satisfied");
                return true;
            }
            context->GetValue<LootObjectStack*>("available loot")->Get()->Add(guid);
            context->GetValue<LootObject>("loot target")->Set(loot);
            OpenLootAction open(botAI);
            bool const opened = open.Execute(Event());
            if (opened)
            {
                LOG_INFO("playerbots", "SelfBot {} quest {} opening object {} spawn {} using ordinary loot action",
                         bot->GetName(), state.QuestId, gameObject->GetEntry(), state.Source.Position.GetCounter());
                state.FailedSources[state.Source.Position.GetRawValue()] = time(nullptr) + DeadSourceDelay;
                ClearTask(bot, state);
            }
            return opened;
        }
        WorldPacket packet(CMSG_GAMEOBJ_USE);
        packet << guid;
        bot->GetSession()->HandleGameObjectUseOpcode(packet);
        state.RefreshAt = 0;
        LOG_INFO("playerbots", "SelfBot {} quest {} used object {} spawn {} via session; verifying counters",
                 bot->GetName(), state.QuestId, gameObject->GetEntry(), state.Source.Position.GetCounter());
        return true;
    }
    return false;
}
