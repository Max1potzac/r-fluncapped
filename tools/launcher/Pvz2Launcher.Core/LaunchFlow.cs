using System.Diagnostics;
using System.Text.Json;

namespace Pvz2Launcher;

/// The questions the launch asks the person (the window answers them).
public interface ILaunchUi
{
    void Log(string line);

    /// "Load custom content?" Returns the folder picked, or null to play without.
    string? AskForCustomContent(GameFolder game);

    /// A new version is out. Returns what to do.
    UpdateChoice AskAboutUpdate(GameFolder game, UpdateCheck.Result result);

    /// The new version's APK and OBB, or null when the person backs out. The OBB is null when the
    /// APK has none (PvZ2 China) or can download its own (ObbUpdate.ConfigUrlInApk).
    (string Apk, string? Obb)? AskForNewVersion(GameFolder game);

    /// The OBB, when downloading it didn't work. Null to give up.
    string? AskForObb(GameFolder game);

    void ShowProblem(string message);

    /// A yes/no question (a big download, say).
    bool Confirm(string message);

    /// A warning that asks whether to go on (the CDN download failed): true to play anyway.
    bool ContinueAnyway(string heading, string message);
}

public enum UpdateChoice { GetNewVersion, PlayAnyway, Cancel }

/// Play, step by step: the game's folder, custom content, the update checks, the mods, then rfu.
/// `downloads` is for the OBB update: an HttpClient with no time limit, since that can be big.
public sealed class LaunchFlow(string launcherDir, HttpClient http, ILaunchUi ui, HttpClient? downloads = null)
{
    /// rfu's exit code when the game asked to be started again (after importing a save, or patching
    /// its OBB); RunAsync(game, restart: true) does that, without asking anything.
    public const int RestartExitCode = 3;

    public async Task<Process?> RunAsync(GameFolder game, bool restart = false)
    {
        if (!await PrepareAsync(game)) return null;
        if (game.Problem() is { } problem) { ui.ShowProblem(problem); return null; }

        var s = game.Settings;
        BuiltinMods.Seed(game.SharedModsDir);
        var known = Mods.List(game);
        s.Mods.RemoveAll(m => !known.Any(k => string.Equals(k.Key, m, StringComparison.OrdinalIgnoreCase)));
        Mods.Sync(game);

        // Custom content: the mods switched on, or a folder picked now.
        if (!restart && !Mods.AnyContentOn(game) && s.AskForCustomContent && ui.AskForCustomContent(game) is { } folder)
        {
            switch (Mods.Classify(folder))
            {
                case Mods.ContentKind.StandaloneGame:
                    ui.ShowProblem("That folder has an .obb: it's a game of its own (like AltverZ), not content for this one. " +
                                   "Add it with \"Add game\" instead.");
                    return null;
                case Mods.ContentKind.Empty:
                    ui.ShowProblem("That folder is empty.");
                    return null;
                default:
                    string name = Mods.Import(game, folder);
                    Mods.Set(game, Mods.Find(game, name)!, true);
                    ui.Log($"added custom content \"{name}\" to {game.ModsDir}");
                    break;
            }
        }
        game.SaveSettings();

        // The CDN: whether the game can download its content (new events), and with no custom
        // content, whether a new version is out, as the game would.
        if (!restart && !s.Offline)
        {
            ui.Log("checking the CDN...");
            var result = await UpdateCheck.RunAsync(game, downloads ?? http);
            ui.Log($"CDN check: {result.Message}");
            if (result.DownloadError is { } error && !ui.ContinueAnyway("Warning! CDN download failed", UpdateCheck.DownloadWarning(game, error)))
            {
                ui.Log("not started: the CDN download failed");
                return null;
            }
            if (result.Outcome == UpdateCheck.Outcome.UpdateAvailable && !Mods.AnyContentOn(game))
            {
                switch (ui.AskAboutUpdate(game, result))
                {
                    case UpdateChoice.Cancel: return null;
                    case UpdateChoice.GetNewVersion:
                        if (ui.AskForNewVersion(game) is not { } files) return null;
                        if (!await InstallVersionAsync(game, files.Apk, files.Obb)) return null;
                        if (game.Problem() is { } p) { ui.ShowProblem(p); return null; }
                        break;
                }
            }
        }

        // Reflourished's own OBB updates, which it does before it starts on Android.
        if (!restart && !s.Offline) await UpdateObbAsync(game);

        var overlays = new List<Mods.Overlay>();
        string? obbPatch = null;
        if (Mods.AnyContentOn(game))
        {
            var conflicts = await Task.Run(() => ModConflicts.Find(game, Mods.Active(game).Where(m => m.HasContent).ToList()));
            if (conflicts.Count > 0)
            {
                ui.ShowProblem("Some mods that are on can't be used together:\n" + string.Join("\n", conflicts.Select(c => c.Describe() + ".")) +
                               "\n\nSwitch " + (conflicts.Any(c => c.Other != null) ? "one of each pair" : "it") + " off in Mods, then play.");
                return null;
            }
            var built = await Task.Run(() => Mods.Build(game, Mods.Active(game).Where(m => m.HasContent).Select(m => m.Key).ToList()));
            built.Log.ForEach(ui.Log);
            overlays = built.Overlays;
            obbPatch = built.ObbPatch;
        }

        string? rfu = Tools.Find(launcherDir, "rfu");
        if (rfu == null) { ui.ShowProblem($"rfu isn't next to the launcher ({launcherDir}). Put rfu.exe and its DLLs there."); return null; }
        var args = RfuCommand.Build(game, overlays, obbPatch);
        ui.Log($"starting {game.DisplayName}");
        var psi = new ProcessStartInfo(rfu) { WorkingDirectory = game.Dir, UseShellExecute = false };
        foreach (var a in args) psi.ArgumentList.Add(a);
        return Process.Start(psi);
    }

