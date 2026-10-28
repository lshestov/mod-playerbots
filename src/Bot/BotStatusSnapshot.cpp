/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotStatusSnapshot.h"

#include <algorithm>
#include <cmath>

#include "DBCStores.h"
#include "GameObjectData.h"
#include "ObjectMgr.h"
#include "Playerbots.h"
#include "SelfBotQuestStrategy.h"
#include "SelfBotTrainerStrategy.h"
#include "SellGrayStrategy.h"

BotStatusSnapshot BotStatusSnapshot::Capture(Player& player)
{
    BotStatusSnapshot snapshot;
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(&player);
    snapshot.Enabled = botAI && botAI->GetMaster() == &player;
    snapshot.Activity = snapshot.Enabled ? BotActivity::Idle : BotActivity::Manual;
    WorldPosition destination;
    bool hasDestination = false;

    if (snapshot.Enabled)
    {
        AiObjectContext* context = botAI->GetAiObjectContext();
        SelfBotQuestState const& questState = context->GetValue<SelfBotQuestState>("selfbot quest state")->RefGet();
        SelfBotTrainerState const& trainer = context->GetValue<SelfBotTrainerState>("selfbot trainer state")->RefGet();
        GrayVendorState const& vendor = context->GetValue<GrayVendorState>("gray vendor state")->RefGet();
        snapshot.LastAction = botAI->HandleRemoteCommand("action");

        if (vendor.Phase != GrayVendorPhase::Idle)
        {
            snapshot.Activity = vendor.Phase == GrayVendorPhase::Selling ? BotActivity::Vendor : BotActivity::Travel;
            snapshot.Title = "Sell items";
            snapshot.Detail = vendor.Phase == GrayVendorPhase::Returning ? "Returning from vendor" : "Visit vendor";
            destination = vendor.Phase == GrayVendorPhase::Returning ? vendor.Origin : vendor.Destination;
            hasDestination = true;
            if (vendor.Phase == GrayVendorPhase::Returning)
                snapshot.Destination = "Return point";
            else if (CreatureData const* spawn = sObjectMgr->GetCreatureData(vendor.SpawnId))
                if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(spawn->id))
                    snapshot.Destination = creature->Name;
        }
        else if (trainer.Phase != SelfBotTrainerPhase::Idle &&
                 botAI->HasStrategy("selfbot trainer", BOT_STATE_NON_COMBAT))
        {
            snapshot.Activity =
                trainer.Phase == SelfBotTrainerPhase::Traveling ? BotActivity::Travel : BotActivity::Train;
            snapshot.Title = "Class training";
            snapshot.Detail = "Learn available skills";
            destination = trainer.Destination;
            hasDestination = true;
            if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(trainer.Entry))
                snapshot.Destination = creature->Name;
        }
        else if (questState.QuestId && questState.Phase != SelfBotQuestPhase::Idle &&
                 botAI->HasStrategy("selfbot quest", BOT_STATE_NON_COMBAT))
        {
            snapshot.QuestId = questState.QuestId;
            snapshot.Objective = questState.Objective;
            snapshot.Activity =
                questState.Phase == SelfBotQuestPhase::Traveling ? BotActivity::Travel : BotActivity::Interact;
            destination = questState.Source.Position;
            hasDestination = true;
            if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(questState.Source.AreaId))
                snapshot.Destination = area->area_name[0];

            if (Quest const* quest = sObjectMgr->GetQuestTemplate(snapshot.QuestId))
            {
                snapshot.Title = quest->GetTitle();
                switch (questState.Task)
                {
                    case SelfBotQuestTask::Accept:
                        snapshot.Detail = "Accept quest";
                        break;
                    case SelfBotQuestTask::TurnIn:
                        snapshot.Detail = "Turn in quest";
                        break;
                    case SelfBotQuestTask::Explore:
                    {
                        snapshot.Detail = "Explore destination";
                        auto const& statuses = player.getQuestStatusMap();
                        auto const status = statuses.find(snapshot.QuestId);
                        snapshot.Done = status != statuses.end() && status->second.Explored ? 1 : 0;
                        snapshot.Total = 1;
                        break;
                    }
                    case SelfBotQuestTask::Item:
                        if (snapshot.Objective < QUEST_ITEM_OBJECTIVES_COUNT)
                        {
                            uint32 const itemId = quest->RequiredItemId[snapshot.Objective];
                            snapshot.Total = quest->RequiredItemCount[snapshot.Objective];
                            snapshot.Done = std::min(player.GetItemCount(itemId), snapshot.Total);
                            if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemId))
                                snapshot.Detail = item->Name1;
                        }
                        break;
                    case SelfBotQuestTask::UseItem:
                        snapshot.Detail = "Use quest item";
                        if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(questState.UseItemId))
                            snapshot.Detail += ": " + item->Name1;
                        if (questState.UseItemObjective && snapshot.Objective < QUEST_ITEM_OBJECTIVES_COUNT)
                        {
                            snapshot.Total = quest->RequiredItemCount[snapshot.Objective];
                            snapshot.Done = std::min(player.GetItemCount(quest->RequiredItemId[snapshot.Objective]),
                                                     snapshot.Total);
                        }
                        else if (!questState.UseItemObjective && snapshot.Objective < QUEST_OBJECTIVES_COUNT)
                        {
                            snapshot.Total = quest->RequiredNpcOrGoCount[snapshot.Objective];
                            uint16 const slot = player.FindQuestSlot(snapshot.QuestId);
                            if (slot < MAX_QUEST_LOG_SIZE)
                                snapshot.Done = std::min<uint32>(player.GetQuestSlotCounter(slot, snapshot.Objective),
                                                                 snapshot.Total);
                        }
                        break;
                    case SelfBotQuestTask::Objective:
                        if (snapshot.Objective < QUEST_OBJECTIVES_COUNT)
                        {
                            snapshot.Total = quest->RequiredNpcOrGoCount[snapshot.Objective];
                            uint16 const slot = player.FindQuestSlot(snapshot.QuestId);
                            if (slot < MAX_QUEST_LOG_SIZE)
                                snapshot.Done = std::min<uint32>(player.GetQuestSlotCounter(slot, snapshot.Objective),
                                                                 snapshot.Total);
                            snapshot.Detail = quest->ObjectiveText[snapshot.Objective];
                            int32 const entry = quest->RequiredNpcOrGo[snapshot.Objective];
                            if (snapshot.Detail.empty() && entry > 0)
                            {
                                if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(entry))
                                    snapshot.Detail = creature->Name;
                            }
                            else if (snapshot.Detail.empty() && entry < 0)
                            {
                                if (GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(-entry))
                                    snapshot.Detail = object->name;
                            }
                        }
                        break;
                }
            }
        }

        if (hasDestination)
        {
            // Use loaded catalog/DBC data only; inspecting status must not load remote terrain.
            if (snapshot.Destination.empty())
                if (MapEntry const* map = sMapStore.LookupEntry(destination.GetMapId()))
                    snapshot.Destination = map->name[0];
            if (destination.GetMapId() == player.GetMapId())
            {
                float const directDistance = player.GetExactDist(destination);
                if (std::isfinite(directDistance))
                    snapshot.Distance = static_cast<int32>(std::min(directDistance, 1000000.0f));
            }
        }

        if (!player.IsAlive())
            snapshot.Activity = BotActivity::Dead;
        else if (player.IsInCombat())
            snapshot.Activity = BotActivity::Combat;
        else if (snapshot.LastAction == "eat" || snapshot.LastAction == "drink")
            snapshot.Activity = BotActivity::Recover;
    }

    return snapshot;
}
