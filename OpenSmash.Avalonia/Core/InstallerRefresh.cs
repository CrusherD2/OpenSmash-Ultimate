using System.Net.Http;
using System.Text;
using System.Text.RegularExpressions;
namespace OpenSmash.Core;
public static class InstallerRefresh
{
    public static bool Needed(string root)
    {
        string script = Path.Combine(root, "tools", "install_windowsmash.ps1");
        if (UpdateCompatibility.InstallerTakesUpdateOnly(File.ReadAllText(script))) return false;
        if (Directory.Exists(Path.Combine(root, ".git")) || File.Exists(Path.Combine(root, ".git")))
            throw new InvalidOperationException("This old WindowSmash folder is a git checkout. Update it with git first; no tracked files were changed.");
        return true;
    }
    public static async Task Refresh(string root, string revision, HttpClient client)
    {
        if (!Needed(root)) return;
        if (!Regex.IsMatch(revision, "^[0-9a-fA-F]{40}$")) throw new InvalidDataException("GitHub did not identify the update commit.");
        byte[] body = await client.GetByteArrayAsync($"https://raw.githubusercontent.com/OpenSmash-Ultimate/OpenSmash-Ultimate/{revision}/tools/install_windowsmash.ps1");
        UpdateCompatibility.ValidateInstaller(Encoding.UTF8.GetString(body));
        string script = Path.Combine(root, "tools", "install_windowsmash.ps1");
        string staged = script + ".avalonia-" + Guid.NewGuid().ToString("N");
        try
        {
            await File.WriteAllBytesAsync(staged, body);
            File.Move(staged, script, true);
        }
        finally { if (File.Exists(staged)) File.Delete(staged); }
    }
}
