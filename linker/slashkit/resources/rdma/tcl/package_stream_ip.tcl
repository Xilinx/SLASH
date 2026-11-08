# SPDX-License-Identifier: MIT
#
# Packages the user region's end of the RDMA service's stream (hdl/user:
# rdma_stream_tx, rdma_stream_rx) as IPs in ip/, which the linker copies into
# the user region's IP repository: the user region is a DFX reconfigurable
# module with manual compile order, where module references do not work.
#
#   vivado -mode batch -source tcl/package_stream_ip.tcl     (from resources/rdma)
#
# The packaged IPs are committed; run this again after changing hdl/user.

set here [file normalize [file join [file dirname [info script]] ..]]
set src [file join $here hdl user rdma_stream_user.v]
set part xcv80-lsva4737-2MHP-e-S

foreach top {rdma_stream_tx rdma_stream_rx} {
    set dst [file join $here ip $top]
    set work [file join [pwd] .pkg_$top]
    file delete -force $dst $work
    create_project -force -part $part pkg_$top $work
    add_files -norecurse $src
    set_property top $top [current_fileset]
    update_compile_order -fileset sources_1
    ipx::package_project -root_dir $dst -vendor slash -library rdma -taxonomy /SLASH \
        -import_files -set_current true
    set core [ipx::current_core]
    set_property name $top $core
    set_property display_name $top $core
    set_property version 1.0 $core
    set_property description "The user region's end of the RDMA service's stream (rdma.[string range $top end-1 end])" $core
    set_property supported_families {versal Production} $core
    ipx::create_xgui_files $core
    ipx::update_checksums $core
    ipx::check_integrity $core
    ipx::save_core $core
    puts "package_stream_ip: [get_property vlnv $core] in $dst, interfaces: [lsort [get_property name [ipx::get_bus_interfaces -of_objects $core]]]"
    close_project
    file delete -force $work
}
