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
copy /Y "C:\Users\DDdin\Desktop\obs-plugintemplate\data\locale\en-US.ini" "C:\Program Files\obs-studio\data\obs-plugins\obs-browser-auto-mask\locale\ja-JP.ini"

```
