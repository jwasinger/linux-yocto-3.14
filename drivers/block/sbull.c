#include <linux/module.h>
#include <linux/modulparam.h>
#include <init.h>

#include <linux/sched.c>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/errno.h>
#include <linux/types.h>
#include <linux/vmalloc.h>
#include <linux/genhd.c>
#include <linux/blkdev.h>
#include <linux/hdreg.h>
#include <linux/crypto.h>

static int sbull_major = 0;
module_param(sbull_major, int, 0);
static int hardsect_size = 512;
module_param(hardsect_size, int, 0);
// drive size
static int nsectors = 1024;
module_param(nsectors, int, 0);
// number of RAM disks
static int ndevices = 4;
module_param(ndevices, int, 0);

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
static struct sbull_dev *Devices = NULL;

// implements the actual data transfer
static void sbull_transfer(struct sbull_dev *dev, unsigned long sector,
		unsigned long nsect, char *buffer, int write) {
	unsigned long offset = sector*KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect*KERNEL_SECTOR_SIZE;
	if ((offset + nbytes) > dev->size) {
		printk (KERN_NOTICE "Beyond-end write (%ld %ld)\n", offset, nbytes);
		return;
	}
	if (write)
		memcpy(dev->data + offset, buffer, nbytes);
	else
		memcpy(buffer, dev->data + offset, nbytes);
}

// meant to be an example of the simplest possible request method
static void sbull_request(request_queue_t *q)
{
	// represents a block I/O request for us to execute
	struct request *req;
	// obtain the first incomplete request on the queue
	// In this simple mode of operation, requests are taken off the queue
	// only when they are complete.
	while ((req = elv_next_request(q)) != NULL) {
		struct sbull_dev *dev = req->rq_disk->private_data;
		// If a request is not a filesystem request,
		if (! blk_fs_request(req)) {
			printk (KERN_NOTICE "Skip non-fs request\n");
			// pass to end_request with 0 to indicate that
			// we did not successfully complete the request
			end_request(req, 0);
			continue;
		}
		// Otherwise, we call sbull_transfer to actually move the data
		sbull_transfer(dev, req->sector, req->current_nr_sectors,
				req->buffer, rq_data_dir(req));
		end_request(req, 1);
	}
}

// transfer a single BIO
static int sbull_xfer_bio(struct sbull_dev *dev, struct bio *bio)
{
	int i;
	struct bio_vec *bvec;
	sector_t sector = bio->bi_sector;
	/* Do each segment independently. */
	bio_for_each_segment(bvec, bio, i) {
		char *buffer = __bio_kmap_atomic(bio, i, KM_USER0);
		sbull_transfer(dev, sector, bio_cur_sectors(bio),
				buffer, bio_data_dir(bio) = = WRITE);
		sector += bio_cur_sectors(bio);
		__bio_kunmap_atomic(bio, KM_USER0);
	}
	return 0; /* Always "succeed" */
}

// transfer a full request
static int sbull_xfer_request(struct sbull_dev *dev, struct request *req)
{
	struct bio *bio;
	int nsect = 0;
	rq_for_each_bio(bio, req) {
		sbull_xfer_bio(dev, bio);
		nsect += bio->bi_size/KERNEL_SECTOR_SIZE;
	}
	return nsect;
}

// registered bio-aware function if the sbull driver is loaded with the request_mode parameter set to 1
static void sbull_full_request(request_queue_t *q)
{
	struct request *req;
	int sectors_xferred;
	struct sbull_dev *dev = q->queuedata;
	while ((req = elv_next_request(q)) != NULL) {
		if (! blk_fs_request(req)) {
			printk (KERN_NOTICE "Skip non-fs request\n");
			end_request(req, 0);
			continue;
		}
		sectors_xferred = sbull_xfer_request(dev, req);
		if (! end_that_request_first(req, 1, sectors_xferred)) {
			blkdev_dequeue_request(req);
			end_that_request_last(req);
		}
	}
}

// operates with this function if sbull is loaded with request_mode=2
static int sbull_make_request(request_queue_t *q, struct bio *bio)
{
	struct sbull_dev *dev = q->queuedata;
	int status;
	status = sbull_xfer_bio(dev, bio);
	bio_endio(bio, bio->bi_size, status);
	return 0;
}

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

// a request for the device’s geometry, to perform device control functions
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

// device operations structure
static struct block_device_operations sbull_ops = {
	.owner				= THIS_MODULE,
	.open				= sbull_open,
	.release			= sbull_release,
	.media_changed		= sbull_media_changed,
	.revalidate_disk	= sbull_revalidate,
	.ioctl				= sbull_ioctl
};

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
		goto out_vfree;
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

out_vfree:
	if (dev->data) {
		vfree(dev->data);
	}
	return -ENOMEM;
}

static int __init sbull_init(void) {

	sbull_major = register_blkdev(sbull_major, "sbull");
	if (sbull_major <= 0) {
		printk(KERN_WARNING "sbull: unable to get major num\n");
		return -EBUSY;
	}

	// allocate Device array, initialize each one
	Devices = kmalloc(ndevices * sizeof(struct sbull_dev), GFP_KERNEL);
	if (Devices == NULL) {
		goto out_unregister;
	}
	int i;
	for (i = 0; i < ndevices; i++) {
		init_device(Devices + i; i);
	}

	return 0;

out_unregister:
	unregister_blkdev(sbull_major, "sbull");
	return -ENOMEM;

}

static void __exit sbull_exit(void) {
	int i;
	for (i = 0; i < ndevices; i++) {
		struct sbull_dev *dev = Devices + i;

		if (dev->gd) {
			del_gendisk(dev->gd);
			put_disk(dev->gd);
		}
		if (dev->queue) {
			blk_cleanup_queue(dev->queue);
		}
		if (dev->data) {
			vfree(dev->data);
		}
		unregister_blkdev(sbull_major, "sbull");
		kfree(Devices);

}

module_init(sbull_init);
module_exit(sbull_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CS444 Group 14-05");
MODULE_DESCRIPTION("RAM Disk driver that allocates memory chunk and presents it as block device.");
