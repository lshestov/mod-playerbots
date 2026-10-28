/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SellGrayStrategy.h"

#include <unordered_set>

#include "Bag.h"
#include "ChooseTargetActions.h"
#include "Creature.h"
#include "ItemPackets.h"
#include "LootAction.h"
#include "Map.h"
#include "MotionMaster.h"
#include "Multiplier.h"
#include "ObjectMgr.h"
#include "PathGenerator.h"
#include "Playerbots.h"
#include "QuestDef.h"
#include "WorldSession.h"

namespace
{
constexpr float VendorSearchDistance = 600.0f;
constexpr float ReturnDistance = 5.0f;
constexpr float ProgressDistance = 3.0f;
constexpr time_t RetryDelay = 60;
constexpr time_t FailedVendorDelay = 300;
constexpr time_t JourneyTimeout = 180;
constexpr time_t StuckTimeout = 30;
constexpr size_t MaxPathAttempts = 3;

bool CanVisitVendor(Player* bot)
{
    WorldSession* session = bot->GetSession();
    return IsSelfBot(bot) && session && !session->IsSocketClosed() && !session->IsLoggingOut() && bot->IsAlive() &&
           !bot->IsInCombat() && !bot->IsInFlight() && !bot->GetVehicle() && !bot->IsBeingTeleported();
}

std::optional<SelfBotSettings> VendorSettings(Player* bot)
{
    PlayerbotMgr* manager = GET_PLAYERBOT_MGR(bot);
    return manager ? manager->GetSelfBotController().GetCachedSettings() : std::nullopt;
}

uint32 FreeGeneralBagSlots(Player* bot)
{
    uint32 freeSlots = 0;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (!bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++freeSlots;
    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (Bag* bag = bot->GetBagByPos(slot); bag && bag->GetTemplate()->Class == ITEM_CLASS_CONTAINER &&
                                               bag->GetTemplate()->SubClass == ITEM_SUBCLASS_CONTAINER)
            freeSlots += bag->GetFreeSlots();
    return freeSlots;
}

std::unordered_set<uint32> ProtectedQuestItems(Player* bot)
{
    std::unordered_set<uint32> items;
    for (uint8 slot = 0; slot < MAX_QUEST_LOG_SIZE; ++slot)
        if (Quest const* quest = sObjectMgr->GetQuestTemplate(bot->GetQuestSlotQuestId(slot)))
        {
            items.insert(quest->GetSrcItemId());
            for (uint32 entry : quest->RequiredItemId)
                items.insert(entry);
            for (uint32 entry : quest->ItemDrop)
                items.insert(entry);
        }
    return items;
}

bool ShouldSellItem(Item const* item, SelfBotSettings const& settings, std::unordered_set<uint32> const& questItems)
{
    ItemTemplate const* itemTemplate = item ? item->GetTemplate() : nullptr;
    return itemTemplate && settings.SellsQuality(itemTemplate->Quality) && itemTemplate->SellPrice > 0 &&
           !item->IsEquipped() && !item->IsInTrade() && !item->IsRefundable() && !item->IsBOPTradable() &&
           !item->IsWrapped() && !item->IsNotEmptyBag() && itemTemplate->Class != ITEM_CLASS_QUEST &&
           itemTemplate->Bonding != BIND_QUEST_ITEM && !itemTemplate->StartQuest && !questItems.count(item->GetEntry());
}

std::vector<ObjectGuid> SaleItemsInBags(Player* bot, SelfBotSettings const& settings)
{
    std::vector<ObjectGuid> items;
    auto const questItems = ProtectedQuestItems(bot);
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        if (Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot); ShouldSellItem(item, settings, questItems))
            items.push_back(item->GetGUID());

    for (uint8 slot = INVENTORY_SLOT_BAG_START; slot < INVENTORY_SLOT_BAG_END; ++slot)
        if (Bag* bag = bot->GetBagByPos(slot))
            for (uint32 itemSlot = 0; itemSlot < bag->GetBagSize(); ++itemSlot)
                if (Item* item = bag->GetItemByPos(itemSlot); ShouldSellItem(item, settings, questItems))
                    items.push_back(item->GetGUID());

    return items;
}

GrayVendorState& VendorState(AiObjectContext* context)
{
    return context->GetValue<GrayVendorState>("gray vendor state")->RefGet();
}

Creature* ResolveVendor(Player* bot, ObjectGuid::LowType spawnId)
{
    auto const bounds = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnId);
    for (auto itr = bounds.first; itr != bounds.second; ++itr)
    {
        Creature* vendor = itr->second;
        if (vendor && vendor->IsInWorld() && vendor->IsAlive() && vendor->InSamePhase(bot) &&
            vendor->HasNpcFlag(UNIT_NPC_FLAG_VENDOR) && !vendor->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR) &&
            !vendor->IsTrigger() && !vendor->HasUnitFlag(UNIT_FLAG_NOT_SELECTABLE) && vendor->IsFriendlyTo(bot))
            return vendor;
    }
    return nullptr;
}

