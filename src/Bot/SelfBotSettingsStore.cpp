/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "SelfBotSettingsStore.h"

#include "Log.h"
#include "PlayerbotsDatabase.h"
#include "QueryResult.h"

namespace
{
constexpr std::uint32_t SchemaVersion = 1;

void ApplyStrategyDefault(std::string_view token, std::string_view name, bool& enabled)
{
    if (token.size() < 2 || token.substr(1) != name)
        return;
    if (token.front() == '+')
        enabled = true;
    else if (token.front() == '-')
        enabled = false;
    else if (token.front() == '~')
        enabled = !enabled;
}
}  // namespace

SelfBotSettings SelfBotSettingsStore::Defaults(std::string_view strategies)
{
    SelfBotSettings settings;
    settings.AutoTrain = false;
    bool sellLoot = false;
    while (!strategies.empty())
    {
        std::size_t const separator = strategies.find(',');
        std::string_view const token = strategies.substr(0, separator);
        ApplyStrategyDefault(token, "selfbot trainer", settings.AutoTrain);
        ApplyStrategyDefault(token, "sell gray", sellLoot);
        if (separator == std::string_view::npos)
            break;
        strategies.remove_prefix(separator + 1);
    }
    settings.SellQualities = sellLoot ? 1 : 0;
    return settings;
}

std::optional<SelfBotSettings> SelfBotSettingsStore::Load(std::uint32_t characterGuid, std::uint32_t accountId,
                                                          SelfBotSettings const& defaults)
{
    PlayerbotsDatabasePreparedStatement* statement =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_SEL_SELFBOT_SETTINGS);
    statement->SetData(0, characterGuid);
    statement->SetData(1, accountId);
    PreparedQueryResult const result = PlayerbotsDatabase.Query(statement);
    if (!result)
        return std::nullopt;

    Field* fields = result->Fetch();
    if (!fields[6].Get<std::uint32_t>())
        return defaults;

    std::uint32_t const schema = fields[0].Get<std::uint32_t>();
    std::uint32_t const revision = fields[1].Get<std::uint32_t>();
    std::uint32_t const sellQualities = fields[2].Get<std::uint32_t>();
    std::uint32_t const vendorFreeSlots = fields[3].Get<std::uint32_t>();
    std::uint32_t const autoTrain = fields[4].Get<std::uint32_t>();
    std::uint32_t const trainingReserveCopper = fields[5].Get<std::uint32_t>();
    if (schema != SchemaVersion || !revision || sellQualities > SelfBotSettings::MaxSellQualities ||
        vendorFreeSlots > SelfBotSettings::MaxVendorFreeSlots || autoTrain > 1 ||
        trainingReserveCopper > SelfBotSettings::MaxTrainingReserveCopper)
    {
        LOG_ERROR("playerbots", "Invalid SelfBot settings for account {}", accountId);
        return std::nullopt;
    }
    return SelfBotSettings{revision, sellQualities, vendorFreeSlots, autoTrain != 0, trainingReserveCopper};
}

TransactionCallback SelfBotSettingsStore::Save(std::uint32_t characterGuid, std::uint32_t accountId,
                                               SelfBotSettings const& previous, SelfBotSettings const& desired)
{
    PlayerbotsDatabaseTransaction transaction = PlayerbotsDatabase.BeginTransaction();
    PlayerbotsDatabasePreparedStatement* initialize =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_INS_SELFBOT_SETTINGS);
    initialize->SetData(0, characterGuid);
    initialize->SetData(1, accountId);
    initialize->SetData(2, SchemaVersion);
    initialize->SetData(3, previous.Revision);
    initialize->SetData(4, previous.SellQualities);
    initialize->SetData(5, previous.VendorFreeSlots);
    initialize->SetData(6, previous.AutoTrain);
    initialize->SetData(7, previous.TrainingReserveCopper);
    transaction->Append(initialize);

    PlayerbotsDatabasePreparedStatement* update =
        PlayerbotsDatabase.GetPreparedStatement(PLAYERBOTS_UPD_SELFBOT_SETTINGS);
    update->SetData(0, desired.Revision);
    update->SetData(1, desired.SellQualities);
    update->SetData(2, desired.VendorFreeSlots);
    update->SetData(3, desired.AutoTrain);
    update->SetData(4, desired.TrainingReserveCopper);
    update->SetData(5, characterGuid);
    update->SetData(6, accountId);
    update->SetData(7, SchemaVersion);
    update->SetData(8, previous.Revision);
    transaction->Append(update);
    return PlayerbotsDatabase.AsyncCommitTransaction(transaction);
}
