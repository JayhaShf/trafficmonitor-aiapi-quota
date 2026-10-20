# Local HTTP test server: replies to /v1/usage after a fixed delay.
# Used to verify the plugin's parallel site queries:
#   4 sites x 1500 ms = ~6 s when serial, ~1.6 s when parallel.
#
# The server is written in compiled C# on purpose: PowerShell script blocks
# cannot run on ThreadPool threads (no runspace), and a serial server would
# mask the very parallelism this benchmark measures.
#
# Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less files as ANSI.
param(
    [int] $Port = 18080,
    [int] $DelayMs = 1500
)

Add-Type -TypeDefinition @'
using System;
using System.Net;
using System.Net.Sockets;
using System.Text;
using System.Threading;

public static class BenchHttpServer
{
    private static TcpListener listener;
    private static int delayMs;

    public static void Start(int port, int delay)
    {
        delayMs = delay;
        listener = new TcpListener(IPAddress.Loopback, port);
        listener.Start();
        var accept = new Thread(AcceptLoop) { IsBackground = true };
        accept.Start();
    }

    private static void AcceptLoop()
    {
        while (true)
        {
            TcpClient client;
            try { client = listener.AcceptTcpClient(); }
            catch { return; }
            var worker = new Thread(() => Handle(client)) { IsBackground = true };
            worker.Start();
        }
    }

    private static void Handle(TcpClient client)
    {
        try
        {
            using (client)
            using (var stream = client.GetStream())
            {
                var buffer = new byte[8192];
                var total = 0;
                while (total < buffer.Length)
                {
                    var read = stream.Read(buffer, total, buffer.Length - total);
                    if (read <= 0) break;
                    total += read;
                    var head = Encoding.ASCII.GetString(buffer, 0, total);
                    if (head.Contains("\r\n\r\n")) break;
                }

                Thread.Sleep(delayMs);

                const string body = "{\"remaining\":123.45,\"unit\":\"USD\",\"planName\":\"bench\"}";
                var bodyBytes = Encoding.UTF8.GetBytes(body);
                var header = "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                    + bodyBytes.Length + "\r\nConnection: close\r\n\r\n";
                var headerBytes = Encoding.ASCII.GetBytes(header);
                stream.Write(headerBytes, 0, headerBytes.Length);
                stream.Write(bodyBytes, 0, bodyBytes.Length);
                stream.Flush();
            }
        }
        catch
        {
            // client hung up early
        }
    }
}
'@

[BenchHttpServer]::Start($Port, $DelayMs)
Write-Host "listening on http://127.0.0.1:$Port/ delay=${DelayMs}ms"

# Keep the process alive while the background accept loop serves requests.
while ($true) { Start-Sleep -Seconds 1 }
