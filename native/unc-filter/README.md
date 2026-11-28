# UNC alias minifilter

This optional component redirects an initial, absolute MUP create for
`\\sfs.HOST.localhost\SHARE\archive.tar.gz\member` to
`\\sfs$\UNC\HOST\SHARE\archive.tar.gz\member`. The existing WinFsp UNC
agent handles the request and its file operations. An installation package
can explicitly enable the shorter `\\sfs.HOST\SHARE\...` spelling as well.
The unqualified spelling reserves the `sfs.` host prefix on that computer.
Qualified aliases are the default to avoid affecting ordinary DNS hosts.

The filter attaches only to the MUP network volume. It handles create and
network-query-open operations, never opens a backing file under an elevated
security context, and does not change MUP provider priority, DNS, SMB
credentials, or the WinFsp driver. Existing `sfs$` and `sfs.localhost` paths
pass through. The UNC agent must already be running. The alias target is
constructed from validated UTF-16 components; traversal, alternate data
streams, recursive alias hosts, malformed UTF-16 and ambiguous host names
are rejected. Host names use ASCII DNS/NetBIOS labels or Windows IPv6-literal
names. Relative opens on an existing redirected file handle pass through.

## Build and validate

The independent **Native UNC alias filter** workflow builds unsigned x64,
ARM64 and x86 binaries with the Microsoft WDK and runs the same portable name
parser under ASan/UBSan. To build locally, use a Visual Studio C++ command
prompt for the target architecture and run:

    pwsh -File native/unc-filter/build.ps1 -Architecture x64

The script can accept `-KitRoot` pointing to extracted WDK and SDK NuGet
packages. For x86 use a Windows 11 22H2 WDK (22621) with its x86 libraries
and Visual Studio 2022 x86 tools.

## Driver packaging

Before creating an installable package, obtain a Microsoft-allocated altitude
in the FSFilter Activity Monitor load-order group. Supply that value using
`-Altitude` to generate the INF alongside the binary. Add
`-IncludeUnqualified` only when the shorter aliases are explicitly desired.
Generate a catalog with the WDK for the intended operating systems and
submit the package through Microsoft's driver signing process. The default
application installer does not include or load this optional component.

Use only a correctly signed driver package on a dedicated Windows test
computer. Validate that the filter is attached to MUP and test absolute UNC
opens, network query opens, file attributes, directory enumeration, random
reads, unrelated SMB access, source access permissions and clean unload
before distributing it. The signed package requires a live Windows kernel
test; a successful compiler run alone does not establish runtime support.
See Microsoft's documentation for [minifilter reparsing](https://learn.microsoft.com/en-us/samples/microsoft/windows-driver-samples/simrep-file-system-minifilter-driver/),
[MUP](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/support-for-unc-naming-and-mup),
and [filter altitude allocation](https://learn.microsoft.com/en-us/windows-hardware/drivers/ifs/allocated-altitudes).

Run the **Signed UNC filter live integration** workflow on a dedicated interactive Windows runner labeled `archivefs-kernel`, with the signed filter for the checked-out source and WinFsp already installed. It creates a temporary SMB share restricted to the test account, checks first access to a fresh host/share without alias registration, validates directory enumeration, concurrent reads and read-only access, and checks that generic UNC and ordinary SMB still work after unload and reload. It restores the previous UNC settings and deletes the temporary share. The user-mode agent reserves the unqualified host prefix when that package policy is enabled, preventing recursive access to the archive namespace from inside the generic network projection.

The filter also reparses the exact server roots `\\$sfs\`, `\\sfs$\` and `\\sfs.localhost\` to the unified `\\$sfs\root` share. The root share can be browsed without installing the optional filter.
