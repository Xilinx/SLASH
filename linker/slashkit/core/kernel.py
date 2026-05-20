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
from dataclasses import dataclass, field
from typing import Dict, Iterable, Optional, List
from pathlib import Path

from slashkit.core.port import Port, BusType
from slashkit.core.bus import Bus
from slashkit.core.regs import MemoryMap


@dataclass(frozen=True)
class Kernel:
    """
    Generic kernel/IP *type* definition.
    Contains bus and port definitions — not instance-specific data.
    """
    name: str
    component_xml_path: Path
    ports: Dict[str, Port] = field(default_factory=dict)
    buses: Dict[str, Bus] = field(default_factory=dict)
    vlnv: Optional[str] = None
    memory_maps: List[MemoryMap] = field(default_factory=list)   # NEW
    hls_data_path: Optional[Path] = None

    def port(self, name: str) -> Port:
        """Retrieve a port by name."""
        try:
            return self.ports[name]
        except KeyError as e:
            raise KeyError(
                f"Kernel '{self.name}' has no port named '{name}'.") from e

    def ports_of_type(self, ptype: BusType) -> Iterable[Port]:
        """Iterate over all ports of a given type."""
        return (p for p in self.ports.values() if p.ptype == ptype)

    def bus(self, name: str) -> Bus:
        """Retrieve a bus by name."""
        try:
            return self.buses[name]
        except KeyError as e:
            raise KeyError(
                f"Kernel '{self.name}' has no bus named '{name}'.") from e

    def buses_of_type(self, ptype: BusType) -> Iterable[Bus]:
        """Iterate over all buses of a given type."""
        return (b for b in self.buses.values() if b.ptype == ptype)

    def bus_physical(self, bus_name: str, logical: Optional[str] = None) -> Optional[Port]:
        """Return a physical Port object for a bus (or None if unknown)."""
        bus = self.buses.get(bus_name)
        if bus is None:
            return None
        return bus.physical_port(logical=logical)

    def bus_physical_port(self, bus_name: str, logical: Optional[str] = None) -> Optional[str]:
        """Return a physical port for a bus (or None if unknown)."""
        p = self.bus_physical(bus_name, logical=logical)
        return p.name if p is not None else None


@dataclass
class KernelInstance:
    """
    A specific instance of a Kernel (e.g., 'dma_0').
    Holds a pointer to the Kernel type and optional parameters.
    """
    name: str
    kernel: Kernel
    params: Dict[str, object] = field(default_factory=dict)

    def port(self, name: str) -> Port:
        return self.kernel.port(name)

    def __repr__(self) -> str:
        return f"<KernelInstance {self.name} : {self.kernel.name}>"
