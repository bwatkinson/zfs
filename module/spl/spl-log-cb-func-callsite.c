////////////////////////////////////
// Brian A. Added this file
////////////////////////////////////

/*
 * This file defines the following #defines and static variables
 * that can be udpated:
 * NUM_LOG_FILES             : Total number of call sites used by cb_func_callsite_cb
 * LOG_FILE_CS               : Paths and names of log files dumped by cb_func_callsite_cb
 *                             (There must be a matching number of these for the NUM_LOG_FILES value)
 */ 

#include <sys/log_cb_func_callsite.h>
#include <linux/file_compat.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#include <sys/vmem.h>
#include <sys/mutex.h>
#include <sys/strings.h>

/*
 * Number of call sites that we will be taking timstamps for
 */
#define NUM_LOG_FILES 3

/* 
 * Change this variable in order to set the default locations where
 * log files are dumped
 */
const static char *LOG_FILE_CS[NUM_LOG_FILES] = {"/var/log/hrtime_dmu_hold_array_list_0.log",
                                                 "/var/log/hrtime_submit_bio_list_1.log",
                                                 "/var/log/hrtime_io_schedule_list_2.log"};


/*
 * Initialization variable that is set and checked
 */
static int cb_func_callsite_max_pids = 0;
static int cb_func_callsite_initialized = 0;
static kmutex_t *cb_func_callsite_mutex = NULL;

/*
 * Defines whether or not the counters should be reset
 */
static uint_t spl_log_cb_func_callsite_reset_counters =  0;
module_param(spl_log_cb_func_callsite_reset_counters, uint, 0644);
MODULE_PARM_DESC(spl_log_cb_func_callsite_reset_counters,
    "Resets the counters for the callsite SPL callback log");

/*******************************************************/
/* Struct definitions for call site timestamps/counts. */
/*******************************************************/
typedef struct call_site_data_s
{
    hrtime_t **arr;
    int dumped;
    int array_size;
    hrtime_t total_pids;
    struct file *dump_file;
    loff_t file_offset;
    kmutex_t lock;
} call_site_data_t;

typedef struct ts_cs_array_s
{
    int row_select_iter;
    unsigned int *pids;
    uint_t pid_cap;
    call_site_data_t arrays[NUM_LOG_FILES];
} ts_cs_array_t;

/*****************************************/
/*       Static timestamp structs        */
/*****************************************/
static ts_cs_array_t cs_arrays;

/***************************************************************************/
/*                      Static Function Defintions                         */
/***************************************************************************/
static void cb_func_callsite_dtor(void *dtor_args)
{
    static int num_logs_written = 0;
    int i, j;
    boolean_t called_from_destroy_func;

    VERIFY3P(dtor_args, !=, NULL);
    called_from_destroy_func = *((boolean_t *)dtor_args);

    if (cb_func_callsite_initialized) {
        num_logs_written += 1;
        if (spl_log_cb_debug_enabled()) {
            int offset;
            cmn_err(CE_WARN, "Inside %s", __func__);
            if (num_logs_written >= NUM_LOG_FILES || called_from_destroy_func) {
                for (offset = 0; offset < NUM_LOG_FILES; offset++) {
                    cmn_err(CE_WARN, "For callsite %d", offset);
                    cmn_err(CE_WARN, "callsite %d collected %d timestamps for %d PIDs",
                            offset, cs_arrays.arrays[offset].array_size, cs_arrays.arrays[offset].total_pids);
                    for (i = 0; i < cs_arrays.pid_cap; i++) {
                        if (cs_arrays.arrays[offset].arr[i][0] != 0) {
                            cmn_err(CE_WARN, "PID: %d collected %d timestamps",
                                    cs_arrays.arrays[offset].arr[i][0],
                                    cs_arrays.arrays[offset].arr[i][1]);
                        }
                    }
                }
            }
        }

        if (num_logs_written >= NUM_LOG_FILES || called_from_destroy_func) {
            cb_func_callsite_initialized = 0;
            num_logs_written = 0;
            cs_arrays.row_select_iter = 0;
            vmem_free(cs_arrays.pids, sizeof(unsigned int) * cs_arrays.pid_cap);
            for (i = 0; i < NUM_LOG_FILES; i++) {
                for (j = 0; j < cs_arrays.pid_cap; j++) {
                    vmem_free(cs_arrays.arrays[i].arr[j], sizeof(hrtime_t) * STATIC_COL_CAP);
                }
                vmem_free(cs_arrays.arrays[i].arr, sizeof(hrtime_t *) * cs_arrays.pid_cap);
            }
        }
    }
}

/**
 * cb_func_callsite_ctor -
 *
 * This function initializes the variables needed by the function cb_func_callsite_cb.
 * If this function is not called, no collections will be done even if the
 * log_callback_t cb_func is called.
 */
