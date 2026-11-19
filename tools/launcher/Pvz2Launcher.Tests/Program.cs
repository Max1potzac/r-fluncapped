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
BuiltinMods.Seed(Path.Combine(root, "mods"));

var tests = new (string Name, Action Run)[]
{
    ("rton round trip", RtonRoundTrip),
    ("versions", Versions),
    ("game folder", GameFolderTest),
    ("china game", ChinaGameTest),
    ("mods", ModsTest),
    ("level pack", LevelPackTest),
    ("update check", UpdateCheckTest),
    ("obb update", ObbUpdateTest),
    ("rfu command", RfuCommandTest),
    ("mod library", ModLibraryTest),
    ("legacy settings", LegacySettingsTest),
    ("localization", LocalizationTest),
    ("keybind layout", KeybindLayoutTest),
    ("controller setup", PadSetupTest),
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
    Expect(LaunchFlow.VersionInName("PvZ2 Reflourished 1.5.0") == "1.5.0", "version in a name");
    Expect(LaunchFlow.VersionInName("PvZ2 Reflourished") == null, "no version in a name");
    Expect(LaunchFlow.RenamedForVersion("PvZ2 Reflourished 1.4.2", "1.5.0") == "PvZ2 Reflourished 1.5.0", "renamed for 1.5.0");
    Expect(LaunchFlow.RenamedForVersion("PvZ2 Reflourished 1.5.0", "1.5.0") == null, "same version: no rename");
    Expect(LaunchFlow.RenamedForVersion("My PvZ2", "1.5.0") == null, "no version: no rename");
    Expect(LaunchFlow.RenamedForVersion("PvZ2 Reflourished 1.4.2", null) == null, "APK name without a version: no rename");
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

// PvZ2 China: libSrc.so, an APK, no OBB. The manifest's obb is null (pvz2conv writes it null).
GameFolder MakeChinaGame(string name)
{
    string dir = Path.Combine(root, "games", name);
    Directory.CreateDirectory(Path.Combine(dir, "lib", "arm64-v8a"));
    File.WriteAllText(Path.Combine(dir, "lib", "arm64-v8a", "libSrc.so"), "");
    File.WriteAllText(Path.Combine(dir, "pvz2cthdlx.apk"), "");
    File.WriteAllText(Path.Combine(dir, "_apk_manifest.json"),
        """{"package":"com.popcap.pvz2cthdlx","version_name":"4.2.4","version_code":"1875","obb":null}""");
    return GameFolder.Open(dir);
}

void ChinaGameTest()
{
    var g = MakeChinaGame("PVZ2C");
    Expect(g.IsChina, "detected as PvZ2 China");
    Expect(g.LibrariesReady, "libSrc.so counts as the game's library");
    Expect(g.Problem() == null, $"no OBB needed for China: {g.Problem()}");
    var args = RfuCommand.Build(g, []);
    Expect(args.Contains("--apk") && !args.Contains("--obb"), $"China runs with --apk, not --obb: {string.Join(" ", args)}");
    int ai = args.IndexOf("--apk");
    Expect(ai >= 0 && args[ai + 1].EndsWith(".apk"), "the APK path follows --apk");
    Expect(g.Settings.Offline, "China defaults to offline");
    Expect(args.Contains("--offline") && args.Contains("--cloud-stub"), $"China offline gets the cloud stub: {string.Join(" ", args)}");
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
    Expect(File.ReadAllText(g.KeybindsPath).Contains("0 seed 10"), "keybinds sample: ten seed slots");
    File.WriteAllText(g.KeybindsPath, File.ReadAllText(g.KeybindsPath).Replace("0 seed 10\n", ""));
    g.EnsureKeybindsFile();
    Expect(File.ReadAllLines(g.KeybindsPath).SkipWhile(l => l != "9 seed 9").Skip(1).FirstOrDefault() == "0 seed 10", "an older keybinds file gets 0 for the tenth slot");
    g.EnsureKeybindsFile();
    Expect(File.ReadAllLines(g.KeybindsPath).Count(l => l == "0 seed 10") == 1, "only once");
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

void ObbUpdateTest()
{
    var g = MakeGame("ObbGame");
    Expect(ObbUpdate.ConfigUrl(g) == null, "an APK that doesn't update its OBB (1.4.2) gets no updates");
    string state = ObbUpdate.StateDir(g);
    Expect(state.EndsWith(Path.Combine("sdcard", "Android", "obb", "com.ea.game.pvz2_rfl")), $"the game's OBB folder: {state}");

    // three versions: 30 (full + patch from 20), 20 (patch from 10 only), 10 (full)
    string Sha(string text) => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes(text))).ToLowerInvariant();
    var json = new Dictionary<string, string>
    {
        ["https://x.test/live/latest.json"] = $$"""{"time":30,"version":"1.5.2","full":"https://x.test/30.obb","full_sha256":"{{Sha("v30")}}","full_size":3,"patch":"https://x.test/30.patch","patch_sha256":"aa","patch_size":5,"prev":20}""",
        ["https://x.test/20.json"] = $$"""{"time":20,"version":"1.5.1","full_sha256":"{{Sha("v20")}}","patch":"https://x.test/20.patch","patch_size":7,"prev":10}""",
        ["https://x.test/10.json"] = $$"""{"time":10,"version":"1.5.0","full":"https://x.test/10.obb","full_sha256":"{{Sha("v10")}}","full_size":3}""",
    };
    var log = new List<string>();
    (ObbUpdate.Node, ObbUpdate.Plan) Plan(string? obbText)
    {
        File.WriteAllText(g.Obb!, obbText ?? "");
        return ObbUpdate.PlanAsync("https://x.test/live/latest.json", obbText == null ? null : g.Obb, state,
            url => Task.FromResult(json[url]), async path => await ObbUpdate.Sha256Async(path), log.Add).GetAwaiter().GetResult();
    }
    Expect(ObbUpdate.Node.Parse(json["https://x.test/20.json"], "https://x.test/live/latest.json").PrevUrl == "https://x.test/10.json", "prev is /<time>.json on the server");

    var (head, plan) = Plan("v30");
    Expect(head.Time == 30 && plan is ObbUpdate.UpToDate, $"newest OBB: {plan}");
    (_, plan) = Plan("v10");
    Expect(plan is ObbUpdate.Patches p && p.Chain.Select(n => n.Time).SequenceEqual([20L, 30L]), $"two behind: patches oldest first: {plan}");
    Expect(ObbUpdate.DownloadSize(plan) == 12, "patch sizes add up");
    (_, plan) = Plan("modded");
    Expect(plan is ObbUpdate.Fresh f && f.Base.Time == 30 && f.Chain.Count == 0, $"unknown OBB: the newest full one: {plan}");
    (_, plan) = Plan(null);
    Expect(plan is ObbUpdate.Fresh { Base.Time: 30 }, "no OBB: the newest full one");

    ObbUpdate.WriteTimestamp(state, 30);
    (_, plan) = Plan("modded");
    Expect(plan is ObbUpdate.UpToDate, "obbTimestamp at the newest: no hashing, up to date");
    ObbUpdate.ForgetTimestamp(g);
    Expect(ObbUpdate.ReadTimestamp(state) == 0, "forgotten");

    // an APK that downloads its own OBB names its update server in its dex
    string apk = Path.Combine(root, "obbapk.apk");
    using (var zip = System.IO.Compression.ZipFile.Open(apk, System.IO.Compression.ZipArchiveMode.Create))
    {
        using (var w = zip.CreateEntry("classes.dex").Open()) w.Write("dex\n\0\u0005https://example.com/x\0\u002bhttps://rsb.example.org/live/latest.json\0more"u8);
        using (var w = zip.CreateEntry("AndroidManifest.xml").Open()) w.Write("https://nope/latest.json\0"u8);
    }
    Expect(ObbUpdate.ConfigUrlInApk(apk) == "https://rsb.example.org/live/latest.json", $"update server from the dex: {ObbUpdate.ConfigUrlInApk(apk)}");
    string plain = Path.Combine(root, "plain.apk");
    using (var zip = System.IO.Compression.ZipFile.Open(plain, System.IO.Compression.ZipArchiveMode.Create))
        using (var w = zip.CreateEntry("classes.dex").Open()) w.Write("dex\n\0https://example.com/x\0"u8);
    Expect(ObbUpdate.ConfigUrlInApk(plain) == null, "an APK without one");
    File.Copy(apk, Path.Combine(g.Dir, "game.apk"), true);
    g.Refresh();
    Expect(ObbUpdate.ConfigUrl(g) == "https://rsb.example.org/live/latest.json", $"one that does: its own server: {ObbUpdate.ConfigUrl(g)}");
    g.Settings.ObbUpdateUrl = "";
    Expect(ObbUpdate.ConfigUrl(g) == null, "switched off with an empty ObbUpdateUrl");
    g.Settings.ObbUpdateUrl = null;

    // the in-game switch (rfu's preferences file)
    Expect(ObbUpdate.GameAllowsUpdates(g), "on by default");
    string prefs = Path.Combine(g.DataDir, "data", "shared_prefs", "PvZ2GameActivity.prefs");
    Directory.CreateDirectory(Path.GetDirectoryName(prefs)!);
    File.WriteAllText(prefs, "i\tMusicVolume\t85\nb\tCheckObbUpdates\t0\n");
    Expect(!ObbUpdate.GameAllowsUpdates(g), "switched off in the game");

    // a patch plan: the patches land in the game's OBB folder, named after the OBB, from 0
    var served = new Dictionary<string, byte[]> { ["https://x.test/20.patch"] = "patch20"u8.ToArray(), ["https://x.test/30.patch"] = "patch30!"u8.ToArray() };
    var http = new HttpClient(new FakeServer(served));
    var chain = new List<ObbUpdate.Node>
    {
        new(20, "1.5.1", null, null, -1, "https://x.test/20.patch", Sha("patch20"), 7, 10, "https://x.test/live/latest.json"),
        new(30, "1.5.2", null, null, -1, "https://x.test/30.patch", Sha("patch30!"), 8, 20, "https://x.test/live/latest.json"),
    };
    string name = Path.GetFileName(g.Obb!);
    File.WriteAllText(ObbUpdate.PatchPath(state, name, 5), "stale");
    File.WriteAllText(ObbUpdate.PatchPath(state, name, 1) + ".part", "patc");  // resumed from here
    ObbUpdate.ExecuteAsync(chain[1], new ObbUpdate.Patches(chain), g.Obb!, state, http, log.Add).GetAwaiter().GetResult();
    Expect(File.ReadAllText(ObbUpdate.PatchPath(state, name, 0)) == "patch20" && File.ReadAllText(ObbUpdate.PatchPath(state, name, 1)) == "patch30!", "patches downloaded, the second resumed");
    Expect(!File.Exists(ObbUpdate.PatchPath(state, name, 5)), "a stale patch is removed");
    Expect(ObbUpdate.ReadTimestamp(state) == 30, "obbTimestamp written");

    // a damaged download is refused
    served["https://x.test/20.patch"] = "patchXX"u8.ToArray();
    File.Delete(ObbUpdate.PatchPath(state, name, 0));
    bool refused = false;
    try { ObbUpdate.ExecuteAsync(chain[1], new ObbUpdate.Patches(chain), g.Obb!, state, http, log.Add).GetAwaiter().GetResult(); }
    catch (ObbUpdate.Error) { refused = true; }
    Expect(refused && !File.Exists(ObbUpdate.PatchPath(state, name, 0)), "SHA-256 mismatch refused");
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
    Mods.Set(g, Mod("keybinds", g), true);
    Directory.CreateDirectory(g.PatchesDir);
    File.WriteAllText(Path.Combine(g.PatchesDir, "a.patch"), "");
    g.Settings.Mods.AddRange(["patches/a.patch", "patches/gone.patch"]);
    var args = RfuCommand.Build(g, [new Mods.Overlay("/x", "/host/x")]);
    string line = string.Join(" ", args);
    foreach (var want in new[] { "--assets low", "--resolution 1280x720", "--offline", "--package com.ea.game.pvz2_rfl",
                                 "--version-code 675", "--version-name 1.4.2", "--overlay /x=/host/x", "--keybinds", "--key-hints", "--controller", "--hover", "--fix-animations" })
        Expect(line.Contains(want), $"rfu gets {want}: {line}");
    int ki = args.IndexOf("--keybinds");
    Expect(ki >= 0 && args[ki + 1] == g.KeybindsPath && File.Exists(g.KeybindsPath), $"keybinds: {{game}} filled in and the file written: {line}");
    Expect(args.Count(a => a == "--patch") == 1, "only patches that exist");
    Expect(!args.Contains("--no-mod-libs") && !args.Contains("--cloud-stub"), "mod libraries always load; no China stub");
    void Switch(string id, bool on) => Mods.Set(g, Mod(id, g), on);
    Switch("hover-collect", false);
    Expect(!RfuCommand.Build(g, []).Contains("--hover"), "hover to collect can be switched off");
    Expect(!RfuCommand.Build(g, []).Contains("--custom-cursor"), "the Windows cursor by default");
    Switch("glove-cursor", true);
    Expect(RfuCommand.Build(g, []).Contains("--custom-cursor"), "the glove cursor when it's on");
    Switch("fix-animations", false);
    Expect(!RfuCommand.Build(g, []).Contains("--fix-animations"), "the animations fix can be switched off");
    Switch("key-hints", false);
    Expect(!RfuCommand.Build(g, []).Contains("--key-hints"), "keybind hints can be switched off");
    Switch("key-hints", true);
    Switch("keybinds", false);
    Expect(RfuCommand.Build(g, []).Contains("--key-hints"), "controller hints without keybinds");
    Switch("controller", false);
    var noPad = RfuCommand.Build(g, []);
    Expect(!noPad.Contains("--controller") && !noPad.Contains("--key-hints"), "the controller can be switched off, and hints with it and keybinds");
    Expect(RfuCommand.IsSize("1920x1080") && !RfuCommand.IsSize("big") && !RfuCommand.IsSize("10x10"), "sizes");
    Switch("controller", true);
    Expect(!RfuCommand.Build(g, []).Contains("--pad-binds"), "no binds while every action is on its button");
    g.Settings.PadBinds["dig"] = "x";
    g.Settings.PadBinds["use"] = "b";
    var padArgs = RfuCommand.Build(g, []);
    int bi = padArgs.IndexOf("--pad-binds");
    Expect(bi >= 0 && padArgs[bi + 1].Contains("dig=x") && padArgs[bi + 1].Contains("use=b") && padArgs[bi + 1].Contains("plant=a"), $"binds: {string.Join(" ", padArgs)}");
    Expect(!padArgs.Contains("--pad-mappings"), "no mappings file, no mappings");
    File.WriteAllText(g.ControllerMappingsPath, "x\n");
    Expect(RfuCommand.Build(g, []).Contains("--pad-mappings"), "the mappings file is passed on");
    File.Delete(g.ControllerMappingsPath);
}

