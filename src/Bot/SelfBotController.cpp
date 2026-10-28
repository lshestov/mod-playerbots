/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfBotController.h"

#include <algorithm>
#include <limits>
#include <tuple>

#include "AiObjectContext.h"
#include "BotWireProtocol.h"
#include "DBCStores.h"
#include "Engine.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotMgr.h"
#include "PlayerbotRepository.h"
#include "SelfBotQuestStrategy.h"
#include "SelfBotSettingsStore.h"
#include "SelfBotTrainerStrategy.h"
#include "SellGrayStrategy.h"
#include "Timer.h"
#include "Transaction.h"
#include "WorldSession.h"

struct SelfBotController::SettingsState
{
    std::optional<SelfBotSettings> Settings;
    std::optional<TransactionCallback> SaveCallback;
    std::uint32_t LastLoadAttempt = 0;
    bool LoadAttempted = false;
    bool Saving = false;
};

namespace
{
constexpr std::uint32_t SettingsReloadDelay = 5000;

void StopAutonomousTravel(Player& player)
{
    if (player.IsInCombat() || player.IsInFlight() || player.GetVehicle())
        return;
    MotionMaster* motion = player.GetMotionMaster();
    if (motion->GetMotionSlot(MOTION_SLOT_ACTIVE))
        motion->MovementExpiredOnSlot(MOTION_SLOT_ACTIVE, false);
    if (!motion->GetMotionSlot(MOTION_SLOT_CONTROLLED))
        player.StopMoving();
}

void SetEngineStrategy(Engine& engine, char const* name, bool enabled)
{
    if (enabled)
        engine.addStrategy(name, false);
    else
        engine.removeStrategy(name, false);
}
}  // namespace

SelfBotController::SelfBotController() : _settingsState(std::make_shared<SettingsState>()) {}

BotStatusSnapshot SelfBotController::GetStatus(Player& player)
{
    BotStatusSnapshot snapshot = BotStatusSnapshot::Capture(player);
    if (_actionQueue.Selected() && snapshot.Enabled && snapshot.Activity == BotActivity::Idle)
    {
        snapshot.Title = "Action queue";
        snapshot.Detail = _actionQueue.Reason().empty()
                              ? "Queue: " + BotWireProtocol::QueueStatusName(_actionQueue.Status())
                              : _actionQueue.Reason();
        if (SelfBotQueuedAction const* current = _actionQueue.Current())
            snapshot.QuestId = current->QuestId;
    }
    snapshot.Revision = ++_statusRevision;
    if (!snapshot.Revision)
        snapshot.Revision = ++_statusRevision;
    return snapshot;
}

std::optional<SelfBotSettings> SelfBotController::GetSettings(Player& player)
{
    if (!player.GetSession() || player.GetSession()->IsSocketClosed())
        return std::nullopt;
    std::uint32_t const now = getMSTime();
    if (_settingsState->LoadAttempted && getMSTimeDiff(_settingsState->LastLoadAttempt, now) < SettingsReloadDelay)
        return _settingsState->Settings;
    _settingsState->LastLoadAttempt = now;
    _settingsState->LoadAttempted = true;
    SelfBotSettings const defaults = SelfBotSettingsStore::Defaults(sPlayerbotAIConfig.NonCombatStrategies);
    std::optional<SelfBotSettings> const loaded =
        SelfBotSettingsStore::Load(player.GetGUID().GetCounter(), player.GetSession()->GetAccountId(), defaults);
    if (loaded && loaded != _settingsState->Settings)
    {
        std::optional<SelfBotSettings> const previous = _settingsState->Settings;
        _settingsState->Settings = loaded;
        if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(&player); botAI && botAI->GetMaster() == &player)
            ApplySettings(player, *botAI, previous);
    }
    return _settingsState->Settings;
}

std::optional<SelfBotSettings> SelfBotController::GetCachedSettings() const { return _settingsState->Settings; }