void BeginReturn(Player* bot, GrayVendorState& state, bool failed)
{
    time_t const now = time(nullptr);
    if (failed)
    {
        state.FailedSpawnId = state.SpawnId;
        state.FailedUntil = now + FailedVendorDelay;
        LOG_INFO("playerbots", "SelfBot {} gray vendor {} unavailable; returning to grind area", bot->GetName(),
                 state.SpawnId);
    }
    state.Phase = GrayVendorPhase::Returning;
    state.Deadline = now + JourneyTimeout;
    state.ProgressAt = now;
    state.LastProgress = WorldPosition(bot);
    bot->StopMoving();
    bot->GetMotionMaster()->Clear();
}

bool HasTimedOut(Player* bot, GrayVendorState& state)
{
    time_t const now = time(nullptr);
    if (bot->GetExactDist(state.LastProgress) >= ProgressDistance)
    {
        state.LastProgress = WorldPosition(bot);
        state.ProgressAt = now;
    }
    return now >= state.Deadline || now - state.ProgressAt >= StuckTimeout;
}

class GrayVendorMultiplier : public Multiplier
{
public:
    GrayVendorMultiplier(PlayerbotAI* botAI) : Multiplier(botAI, "gray vendor") {}

    float GetValue(Action* action) override
    {
        if (!action || VendorState(context).Phase == GrayVendorPhase::Idle)
            return 1.0f;

        if (dynamic_cast<AttackAnythingAction*>(action) || dynamic_cast<MoveRandomAction*>(action) ||
            dynamic_cast<LootAction*>(action) || dynamic_cast<MoveToLootAction*>(action))
            return 0.0f;

        return 1.0f;
    }
};
}  // namespace

bool GrayVendorNeededValue::Calculate()
{
    if (!CanVisitVendor(bot))
        return false;
    auto const settings = VendorSettings(bot);
    return settings && settings->SellQualities && FreeGeneralBagSlots(bot) <= settings->VendorFreeSlots &&
           !SaleItemsInBags(bot, *settings).empty();
}

bool GrayVendorTrigger::IsActive()
{
    GrayVendorState const& state = VendorState(context);
    return CanVisitVendor(bot) && (state.Phase != GrayVendorPhase::Idle ||
                                   (time(nullptr) >= state.RetryAfter && AI_VALUE(bool, "gray vendor needed")));
}

void SellGrayStrategy::InitTriggers(std::vector<TriggerNode*>& triggers)
{
    Strategy::InitTriggers(triggers);
    triggers.push_back(new TriggerNode(
        "gray vendor",
        {NextAction("choose gray vendor", ACTION_MOVE + 1.0f), NextAction("move to gray vendor", ACTION_MOVE + 1.0f),
         NextAction("sell gray loot", ACTION_MOVE + 1.0f), NextAction("return from gray vendor", ACTION_MOVE + 1.0f)}));
}

void SellGrayStrategy::InitMultipliers(std::vector<Multiplier*>& multipliers)
{
    Strategy::InitMultipliers(multipliers);
    multipliers.push_back(new GrayVendorMultiplier(botAI));
}

bool ChooseGrayVendorAction::isUseful()
{
    GrayVendorState const& state = VendorState(context);
    return CanVisitVendor(bot) && state.Phase == GrayVendorPhase::Idle && time(nullptr) >= state.RetryAfter &&
           AI_VALUE(bool, "gray vendor needed");
}

bool ChooseGrayVendorAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;

    GrayVendorState& state = VendorState(context);
    time_t const now = time(nullptr);
    state.RetryAfter = now + RetryDelay;
    GridCoord const center = Acore::ComputeGridCoord(bot->GetPositionX(), bot->GetPositionY());
    int32 const gridRadius = static_cast<int32>(std::ceil(VendorSearchDistance / SIZE_OF_GRIDS));
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
                if (spawnId == state.FailedSpawnId && now < state.FailedUntil)
                    continue;

                CreatureData const* data = sObjectMgr->GetCreatureData(spawnId);
                if (!data || !(data->phaseMask & bot->GetPhaseMask()))
                    continue;

                float const distance = bot->GetExactDist(data->posX, data->posY, data->posZ);
                if (distance > VendorSearchDistance)
                    continue;

                CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(data->id);
                if (!creatureTemplate ||
                    creatureTemplate->HasFlagsExtra(CREATURE_FLAG_EXTRA_NO_SELL_VENDOR | CREATURE_FLAG_EXTRA_TRIGGER) ||
                    !((data->npcflag ? data->npcflag : creatureTemplate->npcflag) & UNIT_NPC_FLAG_VENDOR))
                    continue;

                if ((data->unit_flags ? data->unit_flags : creatureTemplate->unit_flags) & UNIT_FLAG_NOT_SELECTABLE)
                    continue;

                FactionTemplateEntry const* faction = sFactionTemplateStore.LookupEntry(creatureTemplate->faction);
                if (!faction || Unit::GetFactionReactionTo(faction, bot->GetFactionTemplateEntry()) < REP_FRIENDLY)
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
            continue;

        state.SpawnId = data->spawnId;
        state.Destination = WorldPosition(data->mapid, data->posX, data->posY, data->posZ);
        state.Origin = WorldPosition(bot);
        state.LastProgress = state.Origin;
        state.ProgressAt = now;
        state.Deadline = now + JourneyTimeout;
        state.Phase = GrayVendorPhase::Traveling;
        // A vendor trip can interrupt a quest or trainer journey already in motion.
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
        LOG_INFO("playerbots", "SelfBot {} visiting gray-loot vendor {} at {:.1f} yards", bot->GetName(), state.SpawnId,
                 candidate.first);
        return true;
    }

    LOG_INFO("playerbots", "SelfBot {} found no reachable friendly gray-loot vendor within {} yards", bot->GetName(),
             VendorSearchDistance);
    return false;
}