    /// Brings the OBB up to date from the game's update server (see ObbUpdate). A failure is logged
    /// and the game starts with the OBB it has, as the game does.
    public async Task UpdateObbAsync(GameFolder game, CancellationToken cancel = default)
    {
        if (!game.Settings.ObbUpdates || game.Obb == null || ObbUpdate.ConfigUrl(game) is not { } config) return;
        if (!ObbUpdate.GameAllowsUpdates(game)) { ui.Log("OBB update: switched off in the game's settings"); return; }
        var client = downloads ?? http;
        string state = ObbUpdate.StateDir(game);
        try
        {
            ui.Log("OBB update: checking...");
            var (head, plan) = await ObbUpdate.PlanAsync(config, game.Obb, state,
                url => ObbUpdate.GetJsonAsync(http, url, cancel), path => Task.Run(() => ObbUpdate.Sha256Async(path, cancel)), ui.Log);
            if (plan is ObbUpdate.Fresh fresh &&
                !ui.Confirm($"This game's OBB doesn't match any recent {game.DisplayName} release (it may have been changed by hand, " +
                            $"or be too old to patch).\n\nDownload the whole {fresh.Base.Version} OBB ({ObbUpdate.Size(ObbUpdate.DownloadSize(plan))})? " +
                            $"The current one is kept in the game's old folder."))
            {
                ui.Log("OBB update: skipped; playing with the OBB as it is");
                return;
            }
            await ObbUpdate.ExecuteAsync(head, plan, game.Obb, state, client, ui.Log, Path.Combine(game.Dir, "old"), cancel);
            game.Refresh();
        }
        catch (Exception e) when (e is ObbUpdate.Error or HttpRequestException or IOException or JsonException or TaskCanceledException or UnauthorizedAccessException)
        {
            ui.Log($"OBB update failed, playing with the OBB as it is: {e.Message}");
        }
    }

