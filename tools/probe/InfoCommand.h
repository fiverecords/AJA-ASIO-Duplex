// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#pragma once

#include "ProbeOptions.h"

namespace ajaduplex::probe
{

/// duplex-probe info: prints the short report and writes the full one to a
/// file. Exit code 0 on success, 1 when the device cannot be opened or the
/// file cannot be written.
int runInfo(const Options& options);

} // namespace ajaduplex::probe
