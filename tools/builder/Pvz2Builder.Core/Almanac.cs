using System.Globalization;
using System.Text.Json.Nodes;

namespace Pvz2Builder;

/// One stat on a plant's almanac page, the way the page shows it.
/// `Label` and `Value` are as shown; `LabelRaw` and `ValueRaw` as written in the almanac entry
/// ("[STAT_DAMAGE]" is a string's key, anything else is the text itself). `ValueFrom` says where
/// the shown value comes from.
public sealed record AlmanacStat(int Index, string StatName, string StatType, string LabelRaw, string Label, string ValueRaw, string Value,
    string ValueFrom, int LevelToUnlock);

/// A run of description text in one style, from the game's markup ({KEYWORD}Special: {STAT}...).
public sealed record TextRun(string Style, string Text);

/// A plant's almanac page: its name and texts (LawnStrings) and its stats (PLANTALMANACDATA, with
/// numbers from PLANTLEVELS and the plant's properties). Reads and edits through an EditSession, so
/// the page is saved as part of an ordinary mod.
public sealed class PlantAlmanac
{
    public const string DataSheet = "PLANTALMANACDATA", LevelsSheet = "PLANTLEVELS";
    const string Missing = "[MISSING INFO]";

    public EditSession Session { get; }
    public CatalogEntry Plant { get; }
    public string TypeName { get; }
    /// The plant's almanac entry, or -1 when the game has none for it (older games keep no stats in data).
    public int DataIndex { get; }
    public int LevelsIndex { get; }
    public (string Sheet, int Index)? Properties { get; }

    /// The string keys of the page's texts.
    public string NameKey { get; }
    public string TooltipKey => Key("TOOLTIP");
    public string HeaderKey => Key("DESCRIPTION_HEADER");
    public string DescriptionKey => Key("DESCRIPTION");
    public string PlantFoodKey => Key("PLANTFOOD");
    string Key(string part) => $"{NameKey}_{part}";

    PlantAlmanac(EditSession session, CatalogEntry plant, string typeName, int data, int levels, (string, int)? props, string nameKey)
    {
        Session = session;
        Plant = plant;
        TypeName = typeName;
        DataIndex = data;
        LevelsIndex = levels;
        Properties = props;
        NameKey = nameKey;
    }

    public static PlantAlmanac For(EditSession session, GameProfile profile, CatalogEntry plant)
    {
        var game = session.Game;
        string typeName = GameProfile.Str(session.Current(plant.Sheet, plant.Index)?["objdata"]?["TypeName"]) ?? plant.Alias;
        // the name's key: TYPENAME, or PLANT_TYPENAME in games that spell it that way
        string upper = typeName.ToUpperInvariant();
        string nameKey = session.Text(upper) == null && session.Text("PLANT_" + upper) != null ? "PLANT_" + upper : upper;
        var props = plant.Properties is { } p && profile.Resolve(p, plant.Sheet) is { } t ? (t.Sheet, t.Index) : ((string, int)?)null;
        return new PlantAlmanac(session, plant, typeName, Find(game, DataSheet, typeName), Find(game, LevelsSheet, typeName), props, nameKey);
    }

    /// The object for a plant in a sheet keyed by plant (its alias or TypeName), or -1.
    static int Find(Game game, string sheet, string typeName)
    {
        if (game.Sheet(sheet)?["objects"] is not JsonArray objects) return -1;
        for (int i = 0; i < objects.Count; i++)
            if (objects[i] is JsonObject o &&
                ((o["aliases"] is JsonArray a && a.Any(x => string.Equals(GameProfile.Str(x), typeName, StringComparison.OrdinalIgnoreCase))) ||
                 string.Equals(GameProfile.Str(o["objdata"]?["TypeName"]), typeName, StringComparison.OrdinalIgnoreCase)))
                return i;
        return -1;
    }

    public bool HasStats => DataIndex >= 0;

    // ---- texts ----

    public string Text(string key) => Session.Text(key) ?? "";
    public void SetText(string key, string text) => Session.SetText(key, text.Replace("\r\n", "\n"));

    /// A string as the page shows it: "[KEY]" is that string's text, anything else itself.
    public string Resolve(string raw) =>
        raw.Length > 2 && raw[0] == '[' && raw[^1] == ']' && raw != Missing && Session.Text(raw[1..^1]) is { } t ? t : raw;

