#include <sys/log_callbacks.h>

/*
 * This macro allows for a log_callback_list_t to be
 * iterated through
 *
 * l - log_callback_list_t *
 * c - log_callback_func_t *
 */
#define for_each_cb_in_log_cb_list(l, cb)  \
    for (cb = list_head(&((l)->list));     \
         cb != NULL;                       \
         cb = list_next(&((l)->list), cb))

/*
 * This macro checks whether a callback is in the
 * callback list given the name of the callback. Returns
 * 1 on true, else returns 0.
 *
 * l  - log_callback_list_t *
 * nm - char *
 */
#define cb_in_log_cb_list(l, nm)                  \
({                                                \
    int ret = 0;                                  \
    if (get_cb_from_log_cb_list((l), nm) != NULL) \
        ret = 1;                                  \
    ret;                                          \
})

static void print_callbacks(log_callback_list_t *cb_list);
static log_callback_t *get_cb_from_log_cb_list(log_callback_list_t *cb_list,
                                               const char *cb_nm);

log_callback_list_t *create_log_callback_list(void)
{
    log_callback_list_t *cb_list = NULL;
    cb_list = kmem_alloc(sizeof (log_callback_list_t), KM_SLEEP);
    init_log_callback_list(cb_list);
#if defined(LOG_CALLBACK_DEBUG)
    cmn_err(CE_NOTE, "in function: %s", __func__);
    print_callbacks(cb_list);
#endif
    return (cb_list);        
}
EXPORT_SYMBOL(create_log_callback_list);

void init_log_callback_list(log_callback_list_t *cb_list)
{
    VERIFY3P(cb_list, !=, NULL);
    list_create(&cb_list->list, sizeof(log_callback_t),
        offsetof(log_callback_t, cb_node));
    cb_list->num_callbacks = 0;
}
EXPORT_SYMBOL(init_log_callback_list);

void add_callback_to_list(log_callback_list_t *cb_list, log_callback_t *cb)
{
    VERIFY3P(cb_list, !=, NULL);
    VERIFY3P(cb, !=, NULL);

    list_insert_tail(&cb_list->list, cb);
    cb_list->num_callbacks += 1;
#if defined(LOG_CALLBACK_DEBUG)
    cmn_err(CE_NOTE, "in function: %s", __func__);
    print_callbacks(cb_list);
#endif
}
EXPORT_SYMBOL(add_callback_to_list);

log_callback_t *remove_callback_from_list(log_callback_list_t *cb_list, const char *cb_nm)
{
    log_callback_t *cb = NULL;
    
    VERIFY3P(cb_list, !=, NULL);
    VERIFY3P(cb_nm, !=, NULL);
    
    cb = get_cb_from_log_cb_list(cb_list, cb_nm);
    if (cb != NULL) {
        list_remove(&cb_list->list, cb);
        cb_list->num_callbacks -= 1;
    }
#if defined(LOG_CALLBACK_DEBUG)
    cmn_err(CE_NOTE, "in function: %s", __func__);
    print_callbacks(cb_list);
#endif
    return (cb);
}
EXPORT_SYMBOL(remove_callback_from_list);

int get_length_of_log_callback_list(log_callback_list_t *cb_list)
{
    VERIFY3P(cb_list, !=, NULL);
    return (cb_list->num_callbacks);
}
EXPORT_SYMBOL(get_length_of_log_callback_list);

void destroy_log_callback_list(log_callback_list_t *cb_list)
{
    VERIFY3P(cb_list, !=, NULL);

#if defined(LOG_CALLBACK_DEBUG)
    cmn_err(CE_NOTE, "in function: %s", __func__);
    print_callbacks(cb_list);
#endif
    list_destroy(&cb_list->list);
    kmem_free(cb_list, sizeof(log_callback_list_t));
}
EXPORT_SYMBOL(destroy_log_callback_list);

int execute_cb(log_callback_list_t *cb_list, 
               zio_cb_log_type_e zlog_t,
               const char *cb_nm, 
               void *cb_args)
{
    log_callback_t *cb = NULL;
    int ret = 1;

    cb = get_cb_from_log_cb_list(cb_list, cb_nm);
    if (cb)
    {
        if(zlog_t == zio_is_any) {
            cb->cb_func(cb_args);
            ret = 0;
        } else if (cb->zio_type & zlog_t) {
            cb->cb_func(cb_args);
            ret = 0;
        }
    }
    return (ret);
}
EXPORT_SYMBOL(execute_cb);

int execute_dump_func(log_callback_list_t *cb_list, 
                      zio_cb_log_type_e zlog_t,
                      const char *cb_nm, 
                      void *cb_args)
{
    log_callback_t *cb = NULL;
    int ret = 1;

    cb = get_cb_from_log_cb_list(cb_list, cb_nm);
    if (cb)
    {
        if(zlog_t == zio_is_any) {
            cb->dump_func(cb_args);
            ret = 0;
        } else if (cb->zio_type & zlog_t) {
            cb->dump_func(cb_args);
            ret = 0;
        }
    }
    return (ret);
}
EXPORT_SYMBOL(execute_dump_func);

static void print_callbacks(log_callback_list_t *cb_list)
{
    log_callback_t *cb_func = NULL;
    int offset = 0;

    VERIFY3P(cb_list, !=, NULL);
    
    if (list_is_empty(&cb_list->list)) {
        cmn_err(CE_NOTE, "Callback list is currently empty");
    } else {
        cmn_err(CE_NOTE, "Callback list contains: %d callbacks:", cb_list->num_callbacks);
        for_each_cb_in_log_cb_list(cb_list, cb_func) {
            cmn_err(CE_NOTE, "%d: %s", offset, cb_func->cb_name);
            offset += 1;
        }
    }
}

static log_callback_t *get_cb_from_log_cb_list(log_callback_list_t *cb_list,
                                               const char *cb_nm)
{
    log_callback_t *ret = NULL;
    log_callback_t *cb_func = NULL;

    VERIFY3P(cb_list, !=, NULL);
    VERIFY3P(cb_nm, !=, NULL);

    for_each_cb_in_log_cb_list(cb_list, cb_func) {
        if (!strcmp(cb_func->cb_name, cb_nm)) {
            ret = cb_func;
            break;
        }
    }
    return (ret);
}
