/**
 * File: asgn2.c
 * Date: 15/05/2012
 * Author: Calum O'Hare
 * Version: 0.1
 *
 * This is a module which serves as a virtual ramdisk which disk size is
 * limited by the amount of memory available and serves as the requirement for
 * COSC440 assignment 1 in 2012.
 *
 * Note: multiple devices and concurrent modules are not supported in this
 *       version.
 */
 
/* This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/proc_fs.h>
#include <linux/device.h>
#include <linux/ioport.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <asm/io.h>
#include <linux/sched.h>

#define MYDEV_NAME "asgn2"
#define MYIOC_TYPE 'k'

#define BUFFER_SIZE 256                    /* Size of the circular buffer */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Calum O'Hare");
MODULE_DESCRIPTION("COSC440 asgn2");

/**
 * The node structure for the memory page linked list.
 */ 
typedef struct page_node_rec {
  struct list_head list;
  struct page *page;
} page_node;

typedef struct asgn2_dev_t {
  dev_t dev;            /* the device */
  struct cdev *cdev;
  struct list_head mem_list; 
  int num_pages;        /* number of memory pages this module currently holds */
  size_t data_size;     /* total data size in this module */
  atomic_t nprocs;      /* number of processes accessing this device */ 
  atomic_t max_nprocs;  /* max number of processes accessing this device */
  atomic_t read_lock;   /* Whether the read function can be accessed */
  struct kmem_cache *cache;      /* cache memory */
  struct class *class;     /* the udev class */
  struct device *device;   /* the udev device node */
} asgn2_dev;

asgn2_dev asgn2_device;

DECLARE_WAIT_QUEUE_HEAD(my_queue); /* Declare a wait queue for waiting read processes */

int *nulchars;
int num_files = 0;
int read_count = 0;
int eof = 0;

int asgn2_major = 0;                      /* major number of module */  
int asgn2_minor = 0;                      /* minor number of module */
int asgn2_dev_count = 1;                  /* number of devices */
unsigned long parport = 0x378;            /* Parallel port */

typedef struct circular_buffer{
    char content[BUFFER_SIZE];
    int head;
    int tail;
} circ_buf;

circ_buf cbuf;

/*
 * Writes to the circular buffer
 * I choose to overwrite the head
 * if the buffer was full
 */
void write_circ_buf(char data){

    cbuf.content[cbuf.tail] = data;
    cbuf.tail++;
    if(cbuf.tail == cbuf.head){
        cbuf.head++;
        if(cbuf.head == BUFFER_SIZE){
            cbuf.head = 0;
        }
    }
    if(cbuf.tail == BUFFER_SIZE){
        cbuf.tail = 0;
    }
}

/*
 * Reads from the circular buffer
 */
char read_circ_buf(void){
    char read;

    if(cbuf.head == cbuf.tail){
        return '\0';
    }

    read = cbuf.content[cbuf.head];
    cbuf.head++;
    if(cbuf.head == BUFFER_SIZE){
        cbuf.head = 0;
    }

    return read;
}

int is_circ_empty(void){
    if(cbuf.head == cbuf.tail){
        return 1;
    }

    return 0;
}

/**
 * This function frees all memory pages held by the module.
 */
void free_memory_pages(void) {
  page_node *curr;

  /* COMPLETE ME */
  /**
   * Loop through the entire page list {
   *   if (node has a page) {
   *     free the page
   *   }
   *   remove the node from the page list
   *   free the node
   * }
   * reset device data size, and num_pages
   */  

   page_node *tmp;

   list_for_each_entry_safe(curr,tmp,&asgn2_device.mem_list, list){
     if (curr->page != NULL){
        __free_page(curr->page);
     }
     list_del(&(curr->list));
     kfree(curr);
   }

   asgn2_device.data_size = 0;
   asgn2_device.num_pages = 0;

}


/**
 * This function opens the virtual disk, if it is opened in the write-only
 * mode, all memory pages will be freed.
 */
