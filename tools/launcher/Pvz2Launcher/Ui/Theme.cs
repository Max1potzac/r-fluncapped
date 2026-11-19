using System.Drawing;

namespace Pvz2Launcher.Ui;

/// A gradient button's colours: top and bottom of the gradient, and its outline.
public readonly record struct Gloss(Color Top, Color Bottom, Color Edge)
{
    public static Gloss Of(int top, int bottom, int edge) => new(Color.FromArgb(top | unchecked((int)0xFF000000)),
        Color.FromArgb(bottom | unchecked((int)0xFF000000)), Color.FromArgb(edge | unchecked((int)0xFF000000)));
}

/// The launcher's colours. Light is a sunny day on the lawn, Dark the night levels; both keep
/// PvZ2's wooden frames and glossy buttons.
public sealed class Palette
{
    public required string Name { get; init; }
    public bool IsDark { get; init; }

    // the window: sky, and lawn stripes along the bottom
    public Color SkyTop { get; init; }
    public Color SkyBottom { get; init; }
    public Color LawnA { get; init; }
    public Color LawnB { get; init; }
    public Color Speck { get; init; }      // clouds by day, stars by night

    // wooden frames around panels, and what's inside them
    public Color WoodLight { get; init; }
    public Color WoodDark { get; init; }
    public Color WoodEdge { get; init; }
    public Color Panel { get; init; }
    public Color PanelEdge { get; init; }

    // seed-packet cards (the mods), input boxes
    public Color Card { get; init; }
    public Color CardEdge { get; init; }
    public Color CardHover { get; init; }
    public Color CardSelected { get; init; }
    public Color Input { get; init; }

    public Color Text { get; init; }
    public Color TextMuted { get; init; }
    public Color TextOutline { get; init; }   // the dark rim of titles
    public Color Problem { get; init; }

    public Gloss Green { get; init; }
    public Gloss Yellow { get; init; }
    public Gloss Blue { get; init; }
    public Gloss Red { get; init; }
    public Gloss Wood { get; init; }
    public Gloss Plaque { get; init; }        // the title banner on a panel

    public static readonly Palette Light = new()
    {
        Name = "Light",
        SkyTop = Color.FromArgb(0x5B, 0xB8, 0xF2), SkyBottom = Color.FromArgb(0xBD, 0xE8, 0xFF),
        LawnA = Color.FromArgb(0x6C, 0xC2, 0x3A), LawnB = Color.FromArgb(0x5A, 0xAE, 0x2E),
        Speck = Color.FromArgb(220, 255, 255, 255),
        WoodLight = Color.FromArgb(0xC9, 0x8A, 0x4B), WoodDark = Color.FromArgb(0x8B, 0x55, 0x26), WoodEdge = Color.FromArgb(0x4E, 0x2C, 0x10),
        Panel = Color.FromArgb(0xFB, 0xEE, 0xC8), PanelEdge = Color.FromArgb(0xD8, 0xB6, 0x76),
        Card = Color.FromArgb(0xFF, 0xF9, 0xE6), CardEdge = Color.FromArgb(0xD6, 0xB0, 0x6A),
        CardHover = Color.FromArgb(0xFF, 0xF3, 0xCC), CardSelected = Color.FromArgb(0xFF, 0xE9, 0x9E),
        Input = Color.FromArgb(0xFF, 0xFD, 0xF5),
        Text = Color.FromArgb(0x3B, 0x24, 0x10), TextMuted = Color.FromArgb(0x85, 0x66, 0x45), TextOutline = Color.FromArgb(0x3A, 0x1F, 0x08),
        Problem = Color.FromArgb(0xC0, 0x32, 0x1E),
        Green = Gloss.Of(0x9DE85A, 0x3C9A19, 0x1F5A0B),
        Yellow = Gloss.Of(0xFFE85C, 0xF29A12, 0x8E4F00),
        Blue = Gloss.Of(0x86D8FF, 0x2274D6, 0x0F3E78),
        Red = Gloss.Of(0xFF8F73, 0xD13B27, 0x741808),
        Wood = Gloss.Of(0xE2BC84, 0xA36F37, 0x5B3913),
        Plaque = Gloss.Of(0xFFD963, 0xD99A2B, 0x6E3F06),
    };

