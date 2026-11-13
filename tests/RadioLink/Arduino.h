#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <string>
#include <deque>
#include <utility>
class String {
 public:
  std::string value;
  String() = default;
  String(const char* text) : value(text) {}
  String(std::string text) : value(std::move(text)) {}
  unsigned int length() const { return value.size(); }
  bool startsWith(const char* prefix) const { return value.rfind(prefix, 0) == 0; }
  int lastIndexOf(char c) const { auto p = value.rfind(c); return p == std::string::npos ? -1 : int(p); }
  String substring(unsigned int start) const { return value.substr(start); }
  String substring(unsigned int start, unsigned int end) const { return value.substr(start, end - start); }
  const char* c_str() const { return value.c_str(); }
  char operator[](unsigned int index) const { return value[index]; }
  String& operator+=(char c) { value += c; return *this; }
  void trim() {
    auto first = value.find_first_not_of(" \r\n\t"), last = value.find_last_not_of(" \r\n\t");
    value = first == std::string::npos ? "" : value.substr(first, last - first + 1);
  }
};
inline String operator+(const String& a, const String& b) { return a.value + b.value; }
inline unsigned long testTime = 0;
inline unsigned long millis() { return testTime; }
constexpr int HIGH = 1, LOW = 0, INPUT_PULLUP = 2, SERIAL_8N1 = 0;
inline int auxState = HIGH;
inline int digitalRead(int) { return auxState; }
inline void pinMode(int, int) {}
struct TestSerial {
  std::deque<char> rx;
  std::string tx;
  void setRxBufferSize(int) {}
  void begin(int, int, int, int) {}
  int available() { return rx.size(); }
  char read() { char c = rx.front(); rx.pop_front(); return c; }
  size_t write(const uint8_t* data, size_t length) { tx.append(reinterpret_cast<const char*>(data), length); return length; }
  void println(const char*) {}
};
inline TestSerial Serial, Serial2;
