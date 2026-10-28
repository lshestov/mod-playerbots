CREATE TABLE IF NOT EXISTS `playerbots_selfbot_settings` (
    `guid` INT UNSIGNED NOT NULL,
    `account_id` INT UNSIGNED NOT NULL,
    `schema_version` INT UNSIGNED NOT NULL DEFAULT 1,
    `revision` INT UNSIGNED NOT NULL DEFAULT 1,
    `sell_qualities` INT UNSIGNED NOT NULL DEFAULT 1,
    `vendor_free_slots` INT UNSIGNED NOT NULL DEFAULT 4,
    `auto_train` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `training_reserve_copper` INT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (`guid`, `account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
