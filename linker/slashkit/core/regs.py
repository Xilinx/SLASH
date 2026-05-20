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
from typing import List, Optional


@dataclass
class RegField:
    name: str
    description: Optional[str]
    bit_offset: int
    bit_width: int
    access: Optional[str] = None
    modified_write_value: Optional[str] = None
    read_action: Optional[str] = None
    reset_value: Optional[int] = None


@dataclass
class Register:
    name: str
    display_name: Optional[str]
    description: Optional[str]
    address_offset: int
    size: int
    access: Optional[str] = None
    reset_value: Optional[int] = None
    fields: List[RegField] = field(default_factory=list)


@dataclass
class AddressBlock:
    name: str
    base_address: int
    range: int
    width: int
    usage: Optional[str] = None
    access: Optional[str] = None
    offset_base_param: Optional[str] = None
    offset_high_param: Optional[str] = None
    registers: List[Register] = field(default_factory=list)


@dataclass
class MemoryMap:
    name: str
    address_blocks: List[AddressBlock] = field(default_factory=list)
