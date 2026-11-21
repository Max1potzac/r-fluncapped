using System.Text.Json.Nodes;

namespace Pvz2Builder;

/// The fields of one object (its objdata), each with an editor fitting its kind: a tick box, a
/// number, a drop-down of the values the game uses (or its enum), an RTID picker of the objects
/// the field can point at, or JSON for lists and nested objects. Every change goes into the edit
/// session at once.
sealed class FieldEditor : UserControl
{
    readonly TableLayoutPanel _table = new() { ColumnCount = 3, AutoSize = true, AutoSizeMode = AutoSizeMode.GrowAndShrink, Dock = DockStyle.Top, Padding = new Padding(4) };
    readonly Panel _scroll = new() { Dock = DockStyle.Fill, AutoScroll = true };
    readonly Label _title = new() { Dock = DockStyle.Top, Height = 44, Padding = new Padding(6, 4, 6, 0), Font = new Font(SystemFonts.MessageBoxFont!.FontFamily, 10f, FontStyle.Bold) };
    readonly FlowLayoutPanel _addBar = new() { Dock = DockStyle.Bottom, Height = 34, Padding = new Padding(4) };
    readonly ComboBox _addField = new() { Width = 220, DropDownStyle = ComboBoxStyle.DropDownList };
    readonly Button _add = new() { Text = "Add field", AutoSize = true };
    readonly ToolTip _tips = new();

    EditSession? _session;
    GameProfile? _profile;
    Schema? _schema;
    string _sheet = "";
    int _index = -1;

    /// Raised after any change, so lists can mark the object changed.
    public event Action? Changed;
    /// Raised when an RTID's "Go" is pressed: the sheet and index it points at.
    public event Action<string, int>? Navigate;

