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

set src_dir [file dirname [file normalize [info script]]]
set cwd     [pwd]

# Shell build-ID constants injected into the static-region GPIO register
# (top.tcl reads these globals). Set from the git commit at build time via
# SLASH_BUILD_ID_LO/HI; default to 0 so manual/interactive runs still work.
set ::build_id_lo [expr {[info exists ::env(SLASH_BUILD_ID_LO)] ? $::env(SLASH_BUILD_ID_LO) : 0}]
set ::build_id_hi [expr {[info exists ::env(SLASH_BUILD_ID_HI)] ? $::env(SLASH_BUILD_ID_HI) : 0}]

# Bit [28] of the high word identifies the shell variant (0 = service). Forced
# here rather than trusted from the environment so the register still
# identifies this shell correctly when the script is sourced standalone.
set ::build_id_hi [format 0x%08x [expr {$::build_id_hi & ~0x10000000}]]

# Optional parallel job count via -tclargs: an integer in the trailing
# position. Popped off first so the positional parsing below is unaffected by
# whether the caller passed it.
set jobs "14"
set args $argv
if {[llength $args] > 0} {
  set last_arg [lindex $args end]
  if {[string is integer -strict $last_arg]} {
    set jobs $last_arg
    set args [lrange $args 0 end-1]
  }
}

if {[llength $args] > 0} {
  set project_name [lindex $args 0]
}

# Optional IP repository path(s) via -tclargs; defaults to ../../common/iprepo
set default_iprepos [file normalize [file join $src_dir ".." ".." "common" "iprepo"]]
set iprepos $default_iprepos

# Optional action via -tclargs: create | build | all (default: all)
set action "all"

if {[llength $args] >= 2} {
  set arg1 [lindex $args 1]
  if {[llength $args] == 2} {
    if {[lsearch -exact {create build all} $arg1] >= 0} {
      set action $arg1
    } else {
      set iprepos $arg1
    }
  } else {
    set iprepos $arg1
  }
}

if {[llength $args] >= 3} {
  set action [lindex $args 2]
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
  set result [ source $tcl_path ]
  puts "INFO: Finished sourcing $tcl_path"

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
  lappend iprepos $default_iprepos
  puts "INFO: Creating new project '$design_name' in '$cwd' ..."
  create_project $design_name $cwd -part xcv80-lsva4737-2MHP-e-S -force
  set_property ip_repo_paths $iprepos [current_project]
  update_ip_catalog

  # Base shell / containers
  source [file normalize [file join $src_dir "slash_base.tcl"]]
  source [file normalize [file join $src_dir "service_layer.tcl"]]
  source [file normalize [file join $src_dir "top.tcl"]]
  source [file normalize [file join $src_dir "enable_dfx_bdc.tcl"]]

  # Wrapper / XDC / build
  source [file normalize [file join $src_dir ".." ".." "common" "scripts" "make_wrapper.tcl"]]
  source [file normalize [file join $src_dir "add_constraints.tcl"]]
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
  source [file normalize [file join $src_dir "build_project.tcl"]]
  build_project $project_name $jobs
  puts "INFO: Project build complete."
} elseif {$do_create} {
  puts "INFO: Project creation complete (build skipped)."
} else {
  puts "INFO: Build skipped."
}
