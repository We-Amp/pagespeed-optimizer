// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 We-Amp B.V.

using Microsoft.Win32.SafeHandles;

namespace WeAmp.PageSpeed.Native;

internal sealed class SafeCacheHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeCacheHandle() : base(ownsHandle: true) { }
    internal SafeCacheHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.CacheClose(handle);
        return true;
    }
}

internal sealed class SafeReadResultHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeReadResultHandle() : base(ownsHandle: true) { }
    internal SafeReadResultHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.ReadFree(handle);
        return true;
    }
}

internal sealed class SafeWriteHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeWriteHandle() : base(ownsHandle: true) { }
    internal SafeWriteHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        // Abort is the safety net: releases the native handle even if
        // the caller forgot to call Commit(). Normal path calls WriteClose
        // then SetHandleAsInvalid() to prevent double-close.
        NativePageSpeed.WriteAbort(handle);
        return true;
    }
}

internal sealed class SafeHtmlResultHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeHtmlResultHandle() : base(ownsHandle: true) { }
    internal SafeHtmlResultHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.HtmlResultFree(handle);
        return true;
    }
}

internal sealed class SafeScanResultHandle : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeScanResultHandle() : base(ownsHandle: true) { }
    internal SafeScanResultHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.ScanResultFree(handle);
        return true;
    }
}

internal sealed class SafeCriticalCssResultHandle
    : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeCriticalCssResultHandle() : base(ownsHandle: true) { }
    internal SafeCriticalCssResultHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.CriticalCssResultFree(handle);
        return true;
    }
}

internal sealed class SafeHtmlTransformHandle
    : SafeHandleZeroOrMinusOneIsInvalid
{
    public SafeHtmlTransformHandle() : base(ownsHandle: true) { }
    internal SafeHtmlTransformHandle(IntPtr ptr) : base(ownsHandle: true)
        => SetHandle(ptr);
    protected override bool ReleaseHandle()
    {
        NativePageSpeed.HtmlTransformFree(handle);
        return true;
    }
}
