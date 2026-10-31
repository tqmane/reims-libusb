param(
    [Parameter(Mandatory)][string]$Output,
    [Parameter(Mandatory)][string]$CompilerDirectory,
    [Parameter(Mandatory)][string]$SdkDirectory,
    [Parameter(Mandatory)][string]$SdkVersion
)
$ErrorActionPreference = 'Stop'
$source = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$destination = [IO.Path]::GetFullPath($Output)
if ($destination.Equals($source, [StringComparison]::OrdinalIgnoreCase) -or
    $destination.StartsWith($source + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Build output must be outside the source tree.'
}
$compiler = Join-Path $CompilerDirectory 'cl.exe'
$linker = Join-Path $CompilerDirectory 'link.exe'
$vc = [IO.Path]::GetFullPath((Join-Path $CompilerDirectory '../../..'))
$env:INCLUDE = @((Join-Path $vc 'include'), (Join-Path $SdkDirectory "Include/$SdkVersion/ucrt"),
    (Join-Path $SdkDirectory "Include/$SdkVersion/shared"), (Join-Path $SdkDirectory "Include/$SdkVersion/um")) -join ';'
$env:LIB = @((Join-Path $vc 'lib/x64'), (Join-Path $SdkDirectory "Lib/$SdkVersion/ucrt/x64"),
    (Join-Path $SdkDirectory "Lib/$SdkVersion/um/x64")) -join ';'
$objects = Join-Path $destination 'obj'
New-Item -ItemType Directory -Force -Path $objects, (Join-Path $destination 'tmp') | Out-Null
$env:TEMP = $env:TMP = Join-Path $destination 'tmp'
$common = @('/nologo', '/c', '/O2', '/MD', '/W4', '/WX', '/utf-8', '/Gy', '/Gw',
    '/D_CRT_SECURE_NO_WARNINGS', '/D_WIN32_WINNT=0x0600', "/I$source/msvc", "/I$source/libusb")
$sources = @('core', 'descriptor', 'hotplug', 'io', 'strerror', 'sync',
    'os/events_windows', 'os/threads_windows', 'os/windows_common', 'os/windows_usbdk', 'os/windows_winusb')
$compiled = @()
foreach ($name in $sources) {
    $object = Join-Path $objects (($name.Replace('/', '_')) + '.obj')
    & $compiler @common (Join-Path $source "libusb/$name.c") "/Fo$object"
    if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $name" }
    $compiled += $object
}
$dll = Join-Path $destination 'libusb-1.0.dll'
& $linker /nologo /DLL "/OUT:$dll" "/IMPLIB:$destination/libusb-1.0.lib" "/DEF:$source/libusb/libusb-1.0.def" @compiled kernel32.lib
if ($LASTEXITCODE -ne 0) { throw 'DLL link failed.' }
$fixture = Join-Path $objects 'windows_instance_id.obj'
& $compiler @common (Join-Path $PSScriptRoot 'windows_instance_id.c') "/Fo$fixture"
if ($LASTEXITCODE -ne 0) { throw 'CPU fixture compilation failed.' }
$rest = @($compiled | Where-Object { [IO.Path]::GetFileName($_) -ne 'os_windows_winusb.obj' })
$exe = Join-Path $destination 'windows_instance_id.exe'
& $linker /nologo "/OUT:$exe" $fixture @rest kernel32.lib
if ($LASTEXITCODE -ne 0) { throw 'CPU fixture link failed.' }
& $exe
if ($LASTEXITCODE -ne 0) { throw 'CPU fixture failed.' }
Write-Output "Built native DLL and passed no-device API fixture: $dll"
