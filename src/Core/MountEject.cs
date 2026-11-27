// SPDX-License-Identifier: GPL-3.0-only
using System.Diagnostics;
using System.IO.Pipes;
using System.Security.Principal;

namespace WinSquashfs.Core;

public static class MountEject
{
    internal static string PipeName(string path)
    {
        string drive = path.TrimEnd('\\', '/');
        if (drive.Length != 2 || !char.IsAsciiLetter(drive[0]) || drive[1] != ':')
            throw new ArgumentException(Localization.T("Use a drive letter such as X:."));
        using var identity = WindowsIdentity.GetCurrent();
        using var process = Process.GetCurrentProcess();
        return $"WinSquashfs.Eject.{identity.User!.Value}.{process.SessionId}.{char.ToUpperInvariant(drive[0])}";
    }

    public static async Task RequestAsync(string drive, CancellationToken cancellationToken = default)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        timeout.CancelAfter(TimeSpan.FromSeconds(20));
        using var pipe = new NamedPipeClientStream(".", PipeName(drive), PipeDirection.InOut,
            PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
        try
        {
            await pipe.ConnectAsync(3000, timeout.Token);
            await pipe.WriteAsync(new byte[] { 1 }, timeout.Token);
            await pipe.FlushAsync(timeout.Token);
            byte[] response = new byte[1];
            int length = await pipe.ReadAsync(response, timeout.Token);
            if (length != 1)
                throw new IOException(Localization.T("Unable to eject the SquashFS volume {0}.", drive));
            await pipe.WriteAsync(new byte[] { 2 }, timeout.Token);
            await pipe.FlushAsync(timeout.Token);
            if (response[0] != 0)
                throw new IOException(Localization.T("Unable to eject the SquashFS volume {0}.", drive));
        }
        catch (TimeoutException ex)
        {
            throw new IOException(Localization.T("No active WinSquashfs mount was found at {0}.", drive), ex);
        }
        catch (OperationCanceledException ex) when (!cancellationToken.IsCancellationRequested)
        {
            throw new IOException(Localization.T("Eject operation timed out for {0}.", drive), ex);
        }
    }
}

internal sealed class EjectListener : IDisposable
{
    private readonly NamedPipeServerStream pipe;
    private readonly CancellationTokenSource stopping = new();
    private readonly Action eject;
    private readonly Action<Exception?> completed;
    private Task? listening;

    internal EjectListener(string drive, Action eject, Action<Exception?> completed)
    {
        this.eject = eject;
        this.completed = completed;
        pipe = new NamedPipeServerStream(MountEject.PipeName(drive), PipeDirection.InOut, 1,
            PipeTransmissionMode.Byte, PipeOptions.Asynchronous | PipeOptions.CurrentUserOnly);
    }

    internal void Start() => listening = ListenAsync(stopping.Token);

    private async Task ListenAsync(CancellationToken token)
    {
        bool requested = false;
        Exception? error = null;
        try
        {
            while (!token.IsCancellationRequested)
            {
                await pipe.WaitForConnectionAsync(token).ConfigureAwait(false);
                try
                {
                    using var readTimeout = CancellationTokenSource.CreateLinkedTokenSource(token);
                    readTimeout.CancelAfter(TimeSpan.FromSeconds(5));
                    byte[] request = new byte[1];
                    if (await pipe.ReadAsync(request, readTimeout.Token).ConfigureAwait(false) != 1 || request[0] != 1)
                        continue;

                    requested = true;
                    try { eject(); }
                    catch (Exception ex) { error = ex; }

                    using var replyTimeout = CancellationTokenSource.CreateLinkedTokenSource(token);
                    replyTimeout.CancelAfter(TimeSpan.FromSeconds(5));
                    await pipe.WriteAsync(new byte[] { error == null ? (byte)0 : (byte)1 }, replyTimeout.Token).ConfigureAwait(false);
                    await pipe.FlushAsync(replyTimeout.Token).ConfigureAwait(false);
                    // The client confirms receipt before the mount process may exit.
                    await pipe.ReadAsync(request, replyTimeout.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException) when (!token.IsCancellationRequested) { }
                catch (IOException) when (!token.IsCancellationRequested) { }
                finally
                {
                    if (pipe.IsConnected)
                    {
                        try { pipe.Disconnect(); }
                        catch (IOException) { }
                    }
                }
                if (requested) return;
            }
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { }
        catch (IOException) { }
        finally
        {
            pipe.Dispose();
            stopping.Dispose();
            if (requested) completed(error);
        }
    }

    public void Dispose()
    {
        try { stopping.Cancel(); } catch (ObjectDisposedException) { }
        listening?.GetAwaiter().GetResult();
    }
}
