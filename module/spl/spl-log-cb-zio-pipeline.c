////////////////////////////////////
// Brian A. Added this file
////////////////////////////////////

/*
 * This file defines the following #defines and static variables
 * that can be udpated:
 * LOG_FILE_PIPELINE         : Path and name of log file dumped
 */ 

#include <sys/log_cb_zio_pipeline.h>
#include <linux/file_compat.h>
#include <sys/zio.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#include <sys/vmem.h>
#include <sys/mutex.h>
#include <sys/strings.h>

/* 
 * Change this variable in order to set a default location where
 * log file will be dumped
 */
const static char *LOG_FILE_PIPELINE = "/var/log/hrtime_zio_pipeline.log";

/* 
 * Just a simple debugging enable (1) disable (0)
 */
#define CB_FUNC_ZIO_PIPELINE_DEBUG 1

/* 
 * Ignoring PID's with only 10% of requested timestamps
 */
#define MIN_TS_TO_IGNORE (STATIC_PIPELINE_PID_CAP / 10)

/*
 * Determine if ZIO stage is equal to zio_done
 */
#define IS_ZIO_DONE_STAGE(s) \
    ZIOSTAGE_TO_OFFSET((s)) == (ZIO_PIPELINE_STAGES - 1) ? 1 : 0

   
static const char *zio_pipeline_stage_strs[ZIO_PIPELINE_STAGES] =
{
    "zio_NULL",              // [0]
    "zio_read_bp_init",      // [1]  1 << 1
    "zio_write_bp_init",     // [2]  1 << 2
    "zio_free_bp_init",      // [3]  1 << 3
    "zio_issue_async",       // [4]  1 << 4
    "zio_write_compress",    // [5]  1 << 5
    "zio_encrypt",           // [6]  1 << 6
    "zio_checksum_generate", // [7]  1 << 7
    "zio_nop_write",         // [8]  1 << 8
    "zio_ddt_read_start",    // [9]  1 << 9
    "zio_ddt_read_done",     // [10] 1 << 10
    "zio_ddt_write",         // [11] 1 << 11
    "zio_ddt_free",          // [12] 1 << 12
    "zio_gang_assemble",     // [13] 1 << 13
    "zio_gang_issue",        // [14] 1 << 14
    "zio_dva_throttle",      // [15] 1 << 15
    "zio_dva_allocate",      // [16] 1 << 16
    "zio_dva_free",          // [17] 1 << 17
    "zio_dva_claim",         // [18] 1 << 18
    "zio_ready",             // [19] 1 << 19
    "zio_vdev_io_start",     // [20] 1 << 20
    "zio_vdev_io_done",      // [21] 1 << 21
    "zio_vdev_io_assess",    // [22] 1 << 22
    "zio_checksum_verify",   // [23] 1 << 23
    "zio_done"               // [24] 1 << 24
};

static const char *no_stage = "NO_STAGE";

/*
 * Initialization variable that is set and checked
 */
static int cb_zio_pipeline_initialized = 0;


/*******************************************************/
/* Struct definitions for ZIO pipeline                 */
/*******************************************************/
typedef struct pid_io_stages_s
{
    hrtime_t curr_pid;
    const char *zio_pipeline_stage[ZIO_PIPELINE_STAGES];
    hrtime_t io_stages[ZIO_PIPELINE_STAGES][STATIC_PIPELINE_COL_CAP];
    int num_of_ts_collected;
    boolean_t done_collecting;
} pid_io_stages_t;

typedef struct all_pid_io_stages_s
{
    pid_io_stages_t *pid_io_stages;
    int dumped;
    hrtime_t total_pids;
    int row_select_iter;
    struct file *dump_file;
    loff_t file_offset;
    kmutex_t lock;
} all_pid_io_stages_t;

/*****************************************/
/*       Static timestamp structs        */
/*****************************************/
static all_pid_io_stages_t all_pid_io_stages;

/**************************************************************/
/*                Static Function Defintions                  */
/**************************************************************/
static boolean_t cb_zio_pipeline_all_pids_done(void)
{
    int i;
    boolean_t done_collecting = B_TRUE;

    for (i = 0; i < STATIC_PID_CAP; i++) {
        if (all_pid_io_stages.pid_io_stages[i].curr_pid && 
           !all_pid_io_stages.pid_io_stages[i].done_collecting) {
            if (all_pid_io_stages.pid_io_stages[i].num_of_ts_collected <= MIN_TS_TO_IGNORE) {
                /* 
                 * We have arrived here because a particular PID has collected all the 
                 * required pipeline timestamps. However, we may have a few stray PID's
                 * that were collecting timestamps that is below the 10% threshold, so
                 * we just ignore these PID's
                 */
                continue;
            }
            done_collecting = B_FALSE;
            break;
        }
    }
    return done_collecting;
}

