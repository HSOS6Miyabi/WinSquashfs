// SPDX-License-Identifier: GPL-3.0-only
using System.Globalization;
using System.Text.Json;

namespace WinSquashfs.Core;

public static class Localization
{
    public const string SystemLanguage = "system";
    public static string PreferencePath => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WinSquashfs", "language.txt");
    private static readonly Lazy<IReadOnlyDictionary<string, string>> Chinese = new(() =>
    {
        using var stream = typeof(Localization).Assembly.GetManifestResourceStream("WinSquashfs.Localization.zh-CN.json")
            ?? throw new InvalidOperationException("Missing Chinese language resources.");
        return JsonSerializer.Deserialize<Dictionary<string, string>>(stream)
            ?? throw new InvalidOperationException("Invalid Chinese language resources.");
    });
    public static string Preference { get; private set; } = SystemLanguage;
    public static string Language { get; private set; } = "en-US";
    public static bool IsChinese => Language == "zh-CN";
    public static string Resolve(string preference, CultureInfo systemCulture)
    {
        if (preference is "en-US" or "zh-CN") return preference;
        string name = systemCulture.Name;
        return name.Equals("zh", StringComparison.OrdinalIgnoreCase) ||
            name.StartsWith("zh-Hans", StringComparison.OrdinalIgnoreCase) ||
            name.Equals("zh-CN", StringComparison.OrdinalIgnoreCase) ||
            name.Equals("zh-SG", StringComparison.OrdinalIgnoreCase) ? "zh-CN" : "en-US";
    }
    public static void Initialize(string? preference = null)
    {
        var system = CultureInfo.CurrentUICulture;
        if (preference == null)
        {
            try { preference = File.Exists(PreferencePath) ? File.ReadAllText(PreferencePath).Trim() : SystemLanguage; }
            catch (IOException) { preference = SystemLanguage; }
            catch (UnauthorizedAccessException) { preference = SystemLanguage; }
        }
        Preference = preference is SystemLanguage or "en-US" or "zh-CN" ? preference : SystemLanguage;
        Language = Resolve(Preference, system);
        var culture = CultureInfo.GetCultureInfo(Language);
        CultureInfo.DefaultThreadCurrentUICulture = culture;
        CultureInfo.CurrentUICulture = culture;
        CultureInfo.DefaultThreadCurrentCulture = culture;
        CultureInfo.CurrentCulture = culture;
    }
    public static void SavePreference(string preference)
    {
        if (preference is not (SystemLanguage or "en-US" or "zh-CN"))
            throw new ArgumentException(T("Language setting is invalid."));
        Directory.CreateDirectory(Path.GetDirectoryName(PreferencePath)!);
        string temp = PreferencePath + "." + Guid.NewGuid().ToString("N") + ".tmp";
        try
        {
            File.WriteAllText(temp, preference);
            File.Move(temp, PreferencePath, true);
            Preference = preference;
        }
        finally { if (File.Exists(temp)) File.Delete(temp); }
    }
    public static string T(string english, params object?[] args)
    {
        string template = IsChinese && Chinese.Value.TryGetValue(english, out var value) ? value : english;
        return args.Length == 0 ? template : string.Format(CultureInfo.CurrentCulture, template, args);
    }
}
