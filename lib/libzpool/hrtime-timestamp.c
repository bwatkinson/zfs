////////////////////////////////////
// Brian A. Added this file
////////////////////////////////////

/*
 * This file defines the following #defines and static variables
 * that can be udpated:
 * NUM_LOG_FILES             : Total number of call sites used by hrtime_add_timestamp_call_sites
 * LOG_FILE_PIPELINE         : Path and name of log file dumped by hrtime_add_timestamp_pipeline_stages
 * LOG_FILE_TASKQ            : Path and name of log file dumped by hrtime_taskq_count_dump
 * LOG_FILE_CS               : Paths and names of log files dumped by hrtime_add_timestamp_call_sites
 *                             (There must be a matching number of these for the NUM_LOG_FILES value)
 *
 * There is a corresponding file in zfs/lib/libzpool/hrtime-timstamp.c that's values
 * should be updated to match the values in this file.
 *
 * The values STATIC_LIST_CAP, STATIC_PIPELINE_PID_CAP, STATIC_PER_TASKQ_COUNT and STATIC_PID_CAP 
 * are defined in include/sys/hrtime_spl_timestamp.h
 */ 

#include <sys/zfs_context.h>

#define NSEC2USEC(n) ((n) / (NANOSEC / MICROSEC))

/*
 * Number of call sites that we will be taking timstamps for
 */
#define NUM_LOG_FILES 3

/* 
 * Change these variables in order to set a default locations where
 * log files are dumped
 */
const static char *LOG_FILE_PIPELINE = "/var/log/hrtime_zio_pipeline.log";

const static char *LOG_FILE_TASKQ = "/var/log/hrtime_taskq_counts.log";

const static char *LOG_FILE_CS[NUM_LOG_FILES] = {"/var/log/hrtime_dmu_hold_array_list_0.log",
                                                 "/var/log/hrtime_submit_bio_list_1.log",
                                                 "/var/log/hrtime_io_schedule_list_2.log"};


/* 
 * Just a simple debugging enable (1) disable (0)
 */
#define HRTIME_DEBUG 0

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
 * Initialization variables that are set and checked
 */
static int _hrtime_ts_pipeline_initialized = 0;
static int _hrtime_ts_call_site_initialized = 0;
static int _hrtime_taskq_initialized = 0;


/*******************************************************/
/* Struct definitions for call site, ZIO pipeline, and */
/* taskq timestamps/counts.                            */
/*******************************************************/
typedef struct call_site_data_s
{
    hrtime_t arr[STATIC_PID_CAP][STATIC_COL_CAP];
    int dumped;
    int array_size;
    hrtime_t total_pids;
    int row_select_iter;
    FILE *dump_file;
    pthread_mutex_t lock;
} call_site_data_t;

typedef struct ts_cs_array_s
{
    call_site_data_t arrays[NUM_LOG_FILES];
} ts_cs_array_t;

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
    FILE *dump_file;
    pthread_mutex_t lock;
} all_pid_io_stages_t;

typedef struct taskq_name_thread_counts_s
{
    char taskq_name[32];
    int num_threads_present;
    int num_threads[STATIC_COL_CAP_TASKQ];
    int num_counts_collected;
} taskq_name_thread_counts_t;

typedef struct taskq_counts_s
{
    taskq_name_thread_counts_t *taskqs;
    int dumped;
    int total_taskqs;
    FILE *dump_file;
    int num_taskqs_done;
    pthread_mutex_t lock;
} taskq_counts_t;

/*****************************************/
/*       Static timestamp structs        */
/*****************************************/
static ts_cs_array_t cs_arrays;
static all_pid_io_stages_t all_pid_io_stages;
static taskq_counts_t all_taskq_counts;

/***************************************************************************************/
/*                         Static Function Defintions                                  */
/***************************************************************************************/
static boolean_t _hrtime_pipeline_all_pids_done(void)
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

