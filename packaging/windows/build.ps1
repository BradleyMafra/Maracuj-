param(
    [string]$Version = "0.9.5",
    [string]$ProjectRoot = "",
    [string]$BuildDirectory = "build-windows",
    [string]$StageDirectory = "dist/windows/Maracuja",
    [string]$ArtifactsDirectory = "dist/artifacts",
    [string]$QtRoot = $env:QT_ROOT_DIR,
    [ValidateSet("Auto", "MSVC", "MinGW")][string]$Toolchain = "Auto",
    [string]$MinGWRoot = "",
    [string]$BuildPython = "python",
    [string]$ForbiddenTextJson = "",
    [switch]$SkipCompile,
    [switch]$SkipTests,
    [switch]$RecreateStage
)
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$Project = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$OwnershipProject = $Project
if ($ProjectRoot) { $Project = (Resolve-Path -LiteralPath $ProjectRoot).Path }
Set-Location $Project
$env:VSLANG = "1033"
if ($Version -notmatch '^\d+\.\d+\.\d+([.-][A-Za-z0-9.]+)?$') { throw "Invalid version" }
if (-not $QtRoot) { throw "Pass -QtRoot or set QT_ROOT_DIR to the Qt MSVC x64 installation" }
$BuildPython = (Get-Command $BuildPython -CommandType Application -ErrorAction Stop).Source
$QtRoot = (Resolve-Path $QtRoot).Path
if ($Toolchain -eq "Auto") { $Toolchain = if ($QtRoot -match "mingw") { "MinGW" } else { "MSVC" } }
if ($Toolchain -eq "MinGW") {
    if (-not $MinGWRoot) { throw "Pass -MinGWRoot to the private MinGW 13.1 installation" }
    $MinGWRoot = (Resolve-Path $MinGWRoot).Path
    $env:PATH = "$MinGWRoot\bin;$QtRoot\bin;$env:PATH"
}
$Build = [IO.Path]::GetFullPath((Join-Path $Project $BuildDirectory))
$Stage = [IO.Path]::GetFullPath((Join-Path $Project $StageDirectory))
if ($ForbiddenTextJson) {
    if (-not (Test-Path -LiteralPath $ForbiddenTextJson -PathType Leaf)) {
        throw "The private identifier JSON must be an existing file outside the release stage"
    }
    $ForbiddenTextJson = (Resolve-Path -LiteralPath $ForbiddenTextJson).Path
    $stagePrefix = $Stage.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
    if ($ForbiddenTextJson.Equals($Stage, [StringComparison]::OrdinalIgnoreCase) -or
        $ForbiddenTextJson.StartsWith($stagePrefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "The private identifier JSON must remain outside the release stage"
    }
}
$Cache = Join-Path $Project ".package-cache"
$Artifacts = [IO.Path]::GetFullPath((Join-Path $Project $ArtifactsDirectory))
function Run([string]$Program, [string[]]$Arguments) {
    $command = (Get-Command $Program -CommandType Application -ErrorAction Stop).Source
    Write-Host ("Running: " + $Program + " " + ($Arguments -join " "))
    $savedPreference = $ErrorActionPreference
    try {
        # PowerShell 5.1 wraps redirected native stderr in ErrorRecord objects.
        # Keep ordinary diagnostics in the transcript without treating them as
        # command failures; the native exit code remains authoritative.
        $ErrorActionPreference = "Continue"
        $global:LASTEXITCODE = $null
        & $command @Arguments 2>&1 | ForEach-Object { $_.ToString() } | Out-Host
        $commandExitCode = $global:LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $savedPreference
    }
    if ($null -eq $commandExitCode -or $commandExitCode -ne 0) {
        throw "Command failed: $Program (exit $commandExitCode)"
    }
}
$OwnerFile = $Stage + ".maracuja-owned.json"
$OwnershipStage = [IO.Path]::GetFullPath((Join-Path $OwnershipProject $StageDirectory))
if (Test-Path $Stage) {
    if (-not $RecreateStage -or -not (Test-Path $OwnerFile)) { throw "Staging directory already exists; choose a fresh directory" }
    $owner = Get-Content $OwnerFile -Raw | ConvertFrom-Json
    if ($owner.project -ne $OwnershipProject -or $owner.stage -ne $OwnershipStage) { throw "Refusing to remove an unowned staging directory" }
    $stageRoot = [IO.Path]::GetFullPath((Join-Path $Project "dist")) + [IO.Path]::DirectorySeparatorChar
    if (-not $Stage.StartsWith($stageRoot, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Staging recreation is restricted to the project's dist directory"
    }
    Remove-Item -LiteralPath $Stage -Recurse -Force
}
New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($Stage)) | Out-Null
@{ project = $OwnershipProject; stage = $OwnershipStage } | ConvertTo-Json | Set-Content $OwnerFile -Encoding UTF8
if (-not $SkipCompile) {
    $configure = @("-S", $Project, "-B", $Build, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release", "-DBUILD_TESTING=ON", "-DCMAKE_PREFIX_PATH=$QtRoot")
    $cmakeCache = Join-Path $Build "CMakeCache.txt"
    if (Test-Path -LiteralPath $cmakeCache) {
        $cacheContents = Get-Content -LiteralPath $cmakeCache -Raw
        $cachedSource = [regex]::Match($cacheContents, '(?m)^CMAKE_HOME_DIRECTORY:INTERNAL=(.+)$').Groups[1].Value.Trim()
        $expectedSource = $Project.Replace('\', '/').TrimEnd('/')
        # Refresh only when switching from a physical path or another drive
        # mapping. Preserve incremental compilation for ordinary repeat builds.
        if ($cachedSource.TrimEnd('/') -ne $expectedSource -or
            $cacheContents -match '(?m)^CMAKE_RC_COMPILER:[^=]+=.*\\') {
            $configure = @("--fresh") + $configure
        }
    }
    if ($Toolchain -eq "MinGW") {
        # CMake writes the RC compiler into generated .cmake source. Backslashes
        # such as C:\Users become invalid escapes when that file is included.
        $compilerRoot = $MinGWRoot.Replace('\', '/')
        $configure += @("-DCMAKE_C_COMPILER=$compilerRoot/bin/gcc.exe", "-DCMAKE_CXX_COMPILER=$compilerRoot/bin/g++.exe", "-DCMAKE_RC_COMPILER=$compilerRoot/bin/windres.exe")
    }
    Run "cmake" $configure
    Run "cmake" @("--build", $Build, "--parallel", "1")
}
New-Item -ItemType Directory -Force $Stage, $Cache, $Artifacts | Out-Null
Run $BuildPython @("-I", (Join-Path $Project "packaging/linux/fetch_runtime.py"), "--platform", "windows", "--stage", $Stage, "--cache", $Cache)
# Initialize bundled extension registrations once with synthetic input only.
# Every real document still uses a fresh private profile and security settings.
Run (Join-Path $Stage "runtime/python/python.exe") @("-I", "-B", (Join-Path $PSScriptRoot "prepare_office_profile.py"), "--stage", $Stage)
Copy-Item (Join-Path $Build "maracuja.exe") $Stage
$gesture = Join-Path $Build "maracuja-gesture-helper.exe"
if (Test-Path $gesture) { Copy-Item $gesture $Stage }
Run (Join-Path $QtRoot "bin/windeployqt.exe") @("--release", "--compiler-runtime", "--qmldir", (Join-Path $Project "qml"), "--dir", $Stage, (Join-Path $Stage "maracuja.exe"))
if ($Toolchain -eq "MSVC") {
    if (-not $env:VCToolsRedistDir) { throw "The MSVC build requires the x64 developer terminal (VCToolsRedistDir missing)" }
    $CrtDirectories = @(Get-ChildItem (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT")
    if ($CrtDirectories.Count -ne 1) { throw "MSVC redistributable CRT directory missing or ambiguous" }
    Get-ChildItem $CrtDirectories[0].FullName -Filter "*.dll" | Copy-Item -Destination $Stage
}
foreach ($relative in @("bin/Qt6Svg.dll", "plugins/imageformats/qsvg.dll", "plugins/iconengines/qsvgicon.dll", "plugins/platforms/qoffscreen.dll")) {
    $source = Join-Path $QtRoot $relative
    if (-not (Test-Path $source)) { throw "Qt runtime dependency missing: $relative" }
    $destination = if ($relative.StartsWith("bin/")) { Join-Path $Stage ([IO.Path]::GetFileName($relative)) } else { Join-Path $Stage ($relative.Substring(8)) }
    New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($destination)) | Out-Null
    Copy-Item $source $destination
}
if (Test-Path (Join-Path $QtRoot "LICENSES")) { Copy-Item (Join-Path $QtRoot "LICENSES") (Join-Path $Stage "licenses/Qt") -Recurse }
if ($Toolchain -eq "MinGW") {
    $compilerLicenses = Join-Path $MinGWRoot "licenses"
    foreach ($relative in @("gcc/COPYING3", "gcc/COPYING.RUNTIME", "mingw-w64/COPYING.MinGW-w64-runtime.txt", "winpthreads/COPYING")) {
        if (-not (Test-Path -LiteralPath (Join-Path $compilerLicenses $relative) -PathType Leaf)) {
            throw "A required MinGW runtime license document is missing"
        }
    }
    $compilerNoticeStage = Join-Path $Stage "licenses/MinGW"
    New-Item -ItemType Directory -Force $compilerNoticeStage | Out-Null
    Get-ChildItem -LiteralPath $compilerLicenses -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $compilerNoticeStage -Recurse
    }
}
Copy-Item (Join-Path $Project "assets") (Join-Path $Stage "assets") -Recurse
foreach ($name in @("LICENSE", "README.md", "README_WINDOWS.md", "THIRD_PARTY_NOTICES.md", "CREDITS.md")) {
    $path = Join-Path $Project $name
    if (Test-Path $path) { Copy-Item $path $Stage }
}
$BuildPath = $env:PATH
$env:PATH = "$Stage;$(Join-Path $Stage 'tools');$QtRoot\bin;$BuildPath"
$env:MARACUJA_RUNTIME_CONFIG = Join-Path $Stage "runtime-paths.json"
$env:QT_PLUGIN_PATH = $Stage
$env:QML2_IMPORT_PATH = Join-Path $Stage "qml"
$env:QML_IMPORT_PATH = Join-Path $Stage "qml"
$PrivatePython = Join-Path $Stage "runtime/python/python.exe"
if (-not $SkipTests) {
    $env:QT_FORCE_STDERR_LOGGING = "1"
    # Qt's offscreen backend does not discover the native Windows fonts.
    # Use the installed fonts so QML layout checks measure real text metrics.
    if (-not $env:QT_QPA_FONTDIR) { $env:QT_QPA_FONTDIR = Join-Path $env:SystemRoot "Fonts" }
    Run "ctest" @("--test-dir", $Build, "--output-on-failure", "--timeout", "180", "-j1")
    Run $BuildPython @("-I", (Join-Path $Project "tests/archive_privacy_check.py"))
    Run $BuildPython @("-I", "-B", (Join-Path $Project "tests/windows_office_profile_check.py"))
    Run $BuildPython @("-I", "-B", (Join-Path $Project "tests/windows_python_launchers_check.py"))
    Run $BuildPython @("-I", (Join-Path $Project "tests/release_privacy_check.py"))
    Run $BuildPython @("-I", "-B", (Join-Path $Project "tests/qt_notices_check.py"))
    Run $BuildPython @("-I", "-B", (Join-Path $Project "tests/graphics_notices_check.py"))
    try {
        # Validate the distribution without development SDK/compiler DLLs on PATH.
        # Use the physical path so the temporary ASCII build drive cannot hide
        # runtime failures in installation paths containing accented characters.
        $env:PATH = "$OwnershipStage;$(Join-Path $OwnershipStage 'tools');$env:SystemRoot\System32;$env:SystemRoot"
        $env:MARACUJA_RUNTIME_CONFIG = Join-Path $OwnershipStage "runtime-paths.json"
        $env:QT_PLUGIN_PATH = $OwnershipStage
        $env:QML2_IMPORT_PATH = Join-Path $OwnershipStage "qml"
        $env:QML_IMPORT_PATH = Join-Path $OwnershipStage "qml"
        $PrivatePython = Join-Path $OwnershipStage "runtime/python/python.exe"
        Run $PrivatePython @("-I", (Join-Path $OwnershipProject "tests/platform_runtime_check.py"))
        Run $PrivatePython @("-I", "-B", (Join-Path $OwnershipProject "tests/document_worker_check.py"))
        Run $PrivatePython @("-I", "-B", (Join-Path $OwnershipProject "tests/ocr_threshold_check.py"))
        Run $PrivatePython @("-I", (Join-Path $OwnershipProject "tests/file_tools_check.py"), (Join-Path $OwnershipStage "file_tools.py"))
        foreach ($script in @("tests/integration_check.py", "tests/expanded_formats_check.py", "tests/ocr_check.py", "packaging/linux/smoke.py")) {
            Run $PrivatePython @("-I", (Join-Path $OwnershipProject $script), (Join-Path $OwnershipStage "maracuja.exe"))
        }
    }
    finally {
        $env:PATH = $BuildPath
    }
}
$env:PATH = $BuildPath
# Optional wheel console tools embed the build interpreter's absolute path.
# The app imports these libraries through its private Python and never uses
# these entrypoints; omit only the explicitly reviewed seven launchers.
Run $BuildPython @("-I", "-B", (Join-Path $PSScriptRoot "prune_python_launchers.py"), "--stage", $Stage)
# Validation imports can create caches containing this machine's source paths.
# Keep sourceless modules, sanitize traceback paths, then compute shipped hashes.
$OfficePython = Join-Path $Stage "runtime/libreoffice/program/python.exe"
$PrivatePython = Join-Path $Stage "runtime/python/python.exe"
if (-not (Test-Path -LiteralPath $OfficePython -PathType Leaf)) { throw "Packaged LibreOffice Python missing" }
# LibreOffice carries its own Python minor version. Each interpreter inspects
# only its matching bytecode; together these scopes cover the complete stage.
Run $OfficePython @("-I", "-B", (Join-Path $Project "packaging/archive_privacy.py"), "--stage", $Stage, "--subtree", "runtime/libreoffice")
Run $PrivatePython @("-I", "-B", (Join-Path $Project "packaging/archive_privacy.py"), "--stage", $Stage, "--exclude", "runtime/libreoffice")
# The packaged template must still match after bytecode/privacy normalization.
Run $PrivatePython @("-I", "-B", (Join-Path $PSScriptRoot "prepare_office_profile.py"), "--stage", $Stage, "--check")
Run $BuildPython @("-I", "-B", (Join-Path $PSScriptRoot "collect_qt_notices.py"), "--stage", $Stage, "--cache", $Cache)
Run $BuildPython @("-I", "-B", (Join-Path $PSScriptRoot "collect_graphics_notices.py"), "--stage", $Stage, "--cache", $Cache)
Run $BuildPython @("-I", "-B", (Join-Path $PSScriptRoot "license_manifest.py"), "--stage", $Stage, "--version", $Version)
$privacyArguments = @("-I", "-B", (Join-Path $Project "packaging/check_release_privacy.py"), "--stage", $Stage)
if ($ForbiddenTextJson) { $privacyArguments += @("--forbidden-text-json", $ForbiddenTextJson) }
# These release gates also run with -SkipTests. Inspect every file, including
# ZIP members, with the interpreter matching its runtime's bytecode version.
Run $OfficePython ($privacyArguments + @("--subtree", "runtime/libreoffice"))
Run $PrivatePython ($privacyArguments + @("--exclude", "runtime/libreoffice"))
$RuntimeLock = Get-Content (Join-Path $Project "packaging/linux/runtime.lock.json") -Raw | ConvertFrom-Json
$InnoFile = Join-Path $Cache "innosetup-7.1.0-x64.exe"
if (-not (Test-Path $InnoFile) -or (Get-FileHash $InnoFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $RuntimeLock.innosetup.sha256) {
    Invoke-WebRequest -Uri $RuntimeLock.innosetup.url -OutFile $InnoFile
}
if ((Get-FileHash $InnoFile -Algorithm SHA256).Hash.ToLowerInvariant() -ne $RuntimeLock.innosetup.sha256) { throw "Inno Setup SHA256 mismatch" }
$InnoDirectory = Join-Path $Cache "inno-7.1.0"
if (-not (Test-Path (Join-Path $InnoDirectory "ISCC.exe"))) {
    $install = Start-Process -FilePath $InnoFile -ArgumentList @("/CURRENTUSER", "/NOICONS", "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/DIR=`"$InnoDirectory`"") -WindowStyle Hidden -Wait -PassThru
    if ($install.ExitCode -ne 0) { throw "Private Inno Setup preparation failed" }
}
if (-not $SkipTests) {
    # A tiny harness cancels before installation and exercises the production
    # native probe against locked files and its own mapped executable image.
    Run $BuildPython @("-I", "-B", (Join-Path $Project "tests/windows_installer_shutdown_check.py"), "--iscc", (Join-Path $InnoDirectory "ISCC.exe"))
}
Run (Join-Path $InnoDirectory "ISCC.exe") @("/DAppVersion=$Version", "/DStageDir=$Stage", "/DOutputDir=$Artifacts", (Join-Path $PSScriptRoot "Maracuja.iss"))
$zip = Join-Path $Artifacts "Maracuja-$Version-windows-x64-portable.zip"
if ((Test-Path $zip) -and $RecreateStage) { Remove-Item $zip -Force }
Run $BuildPython @("-I", (Join-Path $PSScriptRoot "archive.py"), $Stage, $zip)
foreach ($artifactPath in @($zip, (Join-Path $Artifacts "Maracuja-$Version-Setup-x64.exe"))) {
    if (-not (Test-Path -LiteralPath $artifactPath -PathType Leaf)) { throw "Expected current-version artifact missing" }
    $line = (Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash.ToLowerInvariant() + "  " + [IO.Path]::GetFileName($artifactPath)
    [IO.File]::WriteAllText(($artifactPath + ".sha256"), $line + "`n", [Text.UTF8Encoding]::new($false))
}
Write-Output "Windows artifacts created after completed checks. Windows gesture still needs manual desktop validation."
