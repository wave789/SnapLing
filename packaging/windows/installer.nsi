!include "MUI2.nsh"
Name "SnapLing"
OutFile "SnapLing-Setup.exe"
InstallDir "$LOCALAPPDATA\Programs\SnapLing"
RequestExecutionLevel user
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "SimpChinese"
Section
SetOutPath "$INSTDIR"
File /r "..\..\dist\SnapLing\*"
CreateShortcut "$SMPROGRAMS\SnapLing.lnk" "$INSTDIR\bin\snapling.exe"
WriteUninstaller "$INSTDIR\Uninstall.exe"
WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SnapLing" "DisplayName" "SnapLing"
WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SnapLing" "UninstallString" '"$INSTDIR\Uninstall.exe"'
SectionEnd
Section "Uninstall"
Delete "$SMPROGRAMS\SnapLing.lnk"
DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "SnapLing"
DeleteRegKey HKCU "Software\Microsoft\Windows\CurrentVersion\Uninstall\SnapLing"
RMDir /r "$INSTDIR"
SectionEnd
