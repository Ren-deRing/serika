#include <serika/cpu.h>
#include <serika/wait.h>

void wait_queue_init(struct wait_queue *wq) {
    spin_lock_init(&wq->lock);
    list_init(&wq->head);
}

void prepare_to_wait(struct wait_queue *wq, struct wait_entry *we) {
    struct task *curr = current_task();

    uint64_t flags = spin_lock_irqsave(&wq->lock);

    we->task = curr;
    if (!we->node.next) list_add_tail(&we->node, &wq->head);

    /* in the lock: a waker that sees us queued sees sleeping too */
    __atomic_store_n(&curr->state, TASK_SLEEPING, 5); /* __ATOMIC_SEQ_CST */

    spin_unlock_irqrestore(&wq->lock, flags);
}

void finish_wait(struct wait_queue *wq, struct wait_entry *we) {
    __atomic_store_n(&we->task->state, TASK_RUNNING, 5); /* __ATOMIC_SEQ_CST */

    uint64_t flags = spin_lock_irqsave(&wq->lock);

    if (we->node.next) list_del(&we->node);

    spin_unlock_irqrestore(&wq->lock, flags);
}

void wake_up(struct wait_queue *wq) {
    uint64_t flags = spin_lock_irqsave(&wq->lock);

    /* entries leave in finish_wait(), under this lock */
    for (list_node *n = wq->head.next; n != &wq->head; n = n->next) {
        struct wait_entry *we = container_of(n, struct wait_entry, node);
        wake_up_process(we->task);
    }

    spin_unlock_irqrestore(&wq->lock, flags);
}
