/**
 * Copyright (c) 2015 Stack Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "os/os.h"
#include <assert.h>

/* XXX:
 * 1) Should I check to see if we are within an ISR for some of these?
 * 2) Would I do anything different for os_sem_release() if we were in an
 *    ISR when this was called?
 */

/**
 * os sem create
 *  
 * Create a semaphore and initialize it. 
 * 
 * @param sem Pointer to semaphore 
 *        tokens: # of tokens the semaphore should contain initially.   
 * 
 * @return os_error_t 
 *      OS_INVALID_PARM     Semaphore passed in was NULL.
 *      OS_OK               no error.
 */
os_error_t
os_sem_create(struct os_sem *sem, uint16_t tokens)
{
    if (!sem) {
        return OS_INVALID_PARM;
    }

    sem->sem_tokens = tokens;
    SLIST_FIRST(&sem->sem_head) = NULL;

    return OS_OK;
}

/**
 * os sem release
 *  
 * Release a semaphore. 
 * 
 * @param sem Pointer to the semaphore to be released
 * 
 * @return os_error_t 
 *      OS_INVALID_PARM Semaphore passed in was NULL.
 *      OS_OK No error
 */
os_error_t
os_sem_release(struct os_sem *sem)
{
    int resched;
    os_sr_t sr;
    struct os_task *current;
    struct os_task *rdy;

    /* Check for valid semaphore */
    if (!sem) {
        return OS_INVALID_PARM;
    }

    /* Get current task */
    resched = 0;
    current = os_sched_get_current_task();

    OS_ENTER_CRITICAL(sr);

    /* Check if tasks are waiting for the semaphore */
    rdy = SLIST_FIRST(&sem->sem_head);
    if (rdy) {
        /* Clear flag that we are waiting on the semaphore */
        rdy->t_flags &= ~OS_TASK_FLAG_SEM_WAIT;
        /* XXX: should os_sched_wakeup clear this flag? Should it clear
           all the flags? Is this a problem? */

        /* There is one waiting. Wake it up */
        SLIST_REMOVE_HEAD(&sem->sem_head, t_sem_list);
        SLIST_NEXT(rdy, t_sem_list) = NULL;
        os_sched_wakeup(rdy, 0, 0);

        /* Schedule if waiting task higher priority */
        if (current->t_prio > rdy->t_prio) {
            resched = 1;
        }
    } else {
        /* Add to the number of tokens */
        sem->sem_tokens++;
    }

    OS_EXIT_CRITICAL(sr);

    /* Re-schedule if needed */
    if (resched) {
        os_sched(rdy, 0);
    }

    return OS_OK;
}

/**
 * os sem pend 
 *  
 * Pend (wait) for a semaphore. 
 * 
 * @param mu Pointer to semaphore.
 * @param timeout Timeout, in os ticks. A timeout of 0 means do 
 *                not wait if not available. A timeout of
 *                0xFFFFFFFF means wait forever.
 *              
 * 
 * @return os_error_t 
 *      OS_INVALID_PARM     Semaphore passed in was NULL.
 *      OS_TIMEOUT          Semaphore was owned by another task and timeout=0
 *      OS_OK               no error.
 */ 
os_error_t
os_sem_pend(struct os_sem *sem, uint32_t timeout)
{
    os_sr_t sr;
    os_error_t rc;
    int sched;
    struct os_task *current;
    struct os_task *entry;
    struct os_task *last;

    /* Check for valid semaphore */
    if (!sem) {
        return OS_INVALID_PARM;
    }

    /* Assume we dont have to put task to sleep; get current task */
    sched = 0;
    current = os_sched_get_current_task();

    OS_ENTER_CRITICAL(sr);

    /* 
     * If there is a token available, take it. If no token, either return
     * with error if timeout was 0 or put this task to sleep.
     */
    if (sem->sem_tokens != 0) {
        sem->sem_tokens--;
        rc = OS_OK;
    } else if (timeout == 0) {
        rc = OS_TIMEOUT;
    } else {
        /* Link current task to tasks waiting for semaphore */
        current->t_flags |= OS_TASK_FLAG_SEM_WAIT;
        last = NULL;
        if (!SLIST_EMPTY(&sem->sem_head)) {
            /* Insert in priority order */
            SLIST_FOREACH(entry, &sem->sem_head, t_sem_list) {
                if (current->t_prio < entry->t_prio) { 
                    break;
                }
                last = entry;
            }
        }

        if (last) {
            SLIST_INSERT_AFTER(last, current, t_sem_list);
        } else {
            SLIST_INSERT_HEAD(&sem->sem_head, current, t_sem_list);
        }

        /* We will put this task to sleep */
        sched = 1;
    }

    OS_EXIT_CRITICAL(sr);

    if (sched) {
        os_sched_sleep(current, timeout);

        /* XXX: what happens if task gets posted an event while waiting for
           a sem? Does the task get woken up? */

        /* Check if we timed out or got the semaphore */
        if (current->t_flags & OS_TASK_FLAG_SEM_WAIT) {
            OS_ENTER_CRITICAL(sr);
            current->t_flags &= ~OS_TASK_FLAG_SEM_WAIT;
            OS_EXIT_CRITICAL(sr);
            rc = OS_TIMEOUT;
        } else {
            rc = OS_OK; 
        }
    }

    return rc;
}

/**
 * os sem delete
 * 
 * Delete a semaphore. 
 *  
 * @param mu Pointer to semaphore to delete
 * 
 * @return os_error_t 
 *      OS_INVALID_PARM     Semaphore passed in was NULL.
 *      OS_OK               no error.
 */
os_error_t
os_sem_delete(struct os_sem *sem)
{
    os_sr_t sr;
    struct os_task *current;
    struct os_task *rdy;

    /* Check for valid semaphore */
    if (!sem) {
        return OS_INVALID_PARM;
    }

    /* Get currently running task */
    current = os_sched_get_current_task();

    OS_ENTER_CRITICAL(sr);

    /* Remove all tokens from semaphore */
    sem->sem_tokens = 0;

    /* Now, go through all the tasks waiting on the semaphore */
    while (!SLIST_EMPTY(&sem->sem_head)) {
        rdy = SLIST_FIRST(&sem->sem_head);
        SLIST_REMOVE_HEAD(&sem->sem_head, t_sem_list);
        SLIST_NEXT(rdy, t_sem_list) = NULL;
        os_sched_wakeup(rdy, 0, 0);
    }

    /* XXX: the os_sched_next_task() call is sort of heavyweight. Should
       I just check priority of first task on sem list? */
    /* Is there a task that is ready that is higher priority than us? */
    rdy = os_sched_next_task(0);
    if (rdy != current) {
        /* Re-schedule */
        OS_EXIT_CRITICAL(sr);
        os_sched(rdy, 0);
    } else {
        OS_EXIT_CRITICAL(sr);
    }

    return OS_OK;
}

