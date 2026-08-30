#ifndef SYNC_H
#define SYNC_H

#include <stdint.h>

struct task_struct;  

typedef struct {
    volatile uint32_t locked;
} spinlock_t;

void spin_lock_init   (spinlock_t* lock);
void spin_lock(spinlock_t* lock);
void spin_unlock(spinlock_t* lock);

typedef struct {
    spinlock_t spin;
    uint32_t   saved_flags;   
} irq_spinlock_t;

void irq_spinlock_init   (irq_spinlock_t* lock);
void irq_spinlock_acquire(irq_spinlock_t* lock);
void irq_spinlock_release(irq_spinlock_t* lock);


#define MUTEX_WAIT_QUEUE_MAX 64

typedef struct {
    volatile int        locked;
    spinlock_t          guard;                          /* protects mutex metadata */
    struct task_struct* owner;                          /* current lock owner */
    struct task_struct* waiters[MUTEX_WAIT_QUEUE_MAX];  /* FIFO waiter queue */
    uint32_t            waiter_count;
} mutex_t;

void mutex_init   (mutex_t* m);
void mutex_lock   (mutex_t* m);
void mutex_unlock (mutex_t* m);
int  mutex_trylock(mutex_t* m);

typedef struct {
    spinlock_t guard;
    int        count;
    struct task_struct* waiters[MUTEX_WAIT_QUEUE_MAX];
    uint32_t waiter_count;
} semaphore_t;

void sema_init(semaphore_t* s, int val);
void down(semaphore_t* s);
void up(semaphore_t* s);

#endif