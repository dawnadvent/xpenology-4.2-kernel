/*
 *  linux/drivers/block/loop.c
 *
 *  Written by Theodore Ts'o, 3/29/93
 *
 * Copyright 1993 by Theodore Ts'o.  Redistribution of this file is
 * permitted under the GNU General Public License.
 *
 * DES encryption plus some minor changes by Werner Almesberger, 30-MAY-1993
 * more DES encryption plus IDEA encryption by Nicholas J. Leon, June 20, 1996
 *
 * Modularized and updated for 1.1.16 kernel - Mitch Dsouza 28th May 1994
 * Adapted for 1.3.59 kernel - Andries Brouwer, 1 Feb 1996
 *
 * Fixed do_loop_request() re-entrancy - Vincent.Renardias@waw.com Mar 20, 1997
 *
 * Added devfs support - Richard Gooch <rgooch@atnf.csiro.au> 16-Jan-1998
 *
 * Handle sparse backing files correctly - Kenn Humborg, Jun 28, 1998
 *
 * Loadable modules and other fixes by AK, 1998
 *
 * Make real block number available to downstream transfer functions, enables
 * CBC (and relatives) mode encryption requiring unique IVs per data block.
 * Reed H. Petty, rhp@draper.net
 *
 * Maximum number of loop devices now dynamic via max_loop module parameter.
 * Russell Kroll <rkroll@exploits.org> 19990701
 *
 * Maximum number of loop devices when compiled-in now selectable by passing
 * max_loop=<1-255> to the kernel on boot.
 * Erik I. Bolsø, <eriki@himolde.no>, Oct 31, 1999
 *
 * Completely rewrite request handling to be make_request_fn style and
 * non blocking, pushing work to a helper thread. Lots of fixes from
 * Al Viro too.
 * Jens Axboe <axboe@suse.de>, Nov 2000
 *
 * Support up to 256 loop devices
 * Heinz Mauelshagen <mge@sistina.com>, Feb 2002
 *
 * Support for falling back on the write file operation when the address space
 * operations write_begin is not available on the backing filesystem.
 * Anton Altaparmakov, 16 Feb 2005
 *
 * Still To Fix:
 * - Advisory locking is ignored here.
 * - Should use an own CAP_* category instead of CAP_SYS_ADMIN
 *
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/major.h>
#include <linux/wait.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/init.h>
#include <linux/swap.h>
#include <linux/slab.h>
#include <linux/loop.h>
#include <linux/compat.h>
#include <linux/suspend.h>
#include <linux/freezer.h>
#include <linux/mutex.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>		/* for invalidate_bdev() */
#include <linux/completion.h>
#include <linux/highmem.h>
#include <linux/kthread.h>
#include <linux/splice.h>
#include <linux/sysfs.h>
#include <linux/miscdevice.h>
#include <linux/falloc.h>

#include <asm/uaccess.h>

static DEFINE_IDR(loop_index_idr);
static DEFINE_MUTEX(loop_index_mutex);

static int max_part;
static int part_shift;

/*
 * Transfer functions
 */
static int transfer_none(struct loop_device *lo, int cmd,
			 struct page *raw_page, unsigned raw_off,
			 struct page *loop_page, unsigned loop_off,
			 int size, sector_t real_block)
{
	char *raw_buf = kmap_atomic(raw_page, KM_USER0) + raw_off;
	char *loop_buf = kmap_atomic(loop_page, KM_USER1) + loop_off;

	if (cmd == READ)
		memcpy(loop_buf, raw_buf, size);
	else
		memcpy(raw_buf, loop_buf, size);

	kunmap_atomic(loop_buf, KM_USER1);
	kunmap_atomic(raw_buf, KM_USER0);
	cond_resched();
	return 0;
}

static int transfer_xor(struct loop_device *lo, int cmd,
			struct page *raw_page, unsigned raw_off,
			struct page *loop_page, unsigned loop_off,
			int size, sector_t real_block)
{
	char *raw_buf = kmap_atomic(raw_page, KM_USER0) + raw_off;
	char *loop_buf = kmap_atomic(loop_page, KM_USER1) + loop_off;
	char *in, *out, *key;
	int i, keysize;

	if (cmd == READ) {
		in = raw_buf;
		out = loop_buf;
	} else {
		in = loop_buf;
		out = raw_buf;
	}

	key = lo->lo_encrypt_key;
	keysize = lo->lo_encrypt_key_size;
	for (i = 0; i < size; i++)
		*out++ = *in++ ^ key[(i & 511) % keysize];

	kunmap_atomic(loop_buf, KM_USER1);
	kunmap_atomic(raw_buf, KM_USER0);
	cond_resched();
	return 0;
}

static int xor_init(struct loop_device *lo, const struct loop_info64 *info)
{
	if (unlikely(info->lo_encrypt_key_size <= 0))
		return -EINVAL;
	return 0;
}

static struct loop_func_table none_funcs = {
	.number = LO_CRYPT_NONE,
	.transfer = transfer_none,
}; 	

static struct loop_func_table xor_funcs = {
	.number = LO_CRYPT_XOR,
	.transfer = transfer_xor,
	.init = xor_init
}; 	

/* xfer_funcs[0] is special - its release function is never called */
static struct loop_func_table *xfer_funcs[MAX_LO_CRYPT] = {
	&none_funcs,
	&xor_funcs
};

static loff_t get_size(loff_t offset, loff_t sizelimit, struct file *file)
{
	loff_t size, loopsize;

	/* Compute loopsize in bytes */
	size = i_size_read(file->f_mapping->host);
	loopsize = size - offset;
	/* offset is beyond i_size, wierd but possible */
	if (loopsize < 0)
		return 0;

	if (sizelimit > 0 && sizelimit < loopsize)
		loopsize = sizelimit;
	/*
	 * Unfortunately, if we want to do I/O on the device,
	 * the number of 512-byte sectors has to fit into a sector_t.
	 */
	return loopsize >> 9;
}

static loff_t get_loop_size(struct loop_device *lo, struct file *file)
{
	return get_size(lo->lo_offset, lo->lo_sizelimit, file);
}

static int
figure_loop_size(struct loop_device *lo, loff_t offset, loff_t sizelimit)
{
	loff_t size = get_size(offset, sizelimit, lo->lo_backing_file);
	sector_t x = (sector_t)size;

	if (unlikely((loff_t)x != size))
		return -EFBIG;
	if (lo->lo_offset != offset)
		lo->lo_offset = offset;
	if (lo->lo_sizelimit != sizelimit)
		lo->lo_sizelimit = sizelimit;
	set_capacity(lo->lo_disk, x);
	return 0;
}

static inline int
lo_do_transfer(struct loop_device *lo, int cmd,
	       struct page *rpage, unsigned roffs,
	       struct page *lpage, unsigned loffs,
	       int size, sector_t rblock)
{
	if (unlikely(!lo->transfer))
		return 0;

	return lo->transfer(lo, cmd, rpage, roffs, lpage, loffs, size, rblock);
}

/**
 * __do_lo_send_write - helper for writing data to a loop device
 *
 * This helper just factors out common code between do_lo_send_direct_write()
 * and do_lo_send_write().
 */
static int __do_lo_send_write(struct file *file,
		u8 *buf, const int len, loff_t pos)
{
	ssize_t bw;
	mm_segment_t old_fs = get_fs();

	set_fs(get_ds());
	bw = file->f_op->write(file, buf, len, &pos);
	set_fs(old_fs);
	if (likely(bw == len))
		return 0;
	printk(KERN_ERR "loop: Write error at byte offset %llu, length %i.\n",
			(unsigned long long)pos, len);
	if (bw >= 0)
		bw = -EIO;
	return bw;
}

