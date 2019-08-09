/*
 * This file defines the following #defines and static variables
 * that can be udpated:
 * LOG_FILE_TASKQ            : Path and name of log file dumped by cb_spl_taskq_cb 
 *
 */ 

#include <sys/log_cb_taskq.h>
#include <linux/file_compat.h>
#include <sys/cmn_err.h>
#include <sys/kmem.h>
#include <sys/vmem.h>
#include <sys/mutex.h>
#include <sys/strings.h>

#define TASKQ_NM_LEN 32

/* 
 * Change this variable in order to set a default location where
 * the log file is dumped
 */
const static char *LOG_FILE_TASKQ = "/var/log/hrtime_taskq_counts.log";

/*
 * Initialization variable that is set and checked
 */
static int cb_spl_taskq_initialized = 0;
static kmutex_t *cb_spl_taskq_mutex = NULL;

/*
 * Defines whether or not the counters should be reset
 */
static uint_t spl_log_cb_spl_taskqs_reset_counters = 0;
module_param(spl_log_cb_spl_taskqs_reset_counters, uint, 0644);
MODULE_PARM_DESC(spl_log_cb_spl_taskqs_reset_counters,
    "Resets the counters for the SPL taskq SPL callback log");

/*
 * Defines the total number of counts to collect per SPL TASKQ
 */
static uint_t spl_log_cb_spl_taskqs_num_counts = 5000;
module_param(spl_log_cb_spl_taskqs_num_counts, uint, 0644);
MODULE_PARM_DESC(spl_log_cb_spl_taskqs_num_counts,
    "Sets the number of counts to collect for each of the SPL taskqs");

/*******************************************************/
/* Struct definitions for taskq counts.                */
/*******************************************************/
typedef struct taskq_name_thread_counts_s
{
    char taskq_name[TASKQ_NM_LEN];
    int num_threads_present;
    int *num_threads;
    int num_counts_collected;
} taskq_name_thread_counts_t;

typedef struct taskq_counts_s
{
    taskq_name_thread_counts_t *taskqs;
    char **taskq_names;
    int dumped;
    int total_taskqs;
    struct file *dump_file;
    loff_t file_offset;
    unsigned int counts_per_taskq;
    int num_taskqs_done;
    kmutex_t lock;
} taskq_counts_t;

/*****************************************/
/*       Static taskq count struct       */
/*****************************************/
static taskq_counts_t all_taskq_counts;

/***********************************************************/
/*              Static Function Defintions                 */
/***********************************************************/
static void cb_spl_taskq_dtor(void *dtor_settings)
{
    int i;
    call_ctor_dtor_settings_e settings;

    if (spl_log_cb_debug_enabled()) {
        cmn_err(CE_NOTE, "Inside %s", __func__);
    }
    
    VERIFY3P(dtor_settings, !=, NULL);

    settings = *((call_ctor_dtor_settings_e *)dtor_settings);

    if (cb_spl_taskq_initialized) {
        cb_spl_taskq_initialized = 0;
    
        for (i = 0; i < all_taskq_counts.total_taskqs; i++) {
            /* Busy wait till all threads are no longer using the taskqs */
            while (all_taskq_counts.taskqs[i].num_threads_present > 0) {}
        } 
        
        if (spl_log_cb_debug_enabled()) {
            cmn_err(CE_NOTE, "cb_spl_taskq_initialized = %d",
                    cb_spl_taskq_initialized);
            for (i = 0; i < all_taskq_counts.total_taskqs; i++){
                cmn_err(CE_NOTE, "all_taskq_counts.taskq[%d].taskq_name = %s collected = %d counts", 
                        i, all_taskq_counts.taskqs[i].taskq_name, all_taskq_counts.taskqs[i].num_counts_collected);
            }
        }
       
        for (i = 0; i < all_taskq_counts.total_taskqs; i++) {
            vmem_free(all_taskq_counts.taskqs[i].num_threads, sizeof(int) * 
                      STATIC_COL_CAP_TASKQ(all_taskq_counts.counts_per_taskq));
        } 
        vmem_free(all_taskq_counts.taskqs, 
                  sizeof(taskq_name_thread_counts_t) * all_taskq_counts.total_taskqs);
        all_taskq_counts.taskqs = NULL;
        if (settings & DTOR_DESTROY_FUNC) {
            for (i = 0; i < all_taskq_counts.total_taskqs; i++) {
                strfree(all_taskq_counts.taskq_names[i]);
            }
            vmem_free(all_taskq_counts.taskq_names, sizeof(char *) * all_taskq_counts.total_taskqs);    
        }
    }
}

