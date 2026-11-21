using System.Text;
using System.Text.Json;

namespace Pvz2Builder;

/// A PopAnim (PAM) read for drawing: its images, sprites and main sprite, in PAM units (the
/// game's 1536 resolution: a 768 picture is drawn at twice its size).
public sealed class PamAnimation
{
    public sealed record Image(string Name, string Id, double Width, double Height, Affine Transform);
    public sealed record Append(int Index, int Resource, bool Sprite, bool Additive, int PreloadFrame, double TimeScale);
    public sealed record Change(int Index, Affine? Transform, float[]? Color, int? SpriteFrame);
    public sealed record Frame(int[] Remove, Append[] Append, Change[] Change, string? Label);
    public sealed record Sprite(string Name, Frame[] Frames)
    {
        /// The frame a label starts on, or -1.
        public int LabelFrame(string label) => Array.FindIndex(Frames, f => string.Equals(f.Label, label, StringComparison.OrdinalIgnoreCase));
        public IEnumerable<string> Labels => Frames.Select(f => f.Label).OfType<string>();
    }

    public double FrameRate { get; private init; }
    public IReadOnlyList<Image> Images { get; private init; } = [];
    public IReadOnlyList<Sprite> Sprites { get; private init; } = [];
    public Sprite? Main { get; private init; }

    public static PamAnimation Parse(byte[] pam)
    {
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms)) Pvz2Conv.Pam.ToJson(pam, w, []);
        using var doc = JsonDocument.Parse(ms.ToArray());
        var root = doc.RootElement;
        var images = root.GetProperty("image").EnumerateArray().Select(i =>
        {
            string name = i.GetProperty("name").GetString() ?? "";
            int bar = name.IndexOf('|');
            string id = bar >= 0 ? name[(bar + 1)..] : name;
            double w = 0, h = 0;
            if (i.TryGetProperty("size", out var size) && size.GetArrayLength() == 2) { w = size[0].GetDouble(); h = size[1].GetDouble(); }
            var t = i.GetProperty("transform").EnumerateArray().Select(x => x.GetDouble()).ToArray();
            return new Image(name, id, w, h, ToAffine(t) ?? Affine.Identity);
        }).ToList();
        var sprites = root.GetProperty("sprite").EnumerateArray().Select(ReadSprite).ToList();
        var main = root.TryGetProperty("main_sprite", out var m) && m.ValueKind == JsonValueKind.Object ? ReadSprite(m) : null;
        return new PamAnimation
        {
            FrameRate = root.GetProperty("frame_rate").GetDouble(),
            Images = images,
            Sprites = sprites,
            Main = main,
        };
    }

    static Sprite ReadSprite(JsonElement s)
    {
        string name = s.TryGetProperty("name", out var n) && n.ValueKind == JsonValueKind.String ? n.GetString()! : "";
        var frames = s.GetProperty("frame").EnumerateArray().Select(f => new Frame(
            f.GetProperty("remove").EnumerateArray().Select(r => r.GetProperty("index").GetInt32()).ToArray(),
            f.GetProperty("append").EnumerateArray().Select(a => new Append(
                a.GetProperty("index").GetInt32(), a.GetProperty("resource").GetInt32(), a.GetProperty("sprite").GetBoolean(),
                a.GetProperty("additive").GetBoolean(), a.GetProperty("preload_frame").GetInt32(), a.GetProperty("time_scale").GetDouble())).ToArray(),
            f.GetProperty("change").EnumerateArray().Select(c => new Change(
                c.GetProperty("index").GetInt32(),
                ToAffine(c.GetProperty("transform").EnumerateArray().Select(x => x.GetDouble()).ToArray()),
                c.GetProperty("color").ValueKind == JsonValueKind.Array ? c.GetProperty("color").EnumerateArray().Select(x => (float)x.GetDouble()).ToArray() : null,
                c.GetProperty("sprite_frame_number").ValueKind == JsonValueKind.Number ? c.GetProperty("sprite_frame_number").GetInt32() : null)).ToArray(),
            f.GetProperty("label").ValueKind == JsonValueKind.String ? f.GetProperty("label").GetString() : null)).ToArray();
        return new Sprite(name, frames);
    }

    /// [x, y], [angle, x, y] or [a, b, c, d, x, y] as PAM writes a transform.
    static Affine? ToAffine(double[] t) => t.Length switch
    {
        2 => Affine.Translate(t[0], t[1]),
        3 => new Affine(Math.Cos(t[0]), Math.Sin(t[0]), -Math.Sin(t[0]), Math.Cos(t[0]), t[1], t[2]),
        6 => new Affine(t[0], t[1], t[2], t[3], t[4], t[5]),
        _ => null,
    };
}

