# How the runtime works

## Guest memory

The guest (the game's arm64 code) sees a 64 GiB address space that is a single host reservation,
so a guest address is an offset from `GuestMemory::base()`. dynarmic uses the same reservation as
its fastmem arena: guest loads and stores compile to plain host loads and stores, and only
accesses to unmapped memory reach the slow path, where they become a `GuestFault`.

| Guest address | Contents |
| --- | --- |
| `0` - `0x1000'0000` | never mapped, so null pointer dereferences fault |
| `0x1000'0000` | loaded libraries, one after another |
| `0x4'0000'0000` | the guest heap (8 GiB), used by `malloc`, `mmap` and runtime objects |
| `0x8'0000'0000` | thread stacks, each with an unmapped guard page below it |
| `0xF'0000'0000` | import thunks |

## Calls between guest and host

Every imported function the libraries don't provide to each other (`malloc`, `fopen`,
`pthread_create`, JNI functions...) is bound to a *thunk*: two instructions, `svc #n; ret`. When
the guest calls one, dynarmic stops at the `svc`, and `Cpu::call` runs shim number `n` outside the
JIT. The shim reads its arguments from the guest registers through a `CallContext` (AAPCS64: x0-x7,
v0-v7, then the stack), writes the result to x0 or v0, and the guest resumes at `ret`.

Host code calls guest code with `Runtime::call` (or `Cpu::call` from inside a shim, for callbacks
such as `qsort` comparators): it sets up the arguments, points the link register at a special
return thunk, and runs the JIT until that thunk is reached. The interrupted guest state is saved
and restored around nested calls.

Imports nothing implements are bound to a stub that logs the first call and returns 0, so the game
runs as far as it can and the log says what to implement next.

## Threads

Each guest thread is a host thread with its own guest stack, bionic TLS block (`TPIDR_EL0`,
including the stack-protector cookie at offset 0x28) and its own dynarmic JIT. Guest mutexes,
condition variables and semaphores map to host primitives by address, created on first use, so
statically initialised ones work.

Each thread records the import it is inside (with its first argument and the guest's `lr` and
`fp` at the call). If no frame finishes for 30 seconds, rfu logs every guest thread: the import it
is blocked in, where it was called from, and the guest call stack found by following frame
pointers (`libPVZ2.so+0x...`), which is usually enough to see two threads waiting on each other.

## Android ABI details that matter

- `long` and pointers are 64-bit; `wchar_t` is 32-bit; `long double` is IEEE binary128.
- `va_list` is a 32-byte struct passed by reference; `VaListArgs` walks it.
- `struct stat`, `struct tm`, `FILE`, `pthread_attr_t` and friends use bionic's layouts, which the
  shims fill in field by field.
- RFLib finds libPVZ2 by reading `/proc/self/maps`, which the VFS generates from the loaded modules,
  then patches libPVZ2's code. Patching code is followed by an instruction cache flush, which drops
  the JIT's translations of that range.

## Overlays (custom content)

`--overlay <guest>=<host>` lays a host folder over a guest one, read-only (`Vfs::overlay`). A
read (`open` without write flags, `stat`, `access`, `gzopen` for reading) takes the file from the
topmost overlay that has it, else from the mount underneath; anything that writes goes to the
mount. `opendir` lists the mount's directory with every overlay's files added, each name once. So
custom levels and strings can sit over the CDN folder (`No_Backup/CDN.<version>`) without
changing it: the game's CDN download rewrites the folder underneath, and the overlay still wins.
The launcher builds one overlay for the CDN folder and one for the external files folder from the
mods switched on, converting JSON to RTON where the game's file is RTON and merging LawnStrings.


`--obb-patch <file>` changes how the OBB reads, not the OBB (`Vfs::patch`, `src/core/file_patch.h`):
the file is a list of byte ranges to read in place of the OBB's own, and a size past its end.
Opening the OBB read-only gives a handle that reads through it, `stat` reports the patched size,
and `mmap` (which reads through the handle) sees the same bytes. A patch made for an OBB of
another size is ignored with a warning. The launcher makes it from mods' `fonts/` and `obb/`
files (`ObbPatch.cs`): each resource group with a replaced file gets its packet written again
after the OBB's end, with part 0 stored, and the RSB's entries for the group and its memory pool
are pointed at it. A font replacing one of the game's takes that font's `name` table, since
`FONTS.RTON`'s typefaces pick a font by its family name.

