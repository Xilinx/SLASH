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

proc _slash_usage {} {
    return "Expected -tclargs: --project-name <name> --ip-repo <path> --static-shell-dcp <path> --base-bd <path> --linker-results-dir <path> --rm-work-dir <path> --artifact-out-dir <path> --util-report-file <path> --jobs <n> --pre-synth-tcl <path> ..."
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
    --linker-results-dir ""
    --rm-work-dir ""
    --artifact-out-dir ""
    --util-report-file ""
    --jobs 8
}

set pre_synth_tcls [list]

set idx 0
while {$idx < [llength $argv]} {
    set key [lindex $argv $idx]
    if {$key eq "--pre-synth-tcl"} {
        incr idx
        if {$idx >= [llength $argv]} {
            error "Missing value for '$key'. [_slash_usage]"
        }
        set pre_synth_tcl [lindex $argv $idx]
        if {$pre_synth_tcl eq ""} {
            error "Empty value for '$key'. [_slash_usage]"
        }
        lappend pre_synth_tcls [file normalize $pre_synth_tcl]
        incr idx
        continue
    }
    if {![info exists opts($key)]} {
        error "Unknown argument '$key'. [_slash_usage]"
    }
    incr idx
    if {$idx >= [llength $argv]} {
        error "Missing value for '$key'. [_slash_usage]"
    }
    set opts($key) [lindex $argv $idx]
    incr idx
}

foreach req {--project-name --ip-repo --static-shell-dcp --base-bd --linker-results-dir --rm-work-dir --artifact-out-dir --util-report-file} {
    if {$opts($req) eq ""} {
        error "Missing required argument '$req'. [_slash_usage]"
    }
}

set proj_name $opts(--project-name)
set ip_repo [file normalize $opts(--ip-repo)]
set static_shell_dcp [file normalize $opts(--static-shell-dcp)]
set base_bd [file normalize $opts(--base-bd)]
set linker_results_dir [file normalize $opts(--linker-results-dir)]
set rm_work_dir $opts(--rm-work-dir)
set artifact_out_dir $opts(--artifact-out-dir)
set util_report_file $opts(--util-report-file)
set jobs $opts(--jobs)

file mkdir $rm_work_dir
file mkdir $artifact_out_dir
file mkdir [file dirname $util_report_file]
set rm_work_dir [file normalize $rm_work_dir]
set artifact_out_dir [file normalize $artifact_out_dir]
set util_report_file [file normalize $util_report_file]
set timing_report_file [file join $rm_work_dir "report_timing_${proj_name}.txt"]
set ltx_file [file join $artifact_out_dir "top_i_slash_slash_${proj_name}_inst_0_hw_probes.ltx"]

set generated_bd_tcl [file join $linker_results_dir "slash.tcl"]

_require_file $ip_repo "IP repository directory"
_require_file $static_shell_dcp "static shell DCP"
_require_file $base_bd "installed slash_base BD"
_require_file $generated_bd_tcl "generated slash BD Tcl"
foreach pre_synth_tcl $pre_synth_tcls {
    _require_file $pre_synth_tcl "pre-synth Tcl"
}

puts "PROJECT NAME:      $proj_name"
puts "IP REPO:           $ip_repo"
puts "LINKER RESULTS:    $linker_results_dir"
puts "RM WORK DIR:       $rm_work_dir"
puts "ARTIFACT OUT DIR:  $artifact_out_dir"
puts "UTIL REPORT FILE:  $util_report_file"
puts "TIMING REPORT:     $timing_report_file"
puts "HW PROBES LTX:     $ltx_file"
puts "JOBS:              $jobs"
puts "PRE-SYNTH TCLS:    $pre_synth_tcls"

set slash_proj_name "slash_${proj_name}"
set slash_rm_name "${slash_proj_name}_rm"

create_project $slash_proj_name $rm_work_dir -part xcv80-lsva4737-2MHP-e-S -force

set_property ip_repo_paths [list $ip_repo] [current_project]
update_ip_catalog
add_files $static_shell_dcp
import_files $base_bd
set_property PR_FLOW 1 [current_project]
set_property DESIGN_MODE GateLvl [current_fileset]
set_property top top_wrapper [current_fileset]

create_partition_def -name $slash_proj_name -module slash_base
create_reconfig_module -name $slash_rm_name -partition_def [get_partition_defs $slash_proj_name] -define_from slash_base

create_pr_configuration -name config_1 -partitions [list top_i/slash:$slash_rm_name]
set_property USE_BLACKBOX 0 [get_pr_configuration config_1]
set_property PR_CONFIGURATION config_1 [get_runs impl_1]

set imported_bd [file join $rm_work_dir "${slash_proj_name}.srcs" "sources_1" "bd" "slash_base" "slash_base.bd"]
open_bd_design $imported_bd
foreach p [get_bd_intf_ports] {
    set_property HDL_ATTRIBUTE.LOCKED {TRUE} $p
}
source $generated_bd_tcl
foreach pre_synth_tcl $pre_synth_tcls {
    puts "Sourcing pre-synth Tcl: $pre_synth_tcl"
    source $pre_synth_tcl
}

launch_runs "${slash_rm_name}_synth_1" -jobs $jobs
wait_on_run "${slash_rm_name}_synth_1"

set rm_synth_dcp [file join $rm_work_dir "${slash_proj_name}.runs" "${slash_rm_name}_synth_1" "slash_base.dcp"]
add_files $rm_synth_dcp
set_property SCOPED_TO_CELLS {top_i/slash} [get_files $rm_synth_dcp]
set_property strategy Congestion_SSI_SpreadLogic_high [get_runs impl_1]

launch_runs impl_1 -jobs $jobs
wait_on_run impl_1
open_run impl_1

report_timing_summary -delay_type min_max -check_timing_verbose -max_paths 1 -input_pins -routable_nets -file $timing_report_file

set partial_pdi [file join $artifact_out_dir "top_i_slash_slash_${proj_name}_inst_0_partial.pdi"]
write_device_image -cell top_i/slash -force $partial_pdi
write_debug_probes -cell top_i/slash -force $ltx_file
report_utilization -hierarchical -hierarchical_percentages -file $util_report_file
