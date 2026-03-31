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
#include <stdio.h>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "psapi.lib")

struct browser_mask_filter_data {
    obs_source_t *context;
    char *process_names;
    char *monitor_id;
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

struct monitor_info {
    char id[32];
    RECT rect;
};

struct monitor_enum_ctx {
    obs_property_t *list;
    int index;
    bool found;
    const char *target_id;
    struct monitor_info *out_info;
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
    HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &dwm_rect, sizeof(dwm_rect));

    if (SUCCEEDED(hr) && dwm_rect.right > dwm_rect.left && dwm_rect.bottom > dwm_rect.top)
        ctx->rect = dwm_rect;
    else
        ctx->rect = r;

    ctx->found = true;
    return FALSE;
}

static bool detect_browser_rect(const char *process_names, RECT *out_rect)
{
    if (!process_names || !*process_names || !out_rect)
        return false;

    struct enum_ctx ctx = {0};
    ctx.process_names = process_names;
    EnumWindows(enum_windows_proc, (LPARAM)&ctx);

    if (!ctx.found)
        return false;

    *out_rect = ctx.rect;
    return true;
}

static BOOL CALLBACK enum_monitors_fill_list_proc(HMONITOR monitor, HDC hdc, LPRECT rect, LPARAM lparam)
{
    UNUSED_PARAMETER(monitor);
    UNUSED_PARAMETER(hdc);

    struct monitor_enum_ctx *ctx = (struct monitor_enum_ctx *)lparam;
    char value[32];
    char label[128];
    int width = rect->right - rect->left;
    int height = rect->bottom - rect->top;

    snprintf(value, sizeof(value), "%d", ctx->index);
    snprintf(label, sizeof(label), "%s %d: %dx%d (%ld,%ld)",
             obs_module_text("MonitorLabel"),
             ctx->index + 1,
             width,
             height,
             rect->left,
             rect->top);

    obs_property_list_add_string(ctx->list, label, value);
    ctx->index++;
    return TRUE;
}

static void populate_monitor_list(obs_property_t *list)
{
    if (!list)
        return;

    obs_property_list_clear(list);
    struct monitor_enum_ctx ctx = {0};
    ctx.list = list;
    EnumDisplayMonitors(NULL, NULL, enum_monitors_fill_list_proc, (LPARAM)&ctx);
}

static BOOL CALLBACK enum_monitors_find_proc(HMONITOR monitor, HDC hdc, LPRECT rect, LPARAM lparam)
{
    UNUSED_PARAMETER(monitor);
    UNUSED_PARAMETER(hdc);

    struct monitor_enum_ctx *ctx = (struct monitor_enum_ctx *)lparam;
    char value[32];
    snprintf(value, sizeof(value), "%d", ctx->index);

    if (ctx->target_id && strcmp(ctx->target_id, value) == 0) {
        if (ctx->out_info) {
            strncpy_s(ctx->out_info->id, sizeof(ctx->out_info->id), value, _TRUNCATE);
            ctx->out_info->rect = *rect;
        }
        ctx->found = true;
        return FALSE;
    }

    ctx->index++;
    return TRUE;
}

static bool get_monitor_info_by_id(const char *monitor_id, struct monitor_info *out_info)
{
    if (!monitor_id || !*monitor_id || !out_info)
        return false;

    struct monitor_enum_ctx ctx = {0};
    ctx.target_id = monitor_id;
    ctx.out_info = out_info;
    EnumDisplayMonitors(NULL, NULL, enum_monitors_find_proc, (LPARAM)&ctx);
    return ctx.found;
}

static void sync_capture_rect_from_monitor(obs_data_t *settings)
{
    if (!settings)
        return;

    const char *monitor_id = obs_data_get_string(settings, "monitor_id");
    struct monitor_info info;
    if (!get_monitor_info_by_id(monitor_id, &info))
        return;

    obs_data_set_int(settings, "capture_left", info.rect.left);
    obs_data_set_int(settings, "capture_top", info.rect.top);
    obs_data_set_int(settings, "capture_width", info.rect.right - info.rect.left);
    obs_data_set_int(settings, "capture_height", info.rect.bottom - info.rect.top);
}

