
-------------------------------------------------------------------------------------------------

Gepard Shield has new feature which informs about using cheats/bots by players. 

Gepard Shield saves reports to "gepard_report_log" table of DB. 

This table will be created automatically.

-------------------------------------------------------------------------------------------------
Create file color_nicks.txt in "db" directory with content:

// Color Nicks by Functor

// Group ID, Text Color, Shadow Color

1, 0x4B2D20, 0xFAC79B
2, 0xABC5EE, 0x670DF4
3, 0x4B2D20, 0xFDD154
4, 0x6E5EF2, 0x16F949

-------------------------------------------------------------------------------------------------

New commands:

!ping - shows statistic of connection

@lgp - Enables/Disables LGP.
@square <on/off/1-14> - Display square around the character. It is possible to change the size.
@circle - Displays circle around the character.
@aoes - Displays colored areas of skills: Storm Gust, Lord of Vermillion, Meteor Storm.
@shake - Enables/Disables "shake" effect

@auras
@show_auras

@colornick
@reload_color_db

-------------------------------------------------------------------------------------------------

New script commands:

set_aura(aura id for type number 1, aura id for type number 2, aura id for type number 3);
set_aura_part(type number, aura id);
get_aura_part(type number);

set_color_nick(color group id);
get_color_nick();

------------------------------------------------------------------------------------------------------------------------------------------------------------------
Blocking PC by using unique_id:

Examples:

m - minutes
h - hours
d - days

usage: @gepard_block_nick <duration> <duration_type m/h/d> "<char name>" <reason>
@gepard_block_nick 5 m "Functor" Cheater

usage: @gepard_block_account_id <duration> <duration_type m/h/d> <account ID> <reason>
@gepard_block_account_id 5 d 2000001 Cheater

usage: @gepard_block_unique_id <duration> <duration_type m/h/d> <unique ID> <reason>
@gepard_block_unique_id 7 h 2343543253 Cheater

usage: @gepard_unblock_nick <char name>
@gepard_unblock_nick "Functor"

usage: @gepard_unblock_account_id <account ID>
@gepard_unblock_account_id 2000001

usage: @gepard_unblock_unique_id <unique ID>
@gepard_unblock_unique_id 2343543253