void SelfBotController::SaveSettings(Player& player, std::uint32_t expectedRevision, SelfBotSettings const& desired,
                                     std::function<void(SelfBotCommandResult)> completion)
{
    if (!player.GetSession() || player.GetSession()->IsSocketClosed() || player.GetSession()->IsLoggingOut())
    {
        completion({SelfBotCommandCode::Unavailable, "SelfBot settings are unavailable."});
        return;
    }
    if (_settingsState->Saving)
    {
        completion({SelfBotCommandCode::Busy, "A settings save is already in progress."});
        return;
    }
    if (desired.SellQualities > SelfBotSettings::MaxSellQualities ||
        desired.VendorFreeSlots > SelfBotSettings::MaxVendorFreeSlots ||
        desired.TrainingReserveCopper > SelfBotSettings::MaxTrainingReserveCopper)
    {
        completion({SelfBotCommandCode::SaveFailed, "Invalid bot settings."});
        return;
    }
    if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(&player); botAI && botAI->GetMaster() != &player)
    {
        completion({SelfBotCommandCode::OtherMaster, "This character is controlled by another bot master."});
        return;
    }
    std::optional<SelfBotSettings> const current = GetSettings(player);
    if (!current)
    {
        completion({SelfBotCommandCode::SaveFailed, "Settings storage is unavailable."});
        return;
    }
    if (current->Revision != expectedRevision)
    {
        completion({SelfBotCommandCode::Conflict, "Settings changed. Review the latest values and save again."});
        return;
    }
    if (current->Revision == std::numeric_limits<std::uint32_t>::max())
    {
        completion({SelfBotCommandCode::SaveFailed, "The settings revision limit has been reached."});
        return;
    }

    SelfBotSettings next = desired;
    next.Revision = current->Revision + 1;
    ObjectGuid const characterGuid = player.GetGUID();
    std::uint32_t const accountId = player.GetSession()->GetAccountId();
    std::weak_ptr<SettingsState> const lifetime = _settingsState;
    _settingsState->Saving = true;
    _settingsState->SaveCallback.emplace(
        SelfBotSettingsStore::Save(characterGuid.GetCounter(), accountId, *current, next));
    _settingsState->SaveCallback->AfterComplete(
        [lifetime, characterGuid, accountId, next, completion = std::move(completion)](bool success)
        {
            std::shared_ptr<SettingsState> const state = lifetime.lock();
            if (!state)
                return;
            state->Saving = false;
            Player* owner = ObjectAccessor::FindConnectedPlayer(characterGuid);
            PlayerbotMgr* manager = owner ? sPlayerbotsMgr.GetPlayerbotMgr(owner) : nullptr;
            if (!manager || manager->GetSelfBotController()._settingsState != state || !owner->GetSession() ||
                owner->GetSession()->GetAccountId() != accountId)
                return;
            SelfBotController& controller = manager->GetSelfBotController();
            if (!success)
            {
                completion({SelfBotCommandCode::SaveFailed, "Settings could not be saved. Try again."});
                return;
            }
            state->LastLoadAttempt = getMSTime();
            state->LoadAttempted = true;
            std::optional<SelfBotSettings> const stored =
                SelfBotSettingsStore::Load(characterGuid.GetCounter(), accountId, {});
            if (!stored)
            {
                completion({SelfBotCommandCode::SaveFailed, "The saved settings could not be verified."});
                return;
            }
            std::optional<SelfBotSettings> const previous = state->Settings;
            state->Settings = *stored;
            if (stored != previous)
                if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(owner); botAI && botAI->GetMaster() == owner)
                    controller.ApplySettings(*owner, *botAI, previous);
            if (*stored != next)
            {
                completion(
                    {SelfBotCommandCode::Conflict, "Settings changed. Review the latest values and save again."});
                return;
            }
            completion({SelfBotCommandCode::Ok, "Bot settings saved."});
        });
}

void SelfBotController::UpdateSettings()
{
    if (_settingsState->SaveCallback && _settingsState->SaveCallback->InvokeIfReady())
        _settingsState->SaveCallback.reset();
}

void SelfBotController::ApplySettingsToEngine(Engine& engine) const
{
    if (!_settingsState->Settings)
        return;
    SelfBotSettings const& settings = *_settingsState->Settings;
    SetEngineStrategy(engine, "selfbot trainer", settings.AutoTrain);
    SetEngineStrategy(engine, "sell gray", settings.SellQualities != 0);
}

