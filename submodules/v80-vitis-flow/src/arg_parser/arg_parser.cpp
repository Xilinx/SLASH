/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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

#include "arg_parser.hpp"
#include <stdexcept>
#include <filesystem>

const std::string ArgParser::XML_PATH = "/syn/report/csynth.xml";

ArgParser::ArgParser(int argc, char** argv) {
    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__, "Parsing arguments");
    freqHz = 200000000;
    segmented = false;
    bool parsingKernels = false;
    for (int i = 1; i < argc; i++) {
        std::string arg(argv[i]);
        if (arg == "--cfg" && i + 1 < argc) {
            configFile = argv[++i];
        } else if (arg == "--segmented") {
            segmented = true;
        } else if (arg == "--kernels") {
            parsingKernels = true;
        } else if (arg == "--source-pre-synth") {
            if (i + 1 < argc) {
                tclInjections.scriptsPreSynth.emplace_back(argv[++i]);
            } else {
                throw std::runtime_error("Missing argument to --source-pre-synth");
            }
        } else if (arg == "--source-post-build") {
            if (i + 1 < argc) {
                tclInjections.scriptsPostBuild.emplace_back(argv[++i]);
            } else {
                throw std::runtime_error("Missing argument to --source-pre-synth");
            }
        } else if (arg == "--platform") {
            std::string plat = argv[++i];
            if (plat == "hw") {
                platform = Platform::HARDWARE;
            } else if (plat == "emu") {
                platform = Platform::EMULATOR;
            } else if (plat == "sim") {
                platform = Platform::SIMULATOR;
            } else {
                utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__,
                                   "Unknown platform: {}", plat);
                throw std::runtime_error("Unknown platform");
            }
        } else if (parsingKernels) {
            if (!arg.empty() && arg.back() == '/') {
                arg.pop_back();
            }
            kernelPaths.emplace_back(arg);
        } else {
            utils::Logger::log(utils::LogLevel::ERROR, __PRETTY_FUNCTION__, "Unknown argument: {}",
                               arg);
            throw std::runtime_error("Unknown argument");
        }
    }
    parseConfig();
    kernels = parseKernels();
}

void ArgParser::parseConfig() {
    std::ifstream configFileStream(configFile);
    if (!configFileStream.is_open()) {
        throw std::runtime_error("Config file not provided");
    }
    std::string line;
    while (std::getline(configFileStream, line)) {
        if (line.find("nk=") == 0) {
            std::istringstream iss(line.substr(3));
            std::string kernelType, count, kernelName;
            if (std::getline(iss, kernelType, ':') && std::getline(iss, count, ':')) {
                for (size_t i = 0; i < std::stoi(count) - 1; i++) {
                    utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                       "Kernel type: {}, count: {}", kernelType, count);
                    std::getline(iss, kernelName, '.');
                    kernelEntities[kernelName] = kernelType;
                }
                std::getline(iss, kernelName);
                kernelEntities[kernelName] = kernelType;
            }
        } else if (line.find("stream_connect=") == 0) {
            std::istringstream iss(line.substr(15));
            std::string srcKernel, srcIntf;
            std::string dstKernel, dstIntf;
            if (std::getline(iss, srcKernel, '.') && std::getline(iss, srcIntf, ':') &&
                std::getline(iss, dstKernel, '.') && std::getline(iss, dstIntf)) {
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                   "Parsing connection: {}.{} -> {}.{}", srcKernel, srcIntf,
                                   dstKernel, dstIntf);
                ConnectionElement src{srcKernel, srcIntf};
                ConnectionElement dst{dstKernel, dstIntf};
                Connection conn{src, dst};
                connections.emplace_back(conn);
            }
        } else if (line.find("freqhz=") == 0) {
            std::istringstream iss(line.substr(7));
            std::string freq;
            if (std::getline(iss, freq)) {
                freqHz = std::stoll(freq);
                utils::Logger::log(utils::LogLevel::INFO, __PRETTY_FUNCTION__,
                                   "Setting clock at {} Hz", freqHz);
            }
        } else if (line.find("pre_synth=") == 0) {
            std::istringstream iss(line.substr(10));
            std::string relfile;
            if (std::getline(iss, relfile)) {
                std::filesystem::path base = std::filesystem::absolute(std::filesystem::path(configFile)).parent_path();
                std::filesystem::path candidate = std::filesystem::path(relfile);

                if (candidate.is_relative()) {
                    candidate = base / candidate;
                }

                std::string path = std::filesystem::weakly_canonical(candidate).string();

                tclInjections.scriptsPreSynth.push_back(std::move(path));
            }
        } else if (line.find("post_build=") == 0) {
            std::istringstream iss(line.substr(11));
            std::string relfile;
            if (std::getline(iss, relfile)) {
                std::filesystem::path base = std::filesystem::absolute(std::filesystem::path(configFile)).parent_path();
                std::filesystem::path candidate = std::filesystem::path(relfile);

                if (candidate.is_relative()) {
                    candidate = base / candidate;
                }

                std::string path = std::filesystem::weakly_canonical(candidate).string();

                tclInjections.scriptsPostBuild.push_back(std::move(path));
            }
        }
    }
}

std::vector<Kernel> ArgParser::parseKernels() {
    std::vector<Kernel> kernels_;
    for (const auto& path : kernelPaths) {
        XmlParser parser(path + ArgParser::XML_PATH);
        parser.parseXml();
        std::for_each(kernelEntities.begin(), kernelEntities.end(), [&](const auto& entity) {
            Kernel krnl = parser.getKernel();
            if (krnl.getTopModelName() == entity.second) {
                krnl.setName(entity.first);
                kernels_.emplace_back(krnl);
                krnl.print();
            }
        });
        // kernels_.emplace_back(parser.getKernel());
    }
    return kernels_;
}

std::string ArgParser::getConfigFile() const { return configFile; }

std::vector<Kernel> ArgParser::getKernels() { return kernels; }

std::map<std::string, std::string> ArgParser::getKernelEntities() const { return kernelEntities; }

std::vector<Connection> ArgParser::getConnections() const { return connections; }

uint64_t ArgParser::getFreqHz() const { return freqHz; }

bool ArgParser::isSegmented() const { return segmented; }

Platform ArgParser::getPlatform() { return platform; }

std::vector<std::string> ArgParser::getKernelPaths() { return kernelPaths; }

const TclInjections &ArgParser::getTclInjections() const { return tclInjections; }
