#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <cstdio>
#include <cstdarg>
#define IRAM_ATTR
class String {
    std::string value_;
public:
    String() = default;
    String(const char* s) : value_(s) {}
    String(const std::string& s) : value_(s) {}
    size_t length() const { return value_.size(); }
    const char* c_str() const { return value_.c_str(); }
    char operator[](size_t n) const { return value_[n]; }
    bool startsWith(const char* s) const { return value_.find(s) == 0; }
    String substring(size_t n) const { return String(value_.substr(n)); }
    bool operator==(const char* s) const { return value_ == s; }
};
class Stream {
public:
    std::string output;
    void begin(unsigned) {}
    void print(const char* s) { output += s; }
    template<class T> void print(T value) { output += std::to_string(value); }
    template<class T> void println(T value) { print(value); output += '\n'; }
    void println(const char* s) { output += s; output += '\n'; }
    void printf(const char* format, ...) {
        char buffer[2048]; va_list args; va_start(args, format);
        vsnprintf(buffer, sizeof(buffer), format, args); va_end(args); output += buffer;
    }
};
extern Stream Serial;
extern unsigned long testMillis;
inline unsigned long millis() { return testMillis; }
inline void delay(unsigned long ms) { testMillis += ms; }
inline uint32_t esp_random() { return 123; }