    public static readonly Palette Dark = new()
    {
        Name = "Dark", IsDark = true,
        SkyTop = Color.FromArgb(0x0B, 0x10, 0x2A), SkyBottom = Color.FromArgb(0x23, 0x2E, 0x5C),
        LawnA = Color.FromArgb(0x23, 0x4A, 0x22), LawnB = Color.FromArgb(0x1C, 0x3D, 0x1C),
        Speck = Color.FromArgb(200, 255, 248, 214),
        WoodLight = Color.FromArgb(0x6E, 0x47, 0x26), WoodDark = Color.FromArgb(0x41, 0x27, 0x12), WoodEdge = Color.FromArgb(0x1C, 0x0F, 0x04),
        Panel = Color.FromArgb(0x1E, 0x25, 0x38), PanelEdge = Color.FromArgb(0x33, 0x3D, 0x5A),
        Card = Color.FromArgb(0x2A, 0x33, 0x4C), CardEdge = Color.FromArgb(0x42, 0x50, 0x75),
        CardHover = Color.FromArgb(0x31, 0x3C, 0x59), CardSelected = Color.FromArgb(0x3A, 0x4A, 0x36),
        Input = Color.FromArgb(0x15, 0x1B, 0x2B),
        Text = Color.FromArgb(0xF3, 0xF1, 0xE4), TextMuted = Color.FromArgb(0xA6, 0xB0, 0xC8), TextOutline = Color.FromArgb(0x0A, 0x0A, 0x14),
        Problem = Color.FromArgb(0xFF, 0x7A, 0x66),
        Green = Gloss.Of(0x7FD24A, 0x2E7A14, 0x0E3A05),
        Yellow = Gloss.Of(0xFFD94A, 0xD9800C, 0x5E3300),
        Blue = Gloss.Of(0x6CC2F5, 0x1B5FB8, 0x0A2A55),
        Red = Gloss.Of(0xF5806A, 0xB02E1E, 0x5A1206),
        Wood = Gloss.Of(0x9A6A3C, 0x5E3B1B, 0x2A1706),
        Plaque = Gloss.Of(0xE8B84A, 0xA8661A, 0x4A2604),
    };

    /// A category's colour, for the strip on a mod's card and its badge.
    public static Color CategoryColor(string category) => category.ToLowerInvariant() switch
    {
        "gameplay" => Color.FromArgb(0x4C, 0xB0, 0x2A),
        "controls" => Color.FromArgb(0x2D, 0x86, 0xD9),
        "fixes" => Color.FromArgb(0xEE, 0x8A, 0x14),
        "visuals" => Color.FromArgb(0xA3, 0x5B, 0xD6),
        "language" => Color.FromArgb(0x16, 0xA8, 0x9A),
        "levels" => Color.FromArgb(0x9A, 0x62, 0x2E),
        "network" => Color.FromArgb(0x55, 0x6F, 0x9E),
        "debug" => Color.FromArgb(0xD2, 0x3C, 0x3C),
        "code patches" => Color.FromArgb(0x6E, 0x6E, 0x6E),
        _ => Color.FromArgb(0xC9, 0xA2, 0x1A),
    };
}

/// The theme in use, and the launcher's fonts.
public static class Theme
{
    public static Palette Current { get; private set; } = Palette.Light;
    public static event Action? Changed;

    public static void Set(Palette p)
    {
        if (ReferenceEquals(p, Current)) return;
        Current = p;
        Changed?.Invoke();
    }

    /// "light", "dark", or "system" (Windows' own app setting).
    public static Palette FromSetting(string? setting) => setting?.ToLowerInvariant() switch
    {
        "dark" => Palette.Dark,
        "light" => Palette.Light,
        _ => SystemPrefersDark() ? Palette.Dark : Palette.Light,
    };

    static bool SystemPrefersDark()
    {
        if (!OperatingSystem.IsWindows()) return false;
        try
        {
            using var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize");
            return key?.GetValue("AppsUseLightTheme") is int v && v == 0;
        }
        catch (Exception) { return false; }
    }

    // A chunky face for titles and buttons, like the game's lettering; the first one installed.
    static readonly Lazy<FontFamily> Chunky = new(() => Pick("Segoe UI Black", "Arial Black", "Segoe UI", "Arial", "DejaVu Sans"));
    static readonly Lazy<FontFamily> Body = new(() => Pick("Segoe UI", "Arial", "DejaVu Sans"));

    static FontFamily Pick(params string[] names)
    {
        foreach (var n in names)
        {
            try { return new FontFamily(n); }
            catch (ArgumentException) { }
        }
        return FontFamily.GenericSansSerif;
    }

    public static Font Title(float size) => new(Chunky.Value, size, FontStyle.Regular, GraphicsUnit.Point);
    public static Font Text(float size, FontStyle style = FontStyle.Regular) => new(Body.Value, size, style, GraphicsUnit.Point);
}
