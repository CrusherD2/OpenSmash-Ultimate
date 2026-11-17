namespace OpenSmash.Core;
public static class ProblemReport
{
    public static string Url(string version) => "https://github.com/OpenSmash-Ultimate/OpenSmash-Ultimate/issues/new?template=bug_report.yml"
        + (string.IsNullOrWhiteSpace(version) ? "" : "&version=" + Uri.EscapeDataString(version.Trim()));
    public static string LogFolder(Project project) => project.PathIn("build");
}
