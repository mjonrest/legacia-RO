// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

// Legacia voice chat: link between the map-server and the voice relay
// (tools/voice-server). Protocol is described in doc/voice_chat.md

#ifndef VOICE_HPP
#define VOICE_HPP

#include <common/cbasetypes.hpp>

class map_session_data;

bool voice_config_read( const char* w1, const char* w2 );
void voice_player_online( map_session_data& sd );
void voice_player_quit( map_session_data& sd );
void clif_parse_voice_claim( int32 fd, map_session_data* sd );
const char* voice_command( map_session_data& sd, const char* message );

void do_init_voice();
void do_final_voice();

#endif /* VOICE_HPP */
