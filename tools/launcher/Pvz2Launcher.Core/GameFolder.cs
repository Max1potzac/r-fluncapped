using System.Text.Json;

namespace Pvz2Launcher;

/// A game's folder under games/: the .obb and .apk the user put there, what pvz2conv took out
/// of the APK (lib/arm64-v8a, icon.png, _apk_manifest.json) and what the launcher keeps:
///
///   game.json       the menu's settings
///   keybinds.txt    keys for seed packets, the shovel, or spots of the screen (rfu --keybinds)
///   patches/        code patch files, each one a toggle (rfu --patch)
///   mods/           custom content, one folder per mod (see Mods)
///   data/           the game's saves and downloads (rfu --data)
///   unpacked/       the OBB's resources, when asked for (pvz2conv)
///   .launcher/      built mods and the update check's cache
public sealed class GameFolder
{
    public string Dir { get; }
    public string Name => Path.GetFileName(Dir);
    public GameSettings Settings { get; private set; } = new();

    public string? Obb { get; private set; }
    public string? Apk { get; private set; }
    public ApkInfo? Info { get; private set; }

    public string SettingsPath => Path.Combine(Dir, "game.json");
    public string LibDir => Path.Combine(Dir, "lib", "arm64-v8a");
    public string IconPath => Path.Combine(Dir, "icon.png");
    public string KeybindsPath => Path.Combine(Dir, "keybinds.txt");
    public string PatchesDir => Path.Combine(Dir, "patches");
    public string ModsDir => Path.Combine(Dir, "mods");
    public string DataDir => Path.Combine(Dir, "data");
    public string UnpackedDir => Path.Combine(Dir, "unpacked");
    public string WorkDir => Path.Combine(Dir, ".launcher");
    public string LogPath => Path.Combine(Dir, "rfu-log.txt");

    public string DisplayName => Settings.DisplayName ?? Name;
    public bool HasIcon => File.Exists(IconPath);
    /// The mod's own libraries (libRFLib, libSnowie...): part of the game, so always loaded.
    public IReadOnlyList<string> ModLibraries => Info?.ModLibraries ?? [];
    public bool LibrariesReady => File.Exists(Path.Combine(LibDir, "libPVZ2.so"));

    /// The APK's libraries need taking out: there are none yet, or the APK is newer than them.
    public bool NeedsApkUnpack =>
        Apk != null && (!LibrariesReady || File.GetLastWriteTimeUtc(Apk) > File.GetLastWriteTimeUtc(ManifestPath));

    string ManifestPath => Path.Combine(Dir, "_apk_manifest.json");

    GameFolder(string dir) { Dir = Path.GetFullPath(dir); }

    public static GameFolder Open(string dir)
    {
        var g = new GameFolder(dir);
        g.Refresh();
        return g;
    }

    /// Every game under `gamesDir` (a folder each), by name.
    public static List<GameFolder> Scan(string gamesDir)
    {
        Directory.CreateDirectory(gamesDir);
        return Directory.EnumerateDirectories(gamesDir)
            .Where(d => !Path.GetFileName(d).StartsWith('.'))
            .Select(Open)
            .OrderBy(g => g.DisplayName, StringComparer.OrdinalIgnoreCase)
            .ToList();
    }

    /// Looks at the folder again: the OBB and APK, the manifest, the settings.
    public void Refresh()
    {
        // The newest .obb and .apk (a folder normally has one of each; an update adds a newer one).
        Obb = Newest("*.obb");
        Apk = Newest("*.apk");
        Info = ApkInfo.Read(ManifestPath);
        bool first = !File.Exists(SettingsPath);
        Settings = first ? GameSettings.Defaults(Info?.Package, Name) : GameSettings.Load(SettingsPath);
        // saved once the APK says which game this is, so the defaults fit it
        if (first && Info != null) Settings.Save(SettingsPath);
        // settings made before the APK was read: the CDN address the game's defaults have
        // (an empty one is the person switching the check off, and stays)
        if (!first && Info != null && Settings.CdnUrl == null) Settings.CdnUrl = GameSettings.Defaults(Info.Package, Name).CdnUrl;
    }

    public void SaveSettings() => Settings.Save(SettingsPath);

    string? Newest(string pattern) =>
        Directory.EnumerateFiles(Dir, pattern).OrderByDescending(File.GetLastWriteTimeUtc).FirstOrDefault();

