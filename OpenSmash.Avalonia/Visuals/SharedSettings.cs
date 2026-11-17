using System.Security.Cryptography;
using System.Text;
namespace OpenSmash.Visuals;
public sealed partial class ClassicSurface
{
    bool _reloadingSettings;
    string _settingsSnapshot = "";
    double _settingsCheckedAt;
    string SettingsSnapshot()
    {
        if (Project == null) return "";
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (string name in OpenSmash.Core.Project.ConfigNames)
        {
            string path = Project.ConfigFile(name);
            hash.AppendData(Encoding.UTF8.GetBytes(path + (char)0));
            if (File.Exists(path)) hash.AppendData(File.ReadAllBytes(path));
        }
        return Convert.ToHexString(hash.GetHashAndReset());
    }
    public void ReloadSharedSettings()
    {
        if (_reloadingSettings || !_settingsLoaded || Project == null || _editingKey != null) return;
        _reloadingSettings = true;
        try
        {
            string snapshot = SettingsSnapshot();
            if (snapshot == _settingsSnapshot) return;
            var pending = _changedSettings.ToDictionary(k => k, k => _iniValues.GetValueOrDefault(k, ""));
            ReadSettings();
            foreach (var item in pending) { _iniValues[item.Key] = item.Value; _changedSettings.Add(item.Key); }
            _settingsSnapshot = SettingsSnapshot();
            InvalidateVisual();
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException) { }
        finally { _reloadingSettings = false; }
    }
}
