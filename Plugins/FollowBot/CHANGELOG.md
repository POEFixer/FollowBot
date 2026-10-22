# Changelog

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
