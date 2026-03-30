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

#include <vrt/parser/xml_parser.hpp>

#include <algorithm>

namespace vrt {

namespace {

std::string getNodeContent(xmlNode* node) {
    if (node == nullptr) {
        return "";
    }
    xmlChar* content = xmlNodeGetContent(node);
    if (content == nullptr) {
        return "";
    }
    std::string out(reinterpret_cast<const char*>(content));
    xmlFree(content);
    return out;
}

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

uint32_t parseU32(const std::string& text, uint32_t defaultValue = 0) {
    if (text.empty()) {
        return defaultValue;
    }
    return static_cast<uint32_t>(std::stoul(text, nullptr, 0));
}

uint64_t parseU64(const std::string& text, uint64_t defaultValue = 0) {
    if (text.empty()) {
        return defaultValue;
    }
    return static_cast<uint64_t>(std::stoull(text, nullptr, 0));
}

bool parseBoolInt(const std::string& text, bool defaultValue = false) {
    if (text.empty()) {
        return defaultValue;
    }
    return std::stoi(text, nullptr, 0) != 0;
}

}  // namespace

XMLParser::XMLParser(const std::string& file_path) {
    this->filename = file_path;
    this->document = xmlReadFile(this->filename.c_str(), NULL, 0);
    if (this->document == nullptr) {
        throw std::runtime_error("Failed to parse XML file: " + file_path);
    }
    this->rootNode = xmlDocGetRootElement(this->document);
    if (this->rootNode == nullptr) {
        throw std::runtime_error("XML file has no root element: " + file_path);
    }
    this->workingNode = rootNode->children;
}

void XMLParser::parseXML() {
    for (xmlNode* kernelNode = rootNode->children; kernelNode; kernelNode = kernelNode->next) {
        if (kernelNode->type == XML_ELEMENT_NODE &&
            xmlStrcmp(kernelNode->name, BAD_CAST "Kernel") == 0) {
            std::string name;
            std::string baseAddress;
            std::string range;
            std::vector<Register> registers;
            std::vector<FunctionalArg> functionalArgs;
            std::map<std::string, std::string> connections;
            for (xmlNode* childNode = kernelNode->children; childNode;
                 childNode = childNode->next) {
                if (childNode->type == XML_ELEMENT_NODE) {
                    if (xmlStrcmp(childNode->name, BAD_CAST "Name") == 0) {
                        name = getNodeContent(childNode);
                    } else if (xmlStrcmp(childNode->name, BAD_CAST "BaseAddress") == 0) {
                        baseAddress = getNodeContent(childNode);
                    } else if (xmlStrcmp(childNode->name, BAD_CAST "Range") == 0) {
                        range = getNodeContent(childNode);
                    } else if (xmlStrcmp(childNode->name, BAD_CAST "register") == 0) {
                        std::string offset = getNodeProp(childNode, "offset");
                        std::string regName = getNodeProp(childNode, "name");
                        std::string access = getNodeProp(childNode, "access");
                        std::string description = getNodeProp(childNode, "description");
                        std::string regRange = getNodeProp(childNode, "range");
                        Register reg;
                        reg.setOffset(parseU32(offset));
                        reg.setRegisterName(regName);
                        reg.setRW(access);
                        reg.setDescription(description);
                        reg.setWidth(parseU32(regRange, 32));
                        registers.push_back(reg);
                    } else if (xmlStrcmp(childNode->name, BAD_CAST "connection") == 0) {
                        std::string connPort = getNodeProp(childNode, "port");
                        std::string connTarget = getNodeProp(childNode, "target");
                        if (!connPort.empty() && !connTarget.empty()) {
                            connections[connPort] = connTarget;
                        }
                    } else if (xmlStrcmp(childNode->name, BAD_CAST "functional_args") == 0) {
                        for (xmlNode* argNode = childNode->children; argNode;
                             argNode = argNode->next) {
                            if (argNode->type != XML_ELEMENT_NODE ||
                                xmlStrcmp(argNode->name, BAD_CAST "arg") != 0) {
                                continue;
                            }
                            FunctionalArg arg;
                            arg.idx = parseU32(getNodeProp(argNode, "idx"));
                            arg.name = getNodeProp(argNode, "name");
                            arg.type = getNodeProp(argNode, "type");
                            arg.offset = parseU32(getNodeProp(argNode, "offset"));
                            arg.range = parseU32(getNodeProp(argNode, "range"), 32);
                            arg.readable = parseBoolInt(getNodeProp(argNode, "r"));
                            arg.writable = parseBoolInt(getNodeProp(argNode, "w"));
                            arg.port = getNodeProp(argNode, "port");
                            functionalArgs.push_back(arg);
                        }
                    }
                }
            }
            std::sort(functionalArgs.begin(), functionalArgs.end(),
                      [](const FunctionalArg& a, const FunctionalArg& b) {
                          return a.idx < b.idx;
                      });
            auto ba = parseU64(baseAddress);
            auto r = parseU64(range);
            Kernel kernel(name, ba, r, registers, functionalArgs);
            if (!connections.empty()) {
                kernel.setConnections(connections);
            }
            kernels[name] = kernel;
        } else if (kernelNode->type == XML_ELEMENT_NODE &&
                   xmlStrcmp(kernelNode->name, BAD_CAST "ClockFrequency") == 0) {
            std::string clkFreq = getNodeContent(kernelNode);
            this->clockFrequency = parseU64(clkFreq);
        } else if (kernelNode->type == XML_ELEMENT_NODE &&
                   xmlStrcmp(kernelNode->name, BAD_CAST "Platform") == 0) {
            std::string platform_ = getNodeContent(kernelNode);
            this->platform = (platform_ == "Hardware")     ? Platform::HARDWARE
                             : (platform_ == "Emulation")  ? Platform::EMULATION
                             : (platform_ == "Simulation") ? Platform::SIMULATION
                                                           : Platform::UNKNOWN;
            if (this->platform == Platform::UNKNOWN) {
                throw std::runtime_error("Unknown platform type");
            }
        } else if (kernelNode->type == XML_ELEMENT_NODE &&
                   xmlStrcmp(kernelNode->name, BAD_CAST "Qdma") == 0) {
            std::string kernelName, qdmaStream, syncTypeStr;
            uint32_t qid;
            for (xmlNode* childNode = kernelNode->children; childNode;
                 childNode = childNode->next) {
                if (childNode->type == XML_ELEMENT_NODE) {
                    if (xmlStrcmp(childNode->name, BAD_CAST "kernel") == 0) {
                        kernelName = getNodeContent(childNode);
                    } else if (xmlStrcmp(childNode->name, BAD_CAST "interface") == 0) {
                        qdmaStream = getNodeContent(childNode);
                    } else if (xmlStrcmp(childNode->name, BAD_CAST "direction") == 0) {
                        syncTypeStr = getNodeContent(childNode);
                    } else if (xmlStrcmp(childNode->name, BAD_CAST "qid") == 0) {
                        qid = parseU32(getNodeContent(childNode));
                    }
                }
            }
            qdmaConnections.push_back({kernelName, qid, qdmaStream, syncTypeStr});
        }
    }
}

std::map<std::string, Kernel> XMLParser::getKernels() { return kernels; }

uint64_t XMLParser::getClockFrequency() { return this->clockFrequency; }

Platform XMLParser::getPlatform() { return this->platform; }

std::vector<QdmaConnection> XMLParser::getQdmaConnections() { return this->qdmaConnections; }

XMLParser::~XMLParser() {
    if (this->document != nullptr) {
        xmlFreeDoc(this->document);
    }
}

}  // namespace vrt