// The shared mods/ folder: built-in mods written once, categorised by game, shadowed by a game's own.
void ModLibraryTest()
{
    string shared = Path.Combine(root, "mods");
    Expect(BuiltinMods.Folders.Count >= 9 && BuiltinMods.Folders.Contains("Hover to collect") && BuiltinMods.Folders.Contains("Pirate English"),
        $"built-in mods embedded: {string.Join(", ", BuiltinMods.Folders)}");
    Expect(File.Exists(Path.Combine(shared, "Hover to collect", "mod.json")), "built-in mods written to the shared folder");
    Directory.Delete(Path.Combine(shared, "Glove cursor"), true);
    Expect(BuiltinMods.Seed(shared).Count == 0 && !Directory.Exists(Path.Combine(shared, "Glove cursor")), "a deleted built-in stays deleted");
    BuiltinMods.Restore(shared);
    Expect(Directory.Exists(Path.Combine(shared, "Glove cursor")), "restore writes it back");

    var rfl = MakeGame("LibRfl");
    var china = MakeChinaGame("LibChina");
    Expect(rfl.Family == "reflourished" && china.Family == "china", "families");
    var hover = Mod("hover-collect", rfl);
    Expect(hover.Kind == Mods.ModKind.Injection && hover.Category == "Gameplay" && hover.IsBuiltin, "manifest read");
    Expect(Mods.AppliesTo(hover, rfl) && !Mods.AppliesTo(hover, china), "global injections aren't for China");
    var stub = Mod("china-cloud-stub", china);
    Expect(Mods.AppliesTo(stub, china) && !Mods.AppliesTo(stub, rfl), "the China stub is only for China");
    var noLib = Mod("rfl-no-rflib", rfl);
    Expect(Mods.AppliesTo(noLib, rfl) && !Mods.AppliesTo(noLib, china) && !Mods.IsOn(rfl, noLib), "a Reflourished-only sample, off by default");
    Expect(Mods.IsOn(rfl, hover) && Mods.IsOn(china, stub) && !Mods.IsOn(china, hover), "on by default where they fit");
    Expect(!RfuCommand.Build(china, []).Contains("--hover"), "China gets no global injections");
    china.Settings.Offline = false;
    var online = RfuCommand.Build(china, []);
    Expect(online.Contains("--cloud-record") && !online.Contains("--cloud-stub"), $"China online records: {string.Join(" ", online)}");

    // switched off stays off, even though it's on by default
    Mods.Set(rfl, hover, false);
    rfl.SaveSettings();
    rfl.Refresh();
    Expect(!Mods.IsOn(rfl, Mod("hover-collect", rfl)), "switched off stays off");

    // a mod of the game's own hides the shared one of the same folder name; versions narrow it
    string own = Path.Combine(rfl.ModsDir, "Hover to collect");
    Directory.CreateDirectory(own);
    File.WriteAllText(Path.Combine(own, "mod.json"), """{"id":"hover-collect","name":"My hover","rfu":["--hover"],"versions":["9."]}""");
    var mine = Mods.List(rfl).Single(m => m.Key == "Hover to collect");
    Expect(mine.Name == "My hover" && mine.Source == Mods.ModSource.Game, "the game's own mod wins");
    Expect(!Mods.AppliesTo(mine, rfl), "versions: 9.x only");
    Directory.Delete(own, true);

    // a broken mod.json is shown with its problem and never runs
    string bad = Path.Combine(rfl.ModsDir, "Broken");
    Directory.CreateDirectory(bad);
    File.WriteAllText(Path.Combine(bad, "mod.json"), "{ nope");
    var broken = Mods.List(rfl).Single(m => m.Key == "Broken");
    Expect(broken.Problem != null, "broken manifest reported");
    Mods.Set(rfl, broken, true);
    Expect(!Mods.Active(rfl).Contains(broken), "broken mod left out");
    Directory.Delete(bad, true);

    // a code patch inside a mod folder
    string patchMod = Path.Combine(shared, "My Patch");
    Directory.CreateDirectory(patchMod);
    File.WriteAllText(Path.Combine(patchMod, "mod.json"), """{"name":"My patch","games":["reflourished"]}""");
    File.WriteAllText(Path.Combine(patchMod, "fix.patch"), "libPVZ2.so+0x10 00\n");
    var pm = Mods.List(rfl).Single(m => m.Key == "My Patch");
    Expect(pm.Kind == Mods.ModKind.Injection && pm.Category == "Code patches" && !pm.HasContent, "a mod with a .patch is an injection");
    Mods.Set(rfl, pm, true);
    var withPatch = RfuCommand.Build(rfl, []);
    Expect(withPatch.Contains(Path.Combine(patchMod, "fix.patch")), "its patch is passed to rfu");
    Directory.Delete(patchMod, true);
}

