using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Pvz2Launcher;

/// Custom content. A mod is a folder in the game's mods/ folder, laid out like the game's own
/// files, which rfu lays over them read-only (--overlay), so nothing of the game's is changed and
/// switching a mod off is just not passing it:
///
///   mods/<mod>/mod.json     optional: { "name", "description", "author", "version" }
///   mods/<mod>/cdn/...      over the game's CDN folder (No_Backup/CDN.<version>: levels, strings,
///                           liveconfig ...), the content the game downloads and RFLib reads
///   mods/<mod>/files/...    over the game's external files folder (what Android calls
///                           /sdcard/Android/data/<package>/files)
///
/// A mod with neither cdn/ nor files/ is all CDN content, as level packs ship it: loose level files
/// (anything with a LevelDefinition) go to levels/, a LawnStrings file to the CDN folder itself.
/// Files are converted on the way to what the game expects: x.json becomes x.rton where the game's
/// own file is x.rton, and x.rton.json always becomes x.rton. Mods later in the load order win
/// where two have the same file, except LawnStrings: a mod's strings are merged over the game's
/// current ones (and over earlier mods'), so text the mod doesn't have isn't lost, and the game's
/// CDN download no longer wipes them out.
public static class Mods
{
    public sealed record Mod(string Dir, string FolderName, string Name, string Description, string Author, string Version);

    static readonly string[] Roots = ["cdn", "files"];
    static readonly HashSet<string> NotContent = new(StringComparer.OrdinalIgnoreCase) { "mod.json", "readme.txt", "readme.md", "license", "license.txt" };

    public static List<Mod> List(GameFolder game) =>
        Directory.Exists(game.ModsDir)
            ? Directory.EnumerateDirectories(game.ModsDir).Where(d => !Path.GetFileName(d).StartsWith('.')).Select(Read)
                .OrderBy(m => m.FolderName, StringComparer.OrdinalIgnoreCase).ToList()
            : [];

