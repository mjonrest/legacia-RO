// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "voice.hpp"

#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

#include <common/cbasetypes.hpp>
#include <common/showmsg.hpp>
#include <common/socket.hpp>
#include <common/strlib.hpp>
#include <common/timer.hpp>

#include "atcommand.hpp"
#include "battle.hpp"
#include "clif.hpp"
#include "map.hpp"
#include "packets.hpp"
#include "pc.hpp"
#include "skill.hpp"

// map-server <-> voice server link packets. Every packet starts with cmd(2) len(2).
enum e_voice_link_cmd : uint16 {
	VOICE_LINK_HELLO = 0x7600,     // map -> voice: secret[32]
	VOICE_LINK_PLAYER = 0x7601,    // map -> voice: insert or update one player
	VOICE_LINK_LEAVE = 0x7602,     // map -> voice: char_id(4)
	VOICE_LINK_PING = 0x7603,      // map -> voice
	VOICE_LINK_CLAIM = 0x7604,     // map -> voice: code[16] char_id(4)
	VOICE_LINK_CLAIM_SHORT = 0x7605, // map -> voice: short_code[8] char_id(4), from @voice
	VOICE_LINK_ONLINE = 0x7606,    // map -> voice: ip[4], a player logged in from this address
	VOICE_LINK_CLAIM_AUTO = 0x7607, // map -> voice: short_code[8] char_id(4), answer to AUTO_QUERY
	VOICE_LINK_STATS = 0x7608,     // map -> voice: char_id(4) page(1) text, answer to STATS_QUERY
	VOICE_LINK_CELLS = 0x7609,     // map -> voice: char_id(4) count(2) count * {x(2) y(2) skill_id(2) kind(1)}, answer to CELLS_QUERY
	VOICE_LINK_AURAS = 0x760a,     // map -> voice: char_id(4) count(2) count * {account_id(4) part1(1) part2(1) part3(1)}, answer to AURAS_QUERY
	VOICE_LINK_CLAIM_IDENT = 0x760b, // map -> voice: short_code[8] char_id(4), answer to IDENT_QUERY when the login matched
	VOICE_LINK_SNAP = 0x760c,      // map -> voice: ticket(4) page(1) text, answer to SNAP_QUERY; page VOICE_SNAP_DENIED = no such login in game
	VOICE_LINK_HELLO_ACK = 0x7680, // voice -> map: result(1), 0 = ok
	VOICE_LINK_PONG = 0x7681,      // voice -> map
	VOICE_LINK_CLAIM_RESULT = 0x7682, // voice -> map: char_id(4) result(1), answer to CLAIM_SHORT
	VOICE_LINK_AUTO_QUERY = 0x7683, // voice -> map: ip[4] short_code[8], a voice client from this address wants a code claimed
	VOICE_LINK_STATS_QUERY = 0x7684, // voice -> map: char_id(4) page(1), the plugin's Player Statistics window wants a page
	VOICE_LINK_CELLS_QUERY = 0x7685, // voice -> map: char_id(4), the plugin's LGP wants the skill cells around the player
	VOICE_LINK_IDENT_QUERY = 0x7687, // voice -> map: account_id(4) char_id(4) login_id1(4) short_code[8], the plugin read the game's login
	VOICE_LINK_AURAS_QUERY = 0x7686, // voice -> map: char_id(4), the plugin wants the auras of the players around (it draws them)
	VOICE_LINK_SNAP_QUERY = 0x7688, // voice -> map: ticket(4) account_id(4) char_id(4) login_id1(4), every Player Statistics page at once, no voice sign-in needed
};

// Sent after the secret in HELLO. Older voice servers ignore it; newer ones
// only send packets the map-server said it understands.
enum e_voice_feature : uint8 {
	VOICE_FEATURE_STATS = 0x01, // answers STATS_QUERY
	VOICE_FEATURE_CELLS = 0x02, // answers CELLS_QUERY
	VOICE_FEATURE_AURAS = 0x04, // answers AURAS_QUERY
	VOICE_FEATURE_IDENT = 0x08, // answers IDENT_QUERY
	VOICE_FEATURE_SNAP = 0x10,  // answers SNAP_QUERY
};

enum e_voice_flag : uint8 {
	VOICE_FLAG_MUTED = 0x01,      // chat-banned players can listen but not speak
	VOICE_FLAG_NO_WHISPER = 0x02, // player blocks all whispers (/exall), voice whispers too
};