## Code patches

`--patch <file>` applies small changes to a loaded library after every initializer has run, so
after RFLib's own patches: lines of `<library>+<offset> <bytes> [was <bytes>]`. Every line is
checked first (the offset inside one of the library's segments, the "was" bytes there), and the
file is applied whole or not at all, so a patch written for another build changes nothing. Each
change is followed by `Runtime::invalidate_code`, as a guest's own instruction cache flush is.

## Hooks and the game's own code

`core/hooks.h` runs host code when the guest reaches an address: the instruction there becomes an
`svc` to the hook, and the hook carries on with that instruction, moved to a small trampoline (or
returns from the guest function itself). libPVZ2's game code has no symbols, so rfu finds classes
by their RTTI names (`core/rtti.h`): `--trace-class <name>` hooks every virtual function of a class
and logs calls on its objects, with touches, which is how the fixes in `src/game` were found.

`src/game/hover.cpp` (`--hover`): the window sends the mouse's moves with no button down as moves of
their own pointer, and a hook on the game's WidgetManager hands them to the lawn under the cursor,
the way it hands a sliding finger's. Collectables pick up a finger sliding over them, so sun, coins
and plant food are collected by moving the mouse over them. The addresses are checked against the
code expected there (the libPVZ2 build Reflourished 1.4.2 and AltverZ share); on another build the
fix stays off.

`src/game/anim_fix.cpp` (`--fix-animations`): a plant moves from one animation to the next (a
Citron's charging to its charged idle) in its PlantFramework's special update, which runs from
`Plant::Update`; the game calls that only while the board is running, so before a Last Stand's
first wave and after the last zombie the plant stays on the last frame of what it was playing. A
hook on the plant's per-frame call runs the special update for the plants listed there when the
game skipped it, with the target check (`Plant::FindTarget`) answering no meanwhile, so it only
moves animations along and never fires.

`src/game/plant_behaviours.cpp` (`--kill-shot`, `--homing-retarget`, `--plant-id`): behaviours for new plants
that mods make from property sheets alone (a mod turns them on through mod.json's `"rfu"`), each
naming projectiles by their alias in PROJECTILETYPES. RFLib replaces the game's `Plant::DoAction`
whole (rflib_hooks.json), so the kill shot hooks `Board::AddProjectile` instead: when the
plant that fires its normal projectile is armed (and not in plant food), the props handle in `x1`
points at the special projectile's sheet in its place. `Projectile::Hit` runs after the damage is
done, so a normal projectile whose zombie is left dead (or at 0 hp) arms its plant, and a special
one that hits anything (a zombie, its shield, a grid item) disarms it. A kill shot with an
animation (`PineconeDefault=PineconeKnockback:attack2`) also hooks the call in `PlantAnimRig` that
starts the attack animation (`0x5c6030`, the label a short `std::string` "attack" on its stack in
`x1`): when the rig is an armed plant's (its weak pointer at `+0x118`), the label is written over
with the kill shot's, so that animation's `use_action` fires the special shot. `--homing-retarget` hooks
`HomingThistleLeaf`'s update: one with no target, or a target that can't be hit any more, goes for
the zombie nearest the house that the leaves already chasing it (each counting its damage) won't
finish; at debug level each retarget is logged. `--plant-id` gives a new plant the number the
profile keeps it by: `pp.dat`'s `p` (and the other per-plant lists) hold numbers from a
`std::map<std::string, int>` built into libPVZ2 (`0x11797b4`, RFLib adding its plants, 300 and
up), so a plant only in PLANTTYPES gets 0, and `Profile::AddPlant` (`0x1242314`) leaves one
bought in the shop out. Hooks on what reads the map (name to number at `0x1175f18`, whether it has
a name at `0x11761f0`, number to name at `0x1175f5c`, the name a `std::string` through `x8`, and
the find inside PlantType to number, at `0x1182d08`) answer for the plants given. The addresses are checked
against the code expected there, as for the other fixes.

`src/game/game_keys.cpp` (keybinds `seed <n>`, `shovel`, `plantfood`, `fastforward`, `pause`): the
seed bank (`SeedBankNew`), its packets and the buttons (`ShovelUI`, `PlantfoodUI`,
`FastForwardButton`, `PauseButton`) belong to the game's newer UI, not to Sexy's widgets; their
positions are relative to parents named by handles, laid out by anchors. Hooks on the bank's and
the buttons' per-frame updates keep the live instances. A key resolves the bank's children through
the game's handle registry, asks the game for each packet's rectangle on screen, and sends a touch
at the middle of the chosen one (or the button) into `LawnApp`'s TouchBegan, then TouchEnded when
the key comes up, from the main thread between frames, as `UI_ProcessEvents` would.

`--key-hints` shows those keys on what they press. After each frame the game draws, the same
rectangles (in the UI's units, `LawnApp`'s 1365x768 on a 1280x720 surface, scaled to the surface's
pixels) place a badge per key: seed keys at their packets' top left corners, button keys at the
buttons' bottom right ones. `src/platform/key_hints.cpp` lays a badge out as solid rectangles (a
border, a back and a 5x7 pixel font); `Window::draw_hints` fills them with scissored clears, so
the only GL state it touches is the framebuffer, scissor, clear colour and colour mask, which it
puts back. The letters are the key names written in the keybinds file, not the keyboard layout's.

`--controller` reads game controllers through SDL (`PadInput`: buttons by position, the left stick
as the d-pad, the triggers as buttons, and which labels the pad has, from SDL's controller type)
and plays the lawn the way PvZ1's console version does. The cursor is a tile of the Board's grid:
the grid's origin, size and columns and rows are Board fields in world units (200, 160, 9 x 64,
5 x 76 on a normal lawn), and the Board draws the world zoomed about a centre that depends on the
screen's shape (`Board` 0xab23d0: `ui = centre + floor(zoom * (trunc(1.28 * world) - centre +
board_pos))`, 1.28 being `LawnApp`+0x980). A button queues touches in the UI's units: A taps the
picked packet (unless the game's `PlantCursor` is up already) and then the tile's middle; B held a
second taps the shovel, then the tile; Y plant food, then the tile (held two seconds, the plant
food bar's "+" alone, which buys one: it's drawn by the bar, not a widget, so it's tapped a little
right of the bar's end); X the tile (a plant's own ability, then its target), or held a second in
the first column, the mower left of it; Start and L3 the pause and fast-forward buttons. LT/RT
vacuum: over a few frames, the Board's own `TouchMoved` (vtable +0x268, 0xab24c8) is told of a
finger at every 40 UI units of the screen, the way hover to collect tells it of the mouse, and
everything a sliding finger would pick up is collected. The cursor (corner brackets), a box around
the picked packet, dials that fill while B, Y or X is held and, with `--key-hints`, the buttons'
labels (A/B/X/Y, cross/circle/square/triangle, or Nintendo's) are drawn with the hints. While
something is in front of the lawn (paused, a dialog) the cursor goes away: the widget
`WidgetManager::GetWidgetAt` (0x163e82c) finds at the grid's middle isn't the Board then. (The
Board's focus byte, +0x72, isn't used: it can stay off after the window loses the focus until the
next level.) It's also hidden until the level starts (Board +0xe0, the state: 1 for the pan, 3
for the seed chooser, 5 in play).

Slowmode (R3) holds the game's time scale at half: `TimeMgr` (a lazy singleton at 0x256a010)
steps each frame by the time since the last one (at most 0.04 s) times the float at +0x30, which is
1, or 1.5 while fast-forward's on. While slowmode's on the scale is written back to 0.5 each frame
it differs (fast-forward pressed meanwhile is remembered), and the game's own value goes back when
it's turned off or the level's over (the Board gone, or back at the seed chooser).

The powerups are whatever the level has. Their buttons are `PowerupTimeUI`s, the children of
`PowerupHolderUI` (seen through its draw), taken left to right; each names its `PowerupType` with
a handle at +0x1a0, whose class name (`PowerupSnowball`, `PowerupFlamethrower`,
`PowerupFlickZombie`, `PowerupWizardFinger` for Power Zap, or a mod's own) is a std::string at
+0x20, after its alias at +0x8. A right-stick flick (past 0.7, then back under the dead zone before
the next) taps the button: left the first, up the second, right the third. `PowerupManager` (seen
through its draw) holds a handle to the powerup in use at +0x28, null when there's none. While one
is, X holds a finger down on the tile until it's let go (following the cursor, and lifted if the
powerup runs out); for the flick, two presses within 0.4 s swipe the tile left to right instead.

A plant that aims (Banana Launcher, Missile Toe, the launchers, Escape Root) puts up a cursor of
its own (`BananaTargetCursor`, `MissileToeTargetCursor`, `PlantLauncherTargetCursor`,
`EscapeRootCursor`, seen through their vtables' first function, as `PlantCursor` is). While one
is up, a finger goes down on the cursor's tile and stays (LawnApp's TouchBegan, then TouchMoved,
0x14e8578, as the cursor moves), so the plant's reticle follows it as it follows a finger on a
phone; X lets the finger go, which fires, and A slides it back onto the plant and lets go there to
call it off. X held in the first column holds a finger on the row's mower until X is let go, as
the game wants for launching one.

The seed bank's packets are drawn a little up and left of their rects (about 7 and 3 UI units on
a 119x76 packet), so the gold box and the LB/RB labels are moved by as much. A conveyor level's
bank is a `ConveyorSeedBank` (its update the bank's own), with the same `SeedPacket`s, so LB/RB
step through what's on the belt, top to bottom, as it moves. A column of packets closer together than their rects are
tall (Reflourished 1.5.0's ten slots in Roman Gardens) is drawn smaller by as much, so the
brackets shrink by the spacing over the height, from the top left; at the usual spacing they're
left as they are.

Off the lawn the pad does other things. On the pause menu (`PauseDialog`, found as the widget, or
a parent of the widget, that `GetWidgetAt` gives for the screen's middle) the d-pad moves a box
between its sliders and buttons: the dialog's Sexy widgets (`Sexy::Widget`: its children in a
`std::list` at +0x8, its parent at +0x28, x, y, width and height relative to the parent at +0x44,
shown at +0x70) that are a `Sexy::Slider` or a `PVZ2UIButton`, in rows top to bottom. Left and
right move a slider a tenth (a finger down on its thumb, from its value at +0xc0, across and up),
A taps a button, and B or Start are Android's back, which resumes. The settings (`SettingsDialog`,
from the world map or a level) work the same way, with their `PVZ2UICheckBox`es as buttons; their
rows are in a `PVZ2UIScrollingWidget`, so a row moved to that's out of its view is dragged in
first, from the list's left edge (clear of the sliders), held still before letting go. A page of
the dialog that's off the screen's side (where its sub-pages wait) is left out. Any other
`PVZ2UIDialog` with a button in it (a level's objectives before the chooser, "are you sure?") is
a menu the same way; the chooser's controls are only on while the chooser (or its look at the
lawn button) is about, since Board state 3 starts before it, with the objectives up. Lookout (the
chooser's look at the lawn) counts as the chooser while its button back (`ViewBoardOrZombiesButton`)
is updated and the seed bank isn't (both are while plants go down before "let's rock" in
Reflourished), whatever the Board's state says: after a tutorial's demonstration it reads as
playing.

While a character talks (Crazy Dave's "tap to continue"), the widget under the screen's middle
is the `CrazyNPCManager`: any button taps there, and does nothing else, so the seed chooser's or
the lawn's controls only start once the talk is over.

The worlds' carousel (`UniverseMap`) settles on a world whenever a finger lets go, so a scroll
drag held across it (let go and put down again every stroke) jerks. There left and right tap the
next `UniverseWorldButton` instead (the game glides to it), again every 0.3 s while held. The two
classes have more than one vtable, so they're told by their type_info. Reflourished 1.5.0's
libRFLib redraws the world map's buttons in another order (settings, almanac, arcade, Zen
Garden); the shortcuts follow it when libRFLib has `UIWorldMapZenGardenButton`.

Select held two seconds in a level (a dial at the screen's top) turns the free cursor on: `mode()`
gives Cursor where it would give Lawn, the d-pad and left stick move the glove (there's nothing to
scroll), LB/RB still pick the packet, A taps it first and then touches at the glove, and the
right stick's powerup flick is off. Held again, the tile box is back. In the seed chooser it's the
glove too (from the screen's middle), in place of the chooser's boxes. The title screen
(`MainMenu`) is a menu of its `PVZ2UIButton`s, starting on the one nearest the bottom's middle
(play); LB/RB swipe across the middle, which moves the banner carousel (`DraperCarouselAdWidget`)
one banner. Its pages (`ProfilesDialog`, `EditProfileWidget`) slide in on a
`PVZ2UIScrollingPaneWidget`, the others waiting off its side, so a page under the screen's middle
is the menu instead, its items placed by adding up its parents' positions; the profiles' rows
(`ProfilesListEntry`) are items, a tap on the row picking it.

The controller's cursor off the lawn is a glove (`assets/cursor.png`, embedded by
`tools/gen_cursor.py`; it presses while A's held), drawn like the hints: scissored clears, a run
of one colour at a time, the edges cut at half transparency. `--custom-cursor` makes it the
mouse's cursor too (`SDL_CreateColorCursor`), at the same size: 0.045 of the letterboxed game's
height, remade when the window's resized. Moving the mouse hides the controller's glove.

Reflourished 1.5.0's libRFLib gives the seed chooser a vtable of its own (a copy on the heap, with
no type_info), so an object counts as one of the watched classes when its vtable is the class's or
has the same first two slots and update slot.

The seed chooser (Board state 3, or drawn in the last 150 ms whatever the state: after a
tutorial's demonstration it comes up with the Board already in a playing state) isn't made of
widgets: `SeedChooser` is a UI object (drawn
through vtable slot 13, which is how it's seen) whose children are `SeedPacket`s, both the ones to
pick (with their rects scrolled with the list) and the ones picked, in the slots down the left (an
empty slot's at 0, 0). The d-pad moves a box over the packets to pick; one out of the list's view
(about 0.445 to 0.975 of the screen's height) is scrolled to with a slow drag that's held still
before it lets go, so the list doesn't fling. A taps the packet, LB/RB pick a slot, B taps the
slot's packet (which takes it out; held two seconds, every slot's from the bottom), Y taps
`ViewBoardOrZombiesButton`, LT taps `ReloadLayoutButton` (last level's packets, where the slot
count puts it), RT the almanac button (286, 37 of 768 UI units from the top left), and X held a
second taps "let's rock", which the chooser draws itself in the screen's bottom right corner (117
and 47 of 768 UI units in, at every shape). A d-pad held doesn't move the box while a scroll's
drag is still going, so the box keeps up with the list.

Everywhere else the right stick moves a cursor (a
dot, faster the further the stick is pushed: `PadInput` also brings each stick's position as
`LStick`/`RStick`), A holds a finger down there until it's let go (it drags as the cursor moves),
the d-pad or left stick held scrolls what's under the cursor (a `ScrollDrag`, as the scroll
keybinds do, each stroke lifted after a tenth of the screen's height), and B is Android's back
(not in a level, where B is the shovel; Start and L3 still press pause and fast-forward when a
banner keeps the lawn from taking touches). On the world map (the `WorldMap` widget is under the
screen's middle) the map draws its buttons itself, with no widgets to find, so LT, RT, Start and Y
tap where they're anchored, in 768ths of the screen's height: the almanac at (134, 38), the Zen
Garden at (301, 38) and the settings at (382, 38) from the top left, the shop 75 in from the right
at 53 down. A mouse click (any `TouchInput`) hides the cursor and the pad's labels until the pad is
used again.

The pad's touches go to LawnApp's handlers (`TouchBegan`/`Moved`/`Ended` thunks, 0x14e8568,
0x14e8578, 0x14e8570) as a struct laid out like the ones Android's events become: the pointer id at
+0, the point at +0x10, the event before's point at +0x18 (the down point on a began), 1 and 8 at
+0x20, the time in seconds (a double, steady clock) at +0x28, the phase at +0x30 and 13 at +0x34.
The almanac's lists measure a drag from the previous point and the time: with the current point
there, and no time, they never scrolled.

`--pad-binds` moves actions to other buttons, each screen on its own: the lawn, the seed chooser,
the menus (pause, settings, title), the world map and everything else. When a button goes down,
the screen that's up picks its table and the button is translated to the action it does there
before anything else sees it; it comes up as what it went down as, even if the screen changed in
between. So moving "take a packet" on the chooser leaves A alone on the lawn. The labels show the
button that does each action now. `--pad-layout` files make a button tap a spot (in percent of
the screen) on one screen in place of its action there, for a button rfu doesn't find by itself
(a fan mod's own, or one an update moved); with `--key-hints` the button is labelled at its spot.
`--pad-mappings` adds SDL mappings after the controller subsystem starts, so a joystick SDL didn't
know comes as a controller then; one it still doesn't know is logged with its GUID.

The world map's top buttons (settings, almanac, Zen Garden) and the shop are drawn by the map,
not widgets or button objects rfu can find, so the pad's shortcuts tap fixed spots: the 1.4.x
layout, or 1.5.0's when libRFLib has the `UIWorldMapZenGardenButton` string. `--map-buttons`
overrides any of them for a layout neither matches (pixels at 768 high, a negative x from the
right edge).

## Network

The game fetches its CDN content (`https://reflourished.ducnguyen.org/cdn/1.4.2.main.tar`, the
URL RFLib sets) through the Java class `AndroidHttpTransaction`, which rfu implements over WinHTTP
or libcurl (`src/android/java_http.cpp`). It saves the archive as
`No_Backup/CDN.<version>/tar.full` and unpacks it with libtar into the same folder (liveconfig,
event and market schedules, quests, strings, levels), then applies each file. libtar creates every
directory of a path from the root and stops at any error other than `EEXIST`, so `mkdir` answers
`EEXIST` for the directories above a mount point (`/storage`, `/data`), as Android does.

