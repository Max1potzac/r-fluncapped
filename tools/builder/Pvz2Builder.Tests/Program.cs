using System.Text.Json.Nodes;
using Pvz2Builder;
using Pvz2Launcher;

// Builder tests. Without arguments: the logic on made-up sheets, then (with PVZ2_OBB set to a
// game's .obb) against the real game. Also small tools for looking at a game:
//   profile <obb or game folder>          what the game has
//   render <obb> <POPANIM_ID> <out.png>   a PopAnim's preview
//   sprites <obb> <POPANIM_ID>            a PopAnim's sprites
//   files <obb> <regex> [file]            the OBB's files, and one RTON file as JSON
//   classes <obb> <regex>                 the classes the game's sheets use, with their fields
//   pamjson <obb> <POPANIM_ID> <out.json> a PopAnim as JSON
//   image <obb> <IMAGE_ID or ATLAS.PTX> <out.png>   one picture cut out of its atlas, or a whole atlas
//   image <obb> list <GROUP>              a resource group's PopAnims and pictures

if (args.Length >= 2 && args[0] == "profile")
{
    using var game = Game.Open(args[1]);
    var p = GameProfile.Build(game);
    Console.WriteLine($"{game.Name}: {game.Family}, {game.SheetNames().Count} sheets");
    foreach (var k in GameProfile.Kinds) Console.WriteLine($"  {k}: {p[k].Count}  e.g. {string.Join(", ", p[k].Take(4))}");
    return 0;
}
if (args.Length >= 3 && args[0] == "sprites")
{
    using var game = Game.Open(args[1]);
    var anim = PamAnimation.Parse(Resources.Load(game).PopAnimFile(args[2])!);
    Console.WriteLine("main labels: " + string.Join(",", anim.Main?.Labels ?? []));
    for (int i = 0; i < anim.Sprites.Count; i++)
        Console.WriteLine($"sprite {i}: {anim.Sprites[i].Name} ({anim.Sprites[i].Frames.Length} frames){(PamPreview.DefaultHidden(anim.Sprites[i].Name) ? ", hidden" : "")}");
    return 0;
}
if (args.Length >= 4 && args[0] == "image")
{
    using var game = Game.Open(args[1]);
    var r = Resources.Load(game);
    if (args[2] == "list" && args.Length > 3) { foreach (var g in r.PopAnimsIn(args[3])) Console.WriteLine(g); foreach (var i in r.ImagesIn(args[3])) Console.WriteLine($"{i.Id} {i.AtlasPath} {i.W}x{i.H}"); return 0; }
    var pic = args[2].EndsWith(".PTX", StringComparison.OrdinalIgnoreCase) ? r.Atlas(args[2]) : r.Image(args[2]);
    if (pic == null) { Console.WriteLine("no such picture"); return 1; }
    File.WriteAllBytes(args[3], pic.ToPng());
    Console.WriteLine($"{pic.Width}x{pic.Height}");
    return 0;
}
if (args.Length >= 3 && args[0] == "files")
{
    using var game = Game.Open(args[1]);
    var re = new System.Text.RegularExpressions.Regex(args[2], System.Text.RegularExpressions.RegexOptions.IgnoreCase);
    foreach (var f in game.Obb!.Files.Where(f => re.IsMatch(f)).OrderBy(f => f)) Console.WriteLine(f);
    if (args.Length > 3 && game.Obb.Read(args[3]) is { } data)
        Console.WriteLine(Pvz2Launcher.SheetMerge.Parse(data)?.ToJsonString(new System.Text.Json.JsonSerializerOptions { WriteIndented = true }));
    return 0;
}
if (args.Length >= 3 && args[0] == "classes")
{
    using var game = Game.Open(args[1]);
    var schema = Schema.Infer(game);
    var re = new System.Text.RegularExpressions.Regex(args[2], System.Text.RegularExpressions.RegexOptions.IgnoreCase);
    foreach (var c in schema.Classes.Values.Where(c => re.IsMatch(c.Name)).OrderBy(c => c.Name))
        Console.WriteLine($"{c.Name} ({c.Objects}): {string.Join(", ", c.Fields.Select(f => $"{f.Key}:{f.Value.Main}"))}");
    return 0;
}
if (args.Length >= 4 && args[0] == "pamjson")
{
    using var game = Game.Open(args[1]);
    using var f = File.Create(args[3]);
    using var w = new System.Text.Json.Utf8JsonWriter(f, new() { Indented = true });
    Pvz2Conv.Pam.ToJson(Resources.Load(game).PopAnimFile(args[2])!, w, []);
    return 0;
}
if (args.Length >= 4 && args[0] == "render")
{
    using var game = Game.Open(args[1]);
    var resources = Resources.Load(game);
    Picture? pic; string why = "";
    if (args.Length > 5)
    {
        var anim = PamAnimation.Parse(resources.PopAnimFile(args[2])!);
        pic = PamPreview.Render(anim, id => resources.Image(id) is { } p ? (p, int.Parse(resources.ResOf(id)!)) : null, 0, double.Parse(args[4]), args[5] == "-" ? null : args[5], n => args.Skip(6).Contains(n) || (args[5] == "-" && PamPreview.DefaultHidden(n)));
    }
    else pic = PamPreview.Render(resources, args[2], out why, scale: args.Length > 4 ? double.Parse(args[4]) : 0.5);
    if (pic == null) { Console.WriteLine(why); return 1; }
    File.WriteAllBytes(args[3], pic.ToPng());
    Console.WriteLine($"{pic.Width}x{pic.Height}");
    return 0;
}