static bool monitor_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
    UNUSED_PARAMETER(props);
    UNUSED_PARAMETER(property);
    sync_capture_rect_from_monitor(settings);
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
    bfree(filter->monitor_id);

    filter->process_names = bstrdup(obs_data_get_string(settings, "process_names"));
    filter->monitor_id = bstrdup(obs_data_get_string(settings, "monitor_id"));
    filter->padding = (int)obs_data_get_int(settings, "padding");
    filter->debug_fullscreen_red = obs_data_get_bool(settings, "debug_fullscreen_red");

    struct monitor_info info;
    if (get_monitor_info_by_id(filter->monitor_id, &info)) {
        filter->capture_left = info.rect.left;
        filter->capture_top = info.rect.top;
        filter->capture_width = info.rect.right - info.rect.left;
        filter->capture_height = info.rect.bottom - info.rect.top;

        obs_data_set_int(settings, "capture_left", filter->capture_left);
        obs_data_set_int(settings, "capture_top", filter->capture_top);
        obs_data_set_int(settings, "capture_width", filter->capture_width);
        obs_data_set_int(settings, "capture_height", filter->capture_height);
    }
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
    bfree(filter->monitor_id);
    bfree(filter);
}

static obs_properties_t *browser_mask_filter_properties(void *data)
{
    UNUSED_PARAMETER(data);

    obs_properties_t *props = obs_properties_create();

    obs_properties_add_text(props, "process_names", obs_module_text("ProcessNames"), OBS_TEXT_MULTILINE);

    obs_property_t *monitor_prop = obs_properties_add_list(
        props,
        "monitor_id",
        obs_module_text("TargetMonitor"),
        OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_STRING);
    populate_monitor_list(monitor_prop);
    obs_property_set_modified_callback(monitor_prop, monitor_modified);

    obs_properties_add_int(props, "padding", obs_module_text("Padding"), 0, 1000, 1);
    obs_properties_add_bool(props, "debug_fullscreen_red", obs_module_text("DebugAlwaysRed"));

    return props;
}

static void browser_mask_filter_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "process_names", "chrome.exe\nmsedge.exe\nfirefox.exe\nbrave.exe");
    obs_data_set_default_string(settings, "monitor_id", "0");
    obs_data_set_default_int(settings, "padding", 8);
    obs_data_set_default_bool(settings, "debug_fullscreen_red", false);
    sync_capture_rect_from_monitor(settings);
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

static void draw_solid_rect(float x, float y, float w, float h, float r, float g, float b, float a)
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

    const int pad = filter->padding;
    float scale_x = filter->capture_width > 0 ? (float)width / (float)filter->capture_width : 1.0f;
    float scale_y = filter->capture_height > 0 ? (float)height / (float)filter->capture_height : 1.0f;

    float local_left = ((float)(intersection.left - capture_rect.left) - (float)pad) * scale_x;
    float local_top = ((float)(intersection.top - capture_rect.top) - (float)pad) * scale_y;
    float local_right = ((float)(intersection.right - capture_rect.left) + (float)pad) * scale_x;
    float local_bottom = ((float)(intersection.bottom - capture_rect.top) + (float)pad) * scale_y;

    if (local_left < 0.0f)
        local_left = 0.0f;
    if (local_top < 0.0f)
        local_top = 0.0f;
    if (local_right > (float)width)
        local_right = (float)width;
    if (local_bottom > (float)height)
        local_bottom = (float)height;

    blog(LOG_INFO,
         "[obs-browser-auto-mask] monitor-only-v1 browser=(%ld,%ld,%ld,%ld) capture=(%ld,%ld,%ld,%ld) render=%ux%u scale=(%.4f,%.4f) final=(%.1f,%.1f,%.1f,%.1f)",
         browser_rect.left, browser_rect.top, browser_rect.right, browser_rect.bottom,
         capture_rect.left, capture_rect.top, capture_rect.right, capture_rect.bottom,
         width, height,
         scale_x, scale_y,
         local_left, local_top, local_right, local_bottom);

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
