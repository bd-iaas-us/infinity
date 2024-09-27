#ifndef __LOG_H__
#define __LOG_H__

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

// give me some color see see

#define APP_NAME "infinity"

#define DEBUG(fmt, ...) \
        do { spdlog::get(APP_NAME)->debug(fmt, ##__VA_ARGS__); } while (0)
#define ERROR(fmt, ...) \
        do { spdlog::get(APP_NAME)->error("[{}:{}] " fmt, __FILE__, __LINE__, ##__VA_ARGS__); } while (0)
#define INFO(fmt, ...) \
        do { spdlog::get(APP_NAME)->info(fmt, ##__VA_ARGS__); } while (0)

#define _stringify(_x) #_x
#define stringify(_x) _stringify(_x)

#endif // __LOG_H__
