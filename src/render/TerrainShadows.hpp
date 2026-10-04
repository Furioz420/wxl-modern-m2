// Copyright (C) 2026 WarcraftXL. SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "wxl/M2DrawApi.h"
namespace wxl::modern::terrainshadow
{
    void Install();
    long Draw(void* device,int type,int base,unsigned min,unsigned vertices,unsigned start,unsigned count,WXL_M2Draw_DIPFn original);
}