static const int32 VOICE_CODE_LENGTH = 16;
static const int32 VOICE_SECRET_LENGTH = 32;
static const int32 VOICE_MAP_LENGTH = 16;
static const int32 VOICE_PLAYER_PACKET_LENGTH = 4 + 4 * 4 + 2 * 2 + 1 + VOICE_MAP_LENGTH + NAME_LENGTH + 4;
static const int32 VOICE_CLAIM_PACKET_LENGTH = 4 + VOICE_CODE_LENGTH + 4;
static const int32 VOICE_SHORT_CODE_LENGTH = 8; // codes shown to players are 6 characters, zero padded
static const int32 VOICE_CLAIM_SHORT_PACKET_LENGTH = 4 + VOICE_SHORT_CODE_LENGTH + 4;
static const t_tick VOICE_CLAIM_INTERVAL = 1000;  // per character, for plugin claims
static const t_tick VOICE_COMMAND_INTERVAL = 3000; // per character, for @voice
static const t_tick VOICE_SNAP_INTERVAL = 1500;    // per character, for a whole Player Statistics snapshot
static const uint8 VOICE_SNAP_DENIED = 0xff;
static const t_tick VOICE_STATS_INTERVAL = 250;    // per character, for the Player Statistics window
static const size_t VOICE_STATS_MAX_TEXT = 6000;   // the voice server splits it into UDP packets
static const t_tick VOICE_CELLS_INTERVAL = 250;    // per character, for LGP skill cells
static const size_t VOICE_CELLS_MAX = 200;         // cells per answer, so it fits in one UDP packet
static const size_t VOICE_CELL_SIZE = 7;           // x(2) y(2) skill_id(2) kind(1)
static const t_tick VOICE_AURAS_INTERVAL = 400;    // per character, for the auras the plugin draws
static const size_t VOICE_AURAS_MAX = 200;         // auras per answer, so it fits in one UDP packet
static const size_t VOICE_AURA_SIZE = 7;           // account_id(4) part1(1) part2(1) part3(1)

struct s_voice_player {
	uint32 account_id;
	uint32 party_id;
	uint32 guild_id;
	uint16 x;
	uint16 y;
	uint8 flags;
	char map[VOICE_MAP_LENGTH];
	uint32 chat_id; // chat room the player sits in, 0 = none
	bool synced; // last state was sent to the voice server
	t_tick last_claim;
	t_tick last_stats;
	t_tick last_cells;
	t_tick last_auras;
};

static bool voice_enabled = false;
static char voice_ip_str[128] = "127.0.0.1";
static uint32 voice_ip = 0;
static uint16 voice_port = 6950;
static char voice_secret[VOICE_SECRET_LENGTH + 1] = "";

static int32 voice_fd = -1;
static bool voice_ready = false; // link is up and the voice server accepted our secret

// char_id -> player that linked a voice client during this login
static std::unordered_map<uint32, s_voice_player> voice_players;

bool voice_config_read( const char* w1, const char* w2 ){
	if( strcmpi( w1, "voice_enable" ) == 0 )
		voice_enabled = config_switch( w2 ) != 0;
	else if( strcmpi( w1, "voice_ip" ) == 0 )
		safestrncpy( voice_ip_str, w2, sizeof( voice_ip_str ) );
	else if( strcmpi( w1, "voice_port" ) == 0 )
		voice_port = static_cast<uint16>( atoi( w2 ) );
	else if( strcmpi( w1, "voice_secret" ) == 0 )
		safestrncpy( voice_secret, w2, sizeof( voice_secret ) );
	else
		return false;

	return true;
}

/// Refresh the cached state of a player. Returns true if anything changed.
static bool voice_player_refresh( s_voice_player& player, map_session_data& sd ){
	struct map_data* mapdata = map_getmapdata( sd.m );
	char map[VOICE_MAP_LENGTH] = {};
	uint8 flags = 0;

	if( mapdata != nullptr )
		safestrncpy( map, mapdata->name, sizeof( map ) );
	if( sd.sc.cant.chat || ( sd.state.block_action & PCBLOCK_CHAT ) )
		flags |= VOICE_FLAG_MUTED;
	if( sd.state.ignoreAll )
		flags |= VOICE_FLAG_NO_WHISPER;

	bool changed = !player.synced
		|| player.party_id != static_cast<uint32>( sd.status.party_id )
		|| player.guild_id != static_cast<uint32>( sd.status.guild_id )
		|| player.x != sd.x || player.y != sd.y
		|| player.flags != flags
		|| player.chat_id != sd.chatID
		|| strncmp( player.map, map, VOICE_MAP_LENGTH ) != 0;

	player.party_id = sd.status.party_id;
	player.guild_id = sd.status.guild_id;
	player.x = sd.x;
	player.y = sd.y;
	player.flags = flags;
	player.chat_id = sd.chatID;
	memcpy( player.map, map, VOICE_MAP_LENGTH );

	return changed;
}

