#ifndef SHARED_H
#define SHARED_H


struct SharedClock 
{
	unsigned int second;
	unsigned int nanosecond;
};


struct Message
{
	long mtype;
	int flag;	
	int index;
	pid_t childPid;
	int priority;
	unsigned int burstTime;
	unsigned int spawnSecond;
	unsigned int spawnNanosecond;
	unsigned int waitSecond;
	unsigned int waitNanosecond;
	double waitTime;
	unsigned int second;
	unsigned int nanosecond;
	char message[BUFFER_LENGTH];
};


struct ProcessControlBlock
{
	int pidIndex;
	pid_t actualPid;
	int priority;
	unsigned int lastBurst;
	unsigned int totalBurst;
	unsigned int totalSystemSecond;
	unsigned int totalSystemNanosecond;
	unsigned int totalWaitSecond;
	unsigned int totalWaitNanosecond;
};


struct UserProcess
{
	int index;
	pid_t actualPid;
	int priority;
};


struct QNode 
{ 
	int index;
	struct QNode *next;
}; 


struct Queue 
{ 
	struct QNode *front;
	struct QNode *rear; 
}; 


#endif

