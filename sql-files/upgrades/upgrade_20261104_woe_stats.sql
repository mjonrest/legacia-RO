-- Legacia RO - WoE Stats & Ranking (src/map/woestats.cpp)
-- Run this once on the main (char/map) database, then restart the map server.

--
-- Per-character WoE stats, one row per character per day
--
CREATE TABLE IF NOT EXISTS `woe_char_stats` (
  `char_id` int(11) unsigned NOT NULL,
  `date` date NOT NULL,
  `guild_id` int(11) unsigned NOT NULL default '0',
  `kill_count` int(11) unsigned NOT NULL default '0',
  `death_count` int(11) unsigned NOT NULL default '0',
  `score` int(11) NOT NULL default '0',
  `top_damage` int(11) unsigned NOT NULL default '0',
  `damage_done` bigint(20) unsigned NOT NULL default '0',
  `damage_received` bigint(20) unsigned NOT NULL default '0',
  `emperium_damage` bigint(20) unsigned NOT NULL default '0',
  `guardian_damage` bigint(20) unsigned NOT NULL default '0',
  `barricade_damage` bigint(20) unsigned NOT NULL default '0',
  `gstone_damage` bigint(20) unsigned NOT NULL default '0',
  `emperium_kill` int(11) unsigned NOT NULL default '0',
  `guardian_kill` int(11) unsigned NOT NULL default '0',
  `barricade_kill` int(11) unsigned NOT NULL default '0',
  `gstone_kill` int(11) unsigned NOT NULL default '0',
  `sp_heal_potions` int(11) unsigned NOT NULL default '0',
  `hp_heal_potions` int(11) unsigned NOT NULL default '0',
  `yellow_gemstones` int(11) unsigned NOT NULL default '0',
  `red_gemstones` int(11) unsigned NOT NULL default '0',
  `blue_gemstones` int(11) unsigned NOT NULL default '0',
  `poison_bottles` int(11) unsigned NOT NULL default '0',
  `acid_demonstration` int(11) unsigned NOT NULL default '0',
  `support_skills_used` int(11) unsigned NOT NULL default '0',
  `healing_done` bigint(20) unsigned NOT NULL default '0',
  `wrong_support_skills_used` int(11) unsigned NOT NULL default '0',
  `wrong_healing_done` bigint(20) unsigned NOT NULL default '0',
  `sp_used` bigint(20) unsigned NOT NULL default '0',
  `zeny_used` bigint(20) unsigned NOT NULL default '0',
  `spiritb_used` int(11) unsigned NOT NULL default '0',
  `ammo_used` int(11) unsigned NOT NULL default '0',
  `active_time` int(11) unsigned NOT NULL default '0',
  PRIMARY KEY (`char_id`, `date`),
  KEY `date` (`date`),
  KEY `guild_id` (`guild_id`)
) ENGINE=MyISAM;

--
-- Guild kills/deaths per day
--
CREATE TABLE IF NOT EXISTS `woe_guild_stats` (
  `guild_id` int(11) unsigned NOT NULL,
  `date` date NOT NULL,
  `kill_count` int(11) unsigned NOT NULL default '0',
  `death_count` int(11) unsigned NOT NULL default '0',
  PRIMARY KEY (`guild_id`, `date`),
  KEY `date` (`date`)
) ENGINE=MyISAM;

--
-- Every player kill during WoE
--
CREATE TABLE IF NOT EXISTS `woe_kill_log` (
  `id` int(11) unsigned NOT NULL auto_increment,
  `time` datetime NOT NULL,
  `killer` varchar(30) NOT NULL default '',
  `killer_id` int(11) unsigned NOT NULL default '0',
  `killer_guild_id` int(11) unsigned NOT NULL default '0',
  `killed` varchar(30) NOT NULL default '',
  `killed_id` int(11) unsigned NOT NULL default '0',
  `killed_guild_id` int(11) unsigned NOT NULL default '0',
  `map` varchar(11) NOT NULL default '',
  `skill` smallint(6) unsigned NOT NULL default '0',
  `score_gain` int(11) NOT NULL default '0',
  PRIMARY KEY (`id`),
  KEY `killer_id` (`killer_id`),
  KEY `killed_id` (`killed_id`),
  KEY `time` (`time`)
) ENGINE=MyISAM;

--
-- Current WoE ranking (ELO-style score). Loaded into memory when the map server starts.
-- Season reset: use the script command woerankreset (don't empty this table while the server runs).
--
CREATE TABLE IF NOT EXISTS `woe_ranking` (
  `char_id` int(11) unsigned NOT NULL,
  `name` varchar(30) NOT NULL default '',
  `guild_id` int(11) unsigned NOT NULL default '0',
  `class` smallint(6) unsigned NOT NULL default '0',
  `score` int(11) NOT NULL default '0',
  `kill_count` int(11) unsigned NOT NULL default '0',
  `death_count` int(11) unsigned NOT NULL default '0',
  `last_update` datetime NOT NULL,
  PRIMARY KEY (`char_id`),
  KEY `score` (`score`)
) ENGINE=MyISAM;
