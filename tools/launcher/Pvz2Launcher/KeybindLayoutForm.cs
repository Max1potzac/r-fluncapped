using System.Drawing.Drawing2D;
using Pvz2Launcher.Ui;

namespace Pvz2Launcher;

/// The visual keybind editor, Bluestacks style: a screenshot of the game with a marker for each
/// key, dragged to where its button is on screen. Saved as "<key> tap x% y%" lines, into the
/// game's keybinds.txt or as a layout mod for this game (and version), which suits a fan mod whose
/// buttons sit elsewhere.
///
/// In Controller mode the markers are controller buttons, each for one screen (the lawn, the seed
/// chooser, the world map...): on that screen the button taps its spot in place of what it does,
/// and every other screen keeps the button as it was. Saved in the game's controller-layout.txt
/// (or the layout mod's).
///
///   click an empty spot     a new marker there (it asks for the key or button)
///   drag a marker           move it
///   right-click a marker    remove it
///   double-click a marker   change its key or button
sealed class KeybindLayoutForm : BackdropForm
{
    readonly GameFolder _game;
    readonly Mods.Mod? _layoutMod;
    readonly string _sharedModsDir;
    readonly List<KeybindLayout.Marker> _markers;
    readonly Canvas _canvas;
    readonly ThemedLabel _hint = new("", 9.5f, muted: true) { AutoSize = false };
    readonly GlossButton _keysTab = new("Keyboard", p => p.Blue, 9.5f) { Toggle = true, Checked = true, Height = 36, Margin = new Padding(0, 0, 6, 0) };
    readonly GlossButton _padTab = new("Controller", p => p.Blue, 9.5f) { Toggle = true, Height = 36, Margin = new Padding(0, 0, 18, 0) };
    readonly ComboBox _screenBox = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 230, Margin = new Padding(0, 7, 0, 0) };
    readonly ThemedLabel _screenLabel = new("Screen:", 10f) { AutoSize = false, Width = 64, Height = 34, TextAlign = ContentAlignment.MiddleLeft };
    bool _padMode;
    string _padScreen = "any";
    Image? _shot;
    string? _shotPath;
    int _drag = -1;
    bool _changed;

    /// Set when the layout was saved into the game's keybinds.txt (and controller layout).
    public bool SavedKeybinds { get; private set; }
    /// Set when what was saved has controller buttons in it.
    public bool SavedPad { get; private set; }
    /// The layout mod's folder, when it was saved as one.
    public string? SavedMod { get; private set; }

    public KeybindLayoutForm(GameFolder game, Mods.Mod? layoutMod, string sharedModsDir)
    {
        _game = game;
        _layoutMod = layoutMod;
        _sharedModsDir = sharedModsDir;
        if (layoutMod != null)
        {
            var (markers, shot) = KeybindLayout.ReadMod(layoutMod.Dir);
            _markers = markers;
            _shotPath = shot;
        }
        else
        {
            game.EnsureKeybindsFile();
            _markers = [.. KeybindLayout.Read(game.KeybindsPath), .. KeybindLayout.ReadPad(game.PadLayoutPath)];
            string remembered = Path.Combine(game.WorkDir, "layout-screenshot.png");
            if (File.Exists(remembered)) _shotPath = remembered;
        }
        if (_shotPath != null) LoadShot(_shotPath, remember: false);

        Text = layoutMod != null ? $"Keybind layout - {layoutMod.Name}" : $"Keybind layout - {game.DisplayName}";
        Size = new Size(1100, 760);
        MinimumSize = new Size(760, 520);
        StartPosition = FormStartPosition.CenterParent;
        LawnHeight = 40;
        BackColor = Theme.Current.SkyBottom;
        Font = Theme.Text(9.5f);

        _canvas = new Canvas(this) { Dock = DockStyle.Fill };
        var frame = new WoodPanel("Place your keys and buttons") { Dock = DockStyle.Fill, Padding = new Padding(22, 50, 22, 22) };

        // what's placed: keys, or one screen's controller buttons
        var top = new BackdropFlow { Dock = DockStyle.Top, Height = 50, Padding = new Padding(12, 10, 12, 0), WrapContents = false };
        foreach (var (_, label) in PadSetup.LayoutScreens) _screenBox.Items.Add(label);
        _screenBox.SelectedIndex = 0;
        _screenBox.SelectedIndexChanged += (_, _) =>
        {
            _padScreen = PadSetup.LayoutScreens[Math.Max(0, _screenBox.SelectedIndex)].Name;
            _drag = -1;
            UpdateHint();
            _canvas.Invalidate();
        };
        _keysTab.Click += (_, _) => SetMode(false);
        _padTab.Click += (_, _) => SetMode(true);
        top.Controls.AddRange([_keysTab, _padTab, _screenLabel, _screenBox]);
        frame.Controls.Add(_canvas);

        var bar = new BackdropFlow { Dock = DockStyle.Bottom, Height = 56, Padding = new Padding(12, 8, 12, 4), WrapContents = false };
        GlossButton B(string text, Func<Palette, Gloss> c, Action run)
        {
            var b = new GlossButton(text, c, 9.5f) { Height = 40, Margin = new Padding(0, 0, 8, 0) };
            b.Click += (_, _) => run();
            bar.Controls.Add(b);
            return b;
        }
        B("Load screenshot...", p => p.Blue, PickShot);
        if (layoutMod == null)
        {
            B("Save for this game", p => p.Green, SaveKeybinds);
            B("Save as layout mod...", p => p.Yellow, SaveAsMod);
        }
        else B("Save layout", p => p.Green, SaveAsMod);
        B("Clear", p => p.Red, () =>
        {
            if (_markers.Any(Editing) && Ask(_padMode ? "Remove every controller button on this screen?" : "Remove every key?")) { _markers.RemoveAll(Editing); Touch(); }
        });
        _hint.Width = 420;
        _hint.Height = 40;
        bar.Controls.Add(_hint);

        Controls.Add(frame);
        Controls.Add(bar);
        Controls.Add(top);
        Themed.Apply(this);
        SetMode(layoutMod != null && _markers.Count > 0 && _markers.All(m => m.Pad));
        FormClosing += (_, e) =>
        {
            if (_changed && !SavedKeybinds && SavedMod == null && !Ask("Close without saving the layout?")) e.Cancel = true;
        };
    }

    void UpdateHint() => _hint.Text = _shot == null
        ? "Load a screenshot of the game (any size: positions are kept in percent). Then click where a key should press."
        : _padMode
            ? $"{_markers.Count(Editing)} button(s) here. On this screen each one taps its spot in place of what it does; other screens keep it. Click to add, drag, right-click to remove."
            : $"{_markers.Count(Editing)} key(s). Click to add one, drag to move, right-click to remove, double-click to change its key.";

    /// The markers of what's being edited: keys, or the picked screen's controller buttons.
    bool Editing(KeybindLayout.Marker m) => _padMode ? m.Pad && m.State == _padScreen : !m.Pad;

    void SetMode(bool pad)
    {
        _padMode = pad;
        _keysTab.Checked = !pad;
        _padTab.Checked = pad;
        _screenLabel.Visible = _screenBox.Visible = pad;
        _drag = -1;
        UpdateHint();
        _canvas.Invalidate();
    }

    void Touch()
    {
        _changed = true;
        UpdateHint();
        _canvas.Invalidate();
    }

    bool Ask(string q) => MessageBox.Show(this, q, Text, MessageBoxButtons.YesNo, MessageBoxIcon.Question) == DialogResult.Yes;

    void PickShot()
    {
        using var d = new OpenFileDialog { Title = "A screenshot of the game", Filter = "Images (*.png;*.jpg;*.jpeg;*.bmp)|*.png;*.jpg;*.jpeg;*.bmp|All files|*.*" };
        if (d.ShowDialog(this) != DialogResult.OK) return;
        LoadShot(d.FileName, remember: _layoutMod == null);
        UpdateHint();
        _canvas.Invalidate();
    }

    void LoadShot(string path, bool remember)
    {
        try
        {
            using var fs = File.OpenRead(path);
            using var img = Image.FromStream(fs);
            _shot?.Dispose();
            _shot = new Bitmap(img);
            _shotPath = path;
            if (remember)
            {
                // kept with the game, so the editor opens on it next time
                Directory.CreateDirectory(_game.WorkDir);
                string keep = Path.Combine(_game.WorkDir, "layout-screenshot.png");
                if (!string.Equals(Path.GetFullPath(path), Path.GetFullPath(keep), StringComparison.OrdinalIgnoreCase))
                    _shot.Save(keep, System.Drawing.Imaging.ImageFormat.Png);
                _shotPath = keep;
            }
        }
        catch (Exception e) when (e is IOException or ArgumentException or UnauthorizedAccessException)
        {
            MessageBox.Show(this, $"Couldn't open that picture: {e.Message}", Text);
        }
    }

    void SaveKeybinds()
    {
        KeybindLayout.Write(_game.KeybindsPath, _markers);
        KeybindLayout.WritePad(_game.PadLayoutPath, _markers);
        SavedKeybinds = true;
        SavedPad = _markers.Any(m => m.Pad);
        _changed = false;
        Close();
    }

    void SaveAsMod()
    {
        string name = _layoutMod?.Name ?? $"{_game.DisplayName} layout";
        if (_layoutMod == null)
        {
            var asked = Prompt.Text(this, "Save as layout mod", "A name for the layout. It goes into the shared mods folder, for this game only.", name);
            if (asked == null) return;
            name = asked;
        }
        string? version = _game.Info?.VersionName is { Length: > 0 } v ? string.Join('.', v.Split('.').Take(2)) : null;
        string dir = _layoutMod != null ? Path.GetDirectoryName(_layoutMod.Dir)! : _sharedModsDir;
        SavedMod = KeybindLayout.SaveAsMod(dir, name, _game.Info?.Package, version, _markers, _shotPath);
        SavedPad = _markers.Any(m => m.Pad);
        _changed = false;
        Close();
    }

    /// The key for a marker: the next key pressed (Escape to back out); in Controller mode, the
    /// button picked.
    string? AskKey(string? current)
    {
        if (_padMode)
        {
            using var p = new PadPrompt(current, PadSetup.LayoutScreens.First(s => s.Name == _padScreen).Label);
            return p.ShowDialog(this) == DialogResult.OK ? p.Button : null;
        }
        using var f = new KeyPrompt(current);
        return f.ShowDialog(this) == DialogResult.OK ? f.Key : null;
    }

    /// A controller button's name on its marker, and the colours of its face (Xbox's).
    internal static string PadText(string button) => button switch
    {
        "start" => "Start",
        "back" => "Back",
        _ => button.ToUpperInvariant(),
    };
    internal static Gloss PadColor(Palette p, string button) => button switch
    {
        "a" => p.Green,
        "b" => p.Red,
        "x" => p.Blue,
        "y" => p.Yellow,
        _ => p.Wood,
    };

    /// Where the screenshot sits in the canvas, keeping its shape.
    RectangleF ShotRect(Size canvas)
    {
        float w = _shot?.Width ?? 16, h = _shot?.Height ?? 9;
        float s = Math.Min(canvas.Width / w, canvas.Height / h);
        return new RectangleF((canvas.Width - w * s) / 2, (canvas.Height - h * s) / 2, w * s, h * s);
    }

    int MarkerAt(Point at, Size canvas)
    {
        var r = ShotRect(canvas);
        for (int i = _markers.Count - 1; i >= 0; i--)
        {
            if (!Editing(_markers[i])) continue;
            var c = new PointF(r.X + _markers[i].X * r.Width, r.Y + _markers[i].Y * r.Height);
            if (Math.Abs(at.X - c.X) <= 22 && Math.Abs(at.Y - c.Y) <= 18) return i;
        }
        return -1;
    }

    sealed class Canvas : Control
    {
        readonly KeybindLayoutForm _f;
        public Canvas(KeybindLayoutForm f)
        {
            _f = f;
            SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
            Cursor = Cursors.Cross;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            var g = e.Graphics;
            Draw.Smooth(g);
            var p = Theme.Current;
            using (var bg = new SolidBrush(p.Panel)) g.FillRectangle(bg, ClientRectangle);
            var r = _f.ShotRect(ClientSize);
            if (_f._shot != null) g.DrawImage(_f._shot, r);
            else
            {
                using var hb = new HatchBrush(HatchStyle.LargeCheckerBoard, p.Card, p.CardHover);
                g.FillRectangle(hb, r);
                using var tf = Theme.Title(14);
                Draw.OutlinedText(g, "Load a screenshot of the game", tf, r, Color.White, p.TextOutline, 2.5f);
            }
            using (var edge = new Pen(p.WoodEdge, 2)) g.DrawRectangle(edge, r.X, r.Y, r.Width, r.Height);

            using var kf = Theme.Title(10);
            for (int i = 0; i < _f._markers.Count; i++)
            {
                var m = _f._markers[i];
                if (!_f.Editing(m)) continue;
                var c = new PointF(r.X + m.X * r.Width, r.Y + m.Y * r.Height);
                // a keycap: a glossy rounded square with the key's name, a dot where it touches; a
                // controller button: a round face button, or a pill for the others
                string text = m.Pad ? PadText(m.Key) : m.Key;
                float w = Math.Max(m.Pad ? 30 : 36, g.MeasureString(text, kf).Width + 16);
                var cap = new RectangleF(c.X - w / 2, c.Y - 34, w, 30);
                var face = i == _f._drag ? (m.Pad && m.Key == "y" ? p.Blue : p.Yellow) : m.Pad ? PadColor(p, m.Key) : p.Blue;
                Draw.Button(g, cap, face, text, kf, radius: m.Pad ? 15 : -1);
                using (var line = new Pen(Color.FromArgb(200, Color.White), 2)) g.DrawLine(line, c.X, cap.Bottom - 2, c.X, c.Y - 4);
                using (var dot = new SolidBrush(Color.FromArgb(230, 255, 255, 255))) g.FillEllipse(dot, c.X - 6, c.Y - 6, 12, 12);
                using (var rim = new Pen(face.Edge, 2)) g.DrawEllipse(rim, c.X - 6, c.Y - 6, 12, 12);
            }
        }

        PointF Fraction(Point at)
        {
            var r = _f.ShotRect(ClientSize);
            return new PointF(Math.Clamp((at.X - r.X) / r.Width, 0, 1), Math.Clamp((at.Y - r.Y) / r.Height, 0, 1));
        }

        int Hit(Point at)
        {
            // the marker's dot, or its keycap above it
            int i = _f.MarkerAt(at, ClientSize);
            return i >= 0 ? i : _f.MarkerAt(new Point(at.X, at.Y + 20), ClientSize);
        }

        protected override void OnMouseDown(MouseEventArgs e)
        {
            base.OnMouseDown(e);
            if (_f._shot == null) { _f.PickShot(); return; }
            int i = Hit(e.Location);
            if (e.Button == MouseButtons.Right)
            {
                if (i >= 0) { _f._markers.RemoveAt(i); _f.Touch(); }
                return;
            }
            if (e.Button != MouseButtons.Left) return;
            if (e.Clicks == 2 && i >= 0)
            {
                if (_f.AskKey(_f._markers[i].Key) is { } key)
                {
                    var old = _f._markers[i];
                    _f._markers.RemoveAll(m => m != old && _f.Editing(m) && string.Equals(m.Key, key, StringComparison.OrdinalIgnoreCase));
                    _f._markers[_f._markers.IndexOf(old)] = old with { Key = key };
                    _f.Touch();
                }
                return;
            }
            if (i >= 0) { _f._drag = i; Capture = true; Invalidate(); return; }
            var at = Fraction(e.Location);
            if (_f.AskKey(null) is { } k)
            {
                // one spot per key (per button and screen)
                _f._markers.RemoveAll(m => _f.Editing(m) && string.Equals(m.Key, k, StringComparison.OrdinalIgnoreCase));
                _f._markers.Add(_f._padMode ? new KeybindLayout.Marker(k, at.X, at.Y, _f._padScreen, Pad: true) : new KeybindLayout.Marker(k, at.X, at.Y));
                _f.Touch();
            }
        }

        protected override void OnMouseMove(MouseEventArgs e)
        {
            base.OnMouseMove(e);
            if (_f._drag < 0) { Cursor = Hit(e.Location) >= 0 ? Cursors.SizeAll : Cursors.Cross; return; }
            var at = Fraction(e.Location);
            _f._markers[_f._drag] = _f._markers[_f._drag] with { X = at.X, Y = at.Y };
            _f._changed = true;
            Invalidate();
        }

        protected override void OnMouseUp(MouseEventArgs e)
        {
            base.OnMouseUp(e);
            if (_f._drag < 0) return;
            _f._drag = -1;
            Capture = false;
            _f.Touch();
        }
    }

    /// "Press a key": the next key pressed, in SDL's spelling (the one keybinds.txt uses).
    sealed class KeyPrompt : Form
    {
        public string? Key { get; private set; }
        public KeyPrompt(string? current)
        {
            Text = "Press a key";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(380, 120);
            KeyPreview = true;
            BackColor = Theme.Current.Panel;
            var l = new ThemedLabel(current == null ? "Press the key for this spot (Escape to cancel)." : $"Press the new key for {current} (Escape to cancel).", 10f)
            { AutoSize = false, Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleCenter };
            Controls.Add(l);
        }

        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            var k = keyData & Keys.KeyCode;
            if (k == Keys.Escape) { DialogResult = DialogResult.Cancel; return true; }
            if (SdlName(k) is { } name) { Key = name; DialogResult = DialogResult.OK; return true; }
            return base.ProcessCmdKey(ref msg, keyData);
        }

        protected override void OnKeyDown(KeyEventArgs e)
        {
            if (SdlName(e.KeyCode) is { } name) { Key = name; DialogResult = DialogResult.OK; }
            base.OnKeyDown(e);
        }
    }

    /// "Pick a button": the controller's buttons, one to click.
    sealed class PadPrompt : Form
    {
        public string? Button { get; private set; }
        public PadPrompt(string? current, string screen)
        {
            Text = "Pick a button";
            FormBorderStyle = FormBorderStyle.FixedDialog;
            MaximizeBox = MinimizeBox = false;
            StartPosition = FormStartPosition.CenterParent;
            ClientSize = new Size(420, 236);
            BackColor = Theme.Current.Panel;
            var l = new ThemedLabel(current == null ? $"The button that taps this spot ({screen}):" : $"In place of {PadText(current)} ({screen}):", 10f)
            { AutoSize = false, Bounds = new Rectangle(14, 10, 392, 26), TextAlign = ContentAlignment.MiddleLeft };
            Controls.Add(l);
            for (int i = 0; i < PadSetup.Buttons.Length; i++)
            {
                string name = PadSetup.Buttons[i].Name;
                var b = new GlossButton(PadText(name), p => PadColor(p, name), 9.5f) { Bounds = new Rectangle(14 + i % 4 * 100, 44 + i / 4 * 46, 92, 38) };
                b.Click += (_, _) => { Button = name; DialogResult = DialogResult.OK; };
                Controls.Add(b);
            }
            var cancel = new GlossButton("Cancel", p => p.Wood, 9.5f) { Bounds = new Rectangle(314, 186, 92, 38) };
            cancel.Click += (_, _) => DialogResult = DialogResult.Cancel;
            Controls.Add(cancel);
            Themed.Apply(this);
        }

        protected override bool ProcessCmdKey(ref Message msg, Keys keyData)
        {
            if ((keyData & Keys.KeyCode) == Keys.Escape) { DialogResult = DialogResult.Cancel; return true; }
            return base.ProcessCmdKey(ref msg, keyData);
        }
    }

    /// SDL's name for a Windows key (what rfu's keybinds files use), or null for a key SDL names
    /// differently that isn't worth guessing.
    internal static string? SdlName(Keys k) => k switch
    {
        >= Keys.A and <= Keys.Z => ((char)('A' + (k - Keys.A))).ToString(),
        >= Keys.D0 and <= Keys.D9 => ((char)('0' + (k - Keys.D0))).ToString(),
        >= Keys.NumPad0 and <= Keys.NumPad9 => $"Keypad_{k - Keys.NumPad0}",
        >= Keys.F1 and <= Keys.F24 => $"F{k - Keys.F1 + 1}",
        Keys.Space => "Space",
        Keys.Enter => "Return",
        Keys.Tab => "Tab",
        Keys.Back => "Backspace",
        Keys.Left => "Left",
        Keys.Right => "Right",
        Keys.Up => "Up",
        Keys.Down => "Down",
        Keys.LShiftKey or Keys.ShiftKey => "Left_Shift",
        Keys.RShiftKey => "Right_Shift",
        Keys.LControlKey or Keys.ControlKey => "Left_Ctrl",
        Keys.RControlKey => "Right_Ctrl",
        Keys.Insert => "Insert",
        Keys.Delete => "Delete",
        Keys.Home => "Home",
        Keys.End => "End",
        Keys.PageUp => "PageUp",
        Keys.PageDown => "PageDown",
        Keys.Oemcomma => ",",
        Keys.OemPeriod => ".",
        Keys.OemMinus => "-",
        Keys.Oemplus => "=",
        Keys.OemSemicolon => ";",
        Keys.OemQuotes => "'",
        Keys.OemOpenBrackets => "[",
        Keys.OemCloseBrackets => "]",
        Keys.OemQuestion => "/",
        Keys.OemPipe => "\\",
        Keys.Oemtilde => "`",
        _ => null,
    };
}

