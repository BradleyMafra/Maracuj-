param([string]$Version = "0.9.5")
$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest
$ProgressPreference = "SilentlyContinue"
if ($env:OS -ne "Windows_NT" -or -not [Environment]::Is64BitProcess) {
    throw "Run this script in 64-bit Windows 10/11 PowerShell."
}
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$Project = (Resolve-Path (Join-Path $PSScriptRoot "../..")).Path
$OriginalProject = $Project
$MappedDrive = $null
$Tools = Join-Path $Project ".build-tools"
$Cache = Join-Path $Project ".package-cache"
$Logs = Join-Path $Project "validation"
New-Item -ItemType Directory -Force $Tools, $Cache, $Logs | Out-Null
$LogFile = Join-Path $Logs ("build-windows-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
Start-Transcript -Path $LogFile | Out-Null
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
function Download-Verified($Record) {
    $name = [Uri]::UnescapeDataString(([Uri]$Record.url).AbsolutePath.Split('/')[-1])
    $file = Join-Path $Cache $name
    if ((Test-Path -LiteralPath $file) -and (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant() -eq $Record.sha256) {
        Write-Host "Using verified download: $name"
        return $file
    }
    $partial = $file + ".partial"
    for ($attempt = 1; $attempt -le 3; $attempt++) {
        try {
            Write-Host "Downloading $name (attempt $attempt/3): $($Record.url)"
            Invoke-WebRequest -Uri $Record.url -OutFile $partial -UseBasicParsing -TimeoutSec 300
            if ((Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant() -ne $Record.sha256) {
                throw "SHA256 mismatch for $name"
            }
            Move-Item -LiteralPath $partial -Destination $file -Force
            Write-Host "Verified SHA256: $name"
            return $file
        }
        catch {
            if (Test-Path -LiteralPath $partial) { Remove-Item -LiteralPath $partial -Force }
            if ($attempt -eq 3) { throw }
            Write-Warning ("Download failed for " + $name + ": " + $_.Exception.Message + "; retrying.")
            Start-Sleep -Seconds (2 * $attempt)
        }
    }
}
try {
    Write-Output "Preparing private Windows build tools. No Visual Studio or global Python/Qt installation is required."
    # Qt's MinGW command-line tools do not consistently decode non-ASCII argv.
    # A temporary SUBST drive keeps the real checkout in its existing location.
    if ($Project -match '[^\x00-\x7f]') {
        $MappedDrive = [char[]](90..68) | ForEach-Object { [string]$_ + ':' } |
            Where-Object { -not (Test-Path ($_ + '\')) } | Select-Object -First 1
        if (-not $MappedDrive) { throw "No free drive letter for the temporary ASCII build path" }
        & (Join-Path $env:SystemRoot "System32/subst.exe") $MappedDrive $OriginalProject
        if ($LASTEXITCODE -ne 0) { $MappedDrive = $null; throw "Cannot create temporary build drive" }
        $Project = $MappedDrive + '\'
        $Tools = Join-Path $Project ".build-tools"
        $Cache = Join-Path $Project ".package-cache"
        Write-Host "Temporary build path: $Project -> $OriginalProject"
    }
    $runtime = Get-Content (Join-Path $Project "packaging/linux/runtime.lock.json") -Raw | ConvertFrom-Json
    $bootstrap = Get-Content (Join-Path $PSScriptRoot "bootstrap.lock.json") -Raw | ConvertFrom-Json
    Write-Host "[1/5] Preparing Python."
    $PythonArchive = Download-Verified $runtime.python_windows
    $Python = Join-Path $Tools "python/python.exe"
    $PythonMarker = Join-Path $Tools "python/.maracuja-archive-sha256"
    $PythonReady = (Test-Path -LiteralPath $Python) -and (Test-Path -LiteralPath $PythonMarker) -and
        (Get-Content -LiteralPath $PythonMarker -Raw).Trim() -eq $runtime.python_windows.sha256
    if (-not $PythonReady) {
        Write-Host "Extracting private Python (also repairs an interrupted extraction)."
        if (Test-Path -LiteralPath $PythonMarker) { Remove-Item -LiteralPath $PythonMarker -Force }
        Run (Join-Path $env:SystemRoot "System32/tar.exe") @("-xf", $PythonArchive, "-C", $Tools)
        if (-not (Test-Path -LiteralPath $Python)) { throw "Private Python extraction failed" }
        Run $Python @("-I", "-m", "pip", "--version")
        Set-Content -LiteralPath $PythonMarker -Value $runtime.python_windows.sha256 -Encoding ASCII
    }
    Write-Host "[2/5] Preparing CMake and Ninja."
    foreach ($name in @("cmake", "ninja")) {
        $record = $bootstrap.PSObject.Properties[$name].Value
        $archive = Download-Verified $record
        $destination = Join-Path $Tools $name
        $marker = Join-Path $destination ".maracuja-archive-sha256"
        $executable = if ($name -eq "cmake") {
            Join-Path $destination (([IO.Path]::GetFileNameWithoutExtension($archive)) + "/bin/cmake.exe")
        } else { Join-Path $destination "ninja.exe" }
        $ready = (Test-Path -LiteralPath $executable) -and (Test-Path -LiteralPath $marker) -and
            (Get-Content -LiteralPath $marker -Raw).Trim() -eq $record.sha256
        if (-not $ready) {
            Write-Host "Extracting $name (also repairs an interrupted extraction)."
            if (Test-Path -LiteralPath $marker) { Remove-Item -LiteralPath $marker -Force }
            Expand-Archive -LiteralPath $archive -DestinationPath $destination -Force
            if (-not (Test-Path -LiteralPath $executable)) { throw "Private $name extraction failed" }
            Run $executable @("--version")
            Set-Content -LiteralPath $marker -Value $record.sha256 -Encoding ASCII
        }
        if ($name -eq "cmake") { $CMake = Get-Item -LiteralPath $executable }
    }
    $Ninja = Join-Path $Tools "ninja/ninja.exe"
    if (-not $CMake -or -not (Test-Path $Ninja)) { throw "Private CMake/Ninja extraction failed" }
    $env:PATH = "$($CMake.DirectoryName);$(Split-Path $Ninja);$env:PATH"
    Write-Host "[3/5] Installing verified Python build dependencies."
    Run $Python @("-I", "-m", "pip", "--isolated", "install", "--disable-pip-version-check", "--only-binary=:all:", "--require-hashes", "--cache-dir", (Join-Path $Cache "pip-build"), "-r", (Join-Path $PSScriptRoot "build-requirements.lock"))
    Write-Host "[4/5] Preparing the verified Qt and MinGW SDK."
    Run $Python @("-I", (Join-Path $PSScriptRoot "prepare_qt.py"), "--tools", $Tools, "--cache", $Cache)
    $QtRoot = Join-Path $Tools "Qt/6.8.3/mingw_64"
    $MinGW = Join-Path $Tools "Qt/Tools/mingw1310_64"
    if (-not (Test-Path (Join-Path $MinGW "bin/g++.exe"))) { throw "Private MinGW compiler missing" }
    Write-Host "[5/5] Compiling, testing and packaging Maracuja for Windows."
    & (Join-Path $PSScriptRoot "build.ps1") -Version $Version -ProjectRoot $Project -Toolchain MinGW -QtRoot $QtRoot -MinGWRoot $MinGW -BuildPython $Python -BuildDirectory "build-windows-mingw" -RecreateStage
    if ($LASTEXITCODE -ne 0) { throw "Packaging failed" }
    Write-Output "Completed. Installer/portable ZIP: $OriginalProject\dist\artifacts"
    Write-Output "Compilation checks do not replace an interactive Windows desktop gesture test."
}
catch {
    Write-Error ("Windows build did not complete: " + $_.Exception.Message) -ErrorAction Continue
    Write-Output "Diagnostic log: $LogFile"
    exit 1
}
finally {
    if ($MappedDrive) {
        Set-Location -LiteralPath $OriginalProject
        & (Join-Path $env:SystemRoot "System32/subst.exe") $MappedDrive /D
    }
    Stop-Transcript | Out-Null
}
