using System.Text.Json.Nodes;

namespace Pvz2Builder;

/// A picture in the game's resource manifest: where it sits in which atlas.
public sealed record ImageResource(string Id, string Group, string Res, string AtlasId, string AtlasPath, int X, int Y, int W, int H);

/// The game's resource manifest (PROPERTIES\RESOURCES.RTON): its resource groups, the pictures in
/// each (cut out of atlases) and its PopAnims. Pictures come out of the OBB's atlases as asked for.
public sealed class Resources
{
    public Game Game { get; }
    readonly Dictionary<string, JsonObject> _groups = new(StringComparer.Ordinal);
    readonly Dictionary<string, List<ImageResource>> _images = new(StringComparer.Ordinal);
    readonly Dictionary<string, List<ImageResource>> _byGroup = new(StringComparer.Ordinal);
    readonly Dictionary<string, string> _popAnims = new(StringComparer.Ordinal);
    readonly Dictionary<string, (string Path, string Group)> _atlases = new(StringComparer.Ordinal);
    readonly LinkedList<(string Path, Picture? Picture)> _atlasCache = new();
    public int GroupCount => _groups.Count;
    public IReadOnlyCollection<string> PopAnimIds => _popAnims.Keys;

    Resources(Game game) => Game = game;

    public static Resources Load(Game game)
    {
        var r = new Resources(game);
        var data = game.Obb.Read(@"PROPERTIES\RESOURCES.RTON");
        if (data == null || Pvz2Launcher.SheetMerge.Parse(data) is not JsonObject manifest || manifest["groups"] is not JsonArray groups) return r;
        foreach (var g in groups.OfType<JsonObject>())
        {
            if (GameProfile.Str(g["id"]) is not { } gid) continue;
            r._groups[gid] = g;
            string res = GameProfile.Str(g["res"]) ?? "";
            if (g["resources"] is not JsonArray items) continue;
            foreach (var item in items.OfType<JsonObject>())
                if (GameProfile.Str(item["id"]) is { } id && GameProfile.Str(item["type"]) is { } type && item["path"] is JsonArray path)
                {
                    string obbPath = string.Join('\\', path.Select(GameProfile.Str).OfType<string>()).ToUpperInvariant();
                    if (type == "Image" && item["atlas"] is JsonValue a && a.GetValueKind() == System.Text.Json.JsonValueKind.True)
                        r._atlases[id] = (obbPath + ".PTX", gid);
                    else if (type == "PopAnim")
                        r._popAnims.TryAdd(id, obbPath.EndsWith(".PAM", StringComparison.Ordinal) ? obbPath : obbPath + ".PAM");
                }
        }
        // pictures after atlases, as a group may list them in any order
        foreach (var (gid, g) in r._groups)
        {
            if (g["resources"] is not JsonArray items) continue;
            string res = GameProfile.Str(g["res"]) ?? "";
            foreach (var item in items.OfType<JsonObject>())
                if (GameProfile.Str(item["type"]) == "Image" && GameProfile.Str(item["id"]) is { } id &&
                    GameProfile.Str(item["parent"]) is { } parent && r._atlases.TryGetValue(parent, out var atlas))
                {
                    if (!r._images.TryGetValue(id, out var list)) r._images[id] = list = [];
                    if (!r._byGroup.TryGetValue(gid, out var inGroup)) r._byGroup[gid] = inGroup = [];
                    var image = new ImageResource(id, gid, res, parent, atlas.Path, Int(item["ax"]), Int(item["ay"]), Int(item["aw"]), Int(item["ah"]));
                    list.Add(image);
                    inGroup.Add(image);
                }
        }
        return r;
    }

    static int Int(JsonNode? n) => n is JsonValue v && v.TryGetValue<int>(out int i) ? i : n is JsonValue d && d.TryGetValue<double>(out double x) ? (int)x : 0;

    /// The simple groups a group stands for: a composite's subgroups (one resolution's and the
    /// common one), or the group itself.
    public IReadOnlyList<string> Expand(string group, string res = "768")
    {
        if (!_groups.TryGetValue(group, out var g)) return [];
        if (GameProfile.Str(g["type"]) != "composite" || g["subgroups"] is not JsonArray subs) return [group];
        var result = new List<string>();
        foreach (var s in subs.OfType<JsonObject>())
            if (GameProfile.Str(s["id"]) is { } id && (GameProfile.Str(s["res"]) is not { } r || r == res))
                result.Add(id);
        return result;
    }

    /// The pictures of a group (a composite's at `res`).
    public IReadOnlyList<ImageResource> ImagesIn(string group, string res = "768") =>
        Expand(group, res).SelectMany(g => _byGroup.TryGetValue(g, out var l) ? l : []).OrderBy(i => i.Id, StringComparer.Ordinal).ToList();

    /// The PopAnims a group lists.
    public IReadOnlyList<string> PopAnimsIn(string group, string res = "768") =>
        Expand(group, res).SelectMany(g => _groups.TryGetValue(g, out var o) && o["resources"] is JsonArray a ? a.OfType<JsonObject>() : [])
            .Where(i => GameProfile.Str(i["type"]) == "PopAnim").Select(i => GameProfile.Str(i["id"])).OfType<string>().ToList();

    public bool HasImage(string id) => _images.ContainsKey(id);

    /// A picture by its id, cut out of its atlas, at `res` when the game has it there (else any).
    public Picture? Image(string id, string res = "768")
    {
        if (!_images.TryGetValue(id, out var list)) return null;
        var pick = list.FirstOrDefault(i => i.Res == res) ?? list.FirstOrDefault(i => i.Res == "1536") ?? list[0];
        return Atlas(pick.AtlasPath)?.Crop(pick.X, pick.Y, pick.W, pick.H);
    }

    /// The resolution an image was cut at, for scaling it to PopAnim units.
    public string? ResOf(string id, string res = "768") =>
        _images.TryGetValue(id, out var list) ? (list.FirstOrDefault(i => i.Res == res) ?? list.FirstOrDefault(i => i.Res == "1536") ?? list[0]).Res : null;

    /// An atlas decoded from the OBB (the last few are kept), or null when it can't be.
    public Picture? Atlas(string obbPath)
    {
        lock (_atlasCache)
        {
            for (var n = _atlasCache.First; n != null; n = n.Next)
                if (n.Value.Path == obbPath)
                {
                    _atlasCache.Remove(n);
                    _atlasCache.AddFirst(n);
                    return n.Value.Picture;
                }
            var p = Game.Obb.ReadTexture(obbPath, out _);
            _atlasCache.AddFirst((obbPath, p));
            while (_atlasCache.Count > 8) _atlasCache.RemoveLast();
            return p;
        }
    }

    /// A PopAnim's file (PAM) by its id ("POPANIM_PLANT_PEASHOOTER").
    public byte[]? PopAnimFile(string id) => _popAnims.TryGetValue(id, out var path) ? Game.Obb.Read(path) : null;
}
