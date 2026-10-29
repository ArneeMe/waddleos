#pragma once
// Logs go to Android's log (adb logcat -s waddle).
#include <android/log.h>
#define LOG_ERR(tag, ...) __android_log_print(ANDROID_LOG_ERROR, "waddle", __VA_ARGS__)
#define LOG_INF(tag, ...) __android_log_print(ANDROID_LOG_INFO, "waddle", __VA_ARGS__)
#define LOG_DBG(tag, ...) ((void)0)
