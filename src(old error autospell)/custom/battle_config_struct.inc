// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom battle config structure
 * Place the definition of custom battle configuration structure members in this file.
 *
 * Make sure that the name you want is not already taken by looking into battle.hpp
 * Do not forget to add initilization logic to battle_config_init.inc
 *
 * Format:
 *	 <datatype> name;
 **/

	//BattleGround JinYuichi 3.0
	//Extended Vending system [JinYuichi]
	int extended_vending;
	int show_broadcast_info;
	int show_item_vending;
	int ex_vending_info;
	int ex_vending_report;
	int ex_buying_bound;
	int item_zeny;
	int item_cash;
	int feature_goldpc_active;
	int feature_goldpc_time;
	int feature_goldpc_max_points;
	int feature_goldpc_vip;
	int afk_timeout;
	// Show Element Info [Hyroshima]
	int mob_ele_view;
	// [Athena Studio] Refine UI Announce | Contact: https://web.facebook.com/AthenaStudioINC/
	int refine_announce;
	int equipment;
	int weapon_level_1;
	int weapon_level_2;
	int weapon_level_3;
	int weapon_level_4;
	int weapon_level_5;