int failures = 0;
void Expect(bool ok, string what)
{
    if (ok) return;
    Console.WriteLine($"  FAILED: {what}");
    failures++;
}

string root = Path.Combine(Path.GetTempPath(), "pvz2builder_tests");
if (Directory.Exists(root)) Directory.Delete(root, true);
Directory.CreateDirectory(root);
string? obb = Environment.GetEnvironmentVariable("PVZ2_OBB");

var tests = new List<(string Name, Action Run)>
{
    ("schema from data", SchemaInference),
    ("schema with a class dump", SchemaDump),
    ("edits saved as a mod", EditsSaved),
    ("whole sheet when a merge can't say it", WholeSheet),
    ("affine and canvas", Drawing),
    ("game names", GameNames),
    ("typed values", TypedValues),
};
if (obb != null && File.Exists(obb))
{
    tests.Add(("real game: profile", RealProfile));
    tests.Add(("real game: previews", RealPreviews));
    tests.Add(("real game: edit, save, launcher loads it", RealEditAndLauncher));
}
else Console.WriteLine("(PVZ2_OBB isn't set to a game's .obb: the real-game tests are skipped)");

foreach (var (name, run) in tests)
{
    int before = failures;
    try { run(); }
    catch (Exception e) { Console.WriteLine($"  exception: {e}"); failures++; }
    Console.WriteLine($"{(failures == before ? "[ ok ]" : "[FAIL]")} {name}");
}
Directory.Delete(root, true);
return failures == 0 ? 0 : 1;

static JsonObject J(string json) => (JsonObject)JsonNode.Parse(json)!;

