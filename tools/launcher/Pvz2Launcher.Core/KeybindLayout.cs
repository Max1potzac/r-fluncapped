using System.Globalization;
using System.Text;
using System.Text.Json;

namespace Pvz2Launcher;

/// The visual keybind editor's data: keys placed on a screenshot of the game, kept as
/// "<key> tap <x>% <y>%" lines of a keybinds file (rfu reads those at any resolution), and
/// controller buttons placed on it for one screen (the lawn, the seed chooser, the world map...),
/// kept as "<screen> <button> tap <x>% <y>%" lines of a controller layout file (rfu's
/// --pad-layout): on that screen the button taps there in place of what it does.
///
/// A layout can also be its own mod (SaveAsMod): a folder with the keybinds file, a layout.json
/// and the screenshot, for one game (and version), so a layout made for a fan mod whose buttons
/// sit elsewhere shows up only there. rfu adds a mod's keybinds over the game's own.
public static class KeybindLayout
{
    /// A key on the screen. X and Y are fractions (0 to 1) of the game's screen. State is the
    /// screen it's for ("any" for keyboard taps). Pad: a controller button (Key is rfu's name for
    /// it, "a", "lb", "start"...; State one of PadSetup.LayoutScreens).
    public sealed record Marker(string Key, float X, float Y, string State = "any", bool Pad = false);