libPVZ2 also links libcurl 7.61 and OpenSSL 1.1.1 statically, so rfu provides the BSD socket layer
(`src/shims/libc_net.cpp`): `socket`, `connect`, `send`, `recv`, `getaddrinfo`, `poll`, `select`
and friends map onto host sockets (Winsock on Windows). A socket is an fd in the same table as
files, so `read`, `write`, `close` and `fcntl(O_NONBLOCK)` work on it. bionic's constants and
structs differ from Windows' (`AF_INET6`, the poll bits, `SOL_SOCKET`, errno values,
`struct addrinfo` field order), so every value is translated at the boundary. `--offline` makes
`socket()` and `getaddrinfo()` fail and the Java HTTP requests fail, and the game takes its
offline path.

Images the game fetches this way (the title screen's banners, `cdn/*.png`) are downloaded whole
before the game sees them, and kept in `<data>/image-cache`. When a later download fails or is
cut off (a slow or blocked server, `--offline`), the game gets the kept copy as a 200; a good
download replaces it. With a kept copy the game gets it at once (it gives up on a slow server
before the download would), and the download carries on in the background for the next launch,
so a changed banner shows from the launch after it gets through.

To test against a local copy of a server, set `RFU_URL_REWRITE=<prefix>=<replacement>`, e.g.
`RFU_URL_REWRITE=https://reflourished.ducnguyen.org=http://127.0.0.1:8000`.

## Java

There is no JVM. `src/jni` gives native code a JavaVM and JNIEnv whose functions work on a small
object model (classes by name, strings, arrays, instances with fields). Calls to Java methods are
logged once and answered with a neutral value (0, an empty string, or a new instance of the
declared return type) unless something implements them with `Jni::implement`, which receives the
call's Java arguments decoded from whichever of the three calling conventions native code used
(C varargs, `va_list` or a `jvalue` array).

`src/android` is the game's Java side, written from its decompiled classes
(`com.popcap.SexyAppFramework.*`, `com.popcap.PvZ2.PvZ2GameActivity`):

- **Lifecycle.** `run_app` makes the same native calls the activity does, in the same order:
  `Native_createNativeApplicationLifecycleObserver` (onCreate), `Native_GameAppInitialize` (which
  registers the remaining natives), `applicationWillFinishLaunching`/`DidFinishLaunching`,
  onResume, then the GL surface's `Native_onSurfaceCreated`/`Changed` and the focus logic of
  `AndroidGameApp.TryGainFocus`. Closing the window runs onPause and onDestroy, where the game
  saves.
- **Render loop.** `AndroidRenderer.onDrawFrame`: `PumpMessageQueue`, clear, the frame limiter set
  by `Graphics_SetFramerate`, `Native_onDrawFrame`, swap. Everything runs on the main thread, which
  is both Android's UI thread and its GL thread; the game's own threads are guest threads.
- **Keybinds.** `--keybinds` maps keys to a touch at a point of the game's surface (pointer
  ids from 100, held while the key is), to the back button, to a seed packet or the shovel
  (`GameKeyInput`, also plant food, fast-forward and pause; handled by `src/game/game_keys.cpp`), or to scrolling: a finger that goes down
  under the mouse and drags a little each frame while the key is held (`ScrollDrag`). The mouse
  wheel makes the same drags, a few frames a notch (pointer 97). `Window::poll` turns them into the
  same input as the mouse; they're off while the game's keyboard is up.