int asgn2_open(struct inode *inode, struct file *filp) {
   
   /* If the number of devices is already at maximum return -EBUSY */
   if (atomic_read(&asgn2_device.nprocs) == atomic_read(&asgn2_device.max_nprocs)){
     return -EBUSY;
   }
   
   /* Increment number of devices by 1 */
   atomic_inc(&asgn2_device.nprocs);

   /* If opened in write only mode call free all pages function */
   if ((filp->f_flags & O_ACCMODE) == O_WRONLY){
     free_memory_pages();
   }

   if(read_count == num_files){
       printk(KERN_INFO "(%s) No file to read. Sleeping...\n",MYDEV_NAME);
       wait_event_interruptible(my_queue, (read_count < num_files) && (atomic_read(&asgn2_device.read_lock) == 0));
       atomic_set(&asgn2_device.read_lock,1);
       printk(KERN_INFO "(%s) Waking up!\n",MYDEV_NAME);
   }

   /* Set EOF to 0 */
   eof = 0;

  return 0; /* success */
}


/**
 * This function releases the virtual disk, but nothing needs to be done
 * in this case. 
 */
int asgn2_release (struct inode *inode, struct file *filp) {
  /* COMPLETE ME */
  /**
   * decrement process count
   */

  atomic_dec(&asgn2_device.nprocs);

  eof = 1;
  atomic_set(&asgn2_device.read_lock,0);

  return 0;
}


/**
 * This function reads contents of the virtual disk and writes to the user 
 */
ssize_t asgn2_read(struct file *filp, char __user *buf, size_t count,
		 loff_t *f_pos) {
  size_t size_read = 0;     /* size read from virtual disk in this function */
  size_t begin_offset;      /* the offset from the beginning of a page to
			       start reading */
  int begin_page_no = *f_pos / PAGE_SIZE; /* the first page which contains
					     the requested data */
  int curr_page_no = 0;     /* the current page number */
  size_t curr_size_read;    /* size read from the virtual disk in this round */
  size_t size_to_be_read;   /* size to be read in the current round in 
			       while loop */

  struct list_head *ptr = &asgn2_device.mem_list;

  page_node *curr;

   /* Check if f_pos is beyond data_size if so return 0 */
   if(*f_pos >= asgn2_device.data_size){
        return 0;
   }

    /* Set the initial offset */
    begin_offset = *f_pos % PAGE_SIZE;
    if(*f_pos + count > asgn2_device.data_size){
        count = asgn2_device.data_size - *f_pos;
    }

    printk(KERN_INFO "Read count: %d",read_count);
    printk(KERN_INFO "Num files: %d",num_files);
    if(read_count == num_files){
        printk(KERN_INFO "(%s) Read all files",MYDEV_NAME);
        return 0;
    } else if(eof == 1){
        printk(KERN_INFO "(%s) Already read a file",MYDEV_NAME);
        return 0;
    } else if(read_count > 0){
        begin_offset = ((nulchars[read_count - 1] + 1) % PAGE_SIZE);
        begin_page_no = ((nulchars[read_count - 1] + 1) / PAGE_SIZE);
        printk(KERN_INFO "Not first read so off: %d", (int) begin_offset);
        printk(KERN_INFO "Not first read so begin no: %d", (int) begin_page_no);
    }

    /* For each page in the list */
    list_for_each_entry(curr, ptr, list){
        if(curr_page_no >= begin_page_no){
            do {
                size_to_be_read = min((int) count - (int) size_read, (int) PAGE_SIZE - (int) begin_offset);
                printk(KERN_INFO "NULCHARS %d",read_count);
                printk(KERN_INFO "NULCHARS[count] %d",(int) (nulchars[read_count] % PAGE_SIZE));
                printk(KERN_INFO "OFF %d",begin_offset);
                printk(KERN_INFO "SIZETBREAD %d", size_to_be_read);
                printk(KERN_INFO "BREAD - CARS: %d", size_to_be_read - ((int)(nulchars[read_count] % PAGE_SIZE) - (int) begin_offset));
                if(size_to_be_read > ((nulchars[read_count] % PAGE_SIZE) - (int) begin_offset)){
                    size_to_be_read = ((nulchars[read_count] % PAGE_SIZE) - (int) begin_offset);
                    /*
                    if(num_files > read_count + 1){
                        printk(KERN_INFO "Num greater tehn read upchars:%d",(int)(nulchars[read_count + 1] % PAGE_SIZE));
                        size_to_be_read = ((nulchars[read_count + 1] % PAGE_SIZE) - (nulchars[read_count] % PAGE_SIZE) - (int) begin_offset);
                    }
                    */
                }
                printk(KERN_INFO "suze to b bread: %d",size_to_be_read);
                /* Copy what we read to user space */
                curr_size_read = size_to_be_read - copy_to_user(buf + size_read,
                                page_address(curr->page) + begin_offset, size_to_be_read);
                printk(KERN_INFO "(Asgn2) Read %d from buffer", curr_size_read);
                if(curr_size_read > 0){
                    begin_offset += curr_size_read;
                    size_read += curr_size_read;
                    size_to_be_read -= curr_size_read;
                } else {
                    printk(KERN_ERR "Error in copy to user");
                    /* TODO comment this out */
                    return -1;
                }
                /* Repeat loop if we read in less than what we were supposed to */
                if(size_read >= count){
                    printk(KERN_ERR "read is more than or equal to count");
                    break;
                }
            } while(curr_size_read < size_to_be_read);
            begin_offset = 0;
        }
        curr_page_no++;
    }
    printk(KERN_ERR "Read through all the pages\n");

    *f_pos += size_read + 1;
    read_count++;

    return size_read;
}

