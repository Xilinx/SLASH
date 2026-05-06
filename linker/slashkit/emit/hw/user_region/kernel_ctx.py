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

from __future__ import annotations
from collections import OrderedDict
from typing import Dict
from slashkit.core.kernel import KernelInstance
from slashkit.core.port import BusType


def build_kernel_add_context(instances: Dict[str, KernelInstance]) -> dict:
    """
    Context for your Jinja template:
      - instances: OrderedDict[name -> KernelInstance]
      - clocks:    [{"src_pin": "<inst>/<pin>"} ...]
    """
    ordered = OrderedDict((name, instances[name])
                          for name in sorted(instances.keys()))

    clocks = []
    resets = []
    for name, inst in ordered.items():
        for p in inst.kernel.ports_of_type(BusType.CLOCK):
            phys = inst.kernel.bus_physical_port(p.name) or p.name
            clocks.append({"src_pin": f"{inst.name}/{phys}"})

        for p in inst.kernel.ports_of_type(BusType.RESET):
            phys = inst.kernel.bus_physical_port(p.name) or p.name
            resets.append({"src_pin": f"{inst.name}/{phys}"})

    return {"instances": ordered, "clocks": clocks, "resets": resets}
