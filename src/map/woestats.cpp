// Legacia RO - WoE Stats & Ranking
// See woestats.hpp. Settings: woe_stats_* / woe_rank_* in conf/battle/guild.conf.

#include "woestats.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <map>
#include <unordered_map>
#include <utility>

#include <common/showmsg.hpp>
#include <common/socket.hpp>
#include <common/sql.hpp>
#include <common/strlib.hpp>
#include <common/timer.hpp>

#include "battle.hpp"
#include "guild.hpp"
#include "map.hpp"
#include "mob.hpp"
#include "pc.hpp"

using namespace rathena;

/// Column of `woe_char_stats` for each e_woe_stat
static const char* woestat_columns[WOESTAT_MAX] = {
	"kill_count",
	"death_count",
	"damage_done",
	"damage_received",
	"emperium_damage",
	"guardian_damage",
	"barricade_damage",
	"gstone_damage",
	"emperium_kill",
	"guardian_kill",
	"barricade_kill",
	"gstone_kill",
	"hp_heal_potions",
	"sp_heal_potions",
	"yellow_gemstones",
	"red_gemstones",
	"blue_gemstones",
	"poison_bottles",
	"acid_demonstration",
	"support_skills_used",
	"healing_done",
	"wrong_support_skills_used",
	"wrong_healing_done",
	"sp_used",
	"zeny_used",
	"spiritb_used",
	"ammo_used",
	"active_time",
};

static const char* WOE_CHAR_TABLE = "woe_char_stats";
static const char* WOE_GUILD_TABLE = "woe_guild_stats";
static const char* WOE_KILL_TABLE = "woe_kill_log";
static const char* WOE_RANK_TABLE = "woe_ranking";

struct s_woe_day {
	int32 guild_id = 0;
	int32 top_damage = 0;
	int64 val[WOESTAT_MAX] = {};
};

struct s_woe_guild_day {
	uint32 kills = 0;
	uint32 deaths = 0;
};

// Pending (not yet saved) counters, key = <id, date as YYYYMMDD>
static std::map<std::pair<uint32, int32>, s_woe_day> woe_char_pending;
static std::map<std::pair<int32, int32>, s_woe_guild_day> woe_guild_pending;

// Ranking
static std::unordered_map<uint32, s_woe_rank> woe_rank_db;
static std::vector<s_woe_rank*> woe_rank_sorted;
static std::unordered_map<uint32, int32> woe_rank_pos;
static bool woe_rank_sort_dirty = true;

// Last time killer (high 32 bits) killed victim (low 32 bits), for the repeat-kill rule
static std::unordered_map<uint64, t_tick> woe_last_kill;

// Player whose skill is healing right now (see woestats_healer_scope)
static map_session_data* woe_healer = nullptr;

static int32 woestats_flush_tid = INVALID_TIMER;
static int32 woestats_time_tid = INVALID_TIMER;

#define WOESTATS_TIME_INTERVAL 10 // seconds between active time checks

static int32 woestats_today() {
	time_t now = time(nullptr);
	struct tm* lt = localtime(&now);
	return (lt->tm_year + 1900) * 10000 + (lt->tm_mon + 1) * 100 + lt->tm_mday;
}

bool woestats_active(int16 m) {
	if (!battle_config.woe_stats_enable || m < 0 || !is_agit_start())
		return false;

	map_data* mapdata = map_getmapdata(m);

	if (mapdata == nullptr)
		return false;

	return ((agit_flag || agit2_flag) && mapdata->getMapFlag(MF_GVG_CASTLE))
		|| (agit3_flag && mapdata->getMapFlag(MF_GVG_TE_CASTLE));
}

static s_woe_day& woestats_day(map_session_data* sd) {
	s_woe_day& day = woe_char_pending[std::make_pair(sd->status.char_id, woestats_today())];
	day.guild_id = sd->status.guild_id;
	return day;
}

void woestats_add(map_session_data* sd, e_woe_stat type, int64 value) {
	if (sd == nullptr || value <= 0 || type >= WOESTAT_MAX || !woestats_active(sd->m))
		return;

	woestats_day(sd).val[type] += value;
}

