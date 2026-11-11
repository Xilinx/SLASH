# SPDX-License-Identifier: MIT
# Implementation steps of a service layer with two RDMA cores, sourced by
# service_layer_build.tcl once it has set the run's strategy.
#
# Each core is about 100k LUTs and 260k flip-flops. The strategy's placement
# (Floorplan.BalancedSLR.high) balances the logic between SLR0 and SLR1, so
# it put one core in each, spread across the width of the die and away from
# its NoC ports, and the service clock missed by about 1 ns. The default
# placement keeps each core next to its ports. phys_opt_design after routing
# takes the last few tens of picoseconds.
set run [get_runs impl_1]
set_property STEPS.PLACE_DESIGN.ARGS.SUBDIRECTIVE {} $run
set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE Default $run
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED true $run
set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore $run
