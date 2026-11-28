# Capture the real interactive WinUI window on Windows CI.
Add-Type -AssemblyName System.Drawing,System.Windows.Forms
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class WindowCapture {
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out Rect r);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int w, int height, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
    [DllImport("user32.dll")] public static extern IntPtr GetWindowDpiAwarenessContext(IntPtr h);
    [DllImport("user32.dll")] public static extern bool AreDpiAwarenessContextsEqual(IntPtr first, IntPtr second);
    [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
}
'@
[WindowCapture]::SetProcessDPIAware() | Out-Null
$script:ScreenshotDir=Join-Path (Split-Path -Parent $PSScriptRoot) 'artifacts\screenshots'
New-Item -ItemType Directory -Force $script:ScreenshotDir | Out-Null

function Assert-WindowDpiAwareness($process) {
    $process.Refresh()
    $handle=$process.MainWindowHandle
    if($handle -eq [IntPtr]::Zero){throw 'Window handle is missing for DPI validation'}
    $context=[WindowCapture]::GetWindowDpiAwarenessContext($handle)
    if(![WindowCapture]::AreDpiAwarenessContextsEqual($context,[IntPtr](-4))){throw 'Window does not use PerMonitorV2 DPI awareness'}
    $dpi=[WindowCapture]::GetDpiForWindow($handle)
    if(!$dpi){throw 'Cannot read the window DPI'}
    Write-Host "PASS PerMonitorV2 window DPI=$dpi; displays=$([System.Windows.Forms.Screen]::AllScreens.Count)"
}

function Save-WindowScreenshot($process,[string]$name,[int]$width=1200,[int]$height=780,$screen=[System.Windows.Forms.Screen]::PrimaryScreen) {
    if($script:ScreenshotPrefix){$name=$script:ScreenshotPrefix+$name}
    $process.Refresh()
    $h=$process.MainWindowHandle
    if($h -eq [IntPtr]::Zero){throw "Window handle is missing: $name"}
    $area=$screen.WorkingArea
    $width=[Math]::Min($width,$area.Width)
    $height=[Math]::Min($height,$area.Height)
    [WindowCapture]::SetWindowPos($h,[IntPtr](-1),$area.Left,$area.Top,$width,$height,0x0040) | Out-Null
    [WindowCapture]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 600
    $r=New-Object WindowCapture+Rect
    if(![WindowCapture]::GetWindowRect($h,[ref]$r)){throw "Cannot get window bounds: $name"}
    $w=$r.Right-$r.Left
    $hgt=$r.Bottom-$r.Top
    $bitmap=New-Object System.Drawing.Bitmap($w,$hgt)
    $g=[System.Drawing.Graphics]::FromImage($bitmap)
    try {
        $g.CopyFromScreen($r.Left,$r.Top,0,0,$bitmap.Size)
        $bitmap.Save((Join-Path $script:ScreenshotDir "$name.png"),[System.Drawing.Imaging.ImageFormat]::Png)
        # JPEG previews in the job log support remote visual inspection without artifact extraction.
        $memory=New-Object IO.MemoryStream
        try {
            $encoder=[System.Drawing.Imaging.ImageCodecInfo]::GetImageEncoders() | Where-Object {$_.MimeType -eq 'image/jpeg'} | Select-Object -First 1
            $parameters=New-Object System.Drawing.Imaging.EncoderParameters(1)
            $parameters.Param[0]=New-Object System.Drawing.Imaging.EncoderParameter([System.Drawing.Imaging.Encoder]::Quality,[long]75)
            $bitmap.Save($memory,$encoder,$parameters)
            $parameters.Dispose()
            $data=[Convert]::ToBase64String($memory.ToArray())
            Write-Host "SCREENSHOT_BEGIN $name $w $hgt"
            for($i=0;$i -lt $data.Length;$i+=3500){Write-Host ("SCREENSHOT_DATA " + $data.Substring($i,[Math]::Min(3500,$data.Length-$i)))}
            Write-Host "SCREENSHOT_END $name"
        } finally {$memory.Dispose()}
    } finally {
        $g.Dispose();$bitmap.Dispose()
        [WindowCapture]::SetWindowPos($process.MainWindowHandle,[IntPtr](-2),0,0,0,0,0x0013) | Out-Null
    }
}
