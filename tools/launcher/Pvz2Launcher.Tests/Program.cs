using System.Text;
using System.Text.Json;
using Pvz2Launcher;

// Launcher tests: game folders, mods, the update check and rfu's command line, on a scratch folder.

int failures = 0;
void Expect(bool ok, string what)
{
    if (ok) return;
    Console.WriteLine($"  FAILED: {what}");
    failures++;
}

string root = Path.Combine(Path.GetTempPath(), "pvz2launcher_tests");
if (Directory.Exists(root)) Directory.Delete(root, true);

var tests = new (string Name, Action Run)[]
{
    ("rton round trip", RtonRoundTrip),
    ("versions", Versions),
    ("game folder", GameFolderTest),
    ("mods", ModsTest),
    ("level pack", LevelPackTest),
    ("update check", UpdateCheckTest),
    ("rfu command", RfuCommandTest),
};
foreach (var (name, run) in tests)
{
    int before = failures;
    try { run(); }
    catch (Exception e) { Console.WriteLine($"  exception: {e}"); failures++; }
    Console.WriteLine($"{(failures == before ? "[ ok ]" : "[FAIL]")} {name}");
}
Directory.Delete(root, true);
return failures == 0 ? 0 : 1;

static string ToJson(byte[] rton)
{
    using var ms = new MemoryStream();
    using (var w = new Utf8JsonWriter(ms)) Pvz2Conv.Rton.ToJson(rton, w, []);
    return Encoding.UTF8.GetString(ms.ToArray());
}

void RtonRoundTrip()
{
    const string json = """
        {"version":1,"objects":[{"aliases":["A"],"objclass":"ForceUpdateConfigData","objdata":{"MinVersion":"1.4.2","n":0,"neg":-5,
        "big":5000000000,"f":0.5,"d":0.1,"t":true,"nil":null,"s":"héllo","s2":"héllo","r":"RTID(Peashooter@PlantTypes)",
        "r0":"RTID(0)","rn":"RTID(1.2.0000abcd@Sheet)","arr":[1,"A",[]]}}]}
        """;
    byte[] rton = Pvz2Conv.RtonWriter.FromJson(Encoding.UTF8.GetBytes(json));
    Expect(Pvz2Conv.Rton.LooksLike(rton), "RTON signature");
    using var a = JsonDocument.Parse(json);
    using var b = JsonDocument.Parse(ToJson(rton));
    Expect(JsonSerializer.Serialize(a) == JsonSerializer.Serialize(b), $"round trip: {ToJson(rton)}");
}

void Versions()
{
    Expect(UpdateCheck.CompareVersions("1.4.3", "1.4.2") > 0, "1.4.3 > 1.4.2");
    Expect(UpdateCheck.CompareVersions("1.4", "1.4.0") == 0, "1.4 == 1.4.0");
    Expect(UpdateCheck.CompareVersions("1.10.0", "1.9.9") > 0, "1.10.0 > 1.9.9");
    Expect(UpdateCheck.CompareVersions("1.4.2", "1.5.0b") < 0, "1.4.2 < 1.5.0b");
}

GameFolder MakeGame(string name, string version = "1.4.2")
{
    string dir = Path.Combine(root, "games", name);
    Directory.CreateDirectory(Path.Combine(dir, "lib", "arm64-v8a"));
    File.WriteAllText(Path.Combine(dir, "lib", "arm64-v8a", "libPVZ2.so"), "");
    File.WriteAllText(Path.Combine(dir, "main.675.com.ea.game.pvz2_rfl.obb"), "");
    File.WriteAllText(Path.Combine(dir, "_apk_manifest.json"),
        $$"""{"package":"com.ea.game.pvz2_rfl","version_name":"{{version}}","version_code":"675","obb":"main.675.com.ea.game.pvz2_rfl.obb"}""");
    return GameFolder.Open(dir);
}

