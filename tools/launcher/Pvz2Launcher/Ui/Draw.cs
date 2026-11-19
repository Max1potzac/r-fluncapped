using System.Drawing;
using System.Drawing.Drawing2D;
using System.Drawing.Text;

namespace Pvz2Launcher.Ui;

/// The launcher's drawing: PvZ2's look of glossy outlined buttons, wooden frames, outlined
/// lettering and seed-packet cards, in plain GDI+ so the controls and the preview share it.
public static class Draw
{
    public enum State { Normal, Hover, Pressed, Disabled }

    public static void Smooth(Graphics g)
    {
        g.SmoothingMode = SmoothingMode.AntiAlias;
        g.PixelOffsetMode = PixelOffsetMode.HighQuality;
        g.TextRenderingHint = TextRenderingHint.AntiAliasGridFit;
        g.InterpolationMode = InterpolationMode.HighQualityBicubic;
    }

    public static GraphicsPath Round(RectangleF r, float radius)
    {
        var p = new GraphicsPath();
        float d = Math.Max(1, Math.Min(radius * 2, Math.Min(r.Width, r.Height)));
        if (r.Width <= 0 || r.Height <= 0) { p.AddRectangle(r); return p; }
        p.AddArc(r.X, r.Y, d, d, 180, 90);
        p.AddArc(r.Right - d, r.Y, d, d, 270, 90);
        p.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
        p.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
        p.CloseFigure();
        return p;
    }

    public static Color Mix(Color a, Color b, float t) => Color.FromArgb(
        (int)(a.A + (b.A - a.A) * t), (int)(a.R + (b.R - a.R) * t), (int)(a.G + (b.G - a.G) * t), (int)(a.B + (b.B - a.B) * t));

    static Color Lighter(Color c, float t) => Mix(c, Color.White, t);
    static Color Darker(Color c, float t) => Mix(c, Color.Black, t);

    /// The window's backdrop: the sky, a few clouds or stars, and the lawn's stripes at the bottom.
    public static void Backdrop(Graphics g, Rectangle r, Palette p, int lawnHeight = 70)
    {
        using (var sky = new LinearGradientBrush(r, p.SkyTop, p.SkyBottom, LinearGradientMode.Vertical))
            g.FillRectangle(sky, r);
        var rnd = new Random(7);
        if (p.IsDark)
        {
            for (int i = 0; i < 90; i++)
            {
                float x = r.X + (float)rnd.NextDouble() * r.Width, y = r.Y + (float)rnd.NextDouble() * (r.Height - lawnHeight) * 0.8f;
                float s = 1 + (float)rnd.NextDouble() * 2;
                using var b = new SolidBrush(Color.FromArgb(60 + rnd.Next(140), p.Speck));
                g.FillEllipse(b, x, y, s, s);
            }
            using var moon = new SolidBrush(Color.FromArgb(235, 255, 246, 205));
            g.FillEllipse(moon, r.Right - 330, r.Y + 8, 40, 40);
            using var bite = new SolidBrush(p.SkyTop);
            g.FillEllipse(bite, r.Right - 318, r.Y + 1, 38, 38);
        }
        else
        {
            for (int i = 0; i < 6; i++)
            {
                float cx = r.X + (float)rnd.NextDouble() * r.Width, cy = r.Y + 30 + (float)rnd.NextDouble() * (r.Height * 0.45f);
                float w = 90 + (float)rnd.NextDouble() * 110;
                using var b = new SolidBrush(Color.FromArgb(70, p.Speck));
                g.FillEllipse(b, cx, cy, w, w * 0.38f);
                g.FillEllipse(b, cx + w * 0.2f, cy - w * 0.16f, w * 0.55f, w * 0.42f);
            }
        }
        // the lawn: a row of alternating stripes, darker at the edge
        var lawn = new Rectangle(r.X, r.Bottom - lawnHeight, r.Width, lawnHeight);
        int stripe = 64;
        for (int x = lawn.X, i = 0; x < lawn.Right; x += stripe, i++)
        {
            using var b = new SolidBrush(i % 2 == 0 ? p.LawnA : p.LawnB);
            g.FillRectangle(b, x, lawn.Y, stripe, lawn.Height);
        }
        using (var shade = new LinearGradientBrush(new Rectangle(lawn.X, lawn.Y - 1, lawn.Width, 14), Color.FromArgb(90, 0, 0, 0), Color.FromArgb(0, 0, 0, 0), LinearGradientMode.Vertical))
            g.FillRectangle(shade, lawn.X, lawn.Y, lawn.Width, 14);
    }