- **Input.** `AndroidUIEventManager` serialises touches, keys, text, the back button and gestures
  into a direct ByteBuffer the game passes to `UI_ProcessEvents`: the event count at offset 0,
  events from offset 16, each a type word followed by its fields (see `UiEvents` in
  `src/android/app.cpp`). As on Android, keys only go through the soft keyboard's input
  connection (Enter, Delete and digits as key events, other characters as committed text), and
  touches are dropped until the game removes the splash screen.
- **Storage.** `mount_storage` maps Android's paths onto the `--data` folder: `/data/data/<package>`
  to `data/`, `/sdcard/Android/data/<package>` to `external/`, the rest of `/sdcard` to `sdcard/`,
  and the OBB to `/sdcard/Android/obb/<package>/main.<version>.<package>.obb`.
  `Activity.getPreferences()` is `data/shared_prefs/PvZ2GameActivity.prefs`.
- **Reflourished's FilePickerHelper (1.5.0+).** `getObbDir`/`getPrivateDir` give the Android
  paths. `importFile(name)`, `importFileToDir(dir)` and `importZip(dir)` open the desktop's Open
  window before the next frame (`src/platform/file_dialog.cpp`: `GetOpenFileNameW`, zenity
  elsewhere) and copy or unpack the picked file into the external files folder, where the game
  looks for it (custom levels go to `No_Backup/CDN.<version>/levels/quests/custom<n>.json`, a save
  to `No_Backup/pp.dat.imported`); `exportFile`/`exportFolder` open a Save As window
  (`src/android/file_picker.cpp`). `forceFullAppRestart` closes the game with exit code 3, which
  the launcher answers by starting it again.
