using System.Drawing.Drawing2D;

namespace Pvz2Builder;

/// A plant's almanac page: its name and texts, its stats, and a preview of the page laid out the
/// way the game's almanac shows it (an approximation: the game's own fonts and frame aren't used).
sealed class AlmanacPanel : UserControl
{
    readonly TextBox _name = Box(1), _tooltip = Box(1), _header = Box(2), _description = Box(6), _plantFood = Box(2);
    readonly Label _nameKey = KeyLabel(), _tooltipKey = KeyLabel(), _headerKey = KeyLabel(), _descriptionKey = KeyLabel(), _plantFoodKey = KeyLabel();
    readonly DataGridView _stats = new()
    {
        Dock = DockStyle.Fill, AllowUserToAddRows = false, AllowUserToDeleteRows = false, AllowUserToResizeRows = false, RowHeadersVisible = false,
        SelectionMode = DataGridViewSelectionMode.FullRowSelect, MultiSelect = false, AutoSizeColumnsMode = DataGridViewAutoSizeColumnsMode.Fill,
        BackgroundColor = SystemColors.Window, BorderStyle = BorderStyle.None,
    };
    readonly Label _statsNote = new() { Dock = DockStyle.Top, AutoSize = false, Height = 36, ForeColor = SystemColors.GrayText, Padding = new Padding(0, 4, 0, 0) };
    readonly Button _add = new() { Text = "Add stat", AutoSize = true }, _remove = new() { Text = "Remove", AutoSize = true },
        _up = new() { Text = "Up", AutoSize = true }, _down = new() { Text = "Down", AutoSize = true };
    readonly PagePreview _preview = new() { Dock = DockStyle.Fill };
    readonly Label _empty = new() { Dock = DockStyle.Fill, TextAlign = ContentAlignment.MiddleCenter, Text = "Pick a plant to edit its almanac page." };
    readonly SplitContainer _split = new() { Dock = DockStyle.Fill };

    PlantAlmanac? _page;
    Resources? _resources;
    bool _filling;
    int _pictureVersion;

    public event Action? Changed;

