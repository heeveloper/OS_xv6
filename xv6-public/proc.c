#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct proc mlfq;
int time_quantum[3] = {1, 2, 4};
int time_allotment[3] = {5, 10, 100};

// Lock used to prevent racing when threads approached their common parent.
struct spinlock processlock;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

void
pinit(void)
{
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

  // Scheduling initialization.
  p->isMLFQ = FALSE;
  p->quantum = 0;
  p->ticks = 0;
  p->level = 0;
  p->isStride = FALSE;
  p->share = 0;
  p->stride = 0;
  p->pass = 0;

  // LWP initialization.
  p->tid = 0;
  p->num_of_threads = 0;
  p->sum_of_threads = 0;
  p->retval = 0;

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

  p->isMLFQ = FALSE;
  p->quantum = 0;
  p->ticks = 0;
  p->level = 0;
  p->isStride = FALSE; // If zero, MLFQ(default).
  p->stride = 0;
  p->pass = 0;
  
  p->tid = 0;
  p->num_of_threads = 0; // main thread
  p->sum_of_threads = 0;
  
  // mlfq struct initialization.
  mlfq.isMLFQ = TRUE;
  mlfq.isStride = TRUE;
  mlfq.share = 100; 
  mlfq.stride = TOTALTICKET / mlfq.share;
  mlfq.pass = 0;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  // process case
  if(curproc->tid == 0){
    sz = curproc->sz;
    if(n > 0){
      if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
        return -1;
    } else if(n < 0){
      if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
        return -1;
    }
    curproc->sz = sz;
  }
  // thread case
  else{
    sz = curproc->parent->sz;
    if(n > 0){
      if((sz = allocuvm(curproc->parent->pgdir, sz, sz + n)) == 0)
        return -1;
    } else if(n < 0){
      if((sz = deallocuvm(curproc->parent->pgdir, sz, sz + n)) == 0)
        return -1;
    }
    curproc->parent->sz = sz;
  }

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

  release(&ptable.lock);

  return pid;
}

void
exitProcAndProc(struct proc *curproc){
  struct proc *p; 
  int fd; 

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

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  
  sched();
  panic("zombie exit");
}

void
exitProcAndLWP(struct proc *curproc){
  struct proc *p; 
  int fd; 

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc && p->tid > 0){
      release(&ptable.lock);

      // Close all open files.
      for(fd = 0; fd < NOFILE; fd++){
        if(p->ofile[fd]){
          fileclose(p->ofile[fd]);
          p->ofile[fd] = 0; 
        }    
      }   
      
      begin_op();
      iput(p->cwd);
      end_op();
      p->cwd = 0; 

      acquire(&ptable.lock);

      p->parent->num_of_threads--;

      // If the number of threads of parent process is 0,
      // reduce the momory size and set the total share of threads 0
      // which means it does not use CPU no more.
      if(p->parent->num_of_threads == 0){
        p->parent->sz = deallocuvm(p->parent->pgdir, p->parent->sz, p->parent->sz - 2 * (p->parent->sum_of_threads) * PGSIZE);
        p->parent->sum_of_threads = 0;
      }

      kfree(p->kstack);
      p->kstack = 0;
      p->pid = 0;
      p->parent = 0;

      // It can be removed!!
      p->killed = 0;

      p->level = 0;
      p->quantum = 0;
      mlfq.share += p->share;
      mlfq.stride = (int)(TOTALTICKET / mlfq.share);

      p->tid = 0;
      p->share = 0;
      p->stride = 0;
      p->state = UNUSED;
      p->num_of_threads = 0;
      p->retval = 0;
    }
  }
  release(&ptable.lock);

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

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;

  sched();
  panic("zombie exit");

}

