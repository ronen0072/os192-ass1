#include "schedulinginterface.h"
#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

extern PriorityQueue pq;
extern RoundRobinQueue rrq;
extern RunningProcessesHolder rpholder;

long long getAccumulator(struct proc *p) {
	return p->accumulator;
}

enum policy { ROUND_ROBIN, PRIORITY, E_PRIORITY };
volatile int pol = ROUND_ROBIN;
int min_priority = 0;
int max_priority = 10;
volatile uint time_quantum_counter = 1;
long long MAX_LONG  = 9223372036854775807;


#define DEFAULT_PRIORITY 5
#define NEW_PROCESS 1 	//true
#define OLD_PROCESS 0		//false

static void getTicks(uint * currtick);
static void (*volatile switchFromPolicy)(int toPolicy);
static void (*volatile signToQ)(struct proc * p , int isNew);
static struct proc * (*volatile getProc)(void);
static boolean (*volatile isQEmpty)(void);

static void  switchFromRRQ (int toPolicy);
static void  switchFromPQ (int toPolicy);
static void  switchFromExtPQ (int toPolicy);

static void signToRRQ(struct proc * p , int isNew);
static void signToPQ(struct proc * p , int isNew);
static void signToExtPQ(struct proc * p , int isNew);

static struct proc * getRRQProc(void);
static struct proc * getPQProc(void);
static struct proc * getExtPQProc(void);

static boolean isEmptyRRQ(void);
static boolean isEmptyPQ(void);

static void updateMinAccumulator(struct proc* p);

void (*switchFromPolicyArr[])(int toPolicy) = {
	[0] switchFromRRQ,
	[1] switchFromPQ,
	[2] switchFromExtPQ
};

void (* signToQArr [])(struct proc * p , int isNew) = {
	[0] signToRRQ,
	[1] signToPQ,
	[2] signToExtPQ
};

struct proc * (*getProcArr [3])(void) = {
	[0] getRRQProc,
	[1] getPQProc,
	[2] getExtPQProc
};

boolean (*isQEmptyArr [3])(void) = {
	[0] isEmptyRRQ,
	[1] isEmptyPQ,
	[2] isEmptyPQ
};

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;
static struct proc *lastProc = 0;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void policy(int toPolicy) {

	if(toPolicy < 0 || toPolicy > 2){
		//panic("The policy number is not in range...\n");
	//	cprintf("The policy number is not in range...\n");
		return;
	}
	if(pol == toPolicy){
		//cprintf("Allready in this policy, doing nothing...\n");
		return;
	}
	acquire(&ptable.lock);
	switchFromPolicy(toPolicy);
	pol = toPolicy;
	switchFromPolicy = switchFromPolicyArr[toPolicy];
	signToQ = signToQArr[toPolicy];
	getProc = getProcArr[toPolicy];
	isQEmpty = isQEmptyArr[toPolicy];
	release(&ptable.lock);
}

boolean isEmptyRRQ(){
	return rrq.isEmpty();
}

boolean isEmptyPQ(){
	return pq.isEmpty();
}

void switchFromRRQ (int toPolicy){
	struct proc *pr;
	if(!rrq.switchToPriorityQueuePolicy()){
		panic("switchFromRRQ: falied");
	}
	if(toPolicy == E_PRIORITY){
			min_priority = 0;
		}
	else{
		min_priority = 1;
		for (pr = ptable.proc; pr < &ptable.proc[NPROC]; pr++) {
			pr->priority = pr->priority == 0 ? 1 :pr->priority;
		}
	}
}

void switchFromPQ (int toPolicy){
struct proc * pr;
	if(toPolicy == ROUND_ROBIN){
			if(!pq.switchToRoundRobinPolicy()){
				panic("switchFromPQ: falied");

			}
			for (pr = ptable.proc; pr < &ptable.proc[NPROC]; pr++) {
				pr->accumulator = 0;
			}
	}
	else {
		min_priority=0;
	}
}
void switchFromExtPQ (int toPolicy){
	struct proc * pr;
	if(toPolicy == ROUND_ROBIN){
			if(!pq.switchToRoundRobinPolicy()){
				panic("switchFromExtPQ: falied");
			}

			for (pr = ptable.proc; pr < &ptable.proc[NPROC]; pr++) {
				pr->accumulator = 0;
			}
	}
		else {
			for (pr = ptable.proc; pr < &ptable.proc[NPROC]; pr++) {
				pr->priority = pr->priority == 0 ? 1 :pr->priority;
			}
			min_priority = 1;
	}

}


