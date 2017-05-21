#include <linux/module.h>
#include <linux/modulparam.h>
#include <init.h>

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/genhd.c>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/crypto.h>


#define KERNEL_SECTOR_SIZE 512

// internal structure of device
struct sbull_dev {
	int size; /* Device size in sectors */
	u8 *data; /* The data array */
	short users; /* How many users */
	short media_change; /* Flag a media change? */
	spinlock_t lock; /* For mutual exclusion */
	struct request_queue *queue; /* The device request queue */
	struct gendisk *gd; /* The gendisk structure */
	struct timer_list timer; /* For simulated media changes */
};

static int sbull_open(struct inode *inode, struct file *filp) {
	// the field i_bdev->bd_disk contains a pointer to the associated gendisk
	// structure; this pointer can be used to get to a driver’s internal 
	// data structures for the device
	struct sbull_dev *dev = inode->i_bdev->bd_disk->private_data;
	
	// remove the “media removal” timer, if any is active
	del_timer_sync(&dev->timer);
	filp->private_data = dev;
	// do not lock the device spinlock until after the timer has been deleted;
	// doing otherwise invites deadlock if the timer function runs 
	// before we can delete it
	spin_lock(&dev->lock);
	if (! dev->users)
		// check whether a media change has happened
		check_disk_change(inode->i_bdev);
	// increment user count
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}

// decrement the user count and, if indicated, start the media removal timer
static int sbull_release(struct inode *inode, struct file *filp) {
	struct sbull_dev *dev = inode->i_bdev->bd_disk->private_data;
	spin_lock(&dev->lock);
	dev->users--;
	if (!dev->users) {
		dev->timer.expires = jiffies + INVALIDATE_DELAY;
		add_timer(&dev->timer);
	}
	spin_unlock(&dev->lock);
	return 0;
}

// see whether the media has been changed;
// it should return a nonzero value if this has happened
int sbull_media_changed(struct gendisk *gd) {
	struct sbull_dev *dev = gd->private_data;
	return dev->media_change;
}

// after a media change, do whatever is required to prepare the driver for
// operations on the new media, if any
// the kernel then attempts to reread the partition table and start over 
// with the device
int sbull_revalidate(struct gendisk *gd) {
	struct sbull_dev *dev = gd->private_data;
	if (dev->media_change) {
		dev->media_change = 0;
		memset (dev->data, 0, dev->size);
	}
	return 0;
}

int sbull_ioctl (struct inode *inode, struct file *filp,
		unsigned int cmd, unsigned long arg)
{
	long size;
	struct hd_geometry geo;
	struct sbull_dev *dev = filp->private_data;
	switch(cmd) {
		case HDIO_GETGEO:
		/*
		* Get geometry: since we are a virtual device, we have to make
		* up something plausible. So we claim 16 sectors, four heads,
		* and calculate the corresponding number of cylinders. We set the
		* start of data at sector four.
		*/
		size = dev->size*(hardsect_size/KERNEL_SECTOR_SIZE);
		geo.cylinders = (size & ~0x3f) >> 6;
		geo.heads = 4;
		geo.sectors = 16;
		geo.start = 4;
		if (copy_to_user((void __user *) arg, &geo, sizeof(geo)))
			return -EFAULT;
			return 0;
	}
	return -ENOTTY; /* unknown command */
}

static void init_device(struct sbull_dev *dev) {
	// basic initialization and allocation of the underlying memory
	memset (dev, 0, sizeof (struct sbull_dev));
	dev->size = nsectors*hardsect_size;
	dev->data = vmalloc(dev->size);
	if (dev->data == NULL) {
		printk (KERN_NOTICE "vmalloc failure.\n");
		return;
	}
	spin_lock_init(&dev->lock);

	// allocation of the request queue
	dev->queue = blk_init_queue(sbull_request, &dev->lock);
	if (dev->queue == NULL) {
		goto free_data;
	}

	// inform the kernel of the sector size your device supports
	blk_queue_hardsect_size(dev->queue, hardsect_size);

	// once we have our device memory and request queue in place,
	// allocate, initialize, and install the corresponding gendisk structure
	dev->gd = alloc_disk(SBULL_MINORS);
	if (! dev->gd) {
		printk (KERN_NOTICE "alloc_disk failure\n");
		goto out_vfree;
	}
	dev->gd->major = sbull_major;
	dev->gd->first_minor = which*SBULL_MINORS;
	dev->gd->fops = &sbull_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	snprintf (dev->gd->disk_name, 32, "sbull%c", which + 'a');
	set_capacity(dev->gd, nsectors*(hardsect_size/KERNEL_SECTOR_SIZE));
	add_disk(dev->gd);
	return;

	free_data:
		if (dev->data) {
			vfree(dev->data);
		}
}

