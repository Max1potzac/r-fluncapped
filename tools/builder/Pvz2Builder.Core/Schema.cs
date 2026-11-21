using System.Text.Json;
using System.Text.Json.Nodes;

namespace Pvz2Builder;

public enum FieldKind { Null, Bool, Int, Float, String, Rtid, Array, Object }

/// What a field holds, as far as the game's data (and its class dump, when there is one) says.
public sealed class FieldSchema
{
    const int MaxValues = 200;

    public HashSet<FieldKind> Kinds { get; } = [];
    public int Seen { get; private set; }
    /// The strings seen in it, with how often (only the first 200 different ones).
    public Dictionary<string, int> Values { get; } = new(StringComparer.Ordinal);
    /// The sheets its RTIDs point into, with how often.
    public Dictionary<string, int> RtidSheets { get; } = new(StringComparer.OrdinalIgnoreCase);
    /// Members, when it holds objects; in the order first seen.
    public Dictionary<string, FieldSchema> Fields { get; } = new(StringComparer.Ordinal);
    /// What its lists hold, when it holds lists.
    public FieldSchema? Item { get; private set; }
    /// The game's own type for it, from a class dump ("float32", "RtWeakPtr<PlantPropertySheet>").
    public string? DeclaredType { get; set; }
    /// The values the game accepts, from a class dump's enums.
    public IReadOnlyList<string>? EnumValues { get; set; }

    /// The kind an editor should use: the most specific one seen (Float over Int, a dump's word over data).
    public FieldKind Main
    {
        get
        {
            if (DeclaredType is { } t)
            {
                if (t == "bool") return FieldKind.Bool;
                if (t.StartsWith("float", StringComparison.Ordinal)) return FieldKind.Float;
                if (t.StartsWith("int", StringComparison.Ordinal) || t.StartsWith("uint", StringComparison.Ordinal)) return FieldKind.Int;
                if (t.StartsWith("RtWeakPtr", StringComparison.Ordinal)) return FieldKind.Rtid;
                if (t.StartsWith("std::vector", StringComparison.Ordinal) || t.StartsWith("std::deque", StringComparison.Ordinal) || t.StartsWith("std::set", StringComparison.Ordinal)) return FieldKind.Array;
                if (t is "std::string" or "SexyString" || EnumValues != null) return FieldKind.String;
            }
            var k = Kinds.Where(x => x != FieldKind.Null).ToList();
            if (k.Count == 0) return DeclaredType != null ? FieldKind.Object : FieldKind.Null;
            if (k.Contains(FieldKind.Object)) return FieldKind.Object;
            if (k.Contains(FieldKind.Array)) return FieldKind.Array;
            if (k.Contains(FieldKind.String)) return k.Contains(FieldKind.Rtid) && Values.Count > 0 && RtidCount * 2 >= Seen ? FieldKind.Rtid : FieldKind.String;
            if (k.Contains(FieldKind.Rtid)) return FieldKind.Rtid;
            if (k.Contains(FieldKind.Float)) return FieldKind.Float;
            if (k.Contains(FieldKind.Int)) return FieldKind.Int;
            return k[0];
        }
    }

    int RtidCount => RtidSheets.Values.Sum();

    /// The values a string field takes, for a drop-down: the dump's enum, else the few values the
    /// data uses again and again (an enum the dump hasn't told us about). Null for free text.
    public IReadOnlyList<string>? Choices
    {
        get
        {
            if (EnumValues != null) return EnumValues;
            if (Main != FieldKind.String || Values.Count == 0 || Values.Count >= MaxValues) return null;
            // repeated words, not names: at most 40 of them, each used twice on average
            if (Values.Count > 40 || Seen < Values.Count * 2) return null;
            return Values.OrderByDescending(v => v.Value).ThenBy(v => v.Key, StringComparer.Ordinal).Select(v => v.Key).ToList();
        }
    }

    /// The sheet this field's RTIDs mostly point into.
    public string? RtidSheet => RtidSheets.Count > 0 ? RtidSheets.MaxBy(p => p.Value).Key : null;

    public void Observe(JsonNode? value)
    {
        Seen++;
        var kind = KindOf(value);
        Kinds.Add(kind);
        switch (kind)
        {
            case FieldKind.String:
            case FieldKind.Rtid:
                string s = value!.GetValue<string>();
                if (kind == FieldKind.Rtid && Rtid.Parse(s) is (_, string sheet) && sheet is not ("" or "." or "CurrentLevel"))
                    RtidSheets[sheet] = RtidSheets.GetValueOrDefault(sheet) + 1;
                if (Values.ContainsKey(s) || Values.Count < MaxValues) Values[s] = Values.GetValueOrDefault(s) + 1;
                break;
            case FieldKind.Object:
                foreach (var (k, v) in (JsonObject)value!)
                {
                    if (!Fields.TryGetValue(k, out var f)) Fields[k] = f = new FieldSchema();
                    f.Observe(v);
                }
                break;
            case FieldKind.Array:
                Item ??= new FieldSchema();
                foreach (var v in (JsonArray)value!) Item.Observe(v);
                break;
        }
    }

    public FieldSchema EnsureItem() => Item ??= new FieldSchema();

