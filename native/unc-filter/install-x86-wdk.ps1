# SPDX-License-Identifier: GPL-3.0-only
$ErrorActionPreference='Stop'
$kit=Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Windows Kits/10'
$version='10.0.22621.0'
$root=Join-Path ([IO.Path]::GetTempPath()) ('archivefs-wdk-'+[guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $root | Out-Null
function Install-MicrosoftKit([string]$Uri,[string]$Name){
    $setup=Join-Path $root $Name
    curl.exe --fail --location --retry 4 --output $setup $Uri
    if($LASTEXITCODE){throw "Cannot download $Name"}
    $signature=Get-AuthenticodeSignature -LiteralPath $setup
    if($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'CN=Microsoft Corporation(?:,|$)'){
        throw "Untrusted Microsoft setup signature: $Name"
    }
    $process=Start-Process -FilePath $setup -ArgumentList @('/quiet','/norestart') -Wait -PassThru
    if($process.ExitCode -notin @(0,3010)){throw "Microsoft setup failed: $Name ($($process.ExitCode))"}
}
try{
    # Official Microsoft Windows 11 22H2 SDK/WDK 22621.5193.
    if(!(Test-Path "$kit/Include/$version/shared/ntdef.h")){
        Install-MicrosoftKit 'https://go.microsoft.com/fwlink/?linkid=2311806' 'winsdksetup.exe'
    }
    if(!(Test-Path "$kit/Lib/$version/km/x86/ntoskrnl.lib")){
        Install-MicrosoftKit 'https://go.microsoft.com/fwlink/?linkid=2330411' 'wdksetup.exe'
    }
    foreach($file in @("Include/$version/shared/ntdef.h","Include/$version/km/fltKernel.h","Lib/$version/km/x86/ntoskrnl.lib","Lib/$version/km/x86/fltmgr.lib")){
        if(!(Test-Path (Join-Path $kit $file) -PathType Leaf)){throw "Missing x86 kit component: $file"}
    }
    Write-Host "PASS installed matching SDK and x86 kernel WDK $version"
}finally{
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