/**
 * This function writes from the user buffer to the virtual disk of this
 * module expects a char
 */
ssize_t asgn2_write(char c) {
  int begin_offset = asgn2_device.data_size % PAGE_SIZE;

  struct list_head *ptr = asgn2_device.mem_list.prev;
  page_node *curr;

  if(c == '\0'){
      nulchars[num_files] = asgn2_device.data_size;
      printk(KERN_INFO "write nulchars %d",nulchars[num_files]);
      nulchars = krealloc(nulchars,(++num_files + 1) * sizeof(int), GFP_KERNEL);
      if(nulchars == NULL){
        printk("Error reallocating memory for array of nul chars");
        return -ENOMEM;
      }
      printk(KERN_INFO "(%s) Waking up any waiting processes",MYDEV_NAME);
      wake_up_interruptible(&my_queue);
  }
  
  curr = list_entry(ptr, page_node, list);

  if(begin_offset == 0){
      curr = kmalloc(sizeof(page_node), GFP_KERNEL);
      if(!curr){
          printk(KERN_ERR "Kmalloc failed for new list head\n");
          return -ENOMEM;
      }
      curr->page = alloc_page(GFP_KERNEL);
      if(curr->page == NULL){
          printk(KERN_WARNING "failed to alloc page");
          return -ENOMEM;
      }
      INIT_LIST_HEAD(&curr->list);
      list_add_tail(&curr->list,&(asgn2_device.mem_list));
      ++asgn2_device.num_pages;
      ptr = asgn2_device.mem_list.prev;
      printk(KERN_INFO "(Asgn2) Successfully added new page node");
  }

  memcpy(page_address(curr->page) + begin_offset,&c,1);

  asgn2_device.data_size += sizeof(c);

return sizeof(char);
}

#if 0
/**
 * This function writes from the user buffer to the virtual disk of this
 * module
 */
