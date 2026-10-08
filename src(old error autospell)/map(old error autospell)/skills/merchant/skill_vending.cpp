// Copyright (c) rAthena Dev Teams - Licensed under GNU GPL
// For more information, see LICENCE in the main folder

#include "skill_vending.hpp"

#include "map/intif.hpp"
#include "map/pc.hpp"

SkillVending::SkillVending() : SkillImpl(MC_VENDING) {
}

void SkillVending::castendNoDamageId(block_list *src, block_list *target, uint16 skill_lv, t_tick tick, int32& flag) const {
	map_session_data *sd = BL_CAST(BL_PC, src);
	if (sd) {
		// Prevent vending of GMs with unnecessary Level to trade/drop. [Skotlex]
		if (!pc_can_give_items(sd))
			clif_skill_fail(*sd, MC_VENDING);
		else {
			int32 i = 0;
			sd->state.prevend = 1;
			sd->state.workinprogress = WIP_DISABLE_ALL;
			sd->vend_skill_lv = skill_lv;
			sd->vend_loot = 0;
			ARR_FIND(0, MAX_CART, i, sd->cart.u.items_cart[i].nameid && sd->cart.u.items_cart[i].id == 0);
			if (i < MAX_CART) {
				// Save the cart before opening the vending UI
				sd->state.pending_vending_ui = true;
				intif_storage_save(sd, &sd->cart);
			}
				if (battle_config.extended_vending) {
					std::shared_ptr<item_data> item;
					char output[CHAT_SIZE_MAX];
					int c = 0, d = 0;
					sd->vend_lvl = skill_lv;
					if (battle_config.item_zeny)
						d++;
					if (battle_config.item_cash)
						d++;
					for (const auto &it : itemdb_vending) {
						if ((item = item_db.find(it.first)) != NULL &&
							item->nameid != ITEMID_ZENY && item->nameid != ITEMID_CASH)
							c++;
					}
					c += d;

					if (c > 1)
						clif_vend(sd, sd->vend_lvl);
					else {
						sd->state.prevend = 1;
						if (c) {
							item = item_db.find(battle_config.item_zeny ? battle_config.item_zeny : battle_config.item_cash ? battle_config.item_cash : item->nameid);
							sd->vend_loot = item->nameid;
							sprintf(output, msg_txt(sd, 1596), itemdb_name(sd->vend_loot));
							clif_messagecolor(sd, color_table[COLOR_CYAN], output, false, SELF);
							clif_openvendingreq(*sd, 2 + sd->vend_lvl);
						}
						else {
							sd->vend_loot = 0;
							clif_openvendingreq(*sd, 2 + sd->vend_lvl);
						}
					}
				}
				else {
					sd->state.prevend = 1;
					sd->state.pending_vending_ui = false;
					clif_openvendingreq(*sd, 2 + skill_lv);
			}
		}
	}
}
