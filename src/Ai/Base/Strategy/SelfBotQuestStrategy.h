/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTQUESTSTRATEGY_H
#define PLAYERBOTS_SELFBOTQUESTSTRATEGY_H

#include <map>
#include <tuple>

#include "AttackAction.h"
#include "SelfBotQuestCatalog.h"
#include "SelfBotQuestItemUse.h"
#include "SelfBotQuestPlanner.h"
#include "Strategy.h"
#include "Trigger.h"
#include "Value.h"

enum class SelfBotQuestPhase
{
    Idle,
    Traveling,
    Interacting
};

using SelfBotQuestTask = SelfBotQuestPlanning::Task;

struct SelfBotQuestState
{
    SelfBotQuestPhase Phase = SelfBotQuestPhase::Idle;
    SelfBotQuestTask Task = SelfBotQuestTask::Accept;
    uint32 QuestId = 0;
    uint32 OrderedQuestId = 0;
    bool PickupActive = false;
    bool PickupAllowed = true;
    bool WorkActive = false;
    WorldPosition PickupAnchor;
    WorldPosition WorkAnchor;
    WorldPosition OpportunityAnchor;
    bool OpportunityAnchored = false;
    float OpportunityRadius = SelfBotQuestPlanning::InitialOpportunityRadius;
    float TaskSearchRadius = SelfBotQuestPlanning::InitialOpportunityRadius;
    float PickupSearchRadius = SelfBotQuestPlanning::InitialOpportunityRadius;
    float TaskUtility = 0.0f;
    float TaskRouteDistance = 0.0f;
    float TaskRiskCost = 0.0f;
    float TaskContinuationDistance = 0.0f;
    float TaskTravelLimit = 0.0f;
    float TravelledDistance = 0.0f;
    WorldPosition LastTravelPosition;
    WorldPosition TravelStep;
    bool HasTravelStep = false;
    uint32 WorkSteps = 0;
    time_t WorkStarted = 0;
    uint32 TaskProgress = 0;
    uint8 Objective = 0;
    SelfBotQuestSource Source;
    ObjectGuid LootReseeded;
    uint32 UseItemId = 0;
    uint32 UseSpellId = 0;
    size_t UsePlanIndex = 0;
    bool UseItemObjective = false;
    bool UseRequiresDead = false;
    ObjectGuid UseTarget;
    ObjectGuid UseResultTarget;
    bool UseEffectObserved = false;
    bool UseHadResultAura = false;
    bool UseCombatControl = false;
    std::vector<ObjectGuid> UseExistingSummons;
    uint32 UseObservedHealth = 0;
    time_t UseHealthStableAt = 0;
    SelfBotQuestItemCasting::DamagePause UseDamagePause;
    time_t UseWaitUntil = 0;
    time_t UseStartedAt = 0;
    std::map<std::tuple<uint32, uint32, uint32, uint64>, time_t> ItemUseDeferrals;
    std::vector<SelfBotQuestRow> Rows;
    std::map<uint64, time_t> FailedSources;
    std::map<uint64, time_t> DeadSources;
    std::map<std::pair<uint32, uint64>, time_t> CostDeferrals;
    std::map<uint64, uint8> SourceFailures;
    std::map<uint64, uint64> BlockedSources;
    std::map<uint32, std::string> PickupDeferrals;
    std::string RouteFailure;
    uint64 Fingerprint = 0;
    time_t RefreshAt = 0;
    time_t RetryAt = 0;
    time_t LastActive = 0;
    time_t Deadline = 0;
    time_t ProgressAt = 0;
    time_t RiskCheckAt = 0;
    WorldPosition LastProgress;
    uint8 Attempts = 0;
    bool WasPaused = false;
};

class SelfBotQuestStateValue : public ManualSetValue<SelfBotQuestState>
{
public:
    SelfBotQuestStateValue(PlayerbotAI* botAI) : ManualSetValue<SelfBotQuestState>(botAI, {}, "selfbot quest state") {}
    ~SelfBotQuestStateValue() override;
    void Reset() override;
    void Update() override;
};

void PreserveSelfBotQuestPetState(PlayerbotAI* botAI);
void UpdateSelfBotQuestState(PlayerbotAI* botAI);

class SelfBotQuestTrigger : public Trigger
{
public:
    SelfBotQuestTrigger(PlayerbotAI* botAI) : Trigger(botAI, "selfbot quest", 1) {}
    bool IsActive() override;
};

class SelfBotQuestStrategy : public Strategy
{
public:
    SelfBotQuestStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "selfbot quest"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

class SelfBotQuestItemCombatTrigger : public Trigger
{
public:
    SelfBotQuestItemCombatTrigger(PlayerbotAI* botAI) : Trigger(botAI, "selfbot quest item combat", 1) {}
    bool IsActive() override;
};

class SelfBotQuestItemCombatStrategy : public Strategy
{
public:
    SelfBotQuestItemCombatStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "selfbot quest item combat"; }
    uint32 GetType() const override { return STRATEGY_TYPE_COMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

class ChooseSelfBotQuestAction : public Action
{
public:
    ChooseSelfBotQuestAction(PlayerbotAI* botAI) : Action(botAI, "choose selfbot quest") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class MoveToSelfBotQuestAction : public MovementAction
{
public:
    MoveToSelfBotQuestAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to selfbot quest") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class InteractSelfBotQuestAction : public AttackAction
{
public:
    InteractSelfBotQuestAction(PlayerbotAI* botAI) : AttackAction(botAI, "interact selfbot quest") {}
    bool isUseful() override;
    bool Execute(Event event) override;
    bool ExecuteItemUse();
};

class SelfBotQuestItemCombatAction : public InteractSelfBotQuestAction
{
public:
    SelfBotQuestItemCombatAction(PlayerbotAI* botAI) : InteractSelfBotQuestAction(botAI) {}
    std::string const getName() override { return "selfbot quest item combat"; }
    bool isUseful() override;
    bool Execute(Event event) override;
};

#endif
