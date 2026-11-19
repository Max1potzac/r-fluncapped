using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Pvz2Launcher;

/// Mods: everything the launcher can switch on for a game, each a folder with a mod.json.
///
/// A mod lives in the launcher's shared mods/ folder (for every game it fits) or in a game's own
/// mods/ (for that game only; it hides a shared one of the same folder name). The launcher's own
/// code injections are mods too: it writes them into the shared folder the first time it starts
/// (BuiltinMods), so they can be read, changed, copied and deleted like any other.
///
///   mod.json        { "id", "name", "description", "author", "version",
///                     "kind":     "injection" | "content" | "localization" (worked out when left out),
///                     "category": "Gameplay", "Controls", "Fixes", "Visuals", "Language", "Levels" ...,
///                     "games":    [ "global" | "reflourished" | "pvz2" | "china" | "other" | a package | "*" ],
///                     "versions": [ "1.5" ... ]  the game's version starts with one of these,
///                     "default":  true to switch it on for a game that hasn't seen it yet,
///                     "rfu":      [ rfu arguments; {game}, {mod} and {data} stand for those folders ],
///                     "patches":  [ code patch files in the mod (default: every *.patch in it) ],
///                     "requiresAny": [ ids, one of which must be on too ],
///                     "when":     "offline" | "online",
///                     "createDirs": [ folders made before the game starts ],
///                     "hooks":    [ launcher features: "keybinds-file", "pad-binds" ],
///                     "actions":  [ { "label", "run": "edit-keybinds" | "controller-settings" | "open:<path>" } ],
///                     "language": "en-US", "strings": "strings.json"   (localization) }
///   cdn/...         content over the game's CDN folder (No_Backup/CDN.<version>: levels, strings,
///                   liveconfig ...), the content the game downloads and RFLib reads
///   files/...       content over the game's external files folder (what Android calls
///                   /sdcard/Android/data/<package>/files)
///
/// rfu lays content over the game's files read-only (--overlay), so nothing of the game's is
/// changed and switching a mod off is just not passing it. A folder with no mod.json and neither
/// cdn/ nor files/ is all CDN content, as level packs ship it: loose level files (anything with a
/// LevelDefinition) go to levels/, a LawnStrings file to the CDN folder itself. Files are converted
/// on the way to what the game expects: x.json becomes x.rton where the game's own file is x.rton,
/// and x.rton.json always becomes x.rton. Mods later in the load order win where two have the same
/// file, except LawnStrings: a mod's strings are merged over the game's current ones (and over
/// earlier mods'), so text the mod doesn't have isn't lost, and the game's CDN download no longer
/// wipes them out. A localization's strings file is a plain { "KEY": "text" } object, merged the
/// same way over the game's LawnStrings-<language>.
///
/// The code patch files in a game's patches/ folder show up as injection mods of their own
/// ("patches/<file>").
public static class Mods
{
    public enum ModKind { Injection, Content, Localization }
    public enum ModSource { Shared, Game, PatchFile }
    public sealed record ModAction(string Label, string Run);

    public sealed class Mod
    {
        public required string Dir { get; init; }
        /// What game.json calls it: the folder name, or patches/<file> for a loose code patch.
        public required string Key { get; init; }
        public required string FolderName { get; init; }
        public required string Id { get; init; }
        public required string Name { get; init; }
        public string Description { get; init; } = "";
        public string Author { get; init; } = "";
        public string Version { get; init; } = "";
        public ModKind Kind { get; init; }
        public string Category { get; init; } = "";
        public IReadOnlyList<string> Games { get; init; } = [];
        public IReadOnlyList<string> Versions { get; init; } = [];
        public bool DefaultOn { get; init; }
        public IReadOnlyList<string> Rfu { get; init; } = [];
        /// Full paths of its code patch files.
        public IReadOnlyList<string> Patches { get; init; } = [];
        public IReadOnlyList<string> RequiresAny { get; init; } = [];
        public string? When { get; init; }
        public IReadOnlyList<string> CreateDirs { get; init; } = [];
        public IReadOnlyList<string> Hooks { get; init; } = [];
        public IReadOnlyList<ModAction> Actions { get; init; } = [];
        public string? Language { get; init; }
        /// Full path of a localization's strings file.
        public string? Strings { get; init; }
        public ModSource Source { get; init; }
        /// The mod has files to lay over the game's (cdn/, files/, loose content, or strings).
        public bool HasContent { get; init; }
        /// What's wrong with its mod.json, or null.
        public string? Problem { get; init; }
        public bool IsBuiltin => BuiltinMods.Folders.Contains(FolderName) && Source == ModSource.Shared;

