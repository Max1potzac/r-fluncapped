PvZ2 Launcher - a menu for each PvZ2 game you have, which starts it under rfu
=============================================================================

SETTING UP
  Put these in one folder:
    PvZ2Launcher.exe
    rfu.exe and its DLLs (SDL2.dll, zlib1.dll ...)   (or in a folder "rfu")
    pvz2conv.exe                                    (or in a folder "pvz2conv")
  Start PvZ2Launcher.exe. It makes a "games" folder next to itself.

  Each game gets a folder in games\ (Reflourished, AltverZ, ...). Put the
  game's .apk and .obb there, or press "Add game" and pick them. The launcher
  runs pvz2conv on the APK to take out the libraries, the version and the
  game's icon, and checks the OBB is the one that APK asks for. A game is
  recognised by its APK, so standalone mods such as AltverZ work the same way
  as Reflourished, each with its own saves.

  PvZ2 China (com.popcap.pvz2cthdlx, libSrc.so) has no OBB - all its data is
  inside the APK - so it needs only the .apk. "Add game" doesn't ask for an
  OBB once it sees a China APK, and the launcher runs it from the APK.

  Reflourished 1.5.0 and later download their own OBB, so "Add game" doesn't
  ask for one either: the launcher downloads the newest OBB into the game's
  folder (about 1.3 GB), and asks for the file only if that fails.

THE WINDOW
  It looks like PvZ2's own menus: wooden frames, glossy buttons. "Theme"
  switches Light (a day on the lawn), Dark (the night levels) or Auto
  (Windows' own app setting); it's kept in launcher.json.
  Your Games        each game, its icon, kind (Reflourished, PvZ2, PvZ2 China,
                    other fan mods) and version. Add game / Remove game
                    (sends the game's folder - APK, OBB, saves, its own mods,
                    settings - to the Recycle Bin, after asking).
  The banner        the picked game's name (click to rename), version, OBB,
                    and PLAY (see below).
  Mods tab          every mod for this game (see MODS), as cards with an
                    on/off switch. Chips along the top filter by category;
                    "All games" shows the mods made for other games too,
                    greyed out. A picked mod shows its folder and what it can
                    do: Controller settings, Edit keybinds, List missing
                    strings, Load earlier / later (content), Open folder.
                    "+ Add mod" copies a folder in; "Restore" writes the
                    built-in mods back as they came.
  Display tab       assets (high 1536 / low 768), the resolution the game
                    draws at, window size, full screen, VSync, ANGLE.
  Game tab          Offline skips the CDN and the update checks. "Download
                    OBB updates" (Reflourished, on by default) is step 3 of
                    PLAY. Keybinds, the keybind layout editor, controller
                    settings; Update game, Unpack resources (the OBB as PNG
                    and JSON, for making content), Game folder, Last log.
                    A mod's own libraries (Reflourished's libRFLib, AltverZ's
                    libSnowie) are part of the game and always load.
  Log tab           what the launcher did; the last line is along the bottom.
  Settings are saved as game.json in the game's folder as you change them.