static void cb_zio_pipeline_dtor(void  *lock_held)
{
#if CB_FUNC_ZIO_PIPELINE_DEBUG
    int i;
    cmn_err(CE_WARN, "Inside %s", __func__);
#endif 
    if (!(*((boolean_t *)lock_held))) {
        mutex_enter(&all_pid_io_stages.lock);
    }

    if (cb_zio_pipeline_initialized) {
        cb_zio_pipeline_initialized = 0;
#if CB_FUNC_ZIO_PIPELINE_DEBUG
        cmn_err(CE_WARN, "Contents of the pipeline counts with %d total pids",
                all_pid_io_stages.total_pids);
        for (i = 0; i < STATIC_PID_CAP; i++) {
            if (all_pid_io_stages.pid_io_stages[i].curr_pid &&
               all_pid_io_stages.pid_io_stages[i].num_of_ts_collected <= MIN_TS_TO_IGNORE) {
                cmn_err(CE_WARN, "PID = %u with num_of_ts_collected %d <= %d so will not be in log",
                        all_pid_io_stages.pid_io_stages[i].curr_pid,
                        all_pid_io_stages.pid_io_stages[i].num_of_ts_collected,
                        MIN_TS_TO_IGNORE);
            } else if (all_pid_io_stages.pid_io_stages[i].curr_pid) { 
                cmn_err(CE_WARN, "PID = %u with num_of_ts_collected = %d",
                        all_pid_io_stages.pid_io_stages[i].curr_pid,
                        all_pid_io_stages.pid_io_stages[i].num_of_ts_collected);
            }
        }
#endif /* CB_FUNC_ZIO_PIPELINE_DEBUG */
        kmem_free(all_pid_io_stages.pid_io_stages, sizeof(pid_io_stages_t) * STATIC_PID_CAP);
        all_pid_io_stages.pid_io_stages = NULL;
    }
    mutex_exit(&all_pid_io_stages.lock);
}

/**
 * cb_zio_pipeline_ctor -
 *
 * This function initializes the variables needed to use the function cb_zio_pipeline_cb.
 * If this function is not called, no collections will be done even if the
 * log_callback_t cb_func is called.
 */
static void cb_zio_pipeline_ctor(void *ctor_args)
{
    int i;
    int j;
   
    /* There is no constructor args needed, so always pass NULL to this */
    VERIFY3P(ctor_args, ==, NULL);
    
    if (!cb_zio_pipeline_initialized) {
        all_pid_io_stages.pid_io_stages = NULL;
        all_pid_io_stages.pid_io_stages = vmem_zalloc(sizeof(pid_io_stages_t) * STATIC_PID_CAP, KM_SLEEP);
        ASSERT3P(all_pid_io_stages.pid_io_stages, !=, NULL);
        all_pid_io_stages.dumped = 0;
        all_pid_io_stages.total_pids = 0;
        all_pid_io_stages.row_select_iter = 0;
        all_pid_io_stages.dump_file = NULL;
        all_pid_io_stages.file_offset = 0;
        mutex_init(&all_pid_io_stages.lock, NULL, MUTEX_DEFAULT, NULL);
        for (i = 0; i < STATIC_PID_CAP; i++) {
            for (j = 0; j < ZIO_PIPELINE_STAGES; j++) {
                all_pid_io_stages.pid_io_stages[i].zio_pipeline_stage[j] = no_stage;
            }
        }
        cb_zio_pipeline_initialized = 1;
#if CB_FUNC_ZIO_PIPELINE_DEBUG
        cmn_err(CE_WARN, "Initialized the data for timestamping ZIO pipeline in %s", __func__);
#endif /* CB_FUNC_ZIO_PIPELINE_DEBUG */
    }
}

/**
 * cb_zio_pipeline_cb -
 * @cb_args : Arguments passed to cb_func, see create_cb_zio_pipeline_args 
 *
 * This function adds a timestamp for the PID for the
 * ZIO pipeline stage. After the PID has collected 
 * STATIC_PIPELINE_PID_CAP timetamps for each of the pipeline
 * stages this function is called for, it will stop collection
 * with the caveat that it will only stop collection on the
 * ZIO_STAGE_DONE io_stage. The last PID that has collected 
 * STATIC_PIPELINE_PID_CAP pipeline stage timestamps will then
 * write out all PID's pipeline stage timetamps to the log file
 * specified by LOG_FILE_PIPELINE.
 */
