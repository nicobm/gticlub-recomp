#pragma once
// The options panel (F1) as data: rsx_core.cpp fills it, host_win.cpp draws it (GDI+) into a BGRA bitmap with
// premultiplied alpha, at the size it is shown (sharp text at any window size).
#include <cstdint>
namespace gti::recomp {
struct MenuRow {
    enum Kind { Choice, Toggle, Action } kind = Choice;
    const wchar_t* label = L"";
    const wchar_t* value = L"";   // Choice: the value text; Action: the button text
    const wchar_t* note = L"";    // small text before the value ("en uso: OpenGL")
    bool on = false;              // Toggle
    bool restart = false;         // applies at the next start
    bool pending = false;         // changed since the start, waiting for the restart
};
struct MenuPage {
    const wchar_t* tabs[8] = {};
    int nTabs = 0, tab = 0;            // the tab shown
    const MenuRow* rows = nullptr;     // its rows
    int nRows = 0, sel = 0;            // sel: highlighted row in this tab
    const wchar_t* help = L"";         // what the highlighted option does
    const wchar_t* status = L"";       // top right: the API and the GPU
    bool pending = false;              // some option waits for the restart (banner)
    bool pad = false;                  // a controller is connected: its buttons in the hints
};
bool host_menu_bitmap(const MenuPage& page, int w, int h, uint32_t* out);
} // namespace gti::recomp
