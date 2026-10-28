/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "BotWireProtocol.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <map>
#include <tuple>
#include <utility>

namespace BotWireProtocol
{
namespace
{
bool IsDecimal(std::string_view value)
{
    if (value.empty())
        return false;
    for (char character : value)
        if (character < '0' || character > '9')
            return false;
    return true;
}

std::optional<std::uint32_t> ParseInteger(std::string_view value, std::uint32_t maximum, bool allowZero = false)
{
    if (value.size() > 10 || !IsDecimal(value) || (value.front() == '0' && (!allowZero || value.size() != 1)))
        return std::nullopt;
    std::uint64_t number = 0;
    for (char character : value)
        number = number * 10 + static_cast<unsigned>(character - '0');
    if (number > maximum)
        return std::nullopt;
    return static_cast<std::uint32_t>(number);
}

std::string StateFields(BotStatusSnapshot const& snapshot)
{
    return std::string(snapshot.Enabled ? "1|" : "0|") + ActivityName(snapshot.Activity) + "|" +
           std::to_string(snapshot.QuestId) + "|" + std::to_string(snapshot.Objective) + "|" +
           std::to_string(snapshot.Done) + "|" + std::to_string(snapshot.Total) + "|" +
           std::to_string(snapshot.Distance);
}

std::string Lowercase(std::string text)
{
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return text;
}
}  // namespace

std::optional<Request> ParseRequest(std::string_view payload)
{
    if (payload.size() > MaxPayloadBytes)
        return std::nullopt;

    std::vector<std::string_view> fields;
    while (true)
    {
        std::size_t const separator = payload.find('|');
        fields.push_back(payload.substr(0, separator));
        if (fields.size() > 10)
            return std::nullopt;
        if (separator == std::string_view::npos)
            break;
        payload.remove_prefix(separator + 1);
    }

    if (fields.size() < 5 || fields[0] != "2" || fields[1] != "request" || fields[2].size() > 20 ||
        !IsDecimal(fields[2]))
        return std::nullopt;

    auto const id = ParseInteger(fields[3], MaxRequestId);
    if (!id)
        return std::nullopt;

    Request request;
    request.Session = fields[2];
    request.Id = *id;
    if (fields[4] == "quests" && (fields.size() == 6 || fields.size() == 8))
    {
        auto const offset = ParseInteger(fields[5], std::numeric_limits<std::uint32_t>::max(), true);
        if (!offset || *offset % QuestPageSize)
            return std::nullopt;
        request.Operation = Verb::Quests;
        request.QuestOffset = *offset;
        if (fields.size() == 8)
        {
            auto const zone = ParseInteger(fields[6], std::numeric_limits<std::uint32_t>::max(), true);
            if (!zone || fields[7].size() > 64 || CleanText(std::string(fields[7])) != fields[7])
                return std::nullopt;
            request.QuestBrowser = true;
            request.QuestZoneId = *zone;
            request.QuestSearch = fields[7];
        }
        return request;
    }
    if (fields[4] == "queue" && fields.size() == 5)
    {
        request.Operation = Verb::Queue;
        return request;
    }
    if (fields[4] == "queue_add" || fields[4] == "queue_remove" || fields[4] == "queue_move" ||
        fields[4] == "queue_clear" || fields[4] == "queue_start" || fields[4] == "queue_pause" ||
        fields[4] == "queue_auto")
    {
        bool const adding = fields[4] == "queue_add";
        std::size_t const expectedSize = adding || fields[4] == "queue_move" ? 8 : fields[4] == "queue_remove" ? 7 : 6;
        if (fields.size() != expectedSize && !(adding && fields.size() == expectedSize + 1))
            return std::nullopt;
        auto const revision = ParseInteger(fields[5], std::numeric_limits<std::uint32_t>::max());
        if (!revision)
            return std::nullopt;
        request.ExpectedQueueRevision = *revision;
        if (fields[4] == "queue_add")
        {
            auto const questId = ParseInteger(fields[7], std::numeric_limits<std::uint32_t>::max());
            if (fields[6] != "quest" || !questId)
                return std::nullopt;
            request.Operation = Verb::QueueAdd;
            request.QuestId = *questId;
            if (fields.size() == 9)
            {
                auto const before = ParseInteger(fields[8], std::numeric_limits<std::uint32_t>::max(), true);
                if (!before)
                    return std::nullopt;
                request.BeforeActionId = *before;
            }
        }
        else if (fields[4] == "queue_remove")
        {
            auto const actionId = ParseInteger(fields[6], std::numeric_limits<std::uint32_t>::max());
            if (!actionId)
                return std::nullopt;
            request.Operation = Verb::QueueRemove;
            request.ActionId = *actionId;
        }
        else if (fields[4] == "queue_move")
        {
            auto const actionId = ParseInteger(fields[6], std::numeric_limits<std::uint32_t>::max());
            auto const before = ParseInteger(fields[7], std::numeric_limits<std::uint32_t>::max(), true);
            if (!actionId || !before)
                return std::nullopt;
            request.Operation = Verb::QueueMove;
            request.ActionId = *actionId;
            request.BeforeActionId = *before;
        }
        else if (fields[4] == "queue_clear")
            request.Operation = Verb::QueueClear;
        else if (fields[4] == "queue_start")
            request.Operation = Verb::QueueStart;
        else if (fields[4] == "queue_pause")
            request.Operation = Verb::QueuePause;
        else
            request.Operation = Verb::QueueAuto;
        return request;
    }
    if (fields[4] == "settings_set")
    {
        if (fields.size() != 10)
            return std::nullopt;
        auto const revision = ParseInteger(fields[5], std::numeric_limits<std::uint32_t>::max());
        auto const qualities = ParseInteger(fields[6], SelfBotSettings::MaxSellQualities, true);
        auto const slots = ParseInteger(fields[7], SelfBotSettings::MaxVendorFreeSlots, true);
        auto const reserve = ParseInteger(fields[9], SelfBotSettings::MaxTrainingReserveCopper, true);
        if (!revision || !qualities || !slots || !reserve || (fields[8] != "0" && fields[8] != "1"))
            return std::nullopt;
        request.Operation = Verb::SettingsSet;
        request.ExpectedSettingsRevision = *revision;
        request.Settings.SellQualities = *qualities;
        request.Settings.VendorFreeSlots = *slots;
        request.Settings.AutoTrain = fields[8] == "1";
        request.Settings.TrainingReserveCopper = *reserve;
        return request;
    }
    if (fields.size() != 5)
        return std::nullopt;
    if (fields[4] == "status")
        request.Operation = Verb::Status;
    else if (fields[4] == "start")
        request.Operation = Verb::Start;
    else if (fields[4] == "stop")
        request.Operation = Verb::Stop;
    else
        return std::nullopt;
    return request;
}

RequestDisposition RequestTracker::Check(Request const& request) const
{
    if (!_lastRequest || _lastRequest->Session != request.Session)
        return request.Operation == Verb::Status ? RequestDisposition::New : RequestDisposition::NeedsStatus;
    if (request.Id < _lastRequest->Id)
        return RequestDisposition::Stale;
    if (request.Id == _lastRequest->Id)
        return request.Operation == _lastRequest->Operation &&
                       request.ExpectedSettingsRevision == _lastRequest->ExpectedSettingsRevision &&
                       request.Settings == _lastRequest->Settings &&
                       request.ExpectedQueueRevision == _lastRequest->ExpectedQueueRevision &&
                       request.ActionKind == _lastRequest->ActionKind && request.QuestId == _lastRequest->QuestId &&
                       request.ActionId == _lastRequest->ActionId &&
                       request.BeforeActionId == _lastRequest->BeforeActionId &&
                       request.QuestOffset == _lastRequest->QuestOffset &&
                       request.QuestBrowser == _lastRequest->QuestBrowser &&
                       request.QuestZoneId == _lastRequest->QuestZoneId &&
                       request.QuestSearch == _lastRequest->QuestSearch
                   ? RequestDisposition::Duplicate
                   : RequestDisposition::Conflict;
    return RequestDisposition::New;
}

void RequestTracker::Accept(Request const& request) { _lastRequest = request; }

std::string ActivityName(BotActivity activity)
{
    switch (activity)
    {
        case BotActivity::Manual:
            return "manual";
        case BotActivity::Idle:
            return "idle";
        case BotActivity::Travel:
            return "travel";
        case BotActivity::Interact:
            return "interact";
        case BotActivity::Combat:
            return "combat";
        case BotActivity::Dead:
            return "dead";
        case BotActivity::Recover:
            return "recover";
        case BotActivity::Train:
            return "train";
        case BotActivity::Vendor:
            return "vendor";
    }
    return "idle";
}

std::string CleanText(std::string text)
{
    for (char& character : text)
        if (character == '|' || static_cast<unsigned char>(character) < 32 || character == 127)
            character = ' ';

    if (text.size() > MaxTextBytes)
    {
        std::size_t length = MaxTextBytes;
        while (length && (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80)
            --length;
        text.resize(length);
    }
    return text;
}

bool IsQueueRequest(Verb operation)
{
    return operation == Verb::Queue || operation == Verb::QueueAdd || operation == Verb::QueueRemove ||
           operation == Verb::QueueMove || operation == Verb::QueueClear || operation == Verb::QueueStart ||
           operation == Verb::QueuePause || operation == Verb::QueueAuto;
}

QuestList BuildQuestPage(std::vector<QuestListItem> items, Request const& request)
{
    QuestList page;
    std::map<std::uint32_t, std::string> zones;
    for (QuestListItem const& item : items)
        if (item.ZoneId)
            zones.emplace(item.ZoneId, item.Zone);
    for (auto const& [id, name] : zones)
        page.Zones.push_back({id, name});
    std::sort(page.Zones.begin(), page.Zones.end(), [](QuestZone const& left, QuestZone const& right)
              { return std::tie(left.Name, left.Id) < std::tie(right.Name, right.Id); });
    std::string const query = Lowercase(request.QuestSearch);
    items.erase(
        std::remove_if(items.begin(), items.end(),
                       [&](QuestListItem const& item)
                       {
                           return (request.QuestZoneId && request.QuestZoneId != item.ZoneId) ||
                                  (!query.empty() && Lowercase(item.Title + " " + item.Starter + " " + item.Zone + " " +
                                                               item.Continent + " " + std::to_string(item.QuestId))
                                                             .find(query) == std::string::npos);
                       }),
        items.end());
    std::sort(items.begin(), items.end(), [](QuestListItem const& left, QuestListItem const& right)
              { return std::tie(left.Level, left.QuestId) < std::tie(right.Level, right.QuestId); });
    page.Total = static_cast<std::uint32_t>(items.size());
    page.Offset = page.Total ? std::min(request.QuestOffset, (page.Total - 1) / QuestPageSize * QuestPageSize) : 0;
    for (std::size_t index = page.Offset; index < items.size() && page.Items.size() < QuestPageSize; ++index)
        page.Items.push_back(std::move(items[index]));
    return page;
}

std::string QueueStatusName(SelfBotQueueStatus status)
{
    switch (status)
    {
        case SelfBotQueueStatus::Draft:
            return "draft";
        case SelfBotQueueStatus::Running:
            return "running";
        case SelfBotQueueStatus::Paused:
            return "paused";
        case SelfBotQueueStatus::Blocked:
            return "blocked";
        case SelfBotQueueStatus::Completed:
            return "completed";
    }
    return "draft";
}

std::vector<std::string> SerializeSnapshot(Request const& request, BotStatusSnapshot const& snapshot,
                                           std::string_view result, std::string_view capabilities,
                                           SelfBotSettings const* settings, SelfBotActionQueue const* queue,
                                           QuestList const* quests)
{
    std::string const correlation =
        request.Session + "|" + std::to_string(request.Id) + "|" + std::to_string(snapshot.Revision) + "|";
    auto field = [&](std::string_view key, std::string const& value)
    { return "2|" + std::string(key) + "|" + correlation + CleanText(value); };
    std::vector<std::string> packets{"2|begin|" + correlation + std::string(result) + "|" + std::string(capabilities),
                                     "2|state|" + correlation + StateFields(snapshot),
                                     field("title", snapshot.Title),
                                     field("destination", snapshot.Destination),
                                     field("detail", snapshot.Detail),
                                     field("action", snapshot.LastAction),
                                     field("note", snapshot.Note)};
    if (settings)
        packets.push_back("2|settings|" + correlation + std::to_string(settings->Revision) + "|" +
                          std::to_string(settings->SellQualities) + "|" + std::to_string(settings->VendorFreeSlots) +
                          "|" + (settings->AutoTrain ? "1|" : "0|") + std::to_string(settings->TrainingReserveCopper));
    if (queue)
    {
        SelfBotQueuedAction const* current = queue->Current();
        packets.push_back("2|queue|" + correlation + std::to_string(queue->Revision()) + "|" +
                          (queue->Selected() ? "queue|" : "auto|") + QueueStatusName(queue->Status()) + "|" +
                          std::to_string(queue->Cursor()) + "|" + std::to_string(queue->Actions().size()) + "|" +
                          std::to_string(current ? current->Id : 0) + "|" +
                          std::to_string(current ? current->QuestId : 0));
        packets.push_back(field("queue_note", queue->Reason()));
        for (std::size_t index = 0; index < queue->Actions().size(); ++index)
        {
            SelfBotQueuedAction const& action = queue->Actions()[index];
            packets.push_back("2|queue_item|" + correlation + std::to_string(index + 1) + "|" +
                              std::to_string(action.Id) + "|quest|" + std::to_string(action.QuestId) + "|" +
                              (index < queue->Cursor() ? "done|" : "pending|") + CleanText(action.Title));
        }
    }
    if (quests)
    {
        packets.push_back("2|quests|" + correlation + std::to_string(quests->Offset) + "|" +
                          std::to_string(quests->Total) + "|" + std::to_string(quests->Items.size()));
        for (std::size_t index = 0; index < quests->Items.size(); ++index)
        {
            QuestListItem const& item = quests->Items[index];
            packets.push_back("2|quest_item|" + correlation + std::to_string(index + 1) + "|" +
                              std::to_string(item.QuestId) + "|" + std::to_string(item.Level) + "|" + item.State + "|" +
                              CleanText(item.Title));
            if (request.QuestBrowser)
            {
                std::string const position = std::to_string(index + 1) + "|";
                packets.push_back("2|quest_starter|" + correlation + position + CleanText(item.Starter));
                packets.push_back("2|quest_zone|" + correlation + position + std::to_string(item.ZoneId) + "|" +
                                  CleanText(item.Zone));
                packets.push_back("2|quest_continent|" + correlation + position + CleanText(item.Continent));
            }
        }
        if (request.QuestBrowser)
        {
            packets.push_back("2|quest_zones|" + correlation + std::to_string(quests->Zones.size()));
            for (std::size_t index = 0; index < quests->Zones.size(); ++index)
                packets.push_back("2|quest_zone_option|" + correlation + std::to_string(index + 1) + "|" +
                                  std::to_string(quests->Zones[index].Id) + "|" + CleanText(quests->Zones[index].Name));
        }
    }
    packets.push_back("2|end|" + correlation.substr(0, correlation.size() - 1));
    return packets;
}

std::vector<std::string> SerializeLegacySnapshot(BotStatusSnapshot const& snapshot)
{
    return {"1|state|" + StateFields(snapshot),
            "1|title|" + CleanText(snapshot.Title),
            "1|destination|" + CleanText(snapshot.Destination),
            "1|detail|" + CleanText(snapshot.Detail),
            "1|action|" + CleanText(snapshot.LastAction),
            "1|note|" + CleanText(snapshot.Note)};
}
}  // namespace BotWireProtocol
