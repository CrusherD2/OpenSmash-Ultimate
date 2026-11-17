using System.Net;
using System.Text;
using OpenSmash.Core;
using OpenSmash.Launcher;
using SkiaSharp;
static class ParityChecks
{
    static void Check(bool ok, string message) { if (!ok) throw new Exception(message); Console.WriteLine("PASS " + message); }
    public static async Task Run()
    {
        Check(UpdateCompatibility.IsStableRelease("v1.2.3") && !UpdateCompatibility.IsStableRelease("discord-1") && !UpdateCompatibility.IsStableRelease("v1.2.3-alpha.1"), "only real stable releases select the default release channel");
        Check(UpdateCompatibility.InstallerTakesUpdateOnly("param([switch] $UpdateOnly)") && !UpdateCompatibility.InstallerTakesUpdateOnly("param([switch] $UpdateOnlyOld)"), "legacy installer detection requires the exact update switch");
        string installer = "param([switch]$UpdateOnly) $RepoName = 'OpenSmash-Ultimate'";
        UpdateCompatibility.ValidateInstaller(installer);
        bool rejected = false;
        try { UpdateCompatibility.ValidateInstaller("<html>error</html>"); } catch (InvalidDataException) { rejected = true; }
        Check(rejected, "an invalid fetched installer cannot replace a local installer");
        Check(UpdateCompatibility.ResolveForeignCommit("598d05240fc146f7184b3450653e2eec2ee97312") == "8025b483b0443adb5668395eca31f9250e8c8efb" && UpdateCompatibility.ResolveForeignCommit(new string('a', 40)) == new string('a', 40), "WindowSmash README commits compare through their shared ancestor");
        Check(ProblemReport.Url("13.0.5 & test").EndsWith("&version=13.0.5%20%26%20test") && !ProblemReport.Url("").Contains("&version="), "problem reports prefill and escape the game version");
        string url = "https://cdn.discordapp.com/avatars/1/hash.png";
        Check(CommunityFeed.AvatarUrl(url) == url && CommunityFeed.AvatarUrl("https://cdn.discordapp.com.evil.test/a") == "" && CommunityFeed.AvatarUrl("http://cdn.discordapp.com/a") == "", "leaderboard pictures accept only Discord HTTPS URLs");
        var parsed = CommunityFeed.ParseBoard("""{"players":[{"rank":1,"name":"Player Name","avatar":"https://cdn.discordapp.com/avatars/1/hash.png"}]}""");
        Check(parsed?.Top.Single().Avatar == url && CommunityFeed.Initials("Player Name") == "PN", "leaderboard pictures and initials survive parsing");
        await OldInstallerRefresh();
        using var bitmap = new SKBitmap(2, 2);
        using var image = SKImage.FromBitmap(bitmap);
        using var png = image.Encode(SKEncodedImageFormat.Png, 100);
        var handler = new AvatarHandler(png.ToArray());
        using var client = new HttpClient(handler);
        Check(await BoardAvatar.Fetch(client, url, CancellationToken.None) is { Length: > 0 }, "valid leaderboard images load without a live network");
        int requests = handler.Requests;
        Check(await BoardAvatar.Fetch(client, "https://example.com/a", CancellationToken.None) == null && handler.Requests == requests, "untrusted picture URLs are rejected before making a request");
        handler.Body = Encoding.UTF8.GetBytes("not an image");
        Check(await BoardAvatar.Fetch(client, url, CancellationToken.None) == null, "invalid image bytes fall back to initials");
        handler.Body = new byte[(1 << 20) + 1];
        Check(await BoardAvatar.Fetch(client, url, CancellationToken.None) == null, "oversized avatar downloads are refused");
    }
    static async Task OldInstallerRefresh()
    {
        string root = Path.Combine(Path.GetTempPath(), "avalonia-old-update-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(Path.Combine(root, "tools"));
        string script = Path.Combine(root, "tools", "install_windowsmash.ps1");
        const string original = "old WindowSmash installer";
        var handler = new AvatarHandler(Encoding.UTF8.GetBytes("not a valid installer"));
        using var client = new HttpClient(handler);
        try
        {
            await File.WriteAllTextAsync(script, original);
            bool rejected = false;
            try { await InstallerRefresh.Refresh(root, new string('a', 40), client); }
            catch (InvalidDataException) { rejected = true; }
            Check(rejected && await File.ReadAllTextAsync(script) == original, "failed installer validation preserves the old script byte for byte");
            string valid = "param([switch]$UpdateOnly) $RepoName = 'OpenSmash-Ultimate'";
            handler.Body = Encoding.UTF8.GetBytes(valid);
            await InstallerRefresh.Refresh(root, new string('a', 40), client);
            Check(await File.ReadAllTextAsync(script) == valid && !Directory.EnumerateFiles(Path.GetDirectoryName(script)!).Any(path => path.Contains(".avalonia-")), "a validated commit-pinned installer replaces the legacy script without staging leftovers");
            await File.WriteAllTextAsync(script, original);
            await File.WriteAllTextAsync(Path.Combine(root, ".git"), "synthetic git worktree marker");
            int requests = handler.Requests;
            bool refused = false;
            try { await InstallerRefresh.Refresh(root, new string('a', 40), client); }
            catch (InvalidOperationException) { refused = true; }
            Check(refused && requests == handler.Requests && await File.ReadAllTextAsync(script) == original, "legacy git checkouts refuse installer replacement before any download");
        }
        finally { Directory.Delete(root, true); }
    }
    sealed class AvatarHandler(byte[] body) : HttpMessageHandler
    {
        public byte[] Body = body;
        public int Requests;
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
        {
            Requests++;
            return Task.FromResult(new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(Body) });
        }
    }
}
