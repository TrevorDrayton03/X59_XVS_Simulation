# Run after building Debug. All logs and captures stay under build/verification.
# The only window controlled or captured belongs to the process started here.
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$outputDir = Join-Path $projectRoot 'build/verification'
$tempDir = Join-Path $projectRoot 'build/tmp'
New-Item -ItemType Directory -Force $outputDir, $tempDir | Out-Null
$env:TEMP = $tempDir
$env:TMP = $tempDir

Add-Type -AssemblyName System.Drawing
Add-Type -ReferencedAssemblies System.Drawing -TypeDefinition @'
using System;
using System.Drawing;
using System.Drawing.Imaging;
using System.Runtime.InteropServices;

public static class XvsPhaseAVerifier
{
    private delegate bool EnumWindow(IntPtr window, IntPtr parameter);
    [StructLayout(LayoutKind.Sequential)]
    private struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] private static extern bool EnumWindows(EnumWindow callback, IntPtr parameter);
    [DllImport("user32.dll")] private static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
    [DllImport("user32.dll")] private static extern bool GetClientRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")] private static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("user32.dll")] private static extern bool SetWindowPos(IntPtr window, IntPtr after, int x, int y, int width, int height, uint flags);
    [DllImport("user32.dll")] private static extern bool ShowWindow(IntPtr window, int command);
    [DllImport("user32.dll")] private static extern bool PostMessage(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);

    public static IntPtr FindWindow(uint processId)
    {
        IntPtr result = IntPtr.Zero;
        EnumWindows(delegate(IntPtr window, IntPtr parameter) {
            uint owner;
            GetWindowThreadProcessId(window, out owner);
            Rect rect;
            if (owner == processId && GetClientRect(window, out rect) && rect.Right > 100 && rect.Bottom > 100)
            {
                result = window;
                return false;
            }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static void Resize(IntPtr window, int width, int height)
    {
        if (!SetWindowPos(window, IntPtr.Zero, 0, 0, width, height, 0x16))
            throw new Exception("Could not resize the XVS window.");
    }
    public static void Minimize(IntPtr window) { ShowWindow(window, 6); }
    public static void Restore(IntPtr window) { ShowWindow(window, 9); }
    public static void Close(IntPtr window) { PostMessage(window, 0x10, IntPtr.Zero, IntPtr.Zero); }

    // Linear shader colors are converted by the sRGB swapchain attachment.
    private static int Srgb(double linear)
    {
        return (int)Math.Round(255 * (linear <= 0.0031308 ? 12.92 * linear : 1.055 * Math.Pow(linear, 1.0 / 2.4) - 0.055));
    }

    public static string CaptureAndCheck(IntPtr window, string path, double backgroundBlue)
    {
        Rect rect;
        if (!GetClientRect(window, out rect)) throw new Exception("Could not read XVS client size.");
        using (var bitmap = new Bitmap(rect.Right, rect.Bottom, PixelFormat.Format32bppArgb))
        {
            using (var graphics = Graphics.FromImage(bitmap))
            {
                IntPtr dc = graphics.GetHdc();
                bool captured;
                try { captured = PrintWindow(window, dc, 3); }
                finally { graphics.ReleaseHdc(dc); }
                if (!captured) throw new Exception("XVS window capture failed.");
            }
            bitmap.Save(path, ImageFormat.Png);
            // Expected Phase A arrangement: external vision, standby, navigation, primary flight.
            double[,] regions = {
                { .23, .05, .65, .43, .07, .35, .55 },
                { .04, .28, .16, .18, .85, .58, .19 },
                { .08, .56, .40, .36, .09, .50, .35 },
                { .52, .56, .40, .36, .53, .31, .65 }
            };
            int checkedPixels = 0;
            int[] regionSamples = new int[5];
            for (int y = 3; y < bitmap.Height; y += 7)
            for (int x = 3; x < bitmap.Width; x += 7)
            {
                double r = .02, g = .05, b = backgroundBlue;
                bool nearEdge = false;
                int regionIndex = 4;
                for (int i = 0; i < 4; ++i)
                {
                    double left = regions[i,0] * bitmap.Width;
                    double top = regions[i,1] * bitmap.Height;
                    double right = (regions[i,0] + regions[i,2]) * bitmap.Width;
                    double bottom = (regions[i,1] + regions[i,3]) * bitmap.Height;
                    // Permit rounding of the normalized edges to framebuffer pixels.
                    if (Math.Abs(x-left) <= 2 || Math.Abs(x-right) <= 2 || Math.Abs(y-top) <= 2 || Math.Abs(y-bottom) <= 2)
                        nearEdge = true;
                    if (x > left && x < right && y > top && y < bottom)
                    {
                        r = regions[i,4]; g = regions[i,5]; b = regions[i,6]; regionIndex = i;
                    }
                }
                if (nearEdge) continue;
                Color actual = bitmap.GetPixel(x, y);
                if (Math.Abs(actual.R-Srgb(r)) > 4 || Math.Abs(actual.G-Srgb(g)) > 4 || Math.Abs(actual.B-Srgb(b)) > 4)
                    throw new Exception(String.Format("Pixel ({0},{1}) in {2}: expected RGB {3},{4},{5}, got {6},{7},{8}.", x, y, path, Srgb(r), Srgb(g), Srgb(b), actual.R, actual.G, actual.B));
                ++checkedPixels;
                ++regionSamples[regionIndex];
            }
            foreach (int count in regionSamples)
                if (count == 0) throw new Exception("A display or the background had no verified pixels.");
            return String.Format("PASS {0}: {1}x{2}, {3} sampled pixels match all four regions and background.", System.IO.Path.GetFileName(path), bitmap.Width, bitmap.Height, checkedPixels);
        }
    }
}
'@

$stdout = Join-Path $outputDir 'application.stdout.log'
$stderr = Join-Path $outputDir 'application.stderr.log'
$report = [System.Collections.Generic.List[string]]::new()
$process = $null
$window = [IntPtr]::Zero
$sender = $null
try {
    $process = Start-Process -FilePath (Join-Path $projectRoot 'build/Debug/VulkanTutorial.exe') -WorkingDirectory $projectRoot -WindowStyle Hidden -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    # Retain the handle so Windows PowerShell can retrieve ExitCode after exit.
    $null = $process.Handle
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    while ($window -eq [IntPtr]::Zero -and [DateTime]::UtcNow -lt $deadline) {
        if ($process.HasExited) { throw "XVS exited during startup. See $stderr and $stdout." }
        $window = [XvsPhaseAVerifier]::FindWindow($process.Id)
        Start-Sleep -Milliseconds 100
    }
    if ($window -eq [IntPtr]::Zero) { throw 'XVS did not create its window within 20 seconds.' }
    Start-Sleep -Milliseconds 1200
    $report.Add([XvsPhaseAVerifier]::CaptureAndCheck($window, (Join-Path $outputDir 'initial.png'), .20))

    foreach ($size in @(@(971, 743), @(1517, 947), @(656, 521))) {
        [XvsPhaseAVerifier]::Resize($window, $size[0], $size[1])
        Start-Sleep -Milliseconds 500
        $report.Add([XvsPhaseAVerifier]::CaptureAndCheck($window, (Join-Path $outputDir "resize-$($size[0]).png"), .20))
    }

    [XvsPhaseAVerifier]::Minimize($window)
    Start-Sleep -Milliseconds 400
    if ($process.HasExited) { throw 'XVS exited while minimized.' }
    [XvsPhaseAVerifier]::Restore($window)
    Start-Sleep -Milliseconds 600
    $report.Add([XvsPhaseAVerifier]::CaptureAndCheck($window, (Join-Path $outputDir 'restored.png'), .20))

    $sender = [System.Net.Sockets.UdpClient]::new()
    foreach ($speed in @(80, 240)) {
        $packet = [System.Text.Encoding]::ASCII.GetBytes("37.5,-122.2,10000,0.1,0.2,1.5,$speed,-4.0,0.02")
        [void]$sender.Send($packet, $packet.Length, '127.0.0.1', 5500)
        Start-Sleep -Milliseconds 400
        $report.Add([XvsPhaseAVerifier]::CaptureAndCheck($window, (Join-Path $outputDir "telemetry-$speed.png"), (.20 + $speed / 400.0)))
    }

    # Also exercise shutdown while the window is minimized.
    [XvsPhaseAVerifier]::Minimize($window)
    Start-Sleep -Milliseconds 300
    [XvsPhaseAVerifier]::Close($window)
    if (-not $process.WaitForExit(10000)) { throw 'XVS did not shut down within 10 seconds.' }
    if ($process.ExitCode -ne 0) { throw "XVS exited with code $($process.ExitCode)." }
    $messages = (Get-Content $stdout -Raw) + (Get-Content $stderr -Raw)
    if ($messages -notmatch 'Vulkan validation enabled') { throw 'The Debug validation callback was not enabled.' }
    if ($messages -match 'Vulkan validation error:|VUID-|Application error:') { throw "Application or Vulkan validation errors were logged. See $outputDir." }
    if ($messages -notmatch 'Listening for UE5 telemetry on UDP port 5500') { throw 'UDP startup was not confirmed.' }
    $report.Add('PASS shutdown while minimized (exit 0); Vulkan validation enabled, no errors or VUID diagnostics logged.')
    $warnings = @(Get-Content $stderr | Where-Object { $_ -match 'Vulkan validation warning:' })
    if ($warnings.Count -gt 0) {
        $report.Add("NOTE $($warnings.Count) Vulkan warnings retained in application.stderr.log:")
        foreach ($warning in $warnings) { $report.Add($warning) }
    }
    $report | Set-Content (Join-Path $outputDir 'results.txt')
    $report
}
catch {
    $report.Add("FAIL: $_")
    $report | Set-Content (Join-Path $outputDir 'results.txt')
    throw
}
finally {
    if ($sender) { $sender.Dispose() }
    if ($process -and -not $process.HasExited) {
        if ($window -ne [IntPtr]::Zero) { [XvsPhaseAVerifier]::Close($window) }
        if (-not $process.WaitForExit(3000)) { $process.Kill(); $process.WaitForExit() }
    }
    if ($process) { $process.Dispose() }
}
