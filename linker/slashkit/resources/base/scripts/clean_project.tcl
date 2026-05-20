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

proc clean_project {{project_name "user"} {src_dir ""} {design_name "slash"}} {
  if {$src_dir eq ""} {
    set src_dir [file dirname [file normalize [info script]]]
  }

  set project_build_dir [file normalize [file join $src_dir ".." "build"]]
  set project_xpr [file normalize [file join $project_build_dir "${design_name}.xpr"]]

  set open_projects [get_projects -quiet $design_name]
  if {[llength $open_projects] == 0} {
    if {![file exists $project_xpr]} {
      error "Project file not found: $project_xpr"
    }
    puts "INFO: Opening project '$project_xpr' ..."
    open_project $project_xpr
  }

  set bd_service_dir [file normalize [file join $project_build_dir "slash.srcs" "sources_1" "bd" "service_layer_${project_name}"]]
  set bd_slash_dir [file normalize [file join $project_build_dir "slash.srcs" "sources_1" "bd" "slash_${project_name}"]]

  set impl_run [get_runs -quiet "${project_name}_impl_1"]
  set have_impl [expr {[llength $impl_run] > 0}]
  set have_bd_service [file exists $bd_service_dir]
  set have_bd_slash [file exists $bd_slash_dir]

  if {$have_impl || $have_bd_service || $have_bd_slash} {
    puts "Removing stale design for project '$project_name' ..."

    set bd_files {}
    set service_bd [file normalize [file join $bd_service_dir "service_layer_${project_name}.bd"]]
    if {[file exists $service_bd]} {
      lappend bd_files $service_bd
    }

    set slash_bd [file normalize [file join $bd_slash_dir "slash_${project_name}.bd"]]
    if {[file exists $slash_bd]} {
      lappend bd_files $slash_bd
    }

    if {[llength $bd_files] > 0} {
      remove_files $bd_files
    }

    if {[file exists $bd_service_dir]} {
      file delete -force $bd_service_dir
    }

    set gen_service_dir [file normalize [file join $project_build_dir "slash.gen" "sources_1" "bd" "service_layer_${project_name}"]]
    if {[file exists $gen_service_dir]} {
      file delete -force $gen_service_dir
    }

    if {[file exists $bd_slash_dir]} {
      file delete -force $bd_slash_dir
    }

    set gen_slash_dir [file normalize [file join $project_build_dir "slash.gen" "sources_1" "bd" "slash_${project_name}"]]
    if {[file exists $gen_slash_dir]} {
      file delete -force $gen_slash_dir
    }

    if {$have_impl} {
      delete_runs "${project_name}_impl_1"
    }
  } else {
    puts "INFO: No stale design artifacts found for project '$project_name'."
  }
}

if {[info exists ::argv0] && [file normalize [info script]] eq [file normalize $::argv0]} {
  if {[llength $argv] < 1} {
    puts "INFO: No project_name provided via -tclargs; defaulting to 'user'."
    set project_name "user"
  } else {
    set project_name [lindex $argv 0]
  }
  set src_dir ""
  if {[llength $argv] >= 2} {
    set src_dir [lindex $argv 1]
  }
  set design_name "slash"
  if {[llength $argv] >= 3} {
    set design_name [lindex $argv 2]
  }
  clean_project $project_name $src_dir $design_name
}
