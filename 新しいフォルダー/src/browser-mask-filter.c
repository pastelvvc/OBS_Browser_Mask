#include "browser-mask-filter.h"

#include <obs-module.h>
#include <graphics/graphics.h>
#include <graphics/vec4.h>
#include <util/platform.h>

#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <stdbool.h>
#include <string.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")

struct browser_mask_filter_data {
    obs_source_t *context;
    char *process_names;
    int capture_left;
    int capture_top;
    int capture_width;
    int capture_height;
    int padding;
    bool debug_fullscreen_red;
};

struct enum_ctx {
    const char *process_names;
    RECT rect;
    bool found;
};

static const char *browser_mask_filter_get_name(void *type_data)
{
    UNUSED_PARAMETER(type_data);
    return obs_module_text("BrowserAutoMask");
}

static bool process_name_matches(const char *list, const char *exe_name)
{
    if (!list || !*list || !exe_name || !*exe_name)
        return false;

    char *copy = bstrdup(list);
    if (!copy)
        return false;

    bool matched = false;
    char *ctx = NULL;

    for (char *line = strtok_s(copy, "\r\n", &ctx);
         line;
         line = strtok_s(NULL, "\r\n", &ctx)) {
        while (*line == ' ' || *line == '\t')
            line++;

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) {
            line[len - 1] = '\0';
            len--;
        }

        if (len == 0)
            continue;

        if (_stricmp(line, exe_name) == 0) {
            matched = true;
            break;
        }
    }

    bfree(copy);
    return matched;
}

static bool get_window_exe_name(HWND hwnd, char *buf, size_t buf_size)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (!pid)
        return false;

    HANDLE proc = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, pid);
    if (!proc)
        return false;

    char path[MAX_PATH] = {0};
    DWORD size = (DWORD)sizeof(path);
    bool ok = false;

    if (QueryFullProcessImageNameA(proc, 0, path, &size)) {
        const char *base = strrchr(path, '\\');
        base = base ? base + 1 : path;
        strncpy_s(buf, buf_size, base, _TRUNCATE);
        ok = true;
    }

    CloseHandle(proc);
    return ok;
}

static BOOL CALLBACK enum_windows_proc(HWND hwnd, LPARAM lparam)
{
    struct enum_ctx *ctx = (struct enum_ctx *)lparam;

    if (!IsWindowVisible(hwnd))
        return TRUE;
    if (IsIconic(hwnd))
        return TRUE;
    if (GetWindow(hwnd, GW_OWNER) != NULL)
        return TRUE;

    RECT r = {0};
    if (!GetWindowRect(hwnd, &r))
        return TRUE;
    if (r.right <= r.left || r.bottom <= r.top)
        return TRUE;

    char exe_name[MAX_PATH] = {0};
    if (!get_window_exe_name(hwnd, exe_name, sizeof(exe_name)))
        return TRUE;

    if (!process_name_matches(ctx->process_names, exe_name))
        return TRUE;

    RECT dwm_rect = {0};
    HRESULT hr = DwmGetWindowAttribute(
        hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &dwm_rect, sizeof(dwm_rect));

    if (SUCCEEDED(hr) &&
        dwm_rect.right > dwm_rect.left &&
        dwm_rect.bottom > dwm_rect.top) {
        ctx->rect = dwm_rect;
    } else {
        ctx->rect = r;
    }

    ctx->found = true;
    return FALSE;
}

static bool detect_browser_rect(const char *process_names, RECT *out_rect)
{
    if (!process_names || !*process_names || !out_rect)
        return false;

    struct enum_ctx ctx = {0};
    ctx.process_names = process_names;
    ctx.found = false;

    EnumWindows(enum_windows_proc, (LPARAM)&ctx);

    if (!ctx.found)
        return false;

    *out_rect = ctx.rect;
    return true;
}