- **OBB patches.** At start-up libRFLib looks in the OBB folder for `<obb>.patch0`, `.patch1`, ...
  (rsbpatch over VCDiff), writes the patched OBB as `<obb>.new` and renames it over the OBB. The
  OBB is mounted on its own, so directory listings add the files mounted directly in a folder
  (`Vfs::mounted_children`), and `rename` copies when the OBB is on another drive and, when the
  target is open (Windows won't replace a file in use), replaces it once the game closes
  (`close_guest_files`). Java's ObbUpdater, which downloads the patches, is the launcher's
  `ObbUpdate`.
- **Library updates.** Reflourished 1.5.0+ downloads newer builds of libRFLib itself (a gzipped tar
  from its `libcdn`), installs the `.so` as `files/libRFLib.so` and restarts. Its Java side loads
  that copy instead of the APK's, and so does rfu (`staged_mod_library` in `src/android/app.cpp`),
  with the same safety net: a copy that starts 3 times without the game confirming it
  (`files/_lib_boot`) is dropped, unless `files/liboverride.txt` exists. The launcher deletes the
  staged copy when you update the game, so an old download can't shadow the new APK's library.
- **Web requests.** Besides its CDN client, the game makes some requests through Java
  (`AndroidHttpTransaction`, over `HttpURLConnection`). `src/android/java_http.cpp` runs them on
  four worker threads with the host's HTTP client (`src/platform/http.cpp`: WinHTTP on Windows,
  libcurl elsewhere) and reports back through the same natives Java calls.

