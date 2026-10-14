# Builds the release zip dist\OGL3D-v<version>.zip from bin\ (run build.ps1 -Arch all first):
#   OGL3D\x64\opengl32.dll, OGL3D\x86\opengl32.dll, OGL3D.ini, README.md, README.ko.md, LICENSE
$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$cm = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
if ($cm -notmatch 'project\(OGL3D VERSION ([0-9.]+)') { throw 'version not found in CMakeLists.txt' }
$ver = $Matches[1]
foreach ($a in 'x64', 'x86') {
    $dll = Join-Path $root "bin\$a\opengl32.dll"
    if (-not (Test-Path $dll)) { throw "missing $dll - run build.ps1 -Arch all" }
    $fv = (Get-Item $dll).VersionInfo.ProductVersion
    if ($fv -ne $ver) { throw "$dll is version $fv, CMakeLists.txt says $ver - rebuild" }
}
$stage = Join-Path $root 'dist\stage\OGL3D'
Remove-Item (Join-Path $root 'dist\stage') -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $stage 'x64'), (Join-Path $stage 'x86') | Out-Null
Copy-Item (Join-Path $root 'bin\x64\opengl32.dll') (Join-Path $stage 'x64')
Copy-Item (Join-Path $root 'bin\x86\opengl32.dll') (Join-Path $stage 'x86')
foreach ($f in 'OGL3D.ini', 'README.md', 'README.ko.md', 'LICENSE') { Copy-Item (Join-Path $root $f) $stage }
$zip = Join-Path $root "dist\OGL3D-v$ver.zip"
Remove-Item $zip -ErrorAction SilentlyContinue
Compress-Archive -Path $stage -DestinationPath $zip
Remove-Item (Join-Path $root 'dist\stage') -Recurse -Force
Write-Host "packaged: $zip ($((Get-Item $zip).Length) bytes)"
