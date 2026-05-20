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

create_pblock pblock_slash
add_cells_to_pblock [get_pblocks pblock_slash] [get_cells -quiet [list top_i/slash]]
resize_pblock [get_pblocks pblock_slash] -add {SLICE_X28Y716:SLICE_X351Y879 SLICE_X48Y620:SLICE_X351Y715 SLICE_X244Y575:SLICE_X351Y619 SLICE_X84Y575:SLICE_X163Y619 SLICE_X244Y574:SLICE_X323Y574}
resize_pblock [get_pblocks pblock_slash] -add {BUFG_FABRIC_X4Y144:BUFG_FABRIC_X4Y239 BUFG_FABRIC_X3Y168:BUFG_FABRIC_X3Y239 BUFG_FABRIC_X0Y144:BUFG_FABRIC_X2Y239}
resize_pblock [get_pblocks pblock_slash] -add {BUFG_PS_X2Y48:BUFG_PS_X2Y59}
resize_pblock [get_pblocks pblock_slash] -add {DSP58_CPLX_X0Y310:DSP58_CPLX_X11Y439 DSP58_CPLX_X8Y287:DSP58_CPLX_X11Y309 DSP58_CPLX_X0Y287:DSP58_CPLX_X3Y309}
resize_pblock [get_pblocks pblock_slash] -add {DSP_X0Y310:DSP_X23Y439 DSP_X16Y287:DSP_X23Y309 DSP_X0Y287:DSP_X7Y309}
resize_pblock [get_pblocks pblock_slash] -add {IRI_QUAD_X18Y2892:IRI_QUAD_X229Y3547 IRI_QUAD_X29Y2508:IRI_QUAD_X229Y2891 IRI_QUAD_X65Y2328:IRI_QUAD_X83Y2507 IRI_QUAD_X33Y2328:IRI_QUAD_X48Y2507 IRI_QUAD_X65Y2324:IRI_QUAD_X80Y2327}
resize_pblock [get_pblocks pblock_slash] -add {NOC_NMU512_X0Y13:NOC_NMU512_X3Y17 NOC_NMU512_X3Y12:NOC_NMU512_X3Y12 NOC_NMU512_X0Y12:NOC_NMU512_X1Y12}
resize_pblock [get_pblocks pblock_slash] -add {NOC_NPS_VNOC_X0Y26:NOC_NPS_VNOC_X3Y36 NOC_NPS_VNOC_X3Y24:NOC_NPS_VNOC_X3Y25 NOC_NPS_VNOC_X0Y24:NOC_NPS_VNOC_X1Y25}
resize_pblock [get_pblocks pblock_slash] -add {NOC_NSU512_X0Y13:NOC_NSU512_X3Y18 NOC_NSU512_X3Y12:NOC_NSU512_X3Y12 NOC_NSU512_X0Y12:NOC_NSU512_X1Y12}
resize_pblock [get_pblocks pblock_slash] -add {RAMB18_X1Y312:RAMB18_X15Y441 RAMB18_X10Y288:RAMB18_X15Y311 RAMB18_X2Y288:RAMB18_X5Y311}
resize_pblock [get_pblocks pblock_slash] -add {RAMB36_X1Y156:RAMB36_X15Y220 RAMB36_X10Y144:RAMB36_X15Y155 RAMB36_X2Y144:RAMB36_X5Y155}
resize_pblock [get_pblocks pblock_slash] -add {URAM288_X0Y180:URAM288_X7Y220 URAM288_X1Y156:URAM288_X7Y179 URAM288_X6Y144:URAM288_X7Y155 URAM288_X2Y144:URAM288_X3Y155}
resize_pblock [get_pblocks pblock_slash] -add {URAM_CAS_DLY_X0Y8:URAM_CAS_DLY_X7Y8 URAM_CAS_DLY_X1Y7:URAM_CAS_DLY_X7Y7 URAM_CAS_DLY_X6Y6:URAM_CAS_DLY_X7Y6 URAM_CAS_DLY_X2Y6:URAM_CAS_DLY_X3Y6}
resize_pblock [get_pblocks pblock_slash] -add {CLOCKREGION_X4Y7:CLOCKREGION_X5Y7}
set_property SNAPPING_MODE ON [get_pblocks pblock_slash]
set_property IS_SOFT FALSE [get_pblocks pblock_slash]
create_pblock pblock_service_layer
add_cells_to_pblock [get_pblocks pblock_service_layer] [get_cells -quiet [list top_i/service_layer]]
resize_pblock [get_pblocks pblock_service_layer] -add {SLICE_X0Y525:SLICE_X27Y619 SLICE_X256Y522:SLICE_X363Y524 SLICE_X0Y428:SLICE_X163Y524 SLICE_X244Y428:SLICE_X363Y521 SLICE_X0Y284:SLICE_X363Y427 SLICE_X48Y236:SLICE_X363Y283}
resize_pblock [get_pblocks pblock_service_layer] -add {BUFGCE_X11Y0:BUFGCE_X12Y23}
resize_pblock [get_pblocks pblock_service_layer] -add {BUFG_FABRIC_X4Y48:BUFG_FABRIC_X4Y143 BUFG_FABRIC_X3Y48:BUFG_FABRIC_X3Y119 BUFG_FABRIC_X1Y48:BUFG_FABRIC_X2Y143 BUFG_FABRIC_X0Y72:BUFG_FABRIC_X0Y143}
resize_pblock [get_pblocks pblock_service_layer] -add {BUFG_GT_X0Y167:BUFG_GT_X1Y48}
resize_pblock [get_pblocks pblock_service_layer] -add {BUFG_GT_SYNC_X0Y286:BUFG_GT_SYNC_X1Y82}
resize_pblock [get_pblocks pblock_service_layer] -add {BUFG_PS_X1Y24:BUFG_PS_X1Y47}
resize_pblock [get_pblocks pblock_service_layer] -add {DCMAC_X0Y2:DCMAC_X1Y0}
resize_pblock [get_pblocks pblock_service_layer] -add {DPLL_X14Y6:DPLL_X14Y7 DPLL_X3Y8:DPLL_X3Y11 DPLL_X1Y7:DPLL_X1Y7 DPLL_X0Y10:DPLL_X0Y13}
resize_pblock [get_pblocks pblock_service_layer] -add {DSP58_CPLX_X8Y118:DSP58_CPLX_X11Y262 DSP58_CPLX_X4Y118:DSP58_CPLX_X7Y213 DSP58_CPLX_X0Y118:DSP58_CPLX_X3Y262}
resize_pblock [get_pblocks pblock_service_layer] -add {DSP_X16Y118:DSP_X23Y262 DSP_X8Y118:DSP_X15Y213 DSP_X0Y118:DSP_X7Y262}
resize_pblock [get_pblocks pblock_service_layer] -add {GTM_QUAD_X1Y7:GTM_QUAD_X1Y8 GTM_QUAD_X0Y9:GTM_QUAD_X0Y10}
resize_pblock [get_pblocks pblock_service_layer] -add {GTM_REFCLK_X1Y14:GTM_REFCLK_X1Y17 GTM_REFCLK_X0Y18:GTM_REFCLK_X0Y21}
resize_pblock [get_pblocks pblock_service_layer] -add {HSC_X0Y1:HSC_X0Y1}
resize_pblock [get_pblocks pblock_service_layer] -add {ILKNF_X0Y0:ILKNF_X0Y0}
resize_pblock [get_pblocks pblock_service_layer] -add {IRI_QUAD_X0Y2128:IRI_QUAD_X3Y2507 IRI_QUAD_X67Y2116:IRI_QUAD_X86Y2127 IRI_QUAD_X0Y1740:IRI_QUAD_X48Y2127 IRI_QUAD_X65Y1740:IRI_QUAD_X86Y2115 IRI_QUAD_X29Y1356:IRI_QUAD_X86Y1739 IRI_QUAD_X0Y1164:IRI_QUAD_X244Y1355 IRI_QUAD_X29Y972:IRI_QUAD_X244Y1163}
resize_pblock [get_pblocks pblock_service_layer] -add {MMCM_X11Y0:MMCM_X12Y0}
resize_pblock [get_pblocks pblock_service_layer] -add {MRMAC_X0Y3:MRMAC_X1Y1}
resize_pblock [get_pblocks pblock_service_layer] -add {NOC_NMU512_X3Y5:NOC_NMU512_X3Y10 NOC_NMU512_X2Y5:NOC_NMU512_X2Y8 NOC_NMU512_X0Y5:NOC_NMU512_X1Y10}
resize_pblock [get_pblocks pblock_service_layer] -add {NOC_NPS_VNOC_X3Y10:NOC_NPS_VNOC_X3Y21 NOC_NPS_VNOC_X2Y10:NOC_NPS_VNOC_X2Y17 NOC_NPS_VNOC_X0Y10:NOC_NPS_VNOC_X1Y21}
resize_pblock [get_pblocks pblock_service_layer] -add {NOC_NSU512_X3Y5:NOC_NSU512_X3Y10 NOC_NSU512_X2Y5:NOC_NSU512_X2Y8 NOC_NSU512_X0Y5:NOC_NSU512_X1Y10}
resize_pblock [get_pblocks pblock_service_layer] -add {RAMB18_X10Y120:RAMB18_X16Y265 RAMB18_X6Y120:RAMB18_X9Y215 RAMB18_X1Y120:RAMB18_X5Y265 RAMB18_X0Y144:RAMB18_X0Y311}
resize_pblock [get_pblocks pblock_service_layer] -add {RAMB36_X10Y60:RAMB36_X16Y132 RAMB36_X6Y60:RAMB36_X9Y107 RAMB36_X1Y60:RAMB36_X5Y132 RAMB36_X0Y72:RAMB36_X0Y155}
resize_pblock [get_pblocks pblock_service_layer] -add {URAM288_X6Y60:URAM288_X8Y132 URAM288_X4Y60:URAM288_X5Y107 URAM288_X1Y60:URAM288_X3Y132 URAM288_X0Y72:URAM288_X0Y132}
resize_pblock [get_pblocks pblock_service_layer] -add {URAM_CAS_DLY_X6Y2:URAM_CAS_DLY_X8Y5 URAM_CAS_DLY_X4Y2:URAM_CAS_DLY_X5Y4 URAM_CAS_DLY_X1Y2:URAM_CAS_DLY_X3Y5 URAM_CAS_DLY_X0Y3:URAM_CAS_DLY_X0Y5}
set_property SNAPPING_MODE ON [get_pblocks pblock_service_layer]
set_property IS_SOFT FALSE [get_pblocks pblock_service_layer]
set_property NOC_HIGH_ID_MAX 63 [get_pblocks pblock_service_layer]
set_property NOC_HIGH_ID_MIN 49 [get_pblocks pblock_service_layer]
set_property NOC_HIGH_ID_MAX 48 [get_pblocks pblock_slash]
set_property NOC_HIGH_ID_MIN 31 [get_pblocks pblock_slash]

 #set_false_path -reset_path -from [get_pins {top_i/static_region/clk_rst_shell/proc_sys_reset_0/U0/ACTIVE_LOW_PR_OUT_DFF[0].FDRE_PER_N/C}]
 #set_false_path -reset_path -from [get_pins {top_i/static_region/clk_rst_shell/proc_sys_reset_1/U0/ACTIVE_LOW_PR_OUT_DFF[0].FDRE_PER_N/C}]