// game.json from before injections were mods: the switches become Mods entries once.
void LegacySettingsTest()
{
    string dir = Path.Combine(root, "games", "Legacy");
    Directory.CreateDirectory(Path.Combine(dir, "lib", "arm64-v8a"));
    File.WriteAllText(Path.Combine(dir, "lib", "arm64-v8a", "libPVZ2.so"), "");
    File.WriteAllText(Path.Combine(dir, "main.675.com.ea.game.pvz2_rfl.obb"), "");
    File.WriteAllText(Path.Combine(dir, "_apk_manifest.json"),
        """{"package":"com.ea.game.pvz2_rfl","version_name":"1.4.2","version_code":"675","obb":"main.675.com.ea.game.pvz2_rfl.obb"}""");
    Directory.CreateDirectory(Path.Combine(dir, "mods", "Levels"));
    File.WriteAllText(Path.Combine(dir, "mods", "Levels", "un1.json"), "{}");
    File.WriteAllText(Path.Combine(dir, "game.json"), """
        {"DisplayName":"Old","HoverCollect":false,"FixAnimations":true,"Keybinds":true,"KeyHints":true,"Controller":false,
         "CustomCursor":true,"Patches":["x.patch"],"Mods":["Levels"]}
        """);
    var g = GameFolder.Open(dir);
    var on = g.Settings.Mods;
    Expect(on.Contains("Levels") && on.Contains("Frozen animations fix") && on.Contains("Keybinds") && on.Contains("Keybind hints") &&
           on.Contains("Glove cursor") && on.Contains("patches/x.patch"), $"switched on: {string.Join(", ", on)}");
    Expect(!on.Contains("Hover to collect") && !on.Contains("Controller support"), "switched off stays off, though on by default");
    Expect(!g.Settings.HasLegacyInjections && !File.ReadAllText(g.SettingsPath).Contains("HoverCollect"), "old switches dropped from game.json");
    Expect(on.IndexOf("Levels") == 0, "content keeps its place in the load order");
}

