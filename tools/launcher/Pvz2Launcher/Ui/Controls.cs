using System.ComponentModel;
using System.Drawing.Drawing2D;

namespace Pvz2Launcher.Ui;

/// A glossy PvZ2 button. As a toggle (a filter chip, a tab), it shows its colour when checked and
/// wood when not.
sealed class GlossButton : Control
{
    Draw.State _state = Draw.State.Normal;
    bool _checked;

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Func<Palette, Gloss> Color { get; set; } = p => p.Green;
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Toggle { get; set; }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Checked { get => _checked; set { _checked = value; Invalidate(); } }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public float FontSize { get; set; } = 10f;

    public GlossButton(string text, Func<Palette, Gloss>? color = null, float fontSize = 10f)
    {
        Text = text;
        if (color != null) Color = color;
        FontSize = fontSize;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                 ControlStyles.ResizeRedraw, true);
        Cursor = Cursors.Hand;
        Height = 38;
        AutoSizeToText();
    }

    public void AutoSizeToText()
    {
        using var f = Theme.Title(FontSize);
        Width = TextRenderer.MeasureText(Text, f).Width + 34;
    }

    protected override void OnMouseEnter(EventArgs e) { _state = Draw.State.Hover; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _state = Draw.State.Normal; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnMouseDown(MouseEventArgs e) { if (e.Button == MouseButtons.Left) { _state = Draw.State.Pressed; Invalidate(); } base.OnMouseDown(e); }
    protected override void OnMouseUp(MouseEventArgs e) { _state = ClientRectangle.Contains(e.Location) ? Draw.State.Hover : Draw.State.Normal; Invalidate(); base.OnMouseUp(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }
    protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }

    protected override void OnPaintBackground(PaintEventArgs e) => Backdrop.Paint(this, e.Graphics);

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Draw.Smooth(g);
        var p = Theme.Current;
        var c = Toggle && !Checked ? p.Wood : Color(p);
        var state = !Enabled ? Draw.State.Disabled : Toggle && Checked && _state == Draw.State.Normal ? Draw.State.Normal : _state;
        using var f = Theme.Title(FontSize);
        Draw.Button(g, new RectangleF(0, 0, Width, Height), c, Text, f, state);
    }
}

/// A panel in a wooden frame, with an optional title plaque. Child controls go inside the frame
/// (its Padding is set to fit).
class WoodPanel : Panel, IBackdropSource
{
    string? _title;

    public WoodPanel(string? title = null)
    {
        _title = title;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                 ControlStyles.ResizeRedraw, true);
        Padding = title != null ? new Padding(16, 46, 16, 18) : new Padding(16, 14, 16, 18);
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public string? Title { get => _title; set { _title = value; Invalidate(); } }

    // the frame, drawn once per size, theme and title: every see-through control inside it
    // repaints this behind itself
    Bitmap? _frame;
    (Size, string, string?)? _frameFor;

    protected override void OnPaintBackground(PaintEventArgs e) => Backdrop.Paint(this, e.Graphics);

    protected override void OnPaint(PaintEventArgs e) => PaintFrame(e.Graphics);

    /// What shows through the controls inside it: what's behind the panel, then its frame.
    public void PaintBackdrop(Graphics g)
    {
        Backdrop.Paint(this, g);
        PaintFrame(g);
    }

    void PaintFrame(Graphics target)
    {
        var key = (Size, Theme.Current.Name, _title);
        if (_frame == null || _frameFor != key)
        {
            _frame?.Dispose();
            _frame = new Bitmap(Math.Max(1, Width), Math.Max(1, Height), System.Drawing.Imaging.PixelFormat.Format32bppPArgb);
            using var g = Graphics.FromImage(_frame);
            Draw.Smooth(g);
            using var f = Theme.Title(11);
            Draw.WoodFrame(g, new RectangleF(0, 0, Width, Height), Theme.Current, _title, f);
            _frameFor = key;
        }
        target.DrawImageUnscaled(_frame, 0, 0);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) _frame?.Dispose();
        base.Dispose(disposing);
    }
}

/// An on/off switch with a label beside it.
sealed class SwitchRow : Control
{
    bool _checked, _hover;
    public event EventHandler? CheckedChanged;