MODS
  Everything you can switch on for a game is a mod: a folder with a mod.json.
  Mods in the launcher's mods\ folder (beside games\) are for every game
  they fit; a game's own mods\ folder holds mods for it alone (and hides a
  shared one with the same folder name).

  The launcher's own code injections are mods, written into mods\ the first
  time it starts, so they can be read, changed, copied or deleted:
    Hover to collect        sun, coins and plant food picked up by moving
                            the mouse over them (on by default)
    Frozen animations fix   a Citron that finishes charging before the first
                            wave goes on to its charged idle (on)
    Keybinds                keys from the game's keybinds.txt (see below)
    Keybind hints           each key or button in a badge on what it
                            presses (on; needs Keybinds or Controller)
    Controller support      see CONTROLLER (on)
    Glove cursor            the controller's glove for the mouse too
    Offline cloud / Record cloud session (PvZ2 China only, on): the China
                            cloud stub offline, the recorder online
  and some samples:
    Pirate English          a localization (see below)
    Run without libRFLib    Reflourished only: starts it without its own
                            library, to tell its problems from rfu's
  A deleted built-in mod stays deleted (mods\.builtin.json remembers);
  "Restore" brings them all back.

  mod.json (// comments are fine):
    "id", "name", "description", "author", "version"
    "kind"        injection, content or localization (worked out if left out)
    "category"    Gameplay, Controls, Fixes, Visuals, Language, Levels,
                  Network, Debug... (the chips); anything goes
    "games"       which games it's for: "global" (any but China),
                  "reflourished", "pvz2" (EA's own), "china", "other" (fan
                  mods such as AltverZ), a package name, or "*". Left out:
                  every game.
    "versions"    ["1.5"]: only for a game whose version starts so
    "default"     true: switched on for a game that hasn't seen it yet
    "rfu"         rfu arguments; {game}, {mod} and {data} are those folders
    "patches"     code patch files in the mod (left out: every *.patch in it)
    "requiresAny" ids of mods one of which must be on too
    "when"        "offline" or "online": only then
    "createDirs"  folders made before the game starts
    "hooks"       launcher features: "keybinds-file" (writes keybinds.txt),
                  "pad-binds" (the buttons from Controller settings, and
                  the game's controller-layout.txt)
    "actions"     buttons: [{"label": "...", "run": "edit-keybinds" |
                  "controller-settings" | "edit-layout" | "open:<path>"}]
    "language", "strings"   a localization's language and strings file
    "fonts"       fonts in place of the game's: {"<game's font>": "<file>"}
  Old settings (game.json from before) carry over: what was on stays on.

CONTENT (cdn\ and files\ in a mod)
  rfu lays a mod over the game's own files without changing them: switch it
  off and the game is as it was, and the game's CDN download can't overwrite
  it, so there's no need to turn Wi-Fi off for custom strings.

    cdn\...          over the CDN folder (files\No_Backup\CDN.<version>):
                     levels, LawnStrings, liveconfig ...
    files\...        over the game's files folder (Android's
                     /sdcard/Android/data/<package>/files)

  A folder with neither cdn\ nor files\ (and no "kind": "injection") is all
  CDN content, the way level packs are shared: loose level files (anything
  with a LevelDefinition) go into levels\, a LawnStrings file into the CDN
  folder itself, folders as they are. So a level pack folder can be added
  exactly as downloaded.

  On the way, files are converted to what the game reads:
    x.json         becomes x.rton where the game's own file is x.rton
    x.rton.json    always becomes x.rton
    LawnStrings    merged: the mod's strings go over the game's current ones
                   (and over earlier mods'), so text the mod doesn't have
                   isn't lost. The merge is redone after each CDN download.
  A file spelled differently from the game's (LawnStrings-en-us.rton for
  LawnStrings-en-US.rton) replaces the game's. Where two mods change the
  same file the one loaded later wins (Load earlier / later).

    obb\...          files in place of the OBB's own, at their paths there
                     (obb\PACKAGES\FONTS.json for PACKAGES\FONTS.RTON; .json is
                     converted as above). Only files the OBB already has, and
                     not textures (new art goes in groups\, below).
    fonts\...        fonts in place of the game's own (see LOCALIZATIONS)

  obb\ and fonts\ don't change the OBB either: the launcher writes the
  changed parts to .launcher\obb-patch.bin and rfu reads the OBB through it.

  Property sheets (PLANTTYPES, PLANTPROPERTIES, ZOMBIETYPES, FONTS ... any
  {"objects": [...]} file) are merged, not replaced: a mod's file holds only
  what it adds or changes, and several mods can change the same sheet.
    an object with the aliases (or TypeName) of one the game has
                   changes it: only the fields the mod gives, objects inside
                   merged the same way, lists and values replaced
    "#remove": true   takes the game's object out
    "#new": true      marks an object the mod adds (a new plant): if the game
                   already has one by that name, the mod can't be switched on
    anything else  is added at the end
    "#replace": true at the top of the file replaces the game's sheet whole
    "List#add": [...]   adds to the list List rather than replacing it (what
                   it has already is left out); LawnStrings' LocStringValues
                   by key, so a key it has gets the new text
    "List#merge": [{"#key": "Type", "Type": "plant", ...}]   changes the
                   object of the list List whose Type is "plant" as above
                   (only the fields given, "X#add" too), or adds it if there
                   is none; "#key" can be a list of fields, all of which must
                   match. The Pinecone sample puts its plant into the shop's
                   allow list (MAGENTO_FILTER) this way.
  A mod's obb\PACKAGES\LAWNSTRINGS-<language>.json (a new plant's name and
  almanac text) also goes over the CDN's LawnStrings-<language>: once the
  game has downloaded its CDN content it reads its strings from there and not
  from the OBB, so strings only in the OBB would show as <Missing ...>. The
  game reads that content out of the archive it downloads (CDN.<version>\
  tar.full), not the files it unpacks next to it, so the launcher also puts
  the merged strings (and any other cdn\ file of a mod that the archive has)
  into a copy of that archive, which the game reads in its place. A download
  of new content shows from the launch after it.
  Mods load in order, so where two change the same field the later wins.
  Two mods adding the same new name (the same plant ID, say) can't be on
  together: switching the second on asks which one to keep, and Play stops
  with the list if both are on.

  New art (a new plant's animation and its atlases):
    groups\<name>\...  a new resource group of the OBB, its files at their
                     OBB paths (groups\PlantPinecone_768\ATLASES\
                     PLANTPINECONE_768_00.PNG). PNGs become textures
                     (RGBA8888); everything else (PAM ...) goes in as it is.
    obb\PROPERTIES\RESOURCES.json   the groups' entries, merged into the
                     game's resource manifest: a group the game has takes the
                     mod's fields and resources (by id), a new one is added.
                     Slots are the launcher's to give: an id the game has
                     keeps its slot, a new one gets one after the last.
                     "subgroups#add": [...] adds groups to a composite the
                     game has.
  Some pictures are drawn from the game's own atlas whatever the manifest
  names as their parent: a seed packet from UI_SeedPackets' atlas. Give such
  a picture as an image of the game's group (UI_SeedPackets_768 and _1536),
  its parent the game's atlas, and "#png": "<file in the mod>" in place of
  ax, ay, aw and ah:
    {"id": "UI_SeedPackets_768", "resources": [{"id": "IMAGE_UI_PACKETS_PINECONE",
     "path": ["images", "768", "initial", "UI", "packets", "pinecone"],
     "type": "Image", "parent": "ATLASIMAGE_ATLAS_UI_SEEDPACKETS_768_00",
     "#png": "packets/pinecone_768.png", "x": 18, "y": 4}]}
  The launcher puts the PNG into a clear place of the atlas that no picture
  has, and gives the image that place. One that doesn't fit is left out
  (the log says so).

  A new plant's behaviour that no game class has can come from rfu (mod.json's
  "rfu", see docs\runtime.md, Plant behaviours). The Pinecone sample mod (shared
  on its own, not here, as its art is PvZ2 China's) is a whole new plant made
  this way: its art, sheets, packet, store entry and behaviour.

LOCALIZATIONS
  A localization is a mod with "language" (en-US...) and a strings file of
  plain "KEY": "text" lines (see mods\Pirate English). Its strings are
  merged over the game's own LawnStrings-<language> each time you play, so:
    a key the mod has       shows the mod's text
    a key it doesn't have   shows the game's own text - say a new
                            Reflourished event's strings the translation
                            doesn't cover yet
  The card shows how many of the game's strings it covers. "List missing
  strings" writes missing-strings.json in the mod's folder: every string it
  doesn't have yet, with the game's text, ready to translate and copy in.
  Copy the Pirate English folder to start a translation of your own.

  Fonts: the game's own fonts only have the letters English (and a few
  others) need, so a Russian or Greek translation shows blanks. Put fonts
  that have the letters in the mod's fonts\ folder, named like the game's
  font each one replaces (fonts\BrianneTod.ttf), or list them in mod.json:
    "fonts": { "BrianneTod": "fonts/MyFont-Cyrillic.ttf" }
  The game's fonts (Reflourished): AshleyScriptMTStd, BrianneTod,
  Century751, CroMagnumCondensed, fbUsv8C5eI, House Of Terror, infroman,
  itcblkad, NewsflashBB, PICO12__, rockwellcondensed, ROGFonts, UnmaskedBB.
  A name the game doesn't have is logged with the list. Each font takes the
  name of the one it replaces (the game finds fonts by name), so any .ttf or
  .otf works as it is. Sizes, colours and which font each kind of text uses
  are in the OBB's PACKAGES\FONTS.RTON: a mod can change those too, with its
  own obb\PACKAGES\FONTS.json. Fonts go into the OBB, so PvZ2 China (no OBB)
  can't take them.

