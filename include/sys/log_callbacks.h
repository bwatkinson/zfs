#ifndef _LOG_CALLBACKS_H
#define _LOG_CALLBACKS_H

#include <sys/log_callbacks_impl.h>
#include <sys/spa_impl.h>
#include <sys/spa.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOG_CALLBACK_DEBUG 1

static inline zio_t *cb_zio_walk_parents(zio_t *cio, zio_link_t **zl)
{
    list_t *pl = &cio->io_parent_list;

    *zl = (*zl == NULL) ? list_head(pl) : list_next(pl, *zl);
    if (*zl == NULL)
        return (NULL);

    ASSERT((*zl)->zl_child == cio);
    return ((*zl)->zl_parent);
}

static inline zio_t *cb_zio_log_cb_root_parent(zio_t *zio)
{
    zio_link_t *zl = NULL;
    zio_t *pio = NULL;
    zio_t *prev_pio;

    VERIFY3P(zio, !=, NULL);

    /* Current zio may not have any parents */
    prev_pio = zio;
    
    /*
     * Now just going to walk parents till we get to the last parent
     * in the zio chain
     */
    pio = cb_zio_walk_parents(zio, &zl);
    while (pio != NULL) {
        prev_pio = pio;
        pio = cb_zio_walk_parents(zio, &zl);
    }
    return prev_pio;
}

static inline unsigned int cb_zio_log_get_correct_pid(zio_t *zio)
{
    zio_t *pio = NULL;

    VERIFY3P(zio, !=, NULL);
    
    pio = cb_zio_log_cb_root_parent(zio);
    return (pio->zio_pid);
}


#define CB_ZIO_DO_NOT_IGNORE(z)     \
({                                  \
    boolean_t _ret_do_log = B_TRUE; \
    if (!((z)->zlog_type))          \
        _ret_do_log = B_FALSE;      \
    _ret_do_log;                    \
})

extern log_callback_list_t *create_log_callback_list(void);

extern void init_log_callback_list(log_callback_list_t *cb_list);

extern void add_callback_to_list(log_callback_list_t *cb_list, log_callback_t *cb);

extern log_callback_t *remove_callback_from_list(log_callback_list_t *cb_list, const char *cb_name);

extern int get_length_of_log_callback_list(log_callback_list_t *cb_list);

extern void destroy_log_callback_list(log_callback_list_t *cb_list);

extern int execute_cb(log_callback_list_t *cb_list,
                      zio_cb_log_type_e zlog_t, 
                      const char *cb_nm,
                      void *cb_args);

extern int execute_dump_func(log_callback_list_t *cb_list,
                             zio_cb_log_type_e zlog_t,
                             const char *cb_nm,
                             void *dump_args);

#ifdef __cplusplus
}
#endif

#endif /* _LOG_CALLBACKS_H */
