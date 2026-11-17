using System.Net.Http;
using OpenSmash.Launcher;
using SkiaSharp;
namespace OpenSmash.Core;
public static class BoardAvatar
{
    const int MaxBytes = 1 << 20;
    public static async Task<byte[]?> Fetch(HttpClient client, string url, CancellationToken cancel)
    {
        if (CommunityFeed.AvatarUrl(url) == "") return null;
        try
        {
            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
            timeout.CancelAfter(TimeSpan.FromSeconds(8));
            using var response = await client.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, timeout.Token);
            if (!response.IsSuccessStatusCode || response.Content.Headers.ContentLength > MaxBytes) return null;
            await using var input = await response.Content.ReadAsStreamAsync(timeout.Token);
            using var output = new MemoryStream();
            byte[] buffer = new byte[8192];
            int count;
            while ((count = await input.ReadAsync(buffer, timeout.Token)) > 0)
            {
                if (output.Length + count > MaxBytes) return null;
                output.Write(buffer, 0, count);
            }
            byte[] bytes = output.ToArray();
            using var data = SKData.CreateCopy(bytes);
            using var codec = SKCodec.Create(data);
            return codec != null && codec.Info.Width is > 0 and <= 1024 && codec.Info.Height is > 0 and <= 1024 ? bytes : null;
        }
        catch (Exception error) when (error is HttpRequestException or OperationCanceledException or IOException) { return null; }
    }
}
