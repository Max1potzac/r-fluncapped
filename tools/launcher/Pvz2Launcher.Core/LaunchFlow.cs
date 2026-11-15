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

    /// The new version's APK and OBB, or null when the person backs out.
    (string Apk, string Obb)? AskForNewVersion(GameFolder game);

    void ShowProblem(string message);

    /// A yes/no question (a big download, say).
    bool Confirm(string message);
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
        s.Mods.RemoveAll(m => !Directory.Exists(Path.Combine(game.ModsDir, m)));

        // Custom content: the mods switched on, or a folder picked now.
        if (!restart && s.Mods.Count == 0 && s.AskForCustomContent && ui.AskForCustomContent(game) is { } folder)
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
                    s.Mods.Add(name);
                    ui.Log($"added custom content \"{name}\" to {game.ModsDir}");
                    break;
            }
        }
        game.SaveSettings();

        // No custom content: check the CDN for a new version, as the game would.
        if (!restart && s.Mods.Count == 0 && !s.Offline)
        {
            ui.Log("checking for a new version...");
            var result = await UpdateCheck.RunAsync(game, http);
            ui.Log($"update check: {result.Message}");
            if (result.Outcome == UpdateCheck.Outcome.UpdateAvailable)
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
        if (s.Mods.Count > 0)
        {
            var built = await Task.Run(() => Mods.Build(game, s.Mods));
            built.Log.ForEach(ui.Log);
            overlays = built.Overlays;
        }
        if (s.Keybinds) game.EnsureKeybindsFile();

        string? rfu = Tools.Find(launcherDir, "rfu");
        if (rfu == null) { ui.ShowProblem($"rfu isn't next to the launcher ({launcherDir}). Put rfu.exe and its DLLs there."); return null; }
        var args = RfuCommand.Build(game, overlays);
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
    /// takes its libraries out. Saves and settings stay.
    public async Task<bool> InstallVersionAsync(GameFolder game, string apk, string? obb)
    {
        string old = Path.Combine(game.Dir, "old");
        Directory.CreateDirectory(old);
        foreach (var f in Directory.EnumerateFiles(game.Dir, "*.apk").Concat(Directory.EnumerateFiles(game.Dir, "*.obb")))
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
        return await PrepareAsync(game);
    }

    /// A new game folder from an APK and OBB (a standalone mod such as AltverZ, or the game itself).
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
        return game;
    }
}
