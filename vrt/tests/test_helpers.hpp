#ifndef VRT_TEST_HELPERS_HPP
#define VRT_TEST_HELPERS_HPP

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>

class ScopedEnv {
   public:
    explicit ScopedEnv(const char* name, std::optional<std::string> value = std::nullopt)
        : name_(name) {
        const char* prev = std::getenv(name);
        if (prev) {
            oldValue_ = prev;
        }
        if (value) {
            setenv(name, value->c_str(), 1);
        } else {
            unsetenv(name);
        }
    }

    ~ScopedEnv() {
        if (oldValue_) {
            setenv(name_.c_str(), oldValue_->c_str(), 1);
        } else {
            unsetenv(name_.c_str());
        }
    }

    ScopedEnv(const ScopedEnv&) = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

   private:
    std::string name_;
    std::optional<std::string> oldValue_;
};

inline std::filesystem::path makeTempDir(const std::string& prefix) {
    std::string tmpl = (std::filesystem::temp_directory_path() / (prefix + "-XXXXXX")).string();
    char* result = mkdtemp(tmpl.data());
    if (!result) {
        throw std::runtime_error("Failed to create temp directory");
    }
    return result;
}

inline std::string writeTempFile(const std::filesystem::path& dir, const std::string& name,
                                 const std::string& content) {
    auto path = dir / name;
    std::filesystem::create_directories(path.parent_path());
    std::ofstream ofs(path);
    if (!ofs) {
        throw std::runtime_error("Failed to create temp file: " + path.string());
    }
    ofs << content;
    ofs.close();
    return path.string();
}

#endif  // VRT_TEST_HELPERS_HPP
