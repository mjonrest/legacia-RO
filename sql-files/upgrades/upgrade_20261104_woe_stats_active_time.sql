-- Legacia RO - WoE Stats: active time (seconds on a WoE map while not AFK)
-- Only needed if you already ran upgrade_20261104_woe_stats.sql before this column was added.
ALTER TABLE `woe_char_stats` ADD COLUMN `active_time` int(11) unsigned NOT NULL default '0' AFTER `ammo_used`;