    public static Mod Read(string dir)
    {
        string folder = Path.GetFileName(dir);
        string name = folder, description = "", author = "", version = "";
        var manifest = Path.Combine(dir, "mod.json");
        if (File.Exists(manifest))
        {
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(manifest), new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
                string Get(string key) => doc.RootElement.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString()! : "";
                name = Get("name") is { Length: > 0 } n ? n : folder;
                description = Get("description");
                author = Get("author");
                version = Get("version");
            }
            catch (JsonException) { }
        }
        return new Mod(dir, folder, name, description, author, version);
    }

    /// What a folder the user picked as custom content holds.
    public enum ContentKind { Mod, StandaloneGame, Empty }

    public static ContentKind Classify(string dir)
    {
        if (Directory.EnumerateFiles(dir, "*.obb", SearchOption.TopDirectoryOnly).Any()) return ContentKind.StandaloneGame;
        return Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Any() ? ContentKind.Mod : ContentKind.Empty;
    }

    /// Copies a custom content folder into the game's mods/ and returns its folder name there.
    public static string Import(GameFolder game, string source)
    {
        source = Path.GetFullPath(source).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        string baseName = Path.GetFileName(source), name = baseName;
        for (int i = 2; Directory.Exists(Path.Combine(game.ModsDir, name)); i++) name = $"{baseName} ({i})";
        CopyTree(source, Path.Combine(game.ModsDir, name));
        return name;
    }

    /// One --overlay for rfu: a guest directory and the host directory laid over it.
    public sealed record Overlay(string Guest, string Host);

    public sealed record BuildResult(List<Overlay> Overlays, List<string> Log);

    /// The CDN folder the game uses: the newest No_Backup/CDN.* it has made, else CDN.<version>.
    public static string CdnFolderName(GameFolder game)
    {
        var noBackup = Path.Combine(game.DataDir, "external", "files", "No_Backup");
        var existing = Directory.Exists(noBackup)
            ? Directory.EnumerateDirectories(noBackup, "CDN.*").OrderByDescending(Directory.GetLastWriteTimeUtc).FirstOrDefault()
            : null;
        return existing != null ? Path.GetFileName(existing) : $"CDN.{game.Info?.VersionName ?? "1.0.0"}";
    }

    /// Builds the enabled mods, in load order, into .launcher/overlay and returns what to pass rfu.
    /// Skips the work when nothing changed since the last build.
    public static BuildResult Build(GameFolder game, IReadOnlyList<string> enabled)
    {
        var log = new List<string>();
        var mods = enabled.Select(n => Path.Combine(game.ModsDir, n)).Where(Directory.Exists).ToList();
        foreach (var missing in enabled.Where(n => !Directory.Exists(Path.Combine(game.ModsDir, n))))
            log.Add($"mod {missing} isn't in the mods folder any more; left out");
        if (mods.Count == 0) return new BuildResult([], log);

        string package = game.Info?.Package is { Length: > 0 } p ? p : "com.ea.game.pvz2_rfl";
        string cdnName = CdnFolderName(game);
        string filesGuest = $"/sdcard/Android/data/{package}/files";
        var guest = new Dictionary<string, string> { ["cdn"] = $"{filesGuest}/No_Backup/{cdnName}", ["files"] = filesGuest };
        var baseDirs = new Dictionary<string, string>
        {
            ["cdn"] = Path.Combine(game.DataDir, "external", "files", "No_Backup", cdnName),
            ["files"] = Path.Combine(game.DataDir, "external", "files"),
        };

        // (root, path in the game's folder) -> the mods' files for it, in load order. The last one
        // wins, but LawnStrings are merged, all of them, over the game's own.
        var plan = new Dictionary<(string Root, string Rel), List<string>>(new PathKeyComparer());
        foreach (var mod in mods)
        {
            bool laidOut = Roots.Any(r => Directory.Exists(Path.Combine(mod, r)));
            foreach (var root in Roots)
            {
                string src = laidOut ? Path.Combine(mod, root) : root == "cdn" ? mod : "";
                if (src == "" || !Directory.Exists(src)) continue;
                foreach (var file in Directory.EnumerateFiles(src, "*", SearchOption.AllDirectories))
                {
                    string rel = Path.GetRelativePath(src, file);
                    bool loose = !laidOut && rel == Path.GetFileName(rel);
                    if (loose && NotContent.Contains(rel)) continue;
                    if (loose && !IsStrings(rel) && IsLevel(file)) rel = Path.Combine("levels", rel);
                    var key = (root, Target(rel, baseDirs[root]));
                    if (!plan.TryGetValue(key, out var sources)) plan[key] = sources = [];
                    if (!IsStrings(rel)) sources.Clear();
                    sources.Add(file);
                }
            }
        }

        string outDir = Path.Combine(game.WorkDir, "overlay");
        string stampPath = Path.Combine(game.WorkDir, "overlay.stamp");
        string stamp = Stamp(plan, guest, baseDirs);
        bool fresh = File.Exists(stampPath) && File.ReadAllText(stampPath) == stamp && Directory.Exists(outDir);
        if (!fresh)
        {
            if (Directory.Exists(outDir)) Directory.Delete(outDir, true);
            int converted = 0, copied = 0, failed = 0;
            foreach (var ((root, rel), sources) in plan)
            {
                string dest = Path.Combine(outDir, root, rel);
                string src = sources[^1];
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                try
                {
                    string gameFile = Path.Combine(baseDirs[root], rel);
                    int merged = IsStrings(rel) && File.Exists(gameFile) ? TryMergeStrings(gameFile, sources, dest, log) : -1;
                    if (merged >= 0)
                    {
                        log.Add($"{rel}: {merged} strings from {sources.Count} mod(s) merged over the game's");
                        converted++;
                    }
                    else if (rel.EndsWith(".rton", StringComparison.OrdinalIgnoreCase) && src.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
                    {
                        File.WriteAllBytes(dest, Pvz2Conv.RtonWriter.FromJson(File.ReadAllBytes(src)));
                        converted++;
                    }
                    else
                    {
                        File.Copy(src, dest, true);
                        copied++;
                    }
                }
                catch (Exception e) when (e is JsonException or FormatException or IOException)
                {
                    log.Add($"FAIL {src}: {e.Message}");
                    failed++;
                }
            }
            log.Add($"built {mods.Count} mod(s): {copied} files copied, {converted} converted or merged{(failed > 0 ? $", {failed} failed" : "")}");
            File.WriteAllText(stampPath, failed == 0 ? stamp : "");
        }
        else
        {
            log.Add($"{mods.Count} mod(s) unchanged since the last build");
        }

        var overlays = Roots.Where(r => Directory.Exists(Path.Combine(outDir, r)))
            .Select(r => new Overlay(guest[r], Path.Combine(outDir, r))).ToList();
        return new BuildResult(overlays, log);
    }

    /// Where a mod's file goes: x.rton.json -> x.rton; x.json -> x.rton when the game's file is
    /// an RTON (keeping its spelling); anything else as it is.
    internal static string Target(string rel, string baseDir) => GameSpelling(Convert(rel, baseDir), baseDir);

    static string Convert(string rel, string baseDir)
    {
        if (rel.EndsWith(".rton.json", StringComparison.OrdinalIgnoreCase)) return rel[..^5];
        if (rel.EndsWith(".json", StringComparison.OrdinalIgnoreCase) && !rel.EndsWith(".pam.json", StringComparison.OrdinalIgnoreCase)
            && !rel.EndsWith(".popfx.json", StringComparison.OrdinalIgnoreCase))
        {
            string stem = rel[..^5];
            string? dir = Path.GetDirectoryName(Path.Combine(baseDir, stem));
            if (dir != null && Directory.Exists(dir))
            {
                string want = Path.GetFileName(stem) + ".rton";
                var match = Directory.EnumerateFiles(dir).FirstOrDefault(f => string.Equals(Path.GetFileName(f), want, StringComparison.OrdinalIgnoreCase));
                if (match != null) return Path.Combine(Path.GetDirectoryName(stem) ?? "", Path.GetFileName(match));
            }
        }
        return rel;
    }

    // Everything a build depends on: the mods' files, and the game's strings they're merged over
    // (which change whenever the game downloads its CDN content).
    // The game's own spelling of a file the mod spells differently (LawnStrings-en-us.rton for the
    // game's LawnStrings-en-US.rton), so the mod's file replaces it on any file system.
    static string GameSpelling(string rel, string baseDir)
    {
        string? dir = Path.GetDirectoryName(Path.Combine(baseDir, rel));
        if (dir == null || !Directory.Exists(dir)) return rel;
        string name = Path.GetFileName(rel);
        var match = Directory.EnumerateFiles(dir).Select(Path.GetFileName).FirstOrDefault(f => string.Equals(f, name, StringComparison.OrdinalIgnoreCase));
        return match == null ? rel : Path.Combine(Path.GetDirectoryName(rel) ?? "", match);
    }

    static string Stamp(Dictionary<(string Root, string Rel), List<string>> plan, Dictionary<string, string> guest, Dictionary<string, string> baseDirs)
    {
        var sb = new StringBuilder();
        void File(string path)
        {
            var info = new FileInfo(path);
            sb.Append(path).Append(' ').Append(info.Exists ? info.Length : -1).Append(' ').Append(info.Exists ? info.LastWriteTimeUtc.Ticks : 0).Append('\n');
        }
        foreach (var g in guest) sb.Append(g.Key).Append('=').Append(g.Value).Append('\n');
        foreach (var ((root, rel), sources) in plan.OrderBy(p => p.Key.Root).ThenBy(p => p.Key.Rel, StringComparer.Ordinal))
        {
            sb.Append(root).Append('/').Append(rel).Append('\n');
            sources.ForEach(File);
            if (IsStrings(rel)) File(Path.Combine(baseDirs[root], rel));
        }
        return System.Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(sb.ToString())));
    }

    static bool IsStrings(string rel) => Path.GetFileName(rel).StartsWith("LawnStrings", StringComparison.OrdinalIgnoreCase);

    static readonly byte[] LevelMarker = Encoding.ASCII.GetBytes("LevelDefinition");

    static bool IsLevel(string file)
    {
        string ext = Path.GetExtension(file).ToLowerInvariant();
        if (ext is not (".json" or ".rton")) return false;
        try { return System.IO.File.ReadAllBytes(file).AsSpan().IndexOf(LevelMarker) >= 0; }
        catch (IOException) { return false; }
    }

    // MergeStrings, or -1 when either side isn't a strings list (the mod's file then replaces the game's).
    static int TryMergeStrings(string gameFile, IReadOnlyList<string> mods, string dest, List<string> log)
    {
        try { return MergeStrings(gameFile, mods, dest); }
        catch (FormatException e)
        {
            log.Add($"{Path.GetFileName(gameFile)}: not merged ({e.Message}); the mod's file replaces it");
            return -1;
        }
    }

    /// The game's LawnStrings with each mod's strings put over them (later mods win), written to
    /// `dest` in the game's format (RTON or JSON, and its LocStringValues layout: the flat
    /// [key, value, ...] list Reflourished uses, or a key: value object). Returns how many keys the
    /// mods set.
    internal static int MergeStrings(string gameFile, IReadOnlyList<string> mods, string dest)
    {
        var (gameDoc, gameStrings) = ReadStrings(gameFile);
        var merged = new Dictionary<string, string>(StringComparer.Ordinal);
        var order = new List<string>();
        void Put(string k, string v)
        {
            if (!merged.ContainsKey(k)) order.Add(k);
            merged[k] = v;
        }
        foreach (var (k, v) in gameStrings) Put(k, v);
        var touched = new HashSet<string>(StringComparer.Ordinal);
        foreach (var mod in mods)
            foreach (var (k, v) in ReadStrings(mod).Strings)
            {
                Put(k, v);
                touched.Add(k);
            }

        // write the game's document back with the merged list in place of its own
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms, new JsonWriterOptions { Indented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }))
            WriteReplacing(gameDoc.RootElement, w, order, merged);
        gameDoc.Dispose();
        byte[] json = ms.ToArray();
        System.IO.File.WriteAllBytes(dest, dest.EndsWith(".rton", StringComparison.OrdinalIgnoreCase) ? Pvz2Conv.RtonWriter.FromJson(json) : json);
        return touched.Count;
    }

    static (JsonDocument Doc, List<(string Key, string Value)> Strings) ReadStrings(string file)
    {
        byte[] data = System.IO.File.ReadAllBytes(file);
        if (Pvz2Conv.Rton.LooksLike(data))
        {
            using var ms = new MemoryStream();
            using (var w = new Utf8JsonWriter(ms)) Pvz2Conv.Rton.ToJson(data, w, []);
            data = ms.ToArray();
        }
        var doc = JsonDocument.Parse(data, new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
        var list = new List<(string, string)>();
        if (FindLocStrings(doc.RootElement) is { } values)
        {
            if (values.ValueKind == JsonValueKind.Array)
            {
                var items = values.EnumerateArray().Select(e => e.ValueKind == JsonValueKind.String ? e.GetString()! : e.GetRawText()).ToList();
                for (int i = 0; i + 1 < items.Count; i += 2) list.Add((items[i], items[i + 1]));
            }
            else if (values.ValueKind == JsonValueKind.Object)
            {
                foreach (var p in values.EnumerateObject()) list.Add((p.Name, p.Value.ValueKind == JsonValueKind.String ? p.Value.GetString()! : p.Value.GetRawText()));
            }
        }
        else
        {
            doc.Dispose();
            throw new FormatException($"{Path.GetFileName(file)} has no LocStringValues");
        }
        return (doc, list);
    }

    static JsonElement? FindLocStrings(JsonElement e)
    {
        if (e.ValueKind == JsonValueKind.Object)
        {
            foreach (var p in e.EnumerateObject())
            {
                if (p.Name == "LocStringValues") return p.Value;
                if (FindLocStrings(p.Value) is { } v) return v;
            }
        }
        else if (e.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in e.EnumerateArray())
                if (FindLocStrings(item) is { } v) return v;
        }
        return null;
    }

    static void WriteReplacing(JsonElement e, Utf8JsonWriter w, List<string> order, Dictionary<string, string> merged)
    {
        switch (e.ValueKind)
        {
            case JsonValueKind.Object:
                w.WriteStartObject();
                foreach (var p in e.EnumerateObject())
                {
                    w.WritePropertyName(p.Name);
                    if (p.Name == "LocStringValues" && p.Value.ValueKind == JsonValueKind.Array)
                    {
                        w.WriteStartArray();
                        foreach (var k in order) { w.WriteStringValue(k); w.WriteStringValue(merged[k]); }
                        w.WriteEndArray();
                    }
                    else if (p.Name == "LocStringValues" && p.Value.ValueKind == JsonValueKind.Object)
                    {
                        w.WriteStartObject();
                        foreach (var k in order) w.WriteString(k, merged[k]);
                        w.WriteEndObject();
                    }
                    else WriteReplacing(p.Value, w, order, merged);
                }
                w.WriteEndObject();
                break;
            case JsonValueKind.Array:
                w.WriteStartArray();
                foreach (var item in e.EnumerateArray()) WriteReplacing(item, w, order, merged);
                w.WriteEndArray();
                break;
            default:
                e.WriteTo(w);
                break;
        }
    }

    static void CopyTree(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var dir in Directory.EnumerateDirectories(from, "*", SearchOption.AllDirectories))
            Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, dir)));
        foreach (var file in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
            File.Copy(file, Path.Combine(to, Path.GetRelativePath(from, file)), true);
    }

    // The game's files live on Android's case-sensitive storage, but two mods that spell a path
    // differently must still replace each other on Windows.
    sealed class PathKeyComparer : IEqualityComparer<(string Root, string Rel)>
    {
        public bool Equals((string Root, string Rel) a, (string Root, string Rel) b) =>
            a.Root == b.Root && string.Equals(a.Rel.Replace('\\', '/'), b.Rel.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase);
        public int GetHashCode((string Root, string Rel) k) => HashCode.Combine(k.Root, k.Rel.Replace('\\', '/').ToUpperInvariant());
    }
}
