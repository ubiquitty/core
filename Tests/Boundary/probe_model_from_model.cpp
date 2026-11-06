// Copyright (C) 2026 Stanislav Yaglo
// SPDX-License-Identifier: Apache-2.0

// The positive control for the negative probe beside it. An empty or misplaced
// model stage would make every __has_include in that probe false, and the probe
// would pass while checking nothing; this file refuses to compile in that case.
#if !__has_include("Core/Cell.h")
#error "Core/Cell.h cannot be reached from the model stage. The stage is empty or in the wrong place, and the Rule-10 negative probe would pass without testing anything."
#endif

int main() { }