    public AlmanacPanel()
    {
        _stats.Columns.Add(new DataGridViewTextBoxColumn { Name = "Label", HeaderText = "Label", FillWeight = 30 });
        _stats.Columns.Add(new DataGridViewTextBoxColumn { Name = "Value", HeaderText = "Value", FillWeight = 30 });
        _stats.Columns.Add(new DataGridViewTextBoxColumn { Name = "From", HeaderText = "Value comes from", FillWeight = 40, ReadOnly = true });
        _stats.Columns["From"]!.DefaultCellStyle.ForeColor = SystemColors.GrayText;

        var texts = new TableLayoutPanel { Dock = DockStyle.Top, ColumnCount = 2, AutoSize = true, Padding = new Padding(4) };
        texts.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 120));
        texts.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100));
        void Row(string label, TextBox box, Label key)
        {
            var l = new Label { Text = label, AutoSize = true, Padding = new Padding(0, 4, 0, 0) };
            var right = new Panel { Anchor = AnchorStyles.Left | AnchorStyles.Right | AnchorStyles.Top, Height = box.Height + 16, Margin = new Padding(0, 0, 0, 4) };
            box.Dock = DockStyle.Top;
            key.Dock = DockStyle.Bottom;
            right.Controls.Add(box);
            right.Controls.Add(key);
            texts.Controls.Add(l);
            texts.Controls.Add(right);
        }
        Row("Name", _name, _nameKey);
        Row("Tooltip", _tooltip, _tooltipKey);
        Row("Short description", _header, _headerKey);
        Row("Description", _description, _descriptionKey);
        Row("Plant Food", _plantFood, _plantFoodKey);
        var hint = new Label
        {
            Text = "In the description, {KEYWORD}, {STAT} and {FLAVOR} switch the text's style, as in the game's own pages.",
            Dock = DockStyle.Top, Height = 22, ForeColor = SystemColors.GrayText, Padding = new Padding(4, 2, 0, 0),
        };

        var statsBar = new FlowLayoutPanel { Dock = DockStyle.Bottom, Height = 34, Padding = new Padding(0, 2, 0, 0) };
        statsBar.Controls.AddRange([_add, _remove, _up, _down]);
        var statsBox = new GroupBox { Text = "Stats", Dock = DockStyle.Fill, Padding = new Padding(6) };
        statsBox.Controls.Add(_stats);
        statsBox.Controls.Add(_statsNote);
        statsBox.Controls.Add(statsBar);

        var left = new Panel { Dock = DockStyle.Fill };
        left.Controls.Add(statsBox);
        left.Controls.Add(hint);
        left.Controls.Add(texts);
        _split.Panel1.Controls.Add(left);
        _split.Panel2.Controls.Add(_preview);
        _split.Panel2.BackColor = Color.FromArgb(28, 36, 44);
        Controls.Add(_split);
        Controls.Add(_empty);
        _split.Visible = false;
        HandleCreated += (_, _) => BeginInvoke(() => { if (_split.Width > 400) _split.SplitterDistance = _split.Width * 52 / 100; });

        foreach (var (box, key) in new[] { (_name, 0), (_tooltip, 1), (_header, 2), (_description, 3), (_plantFood, 4) })
            box.TextChanged += (_, _) => TextEdited(box, key);
        _stats.CellEndEdit += (_, e) => StatEdited(e.RowIndex, e.ColumnIndex);
        _stats.SelectionChanged += (_, _) => UpdateButtons();
        _add.Click += (_, _) => AddStat();
        _remove.Click += (_, _) => WithStat(i => _page!.RemoveStat(i), -1);
        _up.Click += (_, _) => WithStat(i => _page!.MoveStat(i, -1), -1);
        _down.Click += (_, _) => WithStat(i => _page!.MoveStat(i, 1), 1);
    }

    static TextBox Box(int lines) => new()
    {
        Multiline = lines > 1, Height = lines > 1 ? 8 + lines * 17 : 24, ScrollBars = lines > 2 ? ScrollBars.Vertical : ScrollBars.None, AcceptsReturn = lines > 1,
    };

    static Label KeyLabel() => new() { AutoSize = false, Height = 16, ForeColor = SystemColors.GrayText, Font = new Font(SystemFonts.DefaultFont.FontFamily, 7.5f) };

    public void Show(EditSession session, GameProfile profile, Resources resources, CatalogEntry? entry)
    {
        _resources = resources;
        if (entry == null || entry.Kind != GameProfile.Plants)
        {
            _page = null;
            _split.Visible = false;
            _empty.Visible = true;
            _empty.Text = entry == null ? "Pick a plant to edit its almanac page." : "Almanac pages are edited for plants (zombies come later).";
            return;
        }
        bool same = _page != null && _page.Plant == entry && _page.Session == session;
        _page = PlantAlmanac.For(session, profile, entry);
        _empty.Visible = false;
        _split.Visible = true;
        _filling = true;
        SetBox(_name, _nameKey, _page.NameKey);
        SetBox(_tooltip, _tooltipKey, _page.TooltipKey);
        SetBox(_header, _headerKey, _page.HeaderKey);
        SetBox(_description, _descriptionKey, _page.DescriptionKey);
        SetBox(_plantFood, _plantFoodKey, _page.PlantFoodKey);
        _filling = false;
        FillStats();
        if (!same) LoadPicture(entry);
        _preview.Page = _page;
        _preview.Invalidate();
    }

    void SetBox(TextBox box, Label key, string k)
    {
        box.Text = (_page!.Text(k)).Replace("\n", Environment.NewLine);
        key.Text = k + (_page.Session.IsTextChanged(k) ? "   (changed)" : _page.Session.OriginalText(k) == null ? "   (new: the game has no such string yet)" : "");
    }

    void TextEdited(TextBox box, int which)
    {
        if (_filling || _page == null) return;
        var (key, label) = which switch
        {
            0 => (_page.NameKey, _nameKey), 1 => (_page.TooltipKey, _tooltipKey), 2 => (_page.HeaderKey, _headerKey),
            3 => (_page.DescriptionKey, _descriptionKey), _ => (_page.PlantFoodKey, _plantFoodKey),
        };
        _page.SetText(key, box.Text);
        label.Text = key + (_page.Session.IsTextChanged(key) ? "   (changed)" : "");
        _preview.Invalidate();
        Changed?.Invoke();
    }

    void FillStats(int select = -1)
    {
        _filling = true;
        _stats.Rows.Clear();
        if (_page is { HasStats: true })
        {
            foreach (var s in _page.Stats) _stats.Rows.Add(s.Label, s.Value, s.ValueFrom);
            _statsNote.Text = "Labels and values are what the page shows. Sun cost and recharge are the plant's real numbers: changing them here changes the plant too.";
            _stats.Enabled = true;
        }
        else
        {
            _statsNote.Text = "This game keeps no almanac stats in its data (no PLANTALMANACDATA entry for this plant), so only the texts can be changed.";
            _stats.Enabled = false;
        }
        if (select >= 0 && select < _stats.Rows.Count) { _stats.ClearSelection(); _stats.Rows[select].Selected = true; _stats.CurrentCell = _stats.Rows[select].Cells[0]; }
        _filling = false;
        UpdateButtons();
    }

    void UpdateButtons()
    {
        bool has = _page is { HasStats: true };
        int i = SelectedStat();
        _add.Enabled = has;
        _remove.Enabled = _up.Enabled = _down.Enabled = has && i >= 0;
        if (has && i >= 0) { _up.Enabled = i > 0; _down.Enabled = i < _stats.Rows.Count - 1; }
    }

    int SelectedStat() => _stats.SelectedRows.Count > 0 ? _stats.SelectedRows[0].Index : _stats.CurrentCell?.RowIndex ?? -1;

    void StatEdited(int row, int col)
    {
        if (_filling || _page == null || row < 0) return;
        var stat = _page.Stats.ElementAtOrDefault(row);
        if (stat == null) return;
        string text = _stats.Rows[row].Cells[col].Value?.ToString() ?? "";
        if (col == 0) _page.SetLabel(stat.Index, text);
        else if (col == 1) _page.SetValue(stat.Index, text);
        BeginInvoke(() => FillStats(row));
        _preview.Invalidate();
        Changed?.Invoke();
    }

    void AddStat()
    {
        if (_page is not { HasStats: true }) return;
        int i = _page.AddStat("NEW STAT", "Value");
        FillStats(i);
        _preview.Invalidate();
        Changed?.Invoke();
        _stats.CurrentCell = _stats.Rows[i].Cells[0];
        _stats.BeginEdit(true);
    }

    void WithStat(Action<int> act, int move)
    {
        if (_page == null) return;
        int i = SelectedStat();
        if (i < 0) return;
        act(i);
        FillStats(move < 0 && i > 0 ? i - 1 : move > 0 ? i + 1 : Math.Min(i, _stats.Rows.Count - 2));
        _preview.Invalidate();
        Changed?.Invoke();
    }

    async void LoadPicture(CatalogEntry entry)
    {
        int version = ++_pictureVersion;
        _preview.Picture = null;
        if (_resources == null || entry.PopAnim == null) { _preview.Invalidate(); return; }
        var res = _resources;
        var pic = await Task.Run(() =>
        {
            try { return PamPreview.Render(res, entry.PopAnim, out _, scale: 0.5); }
            catch (Exception e) when (e is FormatException or InvalidOperationException or IOException) { return null; }
        });
        if (version != _pictureVersion || IsDisposed) return;
        _preview.Picture = pic == null ? null : Pictures.ToBitmap(pic);
        _preview.Invalidate();
    }

    /// The page drawn roughly as the game lays it out: the name over the plant on the left, stats
    /// two to a row on the right, then the texts.
    sealed class PagePreview : Panel
    {
        public PlantAlmanac? Page;
        public Bitmap? Picture;

        static readonly Color Frame = Color.FromArgb(120, 72, 36), Paper = Color.FromArgb(240, 233, 214), PaperDark = Color.FromArgb(222, 214, 192),
            Green = Color.FromArgb(104, 168, 40), Ink = Color.FromArgb(80, 60, 30), StatInk = Color.FromArgb(220, 30, 30), Gold = Color.FromArgb(236, 196, 40);

        public PagePreview()
        {
            DoubleBuffered = true;
            AutoScroll = true;
            ResizeRedraw = true;
        }

        protected override void OnPaint(PaintEventArgs e)
        {
            base.OnPaint(e);
            var g = e.Graphics;
            g.SmoothingMode = SmoothingMode.AntiAlias;
            g.TextRenderingHint = System.Drawing.Text.TextRenderingHint.ClearTypeGridFit;
            g.TranslateTransform(AutoScrollPosition.X, AutoScrollPosition.Y);
            if (Page == null) return;

            int pad = 12, w = Math.Max(520, ClientSize.Width - 2 * pad);
            int cardW = Math.Min(220, w / 3), x0 = pad, y0 = pad;
            using var title = new Font("Segoe UI", 15f, FontStyle.Bold);
            using var small = new Font("Segoe UI", 7.5f, FontStyle.Bold);
            using var big = new Font("Segoe UI", 13f, FontStyle.Bold);
            using var body = new Font("Segoe UI", 10f);
            using var bodyBold = new Font("Segoe UI", 10f, FontStyle.Bold);

            // the plant's card
            var card = new Rectangle(x0, y0, cardW, 300);
            Fill(g, card, Paper, 10);
            Fill(g, new Rectangle(card.X, card.Y, card.Width, 44), Green, 10);
            using (var white = new SolidBrush(Color.White))
                g.DrawString(Page.Text(Page.NameKey), title, white, new RectangleF(card.X, card.Y + 6, card.Width, 34), new StringFormat { Alignment = StringAlignment.Center });
            var pictureBox = new Rectangle(card.X + 8, card.Y + 50, card.Width - 16, 170);
            Fill(g, pictureBox, Color.FromArgb(24, 52, 84), 4);
            if (Picture != null)
            {
                float s = Math.Min(1f, Math.Min((pictureBox.Width - 10f) / Picture.Width, (pictureBox.Height - 10f) / Picture.Height));
                float pw = Picture.Width * s, ph = Picture.Height * s;
                g.DrawImage(Picture, pictureBox.X + (pictureBox.Width - pw) / 2, pictureBox.Y + (pictureBox.Height - ph) / 2, pw, ph);
            }
            using (var ink = new SolidBrush(Ink))
                g.DrawString(Page.Text(Page.TooltipKey), body, ink, new RectangleF(card.X + 8, pictureBox.Bottom + 8, card.Width - 16, 70));

            // the stats, two to a row, then the texts
            int sx = card.Right + pad, sw = x0 + w - sx, y = y0;
            var page = new Rectangle(sx, y0, sw, 0);
            var stats = Page.Stats;
            int rowH = 52;
            int rows = (stats.Count + 1) / 2;
            int textTop = y0 + rows * (rowH + 4) + 10;
            // measure the texts to size the paper
            var runs = new List<(Font Font, Color Color, string Text)>();
            string header = Page.Text(Page.HeaderKey);
            if (header.Length > 0) runs.Add((body, Ink, header + "\n\n"));
            foreach (var r in PlantAlmanac.Markup(Page.Text(Page.DescriptionKey)))
                runs.Add(r.Style switch
                {
                    "KEYWORD" => (bodyBold, Ink, r.Text),
                    "STAT" => (bodyBold, StatInk, r.Text),
                    _ => (body, Ink, r.Text),
                });
            int textH = MeasureRuns(g, runs, sw - 24);
            page.Height = Math.Max(300, textTop - y0 + textH + 20);
            Fill(g, page, Paper, 10);
            for (int i = 0; i < stats.Count; i++)
            {
                int r = i / 2, c = i % 2;
                var cell = new Rectangle(sx + 8 + c * (sw - 16) / 2, y0 + 8 + r * (rowH + 4), (sw - 16) / 2 - 4, rowH);
                if (c == 0) Fill(g, new Rectangle(sx + 4, cell.Y - 2, sw - 8, rowH + 4), r % 2 == 0 ? Paper : PaperDark, 6);
                var icon = new Rectangle(cell.X + 2, cell.Y + 8, 34, 34);
                Fill(g, icon, Color.FromArgb(70, 80, 30), 6);
                using (var ink = new SolidBrush(Ink)) g.DrawString(stats[i].Label.ToUpperInvariant(), small, ink, icon.Right + 6, cell.Y + 6);
                using (var gold = new SolidBrush(Gold))
                using (var outline = new Pen(Color.FromArgb(90, 60, 10), 3f) { LineJoin = LineJoin.Round })
                using (var path = new GraphicsPath())
                {
                    path.AddString(stats[i].Value, big.FontFamily, (int)FontStyle.Bold, g.DpiY * big.SizeInPoints / 72f, new PointF(icon.Right + 6, cell.Y + 20), StringFormat.GenericDefault);
                    g.DrawPath(outline, path);
                    g.FillPath(gold, path);
                }
            }
            DrawRuns(g, runs, new Rectangle(sx + 14, textTop, sw - 28, textH));
            AutoScrollMinSize = new Size(w + 2 * pad, Math.Max(card.Bottom, page.Bottom) + pad);
        }

        static void Fill(Graphics g, Rectangle r, Color c, int radius)
        {
            using var path = new GraphicsPath();
            int d = radius * 2;
            path.AddArc(r.X, r.Y, d, d, 180, 90);
            path.AddArc(r.Right - d, r.Y, d, d, 270, 90);
            path.AddArc(r.Right - d, r.Bottom - d, d, d, 0, 90);
            path.AddArc(r.X, r.Bottom - d, d, d, 90, 90);
            path.CloseFigure();
            using var b = new SolidBrush(c);
            g.FillPath(b, path);
        }

        // styled runs wrapped word by word into a width
        static int MeasureRuns(Graphics g, List<(Font Font, Color Color, string Text)> runs, int width) => FlowText(g, runs, width, null, Point.Empty);

        static void DrawRuns(Graphics g, List<(Font Font, Color Color, string Text)> runs, Rectangle r) => FlowText(g, runs, r.Width, g, r.Location);

        static int FlowText(Graphics measure, List<(Font Font, Color Color, string Text)> runs, int width, Graphics? draw, Point at)
        {
            float x = 0, y = 0, line = 0;
            var fmt = StringFormat.GenericTypographic;
            foreach (var (font, color, text) in runs)
            {
                line = Math.Max(line, font.GetHeight(measure));
                using var brush = new SolidBrush(color);
                foreach (var token in Tokens(text))
                {
                    if (token == "\n") { x = 0; y += line; continue; }
                    float tw = measure.MeasureString(token, font, int.MaxValue, fmt).Width;
                    if (x > 0 && x + tw > width && token != " ") { x = 0; y += line; }
                    if (x == 0 && token == " ") continue;
                    draw?.DrawString(token, font, brush, at.X + x, at.Y + y, fmt);
                    x += tw;
                }
            }
            return (int)Math.Ceiling(y + line);
        }

        static IEnumerable<string> Tokens(string text)
        {
            int start = 0;
            for (int i = 0; i < text.Length; i++)
                if (text[i] is ' ' or '\n')
                {
                    if (i > start) yield return text[start..i];
                    yield return text[i].ToString();
                    start = i + 1;
                }
            if (start < text.Length) yield return text[start..];
        }
    }
}