        /// The games it's for, as the menu shows them: "All games", "Reflourished, PvZ2"...
        public string GamesLabel => Games.Count == 0 || Games.Contains("*") ? "All games" : string.Join(", ", Games.Select(FamilyLabel));
    }

    /// A game family's name for people.
    public static string FamilyLabel(string family) => family.ToLowerInvariant() switch
    {
        "global" => "PvZ2 & fan mods",
        "reflourished" => "Reflourished",
        "pvz2" => "PvZ2",
        "china" => "PvZ2 China",
        "other" => "Other fan mods",
        "*" => "All games",
        _ => family,
    };

    static readonly string[] Roots = ["cdn", "files"];
    static readonly HashSet<string> NotContent = new(StringComparer.OrdinalIgnoreCase) { "mod.json", "missing-strings.json", "readme.txt", "readme.md", "license", "license.txt" };
    static readonly JsonDocumentOptions ManifestOptions = new() { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true };

    /// Every mod the game can see: its own, the shared ones it doesn't hide, and its loose code
    /// patches, by name. Mods for other games are included (see AppliesTo).
    public static List<Mod> List(GameFolder game)
    {
        var mods = new Dictionary<string, Mod>(StringComparer.OrdinalIgnoreCase);
        foreach (var (dir, source) in new[] { (game.SharedModsDir, ModSource.Shared), (game.ModsDir, ModSource.Game) })
            if (Directory.Exists(dir))
                foreach (var d in Directory.EnumerateDirectories(dir).Where(d => !Path.GetFileName(d).StartsWith('.')))
                    mods[Path.GetFileName(d)] = Read(d, source);
        foreach (var p in game.PatchFiles())
            mods["patches/" + p.FileName] = new Mod
            {
                Dir = game.PatchesDir, Key = "patches/" + p.FileName, FolderName = p.FileName, Id = "patches/" + p.FileName,
                Name = p.Name, Description = p.Description, Kind = ModKind.Injection, Category = "Code patches",
                Patches = [p.Path], Source = ModSource.PatchFile,
            };
        return mods.Values.OrderBy(m => m.Name, StringComparer.OrdinalIgnoreCase).ToList();
    }

    public static Mod? Find(GameFolder game, string key) =>
        List(game).FirstOrDefault(m => string.Equals(m.Key, key, StringComparison.OrdinalIgnoreCase));

    public static Mod Read(string dir) => Read(dir, ModSource.Game);

    public static Mod Read(string dir, ModSource source)
    {
        string folder = Path.GetFileName(dir);
        string? problem = null;
        JsonElement r = default;
        JsonDocument? doc = null;
        var manifest = Path.Combine(dir, "mod.json");
        if (File.Exists(manifest))
        {
            try { doc = JsonDocument.Parse(File.ReadAllText(manifest), ManifestOptions); r = doc.RootElement; }
            catch (JsonException e) { problem = $"mod.json: {e.Message}"; }
            catch (IOException e) { problem = $"mod.json: {e.Message}"; }
        }
        using var manifestDoc = doc;
        bool has = r.ValueKind == JsonValueKind.Object;
        string Get(string key) => has && r.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString()!.Trim() : "";
        List<string> List(string key) => has && r.TryGetProperty(key, out var v)
            ? v.ValueKind == JsonValueKind.Array ? v.EnumerateArray().Where(e => e.ValueKind == JsonValueKind.String).Select(e => e.GetString()!).ToList()
              : v.ValueKind == JsonValueKind.String ? [v.GetString()!] : []
            : [];
        bool defaultOn = has && r.TryGetProperty("default", out var dv) && dv.ValueKind == JsonValueKind.True;

        var actions = new List<ModAction>();
        if (has && r.TryGetProperty("actions", out var av) && av.ValueKind == JsonValueKind.Array)
            foreach (var a in av.EnumerateArray().Where(a => a.ValueKind == JsonValueKind.Object))
                if (a.TryGetProperty("label", out var l) && a.TryGetProperty("run", out var run) && l.ValueKind == JsonValueKind.String && run.ValueKind == JsonValueKind.String)
                    actions.Add(new ModAction(l.GetString()!, run.GetString()!));

        string Inside(string rel) => Path.GetFullPath(Path.Combine(dir, rel));
        var patches = List("patches").Select(Inside).ToList();
        if (!has || !r.TryGetProperty("patches", out _))
            patches = Directory.Exists(dir) ? Directory.EnumerateFiles(dir, "*.patch").Order(StringComparer.OrdinalIgnoreCase).ToList() : [];
        string language = Get("language");
        string strings = Get("strings");
        if (language != "" && strings == "") strings = "strings.json";
        string? stringsPath = language != "" ? Inside(strings) : null;
        var rfu = List("rfu");

        bool laidOut = Roots.Any(root => Directory.Exists(Path.Combine(dir, root)));
        bool loose = !laidOut && stringsPath == null && Directory.Exists(dir) &&
                     Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)
                         .Any(f => !NotContent.Contains(Path.GetFileName(f)) && !f.EndsWith(".patch", StringComparison.OrdinalIgnoreCase));