void SelfBotController::ApplySettings(Player& player, PlayerbotAI& botAI,
                                      std::optional<SelfBotSettings> const& previous)
{
    if (!_settingsState->Settings || botAI.GetMaster() != &player)
        return;
    botAI.ChangeStrategy(
        sPlayerbotAIConfig.SelfBotQuesting ? "+selfbot quest item combat" : "-selfbot quest item combat",
        BOT_STATE_COMBAT);
    SelfBotSettings const& settings = *_settingsState->Settings;
    AiObjectContext* context = botAI.GetAiObjectContext();
    bool const trainerChanged = previous && (previous->AutoTrain != settings.AutoTrain ||
                                             previous->TrainingReserveCopper != settings.TrainingReserveCopper);
    bool const vendorChanged = previous && (previous->SellQualities != settings.SellQualities ||
                                            previous->VendorFreeSlots != settings.VendorFreeSlots);
    bool stopTravel = false;
    if (!settings.AutoTrain || trainerChanged)
    {
        SelfBotTrainerState& state = context->GetValue<SelfBotTrainerState>("selfbot trainer state")->RefGet();
        stopTravel = stopTravel || state.Phase != SelfBotTrainerPhase::Idle;
        state = {};
    }
    if (!settings.SellQualities || vendorChanged)
    {
        GrayVendorState& state = context->GetValue<GrayVendorState>("gray vendor state")->RefGet();
        stopTravel = stopTravel || state.Phase != GrayVendorPhase::Idle;
        state = {};
    }
    if (vendorChanged)
        context->GetValue<bool>("gray vendor needed")->Reset();
    if (stopTravel)
        StopAutonomousTravel(player);

    std::string const strategies = std::string(settings.AutoTrain ? "+selfbot trainer" : "-selfbot trainer") +
                                   (settings.SellQualities ? ",+sell gray" : ",-sell gray");
    botAI.ChangeStrategy(strategies, BOT_STATE_NON_COMBAT);
}

SelfBotCommandResult SelfBotController::Start(Player& player)
{
    if (!player.GetSession() || player.GetSession()->IsSocketClosed())
        return {SelfBotCommandCode::Unavailable, "SelfBot control is unavailable."};

    if (_settingsState->Saving)
        return {SelfBotCommandCode::Busy, "A settings save is already in progress."};

    if (PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(&player))
    {
        if (botAI->GetMaster() != &player)
            return {SelfBotCommandCode::OtherMaster, "This character is controlled by another bot master."};
        if (!GetSettings(player))
            return {SelfBotCommandCode::Unavailable, "Settings storage is unavailable."};
        ApplySettings(player, *botAI);
        return {};
    }

    if (!sPlayerbotAIConfig.Enabled || sPlayerbotAIConfig.SelfBotLevel == 0)
        return {SelfBotCommandCode::Disabled, "SelfBot is disabled server-wide."};
    if (sPlayerbotAIConfig.SelfBotLevel == 1 && !player.CanBeGameMaster())
        return {SelfBotCommandCode::Forbidden, "SelfBot is restricted for this account."};

    if (!GetSettings(player))
        return {SelfBotCommandCode::Unavailable, "Settings storage is unavailable."};

    sPlayerbotsMgr.AddPlayerbotData(&player, true);
    PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(&player);
    if (!botAI)
        return {SelfBotCommandCode::Unavailable, "SelfBot control is unavailable."};

    botAI->SetMaster(&player);
    PlayerbotRepository::instance().Load(botAI);
    // Saved strategies may predate the configured SelfBot overrides.
    botAI->ChangeStrategy(sPlayerbotAIConfig.NonCombatStrategies, BOT_STATE_NON_COMBAT);
    ApplySettings(player, *botAI);
    return {SelfBotCommandCode::Ok, "SelfBot is now active."};
}