    /// Text with a dark rim, the way PvZ2 letters its titles and buttons.
    public static void OutlinedText(Graphics g, string text, Font font, RectangleF r, Color fill, Color outline,
        float rim = 3f, StringAlignment align = StringAlignment.Center, StringAlignment lineAlign = StringAlignment.Center, bool shadow = true)
    {
        if (string.IsNullOrEmpty(text)) return;
        using var path = new GraphicsPath();
        using var fmt = new StringFormat { Alignment = align, LineAlignment = lineAlign, Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };
        float em = g.DpiY * font.SizeInPoints / 72f;
        path.AddString(text, font.FontFamily, (int)font.Style, em, r, fmt);
        if (shadow)
        {
            using var m = new Matrix();
            m.Translate(0, Math.Max(1.5f, rim * 0.6f));
            using var sp = (GraphicsPath)path.Clone();
            sp.Transform(m);
            using var sh = new Pen(Color.FromArgb(110, 0, 0, 0), rim * 2) { LineJoin = LineJoin.Round };
            g.DrawPath(sh, sp);
        }
        using (var pen = new Pen(outline, rim * 2) { LineJoin = LineJoin.Round })
            g.DrawPath(pen, path);
        using var brush = new SolidBrush(fill);
        g.FillPath(brush, path);
    }

    /// A glossy PvZ2 button: a gradient body with a dark rim, a shine across the top half and an
    /// outlined label. Pressed, it sinks onto its shadow.
    public static void Button(Graphics g, RectangleF r, Gloss c, string text, Font font, State state = State.Normal, float radius = -1, Image? icon = null)
    {
        if (radius < 0) radius = Math.Min(r.Height / 2.6f, 16);
        if (state == State.Disabled)
        {
            var gray = Mix(c.Top, Color.Gray, 0.75f);
            c = new Gloss(Lighter(gray, 0.2f), Darker(gray, 0.15f), Darker(gray, 0.5f));
        }
        else if (state == State.Hover) c = new Gloss(Lighter(c.Top, 0.18f), Lighter(c.Bottom, 0.12f), c.Edge);
        else if (state == State.Pressed) c = new Gloss(Darker(c.Top, 0.08f), Darker(c.Bottom, 0.12f), c.Edge);

        float drop = Math.Max(3, r.Height * 0.08f);
        bool down = state == State.Pressed;
        var body = new RectangleF(r.X + 1, r.Y + 1 + (down ? drop : 0), r.Width - 2, r.Height - 2 - drop);
        // the shadow underneath: the button's rim colour
        using (var sp = Round(new RectangleF(body.X, r.Y + 1 + drop, body.Width, body.Height), radius))
        using (var sb = new SolidBrush(Darker(c.Edge, 0.1f)))
            g.FillPath(sb, sp);
        using var path = Round(body, radius);
        using (var fill = new LinearGradientBrush(body, c.Top, c.Bottom, LinearGradientMode.Vertical))
        {
            fill.InterpolationColors = new ColorBlend
            {
                Colors = [Lighter(c.Top, 0.05f), c.Top, Mix(c.Top, c.Bottom, 0.55f), c.Bottom],
                Positions = [0f, 0.35f, 0.55f, 1f],
            };
            g.FillPath(fill, path);
        }
        // the shine
        var shineR = new RectangleF(body.X + body.Height * 0.18f, body.Y + 2.5f, body.Width - body.Height * 0.36f, body.Height * 0.44f);
        if (shineR.Width > 4 && shineR.Height > 2)
        {
            using var shine = Round(shineR, Math.Min(radius, shineR.Height / 2));
            using var sb = new LinearGradientBrush(new RectangleF(shineR.X, shineR.Y - 1, shineR.Width, shineR.Height + 2),
                Color.FromArgb(state == State.Disabled ? 50 : 150, 255, 255, 255), Color.FromArgb(10, 255, 255, 255), LinearGradientMode.Vertical);
            g.FillPath(sb, shine);
        }
        using (var rim = new Pen(c.Edge, 2.2f))
            g.DrawPath(rim, path);

        var label = body;
        if (icon != null)
        {
            float s = Math.Min(body.Height - 10, 28);
            float total = s + 6 + g.MeasureString(text, font).Width;
            float x = body.X + (body.Width - total) / 2;
            g.DrawImage(icon, x, body.Y + (body.Height - s) / 2, s, s);
            label = new RectangleF(x + s + 6, body.Y, body.Right - x - s - 6, body.Height);
            OutlinedText(g, text, font, label, Color.White, c.Edge, Math.Max(1.6f, font.SizeInPoints / 6), StringAlignment.Near);
            return;
        }
        OutlinedText(g, text, font, label, state == State.Disabled ? Color.FromArgb(235, 235, 235) : Color.White, c.Edge, Math.Max(1.6f, font.SizeInPoints / 6));
    }

