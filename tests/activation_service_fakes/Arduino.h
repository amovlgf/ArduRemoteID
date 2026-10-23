#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

class String {
public:
    String() = default;
    String(const char *text) : value(text) {}
    String(const std::string &text) : value(text) {}
    template<typename T, typename std::enable_if<std::is_integral<T>::value, int>::type = 0>
    explicit String(T number) : value(std::to_string(number)) {}
    String &operator+=(const String &text) { value += text.value; return *this; }
    String &operator+=(char c) { value += c; return *this; }
    const char *c_str() const { return value.c_str(); }
    friend String operator+(String left, const String &right) { return left += right; }
    friend String operator+(String left, char right) { return left += right; }
private:
    std::string value;
};

struct FakeESP { uint32_t getFreeHeap() const { return 40960; } };
extern FakeESP ESP;