void GameFolderTest()
{
    var g = MakeGame("Reflourished");
    Expect(g.Problem() == null, $"no problem: {g.Problem()}");
    Expect(g.DisplayName == "PvZ2 Reflourished" && g.Settings.CdnUrl!.Contains("{version}"), "Reflourished defaults");
    Expect(File.Exists(g.SettingsPath), "settings saved once the APK is known");
    File.Move(g.Obb!, Path.Combine(g.Dir, "main.700.com.ea.game.pvz2_rfl.obb"));
    g.Refresh();
    Expect(g.Problem()?.Contains("expects main.675") == true, "OBB from another version");
    File.Move(g.Obb!, Path.Combine(g.Dir, "main.675.com.ea.game.pvz2_rfl.obb"));

    Directory.CreateDirectory(g.PatchesDir);
    File.WriteAllText(Path.Combine(g.PatchesDir, "fix.patch"), "# name: Fix\n# description: does a thing\nlibPVZ2.so+0x10 00\n");
    var p = g.PatchFiles();
    Expect(p.Count == 1 && p[0].Name == "Fix" && p[0].Description == "does a thing", "patch header");

    g.EnsureKeybindsFile();
    Expect(File.ReadAllText(g.KeybindsPath).Contains("tap"), "keybinds sample");
    Expect(File.ReadAllText(g.KeybindsPath).Contains("1 seed 1") && File.ReadAllText(g.KeybindsPath).Contains("D shovel") && File.ReadAllText(g.KeybindsPath).Contains("Right scroll right") && File.ReadAllText(g.KeybindsPath).Contains("P pause"), "keybinds sample: seeds, buttons and scrolling on");

    Expect(g.ModLibraries.Count == 0, "no mod libraries listed");
    File.WriteAllText(Path.Combine(g.Dir, "_apk_manifest.json"),
        """{"package":"com.ea.game.pvz2_rfl","version_name":"1.4.2","version_code":"675","obb":"main.675.com.ea.game.pvz2_rfl.obb","mod_libraries":["libSnowie.so"]}""");
    g.Refresh();
    Expect(g.ModLibraries.SequenceEqual(["libSnowie.so"]), "mod libraries from the manifest");

    var other = Path.Combine(root, "games", "AltverZ");
    Directory.CreateDirectory(other);
    var alt = GameFolder.Open(other);
    Expect(alt.Problem()?.Contains(".obb") == true, "empty game folder asks for an OBB");
    Expect(GameFolder.Scan(Path.Combine(root, "games")).Count == 2, "scan");
}

void ModsTest()
{
    var g = MakeGame("ModGame");
    // the game's own CDN folder: strings are RTON, levels JSON
    string cdn = Path.Combine(g.DataDir, "external", "files", "No_Backup", "CDN.1.4.2");
    Directory.CreateDirectory(Path.Combine(cdn, "levels"));
    Directory.CreateDirectory(Path.Combine(cdn, "strings"));
    File.WriteAllText(Path.Combine(cdn, "levels", "egypt1.json"), "{}");
    File.WriteAllBytes(Path.Combine(cdn, "strings", "LawnStrings-en-US.rton"), Pvz2Conv.RtonWriter.FromJson("""{"a":"b"}"""u8));

    // a mod laid out as CDN content directly, another with cdn/ and files/
    string src1 = Path.Combine(root, "src", "Levels Pack");
    Directory.CreateDirectory(Path.Combine(src1, "levels"));
    File.WriteAllText(Path.Combine(src1, "levels", "egypt1.json"), """{"from":"mod1"}""");
    File.WriteAllText(Path.Combine(src1, "levels", "custom1.json"), """{"new":true}""");
    File.WriteAllText(Path.Combine(src1, "mod.json"), """{"name":"Levels","author":"me","version":"1.0"}""");
    string src2 = Path.Combine(root, "src", "Strings");
    Directory.CreateDirectory(Path.Combine(src2, "cdn", "strings"));
    Directory.CreateDirectory(Path.Combine(src2, "cdn", "levels"));
    Directory.CreateDirectory(Path.Combine(src2, "files"));
    File.WriteAllText(Path.Combine(src2, "cdn", "strings", "lawnstrings-en-us.json"), """{"a":"modded"}""");
    File.WriteAllText(Path.Combine(src2, "cdn", "levels", "egypt1.json"), """{"from":"mod2"}""");
    File.WriteAllText(Path.Combine(src2, "cdn", "levels", "extra.rton.json"), """{"x":1}""");
    File.WriteAllText(Path.Combine(src2, "files", "note.txt"), "hi");

    Expect(Mods.Classify(src1) == Mods.ContentKind.Mod, "classify mod");
    string n1 = Mods.Import(g, src1), n2 = Mods.Import(g, src2);
    Expect(Mods.Import(g, src1) == "Levels Pack (2)", "a second import gets its own name");
    var list = Mods.List(g);
    Expect(list.Any(m => m.Name == "Levels" && m.Author == "me"), "mod.json read");

    var r = Mods.Build(g, [n1, n2]);
    string outDir = Path.Combine(g.WorkDir, "overlay");
    Expect(r.Overlays.Count == 2, $"two overlays: {string.Join(", ", r.Overlays)}");
    Expect(r.Overlays.Any(o => o.Guest == "/sdcard/Android/data/com.ea.game.pvz2_rfl/files/No_Backup/CDN.1.4.2"), "CDN guest path");
    Expect(File.ReadAllText(Path.Combine(outDir, "cdn", "levels", "egypt1.json")).Contains("mod2"), "later mod wins");
    Expect(File.Exists(Path.Combine(outDir, "cdn", "levels", "custom1.json")), "new level copied");
    Expect(!File.Exists(Path.Combine(outDir, "cdn", "mod.json")), "mod.json left out");
    string strings = Path.Combine(outDir, "cdn", "strings", "LawnStrings-en-US.rton");
    Expect(File.Exists(strings) && ToJson(File.ReadAllBytes(strings)).Contains("modded"), "JSON over an RTON becomes RTON with the game's spelling");
    Expect(File.Exists(Path.Combine(outDir, "cdn", "levels", "extra.rton")), ".rton.json becomes .rton");
    Expect(File.ReadAllText(Path.Combine(outDir, "files", "note.txt")) == "hi", "files/ root");

    var again = Mods.Build(g, [n1, n2]);
    Expect(again.Log.Any(l => l.Contains("unchanged")), "unchanged mods aren't rebuilt");
    Mods.Build(g, [n2, n1]);
    Expect(File.ReadAllText(Path.Combine(outDir, "cdn", "levels", "egypt1.json")).Contains("mod1"), "load order");
    Expect(Mods.Build(g, []).Overlays.Count == 0, "no mods, no overlays");
}