static void voice_send_player( uint32 char_id, s_voice_player& player, map_session_data& sd ){
	if( !voice_ready )
		return;

	int32 fd = voice_fd;

	WFIFOHEAD( fd, VOICE_PLAYER_PACKET_LENGTH );
	WFIFOW( fd, 0 ) = VOICE_LINK_PLAYER;
	WFIFOW( fd, 2 ) = VOICE_PLAYER_PACKET_LENGTH;
	WFIFOL( fd, 4 ) = player.account_id;
	WFIFOL( fd, 8 ) = char_id;
	WFIFOL( fd, 12 ) = player.party_id;
	WFIFOL( fd, 16 ) = player.guild_id;
	WFIFOW( fd, 20 ) = player.x;
	WFIFOW( fd, 22 ) = player.y;
	WFIFOB( fd, 24 ) = player.flags;
	memcpy( WFIFOP( fd, 25 ), player.map, VOICE_MAP_LENGTH );
	safestrncpy( WFIFOCP( fd, 25 + VOICE_MAP_LENGTH ), sd.status.name, NAME_LENGTH );
	WFIFOL( fd, 25 + VOICE_MAP_LENGTH + NAME_LENGTH ) = player.chat_id;
	WFIFOSET( fd, VOICE_PLAYER_PACKET_LENGTH );

	player.synced = true;
}

static void voice_send_leave( uint32 char_id ){
	if( !voice_ready )
		return;

	WFIFOHEAD( voice_fd, 8 );
	WFIFOW( voice_fd, 0 ) = VOICE_LINK_LEAVE;
	WFIFOW( voice_fd, 2 ) = 8;
	WFIFOL( voice_fd, 4 ) = char_id;
	WFIFOSET( voice_fd, 8 );
}

static void voice_send_online( uint32 ip ){
	if( !voice_ready )
		return;

	WFIFOHEAD( voice_fd, 8 );
	WFIFOW( voice_fd, 0 ) = VOICE_LINK_ONLINE;
	WFIFOW( voice_fd, 2 ) = 8;
	WFIFOB( voice_fd, 4 ) = static_cast<uint8>( ip >> 24 );
	WFIFOB( voice_fd, 5 ) = static_cast<uint8>( ip >> 16 );
	WFIFOB( voice_fd, 6 ) = static_cast<uint8>( ip >> 8 );
	WFIFOB( voice_fd, 7 ) = static_cast<uint8>( ip );
	WFIFOSET( voice_fd, 8 );
}

/// Send every player again, the voice server keeps nothing across reconnects.
static void voice_send_all(){
	for( auto it = voice_players.begin(); it != voice_players.end(); ){
		map_session_data* sd = map_charid2sd( it->first );

		if( sd == nullptr ){
			it = voice_players.erase( it );
			continue;
		}

		it->second.synced = false;
		voice_player_refresh( it->second, *sd );
		voice_send_player( it->first, it->second, *sd );
		++it;
	}

	// voice clients that started while the link was down
	s_mapiterator* iter = mapit_getallusers();

	for( map_session_data* sd = (map_session_data*)mapit_first( iter ); mapit_exists( iter ); sd = (map_session_data*)mapit_next( iter ) ){
		if( !sd->state.autotrade && session_isActive( sd->fd ) )
			voice_send_online( session[sd->fd]->client_addr );
	}
	mapit_free( iter );
}

void voice_player_quit( map_session_data& sd ){
	if( voice_players.erase( sd.status.char_id ) > 0 )
		voice_send_leave( sd.status.char_id );
}

/// Called once per login. A voice client from the same address may be
/// waiting, so the voice server can link it without the player doing anything.
void voice_player_online( map_session_data& sd ){
	if( voice_enabled && session_isActive( sd.fd ) )
		voice_send_online( session[sd.fd]->client_addr );
}

/// Start tracking a player for a claim and send their state to the voice server.
/// Returns nullptr when the character claimed less than interval ago.
static s_voice_player* voice_player_for_claim( map_session_data& sd, t_tick interval ){
	t_tick tick = gettick();
	auto it = voice_players.find( sd.status.char_id );

	if( it == voice_players.end() ){
		s_voice_player player = {};

		player.account_id = sd.status.account_id;
		player.last_claim = tick - interval;
		it = voice_players.emplace( sd.status.char_id, player ).first;
	}

	s_voice_player& player = it->second;

	if( DIFF_TICK( tick, player.last_claim ) < interval )
		return nullptr; // flood protection

	player.last_claim = tick;
	player.synced = false;
	voice_player_refresh( player, sd );
	voice_send_player( sd.status.char_id, player, sd );
	return &player;
}

