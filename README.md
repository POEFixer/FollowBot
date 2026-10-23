# Follow Bot

Current version: 1.0.1

Requires POE2Fixer 365 or newer.

A follower for Path of Exile 2: your second character keeps up with a leader you name, wherever he walks,
finds its way around walls, gets itself unstuck, and always tells you why it does what it does. Fighting with
skill rules, picking up loot, stashing and following through transitions and portals are configured in their
own tabs.

Follow Bot sends real keyboard and mouse input to the game. It only acts while the game window itself is in
front, the POEFixer menu is closed and no panel it did not open is visible, and your own mouse or keyboard input
pauses it for a moment. Use it within the game's rules.

## Setting it up

1. In the game, open **Options -> Input** and bind **Move Only** to a key you do not otherwise use (the bot's
   default is **T**). The bot holds that key while the cursor points where to go; it never clicks to walk.
2. Optional: note your **Dodge Roll** key (for sprinting and stuck recovery) and a key that opens a portal.
3. Enable the plugin in POEFixer -> Plugins, open its tab and, on **General**:
   - type the leader's **character name** (or pick him from *Pick a nearby player* while he stands next to you,
     or tick *Auto* to follow the only other player in the area);
   - set the **Move** key to the key you bound to Move Only, plus the Dodge / Sprint and Portal keys if you have
     them;
   - check the **hotkeys** (Start / stop: F8, Pause: Pause by default).
4. Stand near the leader with the game in front and press **F8** (or the Start button).

## Starting, stopping, taking over

- **Start / stop** (F8): starts or stops the bot.
- **Pause** (Pause): stops every action at once and releases all keys; press again to resume.
- While a Debug tab "walk to the nearest ..." test runs, both keys end the test (the bot stays stopped).
- **Hold to take over** (not set by default): while you hold it the bot stands still; it resumes shortly after
  you let go.
- Just playing works too: your own mouse or keyboard input in the game pauses the bot for 1.5 s (Death & Safety
  tab).
- The bot holds (and says why in the status window) whenever the game is loading, its menu is open, another
  window or the POEFixer menu is in front, POEFixer's own automation runs, or an inventory or stash panel it did
  not open is visible.

## The tabs

**General** - the leader (name, nearby-player picker, auto, upper / lower case), the game keys (Move, Dodge /
Sprint, Dodge only, Portal; left-mouse movement as an option), the hotkeys, and profiles (several settings sets;
with *Auto-select by character* the profile bound to the character you play is used).

**Follow & Movement** - follow and stop distances (the bot starts walking above the first and stops below the
second, so small moves do not cause jitter), aiming ahead of a moving leader, following his route, formation
slots for several followers, following in town and hideout, how long to wait for the leader after a zone change;
steering (look-ahead, cursor distance, re-aim rate, the activation click after focus or zoning); sprint
(distance, no-enemy radius, longest hold, rest); path finding (re-plan rate, how much to keep off walls, search
limit, snap radius); getting unstuck (thresholds and each recovery step). Diagnostics show the current mode,
plan and stuck state.

**Travel** - following the leader through area transitions, portals and boss arenas, and the click timing used
for objects.

**Loot** - which ground items to pick up by kind (currency, quest items, gems, waystones, ... equipment by minimum
rarity), how far from you and from the leader, and whether enemies nearby stop it.

**Skills** - skill and flask rules: each rule presses a key you set when its conditions hold (enemies, life /
mana, buffs, leader state, ...); rules are checked top to bottom. Combat mode (passive, assist, aggressive) and
the leash around the leader.

**Stash** - own or guild stash, when to stash (in town / hideout when the backpack fills, or by leaving the map
through a portal), protected backpack cells that are never stashed.

**Death & Safety** - automatic respawn through the death screen, death limits that stop the bot, the safety
options (pause on your input, run-time limit, closing panels it did not open, emergency exit at low life in
maps) and input timing (minimum gap between actions, random extra, click spread, human-like cursor paths for
clicks, bot tick).

**Debug** - the world overlay (path, look-ahead, cursor target, leader and route, circles, objects, loot targets,
combat radius, stuck marks), the in-game status window, tools (entity dump, UI dump, death-screen test, walk to
the nearest stash / guild stash / portal / transition / checkpoint / waypoint, with or without clicking it), the
decision log (filter, copy, optional file) and live skills and buffs to copy names from.

