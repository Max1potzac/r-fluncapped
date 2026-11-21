using System.Text.Json;
using System.Text.Json.Nodes;

namespace Pvz2Launcher;

/// Pictures a mod puts into one of the game's own texture atlases. Some of the game's pictures
/// are drawn from the atlas their kind is in, whatever atlas the manifest names as their parent:
/// a seed packet's from UI_SeedPackets' (ATLASIMAGE_ATLAS_UI_SEEDPACKETS_768_00 ...), so a new
/// plant's packet in an atlas of its own draws whatever is at its place in that one. A mod's
/// resource manifest gives such a picture as an image of the game's group, its parent the game's
/// atlas, with "#png": "a/file.png" (from the mod's folder) in place of ax, ay, aw and ah: the
/// launcher puts the PNG in a free place of the atlas (one no picture has, and clear) and gives
/// the image that place.
public sealed class AtlasPictures
{
    public const string PngKey = "#png";
    const int Pad = 1;  // clear pixels around a picture, so filtering doesn't bleed its neighbours in

    readonly string obb;
    readonly ObbPatch.Index index;
    readonly Dictionary<string, Atlas?> atlases = new(StringComparer.Ordinal);

    sealed class Atlas
    {
        public required string Path;  // the OBB's ("ATLASES\\UI_SEEDPACKETS_768_00.PTX")
        public required ObbPatch.Texture Texture;
        public required List<(int X, int Y, int W, int H)> Taken;
        public bool Changed;
    }

    public AtlasPictures(string obb, ObbPatch.Index index)
    {
        this.obb = obb;
        this.index = index;
    }

    /// The atlases pictures went into, by OBB path, for ObbPatch.Build.
    public Dictionary<string, byte[]> Changed() =>
        atlases.Values.Where(a => a is { Changed: true }).ToDictionary(a => a!.Path, a => a!.Texture.Rgba, StringComparer.Ordinal);

    /// Puts the pictures `mod`'s manifest asks for ("#png") into the atlases `manifest` (the
    /// merged one so far) has, and gives each its place there; one that can't be is taken out of
    /// `mod`, with a line in `log`.
    public void Place(JsonObject manifest, JsonObject mod, string modDir, List<string> log)
    {
        if (mod["groups"] is not JsonArray groups) return;
        foreach (var group in groups.OfType<JsonObject>())
        {
            if (group["resources"] is not JsonArray resources) continue;
            foreach (var r in resources.OfType<JsonObject>().ToList())
            {
                if (r[PngKey] is not JsonValue v || v.GetValueKind() != JsonValueKind.String) continue;
                string id = r["id"]?.ToString() ?? "(an image)", png = v.ToString();
                r.Remove(PngKey);
                if (Put(manifest, r, Path.Combine(modDir, png), log) is { } why)
                {
                    log.Add($"{id}: {why}; left out");
                    resources.Remove(r);
                }
            }
        }
    }

    // null when `image` got its place, else why not
    string? Put(JsonObject manifest, JsonObject image, string file, List<string> log)
    {
        string? parent = image["parent"]?.ToString();
        if (string.IsNullOrEmpty(parent)) return "a picture (#png) needs the atlas it goes into as its parent";
        var atlas = Find(manifest, parent, log);
        if (atlas == null) return $"no atlas {parent} of the game's that pictures can go into";
        if (!File.Exists(file)) return $"{file} is missing";
        byte[] rgba;
        int w, h;
        try { (rgba, w, h) = Pvz2Conv.Png.Decode(File.ReadAllBytes(file)); }
        catch (FormatException e) { return $"{Path.GetFileName(file)}: {e.Message}"; }
        var t = atlas.Texture;
        if (FindRoom(t.Rgba, t.Width, t.Height, atlas.Taken, w + 2 * Pad, h + 2 * Pad) is not var (x, y))
            return $"{parent} has no free {w}x{h} place for {Path.GetFileName(file)}";
        x += Pad;
        y += Pad;
        for (int row = 0; row < h; row++)
            Array.Copy(rgba, row * w * 4, t.Rgba, ((y + row) * t.Width + x) * 4, w * 4);
        atlas.Taken.Add((x, y, w, h));
        atlas.Changed = true;
        image["ax"] = x;
        image["ay"] = y;
        image["aw"] = w;
        image["ah"] = h;
        return null;
    }

    // the game's atlas resource `id` in `manifest`, its texture read out of the OBB (once)
    Atlas? Find(JsonObject manifest, string id, List<string> log)
    {
        if (atlases.TryGetValue(id, out var known)) return known;
        Atlas? atlas = null;
        var all = ((manifest["groups"] as JsonArray)?.OfType<JsonObject>() ?? [])
            .SelectMany(g => g["resources"] is JsonArray a ? a.OfType<JsonObject>() : []).ToList();
        var res = all.FirstOrDefault(r => r["id"]?.ToString() == id && r["atlas"] is JsonValue a && a.GetValueKind() == JsonValueKind.True);
        if (res?["path"] is JsonArray path && path.Count > 0)
        {
            string file = string.Join("/", path.Select(p => p?.ToString())) + ".PTX";
            if (ObbPatch.ReadTexture(obb, index, file, out string why) is { } texture)
            {
                // where the atlas's pictures are already, which may have clear pixels of their own
                var taken = all.Where(r => r["parent"]?.ToString() == id)
                    .Select(r => (Int(r, "ax"), Int(r, "ay"), Int(r, "aw"), Int(r, "ah"))).ToList();
                atlas = new Atlas { Path = ObbPatch.Key(file), Texture = texture, Taken = taken };
            }
            else log.Add($"{id} ({file}): {why}");
        }
        atlases[id] = atlas;
        return atlas;

        static int Int(JsonObject o, string key) => o[key] is JsonValue v && v.TryGetValue(out int n) ? n : 0;
    }

    /// The top left of the first w x h place, row by row, where `rgba` (width x height) is clear
    /// and no rectangle of `taken` is; null when there is none.
    public static (int X, int Y)? FindRoom(byte[] rgba, int width, int height, IEnumerable<(int X, int Y, int W, int H)> taken, int w, int h)
    {
        if (w > width || h > height) return null;
        var used = new bool[width * height];
        for (int i = 0; i < used.Length; i++) used[i] = rgba[i * 4 + 3] != 0;
        foreach (var (x, y, rw, rh) in taken)
            for (int row = Math.Max(y, 0); row < Math.Min(y + rh, height); row++)
                for (int col = Math.Max(x, 0); col < Math.Min(x + rw, width); col++)
                    used[row * width + col] = true;
        // used pixels in [0, x) x [0, y), for every x and y
        var sum = new int[(width + 1) * (height + 1)];
        for (int y = 0; y < height; y++)
        {
            int line = 0;
            for (int x = 0; x < width; x++)
            {
                if (used[y * width + x]) line++;
                sum[(y + 1) * (width + 1) + x + 1] = sum[y * (width + 1) + x + 1] + line;
            }
        }
        for (int y = 0; y + h <= height; y++)
            for (int x = 0; x + w <= width; x++)
            {
                int n = sum[(y + h) * (width + 1) + x + w] - sum[y * (width + 1) + x + w] - sum[(y + h) * (width + 1) + x] + sum[y * (width + 1) + x];
                if (n == 0) return (x, y);
            }
        return null;
    }
}
