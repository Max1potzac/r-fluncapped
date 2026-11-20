using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Pvz2Launcher;

/// PvZ2's property sheets ({"version": 1, "objects": [...]}, PLANTTYPES, ZOMBIEPROPERTIES ...)
/// merged, so mods add to the game's sheet and to each other's rather than replacing it whole.
///
/// An object is known by its aliases and its objdata.TypeName (FONTS, POWERS ... have no
/// aliases), any one of them. A mod's object that matches one of the sheet's is merged into it:
/// its fields go over the sheet's, objects inside merged the same way, anything else (numbers,
/// strings, lists) replaced, and its aliases added. A new one is added at the end. A mod's object
/// with "#remove": true takes the matching one out. An object with neither aliases nor TypeName
/// merges into the sheet's only object of its objclass (LIVECONFIG's one PVZLiveConfig, LawnStrings'
/// LawnStringsData), else is added; comment-only objects are left out. A mod sheet with "#replace": true at its top
/// replaces the game's (and earlier mods') whole. "#new": true marks an object the mod adds, so
/// that one the game already has by that name is a conflict (ModConflicts), not a change. A field
/// "X#add": [...] adds to the list X rather than replacing it (what X has already is left out).
public static class SheetMerge
{
    public const string ReplaceKey = "#replace", RemoveKey = "#remove", NewKey = "#new", AddSuffix = "#add";
    static readonly JsonDocumentOptions Options = new() { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip };

    public sealed record Counts(int Added, int Changed, int Removed);

    /// A file as JSON, from RTON or JSON text. Throws JsonException or FormatException.
    public static JsonNode? Parse(byte[] data)
    {
        if (Pvz2Conv.Rton.LooksLike(data))
        {
            using var ms = new MemoryStream();
            using (var w = new Utf8JsonWriter(ms)) Pvz2Conv.Rton.ToJson(data, w, []);
            data = ms.ToArray();
        }
        // the game's own sheets have objects with a key twice (GRIDITEMPROPS ...), which JsonNode.Parse
        // refuses: the later one wins, as the game reads them in order
        using var doc = JsonDocument.Parse(data, Options);
        return Node(doc.RootElement);
    }

    static JsonNode? Node(JsonElement e)
    {
        switch (e.ValueKind)
        {
            case JsonValueKind.Object:
                var o = new JsonObject();
                foreach (var p in e.EnumerateObject()) o[p.Name] = Node(p.Value);
                return o;
            case JsonValueKind.Array:
                var a = new JsonArray();
                foreach (var item in e.EnumerateArray()) a.Add(Node(item));
                return a;
            case JsonValueKind.Null:
                return null;
            default:
                return JsonValue.Create(e.Clone());
        }
    }

    public static bool IsSheet(JsonNode? node) => node is JsonObject o && o["objects"] is JsonArray;

    /// Whether a mod's sheet asks to replace the game's, with the ask taken out of it.
    public static bool TakeReplace(JsonObject sheet)
    {
        bool replace = sheet[ReplaceKey] is JsonValue v && v.GetValueKind() == JsonValueKind.True;
        sheet.Remove(ReplaceKey);
        return replace;
    }

    public static byte[] ToRton(JsonNode sheet) => Pvz2Conv.RtonWriter.FromJson(Encoding.UTF8.GetBytes(sheet.ToJsonString()));

