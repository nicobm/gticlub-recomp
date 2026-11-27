// The PlayStation Eye of the game's EyeToy features, on the host's webcam (SDL3 camera API).
// A capture thread keeps the latest image as 640x480 RGB (centre crop to 4:3, nearest sampling); the game's
// cellCameraRead takes it in the format it opened the camera with (RAW8 Bayer like the PS Eye's sensor, YUV422, RGBA).
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gti::recomp {

namespace {
std::mutex g_m;
std::vector<uint8_t> g_rgb(640 * 480 * 3);
std::atomic<uint32_t> g_frame{0};
std::atomic<bool> g_run{false};
std::thread g_thread;
bool g_inited = false;

bool init_camera_subsystem() {
    if (!g_inited) g_inited = SDL_InitSubSystem(SDL_INIT_CAMERA);
    return g_inited;
}

void capture(SDL_CameraID id) {
    SDL_Camera* cam = nullptr;
    int nspec = 0;
    SDL_CameraSpec** specs = SDL_GetCameraSupportedFormats(id, &nspec);
    SDL_CameraSpec want{};
    want.format = SDL_PIXELFORMAT_RGB24;   // SDL converts from the device's own format
    if (specs && nspec > 0) { want.width = specs[0]->width; want.height = specs[0]->height; want.framerate_numerator = specs[0]->framerate_numerator; want.framerate_denominator = specs[0]->framerate_denominator; }
    else { want.width = 640; want.height = 480; want.framerate_numerator = 30; want.framerate_denominator = 1; }
    SDL_free(specs);
    want.colorspace = SDL_COLORSPACE_SRGB;
    cam = SDL_OpenCamera(id, &want);
    if (!cam) { std::fprintf(stderr, "[camera] cannot open: %s\n", SDL_GetError()); g_run = false; return; }
    std::fprintf(stderr, "[camera] %s opened at %dx%d\n", SDL_GetCameraName(id), want.width, want.height);
    while (g_run.load()) {
        if (SDL_GetCameraPermissionState(cam) != SDL_CAMERA_PERMISSION_STATE_APPROVED) { SDL_Delay(20); continue; }
        Uint64 ts = 0;
        SDL_Surface* s = SDL_AcquireCameraFrame(cam, &ts);
        if (!s) { SDL_Delay(2); continue; }
        SDL_Surface* rgb = s->format == SDL_PIXELFORMAT_RGB24 ? s : SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGB24);
        if (rgb) {
            // centre crop to 4:3, then 640x480 by nearest sampling
            const int sw = rgb->w, sh = rgb->h;
            int cw = sw, ch = sh;
            if (sw * 3 > sh * 4) cw = sh * 4 / 3; else ch = sw * 3 / 4;
            const int x0 = (sw - cw) / 2, y0 = (sh - ch) / 2;
            std::vector<uint8_t> img(640 * 480 * 3);
            for (int y = 0; y < 480; ++y) {
                const uint8_t* row = static_cast<const uint8_t*>(rgb->pixels) + size_t(y0 + y * ch / 480) * rgb->pitch;
                uint8_t* d = img.data() + size_t(y) * 640 * 3;
                for (int x = 0; x < 640; ++x) std::memcpy(d + 3 * x, row + 3 * size_t(x0 + x * cw / 640), 3);
            }
            { std::lock_guard<std::mutex> lk(g_m); g_rgb.swap(img); }
            ++g_frame;
            if (rgb != s) SDL_DestroySurface(rgb);
        }
        SDL_ReleaseCameraFrame(cam, s);
    }
    SDL_CloseCamera(cam);
}
}   // namespace

// names of the host's cameras (for the options menu)
std::vector<std::string> host_camera_list() {
    std::vector<std::string> out;
    if (!init_camera_subsystem()) return out;
    int n = 0;
    SDL_CameraID* ids = SDL_GetCameras(&n);
    for (int i = 0; i < n; ++i) { const char* nm = SDL_GetCameraName(ids[i]); out.push_back(nm ? nm : "?"); }
    SDL_free(ids);
    return out;
}

// the camera chosen in the settings: 0 none, k = the k-th camera of the list
bool host_camera_available(int choice) {
    if (choice <= 0) return false;
    return int(host_camera_list().size()) >= choice;
}

bool host_camera_start(int choice) {
    if (g_run.load()) return true;
    if (!host_camera_available(choice)) return false;
    int n = 0;
    SDL_CameraID* ids = SDL_GetCameras(&n);
    const SDL_CameraID id = choice <= n ? ids[choice - 1] : 0;
    SDL_free(ids);
    if (!id) return false;
    g_run = true;
    g_thread = std::thread(capture, id);
    return true;
}

void host_camera_stop() {
    if (!g_run.exchange(false)) return;
    if (g_thread.joinable()) g_thread.join();
}

// the latest image in the game's format (2 RAW8 Bayer, 3 YUV422, 5 RGBA; other formats: grey) at w x h;
// returns the bytes written and the frame number
uint32_t host_camera_read(uint8_t* dst, uint32_t cap, int format, int w, int h, uint32_t& frame) {
    std::vector<uint8_t> img;
    { std::lock_guard<std::mutex> lk(g_m); img = g_rgb; }
    frame = g_frame.load();
    // CAMBAYER (debug): Bayer order 0 BGGR (default), 1 GBRG, 2 GRBG, 3 RGGB
    static const int bayer = std::getenv("CAMBAYER") ? std::atoi(std::getenv("CAMBAYER")) & 3 : 0;
    auto px = [&](int x, int y) { return img.data() + (size_t(y * 480 / h) * 640 + size_t(x * 640 / w)) * 3; };
    uint32_t n = 0;
    if (format == 2) {   // RAW8: one colour per pixel, 2x2 Bayer pattern
        static const int order[4][4] = {{2, 1, 1, 0}, {1, 2, 0, 1}, {1, 0, 2, 1}, {0, 1, 1, 2}};   // channel of (x&1, y&1) as [y*2+x]
        n = uint32_t(w * h);
        if (n > cap) return 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) dst[size_t(y) * w + x] = px(x, y)[order[bayer][(y & 1) * 2 + (x & 1)]];
    } else if (format == 3) {   // YUV422 as Y0 U Y1 V
        n = uint32_t(w * h * 2);
        if (n > cap) return 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; x += 2) {
                const uint8_t* a = px(x, y); const uint8_t* b = px(x + 1, y);
                auto Y = [](const uint8_t* p) { return uint8_t(std::clamp((66 * p[0] + 129 * p[1] + 25 * p[2] + 128) / 256 + 16, 0, 255)); };
                const int r = (a[0] + b[0]) / 2, g = (a[1] + b[1]) / 2, bl = (a[2] + b[2]) / 2;
                uint8_t* d = dst + (size_t(y) * w + x) * 2;
                d[0] = Y(a);
                d[1] = uint8_t(std::clamp((-38 * r - 74 * g + 112 * bl + 128) / 256 + 128, 0, 255));
                d[2] = Y(b);
                d[3] = uint8_t(std::clamp((112 * r - 94 * g - 18 * bl + 128) / 256 + 128, 0, 255));
            }
    } else if (format == 5) {   // RGBA
        n = uint32_t(w * h * 4);
        if (n > cap) return 0;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) { const uint8_t* p = px(x, y); uint8_t* d = dst + (size_t(y) * w + x) * 4; d[0] = p[0]; d[1] = p[1]; d[2] = p[2]; d[3] = 255; }
    } else {
        n = std::min<uint32_t>(cap, uint32_t(w * h));
        std::memset(dst, 128, n);
    }
    return n;
}

}   // namespace gti::recomp
