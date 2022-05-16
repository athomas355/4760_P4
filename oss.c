#include <stdlib.h>     
#include <stdio.h>      
#include <stdbool.h>    
#include <stdint.h>     
#include <string.h>     
#include <unistd.h>     
#include <stdarg.h>     
#include <errno.h>      
#include <signal.h>     
#include <sys/ipc.h>    
#include <sys/msg.h>    
#include <sys/shm.h>    
#include <sys/sem.h>    
#include <sys/time.h>   
#include <sys/types.h>  
#include <sys/wait.h>   
#include <time.h>       
#include "shared.h"

//Constants
#define BUFFER_LENGTH 1024
#define MAX_PROCESS 18
#define TOTAL_PROCESS 100
#define FULL_QUANTUM 5000
#define HALF_QUANTUM FULL_QUANTUM / 2
#define QUAR_QUANTUM HALF_QUANTUM / 2
#define ALPHA 1.2
#define BETA 1.5

//global variables
static FILE *fpw = NULL;
static char *g_exe_name;
static key_t g_key;

static int g_message_queueid = -1;
struct Message g_master_message;
static struct Queue *highQueue;
static struct Queue *midQueue;
static struct Queue *lowQueue;
static int g_smclockid = -1;
static struct SharedClock *g_smclockptr = NULL;
static int g_sema_id = -1;
static struct sembuf g_sema_operation;
static int g_pcbt_smid = -1;
static struct ProcessControlBlock *g_pcbt_shmptr = NULL;

static int g_fork_number = 0;
static pid_t g_pid = -1;
static unsigned char g_bitmap[MAX_PROCESS];

void masterInterrupt(int seconds);
void masterHandler(int signum);
void exitHandler(int signum);
void timer(int seconds);
void finalize();
void discardSharedMemory(int shmid, void *shmaddr, char *shm_name , char *exe_name, char *process_type);
void cleanUp();
void semaLock(int sem_index);
void semaRelease(int sem_index);
void incShmclock();
struct Queue *createQueue();
struct QNode *newNode(int index);
void enQueue(struct Queue* q, int index);
struct QNode *deQueue(struct Queue *q);
bool isQueueEmpty(struct Queue *q);
struct UserProcess *initUserProcess(int index, pid_t pid);
void userToPCB(struct ProcessControlBlock *pcb, struct UserProcess *user);

