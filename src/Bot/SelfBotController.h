/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTCONTROLLER_H
#define PLAYERBOTS_SELFBOTCONTROLLER_H

#include <functional>
#include <memory>
#include <optional>

#include "BotStatusSnapshot.h"
#include "BotWireProtocol.h"
#include "SelfBotActionQueue.h"
#include "SelfBotSettings.h"

class Engine;
class Player;
class PlayerbotAI;

enum class SelfBotCommandCode
{
    Ok,
    Disabled,
    Forbidden,
    OtherMaster,
    Unavailable,
    Conflict,
    SaveFailed,
    Busy,
    InvalidArgument
};

struct SelfBotCommandResult
{
    SelfBotCommandCode Code = SelfBotCommandCode::Ok;
    std::string Message;
};

class SelfBotController
{
public:
    SelfBotController();
    BotStatusSnapshot GetStatus(Player& player);
    std::optional<SelfBotSettings> GetSettings(Player& player);
    std::optional<SelfBotSettings> GetCachedSettings() const;
    void SaveSettings(Player& player, std::uint32_t expectedRevision, SelfBotSettings const& desired,
                      std::function<void(SelfBotCommandResult)> completion);
    void UpdateSettings();
    void ApplySettingsToEngine(Engine& engine) const;
    SelfBotCommandResult Start(Player& player);
    SelfBotCommandResult Stop(Player& player, bool stopCurrentAction = true);
    BotWireProtocol::QuestList GetQuests(Player& player, BotWireProtocol::Request const& request) const;
    SelfBotActionQueue& GetActionQueue() { return _actionQueue; }
    SelfBotActionQueue const& GetActionQueue() const { return _actionQueue; }
    SelfBotCommandResult ChangeQueue(Player& player, SelfBotQueueCommand command, std::uint32_t expectedRevision,
                                     std::uint32_t parameter = 0, std::uint32_t beforeActionId = 0);

private:
    struct SettingsState;
    void ApplySettings(Player& player, PlayerbotAI& botAI,
                       std::optional<SelfBotSettings> const& previous = std::nullopt);

    std::shared_ptr<SettingsState> _settingsState;
    std::uint32_t _statusRevision = 0;
    SelfBotActionQueue _actionQueue;
};

#endif