## Travel: following the leader out of the area

When the leader disappears right next to an area transition, a portal or a boss arena entrance, Follow Bot
walks to that object and takes it too, then picks him up again in the new area. It never clicks anything
before he has gone through. If the object he used is gone or closed, it uses another one of the same kind
nearby (for example another map device portal). After a click it waits for its own area to change; if
nothing happens it walks up again and retries with a slightly different aim. Waypoint and checkpoint warps
cannot be followed: the bot walks to where he vanished and says so.

**Settings (Travel tab)**
- **Area transitions / Portals / Boss arenas** - what to follow. Portals means player and town portals,
  map device portals in the hideout and the map's return portal.
- **Also camp, parent and sub-area portals** - also follow camp teleports and portals back to a parent area
  or into a sub-area (off by default).
- **Object near his last position** (10 cells) - an object this close to where he vanished explains it.
- **Out of range beyond** (125 cells) - he was this far away with no object near him: he only walked out of
  sight, and the bot walks after him.
- **Another object within** (25 cells) - where a replacement of the same kind may stand when his is gone.
- **Click from** (6 cells), **Hover before click** (120 ms), **Hold the click** (50 ms), **Extra hovers** (2),
  **Aim height** (0.5) - how the object is clicked.
- **Wait for the area change** (6 s), **Tries** (3), **Look for the object** (4 s) - retries and waiting.
- **Walk to his last position after a failed travel** - on: walk there and wait; off: stay where it failed
  until he shows up.

The tab's Diagnostics show the last vanish, the object chosen, the attempts and the last failure reason.

## Death and respawn (Death & Safety tab)

When your character dies, the bot lets go of every key, waits a moment and presses a respawn button on the
death screen. It finds the button by its game-internal name, so it works in every client language. Checkpoint
buttons come first. Deaths are counted (in this map and since Start), and the bot stops when a limit is
reached. Automatic respawn needs POEFixer 365 or newer; on older versions, or when no button is found, the
bot holds still with "Dead - respawn manually".

- **Respawn automatically** - off: the bot waits for you.
- **Prefer "Respawn at checkpoint"** - the checkpoint buttons before the generic respawn button.
- **Allow the generic respawn button** - may also press the death screen's other respawn button. Warning:
  what it does depends on the situation; it can send you to town or the hideout (leaving the map), or log
  you out where respawning is not possible.
- **Wait before clicking** (2.5 s) + **Random extra** (0.8 s), **Retry every** (2 s), **Tries** (5).
- **Stop after deaths in a map** (3; 0 = never; deaths outside maps do not count here), **Stop after deaths
  in total** (0 = never), **Dead after** (300 ms of zero life).

## Debug tools for game patches

- **Dump UI tree** - every visible UI element with its control name (POEFixer 365+), text and screen
  rectangle, written to the plugin's dumps folder.
- **Find UI by control name** - search the game UI by exact name, name prefix or the text shown in your
  language, visible elements only or all; lists up to 12 matches with their name path, visibility and
  position.
- **Test death screen lookup** - while dead, shows whether the death screen and each respawn button are
  found and visible, and which one the bot would press.

**Tips:** leaders, wait a second next to the portal you took so the follower can see which one it was.
Avoid waypoint and checkpoint warps when you want the follower along. If a respawn never happens after a
patch, use "Test death screen lookup" while dead and look at the decision log.

## Loot

The bot picks up ground items by their **kind in the game data**, never by the text on the label, so the rules
work with any client language. It loots only near the leader and stops looting whenever following matters more.

**How an item is decided** (first match wins):
1. **Never pick up** list → left on the ground (wins over everything, quest items included).
2. **Quest item** with *Always pick up quest items* on → picked up, even when loot is off.
3. **Pick up loot** off → nothing else is picked up.
4. **Always pick up** list → picked up.
5. **Category** ticked (and its minimum rarity / minimum value met) → picked up.
6. **Value threshold** on and the item's known value reaches it → picked up.

