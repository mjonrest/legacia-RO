# Voice chat, Graymap and Player Statistics (legacia.dll)

In-game voice chat has three parts:

1. **map-server** (`src/map/voice.cpp`): confirms which character a voice client belongs to and tells the voice server who is in which party, guild, map and position.
2. **voice server** (`tools/voice-server`): a small Go program that relays Opus audio between players over UDP. It only lets players hear the people they are allowed to hear.
3. **client plugin** (`tools/legacia-client`, `legacia.dll` + `plugins/legacia_plugin.dll`): loaded by the game client. It captures the microphone, plays audio, and adds the Voice Settings, Graymap and Player Statistics windows.

```
 game client + voice DLL ──(game connection, one extra packet 0x0cf0)──> map-server
          │                                                                  │
          └──────────(UDP 6951, Opus audio)──> voice-server <──(TCP 6950, link)
```

### Signing in

1. The plugin sends HELLO to the voice server and gets back a random 16-byte claim code, plus a 6-character short form of it.
2. Either of these then proves which character the plugin belongs to:
   - **Automatic:** the plugin appends one `CZ_VOICE_CLAIM` packet with the code to the game connection, right after the game's own packets. This only works when the plugin can see the game connection. Gepard hides it, for example.
   - **By address:** right after HELLO, and whenever a player logs in, the voice server asks the map-server (AUTO_QUERY) about the address the plugin sent from. If exactly one player is in game from that address (vendors on @autotrade don't count), it must be them, and the map-server claims the short code for that character. Nothing to do for the player. This needs the map-server to see players' real addresses, not a proxy's.
   - **Typed:** when several players share an address (two clients, same house), the plugin shows `click the mic, or type @voice K7Q2MA`. Clicking the mic on the voice bar types it into game chat (and puts it on the clipboard, in case the game ignores it: Ctrl+V and Enter). The map-server sends the short code with the character to the voice server, and tells the player whether it worked.
3. The map-server sends that character's info and the code to the voice server, which binds the UDP endpoint to the character and sends WELCOME.

Codes are valid for 10 minutes and work once. The plugin asks for a new one before the old one runs out. The plugin never reads or changes game packets coming from the server.

## Channels

| Id | Channel | Who hears it |
|----|---------|--------------|
| 0 | Normal | players on the same map within `-range` cells (14 by default, same as the visible area) |
| 1 | Party | members of the same party, on any map |
| 2 | Guild | members of the same guild, on any map |
| 3 | Room | players in the same in-game chat room |
| 4 | Whisper | one player, picked by character name. Players who block all whispers in game (`/exall`) don't get voice whispers either |

Chat-banned players (`@mute`, `sc.cant.chat`, `PCBLOCK_CHAT`) can still listen but cannot talk.

## Setting it up

### 1. Voice server (Linux)

```sh
cd tools/voice-server
go build -o voice-server .        # needs Go 1.21 or newer
sudo cp voice-server /usr/local/bin/
sudo cp voice-server.service /etc/systemd/system/
sudo systemctl edit voice-server  # set Environment=VOICE_SECRET=... (8 to 32 characters)
sudo systemctl enable --now voice-server
```

Flags:

| Flag | Default | Meaning |
|------|---------|---------|
| `-link` | `127.0.0.1:6950` | TCP address the map-server connects to. Keep it on localhost or a private network. |
| `-udp` | `0.0.0.0:6951` | UDP address players send voice to. Open this port in the firewall. |
| `-range` | `14` | Normal chat hearing range in cells. |
| `-rate` | `100` | Max voice packets per second per player (a 20 ms Opus frame is 50 per second). |

Bandwidth is about 3 to 5 KB/s per player who is talking, times the number of listeners.

### 2. map-server

In `conf/import/map_conf.txt`:

```
voice_enable: yes
voice_ip: 127.0.0.1
voice_port: 6950
voice_secret: <same value as VOICE_SECRET>
```

The map-server log shows `Connected to voice server` when the link is up. It reconnects every 10 seconds if the voice server restarts.

Packet obfuscation (`PACKET_OBFUSCATION`) must stay off, which is the default, because the plugin's extra packet is not obfuscated.

### 3. Client plugin

See `tools/legacia-client/README.md`. Players need `legacia.dll`, `plugins/legacia_plugin.dll` and `legacia.ini` in the game folder, and the client must load `legacia.dll`.

## Protocol

All numbers are little-endian.

### Client -> map-server (custom game packet)

| Packet | Layout |
|--------|--------|
| `0x0cf0` CZ_VOICE_CLAIM | `W packetType, B code[16]` (18 bytes) |

Only the voice plugin sends it, and nothing is sent back on the game connection. Claims more often than once per second per character are ignored.

### map-server <-> voice server (TCP link)

Every packet starts with `W cmd, W length` (length includes the 4 header bytes).

| Cmd | Direction | Body |
|-----|-----------|------|
| `0x7600` HELLO | map -> voice | `S secret[32]` (zero padded), then `B features` (0x01: answers STATS_QUERY, 0x02: answers CELLS_QUERY). Older map-servers send no feature byte, and the voice server then never sends them STATS_QUERY |
| `0x7680` HELLO_ACK | voice -> map | `B result` (0 ok, 1 wrong secret) |
| `0x7601` PLAYER | map -> voice | `L account_id, L char_id, L party_id, L guild_id, W x, W y, B flags, S map[16], S name[24], L chat_id` (flags: 0x01 chat-banned, 0x02 blocks all whispers; older map-servers leave out `chat_id`) |
| `0x7604` CLAIM | map -> voice | `B code[16], L char_id` |
| `0x7605` CLAIM_SHORT | map -> voice | `S short_code[8], L char_id` (from `@voice`, zero padded) |
| `0x7682` CLAIM_RESULT | voice -> map | `L char_id, B result` (0 linked, 1 wrong or expired code; failed CLAIM_AUTO gets no answer) |
| `0x7606` ONLINE | map -> voice | `B ip[4]`, sent when a player logs in, and for everyone after HELLO_ACK |
| `0x7683` AUTO_QUERY | voice -> map | `B ip[4], S short_code[8]`: a client at this address is waiting. Sent after each HELLO (at most once per 2 s per address) and after ONLINE for the newest waiting client |
| `0x7607` CLAIM_AUTO | map -> voice | `S short_code[8], L char_id`, answer to AUTO_QUERY when exactly one player plays from that address |
| `0x7684` STATS_QUERY | voice -> map | `L char_id, B page`: the plugin's Player Statistics window wants a page (0 overview, 1 attack, 2 magic, 3 defense, 4 skills, 5 healing, 6 procs, 7 EXP and drops: the `@statsview` sections) |
| `0x7608` STATS | map -> voice | `L char_id, B page, text`: the page as `\n`-separated lines with `^RRGGBB` colours, the same text as `@statsview` (at most 6000 bytes) |
| `0x7685` CELLS_QUERY | voice -> map | `L char_id`: the plugin's LGP wants the skill cells around the player |
| `0x7609` CELLS | map -> voice | `L char_id, W count, count * {W x, W y, W skill_id, B kind}`: skill units within 14 cells (at most 200; kind 1 = it can hit the player). Hidden traps are left out unless the client would see them |
| `0x7602` LEAVE | map -> voice | `L char_id` |
| `0x7603` PING | map -> voice | (none) |
| `0x7681` PONG | voice -> map | (none) |

After HELLO_ACK the voice server forgets all players and the map-server sends every voice player again (signed-in clients keep their sessions). PLAYER is sent right before each CLAIM and then whenever their party, guild, chat room, map, position or flags change (checked every 500 ms). LEAVE is sent on logout.

### Client <-> voice server (UDP)

| Type | Direction | Layout |
|------|-----------|--------|
| `0x01` HELLO | client -> voice | `B type` |
| `0x86` CHALLENGE | voice -> client | `B type, B code[16], S short_code[6]` (send the code as CZ_VOICE_CLAIM, or show the short code for `@voice`; valid for 10 minutes) |
| `0x81` WELCOME | voice -> client | `B type, L session, L char_id` |
| `0x82` DENIED | voice -> client | `B type, B reason` (1 server busy, 2 unknown session: send HELLO again) |
| `0x02` AUDIO | client -> voice | `B type, L session, W seq, B channel, opus frame (max 1000 bytes)`. On channel 4 (Whisper) `S target_name[24]` comes before the opus frame |
| `0x84` AUDIO | voice -> client | `B type, L speaker_char_id, W seq, B channel, opus frame` |
| `0x85` SPEAKER | voice -> client | `B type, L char_id, S name[24]`, sent before the first audio from a new speaker |
| `0x03` PING | client -> voice | `B type, L session` (send every 10 s; sessions idle for 30 s are dropped) |
| `0x83` PONG | voice -> client | `B type` |
| `0x04` BYE | client -> voice | `B type, L session` |
| `0x06` MODE | client -> voice | `B type, L session, B flags`: flag 0x01 = the player turned voice chat off. The session stays signed in for the Player Statistics window but gets no audio, can't talk and can't be whispered to. Sent after WELCOME, on every change, and with each PING while voice is off |
| `0x05` STATS | client -> voice | `B type, L session, B page`: ask for a Player Statistics page (at most one every 250 ms) |
| `0x87` STATS | voice -> client | `B type, B page, B seq, B part, B parts, text`: one piece (up to 1000 bytes) of the page; join parts 0..parts-1 with the same seq |
| `0x88` NO_STATS | voice -> client | `B type, B page`: no map-server is linked, or it is too old to answer |
| `0x07` CELLS | client -> voice | `B type, L session`: ask for the skill cells around the player (LGP, twice a second, at most one every 250 ms) |
| `0x89` CELLS | voice -> client | `B type, W count, count * {W x, W y, W skill_id, B kind}` |
| `0x8a` NO_CELLS | voice -> client | `B type`: no map-server is linked, or it is too old to answer |

Audio is 48 kHz mono Opus, 20 ms frames. A session only accepts packets from the address that sent HELLO. Muting a single player (the Players tab) is done in the client.

## Player Statistics window

The plugin's Player Statistics window (Stats button, Ctrl+Shift+S) shows the same sections as `@statsview`. It asks the voice server, which asks the map-server over the link. The plugin stays signed in to the voice server even when the player turns voice chat off (MODE 0x01: no audio either way), so the window works for players who don't use voice chat too. It only stops when the voice server isn't running. The voice server only asks for the character its session belongs to, and the map-server only answers for characters whose voice client signed in during this login, so nobody can read another player's stats. Both sides limit requests to one every 250 ms per player; the window itself refreshes every 3 seconds while it's open.

## Abuse protection

- Claim codes are 128-bit random values, used once and only for the endpoint that asked for them. Typed short codes are 30 bits. `@voice` is limited to once every 3 seconds per character, so guessing one is impractical, and a correct guess only links the guesser's own character. Only the map-server can say which character a code belongs to, and it only does that for the logged-in character that sent it.
- Linking by address only happens when exactly one player plays from that address, so it can only pick a player on the same connection. Clients sending from other addresses never match, and it never moves a character that already has a working voice client. AUTO_QUERY is limited to once per 2 s per address and 20 per second overall, so HELLO floods don't load the map-server.
- A session ends when the character logs out.
- A session id from a different IP or port is ignored, so sessions can't be hijacked by guessing ids.
- Players can only talk on Party, Guild or Room if they have one, and only people in that party, guild or chat room hear it.
- A whisper reaches only the named character, and not if they block all whispers. Players can also mute anyone in the Players tab.
- Chat-banned players can't talk.
- Each player is limited to `-rate` packets per second and 1000 bytes per frame.
- The link secret is compared in constant time, and the link port should not be reachable from the internet.
