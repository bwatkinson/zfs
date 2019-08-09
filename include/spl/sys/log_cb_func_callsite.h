/*
 * The log files written out will have the following format:
 *     1. Total number of PID's timestamps were collected for (long long)
 *     2. The length of the function call site name (int)
 *     3. The call site name (sizeof(char) * length of call site name)
 *     4. For each PID we then write out
 *        a. PID (long long)
 *        b. Total number of time stamps collected for this PID (long long)
 *        c. The corresponding time stamps for this PID (long long * times stamps collected)
 *
 * All timestamps collected will be in microseconds.
 */

#ifndef _LOG_CB_FUNC_CALLSITE_H
#define _LOG_CB_FUNC_CALLSITE_H

#include <sys/log_callbacks.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 
 * For each row for a PID the first number is the PID and the
 * second is the current offset into the row hence the + 2
 */
#define STATIC_COL_CAP(c) ((c) + 2)

typedef struct cb_func_callsite_args_s
{
    unsigned int pid;
    unsigned int offset;
    const char *call_site_name;
} cb_func_callsite_args_t;

/*************************************************************/
/*                    Function Declartions                   */
/*************************************************************/
cb_func_callsite_args_t create_cb_func_callsite_args(zio_t *zio, 
                                                     unsigned int offset, 
                                                     const char *call_site_name); 

log_callback_t *create_cb_func_callsite(void);
void destroy_cb_func_callsite(log_callback_t *cb_func_callsite);

#ifdef __cplusplus
}
#endif

#endif /* _LOG_CB_FUNC_CALLSITE_H */
