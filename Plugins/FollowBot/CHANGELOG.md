# Changelog

## 1.0.3

- Built with the current plugin SDK: a controller that is not XInput reports no port (-1) even when ready.

## 1.0.2

- Follows the program language (ten languages).
- The decision log's Copy button copies the English text, as the log file holds it, so a bug report reads the same in every language.
- Works in every game language: with POEFixer 371 or newer, ground loot is priced by what the item is, not by the name your client shows, so the value rules work with any client language (and two items that share one translated name get their own prices).
- The decision log's filter also finds a line by its English words, as the log file and the Copy button show it.
- Rules made with Add or Duplicate and profiles made with New or Duplicate keep their names in the program language: switching the language no longer leaves them in the language they were made in.
- The "Find UI by control name" match list fits its longest translated choice.
- Texts that name a button or hotkey use its label exactly (Russian, Japanese, Korean and Spanish); Traditional Chinese uses the game's word for a portal (時空之門), Japanese the game's word for a town portal (タウンポータル).
- UI dumps and "Find UI" results cut a long game text between characters, never inside one.
- An item POEFixer could not price by what it is yet (its item data still downloading) is priced again a second later, instead of keeping the price of its name for the rest of the area.
- A renamed profile with a long Cyrillic or Thai name is cut between letters, never inside one.

## 1.0.1

- Works with the separate menu window (POEFixer 366): the host hands the plugin the right drawing context for its settings tab and for its overlay.

## 1.0.0

- First version: a follower character that keeps up with a leader you name, walking smart paths around walls instead of straight into them.
- Smooth following: it starts walking when the leader is far enough away and stops close to him, without jittering on every small step; it can aim ahead of a moving leader and keep its own place when several followers run together.
- Gets itself unstuck: plans around the blocked spot, side-steps, opens doors, walks back a little, dodges and tries escape directions before it gives up for a moment.
- Follows the leader through area transitions, portals (player, town, map device, map return) and boss arena entrances: it never clicks before he has gone through, and uses another object of the same kind nearby when his is gone. Waypoint and checkpoint warps cannot be followed yet: it walks to where he was last seen and says why it waits.
- Respawns at the checkpoint through the death screen, counts deaths and stops at the limits you set.
- Picks up loot by kind (currency, quest items, gems, waystones, tablets, fragments, equipment by rarity, gold ...), by name lists and by value from the price service, only near the leader and away from enemies, and only when the backpack has room.
- Skill and flask rules: each rule presses your key when its conditions hold (enemies and their rarity, life / energy shield / mana, buffs and their stacks, the leader's life and buffs, skill readiness, moving or standing, distance to the leader), with passive, assist and aggressive combat and a leash around the leader.
- Stashes into your own or the guild stash in town and hideout, with tab routing by item kind, protected backpack cells and keep rules; optionally leaves the map through a portal to stash and comes back.
- Sprints on long, straight, quiet stretches when you set your Dodge / Sprint key.
- Safe by design: it only acts while the game itself is in front and the POEFixer menu is closed, pauses when you touch the mouse or keyboard, has start / stop, pause and hold-to-take-over hotkeys, and never leaves a key held.
- Several settings profiles, picked automatically for the character you play.
- Debug tools: an overlay of the planned path and targets, a status window, a decision log, entity and UI dumps, and "walk to the nearest stash / portal / transition" tests.
