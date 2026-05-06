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

#ifndef XSI_LOADER_HPP
#define XSI_LOADER_HPP

#include <dlfcn.h>

#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include "xsi.h"

namespace Xsi {

class SharedLibrary {
   public:
    typedef void* handle_type;
    typedef void* symbol_type;

    SharedLibrary() : _lib(0), _retain(false) {}
    ~SharedLibrary() { unload(); }
    operator bool() const { return (_lib != 0); }
    bool loaded() const { return (_lib != 0); }
    handle_type handle() const { return _lib; }
    const std::string& path() const { return _path; }
    const std::string& error() const { return _err; }
    bool load(const std::string& path) {
        unload();
        // reset the retain flag
        _retain = false;

        if (path.empty()) {
            _err =
                "Failed to load shared library. "
                "Path of the shared library is not specified.";
            return false;
        }
        std::string msg;
        bool ok = load_impl(path, msg);
        if (ok) {
            _path = path;
            _err.clear();
        } else {
            _err = "Failed to load shared library \"" + path + "\". " + msg;
        }
        return ok;
    }

    bool load_impl(const std::string& path, std::string& errmsg) {
        bool ok = true;
        _lib = dlopen(path.c_str(), RTLD_LAZY | RTLD_GLOBAL);
        char* err = dlerror();
        if (err != NULL) {
            errmsg = err;
            ok = false;
        }
        return ok;
    }

    void unload() {
        if (_lib) {
            if (!_retain) {
                dlclose(_lib);
            }
            _lib = 0;
        }
        _err.clear();
    }

    void retain() { _retain = true; }

    bool getsymbol(const std::string& name, symbol_type& sym) {
        std::string msg;
        bool ok = true;

        if (_lib == 0) {
            msg = "The shared library is not loaded.";
            ok = false;
        } else {
            dlerror();  // clear error
            sym = (void*)dlsym(_lib, name.c_str());
            char* err = dlerror();
            if (err != NULL) {
                msg = err;
                ok = false;
            }
        }

        if (ok) {
            _err.clear();
        } else {
            _err = "Failed to obtain symbol \"" + name + "\" from shared library. " + msg;
        }

        return ok;
    }

    symbol_type getfunction(const std::string& name) {
        symbol_type sym = NULL;
        return getsymbol(name, sym) ? sym : NULL;
    }

   private:
    // shared library is non-copyable
    SharedLibrary(const SharedLibrary&);
    const SharedLibrary& operator=(const SharedLibrary&);
    handle_type _lib;
    std::string _path;
    std::string _err;
    bool _retain;
};

class LoaderException : public std::exception {
   public:
    LoaderException(const std::string& msg) : _msg("ISim engine error: " + msg) {}

    virtual ~LoaderException() throw() {}

    virtual const char* what() const throw() { return _msg.c_str(); }

   private:
    std::string _msg;
};

class Loader {
   public:
    Loader(const std::string& dll_name, const std::string& simkernel_libname);
    ~Loader();

    bool isopen() const;
    void open(p_xsi_setup_info setup_info);
    void close();
    void run(XSI_INT64 step);
    void restart();
    int get_num_ports();
    float get_time_precision();
    int get_value(int port_number, void* value);
    int get_port_number(const char* port_name);
    int get_port_bits(int port_number);
    bool port_is_input(int port_number);
    const char* get_port_name(int port_number);
    void put_value(int port_number, const void* value);
    int get_status();
    const char* get_error_info();
    void trace_all();

   private:
    bool initialize();

    Xsi::SharedLibrary _design_lib;
    Xsi::SharedLibrary _simkernel_lib;
    std::string _design_libname;
    std::string _simkernel_libname;

    xsiHandle _design_handle;

    t_fp_xsi_open _xsi_open;
    t_fp_xsi_close _xsi_close;
    t_fp_xsi_run _xsi_run;
    t_fp_xsi_get_value _xsi_get_value;
    t_fp_xsi_put_value _xsi_put_value;
    t_fp_xsi_get_status _xsi_get_status;
    t_fp_xsi_get_error_info _xsi_get_error_info;
    t_fp_xsi_restart _xsi_restart;
    t_fp_xsi_get_port_number _xsi_get_port_number;
    t_fp_xsi_get_port_name _xsi_get_port_name;
    t_fp_xsi_trace_all _xsi_trace_all;
    t_fp_xsi_get_int _get_int_property;
    t_fp_xsi_get_int_port _get_int_port_property;

};  // class Loader

}  // namespace Xsi

#endif  // XSI_LOADER_HPP