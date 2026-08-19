using System.Diagnostics;
using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace DiscordRefreshProxy;

internal static class Program
{
    [STAThread]
    private static void Main()
    {
        ApplicationConfiguration.Initialize();
        Application.Run(new MainForm());
    }
}

internal sealed class MainForm : Form
{
    private readonly Button unlockButton = new();
    private readonly Label statusLabel = new();
    private readonly TextBox logBox = new();
    private readonly ProgressBar progress = new();

    public MainForm()
    {
        Text = "Discord Refresh Proxy";
        ClientSize = new Size(650, 440);
        MinimumSize = new Size(666, 479);
        StartPosition = FormStartPosition.CenterScreen;
        Font = new Font("Segoe UI", 9F);
        BackColor = Color.FromArgb(245, 246, 250);

        var title = new Label {
            Text = "Discord Refresh Proxy", AutoSize = true,
            Font = new Font("Segoe UI Semibold", 18F), Location = new Point(20, 18)
        };
        var subtitle = new Label {
            Text = "Proxy temporário apenas durante a recarga do Discord.", AutoSize = true,
            ForeColor = Color.DimGray, Location = new Point(23, 56)
        };
        statusLabel.SetBounds(480, 24, 145, 28);
        statusLabel.TextAlign = ContentAlignment.MiddleCenter;
        statusLabel.Text = "PRONTO";
        statusLabel.BackColor = Color.FromArgb(225, 230, 250);
        statusLabel.ForeColor = Color.FromArgb(65, 80, 170);

        unlockButton.Text = "DESBLOQUEAR DISCORD AGORA";
        unlockButton.SetBounds(20, 88, 605, 58);
        unlockButton.FlatStyle = FlatStyle.Flat;
        unlockButton.FlatAppearance.BorderSize = 0;
        unlockButton.BackColor = Color.FromArgb(35, 145, 85);
        unlockButton.ForeColor = Color.White;
        unlockButton.Font = new Font("Segoe UI Semibold", 12F);
        unlockButton.Cursor = Cursors.Hand;
        unlockButton.Click += async (_, _) => await RunUnlockAsync();

        progress.SetBounds(20, 158, 605, 8);
        progress.Style = ProgressBarStyle.Marquee;
        progress.MarqueeAnimationSpeed = 0;

        logBox.SetBounds(20, 180, 605, 235);
        logBox.Anchor = AnchorStyles.Top | AnchorStyles.Bottom | AnchorStyles.Left | AnchorStyles.Right;
        logBox.Multiline = true;
        logBox.ReadOnly = true;
        logBox.ScrollBars = ScrollBars.Vertical;
        logBox.BackColor = Color.White;
        logBox.Font = new Font("Consolas", 9F);
        logBox.Text = "Abra o Discord e clique no botão acima.\r\nNenhuma janela de terminal será aberta.\r\n";

        Controls.AddRange([title, subtitle, statusLabel, unlockButton, progress, logBox]);
    }

    private async Task RunUnlockAsync()
    {
        unlockButton.Enabled = false;
        progress.MarqueeAnimationSpeed = 25;
        SetStatus("TRABALHANDO", Color.FromArgb(165, 105, 20), Color.FromArgb(255, 240, 210));
        Log("");
        Process? tunnel = null;
        string? configPath = null;
        try
        {
            var discord = FindDiscordWindow() ?? throw new InvalidOperationException(
                "Abra o Discord e aguarde a janela principal aparecer.");
            Log("Discord encontrado.");

            var singBox = await SingBoxInstaller.EnsureInstalledAsync(Log);
            var proxy = await ProxyFinder.FindAsync(Log);
            Log($"Proxy aprovado: {proxy.Host}:{proxy.Port} — {proxy.Country}");

            configPath = Tunnel.CreateConfig(proxy);
            await Tunnel.ValidateAsync(singBox, configPath);
            tunnel = Tunnel.Start(singBox, configPath);
            await Task.Delay(2000);
            if (tunnel.HasExited)
                throw new InvalidOperationException("O túnel temporário não iniciou. Consulte o log do aplicativo.");

            Log("Túnel temporário ativo. Recarregando o Discord...");
            discord.Refresh();
            if (discord.MainWindowHandle == IntPtr.Zero)
                throw new InvalidOperationException("A janela do Discord desapareceu antes da recarga.");
            if (!NativeInput.SendCtrlR(discord.MainWindowHandle))
                throw new InvalidOperationException("O Windows não permitiu colocar o Discord em primeiro plano.");

            Log("Ctrl+R enviado. Mantendo o proxy por 15 segundos...");
            for (var remaining = 15; remaining > 0; remaining--)
            {
                statusLabel.Text = $"AGUARDE {remaining}s";
                await Task.Delay(1000);
            }
            Log($"Concluído usando saída em {proxy.Country}. Proxy desconectado.");
            SetStatus("CONCLUÍDO", Color.FromArgb(28, 125, 70), Color.FromArgb(220, 244, 230));
        }
        catch (Exception ex)
        {
            Log("ERRO: " + ex.Message);
            SetStatus("ERRO", Color.FromArgb(170, 50, 50), Color.FromArgb(255, 225, 225));
        }
        finally
        {
            if (tunnel is { HasExited: false })
            {
                try { tunnel.Kill(entireProcessTree: true); await tunnel.WaitForExitAsync(); } catch { }
            }
            tunnel?.Dispose();
            if (configPath is not null) try { File.Delete(configPath); } catch { }
            progress.MarqueeAnimationSpeed = 0;
            unlockButton.Enabled = true;
        }
    }