void
exitLWPAndLWP(struct proc *curproc){
  struct proc *pp = curproc->parent;
  struct proc *p;
  int fd;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc ->parent && p->tid > 0 && p != curproc){
      release(&ptable.lock);

      // Close all open files.
      for(fd = 0; fd < NOFILE; fd++){
        if(p->ofile[fd]){
          fileclose(p->ofile[fd]);
          p->ofile[fd] = 0;
        }
      }
      begin_op();
      iput(p->cwd);
      end_op();
      p->cwd = 0;

      acquire(&ptable.lock);
      kfree(p->kstack);
      p->parent->num_of_threads--;
      p->pid = 0; 
      p->parent = 0;
      p->kstack = 0;

      p->killed = 0;

      p->level = 0;
      p->quantum = 0;

      mlfq.share += p->share;
      mlfq.stride = (int)(TOTALTICKET / mlfq.share);

      p->tid = 0;
      p->share = 0;
      p->stride = 0;
      p->state = UNUSED;
      p->num_of_threads = 0;
      p->retval = 0;
    }
  }
  release(&ptable.lock);

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

  curproc->parent->num_of_threads--;

  // If the number of threads of parent process is 0,
  // reduce the momory size and set the total share of threads 0
  // which means it does not use CPU no more.
  if(curproc->parent->num_of_threads == 0 && curproc->parent->sum_of_threads > 0){
    curproc->parent->sz=deallocuvm(curproc->parent->pgdir, curproc->parent->sz, curproc->parent->sz - 2 * (curproc->parent->sum_of_threads - 1) * PGSIZE);
    curproc->parent->sum_of_threads = 0;
  }

  curproc->state = ZOMBIE;
  curproc->parent = curproc;

  for(fd = 0; fd < NOFILE; fd++){
    if(pp->ofile[fd]){
      fileclose(pp->ofile[fd]);
      pp->ofile[fd] = 0;
    }
  }


  begin_op();
  iput(pp->cwd);
  end_op();
  pp->cwd = 0;
  acquire(&ptable.lock);

  wakeup1(pp->parent);

  pp->state = ZOMBIE;

  sched();
  panic("zombie exit");

}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();

  if(curproc == initproc)
    panic("init exiting");


  // curproc is a normal process
  // and its child is also a normal process.
  if(curproc->tid == 0 && curproc->num_of_threads == 0){
    exitProcAndProc(curproc);
  }
  // curproc is a normal process
  // and its child is a LWP.
  else if(curproc->tid == 0 && curproc->num_of_threads != 0){
    exitProcAndLWP(curproc);
  }
  // curproc is a LWP 
  // and its child is also a LWP.
  else{
    exitLWPAndLWP(curproc);
  }
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
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

        // Scheduling initialization.
        p->isMLFQ = FALSE;
        p->quantum = 0;
        p->ticks = 0;
        p->level = 0;
        p->isStride = FALSE;
        mlfq.share += p->share;
        mlfq.stride = (int)(TOTALTICKET / mlfq.share);
        p->share = 0;
        p->stride = 0;
        p->pass = 0;    
        // Thread initialization.
        p->tid = 0;
        p->num_of_threads = 0;
        p->sum_of_threads = 0;
        p->retval = 0;

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

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;

  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Stride and MLFQ scheduler.

    // Get the process which has the minimum pass.
    int lowest_pass = 987654321;
    struct proc *selectedproc = 0;
    acquire(&ptable.lock);
    for(p=ptable.proc; p<&ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;
      if(p->isStride == FALSE)
        continue;
      if(lowest_pass > p->pass){
        lowest_pass = p->pass;
        selectedproc = p;
      }
    }

    // If there is no process in Stride scheduler,
    // or the process which has the minimum pass value is mlfq,
    // one more MLFQ scheduling is needed.
    if(selectedproc == 0 || selectedproc->pass > mlfq.pass){
      // MLFQ scheduling below.
      // update mlfq pass value by its stride.
      mlfq.pass += mlfq.stride;
      if(mlfq.pass >= 100000000){
        mlfq.pass = 0;
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
          if(p->isStride){
            p->pass = 0;
          }
        }
      } 
      // level 0 (highest level)
      int found = FALSE;
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE || p->isStride || p->level != 0) {
          continue;
        }

        found = TRUE;
        c->proc = p;
        p->ticks += time_quantum[p->level];
        switchuvm(p);
        p->state = RUNNING;

        swtch(&(c->scheduler), p->context);
        switchkvm();

        if(p->ticks >= time_allotment[p->level]){
          p->ticks = 0;
          p->level++;
        }
        c->proc = 0;
      }
      // level 1 (middle level)
      if(!found){
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
          if(p->state != RUNNABLE || p->isStride || p->level != 1)
            continue;

          found = TRUE;
          c->proc = p;
          p->ticks += time_quantum[p->level];
          switchuvm(p);
          p->state = RUNNING;

          swtch(&(c->scheduler), p->context);
          switchkvm();

          if(p->ticks >= time_allotment[p->level]){
            p->ticks = 0;
            p->level++;
          }
          c->proc = 0;
        }
      }

      // level 2 (lowest level)
      if(!found){
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
          if(p->state != RUNNABLE || p->isStride || p->level != 2)
            continue;

          found = TRUE;
          c->proc = p;
          p->ticks += time_quantum[p->level];
          switchuvm(p);
          p->state = RUNNING;

          swtch(&(c->scheduler), p->context);
          switchkvm();

          // Priority Boost
          if(p->ticks >= time_allotment[p->level]){
            p->ticks = 0;
            p->level = 0;
          }
        }
      }
    }
    // The selected process running in Stride scheduler.
    else{
      //cprintf("STride!!!!!\n");
      c->proc = selectedproc;
      switchuvm(selectedproc);
      selectedproc->state = RUNNING;
      selectedproc->pass += selectedproc->stride;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      swtch(&(c->scheduler), selectedproc->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }

    release(&ptable.lock);

    /* // RR scheduler
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state != RUNNABLE)
    continue;

    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.
    c->proc = p;
    switchuvm(p);
    p->state = RUNNING;

    swtch(&(c->scheduler), p->context);
    switchkvm();

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
    }
    release(&ptable.lock);

*/
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
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
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

  sched();

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
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
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
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
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


// Get the level of current process ready queue of MLFQ.
// Returns one of the level of MLFQ (0/1/2)
int
getlev(void)
{
  return myproc()->level;
}

// Inquire to obtain cpu share(%).
int
set_cpu_share(int share)
{
  struct proc *p;
  struct proc *curproc = myproc();
  int lowest_pass = 987654321;
  if(share <= 0){
    cprintf("Error : No negative share or zero.\n");
    return -1;
  }

  if(mlfq.share - share < 20){
    cprintf("Error : Acceptable share(80%) exceed. %d\n", share);
    return -1;
  }
  
  // Get lowest_pass.
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->isStride){
      if(lowest_pass > p->pass)
        lowest_pass = p->pass;
    }
  }
  release(&ptable.lock);
  

  // curproc is a normal process.
  if(curproc->tid == 0){
    // curproc has LWP childs.
    if(curproc->num_of_threads != 0){
      curproc->isStride = TRUE;
      curproc->share = (int)(share / curproc->num_of_threads);
      curproc->stride = (int)(TOTALTICKET / curproc->share);
      curproc->pass = (lowest_pass < mlfq.pass) ? lowest_pass : mlfq.pass;

      acquire(&ptable.lock);
      for(p = ptable.proc; p < &ptable.proc[NPROC]; p++ ){
        if(p->parent == curproc && p->tid != 0){
          p->isStride = TRUE;
          p->share = (int)(share / curproc->num_of_threads);
          p->stride = (int)(TOTALTICKET / p->share);
          p->pass = curproc->pass;
        }
      }
      release(&ptable.lock);
    }
    // curproc has no child LWP.
    else{ 
      curproc->isStride = TRUE;
      curproc->share = share;
      curproc->stride = (int)(TOTALTICKET / curproc->share);
      curproc->pass = (lowest_pass < mlfq.pass) ? lowest_pass : mlfq.pass;
    }
  }
  // curproc is a LWP.
  else{
   curproc->isStride = TRUE;
   curproc->share = share;
   curproc->stride = (int)(TOTALTICKET / curproc->share);
   curproc->pass = (lowest_pass < mlfq.pass) ? lowest_pass : mlfq.pass;
  }
  
  // Update mlfq share.
  mlfq.share = mlfq.share - share;
  mlfq.stride = (int)(TOTALTICKET / mlfq.share);
  return share;
}

