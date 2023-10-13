#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/semaphore.h>
#include <linux/kthread.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/ktime.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/slab.h>

MODULE_LICENSE("GPL");
MODULE_AUTHOR("B.O.S.N.");
MODULE_DESCRIPTION("Solving the Producer Consumer problem using semaphores");

/* Set up parameters here ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    Reference: https://www.hitchhikersguidetolearning.com/2018/10/24/passing-data-to-a-kernel-module-module_param/

    Parameters:
        buffSize: Size of the buffer
        prod: Number of producers (0 o 1)
        cons: Number of consumers (a non-negative integer)
        uuid: the uuid of the user

    I'm not 100% sure what the permissions should be so I will try 0644 since that's
    what the article uses.

    we can use module_param(name, type, permission) macro to define the parameters
    and MODULE_PARM_DESC(name, description) macro to define the description of the parameter

    ktime_get_ns() returns the time in nanoseconds
    https://docs.kernel.org/core-api/timekeeping.html
    to get time task started, use task_struct->start_time
 */

// Buffer size
static int buffSize = 10;
module_param(buffSize, int, 0644);
MODULE_PARM_DESC(buffSize, "Size of the buffer");

// Number of producers
static int prod = 1;
module_param(prod, int, 0644);
MODULE_PARM_DESC(prod, "Number of producers (0 or 1)");

// Number of consumers
static int cons = 1;
module_param(cons, int, 0644);
MODULE_PARM_DESC(cons, "Number of consumers (a non-negative integer)");

// User UUID
// not sure of the type UUID should be but I will try unsigned int
// https://en.wikipedia.org/wiki/User_identifier?#Type
static uint uuid = 0;
module_param(uuid, uint, 0644);
MODULE_PARM_DESC(uuid, "The uuid of the user");

// Semaphores ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
static struct semaphore empty;
static struct semaphore full;
static struct semaphore mutex;

// Threads ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
static int kthread_producer(void *arg);
static int kthread_consumer(void *arg);

// Thread variables ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
static struct task_struct *producerThread = NULL;
static struct task_struct **consumerThreads = NULL;


/* Buffer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    I'm thinking of using a circular buffer by using an array of
    size buffSize that stores task_struct pointers and two integers
    that indicate the head and tail of the buffer.

    task_struct reference:
        https://docs.huihoo.com/doxygen/linux/kernel/3.7/structtask__struct.html
        task_struct->cred.uid.val is the user id of the task
        task_struct->pid is the process id of the task
        task_struct->start_time is the time the task started

*/
static struct task_struct **buffer;
static int head = 0;
static int tail = 0;

static int totalConsumed = 0;
static long totalProcessNanoseconds = 0;


// Module initializer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
static int __init producer_consumer_init(void){
    printk(KERN_INFO "producer_consumer module loaded\n"); 

    /* printed out the parameters for testing
    printk(KERN_INFO "buffSize: %d\n", buffSize);
    printk(KERN_INFO "prod: %d\n", prod);
    printk(KERN_INFO "cons: %d\n", cons);
    printk(KERN_INFO "uuid: %u\n", uuid);

    Testing passed!
    */

    //Not required, but I am going to check some edge cases 
    //and throw an error if they occur
    if(buffSize < 1){
        printk(KERN_ERR "buffSize must be greater than 0\n");
        return -1;
    }
    if(prod < 0 || prod > 1){
        printk(KERN_ERR "prod must be 0 or 1\n");
        return -1;
    }
    if(cons < 0){
        printk(KERN_ERR "cons must be greater than or equal to 0\n");
        return -1;
    }
    
    /* Program Flow
        1. Initialize semaphores
        2. Initialize buffer
        3. Create consumers
        4. Create producers
        5. Wait for exit
        6. end all threads and free memory
    */

    // 1. Initialize semaphores ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    sema_init(&empty, buffSize);
    sema_init(&full, 0);
    sema_init(&mutex, 1); // binary semaphore set to 1 (unlocked)

    // 2. Initialize buffer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    buffer = kmalloc(buffSize * sizeof(struct task_struct *), GFP_KERNEL);
    if(!buffer){
        printk(KERN_ERR "Failed to allocate memory for buffer\n");
        return -1;
    }

    // 3. Create Consumers ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    if(cons > 0){
        printk(KERN_INFO "Creating consumer threads\n");
        consumerThreads = kmalloc(cons * sizeof(struct task_struct *), GFP_KERNEL);
        if(!consumerThreads){
            printk(KERN_ERR "Failed to allocate memory for consumer threads\n");
            return -1;
        }
        for(int i = 0; i < cons; i++){
            char *name = kmalloc(20 * sizeof(char), GFP_KERNEL);
            sprintf(name, "Consumer-%d", i+1);
            consumerThreads[i] = kthread_run(kthread_consumer, name, "%s", name);
            if (IS_ERR(consumerThreads[i])) {
                printk(KERN_INFO "ERROR: Cannot create thread Consumer\n");
                return PTR_ERR(consumerThreads[i]);;
            }
        }
    }

    // 4. Create producer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    if(prod == 1){
        printk(KERN_INFO "Creating producer thread\n");
        producerThread = kthread_run(kthread_producer, NULL, "Producer-1");
        if (IS_ERR(producerThread)) {
            printk(KERN_INFO "ERROR: Cannot create thread Producer-1\n");
            return PTR_ERR(producerThread);
        }
    }


    return 0;
}