    public static FieldKind KindOf(JsonNode? value) => value switch
    {
        null => FieldKind.Null,
        JsonObject => FieldKind.Object,
        JsonArray => FieldKind.Array,
        JsonValue v => v.GetValueKind() switch
        {
            JsonValueKind.True or JsonValueKind.False => FieldKind.Bool,
            JsonValueKind.Number => IsWhole(v) ? FieldKind.Int : FieldKind.Float,
            JsonValueKind.String => v.GetValue<string>().StartsWith("RTID(", StringComparison.Ordinal) ? FieldKind.Rtid : FieldKind.String,
            _ => FieldKind.Null,
        },
        _ => FieldKind.Null,
    };

    static bool IsWhole(JsonValue v)
    {
        string raw = v.ToJsonString();
        return raw.IndexOfAny(['.', 'e', 'E']) < 0;
    }
}

/// A class's fields (an objclass: PlantPropertySheet, ZombieType ...).
public sealed class ClassSchema
{
    public string Name { get; }
    public string? Base { get; set; }
    public int Objects { get; set; }
    /// Fields the game's data uses, in the order first seen, then those only the dump knows.
    public FieldSchema Data { get; } = new();
    /// Fields that came from the class dump and that no object in the game's data sets.
    public HashSet<string> DumpOnly { get; } = new(StringComparer.Ordinal);
    public ClassSchema(string name) => Name = name;
    public IReadOnlyDictionary<string, FieldSchema> Fields => Data.Fields;
}

/// The field schema for one game: every objclass in its sheets and the fields each one uses, read
/// from the game's own data so it fits whatever game is open (a fan game's new classes included).
/// A class dump (decomp/m1's reflection.json) for the same build adds the game's own types, its
/// enums, and fields no object sets yet.
public sealed class Schema
{
    public Dictionary<string, ClassSchema> Classes { get; } = new(StringComparer.Ordinal);
    public int Sheets { get; private set; }
    public int ObjectCount { get; private set; }
    /// The dump's own description, when one was applied.
    public string? Dump { get; private set; }

    public ClassSchema? Class(string name) => Classes.TryGetValue(name, out var c) ? c : null;

    public static Schema Infer(Game game) => Infer(game.SheetNames().Select(n => game.Sheet(n)).OfType<JsonObject>());

    public static Schema Infer(IEnumerable<JsonObject> sheets)
    {
        var schema = new Schema();
        foreach (var sheet in sheets)
        {
            if (sheet["objects"] is not JsonArray objects) continue;
            schema.Sheets++;
            foreach (var o in objects.OfType<JsonObject>()) schema.Observe(o);
        }
        return schema;
    }

    void Observe(JsonObject o)
    {
        if (GameProfile.Str(o["objclass"]) is not { } cls || o["objdata"] is not JsonObject data) return;
        if (!Classes.TryGetValue(cls, out var c)) Classes[cls] = c = new ClassSchema(cls);
        c.Objects++;
        ObjectCount++;
        c.Data.Observe(data);
    }

    /// Adds a class dump (reflection.json: {"classes": {name: {base, fields: [{name, type}]}},
    /// "enums": {name: {member: value}}}). Returns how many classes it filled in.
    public int ApplyDump(JsonObject dump)
    {
        if (dump["classes"] is not JsonObject classes) throw new FormatException("not a class dump: it has no \"classes\"");
        var enums = new Dictionary<string, List<string>>(StringComparer.Ordinal);
        if (dump["enums"] is JsonObject e)
            foreach (var (name, members) in e)
                if (members is JsonObject m) enums[name] = m.OrderBy(p => p.Value is JsonValue v && v.TryGetValue<long>(out var n) ? n : 0).Select(p => p.Key).ToList();
        int filled = 0;
        foreach (var (name, node) in classes)
        {
            if (node is not JsonObject cls) continue;
            bool known = Classes.TryGetValue(name, out var c);
            c ??= new ClassSchema(name);
            c.Base = GameProfile.Str(cls["base"]) ?? GameProfile.Str(cls["rtclass_parent"]);
            foreach (var (fname, ftype) in DumpFields(classes, name))
            {
                if (!c.Data.Fields.TryGetValue(fname, out var f))
                {
                    c.Data.Fields[fname] = f = new FieldSchema();
                    c.DumpOnly.Add(fname);
                }
                Declare(f, ftype, enums);
            }
            // classes no object uses yet are kept too: a new object of that class can be made
            if (!known) Classes[name] = c;
            filled++;
        }
        Dump = GameProfile.Str(dump["_about"]) ?? "class dump";
        return filled;
    }

    static void Declare(FieldSchema f, string type, Dictionary<string, List<string>> enums)
    {
        f.DeclaredType = type;
        if (enums.TryGetValue(type, out var values)) f.EnumValues = values;
        // std::vector<X> and friends: the item's type
        int lt = type.IndexOf('<');
        if (lt > 0 && type.EndsWith('>') && type[..lt] is "std::vector" or "std::deque" or "std::set")
            Declare(f.EnsureItem(), type[(lt + 1)..^1], enums);
    }

    /// A class's fields with its bases', base first.
    static List<(string Name, string Type)> DumpFields(JsonObject classes, string name)
    {
        var chain = new List<JsonObject>();
        for (string? n = name; n != null && classes[n] is JsonObject c && chain.Count < 32; n = GameProfile.Str(c["base"]) ?? GameProfile.Str(c["rtclass_parent"]))
            chain.Insert(0, c);
        var result = new List<(string, string)>();
        foreach (var c in chain)
            if (c["fields"] is JsonArray fields)
                foreach (var f in fields.OfType<JsonObject>())
                    if (GameProfile.Str(f["name"]) is { } fn && GameProfile.Str(f["type"]) is { } ft) result.Add((fn, ft));
        return result;
    }
}