/**
 * do_lo_send_direct_write - helper for writing data to a loop device
 *
 * This is the fast, non-transforming version that does not need double
 * buffering.
 */
static int do_lo_send_direct_write(struct loop_device *lo,
		struct bio_vec *bvec, loff_t pos, struct page *page)
{
	ssize_t bw = __do_lo_send_write(lo->lo_backing_file,
			kmap(bvec->bv_page) + bvec->bv_offset,
			bvec->bv_len, pos);
	kunmap(bvec->bv_page);
	cond_resched();
	return bw;
}

/**
 * do_lo_send_write - helper for writing data to a loop device
 *
 * This is the slow, transforming version that needs to double buffer the
 * data as it cannot do the transformations in place without having direct
 * access to the destination pages of the backing file.
 */
static int do_lo_send_write(struct loop_device *lo, struct bio_vec *bvec,
		loff_t pos, struct page *page)
{
	int ret = lo_do_transfer(lo, WRITE, page, 0, bvec->bv_page,
			bvec->bv_offset, bvec->bv_len, pos >> 9);
	if (likely(!ret))
		return __do_lo_send_write(lo->lo_backing_file,
				page_address(page), bvec->bv_len,
				pos);
	printk(KERN_ERR "loop: Transfer error at byte offset %llu, "
			"length %i.\n", (unsigned long long)pos, bvec->bv_len);
	if (ret > 0)
		ret = -EIO;
	return ret;
}

static int lo_send(struct loop_device *lo, struct bio *bio, loff_t pos)
{
	int (*do_lo_send)(struct loop_device *, struct bio_vec *, loff_t,
			struct page *page);
	struct bio_vec *bvec;
	struct page *page = NULL;
	int i, ret = 0;

	if (lo->transfer != transfer_none) {
		page = alloc_page(GFP_NOIO | __GFP_HIGHMEM);
		if (unlikely(!page))
			goto fail;
		kmap(page);
		do_lo_send = do_lo_send_write;
	} else {
		do_lo_send = do_lo_send_direct_write;
	}

	bio_for_each_segment(bvec, bio, i) {
		ret = do_lo_send(lo, bvec, pos, page);
		if (ret < 0)
			break;
		pos += bvec->bv_len;
	}
	if (page) {
		kunmap(page);
		__free_page(page);
	}
out:
	return ret;
fail:
	printk(KERN_ERR "loop: Failed to allocate temporary page for write.\n");
	ret = -ENOMEM;
	goto out;
}

struct lo_read_data {
	struct loop_device *lo;
	struct page *page;
	unsigned offset;
	int bsize;
};

static int
lo_splice_actor(struct pipe_inode_info *pipe, struct pipe_buffer *buf,
		struct splice_desc *sd)
{
	struct lo_read_data *p = sd->u.data;
	struct loop_device *lo = p->lo;
	struct page *page = buf->page;
	sector_t IV;
	int size;

	IV = ((sector_t) page->index << (PAGE_CACHE_SHIFT - 9)) +
							(buf->offset >> 9);
	size = sd->len;
	if (size > p->bsize)
		size = p->bsize;

	if (lo_do_transfer(lo, READ, page, buf->offset, p->page, p->offset, size, IV)) {
		printk(KERN_ERR "loop: transfer error block %ld\n",
		       page->index);
		size = -EINVAL;
	}

	flush_dcache_page(p->page);

	if (size > 0)
		p->offset += size;

	return size;
}

static int
lo_direct_splice_actor(struct pipe_inode_info *pipe, struct splice_desc *sd)
{
	return __splice_from_pipe(pipe, sd, lo_splice_actor);
}

static int
do_lo_receive(struct loop_device *lo,
	      struct bio_vec *bvec, int bsize, loff_t pos)
{
	struct lo_read_data cookie;
	struct splice_desc sd;
	struct file *file;
	long retval;

	cookie.lo = lo;
	cookie.page = bvec->bv_page;
	cookie.offset = bvec->bv_offset;
	cookie.bsize = bsize;

	sd.len = 0;
	sd.total_len = bvec->bv_len;
	sd.flags = 0;
	sd.pos = pos;
	sd.u.data = &cookie;

	file = lo->lo_backing_file;
	retval = splice_direct_to_actor(file, &sd, lo_direct_splice_actor);

	if (retval < 0)
		return retval;
	if (retval != bvec->bv_len)
		return -EIO;
	return 0;
}

static int
lo_receive(struct loop_device *lo, struct bio *bio, int bsize, loff_t pos)
{
	struct bio_vec *bvec;
	int i, ret = 0;

	bio_for_each_segment(bvec, bio, i) {
		ret = do_lo_receive(lo, bvec, bsize, pos);
		if (ret < 0)
			break;
		pos += bvec->bv_len;
	}
	return ret;
}

static int do_bio_filebacked(struct loop_device *lo, struct bio *bio)
{
	loff_t pos;
	int ret;

	pos = ((loff_t) bio->bi_sector << 9) + lo->lo_offset;

	if (bio_rw(bio) == WRITE) {
		struct file *file = lo->lo_backing_file;

		if (bio->bi_rw & REQ_FLUSH) {
			ret = vfs_fsync(file, 0);
			if (unlikely(ret && ret != -EINVAL)) {
				ret = -EIO;
				goto out;
			}
		}

		/*
		 * We use punch hole to reclaim the free space used by the
		 * image a.k.a. discard. However we do not support discard if
		 * encryption is enabled, because it may give an attacker
		 * useful information.
		 */
		if (bio->bi_rw & REQ_DISCARD) {
			struct file *file = lo->lo_backing_file;
			int mode = FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE;

			if ((!file->f_op->fallocate) ||
			    lo->lo_encrypt_key_size) {
				ret = -EOPNOTSUPP;
				goto out;
			}
			ret = file->f_op->fallocate(file, mode, pos,
						    bio->bi_size);
			if (unlikely(ret && ret != -EINVAL &&
				     ret != -EOPNOTSUPP))
				ret = -EIO;
			goto out;
		}

		ret = lo_send(lo, bio, pos);

		if ((bio->bi_rw & REQ_FUA) && !ret) {
			ret = vfs_fsync(file, 0);
			if (unlikely(ret && ret != -EINVAL))
				ret = -EIO;
		}
	} else
		ret = lo_receive(lo, bio, lo->lo_blocksize, pos);

out:
	return ret;
}

/*
 * Add bio to back of pending list
 */
static void loop_add_bio(struct loop_device *lo, struct bio *bio)
{
	bio_list_add(&lo->lo_bio_list, bio);
}

/*
 * Grab first pending buffer
 */
static struct bio *loop_get_bio(struct loop_device *lo)
{
	return bio_list_pop(&lo->lo_bio_list);
}

static void loop_make_request(struct request_queue *q, struct bio *old_bio)
{
	struct loop_device *lo = q->queuedata;
	int rw = bio_rw(old_bio);

	if (rw == READA)
		rw = READ;

	BUG_ON(!lo || (rw != READ && rw != WRITE));

	spin_lock_irq(&lo->lo_lock);
	if (lo->lo_state != Lo_bound)
		goto out;
	if (unlikely(rw == WRITE && (lo->lo_flags & LO_FLAGS_READ_ONLY)))
		goto out;
	loop_add_bio(lo, old_bio);
	wake_up(&lo->lo_event);
	spin_unlock_irq(&lo->lo_lock);
	return;

out:
	spin_unlock_irq(&lo->lo_lock);
	bio_io_error(old_bio);
}

struct switch_request {
	struct file *file;
	struct completion wait;
};

static void do_loop_switch(struct loop_device *, struct switch_request *);