int
thread_create(thread_t *thread, void *(*start_routine)(void *), void *arg){
  int i;
  struct proc *np, *curproc, *p;
  uint sp, args[2];

  curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  acquire(&processlock);

  curproc->sz = PGROUNDUP(curproc->sz);
  if((curproc->sz = allocuvm(curproc->pgdir, curproc->sz, curproc->sz + 2 * PGSIZE)) == 0){
    return -1;
  }

  sp = curproc->sz;

  // Shallow copy pgdir and add thread information.
  np->pgdir = curproc->pgdir;   // same address space
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;
  np->tid = (thread_t)np->pid;
  
  // Set return value thread
  *thread = np->tid;

  // Update the caller process.
  np->parent->num_of_threads++;
  np->parent->sum_of_threads++;
  release(&processlock);

  args[0] = 0xDEADDEAD; // fake return address
  args[1] = (uint)arg;

  sp -= 8;

  // Thread function arguments
  if(copyout(np->pgdir, sp, args, (2)*4) < 0){
    return -1;
  }

  // Clear %eax so that thread_create returns 0 in the child.
  np->tf->eax = 0;

  // Set eip to start_routine.
  np->tf->eip = (uint)start_routine;

  // Set esp to current stack pointer.
  np->tf->esp = np->sz - 8;

  // Copy opened files from parent process.
  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);
  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  // Commit to user image.
  switchuvm(curproc);

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  // If parent process has a share,
  // set up thread share based on the process' number of threads.
  if(np->parent->isStride){
    // Parent process has threads.
    if(np->parent->num_of_threads > 0)
      np->parent->share = (int)((np->parent->share) * (np->parent->num_of_threads)) / (np->parent->num_of_threads);
    // Parent process has no thread.
    else{
      np->parent->share = np->parent->share / 2;
    }
    np->parent->stride = (int)(TOTALTICKET / np->parent->share);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent == np->parent && p->tid > 0){
        // print out whether me include this logic!!!
        p->isStride = TRUE;
        p->share = np->parent->share;
        p->stride = (int)(TOTALTICKET / p->share);
        p->pass = np->parent->pass;
      }
    }
  }

  release(&ptable.lock);

  return 0;
}

