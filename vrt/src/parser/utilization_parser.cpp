/**
 * The MIT License (MIT)
 * Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <vrt/parser/utilization_parser.hpp>

#include <stdexcept>

namespace vrt {

namespace {

std::string getNodeProp(xmlNode* node, const char* propName) {
    if (node == nullptr) {
        return "";
    }
    xmlChar* prop = xmlGetProp(node, BAD_CAST propName);
    if (prop == nullptr) {
        return "";
    }
    std::string out(reinterpret_cast<const char*>(prop));
    xmlFree(prop);
    return out;
}

uint32_t parseU32(const std::string& text) {
    if (text.empty()) {
        return 0;
    }
    return static_cast<uint32_t>(std::stoul(text, nullptr, 0));
}

std::optional<float> parseOptionalFloat(const std::string& text) {
    if (text.empty()) {
        return std::nullopt;
    }
    return std::stof(text);
}

ResourceMetrics parseResourceMetrics(xmlNode* node) {
    ResourceMetrics m;
    m.totalPplocs = parseU32(getNodeProp(node, "total_pplocs"));
    m.totalLuts = parseU32(getNodeProp(node, "total_luts"));
    m.lutram = parseU32(getNodeProp(node, "lutram"));
    m.srl = parseU32(getNodeProp(node, "srl"));
    m.ff = parseU32(getNodeProp(node, "ff"));
    m.ramb36 = parseU32(getNodeProp(node, "ramb36"));
    m.ramb18 = parseU32(getNodeProp(node, "ramb18"));
    m.ramb = parseU32(getNodeProp(node, "ramb"));
    m.uram = parseU32(getNodeProp(node, "uram"));
    m.dsp = parseU32(getNodeProp(node, "dsp"));

    m.totalLutsPct = parseOptionalFloat(getNodeProp(node, "total_luts_pct"));
    m.lutramPct = parseOptionalFloat(getNodeProp(node, "lutram_pct"));
    m.srlPct = parseOptionalFloat(getNodeProp(node, "srl_pct"));
    m.ffPct = parseOptionalFloat(getNodeProp(node, "ff_pct"));
    m.ramb36Pct = parseOptionalFloat(getNodeProp(node, "ramb36_pct"));
    m.ramb18Pct = parseOptionalFloat(getNodeProp(node, "ramb18_pct"));
    m.uramPct = parseOptionalFloat(getNodeProp(node, "uram_pct"));
    m.dspPct = parseOptionalFloat(getNodeProp(node, "dsp_pct"));
    return m;
}

/// Parse a <kernel> or <cell> element whose metrics live in a child <totals>.
UtilizationCell parseCellWithTotals(xmlNode* node) {
    UtilizationCell cell;
    cell.instance = getNodeProp(node, "instance");
    cell.module = getNodeProp(node, "module");
    for (xmlNode* child = node->children; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE &&
            xmlStrcmp(child->name, BAD_CAST "totals") == 0) {
            cell.metrics = parseResourceMetrics(child);
            break;
        }
    }
    return cell;
}

/// Parse the <slash> element containing <kernels>, <slash_logic>, <totals>, sums.
UtilizationBlock parseSlashBlock(xmlNode* slashNode) {
    UtilizationBlock block;
    block.name = "slash";
    Subhierarchy sub;

    for (xmlNode* child = slashNode->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) {
            continue;
        }
        if (xmlStrcmp(child->name, BAD_CAST "totals") == 0) {
            block.totals = parseResourceMetrics(child);
        } else if (xmlStrcmp(child->name, BAD_CAST "kernels") == 0) {
            for (xmlNode* k = child->children; k; k = k->next) {
                if (k->type == XML_ELEMENT_NODE &&
                    xmlStrcmp(k->name, BAD_CAST "kernel") == 0) {
                    sub.cells.push_back(parseCellWithTotals(k));
                }
            }
        } else if (xmlStrcmp(child->name, BAD_CAST "slash_logic") == 0) {
            for (xmlNode* c = child->children; c; c = c->next) {
                if (c->type == XML_ELEMENT_NODE &&
                    xmlStrcmp(c->name, BAD_CAST "cell") == 0) {
                    sub.slashLogic.push_back(parseCellWithTotals(c));
                }
            }
        } else if (xmlStrcmp(child->name, BAD_CAST "kernel_sum") == 0) {
            sub.subhierarchySum = parseResourceMetrics(child);
        } else if (xmlStrcmp(child->name, BAD_CAST "slash_logic_sum") == 0) {
            sub.slashLogicSum = parseResourceMetrics(child);
        }
    }

    block.subhierarchy = std::move(sub);
    return block;
}

/// Parse a <service_layer> element (totals only, no subhierarchy).
UtilizationBlock parseServiceLayerBlock(xmlNode* node) {
    UtilizationBlock block;
    block.name = "service_layer";
    for (xmlNode* child = node->children; child; child = child->next) {
        if (child->type == XML_ELEMENT_NODE &&
            xmlStrcmp(child->name, BAD_CAST "totals") == 0) {
            block.totals = parseResourceMetrics(child);
            break;
        }
    }
    return block;
}

}  // namespace

UtilizationParser::UtilizationParser(const std::string& filePath) : filename(filePath) {
    document = xmlReadFile(filePath.c_str(), nullptr, 0);
    if (document == nullptr) {
        throw std::runtime_error("Failed to parse utilization XML: " + filePath);
    }
}

void UtilizationParser::parse() {
    xmlNode* root = xmlDocGetRootElement(document);
    if (root == nullptr) {
        throw std::runtime_error("Utilization XML has no root element: " + filename);
    }

    bool foundSlash = false;
    for (xmlNode* node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) {
            continue;
        }

        if (xmlStrcmp(node->name, BAD_CAST "slash") == 0) {
            report.slash = parseSlashBlock(node);
            foundSlash = true;
        } else if (xmlStrcmp(node->name, BAD_CAST "service_layer") == 0) {
            report.serviceLayer = parseServiceLayerBlock(node);
        }
    }

    if (!foundSlash) {
        throw std::runtime_error("Utilization report missing required 'slash' block: " + filename);
    }
}

const UtilizationReport& UtilizationParser::getReport() const { return report; }

UtilizationParser::~UtilizationParser() {
    if (document != nullptr) {
        xmlFreeDoc(document);
    }
}

}  // namespace vrt
