// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Extensions.Options;
using WeAmp.PageSpeed.Native;

namespace WeAmp.PageSpeed.AspNetCore;

/// <summary>
/// High-level HTML processing API. Thread-safe.
/// Rebuilds NativeHtmlConfig per call to support options hot-reload.
/// </summary>
public sealed class HtmlProcessor : IHtmlProcessor
{
    private readonly IPageSpeedCache? _cache;
    private readonly IOptionsMonitor<PageSpeedOptions> _options;

    /// <summary>
    /// Creates a new HTML processor.
    /// </summary>
    /// <param name="cache">
    /// Optional cache for CSS lookup during processing.
    /// </param>
    /// <param name="options">
    /// Options monitor for hot-reloadable configuration.
    /// </param>
    public HtmlProcessor(
        IPageSpeedCache? cache,
        IOptionsMonitor<PageSpeedOptions> options)
    {
        _cache = cache;
        _options = options;
    }

    /// <inheritdoc/>
    public HtmlResult Process(
        ReadOnlySpan<byte> html, string url, string hostname)
    {
        var opts = _options.CurrentValue.Html;
        var config = BuildNativeConfig(opts);

        int err = NativePageSpeed.HtmlProcess(
            html, (nuint)html.Length,
            url, hostname,
            in config, _cache?.Handle,
            out var raw);
        NativeCheck.ThrowOnError(err);
        return new HtmlResult(new SafeHtmlResultHandle(raw));
    }

    private static NativeHtmlConfig BuildNativeConfig(
        HtmlProcessingOptions opts)
    {
        var config = new NativeHtmlConfig();
        NativePageSpeed.HtmlConfigInit(ref config);
        config.EnableCriticalCss = opts.EnableCriticalCss ? 1 : 0;
        config.EnableLazyLoad = opts.EnableLazyLoad ? 1 : 0;
        config.EnableImageDimensions = opts.EnableImageDimensions ? 1 : 0;
        config.EnableLcpPreload = opts.EnableLcpPreload ? 1 : 0;
        config.EnablePreconnect = opts.EnablePreconnect ? 1 : 0;
        config.EnableSpeculationRules = opts.EnableSpeculationRules ? 1 : 0;
        config.EnableAsyncCss = opts.EnableAsyncCss ? 1 : 0;
        config.Viewport = (int)opts.Viewport;
        config.MaxHtmlSize = (nuint)opts.MaxHtmlSizeBytes;
        config.MaxCssSize = (nuint)opts.MaxCssSizeBytes;
        // Note: Selector pattern arrays (AlwaysIncludeSelectors etc.) require
        // PinnedStringArray marshalling. Supported via the low-level
        // HtmlTransform API. The high-level HtmlProcess sets them to IntPtr.Zero
        // (use defaults from ps_html_config_init).
        return config;
    }
}