// Almost same as the original exit function,
// except that retval set to the proc struct.
void
thread_exit(void *retval){
  struct proc *p, *curproc;
  int fd;

  curproc = myproc();

  if(curproc == initproc)
    panic("init existing");

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

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;

  // Set retval.
  curproc->retval = retval;
  sched();
  panic("zombie exit");
}

// It is simliar to the wait function,
// except that it wait for threads.
// Return -1 if this process has no children.
int
thread_join(thread_t thread, void **retval){
  struct proc *p, *curproc;
  int havekids;
  int ret;

  curproc = myproc();

  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited chlidren.
    havekids = FALSE;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc || p->tid != thread)
        continue;

      // Found.
      havekids = TRUE;
      if(p->state == ZOMBIE){
        p->parent->num_of_threads--;

        // If the number of threads of parent process is 0,
        // reduce the momory size and set the total share of threads 0
        // which means it does not use CPU no more.
        if(p->parent->num_of_threads == 0){
          if((p->parent->sz = 
                deallocuvm(p->parent->pgdir, p->parent->sz, 
                  p->parent->sz - 2 * (p->parent->sum_of_threads) * PGSIZE)) == 0)
            return -1;
          p->parent->sum_of_threads = 0;
        }

        ret = p->pid;
        *retval = p->retval;

        kfree(p->kstack);
        p->kstack = 0;
        p->pid = 0;
        p->parent = 0;
        p->killed = 0;
        p->state = UNUSED;

        // Scheduling initialization.
        p->level = 0;
        p->quantum = 0;
        mlfq.share += p->share;
        mlfq.stride = (int)(TOTALTICKET / mlfq.share);
        p->share = 0;
        p->stride = 0;
        // Thread initialization.
        p->tid = 0;
        p->num_of_threads = 0;
        p->retval = 0;
        release(&ptable.lock);

        return ret;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1; 
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);   //DOC: wait-sleep
  }
}
