# Legacia aura builder

Builds the client files for the Aura Master (`npc/miracle/aura.txt`).

1. Put the aura textures in `auras/1` (ground circles) and `auras/3` (body glows).
2. `python3 genaura.py auras out` writes `out/data/texture/effect/efst_legacia_aura/*.str|*.bmp` and `out/auras.json`.
3. `python3 genlub.py out/auras.json HatEffectIDs.lub HatEffectInfo.lub "out/data/luafiles514/lua files/hateffectinfo"`
   adds the aura entries to the client's two hat effect files (IDs 1001-1152 ground, 1201-1235 glows).
4. `python3 preview.py gif out/data/texture/effect/efst_legacia_aura/g001.str g001.gif` renders a preview.

Ground auras sit on the floor with `hatEffectPos = -11` (same as the official Magic Circle hat effect).
If they show too high or low in game, change that number in `genlub.py` and rebuild.

Server side: custom hat effect IDs 1001-1999 are allowed by `hateffect_id_valid()` in `src/map/script.hpp`.