static inline void loop_handle_bio(struct loop_device *lo, struct bio *bio)
{
	if (unlikely(!bio->bi_bdev)) {
		do_loop_switch(lo, bio->bi_private);
		bio_put(bio);
	} else {
		int ret = do_bio_filebacked(lo, bio);
		bio_endio(bio, ret);
	}
}

/*
 * worker thread that handles reads/writes to file backed loop devices,
 * to avoid blocking in our make_request_fn. it also does loop decrypting
 * on reads for block backed loop, as that is too heavy to do from
 * b_end_io context where irqs may be disabled.
 *
 * Loop explanation:  loop_clr_fd() sets lo_state to Lo_rundown before
 * calling kthread_stop().  Therefore once kthread_should_stop() is
 * true, make_request will not place any more requests.  Therefore
 * once kthread_should_stop() is true and lo_bio is NULL, we are
 * done with the loop.
 */
static int loop_thread(void *data)
{
	struct loop_device *lo = data;
	struct bio *bio;

	set_user_nice(current, -20);

	while (!kthread_should_stop() || !bio_list_empty(&lo->lo_bio_list)) {

		wait_event_interruptible(lo->lo_event,
				!bio_list_empty(&lo->lo_bio_list) ||
				kthread_should_stop());

		if (bio_list_empty(&lo->lo_bio_list))
			continue;
		spin_lock_irq(&lo->lo_lock);
		bio = loop_get_bio(lo);
		spin_unlock_irq(&lo->lo_lock);

		BUG_ON(!bio);
		loop_handle_bio(lo, bio);
	}

	return 0;
}

/*
 * loop_switch performs the hard work of switching a backing store.
 * First it needs to flush existing IO, it does this by sending a magic
 * BIO down the pipe. The completion of this BIO does the actual switch.
 */
static int loop_switch(struct loop_device *lo, struct file *file)
{
	struct switch_request w;
	struct bio *bio = bio_alloc(GFP_KERNEL, 0);
	if (!bio)
		return -ENOMEM;
	init_completion(&w.wait);
	w.file = file;
	bio->bi_private = &w;
	bio->bi_bdev = NULL;
	loop_make_request(lo->lo_queue, bio);
	wait_for_completion(&w.wait);
	return 0;
}

/*
 * Helper to flush the IOs in loop, but keeping loop thread running
 */
static int loop_flush(struct loop_device *lo)
{
	/* loop not yet configured, no running thread, nothing to flush */
	if (!lo->lo_thread)
		return 0;

	return loop_switch(lo, NULL);
}

/*
 * Do the actual switch; called from the BIO completion routine
 */
static void do_loop_switch(struct loop_device *lo, struct switch_request *p)
{
	struct file *file = p->file;
	struct file *old_file = lo->lo_backing_file;
	struct address_space *mapping;

	/* if no new file, only flush of queued bios requested */
	if (!file)
		goto out;

	mapping = file->f_mapping;
	mapping_set_gfp_mask(old_file->f_mapping, lo->old_gfp_mask);
	lo->lo_backing_file = file;
	lo->lo_blocksize = S_ISBLK(mapping->host->i_mode) ?
		mapping->host->i_bdev->bd_block_size : PAGE_SIZE;
	lo->old_gfp_mask = mapping_gfp_mask(mapping);
	mapping_set_gfp_mask(mapping, lo->old_gfp_mask & ~(__GFP_IO|__GFP_FS));
out:
	complete(&p->wait);
}


/*
 * loop_change_fd switched the backing store of a loopback device to
 * a new file. This is useful for operating system installers to free up
 * the original file and in High Availability environments to switch to
 * an alternative location for the content in case of server meltdown.
 * This can only work if the loop device is used read-only, and if the
 * new backing store is the same size and type as the old backing store.
 */
static int loop_change_fd(struct loop_device *lo, struct block_device *bdev,
			  unsigned int arg)
{
	struct file	*file, *old_file;
	struct inode	*inode;
	int		error;

	error = -ENXIO;
	if (lo->lo_state != Lo_bound)
		goto out;

	/* the loop device has to be read-only */
	error = -EINVAL;
	if (!(lo->lo_flags & LO_FLAGS_READ_ONLY))
		goto out;

	error = -EBADF;
	file = fget(arg);
	if (!file)
		goto out;

	inode = file->f_mapping->host;
	old_file = lo->lo_backing_file;

	error = -EINVAL;

	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
		goto out_putf;

	/* size of the new backing store needs to be the same */
	if (get_loop_size(lo, file) != get_loop_size(lo, old_file))
		goto out_putf;

	/* and ... switch */
	error = loop_switch(lo, file);
	if (error)
		goto out_putf;

	fput(old_file);
	if (lo->lo_flags & LO_FLAGS_PARTSCAN)
		ioctl_by_bdev(bdev, BLKRRPART, 0);
	return 0;

 out_putf:
	fput(file);
 out:
	return error;
}

static inline int is_loop_device(struct file *file)
{
	struct inode *i = file->f_mapping->host;

	return i && S_ISBLK(i->i_mode) && MAJOR(i->i_rdev) == LOOP_MAJOR;
}

/* loop sysfs attributes */

static ssize_t loop_attr_show(struct device *dev, char *page,
			      ssize_t (*callback)(struct loop_device *, char *))
{
	struct gendisk *disk = dev_to_disk(dev);
	struct loop_device *lo = disk->private_data;

	return callback(lo, page);
}

#define LOOP_ATTR_RO(_name)						\
static ssize_t loop_attr_##_name##_show(struct loop_device *, char *);	\
static ssize_t loop_attr_do_show_##_name(struct device *d,		\
				struct device_attribute *attr, char *b)	\
{									\
	return loop_attr_show(d, b, loop_attr_##_name##_show);		\
}									\
static struct device_attribute loop_attr_##_name =			\
	__ATTR(_name, S_IRUGO, loop_attr_do_show_##_name, NULL);

static ssize_t loop_attr_backing_file_show(struct loop_device *lo, char *buf)
{
	ssize_t ret;
	char *p = NULL;

	spin_lock_irq(&lo->lo_lock);
	if (lo->lo_backing_file)
		p = d_path(&lo->lo_backing_file->f_path, buf, PAGE_SIZE - 1);
	spin_unlock_irq(&lo->lo_lock);

	if (IS_ERR_OR_NULL(p))
		ret = PTR_ERR(p);
	else {
		ret = strlen(p);
		memmove(buf, p, ret);
		buf[ret++] = '\n';
		buf[ret] = 0;
	}

	return ret;
}

static ssize_t loop_attr_offset_show(struct loop_device *lo, char *buf)
{
	return sprintf(buf, "%llu\n", (unsigned long long)lo->lo_offset);
}

static ssize_t loop_attr_sizelimit_show(struct loop_device *lo, char *buf)
{
	return sprintf(buf, "%llu\n", (unsigned long long)lo->lo_sizelimit);
}

static ssize_t loop_attr_autoclear_show(struct loop_device *lo, char *buf)
{
	int autoclear = (lo->lo_flags & LO_FLAGS_AUTOCLEAR);

	return sprintf(buf, "%s\n", autoclear ? "1" : "0");
}

static ssize_t loop_attr_partscan_show(struct loop_device *lo, char *buf)
{
	int partscan = (lo->lo_flags & LO_FLAGS_PARTSCAN);

	return sprintf(buf, "%s\n", partscan ? "1" : "0");
}

LOOP_ATTR_RO(backing_file);
LOOP_ATTR_RO(offset);
LOOP_ATTR_RO(sizelimit);
LOOP_ATTR_RO(autoclear);
LOOP_ATTR_RO(partscan);

static struct attribute *loop_attrs[] = {
	&loop_attr_backing_file.attr,
	&loop_attr_offset.attr,
	&loop_attr_sizelimit.attr,
	&loop_attr_autoclear.attr,
	&loop_attr_partscan.attr,
	NULL,
};

static struct attribute_group loop_attribute_group = {
	.name = "loop",
	.attrs= loop_attrs,
};

static int loop_sysfs_init(struct loop_device *lo)
{
	return sysfs_create_group(&disk_to_dev(lo->lo_disk)->kobj,
				  &loop_attribute_group);
}

static void loop_sysfs_exit(struct loop_device *lo)
{
	sysfs_remove_group(&disk_to_dev(lo->lo_disk)->kobj,
			   &loop_attribute_group);
}

static void loop_config_discard(struct loop_device *lo)
{
	struct file *file = lo->lo_backing_file;
	struct inode *inode = file->f_mapping->host;
	struct request_queue *q = lo->lo_queue;

	/*
	 * We use punch hole to reclaim the free space used by the
	 * image a.k.a. discard. However we do support discard if
	 * encryption is enabled, because it may give an attacker
	 * useful information.
	 */
	if ((!file->f_op->fallocate) ||
	    lo->lo_encrypt_key_size) {
		q->limits.discard_granularity = 0;
		q->limits.discard_alignment = 0;
		q->limits.max_discard_sectors = 0;
		q->limits.discard_zeroes_data = 0;
		queue_flag_clear_unlocked(QUEUE_FLAG_DISCARD, q);
		return;
	}

	q->limits.discard_granularity = inode->i_sb->s_blocksize;
	q->limits.discard_alignment = 0;
	q->limits.max_discard_sectors = UINT_MAX >> 9;
	q->limits.discard_zeroes_data = 1;
	queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, q);
}

static int loop_set_fd(struct loop_device *lo, fmode_t mode,
		       struct block_device *bdev, unsigned int arg)
{
	struct file	*file, *f;
	struct inode	*inode;
	struct address_space *mapping;
	unsigned lo_blocksize;
	int		lo_flags = 0;
	int		error;
	loff_t		size;

