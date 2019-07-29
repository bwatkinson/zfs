//////////////////////////////////////////
// Brian A. Added this file
//////////////////////////////////////////
/*
 * This header file defines:
 * HRTIME_CALL_SITE_STAMP : Take timestamps for callsite for multiple PID's
 * STATIC_PID_CAP         : Maximum number of PID's to take timestamps for
 *
 * Only the STATIC_LIST_CAP and STATIC_PID_CAP values should be adjusted. They specify
 * the total count numbers and maximum number of PID's to collect for.
 *
 * The log files written out will have the following format:
 *     1. Total number of PID's pipeline stage timestamps were collected for (long long)
 *     2. Total number of possible pipeline stages (long long)
 *     3. For each PID we then write out
 *        a. PID (long long)
 *        b. For each pipeline stage collected for PID we write out
 *           i.   Length of the pipeline stage name (int)
 *           ii.  The name of the pipeline stage (pipeline_stage_name_len * char)
 *           iii. Total number of timestamps collected for that stage (long long)
 *           iv.  Timestamps collected for this pipeline stage (total_number_of_timestamps * long long) 
 *
 * All timestamps collected will be in microseconds.
 *
 * All these counts can be restarted by destroying the Zpool and creating a new Zpool.
 */

#ifndef _LOG_CB_FUNC_CALLSITE_H
#define _LOG_CB_FUNC_CALLSITE_H

#include <sys/log_callbacks.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Change this variables in order to adjust collections of
 * timestamps
 *
 * STATIC_LIST_CAP: This is used by hrtime_add_timestamp_call_sites
 *     and specifies the total number of timestamps to collect for each
 *     call site for all PID's
 */
#define STATIC_LIST_CAP 110000

/*
 * Change this variable in order to set the maximum number of PID's
 * to collect timestamps for
 */
#define STATIC_PID_CAP 48

/****************************************************************/
/****************************************************************/
/*          NEVER MANUALLY CHANGE THIS VARIABLE!!!!!            */
/****************************************************************/
/****************************************************************/
/* 
 * For each row for a PID the first number is the PID and the
 * second is the current offset into the row hence the + 2
 */
#define STATIC_COL_CAP (STATIC_LIST_CAP + 2)

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

/*************************************************************/
/*       External Structs Defined/Exported in .c file        */
/*************************************************************/
extern log_callback_t log_cb_func_callsite;

#ifdef __cplusplus
}
#endif

#endif /* _LOG_CB_FUNC_CALLSITE_H */
