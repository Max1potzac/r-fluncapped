using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Pvz2Launcher;

namespace Pvz2Builder;

/// What a mod made by the Builder says about itself (mod.json).
public sealed record ModInfo(string Name, string Description = "", string Author = "", string Version = "1.0", string Category = "Gameplay");

/// The result of saving: the mod's folder, the files written and anything worth telling.
public sealed record SaveResult(string Dir, IReadOnlyList<string> Files, IReadOnlyList<string> Notes);

/// Changes to a game's sheets, kept apart from the game's own copies and saved as a launcher mod
/// the game loads without its files being touched. Each changed object goes into the mod with just
/// the fields that differ (the launcher merges them over the game's, SheetMerge), so the mod keeps
/// working when the game updates the rest of the object, and mods stack.
public sealed class EditSession
{
    public Game Game { get; }
    readonly Dictionary<(string Sheet, int Index), JsonObject> _edited = [];

    public EditSession(Game game) => Game = game;

    /// The game's own object, as it is in its sheet.
    public JsonObject? Original(string sheet, int index) =>
        Game.Sheet(sheet)?["objects"] is JsonArray a && index >= 0 && index < a.Count ? a[index] as JsonObject : null;

    /// The object as edited so far (the game's own when it hasn't been).
    public JsonObject? Current(string sheet, int index) =>
        _edited.TryGetValue((sheet.ToUpperInvariant(), index), out var e) ? e : Original(sheet, index);

    /// A copy of the object to change; changes to it are what Save writes.
    public JsonObject Edit(string sheet, int index)
    {
        var key = (sheet.ToUpperInvariant(), index);
        if (_edited.TryGetValue(key, out var e)) return e;
        var original = Original(sheet, index) ?? throw new ArgumentOutOfRangeException(nameof(index), $"{sheet} has no object {index}");
        return _edited[key] = (JsonObject)original.DeepClone();
    }

    /// Replaces the edited copy whole (an editor that rebuilt it from text).
    public void Set(string sheet, int index, JsonObject value)
    {
        if (Original(sheet, index) == null) throw new ArgumentOutOfRangeException(nameof(index), $"{sheet} has no object {index}");
        _edited[(sheet.ToUpperInvariant(), index)] = value;
    }

    public bool IsChanged(string sheet, int index) =>
        _edited.TryGetValue((sheet.ToUpperInvariant(), index), out var e) && !JsonNode.DeepEquals(e, Original(sheet, index));

    public void Revert(string sheet, int index) => _edited.Remove((sheet.ToUpperInvariant(), index));

    public void RevertAll() => _edited.Clear();

    public IReadOnlyList<(string Sheet, int Index)> Changed =>
        _edited.Keys.Where(k => IsChanged(k.Sheet, k.Index)).OrderBy(k => k.Sheet, StringComparer.Ordinal).ThenBy(k => k.Index).ToList();

    /// A folder name made from a mod's name.
    public static string FolderName(string name)
    {
        var sb = new StringBuilder();
        // what Windows refuses in a name, wherever the Builder runs
        foreach (char c in name.Trim()) sb.Append(c < ' ' || "<>:\"/\\|?*".Contains(c) ? '_' : c);
        return sb.Length == 0 ? "Builder mod" : sb.ToString().TrimEnd('.', ' ');
    }

    /// Writes the changes as a mod in `modsDir`\<name> (a game's own mods folder, so it's for this
    /// game alone): mod.json, obb\PACKAGES\<SHEET>.json for the OBB's sheets, cdn\... for the CDN's.
    /// Saving again over the same folder replaces what the Builder wrote there before.
    public SaveResult Save(string modsDir, ModInfo info)
    {
        var notes = new List<string>();
        string dir = Path.Combine(modsDir, FolderName(info.Name));
        Directory.CreateDirectory(dir);

        // what an earlier save wrote, so a sheet no longer changed doesn't linger
        string record = Path.Combine(dir, "builder.json");
        if (File.Exists(record))
            try
            {
                if (JsonNode.Parse(File.ReadAllText(record))?["files"] is JsonArray old)
                    foreach (var f in old.Select(GameProfile.Str).OfType<string>())
                    {
                        string full = Path.GetFullPath(Path.Combine(dir, f));
                        if (full.StartsWith(Path.GetFullPath(dir), StringComparison.Ordinal) && File.Exists(full)) File.Delete(full);
                    }
            }
            catch (JsonException) { }

        var files = new List<string>();
        foreach (var group in Changed.GroupBy(k => k.Sheet))
        {
            string sheet = group.Key;
            JsonObject content;
            string rel;
            if (Game.SourceOf(sheet) == "cdn" && Game.CdnDir != null && FindCdnFile(sheet) is { } cdnFile)
            {
                // the CDN's files are laid over the game's whole, not merged (but LawnStrings)
                content = WholeSheet(sheet);
                rel = Path.Combine("cdn", Path.GetRelativePath(Game.CdnDir, cdnFile) + ".json");
                notes.Add($"{sheet}: the game reads it from its CDN folder, so the mod has the whole sheet (redo the save after the game updates it)");
            }
            else
            {
                string? why;
                (content, why) = SheetPatch(sheet, group.Select(k => k.Index).ToList());
                if (why != null) notes.Add($"{sheet}: {why}, so the mod has the whole sheet");
                rel = Path.Combine("obb", "PACKAGES", sheet + ".json");
            }
            string path = Path.Combine(dir, rel);
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content.ToJsonString(Indented));
            files.Add(rel.Replace('\\', '/'));
        }

