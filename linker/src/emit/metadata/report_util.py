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

import logging
import re
from dataclasses import dataclass, field
from typing import Dict, List, Optional
import xml.etree.ElementTree as ET


@dataclass
class ValueWithPercent:
    value: Optional[int]
    pct: Optional[float]


@dataclass
class UtilRow:
    instance_raw: str
    instance: str
    module: str
    pr_attribute: str

    total_pplocs: ValueWithPercent
    total_luts: ValueWithPercent
    lutrams: ValueWithPercent
    srls: ValueWithPercent
    ffs: ValueWithPercent
    ramb36: ValueWithPercent
    ramb18: ValueWithPercent
    uram: ValueWithPercent
    dsp: ValueWithPercent

    depth: int = 0

    def ramb_equivalent_18k(self) -> int:
        return (self.ramb18.value or 0) + 2 * (self.ramb36.value or 0)


@dataclass
class TreeNode:
    row: UtilRow
    children: List["TreeNode"] = field(default_factory=list)


@dataclass
class ResourceTotals:
    total_pplocs: int = 0
    total_luts: int = 0
    lutrams: int = 0
    srls: int = 0
    ffs: int = 0
    ramb36: int = 0
    ramb18: int = 0
    uram: int = 0
    dsp: int = 0

    def add(self, r: UtilRow) -> None:
        self.total_pplocs += r.total_pplocs.value or 0
        self.total_luts += r.total_luts.value or 0
        self.lutrams += r.lutrams.value or 0
        self.srls += r.srls.value or 0
        self.ffs += r.ffs.value or 0
        self.ramb36 += r.ramb36.value or 0
        self.ramb18 += r.ramb18.value or 0
        self.uram += r.uram.value or 0
        self.dsp += r.dsp.value or 0

    def ramb_equivalent_18k(self) -> int:
        return self.ramb18 + 2 * self.ramb36


CELL_VALUE_PCT = re.compile(
    r"^\s*(?P<val>\d+)\s*(?:\(\s*(?P<pct>\d+(?:\.\d+)?)%\s*\))?\s*$")


def parse_cell_value_and_percent(cell: str) -> ValueWithPercent:
    """! @brief Parse a utilization cell containing a value and optional percent.

    @param cell Raw cell string from the utilization table.
    @return Parsed value and percent (if present).
    """
    cell = (cell or "").strip()
    if not cell or cell == "-":
        return ValueWithPercent(None, None)

    m = CELL_VALUE_PCT.match(cell)
    if not m:
        digits = re.match(r"^\s*(\d+)", cell)
        return ValueWithPercent(int(digits.group(1)), None) if digits else ValueWithPercent(None, None)

    val = int(m.group("val"))
    pct = float(m.group("pct")) if m.group("pct") is not None else None
    return ValueWithPercent(val, pct)


def parse_vivado_hierarchical_utilization_table(text: str) -> List[UtilRow]:
    """! @brief Parse the Vivado hierarchical utilization table into rows.

    @param text Full report file contents.
    @return List of parsed utilization rows.
    """
    lines = text.splitlines()
    rows: List[UtilRow] = []
    in_table = False

    for ln in lines:
        if ln.startswith("|") and "Instance" in ln and "Module" in ln and "Total LUTs" in ln:
            in_table = True
            continue

        if not in_table:
            continue

        if ln.startswith("+") and ln.count("+") > 5:
            continue

        if not ln.startswith("|"):
            if rows:
                break
            continue

        raw_parts = ln.strip("\n").strip("|").split("|")
        if len(raw_parts) < 13:
            continue

        instance_col_raw = raw_parts[0]
        module_col_raw = raw_parts[1]
        pr_attr_col_raw = raw_parts[2]

        depth = (len(instance_col_raw) -
                 len(instance_col_raw.lstrip(" "))) // 2
        instance = instance_col_raw.strip()
        module = module_col_raw.strip()
        pr_attr = pr_attr_col_raw.strip()

        rows.append(
            UtilRow(
                instance_raw=instance_col_raw,
                instance=instance,
                module=module,
                pr_attribute=pr_attr,
                total_pplocs=parse_cell_value_and_percent(raw_parts[3]),
                total_luts=parse_cell_value_and_percent(raw_parts[4]),
                lutrams=parse_cell_value_and_percent(raw_parts[6]),
                srls=parse_cell_value_and_percent(raw_parts[7]),
                ffs=parse_cell_value_and_percent(raw_parts[8]),
                ramb36=parse_cell_value_and_percent(raw_parts[9]),
                ramb18=parse_cell_value_and_percent(raw_parts[10]),
                uram=parse_cell_value_and_percent(raw_parts[11]),
                dsp=parse_cell_value_and_percent(raw_parts[12]),
                depth=depth,
            )
        )

    return rows


def build_hierarchy_tree(rows: List[UtilRow]) -> Dict[str, TreeNode]:
    """! @brief Build a parent/child hierarchy from flat utilization rows.

    @param rows Utilization rows with depth set.
    @return Map of instance name to tree node.
    """
    nodes_by_instance: Dict[str, TreeNode] = {}
    stack: List[TreeNode] = []

    for r in rows:
        node = TreeNode(row=r)
        nodes_by_instance[r.instance] = node

        while stack and stack[-1].row.depth >= r.depth:
            stack.pop()

        if stack:
            stack[-1].children.append(node)

        stack.append(node)

    return nodes_by_instance


