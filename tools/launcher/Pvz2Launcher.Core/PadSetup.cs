namespace Pvz2Launcher;

/// The controller's settings: which button does each of the lawn's actions (rfu's --pad-binds),
/// and SDL mappings for controllers SDL doesn't know (rfu's --pad-mappings, one
/// gamecontrollerdb.txt line each).
public static class PadSetup
{
    /// A lawn action, its rfu name and the button that does it by default.
    public sealed record Action(string Name, string Label, string Default);

    public static readonly Action[] Actions =
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
        new("pause", "Pause", "start"),
    ];

    /// The buttons an action can be on, by rfu's name, with Xbox's and PlayStation's labels.
    public static readonly (string Name, string Label)[] Buttons =
    [
        ("a", "A / Cross"), ("b", "B / Circle"), ("x", "X / Square"), ("y", "Y / Triangle"),
        ("lb", "LB / L1"), ("rb", "RB / R1"), ("lt", "LT / L2"), ("rt", "RT / R2"),
        ("l3", "L3 (left stick)"), ("r3", "R3 (right stick)"), ("start", "Start / Options"), ("back", "Back / Share"),
    ];

    /// The binds, as rfu's --pad-binds wants them; null when every action is on its default button.
    public static string? BindsArg(IReadOnlyDictionary<string, string> binds)
    {
        var parts = new List<string>();
        bool changed = false;
        foreach (var a in Actions)
        {
            string button = binds.TryGetValue(a.Name, out var b) && Buttons.Any(x => x.Name == b) ? b : a.Default;
            if (button != a.Default) changed = true;
            parts.Add($"{a.Name}={button}");
        }
        return changed ? string.Join(",", parts) : null;
    }

    /// The actions that share a button with another one.
    public static List<string> Clashes(IReadOnlyDictionary<string, string> binds) =>
        Actions.GroupBy(a => binds.TryGetValue(a.Name, out var b) ? b : a.Default)
            .Where(g => g.Count() > 1).SelectMany(g => g.Select(a => a.Label)).ToList();

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
