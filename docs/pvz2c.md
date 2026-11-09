# PvZ2 China (PVZ2C): file structure

Notes from taking apart the PvZ2 China APK `com.popcap.pvz2cthdlx`, version 4.2.4 (built
2026-09-23). Everything here was checked against that APK; `tools/pvz2conv` unpacks all of it.

## The APK

PVZ2C has no OBB: the game's data ships inside the APK, in `assets/`.

| Path | What it is |
| --- | --- |
| `lib/arm64-v8a/libSrc.so` (99 MB) | The whole game in one library: PvZ2 engine, Lua 5.1.4, LZMA SDK, Tencent MSDK glue. Needs only libEGL, libGLESv3, libz, liblog, libOpenSLES, libm, libdl, libc. 3,905 dynamic symbols (2,460 global functions), so it is stripped like the international `libPVZ2.so`. |
| `lib/arm64-v8a/libBugly_Native.so`, `libkeyprovider.so`, `libsecsdk.so` | Tencent crash reporting and security SDKs, not game code. |
| `lib/armeabi-v7a/` | The same four libraries for 32-bit ARM. |
| `assets/*.rsb.smf` (33 files, 2.2 GB) | The game's packages. Each is an RSLB container around an ordinary RSB (below). |
| `assets/*.rsb.tag.smf` | 32 hex digits, an MD5-sized tag per package, used by the updater. |
| `assets/properties/resources*.rton.smf` (18) | Plain RTON resource manifests for the world packages (the same data as `PROPERTIES/RESOURCES*.RTON` inside each package). |
| `assets/ConfigManifest.json.smf`, `UpdateManifest.json.smf`, `LuaManifest.json.smf` | Plain JSON: hot-update channels `config/` (1.9.2), `update/` (8) and `lua/` (1.9.1), each with a remote manifest URL. The game downloads updated packages over these at run time. |
| `assets/*.xml`, `payconfig`, `supplierconfig.json`, `com.tencent.open.config.json` | Plain-text payment, ad, share and channel settings (Tencent, WeChat). |
| `assets/zlsioh.dat`, `key.cer`, `libwbsafeedit*` | Security SDK data, not game data. |
| `classes*.dex`, `res/` | The Java side (Tencent MSDK, payments, ads). |

`.smf` here is only a suffix: `ConfigManifest.json.smf` is plain JSON and `resourcesBeach.rton.smf`
is plain RTON. Only the `.rsb.smf` packages are wrapped, and not in the zlib SMF format the
international game uses.

## RSLB: the package wrapper