def write_totals_attributes_from_row(elem: ET.Element, r: UtilRow) -> None:
    """! @brief Write utilization totals (including pct) from a row.

    @param elem XML element to update.
    @param r Utilization row source.
    """
    def _set_val_pct(elem: ET.Element, base: str, v: ValueWithPercent) -> None:
        elem.set(base, str(v.value or 0))
        if v.pct is not None:
            elem.set(f"{base}_pct", f"{v.pct:.2f}")

    _set_val_pct(elem, "total_pplocs", r.total_pplocs)
    _set_val_pct(elem, "total_luts", r.total_luts)
    _set_val_pct(elem, "lutram", r.lutrams)
    _set_val_pct(elem, "srl", r.srls)
    _set_val_pct(elem, "ff", r.ffs)
    _set_val_pct(elem, "ramb36", r.ramb36)
    _set_val_pct(elem, "ramb18", r.ramb18)
    elem.set("ramb", str(r.ramb_equivalent_18k()))
    _set_val_pct(elem, "uram", r.uram)
    _set_val_pct(elem, "dsp", r.dsp)


def write_totals_attributes_from_totals(elem: ET.Element, t: ResourceTotals) -> None:
    """! @brief Write utilization totals from accumulated totals.

    @param elem XML element to update.
    @param t Resource totals source.
    """
    elem.set("total_pplocs", str(t.total_pplocs))
    elem.set("total_luts", str(t.total_luts))
    elem.set("lutram", str(t.lutrams))
    elem.set("srl", str(t.srls))
    elem.set("ff", str(t.ffs))
    elem.set("ramb36", str(t.ramb36))
    elem.set("ramb18", str(t.ramb18))
    elem.set("ramb", str(t.ramb_equivalent_18k()))
    elem.set("uram", str(t.uram))
    elem.set("dsp", str(t.dsp))


def write_cell(parent_element: ET.Element, node: TreeNode, is_kernel=False, recurse=False) -> None:
    """! @brief Write a cell/kernel, potentially recursing into sub-cells

    @param parent_element Parent element under which to place the new cell
    @param node Utilization tree node.
    @param is_kernel If true, call the cell a "kernel." Defaults to False, using the "cell" name.
    @param recurse If true, add the cells within the tree node. Defaults to False.
    """
    name = "kernel" if is_kernel else "cell"
    cell_element = ET.SubElement(
        parent_element, name, instance=node.row.instance, module=node.row.module)
    write_totals_attributes_from_row(
        ET.SubElement(cell_element, "totals"), node.row)
    if not recurse or len(node.children) == 0:
        return
    for child in node.children:
        write_cell(cell_element, child, recurse=recurse)


def create_utilization_xml(nodes: Dict[str, TreeNode]) -> ET.ElementTree:
    """! @brief Create the utilization XML tree from parsed nodes.

    @param nodes Map of instance name to tree node.
    @return XML element tree representing utilization report.
    """
    root = ET.Element("utilization_report")
    write_totals_attributes_from_row(ET.SubElement(
        root, "totals"), nodes["top_wrapper"].row)

    for region_name in ["static_region", "service_layer"]:
        if region_name not in nodes:
            continue
        node = nodes[region_name]
        element = ET.SubElement(root, region_name)
        write_totals_attributes_from_row(
            ET.SubElement(element, "totals"), node.row)

    slash_node = nodes["slash"]
    slash_element = ET.SubElement(root, "slash")

    kernels = ET.SubElement(slash_element, "kernels")
    slash_logic_cells = ET.SubElement(slash_element, "slash_logic")

    kernel_sum = ResourceTotals()
    slash_logic_sum = ResourceTotals()

    for child in slash_node.children:
        inst = child.row.instance.strip()
        if inst.startswith("(") and inst.endswith(")"):
            continue

        is_slash_logic = any(p.search(inst) for p in [
            re.compile(r".*_sc_.*"),        # hbm_sc_01, etc.
            re.compile(r"^smartconnect.*"),  # smartconnect_0, etc.
        ])

        if is_slash_logic:
            write_cell(slash_logic_cells, child)
            slash_logic_sum.add(child.row)
        else:
            write_cell(kernels, child, recurse=True, is_kernel=True)
            kernel_sum.add(child.row)

    write_totals_attributes_from_row(ET.SubElement(
        slash_element, "totals"), slash_node.row)
    write_totals_attributes_from_totals(
        ET.SubElement(slash_element, "kernel_sum"), kernel_sum)
    write_totals_attributes_from_totals(
        ET.SubElement(slash_element, "slash_logic_sum"), slash_logic_sum)
    ET.Comment()

    ET.indent(root)
    tree = ET.ElementTree(root)
    return tree


def convert_report_utilization_to_xml(report_path: str, out_xml_path: str) -> None:
    """! @brief Convert Vivado utilization report text to XML.

    @param report_path Path to the report_utilization_*.txt input.
    @param out_xml_path Path to the report_utilization_*.xml output.
    """
    logger.info("Converting utilization report to XML")
    logger.info("Utilization report input: %s", report_path)
    logger.info("Utilization report output: %s", out_xml_path)
    with open(report_path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()

    rows = parse_vivado_hierarchical_utilization_table(text)
    logger.info("Parsed utilization rows: %d", len(rows))
    nodes = build_hierarchy_tree(rows)
    logger.info("Built utilization node map: %d", len(nodes))
    tree = create_utilization_xml(nodes)
    tree.write(out_xml_path, encoding="utf-8", xml_declaration=True)
    logger.info("Utilization report XML generation complete")


logger = logging.getLogger(__name__)
