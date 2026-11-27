#pragma once
// User options of the recompiled game: kept in gtiport.ini next to the executable and edited with the in-game
// options menu (F1). Environment variables used by the test scripts still override them where noted.
namespace gti::recomp {
struct Settings {
    int fullscreen = 0;   // 0 window, 1 borderless window covering the monitor (capturable), 2 full screen
    int scale = 1;        // internal resolution: multiples of the console's 1280x720
    int gameHz = 60;      // emulated vblank rate (the console has 60; 72, 120 and 144 are experimental); GAMEHZ overrides
    int refresh = 1;      // 1 = switch a display that is not a multiple of the game rate (144 Hz) to 120 Hz while playing
    int language = 3;     // PS3 system language id (1 English, 2 French, 3 Spanish, 4 German, 5 Italian)
    int vsync = 0;        // swap interval 1: no tearing in exclusive full screen, a little more latency
    int aniso = 4;        // anisotropic filtering at least 2^aniso (0 = only what the game asks for, 4 = 16x)
    int streaming = 0;    // live streaming / recording: 60 images per second and the display at 120 Hz, whatever gameHz says
    int fxaa = 1;         // edge antialiasing pass on the final image
    int sharpen = 1;      // contrast-adaptive sharpening of the final image: 0 off, 1 low, 2 medium, 3 high
    int camera = 1;       // the EyeToy / PlayStation Eye: 0 none, k = the k-th camera of the host
    int sound = 1;        // sound effects (the game's MultiStream mixer runs on a host thread)
    int showFps = 1;      // frame rate in the window title
    int buttons = 0;      // button icons in the menus: 0 the controller connected, 1 PlayStation, 2 Xbox, 3 Nintendo
    int renderer = 0;     // graphics API: 0 OpenGL, 1 Direct3D 11, 2 Vulkan (read at start; RENDERER=gl|d3d11|vulkan overrides)
    int online = 1;       // online play (direct connection, src/recomp/netplay.cpp); NETPLAY=0|1 overrides
    char onlineName[20] = "";   // player name online (empty: the Windows user name)
    char onlineIp[64] = "";     // address of the friend who hosts (empty: only the local network)
};
Settings& settings();     // loaded from gtiport.ini on first use
void settings_save();
} // namespace gti::recomp