    /// Puts `mod`'s objects into `sheet`, which is changed.
    public static Counts Merge(JsonObject sheet, JsonObject mod)
    {
        var objects = (JsonArray)sheet["objects"]!;
        int added = 0, changed = 0, removed = 0;
        foreach (var node in ((JsonArray)mod["objects"]!).ToList())
        {
            if (node is not JsonObject obj) continue;
            bool remove = Flag(obj, RemoveKey);
            var keys = Ids(obj);
            JsonObject? match = null;
            if (keys.Count > 0)
                match = objects.OfType<JsonObject>().FirstOrDefault(o => Ids(o).Overlaps(keys));
            else if (obj["objclass"] is JsonValue c && c.GetValueKind() == JsonValueKind.String)
            {
                // a sheet's one object of its class (LIVECONFIG, PLANTMASTERY ...)
                var same = objects.OfType<JsonObject>().Where(o => o["objclass"]?.ToString() == c.ToString()).ToList();
                if (same.Count == 1) match = same[0];
            }
            else continue;  // a comment between objects

            if (remove)
            {
                if (match != null) { objects.Remove(match); removed++; }
                continue;
            }
            if (match == null)
            {
                var copy = new JsonObject();
                Over(copy, obj);  // without the merge's own keys
                objects.Add(copy);
                added++;
            }
            else
            {
                Over(match, obj);
                changed++;
            }
        }
        return new Counts(added, changed, removed);
    }

    /// Whether a file is the resource manifest's kind ({"slot_count": n, "groups": [...]}): each
    /// group a list of resources, each resource with a slot.
    public static bool IsManifest(JsonNode? node) => node is JsonObject o && o["groups"] is JsonArray && o["objects"] == null;

    /// Puts `mod`'s resource groups into `manifest`, which is changed. A group known by its id
    /// takes the mod's fields, its resources merged by id (changed or added); a new one is added
    /// ("#remove": true takes one out). Each resource gets the slot the manifest has for its id, or
    /// a new one after the last (any slot the mod gives is left aside), and slot_count grows to
    /// match. "subgroups#add": [...] adds groups to a composite the manifest has. Returns the counts
    /// of groups, and the composite groups the mod adds or adds groups to (with just those groups).
    public static (Counts Counts, List<JsonObject> NewComposites) MergeManifest(JsonObject manifest, JsonObject mod)
    {
        var groups = (JsonArray)manifest["groups"]!;
        var slots = new Dictionary<string, int>(StringComparer.Ordinal);
        int next = manifest["slot_count"] is JsonValue sc && sc.TryGetValue(out int n) ? n : 0;
        foreach (var r in groups.OfType<JsonObject>().SelectMany(Resources))
            if (Id(r) is { } id && r["slot"] is JsonValue v && v.TryGetValue(out int slot))
            {
                slots.TryAdd(id, slot);
                next = Math.Max(next, slot + 1);
            }
        int added = 0, changed = 0, removed = 0;
        var composites = new List<JsonObject>();
        foreach (var node in ((JsonArray)mod["groups"]!).ToList())
        {
            if (node is not JsonObject g || Id(g) is not { } gid) continue;
            var match = groups.OfType<JsonObject>().FirstOrDefault(o => Id(o) == gid);
            if (Flag(g, RemoveKey))
            {
                if (match != null) { groups.Remove(match); removed++; }
                continue;
            }
            var copy = (JsonObject)g.DeepClone();
            copy.Remove(NewKey);
            foreach (var r in Resources(copy))
                if (Id(r) is { } id)
                {
                    if (!slots.TryGetValue(id, out int slot)) slots[id] = slot = next++;
                    r["slot"] = slot;
                }
            if (match == null)
            {
                groups.Add(copy);
                added++;
                if (copy["type"]?.ToString() == "composite") composites.Add(copy);
                continue;
            }
            foreach (var (name, value) in copy.ToList())
            {
                if (name.EndsWith(AddSuffix, StringComparison.Ordinal) && value is JsonArray items)
                {
                    // "subgroups#add": [...] adds to a composite's groups (those it hasn't by id)
                    string list = name[..^AddSuffix.Length];
                    if (match[list] is not JsonArray into) match[list] = into = new JsonArray();
                    var known = into.OfType<JsonObject>().Select(Id).ToHashSet();
                    var fresh = items.OfType<JsonObject>().Where(o => Id(o) is { } id && known.Add(id)).ToList();
                    foreach (var o in fresh) into.Add(o.DeepClone());
                    if (list == "subgroups" && fresh.Count > 0)
                        composites.Add(new JsonObject { ["id"] = gid, ["subgroups"] = new JsonArray(fresh.Select(o => (JsonNode?)o.DeepClone()).ToArray()) });
                    continue;
                }
                if (name == "resources" && match["resources"] is JsonArray have && value is JsonArray more)
                {
                    foreach (var r in more.OfType<JsonObject>())
                    {
                        var old = have.OfType<JsonObject>().FirstOrDefault(o => Id(o) == Id(r));
                        if (old != null) have[have.IndexOf(old)] = r.DeepClone();
                        else have.Add(r.DeepClone());
                    }
                }
                else match[name] = value?.DeepClone();
            }
            changed++;
        }
        manifest["slot_count"] = next;
        return (new Counts(added, changed, removed), composites);

        static IEnumerable<JsonObject> Resources(JsonObject group) => group["resources"] is JsonArray a ? a.OfType<JsonObject>() : [];
        static string? Id(JsonObject o) => o["id"] is JsonValue v && v.GetValueKind() == JsonValueKind.String ? v.ToString() : null;
    }

