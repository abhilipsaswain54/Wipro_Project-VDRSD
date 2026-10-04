/*
 * vdrsd.c - Linux Character Device Driver for VDRSD
 * 
 * Provides /dev/vdrsd device interface for user application writes,
 * buffers data in kernel ring buffer, and signals user-space daemon.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include <linux/ioctl.h>

#define VDRSD_DEVICE_NAME "vdrsd"
#define VDRSD_BUF_SIZE    (4096 * 64) // 256 KB Kernel Ring Buffer

struct vdrsd_stats {
    u64 total_writes;
    u64 total_reads;
    u64 bytes_written;
    u64 bytes_read;
    u32 buffer_used;
    u32 buffer_capacity;
};

#define VDRSD_IOCTL_MAGIC 'v'
#define VDRSD_IOCTL_GET_STATS   _IOR(VDRSD_IOCTL_MAGIC, 1, struct vdrsd_stats)
#define VDRSD_IOCTL_CLEAR_BUFFER _IO(VDRSD_IOCTL_MAGIC, 2)

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Wipro Embedded Track");
MODULE_DESCRIPTION("Virtual Distributed Replicated Storage Device Driver");
MODULE_VERSION("1.0");

static int major_number;
static struct class *vdrsd_class = NULL;
static struct device *vdrsd_device = NULL;
static struct cdev vdrsd_cdev;

// Kernel Ring Buffer
static char *kernel_buffer;
static size_t head = 0;
static size_t tail = 0;
static size_t count = 0;

#include <linux/version.h>

static spinlock_t buffer_lock;
static DECLARE_WAIT_QUEUE_HEAD(read_wait_queue);

static struct vdrsd_stats driver_stats = {0};

static int vdrsd_open(struct inode *inodep, struct file *filep) {
    pr_info("VDRSD: Device opened by PID %d\n", current->pid);
    return 0;
}

static int vdrsd_release(struct inode *inodep, struct file *filep) {
    pr_info("VDRSD: Device closed by PID %d\n", current->pid);
    return 0;
}

static ssize_t vdrsd_read(struct file *filep, char __user *buffer, size_t len, loff_t *offset) {
    size_t bytes_to_read;
    size_t i;
    int ret;
    char *temp_buf;

    if (wait_event_interruptible(read_wait_queue, count > 0)) {
        return -ERESTARTSYS;
    }

    spin_lock(&buffer_lock);

    bytes_to_read = min(len, count);
    if (bytes_to_read == 0) {
        spin_unlock(&buffer_lock);
        return 0;
    }

    temp_buf = kmalloc(bytes_to_read, GFP_KERNEL);
    if (!temp_buf) {
        spin_unlock(&buffer_lock);
        return -ENOMEM;
    }

    for (i = 0; i < bytes_to_read; i++) {
        temp_buf[i] = kernel_buffer[head];
        head = (head + 1) % VDRSD_BUF_SIZE;
    }
    count -= bytes_to_read;

    driver_stats.total_reads++;
    driver_stats.bytes_read += bytes_to_read;
    driver_stats.buffer_used = count;

    spin_unlock(&buffer_lock);

    ret = copy_to_user(buffer, temp_buf, bytes_to_read);
    kfree(temp_buf);

    if (ret != 0) {
        return -EFAULT;
    }

    return bytes_to_read;
}

static ssize_t vdrsd_write(struct file *filep, const char __user *buffer, size_t len, loff_t *offset) {
    size_t bytes_to_write;
    size_t available_space;
    size_t i;
    char *temp_buf;
    int ret;

    temp_buf = kmalloc(len, GFP_KERNEL);
    if (!temp_buf) {
        return -ENOMEM;
    }

    ret = copy_from_user(temp_buf, buffer, len);
    if (ret != 0) {
        kfree(temp_buf);
        return -EFAULT;
    }

    spin_lock(&buffer_lock);

    available_space = VDRSD_BUF_SIZE - count;
    bytes_to_write = min(len, available_space);

    for (i = 0; i < bytes_to_write; i++) {
        kernel_buffer[tail] = temp_buf[i];
        tail = (tail + 1) % VDRSD_BUF_SIZE;
    }
    count += bytes_to_write;

    driver_stats.total_writes++;
    driver_stats.bytes_written += bytes_to_write;
    driver_stats.buffer_used = count;

    spin_unlock(&buffer_lock);

    kfree(temp_buf);

    wake_up_interruptible(&read_wait_queue);

    return bytes_to_write;
}

static long vdrsd_ioctl(struct file *filep, unsigned int cmd, unsigned long arg) {
    switch (cmd) {
        case VDRSD_IOCTL_GET_STATS:
            spin_lock(&buffer_lock);
            driver_stats.buffer_used = count;
            driver_stats.buffer_capacity = VDRSD_BUF_SIZE;
            spin_unlock(&buffer_lock);

            if (copy_to_user((struct vdrsd_stats __user *)arg, &driver_stats, sizeof(driver_stats))) {
                return -EFAULT;
            }
            break;

        case VDRSD_IOCTL_CLEAR_BUFFER:
            spin_lock(&buffer_lock);
            head = 0;
            tail = 0;
            count = 0;
            driver_stats.buffer_used = 0;
            spin_unlock(&buffer_lock);
            pr_info("VDRSD: Buffer cleared via IOCTL\n");
            break;

        default:
            return -ENOTTY;
    }
    return 0;
}

static struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = vdrsd_open,
    .release = vdrsd_release,
    .read = vdrsd_read,
    .write = vdrsd_write,
    .unlocked_ioctl = vdrsd_ioctl,
};

static int __init vdrsd_init(void) {
    dev_t dev_num;
    int ret;

    pr_info("VDRSD: Initializing Kernel Module...\n");

    kernel_buffer = kmalloc(VDRSD_BUF_SIZE, GFP_KERNEL);
    if (!kernel_buffer) {
        pr_err("VDRSD: Failed to allocate kernel ring buffer\n");
        return -ENOMEM;
    }

    spin_lock_init(&buffer_lock);

    ret = alloc_chrdev_region(&dev_num, 0, 1, VDRSD_DEVICE_NAME);
    if (ret < 0) {
        pr_err("VDRSD: Failed to allocate major number\n");
        kfree(kernel_buffer);
        return ret;
    }
    major_number = MAJOR(dev_num);

    cdev_init(&vdrsd_cdev, &fops);
    vdrsd_cdev.owner = THIS_MODULE;

    ret = cdev_add(&vdrsd_cdev, dev_num, 1);
    if (ret < 0) {
        unregister_chrdev_region(dev_num, 1);
        kfree(kernel_buffer);
        return ret;
    }

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    vdrsd_class = class_create("vdrsd_class");
#else
    vdrsd_class = class_create(THIS_MODULE, "vdrsd_class");
#endif
    if (IS_ERR(vdrsd_class)) {
        cdev_del(&vdrsd_cdev);
        unregister_chrdev_region(dev_num, 1);
        kfree(kernel_buffer);
        return PTR_ERR(vdrsd_class);
    }

    vdrsd_device = device_create(vdrsd_class, NULL, dev_num, NULL, VDRSD_DEVICE_NAME);
    if (IS_ERR(vdrsd_device)) {
        class_destroy(vdrsd_class);
        cdev_del(&vdrsd_cdev);
        unregister_chrdev_region(dev_num, 1);
        kfree(kernel_buffer);
        return PTR_ERR(vdrsd_device);
    }

    pr_info("VDRSD: Device registered successfully with Major %d. Created /dev/%s\n", major_number, VDRSD_DEVICE_NAME);
    return 0;
}

static void __exit vdrsd_exit(void) {
    dev_t dev_num = MKDEV(major_number, 0);

    device_destroy(vdrsd_class, dev_num);
    class_destroy(vdrsd_class);
    cdev_del(&vdrsd_cdev);
    unregister_chrdev_region(dev_num, 1);
    kfree(kernel_buffer);

    pr_info("VDRSD: Kernel Module unloaded successfully.\n");
}

module_init(vdrsd_init);
module_exit(vdrsd_exit);
