# Legacia client plugin (legacia.dll)

`legacia.dll` adds voice chat, Graymap and the Player Statistics window to the game client. Server setup and the voice protocol are in `doc/voice_chat.md`.

## What players get

- **Signing in:** happens on its own when you log in, and the server says "Voice chat connected.". If two game clients run on the same connection, the server can't tell them apart, so the screen shows `Voice: click the mic, or type @voice K7Q2MA`. Click the mic on the voice bar (or type that in game chat) once in each client.
- A small **voice bar** sits at the bottom right of the game screen, like the chat tabs:
  - the speaker icon turns all voices off and on, and the mic icon mutes your mic (it turns green while you talk);
  - **Normal / Party / Guild / Room / Whisper** pick who you talk to. Room is the chat room you are sitting in. Whisper asks for a character name (click Whisper again, or right-click it, to change the name);
  - right-click the bar to open the Voice Settings window, and drag the bar by its edges to move it.
- **Ctrl+Shift+V** also opens the Voice Settings window at the bottom right of the game screen (drag its title bar to move it; it stays where you put it until the game restarts). It has three tabs:
  - **Voice:** push to talk or open mic, the push-to-talk key (Mouse 4 by default, click the button to change it), and who you talk to (Nearby, Party or Guild). It also has the mic level meter, mic and speaker volume, a "only send when I speak" gate for open mic, and on/off switches.
  - **Players:** everyone you heard recently. You can mute anyone or change their volume.
  - **Devices:** which microphone and speakers to use.
- While someone is talking, their name shows near the top right of the game window.
- Settings are saved in `legacia.ini`. A log is written to `legacia.log`.

The push-to-talk key and the window shortcut only work while the game window has focus.

## Drawn inside the game

By default every window, button and the voice bar is drawn inside the game picture, as part of the game's own frame (Dear ImGui on the game's Direct3D 9 device, code in `src/gui_d3d.cpp` and `src/gui_windows.cpp`). They look and work the same as before, in the game's colours, but they also show in fullscreen, never float over other programs, and move with the game window. Clicks on a plugin window, and typing in a plugin text box (the whisper name, colour codes), don't reach the game; everything else does.

How it finds the device: the plugin points the exe's import of `Direct3DCreate9Ex` at itself, so it sees the device when the game creates it. If the plugin was loaded too late for that, it searches the game's renderer object for it (only on the 2026-01-07 exe). The hooks swap the device object's own function table for a copy with `Present`/`Reset` replaced; no code in the exe or in `d3d9.dll` is changed.

The game's mouse cursor stays on top of the plugin's windows: the game draws its cursor just before `Present`, so the plugin recognises those quads as they are drawn (the game's screen-space vertices with depth 1e-6 and colour `0xfaffffff`, around the mouse point), keeps a copy with the same render states, and draws them once more after its windows. If no cursor has been recognised for about 30 frames, an arrow is drawn over the plugin windows instead.

If the device isn't found within 20 seconds, the plugin falls back to the separate windows (`src/ui.cpp`, `src/graymap_ui.cpp`, `src/stats_ui.cpp` and the two button files) and writes why to `legacia.log`. `ingame=0` in the `[ui]` section of `legacia.ini` always uses the separate windows.

## Graymap

A **Graymap** button sits under the Cash Shop button at the top right of the game screen, drawn in the same style (code in `src/graymap_button.cpp`). Click it to switch Graymap on or off; while it's on, the little map shows your colours, the rim glows gold and sparkles twinkle. Right-click it, or press **Ctrl+Shift+G**, to open the Graymap window. Drag the button to move it; its spot is saved as `button_right`/`button_top` (pixels from the game window's top right corner) in the `[graymap]` section of `legacia.ini`.

The Graymap window's ON/OFF switch replaces the 3D ground, water and buildings with flat coloured cells, so it's easy to see where you can walk:

| Colour | Cells |
|--------|-------|
| Walkable | cells you can walk on (gat types 0, 2, 4) |
| Unwalkable | walls and objects (types 1, 6) |
| Water | walkable water (type 3) |
| Shootable wall | cells you can't walk on but can shoot over (type 5) |
| Grid lines | the outline of every cell |

