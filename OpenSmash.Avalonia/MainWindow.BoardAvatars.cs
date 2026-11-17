using System.Net.Http;
using OpenSmash.Core;
namespace OpenSmash;
public sealed partial class MainWindow
{
    async Task LoadBoardAvatars()
    {
        var view = Surface.Community;
        string[] urls = view.Top.Select(row => row.Avatar).Where(url => url.Length > 0).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal).ToArray();
        var loaded = new Dictionary<string, byte[]>(StringComparer.Ordinal);
        using var handler = new HttpClientHandler { AllowAutoRedirect = false };
        using var client = new HttpClient(handler);
        foreach (string url in urls)
        {
            if (_closed) return;
            byte[]? bytes = view.BoardAvatars.GetValueOrDefault(url) ?? await BoardAvatar.Fetch(client, url, CancellationToken.None);
            if (bytes != null) loaded[url] = bytes;
        }
        if (!_closed) view.BoardAvatars = loaded;
    }
}