        var kind = Get("kind").ToLowerInvariant() switch
        {
            "injection" => ModKind.Injection,
            "localization" or "localisation" or "language" => ModKind.Localization,
            "content" => ModKind.Content,
            _ => stringsPath != null ? ModKind.Localization : (rfu.Count > 0 || patches.Count > 0) && !laidOut ? ModKind.Injection : ModKind.Content,
        };
        // an injection's own files (a keybinds file, a screenshot) aren't content to lay over the game's
        bool hasContent = laidOut || stringsPath != null || (loose && kind != ModKind.Injection);
        string category = Get("category") is { Length: > 0 } c ? c : kind switch
        {
            ModKind.Localization => "Language",
            ModKind.Injection => patches.Count > 0 ? "Code patches" : "Other",
            _ => LooksLikeLevels(dir) ? "Levels" : "Content",
        };
        if (stringsPath != null && !File.Exists(stringsPath)) problem ??= $"its strings file ({Path.GetFileName(stringsPath)}) is missing";
        string when = Get("when").ToLowerInvariant();

        return new Mod
        {
            Dir = dir, Key = folder, FolderName = folder,
            Id = Get("id") is { Length: > 0 } id ? id : folder,
            Name = Get("name") is { Length: > 0 } n ? n : folder,
            Description = Get("description"), Author = Get("author"), Version = Get("version"),
            Kind = kind, Category = category,
            Games = List("games").Select(g => g.Trim().ToLowerInvariant()).Where(g => g != "").ToList(),
            Versions = List("versions"),
            DefaultOn = defaultOn, Rfu = rfu, Patches = patches, RequiresAny = List("requiresAny"),
            When = when is "offline" or "online" ? when : null,
            CreateDirs = List("createDirs"), Hooks = List("hooks"), Actions = actions,
            Language = language == "" ? null : language, Strings = stringsPath,
            Source = source, HasContent = hasContent, Problem = problem,
        };
    }

    static bool LooksLikeLevels(string dir) =>
        Directory.Exists(Path.Combine(dir, "levels")) || Directory.Exists(Path.Combine(dir, "cdn", "levels"));

    /// Whether a mod is for this game: its family ("reflourished", "pvz2", "china", "other", or
    /// "global" for any but China), its package, or "*"; and its version, when the mod names some.
    public static bool AppliesTo(Mod mod, GameFolder game)
    {
        if (mod.Source == ModSource.PatchFile) return true;
        bool game_ = mod.Games.Count == 0 || mod.Games.Any(g =>
            g == "*" || g == game.Family || (g == "global" && game.Family != "china") ||
            string.Equals(g, game.Info?.Package, StringComparison.OrdinalIgnoreCase));
        bool version = mod.Versions.Count == 0 || game.Info?.VersionName is not { Length: > 0 } v ||
                       mod.Versions.Any(w => v.StartsWith(w, StringComparison.OrdinalIgnoreCase));
        return game_ && version;
    }

    /// Settings made before injections were mod files (HoverCollect ... Patches) become entries in
    /// Mods; mods the game hasn't seen yet are switched on when they're on by default. Returns
    /// whether the settings changed (the caller saves them).
    public static bool Sync(GameFolder game)
    {
        var s = game.Settings;
        bool changed = false;
        var all = List(game);
        if (s.HasLegacyInjections)
        {
            void Legacy(string id, bool? on)
            {
                if (on is null) return;
                if (!s.SeenMods.Contains(id)) s.SeenMods.Add(id);
                var mod = all.FirstOrDefault(m => m.Id == id && m.Source != ModSource.PatchFile);
                if (mod != null && !s.SeenMods.Contains(mod.Key)) s.SeenMods.Add(mod.Key);
                if (on == true && mod != null && !s.Mods.Contains(mod.Key)) s.Mods.Add(mod.Key);
            }
            Legacy("hover-collect", s.HoverCollect);
            Legacy("fix-animations", s.FixAnimations);
            Legacy("keybinds", s.Keybinds);
            Legacy("key-hints", s.KeyHints);
            Legacy("controller", s.Controller);
            Legacy("glove-cursor", s.CustomCursor);
            foreach (var p in s.Patches ?? []) if (!s.Mods.Contains("patches/" + p)) s.Mods.Add("patches/" + p);
            s.HoverCollect = s.FixAnimations = s.Keybinds = s.KeyHints = s.Controller = s.CustomCursor = null;
            s.Patches = null;
            changed = true;
        }
        foreach (var mod in all.Where(m => AppliesTo(m, game) && m.Source != ModSource.PatchFile))
        {
            if (s.SeenMods.Contains(mod.Key, StringComparer.OrdinalIgnoreCase)) continue;
            s.SeenMods.Add(mod.Key);
            // a built-in already decided on before it was a file (its id is in SeenMods)
            bool decided = s.SeenMods.Contains(mod.Id) && mod.Id != mod.Key;
            if (mod.DefaultOn && !decided && !s.Mods.Contains(mod.Key, StringComparer.OrdinalIgnoreCase)) s.Mods.Add(mod.Key);
            changed = true;
        }
        return changed;
    }

    /// Whether a mod is switched on for the game.
    public static bool IsOn(GameFolder game, Mod mod) => game.Settings.Mods.Contains(mod.Key, StringComparer.OrdinalIgnoreCase);

    /// Switches a mod on or off. A mod switched on goes to the end of the load order.
    public static void Set(GameFolder game, Mod mod, bool on)
    {
        var s = game.Settings;
        s.Mods.RemoveAll(k => string.Equals(k, mod.Key, StringComparison.OrdinalIgnoreCase));
        if (on) s.Mods.Add(mod.Key);
        if (!s.SeenMods.Contains(mod.Key, StringComparer.OrdinalIgnoreCase)) s.SeenMods.Add(mod.Key);
    }

    /// The mods that take part in a run: switched on, for this game, the right side of "when", and
    /// with one of their "requiresAny" on too. In load order.
    public static List<Mod> Active(GameFolder game) => Active(game, List(game));

    /// Active, from a list of the game's mods already read (List).
    public static List<Mod> Active(GameFolder game, IReadOnlyList<Mod> all)
    {
        var s = game.Settings;
        var on = s.Mods.Select(k => all.FirstOrDefault(m => string.Equals(m.Key, k, StringComparison.OrdinalIgnoreCase)))
            .OfType<Mod>()
            .Where(m => m.Problem == null && AppliesTo(m, game) && (m.When == null || (m.When == "offline") == s.Offline))
            .ToList();
        var ids = on.Select(m => m.Id).ToHashSet(StringComparer.OrdinalIgnoreCase);
        return on.Where(m => m.RequiresAny.Count == 0 || m.RequiresAny.Any(ids.Contains)).ToList();
    }

    /// Whether any content (files, levels, strings) is switched on: the launcher then skips the
    /// update check, and doesn't ask for custom content.
    public static bool AnyContentOn(GameFolder game) => Active(game).Any(m => m.HasContent);

    /// A mod's rfu argument with its placeholders filled in.
    public static string Expand(string arg, Mod mod, GameFolder game)
    {
        string v = arg.Replace("{game}", game.Dir).Replace("{mod}", mod.Dir).Replace("{data}", game.DataDir);
        return v != arg && !v.StartsWith('-') ? Path.GetFullPath(v) : v;
    }

    /// What a folder the user picked as custom content holds.
    public enum ContentKind { Mod, StandaloneGame, Empty }

    public static ContentKind Classify(string dir)
    {
        if (Directory.EnumerateFiles(dir, "*.obb", SearchOption.TopDirectoryOnly).Any()) return ContentKind.StandaloneGame;
        return Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Any() ? ContentKind.Mod : ContentKind.Empty;
    }

    /// Copies a custom content folder into the game's mods/ and returns its folder name there.
    public static string Import(GameFolder game, string source)
    {
        source = Path.GetFullPath(source).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        string baseName = Path.GetFileName(source), name = baseName;
        for (int i = 2; Directory.Exists(Path.Combine(game.ModsDir, name)); i++) name = $"{baseName} ({i})";
        CopyTree(source, Path.Combine(game.ModsDir, name));
        return name;
    }

    /// One --overlay for rfu: a guest directory and the host directory laid over it.
    public sealed record Overlay(string Guest, string Host);

    public sealed record BuildResult(List<Overlay> Overlays, List<string> Log);

    /// The CDN folder the game uses: the newest No_Backup/CDN.* it has made, else CDN.<version>.
    public static string CdnFolderName(GameFolder game)
    {
        var noBackup = Path.Combine(game.DataDir, "external", "files", "No_Backup");
        var existing = Directory.Exists(noBackup)
            ? Directory.EnumerateDirectories(noBackup, "CDN.*").OrderByDescending(Directory.GetLastWriteTimeUtc).FirstOrDefault()
            : null;
        return existing != null ? Path.GetFileName(existing) : $"CDN.{game.Info?.VersionName ?? "1.0.0"}";
    }

    /// Builds the enabled mods, in load order, into .launcher/overlay and returns what to pass rfu.
    /// Skips the work when nothing changed since the last build.
    public static BuildResult Build(GameFolder game, IReadOnlyList<string> enabled)
    {
        var log = new List<string>();
        var all = List(game);
        var picked = new List<Mod>();
        foreach (var key in enabled)
        {
            var mod = all.FirstOrDefault(m => string.Equals(m.Key, key, StringComparison.OrdinalIgnoreCase));
            if (mod == null) log.Add($"mod {key} isn't in the mods folder any more; left out");
            else if (mod.HasContent && mod.Problem != null) log.Add($"mod {mod.Name}: {mod.Problem}; left out");
            else if (mod.HasContent) picked.Add(mod);
        }
        var mods = picked.Where(m => m.Strings == null).Select(m => m.Dir).ToList();
        var localizations = picked.Where(m => m.Strings != null).ToList();
        if (picked.Count == 0) return new BuildResult([], log);

        string package = game.Info?.Package is { Length: > 0 } p ? p : "com.ea.game.pvz2_rfl";
        string cdnName = CdnFolderName(game);
        string filesGuest = $"/sdcard/Android/data/{package}/files";
        var guest = new Dictionary<string, string> { ["cdn"] = $"{filesGuest}/No_Backup/{cdnName}", ["files"] = filesGuest };
        var baseDirs = new Dictionary<string, string>
        {
            ["cdn"] = Path.Combine(game.DataDir, "external", "files", "No_Backup", cdnName),
            ["files"] = Path.Combine(game.DataDir, "external", "files"),
        };

        // (root, path in the game's folder) -> the mods' files for it, in load order. The last one
        // wins, but LawnStrings are merged, all of them, over the game's own.
        var plan = new Dictionary<(string Root, string Rel), List<string>>(new PathKeyComparer());
        foreach (var mod in mods)
        {
            bool laidOut = Roots.Any(r => Directory.Exists(Path.Combine(mod, r)));
            foreach (var root in Roots)
            {
                string src = laidOut ? Path.Combine(mod, root) : root == "cdn" ? mod : "";
                if (src == "" || !Directory.Exists(src)) continue;
                foreach (var file in Directory.EnumerateFiles(src, "*", SearchOption.AllDirectories))
                {
                    string rel = Path.GetRelativePath(src, file);
                    bool loose = !laidOut && rel == Path.GetFileName(rel);
                    if (loose && NotContent.Contains(rel)) continue;
                    if (loose && !IsStrings(rel) && IsLevel(file)) rel = Path.Combine("levels", rel);
                    var key = (root, Target(rel, baseDirs[root]));
                    if (!plan.TryGetValue(key, out var sources)) plan[key] = sources = [];
                    if (!IsStrings(rel)) sources.Clear();
                    sources.Add(file);
                }
            }
        }

        // localizations: their strings over the game's LawnStrings for their language
        foreach (var mod in localizations)
        {
            string? gameFile = FindStrings(baseDirs["cdn"], mod.Language!);
            if (gameFile == null)
            {
                log.Add($"{mod.Name}: the game has no LawnStrings-{mod.Language} yet (play once online so it downloads them); left out");
                continue;
            }
            var key = ("cdn", Path.GetRelativePath(baseDirs["cdn"], gameFile));
            if (!plan.TryGetValue(key, out var sources)) plan[key] = sources = [];
            sources.Add(mod.Strings!);
        }

        if (plan.Count == 0) return new BuildResult([], log);
        Directory.CreateDirectory(game.WorkDir);
        string outDir = Path.Combine(game.WorkDir, "overlay");
        string stampPath = Path.Combine(game.WorkDir, "overlay.stamp");
        string stamp = Stamp(plan, guest, baseDirs);
        bool fresh = File.Exists(stampPath) && File.ReadAllText(stampPath) == stamp && Directory.Exists(outDir);
        if (!fresh)
        {
            if (Directory.Exists(outDir)) Directory.Delete(outDir, true);
            int converted = 0, copied = 0, failed = 0;
            foreach (var ((root, rel), sources) in plan)
            {
                string dest = Path.Combine(outDir, root, rel);
                string src = sources[^1];
                Directory.CreateDirectory(Path.GetDirectoryName(dest)!);
                try
                {
                    string gameFile = Path.Combine(baseDirs[root], rel);
                    int merged = IsStrings(rel) && File.Exists(gameFile) ? TryMergeStrings(gameFile, sources, dest, log) : -1;
                    if (merged >= 0)
                    {
                        log.Add($"{rel}: {merged} strings from {sources.Count} mod(s) merged over the game's");
                        converted++;
                    }
                    else if (rel.EndsWith(".rton", StringComparison.OrdinalIgnoreCase) && src.EndsWith(".json", StringComparison.OrdinalIgnoreCase))
                    {
                        File.WriteAllBytes(dest, Pvz2Conv.RtonWriter.FromJson(File.ReadAllBytes(src)));
                        converted++;
                    }
                    else
                    {
                        File.Copy(src, dest, true);
                        copied++;
                    }
                }
                catch (Exception e) when (e is JsonException or FormatException or IOException)
                {
                    log.Add($"FAIL {src}: {e.Message}");
                    failed++;
                }
            }
            log.Add($"built {picked.Count} mod(s): {copied} files copied, {converted} converted or merged{(failed > 0 ? $", {failed} failed" : "")}");
            File.WriteAllText(stampPath, failed == 0 ? stamp : "");
        }
        else
        {
            log.Add($"{picked.Count} mod(s) unchanged since the last build");
        }

        var overlays = Roots.Where(r => Directory.Exists(Path.Combine(outDir, r)))
            .Select(r => new Overlay(guest[r], Path.Combine(outDir, r))).ToList();
        return new BuildResult(overlays, log);
    }

    /// Where a mod's file goes: x.rton.json -> x.rton; x.json -> x.rton when the game's file is
    /// an RTON (keeping its spelling); anything else as it is.
    internal static string Target(string rel, string baseDir) => GameSpelling(Convert(rel, baseDir), baseDir);

    static string Convert(string rel, string baseDir)
    {
        if (rel.EndsWith(".rton.json", StringComparison.OrdinalIgnoreCase)) return rel[..^5];
        if (rel.EndsWith(".json", StringComparison.OrdinalIgnoreCase) && !rel.EndsWith(".pam.json", StringComparison.OrdinalIgnoreCase)
            && !rel.EndsWith(".popfx.json", StringComparison.OrdinalIgnoreCase))
        {
            string stem = rel[..^5];
            string? dir = Path.GetDirectoryName(Path.Combine(baseDir, stem));
            if (dir != null && Directory.Exists(dir))
            {
                string want = Path.GetFileName(stem) + ".rton";
                var match = Directory.EnumerateFiles(dir).FirstOrDefault(f => string.Equals(Path.GetFileName(f), want, StringComparison.OrdinalIgnoreCase));
                if (match != null) return Path.Combine(Path.GetDirectoryName(stem) ?? "", Path.GetFileName(match));
            }
        }
        return rel;
    }

    // Everything a build depends on: the mods' files, and the game's strings they're merged over
    // (which change whenever the game downloads its CDN content).
    // The game's own spelling of a file the mod spells differently (LawnStrings-en-us.rton for the
    // game's LawnStrings-en-US.rton), so the mod's file replaces it on any file system.
    static string GameSpelling(string rel, string baseDir)
    {
        string? dir = Path.GetDirectoryName(Path.Combine(baseDir, rel));
        if (dir == null || !Directory.Exists(dir)) return rel;
        string name = Path.GetFileName(rel);
        var match = Directory.EnumerateFiles(dir).Select(Path.GetFileName).FirstOrDefault(f => string.Equals(f, name, StringComparison.OrdinalIgnoreCase));
        return match == null ? rel : Path.Combine(Path.GetDirectoryName(rel) ?? "", match);
    }

    static string Stamp(Dictionary<(string Root, string Rel), List<string>> plan, Dictionary<string, string> guest, Dictionary<string, string> baseDirs)
    {
        var sb = new StringBuilder();
        void File(string path)
        {
            var info = new FileInfo(path);
            sb.Append(path).Append(' ').Append(info.Exists ? info.Length : -1).Append(' ').Append(info.Exists ? info.LastWriteTimeUtc.Ticks : 0).Append('\n');
        }
        foreach (var g in guest) sb.Append(g.Key).Append('=').Append(g.Value).Append('\n');
        foreach (var ((root, rel), sources) in plan.OrderBy(p => p.Key.Root).ThenBy(p => p.Key.Rel, StringComparer.Ordinal))
        {
            sb.Append(root).Append('/').Append(rel).Append('\n');
            sources.ForEach(File);
            if (IsStrings(rel)) File(Path.Combine(baseDirs[root], rel));
        }
        return System.Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(sb.ToString())));
    }

    /// How much of the game's current strings a localization covers. Null when the game hasn't
    /// downloaded its strings for that language yet, or a file can't be read.
    public sealed record StringCoverage(int Covered, int Total, IReadOnlyList<(string Key, string Text)> Missing, int Unused);

    public static StringCoverage? Coverage(GameFolder game, Mod mod)
    {
        if (mod.Strings == null || mod.Language == null || !File.Exists(mod.Strings)) return null;
        if (FindStrings(Path.Combine(game.DataDir, "external", "files", "No_Backup", CdnFolderName(game)), mod.Language) is not { } gameFile) return null;
        try
        {
            var (gameDoc, gameStrings) = ReadStrings(gameFile);
            gameDoc.Dispose();
            var (modDoc, modStrings) = ReadStrings(mod.Strings, allowFlat: true);
            modDoc.Dispose();
            var have = modStrings.Select(p => p.Key).ToHashSet(StringComparer.Ordinal);
            var gameKeys = gameStrings.Select(p => p.Key).ToHashSet(StringComparer.Ordinal);
            var missing = gameStrings.Where(p => !have.Contains(p.Key)).ToList();
            return new StringCoverage(gameStrings.Count - missing.Count, gameStrings.Count, missing, have.Count(k => !gameKeys.Contains(k)));
        }
        catch (Exception e) when (e is FormatException or JsonException or IOException) { return null; }
    }

    /// Writes the strings a localization doesn't have yet, with the game's own text, as
    /// missing-strings.json in its folder: copy lines from it into the strings file to translate them.
    public static string? WriteMissing(GameFolder game, Mod mod)
    {
        if (Coverage(game, mod) is not { } c) return null;
        string path = Path.Combine(mod.Dir, "missing-strings.json");
        using var fs = File.Create(path);
        using var w = new Utf8JsonWriter(fs, new JsonWriterOptions { Indented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping });
        w.WriteStartObject();
        foreach (var (k, v) in c.Missing) w.WriteString(k, v);
        w.WriteEndObject();
        return path;
    }

    /// The game's LawnStrings-<language> file in its CDN folder (at the top, or one folder down).
    static string? FindStrings(string cdnDir, string language)
    {
        if (!Directory.Exists(cdnDir)) return null;
        bool Match(string f) => Path.GetFileNameWithoutExtension(f).Equals("LawnStrings-" + language, StringComparison.OrdinalIgnoreCase) &&
                                Path.GetExtension(f).ToLowerInvariant() is ".rton" or ".json";
        return Directory.EnumerateFiles(cdnDir).FirstOrDefault(Match) ??
               Directory.EnumerateDirectories(cdnDir).SelectMany(Directory.EnumerateFiles).FirstOrDefault(Match);
    }

    static bool IsStrings(string rel) => Path.GetFileName(rel).StartsWith("LawnStrings", StringComparison.OrdinalIgnoreCase);

    static readonly byte[] LevelMarker = Encoding.ASCII.GetBytes("LevelDefinition");

    static bool IsLevel(string file)
    {
        string ext = Path.GetExtension(file).ToLowerInvariant();
        if (ext is not (".json" or ".rton")) return false;
        try { return System.IO.File.ReadAllBytes(file).AsSpan().IndexOf(LevelMarker) >= 0; }
        catch (IOException) { return false; }
    }

    // MergeStrings, or -1 when either side isn't a strings list (the mod's file then replaces the game's).
    static int TryMergeStrings(string gameFile, IReadOnlyList<string> mods, string dest, List<string> log)
    {
        try { return MergeStrings(gameFile, mods, dest); }
        catch (FormatException e)
        {
            log.Add($"{Path.GetFileName(gameFile)}: not merged ({e.Message}); the mod's file replaces it");
            return -1;
        }
    }

    /// The game's LawnStrings with each mod's strings put over them (later mods win), written to
    /// `dest` in the game's format (RTON or JSON, and its LocStringValues layout: the flat
    /// [key, value, ...] list Reflourished uses, or a key: value object). Returns how many keys the
    /// mods set.
    internal static int MergeStrings(string gameFile, IReadOnlyList<string> mods, string dest)
    {
        var (gameDoc, gameStrings) = ReadStrings(gameFile);
        var merged = new Dictionary<string, string>(StringComparer.Ordinal);
        var order = new List<string>();
        void Put(string k, string v)
        {
            if (!merged.ContainsKey(k)) order.Add(k);
            merged[k] = v;
        }
        foreach (var (k, v) in gameStrings) Put(k, v);
        var touched = new HashSet<string>(StringComparer.Ordinal);
        foreach (var mod in mods)
            foreach (var (k, v) in ReadStrings(mod, allowFlat: true).Strings)
            {
                Put(k, v);
                touched.Add(k);
            }

        // write the game's document back with the merged list in place of its own
        using var ms = new MemoryStream();
        using (var w = new Utf8JsonWriter(ms, new JsonWriterOptions { Indented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }))
            WriteReplacing(gameDoc.RootElement, w, order, merged);
        gameDoc.Dispose();
        byte[] json = ms.ToArray();
        System.IO.File.WriteAllBytes(dest, dest.EndsWith(".rton", StringComparison.OrdinalIgnoreCase) ? Pvz2Conv.RtonWriter.FromJson(json) : json);
        return touched.Count;
    }

    static (JsonDocument Doc, List<(string Key, string Value)> Strings) ReadStrings(string file, bool allowFlat = false)
    {
        byte[] data = System.IO.File.ReadAllBytes(file);
        if (Pvz2Conv.Rton.LooksLike(data))
        {
            using var ms = new MemoryStream();
            using (var w = new Utf8JsonWriter(ms)) Pvz2Conv.Rton.ToJson(data, w, []);
            data = ms.ToArray();
        }
        var doc = JsonDocument.Parse(data, new JsonDocumentOptions { AllowTrailingCommas = true, CommentHandling = JsonCommentHandling.Skip });
        var list = new List<(string, string)>();
        if (FindLocStrings(doc.RootElement) is { } values)
        {
            if (values.ValueKind == JsonValueKind.Array)
            {
                var items = values.EnumerateArray().Select(e => e.ValueKind == JsonValueKind.String ? e.GetString()! : e.GetRawText()).ToList();
                for (int i = 0; i + 1 < items.Count; i += 2) list.Add((items[i], items[i + 1]));
            }
            else if (values.ValueKind == JsonValueKind.Object)
            {
                foreach (var p in values.EnumerateObject()) list.Add((p.Name, p.Value.ValueKind == JsonValueKind.String ? p.Value.GetString()! : p.Value.GetRawText()));
            }
        }
        else if (allowFlat && doc.RootElement.ValueKind == JsonValueKind.Object &&
                 doc.RootElement.EnumerateObject().All(p => p.Value.ValueKind == JsonValueKind.String))
        {
            // a localization's plain { "KEY": "text" }
            foreach (var p in doc.RootElement.EnumerateObject()) list.Add((p.Name, p.Value.GetString()!));
        }
        else
        {
            doc.Dispose();
            throw new FormatException($"{Path.GetFileName(file)} has no LocStringValues");
        }
        return (doc, list);
    }

    static JsonElement? FindLocStrings(JsonElement e)
    {
        if (e.ValueKind == JsonValueKind.Object)
        {
            foreach (var p in e.EnumerateObject())
            {
                if (p.Name == "LocStringValues") return p.Value;
                if (FindLocStrings(p.Value) is { } v) return v;
            }
        }
        else if (e.ValueKind == JsonValueKind.Array)
        {
            foreach (var item in e.EnumerateArray())
                if (FindLocStrings(item) is { } v) return v;
        }
        return null;
    }

    static void WriteReplacing(JsonElement e, Utf8JsonWriter w, List<string> order, Dictionary<string, string> merged)
    {
        switch (e.ValueKind)
        {
            case JsonValueKind.Object:
                w.WriteStartObject();
                foreach (var p in e.EnumerateObject())
                {
                    w.WritePropertyName(p.Name);
                    if (p.Name == "LocStringValues" && p.Value.ValueKind == JsonValueKind.Array)
                    {
                        w.WriteStartArray();
                        foreach (var k in order) { w.WriteStringValue(k); w.WriteStringValue(merged[k]); }
                        w.WriteEndArray();
                    }
                    else if (p.Name == "LocStringValues" && p.Value.ValueKind == JsonValueKind.Object)
                    {
                        w.WriteStartObject();
                        foreach (var k in order) w.WriteString(k, merged[k]);
                        w.WriteEndObject();
                    }
                    else WriteReplacing(p.Value, w, order, merged);
                }
                w.WriteEndObject();
                break;
            case JsonValueKind.Array:
                w.WriteStartArray();
                foreach (var item in e.EnumerateArray()) WriteReplacing(item, w, order, merged);
                w.WriteEndArray();
                break;
            default:
                e.WriteTo(w);
                break;
        }
    }

    static void CopyTree(string from, string to)
    {
        Directory.CreateDirectory(to);
        foreach (var dir in Directory.EnumerateDirectories(from, "*", SearchOption.AllDirectories))
            Directory.CreateDirectory(Path.Combine(to, Path.GetRelativePath(from, dir)));
        foreach (var file in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
            File.Copy(file, Path.Combine(to, Path.GetRelativePath(from, file)), true);
    }

    // The game's files live on Android's case-sensitive storage, but two mods that spell a path
    // differently must still replace each other on Windows.
    sealed class PathKeyComparer : IEqualityComparer<(string Root, string Rel)>
    {
        public bool Equals((string Root, string Rel) a, (string Root, string Rel) b) =>
            a.Root == b.Root && string.Equals(a.Rel.Replace('\\', '/'), b.Rel.Replace('\\', '/'), StringComparison.OrdinalIgnoreCase);
        public int GetHashCode((string Root, string Rel) k) => HashCode.Combine(k.Root, k.Rel.Replace('\\', '/').ToUpperInvariant());
    }
}
