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

struct browser_mask_source_data {
    obs_source_t *context;
    char *target_source_name;
    char *process_names;
    char *monitor_id;
    int capture_left;
    int capture_top;
    int capture_width;
    int capture_height;
    int padding;
    bool debug_fullscreen_red;
    gs_texrender_t *texrender;
    gs_texture_t *black_tex;
    bool warned_missing_target;
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

struct source_list_ctx {
    obs_property_t *list;
    const char *current_name;
    bool added_any;
};

static const char *browser_mask_source_get_name(void *type_data)
{
    UNUSED_PARAMETER(type_data);
    return obs_module_text("BrowserAutoMaskSource");
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

    RECT candidate = r;
    if (SUCCEEDED(hr) && dwm_rect.right > dwm_rect.left && dwm_rect.bottom > dwm_rect.top)
        candidate = dwm_rect;

    if (!ctx->found) {
        ctx->rect = candidate;
        ctx->found = true;
    } else {
        if (candidate.left < ctx->rect.left)
            ctx->rect.left = candidate.left;
        if (candidate.top < ctx->rect.top)
            ctx->rect.top = candidate.top;
        if (candidate.right > ctx->rect.right)
            ctx->rect.right = candidate.right;
        if (candidate.bottom > ctx->rect.bottom)
            ctx->rect.bottom = candidate.bottom;
    }

    return TRUE;
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

static bool is_capture_source_id(const char *id)
{
    if (!id || !*id)
        return false;

    return strcmp(id, "monitor_capture") == 0 ||
           strcmp(id, "window_capture") == 0 ||
           strcmp(id, "game_capture") == 0;
}

static bool enum_target_sources_cb(void *param, obs_source_t *src)
{
    struct source_list_ctx *ctx = param;
    const char *id = obs_source_get_id(src);
    const char *name = obs_source_get_name(src);

    if (!ctx || !ctx->list || !name || !*name || !is_capture_source_id(id))
        return true;

    obs_property_list_add_string(ctx->list, name, name);
    ctx->added_any = true;
    return true;
}

static void populate_target_source_list(obs_property_t *list, const char *current_name)
{
    if (!list)
        return;

    obs_property_list_clear(list);
    obs_property_list_add_string(list, obs_module_text("AutoDetect"), "");

    struct source_list_ctx ctx = {0};
    ctx.list = list;
    ctx.current_name = current_name;
    obs_enum_sources(enum_target_sources_cb, &ctx);

    if (current_name && *current_name) {
        bool found = false;
        size_t count = obs_property_list_item_count(list);
        for (size_t i = 0; i < count; i++) {
            const char *val = obs_property_list_item_string(list, i);
            if (val && strcmp(val, current_name) == 0) {
                found = true;
                break;
            }
        }
        if (!found)
            obs_property_list_add_string(list, current_name, current_name);
    }
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

static bool ensure_black_texture(struct browser_mask_source_data *source)
{
    if (!source)
        return false;
    if (source->black_tex)
        return true;

    const uint8_t pixel[4] = {0, 0, 0, 255};
    const uint8_t *data[1] = {pixel};
    source->black_tex = gs_texture_create(1, 1, GS_RGBA, 1, data, 0);
    return source->black_tex != NULL;
}

static bool ensure_texrender(struct browser_mask_source_data *source)
{
    if (!source)
        return false;
    if (source->texrender)
        return true;

    source->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
    return source->texrender != NULL;
}

static void draw_solid_rect(struct browser_mask_source_data *source, float x, float y, float w, float h)
{
    if (!source || !ensure_black_texture(source) || w <= 0.0f || h <= 0.0f)
        return;

    obs_source_draw(source->black_tex, (int)x, (int)y, (uint32_t)w, (uint32_t)h, false);
}

static void browser_mask_source_update(void *data, obs_data_t *settings)
{
    struct browser_mask_source_data *source = data;
    if (!source)
        return;

    bfree(source->target_source_name);
    bfree(source->process_names);
    bfree(source->monitor_id);

    source->target_source_name = bstrdup(obs_data_get_string(settings, "target_source_name"));
    source->process_names = bstrdup(obs_data_get_string(settings, "process_names"));
    source->monitor_id = bstrdup(obs_data_get_string(settings, "monitor_id"));
    source->padding = (int)obs_data_get_int(settings, "padding");
    source->debug_fullscreen_red = obs_data_get_bool(settings, "debug_fullscreen_red");

    struct monitor_info info;
    if (get_monitor_info_by_id(source->monitor_id, &info)) {
        source->capture_left = info.rect.left;
        source->capture_top = info.rect.top;
        source->capture_width = info.rect.right - info.rect.left;
        source->capture_height = info.rect.bottom - info.rect.top;

        obs_data_set_int(settings, "capture_left", source->capture_left);
        obs_data_set_int(settings, "capture_top", source->capture_top);
        obs_data_set_int(settings, "capture_width", source->capture_width);
        obs_data_set_int(settings, "capture_height", source->capture_height);
    }
}

static void *browser_mask_source_create(obs_data_t *settings, obs_source_t *source)
{
    struct browser_mask_source_data *mask = bzalloc(sizeof(*mask));
    mask->context = source;
    ensure_texrender(mask);
    ensure_black_texture(mask);
    browser_mask_source_update(mask, settings);
    return mask;
}

static void browser_mask_source_destroy(void *data)
{
    struct browser_mask_source_data *source = data;
    if (!source)
        return;

    if (source->texrender)
        gs_texrender_destroy(source->texrender);
    if (source->black_tex)
        gs_texture_destroy(source->black_tex);
    bfree(source->target_source_name);
    bfree(source->process_names);
    bfree(source->monitor_id);
    bfree(source);
}

static obs_properties_t *browser_mask_source_properties(void *data)
{
    struct browser_mask_source_data *source = data;

    obs_properties_t *props = obs_properties_create();
    obs_property_t *target_prop = obs_properties_add_list(
        props,
        "target_source_name",
        obs_module_text("TargetSourceName"),
        OBS_COMBO_TYPE_LIST,
        OBS_COMBO_FORMAT_STRING);
    populate_target_source_list(target_prop, source ? source->target_source_name : NULL);
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

static void browser_mask_source_defaults(obs_data_t *settings)
{
    obs_data_set_default_string(settings, "target_source_name", "");
    obs_data_set_default_string(settings, "process_names", "chrome.exe\nmsedge.exe\nfirefox.exe\nbrave.exe");
    obs_data_set_default_string(settings, "monitor_id", "0");
    obs_data_set_default_int(settings, "padding", 8);
    obs_data_set_default_bool(settings, "debug_fullscreen_red", false);
    sync_capture_rect_from_monitor(settings);
}

static obs_source_t *get_named_target_source_ref(struct browser_mask_source_data *source,
                                              const char *name)
{
    if (!source || !name || !*name)
        return NULL;

    obs_source_t *target = obs_get_source_by_name(name);
    if (!target)
        return NULL;

    if (target == source->context) {
        obs_source_release(target);
        return NULL;
    }

    return target;
}

static obs_source_t *get_target_source_ref(struct browser_mask_source_data *source)
{
    static const char *fallback_names[] = {
        "画面キャプチャ",
        "Display Capture",
        "Screen Capture",
        "モニターキャプチャ",
        "ディスプレイキャプチャ",
        "Window Capture",
        "ウィンドウキャプチャ",
        NULL,
    };

    obs_source_t *target = NULL;

    if (!source)
        return NULL;

    if (source->target_source_name && *source->target_source_name)
        target = get_named_target_source_ref(source, source->target_source_name);

    if (target)
        return target;

    for (size_t i = 0; fallback_names[i] != NULL; i++) {
        target = get_named_target_source_ref(source, fallback_names[i]);
        if (target) {
            if (!source->warned_missing_target) {
                blog(LOG_INFO,
                     "[obs-browser-auto-mask] auto-selected target source: %s",
                     fallback_names[i]);
            }
            return target;
        }
    }

    return NULL;
}

static uint32_t browser_mask_source_get_width(void *data)
{
    struct browser_mask_source_data *source = data;
    obs_source_t *target = get_target_source_ref(source);
    uint32_t width = target ? obs_source_get_width(target) : 0;
    if (width == 0 && target)
        width = obs_source_get_base_width(target);
    if (target)
        obs_source_release(target);
    if (width == 0)
        width = source ? (uint32_t)(source->capture_width > 0 ? source->capture_width : 1920) : 1920;
    return width;
}

static uint32_t browser_mask_source_get_height(void *data)
{
    struct browser_mask_source_data *source = data;
    obs_source_t *target = get_target_source_ref(source);
    uint32_t height = target ? obs_source_get_height(target) : 0;
    if (height == 0 && target)
        height = obs_source_get_base_height(target);
    if (target)
        obs_source_release(target);
    if (height == 0)
        height = source ? (uint32_t)(source->capture_height > 0 ? source->capture_height : 1080) : 1080;
    return height;
}

static void browser_mask_source_render(void *data, gs_effect_t *effect)
{
    UNUSED_PARAMETER(effect);

    struct browser_mask_source_data *source = data;
    if (!source || !source->context)
        return;

    uint32_t width = browser_mask_source_get_width(source);
    uint32_t height = browser_mask_source_get_height(source);
    if (width == 0 || height == 0)
        return;

    obs_source_t *target = get_target_source_ref(source);
    if (!target) {
        if (!source->warned_missing_target) {
            blog(LOG_WARNING, "[obs-browser-auto-mask] target source not found: %s",
                 source->target_source_name ? source->target_source_name : "<empty>");
            source->warned_missing_target = true;
        }
        return;
    }
    source->warned_missing_target = false;

    if (ensure_texrender(source) && gs_texrender_begin(source->texrender, width, height)) {
        struct vec4 zero;
        vec4_zero(&zero);
        gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
        gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
        obs_source_video_render(target);
        gs_texrender_end(source->texrender);

        gs_texture_t *tex = gs_texrender_get_texture(source->texrender);
        if (tex)
            obs_source_draw(tex, 0, 0, width, height, false);
    } else {
        obs_source_video_render(target);
    }

    if (source->debug_fullscreen_red) {
        if (ensure_black_texture(source))
            obs_source_draw(source->black_tex, 0, 0, width, height, false);
        obs_source_release(target);
        return;
    }

    if (source->process_names && *source->process_names) {
        RECT browser_rect = {0};
        RECT capture_rect = {
            source->capture_left,
            source->capture_top,
            source->capture_left + source->capture_width,
            source->capture_top + source->capture_height
        };
        RECT intersection = {0};

        if (detect_browser_rect(source->process_names, &browser_rect) &&
            rects_intersect(&browser_rect, &capture_rect, &intersection)) {
            const int pad = source->padding;
            float scale_x = source->capture_width > 0 ? (float)width / (float)source->capture_width : 1.0f;
            float scale_y = source->capture_height > 0 ? (float)height / (float)source->capture_height : 1.0f;

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

            if (local_right > local_left && local_bottom > local_top)
                draw_solid_rect(source, local_left, local_top,
                                local_right - local_left, local_bottom - local_top);
        }
    }

    obs_source_release(target);
}

struct obs_source_info browser_mask_capture_source_info = {
    .id = "browser_mask_capture_source",
    .type = OBS_SOURCE_TYPE_INPUT,
    .output_flags = OBS_SOURCE_VIDEO,
    .get_name = browser_mask_source_get_name,
    .create = browser_mask_source_create,
    .destroy = browser_mask_source_destroy,
    .update = browser_mask_source_update,
    .video_render = browser_mask_source_render,
    .get_width = browser_mask_source_get_width,
    .get_height = browser_mask_source_get_height,
    .get_properties = browser_mask_source_properties,
    .get_defaults = browser_mask_source_defaults,
};
