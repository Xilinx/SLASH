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

#include <iostream>
#include <ap_fixed.h>
#include <hls_stream.h>
#include <ap_int.h>
#include <zmq.hpp>
#include <json/json.h>
#include <cstdint>
#include <map>
#include <vector>
#include <cstring>
#include <sstream>
#include <stdexcept>
#include <fstream>
#include <functional>
#include <thread>
#include <future>
#include <chrono>
#include <cstdlib>
#include <mutex>

{% for p in prototypes %}
{{ p }}
{% endfor %}

template <typename T>
void assignValue(T& var, const Json::Value& value) {
  if (value.isString()) { std::istringstream iss(value.asString()); iss >> var; }
  else if (value.isInt()) var = static_cast<T>(value.asInt());
  else if (value.isUInt()) var = static_cast<T>(value.asUInt());
  else if (value.isDouble()) var = static_cast<T>(value.asDouble());
  else throw std::runtime_error("Unsupported JSON value type");
}

template <typename T>
Json::Value createJsonValue(const T& var) {
  uint32_t raw = 0;
  const size_t n = sizeof(raw) < sizeof(T) ? sizeof(raw) : sizeof(T);
  std::memcpy(&raw, &var, n);
  return Json::Value(raw);
}

template <typename T>
Json::Value createJsonValueHi(const T& var) {
  uint32_t raw = 0;
  if (sizeof(T) > sizeof(uint32_t)) {
    const size_t off = sizeof(uint32_t);
    const size_t remain = sizeof(T) - off;
    const size_t n = sizeof(raw) < remain ? sizeof(raw) : remain;
    std::memcpy(&raw, reinterpret_cast<const uint8_t*>(&var) + off, n);
  }
  return Json::Value(raw);
}

Json::Value createJsonBuffer(const uint8_t* buffer, size_t size) {
  Json::Value value(Json::arrayValue);
  for (size_t i = 0; i < size; ++i) {
    value.append(buffer[i]);
  }
  return value;
}

std::string emuJsonString(const Json::Value& value) {
  Json::StreamWriterBuilder writer;
  writer["indentation"] = "";
  std::string s = Json::writeString(writer, value);
  if (!s.empty() && s.back() == '\n') s.pop_back();
  return s;
}

void emuExecLog(const char* scope, const std::string& msg) {
  std::cerr << "EMU_EXEC: [" << scope << "] " << msg << std::endl;
}

bool emuExecVerboseEnabled() {
  static const bool enabled = []() {
    const char* env = std::getenv("EMU_EXEC_VERBOSE");
    if (env == nullptr) return false;
    std::string v(env);
    return !(v.empty() || v == "0" || v == "false" || v == "FALSE" || v == "off" || v == "OFF");
  }();
  return enabled;
}

void emuExecDebug(const char* scope, const std::string& msg) {
  if (emuExecVerboseEnabled()) {
    emuExecLog(scope, msg);
  }
}

