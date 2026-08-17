# Throwaway build script for the LSF plumbing test. Deliberately mirrors the
# argument shape slashkit uses: -tclargs <project_name> <iprepo> <action> <jobs>
set src_dir [file dirname [file normalize [info script]]]

set project_name [lindex $argv 0]
set iprepo       [lindex $argv 1]
set action       [lindex $argv 2]
set jobs         [lindex $argv 3]

puts "DUMMY: project_name = $project_name"
puts "DUMMY: iprepo       = $iprepo"
puts "DUMMY: action       = $action"
puts "DUMMY: jobs         = $jobs"
puts "DUMMY: host         = [exec hostname]"
puts "DUMMY: pwd          = [pwd]"

create_project $project_name [pwd]/$project_name -part xcv80-lsva4737-2MHP-e-S -force
add_files [file join $src_dir dummy.v]
set_property top dummy [current_fileset]

puts "DUMMY: starting synthesis at [clock format [clock seconds]]"
synth_design -top dummy -part xcv80-lsva4737-2MHP-e-S
puts "DUMMY: synthesis done at [clock format [clock seconds]]"

report_utilization -file [pwd]/$project_name/report_utilization_$project_name.txt
write_checkpoint -force [pwd]/$project_name/${project_name}_synth.dcp
puts "DUMMY: build complete"
