using OpenSmash.Core;
namespace OpenSmash;
public sealed partial class MainWindow
{
    void ReportProblem()
    {
        string version = "";
        if (_project is {} project)
        {
            string file = project.PathIn("version.txt");
            if (File.Exists(file)) version = File.ReadLines(file).FirstOrDefault(line => line.Trim().Length > 0 && !line.StartsWith('#') && !line.StartsWith(';'))?.Trim() ?? "";
            try { if (Directory.Exists(ProblemReport.LogFolder(project))) Commands.OpenPath(ProblemReport.LogFolder(project)); }
            catch (Exception error) when (error is System.ComponentModel.Win32Exception or InvalidOperationException) { }
        }
        Commands.OpenPath(ProblemReport.Url(version));
    }
}
