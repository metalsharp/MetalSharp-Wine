/*
 * MetalSharp userspace synchronization server state
 *
 * Copyright (C) 2026 Radim Vesely
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

extern int do_msync(void);

struct msync;

#ifdef __APPLE__

extern int msync_get_shared_fd(void);
extern struct msync *create_msync( int low, int high, enum inproc_sync_type type );
extern unsigned int msync_get_index( struct msync *sync );
extern unsigned int msync_alloc_wait_slot(void);
extern int msync_register_wait( unsigned int slot_idx, const unsigned int *shm_idxs, unsigned int count );
extern void msync_unregister_wait( unsigned int slot_idx );
extern int msync_wake_waiters( unsigned int shm_idx );
extern void msync_grab_object( struct msync *sync );
extern void msync_destroy( struct msync *sync );
extern void msync_add_server_waiter( struct msync *sync );
extern void msync_remove_server_waiter( struct msync *sync );
extern int msync_get_event_state( struct msync *sync, int *manual, int *signaled );
extern int msync_event_signaled( struct msync *sync );
extern void msync_event_satisfied( struct msync *sync );
extern void msync_set_event( struct msync *sync );
extern void msync_reset_event( struct msync *sync );
extern int msync_semaphore_signaled( struct msync *sync );
extern void msync_semaphore_satisfied( struct msync *sync );
extern int msync_release_semaphore( struct msync *sync, unsigned int count, unsigned int *prev );
extern int msync_get_semaphore_state( struct msync *sync, unsigned int *count, unsigned int *max );
extern int msync_mutex_signaled( struct msync *sync, thread_id_t tid );
extern int msync_mutex_satisfied( struct msync *sync, thread_id_t tid, int *abandoned );
extern int msync_release_mutex( struct msync *sync, thread_id_t tid, unsigned int *prev );
extern int msync_get_mutex_state( struct msync *sync, thread_id_t tid, unsigned int *count,
                                    int *owned, int *abandoned );
extern int msync_abandon_mutex( struct msync *sync, thread_id_t tid );
extern unsigned int msync_try_wait_all( const unsigned int *entries, unsigned int count, thread_id_t tid );

#endif /* __APPLE__ */