static Dictionary<string, JsonObject> Sheets() => new(StringComparer.OrdinalIgnoreCase)
{
    ["PLANTTYPES"] = J("""
        {"version":1,"objects":[
          {"objclass":"PlantType","aliases":["peashooter"],"objdata":{"TypeName":"peashooter","PlantFramework":"PlantPeashooter","Properties":"RTID(PeashooterDefault@PlantProperties)","PopAnim":"POPANIM_PLANT_PEASHOOTER","HomeWorld":"tutorial","PlantResourceGroups":["PlantPeashooter"]}},
          {"objclass":"PlantType","aliases":["sunflower"],"objdata":{"TypeName":"sunflower","PlantFramework":"PlantSunflower","Properties":"RTID(SunflowerDefault@PlantProperties)","HomeWorld":"tutorial","PlantResourceGroups":["PlantSunflower"]}},
          {"objclass":"PlantType","aliases":["wallnut"],"objdata":{"TypeName":"wallnut","PlantFramework":"PlantWallnut","Properties":"RTID(WallnutDefault@PlantProperties)","HomeWorld":"tutorial","PlantResourceGroups":[]}}
        ]}
        """),
    ["PLANTPROPERTIES"] = J("""
        {"version":1,"objects":[
          {"objclass":"PeashooterProps","aliases":["PeashooterDefault"],"objdata":{"Cost":100,"PacketCooldown":7.5,"Hitpoints":300,"Size":"normal","Shot":{"Projectile":"RTID(PeaDefault@ProjectileTypes)","Delay":1.5}}},
          {"objclass":"SunflowerProps","aliases":["SunflowerDefault"],"objdata":{"Cost":50,"PacketCooldown":7.5,"Hitpoints":300,"Size":"normal"}},
          {"objclass":"WallnutProps","aliases":["WallnutDefault"],"objdata":{"Cost":50,"PacketCooldown":30,"Hitpoints":4000,"Size":"big"}}
        ]}
        """),
    ["LIVECONFIG"] = J("""{"version":1,"objects":[{"objclass":"PVZLiveConfig","objdata":{"A":1}},{"objclass":"Other","objdata":{"B":2}},{"objclass":"Other","objdata":{"B":3}}]}"""),
    ["LAWNSTRINGS-EN-US"] = J("""{"version":1,"objects":[{"objclass":"LawnStringsData","objdata":{"LocStringValues":["PEASHOOTER","Peashooter","SUNFLOWER","Sunflower"]}}]}"""),
};

void SchemaInference()
{
    using var game = Game.FromSheets(Sheets());
    var schema = Schema.Infer(game);
    var props = schema.Class("PeashooterProps");
    Expect(props != null && props.Objects == 1, "PeashooterProps is a class with one object");
    Expect(props!.Fields["Cost"].Main == FieldKind.Int, "Cost is a whole number");
    Expect(props.Fields["PacketCooldown"].Main == FieldKind.Float, "PacketCooldown has decimals");
    Expect(props.Fields["Shot"].Main == FieldKind.Object && props.Fields["Shot"].Fields["Projectile"].Main == FieldKind.Rtid, "Shot.Projectile is an RTID");
    Expect(props.Fields["Shot"].Fields["Projectile"].RtidSheet == "ProjectileTypes", "the RTID points into ProjectileTypes");
    var type = schema.Class("PlantType")!;
    Expect(type.Objects == 3, "three PlantTypes");
    Expect(type.Fields["HomeWorld"].Choices is { } c && c.SequenceEqual(["tutorial"]), "HomeWorld is a choice of the values used");
    Expect(type.Fields["TypeName"].Choices == null, "TypeName is free text (every value differs)");
    Expect(type.Fields["PlantResourceGroups"].Main == FieldKind.Array && type.Fields["PlantResourceGroups"].Item!.Main == FieldKind.String, "a list of strings");

    var profile = GameProfile.Build(game);
    Expect(profile[GameProfile.Plants].Count == 3, "three plants");
    Expect(profile[GameProfile.Plants][0].DisplayName == "Peashooter", "the plant's name from LawnStrings");
    Expect(profile[GameProfile.Plants][0].Class == "PlantPeashooter", "the plant's class");
    Expect(profile[GameProfile.Zombies].Count == 0, "no zombies in this game");
    Expect(profile.Resolve("RTID(PeashooterDefault@PlantProperties)", "PLANTTYPES") is { Index: 0 }, "an RTID resolves");
    Expect(profile.AliasesIn("PlantProperties").Count == 3, "aliases for an RTID picker");
}