static void _hrtime_pipeline_destroy(boolean_t lock_held)
{
#if HRTIME_DEBUG
    int i;
    cmn_err(CE_WARN, "Inside %s", __func__);
#endif 
    if (!lock_held) {
        pthread_mutex_lock(&all_pid_io_stages.lock);
    }

    if (_hrtime_ts_pipeline_initialized) {
        _hrtime_ts_pipeline_initialized = 0;
#if HRTIME_DEBUG
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
#endif /* HRTIME_DEBUG */
        free(all_pid_io_stages.pid_io_stages);
        all_pid_io_stages.pid_io_stages = NULL;
    }
    pthread_mutex_unlock(&all_pid_io_stages.lock);
}

static void _hrtime_call_site_destroy(void)
{
    static int num_logs_written = 0;
    num_logs_written += 1;

#if HRTIME_DEBUG
    int i, offset;
    cmn_err(CE_WARN, "Inside %s", __func__);
    if (num_logs_written >= NUM_LOG_FILES) {
        for (offset = 0; offset < NUM_LOG_FILES; offset++) {
            cmn_err(CE_WARN, "For callsite %d", offset);
            cmn_err(CE_WARN, "callsite %d collected %d timestamps for %d PIDs",
                    offset, cs_arrays.arrays[offset].array_size, cs_arrays.arrays[offset].total_pids);
            for (i = 0; i < STATIC_PID_CAP; i++) {
                if (cs_arrays.arrays[offset].arr[i][0] != 0) {
                    cmn_err(CE_WARN, "PID: %d collected %d timestamps",
                            cs_arrays.arrays[offset].arr[i][0],
                            cs_arrays.arrays[offset].arr[i][1]);
                }
            }
        }
    }
#endif /* HRTIME_DEBUG */
    if (_hrtime_ts_call_site_initialized) {
        _hrtime_ts_call_site_initialized = 0;
        num_logs_written = 0;
    }
}

static void _hrtime_taskq_count_destroy(boolean_t lock_held)
{
    int i;
#if HRTIME_DEBUG
    cmn_err(CE_WARN, "Inside %s", __func__);
#endif 
    if (!lock_held) {
        pthread_mutex_lock(&all_taskq_counts.lock);
    }

    if (_hrtime_taskq_initialized) {
        _hrtime_taskq_initialized = 0;

        for (i = 0; i < all_taskq_counts.total_taskqs; i++) {
            /* Busy wait till all threads are no longer using the taskqs */
            while (all_taskq_counts.taskqs[i].num_threads_present > 0) {}
        }
#if HRTIME_DEBUG
        cmn_err(CE_WARN, "_hrtime_taskq_initialized = %d",
                _hrtime_taskq_initialized);
        for (i = 0; i < all_taskq_counts.total_taskqs; i++) {
            cmn_err(CE_WARN, "all_taskq_counts.taskq[%d].taskq_name = %s collected = %d counts", 
                    i, all_taskq_counts.taskqs[i].taskq_name, all_taskq_counts.taskqs[i].num_counts_collected);
        }
#endif 
        free(all_taskq_counts.taskqs); 
        all_taskq_counts.taskqs = NULL;
    }
    pthread_mutex_unlock(&all_taskq_counts.lock);
}



/***************************************************************************************/
/*                                Function Defintions                                  */
/***************************************************************************************/
/**
 * hrtime_timestamp_init -
 *
 * This function initializes the variables needed to use the functions
 * hrtime_add_timestamp_pipeline_stages and hrtime_add_timestamp_call_sites.
 * If this function is not called, no collections will be done even if the
 * pipeline and call site timestamp collection functions are called.
 */