/// Resolve the player behind an attack (homunculus, mercenary, elemental, pet, ... count for their owner)
static map_session_data* woestats_owner(block_list* bl) {
	if (bl == nullptr)
		return nullptr;

	if (bl->type == BL_PC)
		return reinterpret_cast<map_session_data*>(bl);

	block_list* master = battle_get_master(bl);

	if (master != nullptr && master->type == BL_PC)
		return reinterpret_cast<map_session_data*>(master);

	return nullptr;
}

/// Same guild or allied guild (or self)
static bool woestats_friendly(map_session_data* sd, map_session_data* tsd) {
	if (sd == tsd)
		return true;

	int32 gid = sd->status.guild_id, tgid = tsd->status.guild_id;

	return gid != 0 && tgid != 0 && (gid == tgid || guild_isallied(gid, tgid));
}

/// Both players come from the same device (Gepard unique ID, or IP when there is none)
static bool woestats_same_device(map_session_data* sd, map_session_data* tsd) {
	if (!session_isActive(sd->fd) || !session_isActive(tsd->fd))
		return false;

	uint32 uid = session[sd->fd]->gepard_info.unique_id, tuid = session[tsd->fd]->gepard_info.unique_id;

	if (uid != 0 && tuid != 0)
		return uid == tuid;

	return session[sd->fd]->client_addr == session[tsd->fd]->client_addr;
}

enum e_woe_target : uint8 { WOETARGET_NONE, WOETARGET_EMPERIUM, WOETARGET_GUARDIAN, WOETARGET_BARRICADE, WOETARGET_GSTONE };

static e_woe_target woestats_target_type(block_list* bl) {
	if (bl == nullptr || bl->type != BL_MOB)
		return WOETARGET_NONE;

	mob_data* md = reinterpret_cast<mob_data*>(bl);

	switch (md->mob_id) {
		case MOBID_EMPERIUM:
			return WOETARGET_EMPERIUM;
		case MOBID_GUARDIAN_STONE1:
		case MOBID_GUARDIAN_STONE2:
			return WOETARGET_GSTONE;
		case 1905: // BARRICADE
		case 1906: // BARRICADE_
			return WOETARGET_BARRICADE;
	}

	if (md->guardian_data != nullptr)
		return WOETARGET_GUARDIAN;

	return WOETARGET_NONE;
}

void woestats_damage(block_list* src, block_list* target, int32 damage) {
	if (damage <= 0 || src == nullptr || target == nullptr || !woestats_active(target->m))
		return;

	map_session_data* sd = woestats_owner(src);

	if (target->type == BL_PC) {
		map_session_data* tsd = reinterpret_cast<map_session_data*>(target);

		if (sd == tsd)
			return; // self damage (e.g. Sacrifice) doesn't count

		woestats_day(tsd).val[WOESTAT_DAMAGE_RECEIVED] += damage;

		if (sd != nullptr) {
			s_woe_day& day = woestats_day(sd);

			day.val[WOESTAT_DAMAGE_DONE] += damage;
			if (damage > day.top_damage)
				day.top_damage = damage;
		}
		return;
	}

	if (sd == nullptr)
		return;

	switch (woestats_target_type(target)) {
		case WOETARGET_EMPERIUM:  woestats_day(sd).val[WOESTAT_EMPERIUM_DAMAGE] += damage; break;
		case WOETARGET_GUARDIAN:  woestats_day(sd).val[WOESTAT_GUARDIAN_DAMAGE] += damage; break;
		case WOETARGET_BARRICADE: woestats_day(sd).val[WOESTAT_BARRICADE_DAMAGE] += damage; break;
		case WOETARGET_GSTONE:    woestats_day(sd).val[WOESTAT_GSTONE_DAMAGE] += damage; break;
		default: break;
	}
}

/// Get (or create) a character's ranking entry and refresh name/guild/job from the online character
static s_woe_rank& woestats_rank_touch(map_session_data* sd) {
	auto it = woe_rank_db.find(sd->status.char_id);

	if (it == woe_rank_db.end()) {
		s_woe_rank entry = {};

		entry.char_id = sd->status.char_id;
		entry.score = battle_config.woe_rank_start_score;
		it = woe_rank_db.emplace(sd->status.char_id, entry).first;
		woe_rank_sort_dirty = true;
	}

	s_woe_rank& rank = it->second;

	safestrncpy(rank.name, sd->status.name, sizeof(rank.name));
	rank.guild_id = sd->status.guild_id;
	rank.class_ = sd->status.class_;
	rank.dirty = true;

	return rank;
}