/// Draws one frame of a PopAnim with the game's own pictures: what the Builder shows for a plant
/// or zombie without starting the game.
public static class PamPreview
{
    sealed class Layer
    {
        public required PamAnimation.Append Append;
        public int Start;
        public Affine Transform = Affine.Identity;
        public float[] Color = [1, 1, 1, 1];
        public int? SpriteFrame;
    }

    record struct Draw(Picture Picture, Affine M, float[] Color, bool Additive);

    /// Parts the game shows only sometimes, which its animation rig switches on in play: costumes
    /// (custom_*), armour and its damage states, effects. Hidden in a preview unless asked for.
    public static bool DefaultHidden(string sprite)
    {
        string s = sprite.ToLowerInvariant();
        return s.StartsWith("custom", StringComparison.Ordinal) || s.StartsWith("_custom", StringComparison.Ordinal) ||
               s.Contains("armor") || s.Contains("damage") || s.Contains("particle") || s.Contains("ground_swatch") ||
               s is "ink" or "butter" || s.Contains("_blast");
    }

    /// The frame shown for an animation: its "idle" label's, else its first label's, else 0.
    public static int DefaultFrame(PamAnimation.Sprite sprite)
    {
        foreach (var label in new[] { "idle", "idle_loop", "walk", "loop" })
            if (sprite.LabelFrame(label) is int f and >= 0) return f;
        int first = Array.FindIndex(sprite.Frames, f => f.Label != null);
        return Math.Max(0, first);
    }

    /// Renders `frame` of the animation's main sprite (or `sprite`, by name) at `scale` output
    /// pixels per PAM unit, cropped to what's drawn. `image` gives each image's picture by id
    /// and the resolution it was cut at ("768"). Returns null when nothing is drawn.
    public static Picture? Render(PamAnimation anim, Func<string, (Picture Picture, int Res)?> image, int? frame = null, double scale = 0.5,
        string? sprite = null, Func<string, bool>? hide = null)
    {
        hide ??= DefaultHidden;
        var root = sprite == null ? anim.Main ?? anim.Sprites.LastOrDefault() : anim.Sprites.FirstOrDefault(s => s.Name == sprite);
        if (root == null || root.Frames.Length == 0) return null;
        int at = Math.Clamp(frame ?? DefaultFrame(root), 0, root.Frames.Length - 1);
        var pictures = new Dictionary<int, (Picture, int)?>();
        var draws = new List<Draw>();
        Collect(anim, root, at, Affine.Scale(scale, scale), [1, 1, 1, 1], false, draws, hide, i =>
        {
            if (!pictures.TryGetValue(i, out var p)) pictures[i] = p = i < anim.Images.Count ? image(anim.Images[i].Id) : null;
            return p;
        }, 0);
        if (draws.Count == 0) return null;

        double minX = double.MaxValue, minY = double.MaxValue, maxX = double.MinValue, maxY = double.MinValue;
        foreach (var d in draws)
            foreach (var (cx, cy) in new[] { (0.0, 0.0), (d.Picture.Width, 0.0), (0.0, d.Picture.Height), (d.Picture.Width, (double)d.Picture.Height) })
            {
                var (x, y) = d.M.Apply(cx, cy);
                minX = Math.Min(minX, x); maxX = Math.Max(maxX, x);
                minY = Math.Min(minY, y); maxY = Math.Max(maxY, y);
            }
        int pad = 2;
        int width = (int)Math.Ceiling(maxX - minX) + 2 * pad, height = (int)Math.Ceiling(maxY - minY) + 2 * pad;
        if (width <= 0 || height <= 0 || width > 8192 || height > 8192) return null;
        var canvas = new Canvas(width, height);
        var shift = Affine.Translate(pad - Math.Floor(minX), pad - Math.Floor(minY));
        foreach (var d in draws) canvas.Draw(d.Picture, d.M.Then(shift), d.Color, d.Additive);
        var result = canvas.ToPicture();
        return result.IsEmpty() ? null : result;
    }