static void voice_send_claim_short( uint16 cmd, const char* code, uint32 char_id ){
	WFIFOHEAD( voice_fd, VOICE_CLAIM_SHORT_PACKET_LENGTH );
	WFIFOW( voice_fd, 0 ) = cmd;
	WFIFOW( voice_fd, 2 ) = VOICE_CLAIM_SHORT_PACKET_LENGTH;
	memcpy( WFIFOP( voice_fd, 4 ), code, VOICE_SHORT_CODE_LENGTH );
	WFIFOL( voice_fd, 4 + VOICE_SHORT_CODE_LENGTH ) = char_id;
	WFIFOSET( voice_fd, VOICE_CLAIM_SHORT_PACKET_LENGTH );
}

/// AUTO_QUERY: a voice client is waiting at this address. When exactly one
/// player is playing from it, that must be them, so claim the code for them.
/// Shared addresses (several players, or a second client) need @voice.
static void voice_auto_query( uint32 ip, const char* code ){
	map_session_data* found = nullptr;
	int32 count = 0;
	s_mapiterator* iter = mapit_getallusers();

	for( map_session_data* sd = (map_session_data*)mapit_first( iter ); mapit_exists( iter ); sd = (map_session_data*)mapit_next( iter ) ){
		// vendors left on @autotrade have no client
		if( sd->state.autotrade || !session_isActive( sd->fd ) || session[sd->fd]->client_addr != ip )
			continue;
		found = sd;
		if( ++count > 1 )
			break;
	}
	mapit_free( iter );

	if( count != 1 || voice_player_for_claim( *found, VOICE_CLAIM_INTERVAL ) == nullptr )
		return;

	voice_send_claim_short( VOICE_LINK_CLAIM_AUTO, code, found->status.char_id );
}

/// IDENT_QUERY: the plugin read the account, character and login code
/// (login_id1, which only this client and the servers know) from the game
/// client and sent them to the voice server. When they match a player in
/// game, the code is theirs. Works on shared addresses, nothing to type.
static void voice_ident_query( uint32 account_id, uint32 char_id, uint32 login_id1, const char* code ){
	map_session_data* sd = map_charid2sd( char_id );

	if( sd == nullptr || sd->state.autotrade || !session_isActive( sd->fd ) )
		return;
	if( static_cast<uint32>( sd->status.account_id ) != account_id || sd->login_id1 != login_id1 )
		return;
	if( voice_player_for_claim( *sd, VOICE_CLAIM_INTERVAL ) == nullptr )
		return;

	voice_send_claim_short( VOICE_LINK_CLAIM_IDENT, code, sd->status.char_id );
}

/// @voice <code>: links the voice plugin to this character with the code the
/// plugin shows. Works with any client, since nothing but chat is needed.
/// Returns the message to show the player.
const char* voice_command( map_session_data& sd, const char* message ){
	if( !voice_enabled || !voice_ready )
		return "Voice chat is not available right now.";

	char code[VOICE_SHORT_CODE_LENGTH] = {};
	int32 len = 0;

	for( const char* c = message; c != nullptr && *c != '\0'; c++ ){
		if( ISSPACE( *c ) )
			continue;
		if( !ISALNUM( *c ) || len >= 6 )
			return "Usage: @voice <code>, with the 6 letter code shown in the voice bar.";
		code[len++] = static_cast<char>( TOUPPER( *c ) );
	}
	if( len != 6 )
		return "Usage: @voice <code>, with the 6 letter code shown in the voice bar.";

	if( voice_player_for_claim( sd, VOICE_COMMAND_INTERVAL ) == nullptr )
		return "Please wait a few seconds before trying again.";

	voice_send_claim_short( VOICE_LINK_CLAIM_SHORT, code, sd.status.char_id );

	return "Linking voice chat...";
}

/// CZ_VOICE_CLAIM, sent by the voice client plugin through the game connection.
/// The plugin got the code from the voice server over UDP; passing it on proves
/// to the voice server which character that UDP endpoint belongs to.
void clif_parse_voice_claim( int32 fd, map_session_data* sd ){
	if( !voice_enabled || !voice_ready )
		return;

	const PACKET_CZ_VOICE_CLAIM* p = reinterpret_cast<PACKET_CZ_VOICE_CLAIM*>( RFIFOP( fd, 0 ) );

	if( voice_player_for_claim( *sd, VOICE_CLAIM_INTERVAL ) == nullptr )
		return;

	WFIFOHEAD( voice_fd, VOICE_CLAIM_PACKET_LENGTH );
	WFIFOW( voice_fd, 0 ) = VOICE_LINK_CLAIM;
	WFIFOW( voice_fd, 2 ) = VOICE_CLAIM_PACKET_LENGTH;
	memcpy( WFIFOP( voice_fd, 4 ), p->code, VOICE_CODE_LENGTH );
	WFIFOL( voice_fd, 4 + VOICE_CODE_LENGTH ) = sd->status.char_id;
	WFIFOSET( voice_fd, VOICE_CLAIM_PACKET_LENGTH );
}

