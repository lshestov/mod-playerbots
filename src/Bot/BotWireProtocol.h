/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTWIREPROTOCOL_H
#define PLAYERBOTS_BOTWIREPROTOCOL_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "BotStatusSnapshot.h"
#include "SelfBotActionQueue.h"
#include "SelfBotSettings.h"

namespace BotWireProtocol
{
constexpr std::size_t MaxPayloadBytes = 246;
constexpr std::size_t MaxTextBytes = 120;
constexpr std::uint32_t MaxRequestId = 2147483647;
constexpr std::string_view Prefix = "IDLEBOT";

enum class Verb
{
    Status,
    Start,
    Stop,
    SettingsSet,
    Quests,
    Queue,
    QueueAdd,
    QueueRemove,
    QueueMove,
    QueueClear,
    QueueStart,
    QueuePause,
    QueueAuto
};

struct Request
{
    std::string Session;
    std::uint32_t Id = 0;
    Verb Operation = Verb::Status;
    std::uint32_t ExpectedSettingsRevision = 0;
    SelfBotSettings Settings;
    std::uint32_t ExpectedQueueRevision = 0;
    SelfBotActionKind ActionKind = SelfBotActionKind::Quest;
    std::uint32_t QuestId = 0;
    std::uint32_t ActionId = 0;
    std::uint32_t BeforeActionId = 0;
    std::uint32_t QuestOffset = 0;
    bool QuestBrowser = false;
    std::uint32_t QuestZoneId = 0;
    std::string QuestSearch;
};

constexpr std::uint32_t QuestPageSize = 8;

struct QuestListItem
{
    std::uint32_t QuestId = 0;
    std::uint32_t Level = 0;
    std::string State;
    std::string Title;
    std::string Starter;
    std::uint32_t ZoneId = 0;
    std::string Zone;
    std::string Continent;
};

struct QuestZone
{
    std::uint32_t Id = 0;
    std::string Name;
};

struct QuestList
{
    std::uint32_t Offset = 0;
    std::uint32_t Total = 0;
    std::vector<QuestListItem> Items;
    std::vector<QuestZone> Zones;
};

enum class RequestDisposition
{
    New,
    Duplicate,
    Stale,
    NeedsStatus,
    Conflict
};

// Owned by one logged-in player's endpoint, independently of the AI lifetime.
class RequestTracker
{
public:
    RequestDisposition Check(Request const& request) const;
    void Accept(Request const& request);

private:
    std::optional<Request> _lastRequest;
};

std::optional<Request> ParseRequest(std::string_view payload);
std::string ActivityName(BotActivity activity);
std::string CleanText(std::string text);
QuestList BuildQuestPage(std::vector<QuestListItem> items, Request const& request);
bool IsQueueRequest(Verb operation);
std::string QueueStatusName(SelfBotQueueStatus status);
std::vector<std::string> SerializeSnapshot(Request const& request, BotStatusSnapshot const& snapshot,
                                           std::string_view result, std::string_view capabilities = "status,start,stop",
                                           SelfBotSettings const* settings = nullptr,
                                           SelfBotActionQueue const* queue = nullptr,
                                           QuestList const* quests = nullptr);
std::vector<std::string> SerializeLegacySnapshot(BotStatusSnapshot const& snapshot);
}  // namespace BotWireProtocol

#endif
