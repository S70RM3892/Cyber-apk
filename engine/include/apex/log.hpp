// Logging that goes to logcat on Android and stderr elsewhere.
#pragma once

#if defined(__ANDROID__)
#include <android/log.h>
#define APEX_LOGI(...) __android_log_print(ANDROID_LOG_INFO, "CyberApex", __VA_ARGS__)
#define APEX_LOGW(...) __android_log_print(ANDROID_LOG_WARN, "CyberApex", __VA_ARGS__)
#define APEX_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "CyberApex", __VA_ARGS__)
#else
#include <cstdio>
#define APEX_LOG_IMPL_(tag, ...)                   \
    do {                                           \
        std::fprintf(stderr, "[%s] ", tag);        \
        std::fprintf(stderr, __VA_ARGS__);         \
        std::fputc('\n', stderr);                  \
    } while (0)
#define APEX_LOGI(...) APEX_LOG_IMPL_("I", __VA_ARGS__)
#define APEX_LOGW(...) APEX_LOG_IMPL_("W", __VA_ARGS__)
#define APEX_LOGE(...) APEX_LOG_IMPL_("E", __VA_ARGS__)
#endif
