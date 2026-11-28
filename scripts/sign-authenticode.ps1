# SPDX-License-Identifier: GPL-3.0-only
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$Path,[switch]$VerifyOnly)
$ErrorActionPreference='Stop'
$thumbprint=($env:ARCHIVEFS_SIGNING_CERT_SHA1 -replace '\s','').ToUpperInvariant()
if($thumbprint -notmatch '^[0-9A-F]{40}$'){throw 'Set ARCHIVEFS_SIGNING_CERT_SHA1 to the installed signing certificate thumbprint'}
$file=(Resolve-Path -LiteralPath $Path).Path
if(!(Test-Path -LiteralPath $file -PathType Leaf)){throw 'Expected a file'}
$tool=Get-Command signtool.exe -ErrorAction Stop
function Test-SignedFile{
    $signature=Get-AuthenticodeSignature -LiteralPath $file
    if($signature.Status -ne 'Valid' -or $null -eq $signature.SignerCertificate -or
       $signature.SignerCertificate.Thumbprint -ne $thumbprint -or $null -eq $signature.TimeStamperCertificate){
        throw "Invalid, untimestamped or unexpected publisher signature: $file"
    }
    & $tool.Source verify /pa /all /v $file
    if($LASTEXITCODE -ne 0){throw "Authenticode verification failed: $file"}
}
if(!$VerifyOnly){
    $cert=Get-Item -LiteralPath "Cert:\CurrentUser\My\$thumbprint" -ErrorAction Stop
    if(!$cert.HasPrivateKey -or $cert.NotBefore -gt (Get-Date) -or $cert.NotAfter -lt (Get-Date)){
        throw 'The signing certificate must be current and have an accessible private key'
    }
    if('1.3.6.1.5.5.7.3.3' -notin @($cert.EnhancedKeyUsageList | ForEach-Object {$_.ObjectId.Value})){
        throw 'The certificate does not have the code signing extended key usage'
    }
    $signature=Get-AuthenticodeSignature -LiteralPath $file
    if($signature.Status -eq 'Valid' -and $signature.SignerCertificate.Thumbprint -eq $thumbprint -and $null -ne $signature.TimeStamperCertificate){
        Test-SignedFile
        return
    }
    if($signature.Status -ne 'NotSigned'){throw "Refusing to replace an existing or invalid signature: $file"}
    & $tool.Source sign /sha1 $thumbprint /fd SHA256 /tr http://timestamp.digicert.com /td SHA256 $file
    if($LASTEXITCODE -ne 0){throw "Authenticode signing failed: $file"}
}
Test-SignedFile
