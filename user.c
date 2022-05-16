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

//Constants
#define BUFFER_LENGTH 1024
#define MAX_PROCESS 18
#define TOTAL_PROCESS 100
#define FULL_QUANTUM 5000
#define HALF_QUANTUM FULL_QUANTUM / 2
#define QUAR_QUANTUM HALF_QUANTUM / 2
#define ALPHA 1.2
#define BETA 1.5
#include "shared.h"

//global variables
static char *g_exe_name;
static key_t g_key;

static int g_message_queueid = -1;
static struct Message g_user_message;
static int g_smclockid = -1;
static struct SharedClock *g_smclockptr = NULL;
static int g_sema_id = -1;
static struct sembuf g_sema_operation;
static int g_pcbt_smid = -1;
static struct ProcessControlBlock *g_pcbt_smptr = NULL;

void processInterrupt();
void processHandler(int signum);
void resumeHandler(int signum);
void discardSharedMemory(void *shmaddr, char *shm_name , char *exe_name, char *process_type);
void cleanUp();
void semaLock(int sema_index);
void semaRelease(int sema_index);

int main(int argc, char *argv[]) 
{
	//signal handling
	processInterrupt();


	g_exe_name = argv[0];
	srand(getpid());

	//message queues
	g_key = ftok("./oss.c", 1);
	g_message_queueid = msgget(g_key, 0600);
	if(g_message_queueid < 0)
	{
		fprintf(stderr, "%s ERROR: could not get [queue] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	//shared memory
	g_key = ftok("./oss.c", 2);
	g_smclockid = shmget(g_key, sizeof(struct SharedClock), 0600);
	if(g_smclockid < 0)
	{
		fprintf(stderr, "%s ERROR: could not get [smclock] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	//Attaching shared memory and check if can attach it. 
	g_smclockptr = shmat(g_smclockid, NULL, 0);
	if(g_smclockptr == (void *)( -1 ))
	{
		fprintf(stderr, "%s ERROR: fail to attach [smclock] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);	
	}


	//semaphores
	g_key = ftok("./oss.c", 3);
	g_sema_id = semget(g_key, 3, 0600);
	if(g_sema_id == -1)
	{
		fprintf(stderr, "%s ERROR: failed to create a new private semaphore! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}


	//process control block table
	g_key = ftok("./oss.c", 4);
	size_t process_table_size = sizeof(struct ProcessControlBlock) * MAX_PROCESS;
	g_pcbt_smid = shmget(g_key, process_table_size, 0600);
	if(g_pcbt_smid < 0)
	{
		fprintf(stderr, "%s ERROR: could not allocate [pcbt] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);
	}

	//Attaching shared memory and check if can attach it.
	g_pcbt_smptr = shmat(g_pcbt_smid, NULL, 0);
	if(g_pcbt_smptr == (void *)( -1 ))
	{
		fprintf(stderr, "%s ERROR: fail to attach [pcbt] shared memory! Exiting...\n", g_exe_name);
		cleanUp();
		exit(EXIT_FAILURE);	
	}

	int index = -1;
	int priority = -1;
	unsigned int spawn_second = g_smclockptr->second;
	unsigned int spawn_nanosecond = g_smclockptr->nanosecond;
	unsigned int wait_second = g_smclockptr->second;
	unsigned int wait_nanosecond = g_smclockptr->nanosecond;
	int quantum = 0;
	unsigned int burst_time = 0;
	long duration = 0;

	while(1) 
	{
		bool is_terminate = false;
		unsigned long event_time = 0;

		while(1)
		{
			int result = msgrcv(g_message_queueid, &g_user_message, (sizeof(struct Message) - sizeof(long)), getpid(), IPC_NOWAIT);
			if(result != -1)
			{
				if(g_user_message.childPid == getpid())
				{
					//Getting the information from the master message
					index = g_user_message.index;
					priority = g_user_message.priority;

					//Decide what quantum to use base on given priority
					if(priority == 0)
					{
						quantum = FULL_QUANTUM;
					}
					else if(priority == 1)
					{
						quantum = HALF_QUANTUM;
					}
					else if(priority == 2)
					{
						quantum = QUAR_QUANTUM;
					}

					int r = rand() % 2 + 0;
					if(r == 1)
					{
						burst_time = quantum;
					}
					else
					{
						burst_time = rand() % quantum + 0;
					}

					int scheduling_choice = rand() % 4 + 0;
					if(scheduling_choice == 0)
					{
						event_time = 0;
						duration = 0;
						is_terminate = true;
					}
					else if(scheduling_choice == 1)
					{
						event_time = 0;
						duration = burst_time;
					}
					else if(scheduling_choice == 2)
					{
						unsigned int r_second = 0 * 1000000000;
						unsigned int r_nanosecond = rand() % 1001 + 0;

						event_time = r_second + r_nanosecond;
						duration = burst_time + event_time;
					}
					else if(scheduling_choice == 3)
					{
						event_time = 0;
						double p = (rand() % 99 + 1) / 100;
						duration = burst_time - (p * burst_time);
					}	

					//Send a message to master about my dispatch time
					g_user_message.mtype = 1;
					g_user_message.index = index;
					g_user_message.childPid = getpid();
					g_user_message.second = g_smclockptr->second;
					g_user_message.nanosecond = g_smclockptr->nanosecond;
					msgsnd(g_message_queueid, &g_user_message, (sizeof(struct Message) - sizeof(long)), 0);
					break;
				}
			}
		}

		unsigned int c_nanosecond = g_smclockptr->nanosecond;
		while(1)
		{
			duration -= g_smclockptr->nanosecond - c_nanosecond;

			if(duration <= 0)
			{
				g_user_message.mtype = 1;
				g_user_message.index = index;
				g_user_message.childPid = getpid();
				g_user_message.burstTime = burst_time;
				msgsnd(g_message_queueid, &g_user_message, (sizeof(struct Message) - sizeof(long)), 0);
				break;
			}
		}

		g_user_message.mtype = 1;
		if(is_terminate)
		{
			g_user_message.flag = 0;
		}
		else
		{
			g_user_message.flag = 1;

		}
		g_user_message.index = index;
		g_user_message.childPid = getpid();
		g_user_message.burstTime = burst_time;

		unsigned int elapse_second = g_smclockptr->second - spawn_second;
		unsigned int elapse_nanosecond = g_smclockptr->nanosecond + spawn_nanosecond;
		while(elapse_nanosecond >= 1000000000)
		{
			elapse_second++;
			elapse_nanosecond = 1000000000 - elapse_nanosecond;
		}
		g_user_message.spawnSecond = elapse_second;
		g_user_message.spawnNanosecond = elapse_nanosecond;

		unsigned int elapse_wait_second = g_smclockptr->second - wait_second;
		unsigned int elapse_wait_nanosecond = g_smclockptr->nanosecond + wait_nanosecond + event_time;
		while(elapse_wait_nanosecond >= 1000000000)
		{
			elapse_wait_second++;
			elapse_wait_nanosecond = 1000000000 - elapse_wait_nanosecond;
		}
		g_user_message.waitSecond = elapse_wait_second;
		g_user_message.waitNanosecond = elapse_wait_nanosecond;
		g_user_message.waitTime = elapse_wait_second + (elapse_wait_nanosecond / 1000000000.0);
		wait_second = elapse_wait_second;
		wait_nanosecond = elapse_wait_nanosecond;

		msgsnd(g_message_queueid, &g_user_message, (sizeof(struct Message) - sizeof(long)), 0);


		g_user_message.childPid = -1;
		if(is_terminate)
		{
			break;
		}
	}

	exit(index);
}

void processInterrupt()
{
	struct sigaction sa1;
	sigemptyset(&sa1.sa_mask);
	sa1.sa_handler = &processHandler;
	sa1.sa_flags = SA_RESTART;
	if(sigaction(SIGUSR1, &sa1, NULL) == -1)
	{
		perror("ERROR");
	}

	struct sigaction sa2;
	sigemptyset(&sa2.sa_mask);
	sa2.sa_handler = &processHandler;
	sa2.sa_flags = SA_RESTART;
	if(sigaction(SIGINT, &sa2, NULL) == -1)
	{
		perror("ERROR");
	}
}
void processHandler(int signum)
{
	printf("%d: Terminated!\n", getpid());
	cleanUp();
	exit(2);
}


void discardSharedMemory(void *shmaddr, char *shm_name , char *exe_name, char *process_type)
{
	//Detaching...
	if(shmaddr != NULL)
	{
		if((shmdt(shmaddr)) < 0)
		{
			fprintf(stderr, "%s (%s) ERROR: could not detach [%s] shared memory!\n", exe_name, process_type, shm_name);
		}
	}
}

void cleanUp()
{
	//Release [shmclock] shared memory
	discardSharedMemory(g_smclockptr, "smclock", g_exe_name, "Child");

	//Release [pcbt] shared memory
	discardSharedMemory(g_pcbt_smptr, "pcbt", g_exe_name, "Child");
}

void semaLock(int sema_index)
{
	g_sema_operation.sem_num = sema_index;
	g_sema_operation.sem_op = -1;
	g_sema_operation.sem_flg = 0;
	semop(g_sema_id, &g_sema_operation, 1);
}

void semaRelease(int sema_index)
{	
	g_sema_operation.sem_num = sema_index;
	g_sema_operation.sem_op = 1;
	g_sema_operation.sem_flg = 0;
	semop(g_sema_id, &g_sema_operation, 1);
}



