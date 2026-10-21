// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

// Pass Chatterino's cached emotes to MessageHeightTwitch
using System;
using System.Collections.Generic;
using System.Drawing;
using System.IO;
using System.Reflection;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text.Json;
using MessageHeightTwitch.Emotes;
using MessageHeightTwitch.Measurement.EmoteRewriting;
using Mht = MessageHeightTwitch.MessageHeightTwitch;

internal static class ChatterinoExports
{
    private static string Directory = "";
    private static HashSet<string> FfzEmoji = new(StringComparer.Ordinal);
    private static readonly BTTVEmoteProvider Bttv = new();

    private static string Read(nint value) => Marshal.PtrToStringUTF8(value) ?? throw new ArgumentNullException();

    private static nint Resolve(string name, Assembly assembly, DllImportSearchPath? search)
    {
        var filename = OperatingSystem.IsWindows() ? name + ".dll"
            : (name.StartsWith("lib", StringComparison.Ordinal) ? name : "lib" + name) + ".so";
        var file = Path.Combine(Directory, filename);
        return File.Exists(file) ? NativeLibrary.Load(file) : 0;
    }

    [UnmanagedCallersOnly(EntryPoint = "ChatterinoHeightInit", CallConvs = [typeof(CallConvCdecl)])]
    public static int Init(nint directory)
    {
        try {
            Directory = Read(directory);
            NativeLibrary.SetDllImportResolver(typeof(ChatterinoExports).Assembly, Resolve);
            NativeLibrary.SetDllImportResolver(typeof(SkiaSharp.SKObject).Assembly, Resolve);
            NativeLibrary.SetDllImportResolver(typeof(HarfBuzzSharp.Blob).Assembly, Resolve);
            using var emojis = JsonDocument.Parse(File.ReadAllText(Path.Combine(Directory, "ffz-emojis.json")));
            foreach (var entry in emojis.RootElement.EnumerateArray()) FfzEmoji.Add(entry.GetString()!);
            Mht.InitFonts(Path.Combine(Directory, "Fonts"));
            return 1;
        } catch (Exception error) { Console.Error.WriteLine(error); return 0; }
    }

    private static EmoteSet Catalog(JsonElement root, string name, Mht.EMOTE_PROVIDER provider, EmoteSet? target = null)
    {
        var result = target ?? new EmoteSet();
        if (!root.TryGetProperty(name, out var entries)) return result;
        foreach (var entry in entries.EnumerateArray()) {
            var code = entry.GetProperty("name").GetString()!;
            var size = new SizeF(entry.GetProperty("width").GetSingle(), entry.GetProperty("height").GetSingle());
            var modifier = EmoteModifier.Known(code, provider);
            if (provider == Mht.EMOTE_PROVIDER.SEVENTV && entry.TryGetProperty("zeroWidth", out var overlay) && overlay.GetBoolean()) size = new(-size.Width, -size.Height);
            if (entry.TryGetProperty("zeroWidth", out var zeroWidth) && zeroWidth.GetBoolean()) modifier ??= new();
            result.TryAdd(code, new CatalogEmote(size, modifier));
        }
        return result;
    }

    [UnmanagedCallersOnly(EntryPoint = "ChatterinoHeightMeasure", CallConvs = [typeof(CallConvCdecl)])]
    public static float Measure(nint request)
    {
        try {
            using var document = JsonDocument.Parse(Read(request));
            var root = document.RootElement;
            var providers = root.GetProperty("providers");
            var globals = new GlobalEmotes();
            var loading = new EmoteLoading(Globals: globals);
            var bttv = new BTTVEmoteProvider(loading);
            var ffz = new FFZEmoteProvider(loading);
            // Keep channel and global emotes separate so lookup order stays the same
            var bc = Catalog(providers, "bttv", Mht.EMOTE_PROVIDER.BTTV, bttv.Channel);
            var fc = Catalog(providers, "ffz", Mht.EMOTE_PROVIDER.FFZ, ffz.Room);
            var sc = Catalog(providers, "seventv", Mht.EMOTE_PROVIDER.SEVENTV);
            var bg = Catalog(providers, "bttvGlobal", Mht.EMOTE_PROVIDER.BTTV, globals.Bttv);
            var fg = Catalog(providers, "ffzGlobal", Mht.EMOTE_PROVIDER.FFZ, globals.Ffz);
            var sg = Catalog(providers, "seventvGlobal", Mht.EMOTE_PROVIDER.SEVENTV);
            var twitch = Catalog(providers, "twitch", Mht.EMOTE_PROVIDER.NONE);
            var twitchUrls = new Dictionary<string, string>(StringComparer.Ordinal);
            if (providers.TryGetProperty("twitch", out var te))
                foreach (var entry in te.EnumerateArray()) twitchUrls.Add(entry.GetProperty("name").GetString()!, entry.GetProperty("url").GetString()!);
            var emojis = FfzEmoji;
            if (root.TryGetProperty("ffzEmoji", out var explicitEmoji)) {
                emojis = new(StringComparer.Ordinal);
                foreach (var entry in explicitEmoji.EnumerateArray()) emojis.Add(entry.GetString()!);
            }
            static bool Lookup(EmoteSet set, ReadOnlySpan<char> name, out SizeF size) {
                var found = set.TryGet(name, out var emote); size = emote.Size; return found;
            }
            var resolver = new EmoteResolver(
                (ReadOnlySpan<char> name, out SizeF size) => Lookup(sc, name, out size),
                (ReadOnlySpan<char> name, out SizeF size) => Lookup(bc, name, out size),
                (ReadOnlySpan<char> name, out SizeF size) => Lookup(fc, name, out size),
                (name, url) => Lookup(twitch, name, out var size) ? size : throw new ArgumentException("Missing cached Twitch dimensions"),
                Bttv.IsEmojiSupported, emoji => emojis.GetAlternateLookup<ReadOnlySpan<char>>().Contains(emoji)) {
                BTTVProvider = bttv, FFZProvider = ffz,
                BTTVGlobal = (ReadOnlySpan<char> name, out SizeF size) => Lookup(bg, name, out size),
                FFZGlobal = (ReadOnlySpan<char> name, out SizeF size) => Lookup(fg, name, out size),
                SevenTvGlobal = (ReadOnlySpan<char> name, out SizeF size) => Lookup(sg, name, out size)
            };
            using var instance = new Mht(resolver);
            var parameters = Mht.CalculateMessageHeightParams.Default;
            parameters.NumberOfBadges = root.GetProperty("badges").GetInt32();
            parameters.TwitchEmotes = twitchUrls;
            parameters.IsAction = root.TryGetProperty("action", out var action) && action.GetBoolean();
            return instance.CalculateMessageHeightEx(root.GetProperty("text").GetString()!, root.GetProperty("login").GetString()!, root.GetProperty("displayName").GetString()!, parameters);
        } catch (Exception error) { Console.Error.WriteLine(error); return -1; }
    }
}

namespace MessageHeightTwitch {
    public partial class MessageHeightTwitch {
        internal MessageHeightTwitch(EmoteResolver resolver) {
            CheckInit();
            EmoteLifetime = new();
            ChannelFonts = FontGlobalContext.Shared.FallbackList;
            Emotes = resolver;
        }
    }
}