/**
 * cb_spl_taskq_ctor -
 * @ctor_args : Array of the names of the taskqs to collect counts for and total number
 *              of taskq names (see cb_spl_taskq_ctor_args_t)
 *
 * This function initializes the variables needed to use the functions
 * cb_spl_taskq_cb and cb_spl_taskq_dump. If this function
 * is not called, no taskq count collections will be done even if the
 * taskq count add function is called.
 */ 
static void cb_spl_taskq_ctor(void *ctor_args)
{
    int i;
    
    mutex_enter(cb_spl_taskq_mutex);

    VERIFY3P(ctor_args, !=, NULL);
    char **taskq_names = ((cb_spl_taskq_ctor_args_t *)ctor_args)->taskq_names;
    int num_taskqs = ((cb_spl_taskq_ctor_args_t *)ctor_args)->num_taskqs;
    call_ctor_dtor_settings_e settings = ((cb_spl_taskq_ctor_args_t *)ctor_args)->settings;
     
    if (!cb_spl_taskq_initialized) {
        all_taskq_counts.taskqs = NULL;
        all_taskq_counts.total_taskqs = num_taskqs;
        all_taskq_counts.counts_per_taskq = spl_log_cb_spl_taskqs_num_counts;
        all_taskq_counts.dumped = 0;
        all_taskq_counts.dump_file = NULL;
        all_taskq_counts.file_offset = 0;
        all_taskq_counts.num_taskqs_done = 0;
        while (!all_taskq_counts.taskqs) {
            all_taskq_counts.taskqs = 
                vmem_zalloc(sizeof(taskq_name_thread_counts_t) * all_taskq_counts.total_taskqs, KM_NOSLEEP);
        }
        for (i = 0; i < all_taskq_counts.total_taskqs; i++) {
            all_taskq_counts.taskqs[i].num_threads = NULL;
            while (all_taskq_counts.taskqs[i].num_threads == NULL) {
                all_taskq_counts.taskqs[i].num_threads = 
                    vmem_zalloc(sizeof(int) * STATIC_COL_CAP_TASKQ(all_taskq_counts.counts_per_taskq), KM_NOSLEEP);
            }
        }
        ASSERT3P(all_taskq_counts.taskqs, !=, NULL);
        mutex_init(&all_taskq_counts.lock, NULL, MUTEX_DEFAULT, NULL);
        if (CTOR_INITIAL & settings) {
            all_taskq_counts.taskq_names = NULL;
            while (!all_taskq_counts.taskq_names) {
                all_taskq_counts.taskq_names = 
                    vmem_alloc(sizeof(char *) * all_taskq_counts.total_taskqs, KM_SLEEP);
            }
            for (i = 0; i < all_taskq_counts.total_taskqs; i++) {
                all_taskq_counts.taskq_names[i] = strdup(taskq_names[i]);
            }
        }
        for (i = 0; i < all_taskq_counts.total_taskqs; i++) {
            all_taskq_counts.taskqs[i].num_threads_present = 0;
            strcpy(all_taskq_counts.taskqs[i].taskq_name, all_taskq_counts.taskq_names[i]);
        }
        cb_spl_taskq_initialized = 1;
        spl_log_cb_spl_taskqs_reset_counters = 0;
        if (spl_log_cb_debug_enabled()) {
            cmn_err(CE_NOTE, "Initialized the data for timestamping taskq's where all_taskq_counts.total_taskqs = %d in %s and will collect %d counts per taskq",
                    all_taskq_counts.total_taskqs, __func__, all_taskq_counts.counts_per_taskq);
            for (i = 0; i < all_taskq_counts.total_taskqs; i++) {
                cmn_err(CE_NOTE, "all_taskq_counts.taskq[%d].taskq_name = %s in %s with num_threads_present = %d", 
                        i, all_taskq_counts.taskqs[i].taskq_name,
                         __func__, all_taskq_counts.taskqs[i].num_threads_present);
            }
        }
    }
    mutex_exit(cb_spl_taskq_mutex);
}

/**
 * cb_spl_taskq_cb -
 * @cb_args : Arguments passed to cb_func, see create_cb_spl_taskq_args 
 *
 * This function will add the current active thread count for a given taskq
 * to the count array associated with taskq_name.
 * If the taskq_name was not passed to cb_spl_taskq_ctor, then
 * no counts records will be made for the taskq.
 */