	/* This is safe, since we have a reference from open(). */
	__module_get(THIS_MODULE);

	error = -EBADF;
	file = fget(arg);
	if (!file)
		goto out;

	error = -EBUSY;
	if (lo->lo_state != Lo_unbound)
		goto out_putf;

	/* Avoid recursion */
	f = file;
	while (is_loop_device(f)) {
		struct loop_device *l;

		if (f->f_mapping->host->i_bdev == bdev)
			goto out_putf;

		l = f->f_mapping->host->i_bdev->bd_disk->private_data;
		if (l->lo_state == Lo_unbound) {
			error = -EINVAL;
			goto out_putf;
		}
		f = l->lo_backing_file;
	}

	mapping = file->f_mapping;
	inode = mapping->host;

	error = -EINVAL;
	if (!S_ISREG(inode->i_mode) && !S_ISBLK(inode->i_mode))
		goto out_putf;

	if (!(file->f_mode & FMODE_WRITE) || !(mode & FMODE_WRITE) ||
	    !file->f_op->write)
		lo_flags |= LO_FLAGS_READ_ONLY;

	lo_blocksize = S_ISBLK(inode->i_mode) ?
		inode->i_bdev->bd_block_size : PAGE_SIZE;

	error = -EFBIG;
	size = get_loop_size(lo, file);
	if ((loff_t)(sector_t)size != size)
		goto out_putf;

	error = 0;

	set_device_ro(bdev, (lo_flags & LO_FLAGS_READ_ONLY) != 0);

	lo->lo_blocksize = lo_blocksize;
	lo->lo_device = bdev;
	lo->lo_flags = lo_flags;
	lo->lo_backing_file = file;
	lo->transfer = transfer_none;
	lo->ioctl = NULL;
	lo->lo_sizelimit = 0;
	lo->old_gfp_mask = mapping_gfp_mask(mapping);
	mapping_set_gfp_mask(mapping, lo->old_gfp_mask & ~(__GFP_IO|__GFP_FS));

	bio_list_init(&lo->lo_bio_list);

	/*
	 * set queue make_request_fn, and add limits based on lower level
	 * device
	 */
	blk_queue_make_request(lo->lo_queue, loop_make_request);
	lo->lo_queue->queuedata = lo;

	if (!(lo_flags & LO_FLAGS_READ_ONLY) && file->f_op->fsync)
		blk_queue_flush(lo->lo_queue, REQ_FLUSH);

	set_capacity(lo->lo_disk, size);
	bd_set_size(bdev, size << 9);
	loop_sysfs_init(lo);
	/* let user-space know about the new size */
	kobject_uevent(&disk_to_dev(bdev->bd_disk)->kobj, KOBJ_CHANGE);

	set_blocksize(bdev, lo_blocksize);