    /// What stops the game from starting, or null.
    public string? Problem()
    {
        if (Obb == null) return "There's no .obb in this game's folder. Put the game's OBB (main.<number>.<package>.obb) there.";
        if (!LibrariesReady)
            return Apk == null
                ? "The game's libraries are missing. Put the game's .apk in its folder and the launcher takes them out."
                : "The game's libraries haven't been taken out of the APK yet.";
        if (Info?.Obb != null && !string.Equals(Info.Obb, Path.GetFileName(Obb), StringComparison.OrdinalIgnoreCase))
            return $"The APK expects {Info.Obb}, but the OBB here is {Path.GetFileName(Obb)}. Use the OBB from the same version.";
        return null;
    }

    /// The code patch files, with the name and description from their "# name:" lines.
    public List<PatchFile> PatchFiles() =>
        Directory.Exists(PatchesDir)
            ? Directory.EnumerateFiles(PatchesDir, "*.patch").Order(StringComparer.OrdinalIgnoreCase).Select(PatchFile.Read).ToList()
            : [];

    /// Writes a keybinds.txt that explains itself, if there isn't one.
    public void EnsureKeybindsFile()
    {
        if (File.Exists(KeybindsPath)) return;
        File.WriteAllText(KeybindsPath, """
            # Keybinds: a key acts as a finger on a spot of the screen while you hold it.
            #   <key> seed <n>       the n-th seed packet, counted from 1 at the top of the seed bank
            #   <key> shovel         the shovel (press it again to put the shovel back)
            #   <key> plantfood      the plant food button (then click a plant)
            #   <key> fastforward    the fast-forward button (on and off)
            #   <key> pause          the pause button (again to carry on)
            #   <key> scroll <dir>   scrolls what's under the mouse left, right, up or down while held;
            #                        add a speed (pixels a frame, or 3% of the screen's height) to change it
            #   <key> tap <x> <y>    x and y in pixels of the game's screen, or in percent (50% 50% is the middle)
            #   <key> back           Android's back button
            # seed and the buttons are found wherever the level puts them.
            # The mouse wheel scrolls too (with Shift held, sideways).
            # Key names are SDL's (A, 1, Space, F1, Left_Shift, Keypad_1); write _ for a space.
            # Escape is always the back button and F11 always switches full screen.
            # Lines that start with # are ignored.

            1 seed 1
            2 seed 2
            3 seed 3
            4 seed 4
            5 seed 5
            6 seed 6
            7 seed 7
            8 seed 8
            9 seed 9
            D shovel
            F plantfood
            V fastforward
            P pause
            Left scroll left
            Right scroll right
            Up scroll up
            Down scroll down

            # Space tap 50% 50%
            # Backspace back

            """);
    }
}

/// What pvz2conv read from the APK (_apk_manifest.json).
public sealed record ApkInfo(string Package, string VersionName, string VersionCode, string? Obb, IReadOnlyList<string> ModLibraries)
{
    public static ApkInfo? Read(string path)
    {
        if (!File.Exists(path)) return null;
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            var r = doc.RootElement;
            string Get(string name) => r.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString()! : "";
            var mods = r.TryGetProperty("mod_libraries", out var m) && m.ValueKind == JsonValueKind.Array
                ? m.EnumerateArray().Where(e => e.ValueKind == JsonValueKind.String).Select(e => e.GetString()!).ToList()
                : [];
            return new ApkInfo(Get("package"), Get("version_name"), Get("version_code"), Get("obb") is { Length: > 0 } o ? o : null, mods);
        }
        catch (JsonException) { return null; }
    }
}

/// A code patch file: rfu applies it; the launcher shows its name and description as a toggle.
public sealed record PatchFile(string Path, string FileName, string Name, string Description)
{
    public static PatchFile Read(string path)
    {
        string name = System.IO.Path.GetFileNameWithoutExtension(path), description = "";
        foreach (var line in File.ReadLines(path).Take(20))
        {
            var t = line.Trim();
            if (!t.StartsWith('#')) continue;
            t = t.TrimStart('#').Trim();
            if (t.StartsWith("name:", StringComparison.OrdinalIgnoreCase)) name = t[5..].Trim();
            else if (t.StartsWith("description:", StringComparison.OrdinalIgnoreCase)) description = t[12..].Trim();
        }
        return new PatchFile(path, System.IO.Path.GetFileName(path), name, description);
    }
}
