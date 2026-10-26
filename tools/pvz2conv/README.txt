pvz2conv - PvZ2 APK / OBB unpacker and RTON / PAM / POPFX / PTX converter
==========================================================================

HOW TO USE
  Drag the game's .apk and .obb onto pvz2conv.exe (together or one at a time).

  The .apk gives "<apk name>_extracted" with:
    lib\arm64-v8a\           the native libraries; point rfu at this folder
                              with  rfu --lib-dir <...>\lib\arm64-v8a
    assets\                  the APK's assets, if any (.dex files left out)
    _apk_manifest.json       package, version, the OBB name it expects and
                             the SHA-256 of every library
  The Java side (classes.dex, res\) isn't used by rfu and stays in the APK, so
  once this has run the APK itself isn't needed. A file that is really an APK
  is recognised whatever its extension (.zip too). An XAPK / APKS bundle isn't
  unpacked: take the base .apk and the .obb out of it first. A 32-bit-only APK
  or one missing libPVZ2 / libRFLib / libc++_shared / libNimble is reported as
  a failure. When an APK and an OBB are dropped together, pvz2conv checks the
  OBB is the one the APK asks for (main.<version code>.<package>.obb).

  The .obb is unpacked into a folder named
  "<obb name>_extracted" next to it, in the game's own folder layout
  (ATLASES, IMAGES, PACKAGES, SOUNDBANKS ...), and converted on the way:
    ATLASES\X.PTX              ->  ATLASES\X.png
    PACKAGES\X.RTON            ->  PACKAGES\X.json
    IMAGES\...\X.PAM           ->  IMAGES\...\X.pam.json
    X.POPFX                    ->  X.popfx.json
  Everything else (sound banks, text, XML) is written as it is.
  Add --converted-only to leave out the originals of files that converted.

  You can also drop RTON / PAM / POPFX files or folders, as before: each file
  gets a JSON file next to it, and a folder is searched recursively.

  A console window shows progress; press Enter to close it at the end.
  _pvz2conv_log.txt is written into the output folder with everything that
  happened, and an OBB also gets _obb_manifest.json: every composite group,
  every resource group with its files, compression and texture formats.

  Files are recognised by their signature, not their extension. A file or a
  group that fails is reported and skipped; it never stops the run.

OPTIONS (command prompt)
  pvz2conv.exe <.apk, .obb, file or folder> ...
  --out D:\dump        write everything under this folder (an .apk and an
                       .obb can share one: lib\ and assets\ sit beside the
                       game's own folders)
  --converted-only     don't keep the original of a file that converted
                       (for an OBB the PTX / RTON / PAM / POPFX is never
                       written; for loose files the original is deleted).
                       --json-only is the old name and still works.
  --only Zombie        OBB: only groups whose name contains "Zombie"
  --list               OBB: list the groups, extract nothing
  --no-convert         OBB: extract the files as they are
  --skip-existing      don't overwrite JSON that's already there
  --no-pause           don't wait for Enter (for scripts and the launcher)

OBB / RSB
  The .obb is an RSB ("1bsr", version 4 in Reflourished): a header of tables
  (groups, composites, pools, texture formats, name tries), then one packet
  ("pgsr") per resource group with part 0 (ordinary files) and part 1
  (textures). Also accepted: a bare .rsb, an .rsb.smf (zlib-wrapped) and an
  .obb that is really a zip with the RSB inside. An .obb that is cut short
  extracts every group that is complete; a header-only file is listed.
  What it does differently from stock extractors:
    * Each block's compression is read from its own signature (zstd, zlib,
      gzip, raw deflate or stored), not from the packet's flags. Reflourished
      uses zstd, and pads small compressed blocks to 4 KB so they look stored.
    * Name tables: mod repackers leave pointers the game never follows. A
      pointer is only taken when it lands exactly where the alternative
      branch must begin, or when it repeats one an enclosing character
      already owns (Reflourished's PACKAGES and StreamingWave tables), so
      no duplicate files under glued folder names.
    * A file that sits in several groups is written once.
    * Paths longer than 260 characters are written anyway. A single name
      longer than 120 characters is shortened (start, ~, a hash, extension)
      and the log says which; a file that can't be written is logged and
      the rest of its group carries on.

PTX
  A PTX has no header; its size and format come from the RSB, so PNGs are
  made while unpacking (a loose .PTX can't be converted on its own).
    0    RGBA8888                    verified on Reflourished
    147  ETC1 + 8-bit alpha plane    verified on Reflourished
    150  ETC2 RGBA8 (EAC alpha)      verified on Reflourished
    1/2/3     RGBA4444 / RGB565 / RGBA5551            not seen yet
    21/22/23  the same in 32x32 tiles                 not seen yet
  Other formats (PVRTC, ASTC, ETC1 palette alpha) are kept as .PTX and named
  in the log; none appeared in the part of the Reflourished OBB tested.

RTON / PAM / POPFX
  RTON   "RTON" + version 1 ... "DONE". PopCap's binary JSON: typed values,
         varints, a string cache (0x90/0x91, 0x92/0x93) and RTID references,
         written as "RTID(uid@Sheet)" the way other community tools do.
         Encrypted RTON (starts with 10 00) is detected and skipped for now.
  PAM    PopAnim, magic 54 19 F0 BA, versions 1 to 6. Written with the same
         field names as Sen / TwinStar: image (name, size, transform),
         sprite and main_sprite (frames with remove / append / change,
         label, stop, command). Reflourished cuts the trailing zero bytes
         off some PAMs; those are read back as zeros. Others are zero-padded to
         4 KB, which is ignored. Coordinates are converted from 1/20 px to px
         and matrices from 16.16 fixed point to decimals.
  POPFX  "xfcp" version 1, a compiled render effect (shader). Its layout was
         worked out from a single sample, so the output has the string table,
         the shaders with their GLSL source, and every block as raw numbers
         (block_1 ... block_8) so nothing is lost while the rest is identified.

CREDITS
  The OBB and PTX layouts were checked against RipeConsole by FranZ
  (github.com/CyberKnightFran45/RipeConsole, GPL-3.0) and Sen; pvz2conv
  contains no code from either. zstd decoding: ZstdSharp (MIT).

BUILDING
  .NET 8 SDK:  dotnet publish -c Release -o publish
  gives one self-contained publish\pvz2conv.exe (no .NET install needed).