/// ELO-style score change for a player kill. Returns the points the killer won.
static int32 woestats_rank_kill(map_session_data* ksd, map_session_data* vsd) {
	s_woe_rank& killer = woestats_rank_touch(ksd);
	s_woe_rank& victim = woestats_rank_touch(vsd);

	killer.kills++;
	victim.deaths++;
	woe_rank_sort_dirty = true;

	// No points for: friendly kills, kills between characters of the same device,
	// or killing the same character again within woe_rank_repeat_time seconds.
	if (woestats_friendly(ksd, vsd))
		return 0;
	if (battle_config.woe_rank_same_device && woestats_same_device(ksd, vsd))
		return 0;

	uint64 key = (static_cast<uint64>(ksd->status.char_id) << 32) | vsd->status.char_id;
	t_tick now = gettick();
	auto last = woe_last_kill.find(key);
	bool repeat = last != woe_last_kill.end() && DIFF_TICK(now, last->second) < battle_config.woe_rank_repeat_time * 1000;

	woe_last_kill[key] = now;

	if (repeat)
		return 0;

	// Expected chance that the killer wins; beating a stronger player gives more points
	double expected = 1.0 / (1.0 + std::pow(10.0, (victim.score - killer.score) / 400.0));
	int32 gain = static_cast<int32>(std::lround(battle_config.woe_rank_k_factor * (1.0 - expected)));

	gain = std::max(gain, 1);

	int32 loss = gain * battle_config.woe_rank_loss_rate / 100;

	killer.score += gain;
	victim.score = std::max(0, victim.score - loss);

	return gain;
}

static void woestats_log_kill(map_session_data* ksd, map_session_data* vsd, uint16 skill_id, int32 gain) {
	char kname[NAME_LENGTH * 2 + 1], vname[NAME_LENGTH * 2 + 1];
	map_data* mapdata = map_getmapdata(vsd->m);

	Sql_EscapeStringLen(mmysql_handle, kname, ksd->status.name, strnlen(ksd->status.name, NAME_LENGTH));
	Sql_EscapeStringLen(mmysql_handle, vname, vsd->status.name, strnlen(vsd->status.name, NAME_LENGTH));

	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"INSERT INTO `%s` (`time`, `killer`, `killer_id`, `killer_guild_id`, `killed`, `killed_id`, `killed_guild_id`, `map`, `skill`, `score_gain`) "
		"VALUES (NOW(), '%s', '%u', '%d', '%s', '%u', '%d', '%s', '%u', '%d')",
		WOE_KILL_TABLE, kname, ksd->status.char_id, ksd->status.guild_id, vname, vsd->status.char_id, vsd->status.guild_id,
		mapdata != nullptr ? mapdata->name : "", skill_id, gain))
		Sql_ShowDebug(mmysql_handle);
}

void woestats_dead(block_list* src, block_list* target, uint16 skill_id) {
	if (target == nullptr || !woestats_active(target->m))
		return;

	map_session_data* sd = woestats_owner(src);
	int32 today = woestats_today();

	if (target->type == BL_PC) {
		map_session_data* tsd = reinterpret_cast<map_session_data*>(target);

		woestats_day(tsd).val[WOESTAT_DEATH]++;
		if (tsd->status.guild_id != 0)
			woe_guild_pending[std::make_pair(tsd->status.guild_id, today)].deaths++;

		if (sd == nullptr || sd == tsd) {
			// No player killer: still counts as a death in the ranking
			woestats_rank_touch(tsd).deaths++;
			woe_rank_sort_dirty = true;
			return;
		}

		woestats_day(sd).val[WOESTAT_KILL]++;
		if (sd->status.guild_id != 0)
			woe_guild_pending[std::make_pair(sd->status.guild_id, today)].kills++;

		int32 gain = woestats_rank_kill(sd, tsd);

		if (battle_config.woe_stats_kill_log)
			woestats_log_kill(sd, tsd, skill_id, gain);
		return;
	}

	if (sd == nullptr)
		return;

	switch (woestats_target_type(target)) {
		case WOETARGET_EMPERIUM:  woestats_day(sd).val[WOESTAT_EMPERIUM_KILL]++; break;
		case WOETARGET_GUARDIAN:  woestats_day(sd).val[WOESTAT_GUARDIAN_KILL]++; break;
		case WOETARGET_BARRICADE: woestats_day(sd).val[WOESTAT_BARRICADE_KILL]++; break;
		case WOETARGET_GSTONE:    woestats_day(sd).val[WOESTAT_GSTONE_KILL]++; break;
		default: break;
	}
}

