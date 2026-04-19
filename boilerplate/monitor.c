/*
 * monitor.c - Multi-Container Memory Monitor (Linux Kernel Module)
 *
 * Implements:
 *   - Linked list of monitored container processes
 *   - Mutex-protected list access
 *   - Periodic RSS checks (every CHECK_INTERVAL_SEC seconds)
 *   - Soft-limit: log once when process first exceeds soft limit
 *   - Hard-limit: SIGKILL and remove entry when process exceeds hard limit
 *   - ioctl: MONITOR_REGISTER and MONITOR_UNREGISTER
 *   - Clean teardown on module unload
 */

#include <linux/jiffies.h>
#include <linux/timer.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/version.h>

#include "monitor_ioctl.h"

#define DEVICE_NAME        "container_monitor"
#define CHECK_INTERVAL_SEC 1

/* ------------------------------------------------------------------ */
/* TODO 1: Linked-list node struct                                     */
/* ------------------------------------------------------------------ */

/*
 * monitored_entry – one tracked container process.
 *
 * Fields:
 *   node            – list linkage (embedded in the kernel list)
 *   pid             – host PID of the container process
 *   container_id    – human-readable name (from ioctl request)
 *   soft_limit      – bytes; warn once when RSS exceeds this
 *   hard_limit      – bytes; SIGKILL when RSS exceeds this
 *   soft_warned     – flag: 1 after the first soft-limit warning
 */
struct monitored_entry {
    struct list_head node;
    pid_t            pid;
    char             container_id[CONTAINER_ID_MAX];
    unsigned long    soft_limit;
    unsigned long    hard_limit;
    int              soft_warned;
};

/* ------------------------------------------------------------------ */
/* TODO 2: Global list and lock                                        */
/* ------------------------------------------------------------------ */

/*
 * We use a mutex (not a spinlock) because:
 *   - The timer callback runs in a softirq context on older kernels but
 *     with hrtimers / workqueue approach it can sleep.
 *   - Our ioctl paths may sleep (copy_from_user).
 *   - get_rss_bytes() calls get_task_mm() which can block.
 * A spinlock would be unsafe in any context that can sleep.
 * The trade-off is that mutex acquisition cannot happen in hard-IRQ
 * context, which is fine here because we only access the list from
 * the timer callback and from ioctl.
 */
static LIST_HEAD(monitored_list);
static DEFINE_MUTEX(list_lock);

/* ------------------------------------------------------------------ */
/* Provided: internal device / timer state                             */
/* ------------------------------------------------------------------ */
static struct timer_list monitor_timer;
static dev_t              dev_num;
static struct cdev        c_dev;
static struct class      *cl;

/* ------------------------------------------------------------------ */
/* Provided: RSS Helper                                                */
/* ------------------------------------------------------------------ */
static long get_rss_bytes(pid_t pid)
{
    struct task_struct *task;
    struct mm_struct   *mm;
    long rss_pages = 0;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (!task) {
        rcu_read_unlock();
        return -1;
    }
    get_task_struct(task);
    rcu_read_unlock();

    mm = get_task_mm(task);
    if (mm) {
        rss_pages = get_mm_rss(mm);
        mmput(mm);
    }
    put_task_struct(task);

    return rss_pages * PAGE_SIZE;
}