    /// A wooden frame around a panel, PvZ2's popups: the planks, a darker rim, and the panel inside.
    /// With a title, a plaque sits on the top edge. Returns the inside.
    public static RectangleF WoodFrame(Graphics g, RectangleF r, Palette p, string? title = null, Font? titleFont = null, float border = 9)
    {
        float top = title != null ? 16 : 0;
        var frame = new RectangleF(r.X + 1, r.Y + 1 + top, r.Width - 2, r.Height - 6 - top);
        // drop shadow
        using (var sp = Round(new RectangleF(frame.X + 2, frame.Y + 5, frame.Width, frame.Height), 16))
        using (var sb = new SolidBrush(Color.FromArgb(70, 0, 0, 0)))
            g.FillPath(sb, sp);
        using (var path = Round(frame, 16))
        {
            using (var wood = new LinearGradientBrush(frame, p.WoodLight, p.WoodDark, LinearGradientMode.Vertical))
                g.FillPath(wood, path);
            // grain
            var rnd = new Random((int)(r.Width * 31 + r.Height));
            var clip = g.Clip;
            g.SetClip(path, CombineMode.Intersect);
            for (float y = frame.Y + 4; y < frame.Bottom; y += 5 + (float)rnd.NextDouble() * 6)
            {
                using var grain = new Pen(Color.FromArgb(28, p.WoodEdge), 1);
                g.DrawLine(grain, frame.X, y, frame.Right, y + (float)(rnd.NextDouble() * 2 - 1));
            }
            g.Clip = clip;
            using var rim = new Pen(p.WoodEdge, 2.5f);
            g.DrawPath(rim, path);
        }
        var inner = RectangleF.Inflate(frame, -border, -border);
        if (title != null) inner = new RectangleF(inner.X, inner.Y + 12, inner.Width, inner.Height - 12);
        using (var ip = Round(inner, 10))
        {
            using (var fill = new LinearGradientBrush(inner, Lighter(p.Panel, p.IsDark ? 0.04f : 0.25f), p.Panel, LinearGradientMode.Vertical))
                g.FillPath(fill, ip);
            using var edge = new Pen(p.PanelEdge, 2f);
            g.DrawPath(edge, ip);
            using var inset = new Pen(Color.FromArgb(p.IsDark ? 90 : 50, 0, 0, 0), 1.2f);
            using var ip2 = Round(RectangleF.Inflate(inner, -1.5f, -1.5f), 9);
            g.DrawPath(inset, ip2);
        }
        if (title != null)
        {
            titleFont ??= Theme.Title(11);
            float w = Math.Min(frame.Width - 40, g.MeasureString(title, titleFont).Width + 44);
            var plaque = new RectangleF(frame.X + (frame.Width - w) / 2, r.Y + 1, w, 32);
            Plaque(g, plaque, p, title, titleFont);
        }
        return inner;
    }