### Categories
Quest items, Gold, Currency (orbs, shards, abyssal bones, verisium, artifacts, wombgifts), Omens / essences /
catalysts / distilled emotions, Runes / soul cores / idols, Fragments (splinters, map fragments, breachstones,
reliquary, pinnacle and trial keys), Gems (uncut, lineage), Waystones (and expedition logbooks), Tablets,
Jewels, Flasks and charms, Unique items (any unique, whatever its class), Weapons, Armour (incl. shields, foci,
quivers), Accessories (rings, amulets, belts), Other.
- **Min rarity** - for items that have a rarity (equipment, jewels, flasks, waystones, tablets). Equipment
  defaults to Rare and is off by default.
- **Min value** - only for items the price list knows (currency-like loot and uniques); an item without a known
  price is picked by the tick alone.

### Value
- **Also pick anything worth at least** - picks an item whose known value reaches the threshold, even when its
  category is off.
- **Reference currency** - Chaos, Divine or Exalted Orb; every value on the tab is in it.
- **Compare the value per backpack cell** - divides the value by the item's size first.
- Prices come from POEFixer's price list (Settings -> Prices). It is in English: with another client language
  most prices are unknown. Magic and rare equipment has no price (only uniques do).

### Name lists
One entry per line, any letter case. An entry matches when it appears in the base name, the unique name or the
game path (e.g. `CurrencyRerollRare`). Put it in double quotes (`"Chaos Orb"`) to require the whole name, so
`Greater Chaos Orb` is not matched. Names are the ones your client shows.

### Where and when
- **Scan radius** (40 cells) - items farther from you are ignored.
- **Leader within** (45) - loot only while the leader is this close, and only items this close to him (no leader
  set: rule off; leader set: he must be in sight).
- **Stop when leader beyond** (+10) - a pickup in progress stops once he is farther than *Leader within* plus this.
- **No enemy within** (25) - no looting while a monster is this close to you or to the item; **Loot during combat**
  ignores it.
- **Keep free cells** (0) - leave this many backpack cells free (quest items and gold ignore it).
- **Loot in town and hideout** (off).
- A wanted item that does not fit marks the backpack *full*, which starts the stash trip (Stash tab); gold is
  picked up even with a full backpack.

### Pickup
Walk up to (6 cells), Label above item (16 px), Hover before click (120 ms), Click hold (50 ms), Extra hovers (1),
Wait to vanish (900 ms; Wait while walking 2500 ms), Tries per item (3), Then leave it for (60 s), Loot at most
(8 s, then follow), Then follow for (2 s), Price lookups per scan (32). An item the bot cannot reach is left alone
at once.

### Diagnostics
Open the Diagnostics card to see what the bot is doing about loot, the items picked up this session, free
backpack cells, how many items it gave up on, and a live list of nearby items with category, rarity, distance,
value and the decision with its reason ("pick up: its category is on", "leave: on your exclude list",
"wanted, no room", ...).

### Tips
- Turn on the Debug overlay's *Loot targets* to see wanted items (green), the current target (amber), items
  without room (orange) and items it gave up on (red).
- If items are not picked up, check the item filter shows their labels: the bot clicks the label.
- Do not run Pick My Loot at the same time: both would pick up items.

## Skills and flasks

The bot can press your skill and flask keys by rules. The game's key bindings cannot be read, so
every rule names the key you bound to that skill. Rules are checked top to bottom several times a
second; the first rule whose conditions all hold presses its key (list order = priority).

### Combat (Skills tab, first card)
- **Use skill rules** - master switch (flask rules included).
- **Combat mode** - *Passive*: attack rules never fire (buffs, movement, flasks still do).
  *Assist*: attack rules only target hostiles inside the leash around the leader (without a
  visible leader the bot defends itself within the combat radius). *Aggressive*: any hostile
  within the combat radius.
- **Combat radius** (default 50 cells) - a hostile this close puts the bot "in combat"; attack
  rules never target farther than this.
- **Leash around the leader** (default 60) - only hostiles this close to the leader count for
  combat (and as Assist targets). Ignored while the leader is not visible.
- **Attack while leader within** (default 60) - attack rules pause while the leader is farther:
  following comes first.
- **Min time between casts** (default 150 ms) - shortest gap between any two rule presses, on top
  of the general input gate and its jitter.