static bool rects_intersect(const RECT *a, const RECT *b, RECT *out)
{
    RECT r;
    r.left = (a->left > b->left) ? a->left : b->left;
    r.top = (a->top > b->top) ? a->top : b->top;
    r.right = (a->right < b->right) ? a->right : b->right;
    r.bottom = (a->bottom < b->bottom) ? a->bottom : b->bottom;

    if (r.right <= r.left || r.bottom <= r.top)
        return false;

    if (out)
        *out = r;
    return true;
}

static void browser_mask_filter_update(void *data, obs_data_t *settings)
{
    struct browser_mask_filter_data *filter = data;
    if (!filter)
        return;

    bfree(filter->process_names);
    filter->process_names = bstrdup(obs_data_get_string(settings, "process_names"));
    filter->capture_left = (int)obs_data_get_int(settings, "capture_left");
    filter->capture_top = (int)obs_data_get_int(settings, "capture_top");
    filter->capture_width = (int)obs_data_get_int(settings, "capture_width");
    filter->capture_height = (int)obs_data_get_int(settings, "capture_height");
    filter->padding = (int)obs_data_get_int(settings, "padding");
    filter->debug_fullscreen_red = obs_data_get_bool(settings, "debug_fullscreen_red");
}

static void *browser_mask_filter_create(obs_data_t *settings, obs_source_t *source)
{
    struct browser_mask_filter_data *filter = bzalloc(sizeof(*filter));
    filter->context = source;
    browser_mask_filter_update(filter, settings);
    return filter;
}

static void browser_mask_filter_destroy(void *data)
{
    struct browser_mask_filter_data *filter = data;
    if (!filter)
        return;

    bfree(filter->process_names);
    bfree(filter);
}

static obs_properties_t *browser_mask_filter_properties(void *data)
{
    UNUSED_PARAMETER(data);

    obs_properties_t *props = obs_properties_create();

    obs_properties_add_text(
        props,
        "process_names",
        obs_module_text("ProcessNames"),
        OBS_TEXT_MULTILINE);

    obs_properties_add_int(
        props,
        "capture_left",
        obs_module_text("CaptureLeft"),
        -32768, 32767, 1);

    obs_properties_add_int(
        props,
        "capture_top",
        obs_module_text("CaptureTop"),
        -32768, 32767, 1);

    obs_properties_add_int(
        props,
        "capture_width",
        obs_module_text("CaptureWidth"),
        1, 10000, 1);

    obs_properties_add_int(
        props,
        "capture_height",
        obs_module_text("CaptureHeight"),
        1, 10000, 1);

    obs_properties_add_int(
        props,
        "padding",
        obs_module_text("Padding"),
        0, 1000, 1);

    obs_properties_add_bool(
        props,
        "debug_fullscreen_red",
        "Debug: always draw full-screen red overlay");

    return props;
}

static void browser_mask_filter_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(
        settings,
        "process_names",
        "chrome.exe\nmsedge.exe\nfirefox.exe\nbrave.exe");

    obs_data_set_default_int(settings, "capture_left", 0);
    obs_data_set_default_int(settings, "capture_top", 0);
    obs_data_set_default_int(settings, "capture_width", 3840);
    obs_data_set_default_int(settings, "capture_height", 2160);
    obs_data_set_default_int(settings, "padding", 8);
    obs_data_set_default_bool(settings, "debug_fullscreen_red", false);
}

static uint32_t browser_mask_filter_get_width(void *data)
{
    struct browser_mask_filter_data *filter = data;
    if (!filter || !filter->context)
        return 0;

    obs_source_t *target = obs_filter_get_target(filter->context);
    return target ? obs_source_get_base_width(target) : 0;
}

static uint32_t browser_mask_filter_get_height(void *data)
{
    struct browser_mask_filter_data *filter = data;
    if (!filter || !filter->context)
        return 0;

    obs_source_t *target = obs_filter_get_target(filter->context);
    return target ? obs_source_get_base_height(target) : 0;
}

