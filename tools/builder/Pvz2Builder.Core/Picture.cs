namespace Pvz2Builder;

/// An RGBA8888 picture (straight alpha, rows top down): an atlas, a sprite cut from one, or a
/// rendered preview.
public sealed class Picture
{
    public int Width { get; }
    public int Height { get; }
    public byte[] Rgba { get; }

    public Picture(int width, int height, byte[]? rgba = null)
    {
        if (width < 0 || height < 0) throw new ArgumentOutOfRangeException(nameof(width));
        Width = width;
        Height = height;
        Rgba = rgba ?? new byte[width * height * 4];
        if (Rgba.Length < width * height * 4) throw new ArgumentException("too few pixels for the size", nameof(rgba));
    }

    /// The part at (x, y, w, h), clipped to the picture.
    public Picture Crop(int x, int y, int w, int h)
    {
        int x0 = Math.Clamp(x, 0, Width), y0 = Math.Clamp(y, 0, Height);
        int x1 = Math.Clamp(x + w, 0, Width), y1 = Math.Clamp(y + h, 0, Height);
        var p = new Picture(x1 - x0, y1 - y0);
        for (int row = y0; row < y1; row++)
            Array.Copy(Rgba, (row * Width + x0) * 4, p.Rgba, (row - y0) * p.Width * 4, p.Width * 4);
        return p;
    }

    public byte[] ToPng() => Pvz2Conv.Png.Encode(Rgba, Width, Height);

    public static Picture FromPng(byte[] png)
    {
        var (rgba, w, h) = Pvz2Conv.Png.Decode(png);
        return new Picture(w, h, rgba);
    }

    /// Whether every pixel is fully transparent.
    public bool IsEmpty()
    {
        for (int i = 3; i < Rgba.Length; i += 4) if (Rgba[i] != 0) return false;
        return true;
    }
}

/// A 2D affine transform: x' = A x + C y + X, y' = B x + D y + Y (Flash's matrix, as PAM uses).
public readonly record struct Affine(double A, double B, double C, double D, double X, double Y)
{
    public static readonly Affine Identity = new(1, 0, 0, 1, 0, 0);

    public static Affine Translate(double x, double y) => new(1, 0, 0, 1, x, y);
    public static Affine Scale(double sx, double sy) => new(sx, 0, 0, sy, 0, 0);

    /// This transform followed by `then`.
    public Affine Then(Affine then) => new(
        then.A * A + then.C * B, then.B * A + then.D * B,
        then.A * C + then.C * D, then.B * C + then.D * D,
        then.A * X + then.C * Y + then.X, then.B * X + then.D * Y + then.Y);

    public (double X, double Y) Apply(double x, double y) => (A * x + C * y + X, B * x + D * y + Y);

    public Affine? Inverse()
    {
        double det = A * D - B * C;
        if (Math.Abs(det) < 1e-12) return null;
        double ia = D / det, ib = -B / det, ic = -C / det, id = A / det;
        return new Affine(ia, ib, ic, id, -(ia * X + ic * Y), -(ib * X + id * Y));
    }
}

/// A premultiplied floating-point canvas pictures are drawn onto (src-over or additive), then
/// turned back into a Picture.
public sealed class Canvas
{
    public int Width { get; }
    public int Height { get; }
    readonly float[] _px;

    public Canvas(int width, int height)
    {
        Width = width;
        Height = height;
        _px = new float[width * height * 4];
    }