static int kthread_producer(void *arg){
    struct task_struct *task;
    int count = 0;
    for_each_process(task){
        if(task->cred->uid.val == uuid){
            // add task to buffer
            // wait for empty using down_interruptible to allow for module to be unloaded
            if (down_interruptible(&empty)) break;
            if (down_interruptible(&mutex)) break;
            // Critical section ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
            buffer[tail] = task;
            // print with this format: [Producer-1] Produced Item#-12 at buffer index:1 for PID:136042
            count++;
            printk(KERN_INFO "[Producer-1] Produced Item#-%d at buffer index:%d for PID:%d\n", count, tail, task->pid);
            tail = (tail + 1) % buffSize;
            // end critical section ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
            up(&mutex);
            up(&full);
            // signal the semaphore
        }
    }
    return 0;
}

static int kthread_consumer(void *arg){
    char *thread_name = (char *)arg;
    struct task_struct *task;
    char timeFormat[9];
    long taskTime;
    //var to store current time
    printk(KERN_INFO "Consumer thread created\n");
    while (!kthread_should_stop())
    {
        // wait for full using down_interruptible to allow for module to be unloaded
        if (down_interruptible(&full)) break;
        if (down_interruptible(&mutex)) break;
        // Critical section ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        task = buffer[head];
        totalConsumed++;
        taskTime = ktime_get_ns() - task->start_time;
        totalProcessNanoseconds += taskTime;
        // convert taskTime to HH:MM:SS format
        sprintf(timeFormat, "%02ld:%02ld:%02ld", taskTime / 3600000000000, (taskTime / 60000000000) % 60, (taskTime / 1000000000) % 60);
        /* print out task info using this format: 
        [<Consumer-thread-name>] Consumed Item#-<Item-Num> on buffer index: <buffer-index> PID:<PID consumed> Elapsed Time- <Elapsed time of the consumed PID in HH:MM:SS> */
        printk(KERN_INFO "[%s] Consumed Item#-%d on buffer index:%d PID:%d Elapsed Time- %s", thread_name, totalConsumed, head, task->pid, timeFormat);
        head = (head + 1) % buffSize;
        // end critical section ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        up(&mutex);
        up(&empty);
        // signal the semaphore
    }
    kfree(thread_name);
    return 0;
}

// Module exit function ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
static void __exit producer_consumer_exit(void){
    char timeFormat[9];
    /* clean up tasks
        1. stop threads
        2. signal semaphores to wake up threads
        3. free memory
    */
    
    for(int i = 0; i < cons; i++){
        kthread_stop(consumerThreads[i]);
    }

    up(&mutex);
    up(&full);

    kfree(buffer);
    if(cons > 0)
        kfree(consumerThreads);

    // print: The total elapsed time of all processes for UID <UID of the user> is <HH:MM:SS>
    sprintf(timeFormat, "%02ld:%02ld:%02ld", totalProcessNanoseconds / 3600000000000, (totalProcessNanoseconds / 60000000000) % 60, (totalProcessNanoseconds / 1000000000) % 60);
    printk(KERN_INFO "The total elapsed time of all processes for UID %u is %s\n", uuid, timeFormat);

}

// Call module initializer and exit function ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
module_init(producer_consumer_init);
module_exit(producer_consumer_exit);