void SchemaDump()
{
    using var game = Game.FromSheets(Sheets());
    var schema = Schema.Infer(game);
    int n = schema.ApplyDump(J("""
        {"_about":"test dump","classes":{
          "PlantPropertySheet":{"base":"PropertySheetBase","fields":[{"name":"Cost","type":"int32"},{"name":"Hitpoints","type":"float32"},{"name":"Size","type":"PlantSize"},{"name":"Families","type":"std::vector<PlantFamily>"}]},
          "PeashooterProps":{"base":"PlantPropertySheet","fields":[{"name":"Shot","type":"ShootAction"}]},
          "PropertySheetBase":{"base":null,"fields":[]}},
         "enums":{"PlantSize":{"normal":0,"big":1,"small":2},"PlantFamily":{"pea":0,"nut":1}}}
        """));
    Expect(n == 3, "three classes filled in");
    var props = schema.Class("PeashooterProps")!;
    Expect(props.Fields["Hitpoints"].Main == FieldKind.Float, "the dump says Hitpoints is a float, though the data has 300");
    Expect(props.Fields["Size"].Choices is { } c && c.SequenceEqual(["normal", "big", "small"]), "an enum's values, from the base class");
    Expect(props.DumpOnly.Contains("Families") && props.Fields["Families"].Item?.EnumValues?.Count == 2, "a field only the dump knows, a list of an enum");
    Expect(schema.Class("PlantPropertySheet") != null, "a class no object uses is kept");
    Expect(schema.Dump == "test dump", "the dump's description");
}

void EditsSaved()
{
    var sheets = Sheets();
    using var game = Game.FromSheets(sheets);
    var edits = new EditSession(game);
    var pea = edits.Edit("PLANTPROPERTIES", 0);
    pea["objdata"]!["Cost"] = 125;
    pea["objdata"]!["Shot"]!["Delay"] = 1.25;
    Expect(edits.IsChanged("PlantProperties", 0), "the change is seen");
    var sun = edits.Edit("PLANTPROPERTIES", 1);
    Expect(!edits.IsChanged("PLANTPROPERTIES", 1), "an object opened but not changed isn't a change");
    sun["objdata"]!["Cost"] = 25;
    sun["objdata"]!["Cost"] = 50;
    Expect(!edits.IsChanged("PLANTPROPERTIES", 1), "changed back is no change");
    var lc = edits.Edit("LIVECONFIG", 0);
    lc["objdata"]!["A"] = 2;

    string mods = Path.Combine(root, "mods1");
    var result = edits.Save(mods, new ModInfo("Cheaper Peashooter", "test", "me"));
    Expect(result.Files.Count == 2, $"two sheets written ({string.Join(", ", result.Files)})");
    var patch = J(File.ReadAllText(Path.Combine(result.Dir, "obb", "PACKAGES", "PLANTPROPERTIES.json")));
    var objs = (JsonArray)patch["objects"]!;
    Expect(objs.Count == 1, "only the changed object");
    Expect(JsonNode.DeepEquals(objs[0], J("""{"objclass":"PeashooterProps","aliases":["PeashooterDefault"],"objdata":{"Cost":125,"Shot":{"Delay":1.25}}}""")),
        $"only the changed fields: {objs[0]?.ToJsonString()}");
    var mod = J(File.ReadAllText(Path.Combine(result.Dir, "mod.json")));
    Expect(mod["games"]![0]!.GetValue<string>() == "com.example.test" && mod["versions"]![0]!.GetValue<string>() == "1.0", "the mod is for this game and version");
    Expect(mod["kind"]!.GetValue<string>() == "content", "a content mod");

    // the launcher's merge of the mod over the game's sheet gives the edited object back
    var merged = (JsonObject)sheets["PLANTPROPERTIES"].DeepClone();
    SheetMerge.Merge(merged, patch);
    Expect(JsonNode.DeepEquals(merged["objects"]![0], edits.Current("PLANTPROPERTIES", 0)), "merged over the game's sheet it is the edited object");
    var lcMerged = (JsonObject)sheets["LIVECONFIG"].DeepClone();
    SheetMerge.Merge(lcMerged, J(File.ReadAllText(Path.Combine(result.Dir, "obb", "PACKAGES", "LIVECONFIG.json"))));
    Expect(lcMerged["objects"]![0]!["objdata"]!["A"]!.GetValue<int>() == 2, "an object without a name, the only one of its class, is found by its class");

    // taken up again: the same edits
    var again = new EditSession(game);
    var notes = again.LoadMod(result.Dir);
    Expect(notes.Count == 0, $"no notes ({string.Join("; ", notes)})");
    Expect(again.Changed.Count == 2 && JsonNode.DeepEquals(again.Current("PLANTPROPERTIES", 0), edits.Current("PLANTPROPERTIES", 0)), "the mod loads back as the same edits");

    // saved again without the LIVECONFIG change: its file goes
    edits.Revert("LIVECONFIG", 0);
    var second = edits.Save(mods, new ModInfo("Cheaper Peashooter"));
    Expect(!File.Exists(Path.Combine(second.Dir, "obb", "PACKAGES", "LIVECONFIG.json")), "a sheet no longer changed is taken out of the mod");
    Expect(File.Exists(Path.Combine(second.Dir, "obb", "PACKAGES", "PLANTPROPERTIES.json")), "the other stays");
}

