#pragma once
#include "Common/CommonTypes.h"
#define _dbg_assert_(x) ((void)0)
#define _dbg_assert_msg_(x, ...) ((void)0)
#define _assert_(x) ((void)0)
#define _assert_msg_(x, ...) ((void)0)
#ifndef DEBUG_LOG
#define DEBUG_LOG(...) ((void)0)
#define VERBOSE_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define ERROR_LOG(t, ...) vfpu_log(__VA_ARGS__)
#define ERROR_LOG_REPORT(t, ...) vfpu_log(__VA_ARGS__)
#define ERROR_LOG_REPORT_ONCE(n, t, ...) vfpu_log(__VA_ARGS__)
#define WARN_LOG_REPORT(t, ...) vfpu_log(__VA_ARGS__)
#define WARN_LOG_REPORT_ONCE(n, t, ...) vfpu_log(__VA_ARGS__)
#endif
void vfpu_log(const char *fmt, ...);
#define DISALLOW_COPY_AND_ASSIGN(t)