void hrtime_timestamp_init(void)
{
#if defined (HRTIME_PIPELINE_STAMP) || defined (HRTIME_CALL_SITE_STAMP)
    int i;
#endif

#if defined (HRTIME_PIPELINE_STAMP)
    int j;
    if (!_hrtime_ts_pipeline_initialized) {
        all_pid_io_stages.pid_io_stages = NULL;
        all_pid_io_stages.pid_io_stages = 
            (pid_io_stages_t *)malloc(sizeof(pid_io_stages_t) * STATIC_PID_CAP);
        ASSERT3P(all_pid_io_stages.pid_io_stages, !=, NULL);
        all_pid_io_stages.dumped = 0;
        all_pid_io_stages.total_pids = 0;
        all_pid_io_stages.row_select_iter = 0;
        all_pid_io_stages.dump_file = NULL;
        pthread_mutex_init(&all_pid_io_stages.lock, NULL);
        for (i = 0; i < STATIC_PID_CAP; i++) {
            for (j = 0; j < ZIO_PIPELINE_STAGES; j++) {
                all_pid_io_stages.pid_io_stages[i].zio_pipeline_stage[j] = no_stage;
            }
        }
        _hrtime_ts_pipeline_initialized = 1;
#if HRTIME_DEBUG
        cmn_err(CE_WARN, "Initialized the data for timestamping ZIO pipeline in %s", __func__);
#endif /* HRTIME_DEBUG */
    }
#endif /* HRTIME_PIPELINE_STAMP */

#if defined (HRTIME_CALL_SITE_STAMP)
    if (!_hrtime_ts_call_site_initialized) {
        for (i = 0; i < NUM_LOG_FILES; i++) {
            cs_arrays.arrays[i].dumped = 0;
            cs_arrays.arrays[i].array_size = 0;
            cs_arrays.arrays[i].total_pids = 0;
            cs_arrays.arrays[i].row_select_iter = 0;
            cs_arrays.arrays[i].dump_file = NULL;
            pthread_mutex_init(&cs_arrays.arrays[i].lock, NULL);
            memset(cs_arrays.arrays[i].arr, 0, sizeof(hrtime_t) * STATIC_PID_CAP * STATIC_COL_CAP);
        } 
        _hrtime_ts_call_site_initialized = 1;
#if HRTIME_DEBUG
        cmn_err(CE_WARN, "Initialized the data for timestamping call sites in %s", __func__);
#endif /* HRTIME_DEBUG */
    }
#endif /* HRTIME_CALL_SITE_STAMP */
}



/**
 * hrtime_timestamp_fini -
 *
 * This function clears up all memory that was allocated in order to call
 * hrtime_add_timestamp_pipeline_stages and hrtime_add_timestamp_call_sites.
 * After this call is made, no further timestamp collections will be made
 * unless hrtime_timestamp_init is called first.
 */
void hrtime_timestamp_fini(void)
{
#if defined (HRTIME_PIPELINE_STAMP)
#if HRTIME_DEBUG
    cmn_err(CE_WARN, "Inside %s", __func__);
#endif /* HRTIME_DEBUG */
    _hrtime_pipeline_destroy(B_FALSE);
#endif /* HRTIME_PIPELINE_STAMP */
#if defined (HRTIME_CALL_SITE_STAMP)
#if HRTIME_DEBUG
    cmn_err(CE_WARN, "Inside %s", __func__);
#endif /* HRTIME_DEBUG */
    _hrtime_call_site_destroy();
#endif /* HRTIME_CALL_SITE_STAMP */
}



/**
 * hrtime_taskq_count_init -
 * @taskq_names : Array of the names of the taskqs to collect counts for
 * @num_taskqs  : The size of the array of taskq names (AKA the total
 *                number of taskqs to collect counts for)
 *
 * This function initializes the variables needed to use the functions
 * hrtime_taskq_count_add_count and hrtime_taskq_count_dump. If this function
 * is not called, no taskq count collections will be done even if the
 * taskq count add function is called.
 */ 