    public static bool Flag(JsonObject obj, string key) => obj[key] is JsonValue v && v.GetValueKind() == JsonValueKind.True;

    /// An object's names: "alias:x" for each alias, "type:x" for objdata.TypeName.
    public static HashSet<string> Ids(JsonObject obj)
    {
        var keys = new HashSet<string>(StringComparer.Ordinal);
        if (obj["aliases"] is JsonArray aliases)
            foreach (var a in aliases)
                if (a is JsonValue v && v.GetValueKind() == JsonValueKind.String) keys.Add("alias:" + v.ToString());
        if (obj["objdata"] is JsonObject data && data["TypeName"] is JsonValue t && t.GetValueKind() == JsonValueKind.String)
            keys.Add("type:" + t.ToString());
        return keys;
    }

    // `source`'s fields over `target`'s: objects merged, anything else replaced, aliases added to
    static void Over(JsonObject target, JsonObject source)
    {
        foreach (var (name, value) in source.ToList())
        {
            if (name is RemoveKey or NewKey) continue;
            if (name.EndsWith(AddSuffix, StringComparison.Ordinal) && value is JsonArray items)
            {
                // "List#add": [...] puts items at the end of List, those it hasn't already
                string list = name[..^AddSuffix.Length];
                if (target[list] is not JsonArray into) target[list] = into = new JsonArray();
                if (list == "LocStringValues")
                {
                    // LawnStrings' [key, text, key, text ...]: a key it has gets the new text
                    for (int i = 0; i + 1 < items.Count; i += 2)
                    {
                        string? key = items[i]?.ToString();
                        int at = -1;
                        for (int j = 0; j + 1 < into.Count && at < 0; j += 2)
                            if (into[j]?.ToString() == key) at = j;
                        if (at >= 0) into[at + 1] = items[i + 1]?.DeepClone();
                        else { into.Add(items[i]?.DeepClone()); into.Add(items[i + 1]?.DeepClone()); }
                    }
                    continue;
                }
                var known = into.Select(a => a?.ToJsonString()).ToHashSet();
                foreach (var a in items)
                    if (known.Add(a?.ToJsonString())) into.Add(a?.DeepClone());
                continue;
            }
            if (name == "aliases" && target["aliases"] is JsonArray have && value is JsonArray more)
            {
                var known = have.Select(a => a?.ToJsonString()).ToHashSet();
                foreach (var a in more)
                    if (known.Add(a?.ToJsonString())) have.Add(a?.DeepClone());
                continue;
            }
            if (value is JsonObject over)
            {
                if (target[name] is not JsonObject inner) target[name] = inner = new JsonObject();
                Over(inner, over);
            }
            else target[name] = value?.DeepClone();
        }
    }
}