static void voice_on_disconnect(){
	if( voice_ready )
		ShowWarning( "Connection to voice server lost.\n" );

	voice_fd = -1;
	voice_ready = false;

	for( auto& entry : voice_players )
		entry.second.synced = false;
}

// the tabs of the plugin's Player Statistics window, in order (@statsview sections)
static const char* const voice_stats_pages[] = { "overview", "attack", "magic", "defense", "skill", "heal", "effect", "drop" };

/// The text of one Player Statistics page, the same as the @statsview section.
static std::string voice_stats_text( map_session_data& sd, uint8 page ){
	std::vector<std::string> lines;

	statsview_page_lines( &sd, voice_stats_pages[page], lines );

	std::string text;

	for( const std::string& line : lines ){
		if( text.length() + line.length() + 1 > VOICE_STATS_MAX_TEXT ){
			text += "^777777(more in @statsview)^000000";
			break;
		}
		text += line;
		text += '\n';
	}

	return text;
}

/// Player Statistics window in the plugin: same text as the @statsview sections.
/// Only answers for characters whose own voice client signed in this login,
/// so nobody can read another player's stats.
static void voice_stats_query( uint32 char_id, uint8 page ){
	auto it = voice_players.find( char_id );
	map_session_data* sd = map_charid2sd( char_id );

	if( it == voice_players.end() || sd == nullptr || page >= ARRAYLENGTH( voice_stats_pages ) )
		return;

	t_tick tick = gettick();

	if( DIFF_TICK( tick, it->second.last_stats ) < VOICE_STATS_INTERVAL )
		return;
	it->second.last_stats = tick;

	std::string text = voice_stats_text( *sd, page );
	size_t len = 4 + 4 + 1 + text.length();

	WFIFOHEAD( voice_fd, len );
	WFIFOW( voice_fd, 0 ) = VOICE_LINK_STATS;
	WFIFOW( voice_fd, 2 ) = static_cast<uint16>( len );
	WFIFOL( voice_fd, 4 ) = char_id;
	WFIFOB( voice_fd, 8 ) = page;
	memcpy( WFIFOP( voice_fd, 9 ), text.data(), text.length() );
	WFIFOSET( voice_fd, len );
}

static void voice_send_snap_page( uint32 ticket, uint8 page, const std::string& text ){
	size_t len = 4 + 4 + 1 + text.length();

	WFIFOHEAD( voice_fd, len );
	WFIFOW( voice_fd, 0 ) = VOICE_LINK_SNAP;
	WFIFOW( voice_fd, 2 ) = static_cast<uint16>( len );
	WFIFOL( voice_fd, 4 ) = ticket;
	WFIFOB( voice_fd, 8 ) = page;
	memcpy( WFIFOP( voice_fd, 9 ), text.data(), text.length() );
	WFIFOSET( voice_fd, len );
}

/// SNAP_QUERY: the plugin's Player Statistics window was opened and wants
/// every page once, as a snapshot. Like IDENT_QUERY, the account, character
/// and login code (login_id1) the plugin read from the game client prove
/// whose statistics they are, so no voice sign-in is needed. Wrong logins get
/// a VOICE_SNAP_DENIED answer, so the window can tell the player.
static void voice_snap_query( uint32 ticket, uint32 account_id, uint32 char_id, uint32 login_id1 ){
	// per character, so a plugin asking again can't make us build every page over and over
	static std::unordered_map<uint32, t_tick> last_snap;

	map_session_data* sd = map_charid2sd( char_id );

	if( sd == nullptr || sd->state.autotrade || !session_isActive( sd->fd ) ||
		static_cast<uint32>( sd->status.account_id ) != account_id || sd->login_id1 != login_id1 ){
		voice_send_snap_page( ticket, VOICE_SNAP_DENIED, "" );
		return;
	}

	t_tick tick = gettick();
	auto it = last_snap.find( char_id );

	if( it != last_snap.end() && DIFF_TICK( tick, it->second ) < VOICE_SNAP_INTERVAL )
		return;
	if( last_snap.size() > 4096 ){
		for( auto e = last_snap.begin(); e != last_snap.end(); ){
			if( DIFF_TICK( tick, e->second ) >= VOICE_SNAP_INTERVAL )
				e = last_snap.erase( e );
			else
				++e;
		}
	}
	last_snap[char_id] = tick;

	for( uint8 page = 0; page < ARRAYLENGTH( voice_stats_pages ); page++ )
		voice_send_snap_page( ticket, page, voice_stats_text( *sd, page ) );
}