    public SwitchRow(string text)
    {
        Text = text;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                 ControlStyles.ResizeRedraw, true);
        Height = 32;
        Width = 420;
        Cursor = Cursors.Hand;
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public bool Checked
    {
        get => _checked;
        set { if (_checked == value) return; _checked = value; Invalidate(); CheckedChanged?.Invoke(this, EventArgs.Empty); }
    }

    protected override void OnClick(EventArgs e) { Checked = !Checked; base.OnClick(e); }
    protected override void OnMouseEnter(EventArgs e) { _hover = true; Invalidate(); base.OnMouseEnter(e); }
    protected override void OnMouseLeave(EventArgs e) { _hover = false; Invalidate(); base.OnMouseLeave(e); }
    protected override void OnEnabledChanged(EventArgs e) { Invalidate(); base.OnEnabledChanged(e); }

    protected override void OnPaintBackground(PaintEventArgs e) => Backdrop.Paint(this, e.Graphics);

    protected override void OnPaint(PaintEventArgs e)
    {
        var g = e.Graphics;
        Draw.Smooth(g);
        var p = Theme.Current;
        Draw.Switch(g, new RectangleF(1, (Height - 24) / 2f, 46, 24), _checked, p, Enabled, _hover);
        using var f = Theme.Text(10f);
        TextRenderer.DrawText(g, Text, f, new Rectangle(56, 0, Width - 56, Height), Enabled ? p.Text : p.TextMuted,
            TextFormatFlags.VerticalCenter | TextFormatFlags.Left | TextFormatFlags.EndEllipsis);
    }
}

/// A label in the theme's colours (they follow the theme when it changes).
sealed class ThemedLabel : Label
{
    public bool Muted { get; set; }
    public bool Chunky { get; set; }
    public ThemedLabel(string text = "", float size = 10f, bool muted = false, bool chunky = false)
    {
        Text = text;
        Muted = muted;
        Chunky = chunky;
        Font = chunky ? Theme.Title(size) : Theme.Text(size);
        AutoSize = true;
        SetStyle(ControlStyles.OptimizedDoubleBuffer | ControlStyles.AllPaintingInWmPaint, true);
        ApplyTheme();
    }
    protected override void OnPaintBackground(PaintEventArgs e) => Backdrop.Paint(this, e.Graphics);
    public void ApplyTheme() => ForeColor = Muted ? Theme.Current.TextMuted : Theme.Current.Text;
}

/// Big outlined lettering, like the game's titles (the launcher's name, the game's name).
sealed class TitleLabel : Control
{
    public TitleLabel(string text, float size)
    {
        Text = text;
        Size_ = size;
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.UserPaint |
                 ControlStyles.ResizeRedraw, true);
        Height = (int)(size * 2.2f);
    }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public float Size_ { get; set; }
    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    public Func<Palette, Color>? Fill { get; set; }
    protected override void OnTextChanged(EventArgs e) { Invalidate(); base.OnTextChanged(e); }
    protected override void OnPaintBackground(PaintEventArgs e) => Backdrop.Paint(this, e.Graphics);

    protected override void OnPaint(PaintEventArgs e)
    {
        Draw.Smooth(e.Graphics);
        using var f = Theme.Title(Size_);
        var p = Theme.Current;
        Draw.OutlinedText(e.Graphics, Text, f, new RectangleF(4, 0, Width - 8, Height), Fill?.Invoke(p) ?? Color.White, p.TextOutline,
            Math.Max(2, Size_ / 6), StringAlignment.Near);
    }
}

static class Themed
{
    /// Colours a plain WinForms control (text boxes, drop-downs, the log) for the theme.
    public static void Apply(Control c)
    {
        var p = Theme.Current;
        switch (c)
        {
            case TextBox t: t.BackColor = p.Input; t.ForeColor = p.Text; t.BorderStyle = BorderStyle.FixedSingle; break;
            case ComboBox b: b.BackColor = p.Input; b.ForeColor = p.Text; b.FlatStyle = FlatStyle.Flat; break;
            case ListBox l: l.BackColor = p.Panel; l.ForeColor = p.Text; l.BorderStyle = BorderStyle.None; break;
            case ThemedLabel tl: tl.ApplyTheme(); break;
        }
        foreach (Control child in c.Controls) Apply(child);
        c.Invalidate();
    }
}
