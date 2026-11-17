using System.Text.RegularExpressions;
namespace OpenSmash.Core;
public static class UpdateCompatibility
{
    public static bool InstallerTakesUpdateOnly(string text) => Regex.IsMatch(text, @"\[switch\]\s*\$UpdateOnly\b", RegexOptions.IgnoreCase);
    public static bool IsStableRelease(string tag) => Regex.IsMatch(tag, @"^v\d+\.\d+\.\d+$");
    public static string ResolveForeignCommit(string commit) => commit.ToLowerInvariant() switch
    {
        "598d05240fc146f7184b3450653e2eec2ee97312" or "3d5ba9095bf4a2c0d146df31adec0d59b8337bbb" => "8025b483b0443adb5668395eca31f9250e8c8efb",
        _ => commit
    };
    public static void ValidateInstaller(string text)
    {
        if (!InstallerTakesUpdateOnly(text) || !text.Contains("$RepoName = 'OpenSmash-Ultimate'", StringComparison.Ordinal))
            throw new InvalidDataException("The downloaded installer is not the expected OpenSmash updater. Nothing was changed.");
    }
}