## Graphics, sound and the window

`src/platform` is the only part of rfu that knows about SDL2.

- **OpenGL ES.** libPVZ2 imports 57 OpenGL ES 2.0 functions and nothing else (no buffer objects:
  every vertex array is a client-side pointer into guest memory, which the host GL reads in
  place). Each is a shim calling the host's ES implementation through `SDL_GL_GetProcAddress`, with
  guest pointers turned into host ones; `glTexImage2D`'s ninth argument comes from the guest
  stack. The context is ES 3 where available (for `glBlitFramebuffer`), else ES 2: on Windows from the driver's ES profile
  (`WGL_EXT_create_context_es2_profile`, which NVIDIA, AMD and Intel drivers have) or, with
  `--angle`, from ANGLE; on Linux from Mesa through EGL.
- **The game's surface.** The game reads its screen size once, at startup, and keeps it: it
  sizes its UI and maps touches with it, and picks its asset set from the height (the 1536
  assets from 1080 pixels up, the 768 ones below). So the game draws to an offscreen framebuffer
  of a fixed size (`--resolution`, or the desktop's shape at 1080 to 1440 pixels tall, 720 with
  `--assets low`), which `Window::swap` scales into the window, letterboxed, and mouse and touch
  input is mapped back through the same rectangle. Resizing or going full screen changes only the
  scaling. The game binds framebuffer 0 to draw to the screen; the `glBindFramebuffer` shim sends
  that to the offscreen framebuffer, and `glGetIntegerv(GL_FRAMEBUFFER_BINDING)` reports it as 0.
  Without `glBlitFramebuffer` the game draws straight to the window at the window's size.