// A localization: plain strings merged over the game's LawnStrings for its language.
void LocalizationTest()
{
    var g = MakeGame("LocGame");
    string cdn = Path.Combine(g.DataDir, "external", "files", "No_Backup", "CDN.1.4.2");
    Directory.CreateDirectory(cdn);
    var pirate = Mod("pirate-english", g);
    Expect(pirate.Kind == Mods.ModKind.Localization && pirate.Language == "en-US" && pirate.HasContent && !Mods.IsOn(g, pirate), "the sample localization");
    Mods.Set(g, pirate, true);
    var early = Mods.Build(g, Mods.Active(g).Where(m => m.HasContent).Select(m => m.Key).ToList());
    Expect(early.Overlays.Count == 0 && early.Log.Any(l => l.Contains("no LawnStrings-en-US")), "no game strings yet: left out, not a partial file");
    Expect(Mods.Coverage(g, pirate) == null, "no coverage without game strings");

    File.WriteAllBytes(Path.Combine(cdn, "LawnStrings-en-US.rton"), Pvz2Conv.RtonWriter.FromJson("""
        {"version":1,"objects":[{"objclass":"LawnStringsData","objdata":{"LocStringValues":
        ["LETS_ROCK_BUTTON","LET'S ROCK!","LTE_NEW_EVENT","A brand new event","ZEN_GARDEN","Zen Garden"]}}]}
        """u8));
    Expect(Mods.AnyContentOn(g), "a localization counts as content");
    Mods.Build(g, Mods.Active(g).Where(m => m.HasContent).Select(m => m.Key).ToList());
    string built = Path.Combine(g.WorkDir, "overlay", "cdn", "LawnStrings-en-US.rton");
    Expect(File.Exists(built), "strings built over the game's file");
    if (File.Exists(built))
    {
        string json = ToJson(File.ReadAllBytes(built));
        Expect(json.Contains("HOIST THE SAILS!") && json.Contains("A brand new event") && json.Contains("Captain") && !json.Contains("Zen Garden"),
            $"the mod's strings win, an event's new string falls back to the game's: {json}");
    }
    var c = Mods.Coverage(g, pirate);
    Expect(c is { Covered: 2, Total: 3 } && c.Missing.Single().Key == "LTE_NEW_EVENT", $"coverage: {c?.Covered}/{c?.Total}");
    string? missing = Mods.WriteMissing(g, pirate);
    Expect(missing != null && File.ReadAllText(missing).Contains("\"LTE_NEW_EVENT\": \"A brand new event\""), "missing strings written for translating");
    Expect(Mods.List(g).Single(m => m.Id == "pirate-english").HasContent, "missing-strings.json doesn't make it loose content");
}

