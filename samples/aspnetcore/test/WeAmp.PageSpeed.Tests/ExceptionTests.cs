// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Xunit;

namespace WeAmp.PageSpeed.Tests;

/// <summary>
/// Tests PageSpeedException construction without the native library.
/// The constructor uses a try/catch fallback when native error name/description
/// functions are unavailable.
/// </summary>
public class ExceptionTests
{
    [Fact]
    public void Constructor_WithNotFound_SetsErrorCode()
    {
        var ex = new PageSpeedException(PageSpeedError.NotFound, "test");

        Assert.Equal(PageSpeedError.NotFound, ex.ErrorCode);
    }

    [Fact]
    public void Constructor_WithNotFound_FallbackMessageContainsErrorCode()
    {
        var ex = new PageSpeedException(PageSpeedError.NotFound, "test");

        // Without native library, fallback format is "PageSpeed error 1: test"
        Assert.Contains("1", ex.Message);
    }

    [Fact]
    public void Constructor_WithDetail_FallbackMessageContainsDetail()
    {
        var ex = new PageSpeedException(PageSpeedError.IoError, "disk full");

        Assert.Contains("disk full", ex.Message);
    }

    [Fact]
    public void Constructor_WithEmptyDetail_FallbackMessageOmitsDetail()
    {
        var ex = new PageSpeedException(PageSpeedError.Internal, "");

        // Fallback without detail: "PageSpeed error 99"
        Assert.Contains("99", ex.Message);
        Assert.DoesNotContain(":", ex.Message);
    }

    [Fact]
    public void Constructor_WithOk_SetsErrorCodeToOk()
    {
        var ex = new PageSpeedException(PageSpeedError.Ok, "unexpected");

        Assert.Equal(PageSpeedError.Ok, ex.ErrorCode);
        Assert.Contains("0", ex.Message);
    }

    [Fact]
    public void Constructor_WithInternal_SetsErrorCodeTo99()
    {
        var ex = new PageSpeedException(PageSpeedError.Internal, "something broke");

        Assert.Equal(PageSpeedError.Internal, ex.ErrorCode);
        Assert.Contains("99", ex.Message);
        Assert.Contains("something broke", ex.Message);
    }

    [Fact]
    public void IsException_DerivedFromException()
    {
        var ex = new PageSpeedException(PageSpeedError.NotFound, "test");

        Assert.IsAssignableFrom<Exception>(ex);
    }

    [Fact]
    public void AllErrorCodes_CanBeConstructed()
    {
        foreach (PageSpeedError code in Enum.GetValues<PageSpeedError>())
        {
            var ex = new PageSpeedException(code, "test");
            Assert.Equal(code, ex.ErrorCode);
        }
    }
}
