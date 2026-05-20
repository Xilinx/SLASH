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

set sim_root "{{ sim_root }}"
set sim_prj_dir "{{ sim_prj_dir }}"
set ip_repo_path "{{ ip_repo_path }}"
set sim_mem_path "{{ sim_mem_path }}"
set bd_name "{{ bd_name }}"
set part "{{ part }}"

create_project sim_prj "${sim_prj_dir}" -part ${part} -force

add_files -norecurse ${sim_mem_path}
update_compile_order -fileset sources_1
update_compile_order -fileset sim_1

set_property ip_repo_paths "${ip_repo_path}" [current_project]
update_ip_catalog
update_compile_order -fileset sources_1
update_compile_order -fileset sim_1

# --- Preprocess checkpoint-backed kernels into funcsim Verilog (per instance) ---
{% if sim_checkpoint_netlists %}
set slash_ckpt_vlog_files [list]
file mkdir [file join ${sim_root} checkpoint_funcsim]
{% for ck in sim_checkpoint_netlists %}
if {![file exists "{{ ck.dcp_path }}"]} {
  error "Simulation checkpoint DCP not found for instance {{ ck.inst }}: {{ ck.dcp_path }}"
}
open_checkpoint "{{ ck.dcp_path }}"
write_verilog -force -mode funcsim -rename_top {{ ck.rename_top }} -prefix {{ ck.rename_prefix }} "{{ ck.funcsim_v_path }}"
close_design
set ckpt_funcsim_file "{{ ck.funcsim_v_path }}"
lappend slash_ckpt_vlog_files ${ckpt_funcsim_file}
add_files -fileset sim_1 -norecurse ${ckpt_funcsim_file}
set ckpt_funcsim_obj [get_files -all ${ckpt_funcsim_file}]
if {[llength ${ckpt_funcsim_obj}] > 0} {
  # Ensure checkpoint-derived netlists participate in simulation only.
  catch { set_property USED_IN_SIMULATION true ${ckpt_funcsim_obj} }
  catch { set_property USED_IN_SYNTHESIS false ${ckpt_funcsim_obj} }
  catch { set_property USED_IN_IMPLEMENTATION false ${ckpt_funcsim_obj} }
  catch { set_property IS_USER_DISABLED false ${ckpt_funcsim_obj} }
  catch { set_property IS_ENABLED true ${ckpt_funcsim_obj} }
}
{% endfor %}
update_compile_order -fileset sources_1
update_compile_order -fileset sim_1
{% endif %}
create_bd_design ${bd_name}
current_bd_design ${bd_name}

create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 s_axi_ctrl
set_property -dict [list CONFIG.ADDR_WIDTH 64] [get_bd_intf_ports s_axi_ctrl]
set_property -dict [list CONFIG.HAS_BURST 0 CONFIG.HAS_CACHE 0 CONFIG.HAS_LOCK 0 CONFIG.HAS_PROT 0 CONFIG.HAS_QOS 0 CONFIG.HAS_REGION 0] [get_bd_intf_ports s_axi_ctrl]

create_bd_port -dir I -type clk clk
create_bd_port -dir I -type rst rst
create_bd_intf_port -mode Slave -vlnv xilinx.com:interface:aximm_rtl:1.0 mem
set_property -dict [list CONFIG.ADDR_WIDTH 64] [get_bd_intf_ports mem]
set_property -dict [list CONFIG.READ_WRITE_MODE READ_WRITE] [get_bd_intf_ports mem]
set_property -dict [list CONFIG.DATA_WIDTH 64] [get_bd_intf_ports mem]

create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 bram_ctrl
set_property -dict [list CONFIG.SINGLE_PORT_BRAM {0} CONFIG.DATA_WIDTH {64} CONFIG.ECC_TYPE {0} CONFIG.READ_LATENCY {50}] [get_bd_cells bram_ctrl]

create_bd_cell -type module -reference sim_mem sim_mem_0

connect_bd_intf_net [get_bd_intf_pins bram_ctrl/BRAM_PORTA] [get_bd_intf_pins sim_mem_0/MEM_PORT_A]
connect_bd_intf_net [get_bd_intf_pins bram_ctrl/BRAM_PORTB] [get_bd_intf_pins sim_mem_0/MEM_PORT_B]