// A level pack as people share them (Unconnected Nights): loose level files and a LawnStrings.
void LevelPackTest()
{
    var g = MakeGame("PackGame");
    string cdn = Path.Combine(g.DataDir, "external", "files", "No_Backup", "CDN.1.4.2");
    Directory.CreateDirectory(Path.Combine(cdn, "levels"));
    File.WriteAllBytes(Path.Combine(cdn, "LawnStrings-en-US.rton"), Pvz2Conv.RtonWriter.FromJson("""
        {"version":1,"objects":[{"aliases":["LawnStringsData"],"objclass":"LawnStringsData","objdata":{"LocStringValues":
        ["PLAY","Play","QUIT","Quit"]}}]}
        """u8));
    string src = Path.Combine(root, "src", "Unconnected Nights");
    Directory.CreateDirectory(src);
    File.WriteAllText(Path.Combine(src, "un1.json"), """{"objects":[{"objclass":"LevelDefinition","objdata":{}}]}""");
    File.WriteAllText(Path.Combine(src, "README.txt"), "put the levels in levels/");
    File.WriteAllBytes(Path.Combine(src, "LawnStrings-en-us.rton"), Pvz2Conv.RtonWriter.FromJson("""
        {"version":1,"objects":[{"aliases":["LawnStringsData"],"objclass":"LawnStringsData","objdata":{"LocStringValues":
        ["QUIT","Leave","UN_DIALOGUE_1","Hot out here!"]}}]}
        """u8));
    string name = Mods.Import(g, src);
    var r = Mods.Build(g, [name]);
    string outDir = Path.Combine(g.WorkDir, "overlay", "cdn");
    Expect(File.Exists(Path.Combine(outDir, "levels", "un1.json")), "a loose level goes to levels/");
    Expect(!File.Exists(Path.Combine(outDir, "README.txt")) && !File.Exists(Path.Combine(outDir, "levels", "README.txt")), "readme left out");
    string strings = Path.Combine(outDir, "LawnStrings-en-US.rton");
    Expect(File.Exists(strings), $"strings under the game's spelling: {string.Join(", ", r.Log)}");
    if (File.Exists(strings))
    {
        using var doc = JsonDocument.Parse(ToJson(File.ReadAllBytes(strings)));
        var values = doc.RootElement.GetProperty("objects")[0].GetProperty("objdata").GetProperty("LocStringValues").EnumerateArray().Select(e => e.GetString()).ToList();
        Expect(values.SequenceEqual(["PLAY", "Play", "QUIT", "Leave", "UN_DIALOGUE_1", "Hot out here!"]), $"merged strings: {string.Join("|", values)}");
    }
    // the game downloads new strings: the next build merges over those
    File.WriteAllBytes(Path.Combine(cdn, "LawnStrings-en-US.rton"), Pvz2Conv.RtonWriter.FromJson("""
        {"version":1,"objects":[{"objclass":"LawnStringsData","objdata":{"LocStringValues":["PLAY","Play!","NEW","New"]}}]}
        """u8));
    File.SetLastWriteTimeUtc(Path.Combine(cdn, "LawnStrings-en-US.rton"), DateTime.UtcNow.AddMinutes(1));
    Mods.Build(g, [name]);
    Expect(ToJson(File.ReadAllBytes(strings)).Contains("New") && ToJson(File.ReadAllBytes(strings)).Contains("Hot out here!"), "rebuilt over new game strings");
}