PLAY
  1. With no content or localization on, the launcher asks if you want to load
     some (a folder; tick "Don't ask again" to stop it asking). The folder is
     copied into the game's mods\ and switched on.
  2. With no custom content, it checks for a new version of the game. It
     downloads the game's CDN content for your version (only when it changed)
     and reads ForceUpdateConfigData's MinVersion, the same value the game
     uses to tell a phone to update. Above your APK's version: you're told,
     and can point it at the new APK and OBB (the old ones move to old\, saves
     stay) or play anyway. New events, Pinata Parties and Penny's Challenges
     don't change MinVersion, so they never count as updates. Offline, it
     reads the CDN content the game downloaded last time.
  3. Reflourished 1.5.0 and later update their OBB themselves before they
     start (Android's Java side does it, which rfu doesn't run), so the
     launcher does it for them: it asks the game's update server which OBB is
     newest, and if yours is behind downloads the patches up to it into the
     game's OBB folder (data\sdcard\Android\obb\<package>). The game applies
     them itself as it starts, then asks to be restarted, and the launcher
     starts it again. An OBB that matches no recent version (changed by hand,
     or too old) can only be replaced whole: you're asked first, and the old
     one goes to old\. Off when the game's own Settings > Automatic OBB
     Updates is off, when "Download OBB updates" is off, or offline. A
     failed update is logged and the game starts with the OBB it has.
  4. The content mods switched on are built into .launcher\overlay (only when something
     changed) and rfu starts with them laid over the game's files.

  The game's Settings > Import/Export Files (custom levels, saves, quest
  progress) opens Windows' own Open and Save As windows.

