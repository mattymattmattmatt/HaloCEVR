@echo off
:: Remove old zip
del HaloCEVR.zip
:: Copy bindings/manifest files into correct subfolder
xcopy "./Extras/Bindings" "./Output/VR/OpenVR/" /E /I
:: Copy fonts into correct subfolder
xcopy "./Extras/Fonts" "./Output/VR/Fonts/" /E /I
:: Copy images into correct subfolder
xcopy "./Extras/Images" "./Output/VR/Images/" /E /I
:: Copy the config editor next to where config.txt gets generated
xcopy "./Extras/halocevr-config-editor.html" "./Output/VR/" /F
xcopy "./Extras/ConfigEditor/HaloCEVR Config Editor.exe" "./Output/VR/" /F
:: Remove any dev files used to generate the bindings
del ".\Output\VR\OpenVR\*.py"
:: copy openvr_api.dll to top level
xcopy "./ThirdParty/OpenVR/bin/openvr_api.dll" "./Output/" /F
:: Copy .dlls to top level
robocopy "./Release" "./Output" "*.dll"
:: Zip everything
tar -acf HaloCEVR.zip -C Output *.*
:: Remove temp directory
rmdir /s /q Output