using System.Text.Json.Nodes;
using Pvz2Launcher;

namespace Pvz2Builder;

/// One game the Builder works on: EA's PvZ2, Reflourished, AltverZ or any other fan game on the
/// international release. Everything the Builder offers is read from this game's own files (its
/// OBB, and the CDN content it downloaded last), never from another game's.
public sealed class Game : IDisposable
{
    public ObbArchive Obb { get; }
    /// The launcher's folder for the game (games\<name>), when it was opened from one.
    public GameFolder? Folder { get; }
    public string Name { get; }
    /// The Android package ("com.ea.game.pvz2_rfl"), or "" when unknown.
    public string Package { get; }
    /// The game's version ("1.5.0"), or "" when unknown.
    public string Version { get; }
    /// What mods call this kind of game: reflourished, pvz2 (EA's own) or other (fan games).
    public string Family { get; }
    /// The game's own libraries beside libPVZ2 (libRFLib, libSnowie ...): its extra classes.
    public IReadOnlyList<string> ModLibraries { get; }
    /// The CDN folder whose files the game reads over the OBB's (newer levels and strings), if any.
    public string? CdnDir { get; }

    readonly Dictionary<string, JsonObject?> _sheets = new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<string, string> _cdnFiles = new(StringComparer.OrdinalIgnoreCase);

    Game(ObbArchive obb, GameFolder? folder, string name, string package, string version, IReadOnlyList<string> libs, string? cdn)
    {
        Obb = obb;
        Folder = folder;
        Name = name;
        Package = package;
        Version = version;
        Family = FamilyOf(package);
        ModLibraries = libs;
        CdnDir = cdn != null && Directory.Exists(cdn) ? cdn : null;
        if (CdnDir != null)
            foreach (var f in Directory.EnumerateFiles(CdnDir, "*.rton", SearchOption.AllDirectories))
                _cdnFiles.TryAdd(Path.GetFileNameWithoutExtension(f), f);
    }

    /// A game made of sheets alone, for tests.
    internal static Game FromSheets(IReadOnlyDictionary<string, JsonObject> sheets, string package = "com.example.test", string version = "1.0.0")
    {
        var g = new Game(null!, null, "test", package, version, [], null);
        foreach (var (name, sheet) in sheets) g._sheets[name] = sheet;
        return g;
    }

    public static string FamilyOf(string package) => package switch
    {
        "com.ea.game.pvz2_rfl" => "reflourished",
        "com.ea.game.pvz2_row" or "com.ea.game.pvz2_na" or "com.ea.game.pvz2_googleplay" or "com.ea.game.pvz2_kr" => "pvz2",
        "com.popcap.pvz2cthdlx" => "china",
        _ => "other",
    };

    /// A game folder the launcher made (games\<name>, with the game's .apk and .obb).
    public static Game OpenFolder(string dir)
    {
        var folder = GameFolder.Open(dir);
        if (folder.IsChina) throw new NotSupportedException("PvZ2 China isn't supported by the Builder (only the international game and its fan versions are)");
        if (folder.Obb == null) throw new FileNotFoundException($"{folder.Name} has no .obb");
        var obb = ObbArchive.Open(folder.Obb);
        var (pkg, _) = FromObbName(folder.Obb);
        string package = folder.Info?.Package is { Length: > 0 } p ? p : pkg;
        string cdn = Path.Combine(folder.DataDir, "external", "files", "No_Backup", Mods.CdnFolderName(folder));
        return new Game(obb, folder, folder.DisplayName, package, folder.Info?.VersionName ?? "", folder.ModLibraries, cdn);
    }

    /// A bare .obb ("main.675.com.ea.game.pvz2_rfl.obb"): the package comes from its name.
    public static Game OpenObb(string path)
    {
        var (package, code) = FromObbName(path);
        var obb = ObbArchive.Open(path);
        string name = package.Length > 0 ? $"{package} ({code})" : Path.GetFileNameWithoutExtension(path);
        return new Game(obb, null, name, package, "", [], null);
    }

    /// Opens a launcher game folder or an .obb, whichever `path` is.
    public static Game Open(string path) => Directory.Exists(path) ? OpenFolder(path) : OpenObb(path);

    /// main.<version code>.<package>.obb -> (package, version code); ("", "") for any other name.
    public static (string Package, string Code) FromObbName(string path)
    {
        var parts = path[(path.LastIndexOfAny(['/', '\\']) + 1)..].Split('.');
        if (parts.Length >= 4 && parts[0].Equals("main", StringComparison.OrdinalIgnoreCase) && int.TryParse(parts[1], out _))
            return (string.Join('.', parts[2..^1]), parts[1]);
        return ("", "");
    }

