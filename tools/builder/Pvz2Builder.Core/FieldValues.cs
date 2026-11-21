using System.Globalization;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Pvz2Builder;

/// A field's value as the editor shows it and as typed back, for each kind of field.
public static class FieldValues
{
    /// The text an editor shows for a value: strings as they are, everything else as JSON.
    public static string Format(JsonNode? value) => value switch
    {
        null => "",
        JsonValue v when v.GetValueKind() == JsonValueKind.String => v.GetValue<string>(),
        JsonValue v when v.GetValueKind() == JsonValueKind.Number => v.ToJsonString(),
        _ => value.ToJsonString(new JsonSerializerOptions { WriteIndented = value is JsonObject or JsonArray }),
    };

    /// The value typed into an editor for a field of this kind, or an error saying what's wrong.
    public static (JsonNode? Value, string? Error) Parse(string text, FieldKind kind)
    {
        text = text.Trim();
        switch (kind)
        {
            case FieldKind.Bool:
                if (bool.TryParse(text, out bool b)) return (JsonValue.Create(b), null);
                return (null, "true or false");
            case FieldKind.Int:
                if (long.TryParse(text, NumberStyles.Integer, CultureInfo.InvariantCulture, out long l))
                    return (l is >= int.MinValue and <= int.MaxValue ? JsonValue.Create((int)l) : JsonValue.Create(l), null);
                return (null, "a whole number");
            case FieldKind.Float:
                if (double.TryParse(text, NumberStyles.Float, CultureInfo.InvariantCulture, out double d) && double.IsFinite(d))
                    return (d == Math.Floor(d) && Math.Abs(d) < 1e15 ? JsonValue.Create((long)d) : JsonValue.Create(d), null);
                return (null, "a number (with a point for decimals: 1.5)");
            case FieldKind.Rtid:
                if (text.Length == 0) return (JsonValue.Create(""), null);
                if (Rtid.Parse(text) == null) return (null, "RTID(name@Sheet)");
                return (JsonValue.Create(text), null);
            case FieldKind.String:
                return (JsonValue.Create(text), null);
            default:
                if (text.Length == 0) return (null, null);
                try { return (JsonNode.Parse(text), null); }
                catch (JsonException e) { return (null, "JSON: " + e.Message); }
        }
    }

    /// A value to start a field off with when it's added: what the game uses most, else a blank
    /// of its kind.
    public static JsonNode? Default(FieldSchema f) => f.Main switch
    {
        FieldKind.Bool => JsonValue.Create(false),
        FieldKind.Int => JsonValue.Create(0),
        FieldKind.Float => JsonValue.Create(0),
        FieldKind.String => JsonValue.Create(f.Choices?.FirstOrDefault() ?? ""),
        FieldKind.Rtid => JsonValue.Create(""),
        FieldKind.Array => new JsonArray(),
        FieldKind.Object => new JsonObject(),
        _ => null,
    };
}
