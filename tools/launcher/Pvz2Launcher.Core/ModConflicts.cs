using System.Text.Json;
using System.Text.Json.Nodes;

namespace Pvz2Launcher;

/// Mods that can't be on together, or can't be on at all, because of what their obb/ property
/// sheets add (SheetMerge): a plant, zombie or other object the mod marks "#new" that the game
/// already has by that name ("taken"), or the same new name added by two mods ("clash"). Changing
/// one of the game's own objects is never a conflict: mods' changes merge, the later one winning
/// field by field.
public static class ModConflicts
{
    /// `Other` null: the game has `Id` already. Otherwise `Mod` and `Other` both add it.
    public sealed record Conflict(string Sheet, string Id, Mods.Mod Mod, Mods.Mod? Other)
    {
        /// "plant ID "x"", "zombie ID "x"", or ""x" in SHEET".
        public string Label
        {
            get
            {
                string name = Id[(Id.IndexOf(':') + 1)..];
                string file = Path.GetFileName(Sheet.Replace('\\', '/'));
                return file switch
                {
                    "PLANTTYPES" => $"plant ID \"{name}\"",
                    "ZOMBIETYPES" => $"zombie ID \"{name}\"",
                    "PROJECTILETYPES" => $"projectile ID \"{name}\"",
                    "GRIDITEMTYPES" => $"grid item ID \"{name}\"",
                    _ => $"\"{name}\" in {file}",
                };
            }
        }

        public string Describe() => Other == null
            ? $"{Mod.Name} adds {Label}, which the game already has"
            : $"{Mod.Name} and {Other.Name} both add {Label}";
    }

    /// One object of a mod's sheet: its names, and whether the mod marks it new.
    public sealed record Entry(string Sheet, HashSet<string> Ids, bool New);

    /// The conflicts among `mods` (in load order), and with the game. Reads the game's sheets from
    /// the OBB once, then keeps their names in .launcher/obb-ids.json.
    public static List<Conflict> Find(GameFolder game, IReadOnlyList<Mods.Mod> mods)
    {
        var entries = mods.Select(m => (Mod: m, Entries: Entries(m))).Where(e => e.Entries.Count > 0).ToList();
        if (entries.Count == 0) return [];
        var gameIds = GameIds(game, entries.SelectMany(e => e.Entries.Select(x => x.Sheet)).Distinct().ToList());
        var conflicts = new List<Conflict>();
        // what each earlier mod added that the game doesn't have: sheet -> id -> mod
        var added = new Dictionary<string, Dictionary<string, Mods.Mod>>(StringComparer.Ordinal);
        foreach (var (mod, list) in entries)
        {
            var seen = new HashSet<(string, string, string?)>();
            foreach (var e in list)
            {
                var have = gameIds.GetValueOrDefault(e.Sheet) ?? [];
                if (e.Ids.Overlaps(have))
                {
                    if (e.New && seen.Add((e.Sheet, e.Ids.First(have.Contains), null)))
                        conflicts.Add(new Conflict(e.Sheet, e.Ids.First(have.Contains), mod, null));
                    continue;
                }
                if (!added.TryGetValue(e.Sheet, out var byId)) added[e.Sheet] = byId = new(StringComparer.Ordinal);
                // once for each mod it clashes with, by its first name they share
                foreach (var (id, other) in e.Ids.Where(id => byId.TryGetValue(id, out var o) && o != mod).Select(id => (id, byId[id])).DistinctBy(x => x.Item2.Key))
                    if (seen.Add((e.Sheet, id, other.Key))) conflicts.Add(new Conflict(e.Sheet, id, mod, other));
                foreach (var id in e.Ids) byId.TryAdd(id, mod);
            }
        }
        return conflicts;
    }

    /// The conflicts `mod` would have if it were switched on with the mods on now.
    public static List<Conflict> WithActive(GameFolder game, Mods.Mod mod)
    {
        if (Entries(mod).Count == 0) return [];
        var on = Mods.Active(game).Where(m => m.HasContent && m.Key != mod.Key).ToList();
        return Find(game, [.. on, mod]).Where(c => c.Mod.Key == mod.Key || c.Other?.Key == mod.Key).ToList();
    }

    /// A sheet's name: its OBB path without .rton / .json ("PACKAGES\PLANTTYPES").
    public static string SheetOf(string key)
    {
        foreach (var ext in new[] { ".RTON.JSON", ".RTON", ".JSON" })
            if (key.EndsWith(ext, StringComparison.Ordinal)) return key[..^ext.Length];
        return key;
    }

