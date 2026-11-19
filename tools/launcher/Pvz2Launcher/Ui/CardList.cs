using System.Diagnostics.CodeAnalysis;
using System.ComponentModel;

namespace Pvz2Launcher.Ui;

/// A scrolling list of cards (the games, the mods), drawn entirely by the launcher into one
/// off-screen buffer, so nothing flickers: hovering or picking a card repaints only that card, and
/// the scrollbar is a thin wooden one in the theme's colours.
sealed class CardList : Control
{
    readonly List<object> _items = [];
    int _selected = -1, _hover = -1, _scroll;
    bool _thumbHover, _dragging;
    int _dragFrom, _dragScroll;
    const int BarWidth = 12;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int ItemHeight { get; set; }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Action<Graphics, object, RectangleF, bool, bool>? DrawCard { get; set; }
    /// A click on a card: the item, the card's bounds and where it was clicked.
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Action<object, RectangleF, Point>? ClickCard { get; set; }
    public event EventHandler? SelectedIndexChanged;

    public CardList(int itemHeight)
    {
        ItemHeight = itemHeight;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                 ControlStyles.ResizeRedraw | ControlStyles.Selectable, true);
        TabStop = true;
    }

    public IReadOnlyList<object> Items => _items;

    public void SetItems(IEnumerable<object> items, int select = -1)
    {
        _items.Clear();
        _items.AddRange(items);
        _hover = -1;
        _selected = select >= 0 && select < _items.Count ? select : -1;
        _scroll = Math.Clamp(_scroll, 0, MaxScroll);
        if (_selected >= 0) EnsureVisible(_selected);
        Invalidate();
        SelectedIndexChanged?.Invoke(this, EventArgs.Empty);
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public int SelectedIndex
    {
        get => _selected;
        set
        {
            if (value < -1 || value >= _items.Count || value == _selected) return;
            int old = _selected;
            _selected = value;
            InvalidateItem(old);
            InvalidateItem(value);
            if (value >= 0) EnsureVisible(value);
            SelectedIndexChanged?.Invoke(this, EventArgs.Empty);
        }
    }

    public object? SelectedItem => _selected >= 0 && _selected < _items.Count ? _items[_selected] : null;

    int ContentHeight => _items.Count * ItemHeight + 4;
    int MaxScroll => Math.Max(0, ContentHeight - ClientSize.Height);
    bool HasBar => MaxScroll > 0;
    int CardWidth => ClientSize.Width - (HasBar ? BarWidth + 4 : 0);

    Rectangle ItemRect(int i) => new(0, 2 + i * ItemHeight - _scroll, CardWidth, ItemHeight);
    static RectangleF CardBounds(Rectangle item) => new(item.X + 4, item.Y + 3, item.Width - 10, item.Height - 7);

    void InvalidateItem(int i)
    {
        if (i >= 0 && i < _items.Count) Invalidate(Rectangle.Inflate(ItemRect(i), 2, 2));
    }

    /// Repaints one item (its data changed).
    public void Refresh(object item) => InvalidateItem(_items.IndexOf(item));

    void EnsureVisible(int i)
    {
        var r = ItemRect(i);
        if (r.Top < 0) ScrollTo(_scroll + r.Top - 2);
        else if (r.Bottom > ClientSize.Height) ScrollTo(_scroll + r.Bottom - ClientSize.Height + 2);
    }

    void ScrollTo(int y)
    {
        y = Math.Clamp(y, 0, MaxScroll);
        if (y == _scroll) return;
        _scroll = y;
        Invalidate();
    }

    int IndexAt(Point p)
    {
        if (p.X >= CardWidth) return -1;
        int i = (p.Y + _scroll - 2) / ItemHeight;
        return p.Y + _scroll - 2 >= 0 && i < _items.Count ? i : -1;
    }

    Rectangle Track => new(ClientSize.Width - BarWidth - 1, 4, BarWidth, ClientSize.Height - 8);

    Rectangle Thumb
    {
        get
        {
            var t = Track;
            int h = Math.Max(36, (int)((long)t.Height * ClientSize.Height / Math.Max(1, ContentHeight)));
            int y = t.Y + (int)((long)(t.Height - h) * _scroll / Math.Max(1, MaxScroll));
            return new Rectangle(t.X, y, t.Width, h);
        }
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        var p = Theme.Current;
        using (var bg = new SolidBrush(p.Panel)) g.FillRectangle(bg, e.ClipRectangle);
        Draw.Smooth(g);
        int first = Math.Max(0, (_scroll + e.ClipRectangle.Top - 2) / ItemHeight - 1);
        for (int i = first; i < _items.Count; i++)
        {
            var r = ItemRect(i);
            if (r.Top > e.ClipRectangle.Bottom) break;
            if (!r.IntersectsWith(Rectangle.Inflate(e.ClipRectangle, 4, 4))) continue;
            DrawCard?.Invoke(g, _items[i], CardBounds(r), i == _selected, i == _hover);
        }
        if (HasBar)
        {
            var t = Track;
            using (var tp = Draw.Round(t, t.Width / 2f))
            using (var tb = new SolidBrush(Color.FromArgb(p.IsDark ? 70 : 45, 0, 0, 0)))
                g.FillPath(tb, tp);
            var th = Thumb;
            var c = _dragging || _thumbHover ? p.Plaque : p.Wood;
            using var path = Draw.Round(new RectangleF(th.X + 1, th.Y, th.Width - 2, th.Height), (th.Width - 2) / 2f);
            using (var fill = new System.Drawing.Drawing2D.LinearGradientBrush(th, c.Top, c.Bottom, System.Drawing.Drawing2D.LinearGradientMode.Horizontal))
                g.FillPath(fill, path);
            using var rim = new Pen(c.Edge, 1.5f);
            g.DrawPath(rim, path);
        }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (_dragging)
        {
            var t = Track;
            int travel = Math.Max(1, t.Height - Thumb.Height);
            ScrollTo(_dragScroll + (int)((long)(e.Y - _dragFrom) * MaxScroll / travel));
            return;
        }
        bool thumb = HasBar && Thumb.Contains(e.Location);
        if (thumb != _thumbHover) { _thumbHover = thumb; Invalidate(Track); }
        int i = IndexAt(e.Location);
        if (i == _hover) return;
        int old = _hover;
        _hover = i;
        InvalidateItem(old);
        InvalidateItem(i);
        Cursor = i >= 0 ? Cursors.Hand : Cursors.Default;
    }

    protected override void OnMouseLeave(EventArgs e)
    {
        base.OnMouseLeave(e);
        int old = _hover;
        _hover = -1;
        InvalidateItem(old);
        if (_thumbHover) { _thumbHover = false; Invalidate(Track); }
    }

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        Focus();
        if (e.Button != MouseButtons.Left) return;
        if (HasBar && e.X >= Track.X - 2)
        {
            if (Thumb.Contains(e.Location)) { _dragging = true; _dragFrom = e.Y; _dragScroll = _scroll; Capture = true; Invalidate(Track); }
            else ScrollTo(_scroll + (e.Y < Thumb.Y ? -ClientSize.Height : ClientSize.Height));
            return;
        }
        int i = IndexAt(e.Location);
        if (i < 0) return;
        SelectedIndex = i;
        ClickCard?.Invoke(_items[i], CardBounds(ItemRect(i)), e.Location);
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        if (!_dragging) return;
        _dragging = false;
        Capture = false;
        Invalidate(Track);
    }

    protected override void OnMouseWheel(MouseEventArgs e)
    {
        base.OnMouseWheel(e);
        ScrollTo(_scroll - e.Delta * ItemHeight / 120);
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        _scroll = Math.Clamp(_scroll, 0, MaxScroll);
    }

    protected override bool IsInputKey(Keys keyData) => keyData is Keys.Up or Keys.Down or Keys.Space or Keys.Home or Keys.End || base.IsInputKey(keyData);

    protected override void OnKeyDown(KeyEventArgs e)
    {
        switch (e.KeyCode)
        {
            case Keys.Up: SelectedIndex = Math.Max(0, _selected - 1); e.Handled = true; break;
            case Keys.Down: SelectedIndex = Math.Min(_items.Count - 1, _selected + 1); e.Handled = true; break;
            case Keys.Home: if (_items.Count > 0) SelectedIndex = 0; e.Handled = true; break;
            case Keys.End: SelectedIndex = _items.Count - 1; e.Handled = true; break;
        }
        base.OnKeyDown(e);
    }
}