        var mod = new JsonObject
        {
            ["id"] = Slug(info.Name),
            ["name"] = info.Name,
            ["description"] = info.Description,
            ["author"] = info.Author,
            ["version"] = info.Version,
            ["kind"] = "content",
            ["category"] = info.Category,
            ["games"] = new JsonArray(Game.Package.Length > 0 ? Game.Package : Game.Family),
        };
        if (MajorMinor(Game.Version) is { } v) mod["versions"] = new JsonArray(v);
        File.WriteAllText(Path.Combine(dir, "mod.json"), mod.ToJsonString(Indented));
        File.WriteAllText(record, new JsonObject
        {
            ["game"] = Game.Name,
            ["package"] = Game.Package,
            ["version"] = Game.Version,
            ["files"] = new JsonArray(files.Select(f => (JsonNode?)f).ToArray()),
        }.ToJsonString(Indented));
        return new SaveResult(dir, files, notes);
    }

    static readonly JsonSerializerOptions Indented = new() { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping };

    static string? MajorMinor(string version)
    {
        var parts = version.Split('.');
        return parts.Length >= 2 && parts[0].Length > 0 ? $"{parts[0]}.{parts[1]}" : null;
    }

    static string Slug(string name)
    {
        var sb = new StringBuilder();
        foreach (char c in name.ToLowerInvariant()) sb.Append(char.IsAsciiLetterOrDigit(c) ? c : '-');
        string s = sb.ToString().Trim('-');
        return s.Length == 0 ? "builder-mod" : s;
    }

    string? FindCdnFile(string sheet) =>
        Game.CdnDir == null ? null : Directory.EnumerateFiles(Game.CdnDir, "*.rton", SearchOption.AllDirectories)
            .FirstOrDefault(f => Path.GetFileNameWithoutExtension(f).Equals(sheet, StringComparison.OrdinalIgnoreCase));

    JsonObject WholeSheet(string sheet)
    {
        var whole = (JsonObject)Game.Sheet(sheet)!.DeepClone();
        var objects = (JsonArray)whole["objects"]!;
        foreach (var (s, i) in Changed.Where(k => k.Sheet == sheet)) objects[i] = Current(s, i)!.DeepClone();
        return whole;
    }

    /// The sheet a mod needs for these changed objects: each object named the way the launcher's
    /// merge finds it, with only its changed fields. When that can't say it (an object with no
    /// name, a field taken away), the whole sheet with "#replace", and why.
    (JsonObject Sheet, string? Why) SheetPatch(string sheet, List<int> indices)
    {
        var objects = (JsonArray)Game.Sheet(sheet)!["objects"]!;
        var patch = new JsonArray();
        foreach (int i in indices)
        {
            var orig = (JsonObject)objects[i]!;
            var now = Current(sheet, i)!;
            var removed = new List<string>();
            var entry = new JsonObject();
            if (GameProfile.Str(now["objclass"]) is { } cls) entry["objclass"] = cls;
            if (!JsonNode.DeepEquals(orig["objclass"], now["objclass"])) return (Replace(sheet), "an object's class changed");
            string? alias = GameProfile.FirstAlias(orig);
            string? typeName = GameProfile.Str(orig["objdata"]?["TypeName"]);
            if (alias != null) entry["aliases"] = new JsonArray(alias);
            else if (typeName == null && objects.OfType<JsonObject>().Count(o => JsonNode.DeepEquals(o["objclass"], orig["objclass"])) != 1)
                return (Replace(sheet), "a changed object has no alias to find it by");
            // aliases added (never taken away: a merge can't)
            if (now["aliases"] is JsonArray na && orig["aliases"] is JsonArray oa && !JsonNode.DeepEquals(na, oa))
            {
                if (oa.Any(x => !na.Any(y => JsonNode.DeepEquals(x, y)))) return (Replace(sheet), "an alias was taken away");
                entry["aliases"] = na.DeepClone();
            }
            var data = Diff(orig["objdata"] as JsonObject ?? [], now["objdata"] as JsonObject ?? [], removed, "");
            if (removed.Count > 0) return (Replace(sheet), $"{string.Join(", ", removed)} was taken away");
            if (alias == null && typeName != null) data["TypeName"] = typeName;
            entry["objdata"] = data;
            foreach (var (k, v) in now)
                if (k is not ("objclass" or "aliases" or "objdata") && !JsonNode.DeepEquals(v, orig[k]))
                    return (Replace(sheet), $"its {k} changed");
            patch.Add(entry);
        }
        return (new JsonObject { ["version"] = 1, ["objects"] = patch }, null);
    }

    JsonObject Replace(string sheet)
    {
        var whole = WholeSheet(sheet);
        var result = new JsonObject { [SheetMerge.ReplaceKey] = true };
        foreach (var (k, v) in whole) result[k] = v?.DeepClone();
        return result;
    }

    /// The fields of `now` that differ from `orig`: objects compared field by field, anything
    /// else (numbers, strings, lists) whole. Fields `now` lacks go into `removed`.
    public static JsonObject Diff(JsonObject orig, JsonObject now, List<string> removed, string path)
    {
        var result = new JsonObject();
        foreach (var (k, v) in now)
        {
            if (!orig.TryGetPropertyValue(k, out var o)) { result[k] = v?.DeepClone(); continue; }
            if (JsonNode.DeepEquals(o, v)) continue;
            if (o is JsonObject oo && v is JsonObject vo)
            {
                var d = Diff(oo, vo, removed, path + k + ".");
                if (d.Count > 0) result[k] = d;
            }
            else result[k] = v?.DeepClone();
        }
        foreach (var (k, _) in orig)
            if (!now.ContainsKey(k)) removed.Add(path + k);
        return result;
    }

    /// Takes up a mod the Builder (or anyone) made for this game: its obb\PACKAGES sheets are
    /// merged over the game's the way the launcher does it, and every object that comes out
    /// different becomes an edit. Returns notes on what couldn't be taken up.
    public List<string> LoadMod(string modDir)
    {
        var notes = new List<string>();
        string packages = Path.Combine(modDir, "obb", "PACKAGES");
        if (!Directory.Exists(packages)) { notes.Add("the mod has no obb\\PACKAGES sheets"); return notes; }
        foreach (var file in Directory.EnumerateFiles(packages, "*.json"))
        {
            string name = Path.GetFileNameWithoutExtension(file);
            if (name.EndsWith(".rton", StringComparison.OrdinalIgnoreCase)) name = name[..^5];
            name = name.ToUpperInvariant();
            if (Game.Sheet(name) is not { } own) { notes.Add($"{name}: this game has no such sheet"); continue; }
            JsonObject? mine;
            try { mine = SheetMerge.Parse(File.ReadAllBytes(file)) as JsonObject; }
            catch (Exception e) when (e is JsonException or FormatException) { notes.Add($"{name}: {e.Message}"); continue; }
            if (mine == null || !SheetMerge.IsSheet(mine)) { notes.Add($"{name}: not a property sheet"); continue; }
            var merged = SheetMerge.TakeReplace(mine) ? mine : (JsonObject)own.DeepClone();
            if (!ReferenceEquals(merged, mine)) SheetMerge.Merge(merged, mine);
            var before = (JsonArray)own["objects"]!;
            var after = (JsonArray)merged["objects"]!;
            int n = Math.Min(before.Count, after.Count);
            for (int i = 0; i < n; i++)
                if (after[i] is JsonObject o && !JsonNode.DeepEquals(o, before[i])) Set(name, i, (JsonObject)o.DeepClone());
            if (after.Count > before.Count) notes.Add($"{name}: {after.Count - before.Count} new object(s) left out (adding new objects comes with the import tool)");
        }
        return notes;
    }
}
