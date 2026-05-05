# (c) Copyright 2024, Advanced Micro Devices, Inc.
# 
# Permission is hereby granted, free of charge, to any person obtaining a 
# copy of this software and associated documentation files (the "Software"), 
# to deal in the Software without restriction, including without limitation 
# the rights to use, copy, modify, merge, publish, distribute, sublicense, 
# and/or sell copies of the Software, and to permit persons to whom the 
# Software is furnished to do so, subject to the following conditions:
# 
# The above copyright notice and this permission notice shall be included in 
# all copies or substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR 
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, 
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL 
# THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER 
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING 
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER 
# DEALINGS IN THE SOFTWARE.
############################################################

# Set pdi properties to accelerate device download
set_property bitstream.general.npiDmaMode Yes [current_design]
set_property bitstream.general.compress true [current_design]
# set_param place.runBufgInsertion false
# set_param place.runBufgInsertionVersal false

set_property LOC GTM_QUAD_X0Y10 [get_cells top_i/service_layer/qsfp_2_n_3/DCMAC_subsys/dcmac_gt1_wrapper/gt0_quad/inst/quad_inst ]
set_property LOC GTM_QUAD_X1Y7 [get_cells top_i/service_layer/qsfp_0_n_1/DCMAC_subsys/dcmac_gt0_wrapper/gt0_quad/inst/quad_inst ]
set_property -dict { PACKAGE_PIN AR51 } [get_ports "qsfp0_322mhz_clk_p"]
set_property -dict { PACKAGE_PIN AL17 } [get_ports "qsfp2_322mhz_clk_p"]