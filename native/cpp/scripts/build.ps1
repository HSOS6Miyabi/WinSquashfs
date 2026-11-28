# SPDX-License-Identifier: GPL-3.0-only
param([ValidateSet('All','ConfigureDependencies','Dependencies','Application','CoreDistribution','Gui','Tests')][string]$Stage='All')
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path "$PSScriptRoot/../../..").Path
Set-Location $root
function Invoke-Checked { param([string]$Command, [string[]]$Arguments) & $Command @Arguments; if ($LASTEXITCODE) { throw "$Command failed with exit code $LASTEXITCODE" } }
$vcpkg = $env:VCPKG_INSTALLATION_ROOT
if (!$vcpkg) { $vcpkg = $env:VCPKG_ROOT }
if (!$vcpkg) { throw 'VCPKG_INSTALLATION_ROOT or VCPKG_ROOT is required' }
$toolchain = Join-Path $vcpkg 'scripts/buildsystems/vcpkg.cmake'
$deps = Join-Path $root '.deps'
$dwarfs = Join-Path $deps 'dwarfs'
$prefix = Join-Path $deps 'dwarfs-install'
$ports = Join-Path $deps 'vcpkg-installed'
$build = Join-Path $deps 'archivefs-build'
$test = Join-Path $deps 'archivefs-test'
$dist = Join-Path $root 'artifacts/dist'
New-Item -ItemType Directory -Force $deps,$dist | Out-Null
$flags = @('-G','Ninja','-DCMAKE_BUILD_TYPE=Release',"-DCMAKE_TOOLCHAIN_FILE=$toolchain",'-DVCPKG_TARGET_TRIPLET=x64-windows-static-md',"-DVCPKG_INSTALLED_DIR=$ports",'-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL')
if ($Stage -in @('All','ConfigureDependencies','Dependencies')) {
    if (!(Test-Path "$prefix/bin/mkdwarfs.exe") -or !(Test-Path "$prefix/lib/cmake/dwarfs/dwarfs-config.cmake")) {
        if (!(Test-Path "$dwarfs/CMakeLists.txt")) { Invoke-Checked git @('clone','--branch','v0.15.10','--depth','1','--recursive','https://github.com/mhx/dwarfs.git',$dwarfs) }
        # The C++ reader shares the application's CRT and allocator. Upstream's
        # Windows defaults use the static CRT; set the two explicit overrides
        # to match the x64-windows-static-md dependency triplet.
        foreach ($relative in @('cmake/compile.cmake','ricepp/CMakeLists.txt')) {
            $file=Join-Path $dwarfs $relative
            $source=[IO.File]::ReadAllText($file)
            $old='set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")'
            $new='set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")'
            if ($source.Contains($old)) { $source=$source.Replace($old,$new) }
            elseif (!$source.Contains($new)) { throw "Unexpected DwarFS CRT configuration: $relative" }
            $source=$source.Replace('set(MSVC_USE_STATIC_RUNTIME ON CACHE BOOL "static build")','set(MSVC_USE_STATIC_RUNTIME OFF CACHE BOOL "dynamic CRT" FORCE)')
            [IO.File]::WriteAllText($file,$source,[Text.UTF8Encoding]::new($false))
        }
        Invoke-Checked cmake (@('-S',$dwarfs,'-B',"$deps/dwarfs-build","-DCMAKE_INSTALL_PREFIX=$prefix",'-DWITH_TOOLS=ON','-DWITH_FUSE_DRIVER=OFF','-DWITH_TESTS=OFF','-DWITH_BENCHMARKS=OFF','-DWITH_DESKTOP_INTEGRATION=OFF','-DWITH_MAN_OPTION=OFF','-DWITH_PXATTR=OFF','-DENABLE_STACKTRACE=OFF','-DBUILD_SHARED_LIBS=OFF')+$flags)
        if ($Stage -eq 'ConfigureDependencies') { return }
        Invoke-Checked cmake @('--build',"$deps/dwarfs-build",'--target','install','--parallel','4')
        New-Item -ItemType Directory -Force "$prefix/licenses" | Out-Null
        Copy-Item "$dwarfs/LICENSE" "$prefix/licenses/dwarfs.txt" -Force
    }
    if ($Stage -in @('ConfigureDependencies','Dependencies')) { return }
}
if ($Stage -eq 'Gui') {
    Invoke-Checked msbuild @('native/cpp/gui/WinArchiveFSPro.vcxproj','/restore','/m','/p:Configuration=Release','/p:Platform=x64',"/p:ArchiveFSCoreLib=$build/archivefs_core.lib","/p:OutDir=$dist/",'/p:WindowsAppSDKSelfContained=true')
    if (!(Test-Path "$dist/WinArchiveFSPro.exe")) { throw 'Missing WinUI application' }
    return
}
$env:PKG_CONFIG_PATH = "$ports/x64-windows-static-md/lib/pkgconfig"
$env:PATH = "$ports/x64-windows-static-md/tools/pkgconf;$env:PATH"
# The test build uses deterministic mock decoder libraries. The application
# build links the official DwarFS reader and is exercised by real images.
Invoke-Checked cmake (@('-S','native/cpp','-B',$test,'-DARCHIVEFS_WITH_DWARFS=OFF','-DBUILD_TESTING=ON','-DVCPKG_MANIFEST_MODE=OFF')+$flags)
Invoke-Checked cmake @('--build',$test,'--parallel','2')
Invoke-Checked ctest @('--test-dir',$test,'--output-on-failure')
if ($Stage -eq 'Tests') { return }
Invoke-Checked cmake (@('-S','native/cpp','-B',$build,'-DARCHIVEFS_WITH_DWARFS=ON','-DBUILD_TESTING=OFF','-DVCPKG_MANIFEST_MODE=OFF',"-DCMAKE_PREFIX_PATH=$prefix")+$flags)
Invoke-Checked cmake @('--build',$build,'--parallel','2')
Copy-Item "$build/archivefs_core.dll","$build/WinArchiveFSPro.Unc.exe" $dist -Force
Copy-Item "$build/archivefs-read.exe" "$dist/archivefs.exe" -Force
Copy-Item "artifacts/native/*" $dist -Recurse -Force
Copy-Item "$prefix/bin/mkdwarfs.exe" "$dist/tools/mkdwarfs.exe" -Force
if (Test-Path "$prefix/bin/dwarfsck.exe") { Copy-Item "$prefix/bin/dwarfsck.exe" "$dist/tools/dwarfsck.exe" -Force }
New-Item -ItemType Directory -Force "$dist/licenses","$dist/Assets" | Out-Null
Copy-Item "$prefix/licenses/dwarfs.txt" "$dist/licenses/dwarfs.txt" -Force
Copy-Item 'src/App/Assets/app-icon.ico','src/App/Assets/file-icon.ico' "$dist/Assets" -Force
Copy-Item LICENSE,README.md,README.zh-CN.md,THIRD-PARTY.md $dist -Force
$redist=$env:VCToolsRedistDir
if (!$redist) { throw 'VCToolsRedistDir is required for a portable Windows build' }
$crt=Get-ChildItem (Join-Path $redist 'x64') -Directory | Where-Object Name -Match '^Microsoft\.VC[0-9]+\.CRT$' | Select-Object -First 1
if (!$crt) { throw 'Visual C++ redistributable directory is missing' }
Copy-Item (Join-Path $crt.FullName '*.dll') $dist -Force
$licenseRoot=Join-Path $dist 'licenses/vcpkg'
New-Item -ItemType Directory -Force $licenseRoot | Out-Null
Get-ChildItem "$ports/x64-windows-static-md/share" -Directory | ForEach-Object {
    $copyright=Join-Path $_.FullName 'copyright'
    if(Test-Path $copyright){Copy-Item $copyright (Join-Path $licenseRoot ($_.Name+'.txt')) -Force}
}
New-Item -ItemType Directory -Force "$dist/prerequisites" | Out-Null
Copy-Item artifacts/winfsp.msi "$dist/prerequisites/WinFsp.msi" -Force
$version=([xml](Get-Content Directory.Build.props -Raw)).Project.PropertyGroup.Version
Move-Item "$dist/winsqfs-shell.dll" "$dist/winsqfs-shell-$version.dll" -Force
if ($Stage -eq 'CoreDistribution') { return }
Invoke-Checked msbuild @('native/cpp/gui/WinArchiveFSPro.vcxproj','/restore','/m','/p:Configuration=Release','/p:Platform=x64',"/p:ArchiveFSCoreLib=$build/archivefs_core.lib","/p:OutDir=$dist/",'/p:WindowsAppSDKSelfContained=true')
if (!(Test-Path "$dist/WinArchiveFSPro.exe")) { throw 'Missing WinUI application' }