	lo->lo_thread = kthread_create(loop_thread, lo, "loop%d",
						lo->lo_number);
	if (IS_ERR(lo->lo_thread)) {
		error = PTR_ERR(lo->lo_thread);
		goto out_clr;
	}
	lo->lo_state = Lo_bound;
	wake_up_process(lo->lo_thread);
	if (part_shift)
		lo->lo_flags |= LO_FLAGS_PARTSCAN;
	if (lo->lo_flags & LO_FLAGS_PARTSCAN)
		ioctl_by_bdev(bdev, BLKRRPART, 0);
	return 0;

out_clr:
	loop_sysfs_exit(lo);
	lo->lo_thread = NULL;
	lo->lo_device = NULL;
	lo->lo_backing_file = NULL;
	lo->lo_flags = 0;
	set_capacity(lo->lo_disk, 0);
	invalidate_bdev(bdev);
	bd_set_size(bdev, 0);
	kobject_uevent(&disk_to_dev(bdev->bd_disk)->kobj, KOBJ_CHANGE);
	mapping_set_gfp_mask(mapping, lo->old_gfp_mask);
	lo->lo_state = Lo_unbound;
 out_putf:
	fput(file);
 out:
	/* This is safe: open() is still holding a reference. */
	module_put(THIS_MODULE);
	return error;
}

static int
loop_release_xfer(struct loop_device *lo)
{
	int err = 0;
	struct loop_func_table *xfer = lo->lo_encryption;

	if (xfer) {
		if (xfer->release)
			err = xfer->release(lo);
		lo->transfer = NULL;
		lo->lo_encryption = NULL;
		module_put(xfer->owner);
	}
	return err;
}

static int
loop_init_xfer(struct loop_device *lo, struct loop_func_table *xfer,
	       const struct loop_info64 *i)
{
	int err = 0;

	if (xfer) {
		struct module *owner = xfer->owner;

		if (!try_module_get(owner))
			return -EINVAL;
		if (xfer->init)
			err = xfer->init(lo, i);
		if (err)
			module_put(owner);
		else
			lo->lo_encryption = xfer;
	}
	return err;
}

#ifdef MY_ABC_HERE
static int loop_clr_fd(struct loop_device *lo, struct block_device *bdev)
#else
static int loop_clr_fd(struct loop_device *lo)
#endif
{
	struct file *filp = lo->lo_backing_file;
	gfp_t gfp = lo->old_gfp_mask;
#ifdef MY_ABC_HERE
#else
	struct block_device *bdev = lo->lo_device;
#endif

	if (lo->lo_state != Lo_bound)
		return -ENXIO;

	if (lo->lo_refcnt > 1)	/* we needed one fd for the ioctl */
		return -EBUSY;

	if (filp == NULL)
		return -EINVAL;

	spin_lock_irq(&lo->lo_lock);
	lo->lo_state = Lo_rundown;
	spin_unlock_irq(&lo->lo_lock);

	kthread_stop(lo->lo_thread);

	spin_lock_irq(&lo->lo_lock);
	lo->lo_backing_file = NULL;
	spin_unlock_irq(&lo->lo_lock);

	loop_release_xfer(lo);
	lo->transfer = NULL;
	lo->ioctl = NULL;
	lo->lo_device = NULL;
	lo->lo_encryption = NULL;
	lo->lo_offset = 0;
	lo->lo_sizelimit = 0;
	lo->lo_encrypt_key_size = 0;
	lo->lo_thread = NULL;
	memset(lo->lo_encrypt_key, 0, LO_KEY_SIZE);
	memset(lo->lo_crypt_name, 0, LO_NAME_SIZE);
	memset(lo->lo_file_name, 0, LO_NAME_SIZE);
	if (bdev)
		invalidate_bdev(bdev);
	set_capacity(lo->lo_disk, 0);
	loop_sysfs_exit(lo);
	if (bdev) {
		bd_set_size(bdev, 0);
		/* let user-space know about this change */
		kobject_uevent(&disk_to_dev(bdev->bd_disk)->kobj, KOBJ_CHANGE);
	}
	mapping_set_gfp_mask(filp->f_mapping, gfp);
	lo->lo_state = Lo_unbound;
	/* This is safe: open() is still holding a reference. */
	module_put(THIS_MODULE);
	if (lo->lo_flags & LO_FLAGS_PARTSCAN && bdev)
		ioctl_by_bdev(bdev, BLKRRPART, 0);
	lo->lo_flags = 0;
	if (!part_shift)
		lo->lo_disk->flags |= GENHD_FL_NO_PART_SCAN;
	mutex_unlock(&lo->lo_ctl_mutex);
	/*
	 * Need not hold lo_ctl_mutex to fput backing file.
	 * Calling fput holding lo_ctl_mutex triggers a circular
	 * lock dependency possibility warning as fput can take
	 * bd_mutex which is usually taken before lo_ctl_mutex.
	 */
	fput(filp);
	return 0;
}

static int
loop_set_status(struct loop_device *lo, const struct loop_info64 *info)
{
	int err;
	struct loop_func_table *xfer;
	uid_t uid = current_uid();

	if (lo->lo_encrypt_key_size &&
	    lo->lo_key_owner != uid &&
	    !capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (lo->lo_state != Lo_bound)
		return -ENXIO;
	if ((unsigned int) info->lo_encrypt_key_size > LO_KEY_SIZE)
		return -EINVAL;

	err = loop_release_xfer(lo);
	if (err)
		return err;

	if (info->lo_encrypt_type) {
		unsigned int type = info->lo_encrypt_type;

		if (type >= MAX_LO_CRYPT)
			return -EINVAL;
		xfer = xfer_funcs[type];
		if (xfer == NULL)
			return -EINVAL;
	} else
		xfer = NULL;

	err = loop_init_xfer(lo, xfer, info);
	if (err)
		return err;

	if (lo->lo_offset != info->lo_offset ||
	    lo->lo_sizelimit != info->lo_sizelimit) {
		if (figure_loop_size(lo, info->lo_offset, info->lo_sizelimit))
			return -EFBIG;
	}
	loop_config_discard(lo);

	memcpy(lo->lo_file_name, info->lo_file_name, LO_NAME_SIZE);
	memcpy(lo->lo_crypt_name, info->lo_crypt_name, LO_NAME_SIZE);
	lo->lo_file_name[LO_NAME_SIZE-1] = 0;
	lo->lo_crypt_name[LO_NAME_SIZE-1] = 0;

	if (!xfer)
		xfer = &none_funcs;
	lo->transfer = xfer->transfer;
	lo->ioctl = xfer->ioctl;

	if ((lo->lo_flags & LO_FLAGS_AUTOCLEAR) !=
	     (info->lo_flags & LO_FLAGS_AUTOCLEAR))
		lo->lo_flags ^= LO_FLAGS_AUTOCLEAR;

	if ((info->lo_flags & LO_FLAGS_PARTSCAN) &&
	     !(lo->lo_flags & LO_FLAGS_PARTSCAN)) {
		lo->lo_flags |= LO_FLAGS_PARTSCAN;
		lo->lo_disk->flags &= ~GENHD_FL_NO_PART_SCAN;
		ioctl_by_bdev(lo->lo_device, BLKRRPART, 0);
	}

	lo->lo_encrypt_key_size = info->lo_encrypt_key_size;
	lo->lo_init[0] = info->lo_init[0];
	lo->lo_init[1] = info->lo_init[1];
	if (info->lo_encrypt_key_size) {
		memcpy(lo->lo_encrypt_key, info->lo_encrypt_key,
		       info->lo_encrypt_key_size);
		lo->lo_key_owner = uid;
	}	

	return 0;
}

static int
loop_get_status(struct loop_device *lo, struct loop_info64 *info)
{
	struct file *file = lo->lo_backing_file;
	struct kstat stat;
	int error;

	if (lo->lo_state != Lo_bound)
		return -ENXIO;
	error = vfs_getattr(file->f_path.mnt, file->f_path.dentry, &stat);
	if (error)
		return error;
	memset(info, 0, sizeof(*info));
	info->lo_number = lo->lo_number;
	info->lo_device = huge_encode_dev(stat.dev);
	info->lo_inode = stat.ino;
	info->lo_rdevice = huge_encode_dev(lo->lo_device ? stat.rdev : stat.dev);
	info->lo_offset = lo->lo_offset;
	info->lo_sizelimit = lo->lo_sizelimit;
	info->lo_flags = lo->lo_flags;
	memcpy(info->lo_file_name, lo->lo_file_name, LO_NAME_SIZE);
	memcpy(info->lo_crypt_name, lo->lo_crypt_name, LO_NAME_SIZE);
	info->lo_encrypt_type =
		lo->lo_encryption ? lo->lo_encryption->number : 0;
	if (lo->lo_encrypt_key_size && capable(CAP_SYS_ADMIN)) {
		info->lo_encrypt_key_size = lo->lo_encrypt_key_size;
		memcpy(info->lo_encrypt_key, lo->lo_encrypt_key,
		       lo->lo_encrypt_key_size);
	}
	return 0;
}

static void
loop_info64_from_old(const struct loop_info *info, struct loop_info64 *info64)
{
	memset(info64, 0, sizeof(*info64));
	info64->lo_number = info->lo_number;
	info64->lo_device = info->lo_device;
	info64->lo_inode = info->lo_inode;
	info64->lo_rdevice = info->lo_rdevice;
	info64->lo_offset = info->lo_offset;
	info64->lo_sizelimit = 0;
	info64->lo_encrypt_type = info->lo_encrypt_type;
	info64->lo_encrypt_key_size = info->lo_encrypt_key_size;
	info64->lo_flags = info->lo_flags;
	info64->lo_init[0] = info->lo_init[0];
	info64->lo_init[1] = info->lo_init[1];
	if (info->lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info64->lo_crypt_name, info->lo_name, LO_NAME_SIZE);
	else
		memcpy(info64->lo_file_name, info->lo_name, LO_NAME_SIZE);
	memcpy(info64->lo_encrypt_key, info->lo_encrypt_key, LO_KEY_SIZE);
}

static int
loop_info64_to_old(const struct loop_info64 *info64, struct loop_info *info)
{
	memset(info, 0, sizeof(*info));
	info->lo_number = info64->lo_number;
	info->lo_device = info64->lo_device;
	info->lo_inode = info64->lo_inode;
	info->lo_rdevice = info64->lo_rdevice;
	info->lo_offset = info64->lo_offset;
	info->lo_encrypt_type = info64->lo_encrypt_type;
	info->lo_encrypt_key_size = info64->lo_encrypt_key_size;
	info->lo_flags = info64->lo_flags;
	info->lo_init[0] = info64->lo_init[0];
	info->lo_init[1] = info64->lo_init[1];
	if (info->lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info->lo_name, info64->lo_crypt_name, LO_NAME_SIZE);
	else
		memcpy(info->lo_name, info64->lo_file_name, LO_NAME_SIZE);
	memcpy(info->lo_encrypt_key, info64->lo_encrypt_key, LO_KEY_SIZE);

	/* error in case values were truncated */
	if (info->lo_device != info64->lo_device ||
	    info->lo_rdevice != info64->lo_rdevice ||
	    info->lo_inode != info64->lo_inode ||
	    info->lo_offset != info64->lo_offset)
		return -EOVERFLOW;

	return 0;
}

static int
loop_set_status_old(struct loop_device *lo, const struct loop_info __user *arg)
{
	struct loop_info info;
	struct loop_info64 info64;

	if (copy_from_user(&info, arg, sizeof (struct loop_info)))
		return -EFAULT;
	loop_info64_from_old(&info, &info64);
	return loop_set_status(lo, &info64);
}

static int
loop_set_status64(struct loop_device *lo, const struct loop_info64 __user *arg)
{
	struct loop_info64 info64;

	if (copy_from_user(&info64, arg, sizeof (struct loop_info64)))
		return -EFAULT;
	return loop_set_status(lo, &info64);
}

static int
loop_get_status_old(struct loop_device *lo, struct loop_info __user *arg) {
	struct loop_info info;
	struct loop_info64 info64;
	int err = 0;

	if (!arg)
		err = -EINVAL;
	if (!err)
		err = loop_get_status(lo, &info64);
	if (!err)
		err = loop_info64_to_old(&info64, &info);
	if (!err && copy_to_user(arg, &info, sizeof(info)))
		err = -EFAULT;

	return err;
}

static int
loop_get_status64(struct loop_device *lo, struct loop_info64 __user *arg) {
	struct loop_info64 info64;
	int err = 0;

	if (!arg)
		err = -EINVAL;
	if (!err)
		err = loop_get_status(lo, &info64);
	if (!err && copy_to_user(arg, &info64, sizeof(info64)))
		err = -EFAULT;

	return err;
}

static int loop_set_capacity(struct loop_device *lo, struct block_device *bdev)
{
	int err;
	sector_t sec;
	loff_t sz;

	err = -ENXIO;
	if (unlikely(lo->lo_state != Lo_bound))
		goto out;
	err = figure_loop_size(lo, lo->lo_offset, lo->lo_sizelimit);
	if (unlikely(err))
		goto out;
	sec = get_capacity(lo->lo_disk);
	/* the width of sector_t may be narrow for bit-shift */
	sz = sec;
	sz <<= 9;
	mutex_lock(&bdev->bd_mutex);
	bd_set_size(bdev, sz);
	/* let user-space know about the new size */
	kobject_uevent(&disk_to_dev(bdev->bd_disk)->kobj, KOBJ_CHANGE);
	mutex_unlock(&bdev->bd_mutex);

 out:
	return err;
}

static int lo_ioctl(struct block_device *bdev, fmode_t mode,
	unsigned int cmd, unsigned long arg)
{
	struct loop_device *lo = bdev->bd_disk->private_data;
	int err;

	mutex_lock_nested(&lo->lo_ctl_mutex, 1);
	switch (cmd) {
	case LOOP_SET_FD:
		err = loop_set_fd(lo, mode, bdev, arg);
		break;
	case LOOP_CHANGE_FD:
		err = loop_change_fd(lo, bdev, arg);
		break;
	case LOOP_CLR_FD:
		/* loop_clr_fd would have unlocked lo_ctl_mutex on success */
#ifdef MY_ABC_HERE
		err = loop_clr_fd(lo, bdev);
#else
		err = loop_clr_fd(lo);
#endif
		if (!err)
			goto out_unlocked;
		break;
	case LOOP_SET_STATUS:
		err = -EPERM;
		if ((mode & FMODE_WRITE) || capable(CAP_SYS_ADMIN))
			err = loop_set_status_old(lo,
					(struct loop_info __user *)arg);
		break;
	case LOOP_GET_STATUS:
		err = loop_get_status_old(lo, (struct loop_info __user *) arg);
		break;
	case LOOP_SET_STATUS64:
		err = -EPERM;
		if ((mode & FMODE_WRITE) || capable(CAP_SYS_ADMIN))
			err = loop_set_status64(lo,
					(struct loop_info64 __user *) arg);
		break;
	case LOOP_GET_STATUS64:
		err = loop_get_status64(lo, (struct loop_info64 __user *) arg);
		break;
	case LOOP_SET_CAPACITY:
		err = -EPERM;
		if ((mode & FMODE_WRITE) || capable(CAP_SYS_ADMIN))
			err = loop_set_capacity(lo, bdev);
		break;
	default:
		err = lo->ioctl ? lo->ioctl(lo, cmd, arg) : -EINVAL;
	}
	mutex_unlock(&lo->lo_ctl_mutex);

out_unlocked:
	return err;
}

#ifdef CONFIG_COMPAT
struct compat_loop_info {
	compat_int_t	lo_number;      /* ioctl r/o */
	compat_dev_t	lo_device;      /* ioctl r/o */
	compat_ulong_t	lo_inode;       /* ioctl r/o */
	compat_dev_t	lo_rdevice;     /* ioctl r/o */
	compat_int_t	lo_offset;
	compat_int_t	lo_encrypt_type;
	compat_int_t	lo_encrypt_key_size;    /* ioctl w/o */
	compat_int_t	lo_flags;       /* ioctl r/o */
	char		lo_name[LO_NAME_SIZE];
	unsigned char	lo_encrypt_key[LO_KEY_SIZE]; /* ioctl w/o */
	compat_ulong_t	lo_init[2];
	char		reserved[4];
};

/*
 * Transfer 32-bit compatibility structure in userspace to 64-bit loop info
 * - noinlined to reduce stack space usage in main part of driver
 */
static noinline int
loop_info64_from_compat(const struct compat_loop_info __user *arg,
			struct loop_info64 *info64)
{
	struct compat_loop_info info;

	if (copy_from_user(&info, arg, sizeof(info)))
		return -EFAULT;

	memset(info64, 0, sizeof(*info64));
	info64->lo_number = info.lo_number;
	info64->lo_device = info.lo_device;
	info64->lo_inode = info.lo_inode;
	info64->lo_rdevice = info.lo_rdevice;
	info64->lo_offset = info.lo_offset;
	info64->lo_sizelimit = 0;
	info64->lo_encrypt_type = info.lo_encrypt_type;
	info64->lo_encrypt_key_size = info.lo_encrypt_key_size;
	info64->lo_flags = info.lo_flags;
	info64->lo_init[0] = info.lo_init[0];
	info64->lo_init[1] = info.lo_init[1];
	if (info.lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info64->lo_crypt_name, info.lo_name, LO_NAME_SIZE);
	else
		memcpy(info64->lo_file_name, info.lo_name, LO_NAME_SIZE);
	memcpy(info64->lo_encrypt_key, info.lo_encrypt_key, LO_KEY_SIZE);
	return 0;
}

/*
 * Transfer 64-bit loop info to 32-bit compatibility structure in userspace
 * - noinlined to reduce stack space usage in main part of driver
 */
static noinline int
loop_info64_to_compat(const struct loop_info64 *info64,
		      struct compat_loop_info __user *arg)
{
	struct compat_loop_info info;

	memset(&info, 0, sizeof(info));
	info.lo_number = info64->lo_number;
	info.lo_device = info64->lo_device;
	info.lo_inode = info64->lo_inode;
	info.lo_rdevice = info64->lo_rdevice;
	info.lo_offset = info64->lo_offset;
	info.lo_encrypt_type = info64->lo_encrypt_type;
	info.lo_encrypt_key_size = info64->lo_encrypt_key_size;
	info.lo_flags = info64->lo_flags;
	info.lo_init[0] = info64->lo_init[0];
	info.lo_init[1] = info64->lo_init[1];
	if (info.lo_encrypt_type == LO_CRYPT_CRYPTOAPI)
		memcpy(info.lo_name, info64->lo_crypt_name, LO_NAME_SIZE);
	else
		memcpy(info.lo_name, info64->lo_file_name, LO_NAME_SIZE);
	memcpy(info.lo_encrypt_key, info64->lo_encrypt_key, LO_KEY_SIZE);

	/* error in case values were truncated */
	if (info.lo_device != info64->lo_device ||
	    info.lo_rdevice != info64->lo_rdevice ||
	    info.lo_inode != info64->lo_inode ||
	    info.lo_offset != info64->lo_offset ||
	    info.lo_init[0] != info64->lo_init[0] ||
	    info.lo_init[1] != info64->lo_init[1])
		return -EOVERFLOW;

	if (copy_to_user(arg, &info, sizeof(info)))
		return -EFAULT;
	return 0;
}

static int
loop_set_status_compat(struct loop_device *lo,
		       const struct compat_loop_info __user *arg)
{
	struct loop_info64 info64;
	int ret;

	ret = loop_info64_from_compat(arg, &info64);
	if (ret < 0)
		return ret;
	return loop_set_status(lo, &info64);
}

static int
loop_get_status_compat(struct loop_device *lo,
		       struct compat_loop_info __user *arg)
{
	struct loop_info64 info64;
	int err = 0;

	if (!arg)
		err = -EINVAL;
	if (!err)
		err = loop_get_status(lo, &info64);
	if (!err)
		err = loop_info64_to_compat(&info64, arg);
	return err;
}

static int lo_compat_ioctl(struct block_device *bdev, fmode_t mode,
			   unsigned int cmd, unsigned long arg)
{
	struct loop_device *lo = bdev->bd_disk->private_data;
	int err;

	switch(cmd) {
	case LOOP_SET_STATUS:
		mutex_lock(&lo->lo_ctl_mutex);
		err = loop_set_status_compat(
			lo, (const struct compat_loop_info __user *) arg);
		mutex_unlock(&lo->lo_ctl_mutex);
		break;
	case LOOP_GET_STATUS:
		mutex_lock(&lo->lo_ctl_mutex);
		err = loop_get_status_compat(
			lo, (struct compat_loop_info __user *) arg);
		mutex_unlock(&lo->lo_ctl_mutex);
		break;
	case LOOP_SET_CAPACITY:
	case LOOP_CLR_FD:
	case LOOP_GET_STATUS64:
	case LOOP_SET_STATUS64:
		arg = (unsigned long) compat_ptr(arg);
	case LOOP_SET_FD:
	case LOOP_CHANGE_FD:
		err = lo_ioctl(bdev, mode, cmd, arg);
		break;
	default:
		err = -ENOIOCTLCMD;
		break;
	}
	return err;
}
#endif

static int lo_open(struct block_device *bdev, fmode_t mode)
{
	struct loop_device *lo;
	int err = 0;

	mutex_lock(&loop_index_mutex);
	lo = bdev->bd_disk->private_data;
	if (!lo) {
		err = -ENXIO;
		goto out;
	}

	mutex_lock(&lo->lo_ctl_mutex);
	lo->lo_refcnt++;
	mutex_unlock(&lo->lo_ctl_mutex);
out:
	mutex_unlock(&loop_index_mutex);
	return err;
}

static int lo_release(struct gendisk *disk, fmode_t mode)
{
	struct loop_device *lo = disk->private_data;
	int err;

	mutex_lock(&lo->lo_ctl_mutex);

	if (--lo->lo_refcnt)
		goto out;

	if (lo->lo_flags & LO_FLAGS_AUTOCLEAR) {
		/*
		 * In autoclear mode, stop the loop thread
		 * and remove configuration after last close.
		 */
#ifdef MY_ABC_HERE
		err = loop_clr_fd(lo, lo->lo_device);
#else
		err = loop_clr_fd(lo);
#endif
		if (!err)
			goto out_unlocked;
	} else {
		/*
		 * Otherwise keep thread (if running) and config,
		 * but flush possible ongoing bios in thread.
		 */
		loop_flush(lo);
	}

out:
	mutex_unlock(&lo->lo_ctl_mutex);
out_unlocked:
	return 0;
}

static const struct block_device_operations lo_fops = {
	.owner =	THIS_MODULE,
	.open =		lo_open,
	.release =	lo_release,
	.ioctl =	lo_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl =	lo_compat_ioctl,
#endif
};

/*
 * And now the modules code and kernel interface.
 */
static int max_loop;
module_param(max_loop, int, S_IRUGO);
MODULE_PARM_DESC(max_loop, "Maximum number of loop devices");
module_param(max_part, int, S_IRUGO);
MODULE_PARM_DESC(max_part, "Maximum number of partitions per loop device");
MODULE_LICENSE("GPL");
MODULE_ALIAS_BLOCKDEV_MAJOR(LOOP_MAJOR);

int loop_register_transfer(struct loop_func_table *funcs)
{
	unsigned int n = funcs->number;

	if (n >= MAX_LO_CRYPT || xfer_funcs[n])
		return -EINVAL;
	xfer_funcs[n] = funcs;
	return 0;
}

static int unregister_transfer_cb(int id, void *ptr, void *data)
{
	struct loop_device *lo = ptr;
	struct loop_func_table *xfer = data;

	mutex_lock(&lo->lo_ctl_mutex);
	if (lo->lo_encryption == xfer)
		loop_release_xfer(lo);
	mutex_unlock(&lo->lo_ctl_mutex);
	return 0;
}

int loop_unregister_transfer(int number)
{
	unsigned int n = number;
	struct loop_func_table *xfer;

	if (n == 0 || n >= MAX_LO_CRYPT || (xfer = xfer_funcs[n]) == NULL)
		return -EINVAL;

	xfer_funcs[n] = NULL;
	idr_for_each(&loop_index_idr, &unregister_transfer_cb, xfer);
	return 0;
}

EXPORT_SYMBOL(loop_register_transfer);
EXPORT_SYMBOL(loop_unregister_transfer);

static int loop_add(struct loop_device **l, int i)
{
	struct loop_device *lo;
	struct gendisk *disk;
	int err;

	lo = kzalloc(sizeof(*lo), GFP_KERNEL);
	if (!lo) {
		err = -ENOMEM;
		goto out;
	}

	err = idr_pre_get(&loop_index_idr, GFP_KERNEL);
	if (err < 0)
		goto out_free_dev;

	if (i >= 0) {
		int m;

		/* create specific i in the index */
		err = idr_get_new_above(&loop_index_idr, lo, i, &m);
		if (err >= 0 && i != m) {
			idr_remove(&loop_index_idr, m);
			err = -EEXIST;
		}
	} else if (i == -1) {
		int m;

		/* get next free nr */
		err = idr_get_new(&loop_index_idr, lo, &m);
		if (err >= 0)
			i = m;
	} else {
		err = -EINVAL;
	}
	if (err < 0)
		goto out_free_dev;

	lo->lo_queue = blk_alloc_queue(GFP_KERNEL);
	if (!lo->lo_queue)
		goto out_free_dev;

	disk = lo->lo_disk = alloc_disk(1 << part_shift);
	if (!disk)
		goto out_free_queue;

	/*
	 * Disable partition scanning by default. The in-kernel partition
	 * scanning can be requested individually per-device during its
	 * setup. Userspace can always add and remove partitions from all
	 * devices. The needed partition minors are allocated from the
	 * extended minor space, the main loop device numbers will continue
	 * to match the loop minors, regardless of the number of partitions
	 * used.
	 *
	 * If max_part is given, partition scanning is globally enabled for
	 * all loop devices. The minors for the main loop devices will be
	 * multiples of max_part.
	 *
	 * Note: Global-for-all-devices, set-only-at-init, read-only module
	 * parameteters like 'max_loop' and 'max_part' make things needlessly
	 * complicated, are too static, inflexible and may surprise
	 * userspace tools. Parameters like this in general should be avoided.
	 */
	if (!part_shift)
		disk->flags |= GENHD_FL_NO_PART_SCAN;
	disk->flags |= GENHD_FL_EXT_DEVT;
	mutex_init(&lo->lo_ctl_mutex);
	lo->lo_number		= i;
	lo->lo_thread		= NULL;
	init_waitqueue_head(&lo->lo_event);
	spin_lock_init(&lo->lo_lock);
	disk->major		= LOOP_MAJOR;
	disk->first_minor	= i << part_shift;
	disk->fops		= &lo_fops;
	disk->private_data	= lo;
	disk->queue		= lo->lo_queue;
	sprintf(disk->disk_name, "loop%d", i);
	add_disk(disk);
	*l = lo;
	return lo->lo_number;

out_free_queue:
	blk_cleanup_queue(lo->lo_queue);
out_free_dev:
	kfree(lo);
out:
	return err;
}

static void loop_remove(struct loop_device *lo)
{
	del_gendisk(lo->lo_disk);
	blk_cleanup_queue(lo->lo_queue);
	put_disk(lo->lo_disk);
	kfree(lo);
}

static int find_free_cb(int id, void *ptr, void *data)
{
	struct loop_device *lo = ptr;
	struct loop_device **l = data;

	if (lo->lo_state == Lo_unbound) {
		*l = lo;
		return 1;
	}
	return 0;
}

static int loop_lookup(struct loop_device **l, int i)
{
	struct loop_device *lo;
	int ret = -ENODEV;

	if (i < 0) {
		int err;

		err = idr_for_each(&loop_index_idr, &find_free_cb, &lo);
		if (err == 1) {
			*l = lo;
			ret = lo->lo_number;
		}
		goto out;
	}

	/* lookup and return a specific i */
	lo = idr_find(&loop_index_idr, i);
	if (lo) {
		*l = lo;
		ret = lo->lo_number;
	}
out:
	return ret;
}

static struct kobject *loop_probe(dev_t dev, int *part, void *data)
{
	struct loop_device *lo;
	struct kobject *kobj;
	int err;

	mutex_lock(&loop_index_mutex);
	err = loop_lookup(&lo, MINOR(dev) >> part_shift);
	if (err < 0)
		err = loop_add(&lo, MINOR(dev) >> part_shift);
	if (err < 0)
		kobj = ERR_PTR(err);
	else
		kobj = get_disk(lo->lo_disk);
	mutex_unlock(&loop_index_mutex);

	*part = 0;
	return kobj;
}

static long loop_control_ioctl(struct file *file, unsigned int cmd,
			       unsigned long parm)
{
	struct loop_device *lo;
	int ret = -ENOSYS;

	mutex_lock(&loop_index_mutex);
	switch (cmd) {
	case LOOP_CTL_ADD:
		ret = loop_lookup(&lo, parm);
		if (ret >= 0) {
			ret = -EEXIST;
			break;
		}
		ret = loop_add(&lo, parm);
		break;
	case LOOP_CTL_REMOVE:
		ret = loop_lookup(&lo, parm);
		if (ret < 0)
			break;
		mutex_lock(&lo->lo_ctl_mutex);
		if (lo->lo_state != Lo_unbound) {
			ret = -EBUSY;
			mutex_unlock(&lo->lo_ctl_mutex);
			break;
		}
		if (lo->lo_refcnt > 0) {
			ret = -EBUSY;
			mutex_unlock(&lo->lo_ctl_mutex);
			break;
		}
		lo->lo_disk->private_data = NULL;
		mutex_unlock(&lo->lo_ctl_mutex);
		idr_remove(&loop_index_idr, lo->lo_number);
		loop_remove(lo);
		break;
	case LOOP_CTL_GET_FREE:
		ret = loop_lookup(&lo, -1);
		if (ret >= 0)
			break;
		ret = loop_add(&lo, -1);
	}
	mutex_unlock(&loop_index_mutex);

	return ret;
}

static const struct file_operations loop_ctl_fops = {
	.open		= nonseekable_open,
	.unlocked_ioctl	= loop_control_ioctl,
	.compat_ioctl	= loop_control_ioctl,
	.owner		= THIS_MODULE,
	.llseek		= noop_llseek,
};

static struct miscdevice loop_misc = {
	.minor		= LOOP_CTRL_MINOR,
	.name		= "loop-control",
	.fops		= &loop_ctl_fops,
};

MODULE_ALIAS_MISCDEV(LOOP_CTRL_MINOR);
MODULE_ALIAS("devname:loop-control");

static int __init loop_init(void)
{
	int i, nr;
	unsigned long range;
	struct loop_device *lo;
	int err;

	err = misc_register(&loop_misc);
	if (err < 0)
		return err;

	part_shift = 0;
	if (max_part > 0) {
		part_shift = fls(max_part);

		/*
		 * Adjust max_part according to part_shift as it is exported
		 * to user space so that user can decide correct minor number
		 * if [s]he want to create more devices.
		 *
		 * Note that -1 is required because partition 0 is reserved
		 * for the whole disk.
		 */
		max_part = (1UL << part_shift) - 1;
	}

	if ((1UL << part_shift) > DISK_MAX_PARTS)
		return -EINVAL;

	if (max_loop > 1UL << (MINORBITS - part_shift))
		return -EINVAL;

	/*
	 * If max_loop is specified, create that many devices upfront.
	 * This also becomes a hard limit. If max_loop is not specified,
	 * create CONFIG_BLK_DEV_LOOP_MIN_COUNT loop devices at module
	 * init time. Loop devices can be requested on-demand with the
	 * /dev/loop-control interface, or be instantiated by accessing
	 * a 'dead' device node.
	 */
	if (max_loop) {
		nr = max_loop;
		range = max_loop << part_shift;
	} else {
		nr = CONFIG_BLK_DEV_LOOP_MIN_COUNT;
		range = 1UL << MINORBITS;
	}

	if (register_blkdev(LOOP_MAJOR, "loop"))
		return -EIO;

	blk_register_region(MKDEV(LOOP_MAJOR, 0), range,
				  THIS_MODULE, loop_probe, NULL, NULL);

	/* pre-create number of devices given by config or max_loop */
	mutex_lock(&loop_index_mutex);
	for (i = 0; i < nr; i++)
		loop_add(&lo, i);
	mutex_unlock(&loop_index_mutex);

	printk(KERN_INFO "loop: module loaded\n");
	return 0;
}

static int loop_exit_cb(int id, void *ptr, void *data)
{
	struct loop_device *lo = ptr;

	loop_remove(lo);
	return 0;
}

static void __exit loop_exit(void)
{
	unsigned long range;

	range = max_loop ? max_loop << part_shift : 1UL << MINORBITS;

	idr_for_each(&loop_index_idr, &loop_exit_cb, NULL);
	idr_remove_all(&loop_index_idr);
	idr_destroy(&loop_index_idr);

	blk_unregister_region(MKDEV(LOOP_MAJOR, 0), range);
	unregister_blkdev(LOOP_MAJOR, "loop");

	misc_deregister(&loop_misc);
}

module_init(loop_init);
module_exit(loop_exit);

#ifndef MODULE
static int __init max_loop_setup(char *str)
{
	max_loop = simple_strtol(str, NULL, 0);
	return 1;
}

__setup("max_loop=", max_loop_setup);
#endif
