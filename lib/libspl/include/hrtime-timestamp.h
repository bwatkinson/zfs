//////////////////////////////////////////
// Brian A. Added this file
//////////////////////////////////////////
/*
 * This header file defines three #defines:
 * HRTIME_PIPELINE_STAMP  : Take timestamps for ZIO pipeline stages per PID
 * HRTIME_CALL_SITE_STAMP : Take timestamps for callsite for multiple PID's
 * HRTIME_TASKQ_STAMP     : Take counts of total number of threads active in
 *                          SPL taskqs
 * STATIC_PID_CAP         : Maximum number of PID's to take timestamps for when
 *                          HRTIME_PIPELINE_STAMP or HRTIME_CALL_SITE_STAMP
 *                          are defined
 *
 * Only the STATIC_LIST_CAP, STATIC_PIPELINE_PID_CAP, STATIC_PER_TASKQ_COUNT_CAP,
 * and STATIC_PID_CAP values should be adjusted. They specify the total count
 * numbers and maximum number of PID's to collect for.
 * The functions declared in this header are  defined in module/spl/spl-hrtime-timestamps.c. 
 * That file can be updated to define how many call sites collection will be done for
 * as well the log files the data will be written out to. There is a corresponding
 * header file in spl/include/sys/hrtime_spl_timestamp.h that's values should
 * be updated to match the values in this header file if they are changed. 
 *
 * The log files written out will have the following formats if the variables
 * are defined.
 * HRTIME_PIPELINE_STAMP:
 *     1. Total number of PID's pipeline stage timestamps were collected for (long long)
 *     2. Total number of possible pipeline stages (long long)
 *     3. For each PID we then write out
 *        a. PID (long long)
 *        b. For each pipeline stage collected for PID we write out
 *           i.   Length of the pipeline stage name (int)
 *           ii.  The name of the pipeline stage (pipeline_stage_name_len * char)
 *           iii. Total number of timestamps collected for that stage (long long)
 *           iv.  Timestamps collected for this pipeline stage (total_number_of_timestamps * long long) 
 * HRTIME_CALL_SITE_STAMP:
 *     1. Total number of PID's timestamps were collected for at this call site (long long)
 *     2. Length of the call site name (int)
 *     3. Name of the call site (call_site_name_len * char)
 *     4. For each PID we then write out:
 *        a. PID (long long)
 *        b. Total number of timestamps collected for this PID (long long)
 *        c. Timestamps collected for this PID at the call site (total_number_of_timestamps * long long)
 * HRTIME_TASKQ_STAMP:
 *     1. Total number of taskq's counts collected for (int)
 *     2. For each taskq we then write out
 *        a. Length of the taskq name (int)
 *        b. The taskq name (taskq_name_len * char)
 *        c. Total number of counts for this taskq (int)
 *        d. All counts for the taskq (total_number_of_counts * int)
 *
 * All timestamps collected will be in microseconds.
 *
 * All these counts can be restarted by destroying the Zpool and creating a new Zpool.
 */

#ifndef _LIBSPL_HRTIME_TIMESTAMP_H
#define _LIBSPL_HRTIME_TIMESTAMP_H

#define HRTIME_PIPELINE_STAMP
#define HRTIME_CALL_SITE_STAMP
#define HRTIME_TASKQS_STAMP

/*
 * Change this variables in order to adjust collections of
 * timestamps
 *
 * STATIC_LIST_CAP: This is used by hrtime_add_timestamp_call_sites
 *     and specifies the total number of timestamps to collect for each
 *     call site for all PID's
 * STATIC_PIPELINE_PID_CAP: This is used by hrtime_add_timestamp_pipeline_stages
 *     and specifies the total number of ZIO pipeline stage timestamps to collect 
 *     for each PID in the ZIO pipeline.
 * STATIC_PER_TASKSQ_COUNT_CAP: This is used by hrtime_count_add_count and
 *     specifies the total number of counts to collect per SPL taskq
 */
#define STATIC_LIST_CAP 110000

#define STATIC_PIPELINE_PID_CAP 50000

#define STATIC_PER_TASKQ_COUNT_CAP 5000 

/*
 * Change this variable in order to set the maximum number of PID's
 * to collect timestamps for
 */
#define STATIC_PID_CAP 48

/****************************************************************/
/****************************************************************/
/*         NEVER MANUALLY CHANGE THESE VARIABLES!!!!!           */
/****************************************************************/
/****************************************************************/
/* 
 * For each row for a PID the first number is the PID and the
 * second is the current offset into the row hence the + 2
 */
#define STATIC_COL_CAP (STATIC_LIST_CAP + 2)

/*
 * For each row for a PID the first number is the current offset
 * into the row also we allow for extra buffer space to always end
 * on zio_done stage hence the + 50
 */
#define STATIC_PIPELINE_COL_CAP (STATIC_PIPELINE_PID_CAP + 50)

/*
 * For each row for a taskq the first number is the total number
 * of thread counts collected followed by the thread counts collected
 * hence the + 1
 */
#define STATIC_COL_CAP_TASKQ (STATIC_PER_TASKQ_COUNT_CAP + 1)
/****************************************************************/
/****************************************************************/



/*
 * Helper macros and defines
 */
#define ZIOSTAGE_TO_OFFSET(s) \
   highbit64((s)) - 1 

#define ZIO_IS_READ 0x1513AD

#define ZIO_PIPELINE_STAGES 25

typedef unsigned int zio_stage;

/*************************************************************/
/*                    Function Declartions                   */
/*************************************************************/
void hrtime_timestamp_init(void);
void hrtime_timestamp_fini(void);
void hrtime_taskq_count_init(char **taskq_names, int num_taskqs);
void hrtime_taskq_count_fini(void);
void hrtime_taskq_count_add_count(char *taskq_name, int num_threads);
void hrtime_taskq_count_dump(void);
void hrtime_add_timestamp_pipeline_stages(int curr_pid, zio_stage io_stage);
void hrtime_add_timestamp_call_sites(int curr_pid, unsigned int offset, const char *call_site_name);

#endif /* _LIBSPL_HRTIME_TIMESTAMP_H */