// The visual keybind editor's file side: percent taps in keybinds.txt, and layouts as mods.
void KeybindLayoutTest()
{
    var g = MakeGame("LayoutGame");
    g.EnsureKeybindsFile();
    File.AppendAllText(g.KeybindsPath, "Space tap 640 360\nQ tap 10% 20%\n");
    var markers = KeybindLayout.Read(g.KeybindsPath);
    Expect(markers.Count == 1 && markers[0].Key == "Q" && Math.Abs(markers[0].X - 0.1f) < 1e-4 && Math.Abs(markers[0].Y - 0.2f) < 1e-4, "percent taps read, pixel taps left alone");
    KeybindLayout.Write(g.KeybindsPath, [new("Q", 0.5f, 0.25f), new("E", 0.125f, 0.9f)]);
    string text = File.ReadAllText(g.KeybindsPath);
    Expect(text.Contains("Q tap 50% 25%") && text.Contains("E tap 12.5% 90%") && !text.Contains("Q tap 10% 20%"), $"markers written: {text}");
    Expect(text.Contains("Space tap 640 360") && text.Contains("1 seed 1") && text.Contains("D shovel"), "other keybinds kept");
    KeybindLayout.Write(g.KeybindsPath, [new("Q", 0.5f, 0.25f)]);
    Expect(File.ReadAllText(g.KeybindsPath).Split("visual layout (made").Length == 2 && !File.ReadAllText(g.KeybindsPath).Contains("E tap"), "written again, not twice");

    string shot = Path.Combine(root, "shot.png");
    File.WriteAllBytes(shot, [0x89, 0x50, 0x4E, 0x47]);
    string dir = KeybindLayout.SaveAsMod(Path.Combine(root, "mods"), "RFL 1.5 layout", "com.ea.game.pvz2_rfl", "1.4", [new("W", 0.3f, 0.6f)], shot);
    var mod = Mods.List(g).Single(m => m.Key == "RFL 1.5 layout");
    Expect(mod.Kind == Mods.ModKind.Injection && mod.Category == "Controls" && Mods.AppliesTo(mod, g) && !mod.HasContent, "a layout mod for this game");
    Expect(File.Exists(Path.Combine(dir, "screenshot.png")) && File.Exists(Path.Combine(dir, "layout.json")), "screenshot and layout.json kept with it");
    var (back, backShot) = KeybindLayout.ReadMod(dir);
    Expect(back.Count == 1 && back[0].Key == "W" && back[0].State == "any" && backShot != null, "layout read back, with its state");
    Mods.Set(g, mod, true);
    var args = RfuCommand.Build(g, []);
    Expect(args.Contains(Path.Combine(dir, "keybinds.txt")), $"its keybinds go to rfu: {string.Join(" ", args)}");
    Expect(!args.Contains("--pad-layout"), "no controller spots, no controller layout");
    var china = MakeChinaGame("LayoutChina");
    Expect(!Mods.AppliesTo(mod, china), "not for another game");

    // controller buttons, per screen: the game's own file, and in a layout mod
    KeybindLayout.Marker[] both = [new("Q", 0.5f, 0.25f), new("y", 0.95f, 0.075f, "map", Pad: true), new("rt", 0.2f, 0.05f, "any", Pad: true)];
    Mods.Set(g, Mod("controller", g), true);
    KeybindLayout.WritePad(g.PadLayoutPath, both);
    string pad = File.ReadAllText(g.PadLayoutPath);
    Expect(pad.Contains("map y tap 95% 7.5%") && pad.Contains("any rt tap 20% 5%") && !pad.Contains("Q tap"), $"controller spots written: {pad}");
    File.AppendAllText(g.PadLayoutPath, "chooser lb tap 1% 2%   # by hand\nnot a spot\n");
    var padBack = KeybindLayout.ReadPad(g.PadLayoutPath);
    Expect(padBack.Count == 3 && padBack.All(m => m.Pad) && padBack[0] == both[1] && padBack[2].State == "chooser", "controller spots read back");
    KeybindLayout.WritePad(g.PadLayoutPath, both);
    Expect(KeybindLayout.ReadPad(g.PadLayoutPath).Count == 2 && File.ReadAllText(g.PadLayoutPath).Contains("not a spot"), "written again: the editor's spots, other lines kept");
    KeybindLayout.Write(g.KeybindsPath, both);
    Expect(KeybindLayout.Read(g.KeybindsPath).Count == 1, "keybinds.txt gets only the keys");
    args = RfuCommand.Build(g, []);
    int pi = args.IndexOf("--pad-layout");
    Expect(pi >= 0 && args[pi + 1] == g.PadLayoutPath && args.LastIndexOf("--pad-layout") == pi, $"the game's controller layout goes to rfu: {string.Join(" ", args)}");
    Mods.Set(g, Mod("controller", g), false);
    Expect(!RfuCommand.Build(g, []).Contains(g.PadLayoutPath), "not without the controller");
    Mods.Set(g, Mod("controller", g), true);
    File.WriteAllText(g.PadLayoutPath, "not a spot\n");
    KeybindLayout.WritePad(g.PadLayoutPath, []);
    Expect(File.Exists(g.PadLayoutPath), "a file with lines of its own stays");
    File.WriteAllText(g.PadLayoutPath, "# only comments\nmap a tap 1% 1%\n");
    KeybindLayout.WritePad(g.PadLayoutPath, []);
    Expect(!File.Exists(g.PadLayoutPath), "no spots left, no file");
    KeybindLayout.WritePad(g.PadLayoutPath, both);

    dir = KeybindLayout.SaveAsMod(Path.Combine(root, "mods"), "RFL 1.5 layout", "com.ea.game.pvz2_rfl", "1.4", both, shot);
    Expect(File.Exists(Path.Combine(dir, KeybindLayout.PadLayoutFile)) && File.ReadAllText(Path.Combine(dir, KeybindLayout.PadLayoutFile)).Contains("map y tap 95% 7.5%"), "the mod's controller layout");
    (back, _) = KeybindLayout.ReadMod(dir);
    Expect(back.SequenceEqual(both), "the mod's markers read back, keys and buttons");
    mod = Mods.List(g).Single(m => m.Key == "RFL 1.5 layout");
    args = RfuCommand.Build(g, []);
    pi = args.IndexOf(Path.Combine(dir, KeybindLayout.PadLayoutFile));
    Expect(pi > 0 && args[pi - 1] == "--pad-layout" && pi < args.IndexOf(g.PadLayoutPath), $"the mod's controller layout goes first, the game's wins: {string.Join(" ", args)}");
    dir = KeybindLayout.SaveAsMod(Path.Combine(root, "mods"), "RFL 1.5 layout", "com.ea.game.pvz2_rfl", "1.4", [both[1]], shot);
    Expect(!File.Exists(Path.Combine(dir, "keybinds.txt")) && Mods.List(g).Single(m => m.Key == "RFL 1.5 layout").Rfu.SequenceEqual(["--pad-layout", "{mod}/" + KeybindLayout.PadLayoutFile]), "a controller-only layout has no keybinds");
    File.Delete(g.PadLayoutPath);
    Directory.Delete(dir, true);
}