- **Skip monsters the game marks untargetable** (default on) - hidden / untargetable monsters are
  neither aimed at nor counted. Turn off if the bot ignores monsters it should attack.
- **Never target monsters whose path contains** - one entry per line, matched without case
  against the monster's metadata path (e.g. `Totem`). Not targeted, not counted.

### Rules
The table shows each rule's key, kind and **Status** (why it does not fire right now). Buttons:
Add, Duplicate, Up, Down, Delete (asks first) and **Add rules from the skill bar** - one disabled
rule per skill on your bar with its skill name filled in; bind each key, check kind and target,
then enable it. Select a rule to edit it:

- **Key** - click and press the key, or use the mouse button picker (left button included).
  Mouse-button skills are pressed at the aimed point. The bot's own hotkeys, move key and sprint
  key are refused.
- **Kind** - *Attack* (aimed at a target), *Self buff* (aimed at you), *Leader buff* (aimed at the
  leader), *Movement* (25 cells ahead on the path or toward the leader: dashes, blinks), *Flask*
  (never moves the cursor).
- **Target** (Attack) - Nearest, Strongest (rarity then life), Lowest life, Densest cluster (most
  hostiles within 12 cells), Leader, Self, Cursor unchanged. Self / Leader buff rules can choose
  "Cursor unchanged" for skills that need no aim. Off-screen targets are skipped.
- **Skill** - optional skill id (pick from the live list): the rule waits until the skill is ready
  (not on cooldown). It does not choose the key.
- **Own interval** - this rule's own cooldown.
- **Press** - *Tap*; *Hold* (key down for a fixed time, aimed once); *Channel* (key down up to the
  time, re-aiming at the target, letting go early when it is gone). Holds and channels stand still
  and only start while the bot follows (not during a pickup, travel or stash run).
- **Stop moving to cast** - release the move key before a tap; off = keep walking (the cursor
  returns to the walking direction after the press).
- **When it fires** - Combat (always / in combat / out of combat), Movement (any / moving /
  standing still); **Enemies**: at least N hostiles within a radius (around the target for aimed
  attacks, around you for other rules), minimum rarity, target range, line of sight;
  **Life, energy shield and mana**: own life below, ES below, mana above, mana below, leader's life
  below; **Buffs**: only while you lack a buff, only while you have a buff (with at least N
  stacks), only while the leader lacks a buff; **Distance to the leader**: at least / at most.
- **Flask** (Flask kind) - belt slot (Flask 1-2, Charm 1-3, or not checked): the slot must hold
  enough charges for one use; *Skip while its effect is running*.

### Diagnostics
Each rule's "why not firing" line, the last fired rule, your skills (bar slot, ready, cooldown,
charges ready) and your / the leader's buffs. Click a name to copy it, or "Copy all". Skills and
buffs are read while the Skills tab is open.

### Tips
- Put defensive rules (guards, flasks) above attack rules: the first matching rule wins.
- Keep a buff up with "Only while you lack" + the buff id from Diagnostics; link skills on the
  leader with a Leader buff rule + "Only while the leader lacks".
- A catch-up dash: Movement rule, Combat "Always", Distance to the leader "At least 40".
- The two flask rules ship disabled: if POEFixer's AutoPot already drinks, leave them off.
- A rule that never fires: read its Status - it names the first condition that fails.

## Stash

Empties the backpack into your **own stash** or the **guild stash**: the bot walks to the stash, opens it, switches to the right tab, Ctrl+clicks every item that is not protected or kept, and closes the stash again. An item counts as stashed only once it is gone from the backpack; an item still there after the wait is clicked again and skipped after a few tries, and several skips in a row mean the tab is full (the rest of its items stay).

**When it stashes**
- **Stash now** (button on the Stash tab, or the "Stash now" hotkey on the General tab): in a town or hideout at once; in a map through a portal (needs the portal key).
- **In town and hideout** (on by default): when the backpack uses at least *Stash at* cells (or a loot item found no room), a stash of the chosen kind is in the area, and the leader has stood still for *Leader stands still* (or is not around). It only runs when there is something to stash and the backpack has more items than right after the last run, so leftovers never cause a loop.
- **Leave the map through a portal** (off by default): in a map with at least *Trip at* cells used and no enemy within *No enemy within*, the bot presses the portal key, takes its new portal, stashes in the hideout / town and returns through the portal nearest to where it arrived. If the way back fails it waits ("Stranded: take the portal back"), retrying a few times, until you take the portal or the leader comes.
- After a failed automatic run the bot waits *After a failure wait*; after three failures in one area it stops trying there until the area changes. "Stash now" always works.

