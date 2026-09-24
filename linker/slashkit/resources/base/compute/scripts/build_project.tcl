# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
#
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
#
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

# The HBM boundary helper defines slash_check_hbm_boundary_reset, called below.
# It is sourced here rather than relying on the project-creation path: that path
# runs only for action "create"/"all", so building an existing project with
# action "build" would otherwise fail with 'invalid command name'.
source [file join [file dirname [file normalize [info script]]] ".." ".." "common" "scripts" "hbm_boundary_slices.tcl"]

proc build_project {{proj_name "user"} {jobs 14}} {
  puts "INFO: Using proj_name='$proj_name' and jobs='$jobs' (compute-only platform)"

  # Ensure top BD is generated
  generate_target all [get_files "top.bd"]

  # The HBM boundary reset synchroniser only resolves its polarity when the
  # IP is generated, so this is the first point it can be checked.
  slash_check_hbm_boundary_reset

  # Compute-only: single PR partition (slash only, no service_layer).
  #
  # Drop an existing config_1 first. create_pr_configuration errors out if the
  # name is already taken, so without this, re-running the "build" action on a
  # project that has already been built fails with "PR Configuration with name
  # config_1 already exist" - which is exactly the rebuild-an-existing-checkout
  # workflow, and the one you hit after an interrupted build.
  if {[llength [get_pr_configurations -quiet config_1]]} {
    delete_pr_configurations [get_pr_configurations config_1]
  }
  create_pr_configuration -name config_1 \
    -partitions [list \
      top_i/slash:slash_base_inst_0 \
    ]

  # Parent impl run remains 'impl_1'
  set_property PR_CONFIGURATION config_1 [get_runs impl_1]
  set_property strategy Performance_NetDelay_high [get_runs impl_1]
  # Placement directive: AggressiveExplore, not the strategy's default Explore.
  #
  # Measured in a six-point directive sweep at 380 MHz, changing ONLY this:
  #     place=Explore           -> WNS -0.196  (fails 380 by 0.064)
  #     place=AggressiveExplore -> WNS -0.102  (passes 380 by 0.030)
  # 0.108 ns from one directive, and the difference between closing and not.
  # The Explore arm reproduced the standalone build's -0.196 exactly, so this is
  # a real directive effect and not run-to-run noise (which measures ~0.043 ns).
  set_property STEPS.PLACE_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
  # Performance_NetDelay_high already gives opt/place Explore, phys_opt
  # AggressiveExplore and route NoTimingRelaxation, but it leaves the post-route
  # phys_opt disabled. On the 400 MHz HBM datapath the last picoseconds come from
  # exactly that step, so enable it with the same aggressive directive.
  set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.IS_ENABLED     true              [get_runs impl_1]
  set_property STEPS.POST_ROUTE_PHYS_OPT_DESIGN.ARGS.DIRECTIVE AggressiveExplore [get_runs impl_1]
  set_property STEPS.OPT_DESIGN.TCL.POST         [get_files *opt.post.tcl]                [get_runs impl_1]
  set_property STEPS.PLACE_DESIGN.TCL.PRE        [get_files *place.pre.tcl]               [get_runs impl_1]
  set_property STEPS.WRITE_DEVICE_IMAGE.TCL.PRE  [get_files *write_device_image.pre.tcl]  [get_runs impl_1]

  # NOTE - synthesis effort was tried here and did NOT help; do not re-add it
  # without new evidence.
  #
  # Measured A/B at 380 MHz, identical clock and identical implementation, the
  # only variable being Flow_PerfOptimized_high + RETIMING on synth_1 and
  # slash_base_inst_0_synth_1 (confirmed applied - the properties were read back):
  #
  #   default synthesis         -> critical path 2.532 ns (395.0 MHz)
  #   PerfOptimized + retiming  -> critical path 2.547 ns (392.6 MHz)
  #
  # A 0.015 ns difference against a synthesis-draw spread of ~0.10 ns, so this is
  # neutral, not a result: one run per arm cannot resolve an effect that small.
  # Separating them would need roughly three builds per arm. It showed no sign of
  # helping, so the default is kept rather than shipping a non-default setting on
  # no evidence.
  #
  # Also tried and found to be silent no-ops - all three reported success while
  # binding to nothing, each costing a full build to discover:
  #   - set_clock_uncertainty in a used_in_synthesis XDC: the HBM clock is made by
  #     the clock-wizard IP and does not exist during synthesis, so get_clocks
  #     matched nothing ([Project 1-498]); OOC runs do not read constrs_1 either.
  #   - MAX_FANOUT set at implementation on the register-slice control nets: it is
  #     a synthesis attribute, so opt_design ignored it (three arms returned WNS
  #     identical to six decimals).
  #   - an 'if' guard in an XDC: rejected with [Designutils 20-1307], then skipped.

  # Launch and wait
  launch_runs impl_1 -to_step write_bitstream -jobs $jobs
  wait_on_run impl_1
  open_run impl_1

  # Drop the artificial setup pessimism before sign-off.
  #
  # constraints/impl.xdc adds setup uncertainty so that place and route optimise
  # against a 2.500 ns period while the hardware clock is slower. That is a
  # TOOL-EFFORT device, not a property of the delivered design: leaving it in
  # makes the design report a large negative WNS even when it comfortably meets
  # its real clock, which fails the installer's own timing gate
  # (require_static_shell_timing_or_confirm) and would have to be waved through
  # with --ignore-timing-failure - defeating a check that exists for good reason.
  #
  # Clearing it here means place/route still did the harder work, while the
  # report below - the one the gate reads, and the one a human reads - shows the
  # design's true margin against the clock it actually runs at. Vivado's own
  # inherent jitter/phase-error uncertainty is computed separately and still
  # applies, so this is not optimistic.
  set _sign_off_clk [get_clocks -quiet *clk_wizard_0_clk_out1*]
  set_clock_uncertainty -setup 0.000 $_sign_off_clk
  puts "INFO: \[slash\] cleared implementation-only setup uncertainty for sign-off"

  set timing_report_file [file join [file normalize [pwd]] "report_timing_${proj_name}.txt"]
  report_timing_summary -delay_type min_max -check_timing_verbose -max_paths 1 -input_pins -routable_nets -file $timing_report_file
  puts "TIMING REPORT: $timing_report_file"

  set impl_output_dir [get_property DIRECTORY [current_run]]
  write_abstract_shell -cell top_i/slash -force [file join $impl_output_dir "static_shell_slash.dcp"]

  puts "INFO: Implementation complete for run 'impl_1'."
}
