/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTSETTINGS_H
#define PLAYERBOTS_SELFBOTSETTINGS_H

#include <cstdint>

struct SelfBotSettings
{
    static constexpr std::uint32_t MaxSellQualities = 15;
    static constexpr std::uint32_t MaxVendorFreeSlots = 16;
    static constexpr std::uint32_t MaxTrainingReserveCopper = 2147483647;

    std::uint32_t Revision = 1;
    std::uint32_t SellQualities = 1;
    std::uint32_t VendorFreeSlots = 4;
    bool AutoTrain = true;
    std::uint32_t TrainingReserveCopper = 0;

    bool SellsQuality(std::uint32_t quality) const
    {
        return quality < 4 && (SellQualities & (std::uint32_t(1) << quality)) != 0;
    }

    bool CanAffordTraining(std::uint32_t money, std::uint32_t cost) const
    {
        return AutoTrain && money >= TrainingReserveCopper && cost <= money - TrainingReserveCopper;
    }

    bool operator==(SelfBotSettings const&) const = default;
};

#endif
