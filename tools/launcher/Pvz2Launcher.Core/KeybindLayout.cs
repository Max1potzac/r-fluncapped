using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Pvz2Launcher;

/// The visual keybind editor's data: keys placed on a screenshot of the game, kept as
/// "<key> tap <x>% <y>%" lines of a keybinds file (rfu reads those at any resolution).
///
/// A layout can also be its own mod (SaveAsMod): a folder with the keybinds file, a layout.json
/// and the screenshot, for one game (and version), so a layout made for a fan mod whose buttons
/// sit elsewhere shows up only there. rfu adds a mod's keybinds over the game's own.
public static class KeybindLayout
{
    /// A key on the screen. X and Y are fractions (0 to 1) of the game's screen. State is the
    /// screen it's for ("any" for keyboard taps; the controller's per-screen positions use it).
    public sealed record Marker(string Key, float X, float Y, string State = "any");

    const string Begin = "# --- visual layout (made in the launcher's keybind editor) ---";
    const string End = "# --- end of visual layout ---";

    /// The tap keybinds in percent from a keybinds file. Taps in pixels are left out (the editor
    /// leaves those lines alone).
    public static List<Marker> Read(string keybindsPath)
    {
        var markers = new List<Marker>();
        if (!File.Exists(keybindsPath)) return markers;
        foreach (var line in File.ReadLines(keybindsPath))
            if (Parse(line) is { } m) markers.Add(m);
        return markers;
    }

    static Marker? Parse(string line)
    {
        int hash = line.IndexOf('#');
        if (hash >= 0) line = line[..hash];
        var w = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
        if (w.Length != 4 || !w[1].Equals("tap", StringComparison.OrdinalIgnoreCase)) return null;
        if (!w[2].EndsWith('%') || !w[3].EndsWith('%')) return null;
        if (!float.TryParse(w[2][..^1], NumberStyles.Float, CultureInfo.InvariantCulture, out float x) ||
            !float.TryParse(w[3][..^1], NumberStyles.Float, CultureInfo.InvariantCulture, out float y)) return null;
        return new Marker(w[0], Math.Clamp(x / 100, 0, 1), Math.Clamp(y / 100, 0, 1));
    }

    public static string Line(Marker m) =>
        string.Create(CultureInfo.InvariantCulture, $"{m.Key} tap {m.X * 100:0.##}% {m.Y * 100:0.##}%");

    /// Puts the markers in a keybinds file in place of its percent taps, keeping every other line
    /// (seeds, buttons, scrolling, taps in pixels, comments).
    public static void Write(string keybindsPath, IReadOnlyList<Marker> markers)
    {
        var lines = File.Exists(keybindsPath) ? File.ReadAllLines(keybindsPath).ToList() : [];
        int b = lines.IndexOf(Begin), e = lines.IndexOf(End);
        if (b >= 0 && e > b) lines.RemoveRange(b, e - b + 1);
        lines.RemoveAll(l => Parse(l) != null);
        while (lines.Count > 0 && lines[^1].Trim() == "") lines.RemoveAt(lines.Count - 1);
        if (markers.Count > 0)
        {
            lines.Add("");
            lines.Add(Begin);
            lines.AddRange(markers.Select(Line));
            lines.Add(End);
        }
        File.WriteAllText(keybindsPath, string.Join(Environment.NewLine, lines) + Environment.NewLine);
    }

    /// A keybind layout as a mod in `modsDir`, for the game's package (and version, when given).
    /// Returns its folder.
    public static string SaveAsMod(string modsDir, string name, string? package, string? version, IReadOnlyList<Marker> markers, string? screenshot)
    {
        string folder = string.Concat(name.Split(Path.GetInvalidFileNameChars())).Trim();
        if (folder == "") folder = "Keybind layout";
        string dir = Path.Combine(modsDir, folder);
        Directory.CreateDirectory(dir);
        var keybinds = new StringBuilder("# Keys placed on the screen in the launcher's keybind editor.\n");
        foreach (var m in markers) keybinds.Append(Line(m)).Append('\n');
        File.WriteAllText(Path.Combine(dir, "keybinds.txt"), keybinds.ToString());
        string? shot = null;
        if (screenshot != null && File.Exists(screenshot))
        {
            shot = "screenshot" + Path.GetExtension(screenshot).ToLowerInvariant();
            if (!string.Equals(Path.GetFullPath(screenshot), Path.GetFullPath(Path.Combine(dir, shot)), StringComparison.OrdinalIgnoreCase))
                File.Copy(screenshot, Path.Combine(dir, shot), true);
        }
        var opts = new JsonSerializerOptions { WriteIndented = true };
        File.WriteAllText(Path.Combine(dir, "layout.json"), JsonSerializer.Serialize(new
        {
            version = 1,
            screenshot = shot,
            markers = markers.Select(m => new { key = m.Key, action = "tap", x = m.X, y = m.Y, state = m.State }),
        }, opts));
        var manifest = new Dictionary<string, object?>
        {
            ["id"] = "layout-" + folder.ToLowerInvariant().Replace(' ', '-'),
            ["name"] = name,
            ["description"] = $"{markers.Count} key(s) placed on the screen in the keybind editor.",
            ["kind"] = "injection",
            ["category"] = "Controls",
            ["games"] = package != null ? new[] { package } : new[] { "*" },
            ["default"] = false,
            ["rfu"] = new[] { "--keybinds", "{mod}/keybinds.txt" },
            ["actions"] = new[] { new { label = "Edit layout", run = "edit-layout" } },
        };
        if (version != null) manifest["versions"] = new[] { version };
        File.WriteAllText(Path.Combine(dir, "mod.json"), JsonSerializer.Serialize(manifest, opts));
        return dir;
    }

    /// The markers of a layout mod (its layout.json, else its keybinds.txt) and its screenshot.
    public static (List<Marker> Markers, string? Screenshot) ReadMod(string dir)
    {
        string? shot = null;
        var markers = new List<Marker>();
        string layout = Path.Combine(dir, "layout.json");
        if (File.Exists(layout))
        {
            try
            {
                using var doc = JsonDocument.Parse(File.ReadAllText(layout));
                if (doc.RootElement.TryGetProperty("screenshot", out var s) && s.ValueKind == JsonValueKind.String) shot = Path.Combine(dir, s.GetString()!);
                if (doc.RootElement.TryGetProperty("markers", out var ms) && ms.ValueKind == JsonValueKind.Array)
                    foreach (var m in ms.EnumerateArray())
                        markers.Add(new Marker(m.GetProperty("key").GetString() ?? "?", m.GetProperty("x").GetSingle(), m.GetProperty("y").GetSingle(),
                            m.TryGetProperty("state", out var st) && st.ValueKind == JsonValueKind.String ? st.GetString()! : "any"));
                return (markers, shot != null && File.Exists(shot) ? shot : null);
            }
            catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException or FormatException) { markers.Clear(); }
        }
        return (Read(Path.Combine(dir, "keybinds.txt")), null);
    }
}

/// The launcher's own settings, launcher.json beside it: the theme.
public sealed class LauncherSettings
{
    public string Theme { get; set; } = "system";   // system, light or dark

    public static LauncherSettings Load(string dir)
    {
        try
        {
            string path = Path.Combine(dir, "launcher.json");
            if (File.Exists(path)) return JsonSerializer.Deserialize<LauncherSettings>(File.ReadAllText(path)) ?? new();
        }
        catch (Exception e) when (e is JsonException or IOException) { }
        return new();
    }

    public void Save(string dir)
    {
        try { File.WriteAllText(Path.Combine(dir, "launcher.json"), JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true })); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
}
