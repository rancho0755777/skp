#ifndef __US_DAEMON_H__
#define __US_DAEMON_H__

#include <stdbool.h>
#include <syslog.h>

#include "../utils/utils.h"

__BEGIN_DECLS

extern bool isdaemon;
#define __syslog(level, log_type, fmt, ...) \
	syslog(level, \
		"[" #log_type "] [" XX_MODULE_NAME "] %s:%d[%s] - " fmt "\n", \
		__FILENAME__, __LINE__, __FUNCTION__, ##__VA_ARGS__)

#define __log_error(fmt, ...) \
	do { if (isdaemon) __syslog(LOG_ERR, error_color, fmt, ##__VA_ARGS__); \
		else log_error(fmt, ##__VA_ARGS__); } while (0)

#define __log_warn(fmt, ...) \
	do { if (isdaemon) __syslog(LOG_WARNING, warn_color, fmt, ##__VA_ARGS__); \
		else log_warn(fmt, ##__VA_ARGS__); } while (0)

#define __log_info(fmt, ...) \
	do { if (isdaemon) __syslog(LOG_INFO, info_color, fmt, ##__VA_ARGS__); \
		else log_info(fmt, ##__VA_ARGS__); } while (0)

#ifdef DEBUG
  #define __log_debug(fmt, ...) \
	do { if (isdaemon) __syslog(LOG_DEBUG, debug_color, fmt, ##__VA_ARGS__); \
		else log_debug(fmt, ##__VA_ARGS__); } while (0)
#else
  #define __log_debug(fmt, ...)
#endif

extern int daemonize(int argc, char *argv[]);

__END_DECLS

#endif
