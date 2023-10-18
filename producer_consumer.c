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

//when we made the module we set these params

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
static struct semaphore empty;  //when 0 cannot add any more
static struct semaphore full;   //when full is 0, cannot take any
static struct semaphore mutex; //lock

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
static struct task_struct **buffer; //array of task structs which is why double pointer, circular
static int head = 0; //to keep track of where we remove
static int tail = 0; //to keep track of where we add

static int totalConsumed = 0; //totalConsumed is number of processes consumed
static long totalProcessNanoseconds = 0; //keeps track of time of all consumed processes


// Module initializer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
static int __init producer_consumer_init(void){
    printk(KERN_INFO "producer_consumer module loaded\n"); //just left for testing

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
    sema_init(&empty, buffSize); //we make the empty the buffsize
    sema_init(&full, 0); //we make full 0, since we can add (no slots not full)
    sema_init(&mutex, 1); // binary semaphore set to 1 (unlocked)

    // 2. Initialize buffer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    buffer = kmalloc(buffSize * sizeof(struct task_struct *), GFP_KERNEL); //we allocate memory for the buffer, we use gfp_kernel flag(normal allocation), and we allocate size of task-struct pointer * buffersize
    if(!buffer){ //if we fail to allocate memory for buffer
        printk(KERN_ERR "Failed to allocate memory for buffer\n");
        return -1;
    }

    // 3. Create Consumers ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    if(cons > 0){
        printk(KERN_INFO "Creating consumer threads\n"); //we need to keep track of task structs to kill them later, cannot lose(floating memory)
        consumerThreads = kmalloc(cons * sizeof(struct task_struct *), GFP_KERNEL); //allocate the space for consumer thread
        if(!consumerThreads){ //failed to allocate consumer thread
            printk(KERN_ERR "Failed to allocate memory for consumer threads\n");
            return -1;
        }
        for(int i = 0; i < cons; i++){ //loop over all consumer threads, we create consumer threads
            char *name = kmalloc(20 * sizeof(char), GFP_KERNEL); //name consumer thread
            sprintf(name, "Consumer-%d", i+1); //put Consumer-number into the name of the current consumer
            consumerThreads[i] = kthread_run(kthread_consumer, name, "%s", name); //we run the created consumer thread(function(below),name of thread,string type,(name as argument passed into function))
            if (IS_ERR(consumerThreads[i])) {  //if kthread run fails then we cannot create thread consumer
                printk(KERN_INFO "ERROR: Cannot create thread Consumer\n");
                return PTR_ERR(consumerThreads[i]);;
            }
        }
    }

    // 4. Create producer ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
    if(prod == 1){ //we can only have one producer
        printk(KERN_INFO "Creating producer thread\n"); 
        producerThread = kthread_run(kthread_producer, NULL, "Producer-1"); //run the kthread_producer, NULL(default), "Producer-1" is name
        if (IS_ERR(producerThread)) { //if there is error
            printk(KERN_INFO "ERROR: Cannot create thread Producer-1\n");
            return PTR_ERR(producerThread);
        }
    }


    return 0;
}
//producer thread method
static int kthread_producer(void *arg){
    struct task_struct *task; //we create task pointer, stores the current task that our producer is looking at
    int count = 0; //intialize the count
    for_each_process(task){ //for each process running on the system
        if(task->cred->uid.val == uuid){ //if the task belongs to the user (uid = who owns the task)
            // add task to buffer
            // wait for empty using down_interruptible to allow for module to be unloaded
            //down_interruptiable means we can interrupt task and we can decrement
            if (down_interruptible(&empty)) break; // we move empty down, if it cannot aquire a lock it waits. Interrupted(true) --> breaks
            if (down_interruptible(&mutex)) break; // we move lock down, but cannot move we wait. Interrupted(true) --> breaks
            // Critical section ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
            buffer[tail] = task; // we add things to the tail
            // print with this format: [Producer-1] Produced Item#-12 at buffer index:1 for PID:136042
            count++; //increment the item count for print
            printk(KERN_INFO "[Producer-1] Produced Item#-%d at buffer index:%d for PID:%d\n", count, tail, task->pid); // we tell which item was produced where
            tail = (tail + 1) % buffSize; //this is how we keep track of where we are in the tail
            // end critical section ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
            up(&mutex); //increment mutex, release lock
            up(&full); //increment full, addded task (we tell consumer there is stuff to take)
            // signal the semaphore
        }
    }
    return 0;
}

static int kthread_consumer(void *arg){
    char *thread_name = (char *)arg; // we get the name name of thread we are using
    struct task_struct *task; //keep track of current task
    char timeFormat[9]; //string to keep track of time to convert into hours, minutes, seconds
    long taskTime; //time of task in nanoseconds (currentTime - taskTime)
    //var to store current time
    printk(KERN_INFO "Consumer thread created\n"); //for testing
    while (!kthread_should_stop()) //while kernel threasd is running, consumer runs indefintely(producer stops)
    {
        // wait for full using down_interruptible to allow for module to be unloaded
        if (down_interruptible(&full)) break; //break if interrupted, otherwise just wait
        if (down_interruptible(&mutex)) break; //break if interrupted, otherwise just wait
        // Critical section ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        task = buffer[head]; //we take from the head
        totalConsumed++; //increment number of processes consumed 
        taskTime = ktime_get_ns() - task->start_time; //currentTime - startTime
        totalProcessNanoseconds += taskTime; //we add the current taskTime to entire process time
        // convert taskTime to HH:MM:SS format
        sprintf(timeFormat, "%02ld:%02ld:%02ld", taskTime / 3600000000000, (taskTime / 60000000000) % 60, (taskTime / 1000000000) % 60); //formats the task time to put into hours,minutes,seconds
        /* print out task info using this format: 
        [<Consumer-thread-name>] Consumed Item#-<Item-Num> on buffer index: <buffer-index> PID:<PID consumed> Elapsed Time- <Elapsed time of the consumed PID in HH:MM:SS> */
        printk(KERN_INFO "[%s] Consumed Item#-%d on buffer index:%d PID:%d Elapsed Time- %s", thread_name, totalConsumed, head, task->pid, timeFormat); // we print the output
        head = (head + 1) % buffSize; //we move the head to the next slot in buffer
        // end critical section ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
        up(&mutex); //we release lock
        up(&empty); //signal empty slot
        // signal the semaphore
    }
    kfree(thread_name); //release thread name in memory 
    return 0;
}

// Module exit function ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
static void __exit producer_consumer_exit(void){
    char timeFormat[9]; //for our time format
    /* clean up tasks
        1. stop threads
        2. signal semaphores to wake up threads
        3. free memory
    */
    
    for(int i = 0; i < cons; i++){ //for all consumer threads, we stop them from running
        kthread_stop(consumerThreads[i]);
    }

    up(&mutex); //we release the lock
    up(&full); //we signal to the consumer can take stuff

    kfree(buffer); //we release the buffer in memory
    if(cons > 0) //we release consumer threads in memory
        kfree(consumerThreads);

    // print: The total elapsed time of all processes for UID <UID of the user> is <HH:MM:SS>
    sprintf(timeFormat, "%02ld:%02ld:%02ld", totalProcessNanoseconds / 3600000000000, (totalProcessNanoseconds / 60000000000) % 60, (totalProcessNanoseconds / 1000000000) % 60);
    printk(KERN_INFO "The total elapsed time of all processes for UID %u is %s\n", uuid, timeFormat);

}

// Call module initializer and exit function ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
module_init(producer_consumer_init);
module_exit(producer_consumer_exit);