    static void Collect(PamAnimation anim, PamAnimation.Sprite sprite, int frame, Affine parent, float[] parentColor, bool parentAdditive,
        List<Draw> draws, Func<string, bool> hide, Func<int, (Picture Picture, int Res)?> picture, int depth)
    {
        if (depth > 16 || sprite.Frames.Length == 0) return;
        frame = Math.Clamp(frame, 0, sprite.Frames.Length - 1);
        var layers = new SortedDictionary<int, Layer>();
        for (int f = 0; f <= frame; f++)
        {
            var fr = sprite.Frames[f];
            foreach (int r in fr.Remove) layers.Remove(r);
            foreach (var a in fr.Append) layers[a.Index] = new Layer { Append = a, Start = f };
            foreach (var c in fr.Change)
            {
                if (!layers.TryGetValue(c.Index, out var l)) continue;
                if (c.Transform is { } t) l.Transform = t;
                if (c.Color != null) l.Color = c.Color;
                if (c.SpriteFrame != null) l.SpriteFrame = c.SpriteFrame;
            }
        }
        foreach (var l in layers.Values)
        {
            var m = l.Transform.Then(parent);
            float[] color = [l.Color[0] * parentColor[0], l.Color[1] * parentColor[1], l.Color[2] * parentColor[2], l.Color[3] * parentColor[3]];
            bool additive = parentAdditive || l.Append.Additive;
            if (l.Append.Sprite)
            {
                if (l.Append.Resource >= anim.Sprites.Count) continue;
                var child = anim.Sprites[l.Append.Resource];
                if (child.Frames.Length == 0 || hide(child.Name)) continue;
                int cf = l.SpriteFrame ?? (int)Math.Floor((frame - l.Start + l.Append.PreloadFrame) * l.Append.TimeScale);
                cf = ((cf % child.Frames.Length) + child.Frames.Length) % child.Frames.Length;
                Collect(anim, child, cf, m, color, additive, draws, hide, picture, depth + 1);
            }
            else
            {
                if (l.Append.Resource >= anim.Images.Count || picture(l.Append.Resource) is not var (pic, res)) continue;
                var img = anim.Images[l.Append.Resource];
                // the picture (cut at its resolution) to the image's size in PAM units, then placed
                double sx = img.Width > 0 && pic.Width > 0 ? img.Width / pic.Width : 1536.0 / res;
                double sy = img.Height > 0 && pic.Height > 0 ? img.Height / pic.Height : 1536.0 / res;
                draws.Add(new Draw(pic, Affine.Scale(sx, sy).Then(img.Transform).Then(m), color, additive));
            }
        }
    }

    /// Renders a PopAnim by its id from a game's resources, or null (with why).
    public static Picture? Render(Resources resources, string popAnimId, out string why, int? frame = null, double scale = 0.5, string res = "768",
        Func<string, bool>? hide = null)
    {
        why = "";
        var data = resources.PopAnimFile(popAnimId);
        if (data == null) { why = $"the game has no PopAnim {popAnimId}"; return null; }
        PamAnimation anim;
        try { anim = PamAnimation.Parse(data); }
        catch (Exception e) when (e is FormatException or JsonException or InvalidOperationException) { why = $"its PAM can't be read: {e.Message}"; return null; }
        var p = Render(anim, id => resources.Image(id, res) is { } pic && resources.ResOf(id, res) is { } r && int.TryParse(r, out int ri) ? (pic, ri) : null, frame, scale, null, hide);
        if (p == null) why = "nothing in its first frame could be drawn (its pictures may be in a format the Builder can't show yet)";
        return p;
    }
}
