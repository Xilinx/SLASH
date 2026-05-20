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

#ifndef VRT_XML_PARSER_HPP
#define VRT_XML_PARSER_HPP

#include <libxml/parser.h>
#include <libxml/tree.h>

#include <iostream>
#include <map>
#include <string>
#include <vector>

#include <vrt/kernel.hpp>  // Include the Kernel class
#include <vrt/qdma/qdma_connection.hpp>
#include <vrt/utils/platform.hpp>

namespace vrt {
class Kernel;

/**
 * @brief Class for parsing XML files to extract kernel information.
 */
class XMLParser {
    std::string filename;  ///< The name of the XML file to parse.
    xmlDocPtr document;    ///< Pointer to the parsed XML document.
    xmlNode* rootNode;     ///< Pointer to the root node of the XML document.
    xmlNode* workingNode;  ///< Pointer to the current working node in the XML document.
    std::map<std::string, Kernel> kernels;        ///< Map of kernel names to Kernel objects.
    uint64_t clockFrequency;                      ///< The clock frequency of the device.
    Platform platform;                            ///< The platform of the device.
    std::vector<QdmaConnection> qdmaConnections;  ///< Vector of QDMA connections.

   public:
    /**
     * @brief Constructor for XMLParser.
     * @param file The name of the XML file to parse.
     */
    XMLParser(const std::string& file);

    /**
     * @brief Parses the XML file.
     */
    void parseXML();

    /**
     * @brief Converts an xmlChar pointer to a std::string.
     * @param xmlCharPtr The xmlChar pointer to convert.
     * @return The converted std::string.
     */
    static std::string convertFromXmlCharPtr(const xmlChar* xmlCharPtr);

    /**
     * @brief Gets the map of kernels parsed from the XML file.
     * @return The map of kernel names to Kernel objects.
     */
    std::map<std::string, Kernel> getKernels();

    /**
     * @brief Gets the clock frequency of the device.
     * @return The clock frequency of the device.
     */
    uint64_t getClockFrequency();

    /**
     * @brief Gets the platform of the device.
     * @return The platform of the device.
     */
    Platform getPlatform();

    /**
     * @brief Gets the vector of QDMA connections.
     * @return The vector of QDMA connections.
     */
    std::vector<QdmaConnection> getQdmaConnections();

    /**
     * @brief Destructor for XMLParser.
     */
    ~XMLParser();
};

}  // namespace vrt

#endif  // VRT_XML_PARSER_HPP