using System.Text.Json;
using System.Text.Json.Nodes;
using Pvz2Launcher;

namespace Pvz2Builder;

/// The Builder's window: a game's catalogue on the left, the picked object's fields, its
/// properties, a preview and its pictures on the right; edits saved as a launcher mod.
sealed class MainForm : Form
{
    readonly TreeView _tree = new() { Dock = DockStyle.Fill, HideSelection = false };
    readonly TextBox _search = new() { Dock = DockStyle.Top, PlaceholderText = "Search" };
    readonly ListView _list = new() { Dock = DockStyle.Fill, View = View.Details, FullRowSelect = true, HideSelection = false, MultiSelect = false };
    readonly TabControl _tabs = new() { Dock = DockStyle.Fill };
    readonly FieldEditor _fields = new() { Dock = DockStyle.Fill };
    readonly FieldEditor _props = new() { Dock = DockStyle.Fill };
    readonly Label _noProps = new() { Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleCenter, Text = "This object has no Properties sheet of its own." };
    readonly PreviewPanel _preview = new() { Dock = DockStyle.Fill };
    readonly PicturesPanel _pictures = new() { Dock = DockStyle.Fill };
    readonly TextBox _json = new() { Dock = DockStyle.Fill, Multiline = true, ScrollBars = ScrollBars.Both, WordWrap = false, Font = new Font(FontFamily.GenericMonospace, 9f) };
    readonly Label _gameLabel = new() { Dock = DockStyle.Top, Height = 26, Padding = new Padding(6, 6, 6, 0) };
    readonly ToolStripStatusLabel _status = new() { Spring = true, TextAlign = ContentAlignment.MiddleLeft };
    readonly ToolStripStatusLabel _changes = new();
    readonly ToolStripMenuItem _launcherGames = new("Open a launcher game");
    readonly TabPage _propsPage = new("Properties");

    Game? _game;
    GameProfile? _profile;
    Schema? _schema;
    Resources? _resources;
    EditSession? _session;
    string? _savedModDir;

    // what the list shows: entries of a kind, or a sheet's objects
    sealed record Row(string Sheet, int Index, CatalogEntry? Entry, string[] Columns);
    List<Row> _rows = [];
    Row? _current;

    public MainForm(string? open)
    {
        Text = "PvZ2 Builder";
        Width = 1400;
        Height = 860;
        StartPosition = FormStartPosition.CenterScreen;
        Font = SystemFonts.MessageBoxFont ?? Font;

        var menu = new MenuStrip();
        var file = new ToolStripMenuItem("&File");
        file.DropDownItems.Add(_launcherGames);
        file.DropDownItems.Add("Open game &folder...", null, (_, _) => OpenFolderDialog());
        file.DropDownItems.Add("Open .&obb...", null, (_, _) => OpenObbDialog());
        file.DropDownItems.Add(new ToolStripSeparator());
        file.DropDownItems.Add("Open &mod to keep editing...", null, (_, _) => OpenModDialog());
        file.DropDownItems.Add(new ToolStripMenuItem("&Save as mod...", null, (_, _) => SaveMod(), Keys.Control | Keys.S));
        file.DropDownItems.Add(new ToolStripSeparator());
        file.DropDownItems.Add("Load class &dump...", null, (_, _) => LoadDumpDialog());
        file.DropDownItems.Add(new ToolStripSeparator());
        file.DropDownItems.Add("E&xit", null, (_, _) => Close());
        var edit = new ToolStripMenuItem("&Edit");
        edit.DropDownItems.Add("&Revert this object", null, (_, _) => RevertCurrent());
        edit.DropDownItems.Add("Revert &all changes", null, (_, _) => RevertAll());
        edit.DropDownItems.Add(new ToolStripSeparator());
        edit.DropDownItems.Add(new ToolStripMenuItem("&Find", null, (_, _) => _search.Focus(), Keys.Control | Keys.F));
        menu.Items.Add(file);
        menu.Items.Add(edit);
        MainMenuStrip = menu;

        _list.Columns.Add("Name", 220);
        _list.Columns.Add("Alias", 180);
        _list.Columns.Add("Class", 170);
        _list.Columns.Add("", 24);

        _tabs.TabPages.Add(Page("Fields", _fields));
        _propsPage.Controls.Add(_props);
        _propsPage.Controls.Add(_noProps);
        _tabs.TabPages.Add(_propsPage);
        _tabs.TabPages.Add(Page("Preview", _preview));
        _tabs.TabPages.Add(Page("Pictures", _pictures));
        var jsonPage = Page("JSON", _json);
        var jsonBar = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 34, Padding = new Padding(4) };
        var apply = new Button { Text = "Apply", AutoSize = true };
        var reload = new Button { Text = "Undo typing", AutoSize = true };
        apply.Click += (_, _) => ApplyJson();
        reload.Click += (_, _) => ShowJson();
        jsonBar.Controls.AddRange([apply, reload, new Label { Text = "The whole object as JSON: any change here, then Apply.", AutoSize = true, Padding = new Padding(6, 6, 0, 0) }]);
        jsonPage.Controls.Add(jsonBar);
        _tabs.TabPages.Add(jsonPage);
        _tabs.SelectedIndexChanged += (_, _) => ShowTab();

