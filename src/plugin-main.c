#include <obs-module.h>
#include "browser-mask-filter.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-browser-auto-mask", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
    return "Browser auto mask filter";
}

bool obs_module_load(void)
{
    obs_register_source(&browser_mask_filter_info);
    blog(LOG_INFO, "[obs-browser-auto-mask] loaded");
    return true;
}
