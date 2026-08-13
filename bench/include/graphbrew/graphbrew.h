#pragma once
// Umbrella header for GraphBrew public API

// Core GAPBS headers
#include <builder.h>
#include <benchmark.h>
#include <graph.h>
#include <command_line.h>

// GraphBrew extensions
#include "reorder/reorder.h"
#include "partition/cagra/popt.h"
#include "partition/trust.h"

// External algorithms
#include <rabbit/rabbit_order.hpp>
#include <gorder/GoGraph.h>
#include <gorder/GoUtil.h>

// Cache simulation
#include <cache_sim/cache_sim.h>
#include <cache_sim/graph_sim.h>