- **Textures.** Reflourished's PTX textures are RGBA8888, RGBA4444, RGB565, ETC1 with a separate
  alpha plane, and ETC2 RGBA8. When the driver doesn't list an ETC format as a compressed texture
  format, `glCompressedTexImage2D` decodes it to RGBA8 (`src/platform/etc.cpp`, the same decoder as
  pvz2conv) and uploads that instead; `glGetString(GL_EXTENSIONS)` and the compressed-format
  queries always report ETC1 and ETC2, so the game picks the same textures as on a phone.
- **Sound.** The game's sound engine (Wwise) plays PCM through an OpenSL ES buffer-queue player.
  `src/platform/opensles.cpp` implements the engine, output mix and player objects with their
  vtables of thunks; each player is an SDL audio device whose callback copies the queued guest
  buffers out and, when one is used up, calls the game's buffer-queue callback right there, on a
  guest thread bound to SDL's audio thread. The game's sound engine waits for its buffers to come
  back and holds the frame loop up when they don't, so a machine with no sound card falls back to
  SDL's "dummy" driver, which consumes them at the real rate, rather than to no device at all.
- **Exit.** When the game stops, rfu ends the process without unwinding it, as Android does: the
  game's threads are still running and the runtime can't be torn down under them.

## Testing with scripts

