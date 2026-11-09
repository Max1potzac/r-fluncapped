namespace Pvz2Launcher;

/// Controller settings: the controllers plugged in and what SDL (so the game) makes of each, its
/// buttons lighting up as they're pressed, a set-up for one SDL doesn't know (a mapping, saved in
/// controllers.txt), and which button does each of the lawn's actions.
sealed class ControllerForm : Form
{
    readonly GameFolder _game;
    readonly Dictionary<string, string> _binds;
    readonly ComboBox _devices = new() { DropDownStyle = ComboBoxStyle.DropDownList, Width = 420 };
    readonly Label _kind = new() { AutoSize = true, MaximumSize = new Size(560, 0) };
    readonly Label _pressed = new() { AutoSize = true, MaximumSize = new Size(560, 0), Font = new Font(FontFamily.GenericMonospace, 9.5f) };
    readonly Button _setUp = new() { Text = "Set up its buttons...", AutoSize = true };
    readonly Label _prompt = new() { AutoSize = true, MaximumSize = new Size(560, 0), Font = new Font(SystemFonts.DefaultFont, FontStyle.Bold) };
    readonly Button _skip = new() { Text = "Skip this one", AutoSize = true, Visible = false };
    readonly Label _clash = new() { AutoSize = true, ForeColor = Color.Firebrick, MaximumSize = new Size(560, 0) };
    readonly Dictionary<string, ComboBox> _bindBoxes = [];
    readonly System.Windows.Forms.Timer _timer = new() { Interval = 30 };

    readonly bool _sdl;
    int _count = -1;      // joysticks last seen
    int _index = -1;      // the one picked
    IntPtr _joystick, _controller;

    // the set-up in progress
    int _step = -1;
    readonly Dictionary<string, string> _found = [];
    short[] _restAxes = [];
    bool _waitRelease;

    public ControllerForm(GameFolder game, string? rfuDir)
    {
        _game = game;
        _binds = new Dictionary<string, string>(game.Settings.PadBinds);
        Text = $"Controller settings - {game.DisplayName}";
        AutoSize = true;
        AutoSizeMode = AutoSizeMode.GrowAndShrink;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        Padding = new Padding(12);

        var stack = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.TopDown, WrapContents = false };
        stack.Controls.Add(Heading("Controller"));
        stack.Controls.Add(_devices);
        stack.Controls.Add(_kind);
        stack.Controls.Add(_pressed);
        stack.Controls.Add(Row(_setUp, _skip));
        stack.Controls.Add(_prompt);

        stack.Controls.Add(Heading("Buttons for the lawn's actions"));
        var grid = new TableLayoutPanel { AutoSize = true, ColumnCount = 2 };
        foreach (var a in PadSetup.Actions)
        {
            var box = new ComboBox { DropDownStyle = ComboBoxStyle.DropDownList, Width = 170 };
            foreach (var b in PadSetup.Buttons) box.Items.Add(b.Label);
            string current = _binds.TryGetValue(a.Name, out var v) ? v : a.Default;
            box.SelectedIndex = Math.Max(0, Array.FindIndex(PadSetup.Buttons, b => b.Name == current));
            box.SelectedIndexChanged += (_, _) =>
            {
                _binds[a.Name] = PadSetup.Buttons[box.SelectedIndex].Name;
                ShowClashes();
            };
            _bindBoxes[a.Name] = box;
            grid.Controls.Add(new Label { Text = a.Label, AutoSize = true, Anchor = AnchorStyles.Left, Margin = new Padding(3, 7, 12, 3) });
            grid.Controls.Add(box);
        }
        stack.Controls.Add(grid);
        stack.Controls.Add(_clash);
        stack.Controls.Add(new Label { Text = "The d-pad and left stick move the cursor; the right stick is kept for powerups.", AutoSize = true, ForeColor = SystemColors.GrayText });

        var defaults = new Button { Text = "Defaults", AutoSize = true };
        defaults.Click += (_, _) =>
        {
            foreach (var a in PadSetup.Actions) _bindBoxes[a.Name].SelectedIndex = Array.FindIndex(PadSetup.Buttons, b => b.Name == a.Default);
        };
        var ok = new Button { Text = "Save", AutoSize = true, DialogResult = DialogResult.OK };
        var cancel = new Button { Text = "Cancel", AutoSize = true, DialogResult = DialogResult.Cancel };
        stack.Controls.Add(Row(defaults, ok, cancel));
        AcceptButton = ok;
        CancelButton = cancel;
        Controls.Add(stack);

