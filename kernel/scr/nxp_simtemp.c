
// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/workqueue.h>
#include <linux/jiffies.h>
#include <linux/random.h>
#include <linux/slab.h>

#define DRV_NAME "nxp_simtemp"

struct simtemp_dev {
    struct miscdevice miscdev;   // describe el /dev/nxp_simtemp0
    struct device *dev;          // puntero al device asociado
    wait_queue_head_t wq;        // cola de espera para poll/select
    struct delayed_work work;    // tarea periódica
    spinlock_t lock;             // protección de estado

    bool enabled;
    u32  sampling_hz;            // frecuencia de muestreo (1..100 Hz)
    int  temp_mc;                // temperatura en milideg C (p.ej. 40000 = 40.000°C)
    int  threshold_mc;           // umbral m°C (para eventos)
    bool data_ready;             // hay muestra nueva para poll()
    u64  sample_count;           // conteo de muestras
};

static struct simtemp_dev *gdev;  ///Instancia Global

static void simtemp_update_fn(struct work_struct *ws)
{
	struct simtemp_dev *sd = container_of(to_delayed_work(ws), struct simtemp_dev, work);
	unsigned long delay;
	unsigned long flags;

	/* simple pseudo-random walk around 40C */
	u32 r;
	get_random_bytes(&r, sizeof(r));
	/* Small jitter +-500 mC */
	int delta = ((int)(r % 1001)) - 500;

	spin_lock_irqsave(&sd->lock, flags);
	if (!sd->enabled) {
		spin_unlock_irqrestore(&sd->lock, flags);
		return;
	}

	sd->temp_mc = 40000 + delta;
	sd->sample_count++;
	sd->data_ready = true;

	/* Wake up pollers on new sample or threshold crossing */
	if (sd->temp_mc >= sd->threshold_mc)
		wake_up_interruptible(&sd->wq);
	else
		wake_up_interruptible(&sd->wq);

	delay = msecs_to_jiffies(1000 / sd->sampling_hz);
	spin_unlock_irqrestore(&sd->lock, flags);

	schedule_delayed_work(&sd->work, delay);
}

static ssize_t st_read(struct file *f, char __user *buf, size_t len, loff_t *ppos)
{
	char kbuf[64];
	size_t n;
	int ret;
	struct simtemp_dev *sd = f->private_data;

	n = scnprintf(kbuf, sizeof(kbuf), "temp_mc=%d\n", sd->temp_mc);
	if (*ppos != 0)
		return 0;
	if (len < n)
		return -EINVAL;
	ret = copy_to_user(buf, kbuf, n);
	if (ret)
		return -EFAULT;
	*ppos = n;
	return n;
}

static __poll_t st_poll(struct file *f, poll_table *wait)
{
	__poll_t mask = 0;
	unsigned long flags;
	struct simtemp_dev *sd = f->private_data;

	poll_wait(f, &sd->wq, wait);
	spin_lock_irqsave(&sd->lock, flags);
	if (sd->data_ready) {
		mask |= EPOLLIN | EPOLLRDNORM;
		sd->data_ready = false;
	}
	spin_unlock_irqrestore(&sd->lock, flags);
	return mask;
}

static int st_open(struct inode *ino, struct file *f)
{
	f->private_data = gdev;
	return 0;
}

static const struct file_operations st_fops = {
	.owner   = THIS_MODULE,
	.read    = st_read,
	.poll    = st_poll,
	.open    = st_open,
	.llseek  = no_llseek,
};

/* Sysfs helpers */
static ssize_t enable_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct simtemp_dev *sd = gdev;
	return sysfs_emit(buf, "%d\n", sd->enabled ? 1 : 0);
}
static ssize_t enable_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct simtemp_dev *sd = gdev;
	unsigned long flags;
	unsigned int v;
	if (kstrtouint(buf, 0, &v))
		return -EINVAL;
	spin_lock_irqsave(&sd->lock, flags);
	sd->enabled = !!v;
	spin_unlock_irqrestore(&sd->lock, flags);
	if (sd->enabled)
		mod_delayed_work(system_wq, &sd->work, 0);
	else
		cancel_delayed_work_sync(&sd->work);
	return count;
}
static DEVICE_ATTR_RW(enable);