void handleSettings(struct proc * p,int isNew){
	uint currtick;
	getTicks(&currtick);
	p->readyStartTime = currtick;
	if(!isNew){
		p->rutime += (currtick - p->startRunningTime);
		if(pol != ROUND_ROBIN){
			p->accumulator = p->accumulator + p->priority;
		}
	}
	else{
		if(pol != ROUND_ROBIN){
			updateMinAccumulator(p);
		}
	}
}

void signToRRQ(struct proc * p , int isNew){
if (p->state == RUNNABLE){
	handleSettings(p, isNew);
	rrq.enqueue(p);
}
	else panic("signToRRQ: proc not Runnable!\n");

}
void signToPQ(struct proc * p , int isNew){
	if (p->state == RUNNABLE){
	handleSettings(p, isNew);

	pq.put(p);
}
	else panic("signToPQ: proc not Runnable!\n");
}
void signToExtPQ(struct proc * p , int isNew){
	if (p->state == RUNNABLE){

		handleSettings(p, isNew);

		pq.put(p);
}
	else panic("signToExtPQ: proc not Runnable!\n");
}


struct proc * getRRQProc(){
	if(rrq.isEmpty()){
		panic("getRRQProc failed!!");
	}
	struct proc * p = rrq.dequeue();
	return p;


}

struct proc * getPQProc(){
	if (pq.isEmpty()){
		panic("getPQProc failed!!");
	}
	struct proc * p = pq.extractMin();
	return p;


}

struct proc * getExtPQProc(){
	struct proc * p = pq.extractMin();
	if(!p){
		panic("getExtPQProc: Queue is empty!");
	}
	struct proc * nextProc =0;

		if(time_quantum_counter % 100 == 0 && !pq.isEmpty()){
			uint min = MAX_LONG;
			struct proc *cp;

			for (cp = ptable.proc; cp < &ptable.proc[NPROC]; cp++) {
				if (cp->state == RUNNABLE) {
						if (cp->bedTime < min) {
								nextProc = cp;
								min = cp->bedTime;
							 }
					 }

			 }
			 if(nextProc == null){
				 panic("no runnable proc to choose");
			 }
			 if(p!=nextProc){
				 pq.put(p);
				 if(!pq.extractProc(nextProc)){
					 panic("RUNNABLE proc not in queue");
				 }
			 }
		}
		else{

			nextProc =  p;
		}

	time_quantum_counter++;
	lastProc = nextProc;
	return nextProc;
}

void
pinit(void)
{
	switchFromPolicy = switchFromPolicyArr[pol];
	signToQ = signToQArr[pol];
	getProc = getProcArr[pol];
	isQEmpty = isQEmptyArr[pol];
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;

  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);
	p->priority = DEFAULT_PRIORITY;
  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;
	uint currtick;
	getTicks(&currtick);
	p->bedTime = time_quantum_counter;
	p->readyStartTime = 0;
	p->startRunningTime = 0;
	p->ctime = currtick;
	p->ttime = 0;
	p->stime = 0;
	p->rutime = 0;
	p->retime = 0;


  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

	signToQ(p, NEW_PROCESS);
  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

	signToQ(np,NEW_PROCESS);

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait(0) to find out it exited.
void
exit(int status)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);
	curproc->exit_status = status;


  // Parent might be sleeping in wait(0).
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
	uint curtick;
	getTicks(&curtick);
	curproc->ttime = curtick;
	curproc->rutime += (curtick - curproc->startRunningTime);
  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
	sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(int * status)
{
	struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
				p->bedTime = MAX_LONG;

				// discard the status if it's null
				if(status != null){
					*status = p->exit_status;
				}
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)

    sleep(curproc, &ptable.lock);  //DOC: wait-sleep

  }
}