/// A one-line question.
static class Prompt
{
    public static string? Text(IWin32Window owner, string title, string question, string value)
    {
        using var f = new Form
        {
            Text = title, FormBorderStyle = FormBorderStyle.FixedDialog, MaximizeBox = false, MinimizeBox = false,
            StartPosition = FormStartPosition.CenterParent, ClientSize = new Size(440, 150), BackColor = Theme.Current.Panel,
        };
        var label = new ThemedLabel(question, 10f) { AutoSize = false, Bounds = new Rectangle(14, 12, 412, 40) };
        var box = new TextBox { Text = value, Bounds = new Rectangle(14, 56, 412, 26) };
        var ok = new GlossButton("Save", p => p.Green, 9.5f) { Bounds = new Rectangle(226, 96, 96, 40) };
        var cancel = new GlossButton("Cancel", p => p.Wood, 9.5f) { Bounds = new Rectangle(330, 96, 96, 40) };
        ok.Click += (_, _) => { f.DialogResult = DialogResult.OK; };
        cancel.Click += (_, _) => { f.DialogResult = DialogResult.Cancel; };
        f.Controls.AddRange([label, box, ok, cancel]);
        f.AcceptButton = null;
        box.KeyDown += (_, e) => { if (e.KeyCode == Keys.Enter) f.DialogResult = DialogResult.OK; };
        Themed.Apply(f);
        return f.ShowDialog(owner) == DialogResult.OK && box.Text.Trim() is { Length: > 0 } t ? t : null;
    }
}