SelfBotCommandResult SelfBotController::Stop(Player& player, bool stopCurrentAction)
{
    if (!player.GetSession() || player.GetSession()->IsSocketClosed())
        return {SelfBotCommandCode::Unavailable, "SelfBot control is unavailable."};

    PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(&player);
    if (!botAI)
    {
        _actionQueue.Pause();
        return {};
    }
    if (botAI->GetMaster() != &player)
        return {SelfBotCommandCode::OtherMaster, "This character is controlled by another bot master."};

    _actionQueue.Pause();
    delete botAI;

    if (stopCurrentAction)
    {
        if (!player.IsInFlight() && !player.GetVehicle())
        {
            MotionMaster* motion = player.GetMotionMaster();
            if (motion->GetMotionSlot(MOTION_SLOT_ACTIVE))
                motion->MovementExpiredOnSlot(MOTION_SLOT_ACTIVE, false);
            if (!motion->GetMotionSlot(MOTION_SLOT_CONTROLLED))
                player.StopMoving();
        }
        player.AttackStop();
        player.InterruptNonMeleeSpells(true);
        player.SpellQueue.clear();
    }

    if (player.isTaxiCheater())
        player.SetTaxiCheater(false);

    return {SelfBotCommandCode::Ok, "SelfBot is now deactivated."};
}

BotWireProtocol::QuestList SelfBotController::GetQuests(Player& player, BotWireProtocol::Request const& request) const
{
    BotWireProtocol::QuestList list;
    SelfBotQuestCatalog const& catalog = SelfBotQuestCatalog::Instance();
    if (!sPlayerbotAIConfig.SelfBotQuesting || !catalog.IsInitialized())
        return list;
    WorldPosition const origin(&player);
    std::vector<SelfBotQuestRow> rows = catalog.Evaluate(&player, origin, catalog.MaxSearchRadius(origin));
    rows.erase(std::remove_if(rows.begin(), rows.end(),
                              [&](SelfBotQuestRow const& row)
                              {
                                  bool const complete = row.Status == QUEST_STATUS_COMPLETE;
                                  bool const active = row.Status == QUEST_STATUS_INCOMPLETE;
                                  return row.Rewarded || !row.Quest->ExclusionReason.empty() ||
                                         (!complete && (!row.Quest->Supported || row.Quest->SuggestedPlayers > 1 ||
                                                        !SelfBotQuestPlanning::QuestLevelAllowed(row.Quest->QuestLevel,
                                                                                                 player.GetLevel()))) ||
                                         (!complete && !active && !(row.CanTake && row.CanAdd));
                              }),
               rows.end());
    std::vector<BotWireProtocol::QuestListItem> items;
    for (SelfBotQuestRow const& row : rows)
    {
        std::uint32_t const level =
            row.Quest->QuestLevel > 0 ? static_cast<std::uint32_t>(row.Quest->QuestLevel) : player.GetLevel();
        BotWireProtocol::QuestListItem item{row.Quest->Id, level,
                                            row.Status == QUEST_STATUS_COMPLETE     ? "complete"
                                            : row.Status == QUEST_STATUS_INCOMPLETE ? "active"
                                                                                    : "available",
                                            row.Quest->Title};
        SelfBotQuestSource const* starter = nullptr;
        for (SelfBotQuestSource const* source : row.Quest->Givers)
            if (!starter || (source->Position.GetMapId() == player.GetMapId() &&
                             (starter->Position.GetMapId() != player.GetMapId() ||
                              origin.GetExactDist(source->Position) < origin.GetExactDist(starter->Position))))
                starter = source;
        item.Starter = "Unknown starter";
        item.Zone = "Unknown zone";
        item.Continent = "Unknown continent";
        if (starter)
        {
            if (starter->Entry > 0)
            {
                if (CreatureTemplate const* creature = sObjectMgr->GetCreatureTemplate(starter->Entry))
                    item.Starter = creature->Name;
            }
            else if (starter->Entry < 0)
                if (GameObjectTemplate const* object = sObjectMgr->GetGameObjectTemplate(-starter->Entry))
                    item.Starter = object->name;
            AreaTableEntry const* area = starter->AreaId ? sAreaTableStore.LookupEntry(starter->AreaId) : nullptr;
            if (!area)
            {
                WorldPosition position(starter->Position);
                area = position.getArea();
            }
            if (area && area->zone)
                if (AreaTableEntry const* parent = sAreaTableStore.LookupEntry(area->zone))
                    area = parent;
            if (area)
            {
                item.ZoneId = area->ID;
                item.Zone = area->area_name[LOCALE_enUS];
            }
            if (MapEntry const* map = sMapStore.LookupEntry(starter->Position.GetMapId()))
                item.Continent = map->name[LOCALE_enUS];
        }
        items.push_back(std::move(item));
    }
    return BotWireProtocol::BuildQuestPage(std::move(items), request);
}

