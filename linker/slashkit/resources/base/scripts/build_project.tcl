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

proc build_project {{proj_name "user"}} {
  puts "INFO: Using proj_name='$proj_name'"

  # Ensure top BD is generated
  generate_target all [get_files "top.bd"]

  # Static/base configuration
  create_pr_configuration -name config_1 \
    -partitions [list \
      top_i/slash:slash_base_inst_0 \
      top_i/service_layer:service_layer_inst_0 \
    ]

  # Parent impl run remains 'impl_1'
  set_property PR_CONFIGURATION config_1 [get_runs impl_1]
  set_property strategy Congestion_SSI_SpreadLogic_high [get_runs impl_1]
  set_property STEPS.OPT_DESIGN.TCL.POST         [get_files *opt.post.tcl]                [get_runs impl_1]
  set_property STEPS.PLACE_DESIGN.TCL.PRE        [get_files *place.pre.tcl]               [get_runs impl_1]
  set_property STEPS.WRITE_DEVICE_IMAGE.TCL.PRE  [get_files *write_device_image.pre.tcl]  [get_runs impl_1]

  # Launch and wait
  launch_runs impl_1 -to_step write_bitstream -jobs 14
  wait_on_run impl_1
  open_run impl_1
  
  set impl_output_dir [get_property DIRECTORY [current_run]]
  write_abstract_shell -cell top_i/slash -force [file join $impl_output_dir "static_shell_slash.dcp"]
  write_abstract_shell -cell top_i/service_layer -force [file join $impl_output_dir "static_shell_service_layer.dcp"]

  puts "INFO: Implementation complete for run 'impl_1'."
}