static void cb_zio_pipeline_cb(void *cb_args)
{
    int pid_arr_offset; 
    int i, j;
    unsigned int pid_stage_offset; 
    int next_ts_slot;
    int pipeline_stage_name_len = 0;
    hrtime_t num_pipeline_stages = ZIO_PIPELINE_STAGES;
    boolean_t holding_lock = B_TRUE;

    VERIFY3P(cb_args, !=, NULL);
    unsigned int curr_pid = ((cb_zio_pipeline_args_t *)cb_args)->pid;
    zio_stage io_stage = ((cb_zio_pipeline_args_t *)cb_args)->io_stage;

    ASSERT3U(io_stage, <=, ZIO_PIPELINE_STAGES);

    /* First find the row for curr_pid */
    pid_arr_offset = curr_pid % STATIC_PID_CAP;
    /* Next get the pipeline offset for correct pid array */
    pid_stage_offset = ZIOSTAGE_TO_OFFSET(io_stage);

    if (!(all_pid_io_stages.dumped)) {
        if (!cb_zio_pipeline_initialized) {
            /* Initilization code not called, so just bail */
            return;
        }
       
        /* If another PID resides in the slot, find next available slot. If no
         * slots is available just bail
         */
        while ((all_pid_io_stages.pid_io_stages[pid_arr_offset].curr_pid != curr_pid) && 
               (all_pid_io_stages.row_select_iter < STATIC_PID_CAP)) {
            if (all_pid_io_stages.pid_io_stages[pid_arr_offset].curr_pid != 0) {
                all_pid_io_stages.row_select_iter += 1;
                pid_arr_offset += 1;
            } else {
                break;
            }
        }
        
        /* Bailing here if no avialable slot */
        if (all_pid_io_stages.row_select_iter >= STATIC_PID_CAP) {
            return;
        }
       
        if (!strcmp(all_pid_io_stages.pid_io_stages[pid_arr_offset].zio_pipeline_stage[pid_stage_offset], no_stage)) { 
            all_pid_io_stages.pid_io_stages[pid_arr_offset].zio_pipeline_stage[pid_stage_offset] = 
                zio_pipeline_stage_strs[pid_stage_offset];
        }
        
        if (!all_pid_io_stages.pid_io_stages[pid_arr_offset].done_collecting) {
            all_pid_io_stages.pid_io_stages[pid_arr_offset].curr_pid = curr_pid;
            next_ts_slot = all_pid_io_stages.pid_io_stages[pid_arr_offset].io_stages[pid_stage_offset][0] + 1;
            all_pid_io_stages.pid_io_stages[pid_arr_offset].io_stages[pid_stage_offset][next_ts_slot] = NSEC2USEC(gethrtime());
            all_pid_io_stages.pid_io_stages[pid_arr_offset].io_stages[pid_stage_offset][0] += 1;
            all_pid_io_stages.pid_io_stages[pid_arr_offset].num_of_ts_collected += 1;
            if (all_pid_io_stages.pid_io_stages[pid_arr_offset].num_of_ts_collected == STATIC_PIPELINE_COL_CAP) {
                /* We have reached our limit, so stop collecting */
                all_pid_io_stages.pid_io_stages[pid_arr_offset].done_collecting = B_TRUE;
            } else if (all_pid_io_stages.pid_io_stages[pid_arr_offset].num_of_ts_collected >= STATIC_PIPELINE_PID_CAP &&
                       IS_ZIO_DONE_STAGE(io_stage)) {
                /* 
                 * This is where we should always land as we plan on stopping collection
                 * only on zio_done stages for a PID
                 */
                all_pid_io_stages.pid_io_stages[pid_arr_offset].done_collecting = B_TRUE;
            }
        }

        /* If we are at capacity for the timestamps for all PID's, we just need to dump the output */
        if (all_pid_io_stages.pid_io_stages[pid_arr_offset].done_collecting && 
            cb_zio_pipeline_all_pids_done()) {
            /* Only a single thread will write the log file */
            mutex_enter(&all_pid_io_stages.lock);
            if (!all_pid_io_stages.dumped) {
                all_pid_io_stages.dump_file = spl_filp_open(LOG_FILE_PIPELINE, O_WRONLY|O_CREAT, 0666, NULL);
                if (all_pid_io_stages.dump_file == NULL) {
                    cmn_err(CE_WARN, "Could not open all_pid_io_stages.dump_file = %s", LOG_FILE_PIPELINE);
                    mutex_exit(&all_pid_io_stages.lock);
                    return;
                }
                all_pid_io_stages.dumped = 1;
            } else {
                cb_zio_pipeline_dtor(&holding_lock);
                return;
            }
            
            /* First writing out the total number or PID's with timestamps to the file */
            for (i = 0; i < STATIC_PID_CAP; i++) {
                if (all_pid_io_stages.pid_io_stages[i].curr_pid != 0
                    && all_pid_io_stages.pid_io_stages[i].num_of_ts_collected > MIN_TS_TO_IGNORE) {
                    all_pid_io_stages.total_pids += 1;
                }
            }
            spl_kernel_write(all_pid_io_stages.dump_file, 
                             &all_pid_io_stages.total_pids, 
                             sizeof(hrtime_t), 
                             &all_pid_io_stages.file_offset);

            /* Next writing out the total number of possible pipeline stages */
            spl_kernel_write(all_pid_io_stages.dump_file,
                             &num_pipeline_stages,
                             sizeof(hrtime_t),
                             &all_pid_io_stages.file_offset);

            /* Now writing out each PID and its corresponding timestamps */
            for (i = 0; i < STATIC_PID_CAP; i++) {
                if (all_pid_io_stages.pid_io_stages[i].curr_pid != 0 
                    && all_pid_io_stages.pid_io_stages[i].num_of_ts_collected > MIN_TS_TO_IGNORE) {
                    /* Writing out PID */
                    spl_kernel_write(all_pid_io_stages.dump_file,
                                     &all_pid_io_stages.pid_io_stages[i].curr_pid,
                                     sizeof(hrtime_t),
                                     &all_pid_io_stages.file_offset);
                    /* 
                     * Writing out each pipeline stages timestamps for this PID 
                     * by  writing out the following:
                     * 1 - Length of the pipeline stage name (if stage not used length of NO_STAGE)
                     * 2 - The name of the pipeline stage (if stage not used NO_STAGE)
                     * 3 - Total number of timestamps for the stage
                     * 4 - All the timestamps collected for this stage
                     */
                    for (j = 0; j < num_pipeline_stages; j++) {
                        pipeline_stage_name_len = strlen(all_pid_io_stages.pid_io_stages[i].zio_pipeline_stage[j]);
                        /* Length of pipeline stage name */
                        spl_kernel_write(all_pid_io_stages.dump_file,
                                         &pipeline_stage_name_len,
                                         sizeof(int),
                                         &all_pid_io_stages.file_offset);

                        /* Pipeline stage name */
                        spl_kernel_write(all_pid_io_stages.dump_file,
                                         all_pid_io_stages.pid_io_stages[i].zio_pipeline_stage[j],
                                         sizeof(char) * pipeline_stage_name_len,
                                         &all_pid_io_stages.file_offset);

                        /* Total number of time stamps for this pipeline stage */
                        spl_kernel_write(all_pid_io_stages.dump_file,
                                         &all_pid_io_stages.pid_io_stages[i].io_stages[j][0],
                                         sizeof(hrtime_t),
                                         &all_pid_io_stages.file_offset);
                        
                        /* Actual timestamps for this pipeline stage */
                        if (all_pid_io_stages.pid_io_stages[i].io_stages[j][0]) {
                            /* If we have timestamps we write them now */
                            spl_kernel_write(all_pid_io_stages.dump_file,
                                             &all_pid_io_stages.pid_io_stages[i].io_stages[j][1],
                                             sizeof(hrtime_t) * all_pid_io_stages.pid_io_stages[i].io_stages[j][0],
                                             &all_pid_io_stages.file_offset);
                        }
                    }
                }
            }
            spl_filp_close(all_pid_io_stages.dump_file);
            cb_zio_pipeline_dtor(&holding_lock);
            return;
        }
    }
}

/**************************************************************/
/*                    Function Defintions                     */
/**************************************************************/
cb_zio_pipeline_args_t 
create_cb_zio_pipeline_args(zio_t *zio,
                            zio_stage io_stage)
{
    cb_zio_pipeline_args_t cb_args;
    
    cb_args.pid = cb_zio_log_get_correct_pid(zio);
    cb_args.io_stage = io_stage;
    return (cb_args);
}
EXPORT_SYMBOL(create_cb_zio_pipeline_args);

log_callback_t log_cb_zio_pipeline =
{
    .cb_name = "cb_zio_pipeline",
    .zio_type = zio_is_read,
    .ctor = cb_zio_pipeline_ctor,
    .dtor = cb_zio_pipeline_dtor,
    .cb_func = cb_zio_pipeline_cb,
    .dump_func = NULL
};
EXPORT_SYMBOL(log_cb_zio_pipeline);
