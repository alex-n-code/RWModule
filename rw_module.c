#include <linux/init.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/proc_fs.h>
#include <linux/kernel.h>
#include <linux/device.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/jiffies.h>

#define TIMEOUT 1000

static int rwdev_open(struct inode *inode, struct file *file);
static ssize_t rwdev_read(struct file *file, char __user *buf, size_t count, loff_t *offset);
static ssize_t rwdev_write(struct file *file, const char __user *buf, size_t count, loff_t *offset);
static int rwdev_release(struct inode *inode, struct file *file);
static long rwdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg);

struct mutex buff_mutex;

static const struct file_operations rw_fops = 
{
	.owner = THIS_MODULE,
	.open = rwdev_open,
	.release = rwdev_release,
	.unlocked_ioctl = rwdev_ioctl,
	.read = rwdev_read,
	.write = rwdev_write
};

struct rw_device_data {
	struct cdev cdev;
};

struct rw_buff_frag {
	struct list_head list;
	size_t data_len;
	uint8_t *data;
	uint8_t *data_ptr;
};

static int dev_major = 0;
static struct class *rwdev_class = NULL;
static struct rw_device_data rwdev_data;

static struct list_head buff_head;
static struct timer_list print_timer;

static struct list_head *curr_elem;
static size_t curr_offset = 0;

static uint8_t is_word_separator(const char c){
	return (c == ' ' || c == '\t' || c == '\n' || c == '\r');
}

static ssize_t seek_first_char(uint8_t *start, size_t offset, size_t len){
	ssize_t c_offset = offset;
	while(c_offset < len){
		if(!is_word_separator(*(start + c_offset))){
			return c_offset;
		}
		++c_offset;
	}
	return -1;
}

static ssize_t seek_separator(uint8_t *start, size_t offset, size_t len){
	ssize_t c_offset = offset;
	while(c_offset < len){
		if(is_word_separator(*(start + c_offset))){
			return c_offset;
		}
		++c_offset;
	}
	return -1;
}

static int buff_get_word(uint8_t **mem){
	
	size_t len = 0;
	mutex_lock(&buff_mutex);
	
	if (curr_elem->next == curr_elem){
		mutex_unlock(&buff_mutex);
		return 0;
	}
	if(curr_elem == &buff_head){ 
		curr_elem = curr_elem->prev; 
		curr_offset = 0;
	}
	
	while(curr_elem != &buff_head){
		struct rw_buff_frag *frag = (struct rw_buff_frag *)(curr_elem);
		
		ssize_t start_offset, end_offset, next_offset = 0;
		
		start_offset = seek_first_char(frag->data_ptr, curr_offset, frag->data_len);
		if(start_offset < 0){
			curr_elem = curr_elem->prev;
			curr_offset = 0;
			continue;
		}
		end_offset = seek_separator(frag->data_ptr, start_offset, frag->data_len);
		if(end_offset < 0){
			curr_elem = curr_elem->prev;
			curr_offset = 0;
			continue;
		}
		next_offset = seek_first_char(frag->data_ptr, end_offset, frag->data_len);
		if(next_offset > 0){
			len = (size_t)(next_offset - curr_offset);
		}
		else{
			len = (size_t)(frag->data_len - curr_offset);
		}
		*mem = kmalloc(sizeof(uint8_t) * (len + 1), GFP_KERNEL);
		memcpy(*mem, frag->data_ptr + start_offset, len);
		*(*mem + len) = '\0';
		curr_offset += len;
		break;
	}
	
	mutex_unlock(&buff_mutex);
	return len;
}

static int rwdew_uevent(struct device *dev, struct kobj_uevent_env *env){
	add_uevent_var(env, "DEVMODE=%#o", 0666);
	return 0;
}

void print_timer_callback(struct timer_list *data){

	uint8_t *mem;
	size_t len = buff_get_word(&mem);
	
	if(len > 0){ printk(KERN_CONT "%s", mem); kfree(mem); }
	
	mod_timer(&print_timer, jiffies + msecs_to_jiffies(TIMEOUT));

}

