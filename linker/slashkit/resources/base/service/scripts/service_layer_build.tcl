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

proc _service_usage {} {
    return "Expected -tclargs: --project-name <name> --ip-repo <path> --static-shell-dcp <path> --base-bd <path> --opt-post-tcl <path> --linker-results-dir <path> --rm-work-dir <path> --artifact-out-dir <path> --jobs <n>"
}

proc _require_file {path label} {
    if {![file exists $path]} {
        error "Missing ${label}: $path"
    }
}

proc _require_dir {path label} {
    if {![file isdirectory $path]} {
        error "Missing ${label}: $path"
    }
}

array set opts {
    --project-name ""
    --ip-repo ""
    --static-shell-dcp ""
    --base-bd ""
    --opt-post-tcl ""
    --linker-results-dir ""
    --rm-work-dir ""
    --artifact-out-dir ""
    --jobs 8
}

set idx 0
while {$idx < [llength $argv]} {
    set key [lindex $argv $idx]
    if {![info exists opts($key)]} {
        error "Unknown argument '$key'. [_service_usage]"
    }
    incr idx
    if {$idx >= [llength $argv]} {
        error "Missing value for '$key'. [_service_usage]"
    }
    set opts($key) [lindex $argv $idx]
    incr idx
}

foreach req {--project-name --ip-repo --static-shell-dcp --base-bd --opt-post-tcl --linker-results-dir --rm-work-dir --artifact-out-dir} {
    if {$opts($req) eq ""} {
        error "Missing required argument '$req'. [_service_usage]"
    }
}

set proj_name $opts(--project-name)
set ip_repo [file normalize $opts(--ip-repo)]
set static_shell_dcp [file normalize $opts(--static-shell-dcp)]
set base_bd [file normalize $opts(--base-bd)]
set opt_post_tcl [file normalize $opts(--opt-post-tcl)]
set linker_results_dir [file normalize $opts(--linker-results-dir)]
set rm_work_dir $opts(--rm-work-dir)
set artifact_out_dir $opts(--artifact-out-dir)
set jobs $opts(--jobs)

file mkdir $rm_work_dir
file mkdir $artifact_out_dir
set rm_work_dir [file normalize $rm_work_dir]
set artifact_out_dir [file normalize $artifact_out_dir]

set generated_bd_tcl [file join $linker_results_dir "service_layer.tcl"]

_require_dir $ip_repo "IP repository directory"
_require_file $static_shell_dcp "static shell DCP"
_require_file $base_bd "installed service_layer BD"
_require_file $generated_bd_tcl "generated service_layer BD Tcl"
_require_file $opt_post_tcl "service_layer eth opt.post Tcl"

puts "PROJECT NAME:      $proj_name"
puts "IP REPO:           $ip_repo"
puts "LINKER RESULTS:    $linker_results_dir"
puts "RM WORK DIR:       $rm_work_dir"
puts "ARTIFACT OUT DIR:  $artifact_out_dir"
puts "JOBS:              $jobs"
puts "OPT POST HOOK:     $opt_post_tcl"

set rm_proj_name "service_layer_${proj_name}"
set rm_name "${rm_proj_name}_rm"

create_project $rm_proj_name $rm_work_dir -part xcv80-lsva4737-2MHP-e-S -force

set_property ip_repo_paths [list $ip_repo] [current_project]
update_ip_catalog
add_files $static_shell_dcp
import_files $base_bd
set_property PR_FLOW 1 [current_project]
set_property DESIGN_MODE GateLvl [current_fileset]
set_property top top_wrapper [current_fileset]
set_property source_mgmt_mode All [current_project]

create_partition_def -name $rm_proj_name -module service_layer
create_reconfig_module -name $rm_name -partition_def [get_partition_defs $rm_proj_name] -define_from service_layer

create_pr_configuration -name config_1 -partitions [list top_i/service_layer:$rm_name]
set_property USE_BLACKBOX 0 [get_pr_configuration config_1]
set_property PR_CONFIGURATION config_1 [get_runs impl_1]

add_files -fileset utils_1 -norecurse $opt_post_tcl
set opt_post_hook [lindex [get_files -of_objects [get_filesets utils_1] [list "*service_layer_eth.opt.post.tcl"]] 0]
if {$opt_post_hook eq ""} {
    error "Failed to import service-layer opt.post hook into utils_1: $opt_post_tcl"
}


set imported_bd [file join $rm_work_dir "${rm_proj_name}.srcs" "sources_1" "bd" "service_layer" "service_layer.bd"]
open_bd_design $imported_bd
foreach p [get_bd_intf_ports] {
    set_property HDL_ATTRIBUTE.LOCKED {TRUE} $p
}
source $generated_bd_tcl

launch_runs "${rm_name}_synth_1" -jobs $jobs
wait_on_run "${rm_name}_synth_1"

set rm_synth_dcp [file join $rm_work_dir "${rm_proj_name}.runs" "${rm_name}_synth_1" "service_layer.dcp"]
add_files $rm_synth_dcp
set_property SCOPED_TO_CELLS {top_i/service_layer} [get_files $rm_synth_dcp]
set_property strategy Congestion_SSI_SpreadLogic_high [get_runs impl_1]
set_property STEPS.OPT_DESIGN.TCL.POST $opt_post_hook [get_runs impl_1]
puts "Attached impl_1 post-opt hook: $opt_post_hook"

launch_runs impl_1 -jobs $jobs
wait_on_run impl_1
open_run impl_1

set partial_pdi [file join $artifact_out_dir "top_i_service_layer_service_layer_${proj_name}_inst_0_partial.pdi"]
write_device_image -cell top_i/service_layer -force $partial_pdi