void hrtime_taskq_count_init(char **taskq_names, 
                             int num_taskqs)
{
    int i;
    if (!_hrtime_taskq_initialized) {
        all_taskq_counts.taskqs = NULL;
        all_taskq_counts.taskqs = 
            (taskq_name_thread_counts_t *)malloc(sizeof(taskq_name_thread_counts_t) * num_taskqs);
        ASSERT3P(all_taskq_counts.taskqs, !=, NULL);
        all_taskq_counts.dumped = 0;
        all_taskq_counts.total_taskqs = num_taskqs;
        all_taskq_counts.dump_file = NULL;
        all_taskq_counts.num_taskqs_done = 0;
        pthread_mutex_init(&all_taskq_counts.lock, NULL);
        for (i = 0; i < num_taskqs; i++) {
            strcpy(all_taskq_counts.taskqs[i].taskq_name, taskq_names[i]);
            all_taskq_counts.taskqs[i].num_threads_present = 0;
            memset(all_taskq_counts.taskqs[i].num_threads, 0, sizeof(int) * STATIC_COL_CAP_TASKQ);
        }
        _hrtime_taskq_initialized = 1;
#if HRTIME_DEBUG
        cmn_err(CE_WARN, "Initialized the data for timestamping taskq's where all_taskq_counts.total_taskqs = %d in %s",
                all_taskq_counts.total_taskqs, __func__);
        for (i = 0; i < num_taskqs; i++) {
            cmn_err(CE_WARN, "all_taskq_counts.taskq[%d].taskq_name = %s in %s with num_threads_present = %d", 
                    i, all_taskq_counts.taskqs[i].taskq_name,
                     __func__, all_taskq_counts.taskqs[i].num_threads_present);
        }
#endif /* HRTIME_DEBUG */
    }
}



/**
 * hrtime_taskq_count_fini -
 *
 * This function clears up all memory that was allocated in order to call
 * hrtime_taskq_count_add_count. After this call is made, no further taskq
 * counts collections will be made unless hrtime_taskq_count_init is called
 * first.
 */
void hrtime_taskq_count_fini(void)
{
#if HRTIME_DEBUG
    cmn_err(CE_WARN, "Inside %s", __func__);
#endif /* HRTIME_DEBUG */
    _hrtime_taskq_count_destroy(B_FALSE);
}



/**
 * hrtime_taskq_count_add_count -
 * @taskq_name  : Name of the taskq to collect thread counts for
 * @num_threads : The number of active threads for the taskq
 *                taskq_name
 *
 * This function will add the current active thread count passed
 * in by num_threads to the count array associated with taskq_name.
 * If the taskq_name was not passed to hrtime_taskq_count_init, then
 * no counts records will be made for the taskq.
 */
void hrtime_taskq_count_add_count(char *taskq_name,
                                  int num_threads)
{
    int taskq_offset = 0;
    int thread_count_offset = 0;

    if (!(all_taskq_counts.dumped)) {
        if (!_hrtime_taskq_initialized) {
            /* Initilization code not called, so just bail */
            return;
        }
        
        /*
         * First we need to find the taskq we are looking for by name
         */
        for (taskq_offset = 0; taskq_offset < all_taskq_counts.total_taskqs; taskq_offset++) {
            if (!strcmp(all_taskq_counts.taskqs[taskq_offset].taskq_name, taskq_name)) {
                break;
            }
        }

        /* If this is not a taskq we are counting just bail */
        if (taskq_offset == all_taskq_counts.total_taskqs) {
            return;
        }

        if ((all_taskq_counts.taskqs[taskq_offset].num_counts_collected + 1) <= STATIC_PER_TASKQ_COUNT_CAP) {
            all_taskq_counts.taskqs[taskq_offset].num_threads_present += 1;
            all_taskq_counts.taskqs[taskq_offset].num_counts_collected += 1;
            thread_count_offset = all_taskq_counts.taskqs[taskq_offset].num_threads[0] + 1;
            all_taskq_counts.taskqs[taskq_offset].num_threads[thread_count_offset] = num_threads;
            all_taskq_counts.taskqs[taskq_offset].num_threads[0] += 1;
            if (all_taskq_counts.taskqs[taskq_offset].num_counts_collected >= STATIC_PER_TASKQ_COUNT_CAP) {
                all_taskq_counts.num_taskqs_done += 1;
            }
            all_taskq_counts.taskqs[taskq_offset].num_threads_present -= 1;
        }
    }
}



/**
 * hrtime_taskq_count_dump -
 *
 * This function dumps the taskq's active thread counts out to the log
 * file specified by LOG_FILE_TASKQ.
 */