        // splitter positions are set once the window has its size (setting them earlier throws)
        var left = new SplitContainer { Dock = DockStyle.Fill, FixedPanel = FixedPanel.Panel1 };
        left.Panel1.Controls.Add(_tree);
        left.Panel2.Controls.Add(_list);
        left.Panel2.Controls.Add(_search);
        var main = new SplitContainer { Dock = DockStyle.Fill };
        Load += (_, _) =>
        {
            main.SplitterDistance = Math.Max(300, main.Width * 55 / 100);
            left.SplitterDistance = Math.Min(240, left.Width / 3);
        };
        main.Panel1.Controls.Add(left);
        main.Panel2.Controls.Add(_tabs);

        var status = new StatusStrip();
        status.Items.Add(_status);
        status.Items.Add(_changes);

        Controls.Add(main);
        Controls.Add(_gameLabel);
        Controls.Add(menu);
        Controls.Add(status);

        _tree.AfterSelect += (_, e) => FillList();
        _search.TextChanged += (_, _) => FillList(keepSearch: true);
        _list.SelectedIndexChanged += (_, _) => { if (_list.SelectedIndices.Count > 0) Select(_rows[(int)_list.SelectedItems[0].Tag!]); };
        _fields.Changed += OnChanged;
        _props.Changed += OnChanged;
        _fields.Navigate += GoTo;
        _props.Navigate += GoTo;
        FormClosing += (_, e) => { if (!ConfirmDiscard()) e.Cancel = true; };