/// A search field in the theme: a rounded parchment pill with a magnifying glass, around a
/// borderless text box.
sealed class SearchBox : Control
{
    readonly TextBox _box = new() { BorderStyle = BorderStyle.None };
    bool _focused;

    public SearchBox(string placeholder)
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                 ControlStyles.SupportsTransparentBackColor | ControlStyles.ResizeRedraw, true);
        BackColor = Color.Transparent;
        Height = 32;
        _box.PlaceholderText = placeholder;
        _box.Font = Theme.Text(10f);
        _box.TextChanged += (_, _) => OnTextChanged(EventArgs.Empty);
        _box.GotFocus += (_, _) => { _focused = true; Invalidate(); };
        _box.LostFocus += (_, _) => { _focused = false; Invalidate(); };
        Controls.Add(_box);
        Cursor = Cursors.IBeam;
        ApplyTheme();
    }

    [AllowNull]
    public override string Text { get => _box.Text; set => _box.Text = value; }

    public void ApplyTheme()
    {
        _box.BackColor = Theme.Current.Input;
        _box.ForeColor = Theme.Current.Text;
        Invalidate();
    }

    protected override void OnClick(EventArgs e) { _box.Focus(); base.OnClick(e); }

    protected override void OnLayout(LayoutEventArgs e)
    {
        base.OnLayout(e);
        int h = _box.PreferredHeight;
        _box.SetBounds(32, (Height - h) / 2, Math.Max(10, Width - 44), h);
    }

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Draw.Smooth(g);
        var p = Theme.Current;
        var r = new RectangleF(1, 1, Width - 2, Height - 3);
        using var path = Draw.Round(r, r.Height / 2);
        using (var fill = new SolidBrush(p.Input)) g.FillPath(fill, path);
        using (var inset = new Pen(Color.FromArgb(p.IsDark ? 120 : 40, 0, 0, 0), 2f))
        using (var top = Draw.Round(new RectangleF(r.X + 1, r.Y + 1, r.Width - 2, r.Height - 2), r.Height / 2))
            g.DrawPath(inset, top);
        using (var rim = new Pen(_focused ? p.Plaque.Bottom : p.WoodDark, _focused ? 2.2f : 1.6f)) g.DrawPath(rim, path);
        // the magnifying glass
        using var glass = new Pen(p.TextMuted, 2f);
        float cx = 16, cy = Height / 2f - 1;
        g.DrawEllipse(glass, cx - 6, cy - 6, 10, 10);
        g.DrawLine(glass, cx + 2.5f, cy + 2.5f, cx + 6.5f, cy + 6.5f);
    }
}