ssize_t asgn2_write(struct file *filp, const char __user *buf, size_t count,
		  loff_t *f_pos) {
  size_t orig_f_pos = *f_pos;  /* the original file position */
  size_t size_written = 0;  /* size written to virtual disk in this function */
  size_t begin_offset;      /* the offset from the beginning of a page to
			       start writing */
  int begin_page_no = *f_pos / PAGE_SIZE;  /* the first page this finction
					      should start writing to */

  int curr_page_no = 0;     /* the current page number */
  size_t curr_size_written; /* size written to virtual disk in this round */
  size_t size_to_be_written;  /* size to be read in the current round in 
				 while loop */
  
  struct list_head *ptr = asgn2_device.mem_list.next;
  /* struct list_head *ptr = &asgn2_device.mem_list;*/
  page_node *curr;

  /* COMPLETE ME */
  /**
   * Traverse the list until the first page reached, and add nodes if necessary
   *
   * Then write the data page by page, remember to handle the situation
   *   when copy_from_user() writes less than the amount you requested.
   *   a while loop / do-while loop is recommended to handle this situation. 
   */

   /* Set the initial offset */
   begin_offset = *f_pos % PAGE_SIZE;

   /* For each page in the list */
    while(size_written < count){
        curr = list_entry(ptr, page_node, list);

        if(ptr == &asgn2_device.mem_list){
           curr = kmalloc(sizeof(page_node), GFP_KERNEL);
            if(!curr){
                printk(KERN_ERR "Kmalloc failed for new list head\n");
               return -ENOMEM;
           }
           curr->page = alloc_page(GFP_KERNEL);
           if(curr->page == NULL){
                printk(KERN_WARNING "failed to alloc page");
                return -ENOMEM;
            }
           INIT_LIST_HEAD(&curr->list);
           list_add_tail(&curr->list,&(asgn2_device.mem_list));
           ++asgn2_device.num_pages;
           ptr = asgn2_device.mem_list.prev;
           printk(KERN_INFO "(Asgn2) Successfully added new page node");
        } else if(curr_page_no < begin_page_no){
           ptr = ptr->next;
           ++curr_page_no;
        } else {
            do{
                size_to_be_written = min((int) count - (int) size_written,(int) PAGE_SIZE - (int) begin_offset);
                curr_size_written = size_to_be_written - copy_from_user(
                    page_address(curr->page) + begin_offset,
                    &buf[size_written],size_to_be_written);
                printk(KERN_INFO "(Asgn2) Wrote %d to buffer",
                    curr_size_written);
                if(curr_size_written > 0){
                    begin_offset += curr_size_written;
                    *f_pos += curr_size_written;
                    size_written += curr_size_written;
                    size_to_be_written -= curr_size_written;
                } else {
                    printk(KERN_WARNING "Error in copy from user");
                }
            } while (size_to_be_written > 0);
            begin_offset = 0;
            ptr = ptr->next;
            curr_page_no++;
        }
   }

  asgn2_device.data_size = max(asgn2_device.data_size,
                               orig_f_pos + size_written);
  return size_written;
}
#endif /* 0 */

/**
 * Displays information about current status of the module,
 * which helps debugging.
 */
int asgn2_read_procmem(char *buf, char **start, off_t offset, int count,
		     int *eof, void *data) {
  /* stub */
  int result;

  /* COMPLETE ME */
  /**
   * use snprintf to print some info to buf, up to size count
   * set eof
   */

   result = snprintf(buf + offset,count,"Module Asgn2\n");
   result += snprintf(buf + offset + result,count - result,
        "Number of pages in use:%d\n",asgn2_device.num_pages);
   result += snprintf(buf + offset + result,count - result,
        "Datasize:%d\n",asgn2_device.data_size);
   result += snprintf(buf + offset + result,count - result,
        "Number of processes currently accessing module:%d\n",
        atomic_read(&asgn2_device.nprocs));
   result += snprintf(buf + offset + result,count - result,
        "Maximum number of processes allowed:%d\n",
        atomic_read(&asgn2_device.max_nprocs));

   *eof = 1;

  return result;
}

void do_tasklet(unsigned long data){
    char c = read_circ_buf();

    asgn2_write(c);
}

DECLARE_TASKLET(my_tasklet, do_tasklet, 0);

/* Function to hanlde the interrupt */
irqreturn_t my_handler(int irq, void *dev_id){
    char c = inb_p(parport);
    int a = 127;
    c = c & a;
    /*printk(KERN_INFO "(ASGN2) my handler %c", c);*/
    write_circ_buf(c);
    tasklet_schedule(&my_tasklet);
    return IRQ_HANDLED;
}

struct file_operations asgn2_fops = {
  .owner = THIS_MODULE,
  .read = asgn2_read,
  .open = asgn2_open,
  .release = asgn2_release,
};

/**
 * Initialise the module and create the master device
 */
