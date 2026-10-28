/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELLGRAYSTRATEGY_H
#define PLAYERBOTS_SELLGRAYSTRATEGY_H

#include "MovementActions.h"
#include "Strategy.h"
#include "TravelMgr.h"
#include "Trigger.h"
#include "Value.h"

enum class GrayVendorPhase
{
    Idle,
    Traveling,
    Selling,
    Returning
};

struct GrayVendorState
{
    GrayVendorPhase Phase = GrayVendorPhase::Idle;
    ObjectGuid::LowType SpawnId = 0;
    ObjectGuid::LowType FailedSpawnId = 0;
    WorldPosition Destination;
    WorldPosition Origin;
    WorldPosition LastProgress;
    time_t RetryAfter = 0;
    time_t FailedUntil = 0;
    time_t Deadline = 0;
    time_t ProgressAt = 0;
};

class GrayVendorStateValue : public ManualSetValue<GrayVendorState>
{
public:
    GrayVendorStateValue(PlayerbotAI* botAI) : ManualSetValue<GrayVendorState>(botAI, {}, "gray vendor state") {}
};

class GrayVendorNeededValue : public BoolCalculatedValue
{
public:
    GrayVendorNeededValue(PlayerbotAI* botAI) : BoolCalculatedValue(botAI, "gray vendor needed", 5) {}

protected:
    bool Calculate() override;
};

class GrayVendorTrigger : public Trigger
{
public:
    GrayVendorTrigger(PlayerbotAI* botAI) : Trigger(botAI, "gray vendor", 1) {}
    bool IsActive() override;
};

class SellGrayStrategy : public Strategy
{
public:
    SellGrayStrategy(PlayerbotAI* botAI) : Strategy(botAI) {}
    std::string const getName() override { return "sell gray"; }
    uint32 GetType() const override { return STRATEGY_TYPE_NONCOMBAT; }
    void InitTriggers(std::vector<TriggerNode*>& triggers) override;
    void InitMultipliers(std::vector<Multiplier*>& multipliers) override;
};

class ChooseGrayVendorAction : public Action
{
public:
    ChooseGrayVendorAction(PlayerbotAI* botAI) : Action(botAI, "choose gray vendor") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class MoveToGrayVendorAction : public MovementAction
{
public:
    MoveToGrayVendorAction(PlayerbotAI* botAI) : MovementAction(botAI, "move to gray vendor") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class SellGrayLootAction : public Action
{
public:
    SellGrayLootAction(PlayerbotAI* botAI) : Action(botAI, "sell gray loot") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

class ReturnFromGrayVendorAction : public MovementAction
{
public:
    ReturnFromGrayVendorAction(PlayerbotAI* botAI) : MovementAction(botAI, "return from gray vendor") {}
    bool isUseful() override;
    bool Execute(Event event) override;
};

#endif