/// One skill cell near the player, for voice_cells_query.
static int32 voice_cells_sub( block_list* bl, va_list ap ){
	skill_unit* unit = reinterpret_cast<skill_unit*>( bl );
	map_session_data* sd = va_arg( ap, map_session_data* );
	std::vector<uint8>* out = va_arg( ap, std::vector<uint8>* );

	if( out->size() >= VOICE_CELLS_MAX * VOICE_CELL_SIZE || !unit->alive || unit->group == nullptr || unit->group->state.guildaura )
		return 0;

	// hidden traps stay hidden, the same way the game client is told about them
	if( unit->hidden ){
		block_list* src = battle_get_master( unit );

		if( src != sd ){
			uint32 party1 = src != nullptr ? status_get_party_id( src ) : 0;
			uint32 party2 = sd->status.party_id;

			if( party1 == 0 || party2 == 0 || party1 != party2 )
				return 0;
		}
	}

	uint16 skill_id = unit->group->skill_id;
	// 1 = it can hit this player, 0 = it can't (own or friendly skills)
	uint8 kind = battle_check_target( unit, sd, unit->group->target_flag ) > 0 && battle_check_target( unit, sd, BCT_ENEMY ) > 0 ? 1 : 0;
	uint8 entry[VOICE_CELL_SIZE] = {
		static_cast<uint8>( unit->x & 0xff ), static_cast<uint8>( unit->x >> 8 ),
		static_cast<uint8>( unit->y & 0xff ), static_cast<uint8>( unit->y >> 8 ),
		static_cast<uint8>( skill_id & 0xff ), static_cast<uint8>( skill_id >> 8 ),
		kind,
	};

	out->insert( out->end(), entry, entry + VOICE_CELL_SIZE );
	return 1;
}

/// LGP in the plugin: the skill cells on the ground around the player.
/// Only answers for characters whose own voice client signed in this login.
static void voice_cells_query( uint32 char_id ){
	auto it = voice_players.find( char_id );
	map_session_data* sd = map_charid2sd( char_id );

	if( it == voice_players.end() || sd == nullptr || sd->prev == nullptr )
		return;

	t_tick tick = gettick();

	if( DIFF_TICK( tick, it->second.last_cells ) < VOICE_CELLS_INTERVAL )
		return;
	it->second.last_cells = tick;

	std::vector<uint8> cells;

	map_foreachinallarea( voice_cells_sub, sd->m, sd->x - AREA_SIZE, sd->y - AREA_SIZE,
		sd->x + AREA_SIZE, sd->y + AREA_SIZE, BL_SKILL, sd, &cells );

	size_t count = cells.size() / VOICE_CELL_SIZE;
	size_t len = 4 + 4 + 2 + cells.size();

	WFIFOHEAD( voice_fd, len );
	WFIFOW( voice_fd, 0 ) = VOICE_LINK_CELLS;
	WFIFOW( voice_fd, 2 ) = static_cast<uint16>( len );
	WFIFOL( voice_fd, 4 ) = char_id;
	WFIFOW( voice_fd, 8 ) = static_cast<uint16>( count );
	if( !cells.empty() )
		memcpy( WFIFOP( voice_fd, 10 ), cells.data(), cells.size() );
	WFIFOSET( voice_fd, len );
}

/// One player's aura near the viewer, for voice_auras_query.
static int32 voice_auras_sub( block_list* bl, va_list ap ){
	map_session_data* tsd = reinterpret_cast<map_session_data*>( bl );
	map_session_data* sd = va_arg( ap, map_session_data* );
	std::vector<uint8>* out = va_arg( ap, std::vector<uint8>* );

	if( out->size() >= VOICE_AURAS_MAX * VOICE_AURA_SIZE || tsd->aura_data <= 0x1000000 )
		return 0;
	if( tsd != sd ){
		// @show_auras 1: only your own aura
		if( sd->state.show_auras == 1 )
			return 0;
		// players the viewer can't see keep their aura hidden too (same rule as pc_aura_apply)
		if( pc_isinvisible( tsd ) || tsd->sc.getSCE( SC_HIDING ) || tsd->sc.getSCE( SC_CLOAKING ) || tsd->sc.getSCE( SC_CHASEWALK ) || tsd->sc.getSCE( SC_CAMOUFLAGE ) )
			return 0;
	}

	uint32 aid = tsd->status.account_id;
	uint8 entry[VOICE_AURA_SIZE] = {
		static_cast<uint8>( aid & 0xff ), static_cast<uint8>( ( aid >> 8 ) & 0xff ),
		static_cast<uint8>( ( aid >> 16 ) & 0xff ), static_cast<uint8>( aid >> 24 ),
		tsd->aura.part_1, tsd->aura.part_2, tsd->aura.part_3,
	};

	out->insert( out->end(), entry, entry + VOICE_AURA_SIZE );
	return 1;
}

