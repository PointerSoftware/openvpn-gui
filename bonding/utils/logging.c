/**
 * @file logging.c
 * @brief Logging system for bonding module
 */

#include <stdio.h>
#include <stdarg.h>
#include <time.h>
#include <windows.h>

/* Log levels */
#define BONDING_LOG_DEBUG 0
#define BONDING_LOG_INFO  1
#define BONDING_LOG_WARN  2
#define BONDING_LOG_ERROR 3

/* External functions from main.h and misc.h */
extern void MsgToEventLog(WORD type, wchar_t *format, ...);
extern WCHAR* Widen(const char *utf8);

#ifdef DEBUG
extern void PrintDebug(TCHAR *format, ...);
#else
#define PrintDebug(...) do {} while(0)
#endif

/**
 * @brief Log a message with specified level
 * @param level Log level (DEBUG, INFO, WARN, ERROR)
 * @param format Format string (printf-style)
 * @param ... Variable arguments
 */
void bonding_log(int level, const char *format, ...)
{
    va_list args;
    char message[512];
    WCHAR *wmessage;
    time_t now;
    struct tm *timeinfo;
    char timestamp[64];
    WORD event_type;
    
    if (!format)
        return;
    
    /* Get timestamp */
    time(&now);
    timeinfo = localtime(&now);
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S", timeinfo);
    
    /* Format message */
    va_start(args, format);
    vsnprintf(message, sizeof(message) - 1, format, args);
    message[sizeof(message) - 1] = '\0';
    va_end(args);
    
    /* Determine event log type */
    switch (level) {
        case BONDING_LOG_ERROR:
            event_type = EVENTLOG_ERROR_TYPE;
            break;
        case BONDING_LOG_WARN:
            event_type = EVENTLOG_WARNING_TYPE;
            break;
        case BONDING_LOG_INFO:
            event_type = EVENTLOG_INFORMATION_TYPE;
            break;
        case BONDING_LOG_DEBUG:
        default:
            event_type = EVENTLOG_INFORMATION_TYPE;
            break;
    }
    
    /* Log to event log for ERROR and WARN levels */
    if (level >= BONDING_LOG_WARN) {
        wmessage = Widen(message);
        if (wmessage) {
            MsgToEventLog(event_type, L"[Bonding] %hs: %ls", timestamp, wmessage);
            free(wmessage);
        }
    }
    
    /* Print debug messages when DEBUG is defined */
#ifdef DEBUG
    {
        WCHAR *wformat = Widen(format);
        if (wformat) {
            va_start(args, format);
            PrintDebug(L"[Bonding] %hs: ", timestamp);
            /* Note: PrintDebug doesn't support va_list directly, so we use the formatted message */
            WCHAR *wmsg = Widen(message);
            if (wmsg) {
                PrintDebug(L"%ls", wmsg);
                free(wmsg);
            }
            va_end(args);
            free(wformat);
        }
    }
#endif
}
