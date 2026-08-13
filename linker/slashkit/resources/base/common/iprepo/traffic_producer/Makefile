# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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

# Set SLASH_TOOL_LAUNCHER to run the tools on a compute farm instead of here;
# see docs/howto/offload-builds-to-a-cluster.rst. Empty runs them locally.
# --work_dir is relative, so the launcher is told which directory to use.
LAUNCH := $(if $(SLASH_TOOL_LAUNCHER),env SLASH_BUILD_TASK=hls SLASH_TOOL_CWD=$(CURDIR) $(SLASH_TOOL_LAUNCHER))

all: ip/traffic_producer.zip

ip/traffic_producer.zip: $(wildcard *.cpp) $(wildcard *.cfg)
	$(LAUNCH) v++ -c --mode hls --config traffic_producer.cfg --work_dir ip
	$(LAUNCH) vitis-run --mode hls --package --config traffic_producer.cfg --work_dir ip

.PHONY: clean
clean:
	rm -rf ip/ .Xil *.json