/* ------------------------------------------------------------------ */
/* Provided: soft-limit / hard-limit helpers                           */
/* ------------------------------------------------------------------ */
static void log_soft_limit_event(const char *container_id, pid_t pid,
                                 unsigned long limit_bytes, long rss_bytes)
{
    printk(KERN_WARNING
           "[container_monitor] SOFT LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

static void kill_process(const char *container_id, pid_t pid,
                         unsigned long limit_bytes, long rss_bytes)
{
    struct task_struct *task;

    rcu_read_lock();
    task = pid_task(find_vpid(pid), PIDTYPE_PID);
    if (task)
        send_sig(SIGKILL, task, 1);
    rcu_read_unlock();

    printk(KERN_WARNING
           "[container_monitor] HARD LIMIT container=%s pid=%d rss=%ld limit=%lu\n",
           container_id, pid, rss_bytes, limit_bytes);
}

/* ------------------------------------------------------------------ */
/* TODO 3: Timer callback                                              */
/* ------------------------------------------------------------------ */

/*
 * timer_callback – fires every CHECK_INTERVAL_SEC seconds.
 *
 * For each entry in the monitored list:
 *   1. Fetch current RSS via get_rss_bytes().
 *   2. If the process has exited (rss < 0), remove the entry.
 *   3. Else if RSS > hard_limit, kill it and remove the entry.
 *   4. Else if RSS > soft_limit and we have not warned yet,
 *      emit the warning and set soft_warned.
 *
 * Use list_for_each_entry_safe() so we can delete nodes during
 * iteration without use-after-free.
 */
static void timer_callback(struct timer_list *t)
{
    struct monitored_entry *entry, *tmp;
    long rss;

    mutex_lock(&list_lock);

    list_for_each_entry_safe(entry, tmp, &monitored_list, node) {

        rss = get_rss_bytes(entry->pid);

        /* Process no longer exists – clean up stale entry. */
        if (rss < 0) {
            printk(KERN_INFO
                   "[container_monitor] Process exited: container=%s pid=%d (removing)\n",
                   entry->container_id, entry->pid);
            list_del(&entry->node);
            kfree(entry);
            continue;
        }

        /* Hard-limit enforcement. */
        if ((unsigned long)rss > entry->hard_limit) {
            kill_process(entry->container_id, entry->pid,
                         entry->hard_limit, rss);
            list_del(&entry->node);
            kfree(entry);
            continue;
        }

        /* Soft-limit warning (once per entry). */
        if (!entry->soft_warned && (unsigned long)rss > entry->soft_limit) {
            log_soft_limit_event(entry->container_id, entry->pid,
                                 entry->soft_limit, rss);
            entry->soft_warned = 1;
        }
    }

    mutex_unlock(&list_lock);

    /* Re-arm the timer. */
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);
}

/* ------------------------------------------------------------------ */
/* ioctl handler                                                       */
/* ------------------------------------------------------------------ */

static long monitor_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct monitor_request req;

    (void)f;

    if (cmd != MONITOR_REGISTER && cmd != MONITOR_UNREGISTER)
        return -EINVAL;

    if (copy_from_user(&req, (struct monitor_request __user *)arg, sizeof(req)))
        return -EFAULT;

    /* ---------------------------------------------------------------- */
    /* TODO 4: MONITOR_REGISTER – add a new entry                       */
    /* ---------------------------------------------------------------- */
    if (cmd == MONITOR_REGISTER) {
        struct monitored_entry *entry;

        printk(KERN_INFO
               "[container_monitor] Registering container=%s pid=%d soft=%lu hard=%lu\n",
               req.container_id, req.pid,
               req.soft_limit_bytes, req.hard_limit_bytes);

        /* Validate limits */
        if (req.soft_limit_bytes > req.hard_limit_bytes) {
            printk(KERN_WARNING
                   "[container_monitor] Invalid limits for container=%s: soft > hard\n",
                   req.container_id);
            return -EINVAL;
        }

        entry = kmalloc(sizeof(*entry), GFP_KERNEL);
        if (!entry)
            return -ENOMEM;

        entry->pid         = req.pid;
        entry->soft_limit  = req.soft_limit_bytes;
        entry->hard_limit  = req.hard_limit_bytes;
        entry->soft_warned = 0;
        strncpy(entry->container_id, req.container_id,
                CONTAINER_ID_MAX - 1);
        entry->container_id[CONTAINER_ID_MAX - 1] = '\0';

        mutex_lock(&list_lock);
        list_add_tail(&entry->node, &monitored_list);
        mutex_unlock(&list_lock);

        return 0;
    }

    /* ---------------------------------------------------------------- */
    /* TODO 5: MONITOR_UNREGISTER – remove an entry                     */
    /* ---------------------------------------------------------------- */
    printk(KERN_INFO
           "[container_monitor] Unregister request container=%s pid=%d\n",
           req.container_id, req.pid);

    {
        struct monitored_entry *entry, *tmp;
        int found = 0;

        mutex_lock(&list_lock);
        list_for_each_entry_safe(entry, tmp, &monitored_list, node) {
            if (entry->pid == req.pid &&
                strncmp(entry->container_id, req.container_id,
                        CONTAINER_ID_MAX) == 0) {
                list_del(&entry->node);
                kfree(entry);
                found = 1;
                break;
            }
        }
        mutex_unlock(&list_lock);

        if (!found)
            return -ENOENT;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */
static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .unlocked_ioctl = monitor_ioctl,
};

/* ------------------------------------------------------------------ */
/* Module init                                                         */
/* ------------------------------------------------------------------ */
static int __init monitor_init(void)
{
    if (alloc_chrdev_region(&dev_num, 0, 1, DEVICE_NAME) < 0)
        return -1;

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    cl = class_create(DEVICE_NAME);
#else
    cl = class_create(THIS_MODULE, DEVICE_NAME);
#endif
    if (IS_ERR(cl)) {
        unregister_chrdev_region(dev_num, 1);
        return PTR_ERR(cl);
    }

    if (IS_ERR(device_create(cl, NULL, dev_num, NULL, DEVICE_NAME))) {
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    cdev_init(&c_dev, &fops);
    if (cdev_add(&c_dev, dev_num, 1) < 0) {
        device_destroy(cl, dev_num);
        class_destroy(cl);
        unregister_chrdev_region(dev_num, 1);
        return -1;
    }

    timer_setup(&monitor_timer, timer_callback, 0);
    mod_timer(&monitor_timer, jiffies + CHECK_INTERVAL_SEC * HZ);

    printk(KERN_INFO "[container_monitor] Module loaded. Device: /dev/%s\n",
           DEVICE_NAME);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Module exit                                                         */
/* ------------------------------------------------------------------ */
static void __exit monitor_exit(void)
{
    struct monitored_entry *entry, *tmp;

    timer_shutdown_sync(&monitor_timer);

    /* TODO 6: Free all remaining monitored entries. */
    mutex_lock(&list_lock);
    list_for_each_entry_safe(entry, tmp, &monitored_list, node) {
        list_del(&entry->node);
        kfree(entry);
    }
    mutex_unlock(&list_lock);

    cdev_del(&c_dev);
    device_destroy(cl, dev_num);
    class_destroy(cl);
    unregister_chrdev_region(dev_num, 1);

    printk(KERN_INFO "[container_monitor] Module unloaded.\n");
}

module_init(monitor_init);
module_exit(monitor_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Supervised multi-container memory monitor");