// Detach : detach the process with pid from current paret to init
int detach(int pid){
	struct proc *p;
	struct proc *curproc = myproc();
	acquire(&ptable.lock);
	for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
		if (pid!= p->pid || curproc!= p->parent)
			continue;

		p->parent = initproc;
		release(&ptable.lock);
		return 0;

	}
	release(&ptable.lock);
	//cprintf("Detach failed, no child proccess with pid %d \n", pid);
	return -1;
}
void priority(int priority){

	if(priority >= min_priority && priority <= max_priority){
		acquire(&ptable.lock);
	 	myproc()->priority = priority;
		release(&ptable.lock);
 	}
 	//else panic("Priorety is not in allowed range");

}

int wait_stat(int *status, struct perf *performance) {
	struct proc *p;
	int havekids, pid;
	struct proc *curproc = myproc();

	acquire(&ptable.lock);
	for(;;){
		// Scan through table looking for exited children.
		havekids = 0;
		for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
			if(p->parent != curproc)
				continue;
			havekids = 1;
			if(p->state == ZOMBIE){
				performance->ctime = p->ctime;
				performance->ttime = p->ttime;
				performance->stime = p->stime;
				performance->retime = p->retime;
				performance->rutime = p->rutime;
				// Found one.
				pid = p->pid;
				kfree(p->kstack);
				p->kstack = 0;
				freevm(p->pgdir);
				p->pid = 0;
				p->parent = 0;
				p->name[0] = 0;
				p->killed = 0;
				p->state = UNUSED;
				p->bedTime = MAX_LONG;
				p->rutime = 0;
				p->retime = 0;
				p->stime = 0;
				p->ttime = 0;
				p->ctime = 0;

				// discard the status if it's null
				if(status != null){
					*status = p->exit_status;
				}
				release(&ptable.lock);
				return pid;
			}
		}

		// No point waiting if we don't have any children.
		if(!havekids || curproc->killed){
			release(&ptable.lock);
			return -1;
		}

		// Wait for children to exit.  (See wakeup1 call in proc_exit.)

		sleep(curproc, &ptable.lock);  //DOC: wait-sleep

	}
}





void updateMinAccumulator(struct proc* p){

	long long acc_pq, acc_rq;

    int pqSuccess = pq.getMinAccumulator(&acc_pq);
    int rqSuccess = rpholder.getMinAccumulator(&acc_rq);

    if (pqSuccess == 1 && rqSuccess == 1) {
        p->accumulator = acc_pq < acc_rq ? acc_pq : acc_rq;
    } else if (pqSuccess == 1) {
        p->accumulator = acc_pq;
    } else if (rqSuccess == 1) {
        p->accumulator = acc_rq;
    }
		else {
			p->accumulator = 0;
		}
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void){
  struct proc *p=0;
  struct cpu *c = mycpu();
  c->proc = 0;
	uint curtick;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

		if (!isQEmpty()){
			p = getProc();
      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
			getTicks(&curtick);
			p->retime += curtick - p->readyStartTime;
      p->state = RUNNING;
			p->startRunningTime = curtick;
			rpholder.add(p);
      swtch(&(c->scheduler), p->context);
			rpholder.remove(p);
			p->bedTime = time_quantum_counter;
			switchkvm();
			c->proc = 0;
			 // Process is done running for now.
      // It should have changed its p->state before coming back.

    }
    release(&ptable.lock);

  }
}
// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
	struct proc * p = myproc();
  acquire(&ptable.lock);  //DOC: yieldlock
  p->state = RUNNABLE;
	signToQ(p, OLD_PROCESS);
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}
void getTicks(uint * currtick){
	  //acquire(&tickslock);
		*currtick = ticks;
		//release(&tickslock);

}
// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;
	uint currtick;
	getTicks(&currtick);
	p->rutime += (currtick - p->startRunningTime);
	p->bedTime = currtick;
	int beforTick = currtick;
	sched();
	getTicks(&currtick);
	int afterTick = currtick;
	p->stime += (afterTick - beforTick);

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan){
      p->state = RUNNABLE;
			signToQ(p,NEW_PROCESS);
		}
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING){
        p->state = RUNNABLE;
				signToQ(p,NEW_PROCESS);
			}
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
