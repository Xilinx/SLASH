#!/bin/bash
# SPDX-License-Identifier: MIT
# Functional test of rdma_service (payload paths, registers) in xsim.
#   [SEED=<n>] run_tb_rdma_service.sh <work dir> [N=<requests>]
# Needs Vivado on PATH. PASS / FAIL on the last line of <work dir>/xsim.log.
set -e
here=$(cd "$(dirname "$0")" && pwd)
W=$(mkdir -p "${1:?work dir}" && cd "$1" && pwd)
shift
cd "$W"
xvlog -sv "$here"/../hdl/*.sv "$here/tb_rdma_service.sv" > xvlog.log
xvlog "$here"/../hdl/*.v "$here"/../hdl/user/*.v "$XILINX_VIVADO/data/verilog/src/glbl.v" >> xvlog.log
xelab -debug typical tb_rdma_service glbl -L xpm -s tb > xelab.log
xsim tb -R -sv_seed "${SEED:-1}" $(for a in "${@:-N=400}"; do echo --testplusarg "$a"; done) > xsim.log 2>&1 || true
tail -1 xsim.log
