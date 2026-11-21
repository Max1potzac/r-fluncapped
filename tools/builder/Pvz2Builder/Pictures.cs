using System.Drawing.Imaging;

namespace Pvz2Builder;

static class Pictures
{
    /// A Builder picture (RGBA) as a GDI+ bitmap (BGRA).
    public static Bitmap ToBitmap(Picture p)
    {
        var bmp = new Bitmap(Math.Max(1, p.Width), Math.Max(1, p.Height), PixelFormat.Format32bppArgb);
        if (p.Width == 0 || p.Height == 0) return bmp;
        var data = bmp.LockBits(new Rectangle(0, 0, p.Width, p.Height), ImageLockMode.WriteOnly, PixelFormat.Format32bppArgb);
        try
        {
            var row = new byte[p.Width * 4];
            for (int y = 0; y < p.Height; y++)
            {
                for (int x = 0; x < p.Width; x++)
                {
                    int s = (y * p.Width + x) * 4, d = x * 4;
                    row[d] = p.Rgba[s + 2];
                    row[d + 1] = p.Rgba[s + 1];
                    row[d + 2] = p.Rgba[s];
                    row[d + 3] = p.Rgba[s + 3];
                }
                System.Runtime.InteropServices.Marshal.Copy(row, 0, data.Scan0 + y * data.Stride, row.Length);
            }
        }
        finally { bmp.UnlockBits(data); }
        return bmp;
    }
}