static void cb_spl_taskq_cb(void *cb_args)
{
    int taskq_offset = 0;
    int thread_count_offset = 0;

    VERIFY3P(cb_args, !=, NULL);

    if (cb_spl_taskq_initialized == 0 &&
        spl_log_cb_spl_taskqs_reset_counters == 1) {
        cb_spl_taskq_ctor_args_t ctor_args =
        {
            .taskq_names = NULL,
            .num_taskqs = all_taskq_counts.total_taskqs,
            .settings = CTOR_RESET
        };
        cb_spl_taskq_ctor(&ctor_args);
    }
        
    const char *taskq_name = ((cb_spl_taskq_args_t *)cb_args)->taskq_name;
    int num_threads = ((cb_spl_taskq_args_t *)cb_args)->num_threads;

    if (!(all_taskq_counts.dumped)) {
        if (!cb_spl_taskq_initialized) {
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

        if ((all_taskq_counts.taskqs[taskq_offset].num_counts_collected + 1) <= 
             all_taskq_counts.counts_per_taskq) {
            all_taskq_counts.taskqs[taskq_offset].num_threads_present += 1;
            all_taskq_counts.taskqs[taskq_offset].num_counts_collected += 1;
            thread_count_offset = all_taskq_counts.taskqs[taskq_offset].num_threads[0] + 1;
            all_taskq_counts.taskqs[taskq_offset].num_threads[thread_count_offset] = num_threads;
            all_taskq_counts.taskqs[taskq_offset].num_threads[0] += 1;
            if (all_taskq_counts.taskqs[taskq_offset].num_counts_collected >= 
                all_taskq_counts.counts_per_taskq) {
                all_taskq_counts.num_taskqs_done += 1;
            }
            all_taskq_counts.taskqs[taskq_offset].num_threads_present -= 1;
        }
    }
}

/**
 * cb_spl_taskq_dump -
 *
 * This function dumps the taskq's active thread counts out to the log
 * file specified by LOG_FILE_TASKQ.
 */
static void cb_spl_taskq_dump(void *cb_args)
{
    int taskq_name_len = 0;
    int i;
    call_ctor_dtor_settings_e dtor_settings = DTOR_DUMP_FUNC;

    /* The arguements to this function should always be NULL */
    VERIFY3P(cb_args, ==, NULL);

    if (!(all_taskq_counts.dumped)) {
        if (!cb_spl_taskq_initialized) {
            /* Initilization code not called, so just bail */
            return;
        }
        

        if (all_taskq_counts.num_taskqs_done >= all_taskq_counts.total_taskqs) {
            /* Only a single thread will write the log file */
            mutex_enter(&all_taskq_counts.lock);
            if (!all_taskq_counts.dumped) {
                all_taskq_counts.dump_file = spl_filp_open(LOG_FILE_TASKQ, O_WRONLY|O_CREAT, 0666, NULL);
                if (all_taskq_counts.dump_file == NULL) {
                    cmn_err(CE_WARN, "Could not open all_taskq_counts.dump_file = %s", LOG_FILE_TASKQ);
                    mutex_exit(&all_taskq_counts.lock);
                    return;
                }
                all_taskq_counts.dumped = 1;
            } else {
                mutex_exit(&all_taskq_counts.lock);
                return;
            }
            
            /* First writing out the total number of taskqs */
            spl_kernel_write(all_taskq_counts.dump_file,
                             &all_taskq_counts.total_taskqs,
                             sizeof(int),
                             &all_taskq_counts.file_offset);

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
                spl_kernel_write(all_taskq_counts.dump_file,
                                 &taskq_name_len,
                                 sizeof(int),
                                 &all_taskq_counts.file_offset);

                /* Taskq name */
                spl_kernel_write(all_taskq_counts.dump_file,
                                 all_taskq_counts.taskqs[i].taskq_name,
                                 taskq_name_len * sizeof(char),
                                 &all_taskq_counts.file_offset);
                
                /* Taskq total number of thread counts collected */
                spl_kernel_write(all_taskq_counts.dump_file,
                                 &all_taskq_counts.taskqs[i].num_threads[0],
                                 sizeof(int),
                                 &all_taskq_counts.file_offset);    

                /* Taskq thread counts collected */
                spl_kernel_write(all_taskq_counts.dump_file,
                                 &all_taskq_counts.taskqs[i].num_threads[1],
                                 sizeof(int) * all_taskq_counts.taskqs[i].num_threads[0],
                                 &all_taskq_counts.file_offset);
                
            }
            
            spl_filp_close(all_taskq_counts.dump_file);
            cb_spl_taskq_dtor(&dtor_settings);
            mutex_exit(&all_taskq_counts.lock);
            return;
        }
    }
}

