using System.Text.RegularExpressions;
using OpenSmash.Core;
namespace OpenSmash;
public sealed partial class MainWindow
{
    async Task EnsureCompatibleInstaller()
    {
        var project = RequireProject();
        if (!InstallerRefresh.Needed(project.Root)) return;
        string revision = _updateHead;
        if (!Regex.IsMatch(revision, "^[0-9a-fA-F]{40}$"))
        {
            using var commit = await GitHub("/commits/" + await UpdateRef());
            revision = commit.RootElement.GetProperty("sha").GetString() ?? "";
        }
        await InstallerRefresh.Refresh(project.Root, revision, Http);
    }
}
