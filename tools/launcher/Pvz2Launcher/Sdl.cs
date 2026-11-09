using System.Reflection;
using System.Runtime.InteropServices;

namespace Pvz2Launcher;

/// The bits of SDL2 the controller settings read controllers with: the SDL2.dll shipped with rfu,
/// so the launcher sees a controller exactly as the game will.
static class Sdl
{
    const string Lib = "SDL2";
    public const uint InitJoystick = 0x200, InitGameController = 0x2000;

    static string? _dir;

    /// Where SDL2.dll is (rfu's folder). False when it isn't there or doesn't load.
    public static bool Load(string? rfuDir)
    {
        if (rfuDir == null || !File.Exists(Path.Combine(rfuDir, "SDL2.dll"))) return false;
        if (_dir == null)
        {
            _dir = rfuDir;
            NativeLibrary.SetDllImportResolver(Assembly.GetExecutingAssembly(), (name, _, _) =>
                name == Lib && NativeLibrary.TryLoad(Path.Combine(_dir, "SDL2.dll"), out var h) ? h : IntPtr.Zero);
        }
        try
        {
            SDL_SetHint("SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS", "1");  // the launcher's window isn't SDL's
            return SDL_InitSubSystem(InitJoystick | InitGameController) == 0;
        }
        catch (Exception e) when (e is DllNotFoundException or EntryPointNotFoundException or BadImageFormatException) { return false; }
    }

    public static void Quit() => SDL_QuitSubSystem(InitJoystick | InitGameController);

    [StructLayout(LayoutKind.Sequential)]
    public struct Guid { public ulong Lo, Hi; }

    public static string GuidString(int index)
    {
        var buf = new byte[64];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index), buf, buf.Length);
        return System.Text.Encoding.ASCII.GetString(buf).TrimEnd('\0');
    }

    public static string Name(int index) => Marshal.PtrToStringUTF8(SDL_JoystickNameForIndex(index)) ?? "Controller";
    public static string Error() => Marshal.PtrToStringUTF8(SDL_GetError()) ?? "";

    // SDL_GameControllerType: 1 Xbox 360, 2 Xbox One, 3 PS3, 4 PS4, 5 Switch Pro, 7 PS5...
    public static string TypeName(int type) => type switch
    {
        1 or 2 => "Xbox",
        3 or 4 or 7 => "PlayStation",
        5 or 11 or 12 or 13 => "Switch",
        _ => "Xbox (not recognised: Xbox labels)",
    };

    [DllImport(Lib)] public static extern int SDL_InitSubSystem(uint flags);
    [DllImport(Lib)] public static extern void SDL_QuitSubSystem(uint flags);
    [DllImport(Lib)] public static extern int SDL_SetHint([MarshalAs(UnmanagedType.LPUTF8Str)] string name, [MarshalAs(UnmanagedType.LPUTF8Str)] string value);
    [DllImport(Lib)] public static extern IntPtr SDL_GetError();
    [DllImport(Lib)] public static extern void SDL_PumpEvents();
    [DllImport(Lib)] public static extern void SDL_JoystickUpdate();
    [DllImport(Lib)] public static extern void SDL_GameControllerUpdate();
    [DllImport(Lib)] public static extern int SDL_NumJoysticks();
    [DllImport(Lib)] public static extern IntPtr SDL_JoystickNameForIndex(int index);
    [DllImport(Lib)] public static extern Guid SDL_JoystickGetDeviceGUID(int index);
    [DllImport(Lib)] public static extern void SDL_JoystickGetGUIDString(Guid guid, byte[] buf, int size);
    [DllImport(Lib)] public static extern int SDL_IsGameController(int index);
    [DllImport(Lib)] public static extern IntPtr SDL_JoystickOpen(int index);
    [DllImport(Lib)] public static extern void SDL_JoystickClose(IntPtr joystick);
    [DllImport(Lib)] public static extern int SDL_JoystickNumButtons(IntPtr joystick);
    [DllImport(Lib)] public static extern int SDL_JoystickNumAxes(IntPtr joystick);
    [DllImport(Lib)] public static extern int SDL_JoystickNumHats(IntPtr joystick);
    [DllImport(Lib)] public static extern byte SDL_JoystickGetButton(IntPtr joystick, int button);
    [DllImport(Lib)] public static extern short SDL_JoystickGetAxis(IntPtr joystick, int axis);
    [DllImport(Lib)] public static extern byte SDL_JoystickGetHat(IntPtr joystick, int hat);
    [DllImport(Lib)] public static extern IntPtr SDL_GameControllerOpen(int index);
    [DllImport(Lib)] public static extern void SDL_GameControllerClose(IntPtr controller);
    [DllImport(Lib)] public static extern byte SDL_GameControllerGetButton(IntPtr controller, int button);
    [DllImport(Lib)] public static extern short SDL_GameControllerGetAxis(IntPtr controller, int axis);
    [DllImport(Lib)] public static extern int SDL_GameControllerGetType(IntPtr controller);
    [DllImport(Lib)] public static extern int SDL_GameControllerAddMapping([MarshalAs(UnmanagedType.LPUTF8Str)] string mapping);

    // SDL_GameControllerButton and SDL_GameControllerAxis, with PadSetup's names for them.
    public static readonly (string Name, int Button)[] Buttons =
    [
        ("a", 0), ("b", 1), ("x", 2), ("y", 3), ("back", 4), ("start", 6), ("l3", 7), ("r3", 8), ("lb", 9), ("rb", 10),
        ("up", 11), ("down", 12), ("left", 13), ("right", 14),
    ];
    public const int AxisLeftX = 0, AxisLeftY = 1, AxisRightX = 2, AxisRightY = 3, AxisLeftTrigger = 4, AxisRightTrigger = 5;
}