    /// The names of the game's property sheets (PACKAGES\*.RTON in the OBB, plus the CDN's
    /// sheets), upper case: PLANTTYPES, ZOMBIEPROPERTIES ...
    public IReadOnlyList<string> SheetNames()
    {
        var names = new SortedSet<string>(StringComparer.Ordinal);
        if (Obb == null) { lock (_sheets) return _sheets.Keys.Select(k => k.ToUpperInvariant()).Order(StringComparer.Ordinal).ToList(); }
        foreach (var f in Obb.Files)
            if (f.StartsWith(@"PACKAGES\", StringComparison.Ordinal) && f.EndsWith(".RTON", StringComparison.Ordinal) && f.IndexOf('\\', 9) < 0)
                names.Add(f[9..^5]);
        return names.ToList();
    }

    /// Where a sheet is read from: "cdn" when the game's CDN folder has a newer copy, else "obb".
    public string SourceOf(string sheet) => _cdnFiles.ContainsKey(sheet) ? "cdn" : "obb";

    /// A property sheet ({"version": 1, "objects": [...]}) as JSON, by name ("PLANTTYPES",
    /// "PlantProperties"), or null when the game has none. The CDN's copy wins over the OBB's.
    public JsonObject? Sheet(string name)
    {
        lock (_sheets)
        {
            if (_sheets.TryGetValue(name, out var cached)) return cached;
            byte[]? data = _cdnFiles.TryGetValue(name, out var cdnFile) ? File.ReadAllBytes(cdnFile) : Obb?.Read($@"PACKAGES\{name}.RTON");
            JsonObject? sheet = null;
            if (data != null)
                try { sheet = SheetMerge.Parse(data) as JsonObject; }
                catch (Exception e) when (e is System.Text.Json.JsonException or FormatException) { }
            _sheets[name] = sheet;
            return sheet;
        }
    }

    /// The game's text in a language ("en-US"): its LawnStrings, key -> text. Newer games keep it in
    /// a sheet (PACKAGES\LAWNSTRINGS-EN-US.RTON), older ones in LOCALES\EN-US\PROPERTIES\LAWNSTRINGS.TXT.
    public IReadOnlyDictionary<string, string> Strings(string language = "en-US")
    {
        lock (_strings)
        {
            if (_strings.TryGetValue(language, out var cached)) return cached;
            var result = new Dictionary<string, string>(StringComparer.Ordinal);
            if (Sheet(StringsSheet(language)) is { } sheet && sheet["objects"] is JsonArray objects)
            {
                foreach (var o in objects.OfType<JsonObject>())
                {
                    var values = o["objdata"]?["LocStringValues"];
                    if (values is JsonArray pairs)
                        for (int i = 0; i + 1 < pairs.Count; i += 2)
                        {
                            if (pairs[i]?.GetValueKind() == System.Text.Json.JsonValueKind.String && pairs[i + 1]?.GetValueKind() == System.Text.Json.JsonValueKind.String)
                                result[pairs[i]!.GetValue<string>()] = pairs[i + 1]!.GetValue<string>();
                        }
                    else if (values is JsonObject map)
                        foreach (var (k, v) in map)
                            if (v?.GetValueKind() == System.Text.Json.JsonValueKind.String) result[k] = v.GetValue<string>();
                }
            }
            else if (Obb?.Read(StringsTxt(language)) is { } txt)
                foreach (var (k, v) in StringsText.Parse(txt)) result[k] = v;
            return _strings[language] = result;
        }
    }

    readonly Dictionary<string, Dictionary<string, string>> _strings = new(StringComparer.OrdinalIgnoreCase);

    /// The sheet a language's strings are in, when the game keeps them in one ("LAWNSTRINGS-EN-US").
    public static string StringsSheet(string language) => "LAWNSTRINGS-" + language.ToUpperInvariant();

    /// The older games' strings file in the OBB.
    public static string StringsTxt(string language) => $@"LOCALES\{language.ToUpperInvariant()}\PROPERTIES\LAWNSTRINGS.TXT";

    /// Whether the game's strings are a sheet (newer games) rather than a .txt (older ones).
    public bool StringsInSheet(string language = "en-US") => Sheet(StringsSheet(language)) != null;

    public void Dispose() => Obb?.Dispose();
}