woestats_healer_scope::woestats_healer_scope(map_session_data* sd) : prev(woe_healer) {
	woe_healer = sd;
}

woestats_healer_scope::~woestats_healer_scope() {
	woe_healer = prev;
}

void woestats_heal(block_list* target, int32 hp) {
	map_session_data* sd = woe_healer;

	if (sd == nullptr || hp <= 0 || target == nullptr || target->type != BL_PC || !woestats_active(sd->m))
		return;

	map_session_data* tsd = reinterpret_cast<map_session_data*>(target);
	s_woe_day& day = woestats_day(sd);

	if (woestats_friendly(sd, tsd))
		day.val[WOESTAT_HEALING] += hp;
	else
		day.val[WOESTAT_WRONG_HEALING] += hp; // healed an enemy
}

void woestats_support(map_session_data* sd, map_session_data* tsd) {
	if (sd == nullptr || tsd == nullptr || sd == tsd || !woestats_active(sd->m))
		return;

	if (woestats_friendly(sd, tsd))
		woestats_day(sd).val[WOESTAT_SUPPORT_SKILL]++;
	else
		woestats_day(sd).val[WOESTAT_WRONG_SUPPORT_SKILL]++; // buffed an enemy
}

void woestats_item_used(map_session_data* sd, t_itemid nameid, int32 amount) {
	switch (nameid) {
		case 715: woestats_add(sd, WOESTAT_YELLOW_GEMSTONE, amount); break;
		case 716: woestats_add(sd, WOESTAT_RED_GEMSTONE, amount); break;
		case 717: woestats_add(sd, WOESTAT_BLUE_GEMSTONE, amount); break;
		case 678: woestats_add(sd, WOESTAT_POISON_BOTTLE, amount); break;
	}
}

/*==========================================
 * Ranking
 *------------------------------------------*/

static void woestats_rank_sort() {
	if (!woe_rank_sort_dirty)
		return;

	woe_rank_sorted.clear();
	woe_rank_sorted.reserve(woe_rank_db.size());
	for (auto& it : woe_rank_db)
		woe_rank_sorted.push_back(&it.second);

	std::sort(woe_rank_sorted.begin(), woe_rank_sorted.end(), [](const s_woe_rank* a, const s_woe_rank* b) {
		if (a->score != b->score)
			return a->score > b->score;
		if (a->kills != b->kills)
			return a->kills > b->kills;
		return a->char_id < b->char_id;
	});

	woe_rank_pos.clear();
	for (size_t i = 0; i < woe_rank_sorted.size(); i++)
		woe_rank_pos[woe_rank_sorted[i]->char_id] = static_cast<int32>(i + 1);

	woe_rank_sort_dirty = false;
}

s_woe_rank* woestats_rank_get(uint32 char_id) {
	auto it = woe_rank_db.find(char_id);

	return it != woe_rank_db.end() ? &it->second : nullptr;
}

int32 woestats_rank_position(uint32 char_id) {
	woestats_rank_sort();

	auto it = woe_rank_pos.find(char_id);

	return it != woe_rank_pos.end() ? it->second : 0;
}

const std::vector<s_woe_rank*>& woestats_ranking() {
	woestats_rank_sort();
	return woe_rank_sorted;
}

