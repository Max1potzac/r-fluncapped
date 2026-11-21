namespace Pvz2Builder;

/// A PopAnim drawn with the game's own pictures, one frame at a time (an animation label's first
/// frame): the parts the game only shows sometimes (costumes, armour) can be switched on and off.
sealed class PreviewPanel : UserControl
{
    readonly ComboBox _anim = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 300 };
    readonly ComboBox _label = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 140 };
    readonly ComboBox _zoom = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 70 };
    readonly Button _save = new() { Text = "Save PNG...", AutoSize = true };
    readonly PictureBox _picture = new() { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.CenterImage, BackColor = Color.FromArgb(118, 168, 84) };
    readonly CheckedListBox _parts = new() { Dock = DockStyle.Right, Width = 230, CheckOnClick = true, IntegralHeight = false };
    readonly Label _status = new() { Dock = DockStyle.Bottom, Height = 22, TextAlign = ContentAlignment.MiddleLeft };

    Resources? _resources;
    PamAnimation? _animation;
    Picture? _shown;
    int _version;
    bool _filling;

    public PreviewPanel()
    {
        var bar = new FlowLayoutPanel { Dock = DockStyle.Top, Height = 34, Padding = new Padding(4), WrapContents = false };
        bar.Controls.AddRange([new Label { Text = "Animation", AutoSize = true, Padding = new Padding(0, 6, 0, 0) }, _anim,
            new Label { Text = "Frame", AutoSize = true, Padding = new Padding(8, 6, 0, 0) }, _label,
            new Label { Text = "Zoom", AutoSize = true, Padding = new Padding(8, 6, 0, 0) }, _zoom, _save]);
        _zoom.Items.AddRange(["1x", "2x", "3x"]);
        _zoom.SelectedIndex = 1;
        var partsBox = new GroupBox { Text = "Parts shown", Dock = DockStyle.Right, Width = 240, Padding = new Padding(6) };
        _parts.Dock = DockStyle.Fill;
        partsBox.Controls.Add(_parts);
        Controls.Add(_picture);
        Controls.Add(partsBox);
        Controls.Add(_status);
        Controls.Add(bar);
        _anim.SelectedIndexChanged += (_, _) => { if (!_filling) LoadAnimation(); };
        _label.SelectedIndexChanged += (_, _) => { if (!_filling) Draw(); };
        _zoom.SelectedIndexChanged += (_, _) => { if (!_filling) Draw(); };
        _parts.ItemCheck += (_, _) => { if (!_filling) BeginInvoke(Draw); };
        _save.Click += (_, _) => SavePng();
    }

    /// Shows the PopAnims of an entry: its own first, then the rest of its resource groups'.
    public void Show(Resources resources, CatalogEntry? entry)
    {
        _resources = resources;
        _filling = true;
        _anim.Items.Clear();
        if (entry != null)
        {
            var ids = new List<string>();
            if (entry.PopAnim != null) ids.Add(entry.PopAnim);
            foreach (var g in entry.ResourceGroups) ids.AddRange(resources.PopAnimsIn(g));
            foreach (var id in ids.Distinct()) _anim.Items.Add(id);
        }
        _filling = false;
        if (_anim.Items.Count > 0) _anim.SelectedIndex = 0;
        else
        {
            _animation = null;
            SetPicture(null, entry == null ? "" : "This has no animation of its own.");
        }
    }

    void LoadAnimation()
    {
        _animation = null;
        if (_resources == null || _anim.SelectedItem is not string id) return;
        var data = _resources.PopAnimFile(id);
        try { _animation = data == null ? null : PamAnimation.Parse(data); }
        catch (Exception e) when (e is FormatException or System.Text.Json.JsonException or InvalidOperationException) { SetPicture(null, $"Its PAM can't be read: {e.Message}"); return; }
        if (_animation == null) { SetPicture(null, $"The game has no {id}."); return; }
        _filling = true;
        _label.Items.Clear();
        var main = _animation.Main ?? _animation.Sprites.LastOrDefault();
        if (main != null)
        {
            _label.Items.Add("(first frame)");
            foreach (var l in main.Labels.Distinct()) _label.Items.Add(l);
            int def = PamPreview.DefaultFrame(main);
            string? defLabel = main.Frames.ElementAtOrDefault(def)?.Label;
            _label.SelectedItem = defLabel != null && _label.Items.Contains(defLabel) ? defLabel : "(first frame)";
        }
        _parts.Items.Clear();
        foreach (var name in _animation.Sprites.Select(s => s.Name).Where(n => n.Length > 0).Distinct().Order(StringComparer.OrdinalIgnoreCase))
            _parts.Items.Add(name, !PamPreview.DefaultHidden(name));
        _filling = false;
        Draw();
    }

    void Draw()
    {
        if (_resources == null || _animation == null) return;
        var anim = _animation;
        var res = _resources;
        var main = anim.Main ?? anim.Sprites.LastOrDefault();
        int? frame = _label.SelectedItem is string l && l != "(first frame)" && main != null ? main.LabelFrame(l) : 0;
        var hidden = new HashSet<string>(StringComparer.Ordinal);
        for (int i = 0; i < _parts.Items.Count; i++)
            if (!_parts.GetItemChecked(i)) hidden.Add((string)_parts.Items[i]);
        double scale = 0.5 * (_zoom.SelectedIndex + 1);
        int version = ++_version;
        _status.Text = "Drawing...";
        Task.Run(() => PamPreview.Render(anim, id => res.Image(id) is { } pic && int.TryParse(res.ResOf(id), out int r) ? (pic, r) : null,
            frame, scale, null, hidden.Contains)).ContinueWith(t =>
        {
            if (IsDisposed || version != _version) return;
            BeginInvoke(() =>
            {
                if (version != _version) return;
                if (t.Exception != null) SetPicture(null, "Couldn't draw it: " + t.Exception.InnerException?.Message);
                else SetPicture(t.Result, t.Result == null ? "Nothing in this frame could be drawn (its pictures may be in a format the Builder can't show yet)." :
                    $"{t.Result.Width} x {t.Result.Height}, {anim.Images.Count} pictures, {anim.Sprites.Count} parts");
            });
        });
    }

    void SetPicture(Picture? p, string status)
    {
        var old = _picture.Image;
        _shown = p;
        _picture.Image = p == null ? null : Pictures.ToBitmap(p);
        old?.Dispose();
        _status.Text = status;
        _save.Enabled = p != null;
    }

    void SavePng()
    {
        if (_shown == null) return;
        using var dlg = new SaveFileDialog { Filter = "PNG picture|*.png", FileName = (_anim.SelectedItem as string ?? "preview").ToLowerInvariant() + ".png" };
        if (dlg.ShowDialog(this) == DialogResult.OK) File.WriteAllBytes(dlg.FileName, _shown.ToPng());
    }
}