        FillLauncherGames();
        SetStatus("Open a game: File > Open a launcher game, a game folder, or an .obb.");
        Shown += (_, _) => { if (open != null) OpenGame(open); };
        if (Icon == null) try { Icon = Icon.ExtractAssociatedIcon(Application.ExecutablePath); } catch (ArgumentException) { }
    }

    static TabPage Page(string text, Control c)
    {
        var p = new TabPage(text);
        p.Controls.Add(c);
        return p;
    }

    void SetStatus(string text) => _status.Text = text;

    // ---- opening a game ----

    /// The launcher's games folder: beside the Builder, or one up (the Builder in its own folder).
    static string? GamesDir()
    {
        foreach (var dir in new[] { AppContext.BaseDirectory, Path.GetDirectoryName(AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar)) })
            if (dir != null && Directory.Exists(Path.Combine(dir, "games"))) return Path.Combine(dir, "games");
        return null;
    }

    void FillLauncherGames()
    {
        _launcherGames.DropDownItems.Clear();
        var dir = GamesDir();
        var games = dir == null ? [] : GameFolder.Scan(dir).Where(g => !g.IsChina && g.Obb != null).ToList();
        foreach (var g in games)
        {
            string path = g.Dir;
            _launcherGames.DropDownItems.Add($"{g.DisplayName}  ({g.Info?.VersionName ?? "?"})", null, (_, _) => OpenGame(path));
        }
        if (games.Count == 0) _launcherGames.DropDownItems.Add(new ToolStripMenuItem(dir == null ? "(put the Builder beside PvZ2Launcher.exe to list its games)" : "(the launcher has no games with an OBB)") { Enabled = false });
    }

    void OpenFolderDialog()
    {
        using var dlg = new FolderBrowserDialog { Description = "A game's folder in the launcher's games folder (with its .apk and .obb)", UseDescriptionForTitle = true };
        if (GamesDir() is { } g) dlg.InitialDirectory = g;
        if (dlg.ShowDialog(this) == DialogResult.OK) OpenGame(dlg.SelectedPath);
    }

    void OpenObbDialog()
    {
        using var dlg = new OpenFileDialog { Filter = "Game data (*.obb)|*.obb|All files|*.*", Title = "Open a game's .obb" };
        if (dlg.ShowDialog(this) == DialogResult.OK) OpenGame(dlg.FileName);
    }

    async void OpenGame(string path)
    {
        if (!ConfirmDiscard()) return;
        SetStatus($"Opening {path} ...");
        UseWaitCursor = true;
        Enabled = false;
        try
        {
            var (game, profile, schema, resources, dumpNote) = await Task.Run(() =>
            {
                var g = Game.Open(path);
                try
                {
                    var p = GameProfile.Build(g);
                    var s = Schema.Infer(g);
                    string? note = null;
                    if (DumpPath(g) is { } dump && File.Exists(dump))
                        try { s.ApplyDump((JsonObject)JsonNode.Parse(File.ReadAllText(dump))!); }
                        catch (Exception e) when (e is JsonException or FormatException or IOException) { note = $"the saved class dump couldn't be read: {e.Message}"; }
                    return (g, p, s, Resources.Load(g), note);
                }
                catch { g.Dispose(); throw; }
            });
            _game?.Dispose();
            _game = game;
            _profile = profile;
            _schema = schema;
            _resources = resources;
            _session = new EditSession(game);
            _savedModDir = null;
            _current = null;
            FillTree();
            UpdateGameLabel();
            UpdateChanges();
            SetStatus(dumpNote ?? $"Opened {game.Name}.");
        }
        catch (Exception e) when (e is IOException or FormatException or NotSupportedException or UnauthorizedAccessException or InvalidDataException)
        {
            MessageBox.Show(this, $"Couldn't open {path}:\n\n{e.Message}", "PvZ2 Builder", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            SetStatus("");
        }
        finally
        {
            Enabled = true;
            UseWaitCursor = false;
        }
    }

    /// Where a game's class dump is kept once loaded: in its launcher folder, for next time.
    static string? DumpPath(Game g) => g.Folder == null ? null : Path.Combine(g.Folder.Dir, "builder", "class-dump.json");

    void UpdateGameLabel()
    {
        if (_game == null || _profile == null || _schema == null) { _gameLabel.Text = ""; return; }
        string libs = _game.ModLibraries.Count > 0 ? $", its own code: {string.Join(", ", _game.ModLibraries)}" : "";
        _gameLabel.Text = $"{_game.Name}   ·   {_game.Family}{(_game.Version.Length > 0 ? " " + _game.Version : "")}{libs}   ·   " +
                          $"{_schema.Classes.Count} classes from {_schema.ObjectCount} objects in {_schema.Sheets} sheets" +
                          (_schema.Dump != null ? " + class dump" : "") + (_game.CdnDir != null ? "   ·   CDN content read over the OBB's" : "");
    }

    void FillTree()
    {
        _tree.BeginUpdate();
        _tree.Nodes.Clear();
        if (_profile != null && _game != null)
        {
            foreach (var kind in GameProfile.Kinds)
                _tree.Nodes.Add(new TreeNode($"{kind} ({_profile[kind].Count})") { Tag = kind });
            var sheets = new TreeNode("All sheets") { Tag = "" };
            foreach (var s in _game.SheetNames())
                sheets.Nodes.Add(new TreeNode(s + (_game.SourceOf(s) == "cdn" ? "  (CDN)" : "")) { Tag = "sheet:" + s });
            _tree.Nodes.Add(sheets);
            _tree.SelectedNode = _tree.Nodes[0];
        }
        _tree.EndUpdate();
    }

    // ---- the list ----

    void FillList(bool keepSearch = false)
    {
        if (_profile == null || _game == null || _session == null) return;
        if (!keepSearch && _search.Text.Length > 0) { _search.Text = ""; return; }  // TextChanged fills it
        _rows = [];
        string tag = _tree.SelectedNode?.Tag as string ?? "";
        if (tag.StartsWith("sheet:", StringComparison.Ordinal))
        {
            string sheet = tag[6..];
            if (_game.Sheet(sheet)?["objects"] is JsonArray objects)
                for (int i = 0; i < objects.Count; i++)
                    if (objects[i] is JsonObject o && o["objclass"] != null)
                    {
                        string alias = GameProfile.FirstAlias(o) ?? GameProfile.Str(o["objdata"]?["TypeName"]) ?? $"#{i}";
                        _rows.Add(new Row(sheet.ToUpperInvariant(), i, null, [alias, alias, GameProfile.Str(o["objclass"]) ?? ""]));
                    }
        }
        else if (tag.Length > 0)
            foreach (var e in _profile[tag])
                _rows.Add(new Row(e.Sheet, e.Index, e, [e.DisplayName ?? e.Alias, e.Alias, e.Class]));

        string q = _search.Text.Trim();
        _list.BeginUpdate();
        _list.Items.Clear();
        for (int i = 0; i < _rows.Count; i++)
        {
            var r = _rows[i];
            if (q.Length > 0 && !r.Columns.Any(c => c.Contains(q, StringComparison.OrdinalIgnoreCase))) continue;
            var item = new ListViewItem([.. r.Columns, Mark(r)]) { Tag = i };
            _list.Items.Add(item);
        }
        _list.EndUpdate();
        SetStatus($"{_list.Items.Count} shown");
    }

    string Mark(Row r) => _session != null && (_session.IsChanged(r.Sheet, r.Index) || PropsChanged(r)) ? "*" : "";

    bool PropsChanged(Row r) =>
        r.Entry?.Properties is { } p && _profile?.Resolve(p, r.Sheet) is { } t && _session!.IsChanged(t.Sheet, t.Index);

    void RefreshMarks(bool all = false)
    {
        foreach (var item in all ? _list.Items.Cast<ListViewItem>() : _list.SelectedItems.Cast<ListViewItem>())
        {
            var r = _rows[(int)item.Tag!];
            item.SubItems[3].Text = Mark(r);
            item.Font = item.SubItems[3].Text.Length > 0 ? new Font(_list.Font, FontStyle.Bold) : _list.Font;
        }
    }

    // ---- the object ----

    void Select(Row row)
    {
        _current = row;
        ShowTab(all: true);
    }

    void ShowTab(bool all = false)
    {
        if (_session == null || _profile == null || _schema == null || _resources == null || _current == null) return;
        var row = _current;
        var page = _tabs.SelectedTab?.Text;
        if (all || page == "Fields") _fields.Show(_session, _profile, _schema, row.Sheet, row.Index);
        if (all || page == "Properties")
        {
            var props = PropertiesOf(row);
            _props.Visible = props != null;
            _noProps.Visible = props == null;
            if (props is { } t) _props.Show(_session, _profile, _schema, t.Sheet, t.Index);
        }
        if (page == "Preview") _preview.Show(_resources, row.Entry ?? EntryFor(row));
        if (page == "Pictures") _pictures.Show(_resources, row.Entry ?? EntryFor(row));
        if (page == "JSON") ShowJson();
    }

    (string Sheet, int Index)? PropertiesOf(Row row)
    {
        var obj = _session!.Current(row.Sheet, row.Index);
        string? rtid = GameProfile.Str(obj?["objdata"]?["Properties"]);
        return rtid != null && _profile!.Resolve(rtid, row.Sheet) is { } t ? (t.Sheet, t.Index) : null;
    }

    /// The catalogue entry for an object picked from a sheet, when it's one.
    CatalogEntry? EntryFor(Row row) =>
        GameProfile.Kinds.SelectMany(k => _profile![k]).FirstOrDefault(e => e.Sheet.Equals(row.Sheet, StringComparison.OrdinalIgnoreCase) && e.Index == row.Index);

    void ShowJson()
    {
        if (_session == null || _current == null) { _json.Text = ""; return; }
        var obj = _session.Current(_current.Sheet, _current.Index);
        _json.Text = obj?.ToJsonString(new JsonSerializerOptions { WriteIndented = true, Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping }).Replace("\n", Environment.NewLine) ?? "";
    }

    void ApplyJson()
    {
        if (_session == null || _current == null) return;
        try
        {
            if (JsonNode.Parse(_json.Text) is not JsonObject obj) throw new JsonException("it must be one object: { ... }");
            _session.Set(_current.Sheet, _current.Index, obj);
            OnChanged();
            _fields.Show(_session, _profile!, _schema!, _current.Sheet, _current.Index);
            SetStatus("Applied.");
        }
        catch (JsonException e) { MessageBox.Show(this, "That isn't valid JSON:\n\n" + e.Message, "PvZ2 Builder"); }
    }

    void GoTo(string sheet, int index)
    {
        if (_game == null) return;
        var sheetsNode = _tree.Nodes.Cast<TreeNode>().FirstOrDefault(n => (n.Tag as string) == "");
        var node = sheetsNode?.Nodes.Cast<TreeNode>().FirstOrDefault(n => string.Equals(n.Tag as string, "sheet:" + sheet, StringComparison.OrdinalIgnoreCase));
        if (node == null) return;
        _search.Text = "";
        _tree.SelectedNode = node;
        foreach (ListViewItem item in _list.Items)
            if (_rows[(int)item.Tag!].Index == index)
            {
                item.Selected = true;
                item.EnsureVisible();
                break;
            }
        _tabs.SelectedIndex = 0;
    }

    void OnChanged()
    {
        RefreshMarks();
        UpdateChanges();
    }

    void UpdateChanges()
    {
        int n = _session?.Changed.Count ?? 0;
        _changes.Text = n == 0 ? "No changes" : $"{n} object(s) changed, not saved yet";
        Text = "PvZ2 Builder" + (_game != null ? $" - {_game.Name}" : "") + (n > 0 ? " *" : "");
    }

    void RevertCurrent()
    {
        if (_session == null || _current == null) return;
        _session.Revert(_current.Sheet, _current.Index);
        if (PropertiesOf(_current) is { } p) _session.Revert(p.Sheet, p.Index);
        OnChanged();
        ShowTab(all: true);
    }

    void RevertAll()
    {
        if (_session == null || _session.Changed.Count == 0) return;
        if (MessageBox.Show(this, $"Throw away all {_session.Changed.Count} changes?", "PvZ2 Builder", MessageBoxButtons.YesNo) != DialogResult.Yes) return;
        _session.RevertAll();
        RefreshMarks(all: true);
        UpdateChanges();
        ShowTab(all: true);
    }

    bool ConfirmDiscard()
    {
        if (_session == null || _session.Changed.Count == 0) return true;
        var answer = MessageBox.Show(this, $"{_session.Changed.Count} changed object(s) aren't saved as a mod. Save them first?", "PvZ2 Builder",
            MessageBoxButtons.YesNoCancel, MessageBoxIcon.Question);
        if (answer == DialogResult.Cancel) return false;
        if (answer == DialogResult.Yes) return SaveMod();
        return true;
    }

    // ---- mods ----

    bool SaveMod()
    {
        if (_session == null || _game == null) return false;
        if (_session.Changed.Count == 0) { MessageBox.Show(this, "Nothing has changed yet.", "PvZ2 Builder"); return false; }
        using var dlg = new SaveModDialog(_game, _savedModDir);
        if (dlg.ShowDialog(this) != DialogResult.OK) return false;
        try
        {
            var result = _session.Save(dlg.ModsDir, dlg.Info);
            _savedModDir = result.Dir;
            string notes = result.Notes.Count > 0 ? "\n\nWorth knowing:\n" + string.Join("\n", result.Notes.Select(n => "- " + n)) : "";
            string where = _game.Folder != null && Path.GetFullPath(dlg.ModsDir) == Path.GetFullPath(_game.Folder.ModsDir)
                ? "Switch it on in the launcher's Mods tab for this game."
                : "Copy the folder into the game's mods folder in the launcher (or use + Add mod).";
            MessageBox.Show(this, $"Saved {result.Files.Count} sheet(s) to\n{result.Dir}\n\n{where}{notes}", "PvZ2 Builder");
            SetStatus($"Saved to {result.Dir}");
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            MessageBox.Show(this, "Couldn't save the mod:\n\n" + e.Message, "PvZ2 Builder", MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return false;
        }
    }

    void OpenModDialog()
    {
        if (_session == null || _game == null) { MessageBox.Show(this, "Open the mod's game first.", "PvZ2 Builder"); return; }
        using var dlg = new FolderBrowserDialog { Description = "A mod's folder (with obb\\PACKAGES in it)", UseDescriptionForTitle = true };
        if (_game.Folder != null && Directory.Exists(_game.Folder.ModsDir)) dlg.InitialDirectory = _game.Folder.ModsDir;
        if (dlg.ShowDialog(this) != DialogResult.OK) return;
        var notes = _session.LoadMod(dlg.SelectedPath);
        _savedModDir = dlg.SelectedPath;
        RefreshMarks(all: true);
        UpdateChanges();
        ShowTab(all: true);
        MessageBox.Show(this, $"{_session.Changed.Count} object(s) changed by the mod." + (notes.Count > 0 ? "\n\n" + string.Join("\n", notes.Select(n => "- " + n)) : ""), "PvZ2 Builder");
    }

    void LoadDumpDialog()
    {
        if (_schema == null || _game == null) { MessageBox.Show(this, "Open a game first.", "PvZ2 Builder"); return; }
        using var dlg = new OpenFileDialog { Filter = "Class dump (reflection.json)|*.json", Title = "A class dump made from this game's own libraries" };
        if (dlg.ShowDialog(this) != DialogResult.OK) return;
        try
        {
            var dump = (JsonObject)JsonNode.Parse(File.ReadAllText(dlg.FileName))!;
            int n = _schema.ApplyDump(dump);
            if (DumpPath(_game) is { } keep)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(keep)!);
                File.Copy(dlg.FileName, keep, true);
            }
            UpdateGameLabel();
            ShowTab(all: true);
            SetStatus($"Class dump: {n} classes" + (DumpPath(_game) != null ? ", kept with the game for next time" : ""));
        }
        catch (Exception e) when (e is JsonException or FormatException or InvalidCastException or IOException)
        {
            MessageBox.Show(this, "That isn't a class dump:\n\n" + e.Message, "PvZ2 Builder");
        }
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) _game?.Dispose();
        base.Dispose(disposing);
    }
}

