// Legacia RO - WoE Stats & Ranking
// Records what every character does during War of Emperium (only on castle maps
// while the siege is running) and keeps an ELO-style WoE ranking.
// Data goes to the SQL tables in sql-files/upgrades/upgrade_20261104_woe_stats.sql.

#ifndef WOESTATS_HPP
#define WOESTATS_HPP

#include <string>
#include <vector>

#include <common/cbasetypes.hpp>
#include <common/mmo.hpp>

struct block_list;
class map_session_data;

/// Counters kept per character per day (columns of `woe_char_stats`)
enum e_woe_stat : uint8 {
	WOESTAT_KILL = 0,
	WOESTAT_DEATH,
	WOESTAT_DAMAGE_DONE,
	WOESTAT_DAMAGE_RECEIVED,
	WOESTAT_EMPERIUM_DAMAGE,
	WOESTAT_GUARDIAN_DAMAGE,
	WOESTAT_BARRICADE_DAMAGE,
	WOESTAT_GSTONE_DAMAGE,
	WOESTAT_EMPERIUM_KILL,
	WOESTAT_GUARDIAN_KILL,
	WOESTAT_BARRICADE_KILL,
	WOESTAT_GSTONE_KILL,
	WOESTAT_HP_POTION,
	WOESTAT_SP_POTION,
	WOESTAT_YELLOW_GEMSTONE,
	WOESTAT_RED_GEMSTONE,
	WOESTAT_BLUE_GEMSTONE,
	WOESTAT_POISON_BOTTLE,
	WOESTAT_ACID_DEMONSTRATION,
	WOESTAT_SUPPORT_SKILL,
	WOESTAT_HEALING,
	WOESTAT_WRONG_SUPPORT_SKILL,
	WOESTAT_WRONG_HEALING,
	WOESTAT_SP_USED,
	WOESTAT_ZENY_USED,
	WOESTAT_SPIRITBALL_USED,
	WOESTAT_AMMO_USED,
	WOESTAT_ACTIVE_TIME, // seconds on a WoE map while not idle
	WOESTAT_MAX
};

/// One character in the WoE ranking (`woe_ranking` table)
struct s_woe_rank {
	uint32 char_id;
	char name[NAME_LENGTH];
	int32 guild_id;
	int32 class_;
	int32 score;
	uint32 kills;
	uint32 deaths;
	bool dirty; // needs saving
};

/// Is WoE stats tracking active on this map right now? (WoE castle map + siege running)
bool woestats_active(int16 m);

/// Add to one of a character's counters (does nothing outside an active WoE map)
void woestats_add(map_session_data* sd, e_woe_stat type, int64 value);

/// Damage hook (status_damage): src hit target for `damage` HP
void woestats_damage(block_list* src, block_list* target, int32 damage);
/// Death hook (status_damage, after the death went through)
void woestats_dead(block_list* src, block_list* target, uint16 skill_id);
/// Heal hook (status_heal): only counted while a player's skill is healing (see woestats_healer_scope)
void woestats_heal(block_list* target, int32 hp);
/// Support skill used by sd on another player
void woestats_support(map_session_data* sd, map_session_data* tsd);
/// Item consumed as a skill requirement (gemstones, poison bottles, ...)
void woestats_item_used(map_session_data* sd, t_itemid nameid, int32 amount);

/// Marks a player as the source of heals for as long as this object lives.
class woestats_healer_scope {
	map_session_data* prev;
public:
	woestats_healer_scope(map_session_data* sd);
	~woestats_healer_scope();
};

// Ranking
s_woe_rank* woestats_rank_get(uint32 char_id);
int32 woestats_rank_position(uint32 char_id); // 1 = best, 0 = not ranked
const std::vector<s_woe_rank*>& woestats_ranking();
void woestats_rank_reset();

void woestats_flush();
void do_init_woestats();
void do_final_woestats();

#endif /* WOESTATS_HPP */