Mods.Mod Mod(string id, GameFolder? game = null)
{
    game ??= MakeGame("ModLookup");
    return Mods.List(game).First(m => m.Id == id);
}

void PadSetupTest()
{
    var lawn = PadSetup.Screens[0];
    var chooser = PadSetup.Screens.Single(s => s.Name == "chooser");
    var binds = new Dictionary<string, string> { ["plant"] = "b" };
    Expect(PadSetup.Clashes(binds, lawn).Count == 2, "plant and dig both on B clash");
    Expect(PadSetup.Clashes(binds, chooser).Count == 0, "the lawn's binds don't clash on the chooser");
    binds["dig"] = "a";
    Expect(PadSetup.Clashes(binds, lawn).Count == 0, "swapped, no clash");
    Expect(PadSetup.BindsArg(new Dictionary<string, string> { ["plant"] = "nonsense" }) == null, "an unknown button is the default");
    // each screen on its own: only the screens with a moved action are passed, with their screen
    string arg = PadSetup.BindsArg(new Dictionary<string, string> { ["chooser.take"] = "x", ["chooser.letsrock"] = "a" })!;
    Expect(arg.Contains("chooser.take=x") && arg.Contains("chooser.letsrock=a") && arg.Contains("chooser.pause=start") && !arg.Contains("plant=") && !arg.Contains("map."), arg);
    Expect(PadSetup.Clashes(new Dictionary<string, string> { ["chooser.take"] = "x" }, chooser).Count == 2, "take and let's rock both on X clash");
    var moved = PadSetup.Moved(new Dictionary<string, string> { ["plant"] = "a", ["map.shop"] = "x", ["junk"] = "a" });
    Expect(moved.Count == 1 && moved["map.shop"] == "x", "only moved actions are kept");
    Expect(PadSetup.Screens.Select(s => s.Name).SequenceEqual(["lawn", "chooser", "menu", "map", "other"]), "rfu's screens, in its order");
    string m = PadSetup.Mapping("030000004c050000c405000000000000", "Wireless, Controller",
        [new("a", PadSetup.Button(1)), new("dpup", PadSetup.Hat(0, 1)), new("leftx", PadSetup.Axis(0, false)), new("lefty", PadSetup.Axis(1, true)), new("x", "")]);
    Expect(m == "030000004c050000c405000000000000,Wireless Controller,a:b1,dpup:h0.1,leftx:a0,lefty:a1~,platform:Windows,", m);
    string path = Path.Combine(root, "controllers.txt");
    Directory.CreateDirectory(root);
    File.WriteAllLines(path, ["0300aa,Old,a:b0,platform:Windows,", "0300bb,Other,a:b0,platform:Windows,"]);
    PadSetup.SaveMapping(path, "0300AA,New,a:b2,platform:Windows,");
    var lines = File.ReadAllLines(path);
    Expect(lines.Length == 2 && lines.Contains("0300bb,Other,a:b0,platform:Windows,") && lines.Contains("0300AA,New,a:b2,platform:Windows,"), "a mapping replaces its GUID's line");
}

/// Serves fixed bytes by URL, with Range requests.
sealed class FakeServer(Dictionary<string, byte[]> files) : HttpMessageHandler
{
    protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancel)
    {
        if (!files.TryGetValue(request.RequestUri!.ToString(), out var data))
            return Task.FromResult(new HttpResponseMessage(System.Net.HttpStatusCode.NotFound));
        long from = request.Headers.Range?.Ranges.First().From ?? 0;
        var response = new HttpResponseMessage(from > 0 ? System.Net.HttpStatusCode.PartialContent : System.Net.HttpStatusCode.OK)
        {
            Content = new ByteArrayContent(data[(int)from..]),
        };
        return Task.FromResult(response);
    }
}