/// The pictures in an entry's resource groups, each cut out of its atlas.
sealed class PicturesPanel : UserControl
{
    readonly ListBox _list = new() { Dock = DockStyle.Left, Width = 380, IntegralHeight = false };
    readonly PictureBox _picture = new() { Dock = DockStyle.Fill, SizeMode = PictureBoxSizeMode.CenterImage, BackColor = Color.FromArgb(118, 168, 84) };
    readonly Label _status = new() { Dock = DockStyle.Bottom, Height = 22, TextAlign = ContentAlignment.MiddleLeft };
    readonly Button _save = new() { Text = "Save PNG...", AutoSize = true, Dock = DockStyle.Top };
    Resources? _resources;
    Picture? _shown;

    public PicturesPanel()
    {
        Controls.Add(_picture);
        Controls.Add(_list);
        Controls.Add(_save);
        Controls.Add(_status);
        _list.SelectedIndexChanged += (_, _) => ShowSelected();
        _save.Click += (_, _) =>
        {
            if (_shown == null || _list.SelectedItem is not ImageItem item) return;
            using var dlg = new SaveFileDialog { Filter = "PNG picture|*.png", FileName = item.Image.Id.ToLowerInvariant() + ".png" };
            if (dlg.ShowDialog(this) == DialogResult.OK) File.WriteAllBytes(dlg.FileName, _shown.ToPng());
        };
    }

    sealed record ImageItem(ImageResource Image)
    {
        public override string ToString() => $"{Image.Id}  ({Image.W} x {Image.H})";
    }

    public void Show(Resources resources, CatalogEntry? entry)
    {
        _resources = resources;
        _list.Items.Clear();
        if (entry != null)
            foreach (var g in entry.ResourceGroups)
                foreach (var img in resources.ImagesIn(g)) _list.Items.Add(new ImageItem(img));
        _status.Text = entry == null ? "" : _list.Items.Count == 0 ? "No pictures in its resource groups." : $"{_list.Items.Count} pictures (768)";
        if (_list.Items.Count > 0) _list.SelectedIndex = 0;
        else SetPicture(null);
    }

    void ShowSelected()
    {
        if (_resources == null || _list.SelectedItem is not ImageItem item) return;
        var p = _resources.Image(item.Image.Id);
        SetPicture(p);
        if (p == null) _status.Text = $"{item.Image.AtlasPath} can't be shown (its texture format isn't decoded yet).";
    }

    void SetPicture(Picture? p)
    {
        var old = _picture.Image;
        _shown = p;
        _picture.Image = p == null ? null : Pictures.ToBitmap(p);
        old?.Dispose();
        _save.Enabled = p != null;
    }
}
