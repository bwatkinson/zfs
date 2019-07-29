#ifndef _LOG_CALLBACKS_IMPL_H
#define _LOG_CALLBACKS_IMPL_H

#include <sys/zfs_context.h>
#include <sys/zio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct log_callback_s
{
    const char *cb_name;        /* name of callback */
    zio_cb_log_type_e zio_type; /* type of ZIO op that this callback cares about */
    void (*ctor)(void *);       /* constructor for callback (intitialization) */
    void (*dtor)(void *);       /* destructor of callback (cleanup) */
    void (*cb_func)(void *);    /* callback function */
    void (*dump_func)(void *);  /* extra dump funcation if necessary */
    list_node_t cb_node;        /* link in callback list */
} log_callback_t; 

typedef struct log_callback_list_s
{
    int num_callbacks;
    list_t list;
} log_callback_list_t;

#ifdef __cplusplus
}
#endif

#endif /* _LOG_CALLBACKS_IMPL_H */