int main() {
  zmq::context_t context(1);
  zmq::socket_t socket(context, ZMQ_REP);
  socket.bind("tcp://*:5555");
  emuExecLog("startup", std::string("bound REP socket to tcp://*:5555")
                        + (emuExecVerboseEnabled() ? " (verbose=on)" : " (verbose=off)"));

  std::map<std::string, void*> buffers;
  std::map<std::string, size_t> bufferSizes;

{% for v in vars %}
  {{ v }};
{% endfor %}

{% for w in wires %}
  hls::stream<{{ w.ctype }}> {{ w.name }};
{% endfor %}

  std::map<std::string, std::function<void()>> autostartRegistry;
{% for ac in autostart_calls %}
  autostartRegistry["{{ ac.inst }}"] = [&]() {
    {{ ac.top }}({{ ac.call_args | join(", ") }});
  };
{% endfor %}
  std::map<std::string, std::future<void>> activeCallFutures;

  std::map<std::string, std::function<Json::Value()>> fetchScalarRegistry;
{% for sym in fetch_scalar_var_symbols %}
  fetchScalarRegistry["{{ sym }}"] = [&]() {
    return createJsonValue({{ sym }});
  };
{% endfor %}
  std::map<std::string, std::function<Json::Value()>> fetchScalarHiRegistry;
{% for sym in fetch_scalar_var_symbols %}
  fetchScalarHiRegistry["{{ sym }}"] = [&]() {
    return createJsonValueHi({{ sym }});
  };
{% endfor %}
  std::map<std::string, std::map<uint32_t, uint32_t>> kernelRegisterShadow;
  std::mutex kernelRegisterShadowMutex;

  Json::Value emuManifest;
  bool emuManifestLoaded = false;
  {
    std::ifstream manifestFile("emu_manifest.json");
    if (!manifestFile.is_open()) {
      emuExecLog("manifest", "emu_manifest.json not found");
      return 1;
    }
    emuExecDebug("manifest", "opened emu_manifest.json");
    Json::Reader manifestReader;
    emuManifestLoaded = manifestReader.parse(manifestFile, emuManifest);
    emuExecDebug("manifest", std::string("parse result=") + (emuManifestLoaded ? "success" : "failure"));
    if (!emuManifestLoaded) {
      emuExecLog("manifest", "parse failed");
      return 1;
    }
  }

  bool emuManifestUsable = emuManifestLoaded && emuManifest.isObject();
  bool emuManifestHasKernelMetadata = false;
  bool emuManifestHasFetchMetadata = false;
  bool emuManifestHasRegisterMetadata = false;
  bool emuManifestSchemaValidated = false;
  bool requireFastExitOnExit = false;
  size_t manifestFetchScalarRouteCount = 0;
  size_t manifestCallableKernelCount = 0;
  size_t manifestAutostartKernelCount = 0;
  size_t manifestRegisterCount = 0;
  std::map<std::string, Json::Value> kernelManifestRegistry;

  if (emuManifestLoaded && !emuManifest.isObject()) {
    emuExecLog("manifest", "emu_manifest.json parsed but root is not an object");
    return 1;
  }
  if (emuManifestUsable) {
    const Json::Value schema = emuManifest["manifest_schema"];
    if (schema.isObject()) {
      emuExecDebug("manifest", std::string("manifest_schema=") + emuJsonString(schema));
      const Json::Value required = schema["required_sections"];
      bool requiredOk = required.isArray();
      if (requiredOk) {
        for (const auto& name : required) {
          if (!name.isString()) {
            requiredOk = false;
            emuExecLog("manifest", "required_sections contains non-string entry");
            break;
          }
              if (!emuManifest.isMember(name.asString())) {
                requiredOk = false;
                emuExecLog("manifest", std::string("missing required section '") + name.asString()
                                   + "'");
                break;
              }
        }
      }
      emuManifestSchemaValidated = schema.get("version", 0).asUInt() >= 1 && requiredOk;
    } else {
      emuExecLog("manifest", "emu_manifest.json has no manifest_schema");
      return 1;
    }

    const Json::Value fetchMeta = emuManifest["fetch"];
    if (fetchMeta.isObject()) {
      const Json::Value fetchScalar = fetchMeta["scalar"];
      if (fetchScalar.isArray()) {
        emuManifestHasFetchMetadata = true;
        manifestFetchScalarRouteCount = fetchScalar.size();
        emuExecDebug("manifest", "fetch.scalar routes=" + std::to_string(manifestFetchScalarRouteCount));
      }
    }
  }

  if (!emuManifestSchemaValidated) {
    emuExecLog("manifest", "manifest schema validation failed");
    return 1;
  }

  if (emuManifestUsable) {
    const Json::Value kernels = emuManifest["kernels"];
    if (kernels.isArray()) {
      emuManifestHasKernelMetadata = true;
      for (const auto& k : kernels) {
        if (!k.isObject()) continue;
        std::string instance = k.get("instance", "").asString();
        if (!instance.empty()) {
          kernelManifestRegistry[instance] = k;
          std::map<uint32_t, uint32_t> regShadow;
          const Json::Value regs = k["registers"];
          if (regs.isArray()) {
            for (const auto& reg : regs) {
              if (!reg.isObject()) continue;
              const Json::Value offsetVal = reg["offset"];
              if (!offsetVal.isUInt() && !offsetVal.isInt()) continue;
              regShadow[static_cast<uint32_t>(offsetVal.asUInt())] = 0u;
              manifestRegisterCount += 1;
            }
            emuManifestHasRegisterMetadata = true;
          }
          auto ctrlIt = regShadow.find(0x00u);
          if (ctrlIt != regShadow.end()) {
            // HLS ap_ctrl_hs reset-like state: idle=1, ready=1, done=0, start=0.
            ctrlIt->second = 0x0Cu;
          }
          kernelRegisterShadow[instance] = std::move(regShadow);
          emuExecDebug("manifest", std::string("kernel meta loaded for '") + instance + "'");
        }
        if (k.get("callable", false).asBool()) {
          manifestCallableKernelCount += 1;
        }
        if (!k.get("autostart", false).asBool()) continue;
        manifestAutostartKernelCount += 1;
        auto it = autostartRegistry.find(instance);
        if (it == autostartRegistry.end()) {
          emuExecLog("autostart", std::string("manifest requested autostart for unknown instance '") + instance + "'");
          continue;
        }
        if (k.get("shutdown_policy", "").asString() == "fast_exit") {
          requireFastExitOnExit = true;
        }
        emuExecLog("autostart", std::string("launching '") + instance + "' from manifest");
        std::thread(it->second).detach();
      }
    }
  }

  if (!emuManifestHasKernelMetadata) {
    emuExecLog("manifest", "manifest missing kernels metadata");
    return 1;
  }
  if (!emuManifestHasFetchMetadata) {
    emuExecLog("manifest", "manifest missing fetch.scalar metadata");
    return 1;
  }
  if (!emuManifestHasRegisterMetadata) {
    emuExecLog("manifest", "manifest missing kernel register metadata");
    return 1;
  }

  emuExecLog("manifest",
    std::string("manifest loaded")
    + " schema=" + (emuManifestSchemaValidated ? "ok" : "invalid")
    + " kernels=" + std::to_string(kernelManifestRegistry.size())
    + " regs=" + std::to_string(manifestRegisterCount)
    + " callable=" + std::to_string(manifestCallableKernelCount)
    + " autostart=" + std::to_string(manifestAutostartKernelCount)
    + " fetch.scalar=" + std::to_string(manifestFetchScalarRouteCount));

  auto setKernelCtrlRunning = [&](const std::string& functionName) {
    std::lock_guard<std::mutex> lock(kernelRegisterShadowMutex);
    auto kernelIt = kernelRegisterShadow.find(functionName);
    if (kernelIt == kernelRegisterShadow.end()) return;
    auto ctrlIt = kernelIt->second.find(0x00u);
    if (ctrlIt == kernelIt->second.end()) return;
    const uint32_t autoRestart = ctrlIt->second & 0x80u;
    ctrlIt->second = autoRestart | 0x01u;  // ap_start=1, done/idle/ready cleared while active
  };

  auto setKernelCtrlCompleted = [&](const std::string& functionName) {
    std::lock_guard<std::mutex> lock(kernelRegisterShadowMutex);
    auto kernelIt = kernelRegisterShadow.find(functionName);
    if (kernelIt == kernelRegisterShadow.end()) return;
    auto ctrlIt = kernelIt->second.find(0x00u);
    if (ctrlIt == kernelIt->second.end()) return;
    const uint32_t autoRestart = ctrlIt->second & 0x80u;
    ctrlIt->second = autoRestart | 0x0Eu;  // ap_done=1, ap_idle=1, ap_ready=1
  };

  auto refreshKernelRegisterShadowFromManifest =
      [&](const std::string& functionName, bool includeSyntheticCtrlValid) {
        std::lock_guard<std::mutex> lock(kernelRegisterShadowMutex);
        auto kernelIt = kernelRegisterShadow.find(functionName);
        if (kernelIt == kernelRegisterShadow.end()) return;
        const Json::Value fetchRoutes = emuManifest["fetch"]["scalar"];
        if (!fetchRoutes.isArray()) return;
        const Json::Value kernelMeta = kernelManifestRegistry[functionName];
        const Json::Value callableArgs = kernelMeta["call_args"];

        for (const auto& route : fetchRoutes) {
          if (!route.isObject()) continue;
          if (route.get("function", "").asString() != functionName) continue;
          if (!includeSyntheticCtrlValid && callableArgs.isArray()) {
            const std::string routeArg = route.get("arg", "").asString();
            bool routeArgIsCallBound = false;
            for (const auto& ca : callableArgs) {
              if (!ca.isObject()) continue;
              if (ca.get("arg", "").asString() == routeArg) {
                routeArgIsCallBound = true;
                break;
              }
            }
            if (!routeArgIsCallBound) continue;
          }
          const Json::Value source = route["source"];
          if (!source.isObject()) continue;
          const Json::Value regOffVal = source["register_offset"];
          if (!regOffVal.isUInt() && !regOffVal.isInt()) continue;
          const uint32_t regOff = static_cast<uint32_t>(regOffVal.asUInt());
          auto regIt = kernelIt->second.find(regOff);
          if (regIt == kernelIt->second.end()) continue;

          const bool isSyntheticCtrlValid =
              source.isMember("synthetic") && source["synthetic"].isString()
              && source["synthetic"].asString() == "ctrl_valid";
          if (isSyntheticCtrlValid && !includeSyntheticCtrlValid) {
            regIt->second = 0u;
            continue;
          }

          Json::Value routeValue;
          bool handled = false;
          const std::string kind = route.get("kind", "").asString();
          if (kind == "var") {
            std::string symbol = route.get("var_symbol", "").asString();
            auto it = fetchScalarRegistry.find(symbol);
            if (it != fetchScalarRegistry.end()) {
              routeValue = it->second();
              handled = true;
            } else {
              emuExecLog("reg_shadow",
                        "manifest var route symbol missing from fetchScalarRegistry: " + symbol);
            }
          } else if (kind == "var_u32_hi") {
            std::string symbol = route.get("var_symbol", "").asString();
            auto it = fetchScalarHiRegistry.find(symbol);
            if (it != fetchScalarHiRegistry.end()) {
              routeValue = it->second();
              handled = true;
            } else {
              emuExecLog("reg_shadow",
                        "manifest var_u32_hi symbol missing from fetchScalarHiRegistry: " + symbol);
            }
          } else if (kind == "const_u32") {
            routeValue = Json::Value(static_cast<Json::UInt>(route.get("value", 0).asUInt()));
            handled = true;
          } else {
            emuExecLog("reg_shadow", "unsupported manifest route kind for shadow refresh: " + kind);
          }

          if (!handled) continue;
          if (!routeValue.isUInt() && !routeValue.isInt()) {
            emuExecLog("reg_shadow", "non-scalar manifest route value for shadow refresh");
            continue;
          }
          regIt->second = static_cast<uint32_t>(routeValue.asUInt());
        }
      };

  while (true) {
    zmq::message_t request;
    if (!socket.recv(request, zmq::recv_flags::none)) {
      emuExecDebug("zmq", "recv(request) returned no message");
      continue;
    }
    std::string req_str(static_cast<char*>(request.data()), request.size());
    Json::Value root;
    Json::Reader reader;
    if (!reader.parse(req_str, root)) {
      emuExecLog("request", std::string("JSON parse failed: ") + reader.getFormattedErrorMessages());
      emuExecDebug("request", std::string("raw=") + req_str);
      socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
      continue;
    }

    std::string command = root["command"].asString();
    std::string argType;
    emuExecDebug("request", "command=" + command + " json=" + emuJsonString(root));

    if (command == "populate") {
      std::string name = root["name"].asString();
      size_t bufferSize = root["size"].asUInt64();
      emuExecDebug("populate", "name=" + name + " size=" + std::to_string(bufferSize));

      zmq::message_t data;
      if (!socket.recv(data, zmq::recv_flags::none)) {
        emuExecLog("populate", "failed to receive payload frame");
        socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
        continue;
      }
      void* buffer = new uint8_t[bufferSize];
      memcpy(buffer, data.data(), bufferSize);

      buffers[name] = buffer;
      bufferSizes[name] = bufferSize;
      emuExecDebug("populate", "stored buffer '" + name + "' bytes=" + std::to_string(bufferSize));
      socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);
    } else if (command == "stream_in") {
      std::string name = root["name"].asString();
      emuExecDebug("stream_in", "name=" + name);
      zmq::message_t data;
      if (!socket.recv(data, zmq::recv_flags::none)) {
        emuExecLog("stream_in", "failed to receive payload frame");
        socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
        continue;
      }
      bool handled = false;

{% for s in stream_routes %}
{% for alias in s.names %}
      if (!handled && name == "{{ alias }}") {
        handled = true;
        emuExecDebug("stream_in", "alias '" + name + "' -> wire '{{ s.wire }}' ctype='{{ s.ctype }}' bytes=" + std::to_string(data.size()));
        for (size_t i = 0; i < data.size() / sizeof({{ s.ctype }}); i++) {
          {{ s.ctype }} value;
          memcpy(&value, static_cast<uint8_t*>(data.data()) + i * sizeof({{ s.ctype }}), sizeof({{ s.ctype }}));
          {{ s.wire }}.write(value);
        }
      }
{% endfor %}
{% endfor %}

      if (!handled) {
        emuExecLog("stream_in", "no alias match for '" + name + "'");
      }
      socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);
    } else if (command == "stream_out") {
      std::string name = root["name"].asString();
      size_t size = root["size"].asUInt64();
      std::vector<uint8_t> buffer(size, 0);
      bool handled = false;
      emuExecDebug("stream_out", "name=" + name + " size=" + std::to_string(size));

{% for s in stream_routes %}
{% for alias in s.names %}
      if (!handled && name == "{{ alias }}") {
        handled = true;
        emuExecDebug("stream_out", "alias '" + name + "' -> wire '{{ s.wire }}' ctype='{{ s.ctype }}' bytes=" + std::to_string(size));
        for (size_t i = 0; i < size / sizeof({{ s.ctype }}); i++) {
          {{ s.ctype }} value = {{ s.wire }}.read();
          memcpy(buffer.data() + i * sizeof({{ s.ctype }}), &value, sizeof({{ s.ctype }}));
        }
      }
{% endfor %}
{% endfor %}

      if (!handled) {
        emuExecLog("stream_out", "no alias match for '" + name + "'");
      }
      socket.send(zmq::message_t(buffer.data(), buffer.size()), zmq::send_flags::none);
    } else if (command == "call" || command == "start") {
      std::string functionName = root["function"].asString();
      bool isAsyncStart = (command == "start");
      bool callRejected = false;
      std::string callRejectReason;
      emuExecDebug("call", "function=" + functionName
                           + " mode=" + (isAsyncStart ? "start_async" : "call_sync"));

      if (emuManifestHasKernelMetadata) {
        auto kernelIt = kernelManifestRegistry.find(functionName);
        if (kernelIt == kernelManifestRegistry.end()) {
          callRejected = true;
          callRejectReason = "unknown_function";
          emuExecDebug("call", "manifest validation failed: function missing from kernelManifestRegistry");
        } else {
          const Json::Value& kernelMeta = kernelIt->second;
          emuExecDebug("call", "manifest kernel meta=" + emuJsonString(kernelMeta));
          if (!kernelMeta.get("callable", true).asBool()) {
            callRejected = true;
            callRejectReason = "kernel_not_callable";
            emuExecDebug("call", "manifest validation failed: kernel marked callable=false");
          } else {
            const Json::Value expectedArgs = kernelMeta["call_args"];
            const Json::Value providedArgs = root["args"];
            if (expectedArgs.isArray()) {
              if (!providedArgs.isObject()) {
                if (expectedArgs.size() != 0) {
                  callRejected = true;
                  callRejectReason = "missing_args_object";
                  emuExecDebug("call", "manifest validation failed: missing args object, expected "
                                    + std::to_string(expectedArgs.size()) + " args");
                }
              } else {
                size_t providedArgCount = 0;
                for (const auto& memberName : providedArgs.getMemberNames()) {
                  if (memberName.rfind("arg", 0) == 0) {
                    providedArgCount += 1;
                  }
                }
                emuExecDebug("call", "arg count provided=" + std::to_string(providedArgCount)
                                 + " expected=" + std::to_string(expectedArgs.size()));
                if (providedArgCount != static_cast<size_t>(expectedArgs.size())) {
                  callRejected = true;
                  callRejectReason = "arg_count_mismatch";
                  emuExecDebug("call", "manifest validation failed: arg_count_mismatch");
                }
              }

              if (!callRejected) {
                for (const auto& spec : expectedArgs) {
                  if (!spec.isObject()) continue;
                  std::string argKey = spec.get("arg", "").asString();
                  std::string expectedKind = spec.get("kind", "").asString();
                  if (argKey.empty()) continue;
                  emuExecDebug("call", "validating " + argKey + " expectedKind=" + expectedKind);
                  if (!providedArgs.isObject() || !providedArgs.isMember(argKey) || !providedArgs[argKey].isObject()) {
                    callRejected = true;
                    callRejectReason = "missing_" + argKey;
                    emuExecDebug("call", "manifest validation failed: missing arg object for " + argKey);
                    break;
                  }
                  std::string actualKind = providedArgs[argKey].get("type", "").asString();
                  emuExecDebug("call", argKey + " actualKind=" + actualKind
                                   + " payload=" + emuJsonString(providedArgs[argKey]));
                  if (!expectedKind.empty() && actualKind != expectedKind) {
                    callRejected = true;
                    callRejectReason = "arg_kind_mismatch_" + argKey;
                    emuExecDebug("call", "manifest validation failed: arg kind mismatch for " + argKey);
                    break;
                  }
                }
              }
            }
          }
        }
      }

      if (callRejected) {
        emuExecLog("call", "rejecting call to '" + functionName + "': " + callRejectReason);
        socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
        continue;
      }

      bool handledCall = false;
      std::function<void()> invokeCall;

{% for fc in function_calls %}
      if (functionName == "{{ fc.inst }}") {
        handledCall = true;
        emuExecDebug("call", "dispatch -> instance='{{ fc.inst }}' top='{{ fc.top }}'");
{% for line in fc.decode_blocks %}
        {{ line }}
{% endfor %}
        invokeCall = [&]() {
          {{ fc.top }}({{ fc.call_args | join(", ") }});
          emuExecDebug("call", "completed instance='{{ fc.inst }}'");
        };
      }
{% endfor %}

      if (handledCall) {
        auto invokeCallTracked = [
          invokeCall,
          functionName,
          &refreshKernelRegisterShadowFromManifest,
          &setKernelCtrlCompleted
        ]() {
          invokeCall();
          refreshKernelRegisterShadowFromManifest(functionName, true);
          setKernelCtrlCompleted(functionName);
        };

        if (isAsyncStart) {
          auto activeIt = activeCallFutures.find(functionName);
          if (activeIt != activeCallFutures.end()) {
            if (activeIt->second.valid()
                && activeIt->second.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready) {
              emuExecLog("call", "rejecting async start for busy kernel '" + functionName + "'");
              socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
              continue;
            }
            if (activeIt->second.valid()) {
              try {
                activeIt->second.get();
              } catch (const std::exception& e) {
                emuExecLog("call", std::string("prior async kernel '") + functionName
                                     + "' completed with error before restart: " + e.what());
              } catch (...) {
                emuExecLog("call", std::string("prior async kernel '") + functionName
                                     + "' completed with unknown error before restart");
              }
            }
            activeCallFutures.erase(activeIt);
          }

          try {
            setKernelCtrlRunning(functionName);
            refreshKernelRegisterShadowFromManifest(functionName, false);
            activeCallFutures[functionName] = std::async(std::launch::async, invokeCallTracked);
            emuExecDebug("call", "launched async instance='" + functionName + "'");
            socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);
          } catch (const std::exception& e) {
            emuExecLog("call", std::string("async launch failed for '") + functionName + "': " + e.what());
            socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
          } catch (...) {
            emuExecLog("call", std::string("async launch failed for '") + functionName + "' (unknown error)");
            socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
          }
        } else {
          try {
            setKernelCtrlRunning(functionName);
            refreshKernelRegisterShadowFromManifest(functionName, false);
            invokeCallTracked();
            socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);
          } catch (const std::exception& e) {
            emuExecLog("call", std::string("call failed for '") + functionName + "': " + e.what());
            socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
          } catch (...) {
            emuExecLog("call", std::string("call failed for '") + functionName + "' (unknown error)");
            socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
          }
        }
      } else {
        emuExecLog("call", "unknown call target '" + functionName + "'");
        socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
      }
    } else if (command == "wait") {
      std::string functionName = root["function"].asString();
      emuExecDebug("wait", "function=" + functionName);
      auto activeIt = activeCallFutures.find(functionName);
      if (activeIt == activeCallFutures.end() || !activeIt->second.valid()) {
        emuExecDebug("wait", "no active async call for '" + functionName + "' (no-op)");
        socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);
        continue;
      }

      try {
        activeIt->second.get();
        activeCallFutures.erase(activeIt);
        emuExecDebug("wait", "completed async wait for '" + functionName + "'");
        socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);
      } catch (const std::exception& e) {
        activeCallFutures.erase(activeIt);
        emuExecLog("wait", std::string("async kernel '") + functionName + "' failed: " + e.what());
        socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
      } catch (...) {
        activeCallFutures.erase(activeIt);
        emuExecLog("wait", std::string("async kernel '") + functionName + "' failed (unknown error)");
        socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
      }
    } else if (command == "read_register") {
      Json::Value response;
      std::string functionName = root["function"].asString();
      const Json::Value offsetVal = root["offset"];
      if (!offsetVal.isUInt() && !offsetVal.isInt()) {
        emuExecLog("read_register", "invalid or missing offset");
        response["error"] = "invalid_offset";
      } else {
        const uint32_t offset = static_cast<uint32_t>(offsetVal.asUInt());
        std::lock_guard<std::mutex> lock(kernelRegisterShadowMutex);
        auto kernelIt = kernelRegisterShadow.find(functionName);
        if (kernelIt == kernelRegisterShadow.end()) {
          emuExecLog("read_register", "unknown function '" + functionName + "'");
          response["error"] = "unknown_function";
          response["function"] = functionName;
        } else {
          auto regIt = kernelIt->second.find(offset);
          if (regIt == kernelIt->second.end()) {
            emuExecLog("read_register", "unknown register offset for '" + functionName
                          + "' offset=0x" + std::to_string(offset));
            response["error"] = "unknown_register";
            response["function"] = functionName;
            response["offset"] = offset;
          } else {
            response = Json::Value(static_cast<Json::UInt>(regIt->second));
            emuExecDebug("read_register", "function=" + functionName
                         + " offset=0x" + std::to_string(offset)
                         + " value=" + emuJsonString(response));
          }
        }
      }

      std::string responseStr = Json::writeString(Json::StreamWriterBuilder(), response);
      socket.send(zmq::message_t(responseStr.c_str(), responseStr.size()), zmq::send_flags::none);
    } else if (command == "fetch") {
      std::string type = root["type"].asString();
      Json::Value response;
      emuExecDebug("fetch", "type=" + type);

      if (type == "scalar") {
        std::string functionName = root["function"].asString();
        std::string arg = root["arg"].asString();
        bool hasOffsetHint = root.isMember("offset") && root["offset"].isUInt();
        Json::UInt offsetHint = hasOffsetHint ? root["offset"].asUInt() : 0;
        bool handledScalar = false;
        bool manifestRouteMatched = false;
        emuExecDebug("fetch", "scalar function=" + functionName + " arg=" + arg);

        if (emuManifestHasFetchMetadata) {
          const Json::Value fetchRoutes = emuManifest["fetch"]["scalar"];
          if (fetchRoutes.isArray()) {
            for (const auto& route : fetchRoutes) {
              if (!route.isObject()) continue;
              if (route.get("function", "").asString() != functionName) continue;
              if (route.get("arg", "").asString() != arg) continue;
              if (hasOffsetHint) {
                const Json::Value source = route["source"];
                if (!source.isObject() || !source.isMember("register_offset")
                    || !source["register_offset"].isUInt()) {
                  continue;
                }
                if (source["register_offset"].asUInt() != offsetHint) {
                  continue;
                }
              }
              manifestRouteMatched = true;
              emuExecDebug("fetch", "manifest route match " + emuJsonString(route));

              std::string kind = route.get("kind", "").asString();
              if (kind == "var") {
                std::string symbol = route.get("var_symbol", "").asString();
                auto it = fetchScalarRegistry.find(symbol);
                if (it != fetchScalarRegistry.end()) {
                  response = it->second();
                  handledScalar = true;
                  emuExecDebug("fetch", "manifest var route resolved symbol=" + symbol
                                   + " value=" + emuJsonString(response));
                  break;
                } else {
                  emuExecLog("fetch", "manifest route matched but symbol missing from fetchScalarRegistry: " + symbol);
                }
              } else if (kind == "var_u32_hi") {
                std::string symbol = route.get("var_symbol", "").asString();
                auto it = fetchScalarHiRegistry.find(symbol);
                if (it != fetchScalarHiRegistry.end()) {
                  response = it->second();
                  handledScalar = true;
                  emuExecDebug("fetch", "manifest var_u32_hi route resolved symbol=" + symbol
                                   + " value=" + emuJsonString(response));
                  break;
                } else {
                  emuExecLog("fetch", "manifest route matched but symbol missing from fetchScalarHiRegistry: " + symbol);
                }
              } else if (kind == "const_u32") {
                response = Json::Value(static_cast<Json::UInt>(route.get("value", 0).asUInt()));
                handledScalar = true;
                emuExecDebug("fetch", "manifest const_u32 route value=" + emuJsonString(response));
                break;
              } else {
                emuExecLog("fetch", "manifest route matched but kind unsupported: " + kind);
                break;
              }
            }
            if (!manifestRouteMatched) {
              emuExecDebug("fetch", "no manifest route matched scalar fetch request");
            }
          }
        }

        if (!handledScalar) {
          emuExecLog("fetch", "scalar fetch unresolved in manifest");
          response["error"] = "scalar_fetch_unresolved";
          response["function"] = functionName;
          response["arg"] = arg;
          if (hasOffsetHint) {
            response["offset"] = offsetHint;
          }
        }
      } else if (type == "buffer") {
        std::string name = root["name"].asString();
        emuExecDebug("fetch", "buffer name=" + name);
        if (buffers.find(name) != buffers.end()) {
          response = createJsonBuffer(static_cast<uint8_t*>(buffers[name]), bufferSizes[name]);
          emuExecDebug("fetch", "buffer fetch hit name=" + name + " bytes=" + std::to_string(bufferSizes[name]));
        } else {
          emuExecLog("fetch", "buffer fetch miss for name=" + name);
        }
      } else {
        emuExecLog("fetch", "unsupported fetch type=" + type);
      }

      std::string responseStr = Json::writeString(Json::StreamWriterBuilder(), response);
      emuExecDebug("fetch", "reply=" + responseStr);
      socket.send(zmq::message_t(responseStr.c_str(), responseStr.size()), zmq::send_flags::none);
    } else if (command == "exit") {
      emuExecLog("exit", std::string("received exit; fast_exit=") + (requireFastExitOnExit ? "true" : "false"));
      socket.send(zmq::message_t("OK", 2), zmq::send_flags::none);
      if (requireFastExitOnExit) {
        // Free-running autostart kernels (e.g. ap_ctrl_none stream-only kernels)
        // run in detached threads and may never return. Exiting main() would destroy
        // local hls::stream objects while those threads are still active.
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        emuExecLog("exit", "terminating via std::_Exit(0)");
        std::_Exit(0);
      }
      emuExecDebug("exit", "clean shutdown via return path");
      break;
    } else {
      emuExecLog("request", "unknown command '" + command + "'");
      socket.send(zmq::message_t("ERR", 3), zmq::send_flags::none);
    }
  }

  emuExecDebug("shutdown", "main() returning");
  return 0;
}
