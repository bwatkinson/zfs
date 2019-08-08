//////////////////////////////////////////
// Brian A. Added this file
//////////////////////////////////////////
/*
 * This header file defines three #defines:
 * STATIC_PIPELINE_PID_CAP  : Maximum number of pipeline stages to collect per PID
 * STATIC_PID_CAP           : Maximum number of PID's to take timestamps for
 *
 * Only the STATIC_LIST_CAP and STATIC_PIPELINE_PID_CAP values should be adjusted. 
 * They specify the total count numbers and maximum number of PID's to collect for.
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

#ifndef _LOG_CB_ZIO_PIPELINE_H
#define _LOG_CB_ZIO_PIPELINE_H

#include <sys/log_callbacks.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Change this variable in order to adjust collections of
 * timestamps
 *
 * STATIC_PIPELINE_PID_CAP: This is used by hrtime_add_timestamp_pipeline_stages
 *     and specifies the total number of ZIO pipeline stage timestamps to collect 
 *     for each PID in the ZIO pipeline.
 */
#define STATIC_PIPELINE_PID_CAP 50000

/****************************************************************/
/****************************************************************/
/*          NEVER MANUALLY CHANGE THIS VARIABLE!!!!!            */
/****************************************************************/
/****************************************************************/
/*
 * For each row for a PID the first number is the current offset
 * into the row also we allow for extra buffer space to always end
 * on zio_done stage hence the + 50
 */
#define STATIC_PIPELINE_COL_CAP (STATIC_PIPELINE_PID_CAP + 50)


/*
 * Helper macros and defines
 */
#define ZIOSTAGE_TO_OFFSET(s) \
   highbit64((s)) - 1 

#define ZIO_PIPELINE_STAGES 25

typedef unsigned int zio_stage;

typedef struct cb_zio_pipeline_args_s
{
    unsigned int pid;
    zio_stage io_stage;
} cb_zio_pipeline_args_t;

/*************************************************************/
/*                    Function Declartions                   */
/*************************************************************/
cb_zio_pipeline_args_t create_cb_zio_pipeline_args(zio_t *zio,
                                                   zio_stage io_stage);

log_callback_t *create_cb_zio_pipeline(void);
void destroy_cb_zio_pipeline(log_callback_t *cb_zio_pipeline);

#ifdef __cplusplus
}
#endif

#endif /* _LOG_CB_ZIO_PIPELINE_H */
