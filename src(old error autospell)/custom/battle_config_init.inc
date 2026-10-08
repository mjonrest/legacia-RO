// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom battle config initializations
 * Place the definition of custom battle configuration initializations in this file.
 *
 * Do not forget to add a comma at the end of the line, if you want to add another config
 *
 * Format:
 *	 { "name", &battle_config.<variable name>, <default value>, <minimum value>, <maximum value> },
 **/

//Extended Vending system [JinYuichi]
	{ "extended_vending",			&battle_config.extended_vending,				1,		0,		1,				},
	{ "show_broadcast_info",		&battle_config.show_broadcast_info,				1,		0,		1,				},
	{ "show_item_vending",			&battle_config.show_item_vending,				1,		0,		1,				},
	{ "ex_vending_info",			&battle_config.ex_vending_info,					1,		0,		1,				},
	{ "ex_vending_report",			&battle_config.ex_vending_report,				1,		0,		1,				}, // [JinYuichi]
	{ "ex_buying_bound",			&battle_config.ex_buying_bound,					1,		0,		1,				}, // [JinYuichi]
	{ "item_zeny",					&battle_config.item_zeny,						0,		0,		INT32_MAX,		},
	{ "item_cash",					&battle_config.item_cash,						0,		0,		INT32_MAX,		},
	{ "feature.goldpc_active",              &battle_config.feature_goldpc_active,           1,      0,      1,              },
	{ "feature.goldpc_time",                &battle_config.feature_goldpc_time,          3600,      0,   3600,              },
	{ "feature.goldpc_max_points",          &battle_config.feature_goldpc_max_points,     300,      0,    300,              },
	{ "feature.goldpc_vip",                 &battle_config.feature_goldpc_vip,              1,      0,      1,              },
	{ "afk_timeout",                        &battle_config.afk_timeout,                     0,      0,      INT_MAX,        },
	// Show Element Info [Hyroshima]
	{ "mob_ele_view",                       &battle_config.mob_ele_view,                    0,      0,      1,              },
	// [Athena Studio] Refine UI Announce | Contact: https://web.facebook.com/AthenaStudioINC/
	{ "refine_announce",              		&battle_config.refine_announce,           		0,      0,      1,        		},
	{ "equipment",              			&battle_config.equipment,           			0,      0,      MAX_REFINE,     },
	{ "weapon_level_1",              		&battle_config.weapon_level_1,           		0,      0,      MAX_REFINE,     },
	{ "weapon_level_2",              		&battle_config.weapon_level_2,           		0,      0,      MAX_REFINE,     },
	{ "weapon_level_3",              		&battle_config.weapon_level_3,           		0,      0,      MAX_REFINE,     },
	{ "weapon_level_4",              		&battle_config.weapon_level_4,           		0,      0,      MAX_REFINE,     },
	{ "weapon_level_5",              		&battle_config.weapon_level_5,           		0,      0,      MAX_REFINE,     },