    public FieldEditor()
    {
        _scroll.Controls.Add(_table);
        _table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 210));
        _table.ColumnStyles.Add(new ColumnStyle(SizeType.Absolute, 320));
        _table.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _addBar.Controls.Add(new Label { Text = "Field the class has but this object doesn't set:", AutoSize = true, Padding = new Padding(0, 6, 0, 0) });
        _addBar.Controls.Add(_addField);
        _addBar.Controls.Add(_add);
        _add.Click += (_, _) => AddField();
        Controls.Add(_scroll);
        Controls.Add(_addBar);
        Controls.Add(_title);
    }

    public string Sheet => _sheet;
    public int Index => _index;

    public void Show(EditSession session, GameProfile profile, Schema schema, string sheet, int index)
    {
        _session = session;
        _profile = profile;
        _schema = schema;
        _sheet = sheet;
        _index = index;
        Rebuild();
    }

    public void Clear()
    {
        _session = null;
        _table.Controls.Clear();
        _table.RowCount = 0;
        _title.Text = "";
        _addField.Items.Clear();
    }

    public void Rebuild()
    {
        _table.SuspendLayout();
        _table.Controls.Clear();
        _table.RowStyles.Clear();
        _table.RowCount = 0;
        _addField.Items.Clear();
        if (_session?.Current(_sheet, _index) is not { } obj) { _title.Text = ""; _table.ResumeLayout(); return; }
        var original = _session.Original(_sheet, _index);
        string cls = GameProfile.Str(obj["objclass"]) ?? "(no class)";
        string aliases = obj["aliases"] is JsonArray a ? string.Join(", ", a.Select(GameProfile.Str).OfType<string>()) : "";
        var classSchema = _schema?.Class(cls);
        _title.Text = $"{(aliases.Length > 0 ? aliases : $"object {_index}")}   ·   {cls}   ·   {_sheet}\n" +
                      (classSchema == null ? "no schema for this class" : $"{classSchema.Objects} object(s) of this class in the game" + (_schema!.Dump != null ? ", with the class dump" : ""));

        var data = obj["objdata"] as JsonObject ?? [];
        var origData = original?["objdata"] as JsonObject;
        foreach (var (name, value) in data.ToList())
        {
            var fs = classSchema?.Fields.GetValueOrDefault(name);
            bool changed = origData == null || !origData.ContainsKey(name) || !JsonNode.DeepEquals(origData[name], value);
            AddRow(name, value, fs, changed);
        }
        if (classSchema != null)
            foreach (var (name, _) in classSchema.Fields)
                if (!data.ContainsKey(name)) _addField.Items.Add(name);
        _add.Enabled = _addField.Items.Count > 0;
        if (_addField.Items.Count > 0) _addField.SelectedIndex = 0;
        _table.ResumeLayout();
    }

    void AddRow(string name, JsonNode? value, FieldSchema? fs, bool changed)
    {
        var kind = fs?.Main is { } m and not FieldKind.Null ? m : FieldSchema.KindOf(value);
        // a value of another kind than the schema says (a float field holding 300) keeps its own
        if (kind is FieldKind.Int && FieldSchema.KindOf(value) == FieldKind.Float) kind = FieldKind.Float;
        var label = new Label
        {
            Text = name,
            AutoSize = false,
            Width = 200,
            Height = 24,
            TextAlign = ContentAlignment.MiddleLeft,
            Font = changed ? new Font(Font, FontStyle.Bold) : Font,
            ForeColor = changed ? Color.DarkGreen : SystemColors.ControlText,
        };
        _tips.SetToolTip(label, Describe(name, fs, kind, changed));
        // a value that isn't the field's kind (a string where lists usually are) is edited as JSON
        var own = FieldSchema.KindOf(value);
        if (own is FieldKind.Array or FieldKind.Object) kind = own;
        else if (kind is FieldKind.Array or FieldKind.Object && own != FieldKind.Null) kind = own;
        Control editor = kind switch
        {
            FieldKind.Bool => BoolEditor(name, value),
            FieldKind.Int or FieldKind.Float => TextEditor(name, value, kind),
            FieldKind.String => fs?.Choices is { } choices ? ChoiceEditor(name, value, kind, choices) : TextEditor(name, value, kind),
            FieldKind.Rtid => ChoiceEditor(name, value, kind, RtidChoices(fs, value)),
            _ => JsonEditor(name, value),
        };
        int row = _table.RowCount++;
        _table.RowStyles.Add(new RowStyle(SizeType.AutoSize));
        _table.Controls.Add(label, 0, row);
        _table.Controls.Add(editor, 1, row);
        if (kind == FieldKind.Rtid && GameProfile.Str(value) is { Length: > 0 } rtid)
        {
            var go = new Button { Text = "Go", AutoSize = true };
            go.Click += (_, _) =>
            {
                if (_profile?.Resolve(rtid, _sheet) is { } target) Navigate?.Invoke(target.Sheet, target.Index);
                else MessageBox.Show(this, $"{rtid} isn't in this game's sheets.", "PvZ2 Builder");
            };
            _table.Controls.Add(go, 2, row);
        }
    }

    static string Describe(string name, FieldSchema? fs, FieldKind kind, bool changed)
    {
        var lines = new List<string> { $"{name}: {kind}" };
        if (fs?.DeclaredType != null) lines.Add($"the game's type: {fs.DeclaredType}");
        if (fs != null && fs.Seen > 0) lines.Add($"set in {fs.Seen} object(s) of this class");
        if (fs?.EnumValues != null) lines.Add($"one of: {string.Join(", ", fs.EnumValues.Take(12))}{(fs.EnumValues.Count > 12 ? " ..." : "")}");
        if (fs?.RtidSheet != null) lines.Add($"points into {fs.RtidSheet}");
        if (changed) lines.Add("changed");
        return string.Join("\n", lines);
    }

    IReadOnlyList<string> RtidChoices(FieldSchema? fs, JsonNode? value)
    {
        string? sheet = fs?.RtidSheet ?? (Rtid.Parse(GameProfile.Str(value)) is (_, string s) && s is not ("" or ".") ? s : null);
        if (sheet == null || _profile == null) return [];
        return _profile.AliasesIn(sheet).Distinct(StringComparer.Ordinal).Select(a => Rtid.Make(a, sheet)).ToList();
    }

    Control BoolEditor(string name, JsonNode? value)
    {
        var box = new CheckBox { Checked = value is JsonValue v && v.GetValueKind() == System.Text.Json.JsonValueKind.True, AutoSize = true, Padding = new Padding(0, 4, 0, 0) };
        box.CheckedChanged += (_, _) => Set(name, JsonValue.Create(box.Checked));
        return box;
    }

    Control TextEditor(string name, JsonNode? value, FieldKind kind)
    {
        var box = new TextBox { Text = FieldValues.Format(value), Width = 310 };
        box.Leave += (_, _) => Commit(box, name, kind);
        box.KeyDown += (_, e) => { if (e.KeyCode == Keys.Enter) { Commit(box, name, kind); e.SuppressKeyPress = true; } };
        return box;
    }

    Control ChoiceEditor(string name, JsonNode? value, FieldKind kind, IReadOnlyList<string> choices)
    {
        var box = new ComboBox
        {
            Width = 310,
            DropDownStyle = ComboBoxStyle.DropDown,
            AutoCompleteMode = AutoCompleteMode.SuggestAppend,
            AutoCompleteSource = AutoCompleteSource.ListItems,
            MaxDropDownItems = 20,
        };
        box.Items.AddRange(choices.Cast<object>().ToArray());
        box.Text = FieldValues.Format(value);
        box.SelectionChangeCommitted += (_, _) => { box.Text = box.SelectedItem?.ToString() ?? box.Text; Commit(box, name, kind); };
        box.Leave += (_, _) => Commit(box, name, kind);
        box.KeyDown += (_, e) => { if (e.KeyCode == Keys.Enter) { Commit(box, name, kind); e.SuppressKeyPress = true; } };
        return box;
    }

    Control JsonEditor(string name, JsonNode? value)
    {
        string text = FieldValues.Format(value);
        int lines = Math.Clamp(text.Count(c => c == '\n') + 1, 1, 12);
        var box = new TextBox
        {
            Text = text.Replace("\n", Environment.NewLine),
            Multiline = true,
            ScrollBars = lines >= 12 ? ScrollBars.Vertical : ScrollBars.None,
            Width = 310,
            Height = 8 + lines * (Font.Height + 2),
            Font = new Font(FontFamily.GenericMonospace, 8.5f),
            AcceptsReturn = true,
        };
        box.Leave += (_, _) => Commit(box, name, FieldKind.Object);
        return box;
    }

    void Commit(Control box, string name, FieldKind kind)
    {
        var (value, error) = FieldValues.Parse(box.Text, kind);
        if (error != null)
        {
            box.BackColor = Color.MistyRose;
            _tips.SetToolTip(box, $"Not saved: this needs {error}");
            return;
        }
        box.BackColor = SystemColors.Window;
        _tips.SetToolTip(box, "");
        var current = _session?.Current(_sheet, _index)?["objdata"]?[name];
        if (JsonNode.DeepEquals(current, value)) return;
        Set(name, value);
    }

    void Set(string name, JsonNode? value)
    {
        if (_session == null) return;
        var obj = _session.Edit(_sheet, _index);
        if (obj["objdata"] is not JsonObject data) obj["objdata"] = data = new JsonObject();
        data[name] = value?.DeepClone();
        Changed?.Invoke();
        // mark the label without rebuilding (which would take the focus away)
        BeginInvoke(MarkLabels);
    }

    void MarkLabels()
    {
        if (_session == null) return;
        var data = _session.Current(_sheet, _index)?["objdata"] as JsonObject;
        var orig = _session.Original(_sheet, _index)?["objdata"] as JsonObject;
        foreach (Control c in _table.Controls)
            if (c is Label l && _table.GetColumn(l) == 0 && data != null)
            {
                bool changed = orig == null || !orig.ContainsKey(l.Text) || !JsonNode.DeepEquals(orig[l.Text], data[l.Text]);
                l.Font = changed ? new Font(Font, FontStyle.Bold) : Font;
                l.ForeColor = changed ? Color.DarkGreen : SystemColors.ControlText;
            }
    }

    void AddField()
    {
        if (_session == null || _addField.SelectedItem is not string name) return;
        var obj = _session.Current(_sheet, _index);
        var fs = _schema?.Class(GameProfile.Str(obj?["objclass"]) ?? "")?.Fields.GetValueOrDefault(name);
        Set(name, fs != null ? FieldValues.Default(fs) : null);
        Rebuild();
    }
}