    /// Takes the libraries out of a new or newer APK.
    public async Task<bool> PrepareAsync(GameFolder game)
    {
        game.Refresh();
        if (!game.NeedsApkUnpack) return true;
        string? conv = Tools.Find(launcherDir, "pvz2conv");
        if (conv == null) { ui.ShowProblem($"pvz2conv isn't next to the launcher ({launcherDir}); it's needed to read the APK."); return false; }
        ui.Log($"taking the libraries out of {Path.GetFileName(game.Apk)}...");
        int code = await Tools.UnpackApkAsync(conv, game, ui.Log);
        game.Refresh();
        if (code != 0 || !game.LibrariesReady) { ui.ShowProblem("pvz2conv couldn't take the libraries out of the APK; see the log."); return false; }
        return true;
    }

    /// Puts a new version's APK and OBB in the game's folder (the old ones go to old/), then
    /// takes the libraries out. With no OBB given, the OBB stays, and an APK that can download its
    /// OBB brings it up to date (or downloads it, when the new version wants another file).
    public async Task<bool> InstallVersionAsync(GameFolder game, string apk, string? obb)
    {
        string old = Path.Combine(game.Dir, "old");
        Directory.CreateDirectory(old);
        var moving = Directory.EnumerateFiles(game.Dir, "*.apk");
        if (!string.IsNullOrEmpty(obb)) moving = moving.Concat(Directory.EnumerateFiles(game.Dir, "*.obb"));
        foreach (var f in moving.ToList())
            File.Move(f, Path.Combine(old, Path.GetFileName(f)), true);
        ui.Log(string.IsNullOrEmpty(obb) ? "copying the new APK..." : "copying the new APK and OBB...");
        await Task.Run(() =>
        {
            File.Copy(apk, Path.Combine(game.Dir, Path.GetFileName(apk)), true);
            if (!string.IsNullOrEmpty(obb))
            {
                File.Copy(obb, Path.Combine(game.Dir, Path.GetFileName(obb)), true);
                ObbUpdate.ForgetTimestamp(game);  // the new OBB's version is worked out afresh
            }
        });
        // A library the old version downloaded for itself (Reflourished's own updates, in its
        // private files) would be loaded in place of the new APK's.
        foreach (var f in new[] { "libRFLib.so", "_lib_boot" })
            File.Delete(Path.Combine(game.DataDir, "data", "files", f));
        // The name says the old version: the new one comes from the APK's file name, since
        // Reflourished's manifest doesn't carry it (1.5.0's says 1.4.0).
        if (RenamedForVersion(game.DisplayName, VersionInName(Path.GetFileNameWithoutExtension(apk))) is { } renamed)
        {
            ui.Log($"{game.DisplayName} is now called {renamed}");
            game.Settings.DisplayName = renamed;
            game.SaveSettings();
        }
        if (!await PrepareAsync(game)) return false;
        if (string.IsNullOrEmpty(obb) && !game.IsChina) return await EnsureObbAsync(game, apk);
        return true;
    }

    /// The last version number in a name ("PvZ2 Reflourished 1.5.0" gives 1.5.0), or null.
    public static string? VersionInName(string name) =>
        System.Text.RegularExpressions.Regex.Matches(name, @"(?<![\d.])\d+(\.\d+){1,3}(?![\d.])") is { Count: > 0 } m ? m[^1].Value : null;

    /// The name with its version swapped for `version`, or null when there's nothing to change.
    public static string? RenamedForVersion(string name, string? version)
    {
        if (version == null || VersionInName(name) is not { } old || old == version) return null;
        int at = name.LastIndexOf(old, StringComparison.Ordinal);
        return name[..at] + version + name[(at + old.Length)..];
    }