void WholeSheet()
{
    var sheets = Sheets();
    using var game = Game.FromSheets(sheets);
    var edits = new EditSession(game);
    // the second of two nameless objects of one class can't be found by a merge
    edits.Edit("LIVECONFIG", 2)["objdata"]!["B"] = 9;
    // a field taken away can't be said by a merge
    ((JsonObject)edits.Edit("PLANTPROPERTIES", 2)["objdata"]!).Remove("Size");
    var result = edits.Save(Path.Combine(root, "mods2"), new ModInfo("Whole"));
    Expect(result.Notes.Count == 2, $"both said why ({string.Join("; ", result.Notes)})");
    var live = J(File.ReadAllText(Path.Combine(result.Dir, "obb", "PACKAGES", "LIVECONFIG.json")));
    Expect(live[SheetMerge.ReplaceKey]?.GetValue<bool>() == true && live["objects"]!.AsArray().Count == 3, "the whole sheet, replacing the game's");
    Expect(live["objects"]![2]!["objdata"]!["B"]!.GetValue<int>() == 9, "with the change");
    var props = J(File.ReadAllText(Path.Combine(result.Dir, "obb", "PACKAGES", "PLANTPROPERTIES.json")));
    Expect(props["objects"]![2]!["objdata"]!["Size"] == null && props["objects"]!.AsArray().Count == 3, "the field is gone in the whole sheet");
}

void Drawing()
{
    var m = new Affine(0, 1, -1, 0, 10, 20);  // 90 degrees, then moved
    var (x, y) = m.Apply(1, 0);
    Expect(Math.Abs(x - 10) < 1e-9 && Math.Abs(y - 21) < 1e-9, "a quarter turn");
    var inv = m.Inverse()!.Value;
    var (bx, by) = inv.Apply(x, y);
    Expect(Math.Abs(bx - 1) < 1e-9 && Math.Abs(by) < 1e-9, "and back");
    var t = Affine.Scale(2, 2).Then(Affine.Translate(5, 0));
    Expect(t.Apply(1, 1) == (7, 2), "scale then move");

    var red = new Picture(2, 2, [255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255]);
    var canvas = new Canvas(8, 8);
    canvas.Draw(red, Affine.Scale(2, 2).Then(Affine.Translate(2, 2)), [1, 1, 1, 1]);
    var p = canvas.ToPicture();
    int At(int px, int py, int c) => p.Rgba[(py * 8 + px) * 4 + c];
    Expect(At(3, 3, 0) == 255 && At(3, 3, 3) == 255, "drawn where it was put");
    Expect(At(0, 0, 3) == 0 && At(7, 7, 3) == 0, "nothing elsewhere");
    canvas.Draw(red, Affine.Scale(4, 4), [0, 0, 1, 0.5f]);
    var q = canvas.ToPicture();
    Expect(q.Rgba[(1 * 8 + 1) * 4 + 3] is > 120 and < 135, "half see-through where nothing was before");
    Expect(q.Crop(2, 2, 2, 2).Width == 2 && q.Crop(6, 6, 5, 5).Width == 2, "a crop, clipped");
    // a picture's see-through edge keeps its colour (a dark outline stays dark, not bright)
    var edge = new Canvas(1, 1);
    edge.Draw(new Picture(1, 1, [0, 80, 0, 77]), Affine.Identity, [1, 1, 1, 1]);
    var e = edge.ToPicture();
    Expect(Math.Abs(e.Rgba[1] - 80) <= 1 && Math.Abs(e.Rgba[3] - 77) <= 1, $"a see-through pixel keeps its colour (got {e.Rgba[0]},{e.Rgba[1]},{e.Rgba[2]},{e.Rgba[3]})");
    var over = new Canvas(1, 1);
    over.Draw(new Picture(1, 1, [255, 255, 255, 255]), Affine.Identity, [1, 1, 1, 1]);
    over.Draw(new Picture(1, 1, [0, 0, 0, 128]), Affine.Identity, [1, 1, 1, 1]);
    Expect(over.ToPicture().Rgba[0] is >= 125 and <= 130, "half black over white is grey");
    var png = Picture.FromPng(q.ToPng());
    Expect(png.Width == 8 && png.Rgba.SequenceEqual(q.Rgba), "PNG both ways");
}

