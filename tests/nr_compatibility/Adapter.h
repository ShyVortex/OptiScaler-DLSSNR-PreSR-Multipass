#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <format>
#include <cstdio>
template<class... T> void CompatibilityLog(std::format_string<T...> fmt,T&&... args) {
    auto text=std::format(fmt,std::forward<T>(args)...); std::puts(text.c_str());
}
#define LOG_INFO(...) CompatibilityLog(__VA_ARGS__)
#define LOG_ERROR(...) CompatibilityLog(__VA_ARGS__)
#define LOG_WARN(...) CompatibilityLog(__VA_ARGS__)