void hrtime_taskq_count_dump(void)
{
    int taskq_name_len = 0;
    int i;

    if (!(all_taskq_counts.dumped)) {
        if (!_hrtime_taskq_initialized) {
            /* Initilization code not called, so just bail */
            return;
        }
        
        if (all_taskq_counts.num_taskqs_done >= all_taskq_counts.total_taskqs) {
            /* Only a single thread will write the log file */
            pthread_mutex_lock(&all_taskq_counts.lock);
            if (!all_taskq_counts.dumped) {
                all_taskq_counts.dump_file = fopen(LOG_FILE_TASKQ, "w");
                if (all_taskq_counts.dump_file == NULL) {
                    cmn_err(CE_WARN, "Could not open all_taskq_counts.dump_file = %s", LOG_FILE_TASKQ);
                    pthread_mutex_unlock(&all_taskq_counts.lock);
                    return;
                }
                all_taskq_counts.dumped = 1;
            } else {
                _hrtime_taskq_count_destroy(B_TRUE);
                return;
            }
            
            /* First writing out the total number of taskqs */
            fwrite(&all_taskq_counts.total_taskqs,
                   sizeof(int),
                   1,
                   all_taskq_counts.dump_file);

            /* 
             * Next we will write out the counts of the taskqs by doing the following
             * for each taskq:
             * 1 - Write out the length of the taskq name
             * 2 - Write out the taskq name
             * 3 - Write out that total number of thread counts collected for the taskq
             * 4 - Write out the thread counts for the taskq collected
             */
            for (i = 0; i < all_taskq_counts.total_taskqs; i++) {
                taskq_name_len = strlen(all_taskq_counts.taskqs[i].taskq_name);
                /* Taskq name length */
                fwrite(&taskq_name_len,
                       sizeof(int),
                       1,
                       all_taskq_counts.dump_file);

                /* Taskq name */
                fwrite(all_taskq_counts.taskqs[i].taskq_name,
                       sizeof(char),
                       taskq_name_len,
                       all_taskq_counts.dump_file);
                
                /* Taskq total number of thread counts collected */
                fwrite(&all_taskq_counts.taskqs[i].num_threads[0],
                       sizeof(int),
                       1,
                       all_taskq_counts.dump_file);    

                /* Taskq thread counts collected */
                fwrite(&all_taskq_counts.taskqs[i].num_threads[1],
                       sizeof(int),
                       all_taskq_counts.taskqs[i].num_threads[0],
                       all_taskq_counts.dump_file);
                
            }
            
            fclose(all_taskq_counts.dump_file);
            _hrtime_taskq_count_destroy(B_TRUE);
            return;
        }
    }
}



/**
 * hrtime_add_timestamp_pipeline_stages -
 * @curr_pid : The PID that is executing the ZIO pipeline stage
 * @io_stage : The current ZIO pipeline stage for this PID
 *
 * This function adds a timestamp for the PID curr_pid for the
 * ZIO pipeline stage io_stage. After the PID has collected 
 * STATIC_PIPELINE_PID_CAP timetamps for each of the pipeline
 * stages this function is called for, it will stop collection
 * with the caveat that it will only stop collection on the
 * ZIO_STAGE_DONE io_stage. The last PID that has collected 
 * STATIC_PIPELINE_PID_CAP pipeline stage timestamps will then
 * write out all PID's pipeline stage timetamps to the log file
 * specified by LOG_FILE_PIPELINE.
 */