int __init asgn2_init_module(void){
  int result; 

  /* COMPLETE ME */
  /**
   * set nprocs and max_nprocs of the device
   *
   * allocate major number
   * allocate cdev, and set ops and owner field 
   * add cdev
   * initialize the page list
   * create proc entries
   */

  int rv;
  int err = 0;
  void* pde;

   printk(KERN_INFO "Initialising Asgn2");
  /* Set nprocs */
  atomic_set(&asgn2_device.nprocs, 0);
  /* Set max_nprocs */
  atomic_set(&asgn2_device.max_nprocs, 10);

  asgn2_device.data_size = 0;
  asgn2_device.num_pages = 0;

  /* Dynamically allocate the device major number */
  rv = alloc_chrdev_region(&asgn2_device.dev,0,1,"Asgn2 Module");
  if(rv < 0){
    printk(KERN_WARNING "Device dynamic major number  allocation failed\n");
  }
  asgn2_major = MAJOR(asgn2_device.dev);
  asgn2_minor = MINOR(asgn2_device.dev);

  printk(KERN_INFO "(Asgn2) Major Num:%d, Minor Number:%d",asgn2_major,asgn2_minor);
  /* Allocate cdev */
  asgn2_device.cdev = cdev_alloc();
  if(asgn2_device.cdev == NULL){
    printk(KERN_WARNING "%s: cdev_alloc failed\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_class;
  }
  /* Initialise cdev */
  cdev_init(asgn2_device.cdev, &asgn2_fops);
  /* Set cdev owner */
  asgn2_device.cdev->owner = THIS_MODULE;
  /*add cdev */
  err = cdev_add(asgn2_device.cdev, asgn2_device.dev, 1);
  if(err < 0){
    printk(KERN_WARNING "%s: cdev_add failed\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_class;
  }

  /* Initialize the page list */
  INIT_LIST_HEAD(&asgn2_device.mem_list);

  /* Create proc entries */
  pde = create_proc_read_entry(MYDEV_NAME, O_RDONLY, NULL, asgn2_read_procmem, NULL);
  if(pde == NULL){
    printk(KERN_WARNING "%s: create_proc_read_entry failed\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_class;
  }

  if((err = check_region(parport,3)) < 0) return err;

  /* Get access to parallel port */
  if(request_region(parport,3,MYDEV_NAME) == NULL){
    printk(KERN_WARNING "%s: request_region failed\n", MYDEV_NAME);
    result = -1;
    goto fail_req_region;
  }

  /* Install interrupt handler */
  if(request_irq(7,my_handler,0,MYDEV_NAME,&asgn2_device) != 0){
    printk(KERN_WARNING "%s: request_irq failed\n", MYDEV_NAME);
    result = -1;
    goto fail_req_irq;
  }

  /* Enable the interrupt of the parallel port */
  outb_p(inb_p(0x378 + 2) | 0x10, 0x378 + 2);

  /* Initialise array of nul character pointers */
  nulchars = kmalloc(sizeof(int), GFP_KERNEL);
  if(nulchars == NULL){
    printk("Error reallocating memory for array of nul chars");
    return -ENOMEM;
  }

  asgn2_device.class = class_create(THIS_MODULE, MYDEV_NAME);
  if (IS_ERR(asgn2_device.class)) {
    printk(KERN_WARNING "%s: can't create udev class\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_class;
  }

  asgn2_device.device = device_create(asgn2_device.class, NULL, 
                                      asgn2_device.dev, "%s", MYDEV_NAME);
  if (IS_ERR(asgn2_device.device)) {
    printk(KERN_WARNING "%s: can't create udev device\n", MYDEV_NAME);
    result = -ENOMEM;
    goto fail_device;
  }
  
  printk(KERN_WARNING "set up udev entry\n");
  printk(KERN_WARNING "Hello world from %s\n", MYDEV_NAME);
  return 0;

fail_req_irq:
  //free_irq(7,&asgn2_device);
  release_region(0x378,3);
  goto fail_req_region;
    

fail_req_region:
  goto fail_class;


fail_class:
  cdev_del(asgn2_device.cdev);
  unregister_chrdev_region(asgn2_device.dev, 1);
  remove_proc_entry(MYDEV_NAME, NULL);

  return result;

  /* cleanup code called when any of the initialization steps fail */
fail_device:
   class_destroy(asgn2_device.class);

  /* COMPLETE ME */
  /* PLEASE PUT YOUR CLEANUP CODE HERE, IN REVERSE ORDER OF ALLOCATION */
  goto fail_class;

  return result;
}


/**
 * Finalise the module
 */
void __exit asgn2_exit_module(void){
  device_destroy(asgn2_device.class, asgn2_device.dev);
  class_destroy(asgn2_device.class);
  printk(KERN_WARNING "cleaned up udev entry\n");
  
  /* COMPLETE ME */
  /**
   * free all pages in the page list 
   * cleanup in reverse order
   */

  free_irq(7,&asgn2_device);
  release_region(0x378,3);
  remove_proc_entry(MYDEV_NAME, NULL);
  free_memory_pages();
  cdev_del(asgn2_device.cdev);
  unregister_chrdev_region(asgn2_device.dev, 1);

  printk(KERN_WARNING "Good bye from %s\n", MYDEV_NAME);
}


module_init(asgn2_init_module);
module_exit(asgn2_exit_module);


