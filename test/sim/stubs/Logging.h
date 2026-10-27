#pragma once
#include <cstdio>
#define LOG_ERR(tag, ...) (std::fprintf(stderr, "[ERR %s] ", tag), std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#define LOG_INF(tag, ...) (std::fprintf(stderr, "[INF %s] ", tag), std::fprintf(stderr, __VA_ARGS__), std::fputc('\n', stderr))
#define LOG_DBG(tag, ...) ((void)0)
