using System.Diagnostics;

namespace Pvz2Launcher;

/// rfu's command line for a game, from its folder and settings.
public static class RfuCommand
{
    public static List<string> Build(GameFolder game, IReadOnlyList<Mods.Overlay> overlays)
    {
        var s = game.Settings;
        var args = new List<string> { "--lib-dir", game.LibDir, "--obb", game.Obb!, "--data", game.DataDir, "--log-file", game.LogPath };
        if (game.Info is { } info)
        {
            if (info.Package != "") args.AddRange(["--package", info.Package]);
            if (info.VersionCode != "") args.AddRange(["--version-code", info.VersionCode]);
            if (info.VersionName != "") args.AddRange(["--version-name", info.VersionName]);
        }
        args.AddRange(["--title", game.DisplayName]);
        if (game.HasIcon) args.AddRange(["--icon", game.IconPath]);

        args.AddRange(["--assets", s.Assets == "low" ? "low" : "high"]);
        if (!string.IsNullOrWhiteSpace(s.Resolution)) args.AddRange(["--resolution", s.Resolution]);
        if (!string.IsNullOrWhiteSpace(s.Window)) args.AddRange(["--window", s.Window]);
        if (s.Fullscreen) args.Add("--fullscreen");
        if (!s.VSync) args.Add("--no-vsync");
        if (s.Angle) args.Add("--angle");
        if (s.Offline) args.Add("--offline");

        if (s.HoverCollect) args.Add("--hover");
        if (s.FixAnimations) args.Add("--fix-animations");
        if (s.Keybinds && File.Exists(game.KeybindsPath)) args.AddRange(["--keybinds", game.KeybindsPath]);
        foreach (var patch in s.Patches)
        {
            string path = Path.Combine(game.PatchesDir, patch);
            if (File.Exists(path)) args.AddRange(["--patch", path]);
        }
        foreach (var o in overlays) args.AddRange(["--overlay", $"{o.Guest}={o.Host}"]);
        args.AddRange(s.ExtraArgs);
        return args;
    }

    /// "WxH" with sensible numbers.
    public static bool IsSize(string? v) =>
        v != null && v.Split('x') is [var w, var h] && int.TryParse(w, out int x) && int.TryParse(h, out int y) && x is >= 160 and <= 16384 && y is >= 120 and <= 16384;
}

/// The programs the launcher runs: rfu (the game) and pvz2conv (unpacking), found next to the
/// launcher or in a folder of their own beside it.
public static class Tools
{
    public static string? Find(string baseDir, string name)
    {
        string exe = OperatingSystem.IsWindows() ? name + ".exe" : name;
        foreach (var dir in new[] { baseDir, Path.Combine(baseDir, name), Path.Combine(baseDir, "tools"), Path.Combine(baseDir, "bin") })
        {
            string p = Path.Combine(dir, exe);
            if (File.Exists(p)) return p;
        }
        return null;
    }

    /// Runs a program to the end, passing each line it prints to `output`. Returns its exit code.
    public static async Task<int> RunAsync(string exe, IEnumerable<string> args, string workDir, Action<string> output, CancellationToken cancel = default)
    {
        var psi = new ProcessStartInfo(exe)
        {
            WorkingDirectory = workDir,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            RedirectStandardInput = true,
            UseShellExecute = false,
            CreateNoWindow = true,
        };
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = new Process { StartInfo = psi };
        p.OutputDataReceived += (_, e) => { if (e.Data != null) output(e.Data); };
        p.ErrorDataReceived += (_, e) => { if (e.Data != null) output(e.Data); };
        p.Start();
        p.StandardInput.Close();
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
        await p.WaitForExitAsync(cancel);
        return p.ExitCode;
    }

    /// Takes the libraries, version and icon out of the game's APK, into its folder.
    public static Task<int> UnpackApkAsync(string pvz2conv, GameFolder game, Action<string> output) =>
        RunAsync(pvz2conv, ["--no-pause", "--out", game.Dir, game.Apk!], game.Dir, output);

    /// Unpacks the OBB's resources into unpacked/, converted to PNG and JSON, for modding.
    public static Task<int> UnpackObbAsync(string pvz2conv, GameFolder game, Action<string> output) =>
        RunAsync(pvz2conv, ["--no-pause", "--out", game.UnpackedDir, game.Obb!], game.Dir, output);
}
