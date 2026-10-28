/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTPROTOCOL_H
#define PLAYERBOTS_BOTPROTOCOL_H

#include <memory>

#include "BotWireProtocol.h"

class Player;
class SelfBotController;

class BotProtocol
{
public:
    void HandleRequest(Player& player, SelfBotController& controller, std::string const& payload);
    static void SendLegacyStatus(Player& player, BotStatusSnapshot const& snapshot);
    static void SendUnavailable(Player& player, std::string const& payload);

private:
    BotWireProtocol::RequestTracker _requests;
    std::vector<std::string> _lastReply;
    std::uint32_t _lastReplyAt = 0;
    bool _hasReplied = false;
    bool _saving = false;
    std::shared_ptr<char> _lifetime = std::make_shared<char>();
};

#endif