    private static Process? FindDiscordWindow() =>
        new[] { "Discord", "DiscordCanary", "DiscordPTB", "DiscordDevelopment" }
            .SelectMany(name => Process.GetProcessesByName(name))
            .FirstOrDefault(p => p.MainWindowHandle != IntPtr.Zero);

    private void Log(string text)
    {
        if (InvokeRequired) { BeginInvoke(new Action(() => Log(text))); return; }
        logBox.AppendText(text + Environment.NewLine);
        logBox.SelectionStart = logBox.TextLength;
        logBox.ScrollToCaret();
    }

    private void SetStatus(string text, Color foreground, Color background)
    {
        statusLabel.Text = text;
        statusLabel.ForeColor = foreground;
        statusLabel.BackColor = background;
    }
}

internal sealed record ProxyInfo(string Host, int Port, string Country);

internal static class ProxyFinder
{
    private static readonly (string Code, string Name)[] Countries = [
        ("US", "Estados Unidos"), ("CA", "Canadá"), ("NL", "Países Baixos"),
        ("DE", "Alemanha"), ("FR", "França"), ("GB", "Reino Unido"),
        ("JP", "Japão"), ("SG", "Singapura")
    ];

    public static async Task<ProxyInfo> FindAsync(Action<string> log)
    {
        using var listClient = CreateClient(TimeSpan.FromSeconds(20));
        foreach (var country in Countries.OrderBy(_ => Random.Shared.Next()))
        {
            log($"Buscando proxies em {country.Name}...");
            var uri = "https://api.proxyscrape.com/v4/free-proxy-list/get" +
                      $"?request=getproxies&protocol=http&country={country.Code}" +
                      "&timeout=5000&ssl=yes&anonymity=elite,anonymous&limit=80";
            string body;
            try { body = await listClient.GetStringAsync(uri); }
            catch (Exception ex) { log("  Lista indisponível: " + ex.Message); continue; }

            var candidates = body.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries)
                .Select(x => x.Trim())
                .Where(x => Regex.IsMatch(x, @"^(?:\d{1,3}\.){3}\d{1,3}:\d{2,5}$"))
                .OrderBy(_ => Random.Shared.Next()).Take(12);
            foreach (var candidate in candidates)
            {
                log("  Testando " + candidate + "...");
                var pieces = candidate.Split(':');
                if (!int.TryParse(pieces[1], out var port)) continue;
                if (await TestAsync(pieces[0], port)) return new ProxyInfo(pieces[0], port, country.Name);
            }
        }
        throw new InvalidOperationException("Nenhum proxy público funcional foi encontrado. Tente novamente em alguns minutos.");
    }

    private static async Task<bool> TestAsync(string host, int port)
    {
        try
        {
            using var handler = new HttpClientHandler { Proxy = new WebProxy(host, port), UseProxy = true };
            using var client = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(7) };
            client.DefaultRequestHeaders.UserAgent.ParseAdd("DiscordRefreshProxy/1.0");
            using var response = await client.GetAsync("https://discord.com/api/v10/gateway",
                HttpCompletionOption.ResponseHeadersRead);
            return (int)response.StatusCode is >= 200 and < 400;
        }
        catch { return false; }
    }

    private static HttpClient CreateClient(TimeSpan timeout)
    {
        var client = new HttpClient { Timeout = timeout };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("DiscordRefreshProxy/1.0");
        return client;
    }
}