/// Start a new season: everyone back to the start score, kills/deaths cleared. Daily stats stay.
void woestats_rank_reset() {
	woestats_flush(); // save pending daily stats with the old scores first

	woe_rank_db.clear();
	woe_rank_sorted.clear();
	woe_rank_pos.clear();
	woe_last_kill.clear();
	woe_rank_sort_dirty = true;

	if (SQL_ERROR == Sql_Query(mmysql_handle, "DELETE FROM `%s`", WOE_RANK_TABLE))
		Sql_ShowDebug(mmysql_handle);

	ShowStatus("WoE Stats: ranking reset.\n");
}

static void woestats_rank_load() {
	woe_rank_db.clear();

	if (SQL_ERROR == Sql_Query(mmysql_handle,
		"SELECT `char_id`, `name`, `guild_id`, `class`, `score`, `kill_count`, `death_count` FROM `%s`", WOE_RANK_TABLE)) {
		Sql_ShowDebug(mmysql_handle);
		ShowError("WoE Stats: could not load `%s`. Did you run sql-files/upgrades/upgrade_20261104_woe_stats.sql?\n", WOE_RANK_TABLE);
		return;
	}

	while (SQL_SUCCESS == Sql_NextRow(mmysql_handle)) {
		char* data;
		s_woe_rank entry = {};

		Sql_GetData(mmysql_handle, 0, &data, nullptr); entry.char_id = strtoul(data, nullptr, 10);
		Sql_GetData(mmysql_handle, 1, &data, nullptr); safestrncpy(entry.name, data ? data : "", sizeof(entry.name));
		Sql_GetData(mmysql_handle, 2, &data, nullptr); entry.guild_id = atoi(data);
		Sql_GetData(mmysql_handle, 3, &data, nullptr); entry.class_ = atoi(data);
		Sql_GetData(mmysql_handle, 4, &data, nullptr); entry.score = atoi(data);
		Sql_GetData(mmysql_handle, 5, &data, nullptr); entry.kills = strtoul(data, nullptr, 10);
		Sql_GetData(mmysql_handle, 6, &data, nullptr); entry.deaths = strtoul(data, nullptr, 10);

		woe_rank_db[entry.char_id] = entry;
	}
	Sql_FreeResult(mmysql_handle);

	woe_rank_sort_dirty = true;
	ShowStatus("WoE Stats: loaded '" CL_WHITE "%" PRIuPTR CL_RESET "' ranked characters.\n", woe_rank_db.size());
}

/*==========================================
 * Saving
 *------------------------------------------*/

static void woestats_date_str(int32 date, char* out, size_t len) {
	safesnprintf(out, len, "%04d-%02d-%02d", date / 10000, (date / 100) % 100, date % 100);
}