static int __init rwdev_init(void){
	int err;
	dev_t dev;
	
	err = alloc_chrdev_region(&dev, 0, 1, "rwdev");
	if(err != 0){
		printk("Fehler beim allocierung von chardev Region. Fehler Nr. %d", err);
		return err;
	}
	
	dev_major = MAJOR(dev);
	rwdev_class = class_create(THIS_MODULE, "rwdev");
	rwdev_class->dev_uevent = rwdew_uevent;
	
	cdev_init(&rwdev_data.cdev, &rw_fops);
	rwdev_data.cdev.owner = THIS_MODULE;
		
	cdev_add(&rwdev_data.cdev, MKDEV(dev_major, 0), 1);
	device_create(rwdev_class, NULL, MKDEV(dev_major, 0), NULL, "rwdev-0");
	
	mutex_init(&buff_mutex);
	
	INIT_LIST_HEAD(&buff_head);
	
	curr_elem = &buff_head;
	curr_offset  = 0;
	
	timer_setup(&print_timer, print_timer_callback, 0);
	mod_timer(&print_timer, jiffies + msecs_to_jiffies(TIMEOUT));
	
	printk("RWDEV: Modul erflogrieich geladen");
	
	return 0;
}

static void __exit rwdev_exit(void){
	
	struct list_head *elem = buff_head.next;
	
	device_destroy(rwdev_class, MKDEV(dev_major, 0));
	
	class_unregister(rwdev_class);
	class_destroy(rwdev_class);
	
	del_timer(&print_timer);
	
	unregister_chrdev_region(MKDEV(dev_major, 0), 1);
	
	while(elem != &buff_head){
		struct rw_buff_frag *buff_frag = (struct rw_buff_frag *)(elem);
		elem = elem->next;
		list_del(elem->prev);
		kfree(buff_frag->data);
		kfree(buff_frag);
	} 
	
	printk("RWDEV: Modul entfernt");
}

static ssize_t rwdev_read(struct file *file, char __user *buf, size_t count, loff_t *offset){
	
	size_t read_cnt = 0;
	struct list_head *elem;
	
	mutex_lock(&buff_mutex);
 	elem = buff_head.prev; 
	while(elem != &buff_head && read_cnt < count){
		struct rw_buff_frag *frag = (struct rw_buff_frag *)(elem);
		size_t to_read = count - read_cnt;
		size_t n_copied = 0;
		
		if(frag->data_len < to_read){ to_read = frag->data_len; }
		
		n_copied = copy_to_user(buf, frag->data_ptr, to_read);
		to_read -= n_copied;
		if(frag->data_len == to_read){
			//Update next word pointer
			if(curr_elem == elem){ curr_elem = elem->prev; curr_offset = 0;}
			
			elem = elem->prev;
			list_del(elem->next);
			kfree(frag->data);
			kfree(frag);
		}
		else{
			frag->data_len -= to_read;
			frag->data_ptr += to_read;
			
			//Update next word pointer
			if(curr_elem == elem){
				if(curr_offset <= to_read){ curr_offset = 0; }
				else{
					curr_offset -= to_read;
				}
			}
		}
		read_cnt += to_read;
	}
	mutex_unlock(&buff_mutex);
	
	return read_cnt;
}

static ssize_t rwdev_write(struct file *file, const char __user *buf, size_t count, loff_t *offset){

	struct rw_buff_frag *buff_frag = (struct rw_buff_frag*) kmalloc(sizeof(struct rw_buff_frag), GFP_KERNEL);
	uint8_t *data = (uint8_t *) kmalloc(sizeof(uint8_t) * count, GFP_KERNEL);
	size_t n_copied = 0;

	buff_frag->data_len = count;
	buff_frag->data = data;
	buff_frag->data_ptr = data;

	n_copied = copy_from_user(data, buf, count);
	if(n_copied != 0){
		kfree(data);
		kfree(buff_frag);
		return 0;
	}

	mutex_lock(&buff_mutex);
	list_add(&buff_frag->list, &buff_head);
	mutex_unlock(&buff_mutex);

	return count;

}

static int rwdev_open(struct inode *inode, struct file *file){
	printk("RWDEV: geoefnet\n");
	return 0;
}

static int rwdev_release(struct inode *inode, struct file *file){
	printk("RWDEV: geschloessen\n");
	return 0;
}

static long rwdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg){
	//printk("RWDEV: Device ioctl\n");
	return 0;
}

module_init(rwdev_init);
module_exit(rwdev_exit);

MODULE_AUTHOR("Aleksandar Ilic");
MODULE_LICENSE("GPL");
