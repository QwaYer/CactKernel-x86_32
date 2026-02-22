#include "sync.h"
#include "task.h"   
#include "kernel.h" 


void spinlock_init(spinlock_t* lock) {
    lock->locked = 0;
}

void spinlock_acquire(spinlock_t* lock) {
    while (__sync_lock_test_and_set(&lock->locked, 1)) {
        while (lock->locked)
            __asm__ __volatile__("pause");
    }
}

void spinlock_release(spinlock_t* lock) {
    __sync_lock_release(&lock->locked);
}

static inline uint32_t read_eflags(void) {
    uint32_t flags;
    __asm__ __volatile__("pushf; pop %0" : "=r"(flags));
    return flags;
}

void irq_spinlock_init(irq_spinlock_t* lock) {
    spinlock_init(&lock->spin);
    lock->saved_flags = 0;
}

void irq_spinlock_acquire(irq_spinlock_t* lock) {
    lock->saved_flags = read_eflags();
    __asm__ __volatile__("cli");
    spinlock_acquire(&lock->spin);
}

void irq_spinlock_release(irq_spinlock_t* lock) {
    spinlock_release(&lock->spin);
    if (lock->saved_flags & (1 << 9))   /* IF = бит 9 в EFLAGS */
        __asm__ __volatile__("sti");
}

void mutex_init(mutex_t* m) {
    m->locked       = 0;
    m->owner        = 0;
    m->waiter_count = 0;
    spinlock_init(&m->guard);
}

void mutex_lock(mutex_t* m) {
    while (1) {
        spinlock_acquire(&m->guard);

        if (!m->locked) {
            m->locked = 1;
            m->owner  = current_task;
            spinlock_release(&m->guard);
            return;
        }

        if (current_task && m->waiter_count < MUTEX_WAIT_QUEUE_MAX) {
            m->waiters[m->waiter_count++] = current_task;

            spinlock_acquire(&scheduler_lock);
            current_task->state = TASK_SLEEPING;
            spinlock_release(&scheduler_lock);

            spinlock_release(&m->guard);

            schedule();

        } else {

            spinlock_release(&m->guard);
            kprint_color("mutex: waiter queue full, spinning!\n", 12 COLOR_LIGHT_RED );
            __asm__ __volatile__("pause");
        }
    }
}

int mutex_trylock(mutex_t* m) {
    spinlock_acquire(&m->guard);

    if (!m->locked) {
        m->locked = 1;
        m->owner  = current_task;
        spinlock_release(&m->guard);
        return 0;  
    }

    spinlock_release(&m->guard);
    return -1;     
}

void mutex_unlock(mutex_t* m) {
    spinlock_acquire(&m->guard);

    if (!m->locked) {
        spinlock_release(&m->guard);
        kprint_color("mutex: unlock of unlocked mutex!\n", 12);
        return;
    }

    m->locked = 0;
    m->owner  = 0;

    if (m->waiter_count > 0) {
        struct task_struct* woken = m->waiters[0];

        for (uint32_t i = 1; i < m->waiter_count; i++)
            m->waiters[i - 1] = m->waiters[i];
        m->waiter_count--;

        spinlock_release(&m->guard);

        spinlock_acquire(&scheduler_lock);
        if (woken && woken->state == TASK_SLEEPING)
            woken->state = TASK_READY;
        spinlock_release(&scheduler_lock);
    } else {
        spinlock_release(&m->guard);
    }
}