void woestats_flush() {
	char date[11];

	for (const auto& it : woe_char_pending) {
		const s_woe_day& day = it.second;
		s_woe_rank* rank = woestats_rank_get(it.first.first);
		StringBuf buf;

		woestats_date_str(it.first.second, date, sizeof(date));

		StringBuf_Init(&buf);
		StringBuf_Printf(&buf, "INSERT INTO `%s` (`char_id`, `date`, `guild_id`, `score`, `top_damage`", WOE_CHAR_TABLE);
		for (int32 i = 0; i < WOESTAT_MAX; i++)
			StringBuf_Printf(&buf, ", `%s`", woestat_columns[i]);
		StringBuf_Printf(&buf, ") VALUES ('%u', '%s', '%d', '%d', '%d'", it.first.first, date, day.guild_id,
			rank != nullptr ? rank->score : battle_config.woe_rank_start_score, day.top_damage);
		for (int32 i = 0; i < WOESTAT_MAX; i++)
			StringBuf_Printf(&buf, ", '%" PRId64 "'", day.val[i]);
		StringBuf_Printf(&buf, ") ON DUPLICATE KEY UPDATE `guild_id` = VALUES(`guild_id`), `score` = VALUES(`score`), `top_damage` = GREATEST(`top_damage`, VALUES(`top_damage`))");
		for (int32 i = 0; i < WOESTAT_MAX; i++)
			StringBuf_Printf(&buf, ", `%s` = `%s` + VALUES(`%s`)", woestat_columns[i], woestat_columns[i], woestat_columns[i]);

		if (SQL_ERROR == Sql_QueryStr(mmysql_handle, StringBuf_Value(&buf)))
			Sql_ShowDebug(mmysql_handle);
	}
	woe_char_pending.clear();

	for (const auto& it : woe_guild_pending) {
		woestats_date_str(it.first.second, date, sizeof(date));

		if (SQL_ERROR == Sql_Query(mmysql_handle,
			"INSERT INTO `%s` (`guild_id`, `date`, `kill_count`, `death_count`) VALUES ('%d', '%s', '%u', '%u') "
			"ON DUPLICATE KEY UPDATE `kill_count` = `kill_count` + VALUES(`kill_count`), `death_count` = `death_count` + VALUES(`death_count`)",
			WOE_GUILD_TABLE, it.first.first, date, it.second.kills, it.second.deaths))
			Sql_ShowDebug(mmysql_handle);
	}
	woe_guild_pending.clear();

	for (auto& it : woe_rank_db) {
		s_woe_rank& rank = it.second;

		if (!rank.dirty)
			continue;

		char esc_name[NAME_LENGTH * 2 + 1];

		Sql_EscapeStringLen(mmysql_handle, esc_name, rank.name, strnlen(rank.name, NAME_LENGTH));

		if (SQL_ERROR == Sql_Query(mmysql_handle,
			"REPLACE INTO `%s` (`char_id`, `name`, `guild_id`, `class`, `score`, `kill_count`, `death_count`, `last_update`) "
			"VALUES ('%u', '%s', '%d', '%d', '%d', '%u', '%u', NOW())",
			WOE_RANK_TABLE, rank.char_id, esc_name, rank.guild_id, rank.class_, rank.score, rank.kills, rank.deaths))
			Sql_ShowDebug(mmysql_handle);
		else
			rank.dirty = false;
	}

	// Forget old repeat-kill entries
	t_tick now = gettick();
	for (auto it = woe_last_kill.begin(); it != woe_last_kill.end(); ) {
		if (DIFF_TICK(now, it->second) >= battle_config.woe_rank_repeat_time * 1000)
			it = woe_last_kill.erase(it);
		else
			++it;
	}
}

static TIMER_FUNC(woestats_flush_timer) {
	woestats_flush();
	return 0;
}

/// Adds active time to every character standing on an active WoE map who isn't idle
static TIMER_FUNC(woestats_time_timer) {
	if (!battle_config.woe_stats_enable || !is_agit_start())
		return 0;

	s_mapiterator* iter = mapit_getallusers();

	for (map_session_data* sd = (map_session_data*)mapit_first(iter); mapit_exists(iter); sd = (map_session_data*)mapit_next(iter)) {
		if (sd->state.autotrade || sd->chatID || sd->state.vending)
			continue;
		if (DIFF_TICK(last_tick, sd->idletime) >= battle_config.woe_stats_idle_time)
			continue; // AFK
		woestats_add(sd, WOESTAT_ACTIVE_TIME, WOESTATS_TIME_INTERVAL);
	}
	mapit_free(iter);

	return 0;
}

void do_init_woestats() {
	add_timer_func_list(woestats_flush_timer, "woestats_flush_timer");
	add_timer_func_list(woestats_time_timer, "woestats_time_timer");
	woestats_time_tid = add_timer_interval(gettick() + WOESTATS_TIME_INTERVAL * 1000, woestats_time_timer, 0, 0, WOESTATS_TIME_INTERVAL * 1000);

	woestats_rank_load();

	int32 interval = std::max(battle_config.woe_stats_save_interval, 10) * 1000;

	woestats_flush_tid = add_timer_interval(gettick() + interval, woestats_flush_timer, 0, 0, interval);
}

void do_final_woestats() {
	if (woestats_flush_tid != INVALID_TIMER) {
		delete_timer(woestats_flush_tid, woestats_flush_timer);
		woestats_flush_tid = INVALID_TIMER;
	}
	if (woestats_time_tid != INVALID_TIMER) {
		delete_timer(woestats_time_tid, woestats_time_timer);
		woestats_time_tid = INVALID_TIMER;
	}

	woestats_flush();

	woe_char_pending.clear();
	woe_guild_pending.clear();
	woe_rank_db.clear();
	woe_rank_sorted.clear();
	woe_rank_pos.clear();
	woe_last_kill.clear();
}