static void cb_func_callsite_ctor(void *ctor_args)
{
    int i;
    int j;

    mutex_enter(cb_func_callsite_mutex);
    /* There is no constructor args needed, so always pass NULL to this */
    VERIFY3P(ctor_args, ==, NULL);

    if (!cb_func_callsite_initialized) {
        cb_func_callsite_max_pids = get_log_cb_max_pids();
        cs_arrays.pid_cap = cb_func_callsite_max_pids;
        cs_arrays.row_select_iter = 0;
        cs_arrays.pids = vmem_zalloc(sizeof(unsigned int) * cs_arrays.pid_cap, KM_SLEEP);
        VERIFY3P(cs_arrays.pids, !=, NULL);
        for (i = 0; i < NUM_LOG_FILES; i++) {
            cs_arrays.arrays[i].dumped = 0;
            cs_arrays.arrays[i].array_size = 0;
            cs_arrays.arrays[i].total_pids = 0;
            cs_arrays.arrays[i].dump_file = NULL;
            cs_arrays.arrays[i].file_offset = 0;
            mutex_init(&cs_arrays.arrays[i].lock, NULL, MUTEX_DEFAULT, NULL);
            cs_arrays.arrays[i].arr = 
                vmem_alloc(sizeof(hrtime_t *) * cs_arrays.pid_cap, KM_SLEEP);
            VERIFY3P(cs_arrays.arrays[i].arr, !=, NULL);
            for (j = 0; j < cs_arrays.pid_cap; j++) {
                cs_arrays.arrays[i].arr[j] = 
                    vmem_zalloc(sizeof(hrtime_t) * STATIC_COL_CAP, KM_SLEEP);
                VERIFY3P(cs_arrays.arrays[i].arr[j], !=, NULL);
            }
        } 
        cb_func_callsite_initialized = 1;
        spl_log_cb_func_callsite_reset_counters = 0;
        if (spl_log_cb_debug_enabled()) {
            cmn_err(CE_WARN, "Initialized the data for timestamping call sites in %s and will watch up to %d pids", 
                    __func__, cs_arrays.pid_cap);
        }
    }
    mutex_exit(cb_func_callsite_mutex);
}

/**
 * cb_func_callsite_cb - 
 * @cb_args : Arguments passed to cb_func, see create_cb_func_callsite_args
 *
 * This function adds a timestamp for the PID for the current call site specified
 * by the offset parameter. Once all PID's for a given call site have collected
 * a total of STATIC_LIST_CAP, the log file specifed by LOG_FILE_CS[offset] will
 * be written out.
 *
 * Ideally this should be used with threads where no locking should occcur; however, 
 * a race condition does exist when adding items to arrays. 
 */
