namespace Pvz2Launcher.Ui;

/// Something that draws a background its children show through: the window's sky and lawn, a
/// wooden frame. It keeps that drawing as a bitmap.
interface IBackdropSource
{
    /// Draws the background, in its own coordinates.
    void PaintBackdrop(Graphics g);
}

/// How the launcher's controls look see-through without WinForms' transparency (which repaints
/// the parent for every child, and flickers): each control is opaque and copies the part of the
/// nearest backdrop it covers from that backdrop's bitmap.
static class Backdrop
{
    public static void Paint(Control c, Graphics g)
    {
        var offset = c.Location;
        var p = c.Parent;
        while (p != null && p is not IBackdropSource)
        {
            offset.Offset(p.Location);
            p = p.Parent;
        }
        if (p is IBackdropSource src)
        {
            var state = g.Save();
            g.TranslateTransform(-offset.X, -offset.Y);
            src.PaintBackdrop(g);
            g.Restore(state);
        }
        else
        {
            g.Clear(Theme.Current.Panel);
        }
    }

    public const ControlStyles Styles = ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                                        ControlStyles.UserPaint | ControlStyles.ResizeRedraw;
}

/// A panel that shows the backdrop behind it (a page in a wooden frame, a row of buttons).
class BackdropPanel : Panel
{
    public BackdropPanel() { SetStyle(Backdrop.Styles, true); }
    protected override void OnPaintBackground(PaintEventArgs e) => Backdrop.Paint(this, e.Graphics);
}

/// A FlowLayoutPanel that shows the backdrop behind it.
class BackdropFlow : FlowLayoutPanel
{
    public BackdropFlow() { SetStyle(Backdrop.Styles, true); }
    protected override void OnPaintBackground(PaintEventArgs e) => Backdrop.Paint(this, e.Graphics);
}

/// A window with the sky-and-lawn backdrop, drawn once per size and theme.
class BackdropForm : Form, IBackdropSource
{
    Bitmap? _backdrop;
    (Size, string)? _for;
    protected int LawnHeight { get; set; } = 74;

    public BackdropForm()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer | ControlStyles.ResizeRedraw, true);
    }

    public void PaintBackdrop(Graphics g)
    {
        var key = (ClientSize, Theme.Current.Name);
        if (_backdrop == null || _for != key)
        {
            _backdrop?.Dispose();
            _backdrop = new Bitmap(Math.Max(1, ClientSize.Width), Math.Max(1, ClientSize.Height));
            using var bg = Graphics.FromImage(_backdrop);
            Draw.Smooth(bg);
            Draw.Backdrop(bg, ClientRectangle, Theme.Current, LawnHeight);
            _for = key;
        }
        g.DrawImageUnscaled(_backdrop, 0, 0);
    }

    protected override void OnPaintBackground(PaintEventArgs e) => PaintBackdrop(e.Graphics);

    /// Changes many controls at once without the window showing the steps (switching a tab).
    public void Batch(Control area, Action change)
    {
        const int WM_SETREDRAW = 0x000B;
        var m = Message.Create(area.Handle, WM_SETREDRAW, IntPtr.Zero, IntPtr.Zero);
        WndProcOf(area, ref m);
        try { change(); }
        finally
        {
            m = Message.Create(area.Handle, WM_SETREDRAW, new IntPtr(1), IntPtr.Zero);
            WndProcOf(area, ref m);
            area.Invalidate(true);
            area.Update();
        }
    }

    static void WndProcOf(Control c, ref Message m) => NativeSendMessage(c.Handle, m.Msg, m.WParam, m.LParam);

    [System.Runtime.InteropServices.DllImport("user32.dll", EntryPoint = "SendMessageW")]
    static extern IntPtr NativeSendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    protected override void Dispose(bool disposing)
    {
        if (disposing) _backdrop?.Dispose();
        base.Dispose(disposing);
    }
}