    /// Description markup cut into styled runs: {FLAVOR}, {KEYWORD}, {STAT} ... switch the style.
    public static List<TextRun> Markup(string text)
    {
        var runs = new List<TextRun>();
        string style = "";
        int start = 0;
        for (int i = 0; i < text.Length; i++)
        {
            if (text[i] != '{') continue;
            int end = text.IndexOf('}', i);
            if (end < 0 || end - i < 2 || !text.AsSpan(i + 1, end - i - 1).ToString().All(c => char.IsAsciiLetterUpper(c) || c == '_')) continue;
            if (i > start) runs.Add(new TextRun(style, text[start..i]));
            style = text[(i + 1)..end];
            start = end + 1;
            i = end;
        }
        if (start < text.Length) runs.Add(new TextRun(style, text[start..]));
        return runs;
    }

    // ---- stats ----

    JsonArray? Entries(bool edit = false)
    {
        if (DataIndex < 0) return null;
        var obj = edit ? Session.Edit(DataSheet, DataIndex) : Session.Current(DataSheet, DataIndex);
        if (obj?["objdata"] is not JsonObject data) return null;
        if (data["AlmanacDataEntries"] is JsonArray a) return a;
        if (!edit) return null;
        return (JsonArray)(data["AlmanacDataEntries"] = new JsonArray());
    }

    JsonObject? Levels => LevelsIndex >= 0 ? Session.Current(LevelsSheet, LevelsIndex)?["objdata"] as JsonObject : null;

    public IReadOnlyList<AlmanacStat> Stats
    {
        get
        {
            var result = new List<AlmanacStat>();
            if (Entries() is not { } entries) return result;
            for (int i = 0; i < entries.Count; i++)
            {
                if (entries[i] is not JsonObject e) continue;
                string name = GameProfile.Str(e["StatName"]) ?? "";
                string labelRaw = GameProfile.Str(e["TypeStringID"]) ?? "";
                string valueRaw = GameProfile.Str(e["ValueStringID"]) ?? "";
                var (value, from) = ValueOf(name, valueRaw);
                int unlock = e["LevelToUnlock"] is JsonValue lv && (lv.TryGetValue<int>(out int n) || (lv.TryGetValue<string>(out var ls) && int.TryParse(ls, out n))) ? n : 0;
                result.Add(new AlmanacStat(i, name, GameProfile.Str(e["StatType"]) ?? "", labelRaw, Resolve(labelRaw), valueRaw, value, from, unlock));
            }
            return result;
        }
    }

    /// The value the page shows for a stat, and where it comes from: a level's text for the stat
    /// (PLANTLEVELS StringStats), else the entry's own value, else the level's number (FloatStats:
    /// how Cost reaches the page), else the plant's property of that name.
    (string Value, string From) ValueOf(string statName, string valueRaw)
    {
        if (LevelValue("StringStats", statName) is { Length: > 0 } s) return (Resolve(s), $"{LevelsSheet}, level 1");
        if (valueRaw.Length > 0 && valueRaw != Missing && valueRaw != "MISSING INFO") return (Resolve(valueRaw), "the almanac entry");
        if (LevelValue("FloatStats", statName) is { } f) return (f, $"{LevelsSheet}, level 1");
        if (Properties is { } p && Session.Current(p.Sheet, p.Index)?["objdata"]?[statName] is JsonValue v && v.GetValueKind() == System.Text.Json.JsonValueKind.Number)
            return (FieldValues.Format(v), "the plant's properties");
        return ("", "nowhere (the page shows it blank)");
    }

    string? LevelValue(string list, string statName)
    {
        if (Levels?[list] is not JsonArray stats) return null;
        foreach (var s in stats.OfType<JsonObject>())
            if (string.Equals(GameProfile.Str(s["Name"]), statName, StringComparison.Ordinal) && s["Values"] is JsonArray values && values.Count > 0)
                return values[0] is JsonValue v && v.GetValueKind() == System.Text.Json.JsonValueKind.Number ? FieldValues.Format(v) : GameProfile.Str(values[0]);
        return null;
    }

    /// Changes what a stat's label shows. Text written over a shared string's key ("[STAT_DAMAGE]")
    /// goes into this entry alone, so other plants keep theirs.
    public void SetLabel(int index, string text)
    {
        if (Entries(edit: true)?[index] is not JsonObject e) return;
        string raw = GameProfile.Str(e["TypeStringID"]) ?? "";
        if (text == Resolve(raw)) return;
        e["TypeStringID"] = text;
    }