    static readonly Dictionary<string, (long Size, DateTime Time, List<Entry> Entries)> cache = new(StringComparer.OrdinalIgnoreCase);

    /// The objects of a mod's obb/ sheets (objects it takes out left aside).
    public static List<Entry> Entries(Mods.Mod mod)
    {
        string dir = Path.Combine(mod.Dir, "obb");
        var list = new List<Entry>();
        if (!Directory.Exists(dir)) return list;
        foreach (var file in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)
                     .Where(f => f.EndsWith(".json", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".rton", StringComparison.OrdinalIgnoreCase))
                     .Order(StringComparer.OrdinalIgnoreCase))
        {
            var info = new FileInfo(file);
            lock (cache)
                if (cache.TryGetValue(file, out var c) && c.Size == info.Length && c.Time == info.LastWriteTimeUtc) { list.AddRange(c.Entries); continue; }
            var found = new List<Entry>();
            try
            {
                if (SheetMerge.Parse(File.ReadAllBytes(file)) is JsonObject sheet && SheetMerge.IsSheet(sheet))
                {
                    string name = SheetOf(ObbPatch.Key(Path.GetRelativePath(dir, file)));
                    foreach (var obj in ((JsonArray)sheet["objects"]!).OfType<JsonObject>())
                        if (!SheetMerge.Flag(obj, SheetMerge.RemoveKey) && SheetMerge.Ids(obj) is { Count: > 0 } ids)
                            found.Add(new Entry(name, ids, SheetMerge.Flag(obj, SheetMerge.NewKey)));
                }
            }
            catch (Exception e) when (e is JsonException or FormatException or IOException) { }
            lock (cache) cache[file] = (info.Length, info.LastWriteTimeUtc, found);
            list.AddRange(found);
        }
        return list;
    }

    // the names of the game's objects in these sheets, from the OBB (empty for a sheet it hasn't)
    static Dictionary<string, HashSet<string>> GameIds(GameFolder game, IReadOnlyList<string> sheets)
    {
        var result = sheets.ToDictionary(s => s, _ => new HashSet<string>(StringComparer.Ordinal), StringComparer.Ordinal);
        if (game.Obb == null || !File.Exists(game.Obb)) return result;
        string path = Path.Combine(game.WorkDir, "obb-ids.json");
        string stamp = $"{game.Obb}|{new FileInfo(game.Obb).Length}|{File.GetLastWriteTimeUtc(game.Obb).Ticks}";
        var known = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        try
        {
            if (File.Exists(path) && JsonNode.Parse(File.ReadAllText(path)) is JsonObject saved && saved["obb"]?.ToString() == stamp && saved["sheets"] is JsonObject s)
                foreach (var (k, v) in s)
                    if (v is JsonArray a) known[k] = a.Select(x => x!.ToString()).ToList();
        }
        catch (Exception e) when (e is JsonException or IOException or InvalidOperationException) { known.Clear(); }

        var missing = sheets.Where(s => !known.ContainsKey(s)).ToList();
        if (missing.Count > 0)
        {
            try
            {
                var index = ObbPatch.Read(game.Obb);
                var targets = missing.Select(s => index.Files.ContainsKey(s + ".RTON") ? s + ".RTON" : s + ".JSON").ToList();
                var files = ObbPatch.ReadFiles(game.Obb, index, targets);
                foreach (var (sheet, target) in missing.Zip(targets))
                {
                    var ids = new List<string>();
                    if (files.TryGetValue(target, out var data) && SheetMerge.Parse(data) is JsonObject sheetJson && SheetMerge.IsSheet(sheetJson))
                        ids = ((JsonArray)sheetJson["objects"]!).OfType<JsonObject>().SelectMany(SheetMerge.Ids).Distinct().ToList();
                    known[sheet] = ids;
                }
                Directory.CreateDirectory(game.WorkDir);
                var save = new JsonObject { ["obb"] = stamp, ["sheets"] = new JsonObject(known.Select(k => KeyValuePair.Create(k.Key, (JsonNode?)new JsonArray(k.Value.Select(v => (JsonNode?)v).ToArray())))) };
                File.WriteAllText(path, save.ToJsonString());
            }
            catch (Exception e) when (e is FormatException or IOException or JsonException or InvalidDataException or UnauthorizedAccessException) { }
        }
        foreach (var s in sheets)
            if (known.TryGetValue(s, out var ids)) result[s].UnionWith(ids);
        return result;
    }
}