static ssize_t sampling_hz_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct simtemp_dev *sd = gdev;
	return sysfs_emit(buf, "%u\n", sd->sampling_hz);
}
static ssize_t sampling_hz_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct simtemp_dev *sd = gdev;
	unsigned int v;
	if (kstrtouint(buf, 0, &v))
		return -EINVAL;
	if (v < 1 || v > 100)
		return -EINVAL;
	sd->sampling_hz = v;
	return count;
}
static DEVICE_ATTR_RW(sampling_hz);

static ssize_t threshold_mc_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct simtemp_dev *sd = gdev;
	return sysfs_emit(buf, "%d\n", sd->threshold_mc);
}
static ssize_t threshold_mc_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct simtemp_dev *sd = gdev;
	int v;
	if (kstrtoint(buf, 0, &v))
		return -EINVAL;
	sd->threshold_mc = v;
	return count;
}
static DEVICE_ATTR_RW(threshold_mc);

static ssize_t temp_mc_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct simtemp_dev *sd = gdev;
	return sysfs_emit(buf, "%d\n", sd->temp_mc);
}
static DEVICE_ATTR_RO(temp_mc);

static struct attribute *st_attrs[] = {
	&dev_attr_enable.attr,
	&dev_attr_sampling_hz.attr,
	&dev_attr_threshold_mc.attr,
	&dev_attr_temp_mc.attr,
	NULL,
};
ATTRIBUTE_GROUPS(st);

static const struct of_device_id st_of_match[] = {
	{ .compatible = "nxp,simtemp" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, st_of_match);

static int __init st_init(void)
{
	int ret;
	struct simtemp_dev *sd;

	sd = kzalloc(sizeof(*sd), GFP_KERNEL);
	if (!sd)
		return -ENOMEM;

	gdev = sd;
	spin_lock_init(&sd->lock);
	init_waitqueue_head(&sd->wq);
	INIT_DELAYED_WORK(&sd->work, simtemp_update_fn);

	/* defaults */
	sd->enabled = false;
	sd->sampling_hz = 2;
	sd->temp_mc = 40000;
	sd->threshold_mc = 45000;
	sd->rng_seed = 1;

	/* Device Tree (best-effort parse on parent device if present) */
	if (of_have_populated_dt()) {
		struct device_node *np;
		for_each_compatible_node(np, NULL, "nxp,simtemp") {
			u32 val;
			if (!of_property_read_u32(np, "sampling-hz", &val))
				sd->sampling_hz = val;
			if (!of_property_read_u32(np, "threshold-mc", &val))
				sd->threshold_mc = (int)val;
			if (!of_property_read_u32(np, "rng-seed", &val))
				sd->rng_seed = val;
			break; /* first match */
		}
	}

	sd->miscdev.minor = MISC_DYNAMIC_MINOR;
	sd->miscdev.name  = "nxp_simtemp0";
	sd->miscdev.fops  = &st_fops;
	sd->miscdev.groups = st_groups;

	ret = misc_register(&sd->miscdev);
	if (ret) {
		kfree(sd);
		return ret;
	}

	sd->dev = sd->miscdev.this_device;

	dev_info(sd->dev, DRV_NAME ": loaded. sampling_hz=%u threshold_mc=%d\n",
	         sd->sampling_hz, sd->threshold_mc);
	return 0;
}

static void __exit st_exit(void)
{
	struct simtemp_dev *sd = gdev;
	cancel_delayed_work_sync(&sd->work);
	misc_deregister(&sd->miscdev);
	kfree(sd);
	pr_info(DRV_NAME ": unloaded\n");
}

module_init(st_init);
module_exit(st_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("NXP simulated temperature sensor");
MODULE_VERSION("0.1");
