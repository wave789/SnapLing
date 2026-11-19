param([string]$Build="build",[string]$QtBin="$env:Qt6_DIR/bin",[string]$Output="dist/SnapLing")
$ErrorActionPreference="Stop"
cmake --install $Build --config Release --prefix $Output
if ($LASTEXITCODE) { throw "Install failed" }
& "$QtBin/windeployqt.exe" --release --compiler-runtime --no-translations "$Output/bin/snapling.exe"
if ($LASTEXITCODE) { throw "Qt deployment failed" }
if (Test-Path "$Build/Release") {Get-ChildItem "$Build/Release/*.dll" | Copy-Item -Destination "$Output/bin"}
Copy-Item README.md,BUILD.md,LIMITATIONS.md,THIRD_PARTY.md,LICENSE -Destination $Output
Compress-Archive -Path "$Output/*" -DestinationPath "dist/SnapLing-Windows-x64.zip" -Force