CUSTOM CONTENT (mods\<name>\)
  rfu lays a mod over the game's own files without changing them: switch it
  off and the game is as it was, and the game's CDN download can't overwrite
  it, so there's no need to turn Wi-Fi off for custom strings.

    mod.json         optional: {"name", "description", "author", "version"}
    cdn\...          over the CDN folder (files\No_Backup\CDN.<version>):
                     levels, LawnStrings, liveconfig ...
    files\...        over the game's files folder (Android's
                     /sdcard/Android/data/<package>/files)

  A folder with neither cdn\ nor files\ is all CDN content, the way level
  packs are shared: loose level files (anything with a LevelDefinition) go
  into levels\, a LawnStrings file into the CDN folder itself, folders as
  they are. So a level pack folder can be added exactly as downloaded.

  On the way, files are converted to what the game reads:
    x.json         becomes x.rton where the game's own file is x.rton
    x.rton.json    always becomes x.rton
    LawnStrings    merged: the mod's strings go over the game's current ones
                   (and over earlier mods'), so text the mod doesn't have
                   isn't lost. The merge is redone after each CDN download.
  A file spelled differently from the game's (LawnStrings-en-us.rton for
  LawnStrings-en-US.rton) replaces the game's.

  Content inside the OBB (plants, zombies, textures, PAM animations) isn't
  covered yet: that needs the OBB repacked, which pvz2conv can't do yet.