static void draw_solid_rect(float x, float y, float w, float h,
                            float r, float g, float b, float a)
{
    gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
    if (!solid)
        return;

    gs_eparam_t *color_param = gs_effect_get_param_by_name(solid, "color");
    if (!color_param)
        return;

    struct vec4 color;
    vec4_set(&color, r, g, b, a);
    gs_effect_set_vec4(color_param, &color);

    gs_matrix_push();
    gs_matrix_identity();
    gs_matrix_translate3f(x, y, 0.0f);
    gs_matrix_scale3f(w, h, 1.0f);

    while (gs_effect_loop(solid, "Solid"))
        gs_draw_sprite(NULL, 0, 1, 1);

    gs_matrix_pop();
}

static void browser_mask_filter_render(void *data, gs_effect_t *effect)
{
    struct browser_mask_filter_data *filter = data;
    if (!filter || !filter->context)
        return;

    obs_source_t *target = obs_filter_get_target(filter->context);
    if (!target)
        return;

    uint32_t width = obs_source_get_base_width(target);
    uint32_t height = obs_source_get_base_height(target);

    if (width == 0 || height == 0) {
        width = obs_source_get_width(target);
        height = obs_source_get_height(target);
    }

    if (width == 0 || height == 0)
        return;

    if (!obs_source_process_filter_begin(filter->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING))
        return;

    gs_effect_t *draw_effect = effect ? effect : obs_get_base_effect(OBS_EFFECT_DEFAULT);
    obs_source_process_filter_end(filter->context, draw_effect, width, height);

    gs_blend_state_push();
    gs_reset_blend_state();

    if (filter->debug_fullscreen_red) {
        draw_solid_rect(0.0f, 0.0f, (float)width, (float)height, 1.0f, 0.0f, 0.0f, 0.35f);
        gs_blend_state_pop();
        return;
    }

    if (!filter->process_names || !*filter->process_names) {
        gs_blend_state_pop();
        return;
    }

    RECT browser_rect = {0};
    if (!detect_browser_rect(filter->process_names, &browser_rect)) {
        gs_blend_state_pop();
        return;
    }

    RECT capture_rect = {
        filter->capture_left,
        filter->capture_top,
        filter->capture_left + filter->capture_width,
        filter->capture_top + filter->capture_height
    };

    RECT intersection = {0};
    if (!rects_intersect(&browser_rect, &capture_rect, &intersection)) {
        gs_blend_state_pop();
        return;
    }

    blog(LOG_INFO,
         "[obs-browser-auto-mask] draw rect: left=%ld top=%ld right=%ld bottom=%ld",
         intersection.left, intersection.top, intersection.right, intersection.bottom);

    const int pad = filter->padding;
    float local_left = (float)(intersection.left - capture_rect.left - pad);
    float local_top = (float)(intersection.top - capture_rect.top - pad);
    float local_right = (float)(intersection.right - capture_rect.left + pad);
    float local_bottom = (float)(intersection.bottom - capture_rect.top + pad);

    if (local_left < 0.0f)
        local_left = 0.0f;
    if (local_top < 0.0f)
        local_top = 0.0f;
    if (local_right > (float)width)
        local_right = (float)width;
    if (local_bottom > (float)height)
        local_bottom = (float)height;

    float rect_w = local_right - local_left;
    float rect_h = local_bottom - local_top;

    if (rect_w > 0.0f && rect_h > 0.0f)
        draw_solid_rect(local_left, local_top, rect_w, rect_h, 0.0f, 0.0f, 0.0f, 1.0f);

    gs_blend_state_pop();
}

struct obs_source_info browser_mask_filter_info = {
    .id = "browser_mask_filter",
    .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = browser_mask_filter_get_name,
    .create = browser_mask_filter_create,
    .destroy = browser_mask_filter_destroy,
    .update = browser_mask_filter_update,
    .video_render = browser_mask_filter_render,
    .get_width = browser_mask_filter_get_width,
    .get_height = browser_mask_filter_get_height,
    .get_properties = browser_mask_filter_properties,
    .get_defaults = browser_mask_filter_defaults,
};