    /// Draws `src` with `m` taking its pixel coordinates (0..Width, 0..Height) to the canvas's,
    /// its colours multiplied by `color` (r, g, b, a in 0..1), sampled bilinearly.
    public void Draw(Picture src, Affine m, ReadOnlySpan<float> color, bool additive = false)
    {
        if (src.Width == 0 || src.Height == 0 || color[3] <= 0) return;
        if (m.Inverse() is not { } inv) return;
        double minX = double.MaxValue, minY = double.MaxValue, maxX = double.MinValue, maxY = double.MinValue;
        foreach (var (cx, cy) in new[] { (0.0, 0.0), (src.Width, 0.0), (0.0, src.Height), (src.Width, (double)src.Height) })
        {
            var (x, y) = m.Apply(cx, cy);
            minX = Math.Min(minX, x); maxX = Math.Max(maxX, x);
            minY = Math.Min(minY, y); maxY = Math.Max(maxY, y);
        }
        int x0 = Math.Max(0, (int)Math.Floor(minX)), x1 = Math.Min(Width, (int)Math.Ceiling(maxX));
        int y0 = Math.Max(0, (int)Math.Floor(minY)), y1 = Math.Min(Height, (int)Math.Ceiling(maxY));
        float cr = color[0], cg = color[1], cb = color[2], ca = color[3];
        Span<float> s = stackalloc float[4];
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++)
            {
                var (u, v) = inv.Apply(x + 0.5, y + 0.5);
                if (u < 0 || v < 0 || u >= src.Width || v >= src.Height) continue;
                Sample(src, u - 0.5, v - 0.5, s);
                float a = s[3] * ca;
                if (a <= 0) continue;
                float r = s[0] * cr * a, g = s[1] * cg * a, b = s[2] * cb * a;  // premultiplied: the sample is straight
                int o = (y * Width + x) * 4;
                if (additive)
                {
                    _px[o] = Math.Min(1, _px[o] + r); _px[o + 1] = Math.Min(1, _px[o + 1] + g); _px[o + 2] = Math.Min(1, _px[o + 2] + b);
                    _px[o + 3] = Math.Min(1, _px[o + 3] + a);
                }
                else
                {
                    float k = 1 - a;
                    _px[o] = r + _px[o] * k; _px[o + 1] = g + _px[o + 1] * k; _px[o + 2] = b + _px[o + 2] * k; _px[o + 3] = a + _px[o + 3] * k;
                }
            }
    }

    /// Bilinear, premultiplied (so transparent edges don't darken).
    static void Sample(Picture p, double u, double v, Span<float> result)
    {
        int ix = (int)Math.Floor(u), iy = (int)Math.Floor(v);
        float fx = (float)(u - ix), fy = (float)(v - iy);
        result.Clear();
        for (int dy = 0; dy < 2; dy++)
            for (int dx = 0; dx < 2; dx++)
            {
                int sx = Math.Clamp(ix + dx, 0, p.Width - 1), sy = Math.Clamp(iy + dy, 0, p.Height - 1);
                float w = (dx == 0 ? 1 - fx : fx) * (dy == 0 ? 1 - fy : fy);
                int o = (sy * p.Width + sx) * 4;
                float a = p.Rgba[o + 3] / 255f;
                result[0] += p.Rgba[o] / 255f * a * w;
                result[1] += p.Rgba[o + 1] / 255f * a * w;
                result[2] += p.Rgba[o + 2] / 255f * a * w;
                result[3] += a * w;
            }
        if (result[3] > 0) { result[0] /= result[3]; result[1] /= result[3]; result[2] /= result[3]; }
    }

    public Picture ToPicture()
    {
        var p = new Picture(Width, Height);
        for (int i = 0; i < _px.Length; i += 4)
        {
            float a = _px[i + 3];
            if (a <= 0) continue;
            p.Rgba[i] = (byte)Math.Clamp(_px[i] / a * 255 + 0.5f, 0, 255);
            p.Rgba[i + 1] = (byte)Math.Clamp(_px[i + 1] / a * 255 + 0.5f, 0, 255);
            p.Rgba[i + 2] = (byte)Math.Clamp(_px[i + 2] / a * 255 + 0.5f, 0, 255);
            p.Rgba[i + 3] = (byte)Math.Clamp(a * 255 + 0.5f, 0, 255);
        }
        return p;
    }
}
