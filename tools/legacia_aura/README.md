# Legacia aura builder

Builds the client files that draw the Gepard-style auras (3 parts) without Gepard.
The server sends each part as a hat effect (`pc_aura_apply` in `src/map/pc.cpp`):

| Part | What | Count | Hat effect ID |
|---|---|---|---|
| 1 | Ground circle (spins on the floor) | 152 | 1000 + n |
| 2 | Rings around the player that start at the inner edge of the ground circle and grow, tilted (strip wraps once around each ring) | 117 | 1300 + n |
| 3 | Sparkles (small copies shoot out from the feet across the floor and fade) | 35 | 1600 + n |

1. Put the Gepard aura textures in `auras/1`, `auras/2`, `auras/3`.
2. `python3 genaura.py auras out` writes `out/data/texture/effect/efst_legacia_aura/*.str|*.bmp` and `out/auras.json`.
3. `python3 genlub.py out/auras.json HatEffectIDs.lub HatEffectInfo.lub "out/data/luafiles514/lua files/hateffectinfo"`
   adds the entries to the client's two hat effect files (plain-text lub from the client).
4. `python3 preview.py gif out/data/texture/effect/efst_legacia_aura/g001.str g001.gif` renders a preview.

The look was matched to a recording of the Gepard aura. All parts are anchored at the feet with `hatEffectPos = -11` (same as the official Magic Circle hat effect).
If they sit too high or low in game, change that number in `genlub.py` and rebuild.
Sizes and speeds are the constants at the top of each section in `genaura.py`.
