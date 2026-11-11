// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#pragma once

namespace ajaduplex::probe
{

/// duplex-probe list: one line per AJA device. Exit code 0 when at least one
/// device was found, 1 otherwise.
int runList();

} // namespace ajaduplex::probe
