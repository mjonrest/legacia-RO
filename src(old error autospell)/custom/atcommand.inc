// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

/**
 * Custom Atcommands
 * Place the body of custom atcommands in this file.
 * Format:
 *	ACMD_FUNC(command_name)
 *	{
 *		<code>;
 *	}
 **/

//ACMD_FUNC(newcommand)
//{
//	clif_displaymessage(fd, "It works!");
//	clif_specialeffect(&sd->bl, EF_HEARTCASTING, AREA);
//	return 0;
//}


/*==========================================
* @afk
*------------------------------------------*/
ACMD_FUNC(afk) {
 
        nullpo_retr(-1, sd);
								
		if( pc_isdead(sd) ) {
		clif_displaymessage(fd, "Cannot @afk if you are dead.");
		return -1;
		}

                if( map_getmapflag(sd->m, MF_AUTOTRADE) == battle_config.autotrade_mapflag )
                {

                if(map_getmapflag(sd->m, MF_PVP)  || map_getmapflag(sd->m, MF_GVG)){
                clif_displaymessage(fd, "You may not use the @afk maps PVP or GVG.");
                return -1;}

                        sd->state.autotrade = 1;
			if (battle_config.autotrade_monsterignore)
			sd->state.block_action |= PCBLOCK_IMMUNE;

			pc_setsit(sd);
                        skill_sit(sd,1);
                        clif_sitting(*sd);
			clif_changelook(sd,LOOK_HEAD_TOP,471);
                        clif_specialeffect(sd, 234,AREA);                       
                        if( battle_config.afk_timeout )
                        {
                                int timeout = atoi(message);
                                status_change_start(NULL, sd, SC_AUTOTRADE, 10000,0,0,0,0, ((timeout > 0) ? min(timeout,battle_config.afk_timeout) : battle_config.afk_timeout)*60000,0);
                        }
                        clif_authfail_fd(fd, 15);
                } else
                        clif_displaymessage(fd, "@afk is not allowed on this map.");
        return 0;
}