SelfBotCommandResult SelfBotController::ChangeQueue(Player& player, SelfBotQueueCommand command,
                                                    std::uint32_t expectedRevision, std::uint32_t parameter,
                                                    std::uint32_t beforeActionId)
{
    if (!player.GetSession() || player.GetSession()->IsSocketClosed() || player.GetSession()->IsLoggingOut())
        return {SelfBotCommandCode::Unavailable, "Action queue is unavailable."};
    PlayerbotAI* botAI = sPlayerbotsMgr.GetPlayerbotAI(&player);
    if (botAI && botAI->GetMaster() != &player)
        return {SelfBotCommandCode::OtherMaster, "This character is controlled by another bot master."};
    if (expectedRevision != _actionQueue.Revision())
        return {SelfBotCommandCode::Conflict, "The queue changed. Read it again before editing."};
    bool const editing = command == SelfBotQueueCommand::AddQuest || command == SelfBotQueueCommand::Remove ||
                         command == SelfBotQueueCommand::Move || command == SelfBotQueueCommand::Clear;
    if (editing && !_actionQueue.Editable())
        return {SelfBotCommandCode::Busy, "Pause the queue before editing it."};
    bool const queueWasSelected = _actionQueue.Selected();

    switch (command)
    {
        case SelfBotQueueCommand::AddQuest:
        {
            if (!sPlayerbotAIConfig.SelfBotQuesting || !SelfBotQuestCatalog::Instance().IsInitialized())
                return {SelfBotCommandCode::Disabled, "SelfBot questing is disabled."};
            SelfBotQuestDefinition const* definition = SelfBotQuestCatalog::Instance().Find(parameter);
            if (!definition)
                return {SelfBotCommandCode::InvalidArgument, "Quest is not in the server quest catalog."};
            if (!_actionQueue.Insert(SelfBotActionKind::Quest, parameter, beforeActionId, definition->Title))
                return {SelfBotCommandCode::InvalidArgument,
                        "The queue is full, the quest is duplicated, or the insertion target is not pending."};
            return {};
        }
        case SelfBotQueueCommand::Remove:
            if (!_actionQueue.Remove(parameter))
                return {SelfBotCommandCode::InvalidArgument, "Queue action was not found."};
            break;
        case SelfBotQueueCommand::Move:
            if (!_actionQueue.Move(parameter, beforeActionId))
                return {SelfBotCommandCode::InvalidArgument, "Only existing pending actions can be reordered."};
            break;
        case SelfBotQueueCommand::Clear:
            _actionQueue.Clear();
            break;
        case SelfBotQueueCommand::Start:
        {
            if (!sPlayerbotAIConfig.SelfBotQuesting || !SelfBotQuestCatalog::Instance().IsInitialized())
                return {SelfBotCommandCode::Disabled, "SelfBot questing is disabled."};
            if (!_actionQueue.Current())
                return {SelfBotCommandCode::InvalidArgument, "The queue has no pending actions."};
            SelfBotCommandResult const started = Start(player);
            if (started.Code != SelfBotCommandCode::Ok)
                return started;
            botAI = sPlayerbotsMgr.GetPlayerbotAI(&player);
            if (!botAI)
                return {SelfBotCommandCode::Unavailable, "SelfBot control is unavailable."};
            botAI->ChangeStrategy("+selfbot quest", BOT_STATE_NON_COMBAT);
            botAI->ChangeStrategy("+selfbot quest item combat", BOT_STATE_COMBAT);
            _actionQueue.Start();
            break;
        }
        case SelfBotQueueCommand::Pause:
            _actionQueue.Pause();
            break;
        case SelfBotQueueCommand::Auto:
            _actionQueue.UseAutomatic();
            break;
    }
    if (botAI && (queueWasSelected || _actionQueue.Selected()))
    {
        botAI->GetAiObjectContext()->GetValue<SelfBotQuestState>("selfbot quest state")->Reset();
        StopAutonomousTravel(player);
    }
    return {};
}