        _setUp.Click += (_, _) => StartSetUp();
        _skip.Click += (_, _) => NextStep("");
        _devices.SelectedIndexChanged += (_, _) => Open(_devices.SelectedIndex);
        FormClosing += (_, e) =>
        {
            if (DialogResult == DialogResult.OK && PadSetup.Clashes(_binds).Count > 0 &&
                MessageBox.Show(this, "Some actions share a button; only one of each will work. Save anyway?", Text, MessageBoxButtons.YesNo) != DialogResult.Yes)
                e.Cancel = true;
        };
        FormClosed += (_, _) =>
        {
            _timer.Stop();
            Close(ref _joystick, ref _controller);
            if (_sdl) Sdl.Quit();
            if (DialogResult == DialogResult.OK)
            {
                _game.Settings.PadBinds = _binds.Where(b => PadSetup.Actions.Any(a => a.Name == b.Key && a.Default != b.Value))
                    .ToDictionary(b => b.Key, b => b.Value);
                _game.SaveSettings();
            }
        };
        ShowClashes();

        _sdl = Sdl.Load(rfuDir);
        if (!_sdl)
        {
            _kind.Text = "SDL2.dll (next to rfu.exe) couldn't be loaded, so controllers can't be tested here. The button settings still work.";
            _devices.Enabled = _setUp.Enabled = false;
            return;
        }
        // the mappings made before, as the game will have them
        if (File.Exists(game.ControllerMappingsPath))
            foreach (var line in File.ReadAllLines(game.ControllerMappingsPath))
                if (line.Trim() is { Length: > 0 } l && !l.StartsWith('#')) Sdl.SDL_GameControllerAddMapping(l);
        _timer.Tick += (_, _) => Poll();
        _timer.Start();
        Poll();
    }

    static Label Heading(string text) => new() { Text = text, AutoSize = true, Font = new Font(SystemFonts.DefaultFont.FontFamily, 10.5f, FontStyle.Bold), Margin = new Padding(0, 10, 0, 4) };

    static FlowLayoutPanel Row(params Control[] controls)
    {
        var row = new FlowLayoutPanel { AutoSize = true, FlowDirection = FlowDirection.LeftToRight, WrapContents = false };
        row.Controls.AddRange(controls);
        return row;
    }

    void ShowClashes()
    {
        var clashes = PadSetup.Clashes(_binds);
        _clash.Text = clashes.Count == 0 ? "" : "On the same button: " + string.Join(", ", clashes);
    }

    static void Close(ref IntPtr joystick, ref IntPtr controller)
    {
        if (controller != IntPtr.Zero) Sdl.SDL_GameControllerClose(controller);
        else if (joystick != IntPtr.Zero) Sdl.SDL_JoystickClose(joystick);
        controller = joystick = IntPtr.Zero;
    }

    void Poll()
    {
        Sdl.SDL_PumpEvents();
        Sdl.SDL_JoystickUpdate();
        int count = Sdl.SDL_NumJoysticks();
        if (count != _count)
        {
            _count = count;
            _step = -1;
            _devices.Items.Clear();
            for (int i = 0; i < count; i++) _devices.Items.Add(Sdl.Name(i));
            if (count == 0)
            {
                Close(ref _joystick, ref _controller);
                _index = -1;
                _kind.Text = "No controller found. Plug one in (or pair it), and it shows up here.";
                _pressed.Text = "";
                _setUp.Enabled = false;
                return;
            }
            _devices.SelectedIndex = 0;  // opens it
        }
        if (_index < 0) return;
        if (_step >= 0) { SetUpStep(); return; }
        _pressed.Text = "Pressed: " + Pressed();
    }

    void Open(int index)
    {
        Close(ref _joystick, ref _controller);
        _index = index;
        _step = -1;
        _prompt.Text = "";
        _skip.Visible = false;
        if (index < 0) return;
        _setUp.Enabled = true;
        if (Sdl.SDL_IsGameController(index) != 0 && (_controller = Sdl.SDL_GameControllerOpen(index)) != IntPtr.Zero)
        {
            _kind.Text = $"Recognised. The game shows {Sdl.TypeName(Sdl.SDL_GameControllerGetType(_controller))} buttons for it. Press its buttons to test them.";
            return;
        }
        _joystick = Sdl.SDL_JoystickOpen(index);
        _kind.Text = _joystick == IntPtr.Zero
            ? $"It can't be opened: {Sdl.Error()}"
            : "Not recognised: SDL (so the game) doesn't know which of its buttons is which. Press \"Set up its buttons\" and it will ask for each one.";
    }

    // What's held: the controller's buttons by name, or a joystick's raw buttons and axes.
    string Pressed()
    {
        var held = new List<string>();
        if (_controller != IntPtr.Zero)
        {
            foreach (var (name, button) in Sdl.Buttons)
                if (Sdl.SDL_GameControllerGetButton(_controller, button) != 0) held.Add(name.ToUpperInvariant());
            if (Sdl.SDL_GameControllerGetAxis(_controller, Sdl.AxisLeftTrigger) > 16000) held.Add("LT");
            if (Sdl.SDL_GameControllerGetAxis(_controller, Sdl.AxisRightTrigger) > 16000) held.Add("RT");
            Stick(held, "left stick", Sdl.SDL_GameControllerGetAxis(_controller, Sdl.AxisLeftX), Sdl.SDL_GameControllerGetAxis(_controller, Sdl.AxisLeftY));
            Stick(held, "right stick", Sdl.SDL_GameControllerGetAxis(_controller, Sdl.AxisRightX), Sdl.SDL_GameControllerGetAxis(_controller, Sdl.AxisRightY));
        }
        else if (_joystick != IntPtr.Zero)
        {
            for (int i = 0; i < Sdl.SDL_JoystickNumButtons(_joystick); i++)
                if (Sdl.SDL_JoystickGetButton(_joystick, i) != 0) held.Add($"button {i}");
            for (int i = 0; i < Sdl.SDL_JoystickNumHats(_joystick); i++)
                if (Sdl.SDL_JoystickGetHat(_joystick, i) is var h && h != 0) held.Add($"hat {i}:{h}");
            for (int i = 0; i < Sdl.SDL_JoystickNumAxes(_joystick); i++)
                if (Sdl.SDL_JoystickGetAxis(_joystick, i) is var v && Math.Abs((int)v) > 20000) held.Add($"axis {i} {(v > 0 ? "+" : "-")}");
        }
        return held.Count == 0 ? "(nothing)" : string.Join(", ", held);
    }

    static void Stick(List<string> held, string name, short x, short y)
    {
        const int dead = 16000;
        string dir = (y < -dead ? "up" : y > dead ? "down" : "") + (x < -dead ? "left" : x > dead ? "right" : "");
        if (dir != "") held.Add($"{name} {dir}");
    }

    // --- set-up: each part asked for in turn, found as what changes from rest -----------------

    void StartSetUp()
    {
        if (_index < 0) return;
        Close(ref _joystick, ref _controller);
        _joystick = Sdl.SDL_JoystickOpen(_index);
        if (_joystick == IntPtr.Zero) { _kind.Text = $"It can't be opened: {Sdl.Error()}"; return; }
        _found.Clear();
        _restAxes = Axes();
        _waitRelease = false;
        _step = 0;
        _skip.Visible = true;
        _setUp.Enabled = false;
        _prompt.Text = PadSetup.Parts[0].Prompt;
    }

    short[] Axes() => Enumerable.Range(0, Sdl.SDL_JoystickNumAxes(_joystick)).Select(i => Sdl.SDL_JoystickGetAxis(_joystick, i)).ToArray();

    void SetUpStep()
    {
        _pressed.Text = "Pressed: " + Pressed();
        var part = PadSetup.Parts[_step];
        bool anyHeld = Enumerable.Range(0, Sdl.SDL_JoystickNumButtons(_joystick)).Any(i => Sdl.SDL_JoystickGetButton(_joystick, i) != 0)
                       || Enumerable.Range(0, Sdl.SDL_JoystickNumHats(_joystick)).Any(i => Sdl.SDL_JoystickGetHat(_joystick, i) != 0)
                       || Axes().Select((v, i) => Math.Abs(v - _restAxes[i])).Any(d => d > 12000);
        // one press per part: everything let go before the next is read
        if (_waitRelease) { _waitRelease = anyHeld; return; }
        string got = "";
        for (int i = 0; i < Sdl.SDL_JoystickNumButtons(_joystick) && got == ""; i++)
            if (Sdl.SDL_JoystickGetButton(_joystick, i) != 0 && !_found.ContainsValue(PadSetup.Button(i))) got = PadSetup.Button(i);
        for (int i = 0; i < Sdl.SDL_JoystickNumHats(_joystick) && got == ""; i++)
        {
            int h = Sdl.SDL_JoystickGetHat(_joystick, i);
            if (h is 1 or 2 or 4 or 8 && !_found.ContainsValue(PadSetup.Hat(i, h))) got = PadSetup.Hat(i, h);
        }
        var axes = Axes();
        for (int i = 0; i < axes.Length && got == ""; i++)
        {
            int moved = axes[i] - _restAxes[i];
            if (Math.Abs(moved) < 16000 || _found.ContainsValue($"a{i}") || _found.ContainsValue($"a{i}~")) continue;
            // a stick pushed right or down should read positive; a trigger is the whole axis
            got = part.Axis ? PadSetup.Axis(i, moved < 0) : PadSetup.Axis(i, false);
        }
        if (got != "") NextStep(got);
    }

    void NextStep(string got)
    {
        if (_step < 0) return;
        _found[PadSetup.Parts[_step].Sdl] = got;
        _waitRelease = true;
        if (++_step < PadSetup.Parts.Length)
        {
            _prompt.Text = PadSetup.Parts[_step].Prompt;
            return;
        }
        // all asked: the mapping, for the game and for this window
        string mapping = PadSetup.Mapping(Sdl.GuidString(_index), Sdl.Name(_index), _found);
        PadSetup.SaveMapping(_game.ControllerMappingsPath, mapping);
        Sdl.SDL_GameControllerAddMapping(mapping);
        _step = -1;
        _skip.Visible = false;
        _prompt.Text = "Saved. The game will know this controller from now on.";
        Open(_index);
    }
}
