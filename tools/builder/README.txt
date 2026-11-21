PvZ2 Builder - a modding toolkit for PvZ2 and the games made from it
=====================================================================

This is milestone 0 of the Builder: the core every later tool sits on (almanac editor, import
tool, level editor). It opens a game, lists what that game has, lets you change any of its
property sheets with fields that know their types, shows its animations and pictures without
starting the game, and saves your changes as a mod the launcher loads.

WHICH GAMES
  Any game on PvZ2's international release: EA's own PvZ2, Reflourished, AltverZ and other fan
  games. Everything the Builder offers comes from the open game's own files, never another
  game's: the plants, zombies, grid items, projectiles and level modules it lists are the ones
  that game has, and the fields it offers are the ones that game's data uses. PvZ2 China isn't
  supported.

SETTING UP
  Put PvZ2Builder.exe beside PvZ2Launcher.exe (or in a folder next to it). File > Open a
  launcher game then lists the launcher's games. You can also open any game folder from the
  launcher's games\ folder, or a bare .obb (main.<version>.<package>.obb).

THE WINDOW
  Left       what the game has: Plants, Zombies, Grid items, Projectiles, Level modules (each
             with its name from the game's strings), and All sheets: every PACKAGES sheet.
             Search filters the list. A changed object is marked * and bold.
  Fields     the picked object's fields. Each has an editor for its kind:
               tick box         true / false
               number           whole numbers, or decimals with a point (7.5)
               drop-down        a field the game uses with a few values again and again, or
                                (with a class dump) an enum: only its values
               RTID picker      the objects the field can point at ("Go" opens the one it does)
               JSON             lists and objects inside the object
             Hover a field's name for its type, how many of the game's objects set it, and
             what an RTID points into. Changed fields are bold and green. "Add field" adds a
             field the class has that this object doesn't set.
  Properties a plant's, zombie's or grid item's property sheet (its Properties RTID), edited
             the same way: cost, recharge, hitpoints and so on live there.
  Preview    the object's animation (PopAnim) drawn with the game's own pictures, at the first
             frame of a label (idle, attack ...). Parts the game only shows some of the time
             (costumes, armour and its damage, effects) start hidden; tick them under "Parts
             shown". Save PNG keeps the picture.
  Pictures   every picture in its resource groups, cut out of the game's atlases.
  JSON       the whole object as JSON, for anything the fields don't cover.

SAVING (File > Save as mod, Ctrl+S)
  The changes become a mod folder, by default in the game's own mods folder in the launcher,
  so it's for that game alone: switch it on in the launcher's Mods tab. A mod holds:
    mod.json                 its name, and "games"/"versions" set to this game and version
    obb\PACKAGES\X.json      each changed sheet, with only the changed objects, and in those
                             only the changed fields; the launcher merges them over the game's
                             own, so the mod keeps working when the game changes the rest, and
                             several mods can change the same sheet
    builder.json             what the Builder wrote, so saving again replaces it
  A change a merge can't express (a field taken away, a changed object with no alias when its
  sheet has several of its class) puts the whole sheet in the mod instead ("#replace"), and the
  save says so. A sheet the game reads from its CDN folder (newer content it downloaded) goes
  into cdn\ whole.
  File > Open mod to keep editing takes up a mod made for this game: its changes become edits
  again.

CLASS DUMP (File > Load class dump)
  The fields are worked out from the game's own data, which fits any game. A class dump made
  from the same game's libraries (decomp\m1\reflection.json for Reflourished 1.4.2) adds the
  game's own types, enums and fields no object sets yet. Loaded once, it's kept in the game's
  folder (builder\class-dump.json) for next time. Load only a dump made for that game.

WHAT'S NOT IN MILESTONE 0
  New objects (new plants, zombies ...: the import tool), levels (the level editor), the
  almanac page preview, animation playback (only one frame is drawn), ASTC textures.

BUILDING
  .NET 8 SDK, on Windows:
    dotnet publish Pvz2Builder -c Release -o publish
  gives one self-contained publish\PvZ2Builder.exe.
  Pvz2Builder.Core has everything but the window and builds anywhere; its tests:
    dotnet run --project Pvz2Builder.Tests
  With PVZ2_OBB set to a game's .obb they also run against the real game (and with PVZ2_DUMP
  set to a class dump, with it). The tests' tools: "profile <obb>", "render <obb> <POPANIM_ID>
  <out.png>", "sprites <obb> <POPANIM_ID>".