    /// The banner a PvZ2 popup has its title on.
    public static void Plaque(Graphics g, RectangleF r, Palette p, string text, Font font)
    {
        var c = p.Plaque;
        // ribbon tails
        using (var tail = new SolidBrush(Darker(c.Bottom, 0.25f)))
        {
            g.FillPolygon(tail, new[] { new PointF(r.X + 6, r.Y + 8), new PointF(r.X - 12, r.Y + 10), new PointF(r.X - 4, r.Y + r.Height / 2 + 4), new PointF(r.X - 12, r.Bottom - 2), new PointF(r.X + 6, r.Bottom - 4) });
            g.FillPolygon(tail, new[] { new PointF(r.Right - 6, r.Y + 8), new PointF(r.Right + 12, r.Y + 10), new PointF(r.Right + 4, r.Y + r.Height / 2 + 4), new PointF(r.Right + 12, r.Bottom - 2), new PointF(r.Right - 6, r.Bottom - 4) });
        }
        using var path = Round(r, 8);
        using (var fill = new LinearGradientBrush(r, Lighter(c.Top, 0.1f), c.Bottom, LinearGradientMode.Vertical))
            g.FillPath(fill, path);
        using (var shine = Round(new RectangleF(r.X + 8, r.Y + 3, r.Width - 16, r.Height * 0.38f), 5))
        using (var sb = new SolidBrush(Color.FromArgb(90, 255, 255, 255)))
            g.FillPath(sb, shine);
        using (var rim = new Pen(c.Edge, 2.2f))
            g.DrawPath(rim, path);
        OutlinedText(g, text, font, r, Color.White, c.Edge, 2.4f);
    }

    /// A small rounded label: a mod's category, its kind, the games it's for.
    public static float Badge(Graphics g, PointF at, string text, Color color, Font font, bool filled = true)
    {
        var size = g.MeasureString(text, font);
        var r = new RectangleF(at.X, at.Y, size.Width + 10, size.Height + 2);
        using var path = Round(r, r.Height / 2);
        if (filled)
        {
            using var b = new LinearGradientBrush(r, Lighter(color, 0.15f), Darker(color, 0.1f), LinearGradientMode.Vertical);
            g.FillPath(b, path);
            using var t = new SolidBrush(Color.White);
            g.DrawString(text, font, t, r.X + 5, r.Y + 1);
        }
        else
        {
            using var pen = new Pen(color, 1.4f);
            g.DrawPath(pen, path);
            using var t = new SolidBrush(color);
            g.DrawString(text, font, t, r.X + 5, r.Y + 1);
        }
        return r.Width;
    }