KEYBINDS (keybinds.txt)
  Switch the Keybinds mod on and press "Edit keybinds". One key per line:
    1 seed 1            1 picks the first seed packet (top of the seed bank)
    D shovel            D picks up the shovel; D again puts it back
    F plantfood         F picks up plant food (then click a plant);
                        fastforward and pause press those buttons
    Right scroll right  hold Right to scroll what's under the mouse (the
                        world map, the almanac); "scroll right 3%" is faster
    1 tap 5% 18%        hold 1 = a finger at 5% across, 18% down
    Space tap 640 360   in pixels of the game's screen (its resolution)
    Backspace back      Android's back button
  seed and the buttons are found wherever the level puts them. A new
  keybinds.txt starts with 1-9 and 0 for the packets (0 is the tenth, for
  Reflourished's ten-slot levels; an older file gets the 0 line added), D for the shovel, F for
  plant food, V for fast-forward, P for pause and the arrow keys for
  scrolling (an older one keeps what it has:
  add those lines to it). The mouse wheel scrolls too, sideways with Shift.
  Key names are SDL's (A, 1, Space, F1, Left_Shift, Keypad_1). Keybinds are
  off while the game shows its keyboard, so typing a name still works.

KEYBIND LAYOUT EDITOR (Game tab)
  Bluestacks style: load a screenshot of the game (any size), click where a
  key should press and press the key; drag a key to move it, right-click to
  remove it, double-click to change it. Positions are kept in percent, so
  they fit any resolution. "Save for this game" puts them in the game's
  keybinds.txt (other lines stay). "Save as layout mod" makes a mod for this
  game and version (keybinds.txt, layout.json and the screenshot), handy for
  a fan mod whose buttons sit elsewhere; rfu adds its keys over the game's
  own keybinds (a key both bind does what the layout says).
  "Controller" at the top places controller buttons instead, for one screen
  at a time (Lawn, Seed chooser, Pause menu, World map, Everywhere else, or
  Every screen): click a spot and pick the button. On that screen the
  button then taps its spot in place of what it does; every other screen
  keeps the button as it was. Use it for a button the controller doesn't
  know, like one a fan mod adds or an update moves (RFL 1.5's map buttons).
  They're saved in the game's controller-layout.txt (or the layout mod's),
  and with keybind hints on the game labels each button at its spot.
  Saving controller buttons switches Controller support on.

CONTROLLER
  Any controller Windows knows (Xbox, PlayStation, Switch Pro...) plays the
  lawn like PvZ1's console version. A cursor box appears on the lawn once
  the controller is used:
    D-pad / left stick  move the cursor a tile
    LB / RB             pick the seed packet (boxed in gold)
    A                   plant the picked packet on the cursor's tile
    B (hold 1 second)   dig up the plant there (a dial fills as you hold)
    Y                   feed it plant food
    Y (hold 2 seconds)  buy plant food (the "+" by the plant food bar)
    X                   tap the tile: a plant's own ability. A plant that aims
                        (Banana Launcher...) then aims where the cursor is:
                        X fires, A calls it off
    X (hold)            in the first column: hold the row's mower until the
                        game's dial fills
    LT / RT             vacuum up all the sun, coins and plant food
    Start               pause (again to resume)
    L3                  fast-forward
    R3                  slowmode: the game at half speed (again to go back)
    Right stick flick   pick a powerup: left the first, up the second,
                        right the third (whichever the level has)
    X (powerup in use)  hold to use it on the cursor's tile (snowballs,
                        flamethrower, Power Zap...); press twice quickly
                        to swipe the tile with the flick (Power Toss)
  When a character talks ("tap to continue"), any button moves it on.
  On the pause menu and the settings a box picks the sliders, buttons
  and checkboxes instead (the settings' list scrolls along):
    D-pad / left stick  move the box; left and right move a slider
    A                   press the button
    B / Start           resume
  On the seed selection:
    D-pad / left stick  move the box over the packets (the list scrolls)
    A                   pick the packet
    LB / RB             pick a slot on the left (boxed in gold)
    B                   take the slot's packet out
    B (hold 2 seconds)  empty every slot
    Y                   look at the lawn (again to come back)
    LT                  pick last level's packets again
    RT                  almanac
    X (hold 1 second)   Let's Rock
  Everywhere else (the world map, the menus...):
    Right stick         move a cursor (faster the further it's pushed)
    A                   touch where the cursor is (hold to drag)
    D-pad / left stick  scroll (on the worlds, left and right go world by
                        world)
    B                   back
  On the world map, also:
    LT                  almanac
    RT                  Zen Garden
    Start               settings
    Y                   shop
  Using the mouse hides the controller's cursor and labels; they come back
  when the controller is used.
  With keybind hints on, the buttons are labelled for the controller
  plugged in (Xbox's when it isn't recognised).
  "Controller settings" shows the controllers plugged in, lights up their
  buttons as you press them, and says whether the game recognises each one.
  One it doesn't (some DualShock 4s, generic pads) can be set up there: it
  asks for each button in turn and saves it in games\controllers.txt, which
  every game then reads. Below that, any action can be moved to another
  button, screen by screen: pick the screen (Lawn, Seed chooser, Pause menu,
  World map, Everywhere else) and move its actions. Each screen keeps its
  own buttons, so putting "take the packet" on X in the seed chooser leaves
  A planting on the lawn and pressing in the menus. "Defaults for this
  screen" puts that screen back.

CODE PATCHES (*.patch in a mod, or a game's patches\)
  Small fixes as bytes for rfu to put into a library after it loads, after
  RFLib's own patches. A mod's .patch files go with it; each file in a game's
  patches\ folder is a mod of its own (category "Code patches"):
    # name: Example fix
    # description: what it does
    libPVZ2.so+0x1a2b3c 1F 20 03 D5 was 00 00 80 52
  "was" is what must be there before; if it isn't (another build of the
  game), the whole file is left out and the log says so.

BUILDING
  .NET 8 SDK, on Windows:
    dotnet publish Pvz2Launcher -c Release -o publish
  gives one self-contained publish\PvZ2Launcher.exe.
  Pvz2Launcher.Core has everything but the window and builds anywhere; its
  tests: dotnet run --project Pvz2Launcher.Tests
  The built-in mods are tools\launcher\mods in the source, built into the
  launcher. The window's drawing (Ui\Draw.cs) is plain GDI+.
