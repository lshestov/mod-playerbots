/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotProtocol.h"

#include "Chat.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotMgr.h"
#include "SelfBotController.h"
#include "Timer.h"
#include "WorldSession.h"

namespace
{
constexpr uint32 MinRequestIntervalMs = 200;

void SendPackets(Player& player, std::vector<std::string> const& payloads)
{
    if (!player.GetSession() || player.GetSession()->IsSocketClosed())
        return;
    for (std::string const& payload : payloads)
    {
        WorldPacket packet;
        ChatHandler::BuildChatPacket(packet, CHAT_MSG_WHISPER, LANG_ADDON, &player, &player,
                                     std::string(BotWireProtocol::Prefix) + '\t' + payload);
        player.SendDirectMessage(&packet);
    }
}

std::string ResultName(SelfBotCommandCode code)
{
    switch (code)
    {
        case SelfBotCommandCode::Ok:
            return "ok";
        case SelfBotCommandCode::Disabled:
            return "disabled";
        case SelfBotCommandCode::Forbidden:
            return "forbidden";
        case SelfBotCommandCode::OtherMaster:
            return "other_master";
        case SelfBotCommandCode::Unavailable:
            return "unavailable";
        case SelfBotCommandCode::Conflict:
            return "conflict";
        case SelfBotCommandCode::SaveFailed:
            return "save_failed";
        case SelfBotCommandCode::Busy:
            return "busy";
        case SelfBotCommandCode::InvalidArgument:
            return "invalid_argument";
    }
    return "unavailable";
}

std::vector<std::string> CaptureReply(Player& player, SelfBotController& controller,
                                      BotWireProtocol::Request const& request, std::string_view result,
                                      std::string const& message)
{
    auto const settings = controller.GetSettings(player);
    BotStatusSnapshot snapshot = controller.GetStatus(player);
    snapshot.Note = message;
    std::string capabilities = settings ? "status,start,stop,settings,settings_set" : "status,start,stop";
    capabilities +=
        ",queue,queue_add,queue_remove,queue_move,queue_clear,queue_start,queue_pause,queue_auto,quests,quest_browser";
    auto const quests = request.Operation == BotWireProtocol::Verb::Quests
                            ? std::optional<BotWireProtocol::QuestList>(controller.GetQuests(player, request))
                            : std::nullopt;
    return BotWireProtocol::SerializeSnapshot(
        request, snapshot, result, capabilities, settings ? &*settings : nullptr,
        BotWireProtocol::IsQueueRequest(request.Operation) || quests ? &controller.GetActionQueue() : nullptr,
        quests ? &*quests : nullptr);
}
}  // namespace

void BotProtocol::HandleRequest(Player& player, SelfBotController& controller, std::string const& payload)
{
    auto const request = BotWireProtocol::ParseRequest(payload);
    if (!request)
        return;

    auto const disposition = _requests.Check(*request);
    if (disposition == BotWireProtocol::RequestDisposition::Duplicate)
    {
        SendPackets(player, _lastReply);
        return;
    }

    if (disposition != BotWireProtocol::RequestDisposition::New)
    {
        SendPackets(player, CaptureReply(player, controller, *request, "invalid_request",
                                         "Status synchronization is required before this command."));
        return;
    }

    if (_saving)
    {
        SendPackets(player, CaptureReply(player, controller, *request, "busy", "Settings are still being saved."));
        return;
    }

    uint32 const now = getMSTime();
    bool const throttled = _hasReplied && getMSTimeDiff(_lastReplyAt, now) < MinRequestIntervalMs;
    SelfBotCommandResult result;
    if (throttled)
        result.Message = "Requests are arriving too quickly. Try again.";
    else
    {
        _lastReplyAt = now;
        _hasReplied = true;
        // Chat packets run on the world thread, between AI map updates.
        switch (request->Operation)
        {
            case BotWireProtocol::Verb::Status:
            case BotWireProtocol::Verb::Queue:
            case BotWireProtocol::Verb::Quests:
                break;
            case BotWireProtocol::Verb::Start:
                result = controller.Start(player);
                break;
            case BotWireProtocol::Verb::Stop:
                result = controller.Stop(player);
                break;
            case BotWireProtocol::Verb::QueueAdd:
                result = controller.ChangeQueue(player, SelfBotQueueCommand::AddQuest, request->ExpectedQueueRevision,
                                                request->QuestId, request->BeforeActionId);
                break;
            case BotWireProtocol::Verb::QueueRemove:
                result = controller.ChangeQueue(player, SelfBotQueueCommand::Remove, request->ExpectedQueueRevision,
                                                request->ActionId);
                break;
            case BotWireProtocol::Verb::QueueClear:
                result = controller.ChangeQueue(player, SelfBotQueueCommand::Clear, request->ExpectedQueueRevision);
                break;
            case BotWireProtocol::Verb::QueueMove:
                result = controller.ChangeQueue(player, SelfBotQueueCommand::Move, request->ExpectedQueueRevision,
                                                request->ActionId, request->BeforeActionId);
                break;
            case BotWireProtocol::Verb::QueueStart:
                result = controller.ChangeQueue(player, SelfBotQueueCommand::Start, request->ExpectedQueueRevision);
                break;
            case BotWireProtocol::Verb::QueuePause:
                result = controller.ChangeQueue(player, SelfBotQueueCommand::Pause, request->ExpectedQueueRevision);
                break;
            case BotWireProtocol::Verb::QueueAuto:
                result = controller.ChangeQueue(player, SelfBotQueueCommand::Auto, request->ExpectedQueueRevision);
                break;
            case BotWireProtocol::Verb::SettingsSet:
            {
                _saving = true;
                _requests.Accept(*request);
                _lastReply.clear();
                std::weak_ptr<char> const lifetime = _lifetime;
                ObjectGuid const guid = player.GetGUID();
                controller.SaveSettings(
                    player, request->ExpectedSettingsRevision, request->Settings,
                    [lifetime, guid, request = *request](SelfBotCommandResult completed)
                    {
                        auto const owner = lifetime.lock();
                        if (!owner)
                            return;
                        Player* currentPlayer = ObjectAccessor::FindConnectedPlayer(guid);
                        PlayerbotMgr* manager = currentPlayer ? sPlayerbotsMgr.GetPlayerbotMgr(currentPlayer) : nullptr;
                        if (!manager)
                            return;
                        BotProtocol& protocol = manager->GetBotProtocol();
                        if (protocol._lifetime != owner)
                            return;
                        protocol._saving = false;
                        protocol._lastReply = CaptureReply(*currentPlayer, manager->GetSelfBotController(), request,
                                                           ResultName(completed.Code), completed.Message);
                        SendPackets(*currentPlayer, protocol._lastReply);
                    });
                return;
            }
        }
    }

    _lastReply =
        CaptureReply(player, controller, *request, throttled ? "throttled" : ResultName(result.Code), result.Message);
    _requests.Accept(*request);
    SendPackets(player, _lastReply);
}

void BotProtocol::SendLegacyStatus(Player& player, BotStatusSnapshot const& snapshot)
{
    SendPackets(player, BotWireProtocol::SerializeLegacySnapshot(snapshot));
}

void BotProtocol::SendUnavailable(Player& player, std::string const& payload)
{
    auto const request = BotWireProtocol::ParseRequest(payload);
    if (!request)
        return;
    BotStatusSnapshot snapshot;
    snapshot.Revision = 1;
    snapshot.Note = "SelfBot control is unavailable.";
    SendPackets(player, BotWireProtocol::SerializeSnapshot(*request, snapshot, "unavailable", "status"));
}