The game's own error messages call this "block-lzma" (`RSB Error: Could not block-lzma uncompress
resource stream bundle`). Little-endian:

| Offset | Size | Field |
| --- | --- | --- |
| 0x00 | 4 | `RSLB` |
| 0x04 | 4 | version, 1 |
| 0x08 | 8 | size of the RSB inside |
| 0x10 | 8 | size of everything after the header |
| 0x18 | 4 | block size, 0x02000000 (32 MB) |
| 0x1C | 4 | block count |
| 0x20 | 36 × count | per block: u64 offset in the RSB, u64 size there, u64 offset in this file, u32 stored size, 5 bytes LZMA properties (`5D` + u32 dictionary size), 3 zero bytes |

Each block is its 5 property bytes again, then a raw LZMA1 stream with no length field. Decoded and
joined in order, the blocks make an ordinary RSB.

## RSB

The RSB inside is the same `1bsr` version 4 the international game and Reflourished use: same
header tables, `pgsr` packets with part 0 (files) and part 1 (textures). Nothing about the RSB itself
differs. Every package has only the 1200 resolution.

| Package | .rsb.smf | RSB | Groups | Composites | Textures | Files |
| --- | --- | --- | --- | --- | --- | --- |
| init | 401 MB | 579 MB | 79 | 57 | 44 | 1,163 |
| dynamic | 65 MB | 140 MB | 29 | 26 | 10 | 8,036 |
| LuaAct | 25 MB | 75 MB | 15 | 13 | 14 | 106 |
| update | 116 MB | 322 MB | 30 | 28 | 50 | 92 |
| uicommon | 91 MB | 330 MB | 94 | 67 | 77 | 208 |
| uiactive, uiactive2, uiactive3 | 196 MB | 676 MB | 239 | 177 | 188 | 465 |
| plant1, plant2, plant3 | 303 MB | 1,158 MB | 922 | 595 | 343 | 2,038 |
| zombie1, zombie2, zombie3 | 228 MB | 833 MB | 965 | 561 | 414 | 1,464 |
| 20 worlds (egypt ... renai) | 785 MB | 2,330 MB | 572 | 368 | 371 | 1,410 |

The 20 world packages are atlantis, beach, cowboy, dark, dino, egypt, eighties, future, heian,
iceage, kongfu, lostcity, modern, moon, pirate, renai, skycity, steam and twister (twister also
holds the BGM and event sound banks). No file path appears in two packages with different contents
(14,763 unique paths), so the packages can be extracted into one folder.

What's where:

- `dynamic/PACKAGES/`: the game data, all encrypted RTON. 141 top-level sheets (`PLANTTYPES` with
  385 plants, `ZOMBIETYPES` with 1,050, `PROPERTYSHEETS`, `GAMEFEATURES`, `LEVELMODULES`,
  `WORLDMAPLIST*`, the Rift, Joust, PvP, gacha and shop configs ...), `LEVELS/` (6,612 levels:
  3,008 `PVZ*`, 1,606 `RIFT_*`, the world levels, events), `WORLDS/` (687), `LAYOUT/` (365 UI
  layouts) and `HOTUI/` (174).
- `dynamic/PROPERTIES/`: text, not RTON. `LAWNSTRINGS.TXT` holds the game's strings as
  `[KEY]` / value pairs in UTF-8 (the PvZ1 format), next to `LAWNSTRINGSFONT.TXT`,
  `RANDOMSTRINGS.TXT`, `FILTERSTRINGS.TXT` (the chat filter), `CREDITS.TXT`, `DEFAULT.XML`,
  `LAYOUT.XML` and `CONFIGUI/*.INI` (UI definitions).
- `LuaAct/`: `LUA/*.LUA`, 43 plain-text Lua 5.1 scripts for events and UI (anniversary, monthly
  card, boss challenge, shops ...), with their RTON layouts and `PROPERTIES/LAWNSTRINGSLUA.TXT`.
- `init/`: startup art, `EFFECTS/` (POPFX shaders), `PRIMEFONTS/` (four TrueType fonts: FZ
  Chinese faces), `INIT_SOUNDBANKS/` and `INIT_STREAMINGWAVES/` (WEM music).
- Every package: `ATLASES/` (textures), `IMAGES/1200/...` (PAM animations), `*_SOUNDBANKS/`
  (Wwise BNK) and `PROPERTIES/RESOURCES<PACKAGE>.RTON` (its resource manifest, plain RTON).

Totals: 8,104 RTON, 3,819 PAM, 1,494 textures, 507 BNK, 793 WEM, 10 POPFX, 43 Lua, 4 TTF and about
120 text, INI and XML files.

## Encrypted RTON

8,053 of the RTON files (all of `PACKAGES/`) are encrypted. Only the resource manifests are plain.

- The file is the bytes `10 00`, then the RTON encrypted with Rijndael in CBC mode with a
  **192-bit block** and 256-bit key, zero-padded to a whole 24-byte block.
- Key: the 32 lowercase ASCII hex digits of MD5(`com_popcap_pvz2_magento_product_2013_05_05`).
  The string is in `libSrc.so`.
- IV: key characters 4 to 27 (24 bytes).

This is the same scheme community tools know from older PvZ2 China versions. Decrypting gives a
normal RTON version 1 that pvz2conv already reads; `pvz2conv --to-rton --encrypt` re-encrypts
(checked: what it writes decrypts back byte for byte, and RTON -> JSON -> encrypted RTON -> JSON
gives the same JSON for the game's files).

## Textures

PTX is headerless as everywhere else; format and size come from the RSB texture table.

| Format | Count | What it is |
| --- | --- | --- |
| 161 | 1,422 | ASTC 5x5 (LDR): exactly ceil(w/5) × ceil(h/5) 16-byte blocks; many start with the ASTC void-extent block `FC FD FF FF ...` |
| 162 | 70 | ASTC 6x6, same reasoning |
| 147 | 1 | ETC1 with **palette alpha**: after the ETC1 blocks, a count n (16 here), n 4-bit alpha values, then each pixel's palette index in ceil(log2 n) bits, high bits first (`WORLDMAP_MODERN_NONPVR`) |
| 0 | 1 | RGBA8888 |

The international formats (ETC1 + A8, ETC2 RGBA) don't appear. pvz2conv writes the ASTC textures
as standard `.astc` files (16-byte header + the blocks), which astcenc
(`astcenc -dl X.astc X.png`) and other tools open; it doesn't decode ASTC to PNG itself.

## Unchanged from the international game

PAM (versions up to 6, as in Reflourished), POPFX, Wwise BNK and WEM, and the RSB layout all
decode with the existing code unchanged.
