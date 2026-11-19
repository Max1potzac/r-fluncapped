using System.Diagnostics;
using Pvz2Launcher.Ui;

namespace Pvz2Launcher;

/// The launcher's window, dressed like PvZ2's own menus: the games on the left, the picked game
/// in a banner with its Play button, and tabs for its mods (code injections, content and
/// localizations, filtered by category and by the games they're for), display, game and the log.
/// Light (a day on the lawn) or Dark (the night levels), or Windows' own choice.
sealed class MainForm : BackdropForm, ILaunchUi
{
    readonly string _launcherDir;
    readonly string _gamesDir;
    readonly string _sharedModsDir;
    readonly LaunchFlow _flow;
    readonly LauncherSettings _settings;
    readonly HttpClient _http = new() { Timeout = TimeSpan.FromSeconds(60) };
    readonly HttpClient _downloads = new() { Timeout = Timeout.InfiniteTimeSpan };  // OBB updates can be a gigabyte

    List<GameFolder> _games = [];
    GameFolder? _game;
    bool _loading;  // filling the controls in, not the person changing them
    readonly Dictionary<string, Process> _running = new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<string, List<DateTime>> _restarts = new(StringComparer.OrdinalIgnoreCase);
    readonly Dictionary<string, Image?> _iconCache = new(StringComparer.OrdinalIgnoreCase);

    // top
    readonly TitleLabel _title = new("PvZ2 Launcher", 20) { Fill = p => p.IsDark ? Color.FromArgb(0xC8, 0xF0, 0x7A) : Color.FromArgb(0xB6, 0xF0, 0x5A) };
    readonly GlossButton _themeButton = new("Theme: Auto", p => p.Blue);
    readonly GlossButton _sharedModsButton = new("Mods folder", p => p.Wood);
    readonly GlossButton _gamesFolderButton = new("Games folder", p => p.Wood);

    // games
    readonly WoodPanel _gamesPanel = new("Your Games");
    readonly CardList _gameList = new(66);
    readonly GlossButton _addGame = new("+ Add game", p => p.Green);
    readonly GlossButton _removeGame = new("Remove", p => p.Red);
    readonly GlossButton _refresh = new("Refresh", p => p.Wood);  // in the top bar

    // the game's banner
    readonly HeaderPanel _header = new();
    readonly TextBox _name = new() { BorderStyle = BorderStyle.None };
    readonly ThemedLabel _status = new("", 9.5f, muted: true) { AutoSize = false };
    readonly GlossButton _play = new("PLAY", p => p.Yellow, 22) { Height = 76, Width = 210 };

    // tabs
    readonly GlossButton[] _tabs = [new("Mods", p => p.Green), new("Display", p => p.Green), new("Game", p => p.Green), new("Log", p => p.Green)];
    readonly WoodPanel _body = new();
    readonly Panel[] _pages = [new BackdropPanel(), new BackdropPanel(), new BackdropPanel(), new BackdropPanel()];

    // mods page
    readonly BackdropFlow _chips = new() { WrapContents = false, AutoSize = false, Height = 36 };
    readonly SearchBox _search = new("Search mods") { Width = 170 };
    readonly SwitchRow _otherGames = new("All games") { Width = 136 };
    readonly CardList _modList = new(Draw.ModCardHeight + 6);
    readonly ThemedLabel _modDetail = new("", 9f, muted: true) { AutoSize = false };
    readonly BackdropFlow _modActions = new() { WrapContents = false, AutoSize = false, Height = 42, FlowDirection = FlowDirection.RightToLeft };
    readonly BackdropFlow _modButtons = new() { WrapContents = false, AutoSize = false, Height = 42 };
    string _category = "All";
    readonly Dictionary<string, Mods.StringCoverage?> _coverage = new(StringComparer.OrdinalIgnoreCase);
    // the game's mods as last read from disk, and the keys of the ones that take part in a run:
    // read when something changes, never while drawing
    List<Mods.Mod> _allMods = [];
    HashSet<string> _activeKeys = new(StringComparer.OrdinalIgnoreCase);
    (string Key, bool On)? _detailFor;