Pick a colour on the left, then choose it with the picker, the brightness slider, or by typing a hex code or RGB values. Changes show on the map right away and are saved in the `[graymap]` section of `legacia.ini`. Players, monsters, NPCs and skill effects are still drawn normally.

Graymap hooks the client's own ground drawing, so it only works with the exe it was made for (the 2026-01-07 Ragexe the addresses were taken from, the same base exe Miracle uses). If the Legacia exe is a different build, Graymap and the crash guard stay off and log it, and voice chat and Player Statistics still work; the addresses at the top of `src/graymap.cpp` and `src/crashguard.cpp` would then need to be found again for that exe. It checks the code first, so a wrong exe can't crash; the Graymap window then says it isn't available. It draws each cell with the client's own ground-cursor function (the one that marks skill target cells), and skips the client's ground, water and map model drawing while it's on.


## LGP

The Graymap window has an **LGP** tab: coloured cells drawn on the ground around your character, with Graymap on or off (code in `src/graymap.cpp`, settings in the `[lgp]` section of `legacia.ini`).

- **Square** and **Circle**: each can be on or off, a ring or filled, with its own colour (click the colour square), size, and wave (Flat, or 1 to 5 from slow swells to quick ripples moving outwards).
- **AoE skill cells**: skills on the ground near you (Storm Gust, Lord of Vermilion, traps you can see, ...), red when they can hit you and blue when they can't. The map-server sends them through the voice server, so this layer needs the voice server running, like Player Statistics.
- **Dead cells**: cells you can't walk on inside the shapes, in dark red. The shapes skip them.
- **Gutter lines**: a gap between cells. **Smooth follow**: the shapes glide with you instead of jumping one cell at a time.
- **Tile** Box or Round, **Depth** 2D flat or 3D raised.

Where you stand comes from the camera (it always looks at your character), so no player data in the exe is read. The cells are drawn with the same cell function as Graymap, so LGP needs the same exe as Graymap.

## Auras

The Gepard client no longer draws Gepard's 3-part auras, so the plugin draws them (code in `src/aura.cpp`, settings in the `[aura]` section of `legacia.ini`, and the **Aura** tab of the Graymap window: on/off, my aura, other players, size 50-150%).

- **Part 1, ground circle**: turns flat on the floor under the player.
- **Part 2, rings**: three tilted rings round the player that start at the inner edge of the ground circle, grow and fade while their tilt turns round the player.
- **Part 3, sparkles**: small sparkles shooting out from the feet across the floor.

The server keeps each player's aura (`AURA_DATA`, set by the Aura Master NPC `npc/miracle/aura_master.txt`, `set_aura`, `@auras`) and sends the auras of the players around through the voice server about once a second, so auras need the voice server running, like Player Statistics (`@show_auras` still works). Each piece is one of the client's own polygons: projected with the game's camera and added to its sorted list of see-through polygons, so auras sit on the ground in 3D, turn with the camera and are drawn in the right order with the characters. Positions come from the client's actor list. Like Graymap it only works with the exe it was made for (addresses at the top of `src/aura.cpp`, checked before use).