/// The plugin draws the auras (part 1 ground circle, part 2 rings, part 3
/// sparkles) of the players around, so it asks for them about once a second.
/// Only answers for characters whose own voice client signed in this login.
static void voice_auras_query( uint32 char_id ){
	auto it = voice_players.find( char_id );
	map_session_data* sd = map_charid2sd( char_id );

	t_tick tick = gettick();

	if( it == voice_players.end() || sd == nullptr || sd->prev == nullptr ){
		static t_tick last_warn;

		if( DIFF_TICK( tick, last_warn ) > 60000 ){
			last_warn = tick;
			ShowWarning( "voice: auras asked for char %u, but %s.\n", char_id,
				sd == nullptr ? "that char is not online" : it == voice_players.end() ? "its voice client did not sign in" : "it is not on a map" );
		}
		return;
	}

	if( DIFF_TICK( tick, it->second.last_auras ) < VOICE_AURAS_INTERVAL )
		return;
	it->second.last_auras = tick;

	std::vector<uint8> auras;

	// @show_auras 2: no auras at all
	if( sd->state.show_auras != 2 )
		map_foreachinallarea( voice_auras_sub, sd->m, sd->x - AREA_SIZE, sd->y - AREA_SIZE,
			sd->x + AREA_SIZE, sd->y + AREA_SIZE, BL_PC, sd, &auras );

	size_t count = auras.size() / VOICE_AURA_SIZE;
	size_t len = 4 + 4 + 2 + auras.size();

	WFIFOHEAD( voice_fd, len );
	WFIFOW( voice_fd, 0 ) = VOICE_LINK_AURAS;
	WFIFOW( voice_fd, 2 ) = static_cast<uint16>( len );
	WFIFOL( voice_fd, 4 ) = char_id;
	WFIFOW( voice_fd, 8 ) = static_cast<uint16>( count );
	if( !auras.empty() )
		memcpy( WFIFOP( voice_fd, 10 ), auras.data(), auras.size() );
	WFIFOSET( voice_fd, len );
}

static int32 voice_parse( int32 fd ){
	if( fd != voice_fd ){
		do_close( fd );
		return 0;
	}

	if( session[fd]->flag.eof ){
		do_close( fd );
		voice_on_disconnect();
		return 0;
	}else if( session[fd]->flag.ping ){
		if( DIFF_TICK( last_tick, session[fd]->rdata_tick ) > ( stall_time * 2 ) ){
			set_eof( fd );
			return 0;
		}else if( session[fd]->flag.ping != 2 ){
			WFIFOHEAD( fd, 4 );
			WFIFOW( fd, 0 ) = VOICE_LINK_PING;
			WFIFOW( fd, 2 ) = 4;
			WFIFOSET( fd, 4 );
			session[fd]->flag.ping = 2;
		}
	}

	while( RFIFOREST( fd ) >= 4 ){
		uint16 cmd = RFIFOW( fd, 0 );
		uint16 len = RFIFOW( fd, 2 );

		if( len < 4 ){
			set_eof( fd );
			return 0;
		}
		if( RFIFOREST( fd ) < len )
			return 0;

		switch( cmd ){
			case VOICE_LINK_HELLO_ACK:
				if( len < 5 || RFIFOB( fd, 4 ) != 0 ){
					ShowError( "Voice server refused the connection, check voice_secret.\n" );
					set_eof( fd );
					return 0;
				}
				voice_ready = true;
				ShowStatus( "Connected to voice server at '" CL_WHITE "%s:%d" CL_RESET "'.\n", voice_ip_str, voice_port );
				voice_send_all();
				break;
			case VOICE_LINK_PONG:
				// the link answered: ping again after the next idle stall_time
				// (left at 2, the link was closed 2 * stall_time later)
				session[fd]->flag.ping = 0;
				break;
			case VOICE_LINK_CLAIM_RESULT: {
				if( len < 9 )
					break;
				map_session_data* sd = map_charid2sd( RFIFOL( fd, 4 ) );

				if( sd != nullptr )
					clif_displaymessage( sd->fd, RFIFOB( fd, 8 ) == 0 ? "Voice chat connected."
						: "That voice code is wrong or expired. Type the code shown in the voice bar." );
				break;
			}
			case VOICE_LINK_AUTO_QUERY: {
				if( len < 4 + 4 + VOICE_SHORT_CODE_LENGTH )
					break;
				uint32 ip = ( RFIFOB( fd, 4 ) << 24 ) | ( RFIFOB( fd, 5 ) << 16 ) | ( RFIFOB( fd, 6 ) << 8 ) | RFIFOB( fd, 7 );
				char code[VOICE_SHORT_CODE_LENGTH] = {};

				memcpy( code, RFIFOP( fd, 8 ), 6 );
				voice_auto_query( ip, code );
				break;
			}
			case VOICE_LINK_IDENT_QUERY: {
				if( len < 4 + 12 + VOICE_SHORT_CODE_LENGTH )
					break;
				char code[VOICE_SHORT_CODE_LENGTH] = {};

				memcpy( code, RFIFOP( fd, 16 ), 6 );
				voice_ident_query( RFIFOL( fd, 4 ), RFIFOL( fd, 8 ), RFIFOL( fd, 12 ), code );
				break;
			}
			case VOICE_LINK_STATS_QUERY:
				if( len < 9 )
					break;
				voice_stats_query( RFIFOL( fd, 4 ), RFIFOB( fd, 8 ) );
				break;
			case VOICE_LINK_CELLS_QUERY:
				if( len < 8 )
					break;
				voice_cells_query( RFIFOL( fd, 4 ) );
				break;
			case VOICE_LINK_SNAP_QUERY:
				if( len < 4 + 16 )
					break;
				voice_snap_query( RFIFOL( fd, 4 ), RFIFOL( fd, 8 ), RFIFOL( fd, 12 ), RFIFOL( fd, 16 ) );
				break;
			case VOICE_LINK_AURAS_QUERY:
				if( len < 8 )
					break;
				voice_auras_query( RFIFOL( fd, 4 ) );
				break;
			default:
				ShowError( "voice_parse: unknown packet 0x%04x from voice server, disconnecting.\n", cmd );
				set_eof( fd );
				return 0;
		}

		RFIFOSKIP( fd, len );
	}

	return 0;
}