`--script <file>` feeds input to the game at given frames, and takes screenshots, so a run can be
repeated without a person at the mouse (and on a machine without one). Each line is
`<frame> <action> [arguments]`, `#` starts a comment:

| Action | What |
| --- | --- |
| `tap <x> <y>` | press and release (three frames later) at pixel x, y of the game's surface (`--resolution`) |
| `drag <x1> <y1> <x2> <y2> [frames]` | press, move over `frames` frames (default 10), release |
| `text <characters>` | type, as on the soft keyboard (digits as key presses) |
| `key <android keycode>` | press and release a key: 66 Enter, 67 Delete, 7-16 digits |
| `back` | Android's back button |
| `hover <x1> <y1> <x2> <y2> [frames]` | the mouse moving with no button down, as with `--hover` |
| `screenshot <file.bmp>` | save the frame |
| `peek <hex address> <bytes>` | log guest memory as words and floats |
| `seed <n>`, `shovel`, `plantfood`, `fastforward`, `pause` | a game keybind, held for three frames |
| `scroll <dir> <frames> [speed] [x y]` | a scroll keybind held for `frames` frames, the mouse at (x, y) (the middle by default) |
| `pad <button> [frames] [xbox\|ps\|switch]` | a controller button (`a`, `b`, `x`, `y`, `lb`, `rb`, `lt`, `rt`, `l3`, `start`, `up`...) held for `frames` frames (default 3) |
| `stick <l\|r> <x> <y> [frames]` | a controller stick pushed to (x, y), each -1 to 1 (right and down positive), for `frames` frames (default 30), then let go |
| `focus on\|off` | the window getting or losing the focus (alt-tab) |
| `quit` | close the window |

rfu keeps reading the file while the game runs, so lines appended to it drive the game live; a
frame written `+N` is N frames from when the line is read (each line needs its newline).

For example, from a fresh `--data` folder on the 1.4.2 OBB with `--resolution 1280x720`, this reaches the intro:

```
800  tap 640 225     # the name box on the Welcome dialog
860  text 25
900  key 66          # Enter: accept
960  tap 476 486     # "Skip base game content?": No
1070 tap 640 656     # Play
1300 screenshot intro.bmp
1310 quit
```
