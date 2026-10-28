/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_BOTSTATUSSNAPSHOT_H
#define PLAYERBOTS_BOTSTATUSSNAPSHOT_H

#include <cstdint>
#include <string>

class Player;

enum class BotActivity
{
    Manual,
    Idle,
    Travel,
    Interact,
    Combat,
    Dead,
    Recover,
    Train,
    Vendor
};

struct BotStatusSnapshot
{
    static BotStatusSnapshot Capture(Player& player);

    std::uint32_t Revision = 0;
    bool Enabled = false;
    BotActivity Activity = BotActivity::Manual;
    std::uint32_t QuestId = 0;
    std::uint32_t Objective = 0;
    std::uint32_t Done = 0;
    std::uint32_t Total = 0;
    std::int32_t Distance = -1;
    std::string Title;
    std::string Destination;
    std::string Detail;
    std::string LastAction;
    std::string Note;
};

#endif