    /// An on/off switch: a green glossy pill with the knob on the right when on.
    public static void Switch(Graphics g, RectangleF r, bool on, Palette p, bool enabled = true, bool hover = false)
    {
        using var path = Round(r, r.Height / 2);
        var c = on ? p.Green : new Gloss(p.IsDark ? Color.FromArgb(0x4A, 0x52, 0x66) : Color.FromArgb(0xD5, 0xCB, 0xB4),
                                         p.IsDark ? Color.FromArgb(0x33, 0x3A, 0x4C) : Color.FromArgb(0xB3, 0xA5, 0x88),
                                         p.IsDark ? Color.FromArgb(0x1A, 0x1E, 0x2A) : Color.FromArgb(0x7C, 0x6A, 0x4A));
        if (!enabled) c = new Gloss(Mix(c.Top, Color.Gray, 0.6f), Mix(c.Bottom, Color.Gray, 0.6f), Mix(c.Edge, Color.Gray, 0.5f));
        using (var fill = new LinearGradientBrush(r, c.Bottom, c.Top, LinearGradientMode.Vertical))
            g.FillPath(fill, path);
        using (var rim = new Pen(c.Edge, 1.8f))
            g.DrawPath(rim, path);
        float d = r.Height - 6;
        var knob = new RectangleF(on ? r.Right - d - 3 : r.X + 3, r.Y + 3, d, d);
        using (var ks = new SolidBrush(Color.FromArgb(60, 0, 0, 0)))
            g.FillEllipse(ks, knob.X, knob.Y + 1.5f, knob.Width, knob.Height);
        using (var kb = new LinearGradientBrush(knob, Color.White, hover ? Color.FromArgb(0xF7, 0xE7, 0xB0) : Color.FromArgb(0xE2, 0xE2, 0xE2), LinearGradientMode.Vertical))
            g.FillEllipse(kb, knob);
        using (var ke = new Pen(Color.FromArgb(120, c.Edge), 1.2f))
            g.DrawEllipse(ke, knob);
    }

    /// A mod as a seed packet: a card with a coloured strip for its category down the left.
    public static void Card(Graphics g, RectangleF r, Palette p, Color strip, bool selected, bool hover, bool dim)
    {
        using var path = Round(r, 10);
        using (var sh = Round(new RectangleF(r.X + 1, r.Y + 3, r.Width, r.Height), 10))
        using (var sb = new SolidBrush(Color.FromArgb(p.IsDark ? 90 : 45, 0, 0, 0)))
            g.FillPath(sb, sh);
        var fillC = selected ? p.CardSelected : hover ? p.CardHover : p.Card;
        using (var fill = new LinearGradientBrush(r, Lighter(fillC, p.IsDark ? 0.03f : 0.3f), fillC, LinearGradientMode.Vertical))
            g.FillPath(fill, path);
        var clip = g.Clip;
        g.SetClip(path, CombineMode.Intersect);
        using (var sb = new LinearGradientBrush(new RectangleF(r.X, r.Y, 10, r.Height), Lighter(strip, 0.2f), Darker(strip, 0.15f), LinearGradientMode.Vertical))
            g.FillRectangle(sb, r.X, r.Y, 9, r.Height);
        g.Clip = clip;
        using (var rim = new Pen(selected ? p.Plaque.Bottom : p.CardEdge, selected ? 2.4f : 1.4f))
            g.DrawPath(rim, path);
        if (dim)
        {
            using var veil = new SolidBrush(Color.FromArgb(p.IsDark ? 120 : 110, p.Panel));
            g.FillPath(veil, path);
        }
    }

    /// What a mod's card shows.
    public sealed record ModCardInfo(string Name, string Byline, string Description, string Category, string Kind, string Games,
        bool On, bool Fits, string? Note, bool NoteIsProblem);

    public const int ModCardHeight = 70;
    public static RectangleF ModSwitchRect(RectangleF card) => new(card.X + 22, card.Y + (card.Height - 24) / 2, 46, 24);