void GameNames()
{
    Expect(Game.FromObbName(@"C:\g\main.675.com.ea.game.pvz2_rfl.obb") == ("com.ea.game.pvz2_rfl", "675"), "package from the OBB's name");
    Expect(Game.FromObbName("whatever.obb") == ("", ""), "no package in another name");
    Expect(Game.FamilyOf("com.ea.game.pvz2_row") == "pvz2" && Game.FamilyOf("com.ea.game.pvz2_rfl") == "reflourished" && Game.FamilyOf("com.snowie.altverz") == "other", "families");
    Expect(EditSession.FolderName("a/b:c?") == "a_b_c_", "a folder name from a mod's name");
}

void TypedValues()
{
    Expect(FieldValues.Parse("125", FieldKind.Int).Value?.GetValue<int>() == 125, "a whole number");
    Expect(FieldValues.Parse("1,5", FieldKind.Float).Error != null, "a comma isn't a decimal point");
    Expect(FieldValues.Parse("7.5", FieldKind.Float).Value?.GetValue<double>() == 7.5, "a decimal");
    Expect(FieldValues.Parse("300", FieldKind.Float).Value?.ToJsonString() == "300", "a whole float stays whole, as the game writes it");
    Expect(FieldValues.Parse("true", FieldKind.Bool).Value?.GetValue<bool>() == true, "true");
    Expect(FieldValues.Parse("Pea", FieldKind.Rtid).Error != null && FieldValues.Parse("RTID(Pea@ProjectileTypes)", FieldKind.Rtid).Error == null, "an RTID");
    Expect(FieldValues.Parse("[1, 2", FieldKind.Array).Error != null && FieldValues.Parse("[1, 2]", FieldKind.Array).Value is JsonArray { Count: 2 }, "JSON for a list");
    Expect(FieldValues.Format(JsonValue.Create("x")) == "x" && FieldValues.Format(JsonValue.Create(1.5)) == "1.5", "shown as typed");
}

void RealProfile()
{
    using var game = Game.Open(obb!);
    var p = GameProfile.Build(game);
    Console.WriteLine($"  {game.Name}: {p[GameProfile.Plants].Count} plants, {p[GameProfile.Zombies].Count} zombies, {p[GameProfile.GridItems].Count} grid items, " +
                      $"{p[GameProfile.Projectiles].Count} projectiles, {p[GameProfile.LevelModules].Count} level modules");
    Expect(p[GameProfile.Plants].Count > 30 && p[GameProfile.Zombies].Count > 30, "plants and zombies");
    Expect(p.Find(GameProfile.Plants, "peashooter")?.DisplayName == "Peashooter", "Peashooter by name");
    var schema = Schema.Infer(game);
    Console.WriteLine($"  schema: {schema.Classes.Count} classes from {schema.ObjectCount} objects in {schema.Sheets} sheets");
    Expect(schema.Class("PlantType")?.Fields.ContainsKey("PlantFramework") == true, "PlantType's fields");
    Expect(schema.Class("ZombiePropertySheet")?.Fields["Hitpoints"].Main is FieldKind.Int or FieldKind.Float, "a zombie's hitpoints are a number");
    string? dump = Environment.GetEnvironmentVariable("PVZ2_DUMP");
    if (dump != null && File.Exists(dump))
    {
        int n = schema.ApplyDump((JsonObject)JsonNode.Parse(File.ReadAllText(dump))!);
        Console.WriteLine($"  class dump: {n} classes");
        Expect(schema.Class("ZombiePropertySheet")!.Fields["Hitpoints"].Main == FieldKind.Float, "the dump makes hitpoints a float");
    }
}