Textures: `plugins\auras\1\001.bmp` ... (`1` ground circles, `2` rings, `3` sparkles), 32-bit BMPs made from the Gepard aura textures with `python3 tools/legacia_aura/genplugin.py <gepard auras folder> <out>` (they are not in git). `test/aura_host.cpp` runs `src/aura.cpp` under Wine against a fake game (the exe's own projection code) and `test/render_faces.py` draws what it produced.

## Player Statistics

A **Stats** button sits under the Graymap button, in the same style (code in `src/stats_button.cpp`). Click it, or press **Ctrl+Shift+S**, to open the Player Statistics window (`src/stats_ui.cpp`). Drag the button to move it; its spot is saved as `button_right`/`button_top` in the `[stats]` section of `legacia.ini`.

The window has one tab per page, the same sections as the in-game `@statsview` command:

| Tab | Shows |
|-----|-------|
| Overview | name, job, levels, HP/SP, zeny and the `@statsview status` section (stats, ATK, MATK, HIT, FLEE, CRIT, DEF, MDEF, ASPD, elements) |
| Attack, Magic, Defense, Skills, Healing, Procs, EXP & Drops | the `@statsview attack`, `magic`, `defense`, `skill`, `heal`, `effect` and `drop` sections |

**Backgrounds:** put any `.png`, `.jpg` or `.bmp` in `plugins\stats_backgrounds\` (the plugin makes the folder the first time it runs). Any size works: the picture is scaled to cover the 520x440 window and cropped around its centre, so pictures of about 520x440 or bigger, with the interesting part in the middle, look best. The **Background** button at the bottom lists the pictures, sets how see-through the text panels are (40% to solid) and opens the folder; **<** and **>** step through them. The choice is saved as `background` and `panel_opacity` in the `[stats]` section of `legacia.ini`, so a server can ship a default picture by setting `background=` there.

While it's open, it refreshes the shown tab every 3 seconds, so it follows gear and buff changes. Scroll with the mouse wheel, the scrollbar or the arrow and Page keys. The text comes from the map-server through the voice server. Turning voice chat off doesn't stop it: the plugin stays signed in quietly (no mic, no audio, the server skips it for voice), so the window works for players who never use voice chat. It needs a running voice server and map-server that know the STATS packets (see `doc/voice_chat.md`).

## Crash guard

the Legacia client exe (2026-01-07 Ragexe) can crash when a skill is aimed at a monster that disappears at that moment (killed by someone else's AoE, for example): the game looks the target up and reads its position without checking that it still exists. `src/crashguard.cpp` catches exactly that one fault (address 0xce0d35 with a missing actor) and sends the game down its own "target gone, give up" path, so the skill is simply not used. It changes no game code; it checks the exe's bytes first and stays off on any other exe. Each catch is written to `legacia.log`.

## Gepard: loader + plugins folder

Gepard Shield hash-checks every DLL in the client folder, so each new plugin build would need a new (paid) approval. It does not check DLLs loaded from a subfolder. `build.sh` therefore also makes `build/package/`:

```
legacia.dll                  <- loader (~40 KB). The exe imports it; Gepard approves it once
plugins/legacia_plugin.dll   <- the real plugin (voice + Graymap + Player Statistics + auras)
plugins/auras/               <- aura textures (see Auras)
legacia.ini                  <- settings (set host= before shipping)
```

The loader (`loader/loader.cpp`) only loads `plugins\legacia_plugin.dll` and writes `loader: loaded ...` (or the error) to `legacia.log`. It never needs to change, so later updates only replace `plugins\legacia_plugin.dll`. `legacia.ini` and `legacia.log` stay in the client folder.

## Building

On Linux, with `g++-mingw-w64-i686`, `cmake` and `curl` installed:

```sh
./build.sh   # downloads and checks Opus 1.5.2, then builds build/package/
```

The DLL is 32-bit and statically linked, so players don't need any other files.

## Shipping it to players

1. Set `host=` in `legacia.ini` to the voice server's public IP or hostname.
2. Make the client load `legacia.dll` at startup. With WARP, add this to the DLL spec file used by the "Load Custom DLLs" patch (keep the entries already there), then re-patch the exe:
   ```yaml
   -
     Name: legacia.dll
     Funcs:
       - Name: "LegaciaVersion"
   ```
3. Add `legacia.dll`, `plugins/legacia_plugin.dll`, `legacia.ini` and the new exe to the patcher.
4. Gepard Shield checks which modules are loaded into the client. Send `legacia.dll` (the loader) to the Gepard team so it's allowed before you release it. Later plugin updates only replace `plugins/legacia_plugin.dll`.

## How it works

- It patches the exe's imports of `send` and `closesocket`. It only watches outgoing packets: `0x007d` means the player entered a map. After that, it appends its own 18-byte claim packet once, right after one of the game's packets. It never reads or changes packets from the server.
- Audio is 48 kHz mono Opus at 24 kbps in 20 ms frames, with in-band FEC. Each speaker has a 60 ms jitter buffer, and their audio is mixed to the chosen output device with `waveOut`.
- It does not do echo cancelling, so players on open mic should use headphones.
