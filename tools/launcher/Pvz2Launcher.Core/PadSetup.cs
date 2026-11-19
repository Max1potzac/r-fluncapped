namespace Pvz2Launcher;

/// The controller's settings: which button does each action, screen by screen (rfu's
/// --pad-binds), and SDL mappings for controllers SDL doesn't know (rfu's --pad-mappings, one
/// gamecontrollerdb.txt line each).
///
/// Each screen keeps its own buttons: moving "take a packet" on the seed chooser leaves A alone on
/// the lawn and in the menus. In the settings an action is "<screen>.<action>" ("chooser.take"),
/// the lawn's without a screen ("plant"), as rfu reads them.
public static class PadSetup
{
    /// An action, its rfu name and the button that does it by default.
    public sealed record Action(string Name, string Label, string Default);

    /// A screen with buttons of its own: its rfu name ("" prefix for the lawn) and its actions.
    public sealed record Screen(string Name, string Label, Action[] Actions)
    {
        /// The settings key for one of its actions.
        public string Key(Action a) => Name == "lawn" ? a.Name : $"{Name}.{a.Name}";
    }

    public static readonly Screen[] Screens =
    [
        new("lawn", "Lawn",
        [
            new("plant", "Plant", "a"),
            new("dig", "Dig up (hold)", "b"),
            new("use", "Plant ability, target, mower (hold)", "x"),
            new("plantfood", "Plant food (hold to buy)", "y"),
            new("prevseed", "Previous seed packet", "lb"),
            new("nextseed", "Next seed packet", "rb"),
            new("vacuum", "Collect everything", "lt"),
            new("vacuum2", "Collect everything (2nd)", "rt"),
            new("fastforward", "Fast-forward", "l3"),
            new("slowmode", "Slowmode (half speed)", "r3"),
            new("pause", "Pause", "start"),
            new("freecursor", "Free cursor (hold)", "back"),
        ]),
        new("chooser", "Seed chooser",
        [
            new("take", "Take the packet", "a"),
            new("remove", "Put the slot's packet back (hold: all)", "b"),
            new("letsrock", "Let's Rock (hold)", "x"),
            new("viewlawn", "Look at the lawn", "y"),
            new("prevslot", "Previous slot", "lb"),
            new("nextslot", "Next slot", "rb"),
            new("reload", "Last level's packets", "lt"),
            new("almanac", "Almanac", "rt"),
            new("pause", "Pause", "start"),
            new("freecursor", "Free cursor (hold)", "back"),
        ]),
        new("menu", "Pause menu, settings, title",
        [
            new("press", "Press the picked button", "a"),
            new("back", "Resume / back", "b"),
            new("start", "Resume (presses on the title screen)", "start"),
            new("prevbanner", "Title screen: previous banner", "lb"),
            new("nextbanner", "Title screen: next banner", "rb"),
        ]),
        new("map", "World map",
        [
            new("touch", "Touch at the cursor (hold to drag)", "a"),
            new("back", "Back", "b"),
            new("shop", "Shop", "y"),
            new("almanac", "Almanac", "lt"),
            new("zengarden", "Zen Garden", "rt"),
            new("settings", "Settings", "start"),
        ]),
        new("other", "Everywhere else",
        [
            new("touch", "Touch at the cursor (hold to drag)", "a"),
            new("back", "Back", "b"),
            new("pause", "Pause (in a level)", "start"),
            new("fastforward", "Fast-forward (in a level)", "l3"),
        ]),
    ];

    /// The lawn's actions.
    public static Action[] Actions => Screens[0].Actions;

    /// The buttons an action can be on, by rfu's name, with Xbox's and PlayStation's labels.
    public static readonly (string Name, string Label)[] Buttons =
    [
        ("a", "A / Cross"), ("b", "B / Circle"), ("x", "X / Square"), ("y", "Y / Triangle"),
        ("lb", "LB / L1"), ("rb", "RB / R1"), ("lt", "LT / L2"), ("rt", "RT / R2"),
        ("l3", "L3 (left stick)"), ("r3", "R3 (right stick)"), ("start", "Start / Options"), ("back", "Back / Share"),
    ];

    /// The button an action is on, by its settings.
    public static string ButtonOf(IReadOnlyDictionary<string, string> binds, Screen screen, Action a) =>
        binds.TryGetValue(screen.Key(a), out var b) && Buttons.Any(x => x.Name == b) ? b : a.Default;