static TIMER_FUNC( voice_check_connect ){
	if( !voice_enabled || ( voice_fd > 0 && session[voice_fd] != nullptr ) )
		return 0;

	voice_fd = make_connection( voice_ip, voice_port, true, 1 );

	if( voice_fd == -1 )
		return 0;

	session[voice_fd]->func_parse = voice_parse;
	session[voice_fd]->flag.server = 1;
	realloc_fifo( voice_fd, FIFOSIZE_SERVERLINK, FIFOSIZE_SERVERLINK );

	WFIFOHEAD( voice_fd, 4 + VOICE_SECRET_LENGTH + 1 );
	WFIFOW( voice_fd, 0 ) = VOICE_LINK_HELLO;
	WFIFOW( voice_fd, 2 ) = 4 + VOICE_SECRET_LENGTH + 1;
	memset( WFIFOP( voice_fd, 4 ), 0, VOICE_SECRET_LENGTH );
	memcpy( WFIFOP( voice_fd, 4 ), voice_secret, strnlen( voice_secret, VOICE_SECRET_LENGTH ) );
	WFIFOB( voice_fd, 4 + VOICE_SECRET_LENGTH ) = VOICE_FEATURE_STATS | VOICE_FEATURE_CELLS | VOICE_FEATURE_AURAS | VOICE_FEATURE_IDENT | VOICE_FEATURE_SNAP;
	WFIFOSET( voice_fd, 4 + VOICE_SECRET_LENGTH + 1 );

	return 0;
}

/// Push party, guild, chat room, map and position changes to the voice server.
static TIMER_FUNC( voice_sync_timer ){
	for( auto it = voice_players.begin(); it != voice_players.end(); ){
		map_session_data* sd = map_charid2sd( it->first );

		if( sd == nullptr ){
			voice_send_leave( it->first );
			it = voice_players.erase( it );
			continue;
		}

		if( voice_player_refresh( it->second, *sd ) )
			voice_send_player( it->first, it->second, *sd );
		++it;
	}

	return 0;
}

void do_init_voice(){
	if( !voice_enabled )
		return;

	if( strlen( voice_secret ) < 8 ){
		ShowError( "voice_enable is on but voice_secret is shorter than 8 characters, voice chat stays off.\n" );
		voice_enabled = false;
		return;
	}

	voice_ip = host2ip( voice_ip_str );

	add_timer_func_list( voice_check_connect, "voice_check_connect" );
	add_timer_func_list( voice_sync_timer, "voice_sync_timer" );
	add_timer_interval( gettick() + 1000, voice_check_connect, 0, 0, 10 * 1000 );
	add_timer_interval( gettick() + 1000, voice_sync_timer, 0, 0, 500 );
}

void do_final_voice(){
	if( voice_fd > 0 && session[voice_fd] != nullptr )
		do_close( voice_fd );

	voice_fd = -1;
	voice_ready = false;
	voice_players.clear();
}