/***********************************************************/
/*                  Function Defintions                    */
/***********************************************************/
cb_spl_taskq_args_t 
create_cb_spl_taskq_args(const char *taskq_name,
                         int num_threads)
{
    cb_spl_taskq_args_t cb_args;

    cb_args.taskq_name = taskq_name,
    cb_args.num_threads = num_threads;
    return (cb_args);  
}
EXPORT_SYMBOL(create_cb_spl_taskq_args);

cb_spl_taskq_ctor_args_t *create_init_cb_spl_taskq_ctor_args(int num_taskqs)
{
    cb_spl_taskq_ctor_args_t *ctor_args = NULL;

    VERIFY3S(num_taskqs, >, 0);
    
    ctor_args = kmem_alloc(sizeof(cb_spl_taskq_ctor_args_t), KM_SLEEP);
    VERIFY3P(ctor_args, !=, NULL);
    ctor_args->taskq_names = 
        kmem_alloc(num_taskqs * sizeof(char *), KM_SLEEP);
    VERIFY3P(ctor_args->taskq_names, !=, NULL);
    ctor_args->num_taskqs = num_taskqs;
    ctor_args->settings = CTOR_INITIAL;
    return (ctor_args); 
}
EXPORT_SYMBOL(create_init_cb_spl_taskq_ctor_args);

void destroy_cb_spl_taskq_ctor_args(cb_spl_taskq_ctor_args_t *ctor_args)
{
    int i = 0;
    VERIFY3P(ctor_args, !=, NULL);
    for (i = 0; i < ctor_args->num_taskqs; i++)
        strfree(ctor_args->taskq_names[i]);
    kmem_free(ctor_args->taskq_names, sizeof(char *) * ctor_args->num_taskqs);
    kmem_free(ctor_args, sizeof(cb_spl_taskq_ctor_args_t));
}
EXPORT_SYMBOL(destroy_cb_spl_taskq_ctor_args);

void add_taskq_to_cb_spl_taskq_ctor_args(cb_spl_taskq_ctor_args_t *ctor_args,
                                         char *taskq_nm, 
                                         int offset)
{
    VERIFY3P(ctor_args, !=, NULL);
    VERIFY3S(offset, >, -1);
    VERIFY3S(offset, <, ctor_args->num_taskqs);
    ctor_args->taskq_names[offset] = strdup(taskq_nm);
}
EXPORT_SYMBOL(add_taskq_to_cb_spl_taskq_ctor_args);

log_callback_t *create_cb_spl_taskq(cb_spl_taskq_ctor_args_t *ctor_args)
{
    VERIFY3P(ctor_args, !=, NULL);
    log_callback_t *cb_log =
        kmem_alloc(sizeof(log_callback_t), KM_SLEEP);
    VERIFY3P(cb_log, !=, NULL);
    cb_log->cb_name = "cb_spl_taskq";
    cb_log->zio_type = zio_is_read;
    cb_log->ctor = cb_spl_taskq_ctor;
    cb_log->dtor = cb_spl_taskq_dtor;
    cb_log->cb_func = cb_spl_taskq_cb;
    cb_log->dump_func = cb_spl_taskq_dump;
    cb_spl_taskq_mutex =
        kmem_alloc(sizeof(kmutex_t), KM_SLEEP);
    VERIFY3P(cb_spl_taskq_mutex, !=, NULL);
    mutex_init(cb_spl_taskq_mutex, NULL, MUTEX_DEFAULT, NULL);
    cb_log->ctor(ctor_args);
    return (cb_log);
}
EXPORT_SYMBOL(create_cb_spl_taskq);

void destroy_cb_spl_taskq(log_callback_t *cb_spl_taskq)
{
    call_ctor_dtor_settings_e settings = DTOR_DESTROY_FUNC;
    VERIFY3P(cb_spl_taskq, !=, NULL);
    cb_spl_taskq->dtor(&settings);
    kmem_free(cb_spl_taskq_mutex, sizeof(kmutex_t));
    kmem_free(cb_spl_taskq, sizeof(log_callback_t));
}
EXPORT_SYMBOL(destroy_cb_spl_taskq);
