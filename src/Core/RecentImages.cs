// SPDX-License-Identifier: GPL-3.0-only
using System.Text.Json;

namespace WinSquashfs.Core;
public static class RecentImages
{
    private static readonly string PathName = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        "WinSquashfs", "recent.json");
    public static IReadOnlyList<string> Load()
    {
        try { return File.Exists(PathName) ? JsonSerializer.Deserialize<string[]>(File.ReadAllText(PathName)) ?? [] : []; }
        catch (IOException) { return []; }
        catch (UnauthorizedAccessException) { return []; }
        catch (JsonException) { return []; }
    }
    public static void Add(string path) => Save(new[] { Path.GetFullPath(path) }.Concat(Load()).Distinct(StringComparer.OrdinalIgnoreCase).Take(15));
    public static void Remove(string path) => Save(Load().Where(p => !string.Equals(p, path, StringComparison.OrdinalIgnoreCase)));
    public static void Clear() => Save(Array.Empty<string>());
    private static void Save(IEnumerable<string> values)
    {
        string temporary = PathName + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(PathName)!);
            File.WriteAllText(temporary, JsonSerializer.Serialize(values.ToArray()));
            File.Move(temporary, PathName, true);
        }
        catch (IOException) { }
        catch (UnauthorizedAccessException) { }
        finally { try { if (File.Exists(temporary)) File.Delete(temporary); } catch (IOException) { } catch (UnauthorizedAccessException) { } }
    }
}
