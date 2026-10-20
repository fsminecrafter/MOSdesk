/*
 * Cube - a rotating, lit 3D cube drawn with FarcadeGL (the SDK's 3D
 * library) in a mosWM window. Rendering goes through the GPU interface
 * (SYS_GPU); frames are shown straight into the window.
 *
 * Keys: L switches the lighting (per pixel / per vertex / none),
 *       W toggles wireframe, Escape closes.
 */
#include "farcadegl.h"
#include "window.h"
#include "string.h"

#define KEY_L 0x0F
#define KEY_W 0x1A
#define KEY_ESCAPE 0x29

static volatile int g_quit;
static uint32_t g_program = FGL_PROGRAM_PHONG;
static int g_wire;

static void number(char* out, long v) {
    char t[24];
    int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) *out++ = t[--n];
    *out = '\0';
}

int main(void) {
    const uint32_t width = 560, height = 420;
    uint32_t window = mos_window_create(0, 140, 100, width, height, "Cube");
    if (!window) return 1;
    mos_window_set_resizable(window, false);

    FGLContextDesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.width = width;
    desc.height = height;
    desc.window_id = window;
    desc.depth = 1;
    FGLContext* gl = fglCreateContext(&desc);
    if (!gl) { mos_window_close(window); return 1; }

    FGLVertex cube[36];
    uint32_t count = fglMakeCube(cube, 1.0f, NULL);
    FGLBuffer vb = fglCreateBuffer(gl, sizeof(cube));
    fglBufferData(gl, vb, 0, cube, sizeof(cube));

    long start = mos_uptime(), last = start, frames = 0;
    while (!g_quit) {
        float t = (float)(mos_uptime() - start) / 1000.0f;
        fglClearColor(gl, 0.07f, 0.09f, 0.14f);
        fglClear(gl, FGL_COLOR_BUFFER_BIT | FGL_DEPTH_BUFFER_BIT);
        fglEnable(gl, FGL_DEPTH_TEST | FGL_CULL_FACE);
        if (g_wire) fglEnable(gl, FGL_WIREFRAME); else fglDisable(gl, FGL_WIREFRAME);
        fglUseProgram(gl, g_program);
        fglSetProjection(gl, fglPerspective(0.9f, (float)width / (float)height, 0.1f, 50.0f));
        fglSetView(gl, fglLookAt(fglVec3(0, 1.2f, 4.5f), fglVec3(0, 0, 0), fglVec3(0, 1, 0)));
        fglSetModel(gl, fglMul(fglRotate(t * 0.9f, fglVec3(0, 1, 0)),
                               fglRotate(t * 0.6f, fglVec3(1, 0, 0))));
        fglDraw(gl, vb, 0, count);
        if (fglPresent(gl) != 0) break;
        frames++;

        long now = mos_uptime();
        if (now - last >= 1000) {
            char title[48] = "Cube - ";
            char* end = title + strlen(title);
            number(end, frames * 1000 / (now - last));
            end += strlen(end);
            memcpy(end, " fps", 5);
            mos_window_set_title(window, title);
            frames = 0;
            last = now;
        }

        syscall_window_event_t ev;
        while (mos_window_next_event(window, &ev) == SYS_SUCCESS) {
            if (ev.type == SYSCALL_WINDOW_EVENT_CLOSE_REQUEST) g_quit = 1;
            if (ev.type == SYSCALL_WINDOW_EVENT_KEY && ev.buttons /* key down */) {
                if (ev.key == KEY_ESCAPE) g_quit = 1;
                else if (ev.key == KEY_W) g_wire = !g_wire;
                else if (ev.key == KEY_L)
                    g_program = g_program == FGL_PROGRAM_PHONG ? FGL_PROGRAM_GOURAUD :
                                g_program == FGL_PROGRAM_GOURAUD ? FGL_PROGRAM_COLOR :
                                                                   FGL_PROGRAM_PHONG;
            }
        }
    }

    fglDestroyBuffer(gl, vb);
    fglDestroyContext(gl);
    mos_window_close(window);
    return 0;
}
