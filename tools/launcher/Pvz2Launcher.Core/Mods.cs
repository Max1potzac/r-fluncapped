using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

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
///                     "language": "en-US", "strings": "strings.json"   (localization),
///                     "fonts":    { "<the game's font>": "<a font in the mod>" } }
///   cdn/...         content over the game's CDN folder (No_Backup/CDN.<version>: levels, strings,
///                   liveconfig ...), the content the game downloads and RFLib reads
///   files/...       content over the game's external files folder (what Android calls
///                   /storage/emulated/0/Android/data/<package>/files)
///   fonts/...       fonts in place of the game's, by its font's file name (BrianneTod.ttf): a
///                   localization's fonts with the letters the game's lack. Each takes the name of
///                   the font it replaces, so the game's typefaces (FONTS.RTON) use it.
///   obb/...         files in place of the OBB's own, at their paths there (PACKAGES/FONTS.rton,
///                   or .json converted): ordinary files the OBB has, not textures (ObbPatch).
///                   Property sheets merge into the game's (SheetMerge); a new plant or zombie
///                   that the game or another mod has already is a conflict (ModConflicts)
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
/// Fonts and obb/ files go into the OBB as rfu reads it, through a patch (ObbPatch, --obb-patch),
/// so the OBB isn't changed either.
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
        /// Its fonts: the game's font file name ("BrianneTod.ttf") -> the mod's font (full path).
        public IReadOnlyDictionary<string, string> Fonts { get; init; } = new Dictionary<string, string>();
        public ModSource Source { get; init; }
        /// The mod has files to lay over the game's (cdn/, files/, loose content, strings, fonts
        /// or obb/ files).
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
    // what goes into the OBB rather than over a folder
    static readonly string[] ObbRoots = ["fonts", "obb", "groups"];
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

        // fonts: mod.json's, then the fonts/ folder's that mod.json doesn't name, by the game's
        // font file name
        var fonts = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        if (has && r.TryGetProperty("fonts", out var fv) && fv.ValueKind == JsonValueKind.Object)
            foreach (var f in fv.EnumerateObject().Where(f => f.Value.ValueKind == JsonValueKind.String))
            {
                string gameFont = Path.GetFileNameWithoutExtension(f.Name.Trim()) + ".ttf";
                string mine = Inside(f.Value.GetString()!);
                fonts[gameFont] = mine;
                if (!File.Exists(mine)) problem ??= $"its font {f.Value.GetString()} is missing";
            }
        string fontsDir = Path.Combine(dir, "fonts");
        if (Directory.Exists(fontsDir))
            foreach (var f in Directory.EnumerateFiles(fontsDir).Where(IsFont))
                if (!fonts.Values.Contains(Path.GetFullPath(f), StringComparer.OrdinalIgnoreCase))
                    fonts.TryAdd(Path.GetFileNameWithoutExtension(f) + ".ttf", f);

        bool laidOut = Roots.Concat(ObbRoots).Any(root => Directory.Exists(Path.Combine(dir, root))) || fonts.Count > 0;
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
            Language = language == "" ? null : language, Strings = stringsPath, Fonts = fonts,
            Source = source, HasContent = hasContent, Problem = problem,
        };
    }

    static bool IsFont(string path) =>
        path.EndsWith(".ttf", StringComparison.OrdinalIgnoreCase) || path.EndsWith(".otf", StringComparison.OrdinalIgnoreCase);

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

    /// What to pass rfu: the overlays, and the OBB patch (--obb-patch) when mods have fonts or
    /// obb/ files.
    public sealed record BuildResult(List<Overlay> Overlays, List<string> Log, string? ObbPatch = null);

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
        // a mod whose new plant (or zombie ...) the game has, or the later of two adding the same one
        for (var conflicts = ModConflicts.Find(game, picked); conflicts.Count > 0; conflicts = ModConflicts.Find(game, picked))
        {
            picked.Remove(conflicts[0].Mod);
            log.Add($"mod {conflicts[0].Mod.Name} left out: {conflicts[0].Describe()}");
        }
        var mods = picked.Where(m => m.Strings == null).Select(m => m.Dir).ToList();
        var localizations = picked.Where(m => m.Strings != null).ToList();
        string? obbPatch = BuildObbPatch(game, picked, log);
        if (picked.Count == 0) return new BuildResult([], log, obbPatch);

        string package = game.Info?.Package is { Length: > 0 } p ? p : "com.ea.game.pvz2_rfl";
        string cdnName = CdnFolderName(game);
        string filesGuest = $"/storage/emulated/0/Android/data/{package}/files";  // where the game is told its files are (rfu takes /sdcard too)
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
            bool laidOut = Roots.Concat(ObbRoots).Any(r => Directory.Exists(Path.Combine(mod, r)));
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

        // a mod's strings for the OBB's LAWNSTRINGS-<language> (a new plant's name...) go over the
        // CDN's LawnStrings-<language> too, which the game reads in their place once it has them
        foreach (var mod in mods)
        {
            string packages = Path.Combine(mod, "obb", "PACKAGES");
            if (!Directory.Exists(packages)) continue;
            foreach (var file in Directory.EnumerateFiles(packages).Order(StringComparer.OrdinalIgnoreCase))
            {
                string stem = Path.GetFileNameWithoutExtension(file);
                if (!stem.StartsWith("LawnStrings-", StringComparison.OrdinalIgnoreCase) || !file.EndsWith(".json", StringComparison.OrdinalIgnoreCase)) continue;
                if (FindStrings(baseDirs["cdn"], stem["LawnStrings-".Length..]) is not { } gameFile)
                {
                    log.Add($"{Path.GetFileName(mod)}: no {stem} in {baseDirs["cdn"]}, so its {Path.GetFileName(file)} is in the OBB only " +
                            "(the game reads the online copy in its place once it has downloaded it)");
                    continue;
                }
                var key = ("cdn", Path.GetRelativePath(baseDirs["cdn"], gameFile));
                if (!plan.TryGetValue(key, out var sources)) plan[key] = sources = [];
                sources.Add(file);
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

        if (plan.Count == 0) return new BuildResult([], log, obbPatch);
        Directory.CreateDirectory(game.WorkDir);
        string outDir = Path.Combine(game.WorkDir, "overlay");
        string stampPath = Path.Combine(game.WorkDir, "overlay.stamp");
        string stamp = Stamp(plan, guest, baseDirs, Path.Combine(baseDirs["cdn"], CdnArchive));
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
            if (plan.Keys.Any(k => k.Root == "cdn") && !PutIntoArchive(Path.Combine(baseDirs["cdn"], CdnArchive), Path.Combine(outDir, "cdn"), log)) failed++;
            log.Add($"built {picked.Count} mod(s): {copied} files copied, {converted} converted or merged{(failed > 0 ? $", {failed} failed" : "")}");
            File.WriteAllText(stampPath, failed == 0 ? stamp : "");
        }
        else
        {
            log.Add($"{picked.Count} mod(s) unchanged since the last build");
        }

        var overlays = Roots.Where(r => Directory.Exists(Path.Combine(outDir, r)))
            .Select(r => new Overlay(guest[r], Path.Combine(outDir, r))).ToList();
        return new BuildResult(overlays, log, obbPatch);
    }

    /// The mods' fonts and obb/ files put into the game's OBB as a patch for rfu, in
    /// .launcher/obb-patch.bin, or null when there are none (or it can't be made). Made again only
    /// when the OBB or a file changed. Property sheets are merged (SheetMerge): the game's, then each
    /// mod's in load order.
    static string? BuildObbPatch(GameFolder game, IReadOnlyList<Mod> mods, List<string> log)
    {
        // fonts: the game's font -> the mod's file, the last mod in load order winning
        var fonts = new Dictionary<string, string>(StringComparer.Ordinal);
        // obb/ files: (their OBB path, the mod's file), in load order, and each one's mod folder
        var obbFiles = new List<(string Key, string File)>();
        var modDirs = new Dictionary<string, string>(StringComparer.Ordinal);
        // pictures for the game's atlases ("#png" in a mod's manifest), anywhere in a mod but obb/ and groups/
        var pictureFiles = new List<string>();
        // groups/<name>/: new resource groups, by name (the last mod in load order winning)
        var newGroups = new Dictionary<string, (Mod Mod, List<(string Key, string File)> Files)>(StringComparer.OrdinalIgnoreCase);
        foreach (var mod in mods)
        {
            foreach (var (gameFont, file) in mod.Fonts) fonts[ObbPatch.Key("PRIMEFONTS/" + gameFont)] = file;
            string obbDir = Path.Combine(mod.Dir, "obb");
            if (Directory.Exists(obbDir))
                foreach (var file in Directory.EnumerateFiles(obbDir, "*", SearchOption.AllDirectories).Order(StringComparer.OrdinalIgnoreCase))
                {
                    obbFiles.Add((ObbPatch.Key(Path.GetRelativePath(obbDir, file)), file));
                    modDirs[file] = mod.Dir;
                }
            if (Directory.Exists(mod.Dir))
                pictureFiles.AddRange(Directory.EnumerateFiles(mod.Dir, "*.png", new EnumerationOptions { RecurseSubdirectories = true, MatchCasing = MatchCasing.CaseInsensitive })
                    .Where(f => Path.GetRelativePath(mod.Dir, f).Split(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar)[0].ToLowerInvariant() is not ("obb" or "groups"))
                    .Order(StringComparer.OrdinalIgnoreCase));
            foreach (var (name, files) in GroupFolders(mod))
            {
                if (newGroups.TryGetValue(name, out var had)) log.Add($"group {name}: {mod.Name}'s, not {had.Mod.Name}'s");
                newGroups[name] = (mod, files);
            }
        }
        string patchPath = Path.Combine(game.WorkDir, "obb-patch.bin"), stampPath = Path.Combine(game.WorkDir, "obb-patch.stamp");
        int count = fonts.Count + obbFiles.Count + newGroups.Values.Sum(g => g.Files.Count);
        if (count == 0) return null;
        if (game.Obb == null || !File.Exists(game.Obb))
        {
            log.Add("fonts and obb/ files go into the OBB, and this game has none (PvZ2 China's data is in its APK); left out");
            return null;
        }
        var stamp = new StringBuilder($"obb-patch v3\n{game.Obb}|{new FileInfo(game.Obb).Length}|{File.GetLastWriteTimeUtc(game.Obb).Ticks}\n");
        foreach (var (key, file, font) in fonts.OrderBy(p => p.Key, StringComparer.Ordinal).Select(p => (p.Key, p.Value, true)).Concat(obbFiles.Select(f => (f.Key, f.File, false))))
            stamp.Append($"{key}|{file}|{font}|{(File.Exists(file) ? $"{new FileInfo(file).Length}|{File.GetLastWriteTimeUtc(file).Ticks}" : "-")}\n");
        foreach (var (name, (_, groupFiles)) in newGroups.OrderBy(g => g.Key, StringComparer.OrdinalIgnoreCase))
            foreach (var (key, file) in groupFiles)
                stamp.Append($"group {name}|{key}|{file}|{new FileInfo(file).Length}|{File.GetLastWriteTimeUtc(file).Ticks}\n");
        foreach (var file in pictureFiles)
            stamp.Append($"picture {file}|{new FileInfo(file).Length}|{File.GetLastWriteTimeUtc(file).Ticks}\n");
        if (File.Exists(patchPath) && File.Exists(stampPath) && File.ReadAllText(stampPath) == stamp.ToString())
        {
            log.Add($"OBB patch: {count} file(s), unchanged since the last build");
            return patchPath;
        }
        try
        {
            var index = ObbPatch.Read(game.Obb);
            var files = new Dictionary<string, byte[]>(StringComparer.Ordinal);
            var pictures = new AtlasPictures(game.Obb, index);

            // fonts, with the names of the game's font each replaces
            var originals = ObbPatch.ReadFiles(game.Obb, index, fonts.Keys);
            foreach (var (key, file) in fonts)
            {
                if (!File.Exists(file)) { log.Add($"{file} is missing; left out"); continue; }
                if (!originals.TryGetValue(key, out var original))
                {
                    var known = index.Files.Keys.Where(k => k.StartsWith("PRIMEFONTS\\", StringComparison.Ordinal)).Select(k => Path.GetFileName(k.Replace('\\', '/')));
                    log.Add($"{Path.GetFileName(file)}: the game has no font {key[11..]} (its fonts: {string.Join(", ", known)}); left out");
                    continue;
                }
                try { files[key] = Fonts.WithNames(File.ReadAllBytes(file), original); }
                catch (FormatException e) { log.Add($"{Path.GetFileName(file)}: {e.Message}; left out"); }
            }

            // obb/ files, by where they go: x.rton.json -> x.rton, x.json -> x.rton where the OBB's is RTON
            string Target(string key)
            {
                if (key.EndsWith(".RTON.JSON", StringComparison.Ordinal)) return key[..^5];
                if (key.EndsWith(".JSON", StringComparison.Ordinal) && index.Files.ContainsKey(key[..^5] + ".RTON")) return key[..^5] + ".RTON";
                return key;
            }
            var byTarget = obbFiles.GroupBy(f => Target(f.Key)).ToList();
            var composites = new List<JsonObject>();
            var gameSheets = ObbPatch.ReadFiles(game.Obb, index,
                byTarget.Select(g => g.Key).Where(t => t.EndsWith(".RTON", StringComparison.Ordinal) || t.EndsWith(".JSON", StringComparison.Ordinal)));
            foreach (var group in byTarget)
            {
                string target = group.Key;
                bool rton = target.EndsWith(".RTON", StringComparison.Ordinal);
                JsonObject? sheet = null;  // the merged sheet so far, or null when the last file isn't one
                JsonObject? manifest = null;  // the same for the resource manifest
                byte[]? whole = null;      // the last file that isn't a sheet, as it goes in
                int merged = 0, added = 0, changed = 0, removed = 0;
                foreach (var (key, file) in group)
                {
                    if (!File.Exists(file)) { log.Add($"{file} is missing; left out"); continue; }
                    byte[] data = File.ReadAllBytes(file);
                    JsonObject? mine = null;
                    if (rton || target.EndsWith(".JSON", StringComparison.Ordinal))
                    {
                        try { mine = SheetMerge.Parse(data) as JsonObject; }
                        catch (Exception e) when (e is JsonException or FormatException) { if (key != target) throw new FormatException($"{file}: {e.Message}"); }
                    }
                    if (mine != null && SheetMerge.IsManifest(mine) && !SheetMerge.IsSheet(mine))
                    {
                        // the resource manifest: the game's (or the merged one so far), the mod's groups added
                        manifest ??= whole == null && gameSheets.TryGetValue(target, out var own) ? SheetMerge.Parse(own) as JsonObject : null;
                        if (manifest == null || !SheetMerge.IsManifest(manifest)) { manifest = mine; whole = null; continue; }
                        if (modDirs.TryGetValue(file, out var modDir)) pictures.Place(manifest, mine, modDir, log);
                        var (mc, comps) = SheetMerge.MergeManifest(manifest, mine);
                        composites.AddRange(comps);
                        whole = null;
                        log.Add($"{target}: {Path.GetFileName(file)} merged in ({mc.Added} group(s) added, {mc.Changed} changed, {mc.Removed} removed)");
                        continue;
                    }
                    if (mine == null || !SheetMerge.IsSheet(mine))
                    {
                        manifest = null;
                        sheet = null;
                        whole = key != target && rton ? Pvz2Conv.RtonWriter.FromJson(data) : data;
                        continue;
                    }
                    if (SheetMerge.TakeReplace(mine)) { sheet = mine; whole = null; continue; }
                    if (sheet == null)
                    {
                        // the start: the game's own sheet, unless an earlier mod's file replaced it whole
                        JsonObject? start = null;
                        if (whole == null && gameSheets.TryGetValue(target, out var own))
                            try { start = SheetMerge.Parse(own) as JsonObject; } catch (Exception e) when (e is JsonException or FormatException) { }
                        if (start == null || !SheetMerge.IsSheet(start))
                        {
                            sheet = mine;
                            whole = null;
                            continue;
                        }
                        sheet = start;
                        whole = null;
                    }
                    var c = SheetMerge.Merge(sheet, mine);
                    merged++;
                    added += c.Added; changed += c.Changed; removed += c.Removed;
                }
                if (manifest != null) files[target] = rton ? SheetMerge.ToRton(manifest) : Encoding.UTF8.GetBytes(manifest.ToJsonString());
                else if (sheet != null)
                {
                    files[target] = rton ? SheetMerge.ToRton(sheet) : Encoding.UTF8.GetBytes(sheet.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
                    if (merged > 0) log.Add($"{target}: {merged} mod file(s) merged into the game's ({added} added, {changed} changed, {removed} removed)");
                }
                else if (whole != null) files[target] = whole;
            }

            // new groups: their files, PNGs as textures (RGBA8888)
            var groups = new List<ObbPatch.NewGroup>();
            foreach (var (name, (mod, groupFiles)) in newGroups.OrderBy(g => g.Key, StringComparer.OrdinalIgnoreCase))
            {
                var plain = new List<(string, byte[])>();
                var textures = new List<ObbPatch.NewTexture>();
                foreach (var (key, file) in groupFiles)
                {
                    if (!key.EndsWith(".PNG", StringComparison.Ordinal)) { plain.Add((key, File.ReadAllBytes(file))); continue; }
                    try
                    {
                        var (rgba, w, h) = Pvz2Conv.Png.Decode(File.ReadAllBytes(file));
                        textures.Add(new ObbPatch.NewTexture(key[..^4] + ".PTX", rgba, w, h, 0));
                    }
                    catch (FormatException e) { log.Add($"{file}: {e.Message}; left out"); }
                }
                groups.Add(new ObbPatch.NewGroup(name, plain, textures));
            }
            // composite groups the mods' manifests add (or add groups to), with the RSB's resolutions
            // (1536, 768, else any)
            var newComposites = composites
                .Where(c => c["subgroups"] is JsonArray)
                .Select(c => new ObbPatch.NewComposite(c["id"]!.ToString(), ((JsonArray)c["subgroups"]!).OfType<JsonObject>()
                    .Where(sg => sg["id"] != null)
                    .Select(sg => (sg["id"]!.ToString(), int.TryParse(sg["res"]?.ToString(), out int res) && res is 1536 or 768 ? res : 0)).ToList()))
                .ToList();

            var atlases = pictures.Changed();
            foreach (var path in atlases.Keys) log.Add($"{path}: mods' pictures put into it");
            var patch = ObbPatch.Build(game.Obb, index, files, log, groups, newComposites, atlases);
            Directory.CreateDirectory(game.WorkDir);
            if (patch.Length == 0)
            {
                File.Delete(patchPath);
                File.WriteAllText(stampPath, stamp.ToString());
                return null;
            }
            File.WriteAllBytes(patchPath, patch);
            File.WriteAllText(stampPath, stamp.ToString());
            log.Add($"OBB patch: {files.Count} file(s) put into the OBB as the game reads it");
            return patchPath;
        }
        catch (Exception e) when (e is FormatException or IOException or JsonException or InvalidDataException or UnauthorizedAccessException)
        {
            log.Add($"FAIL the OBB patch: {e.Message}; fonts and obb/ files left out");
            if (File.Exists(stampPath)) File.Delete(stampPath);
            return null;
        }
    }

    /// A mod's new resource groups: each folder of groups/, with its files by OBB path.
    public static List<(string Name, List<(string Key, string File)> Files)> GroupFolders(Mod mod)
    {
        string dir = Path.Combine(mod.Dir, "groups");
        if (!Directory.Exists(dir)) return [];
        return Directory.EnumerateDirectories(dir).Order(StringComparer.OrdinalIgnoreCase)
            .Select(g => (Path.GetFileName(g), Directory.EnumerateFiles(g, "*", SearchOption.AllDirectories).Order(StringComparer.OrdinalIgnoreCase)
                .Select(f => (ObbPatch.Key(Path.GetRelativePath(g, f)), f)).ToList()))
            .Where(g => g.Item2.Count > 0).ToList();
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

    // The archive the game downloads its CDN content as, in the CDN folder. It reads the files it
    // applies (strings, schedules ...) out of it, not from the copies it unpacks next to it.
    const string CdnArchive = "tar.full";

    /// The CDN archive with the overlay's files in place of its own (by path, ignoring case), as
    /// the overlay's tar.full, which the game then reads in place of its download. False when the
    /// archive can't be read; true, and nothing written, when there's no archive or nothing in it to swap.
    static bool PutIntoArchive(string archive, string overlayCdn, List<string> log)
    {
        if (!System.IO.File.Exists(archive) || !Directory.Exists(overlayCdn)) return true;
        static string Norm(string p) => p.Replace('\\', '/').TrimStart('.', '/').ToLowerInvariant();
        var ours = Directory.EnumerateFiles(overlayCdn, "*", SearchOption.AllDirectories)
            .ToDictionary(f => Norm(Path.GetRelativePath(overlayCdn, f)), f => f);
        ours.Remove(CdnArchive);
        if (ours.Count == 0) return true;
        string dest = Path.Combine(overlayCdn, CdnArchive), temp = dest + ".part";
        try
        {
            using (var input = System.IO.File.OpenRead(archive))
            {
                if (input.ReadByte() == 0x1F) { log.Add($"{archive} is compressed; the mods' strings and files are left out of it"); return true; }
                input.Position = 0;
                List<string> swapped;
                using (var output = System.IO.File.Create(temp))
                    swapped = Tar.Rewrite(input, output, name => ours.TryGetValue(Norm(name), out var f) ? System.IO.File.ReadAllBytes(f) : null);
                if (swapped.Count == 0)
                {
                    System.IO.File.Delete(temp);
                    return true;
                }
                log.Add($"{CdnArchive}: {string.Join(", ", swapped)} put into the game's downloaded content");
            }
            System.IO.File.Move(temp, dest, true);
            return true;
        }
        catch (Exception e) when (e is IOException or InvalidDataException or UnauthorizedAccessException)
        {
            log.Add($"FAIL {archive}: {e.Message}");
            try { System.IO.File.Delete(temp); } catch (IOException) { }
            return false;
        }
    }

    static string Stamp(Dictionary<(string Root, string Rel), List<string>> plan, Dictionary<string, string> guest, Dictionary<string, string> baseDirs, string archive)
    {
        var sb = new StringBuilder();
        void File(string path)
        {
            var info = new FileInfo(path);
            sb.Append(path).Append(' ').Append(info.Exists ? info.Length : -1).Append(' ').Append(info.Exists ? info.LastWriteTimeUtc.Ticks : 0).Append('\n');
        }
        File(archive);
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

    /// The game's LawnStrings-<language> file in its CDN folder, the shallowest one there.
    static string? FindStrings(string cdnDir, string language)
    {
        if (!Directory.Exists(cdnDir)) return null;
        bool Match(string f) => Path.GetFileNameWithoutExtension(f).Equals("LawnStrings-" + language, StringComparison.OrdinalIgnoreCase) &&
                                Path.GetExtension(f).ToLowerInvariant() is ".rton" or ".json";
        return Directory.EnumerateFiles(cdnDir, "*", SearchOption.AllDirectories).Where(Match)
            .OrderBy(f => Path.GetRelativePath(cdnDir, f).Count(c => c == Path.DirectorySeparatorChar)).ThenBy(f => f, StringComparer.Ordinal)
            .FirstOrDefault();
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
                if (p.Name is "LocStringValues" or "LocStringValues" + SheetMerge.AddSuffix) return p.Value;
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
