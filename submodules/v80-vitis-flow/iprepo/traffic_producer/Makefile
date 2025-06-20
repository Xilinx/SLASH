# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

all: ip/*.zip

ip/*.zip: *.cpp
	v++ -c --mode hls --config hls_config.cfg --work_dir ip
	vitis-run --mode hls --package --config hls_config.cfg --work_dir ip

clean:
	rm -rf ip/ .Xil *.json