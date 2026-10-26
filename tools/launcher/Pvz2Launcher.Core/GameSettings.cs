using System.Text.Json;
using System.Text.Json.Serialization;

namespace Pvz2Launcher;

/// One game's launcher settings, kept as game.json in its folder. Everything the menu shows.
public sealed class GameSettings
{
    public string? DisplayName { get; set; }

    // what rfu draws
    public string Assets { get; set; } = "high";       // high (1536) or low (768)
    public string? Resolution { get; set; }             // "WxH", or null for rfu's default
    public string Window { get; set; } = "1280x720";
    public bool Fullscreen { get; set; }
    public bool VSync { get; set; } = true;
    public bool Angle { get; set; }

    // what the game does
    public bool Offline { get; set; }

    // code injections
    public bool HoverCollect { get; set; } = true;     // sun, coins and plant food picked up by moving the mouse over them
    public bool FixAnimations { get; set; } = true;    // plants that freeze on an animation while the level isn't running
    public bool Keybinds { get; set; }
    public List<string> Patches { get; set; } = [];     // file names in patches/ that are switched on

    // custom content: folder names in mods/ that are switched on, in load order (the last wins)
    public List<string> Mods { get; set; } = [];
    public bool AskForCustomContent { get; set; } = true;

    // The game's CDN archive for a version, {version} standing for the APK's version name. Empty: no
    // update check. Filled in for the games the launcher knows (see Defaults) while it's null.
    public string? CdnUrl { get; set; }

    public List<string> ExtraArgs { get; set; } = [];   // passed to rfu as they are

    static readonly JsonSerializerOptions Json = new()
    {
        WriteIndented = true,
        DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull,
        ReadCommentHandling = JsonCommentHandling.Skip,
        AllowTrailingCommas = true,
    };

    public static GameSettings Load(string path)
    {
        if (!File.Exists(path)) return new GameSettings();
        try { return JsonSerializer.Deserialize<GameSettings>(File.ReadAllText(path), Json) ?? new GameSettings(); }
        catch (JsonException) { return new GameSettings(); }
    }

    public void Save(string path) => File.WriteAllText(path, JsonSerializer.Serialize(this, Json));

    /// Settings for a game seen for the first time, by its package name.
    public static GameSettings Defaults(string? package, string folderName)
    {
        var s = new GameSettings { DisplayName = folderName };
        if (package == "com.ea.game.pvz2_rfl")
        {
            s.DisplayName = "PvZ2 Reflourished";
            // the URL libRFLib gives the game's CDN client
            s.CdnUrl = "https://reflourished.ducnguyen.org/cdn/{version}.main.tar";
        }
        return s;
    }
}