    /// For an APK that downloads its own OBB (Reflourished 1.5.0+): the OBB it wants, downloaded or
    /// brought up to date next to the APK. An APK that can't, or a failed download, asks for the file.
    public async Task<bool> EnsureObbAsync(GameFolder game, string apk, CancellationToken cancel = default)
    {
        game.Refresh();
        string want = game.Info?.Obb ?? $"main.{game.Info?.VersionCode}.{game.Info?.Package}.obb";
        string target = Path.Combine(game.Dir, want);
        if (ObbUpdate.ConfigUrlInApk(Path.Combine(game.Dir, Path.GetFileName(apk))) is not { } config)
            return AskObb(game, target);
        if (game.Settings.ObbUpdateUrl == null && config != ObbUpdate.ReflourishedConfig)
        {
            game.Settings.ObbUpdateUrl = config;
            game.SaveSettings();
        }
        // an OBB for another version moves aside; the new one is downloaded whole
        foreach (var f in Directory.EnumerateFiles(game.Dir, "*.obb").Where(f => !string.Equals(Path.GetFileName(f), want, StringComparison.OrdinalIgnoreCase)).ToList())
        {
            Directory.CreateDirectory(Path.Combine(game.Dir, "old"));
            File.Move(f, Path.Combine(game.Dir, "old", Path.GetFileName(f)), true);
            ObbUpdate.ForgetTimestamp(game);
        }
        string state = ObbUpdate.StateDir(game);
        try
        {
            ui.Log($"the APK downloads its own OBB: asking {config}...");
            var (head, plan) = await ObbUpdate.PlanAsync(config, File.Exists(target) ? target : null, state,
                url => ObbUpdate.GetJsonAsync(http, url, cancel), path => Task.Run(() => ObbUpdate.Sha256Async(path, cancel)), ui.Log);
            if (plan is ObbUpdate.Fresh) ui.Log($"downloading the {head.Version} OBB ({ObbUpdate.Size(ObbUpdate.DownloadSize(plan))})...");
            await ObbUpdate.ExecuteAsync(head, plan, target, state, downloads ?? http, ui.Log, Path.Combine(game.Dir, "old"), cancel);
            game.Refresh();
            return true;
        }
        catch (Exception e) when (e is ObbUpdate.Error or HttpRequestException or IOException or JsonException or TaskCanceledException or UnauthorizedAccessException)
        {
            ui.Log($"couldn't download the OBB: {e.Message}");
            return AskObb(game, target);
        }
    }

    bool AskObb(GameFolder game, string target)
    {
        if (File.Exists(target)) return true;
        if (ui.AskForObb(game) is not { } obb) return false;
        File.Copy(obb, Path.Combine(game.Dir, Path.GetFileName(obb)), true);
        ObbUpdate.ForgetTimestamp(game);
        game.Refresh();
        return true;
    }

    /// A new game folder from an APK and OBB (a standalone mod such as AltverZ, or the game itself).
    /// With no OBB, an APK that downloads its own gets it now (see EnsureObbAsync).
    public async Task<GameFolder?> AddGameAsync(string gamesDir, string name, string apk, string? obb)
    {
        string dir = Path.Combine(gamesDir, string.Concat(name.Split(Path.GetInvalidFileNameChars())).Trim());
        if (Directory.Exists(dir) && Directory.EnumerateFileSystemEntries(dir).Any()) { ui.ShowProblem($"{dir} already exists."); return null; }
        Directory.CreateDirectory(dir);
        await Task.Run(() =>
        {
            File.Copy(apk, Path.Combine(dir, Path.GetFileName(apk)));
            if (!string.IsNullOrEmpty(obb)) File.Copy(obb, Path.Combine(dir, Path.GetFileName(obb)));
        });
        var game = GameFolder.Open(dir);
        if (!await PrepareAsync(game)) return game;
        game.Settings.DisplayName = name;
        game.SaveSettings();
        if (string.IsNullOrEmpty(obb) && !game.IsChina) await EnsureObbAsync(game, apk);
        return game;
    }
}
