// Copyright (C) 2026 WarcraftXL. GPL-3.0-or-later.
#pragma once
namespace wxl::modern::materialdiag
{
    void Initialize() noexcept;
    // Shared read-only GPU report. IDs >=1000 are reserved for the particle probe.
    void ReportGpu(void* device, unsigned id) noexcept;
    // Call only on the ordinary M2 DIP branch, after deferred state flush and before
    // the original draw. Does not intercept, issue draws, or change device state.
    void BeforeDraw(void* device, void* drawContext, void* instance,
                    unsigned startIndex, unsigned primitiveCount) noexcept;
}