static void cb_func_callsite_cb(void *cb_args)
{
    int i = 0;
    int pid_row = 0;
    int call_site_name_len = 0;
    int all_logs_dumped = 1;
    boolean_t called_from_destroy_func = B_FALSE;
    boolean_t pid_in_slot = B_FALSE;

    VERIFY3P(cb_args, !=, NULL);

    if (cb_func_callsite_initialized == 0 &&
        spl_log_cb_func_callsite_reset_counters == 1) {
        cb_func_callsite_ctor(NULL);
    }

    unsigned int curr_pid = ((cb_func_callsite_args_t *)cb_args)->pid; 
    unsigned int offset = ((cb_func_callsite_args_t *)cb_args)->offset;
    const char *call_site_name = 
        ((cb_func_callsite_args_t *)cb_args)->call_site_name;

    /* Making sure a valid offset was passed */
    ASSERT3U(offset, <, NUM_LOG_FILES);

    if (!(cs_arrays.arrays[offset].dumped)) {
        if (!cb_func_callsite_initialized) {
            /* Initilization code not called, so just bail */
            return;
        }

        /* First find the row for curr_pid */
        pid_row = curr_pid % cs_arrays.pid_cap;

        /* 
         * If another PID resides in the slot find next available slot. If no
         * slot is available just bail
         */
        while ((cs_arrays.pids[pid_row] != curr_pid) && (cs_arrays.row_select_iter < cs_arrays.pid_cap)) {
            if (cs_arrays.pids[pid_row] != 0) {
                cs_arrays.row_select_iter += 1;
                pid_row = (pid_row + 1) % cs_arrays.pid_cap;
            } else {
                cs_arrays.pids[pid_row] = curr_pid;
                pid_in_slot = B_TRUE; 
                break;
            }
        }

        /* 
         * While this seems rather pointless... It is the only way to avoid locking the array and throwing
         * off time stamp values. Just simply do a final check to see if we are in the PID array. There is always
         * the possibility we landed in another slot other than pid_row when less threads are run then
         * pid_cap.
         */
        if (!pid_in_slot) {
            for (i = 0; i < cs_arrays.pid_cap; i++) {
                if (cs_arrays.pids[i] == curr_pid) {
                    pid_in_slot = B_TRUE;
                    break;
                }
            }
        }

        /* Bailing here if no avialable slot */
        if (!pid_in_slot) {
            return;
        }

        cs_arrays.arrays[offset].array_size += 1;
        cs_arrays.arrays[offset].arr[pid_row][0] = curr_pid;
        cs_arrays.arrays[offset].arr[pid_row][cs_arrays.arrays[offset].arr[pid_row][1] + 2] = NSEC2USEC(gethrtime());
        cs_arrays.arrays[offset].arr[pid_row][1] += 1;
        if (cs_arrays.arrays[offset].array_size >= STATIC_LIST_CAP) {
            /* Only a single thread will write the log file */
            mutex_enter(&cs_arrays.arrays[offset].lock);
            
            if (!cs_arrays.arrays[offset].dumped) {
                cs_arrays.arrays[offset].dump_file = spl_filp_open(LOG_FILE_CS[offset], O_WRONLY|O_CREAT, 0666, NULL);
                if (cs_arrays.arrays[offset].dump_file == NULL) {
                    cmn_err(CE_WARN, "Could not open cs_arrays.array[%d].dump_file = %s", offset, LOG_FILE_CS[offset]);
                    mutex_exit(&cs_arrays.arrays[offset].lock);
                    return;
                }
                cs_arrays.arrays[offset].dumped = 1;
            } else {
                mutex_exit(&cs_arrays.arrays[offset].lock);
                return;
            }

            /* First writing out the total number of PID's with timestamps to file */
            for (i = 0; i < cs_arrays.pid_cap; i++) {
                if (cs_arrays.arrays[offset].arr[i][0] != 0) {
                    cs_arrays.arrays[offset].total_pids += 1;
                }
            }

            spl_kernel_write(cs_arrays.arrays[offset].dump_file, 
                             &cs_arrays.arrays[offset].total_pids, 
                             sizeof(hrtime_t), 
                             &cs_arrays.arrays[offset].file_offset);

            /* 
             * Next writing out the name of the call site by first writing out the length
             * of the call site name and then the actual call site
             */
            call_site_name_len = strlen(call_site_name);
            /* Length of call site name */
            spl_kernel_write(cs_arrays.arrays[offset].dump_file,
                             &call_site_name_len,
                             sizeof(int),
                             &cs_arrays.arrays[offset].file_offset);

            /* Call site name */
            spl_kernel_write(cs_arrays.arrays[offset].dump_file,
                             call_site_name,
                             sizeof(char) * call_site_name_len,
                             &cs_arrays.arrays[offset].file_offset);

            /* Now writing out each PID with it corresponding timestamps */
            for (i = 0; i < cs_arrays.pid_cap; i++) {
                if (cs_arrays.arrays[offset].arr[i][0] != 0) {
                    spl_kernel_write(cs_arrays.arrays[offset].dump_file, 
                                     &cs_arrays.arrays[offset].arr[i], 
                                     sizeof(hrtime_t)*(cs_arrays.arrays[offset].arr[i][1] + 2), 
                                     &cs_arrays.arrays[offset].file_offset);
                }
            }
            spl_filp_close(cs_arrays.arrays[offset].dump_file);
            for (i = 0; i < NUM_LOG_FILES; i++) {
                all_logs_dumped &= cs_arrays.arrays[offset].dumped;
            }
            if (all_logs_dumped) {
                cb_func_callsite_dtor(&called_from_destroy_func);
            }
            mutex_exit(&cs_arrays.arrays[offset].lock);
        }
    }
}

/***************************************************************************/
/*                         Function Defintions                             */
/***************************************************************************/
cb_func_callsite_args_t 
create_cb_func_callsite_args(zio_t *zio,
                             unsigned int offset,
                             const char *call_site_name)
{
    cb_func_callsite_args_t cb_args;
    
    cb_args.pid = cb_zio_log_get_correct_pid(zio);
    cb_args.offset = offset;
    cb_args.call_site_name = call_site_name;
    return (cb_args);
}
EXPORT_SYMBOL(create_cb_func_callsite_args);

log_callback_t *create_cb_func_callsite(void)
{
    log_callback_t *cb_log =
        kmem_alloc(sizeof(log_callback_t), KM_SLEEP);
    VERIFY3P(cb_log, !=, NULL);
    cb_log->cb_name = "cb_func_callsite";
    cb_log->zio_type = zio_is_read;
    cb_log->ctor = cb_func_callsite_ctor;
    cb_log->dtor = cb_func_callsite_dtor;
    cb_log->cb_func = cb_func_callsite_cb;
    cb_log->dump_func = NULL; 
    cb_func_callsite_mutex =
            kmem_alloc(sizeof(kmutex_t), KM_SLEEP);
    VERIFY3P(cb_func_callsite_mutex, !=, NULL);
    mutex_init(cb_func_callsite_mutex, NULL, MUTEX_DEFAULT, NULL);
    cb_log->ctor(NULL);
    return (cb_log);
}
EXPORT_SYMBOL(create_cb_func_callsite);

void destroy_cb_func_callsite(log_callback_t *cb_func_callsite)
{
    boolean_t called_from_destroy_func = B_TRUE;
    VERIFY3P(cb_func_callsite, !=, NULL);
    cb_func_callsite->dtor(&called_from_destroy_func);
    kmem_free(cb_func_callsite_mutex, sizeof(kmutex_t));
    kmem_free(cb_func_callsite, sizeof(log_callback_t));
}
EXPORT_SYMBOL(destroy_cb_func_callsite);
