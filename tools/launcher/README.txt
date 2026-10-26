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

THE MENU
  Display           assets (high 1536 / low 768), the resolution the game
                    draws at, window size, full screen, VSync, ANGLE.
  Code injections   hover to collect (on by default: moving the mouse over sun,
                    coins and plant food picks them up), keybinds, and each
                    code patch in patches\.
                    Offline skips the CDN and the update check. A mod's own
                    libraries (Reflourished's libRFLib, AltverZ's libSnowie)
                    are part of the game and always load; the menu lists them.
  Custom content    the mods in mods\, ticked ones in load order (the lowest
                    wins where two change the same file). Up / Down reorder.
  Play              see below. "Unpack resources" unpacks the OBB into
                    unpacked\ as PNG and JSON, for making content. "Last log"
                    opens rfu-log.txt from the last run.
  Settings are saved as game.json in the game's folder as you change them.

PLAY
  1. With no custom content ticked, the launcher asks if you want to load
     some (a folder; tick "Don't ask again" to stop it asking). The folder is
     copied into the game's mods\ and ticked.
  2. With no custom content, it checks for a new version of the game. It
     downloads the game's CDN content for your version (only when it changed)
     and reads ForceUpdateConfigData's MinVersion, the same value the game
     uses to tell a phone to update. Above your APK's version: you're told,
     and can point it at the new APK and OBB (the old ones move to old\, saves
     stay) or play anyway. New events, Pinata Parties and Penny's Challenges
     don't change MinVersion, so they never count as updates. Offline, it
     reads the CDN content the game downloaded last time.
  3. The ticked mods are built into .launcher\overlay (only when something
     changed) and rfu starts with them laid over the game's files.

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
  Tick "Keybinds" and press "Edit keybinds". One key per line:
    1 seed 1            1 picks the first seed packet (top of the seed bank)
    D shovel            D picks up the shovel; D again puts it back
    1 tap 5% 18%        hold 1 = a finger at 5% across, 18% down
    Space tap 640 360   in pixels of the game's screen (its resolution)
    Backspace back      Android's back button
  seed and shovel find the packet or the shovel wherever the level puts
  them; a new keybinds.txt starts with 1-9 for the packets and D for the
  shovel (an older one keeps what it has: add those lines to it).
  Key names are SDL's (A, 1, Space, F1, Left_Shift, Keypad_1). Keybinds are
  off while the game shows its keyboard, so typing a name still works.

CODE PATCHES (patches\*.patch)
  Small fixes as bytes for rfu to put into a library after it loads, after
  RFLib's own patches. Each file is a toggle in the menu:
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