void hrtime_add_timestamp_pipeline_stages(int curr_pid, 
                                          zio_stage io_stage)
{
    int pid_arr_offset; 
    int i, j;
    unsigned int pid_stage_offset; 
    int next_ts_slot;
    int pipeline_stage_name_len = 0;
    hrtime_t num_pipeline_stages = ZIO_PIPELINE_STAGES;

    ASSERT3U(io_stage, <=, ZIO_PIPELINE_STAGES);

    /* First find the row for curr_pid */
    pid_arr_offset = curr_pid % STATIC_PID_CAP;
    /* Next get the pipeline offset for correct pid array */
    pid_stage_offset = ZIOSTAGE_TO_OFFSET(io_stage);

    if (!(all_pid_io_stages.dumped)) {
        if (!_hrtime_ts_pipeline_initialized) {
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
           _hrtime_pipeline_all_pids_done()) {
            /* Only a single thread will write the log file */
            pthread_mutex_lock(&all_pid_io_stages.lock);
            if (!all_pid_io_stages.dumped) {
                all_pid_io_stages.dump_file = fopen(LOG_FILE_PIPELINE, "w");
                if (all_pid_io_stages.dump_file == NULL) {
                    cmn_err(CE_WARN, "Could not open all_pid_io_stages.dump_file = %s", LOG_FILE_PIPELINE);
                    pthread_mutex_unlock(&all_pid_io_stages.lock);
                    return;
                }
                all_pid_io_stages.dumped = 1;
            } else {
                _hrtime_pipeline_destroy(B_TRUE);
                return;
            }
            
            /* First writing out the total number or PID's with timestamps to the file */
            for (i = 0; i < STATIC_PID_CAP; i++) {
                if (all_pid_io_stages.pid_io_stages[i].curr_pid != 0
                    && all_pid_io_stages.pid_io_stages[i].num_of_ts_collected > MIN_TS_TO_IGNORE) {
                    all_pid_io_stages.total_pids += 1;
                }
            }
            fwrite(&all_pid_io_stages.total_pids, 
                   sizeof(hrtime_t),
                   1, 
                   all_pid_io_stages.dump_file);

            /* Next writing out the total number of possible pipeline stages */
            fwrite(&num_pipeline_stages,
                   sizeof(hrtime_t),
                   1,
                   all_pid_io_stages.dump_file);

            /* Now writing out each PID and its corresponding timestamps */
            for (i = 0; i < STATIC_PID_CAP; i++) {
                if (all_pid_io_stages.pid_io_stages[i].curr_pid != 0 
                    && all_pid_io_stages.pid_io_stages[i].num_of_ts_collected > MIN_TS_TO_IGNORE) {
                    /* Writing out PID */
                    fwrite(&all_pid_io_stages.pid_io_stages[i].curr_pid,
                           sizeof(hrtime_t),
                           1,
                           all_pid_io_stages.dump_file);
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
                        fwrite(&pipeline_stage_name_len,
                               sizeof(int),
                               1,
                               all_pid_io_stages.dump_file);

                        /* Pipeline stage name */
                        fwrite(all_pid_io_stages.pid_io_stages[i].zio_pipeline_stage[j],
                               sizeof(char),
                               pipeline_stage_name_len,
                               all_pid_io_stages.dump_file);

                        /* Total number of time stamps for this pipeline stage */
                        fwrite(&all_pid_io_stages.pid_io_stages[i].io_stages[j][0],
                               sizeof(hrtime_t),
                               1,
                               all_pid_io_stages.dump_file);
                        
                        /* Actual timestamps for this pipeline stage */
                        if (all_pid_io_stages.pid_io_stages[i].io_stages[j][0]) {
                            /* If we have timestamps we write them now */
                            fwrite(&all_pid_io_stages.pid_io_stages[i].io_stages[j][1],
                                   sizeof(hrtime_t),
                                   all_pid_io_stages.pid_io_stages[i].io_stages[j][0],
                                   all_pid_io_stages.dump_file);
                        }
                    }
                }
            }
            fclose(all_pid_io_stages.dump_file);
            _hrtime_pipeline_destroy(B_TRUE);
            return;
        }
    }
}



/**
 * hrtime_add_timesptamp_call_sites - 
 * @curr_pid : Process ID that is calling this function 
 * @offset   : Unique number from 0 to NUM_LOG_FILES - 1 for the call site
 * @call_site_name : Name of the function calling this (use __func__)
 *
 * This function adds a timestamp for the PID for the current call site specified
 * by the offset parameter. Once all PID's for a given call site have collected
 * a total of STATIC_LIST_CAP, the log file specifed by LOG_FILE_CS[offset] will
 * be written out.
 *
 * Ideally this should be used with threads where no locking should occcur; however, 
 * a race condition does exist when adding items to arrays. 
 */
