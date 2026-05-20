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

#ifndef VRT_LOGGER_HPP
#define VRT_LOGGER_HPP

#include <bitset>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>

namespace vrt {
namespace utils {
/**
 * @brief Enumeration for log message levels.
 *
 * This enum represents the different severity levels for log messages,
 * allowing for filtering of messages based on importance.
 */
enum class LogLevel {
    NONE,   ///< No logging
    WARN,   ///< Warning messages
    ERROR,  ///< Error messages
    INFO,   ///< Informational messages
    DEBUG   ///< Debug messages
};

/**
 * @brief Class for logging messages.
 *
 * The Logger class provides functionality for logging messages with different
 * severity levels, formatting, and output destinations.
 */
class Logger {
   public:
    /**
     * @brief Sets the current log level.
     *
     * @param level The minimum log level to display.
     *
     * Only messages with a severity level greater than or equal to the specified level will be
     * logged.
     */
    static void setLogLevel(LogLevel level);

    /**
     * @brief Sets the output destination for log messages.
     *
     * @param filename Path to the file where log messages will be written.
     *
     * If the file cannot be opened, output will be directed to standard output.
     */
    static void setOutput(const std::string& filename);

    /**
     * @brief Logs a message with the specified severity level.
     *
     * @param level The severity level of the message.
     * @param function The name of the function where the log message originated.
     * @param format Format string with placeholders for arguments.
     * @param args Variable arguments to be formatted into the message.
     *
     * This method logs a message with the specified severity level if it meets
     * the current log level threshold. The message is formatted according to the
     * format string and arguments, and includes timestamp, log level, and function name.
     *
     * Format specifiers:
     * - {} : General placeholder for any value
     * - {x} : Format value as hexadecimal
     * - {b} : Format value as binary
     * - {o} : Format value as octal
     */
    template <typename... Args>
    static void log(LogLevel level, const char* function, const char* format, Args... args) {
        if (level > currentLogLevel_) {
            return;
        }
        std::string color = getColor(level);
        std::string resetColor = "\033[0m";
        std::string levelStr = getLevelString(level);
        std::string currentTime = getCurrentTime();
        std::string message = formatString(format, std::forward<Args>(args)...);
        (*output_) << color << "[" << currentTime << "] [" << std::setw(5) << std::left << levelStr
                   << "] " << std::setw(80) << std::left << function << resetColor << ": "
                   << message << std::endl;
    }

   private:
    static std::unique_ptr<std::ofstream> fileStream_;  ///< File stream for log output.
    static std::ostream* output_;                       ///< Current output stream for logging.
    static LogLevel currentLogLevel_;                   ///< Current minimum log level threshold.

    /**
     * @brief Gets the color code for a log level.
     *
     * @param level The log level to get the color for.
     * @return ANSI color code string for the specified log level.
     */
    static std::string getColor(LogLevel level);

    /**
     * @brief Gets the string representation of a log level.
     *
     * @param level The log level to convert to string.
     * @return String representation of the log level.
     */
    static std::string getLevelString(LogLevel level);

    /**
     * @brief Gets the current time as a formatted string.
     *
     * @return Current time formatted as a string.
     */
    static std::string getCurrentTime();

    /**
     * @brief Helper function for string formatting (base case).
     *
     * @param oss Output string stream to append to.
     * @param format Format string to process.
     *
     * This is the base case for the recursive string formatting,
     * handling the end of the format string.
     */
    static inline void formatStringHelper(std::ostringstream& oss, const char* format) {
        while (*format) {
            if (*format == '{' && *(format + 1) == '}') {
                throw std::runtime_error("Too few arguments provided for format string");
            }
            oss << *format++;
        }
    }

    /**
     * @brief Helper function for string formatting (recursive case).
     *
     * @param oss Output string stream to append to.
     * @param format Format string to process.
     * @param value Value to insert at the next placeholder.
     * @param args Remaining arguments to process.
     *
     * This method recursively processes the format string, inserting values
     * at placeholders and handling special formatting codes.
     */
    template <typename T, typename... Args>
    static void formatStringHelper(std::ostringstream& oss, const char* format, T value,
                                   Args&&... args) {
        while (*format) {
            if (*format == '{' && *(format + 1) == '}') {
                oss << value;
                format += 2;
                formatStringHelper(oss, format, std::forward<Args>(args)...);
                return;
            } else if (*format == '{' && *(format + 1) == 'x' && *(format + 2) == '}') {
                oss << std::hex << std::showbase << value;
                format += 3;
                formatStringHelper(oss, format, std::forward<Args>(args)...);
                return;
            } else if (*format == '{' && *(format + 1) == 'b' && *(format + 2) == '}') {
                oss << "0b" << std::bitset<sizeof(T) * 8>(value);
                format += 3;
                formatStringHelper(oss, format, std::forward<Args>(args)...);
                return;
            } else if (*format == '{' && *(format + 1) == 'o' && *(format + 2) == '}') {
                oss << "0o" << std::oct << std::showbase << value;
                format += 3;
                formatStringHelper(oss, format, std::forward<Args>(args)...);
                return;
            }
            oss << *format++;
        }
        throw std::runtime_error("Too many arguments provided for format string");
    }

    /**
     * @brief Formats a string with placeholders.
     *
     * @param format Format string with placeholders.
     * @param args Arguments to insert at placeholders.
     * @return Formatted string.
     *
     * This method formats a string by replacing placeholders with the provided arguments.
     */
    template <typename... Args>
    static std::string formatString(const char* format, Args&&... args) {
        std::ostringstream oss;
        formatStringHelper(oss, format, std::forward<Args>(args)...);
        return oss.str();
    }
};

}  // namespace utils

}  // namespace vrt

#endif  // VRT_LOGGER_HPP