    /// Changes what a stat's value shows. A number the game reads from PLANTLEVELS (sun cost,
    /// recharge) is changed there, and in the plant's properties, so the page and the plant agree.
    public void SetValue(int index, string text)
    {
        if (Entries(edit: true)?[index] is not JsonObject e) return;
        string name = GameProfile.Str(e["StatName"]) ?? "";
        string raw = GameProfile.Str(e["ValueStringID"]) ?? "";
        if (text == ValueOf(name, raw).Value) return;
        if (LevelValue("StringStats", name) is { Length: > 0 })
        {
            SetLevelStat("StringStats", name, JsonValue.Create(text));
            return;
        }
        bool fromNumber = raw.Length == 0 || raw == Missing || raw == "MISSING INFO";
        if (fromNumber && double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out double number))
        {
            SetNumber(name, number);
            return;
        }
        e["ValueStringID"] = text;
    }

    /// Sets a number the plant really plays with (Cost, PacketCooldown ...): every level's value in
    /// PLANTLEVELS, and the property of that name, where they have it.
    public void SetNumber(string statName, double value)
    {
        JsonNode Num() => value == Math.Floor(value) && Math.Abs(value) < int.MaxValue ? JsonValue.Create((int)value) : JsonValue.Create(value);
        SetLevelStat("FloatStats", statName, Num());
        if (Properties is { } p && Session.Current(p.Sheet, p.Index)?["objdata"]?[statName] is JsonValue)
            Session.Edit(p.Sheet, p.Index)["objdata"]![statName] = Num();
    }

    /// A number the plant plays with, from its levels or properties (null when neither has it).
    public double? Number(string statName)
    {
        if (LevelValue("FloatStats", statName) is { } s && double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out double d)) return d;
        if (Properties is { } p && Session.Current(p.Sheet, p.Index)?["objdata"]?[statName] is JsonValue v && v.TryGetValue<double>(out double x)) return x;
        return null;
    }

    void SetLevelStat(string list, string statName, JsonNode value)
    {
        if (LevelsIndex < 0) return;
        var data = Session.Edit(LevelsSheet, LevelsIndex)["objdata"] as JsonObject;
        if (data?[list] is not JsonArray stats) return;
        foreach (var s in stats.OfType<JsonObject>())
            if (string.Equals(GameProfile.Str(s["Name"]), statName, StringComparison.Ordinal) && s["Values"] is JsonArray values)
                for (int i = 0; i < values.Count; i++) values[i] = value.DeepClone();
    }

    /// Adds a stat with its own label and value text (not tied to a number the plant plays with).
    public int AddStat(string label, string value, string statType = "special")
    {
        var entries = Entries(edit: true) ?? throw new InvalidOperationException($"{TypeName} has no almanac entry");
        // a name of its own, so the game finds no level value for it
        string name = "BuilderStat";
        for (int n = 1; entries.OfType<JsonObject>().Any(e => GameProfile.Str(e["StatName"]) == name); n++) name = "BuilderStat" + n;
        entries.Add(new JsonObject
        {
            ["LevelToUnlock"] = "0",
            ["StatName"] = name,
            ["StatNameAlias"] = "Special",
            ["StatType"] = statType,
            ["StatValue"] = "special_chill",
            ["TypeStringID"] = label,
            ["ValueStringID"] = value,
        });
        return entries.Count - 1;
    }

    public void RemoveStat(int index)
    {
        if (Entries(edit: true) is { } entries && index >= 0 && index < entries.Count) entries.RemoveAt(index);
    }

    public void MoveStat(int index, int by)
    {
        if (Entries(edit: true) is not { } entries) return;
        int to = index + by;
        if (index < 0 || index >= entries.Count || to < 0 || to >= entries.Count) return;
        var node = entries[index];
        entries.RemoveAt(index);
        entries.Insert(to, node);
    }

    /// Whether anything on the page differs from the game's own.
    public bool IsChanged =>
        (DataIndex >= 0 && Session.IsChanged(DataSheet, DataIndex)) || (LevelsIndex >= 0 && Session.IsChanged(LevelsSheet, LevelsIndex)) ||
        new[] { NameKey, TooltipKey, HeaderKey, DescriptionKey, PlantFoodKey }.Any(Session.IsTextChanged);
}