/// Asks for a mod's name, description and author, and where it goes.
sealed class SaveModDialog : Form
{
    readonly TextBox _name = new() { Width = 360 };
    readonly TextBox _description = new() { Width = 360, Multiline = true, Height = 60 };
    readonly TextBox _author = new() { Width = 360 };
    readonly TextBox _version = new() { Width = 100, Text = "1.0" };
    readonly TextBox _dir = new() { Width = 300, ReadOnly = true };

    public string ModsDir => _dir.Text;
    public ModInfo Info => new(_name.Text.Trim(), _description.Text.Trim(), _author.Text.Trim(), _version.Text.Trim());

    public SaveModDialog(Game game, string? existing)
    {
        Text = "Save as mod";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        StartPosition = FormStartPosition.CenterParent;
        MinimizeBox = MaximizeBox = false;
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        Padding = new Padding(10);

        if (existing != null && File.Exists(Path.Combine(existing, "mod.json")))
        {
            try
            {
                var mod = JsonNode.Parse(File.ReadAllText(Path.Combine(existing, "mod.json")), documentOptions: new JsonDocumentOptions { CommentHandling = JsonCommentHandling.Skip, AllowTrailingCommas = true });
                _name.Text = GameProfile.Str(mod?["name"]) ?? Path.GetFileName(existing);
                _description.Text = GameProfile.Str(mod?["description"]) ?? "";
                _author.Text = GameProfile.Str(mod?["author"]) ?? "";
                _version.Text = GameProfile.Str(mod?["version"]) ?? "1.0";
            }
            catch (JsonException) { _name.Text = Path.GetFileName(existing); }
            _dir.Text = Path.GetDirectoryName(existing)!;
        }
        else
        {
            _name.Text = "My mod";
            _dir.Text = game.Folder?.ModsDir ?? "";
        }

        var browse = new Button { Text = "Change...", AutoSize = true };
        browse.Click += (_, _) =>
        {
            using var dlg = new FolderBrowserDialog { Description = "The mods folder the mod goes in", UseDescriptionForTitle = true };
            if (Directory.Exists(_dir.Text)) dlg.InitialDirectory = _dir.Text;
            if (dlg.ShowDialog(this) == DialogResult.OK) _dir.Text = dlg.SelectedPath;
        };
        var ok = new Button { Text = "Save", DialogResult = DialogResult.OK, AutoSize = true };
        var cancel = new Button { Text = "Cancel", DialogResult = DialogResult.Cancel, AutoSize = true };
        AcceptButton = ok;
        CancelButton = cancel;
        ok.Click += (_, e) =>
        {
            if (_name.Text.Trim().Length == 0 || _dir.Text.Length == 0)
            {
                MessageBox.Show(this, "The mod needs a name and a folder to go in.", "PvZ2 Builder");
                DialogResult = DialogResult.None;
            }
        };

        var grid = new TableLayoutPanel { ColumnCount = 2, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink };
        void Row(string label, Control c)
        {
            grid.Controls.Add(new Label { Text = label, AutoSize = true, Padding = new Padding(0, 6, 8, 0) });
            grid.Controls.Add(c);
        }
        Row("Name", _name);
        Row("Description", _description);
        Row("Author", _author);
        Row("Version", _version);
        var dirRow = new FlowLayoutPanel { AutoSize = true, WrapContents = false };
        dirRow.Controls.Add(_dir);
        dirRow.Controls.Add(browse);
        Row("Mods folder", dirRow);
        string target = game.Package.Length > 0 ? game.Package : game.Family;
        grid.Controls.Add(new Label());
        grid.Controls.Add(new Label
        {
            Text = $"For {target}{(game.Version.Length > 0 ? " " + game.Version : "")} only. The mod holds just the changed fields,\nso it keeps working when the game changes the rest.",
            AutoSize = true,
            ForeColor = SystemColors.GrayText,
        });
        var buttons = new FlowLayoutPanel { AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, FlowDirection = FlowDirection.RightToLeft, Anchor = AnchorStyles.Right };
        buttons.Controls.Add(cancel);
        buttons.Controls.Add(ok);
        var outer = new TableLayoutPanel { ColumnCount = 1, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Location = new Point(10, 10) };
        outer.Controls.Add(grid);
        outer.Controls.Add(buttons);
        Controls.Add(outer);
    }
}
