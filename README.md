最初にobs-plugintemplateをクローンする
```cmd
git clone https://github.com/obsproject/obs-plugintemplate.git
```
downloadしたのを、ぽいぽいしてからビルド
```cmd
cd C:\Users\DDdin\Desktop\obs-plugintemplate
cmake --list-presets
cmake --preset windows-x64
cmake --build --preset windows-x64


taskkill /F /IM obs64.exe
taskkill /F /IM obs-browser-page.exe
taskkill /F /IM cef-bootstrap.exe
cd C:\Users\DDdin\Desktop\obs-plugintemplate
cmake --build --preset windows-x64
copy /Y "C:\Users\DDdin\Desktop\obs-plugintemplate\build_x64\RelWithDebInfo\obs-browser-auto-mask.dll" "C:\Program Files\obs-studio\obs-plugins\64bit\"
copy /Y "C:\Users\DDdin\Desktop\obs-plugintemplate\data\locale\en-US.ini" "C:\Program Files\obs-studio\data\obs-plugins\obs-browser-auto-mask\locale\en-US.ini"
copy /Y "C:\Users\DDdin\Desktop\obs-plugintemplate\data\locale\ja-JP.ini" "C:\Program Files\obs-studio\data\obs-plugins\obs-browser-auto-mask\locale\ja-JP.ini"

```


## Dedicated source mode

This version registers a dedicated input source instead of a filter. Add **Browser Auto Mask Source** to the scene, then set **Target Source Name** to the name of your Display Capture or Window Capture source. Resize the Browser Auto Mask Source in the scene, not the original capture source. Because the capture and mask are rendered inside the same source, they scale together.


## v2 dedicated source fix
- Renders the target source through a texrender first, then draws the mask on top.
- Adds a warning log when Target Source Name is not found.
- This is intended to avoid repeated `gs_effect_loop: An effect is already active` messages.


## Target source selection
- `Target source` is now a dropdown.
- Leave it on `Auto detect` to let the plugin try common capture sources automatically.
- Use `Refresh source list` after adding or renaming capture sources in OBS.