    const string Begin = "# --- visual layout (made in the launcher's keybind editor) ---";
    const string End = "# --- end of visual layout ---";
    const string PadHeader = "# Controller buttons that tap a spot on one screen, in place of what they do there\n" +
                             "# (made in the launcher's keybind layout editor): <screen> <button> tap <x>% <y>%\n" +
                             "# Screens: lawn, chooser, menu, map, other, or any.";

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
        var w = Words(line);
        if (w.Length != 4 || !w[1].Equals("tap", StringComparison.OrdinalIgnoreCase)) return null;
        return Spot(w[2], w[3]) is var (x, y) ? new Marker(w[0], x, y) : null;
    }

    static string[] Words(string line)
    {
        int hash = line.IndexOf('#');
        if (hash >= 0) line = line[..hash];
        return line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
    }

    static (float, float)? Spot(string xs, string ys)
    {
        if (!xs.EndsWith('%') || !ys.EndsWith('%')) return null;
        if (!float.TryParse(xs[..^1], NumberStyles.Float, CultureInfo.InvariantCulture, out float x) ||
            !float.TryParse(ys[..^1], NumberStyles.Float, CultureInfo.InvariantCulture, out float y)) return null;
        return (Math.Clamp(x / 100, 0, 1), Math.Clamp(y / 100, 0, 1));
    }

    /// A controller layout line: "<screen> <button> tap <x>% <y>%".
    static Marker? ParsePad(string line)
    {
        var w = Words(line);
        if (w.Length != 5 || !w[2].Equals("tap", StringComparison.OrdinalIgnoreCase)) return null;
        string screen = w[0].ToLowerInvariant(), button = w[1].ToLowerInvariant();
        if (!PadSetup.LayoutScreens.Any(s => s.Name == screen) || !PadSetup.Buttons.Any(b => b.Name == button)) return null;
        return Spot(w[3], w[4]) is var (x, y) ? new Marker(button, x, y, screen, Pad: true) : null;
    }

    public static string Line(Marker m) => m.Pad
        ? string.Create(CultureInfo.InvariantCulture, $"{m.State} {m.Key} tap {m.X * 100:0.##}% {m.Y * 100:0.##}%")
        : string.Create(CultureInfo.InvariantCulture, $"{m.Key} tap {m.X * 100:0.##}% {m.Y * 100:0.##}%");

    /// The controller buttons placed on screens, from a controller layout file.
    public static List<Marker> ReadPad(string path)
    {
        var markers = new List<Marker>();
        if (!File.Exists(path)) return markers;
        foreach (var line in File.ReadLines(path))
            if (ParsePad(line) is { } m) markers.Add(m);
        return markers;
    }

    /// Puts the markers in a keybinds file in place of its percent taps, keeping every other line
    /// (seeds, buttons, scrolling, taps in pixels, comments).
    public static void Write(string keybindsPath, IReadOnlyList<Marker> markers) =>
        WriteBlock(keybindsPath, markers.Where(m => !m.Pad).ToList(), l => Parse(l) != null, null);

    /// Puts the controller markers in a controller layout file in place of its taps (a file with
    /// none left is removed, so rfu isn't handed an empty one).
    public static void WritePad(string path, IReadOnlyList<Marker> markers)
    {
        var pad = markers.Where(m => m.Pad).ToList();
        if (pad.Count == 0 && File.Exists(path) && File.ReadLines(path).All(l => ParsePad(l) != null || l.Trim() == "" || l.TrimStart().StartsWith('#')))
        {
            File.Delete(path);
            return;
        }
        if (pad.Count == 0 && !File.Exists(path)) return;
        WriteBlock(path, pad, l => ParsePad(l) != null, PadHeader);
    }

    static void WriteBlock(string path, List<Marker> markers, Func<string, bool> ours, string? header)
    {
        var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : header != null ? [.. header.Split('\n')] : [];
        int b = lines.IndexOf(Begin), e = lines.IndexOf(End);
        if (b >= 0 && e > b) lines.RemoveRange(b, e - b + 1);
        lines.RemoveAll(l => ours(l));
        while (lines.Count > 0 && lines[^1].Trim() == "") lines.RemoveAt(lines.Count - 1);
        if (markers.Count > 0)
        {
            lines.Add("");
            lines.Add(Begin);
            lines.AddRange(markers.Select(Line));
            lines.Add(End);
        }
        File.WriteAllText(path, string.Join(Environment.NewLine, lines) + Environment.NewLine);
    }

    /// A keybind layout as a mod in `modsDir`, for the game's package (and version, when given).
    /// Returns its folder.
    public static string SaveAsMod(string modsDir, string name, string? package, string? version, IReadOnlyList<Marker> markers, string? screenshot)
    {
        string folder = string.Concat(name.Split(Path.GetInvalidFileNameChars())).Trim();
        if (folder == "") folder = "Keybind layout";
        string dir = Path.Combine(modsDir, folder);
        Directory.CreateDirectory(dir);
        var keys = markers.Where(m => !m.Pad).ToList();
        var pads = markers.Where(m => m.Pad).ToList();
        var rfu = new List<string>();
        string keysFile = Path.Combine(dir, "keybinds.txt"), padFile = Path.Combine(dir, PadLayoutFile);
        if (keys.Count > 0 || pads.Count == 0)
        {
            var keybinds = new StringBuilder("# Keys placed on the screen in the launcher's keybind editor.\n");
            foreach (var m in keys) keybinds.Append(Line(m)).Append('\n');
            File.WriteAllText(keysFile, keybinds.ToString());
            rfu.AddRange(["--keybinds", "{mod}/keybinds.txt"]);
        }
        else if (File.Exists(keysFile)) File.Delete(keysFile);
        if (pads.Count > 0)
        {
            var pad = new StringBuilder(PadHeader).Append('\n');
            foreach (var m in pads) pad.Append(Line(m)).Append('\n');
            File.WriteAllText(padFile, pad.ToString());
            rfu.AddRange(["--pad-layout", "{mod}/" + PadLayoutFile]);
        }
        else if (File.Exists(padFile)) File.Delete(padFile);
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
            markers = markers.Select(m => new { key = m.Key, action = "tap", x = m.X, y = m.Y, state = m.State, device = m.Pad ? "pad" : "keyboard" }),
        }, opts));
        var manifest = new Dictionary<string, object?>
        {
            ["id"] = "layout-" + folder.ToLowerInvariant().Replace(' ', '-'),
            ["name"] = name,
            ["description"] = Describe(keys.Count, pads.Count),
            ["kind"] = "injection",
            ["category"] = "Controls",
            ["games"] = package != null ? new[] { package } : new[] { "*" },
            ["default"] = false,
            ["rfu"] = rfu,
            ["actions"] = new[] { new { label = "Edit layout", run = "edit-layout" } },
        };
        if (version != null) manifest["versions"] = new[] { version };
        File.WriteAllText(Path.Combine(dir, "mod.json"), JsonSerializer.Serialize(manifest, opts));
        return dir;
    }

    /// The controller layout file's name, in a game's folder or a layout mod's.
    public const string PadLayoutFile = "controller-layout.txt";

    static string Describe(int keys, int pads) =>
        (keys, pads) switch
        {
            (_, 0) => $"{keys} key(s) placed on the screen in the keybind editor.",
            (0, _) => $"{pads} controller button(s) placed on the screen in the keybind editor.",
            _ => $"{keys} key(s) and {pads} controller button(s) placed on the screen in the keybind editor.",
        };

    /// The markers of a layout mod (its layout.json, else its keybinds.txt and controller layout)
    /// and its screenshot.
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
                            m.TryGetProperty("state", out var st) && st.ValueKind == JsonValueKind.String ? st.GetString()! : "any",
                            m.TryGetProperty("device", out var dv) && dv.ValueKind == JsonValueKind.String && dv.GetString() == "pad"));
                return (markers, shot != null && File.Exists(shot) ? shot : null);
            }
            catch (Exception e) when (e is JsonException or KeyNotFoundException or InvalidOperationException or FormatException) { markers.Clear(); }
        }
        return ([.. Read(Path.Combine(dir, "keybinds.txt")), .. ReadPad(Path.Combine(dir, PadLayoutFile))], null);
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