internal static class SingBoxInstaller
{
    private static readonly string Root = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DiscordRefreshProxy");

    public static async Task<string> EnsureInstalledAsync(Action<string> log)
    {
        var currentPointer = Path.Combine(Root, "current.txt");
        if (File.Exists(currentPointer))
        {
            var existing = Path.Combine(Root, File.ReadAllText(currentPointer).Trim(), "sing-box.exe");
            if (File.Exists(existing)) { log("Componente de rede já instalado."); return existing; }
        }

        log("Consultando o último release estável do sing-box...");
        using var client = new HttpClient { Timeout = TimeSpan.FromMinutes(3) };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("DiscordRefreshProxy/1.0");
        client.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        using var response = await client.GetAsync("https://api.github.com/repos/SagerNet/sing-box/releases/latest");
        response.EnsureSuccessStatusCode();
        using var release = JsonDocument.Parse(await response.Content.ReadAsStreamAsync());
        var tag = release.RootElement.GetProperty("tag_name").GetString()
                  ?? throw new InvalidOperationException("Release sem versão.");
        var version = tag.TrimStart('v');
        var arch = RuntimeInformation.ProcessArchitecture == Architecture.Arm64 ? "arm64" : "amd64";
        var wanted = $"sing-box-{version}-windows-{arch}.zip";
        JsonElement? asset = null;
        JsonElement? checksumAsset = null;
        foreach (var item in release.RootElement.GetProperty("assets").EnumerateArray())
        {
            var name = item.GetProperty("name").GetString() ?? "";
            if (name == wanted) asset = item.Clone();
            if (Regex.IsMatch(name, "checksums?.*\\.txt$", RegexOptions.IgnoreCase)) checksumAsset = item.Clone();
        }
        if (asset is null) throw new InvalidOperationException($"O release {tag} não contém {wanted}.");

        var work = Path.Combine(Path.GetTempPath(), "discord-refresh-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(work);
        try
        {
            var zipPath = Path.Combine(work, wanted);
            log($"Baixando sing-box oficial {tag}...");
            await DownloadAsync(client, asset.Value.GetProperty("browser_download_url").GetString()!, zipPath);
            var expected = asset.Value.TryGetProperty("digest", out var digestElement)
                ? digestElement.GetString()?.Replace("sha256:", "", StringComparison.OrdinalIgnoreCase) : null;
            if (string.IsNullOrWhiteSpace(expected) && checksumAsset is not null)
            {
                var checksumPath = Path.Combine(work, "checksums.txt");
                await DownloadAsync(client, checksumAsset.Value.GetProperty("browser_download_url").GetString()!, checksumPath);
                expected = File.ReadLines(checksumPath)
                    .FirstOrDefault(x => x.EndsWith(wanted, StringComparison.OrdinalIgnoreCase))?.Split((char[]?)null,
                        StringSplitOptions.RemoveEmptyEntries).FirstOrDefault();
            }
            if (string.IsNullOrWhiteSpace(expected))
                throw new InvalidOperationException("O release não publicou um checksum verificável.");
            await using var archiveStream = File.OpenRead(zipPath);
            var actual = Convert.ToHexString(await SHA256.HashDataAsync(archiveStream));
            if (!actual.Equals(expected, StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException("O checksum do sing-box baixado não confere.");

            var target = Path.Combine(Root, version);
            Directory.CreateDirectory(target);
            ZipFile.ExtractToDirectory(zipPath, work, overwriteFiles: true);
            var executable = Directory.EnumerateFiles(work, "sing-box.exe", SearchOption.AllDirectories).FirstOrDefault()
                             ?? throw new InvalidOperationException("sing-box.exe não encontrado no pacote oficial.");
            File.Copy(executable, Path.Combine(target, "sing-box.exe"), overwrite: true);
            Directory.CreateDirectory(Root);
            File.WriteAllText(currentPointer, version, new UTF8Encoding(false));
            log("Componente instalado e checksum verificado.");
            return Path.Combine(target, "sing-box.exe");
        }
        finally { try { Directory.Delete(work, recursive: true); } catch { } }
    }

    private static async Task DownloadAsync(HttpClient client, string uri, string destination)
    {
        using var response = await client.GetAsync(uri, HttpCompletionOption.ResponseHeadersRead);
        response.EnsureSuccessStatusCode();
        await using var source = await response.Content.ReadAsStreamAsync();
        await using var target = File.Create(destination);
        await source.CopyToAsync(target);
    }
}

internal static class Tunnel
{
    private static readonly string Root = Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DiscordRefreshProxy");
    private static readonly string Logs = Path.Combine(Root, "logs");

    public static string CreateConfig(ProxyInfo proxy)
    {
        Directory.CreateDirectory(Logs);
        var configPath = Path.Combine(Root, "session-" + Guid.NewGuid().ToString("N") + ".json");
        var config = new {
            log = new { level = "info", output = Path.Combine(Logs, "session.log"), timestamp = true },
            dns = new { servers = new[] { new { type = "local", tag = "dns-local" } }, final = "dns-local", strategy = "prefer_ipv4" },
            inbounds = new[] { new { type = "tun", tag = "tun-in", interface_name = "DiscordRefresh", address = new[] { "172.20.0.1/30" }, mtu = 1408, auto_route = true, strict_route = true, stack = "mixed" } },
            outbounds = new object[] { new { type = "direct", tag = "direct" }, new { type = "http", tag = "temp-proxy", server = proxy.Host, server_port = proxy.Port } },
            route = new {
                auto_detect_interface = true, default_domain_resolver = "dns-local",
                rules = new object[] {
                    new { action = "sniff" },
                    new { protocol = "dns", action = "hijack-dns" },
                    new { ip_is_private = true, action = "route", outbound = "direct" },
                    new { process_name = new[] { "Discord.exe", "DiscordCanary.exe", "DiscordPTB.exe", "DiscordDevelopment.exe" }, network = "tcp", action = "route", outbound = "temp-proxy" }
                }, final = "direct"
            }
        };
        File.WriteAllText(configPath, JsonSerializer.Serialize(config), new UTF8Encoding(false));
        return configPath;
    }

    public static async Task ValidateAsync(string executable, string config)
    {
        using var process = StartProcess(executable, $"check -c \"{config}\"", redirect: true);
        var error = await process.StandardError.ReadToEndAsync();
        await process.WaitForExitAsync();
        if (process.ExitCode != 0) throw new InvalidOperationException("Configuração de rede inválida: " + Clean(error));
    }

    public static Process Start(string executable, string config) =>
        StartProcess(executable, $"run -c \"{config}\"", redirect: false);

    private static Process StartProcess(string executable, string arguments, bool redirect)
    {
        var info = new ProcessStartInfo(executable, arguments) {
            UseShellExecute = false, CreateNoWindow = true, WindowStyle = ProcessWindowStyle.Hidden,
            RedirectStandardError = redirect, RedirectStandardOutput = redirect,
            WorkingDirectory = Path.GetDirectoryName(executable)!
        };
        return Process.Start(info) ?? throw new InvalidOperationException("Não foi possível iniciar o componente de rede.");
    }

    private static string Clean(string value) => Regex.Replace(value, @"\x1B\[[0-?]*[ -/]*[@-~]", "").Trim();
}

internal static class NativeInput
{
    private const int SwRestore = 9;
    private const ushort VkControl = 0x11;
    private const ushort VkR = 0x52;
    private const uint KeyUp = 0x0002;
    private const uint InputKeyboard = 1;

    public static bool SendCtrlR(IntPtr window)
    {
        ShowWindowAsync(window, SwRestore);
        SetForegroundWindow(window);
        Thread.Sleep(700);
        if (GetForegroundWindow() != window) return false;
        var inputs = new[] {
            Key(VkControl, 0), Key(VkR, 0), Key(VkR, KeyUp), Key(VkControl, KeyUp)
        };
        return SendInput((uint)inputs.Length, inputs, Marshal.SizeOf<INPUT>()) == inputs.Length;
    }

    private static INPUT Key(ushort code, uint flags) => new() {
        type = InputKeyboard, U = new InputUnion { ki = new KEYBDINPUT { wVk = code, dwFlags = flags } }
    };

    [StructLayout(LayoutKind.Sequential)] private struct INPUT { public uint type; public InputUnion U; }
    [StructLayout(LayoutKind.Explicit)] private struct InputUnion { [FieldOffset(0)] public KEYBDINPUT ki; }
    [StructLayout(LayoutKind.Sequential)] private struct KEYBDINPUT {
        public ushort wVk, wScan; public uint dwFlags, time; public UIntPtr dwExtraInfo;
    }
    [DllImport("user32.dll")] private static extern bool SetForegroundWindow(IntPtr hWnd);
    [DllImport("user32.dll")] private static extern bool ShowWindowAsync(IntPtr hWnd, int command);
    [DllImport("user32.dll")] private static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll", SetLastError = true)] private static extern uint SendInput(uint count, INPUT[] inputs, int size);
}
