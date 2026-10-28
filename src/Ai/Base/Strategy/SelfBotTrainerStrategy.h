/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTTRAINERSTRATEGY_H
#define PLAYERBOTS_SELFBOTTRAINERSTRATEGY_H

#include <map>
#include <set>

#include "MovementActions.h"
#include "Strategy.h"
#include "TravelMgr.h"
#include "Trigger.h"
#include "Value.h"

enum class SelfBotTrainerPhase
{
    Idle,
    Traveling,
    Learning
};

struct SelfBotTrainerState
{
    SelfBotTrainerPhase Phase = SelfBotTrainerPhase::Idle;
    ObjectGuid::LowType SpawnId = 0;
    uint32 Entry = 0;
    WorldPosition Destination;
    WorldPosition LastProgress;
    time_t RetryAfter = 0;
    time_t Deadline = 0;
    time_t HardDeadline = 0;
    time_t ProgressAt = 0;
    time_t LastActive = 0;
    std::map<ObjectGuid::LowType, time_t> FailedSpawns;
    std::set<uint32> AttemptedSpells;
};

class SelfBotTrainerStateValue : public ManualSetValue<SelfBotTrainerState>
{
public:
    SelfBotTrainerStateValue(PlayerbotAI* botAI)
        : ManualSetValue<SelfBotTrainerState>(botAI, {}, "selfbot trainer state")
    {
    }
};

class SelfBotTrainerTrigger : public Trigger
{
public:
    SelfBotTrainerTrigger(PlayerbotAI* botAI) : Trigger(botAI, "selfbot trainer", 1) {}
    bool IsActive() override;
};

class SelfBotTrainerStrategy : public Strategy
{
public:
    SelfBotTrainerStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "selfbot trainer"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

class VisitSelfBotTrainerAction : public MovementAction
{
public:
    VisitSelfBotTrainerAction(PlayerbotAI* botAI) : MovementAction(botAI, "visit selfbot trainer") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

#endif
