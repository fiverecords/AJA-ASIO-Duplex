// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Joaquin Villodre

#pragma once

#include "ProbeOptions.h"

namespace ajaduplex::probe
{

/// duplex-probe heads (see HeadsProbe.h). Exit code 0 on success, 1 when the
/// card cannot be used, a file cannot be written or the run was interrupted.
int runHeadsCommand(const Options& options);

/// duplex-probe cost (see CostProbe.h). Same exit codes.
int runCostCommand(const Options& options);

/// duplex-probe loop (see LoopProbe.h). Same exit codes; a run that is not
/// clean is a result, not an error.
int runLoopCommand(const Options& options);

/// duplex-probe routing (see RoutingProbe.h). Same exit codes.
int runRoutingCommand(const Options& options);
int runWatchCommand(const Options& options);

} // namespace ajaduplex::probe