**Settings**
- *Stash items*: master switch. *Stash*: own or guild stash.
- *Stash at*, *Leader stands still*, *Trip at*, *No enemy within*, *Portal key* (bind the game's portal skill to it; same key as on the General tab), *Wait for the portal*, *Look for it within*, *After a failure wait*.
- **Tabs**: routes "category -> tab name" (your own tab names, any case), a *Default tab* for everything else (empty = whatever tab is open), and *Use the open tab when a tab cannot be used* (off: items for a missing tab stay in the backpack).
- **What stays in the backpack**: a 12x5 grid of protected cells (click or drag; a dot shows where items are now; *Protect what is there now* marks every occupied cell), kept categories, and kept names (one per line; an item stays when its base name, unique name or metadata path contains one). Quest items always stay.
- **Timing**: click gaps for the own and guild stash (the guild stash is slower: each deposit waits for the server), wait for an item to move, waits per item, skips in a row = full, wait for the panel, clicks on the stash, stand this far away, wait after a tab click, hover on the stash before clicking.

**Tips**
- Protect the cells of anything you always carry before the first run.
- Use a slower click gap for the guild stash if items are skipped there.
- Tab switching and finding stashes the game hides from the entity list need POEFixer 365 or newer; on older versions everything goes into the open tab and only visible stashes are used.
- If an item is ever on the cursor, the bot stops (a click could drop it): put the item down, close the panel and start again.
- When a tab is full (or a tab of your routes cannot be used and *Use the open tab* is off), automatic stashing pauses: make room in the stash, then press Stash now (or empty the backpack yourself, or Start again).
- The Diagnostics card shows what the bot sees: host support, known stash objects, the last tab list (name, selected, on screen), the current plan, the run's state, the last failure and warnings.

## Files

Everything lives in the plugin folder: `config/settings.json` (settings; a file that cannot be read is never
overwritten - an unreadable one is kept as `settings.json.corrupt`), `logs/` (decision log when enabled),
`dumps/` (tool output).

## Notes

- Some features need a newer POEFixer (365+): automatic respawn (locale-independent UI names), stash tab routing
  and seeing stash objects the game hides. On older versions the bot says so and keeps working without them.
- Holding still while POEFixer's own automation drives the game (crafting, memory-mode Live Search and Manual Buy,
  auto-stash, trade recovery, auto-login) also needs POEFixer 365 or newer: on older versions do not run those on
  the same game client while the bot runs.
- Do not run Pick My Loot's or Auto My Aim's cursor actions at the same time as the bot's own loot pickup: the bot
  honours their busy signal, but two pickers fight over the cursor.
- Distances are in grid cells: one cell is about 11 world units.

## Install

Install it from POE2Fixer → Plugins → Plugin Manager (one click, updates included), or download `FollowBot.zip` from [Releases](https://github.com/POEFixer/FollowBot/releases/latest) and extract it into the POE2Fixer folder (it creates `Plugins\FollowBot\`). Then enable the plugin in the Plugins tab.

## Build from source

This repository is the plugin folder from the POE2Fixer source tree (`Plugins/FollowBot/`) together with the host files it compiles against, kept at the same relative paths:

- `POEFixer/plugin_sdk/` - the POE2Fixer Plugin SDK v6 headers (`PluginAbi.h`, `PluginSDK.h`)
- `POEFixer/imgui/` - the Dear ImGui sources the DLL compiles against

1. Install Visual Studio 2022 with the "Desktop development with C++" workload (MSVC v143, Windows 10/11 SDK).
2. Open `FollowBot.sln`, select **Release | x64** and build the solution.
3. The plugin is written to `x64\Release\Plugins\FollowBot\FollowBot.dll` (Release builds carry no debug information, see `Directory.Build.targets`).
4. Copy `FollowBot.dll` into `Plugins\FollowBot\` next to the POE2Fixer executable.
