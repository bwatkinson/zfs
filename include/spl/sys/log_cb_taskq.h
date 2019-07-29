//////////////////////////////////////////
// Brian A. Added this file
//////////////////////////////////////////
/*
 * This header file defines three #defines:
 * HRTIME_TASKQ_STAMP     : Take counts of total number of threads active in
 *                          SPL taskqs
 * STATIC_PID_CAP         : Maximum number of PID's to take timestamps for
 *
 * Only the STATIC_PER_TASKQ_COUNT_CAP and STATIC_PID_CAP values should be adjusted.
 * They specify the total count numbers and maximum number of PID's to collect for.
 *
 * The log file written out will have the following format:
 *     1. Total number of taskq's counts collected for (int)
 *     2. For each taskq we then write out
 *        a. Length of the taskq name (int)
 *        b. The taskq name (taskq_name_len * char)
 *        c. Total number of counts for this taskq (int)
 *        d. All counts for the taskq (total_number_of_counts * int)
 *
 * All these counts can be restarted by destroying the Zpool and creating a new Zpool.
 */

#ifndef _LOG_CB_TASKQ_H
#define _LOG_CB_TASKQ_H

#include <sys/log_callbacks.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Change this variable in order to adjust collections of taskq counts
 *
 * STATIC_PER_TASKSQ_COUNT_CAP: This is used by hrtime_count_add_count and
 *     specifies the total number of counts to collect per SPL taskq
 */
#define STATIC_PER_TASKQ_COUNT_CAP 5000 

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
 * For each row for a taskq the first number is the total number
 * of thread counts collected followed by the thread counts collected
 * hence the + 1
 */
#define STATIC_COL_CAP_TASKQ (STATIC_PER_TASKQ_COUNT_CAP + 1)

typedef struct cb_func_spl_taskq_args_s
{
    const char *taskq_name;
    int num_threads;
} cb_spl_taskq_args_t;

typedef struct cb_spl_taskq_ctor_args_s
{
    char **taskq_names;
    int num_taskqs;
} cb_spl_taskq_ctor_args_t;

/*************************************************************/
/*                    Function Declartions                   */
/*************************************************************/
cb_spl_taskq_args_t create_cb_spl_taskq_args(const char *taskq_name,
                                             int num_threads);

cb_spl_taskq_ctor_args_t *create_init_cb_spl_taskq_ctor_args(int num_taskqs);

void destroy_cb_spl_taskq_ctor_args(cb_spl_taskq_ctor_args_t *ctor_args);

void add_taskq_to_cb_spl_taskq_ctor_args(cb_spl_taskq_ctor_args_t *ctor_args,
                                         char *taskq_nm, 
                                         int offset);

/*************************************************************/
/*       External Structs Defined/Exported in .c file        */
/*************************************************************/
extern log_callback_t log_cb_spl_taskq; 

#ifdef __cplusplus
}
#endif

#endif /* _LOG_CB_TASKQ_H */
