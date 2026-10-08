// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom Script Commands
 * Place the body of custom script commands in this file.
 * Format:
 *	BUILDIN_FUNC(command_name)
 *	{
 *		<code>;
 *	}
 **/

//BUILDIN_FUNC(example)
//{
//	ShowInfo("buildin_example: It works!\n");
//	script_pushint(st,1);
//	return 0;
//}

BUILDIN_FUNC(mobremove)
{

        struct block_list *bl = NULL;

 

        bl = map_id2bl( script_getnum(st,2) );

 

        if ( bl && bl->type == BL_MOB )

                unit_free(bl,CLR_OUTSIGHT);

 

        return SCRIPT_CMD_SUCCESS;
}

/// Returns the skill's name [Easycore]
///
/// getskillname(<skill id>)
/// getskillname("<skill name>")
BUILDIN_FUNC(getskillname)
{
	uint16 skill_id = (script_isstring(st,2) ? skill_name2id(script_getstr(st,2)) : script_getnum(st,2));
	if (!skill_get_index(skill_id))
		script_pushconststr(st, "Unknown Skill");
	else
		script_pushstrcopy(st,skill_get_desc(skill_id));
	return SCRIPT_CMD_SUCCESS;
}

BUILDIN_FUNC(unitisforcewalk)
{
	block_list *bl = nullptr;
	bool force_walk = false;

	if (script_getnum(st, 2))
		bl = map_id2bl(script_getnum(st,2));
	else
		bl = map_id2bl(st->rid);

	if (bl != nullptr) {
		struct unit_data *ud = unit_bl2ud(bl);

		if (ud != nullptr)
			force_walk = (ud->walktimer == INVALID_TIMER && ud->state.force_walk);
	}
	script_pushint(st, force_walk);

	return SCRIPT_CMD_SUCCESS;
}