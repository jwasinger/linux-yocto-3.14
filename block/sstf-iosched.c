/*
 * elevator sstf
 */
#include <linux/blkdev.h>
#include <linux/elevator.h>
#include <linux/bio.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>

// need the last spindle location and moving direction to know how to choose
// the request(aka, moving spindles in which direction and locating the
// spindles). So this struct is like a state for the spindle.
struct sstf_data {
	struct list_head queue;
};

static void sstf_merged_requests(struct request_queue *q, struct request *rq,
				 struct request *next)
{
	list_del_init(&next->queuelist);
}

// The spindle’s moving direction and last location will tell us how to move
// the spindle. Noop moves the spindle in the request queue’s order since it's
// FCFS.
//		If we are moving towards higher sectors, in the request list find the
//		first request that has the greater sector number than the last spindle
//		location. If there is none, change spindle direction and move the
//		cursor to the PREVIOUS struct of where the cursor pointed in the
//		request list. If there is one, move the spindle location by change the
//		corresponding value in sstf_data structs.
//		If moving towards lower sectors, vice versa.
//	
//	two kernel calls to understand so that you can know how to change them:
//		list_del_init
//		elv_dispatch_*
static int sstf_dispatch(struct request_queue *q, int force)
{
	struct sstf_data *nd = q->elevator->elevator_data;

	if (!list_empty(&nd->queue)) {
		struct request *rq;
		rq = list_entry(nd->queue.next, struct request, queuelist);
		list_del_init(&rq->queuelist);
		elv_dispatch_sort(q, rq);

		// debug prints to show that scheduler dispatches as intended
		if (rq_data_dir(rq) == READ) {
			printk("KERN_INFO [CLOOK] dsp R %llu\n", blk_rq_pos(rq));
		} else {
			printk("KERN_INFO [CLOOK] dsp W %llu\n", blk_rq_pos(rq));
		}

		return 1;
	}
	return 0;
}

static void sstf_add_request(struct request_queue *q, struct request *rq)
{
	struct sstf_data *nd = q->elevator->elevator_data;
	// We need to maintain the request list as sorted,
    // so sstf_add_request adds the new request to the
    // correct location in the list.
	struct list_head *cur = NULL;

	//if list is empty, add request
	if (list_empty(&nd->queue)) {
		list_add(&rq->queuelist, &nd->queue);
	}
	
	// sorts by checking physical address
	// source:
	// rq_end_sector(rq) defined in linux/elevator.h as a macro
	// - (blk_rq_pos(rq) + blk_rq_sectors(rq))
	// http://funwithgraphics.blogspot.com/2012/10/implementing-clook-algorithm-part-1.html
	list_for_each(cur, &nd->queue) {
		struct request *c_req = list_entry(cur, struct request, queuelist);

		if(rq_end_sector(c_req) > rq_end_sector(rq)) {
			break;
		}
	}
	list_add_tail(&rq->queuelist, cur);

	// debug prints to show that scheduler adds as intended
	if (rq_data_dir(rq) == READ) {
		printk("KERN_INFO [CLOOK] add R %llu\n", blk_rq_pos(rq));
	} else {
		printk("KERN_INFO [CLOOK] add W %llu\n", blk_rq_pos(rq));
	}
}

static struct request *
sstf_former_request(struct request_queue *q, struct request *rq)
{
	struct sstf_data *nd = q->elevator->elevator_data;

	if (rq->queuelist.prev == &nd->queue)
		return NULL;
	return list_entry(rq->queuelist.prev, struct request, queuelist);
}

static struct request *
sstf_latter_request(struct request_queue *q, struct request *rq)
{
	struct sstf_data *nd = q->elevator->elevator_data;

	if (rq->queuelist.next == &nd->queue)
		return NULL;
	return list_entry(rq->queuelist.next, struct request, queuelist);
}

// needs minor changes that corresponds to the changes in sstf_data.
static int sstf_init_queue(struct request_queue *q, struct elevator_type *e)
{
	struct sstf_data *nd;
	struct elevator_queue *eq;

	eq = elevator_alloc(q, e);
	if (!eq)
		return -ENOMEM;

	nd = kmalloc_node(sizeof(*nd), GFP_KERNEL, q->node);
	if (!nd) {
		kobject_put(&eq->kobj);
		return -ENOMEM;
	}
	eq->elevator_data = nd;

	INIT_LIST_HEAD(&nd->queue);

	spin_lock_irq(q->queue_lock);
	q->elevator = eq;
	spin_unlock_irq(q->queue_lock);
	return 0;
}

static void sstf_exit_queue(struct elevator_queue *e)
{
	struct sstf_data *nd = e->elevator_data;

	BUG_ON(!list_empty(&nd->queue));
	kfree(nd);
}

static struct elevator_type elevator_sstf = {
	.ops = {
		.elevator_merge_req_fn		= sstf_merged_requests,
		.elevator_dispatch_fn		= sstf_dispatch,
		.elevator_add_req_fn		= sstf_add_request,
		.elevator_former_req_fn		= sstf_former_request,
		.elevator_latter_req_fn		= sstf_latter_request,
		.elevator_init_fn			= sstf_init_queue,
		.elevator_exit_fn			= sstf_exit_queue,
	},
	.elevator_name = "sstf",
	.elevator_owner = THIS_MODULE,
};

static int __init sstf_init(void)
{
	return elv_register(&elevator_sstf);
}

static void __exit sstf_exit(void)
{
	elv_unregister(&elevator_sstf);
}

module_init(sstf_init);
module_exit(sstf_exit);


MODULE_AUTHOR("CS444 - Group 14-05");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SSTF IO scheduler");
