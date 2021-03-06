#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl) {
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table at boot time.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void) {
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void) {
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid() {
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  p->Trace = 0;

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;
  p->rtime = 0;
  p->etime = 0;
  p->ctime = ticks;


  #ifdef FCFS
  p->start_time = ticks;
  #endif

  #ifdef PBS
  p->Static_priority = 60;
  p->Times_scheduled = 0;
  p->start_time = ticks;
  p->running_time = -1;
  p->sleeping_time = -1;
  #endif

  #ifdef MLFQ
  p->priority_number = 0;
  p->time_added = ticks;
  p->no_of_ticks = 0;
  p->No_times = 0;
  #endif

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process,
// with no user memory, but with trampoline pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe just below TRAMPOLINE, for trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// a user program that calls exec("/init")
// od -t xC initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  // allocate one user page and copy init's instructions
  // and data into it.
  uvminit(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if((sz = uvmalloc(p->pagetable, sz, sz + n)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;
  np->Trace = p->Trace;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  #ifdef MLFQ
  if(p->priority_number != 0) //premeption part of the MLFQ
  {
    p->no_of_ticks = 0;
    yield();
  }
  #endif

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
exit(int status)
{
  struct proc *p = myproc();

  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;
  p->etime = ticks;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(uint64 addr)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

int
waitx(uint64 addr, uint* rtime, uint* wtime)
{
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&np->lock);

        havekids = 1;
        if(np->state == ZOMBIE){
          // Found one.
          pid = np->pid;
          *rtime = np->rtime;
          *wtime = np->etime - np->ctime - np->rtime;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&np->xstate,
                                  sizeof(np->xstate)) < 0) {
            release(&np->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }

    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

void
update_time()
{
  struct proc* p;
  for (p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if (p->state == RUNNING) {
      p->rtime++;
    }
    release(&p->lock); 
  }
}

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
#ifdef DEFAULT
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  
  c->proc = 0;
  for(;;){
    // Avoid deadlock by ensuring that devices can interrupt.
    intr_on();

    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
      }
      release(&p->lock);
    }
  }
}
#endif


#ifdef FCFS
void
scheduler(void)
{
  struct proc *p, *ToBeScheduled = 0;
  struct cpu *c = mycpu();

  while(1)
  {
    c->proc = 0;
    // Avoid deadlock by ensuring that devices can interrupt.  
    intr_on();

    int First_start = -1;
    for(p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if(p->state == RUNNABLE)
      {
        if(First_start == -1)                           //Intiallizng with first runnable process. 
        {
          First_start = p->start_time;
          ToBeScheduled = p;
          continue;
        }
        
        if(p->start_time < First_start)                //found a process which was started earilier.  
        {
          release(&ToBeScheduled->lock);
          First_start = p->start_time;
          ToBeScheduled = p;
          continue;
        }
      }
      release(&p->lock);
    }

    if(First_start == -1)
      continue;
      

    ToBeScheduled->state = RUNNING;
    c->proc = ToBeScheduled;
    swtch(&c->context, &ToBeScheduled->context);
    release(&ToBeScheduled->lock);
  }
  
}
#endif


#ifdef PBS
int PSBPriority(struct proc* p)
{
  //caliculating nicess
  int niceness = -1;
  if(p->sleeping_time == -1 && p->running_time == -1) //default
    niceness = 5;
  else
    niceness = (int)(p->sleeping_time*10)/(p->running_time + p->sleeping_time); 
  
  //caliculating priority using niceness and static priority.
  int Value = p->Static_priority - niceness + 5;

  if(Value > 100)
    return 100;
  if(Value < 0)
    return 0;
    
  return Value;
}

// positive if q > p.
int PcbCompare(struct proc *p, struct proc *q)
{
  int p_priority = PSBPriority(p), q_priority = PSBPriority(q);

  if(p_priority < q_priority)  //p is more important
    return -1;
  if(p_priority > q_priority)  //q is more important
    return 1;

  //p_priority = q_priority.  process with less number of times scheduled
  if(p->Times_scheduled > q->Times_scheduled) //q is run less number of times
    return 1;
  if(q->Times_scheduled > p->Times_scheduled) //p is run less number of times.
    return -1;

  //Times schecduled are equal. Started early given priority.
  if(p->start_time < q->start_time)  //p started before
    return -1;
  if(q->start_time < p->start_time) //q started before
    return 1;
  
  //All values are tied. (same time multiprocessor) //extrememly rare.
  return -1;
}

void
scheduler(void)
{
  struct proc *p, *ToBeScheduled = 0;
  struct cpu *c = mycpu();
  int intialized;
  
  while(1)
  {
    c->proc = 0;
    // Avoid deadlock by ensuring that devices can interrupt.  
    intr_on();

    intialized = -1;
    for(p = proc; p < &proc[NPROC]; p++)
    {
      acquire(&p->lock);
      if(p->state == RUNNABLE)
      {
        if(intialized == -1)                              //Intiallizng with first runnable process. 
        {
          ToBeScheduled = p;
          intialized = 0;
          continue;
        } 
        
        if(PcbCompare(ToBeScheduled, p) > 0) 
        {               //found a process which was found with higher priority
          release(&ToBeScheduled->lock);
          ToBeScheduled = p;
          continue;
        }
      }
      release(&p->lock);
    }

    if(intialized == -1)
      continue;

    ToBeScheduled->state = RUNNING;
    ToBeScheduled->Times_scheduled++;
    ToBeScheduled->sleeping_time = 0;
    ToBeScheduled->running_time = ticks;
    c->proc = ToBeScheduled;
    swtch(&c->context, &ToBeScheduled->context);
    release(&ToBeScheduled->lock);
  }
  
}
#endif

#ifdef MLFQ
void UpgradePolicy()
{
  static const int Max_wait[5] = {0, 10, 30, 100, 150};
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++)
  {
    if(p->state == RUNNABLE && p->priority_number != 0)
    {
      if(ticks - p->time_added > Max_wait[p->priority_number])
      {
        p->time_added = ticks;
        p->priority_number--;
      }
    }
  }
}

void
scheduler(void)
{
  struct proc *p, *toSchedule;
  struct cpu *c = mycpu();
  int rr_s = 0;
  uint minimum_time;

  //the below for loop is run when a scheduler is called. 
  //locks are not used single processor system no need
  for(;;)
  {
    intr_on();
    c->proc = 0;
    UpgradePolicy();

    for(int i = 0; i < 4; i++) //for i = 0,1,2,3
    {
      minimum_time = 0;
      for(p = proc; p < &proc[NPROC]; p++)
      {
        if(p->state == RUNNABLE && p->priority_number == i)
        {
          if(minimum_time == 0)
          {
            minimum_time = p->time_added;
            toSchedule = p;
            continue;
          }
          
          if(p->time_added < minimum_time)
          {
            minimum_time = p->time_added;
            toSchedule = p;
          }
        }
      }
      if(minimum_time != 0) goto Schedu;
    }
  
    while(1) //roundroubin
    {
      if(proc[rr_s].state == RUNNABLE)
      {
        toSchedule = &proc[rr_s];
        rr_s = (rr_s + 1) % NPROC;
        goto Schedu;
      }
      rr_s = (rr_s + 1) % NPROC;
    }

    //switching part. The process to schedule is in "toSchedule"
    Schedu:
    acquire(&toSchedule->lock);
    c->proc = toSchedule;
    toSchedule->state = RUNNING;
    toSchedule->time_added = 0;  //not a time to acess set back when added to que.//sleep timer
    toSchedule->No_times++;
    swtch(&c->context, &toSchedule->context);
    release(&toSchedule->lock);
  }
}
#endif

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    first = 0;
    fsinit(ROOTDEV);
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  #ifdef PBS
  p->running_time = ticks - p->running_time;
  p->sleeping_time = ticks;
  #endif

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc())
    {
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) 
      {
        p->state = RUNNABLE;
        #ifdef PBS
        p->sleeping_time = ticks - p->sleeping_time;
        #endif

        #ifdef MLFQ
        p->time_added = ticks;
        p->no_of_ticks = 0;
        #endif
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++)
  {
    if(p->state == UNUSED)
      continue;

    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";

    #ifdef DEFAULT
    printf("%d %s %s", p->pid, state, p->name);
    #endif

    #ifdef FCFS
    printf("%d %s %s", p->pid, state, p->name);
    #endif

    #ifdef PBS
    uint waittime;
    if(p->etime == 0)
      waittime = ticks - p->ctime - p->rtime;
    else
      waittime = p->etime - p->ctime - p->rtime;
    printf("%d %d %s %d %d %d", p->pid, PSBPriority(p), state, p->rtime, waittime, p->Times_scheduled);
    #endif

    #ifdef MLFQ
    uint waittime;
    if(p->etime == 0)
      waittime = ticks - p->ctime - p->rtime;
    else
      waittime = p->etime - p->ctime - p->rtime;
    printf("%d %d %s %d %d %d", p->pid, p->priority_number, state, p->rtime, waittime, p->No_times);
    #endif

    printf("\n");
  }
}

int set_priority_i(int priority, int pid)
{
  #ifndef PBS
  return 10; //not defined
  #endif

  #ifdef PBS
  if(priority < 0 || priority > 100)             //invalid values for static priority
    return 1;
  
  struct proc* pid_process;
  int Ppid = -1, old_priority;
  for(pid_process = proc; pid_process < &proc[NPROC]; pid_process++)
  {
    acquire(&pid_process->lock);
    if( (pid_process->state == RUNNABLE || pid_process->state == SLEEPING) && pid_process->pid == pid)          //even sleeping take to be care *****
    {
      Ppid = pid_process->pid;
      break;
    }
    release(&pid_process->lock);
  }
  if(Ppid == -1)
    return 2;

  old_priority = pid_process->Static_priority;
  pid_process->Static_priority = priority;
  pid_process->running_time = -1;
  pid_process->sleeping_time = -1;
  release(&pid_process->lock);

  if(priority > old_priority)
    yield();
  return old_priority;
  #endif

}