    /// The binds, as rfu's --pad-binds wants them: every action of each screen where one's moved
    /// (the others are left as they are); null when every action is on its default button.
    public static string? BindsArg(IReadOnlyDictionary<string, string> binds)
    {
        var parts = new List<string>();
        foreach (var screen in Screens)
        {
            var mine = screen.Actions.Select(a => (Key: screen.Key(a), Button: ButtonOf(binds, screen, a), a.Default)).ToList();
            if (mine.All(m => m.Button == m.Default)) continue;
            parts.AddRange(mine.Select(m => $"{m.Key}={m.Button}"));
        }
        return parts.Count > 0 ? string.Join(",", parts) : null;
    }

    /// The actions of a screen that share a button with another one there.
    public static List<string> Clashes(IReadOnlyDictionary<string, string> binds, Screen screen) =>
        screen.Actions.GroupBy(a => ButtonOf(binds, screen, a))
            .Where(g => g.Count() > 1).SelectMany(g => g.Select(a => a.Label)).ToList();

    /// The binds worth keeping: the actions moved off their default buttons.
    public static Dictionary<string, string> Moved(IReadOnlyDictionary<string, string> binds) =>
        Screens.SelectMany(s => s.Actions.Select(a => (Key: s.Key(a), Button: ButtonOf(binds, s, a), a.Default)))
            .Where(x => x.Button != x.Default).ToDictionary(x => x.Key, x => x.Button);

    /// The screens a controller layout's spot can be for: rfu's names, "any" for all of them.
    public static readonly (string Name, string Label)[] LayoutScreens =
        [("any", "Every screen"), .. Screens.Select(s => (s.Name, s.Label))];

    // SDL's names for a controller's parts, in the order the setup asks for them.
    public sealed record Part(string Sdl, string Prompt, bool Axis);

    public static readonly Part[] Parts =
    [
        new("a", "Press the bottom face button (A / Cross)", false),
        new("b", "Press the right face button (B / Circle)", false),
        new("x", "Press the left face button (X / Square)", false),
        new("y", "Press the top face button (Y / Triangle)", false),
        new("leftshoulder", "Press the left bumper (LB / L1)", false),
        new("rightshoulder", "Press the right bumper (RB / R1)", false),
        new("lefttrigger", "Pull the left trigger (LT / L2)", false),
        new("righttrigger", "Pull the right trigger (RT / R2)", false),
        new("back", "Press Back / Share", false),
        new("start", "Press Start / Options", false),
        new("leftstick", "Click the left stick in (L3)", false),
        new("rightstick", "Click the right stick in (R3)", false),
        new("dpup", "Press up on the d-pad", false),
        new("dpdown", "Press down on the d-pad", false),
        new("dpleft", "Press left on the d-pad", false),
        new("dpright", "Press right on the d-pad", false),
        new("leftx", "Push the left stick right", true),
        new("lefty", "Push the left stick down", true),
        new("rightx", "Push the right stick right", true),
        new("righty", "Push the right stick down", true),
    ];

    /// What a part of the controller was found to be: a button ("b3"), a hat's direction
    /// ("h0.4"), or an axis ("a2", "a2~" when it goes the other way).
    public static string Button(int index) => $"b{index}";
    public static string Hat(int hat, int direction) => $"h{hat}.{direction}";
    public static string Axis(int axis, bool inverted) => inverted ? $"a{axis}~" : $"a{axis}";

    /// A gamecontrollerdb.txt line for a joystick: "GUID,name,a:b0,...,platform:Windows,".
    public static string Mapping(string guid, string name, IEnumerable<KeyValuePair<string, string>> parts)
    {
        string clean = new(name.Where(c => c != ',' && !char.IsControl(c)).ToArray());
        var items = parts.Where(p => p.Value != "").Select(p => $"{p.Key}:{p.Value}");
        return $"{guid},{(clean.Trim() == "" ? "Controller" : clean.Trim())},{string.Join(",", items)},platform:Windows,";
    }

    /// Puts a mapping into a mappings file, in place of any line for the same GUID.
    public static void SaveMapping(string path, string mapping)
    {
        string guid = mapping.Split(',')[0];
        var lines = File.Exists(path) ? File.ReadAllLines(path).ToList() : [];
        lines.RemoveAll(l => l.Split(',')[0].Equals(guid, StringComparison.OrdinalIgnoreCase));
        lines.Add(mapping);
        File.WriteAllLines(path, lines);
    }
}