    // display page
    readonly ComboBox _assets = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 170 };
    readonly ComboBox _resolution = new() { Width = 170 };
    readonly ComboBox _window = new() { Width = 170 };
    readonly SwitchRow _fullscreen = new("Full screen");
    readonly SwitchRow _vsync = new("VSync");
    readonly SwitchRow _angle = new("Draw through ANGLE");

    // game page
    readonly SwitchRow _offline = new("Offline (no CDN, no update check)");
    readonly SwitchRow _obbUpdates = new("Download OBB updates (Reflourished)");
    readonly SwitchRow _askContent = new("Ask for custom content when I press Play");
    readonly ThemedLabel _modLibs = new("", 9.5f, muted: true) { AutoSize = false };

    // log
    readonly TextBox _log = new() { Multiline = true, ReadOnly = true, ScrollBars = ScrollBars.Vertical, Dock = DockStyle.Fill };
    readonly StatusBar _statusBar = new();
    readonly ToolTip _tips = new();

    public MainForm(string launcherDir)
    {
        _launcherDir = launcherDir;
        _gamesDir = Path.Combine(launcherDir, "games");
        _sharedModsDir = Path.Combine(launcherDir, "mods");
        _flow = new LaunchFlow(launcherDir, _http, this, _downloads);
        _settings = LauncherSettings.Load(launcherDir);
        Theme.Set(Theme.FromSetting(_settings.Theme));

        Text = "PvZ2 Launcher";
        Size = new Size(1200, 800);
        MinimumSize = new Size(980, 680);
        StartPosition = FormStartPosition.CenterScreen;
        DoubleBuffered = true;
        SetStyle(ControlStyles.ResizeRedraw, true);
        Font = Theme.Text(9.5f);

        BuildLayout();
        ApplyTheme();
        Theme.Changed += ApplyTheme;
        Shown += async (_, _) =>
        {
            try
            {
                var added = BuiltinMods.Seed(_sharedModsDir);
                if (added.Count > 0) Log($"built-in mods written to {_sharedModsDir}: {string.Join(", ", added)}");
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Log($"couldn't write the built-in mods: {e.Message}"); }
            await RefreshGamesAsync();
        };
    }

    // --- layout ---------------------------------------------------------------------------------

    void BuildLayout()
    {
        Controls.AddRange([_title, _themeButton, _refresh, _sharedModsButton, _gamesFolderButton, _gamesPanel, _header, _body, _statusBar]);
        foreach (var t in _tabs) { t.Toggle = true; t.Width = 116; Controls.Add(t); }
        _title.Width = 420;
        _themeButton.Click += (_, _) => CycleTheme();
        _sharedModsButton.Click += (_, _) => OpenFolder(_sharedModsDir);
        _gamesFolderButton.Click += (_, _) => OpenFolder(_gamesDir);
        _tips.SetToolTip(_sharedModsButton, "Mods for every game they fit: the built-in injections and samples, and your own.");
        _tips.SetToolTip(_themeButton, "Light, Dark, or Windows' own setting");

        // games
        _gamesPanel.Controls.AddRange([_gameList, _addGame, _removeGame]);
        _gameList.DrawCard = DrawGameCard;
        _gameList.SelectedIndexChanged += (_, _) => { if (_gameList.SelectedItem is GameFolder g && g != _game) Select(g); };
        _addGame.Click += async (_, _) => await AddGameAsync();
        _removeGame.Click += async (_, _) => await RemoveGameAsync();
        _refresh.Click += async (_, _) => await RefreshGamesAsync();
        _gamesPanel.Resize += (_, _) => LayoutGames();

        // banner
        _header.Controls.AddRange([_name, _status, _play]);
        _name.Font = Theme.Title(17);
        _play.Click += async (_, _) => await PlayAsync();
        _header.Resize += (_, _) => LayoutHeader();

        // tabs and pages
        for (int i = 0; i < _tabs.Length; i++)
        {
            int n = i;
            _tabs[i].Click += (_, _) => ShowPage(n);
            _pages[i].Dock = DockStyle.Fill;
            _body.Controls.Add(_pages[i]);
        }
        BuildModsPage(_pages[0]);
        BuildDisplayPage(_pages[1]);
        BuildGamePage(_pages[2]);
        _pages[3].Controls.Add(_log);
        ShowPage(0);

        Resize += (_, _) => LayoutWindow();
        Load += (_, _) => LayoutWindow();

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
        _offline.CheckedChanged += (_, _) => { Change(s => s.Offline = _offline.Checked); if (!_loading) { RecomputeActive(); _modList.Invalidate(); } };
        _obbUpdates.CheckedChanged += (_, _) => Change(s => s.ObbUpdates = _obbUpdates.Checked);
        _askContent.CheckedChanged += (_, _) => Change(s => s.AskForCustomContent = _askContent.Checked);
    }

    void BuildModsPage(Panel page)
    {
        page.Controls.AddRange([_chips, _search, _otherGames, _modList, _modDetail, _modButtons, _modActions]);
        _search.TextChanged += (_, _) => FillMods();
        _otherGames.CheckedChanged += (_, _) => FillMods();
        _modList.DrawCard = DrawModCard;
        _modList.ClickCard = (item, bounds, at) =>
        {
            if (item is Mods.Mod m && Draw.ModSwitchRect(bounds).Contains(at)) ToggleMod(m);
        };
        _modList.DoubleClick += (_, _) => { if (_modList.SelectedItem is Mods.Mod m) ToggleMod(m); };
        _modList.KeyDown += (_, e) => { if (e.KeyCode == Keys.Space && _modList.SelectedItem is Mods.Mod m) { ToggleMod(m); e.Handled = true; } };
        _modList.SelectedIndexChanged += (_, _) => ShowModDetail();
        _tips.SetToolTip(_otherGames, "Show the mods made for other games too, greyed out");

        var add = new GlossButton("+ Add mod", p => p.Green, 9.5f) { Height = 36, Margin = new Padding(0, 3, 6, 3) };
        add.Click += (_, _) => AddContent();
        var restore = new GlossButton("Restore", p => p.Wood, 9.5f) { Height = 36, Margin = new Padding(0, 3, 6, 3) };
        restore.Click += (_, _) => RestoreBuiltins();
        _tips.SetToolTip(restore, "Write the launcher's own mods back into the mods folder, as they came");
        _modButtons.Controls.AddRange([add, restore]);
        page.Resize += (_, _) => LayoutModsPage(page);
    }

    void BuildDisplayPage(Panel page)
    {
        _assets.Items.AddRange(["High (1536)", "Low (768)"]);
        _resolution.Items.AddRange(["Auto", "1280x720", "1600x900", "1920x1080", "2400x1080", "2560x1440"]);
        _window.Items.AddRange(["1280x720", "1600x900", "1920x1080"]);
        _tips.SetToolTip(_resolution, "The size the game draws at, whatever the window's. Auto: your screen's shape.");
        _tips.SetToolTip(_assets, "Low is lighter on slow graphics cards.");
        var stack = Stack(page);
        stack.Controls.Add(Heading("Graphics"));
        stack.Controls.Add(Field("Assets", _assets));
        stack.Controls.Add(Field("Resolution", _resolution));
        stack.Controls.Add(Field("Window", _window));
        stack.Controls.AddRange([_fullscreen, _vsync, _angle]);
    }

    void BuildGamePage(Panel page)
    {
        var stack = Stack(page);
        stack.Controls.Add(Heading("Playing"));
        stack.Controls.AddRange([_offline, _obbUpdates, _askContent]);
        stack.Controls.Add(Heading("Controls"));
        stack.Controls.Add(Buttons(("Edit keybinds", EditKeybinds), ("Keybind layout editor", () => OpenLayoutEditor(null)),
            ("Controller settings", ControllerSettings)));
        stack.Controls.Add(Heading("The game's files"));
        stack.Controls.Add(Buttons(("Update game...", async () => await UpdateGameAsync()), ("Unpack resources", async () => await UnpackResourcesAsync()),
            ("Game folder", () => { if (_game != null) OpenFolder(_game.Dir); }),
            ("Last log", () => { if (_game != null && File.Exists(_game.LogPath)) Open(_game.LogPath); })));
        _modLibs.Width = 640;
        _modLibs.Height = 40;
        stack.Controls.Add(_modLibs);
    }

    static FlowLayoutPanel Stack(Panel page)
    {
        var stack = new BackdropFlow
        {
            Dock = DockStyle.Fill, FlowDirection = FlowDirection.TopDown, WrapContents = false, AutoScroll = true,
            Padding = new Padding(8, 4, 8, 4),
        };
        page.Controls.Add(stack);
        return stack;
    }

    static ThemedLabel Heading(string text) => new(text, 12f, chunky: true) { Margin = new Padding(0, 10, 0, 6) };

    static FlowLayoutPanel Field(string label, Control c)
    {
        var row = new BackdropFlow { AutoSize = true, WrapContents = false, Margin = new Padding(0, 2, 0, 2) };
        row.Controls.Add(new ThemedLabel(label, 10f) { AutoSize = false, Width = 110, Height = 28, TextAlign = ContentAlignment.MiddleLeft });
        row.Controls.Add(c);
        return row;
    }

    static FlowLayoutPanel Buttons(params (string Text, Action OnClick)[] buttons)
    {
        var row = new BackdropFlow { AutoSize = true, WrapContents = true, MaximumSize = new Size(700, 0) };
        foreach (var (text, onClick) in buttons)
        {
            var b = new GlossButton(text, p => p.Blue, 9.5f) { Height = 38, Margin = new Padding(0, 2, 8, 6) };
            b.Click += (_, _) => onClick();
            row.Controls.Add(b);
        }
        return row;
    }

    void LayoutWindow()
    {
        if (ClientSize.Width < 100) return;
        const int m = 16, top = 64, statusH = 28;
        int w = ClientSize.Width, h = ClientSize.Height;
        _title.SetBounds(m, 8, 420, 50);
        _gamesFolderButton.Location = new Point(w - m - _gamesFolderButton.Width, 14);
        _sharedModsButton.Location = new Point(_gamesFolderButton.Left - 8 - _sharedModsButton.Width, 14);
        _refresh.Location = new Point(_sharedModsButton.Left - 8 - _refresh.Width, 14);
        _themeButton.Location = new Point(_refresh.Left - 8 - _themeButton.Width, 14);
        int left = 286;
        _gamesPanel.SetBounds(m, top, left - m, h - top - statusH - 46);
        int x = left + 14, rw = w - x - m;
        _header.SetBounds(x, top, rw, 150);
        int tx = x + 10;
        foreach (var t in _tabs) { t.Location = new Point(tx, top + 156); tx += t.Width + 6; }
        _body.SetBounds(x, top + 198, rw, h - (top + 198) - statusH - 46);
        _statusBar.SetBounds(0, h - statusH, w, statusH);
        Invalidate();
    }

    void LayoutGames()
    {
        var pad = _gamesPanel.Padding;
        int w = _gamesPanel.Width - pad.Horizontal, bottom = _gamesPanel.Height - pad.Bottom;
        int half = (w - 6) / 2;
        _addGame.SetBounds(pad.Left, bottom - 42, half + 14, 40);
        _removeGame.SetBounds(_addGame.Right + 6, bottom - 42, w - half - 20, 40);
        _gameList.SetBounds(pad.Left + 2, pad.Top, w - 4, bottom - 50 - pad.Top);
    }

    void LayoutHeader()
    {
        var pad = new Padding(16, 14, 16, 18);
        int left = pad.Left + 110;
        _play.Location = new Point(_header.Width - pad.Right - _play.Width - 10, pad.Top + 18);
        _name.SetBounds(left, pad.Top + 10, Math.Max(100, _play.Left - left - 16), 34);
        _status.SetBounds(left + 2, pad.Top + 46, Math.Max(100, _play.Left - left - 16), 40);
    }

    void LayoutModsPage(Panel page)
    {
        int w = page.Width, h = page.Height;
        _search.SetBounds(w - _search.Width - 4, 5, _search.Width, 32);
        _otherGames.SetBounds(_search.Left - _otherGames.Width - 6, 5, _otherGames.Width, 32);
        _chips.SetBounds(2, 2, Math.Max(50, _otherGames.Left - 8), 40);
        _modButtons.SetBounds(2, h - 44, 230, 42);
        _modActions.SetBounds(_modButtons.Right, h - 44, Math.Max(10, w - _modButtons.Right - 2), 42);
        _modDetail.SetBounds(6, h - 80, w - 12, 34);
        _modList.SetBounds(2, 46, w - 4, h - 46 - 84);
    }

    void ShowPage(int n)
    {
        void Swap()
        {
            for (int i = 0; i < _pages.Length; i++)
            {
                _pages[i].Visible = i == n;
                _tabs[i].Checked = i == n;
            }
        }
        // the page appears whole, not control by control
        if (_body.IsHandleCreated) Batch(_body, Swap);
        else Swap();
    }

    // --- theme ----------------------------------------------------------------------------------

    void CycleTheme()
    {
        _settings.Theme = _settings.Theme switch { "light" => "dark", "dark" => "system", _ => "light" };
        _settings.Save(_launcherDir);
        Theme.Set(Theme.FromSetting(_settings.Theme));
        ApplyTheme();
    }

    void ApplyTheme()
    {
        var p = Theme.Current;
        _themeButton.Text = _settings.Theme switch { "light" => "Theme: Light", "dark" => "Theme: Dark", _ => "Theme: Auto" };
        _themeButton.AutoSizeToText();
        BackColor = p.SkyBottom;
        Themed.Apply(this);
        _name.BackColor = p.Panel;
        _name.ForeColor = p.Text;
        _log.BackColor = p.Input;
        _log.ForeColor = p.Text;
        _log.Font = new Font(FontFamily.GenericMonospace, 9f);
        _search.ApplyTheme();
        LayoutWindow();
        Invalidate(true);
    }

    // --- games ----------------------------------------------------------------------------------

    async Task RefreshGamesAsync()
    {
        string? selected = _game?.Dir;
        _games = GameFolder.Scan(_gamesDir);
        // a new or newer APK: take its libraries (and icon) out now, so the menu can show them
        foreach (var g in _games.Where(g => g.NeedsApkUnpack).ToList())
            await _flow.PrepareAsync(g);

        _iconCache.Clear();
        _gameList.SetItems(_games);
        if (_games.Count == 0)
        {
            Log($"No games yet. Press \"Add game\", or make a folder for each game in {_gamesDir} and put its .apk and .obb there.");
            _game = null;
            _header.Game = null;
            SetMenuEnabled(false);
            return;
        }
        int pick = Math.Max(_games.FindIndex(g => g.Dir == selected), 0);
        _game = null;
        _gameList.SelectedIndex = pick;
    }

    void SetMenuEnabled(bool on)
    {
        _body.Enabled = on;
        _play.Enabled = on;
        _name.Enabled = on;
        foreach (var t in _tabs) t.Enabled = on;
        _removeGame.Enabled = on;
    }

    Image? IconOf(GameFolder g)
    {
        if (_iconCache.TryGetValue(g.Dir, out var img)) return img;
        return _iconCache[g.Dir] = g.HasIcon ? LoadImage(g.IconPath) : null;
    }

    static Color FamilyColor(Palette p, string family) => family switch
    {
        "reflourished" => p.Green.Bottom,
        "china" => p.Red.Bottom,
        "pvz2" => p.Yellow.Bottom,
        _ => p.Blue.Bottom,
    };

    void DrawGameCard(Graphics g, object item, RectangleF r, bool selected, bool hover)
    {
        var game = (GameFolder)item;
        bool problem = game.Problem() != null;
        string detail = _running.ContainsKey(game.Dir) ? "playing now"
            : game.Info?.VersionName is { Length: > 0 } v ? $"{Mods.FamilyLabel(game.Family)} · {v}"
            : problem ? "needs files" : "";
        Draw.GameCard(g, r, Theme.Current, IconOf(game), game.DisplayName, detail, FamilyColor(Theme.Current, game.Family), selected, hover, problem && !_running.ContainsKey(game.Dir));
    }

    void Select(GameFolder game)
    {
        _game = game;
        SetMenuEnabled(true);
        _loading = true;
        try
        {
            if (Mods.Sync(game)) game.SaveSettings();
            var s = game.Settings;
            _header.Game = game;
            _header.Icon_ = IconOf(game);
            _header.FamilyColor = FamilyColor(Theme.Current, game.Family);
            if (IconOf(game) is Bitmap bmp) Icon = System.Drawing.Icon.FromHandle(new Bitmap(bmp, 32, 32).GetHicon());
            _name.Text = game.DisplayName;
            var info = game.Info;
            string version = info != null ? $"{info.Package}   version {info.VersionName} ({info.VersionCode})" : "";
            _status.Text = game.Problem() is { } problem ? $"{version}\n{problem}".Trim() : $"{version}\n{(game.IsChina ? "PvZ2 China (no OBB)" : Path.GetFileName(game.Obb))}";
            _status.ForeColor = game.Problem() != null ? Theme.Current.Problem : Theme.Current.TextMuted;
            _modLibs.Text = game.ModLibraries.Count > 0
                ? $"This game's own code ({string.Join(", ", game.ModLibraries)}) is part of it and always loads."
                : "";

            _assets.SelectedIndex = s.Assets == "low" ? 1 : 0;
            _resolution.Text = s.Resolution ?? "Auto";
            _window.Text = s.Window;
            _fullscreen.Checked = s.Fullscreen;
            _vsync.Checked = s.VSync;
            _angle.Checked = s.Angle;
            _offline.Checked = s.Offline;
            _obbUpdates.Checked = s.ObbUpdates;
            _obbUpdates.Visible = ObbUpdate.ConfigUrl(game) != null;
            _askContent.Checked = s.AskForCustomContent;

            _coverage.Clear();
            ReloadMods();
            FillChips();
            FillMods();
            _play.Enabled = game.Problem() == null && !_running.ContainsKey(game.Dir);
            _header.Invalidate();
            _gameList.Invalidate();
        }
        finally { _loading = false; }
    }

    void Change(Action<GameSettings> edit)
    {
        if (_loading || _game == null) return;
        edit(_game.Settings);
        _game.SaveSettings();
    }

    // --- mods -----------------------------------------------------------------------------------

    List<Mods.Mod> AllMods() => _allMods;

    /// Reads the game's mods from disk again (after adding, restoring or saving one).
    void ReloadMods()
    {
        _allMods = _game == null ? [] : Mods.List(_game);
        _detailFor = null;
        RecomputeActive();
    }

    /// Which mods take part in a run, after a switch or the offline setting changed.
    void RecomputeActive()
    {
        _activeKeys = _game == null ? new(StringComparer.OrdinalIgnoreCase)
            : Mods.Active(_game, _allMods).Select(m => m.Key).ToHashSet(StringComparer.OrdinalIgnoreCase);
        _header.ModsOn = _activeKeys.Count;
        _header.Invalidate();
    }

    void FillChips()
    {
        _chips.SuspendLayout();
        _chips.Controls.Clear();
        var cats = AllMods().Where(m => _otherGames.Checked || _game == null || Mods.AppliesTo(m, _game))
            .Select(m => m.Category).Distinct(StringComparer.OrdinalIgnoreCase).Order(StringComparer.OrdinalIgnoreCase).ToList();
        if (_category != "All" && !cats.Contains(_category, StringComparer.OrdinalIgnoreCase)) _category = "All";
        foreach (var c in new[] { "All" }.Concat(cats))
        {
            var chip = new GlossButton(c, p => p.Yellow, 8f) { Toggle = true, Height = 32, Margin = new Padding(0, 2, 3, 2), Checked = c == _category };
            chip.Width -= 10;
            chip.Click += (_, _) => { _category = c; foreach (GlossButton b in _chips.Controls) b.Checked = b.Text == c; FillMods(); };
            _chips.Controls.Add(chip);
        }
        _chips.ResumeLayout();
    }

    void FillMods()
    {
        if (_game == null) return;
        var game = _game;
        string q = _search.Text.Trim();
        var selected = (_modList.SelectedItem as Mods.Mod)?.Key;
        var mods = AllMods()
            .Where(m => _otherGames.Checked || Mods.AppliesTo(m, game))
            .Where(m => _category == "All" || string.Equals(m.Category, _category, StringComparison.OrdinalIgnoreCase))
            .Where(m => q == "" || m.Name.Contains(q, StringComparison.OrdinalIgnoreCase) || m.Description.Contains(q, StringComparison.OrdinalIgnoreCase) ||
                        m.Author.Contains(q, StringComparison.OrdinalIgnoreCase) || m.Category.Contains(q, StringComparison.OrdinalIgnoreCase))
            // the ones for this game first, then by category and name
            .OrderBy(m => Mods.AppliesTo(m, game) ? 0 : 1)
            .ThenBy(m => m.Category, StringComparer.OrdinalIgnoreCase)
            .ThenBy(m => m.Name, StringComparer.OrdinalIgnoreCase)
            .ToList();
        _modList.SetItems(mods, mods.FindIndex(m => m.Key == selected));
    }

    Mods.StringCoverage? CoverageOf(Mods.Mod m)
    {
        if (_game == null || m.Kind != Mods.ModKind.Localization) return null;
        if (!_coverage.TryGetValue(m.Key, out var c)) _coverage[m.Key] = c = Mods.Coverage(_game, m);
        return c;
    }

    void DrawModCard(Graphics g, object item, RectangleF r, bool selected, bool hover)
    {
        if (_game is not { } game) return;
        var m = (Mods.Mod)item;
        bool fits = Mods.AppliesTo(m, game);
        bool on = Mods.IsOn(game, m);
        string? note = null;
        bool problem = false;
        if (m.Problem != null) { note = m.Problem; problem = true; }
        else if (!fits) { note = "Not for this game"; problem = true; }
        else if (on && m.When != null && (m.When == "offline") != game.Settings.Offline) note = m.When == "offline" ? "Only offline" : "Only online";
        else if (on && m.RequiresAny.Count > 0 && !_activeKeys.Contains(m.Key))
            note = "Needs " + string.Join(" or ", m.RequiresAny.Select(id => AllMods().FirstOrDefault(x => x.Id == id)?.Name ?? id));
        else if (CoverageOf(m) is { } c) note = $"{c.Covered:N0} of {c.Total:N0} strings" + (c.Missing.Count > 0 ? $" · {c.Missing.Count:N0} from the game" : "");
        else if (on && m.HasContent) note = $"load order {game.Settings.Mods.Where(k => AllMods().Any(x => x.Key == k && x.HasContent)).ToList().IndexOf(m.Key) + 1}";
        string kind = m.Source == Mods.ModSource.PatchFile ? "Code patch" : m.Kind switch
        {
            Mods.ModKind.Injection => "Injection",
            Mods.ModKind.Localization => m.Language is { } l ? $"Localization · {l}" : "Localization",
            _ => "Content",
        };
        string byline = string.Join(" · ", new[] { m.Version, m.Author }.Where(s => s != ""));
        if (m.Source == Mods.ModSource.Game) byline = byline == "" ? "this game's own" : byline + " · this game's own";
        Draw.ModCard(g, r, Theme.Current, new Draw.ModCardInfo(m.Name, byline, m.Description == "" ? "(no description)" : m.Description,
            m.Category, kind, m.GamesLabel, on, fits && m.Problem == null, note, problem), selected, hover);
    }

    void ToggleMod(Mods.Mod m)
    {
        if (_game == null) return;
        if (!Mods.AppliesTo(m, _game)) { Log($"{m.Name} is made for {m.GamesLabel}, not this game"); return; }
        if (m.Problem != null) { ShowProblem($"{m.Name} can't be used: {m.Problem}"); return; }
        bool on = !Mods.IsOn(_game, m);
        Mods.Set(_game, m, on);
        if (on && m.Hooks.Contains("keybinds-file")) _game.EnsureKeybindsFile();
        _game.SaveSettings();
        Log($"{m.Name}: {(on ? "on" : "off")}");
        RecomputeActive();
        _modList.Invalidate();
        ShowModDetail();
    }

    void ShowModDetail()
    {
        // the same mod, switched the same way: its buttons stay as they are
        var key = _game != null && _modList.SelectedItem is Mods.Mod sel ? (sel.Key, Mods.IsOn(_game, sel)) : ((string, bool)?)null;
        if (key == _detailFor && key != null) return;
        _detailFor = key;
        _modActions.SuspendLayout();
        var old = _modActions.Controls.Cast<Control>().ToList();
        _modActions.Controls.Clear();
        old.ForEach(o => o.Dispose());
        if (_game == null || _modList.SelectedItem is not Mods.Mod m)
        {
            _modDetail.Text = "Pick a mod to see what it does and what it can do. Its switch turns it on for this game.";
            _modActions.ResumeLayout();
            return;
        }
        var parts = new List<string> { m.Source == Mods.ModSource.PatchFile ? m.Patches[0] : m.Dir };
        if (m.Rfu.Count > 0) parts.Add("rfu " + string.Join(" ", m.Rfu));
        if (m.Kind == Mods.ModKind.Localization && CoverageOf(m) is { } c && c.Missing.Count > 0)
            parts.Add($"{c.Missing.Count:N0} of the game's strings aren't translated yet; the game's own text shows for those");
        _modDetail.Text = string.Join("   ·   ", parts);

        void Action(string text, Func<Palette, Gloss> color, Action run)
        {
            var b = new GlossButton(text, color, 9f) { Height = 36, Margin = new Padding(6, 3, 0, 3) };
            b.Click += (_, _) => run();
            _modActions.Controls.Add(b);
        }
        // right to left
        Action("Open folder", p => p.Wood, () => OpenFolder(m.Source == Mods.ModSource.PatchFile ? _game.PatchesDir : m.Dir));
        if (m.HasContent && Mods.IsOn(_game, m))
        {
            Action("Load later", p => p.Wood, () => MoveMod(m, 1));
            Action("Load earlier", p => p.Wood, () => MoveMod(m, -1));
        }
        if (m.Kind == Mods.ModKind.Localization)
            Action("List missing strings", p => p.Blue, () =>
            {
                if (_game != null && Mods.WriteMissing(_game, m) is { } path) { Log($"untranslated strings written to {path}"); Open(path); }
                else ShowProblem("The game hasn't downloaded its strings for that language yet: play once online, then try again.");
            });
        foreach (var a in m.Actions.Reverse())
            Action(a.Label, p => p.Blue, () => RunModAction(m, a.Run));
        _modActions.ResumeLayout();
    }

    void RunModAction(Mods.Mod m, string run)
    {
        if (_game == null) return;
        switch (run)
        {
            case "edit-keybinds": EditKeybinds(); break;
            case "controller-settings": ControllerSettings(); break;
            case "edit-layout": OpenLayoutEditor(m); break;
            case var open when open.StartsWith("open:", StringComparison.Ordinal):
                string path = Mods.Expand(open[5..], m, _game);
                if (File.Exists(path) || Directory.Exists(path)) Open(path);
                else ShowProblem($"{path} isn't there.");
                break;
            default: ShowProblem($"{m.Name}: the launcher doesn't know the action \"{run}\"."); break;
        }
    }

    /// Moves an enabled content mod in the load order (among the content mods).
    void MoveMod(Mods.Mod m, int by)
    {
        if (_game == null) return;
        var list = _game.Settings.Mods;
        var content = AllMods().Where(x => x.HasContent).Select(x => x.Key).ToHashSet(StringComparer.OrdinalIgnoreCase);
        int i = list.FindIndex(k => string.Equals(k, m.Key, StringComparison.OrdinalIgnoreCase));
        if (i < 0) return;
        int j = i + by;
        while (j >= 0 && j < list.Count && !content.Contains(list[j])) j += by;
        if (j < 0 || j >= list.Count) return;
        (list[i], list[j]) = (list[j], list[i]);
        _game.SaveSettings();
        _modList.Invalidate();
    }

    void RestoreBuiltins()
    {
        if (!Confirm($"Write the launcher's own mods back into {_sharedModsDir}, as they came? Changes you made to them are lost; your other mods stay.")) return;
        try
        {
            BuiltinMods.Restore(_sharedModsDir);
            Log("built-in mods restored");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { ShowProblem($"Couldn't restore them: {e.Message}"); }
        if (_game != null) { Mods.Sync(_game); _game.SaveSettings(); ReloadMods(); FillChips(); FillMods(); }
    }

    // --- actions --------------------------------------------------------------------------------

    async Task PlayAsync() { if (_game != null) await PlayAsync(_game, false); }

    async Task PlayAsync(GameFolder game, bool restart)
    {
        _play.Enabled = false;
        try
        {
            var process = await _flow.RunAsync(game, restart);
            if (_game == game) Select(game);  // the flow may have added content or a new version
            if (process == null) return;
            _running[game.Dir] = process;
            _play.Enabled = false;
            _play.Text = "PLAYING";
            _gameList.Invalidate();
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
        _play.Text = "PLAY";
        _gameList.Invalidate();
        int code = process.ExitCode;
        if (code == LaunchFlow.RestartExitCode)
        {
            // the game asked to be started again (a save imported, its OBB patched, or its own
            // library updated), but not over and over
            var recent = _restarts.TryGetValue(game.Dir, out var r) ? r : _restarts[game.Dir] = new List<DateTime>();
            recent.RemoveAll(t => DateTime.Now - t > TimeSpan.FromMinutes(2));
            if (recent.Count < 3)
            {
                recent.Add(DateTime.Now);
                Log($"{game.DisplayName} asked to be restarted");
                _ = PlayAsync(game, true);
                return;
            }
            Log($"{game.DisplayName} asked to be restarted again, 3 times in 2 minutes: not starting it this time");
            return;
        }
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
        if (ObbFor(apk, "The game's OBB") is not { } obb) return;
        var game = await _flow.AddGameAsync(_gamesDir, Path.GetFileNameWithoutExtension(apk), apk, obb == "" ? null : obb);
        if (game == null) return;
        _game = game;
        await RefreshGamesAsync();
    }

    /// The OBB to go with an APK: "" when there's none to ask for (PvZ2 China has its data in the
    /// APK; Reflourished 1.5.0+ downloads its own), null when the person backs out.
    string? ObbFor(string apk, string title)
    {
        if (GameFolder.ApkIsChina(apk))
        {
            Log("PvZ2 China APK: no OBB needed, its data is in the APK.");
            return "";
        }
        if (ObbUpdate.ConfigUrlInApk(apk) != null)
        {
            Log("this APK downloads its own OBB: the launcher does that for it.");
            return "";
        }
        return PickFile(title, "Game data (*.obb)|*.obb|All files|*.*", Path.GetDirectoryName(apk));
    }

    /// A new version of the selected game: its APK, and its OBB unless the APK fetches its own.
    async Task UpdateGameAsync()
    {
        if (_game is not { } game) return;
        if (_running.ContainsKey(game.Dir)) { ShowProblem($"Close {game.DisplayName} first."); return; }
        if (AskForNewVersion(game) is not { } files) return;
        _play.Enabled = false;
        try
        {
            if (await _flow.InstallVersionAsync(game, files.Apk, files.Obb))
                Log($"{game.DisplayName} is updated");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            ShowProblem($"Couldn't update the game: {e.Message}");
        }
        await RefreshGamesAsync();
    }

    /// Deletes the selected game's folder (to the Recycle Bin), saves and mods included.
    async Task RemoveGameAsync()
    {
        if (_game is not { } game) return;
        if (_running.ContainsKey(game.Dir)) { ShowProblem($"Close {game.DisplayName} first."); return; }
        var remove = new TaskDialogButton("Remove");
        var page = new TaskDialogPage
        {
            Caption = game.DisplayName,
            Heading = $"Remove {game.DisplayName}?",
            Text = $"Its folder ({game.Dir}) goes to the Recycle Bin: the APK, the OBB, your saves, its own mods and settings. " +
                   "Export your save from the game first (Settings > Import/Export Files) if you want to keep it apart. " +
                   "The shared mods folder isn't touched.",
            Icon = TaskDialogIcon.Warning,
            Buttons = { remove, TaskDialogButton.Cancel },
            DefaultButton = TaskDialogButton.Cancel,
        };
        if (TaskDialog.ShowDialog(this, page) != remove) return;
        try
        {
            Microsoft.VisualBasic.FileIO.FileSystem.DeleteDirectory(game.Dir, Microsoft.VisualBasic.FileIO.UIOption.OnlyErrorDialogs,
                Microsoft.VisualBasic.FileIO.RecycleOption.SendToRecycleBin);
            Log($"removed {game.DisplayName} ({game.Dir} is in the Recycle Bin)");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or OperationCanceledException)
        {
            ShowProblem($"Couldn't remove {game.Dir}: {e.Message}");
        }
        _game = null;
        await RefreshGamesAsync();
    }

    void AddContent()
    {
        if (_game == null) return;
        var folder = PickFolder("The mod's folder (custom levels, strings, a mod with a mod.json...)");
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
        if (Mods.Find(_game, name) is { } mod) Mods.Set(_game, mod, true);
        _game.SaveSettings();
        Log($"added the mod \"{name}\" to {_game.ModsDir}");
        ReloadMods();
        FillChips();
        FillMods();
    }

    void EditKeybinds()
    {
        if (_game == null) return;
        _game.EnsureKeybindsFile();
        Open(_game.KeybindsPath);
    }

    void OpenLayoutEditor(Mods.Mod? layoutMod)
    {
        if (_game == null) return;
        using var form = new KeybindLayoutForm(_game, layoutMod, _sharedModsDir);
        form.ShowDialog(this);
        if (form.SavedMod is { } dir)
        {
            Log($"keybind layout saved as a mod: {dir}");
            if (Mods.Find(_game, Path.GetFileName(dir)) is { } m && !Mods.IsOn(_game, m)) { Mods.Set(_game, m, true); _game.SaveSettings(); }
            ReloadMods();
            FillChips();
            FillMods();
        }
        else if (form.SavedKeybinds)
        {
            Log($"keybind layout saved to {_game.KeybindsPath}");
            if (Mods.List(_game).FirstOrDefault(m => m.Id == "keybinds") is { } kb && !Mods.IsOn(_game, kb))
            {
                Mods.Set(_game, kb, true);
                _game.SaveSettings();
                Log("Keybinds switched on, so the layout is used");
            }
            RecomputeActive();
            _modList.Invalidate();
        }
    }

    void ControllerSettings()
    {
        if (_game == null) return;
        string? rfu = Tools.Find(_launcherDir, "rfu");
        using var form = new ControllerForm(_game, rfu == null ? null : Path.GetDirectoryName(rfu));
        form.ShowDialog(this);
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
        _statusBar.Text = $"{DateTime.Now:HH:mm:ss}  {line}";
    }

    public string? AskForCustomContent(GameFolder game)
    {
        var choose = new TaskDialogButton("Choose a folder...");
        var none = new TaskDialogButton("Play without");
        var page = new TaskDialogPage
        {
            Caption = game.DisplayName,
            Heading = "Load custom content?",
            Text = "Custom levels, strings or other content to lay over the game's own. It's kept in the game's mods folder for next time.",
            Buttons = { choose, none },
            DefaultButton = none,
            Verification = new TaskDialogVerificationCheckBox("Don't ask again (add mods from the Mods tab)"),
        };
        var answer = TaskDialog.ShowDialog(this, page);
        if (page.Verification.Checked)
        {
            game.Settings.AskForCustomContent = false;
            game.SaveSettings();
        }
        return answer == choose ? PickFolder("The custom content's folder") : null;
    }

    public bool Confirm(string message)
    {
        var page = new TaskDialogPage
        {
            Caption = "PvZ2 Launcher",
            Text = message,
            Icon = TaskDialogIcon.Information,
            Buttons = { TaskDialogButton.Yes, TaskDialogButton.No },
            DefaultButton = TaskDialogButton.Yes,
        };
        return TaskDialog.ShowDialog(this, page) == TaskDialogButton.Yes;
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

    public (string Apk, string? Obb)? AskForNewVersion(GameFolder game)
    {
        var apk = PickFile($"The new {game.DisplayName} APK", "Android app (*.apk)|*.apk|All files|*.*");
        if (apk == null) return null;
        if (ObbFor(apk, $"The new {game.DisplayName} OBB") is not { } obb) return null;
        return (apk, obb == "" ? null : obb);
    }

    public string? AskForObb(GameFolder game) =>
        PickFile($"The OBB couldn't be downloaded: pick {game.Info?.Obb ?? "the game's OBB"}", "Game data (*.obb)|*.obb|All files|*.*");

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

    // --- the window's own pieces ------------------------------------------------------------------

    /// The picked game's banner: its icon, and badges for its kind of game and whether it's ready.
    sealed class HeaderPanel : WoodPanel
    {
        public GameFolder? Game { get; set; }
        public Image? Icon_ { get; set; }
        public Color FamilyColor { get; set; }
        public int ModsOn { get; set; }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var g = e.Graphics;
            var p = Theme.Current;
            var ir = new RectangleF(30, 26, 88, 88);
            if (Game == null)
            {
                using var f = Theme.Title(14);
                Draw.OutlinedText(g, "Add a game to get started", f, new RectangleF(30, 20, Width - 60, Height - 40), Color.White, p.TextOutline, 2.5f, StringAlignment.Near);
                return;
            }
            using (var shadow = Draw.Round(new RectangleF(ir.X + 1, ir.Y + 4, ir.Width, ir.Height), 18))
            using (var sb = new SolidBrush(Color.FromArgb(70, 0, 0, 0)))
                g.FillPath(sb, shadow);
            using (var path = Draw.Round(ir, 18))
            {
                if (Icon_ != null)
                {
                    var old = g.Clip;
                    g.SetClip(path, System.Drawing.Drawing2D.CombineMode.Intersect);
                    g.DrawImage(Icon_, ir);
                    g.Clip = old;
                }
                else
                {
                    using var b = new System.Drawing.Drawing2D.LinearGradientBrush(ir, p.Green.Top, p.Green.Bottom, System.Drawing.Drawing2D.LinearGradientMode.Vertical);
                    g.FillPath(b, path);
                }
                using var rim = new Pen(p.WoodEdge, 2.5f);
                g.DrawPath(rim, path);
            }
            using var bf = Theme.Text(8f, FontStyle.Bold);
            float x = 128;
            x += Draw.Badge(g, new PointF(x, Height - 44), Mods.FamilyLabel(Game.Family), FamilyColor, bf) + 6;
            bool ready = Game.Problem() == null;
            x += Draw.Badge(g, new PointF(x, Height - 44), ready ? "Ready to play" : "Needs files", ready ? p.Blue.Bottom : p.Problem, bf) + 6;
            int on = ModsOn;
            Draw.Badge(g, new PointF(x, Height - 44), $"{on} mod{(on == 1 ? "" : "s")} on", p.TextMuted, bf, filled: false);
        }
    }

    /// The last line of the log, along the bottom of the window.
    sealed class StatusBar : Control
    {
        public StatusBar()
        {
            SetStyle(Backdrop.Styles, true);
        }
        protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }
        protected override void OnPaintBackground(PaintEventArgs e) => Backdrop.Paint(this, e.Graphics);
        protected override void OnPaint(PaintEventArgs e)
        {
            using (var b = new SolidBrush(Color.FromArgb(150, 0, 0, 0))) e.Graphics.FillRectangle(b, ClientRectangle);
            using var f = Theme.Text(9f);
            TextRenderer.DrawText(e.Graphics, Text, f, new Rectangle(12, 0, Width - 24, Height), Color.White,
                TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis | TextFormatFlags.SingleLine);
        }
    }
}
