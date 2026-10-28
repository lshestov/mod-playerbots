/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SELFBOTSETTINGSSTORE_H
#define PLAYERBOTS_SELFBOTSETTINGSSTORE_H

#include <optional>
#include <string_view>

#include "SelfBotSettings.h"

class TransactionCallback;

namespace SelfBotSettingsStore
{
SelfBotSettings Defaults(std::string_view strategies);
std::optional<SelfBotSettings> Load(std::uint32_t characterGuid, std::uint32_t accountId,
                                    SelfBotSettings const& defaults);
TransactionCallback Save(std::uint32_t characterGuid, std::uint32_t accountId, SelfBotSettings const& previous,
                         SelfBotSettings const& desired);
}  // namespace SelfBotSettingsStore

#endif
