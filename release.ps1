# Builds both apps, makes deltas against the previous GitHub release, and publishes vX.Y.Z.
# Usage: .\release.ps1 1.0.1
#
# Release assets (the updaters in windows/app.cpp and Updater.kt read these by name from releases/latest/download/):
#   openCam.exe, opencam_vcam.dll, openCam.apk     full files
#   <file>.from-<previous version>.delta            msdelta for exe/dll, OCD1 block delta for the APK
#   manifest.json                                   {"version": "...", "<file>": "<sha256>"}
param([Parameter(Mandatory)][string]$Version)
$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

Set-Content VERSION $Version -Encoding ascii
# Native tools write progress to stderr; under 'Stop', Windows PowerShell 5.1 would treat that as a failure
# when the output is redirected. Exit codes are checked instead.
& "$env:SystemRoot\System32\cmd.exe" /c "windows\build.bat 2>&1"
if ($LASTEXITCODE) { throw 'Windows build failed' }
Push-Location android
& "$env:SystemRoot\System32\cmd.exe" /c ".\gradlew.bat assembleRelease --console=plain -q 2>&1"
$rc = $LASTEXITCODE
Pop-Location
if ($rc) { throw 'Android build failed' }

$dist = Join-Path $PSScriptRoot "dist\$Version"
New-Item -ItemType Directory $dist -Force | Out-Null
Copy-Item windows\out\openCam.exe, windows\out\opencam_vcam.dll $dist
Copy-Item android\app\build\outputs\apk\release\app-release.apk "$dist\openCam.apk"
$files = 'openCam.exe', 'opencam_vcam.dll', 'openCam.apk'

$prev = gh release list --limit 1 --json tagName -q '.[0].tagName'
if ($prev -and $prev -ne "v$Version") {
    $old = Join-Path $PSScriptRoot "dist\$($prev.TrimStart('v'))-download"
    gh release download $prev -D $old --clobber -p openCam.exe -p opencam_vcam.dll -p openCam.apk
    foreach ($f in $files) {
        $p = Start-Process windows\out\openCam.exe -ArgumentList '--mkdelta', "`"$old\$f`"", "`"$dist\$f`"", "`"$dist\$f.from-$($prev.TrimStart('v')).delta`"" -Wait -PassThru
        if ($p.ExitCode) { throw "delta for $f failed" }
    }
}

$m = [ordered]@{ version = $Version }
foreach ($f in $files) { $m[$f] = (Get-FileHash "$dist\$f" -Algorithm SHA256).Hash.ToLower() }
$m | ConvertTo-Json | Set-Content "$dist\manifest.json" -Encoding ascii

git add -A
git commit -m "Release v$Version"
git tag "v$Version"
git push origin HEAD "v$Version"
gh release create "v$Version" (Get-ChildItem $dist -File).FullName --title "openCam v$Version" --generate-notes