create_bd_cell -type ip -vlnv xilinx.com:ip:axi_bram_ctrl:4.1 bram_ctrl_ddr
set_property -dict [list CONFIG.SINGLE_PORT_BRAM {0} CONFIG.DATA_WIDTH {64} CONFIG.ECC_TYPE {0} CONFIG.READ_LATENCY {50}] [get_bd_cells bram_ctrl_ddr]

create_bd_cell -type module -reference sim_mem sim_mem_1

connect_bd_intf_net [get_bd_intf_pins bram_ctrl_ddr/BRAM_PORTA] [get_bd_intf_pins sim_mem_1/MEM_PORT_A]
connect_bd_intf_net [get_bd_intf_pins bram_ctrl_ddr/BRAM_PORTB] [get_bd_intf_pins sim_mem_1/MEM_PORT_B]

# --- Kernel IPs ---
{% for k in kernels %}
# set custom kernel: {{ k.vlnv }}
set {{ k.name }} [ create_bd_cell -type ip -vlnv {{ k.vlnv }} {{ k.name }} ]
{% endfor %}

# --- AXI-Lite SmartConnect fanout tree ---
{% for sc in axilite_scs %}
set {{ sc.name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect {{ sc.name }} ]
set_property -dict [list \
  CONFIG.NUM_SI {1} \
  CONFIG.NUM_MI {{ "{" ~ sc.num_mi ~ "}" }} \
] [get_bd_cells {{ sc.name }}]
connect_bd_net [get_bd_ports clk] [get_bd_pins {{ sc.name }}/aclk]
connect_bd_net [get_bd_ports rst] [get_bd_pins {{ sc.name }}/aresetn]
{% endfor %}

{% for sc in axilite_scs %}
{% if sc.si_from.type == "bd_port" %}
connect_bd_intf_net [get_bd_intf_ports {{ sc.si_from.name }}] [get_bd_intf_pins {{ sc.name }}/S00_AXI]
{% else %}
connect_bd_intf_net [get_bd_intf_pins {{ sc.si_from.prev }}/{{ sc.si_from.prev_slot_name }}] [get_bd_intf_pins {{ sc.name }}/S00_AXI]
{% endif %}
{% for mi in sc.mi %}
connect_bd_intf_net [get_bd_intf_pins {{ sc.name }}/{{ mi.slot_name }}] [get_bd_intf_pins {{ mi.dst_pin }}]
{% endfor %}
{% endfor %}

# --- Unified Memory SmartConnect root (mem port + kernel masters -> memories) ---
set mem_sc [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect mem_sc ]
set_property -dict [list \
  CONFIG.NUM_SI {{ "{" ~ mem_sc_num_si ~ "}" }} \
  CONFIG.NUM_MI {2} \
] [get_bd_cells mem_sc]
connect_bd_net [get_bd_ports clk] [get_bd_pins mem_sc/aclk]
connect_bd_net [get_bd_ports rst] [get_bd_pins mem_sc/aresetn]

connect_bd_intf_net [get_bd_intf_pins mem_sc/M00_AXI] [get_bd_intf_pins bram_ctrl/S_AXI]
connect_bd_intf_net [get_bd_intf_pins mem_sc/S00_AXI] [get_bd_intf_ports mem]
connect_bd_intf_net [get_bd_intf_pins mem_sc/M01_AXI] [get_bd_intf_pins bram_ctrl_ddr/S_AXI]
connect_bd_net [get_bd_ports clk] [get_bd_pins bram_ctrl/s_axi_aclk]
connect_bd_net [get_bd_ports rst] [get_bd_pins bram_ctrl/s_axi_aresetn]
connect_bd_net [get_bd_ports clk] [get_bd_pins bram_ctrl_ddr/s_axi_aclk]
connect_bd_net [get_bd_ports rst] [get_bd_pins bram_ctrl_ddr/s_axi_aresetn]

# --- AXI-Full reduction tree (fan-in) ---
{% for n in mem_reduce_nodes %}
set {{ n.name }} [ create_bd_cell -type ip -vlnv xilinx.com:ip:smartconnect {{ n.name }} ]
set_property -dict [list \
  CONFIG.NUM_SI {{ "{" ~ n.num_si ~ "}" }} \
  CONFIG.NUM_MI {1} \
] [get_bd_cells {{ n.name }}]
connect_bd_net [get_bd_ports clk] [get_bd_pins {{ n.name }}/aclk]
connect_bd_net [get_bd_ports rst] [get_bd_pins {{ n.name }}/aresetn]
{% for si in n.si %}
connect_bd_intf_net [get_bd_intf_pins {{ n.name }}/{{ si.slot_name }}] [get_bd_intf_pins {{ si.src }}]
{% endfor %}
{% endfor %}

{% for root in mem_roots %}
connect_bd_intf_net [get_bd_intf_pins mem_sc/{{ root.slot_name }}] [get_bd_intf_pins {{ root.src_pin }}]
{% endfor %}

# --- Clocks / resets to kernels ---
{% for p in clock_ports %}
connect_bd_net [get_bd_ports clk] [get_bd_pins {{ p }}]
{% endfor %}
{% for p in reset_ports %}
connect_bd_net [get_bd_ports rst] [get_bd_pins {{ p }}]
{% endfor %}

# --- AXIS streams ---
{% for s in axis_streams %}
connect_bd_intf_net -intf_net {{ s.net_name }} [get_bd_intf_pins {{ s.src_pin }}] [get_bd_intf_pins {{ s.dst_pin }}]
{% endfor %}

# --- AXI-Lite address assignment ---
{% for a in axilite_addr %}
assign_bd_address -offset {{ a.offset_hex }} -range {{ a.range_hex }} [get_bd_addr_segs {{ a.inst }}/{{ a.busif }}/{{ a.segment }}] -force
{% endfor %}

assign_bd_address -offset 0x4000000000 -range 128M [get_bd_addr_segs /bram_ctrl/S_AXI/Mem0] -force
assign_bd_address -offset 0x60000000000 -range 128M [get_bd_addr_segs /bram_ctrl_ddr/S_AXI/Mem0] -force
save_bd_design
validate_bd_design
add_files -norecurse [make_wrapper -files [get_files "${bd_name}.bd"] -top]
set_property top top_wrapper [current_fileset]
update_compile_order -fileset sources_1
set_property top top_wrapper [get_filesets sim_1]
set_property top_lib xil_defaultlib [get_filesets sim_1]
update_compile_order -fileset sim_1
set_property -name {xsim.elaborate.xelab.more_options} -value {-dll} -objects [get_filesets sim_1]
set_property generate_scripts_only 1 [current_fileset -simset]
launch_simulation -scripts_only
{% if sim_checkpoint_netlists %}
# Vivado may mark checkpoint-derived netlists as AutoDisabled in the project file.
# Ensure they are present in the generated XSIM compile project.
set vlog_prj [file join ${sim_prj_dir} "sim_prj.sim" "sim_1" "behav" "xsim" "top_wrapper_vlog.prj"]
if {[file exists ${vlog_prj}]} {
  set fh [open ${vlog_prj} r]
  set prj_data [read ${fh}]
  close ${fh}

  set missing_ckpt [list]
  foreach f ${slash_ckpt_vlog_files} {
    if {[string first ${f} ${prj_data}] < 0} {
      lappend missing_ckpt ${f}
    }
  }

  if {[llength ${missing_ckpt}] > 0} {
    set insert_block "\n# SLASH checkpoint-backed kernel netlists\n"
    foreach f ${missing_ckpt} {
      append insert_block "verilog xil_defaultlib \"${f}\"\n"
    }

    set marker "# compile glbl module"
    set idx [string first ${marker} ${prj_data}]
    if {$idx >= 0} {
      set new_data "[string range ${prj_data} 0 [expr {$idx - 1}]]${insert_block}[string range ${prj_data} ${idx} end]"
    } else {
      set new_data "${prj_data}${insert_block}"
    }

    set fh [open ${vlog_prj} w]
    puts -nonewline ${fh} ${new_data}
    close ${fh}
  }
}
{% endif %}
close_project
exit
