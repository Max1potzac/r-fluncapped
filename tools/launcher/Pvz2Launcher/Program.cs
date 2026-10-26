namespace Pvz2Launcher;

// PvZ2 launcher: a menu for each game in games/ (Reflourished, AltverZ ...), which starts it
// under rfu with the settings, code injections and custom content picked there.
static class Program
{
    [STAThread]
    static void Main()
    {
        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm(AppContext.BaseDirectory));
    }
}