bool MoveToGrayVendorAction::isUseful()
{
    return CanVisitVendor(bot) && VendorState(context).Phase == GrayVendorPhase::Traveling;
}

bool MoveToGrayVendorAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;

    GrayVendorState& state = VendorState(context);
    if (bot->GetMapId() != state.Destination.GetMapId() || HasTimedOut(bot, state))
    {
        BeginReturn(bot, state, true);
        return true;
    }

    Creature* vendor = ResolveVendor(bot, state.SpawnId);
    if (vendor && bot->GetNPCIfCanInteractWith(vendor->GetGUID(), UNIT_NPC_FLAG_VENDOR))
    {
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
        state.Phase = GrayVendorPhase::Selling;
        return true;
    }

    if (!vendor && bot->GetExactDist(state.Destination) <= INTERACTION_DISTANCE)
    {
        BeginReturn(bot, state, true);
        return true;
    }

    if (bot->isMoving())
        return true;

    WorldPosition const destination = vendor ? WorldPosition(vendor) : state.Destination;
    return MoveTo(destination.GetMapId(), destination.GetPositionX(), destination.GetPositionY(),
                  destination.GetPositionZ(), false, false, true);
}

bool SellGrayLootAction::isUseful()
{
    return CanVisitVendor(bot) && VendorState(context).Phase == GrayVendorPhase::Selling;
}

bool SellGrayLootAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;

    GrayVendorState& state = VendorState(context);
    Creature* vendor = ResolveVendor(bot, state.SpawnId);
    if (!vendor || !bot->GetNPCIfCanInteractWith(vendor->GetGUID(), UNIT_NPC_FLAG_VENDOR))
    {
        BeginReturn(bot, state, true);
        return false;
    }

    auto const settings = VendorSettings(bot);
    if (!settings || !settings->SellQualities)
    {
        BeginReturn(bot, state, false);
        return true;
    }
    std::vector<ObjectGuid> const items = SaleItemsInBags(bot, *settings);
    auto const questItems = ProtectedQuestItems(bot);
    uint32 const moneyBefore = bot->GetMoney();
    uint32 sold = 0;
    for (ObjectGuid const itemGuid : items)
    {
        if (!CanVisitVendor(bot))
            return false;

        Item* item = bot->GetItemByGuid(itemGuid);
        if (!ShouldSellItem(item, *settings, questItems))
            continue;

        WorldPacket packet(CMSG_SELL_ITEM);
        packet << vendor->GetGUID() << itemGuid << item->GetCount();
        WorldPackets::Item::SellItem sellPacket(std::move(packet));
        sellPacket.Read();
        bot->GetSession()->HandleSellItemOpcode(sellPacket);
        if (!bot->GetItemByGuid(itemGuid))
            ++sold;
    }
    LOG_INFO("playerbots", "SelfBot {} sold {} selected-quality stacks at vendor {}; money {} -> {} copper",
             bot->GetName(), sold, state.SpawnId, moneyBefore, bot->GetMoney());
    context->GetValue<uint8>("bag space")->Reset();
    context->GetValue<bool>("gray vendor needed")->Reset();
    BeginReturn(bot, state, sold == 0 && !items.empty());
    return true;
}

bool ReturnFromGrayVendorAction::isUseful()
{
    return CanVisitVendor(bot) && VendorState(context).Phase == GrayVendorPhase::Returning;
}

bool ReturnFromGrayVendorAction::Execute(Event /*event*/)
{
    if (!isUseful())
        return false;

    GrayVendorState& state = VendorState(context);
    bool const arrived =
        bot->GetMapId() == state.Origin.GetMapId() && bot->GetExactDist(state.Origin) <= ReturnDistance;
    if (arrived || bot->GetMapId() != state.Origin.GetMapId() || HasTimedOut(bot, state))
    {
        bot->StopMoving();
        bot->GetMotionMaster()->Clear();
        state.Phase = GrayVendorPhase::Idle;
        state.RetryAfter = time(nullptr) + RetryDelay;
        LOG_INFO("playerbots", "SelfBot {} gray vendor trip ended; returned to grind area: {}", bot->GetName(),
                 arrived);
        return true;
    }

    if (bot->isMoving())
        return true;

    return MoveTo(state.Origin.GetMapId(), state.Origin.GetPositionX(), state.Origin.GetPositionY(),
                  state.Origin.GetPositionZ(), false, false, true);
}