int main(int argc, char *argv[]) {
	//declare variables
	g_exe_name = argv[0];
	srand(getpid());

	char log_file[256] = "log.dat";

	//parse command line
	int opt;
	while((opt = getopt(argc, argv, "hl:")) != -1) {
		switch(opt) {
			case 'h':
				printf("NAME:\n");
				printf("	%s - simulate the process scheduling part of an operating system with the use of message queues for synchronization.\n", g_exe_name);
				printf("\nUSAGE:\n");
				printf("	%s [-h] [-l logfile] [-t time].\n", g_exe_name);
				printf("\nDESCRIPTION:\n");
				printf("	-h          : print the help page and exit.\n");
				printf("	-l filename : the log file used (default log.dat).\n");
				exit(0);

			case 'l':
				strncpy(log_file, optarg, 255);
				printf("Your new log file is: %s\n", log_file);
				break;
				
			default:
				fprintf(stderr, "%s: please use \"-h\" option for more info.\n", g_exe_name);
				exit(EXIT_FAILURE);
		}
	}
	
	//Check for extra arguments
	if(optind < argc) {
		fprintf(stderr, "%s ERROR: extra arguments was given! Please use \"-h\" option for more info.\n", g_exe_name);
		exit(EXIT_FAILURE);
	}

	//logfile
	fpw = fopen(log_file, "w");
	if(fpw == NULL) {
		fprintf(stderr, "%s ERROR: unable to write the output file.\n", argv[0]);
		exit(EXIT_FAILURE);
	}

	//bitmap
	memset(g_bitmap, '\0', sizeof(g_bitmap));


	//message queue
	g_key = ftok("./oss.c", 1);
	g_message_queueid = msgget(g_key, IPC_CREAT | 0600);
	if(g_message_queueid < 0) {
		fprintf(stderr, "%s ERROR: could not allocate [queue] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	g_key = ftok("./oss.c", 2);
	g_smclockid = shmget(g_key, sizeof(struct SharedClock), IPC_CREAT | 0600);
	if(g_smclockid < 0) {
		fprintf(stderr, "%s ERROR: could not allocate [shmclock] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	//Attaching shared memory and check if can attach it. If not, delete the [shmclock] shared memory
	g_smclockptr = shmat(g_smclockid, NULL, 0);
	if(g_smclockptr == (void *)( -1 )) {
		fprintf(stderr, "%s ERROR: fail to attach [shmclock] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);	
	}

	//Initialize shared memory attribute of [shmclock]
	g_smclockptr->second = 0;
	g_smclockptr->nanosecond = 0;


	//semaphore
	g_key = ftok("./oss.c", 3);
	g_sema_id = semget(g_key, 3, IPC_CREAT | IPC_EXCL | 0600);
	if(g_sema_id == -1) {
		fprintf(stderr, "%s ERROR: failed to create a new private semaphore! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}
	
	//Initialize the semaphore(s) in our set to 1
	semctl(g_sema_id, 0, SETVAL, 1);	
	semctl(g_sema_id, 1, SETVAL, 1);	
	semctl(g_sema_id, 2, SETVAL, 1);	
	

	//process control block
	g_key = ftok("./oss.c", 4);
	size_t process_table_size = sizeof(struct ProcessControlBlock) * MAX_PROCESS;
	g_pcbt_smid = shmget(g_key, process_table_size, IPC_CREAT | 0600);
	if(g_pcbt_smid < 0) {
		fprintf(stderr, "%s ERROR: could not allocate [pcbt] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	//Attaching shared memory and check if can attach it. If not, delete the [pcbt] shared memory
	g_pcbt_shmptr = shmat(g_pcbt_smid, NULL, 0);
	if(g_pcbt_shmptr == (void *)( -1 )) {
		fprintf(stderr, "%s ERROR: fail to attach [pcbt] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);	
	}


	//queues
	highQueue = createQueue();
	midQueue = createQueue();
	lowQueue = createQueue();


	//signal handling
	masterInterrupt(3);

	int last_index = -1;
	bool is_open = false;
	int current_queue = 0;
	float mid_average_wait_time = 0.0;
	float low_average_wait_time = 0.0;
	while(1) {
		//Is this spot in the bit map open?
		is_open = false;
		int count_process = 0;
		while(1) {
			last_index = (last_index + 1) % MAX_PROCESS;
			uint32_t bit = g_bitmap[last_index / 8] & (1 << (last_index % 8));
			if(bit == 0) {
				is_open = true;
				break;
			}
			else {
				is_open = false;
			}

			if(count_process >= MAX_PROCESS - 1) {
				fprintf(stderr, "%s: bitmap is full (size: %d)\n", g_exe_name, MAX_PROCESS);
				fprintf(fpw, "%s: bitmap is full (size: %d)\n", g_exe_name, MAX_PROCESS);
				fflush(fpw);
				break;
			}
			count_process++;
		}

		if(is_open == true) {
			g_pid = fork();

			if(g_pid == -1) {
				fprintf(stderr, "%s (Master) ERROR: %s\n", g_exe_name, strerror(errno));
				finalize();
				cleanUp();
				exit(0);
			}
		
			if(g_pid == 0)  { //Child
				//Simple signal handler: this is for mis-synchronization when timer fire off
				signal(SIGUSR1, exitHandler);

				//Replaces the current running process with a new process (user)
				int exect_status = execl("./user", "./user", NULL);
				if(exect_status == -1) {	
					fprintf(stderr, "%s (Child) ERROR: execl fail to execute! Exiting...\n", g_exe_name);
				}
				
				finalize();
				cleanUp();
				exit(EXIT_FAILURE);
			}
			else  {	 //Parent
				//Increment the total number of fork in execution
				g_fork_number++;

				//Set the current index to one bit (meaning it is taken)
				g_bitmap[last_index / 8] |= (1 << (last_index % 8));

				//Initialize the user process and transfer the user process information to the process control block table
				struct UserProcess *user = initUserProcess(last_index, g_pid);
				userToPCB(&g_pcbt_shmptr[last_index], user);

				//Add the process to highest queue
				enQueue(highQueue, last_index);

				//Display creation time
				fprintf(stderr, "%s: generating process with PID (%d) [%d] and putting it in queue [%d] at time %d.%d\n", 
					g_exe_name, g_pcbt_shmptr[last_index].pidIndex, g_pcbt_shmptr[last_index].actualPid, 
					g_pcbt_shmptr[last_index].priority, g_smclockptr->second, g_smclockptr->nanosecond);
						
				fprintf(fpw, "%s: generating process with PID (%d) [%d] and putting it in queue [%d] at time %d.%d\n", 
					g_exe_name, g_pcbt_shmptr[last_index].pidIndex, g_pcbt_shmptr[last_index].actualPid, 
					g_pcbt_shmptr[last_index].priority, g_smclockptr->second, g_smclockptr->nanosecond);
				fflush(fpw);
			}
		}

		fprintf(stderr, "\n%s: working with queue [%d]\n", g_exe_name, current_queue);
		fprintf(fpw, "\n%s: working with queue [%d]\n", g_exe_name, current_queue);
		fflush(fpw);
		struct QNode next;
		if(current_queue == 0) {
			next.next = highQueue->front;
		}
		else if(current_queue == 1) {
			next.next = midQueue->front;
		}
		else if(current_queue == 2) {
			next.next = lowQueue->front;
		}

		int total_process = 0;
		float total_wait_time = 0.0;
		struct Queue *tempQueue = createQueue();
		while(next.next != NULL) {
			total_process++;

			//- CRITICAL SECTION -//
			incShmclock();

			//Sending a message to a specific child to tell him it is his turn
			int c_index = next.next->index;
			g_master_message.mtype = g_pcbt_shmptr[c_index].actualPid;
			g_master_message.index = c_index;
			g_master_message.childPid = g_pcbt_shmptr[c_index].actualPid;
			g_master_message.priority = g_pcbt_shmptr[c_index].priority = current_queue;
			msgsnd(g_message_queueid, &g_master_message, (sizeof(struct Message) - sizeof(long)), 0);
			fprintf(stderr, "%s: signaling process with PID (%d) [%d] from queue [%d] to dispatch\n",
				g_exe_name, g_master_message.index, g_master_message.childPid, current_queue);

			fprintf(fpw, "%s: signaling process with PID (%d) [%d] from queue [%d] to dispatch\n",
				g_exe_name, g_master_message.index, g_master_message.childPid, current_queue);
			fflush(fpw);

			//- CRITICAL SECTION -//
			incShmclock();

			//Getting dispatching time from the child
			msgrcv(g_message_queueid, &g_master_message, (sizeof(struct Message) - sizeof(long)), 1, 0);
			
			fprintf(stderr, "%s: dispatching process with PID (%d) [%d] from queue [%d] at time %d.%d\n",
				g_exe_name, g_master_message.index, g_master_message.childPid, current_queue, 
				g_master_message.second, g_master_message.nanosecond);

			fprintf(fpw, "%s: dispatching process with PID (%d) [%d] from queue [%d] at time %d.%d\n",
				g_exe_name, g_master_message.index, g_master_message.childPid, current_queue, 
				g_master_message.second, g_master_message.nanosecond);
			fflush(fpw);

			//- CRITICAL SECTION -//
			incShmclock();

			//Calculate the total time of the dispatch
			int difference_nanosecond = g_smclockptr->nanosecond - g_master_message.nanosecond;
			fprintf(stderr, "%s: total time of this dispatch was %d nanoseconds\n", g_exe_name, difference_nanosecond);

			fprintf(fpw, "%s: total time of this dispatch was %d nanoseconds\n", g_exe_name, difference_nanosecond);
			fflush(fpw);

			while(1) {
				//- CRITICAL SECTION -//
				incShmclock();
				
				int result = msgrcv(g_message_queueid, &g_master_message, (sizeof(struct Message) - sizeof(long)), 1, IPC_NOWAIT);
				if(result != -1) {
					fprintf(stderr, "%s: receiving that process with PID (%d) [%d] ran for %d nanoseconds\n", 
						g_exe_name, g_master_message.index, g_master_message.childPid, g_master_message.burstTime);

					fprintf(fpw, "%s: receiving that process with PID (%d) [%d] ran for %d nanoseconds\n", 
						g_exe_name, g_master_message.index, g_master_message.childPid, g_master_message.burstTime);
					fflush(fpw);
					break;
				}
			}

			//- CRITICAL SECTION -//
			incShmclock();
			
			msgrcv(g_message_queueid, &g_master_message, (sizeof(struct Message) - sizeof(long)), 1, 0);
			g_pcbt_shmptr[c_index].lastBurst = g_master_message.burstTime;
			g_pcbt_shmptr[c_index].totalBurst += g_master_message.burstTime;
			g_pcbt_shmptr[c_index].totalSystemSecond = g_master_message.spawnSecond;
			g_pcbt_shmptr[c_index].totalSystemNanosecond = g_master_message.spawnNanosecond;
			g_pcbt_shmptr[c_index].totalWaitSecond += g_master_message.waitSecond;
			g_pcbt_shmptr[c_index].totalWaitNanosecond += g_master_message.waitNanosecond;
			

			if(g_master_message.flag == 0) {
				fprintf(stderr, "%s: process with PID (%d) [%d] has finish running at my time %d.%d\n\tLast Burst (nano): %d\n\tTotal Burst (nano): %d\n\tTotal CPU Time (second.nano): %d.%d\n\tTotal Wait Time (second.nano): %d.%d\n",
					g_exe_name, g_master_message.index, g_master_message.childPid, g_smclockptr->second, g_smclockptr->nanosecond,
					g_pcbt_shmptr[c_index].lastBurst, g_pcbt_shmptr[c_index].totalBurst, g_pcbt_shmptr[c_index].totalSystemSecond, 
					g_pcbt_shmptr[c_index].totalSystemNanosecond, g_pcbt_shmptr[c_index].totalWaitSecond, g_pcbt_shmptr[c_index].totalWaitNanosecond);

				fprintf(fpw, "%s: process with PID (%d) [%d] has finish running at my time %d.%d\n\tLast Burst (nano): %d\n\tTotal Burst (nano): %d\n\tTotal CPU Time (second.nano): %d.%d\n\tTotal Wait Time (second.nano): %d.%d\n",
					g_exe_name, g_master_message.index, g_master_message.childPid, g_smclockptr->second, g_smclockptr->nanosecond,
					g_pcbt_shmptr[c_index].lastBurst, g_pcbt_shmptr[c_index].totalBurst, g_pcbt_shmptr[c_index].totalSystemSecond, 
					g_pcbt_shmptr[c_index].totalSystemNanosecond, g_pcbt_shmptr[c_index].totalWaitSecond, g_pcbt_shmptr[c_index].totalWaitNanosecond);
				fflush(fpw);

				total_wait_time += g_master_message.waitTime;
			}
			else {
				if(current_queue == 0) {
					if(g_master_message.waitTime > (ALPHA * mid_average_wait_time)) {
						fprintf(stderr, "%s: putting process with PID (%d) [%d] to queue [1]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);

						fprintf(fpw, "%s: putting process with PID (%d) [%d] to queue [1]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);
						fflush(fpw);

						enQueue(midQueue, c_index);
						g_pcbt_shmptr[c_index].priority = 1;
					}
					else {
						fprintf(stderr, "%s: not using its entire time quantum. Putting process with PID (%d) [%d] to queue [0]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);

						fprintf(fpw, "%s: not using its entire time quantum. Putting process with PID (%d) [%d] to queue [0]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);
						fflush(fpw);

						enQueue(tempQueue, c_index);
						g_pcbt_shmptr[c_index].priority = 0;
					}
				}
				else if(current_queue == 1) {
					if(g_master_message.waitTime > (BETA * low_average_wait_time)) {
						fprintf(stderr, "%s: putting process with PID (%d) [%d] to queue [2]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);

						fprintf(fpw, "%s: putting process with PID (%d) [%d] to queue [2]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);
						fflush(fpw);

						enQueue(lowQueue, c_index);
						g_pcbt_shmptr[c_index].priority = 2;
					}
					else {
						fprintf(stderr, "%s: not using its entire time quantum. Putting process with PID (%d) [%d] to queue [1]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);

						fprintf(fpw, "%s: not using its entire time quantum. Putting process with PID (%d) [%d] to queue [1]\n",
							g_exe_name, g_master_message.index, g_master_message.childPid);
						fflush(fpw);

						enQueue(tempQueue, c_index);
						g_pcbt_shmptr[c_index].priority = 1;
					}
				}
				else if(current_queue == 2){
					fprintf(stderr, "%s: putting process with PID (%d) [%d] to queue [2]\n",
						g_exe_name, g_master_message.index, g_master_message.childPid);

					fprintf(fpw, "%s: putting process with PID (%d) [%d] to queue [2]\n",
						g_exe_name, g_master_message.index, g_master_message.childPid);
					fflush(fpw);

					enQueue(tempQueue, c_index);
					g_pcbt_shmptr[c_index].priority = 2;
				}

				total_wait_time += g_master_message.waitTime;
			}

			if(next.next->next != NULL) {
				next.next = next.next->next;
			}
			else {
				next.next = NULL;
			}
		}

		if(total_process == 0) {
			total_process = 1;
		}

		if(current_queue == 1) {
			mid_average_wait_time = (total_wait_time / total_process);
		}
		else if(current_queue == 2) {
			low_average_wait_time = (total_wait_time / total_process);
		}


		if(current_queue == 0) {
			while(!isQueueEmpty(highQueue)) {
				deQueue(highQueue);
			}
			while(!isQueueEmpty(tempQueue)) {
				int i = tempQueue->front->index;
				enQueue(highQueue, i);
				deQueue(tempQueue);
			}
		}
		else if(current_queue == 1) {
			while(!isQueueEmpty(midQueue)) {
				deQueue(midQueue);
			}
			while(!isQueueEmpty(tempQueue)) {
				int i = tempQueue->front->index;
				enQueue(midQueue, i);
				deQueue(tempQueue);
			}
		}
		else if(current_queue == 2) {
			while(!isQueueEmpty(lowQueue)) {
				deQueue(lowQueue);
			}
			while(!isQueueEmpty(tempQueue)) {
				int i = tempQueue->front->index;
				enQueue(lowQueue, i);
				deQueue(tempQueue);
			}
		}
		free(tempQueue);

		current_queue = (current_queue + 1) % 3;

		//- CRITICAL SECTION -//
		incShmclock();

		//--------------------------------------------------
		//Check to see if a child exit, wait no bound (return immediately if no child has exit)
		int child_status = 0;
		pid_t child_pid = waitpid(-1, &child_status, WNOHANG);

		//Set the return index bit back to zero (which mean there is a spot open for this specific index in the bitmap)
		if(child_pid > 0) {
			int return_index = WEXITSTATUS(child_status);
			g_bitmap[return_index / 8] &= ~(1 << (return_index % 8));
		}

	
		if(g_fork_number >= TOTAL_PROCESS) {
			timer(0);
			masterHandler(0);
		}
	}

	return EXIT_SUCCESS; 
}

void masterInterrupt(int seconds) {
	timer(seconds);
	signal(SIGUSR1, SIG_IGN);

	struct sigaction sa1;
	sigemptyset(&sa1.sa_mask);
	sa1.sa_handler = &masterHandler;
	sa1.sa_flags = SA_RESTART;
	if(sigaction(SIGALRM, &sa1, NULL) == -1) {
		perror("ERROR");
	}

	struct sigaction sa2;
	sigemptyset(&sa2.sa_mask);
	sa2.sa_handler = &masterHandler;
	sa2.sa_flags = SA_RESTART;
	if(sigaction(SIGINT, &sa2, NULL) == -1) {
		perror("ERROR");
	}
}
void masterHandler(int signum) {
	finalize();

	//Print out basic statistic
	fprintf(stderr, "- Master PID: %d\n", getpid());
	fprintf(stderr, "- Number of forking during this execution: %d\n", g_fork_number);
	fprintf(stderr, "- Final simulation time of this execution: %d.%d\n", g_smclockptr->second, g_smclockptr->nanosecond);

	double throughput = g_smclockptr->second + ((double)g_smclockptr->nanosecond / 10000000000.0);
	throughput = (double)g_fork_number / throughput;
	fprintf(stderr, "- Throughput Time: %f (process per time)\n", throughput);

	cleanUp();

	//Final check for closing log file
	if(fpw != NULL) {
		fclose(fpw);
		fpw = NULL;
	}

	exit(EXIT_SUCCESS); 
}
void exitHandler(int signum) {
	printf("%d: Terminated!\n", getpid());
	exit(EXIT_SUCCESS);
}

void timer(int seconds) {
	//Timers decrement from it_value to zero, generate a signal, and reset to it_interval.
	//A timer which is set to zero (it_value is zero or the timer expires and it_interval is zero) stops.
	struct itimerval value;
	value.it_value.tv_sec = seconds;
	value.it_value.tv_usec = 0;
	value.it_interval.tv_sec = 0;
	value.it_interval.tv_usec = 0;
	
	if(setitimer(ITIMER_REAL, &value, NULL) == -1) {
		perror("ERROR");
	}
}

void finalize() {
	fprintf(stderr, "\nLimitation has reached! Invoking termination...\n");
	kill(0, SIGUSR1);
	pid_t p = 0;
	while(p >= 0) {
		p = waitpid(-1, NULL, WNOHANG);
	}
}

void discardSharedMemory(int shmid, void *shmaddr, char *shm_name , char *exe_name, char *process_type)
{
	if(shmaddr != NULL) {
		if((shmdt(shmaddr)) < 0) {
			fprintf(stderr, "%s (%s) ERROR: could not detach [%s] shared memory!\n", exe_name, process_type, shm_name);
		}
	}
	
	//Deleting Shared Memory
	if(shmid > 0) {
		if((shmctl(shmid, IPC_RMID, NULL)) < 0) {
			fprintf(stderr, "%s (%s) ERROR: could not delete [%s] shared memory! Exiting...\n", exe_name, process_type, shm_name);
		}
	}
}

void cleanUp() {
	//Delete [queue] shared memory
	if(g_message_queueid > 0) {
		msgctl(g_message_queueid, IPC_RMID, NULL);
	}

	//Release and delete [smclock] shared memory
	discardSharedMemory(g_smclockid, g_smclockptr, "shmclock", g_exe_name, "Master");

	//Delete semaphore
	if(g_sema_id > 0) {
		semctl(g_sema_id, 0, IPC_RMID);
	}

	//Release and delete [pcbt] shared memory
	discardSharedMemory(g_pcbt_smid, g_pcbt_shmptr, "pcbt", g_exe_name, "Master");
}

void semaLock(int sema_index) {
	g_sema_operation.sem_num = sema_index;
	g_sema_operation.sem_op = -1;
	g_sema_operation.sem_flg = 0;
	semop(g_sema_id, &g_sema_operation, 1);
}

void semaRelease(int sem_index) {	
	g_sema_operation.sem_num = sem_index;
	g_sema_operation.sem_op = 1;
	g_sema_operation.sem_flg = 0;
	semop(g_sema_id, &g_sema_operation, 1);
}

void incShmclock() {
	semaLock(0);

	g_smclockptr->nanosecond += rand() % 1000 + 1;
	if(g_smclockptr->nanosecond >= 1000000000) {
		g_smclockptr->second++;
		g_smclockptr->nanosecond = 1000000000 - g_smclockptr->nanosecond;
	}

	semaRelease(0);
}

struct Queue *createQueue() {
	struct Queue *q = (struct Queue *)malloc(sizeof(struct Queue));
	q->front = NULL;
	q->rear = NULL;
	return q;
}

struct QNode *newNode(int index) { 
    struct QNode *temp = (struct QNode *)malloc(sizeof(struct QNode));
    temp->index = index;
    temp->next = NULL;
    return temp;
} 

void enQueue(struct Queue *q, int index) { 
	//Create a new LL node
	struct QNode *temp = newNode(index);

	//If queue is empty, then new node is front and rear both
	if(q->rear == NULL) {
		q->front = q->rear = temp;
		return;
	}

	//Add the new node at the end of queue and change rear 
	q->rear->next = temp;
	q->rear = temp;
}

struct QNode *deQueue(struct Queue *q)  {
	//If queue is empty, return NULL
	if(q->front == NULL) {
		return NULL;
	}

	//Store previous front and move front one node ahead
	struct QNode *temp = q->front;
	free(temp);
	q->front = q->front->next;

	//If front becomes NULL, then change rear also as NULL
	if (q->front == NULL) {
		q->rear = NULL;
	}

	return temp;
} 

bool isQueueEmpty(struct Queue *q) {
	if(q->rear == NULL) {
		return true;
	}
	else {
		return false;
	}
}

struct UserProcess *initUserProcess(int index, pid_t pid) {
	struct UserProcess *user = (struct UserProcess *)malloc(sizeof(struct UserProcess));
	user->index = index;
	user->actualPid = pid;
	user->priority = 0;
	return user;
}

void userToPCB(struct ProcessControlBlock *pcb, struct UserProcess *user) {
	pcb->pidIndex = user->index;
	pcb->actualPid = user->actualPid;
	pcb->priority = user->priority;
	pcb->lastBurst = 0;
	pcb->totalBurst = 0;
	pcb->totalSystemSecond = 0;
	pcb->totalSystemNanosecond = 0;
	pcb->totalWaitSecond = 0;
	pcb->totalWaitNanosecond = 0;
}