void RealPreviews()
{
    using var game = Game.Open(obb!);
    var res = Resources.Load(game);
    var p = GameProfile.Build(game);
    Expect(res.GroupCount > 100, "the resource manifest");
    int drawn = 0, tried = 0;
    foreach (var e in p[GameProfile.Plants].Take(20).Concat(p[GameProfile.Zombies].Take(20)))
    {
        if (e.PopAnim == null) continue;
        tried++;
        if (PamPreview.Render(res, e.PopAnim, out _) is { Width: > 10, Height: > 10 }) drawn++;
    }
    Console.WriteLine($"  {drawn} of {tried} previews drawn");
    Expect(drawn * 10 >= tried * 8, "most previews draw");
    var pea = res.ImagesIn("PlantPeashooter");
    Expect(pea.Count > 5 && res.Image(pea[0].Id) is { Width: > 0 }, "a group's pictures cut from its atlas");
}

void RealEditAndLauncher()
{
    // a launcher games folder for the OBB, as "Add game" leaves it
    string gameDir = Path.Combine(root, "launcher", "games", "Test");
    Directory.CreateDirectory(gameDir);
    string link = Path.Combine(gameDir, Path.GetFileName(obb!));
    File.CreateSymbolicLink(link, obb!);
    using var game = Game.OpenFolder(gameDir);
    var p = GameProfile.Build(game);
    var pea = p.Find(GameProfile.Plants, "peashooter")!;
    var props = p.Resolve(pea.Properties!, pea.Sheet)!.Value;
    var edits = new EditSession(game);
    var obj = edits.Edit(props.Sheet, props.Index);
    int cost = obj["objdata"]!["Cost"]!.GetValue<int>();
    obj["objdata"]!["Cost"] = cost + 25;
    var saved = edits.Save(game.Folder!.ModsDir, new ModInfo("Builder test"));
    Expect(saved.Files.SequenceEqual(["obb/PACKAGES/PLANTPROPERTIES.json"]), $"one sheet ({string.Join(", ", saved.Files)})");

    // the launcher lists it for this game and builds it into the OBB patch rfu reads
    var mod = Mods.List(game.Folder).FirstOrDefault(m => m.Name == "Builder test");
    Expect(mod != null, "the launcher lists the mod");
    if (mod == null) return;
    Expect(mod.HasContent && mod.Problem == null, $"as content it can use ({mod.Problem})");
    var build = Mods.Build(game.Folder, [mod.Key]);
    Console.WriteLine("  launcher: " + string.Join(" | ", build.Log.Where(l => l.Contains("PLANTPROPERTIES", StringComparison.OrdinalIgnoreCase) || l.Contains("FAIL"))));
    Expect(build.ObbPatch != null && File.Exists(build.ObbPatch), "an OBB patch was built");
    Expect(!build.Log.Any(l => l.Contains("FAIL")), "with no failures");

    // what the game reads through the patch: the OBB with the patch applied has the new cost
    string patched = Path.Combine(root, "patched.obb");
    ObbPatch.Apply(obb!, File.ReadAllBytes(build.ObbPatch!), patched);
    using var after = ObbArchive.Open(patched);
    var sheet = SheetMerge.Parse(after.Read(@"PACKAGES\PLANTPROPERTIES.RTON")!) as JsonObject;
    var now = sheet!["objects"]![props.Index]!;
    Expect(now["objdata"]!["Cost"]!.GetValue<int>() == cost + 25, "the game reads the new cost");
    Expect(JsonNode.DeepEquals(now, edits.Current(props.Sheet, props.Index)), "and the object is exactly the edited one");
}
