using System.Text.Json;
using System.Text.Json.Nodes;

namespace Pvz2Builder;

/// One thing a game has: a plant, zombie, grid item, projectile or level module, as its own
/// sheets define it.
public sealed record CatalogEntry(
    string Kind, string Alias, string Class, string Sheet, int Index, string? DisplayName,
    string? Properties, string? PopAnim, IReadOnlyList<string> ResourceGroups, string? HomeWorld)
{
    public override string ToString() => DisplayName is { Length: > 0 } n ? $"{n} ({Alias})" : Alias;
}

/// A game's catalogue, built from its own files only: the plants, zombies, grid items,
/// projectiles and level modules it really has, so every list the Builder shows (a level's zombie
/// pool, a plant picker) holds what this game can load and nothing from another game.
public sealed class GameProfile
{
    public const string Plants = "Plants", Zombies = "Zombies", GridItems = "Grid items", Projectiles = "Projectiles", LevelModules = "Level modules";
    public static readonly string[] Kinds = [Plants, Zombies, GridItems, Projectiles, LevelModules];

    public Game Game { get; }
    public IReadOnlyDictionary<string, string> Strings { get; }
    readonly Dictionary<string, List<CatalogEntry>> _entries = [];

    public IReadOnlyList<CatalogEntry> this[string kind] => _entries.TryGetValue(kind, out var l) ? l : [];

    GameProfile(Game game)
    {
        Game = game;
        Strings = game.Strings();
    }

    public static GameProfile Build(Game game)
    {
        var p = new GameProfile(game);
        p.Add(Plants, "PLANTTYPES", "PlantType", "PlantFramework", "PlantResourceGroups", ["{0}", "PLANT_{0}"]);
        p.Add(Zombies, "ZOMBIETYPES", "ZombieType", "ZombieClass", "ResourceGroups", ["ZOMBIE_{0}", "{0}"]);
        p.Add(GridItems, "GRIDITEMTYPES", "GridItemType", "GridItemClass", "ResourceGroups", ["GRIDITEM_{0}", "{0}"]);
        p.Add(Projectiles, "PROJECTILETYPES", null, "ClassName", null, []);
        p.Add(LevelModules, "LEVELMODULES", null, null, null, []);
        return p;
    }

    void Add(string kind, string sheetName, string? objclass, string? classField, string? groupsField, string[] nameKeys)
    {
        var list = new List<CatalogEntry>();
        _entries[kind] = list;
        if (Game.Sheet(sheetName)?["objects"] is not JsonArray objects) return;
        for (int i = 0; i < objects.Count; i++)
        {
            if (objects[i] is not JsonObject o) continue;
            string? cls = Str(o["objclass"]);
            if (cls == null || (objclass != null && cls != objclass)) continue;
            var data = o["objdata"] as JsonObject;
            string? alias = FirstAlias(o) ?? Str(data?["TypeName"]);
            if (alias == null) continue;
            string typeName = Str(data?["TypeName"]) ?? alias;
            string? display = null;
            foreach (var k in nameKeys)
                if (Strings.TryGetValue(string.Format(k, typeName.ToUpperInvariant()), out var s)) { display = s; break; }
            var groups = groupsField != null && data?[groupsField] is JsonArray g ? g.Select(Str).OfType<string>().ToList() : [];
            list.Add(new CatalogEntry(kind, alias, (classField != null ? Str(data?[classField]) : null) ?? cls, sheetName, i, display,
                Str(data?["Properties"]), Str(data?["PopAnim"]), groups, Str(data?["HomeWorld"])));
        }
    }

    public static string? FirstAlias(JsonObject o) => o["aliases"] is JsonArray a && a.Count > 0 ? Str(a[0]) : null;

    public static string? Str(JsonNode? n) => n is JsonValue v && v.GetValueKind() == JsonValueKind.String ? v.GetValue<string>() : null;

    /// The object an RTID ("RTID(PeashooterDefault@PlantProperties)") points at: its sheet, its
    /// place there and the object. "@." means the sheet it's written in.
    public (string Sheet, int Index, JsonObject Obj)? Resolve(string rtid, string fromSheet)
    {
        if (Rtid.Parse(rtid) is not (string alias, string sheet)) return null;
        if (sheet is "." or "") sheet = fromSheet;
        if (Game.Sheet(sheet)?["objects"] is not JsonArray objects) return null;
        Dictionary<string, int>? index;
        lock (_aliasIndex)
            if (!_aliasIndex.TryGetValue(sheet, out index))
            {
                _aliasIndex[sheet] = index = new Dictionary<string, int>(StringComparer.Ordinal);
                for (int i = 0; i < objects.Count; i++)
                    if (objects[i] is JsonObject o && o["aliases"] is JsonArray a)
                        foreach (var x in a)
                            if (Str(x) is { } name) index.TryAdd(name, i);
            }
        return index.TryGetValue(alias, out int at) && objects[at] is JsonObject found ? (sheet.ToUpperInvariant(), at, found) : null;
    }

    readonly Dictionary<string, Dictionary<string, int>> _aliasIndex = new(StringComparer.OrdinalIgnoreCase);

    /// Every alias a sheet defines, for an RTID picker.
    public IReadOnlyList<string> AliasesIn(string sheet)
    {
        if (Game.Sheet(sheet)?["objects"] is not JsonArray objects) return [];
        return objects.OfType<JsonObject>().SelectMany(o => o["aliases"] is JsonArray a ? a.Select(Str).OfType<string>() : []).ToList();
    }

    /// The entry with this alias, in any kind.
    public CatalogEntry? Find(string kind, string alias) => this[kind].FirstOrDefault(e => e.Alias.Equals(alias, StringComparison.OrdinalIgnoreCase));
}

/// RTID(alias@Sheet), the way RTON references another object.
public static class Rtid
{
    public static (string Alias, string Sheet)? Parse(string? s)
    {
        if (s == null || !s.StartsWith("RTID(", StringComparison.Ordinal) || !s.EndsWith(')')) return null;
        string inner = s[5..^1];
        int at = inner.LastIndexOf('@');
        return at < 0 ? (inner, "") : (inner[..at], inner[(at + 1)..]);
    }

    public static string Make(string alias, string sheet) => $"RTID({alias}@{sheet})";
}
