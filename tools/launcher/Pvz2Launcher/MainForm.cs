using System.Diagnostics;

namespace Pvz2Launcher;

/// The launcher's window: the games on the left, the picked game's menu on the right (display
/// settings, code injections, custom content, Play), and a log along the bottom.
sealed class MainForm : Form, ILaunchUi
{
    readonly string _launcherDir;
    readonly string _gamesDir;
    readonly LaunchFlow _flow;
    readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(60) };

    List<GameFolder> _games = [];
    GameFolder? _game;
    bool _loading;  // filling the controls in, not the person changing them
    readonly Dictionary<string, Process> _running = new(StringComparer.OrdinalIgnoreCase);

    // games
    readonly ListView _list = new() { View = View.Tile, MultiSelect = false, HideSelection = false, Dock = DockStyle.Fill };
    readonly ImageList _icons = new() { ImageSize = new Size(48, 48), ColorDepth = ColorDepth.Depth32Bit };

    // the game's menu
    readonly PictureBox _icon = new() { Size = new Size(72, 72), SizeMode = PictureBoxSizeMode.Zoom };
    readonly TextBox _name = new() { Width = 320, Font = new Font(SystemFonts.DefaultFont.FontFamily, 13f, FontStyle.Bold) };
    readonly Label _status = new() { AutoSize = true, MaximumSize = new Size(520, 0) };

    readonly ComboBox _assets = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 150 };
    readonly ComboBox _resolution = new() { Width = 150 };
    readonly ComboBox _window = new() { Width = 150 };
    readonly CheckBox _fullscreen = new() { Text = "Full screen", AutoSize = true };
    readonly CheckBox _vsync = new() { Text = "VSync", AutoSize = true };
    readonly CheckBox _angle = new() { Text = "Draw through ANGLE", AutoSize = true };
    readonly CheckBox _offline = new() { Text = "Offline (no CDN, no update check)", AutoSize = true };

    readonly CheckedListBox _injections = new() { CheckOnClick = true, Height = 110, Width = 360, IntegralHeight = false };
    readonly CheckedListBox _mods = new() { CheckOnClick = true, Height = 110, Width = 360, IntegralHeight = false };
    readonly CheckBox _askContent = new() { Text = "Ask for custom content when I press Play", AutoSize = true };
    readonly ToolTip _tips = new();

    readonly Button _play = new() { Text = "Play", Width = 140, Height = 44, Font = new Font(SystemFonts.DefaultFont.FontFamily, 12f, FontStyle.Bold) };
    readonly TextBox _log = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill, Font = new Font(FontFamily.GenericMonospace, 8.5f) };
    readonly Panel _menu = new() { Dock = DockStyle.Fill, AutoScroll = true, Padding = new Padding(12) };

    const string KeybindsItem = "Keybinds (keybinds.txt)";

    public MainForm(string launcherDir)
    {
        _launcherDir = launcherDir;
        _gamesDir = Path.Combine(launcherDir, "games");
        _flow = new LaunchFlow(launcherDir, _http, this);

        Text = "PvZ2 Launcher";
        Size = new Size(1000, 760);
        MinimumSize = new Size(820, 600);
        StartPosition = FormStartPosition.CenterScreen;

        BuildLayout();
        Shown += async (_, _) => await RefreshGamesAsync();
    }

    // --- layout ---------------------------------------------------------------------------------

    void BuildLayout()
    {
        _list.LargeImageList = _icons;
        _list.Columns.AddRange([new ColumnHeader(), new ColumnHeader()]);  // a tile shows its second line from a column
        _list.TileSize = new Size(230, 56);
        _list.SelectedIndexChanged += (_, _) => { if (_list.SelectedItems.Count > 0) Select((GameFolder)_list.SelectedItems[0].Tag!); };

        var gameButtons = new FlowLayoutPanel { Dock = DockStyle.Bottom, AutoSize = true, FlowDirection = FlowDirection.LeftToRight };
        gameButtons.Controls.Add(MakeButton("Add game...", async () => await AddGameAsync()));
        gameButtons.Controls.Add(MakeButton("Games folder", () => OpenFolder(_gamesDir)));
        gameButtons.Controls.Add(MakeButton("Refresh", async () => await RefreshGamesAsync()));
        var left = new Panel { Dock = DockStyle.Fill };
        left.Controls.Add(_list);
        left.Controls.Add(gameButtons);

        _assets.Items.AddRange(["High (1536)", "Low (768)"]);
        _resolution.Items.AddRange(["Auto", "1280x720", "1600x900", "1920x1080", "2400x1080", "2560x1440"]);
        _window.Items.AddRange(["1280x720", "1600x900", "1920x1080"]);
        _tips.SetToolTip(_resolution, "The size the game draws at, whatever the window's. Auto: your screen's shape.");
        _tips.SetToolTip(_assets, "Low is lighter on slow graphics cards.");

        var header = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, WrapContents = false };
        var titleBox = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown };
        titleBox.Controls.Add(_name);
        titleBox.Controls.Add(_status);
        header.Controls.Add(_icon);
        header.Controls.Add(titleBox);

        var display = Group("Display", Row(new Label { Text = "Assets", AutoSize = true, Width = 90 }, _assets),
            Row(new Label { Text = "Resolution", AutoSize = true, Width = 90 }, _resolution),
            Row(new Label { Text = "Window", AutoSize = true, Width = 90 }, _window),
            Row(_fullscreen, _vsync, _angle));

        var injections = Group("Code injections and fixes", _injections,
            Row(MakeButton("Edit keybinds", EditKeybinds), MakeButton("Patches folder", () => { if (_game != null) OpenFolder(_game.PatchesDir); })),
            _offline);

        var content = Group("Custom content (the lowest one wins where two change the same file)", _mods,
            Row(MakeButton("Add custom content...", AddContent), MakeButton("Up", () => MoveMod(-1)), MakeButton("Down", () => MoveMod(1)),
                MakeButton("Mods folder", () => { if (_game != null) OpenFolder(_game.ModsDir); })),
            _askContent);

        var actions = Row(_play, MakeButton("Unpack resources", async () => await UnpackResourcesAsync()),
            MakeButton("Game folder", () => { if (_game != null) OpenFolder(_game.Dir); }),
            MakeButton("Last log", () => { if (_game != null && File.Exists(_game.LogPath)) Open(_game.LogPath); }));
        _play.Click += async (_, _) => await PlayAsync();

        var stack = new FlowLayoutPanel { Dock = DockStyle.Top, AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false };
        stack.Controls.AddRange([header, display, injections, content, actions]);
        _menu.Controls.Add(stack);

        var split = new SplitContainer { Dock = DockStyle.Fill, FixedPanel = FixedPanel.Panel1 };
        split.Panel1.Controls.Add(left);
        split.Panel2.Controls.Add(_menu);
        var outer = new SplitContainer { Dock = DockStyle.Fill, Orientation = Orientation.Horizontal, FixedPanel = FixedPanel.Panel2 };
        outer.Panel1.Controls.Add(split);
        outer.Panel2.Controls.Add(_log);
        Controls.Add(outer);
        // only once the window has its size (a SplitContainer refuses distances beyond it)
        Load += (_, _) =>
        {
            split.SplitterDistance = 250;
            outer.SplitterDistance = Math.Max(200, outer.Height - 150);
        };

        // every change is saved straight away
        _name.TextChanged += (_, _) => Change(s => s.DisplayName = _name.Text.Trim() is { Length: > 0 } n ? n : null);
        _assets.SelectedIndexChanged += (_, _) => Change(s => s.Assets = _assets.SelectedIndex == 1 ? "low" : "high");
        _resolution.TextChanged += (_, _) => Change(s =>
        {
            string v = _resolution.Text.Trim();
            if (v is "" or "Auto") s.Resolution = null;
            else if (RfuCommand.IsSize(v)) s.Resolution = v;
        });
        _window.TextChanged += (_, _) => Change(s => { if (RfuCommand.IsSize(_window.Text.Trim())) s.Window = _window.Text.Trim(); });
        _fullscreen.CheckedChanged += (_, _) => Change(s => s.Fullscreen = _fullscreen.Checked);
        _vsync.CheckedChanged += (_, _) => Change(s => s.VSync = _vsync.Checked);
        _angle.CheckedChanged += (_, _) => Change(s => s.Angle = _angle.Checked);
        _offline.CheckedChanged += (_, _) => Change(s => s.Offline = _offline.Checked);
        _askContent.CheckedChanged += (_, _) => Change(s => s.AskForCustomContent = _askContent.Checked);
        // ItemCheck fires before the box changes; read the lists once it has
        _injections.ItemCheck += (_, _) => BeginInvoke(() => Change(ReadInjections));
        _mods.ItemCheck += (_, _) => BeginInvoke(() => Change(ReadMods));
    }

    static GroupBox Group(string title, params Control[] rows)
    {
        var flow = new FlowLayoutPanel { Location = new Point(8, 20), AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false };
        flow.Controls.AddRange(rows);
        var g = new GroupBox { Text = title, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Padding = new Padding(8), MinimumSize = new Size(540, 0) };
        g.Controls.Add(flow);
        return g;
    }

    static FlowLayoutPanel Row(params Control[] controls)
    {
        var row = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, WrapContents = false };
        row.Controls.AddRange(controls);
        return row;
    }

    static Button MakeButton(string text, Action onClick)
    {
        var b = new Button { Text = text, AutoSize = true, Padding = new Padding(4, 2, 4, 2) };
        b.Click += (_, _) => onClick();
        return b;
    }

    // --- games ----------------------------------------------------------------------------------

    async Task RefreshGamesAsync()
    {
        string? selected = _game?.Dir;
        _games = GameFolder.Scan(_gamesDir);
        // a new or newer APK: take its libraries (and icon) out now, so the menu can show them
        foreach (var g in _games.Where(g => g.NeedsApkUnpack).ToList())
            await _flow.PrepareAsync(g);

        _list.Items.Clear();
        _icons.Images.Clear();
        foreach (var g in _games)
        {
            if (g.HasIcon && LoadImage(g.IconPath) is { } img) _icons.Images.Add(g.Dir, img);
            var item = new ListViewItem(g.DisplayName, g.Dir) { Tag = g };
            item.SubItems.Add(g.Info?.VersionName is { Length: > 0 } v ? $"version {v}" : g.Problem() != null ? "needs files" : "");
            _list.Items.Add(item);
        }
        if (_games.Count == 0)
        {
            Log($"No games yet. Make a folder for each game in {_gamesDir} and put its .apk and .obb there, or use \"Add game\".");
            _menu.Enabled = false;
            return;
        }
        var pick = _games.FindIndex(g => g.Dir == selected);
        _list.Items[Math.Max(pick, 0)].Selected = true;
    }

    void Select(GameFolder game)
    {
        _game = game;
        _menu.Enabled = true;
        _loading = true;
        try
        {
            var s = game.Settings;
            _icon.Image = game.HasIcon ? LoadImage(game.IconPath) : null;
            if (_icon.Image is Bitmap bmp) Icon = Icon.FromHandle(new Bitmap(bmp, 32, 32).GetHicon());
            _name.Text = game.DisplayName;
            var info = game.Info;
            string version = info != null ? $"{info.Package}  version {info.VersionName} ({info.VersionCode})" : "";
            if (game.ModLibraries.Count > 0) version += $"\nmod code: {string.Join(", ", game.ModLibraries)}";
            _status.Text = game.Problem() is { } problem ? $"{version}\n{problem}".Trim() : $"{version}\n{Path.GetFileName(game.Obb)}";
            _status.ForeColor = game.Problem() != null ? Color.Firebrick : SystemColors.ControlText;

            _assets.SelectedIndex = s.Assets == "low" ? 1 : 0;
            _resolution.Text = s.Resolution ?? "Auto";
            _window.Text = s.Window;
            _fullscreen.Checked = s.Fullscreen;
            _vsync.Checked = s.VSync;
            _angle.Checked = s.Angle;
            _offline.Checked = s.Offline;
            _askContent.Checked = s.AskForCustomContent;

            _injections.Items.Clear();
            _injections.Items.Add(KeybindsItem, s.Keybinds);
            foreach (var p in game.PatchFiles())
                _injections.Items.Add(new PatchItem(p), s.Patches.Contains(p.FileName, StringComparer.OrdinalIgnoreCase));

            FillMods();
            _play.Enabled = game.Problem() == null && !_running.ContainsKey(game.Dir);
        }
        finally { _loading = false; }
    }

    // Enabled mods first, in load order, then the rest by name.
    void FillMods()
    {
        if (_game == null) return;
        var all = Mods.List(_game);
        var s = _game.Settings;
        _mods.Items.Clear();
        foreach (var name in s.Mods)
            if (all.FirstOrDefault(m => m.FolderName == name) is { } m) _mods.Items.Add(new ModItem(m), true);
        foreach (var m in all.Where(m => !s.Mods.Contains(m.FolderName)))
            _mods.Items.Add(new ModItem(m), false);
    }

    void Change(Action<GameSettings> edit)
    {
        if (_loading || _game == null) return;
        edit(_game.Settings);
        _game.SaveSettings();
    }

    void ReadInjections(GameSettings s)
    {
        s.Patches.Clear();
        for (int i = 0; i < _injections.Items.Count; i++)
        {
            bool on = _injections.GetItemChecked(i);
            switch (_injections.Items[i])
            {
                case KeybindsItem: s.Keybinds = on; break;
                case PatchItem p when on: s.Patches.Add(p.Patch.FileName); break;
            }
        }
        if (s.Keybinds) _game?.EnsureKeybindsFile();
    }

    void ReadMods(GameSettings s)
    {
        s.Mods.Clear();
        for (int i = 0; i < _mods.Items.Count; i++)
            if (_mods.GetItemChecked(i)) s.Mods.Add(((ModItem)_mods.Items[i]).Mod.FolderName);
    }

    void MoveMod(int by)
    {
        int i = _mods.SelectedIndex, j = i + by;
        if (i < 0 || j < 0 || j >= _mods.Items.Count) return;
        var item = _mods.Items[i];
        bool on = _mods.GetItemChecked(i);
        _loading = true;
        _mods.Items.RemoveAt(i);
        _mods.Items.Insert(j, item);
        _mods.SetItemChecked(j, on);
        _mods.SelectedIndex = j;
        _loading = false;
        Change(ReadMods);
    }

    sealed record PatchItem(PatchFile Patch)
    {
        public override string ToString() => Patch.Description == "" ? Patch.Name : $"{Patch.Name} - {Patch.Description}";
    }

    sealed record ModItem(Mods.Mod Mod)
    {
        public override string ToString() =>
            Mod.Name + (Mod.Version != "" ? $" {Mod.Version}" : "") + (Mod.Author != "" ? $" by {Mod.Author}" : "") +
            (Mod.Description != "" ? $" - {Mod.Description}" : "");
    }

    // --- actions --------------------------------------------------------------------------------

    async Task PlayAsync()
    {
        if (_game == null) return;
        var game = _game;
        _play.Enabled = false;
        try
        {
            var process = await _flow.RunAsync(game);
            Select(game);  // the flow may have added content or a new version
            if (process == null) return;
            _running[game.Dir] = process;
            _play.Enabled = false;
            process.EnableRaisingEvents = true;
            process.Exited += (_, _) => BeginInvoke(() => GameExited(game, process));
        }
        catch (Exception e)
        {
            ShowProblem($"Couldn't start the game: {e.Message}");
        }
        finally
        {
            if (!_running.ContainsKey(game.Dir) && _game == game) _play.Enabled = game.Problem() == null;
        }
    }

    void GameExited(GameFolder game, Process process)
    {
        _running.Remove(game.Dir);
        int code = process.ExitCode;
        Log($"{game.DisplayName} closed{(code != 0 ? $" with exit code {code}" : "")}");
        if (code != 0 && File.Exists(game.LogPath))
        {
            // the end of the log usually says what went wrong
            foreach (var line in File.ReadLines(game.LogPath).TakeLast(15)) Log("  " + line);
        }
        if (_game == game) _play.Enabled = game.Problem() == null;
    }

    async Task AddGameAsync()
    {
        var apk = PickFile("The game's APK", "Android app (*.apk)|*.apk|All files|*.*");
        if (apk == null) return;
        var obb = PickFile("The game's OBB", "Game data (*.obb)|*.obb|All files|*.*", Path.GetDirectoryName(apk));
        if (obb == null) return;
        var game = await _flow.AddGameAsync(_gamesDir, Path.GetFileNameWithoutExtension(apk), apk, obb);
        if (game == null) return;
        _game = game;
        await RefreshGamesAsync();
    }

    void AddContent()
    {
        if (_game == null) return;
        var folder = PickFolder("The custom content's folder");
        if (folder == null) return;
        switch (Mods.Classify(folder))
        {
            case Mods.ContentKind.StandaloneGame:
                ShowProblem("That folder has an .obb: it's a game of its own (like AltverZ). Add it with \"Add game\" instead.");
                return;
            case Mods.ContentKind.Empty:
                ShowProblem("That folder is empty.");
                return;
        }
        string name = Mods.Import(_game, folder);
        _game.Settings.Mods.Add(name);
        _game.SaveSettings();
        Log($"added custom content \"{name}\"");
        _loading = true;
        FillMods();
        _loading = false;
    }

    void EditKeybinds()
    {
        if (_game == null) return;
        _game.EnsureKeybindsFile();
        Open(_game.KeybindsPath);
    }

    async Task UnpackResourcesAsync()
    {
        if (_game?.Obb == null) return;
        string? conv = Tools.Find(_launcherDir, "pvz2conv");
        if (conv == null) { ShowProblem("pvz2conv isn't next to the launcher."); return; }
        var game = _game;
        Log($"unpacking {Path.GetFileName(game.Obb)} into {game.UnpackedDir} (this takes a while)...");
        int code = await Tools.UnpackObbAsync(conv, game, Log);
        Log(code == 0 ? $"done: {game.UnpackedDir}" : "pvz2conv reported failures; see _pvz2conv_log.txt in the unpacked folder");
    }

    // --- ILaunchUi --------------------------------------------------------------------------------

    public void Log(string line)
    {
        if (InvokeRequired) { BeginInvoke(() => Log(line)); return; }
        _log.AppendText($"{DateTime.Now:HH:mm:ss}  {line}{Environment.NewLine}");
    }

    public string? AskForCustomContent(GameFolder game)
    {
        var choose = new TaskDialogButton("Choose a folder...");
        var none = new TaskDialogButton("Play without");
        var page = new TaskDialogPage
        {
            Caption = game.DisplayName,
            Heading = "Load custom content?",
            Text = "Custom levels, strings or other content to lay over the game's own for this session. It's kept in the game's mods folder for next time.",
            Buttons = { choose, none },
            DefaultButton = none,
            Verification = new TaskDialogVerificationCheckBox("Don't ask again (add content from the menu)"),
        };
        var answer = TaskDialog.ShowDialog(this, page);
        if (page.Verification.Checked)
        {
            game.Settings.AskForCustomContent = false;
            game.SaveSettings();
        }
        return answer == choose ? PickFolder("The custom content's folder") : null;
    }

    public UpdateChoice AskAboutUpdate(GameFolder game, UpdateCheck.Result result)
    {
        var update = new TaskDialogButton("Give it the new version...");
        var play = new TaskDialogButton("Play anyway");
        var page = new TaskDialogPage
        {
            Caption = game.DisplayName,
            Heading = "A new version of the game is out",
            Text = $"The CDN says {result.Message}. Point the launcher at the new version's APK and OBB, and it puts them in place " +
                   "(your saves stay). Playing the old version may not get the CDN's content.",
            Icon = TaskDialogIcon.Information,
            Buttons = { update, play, TaskDialogButton.Cancel },
            DefaultButton = update,
        };
        var answer = TaskDialog.ShowDialog(this, page);
        return answer == update ? UpdateChoice.GetNewVersion : answer == play ? UpdateChoice.PlayAnyway : UpdateChoice.Cancel;
    }

    public (string Apk, string Obb)? AskForNewVersion(GameFolder game)
    {
        var apk = PickFile($"The new {game.DisplayName} APK", "Android app (*.apk)|*.apk|All files|*.*");
        if (apk == null) return null;
        var obb = PickFile($"The new {game.DisplayName} OBB", "Game data (*.obb)|*.obb|All files|*.*", Path.GetDirectoryName(apk));
        return obb == null ? null : (apk, obb);
    }

    public void ShowProblem(string message)
    {
        Log(message);
        MessageBox.Show(this, message, Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
    }

    // --- helpers --------------------------------------------------------------------------------

    string? PickFile(string title, string filter, string? dir = null)
    {
        using var d = new OpenFileDialog { Title = title, Filter = filter, InitialDirectory = dir ?? "" };
        return d.ShowDialog(this) == DialogResult.OK ? d.FileName : null;
    }

    string? PickFolder(string title)
    {
        using var d = new FolderBrowserDialog { Description = title, UseDescriptionForTitle = true };
        return d.ShowDialog(this) == DialogResult.OK ? d.SelectedPath : null;
    }

    static Image? LoadImage(string path)
    {
        try
        {
            // a copy, so the file isn't kept open
            using var fs = File.OpenRead(path);
            using var img = Image.FromStream(fs);
            return new Bitmap(img, 128, 128);
        }
        catch (Exception e) when (e is IOException or ArgumentException) { return null; }
    }

    static void OpenFolder(string dir)
    {
        Directory.CreateDirectory(dir);
        Open(dir);
    }

    static void Open(string path) => Process.Start(new ProcessStartInfo(path) { UseShellExecute = true });
}