void hrtime_add_timestamp_call_sites(int curr_pid, 
                                     unsigned int offset,
                                     const char *call_site_name)
{
    int i = 0;
    int pid_row = 0;
    int call_site_name_len = 0;
    int all_logs_dumped = 1;

    /* Making sure a valid offset was passed */
    ASSERT3U(offset, <, NUM_LOG_FILES);

    if (!(cs_arrays.arrays[offset].dumped)) {
        if (!_hrtime_ts_call_site_initialized) {
            /* Initilization code not called, so just bail */
            return;
        }

        /* First find the row for curr_pid */
        pid_row = curr_pid % STATIC_PID_CAP;

        /* 
         * If another PID resides in the slot find next available slot. If no
         * slot is available just bail
         */
        while ((cs_arrays.arrays[offset].arr[pid_row][0] != curr_pid) && 
               (cs_arrays.arrays[offset].row_select_iter < STATIC_PID_CAP)) {
            if (cs_arrays.arrays[offset].arr[pid_row][0] != 0) {
                cs_arrays.arrays[offset].row_select_iter += 1;
                pid_row += 1;
            } else {
                break;
            }
        }

        /* Bailing here if no avialable slot */
        if (cs_arrays.arrays[offset].row_select_iter >= STATIC_PID_CAP) {
            return;
        }

        cs_arrays.arrays[offset].array_size += 1;
        cs_arrays.arrays[offset].arr[pid_row][0] = curr_pid;
        cs_arrays.arrays[offset].arr[pid_row][cs_arrays.arrays[offset].arr[pid_row][1] + 2] = NSEC2USEC(gethrtime());
        cs_arrays.arrays[offset].arr[pid_row][1] += 1;
        if (cs_arrays.arrays[offset].array_size >= STATIC_LIST_CAP) {
            /* Only a single thread will write the log file */
            pthread_mutex_lock(&cs_arrays.arrays[offset].lock);
            
            if (!cs_arrays.arrays[offset].dumped) {
                cs_arrays.arrays[offset].dump_file = fopen(LOG_FILE_CS[offset], "w");
                if (cs_arrays.arrays[offset].dump_file == NULL) {
                    cmn_err(CE_WARN, "Could not open cs_arrays.array[%d].dump_file = %s", offset, LOG_FILE_CS[offset]);
                    pthread_mutex_unlock(&cs_arrays.arrays[offset].lock);
                    return;
                }
                cs_arrays.arrays[offset].dumped = 1;
            } else {
                pthread_mutex_unlock(&cs_arrays.arrays[offset].lock);
                return;
            }

            /* First writing out the total number of PID's with timestamps to file */
            for (i = 0; i < STATIC_PID_CAP; i++) {
                if (cs_arrays.arrays[offset].arr[i][0] != 0) {
                    cs_arrays.arrays[offset].total_pids += 1;
                }
            }

            fwrite(&cs_arrays.arrays[offset].total_pids, 
                   sizeof(hrtime_t),
                   1,
                   cs_arrays.arrays[offset].dump_file);

            /* 
             * Next writing out the name of the call site by first writing out the length
             * of the call site name and then the actual call site
             */
            call_site_name_len = strlen(call_site_name);
            /* Length of call site name */
            fwrite(&call_site_name_len,
                   sizeof(int),
                   1,
                   cs_arrays.arrays[offset].dump_file);

            /* Call site name */
            fwrite(call_site_name,
                   sizeof(char),
                   call_site_name_len,
                   cs_arrays.arrays[offset].dump_file);

            /* Now writing out each PID with it corresponding timestamps */
            for (i = 0; i < STATIC_PID_CAP; i++) {
                if (cs_arrays.arrays[offset].arr[i][0] != 0) {
                    fwrite(&cs_arrays.arrays[offset].arr[i], 
                           sizeof(hrtime_t),
                           cs_arrays.arrays[offset].arr[i][1] + 2, 
                           cs_arrays.arrays[offset].dump_file);
                }
            }
            fclose(cs_arrays.arrays[offset].dump_file);
            for (i = 0; i < NUM_LOG_FILES; i++) {
                all_logs_dumped &= cs_arrays.arrays[offset].dumped;
            }
            if (all_logs_dumped) {
                _hrtime_call_site_destroy();
            }
            pthread_mutex_unlock(&cs_arrays.arrays[offset].lock);
        }
    }
}