    /// A mod as a seed-packet card: its switch, name, byline and description, and badges for its
    /// kind, category and the games it's for.
    public static void ModCard(Graphics g, RectangleF r, Palette p, ModCardInfo m, bool selected, bool hover)
    {
        var col = Palette.CategoryColor(m.Category);
        Card(g, r, p, col, selected, hover, false);
        Switch(g, ModSwitchRect(r), m.On, p, m.Fits, hover);
        using var nf = Theme.Text(10.5f, FontStyle.Bold);
        using var df = Theme.Text(9f);
        using var bf = Theme.Text(8f, FontStyle.Bold);
        using var tb = new SolidBrush(m.Fits ? p.Text : p.TextMuted);
        using var mb = new SolidBrush(p.TextMuted);
        float x = r.X + 82;
        // badges from the right
        float bx = r.Right - 12;
        var muted = p.TextMuted;
        var kindColor = p.IsDark ? p.Blue.Top : p.Blue.Bottom;
        foreach (var (t, c, fill) in new[] { (m.Games, muted, false), (m.Category, col, true), (m.Kind, kindColor, false) })
        {
            if (t == "") continue;
            float w = g.MeasureString(t, bf).Width + 10;
            bx -= w;
            Badge(g, new PointF(bx, r.Y + 10), t, c, bf, fill);
            bx -= 5;
        }
        var nameR = new RectangleF(x, r.Y + 8, Math.Max(40, bx - x - 8), 22);
        using var fmt = new StringFormat { Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };
        g.DrawString(m.Name, nf, tb, nameR, fmt);
        float nameW = Math.Min(g.MeasureString(m.Name, nf).Width, nameR.Width);
        if (m.Byline != "" && x + nameW + 20 < bx)
            g.DrawString(m.Byline, df, mb, new RectangleF(x + nameW + 2, r.Y + 10, bx - x - nameW - 6, 20), fmt);
        float noteW = 0;
        if (m.Note != null)
        {
            noteW = Math.Min(g.MeasureString(m.Note, bf).Width + 4, r.Width * 0.4f);
            using var nb = new SolidBrush(m.NoteIsProblem ? p.Problem : p.TextMuted);
            g.DrawString(m.Note, bf, nb, new RectangleF(r.Right - 12 - noteW, r.Y + 42, noteW, 18), fmt);
        }
        using var dfmt = new StringFormat { Trimming = StringTrimming.EllipsisWord };
        g.DrawString(m.Description, df, mb, new RectangleF(x, r.Y + 32, r.Right - x - 16 - noteW, r.Height - 36), dfmt);
    }

    /// A game in the list: its icon, name and version, with its family's colour down the side.
    public static void GameCard(Graphics g, RectangleF r, Palette p, Image? icon, string name, string detail, Color family, bool selected, bool hover, bool problem)
    {
        Card(g, r, p, family, selected, hover, false);
        var ir = new RectangleF(r.X + 16, r.Y + (r.Height - 44) / 2, 44, 44);
        if (icon != null)
        {
            using var clip = Round(ir, 10);
            var old = g.Clip;
            g.SetClip(clip, CombineMode.Intersect);
            g.DrawImage(icon, ir);
            g.Clip = old;
            using var e = new Pen(Color.FromArgb(90, 0, 0, 0), 1.2f);
            g.DrawPath(e, clip);
        }
        else
        {
            using var b = new LinearGradientBrush(ir, p.Green.Top, p.Green.Bottom, LinearGradientMode.Vertical);
            using var path = Round(ir, 10);
            g.FillPath(b, path);
            using var f = Theme.Title(14);
            OutlinedText(g, name.Length > 0 ? name[..1].ToUpperInvariant() : "?", f, ir, Color.White, p.Green.Edge, 2);
        }
        using var nf = Theme.Text(10.5f, FontStyle.Bold);
        using var sf = Theme.Text(8.5f);
        using var tb = new SolidBrush(p.Text);
        using var mb = new SolidBrush(problem ? p.Problem : p.TextMuted);
        using var fmt = new StringFormat { Trimming = StringTrimming.EllipsisCharacter, FormatFlags = StringFormatFlags.NoWrap };
        g.DrawString(name, nf, tb, new RectangleF(r.X + 68, r.Y + 10, r.Width - 76, 22), fmt);
        g.DrawString(detail, sf, mb, new RectangleF(r.X + 68, r.Y + 33, r.Width - 76, 20), fmt);
    }
}