void UpdateCheckTest()
{
    byte[] sheet = Pvz2Conv.RtonWriter.FromJson("""
        {"version":1,"objects":[{"aliases":["Events"],"objclass":"LiveEventSchedule","objdata":{}},
        {"aliases":["ForceUpdate"],"objclass":"ForceUpdateConfigData","objdata":{"MinVersion":"1.4.3","AndroidURL":""}}]}
        """u8);
    string tar = Path.Combine(root, "cdn.tar");
    using (var fs = File.Create(tar))
    {
        WriteTarEntry(fs, "levels/egypt1.json", "{}"u8.ToArray());
        WriteTarEntry(fs, "liveconfig/" + new string('x', 120) + ".rton", sheet);  // a GNU long name
        fs.Write(new byte[1024]);
    }
    Expect(UpdateCheck.MinVersionFromArchive(tar) == "1.4.3", "MinVersion from an RTON in a tar");
    Expect(UpdateCheck.MinVersionFromFile("""{"objects":[{"objclass":"ForceUpdateConfigData","objdata":{"MinVersion":"2.0"}}]}"""u8.ToArray()) == "2.0", "from JSON");
    Expect(UpdateCheck.MinVersionFromFile("""{"objects":[{"objclass":"PinataParty"}]}"""u8.ToArray()) == null, "events aren't updates");

    // offline: the archive the game downloaded on its last run
    var g = MakeGame("UpdateGame");
    g.Settings.CdnUrl = "http://127.0.0.1:9/{version}.main.tar";
    string cdn = Path.Combine(g.DataDir, "external", "files", "No_Backup", "CDN.1.4.2");
    Directory.CreateDirectory(cdn);
    File.Copy(tar, Path.Combine(cdn, "tar.full"));
    var result = UpdateCheck.RunAsync(g, new HttpClient { Timeout = TimeSpan.FromSeconds(5) }).GetAwaiter().GetResult();
    Expect(result.Outcome == UpdateCheck.Outcome.UpdateAvailable && result.MinVersion == "1.4.3", $"offline fallback: {result.Message}");
    g.Settings.CdnUrl = "";
    result = UpdateCheck.RunAsync(g, new HttpClient()).GetAwaiter().GetResult();
    Expect(result.Outcome == UpdateCheck.Outcome.Unknown, "no CDN address, no check");
}

static void WriteTarEntry(Stream s, string name, byte[] data)
{
    void Header(string n, long size, char type)
    {
        var h = new byte[512];
        Encoding.ASCII.GetBytes(n.Length > 99 ? n[..99] : n).CopyTo(h, 0);
        Encoding.ASCII.GetBytes(Convert.ToString(size, 8).PadLeft(11, '0')).CopyTo(h, 124);
        h[156] = (byte)type;
        Encoding.ASCII.GetBytes("ustar ").CopyTo(h, 257);
        s.Write(h);
    }
    void Body(byte[] b)
    {
        s.Write(b);
        s.Write(new byte[(512 - b.Length % 512) % 512]);
    }
    if (name.Length > 99)
    {
        var n = Encoding.UTF8.GetBytes(name + "\0");
        Header("././@LongLink", n.Length, 'L');
        Body(n);
    }
    Header(name, data.Length, '0');
    Body(data);
}

void RfuCommandTest()
{
    var g = MakeGame("CmdGame");
    g.Settings.Assets = "low";
    g.Settings.Resolution = "1280x720";
    g.Settings.Offline = true;
    g.Settings.Keybinds = true;
    g.EnsureKeybindsFile();
    Directory.CreateDirectory(g.PatchesDir);
    File.WriteAllText(Path.Combine(g.PatchesDir, "a.patch"), "");
    g.Settings.Patches = ["a.patch", "gone.patch"];
    var args = RfuCommand.Build(g, [new Mods.Overlay("/x", "/host/x")]);
    string line = string.Join(" ", args);
    foreach (var want in new[] { "--assets low", "--resolution 1280x720", "--offline", "--package com.ea.game.pvz2_rfl",
                                 "--version-code 675", "--version-name 1.4.2", "--overlay /x=/host/x", "--keybinds", "--key-hints", "--hover", "--fix-animations" })
        Expect(line.Contains(want), $"rfu gets {want}: {line}");
    Expect(args.Count(a => a == "--patch") == 1, "only patches that exist");
    Expect(!args.Contains("--no-mod-libs"), "mod libraries always load");
    g.Settings.HoverCollect = false;
    Expect(!RfuCommand.Build(g, []).Contains("--hover"), "hover to collect can be switched off");
    g.Settings.FixAnimations = false;
    Expect(!RfuCommand.Build(g, []).Contains("--fix-animations"), "the animations fix can be switched off");
    g.Settings.KeyHints = false;
    Expect(!RfuCommand.Build(g, []).Contains("--key-hints"), "keybind hints can be switched off");
    g.Settings.KeyHints = true;
    g.Settings.Keybinds = false;
    Expect(!RfuCommand.Build(g, []).Contains("--key-hints"), "no hints without keybinds");
    Expect(RfuCommand.IsSize("1920x1080") && !RfuCommand.IsSize("big") && !RfuCommand.IsSize("10x10"), "sizes");
}
