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

# Usage:
#   vivado -mode batch -source $script -tclargs [project_name] [iprepos] [action] [jobs]
# 
# Arguments:
#   project_name   Name of the Vivado project.
#                  Default: user
# 
#   iprepos        Path to the IP repository directory.
# 
#   action         What to do. One of:
#                    create  – create the project only
#                    build   – run synthesis and implementation only
#                    all     – create then build  (default)
# 
#   jobs           Number of parallel jobs for implementation.
#                  Default: 14

set src_dir [file dirname [file normalize [info script]]]
set cwd     [pwd]
set default_iprepos [file normalize [file join $src_dir ".." "iprepo"]]

set project_name "user"
set iprepos $default_iprepos
set action "all"
set jobs "14"

if {[llength $argv] > 0} {
  set project_name [lindex $argv 0]
  set remaining_args [lrange $argv 1 end]

  if {[llength $remaining_args] > 0} {
    set arg [lindex $remaining_args end]
    if {[string is integer -strict $arg]} {
      set jobs $arg
      set remaining_args [lrange $remaining_args 0 end-1]
    }
  }

  if {[llength $remaining_args] > 0} {
    set arg [lindex $remaining_args end]
    if {[lsearch -exact {create build all} $arg] >= 0} {
      set action $arg
      set remaining_args [lrange $remaining_args 0 end-1]
    }
  }

  if {[llength $remaining_args] > 0} {
    set iprepos [lindex $remaining_args end]
    set remaining_args [lrange $remaining_args 0 end-1]
  }

  if {[llength $remaining_args] > 0} {
    error "Too many arguments provided via -tclargs: $remaining_args"
  }
} else {
  puts "INFO: No project_name provided via -tclargs; defaulting to '$project_name'."
}

set do_create 0
set do_build 0
switch -exact -- $action {
  "create" { set do_create 1 }
  "build"  { set do_build 1 }
  "all"    { set do_create 1; set do_build 1 }
  default  { error "Unknown action '$action'. Expected: create, build, or all." }
}

# Design/BD names
set design_name "slash"
set bd_slash_name        "slash_${project_name}"
set bd_service_layer_name "service_layer_${project_name}"

puts "PROJECT:        $project_name"
puts "IP REPOS:       $iprepos"
puts "ACTION:         $action"
puts "BUILD DIR:      $cwd"

proc safe_source {tcl_path} {
  puts "INFO: Sourcing $tcl_path ..."
  catch {source $tcl_path} result
  if {[string is integer -strict $result] && $result != 0} {
    puts "EXIT: '$tcl_path' returned $result"
    exit 1
  }
}

set proj_exists [file normalize [file join $cwd "${design_name}.xpr"]]
if {![file exists $proj_exists]} {
  if {!$do_create} {
    error "Project not found at $proj_exists. Run with action 'create' first."
  }
  if {[lsearch -exact $iprepos $default_iprepos] == -1} {
    lappend iprepos $default_iprepos
  }
  puts "INFO: Creating new project '$design_name' in '$cwd' ..."
  create_project $design_name $cwd -part xcv80-lsva4737-2MHP-e-S -force
  set_property ip_repo_paths $iprepos [current_project]
  update_ip_catalog

  # Base shell / containers
  safe_source [file normalize [file join $src_dir "slash_base.tcl"]]
  safe_source [file normalize [file join $src_dir "service_layer.tcl"]]
  safe_source [file normalize [file join $src_dir "top.tcl"]]
  safe_source [file normalize [file join $src_dir "enable_dfx_bdc.tcl"]]

  # Wrapper / XDC / build
  safe_source [file normalize [file join $src_dir "make_wrapper.tcl"]]
  safe_source [file normalize [file join $src_dir "add_constraints.tcl"]]
} else {
  puts "INFO: Project already exists; opening '$proj_exists'."
  open_project [file normalize [file join $cwd "slash.xpr"]]
  if {$do_create} {
    set repos [get_property ip_repo_paths [current_project]]
    set iprepos [concat $iprepos $repos]
    set_property ip_repo_paths $iprepos [current_project]
    update_ip_catalog
    puts "INFO: Project already exists; create step is a no-op for base-image flow."
  }
}

if {$do_build} {
  safe_source [file normalize [file join $src_dir "build_project.tcl"]]
  build_project $project_name $jobs
  puts "INFO: Project build complete."
} elseif {$do_create} {
  puts "INFO: Project creation complete (build skipped)."
} else {
  puts "INFO: Build skipped."
}
