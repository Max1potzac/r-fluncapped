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
/// merges into the sheet's only object of its objclass (LIVECONFIG's one PVZLiveConfig), else is
/// added; comment-only objects are left out. A mod sheet with "#replace": true at its top
/// replaces the game's (and earlier mods') whole. "#new": true marks an object the mod adds, so
/// that one the game already has by that name is a conflict (ModConflicts), not a change.
public static class SheetMerge
{
    public const string ReplaceKey = "#replace", RemoveKey = "#remove", NewKey = "#new";
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
                var same = objects.OfType<JsonObject>().Where(o => Ids(o).Count == 0 && o["objclass"]?.ToString() == c.ToString()).ToList();
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
                var copy = (JsonObject)obj.DeepClone();
                copy.Remove(NewKey);
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
            if (name == "aliases" && target["aliases"] is JsonArray have && value is JsonArray more)
            {
                var known = have.Select(a => a?.ToJsonString()).ToHashSet();
                foreach (var a in more)
                    if (known.Add(a?.ToJsonString())) have.Add(a?.DeepClone());
                continue;
            }
            if (target[name] is JsonObject inner && value is JsonObject over) Over(inner, over);
            else target[name] = value?.DeepClone();
        }
    }
}
