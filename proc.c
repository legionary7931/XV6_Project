#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "elf.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

struct mmap_area{
  struct file* f; // file descriptor
  uint addr; // start address of virtual address
  int length; // 
  int offset;
  int prot; // 
  int flags;
  struct proc* p;
  int state; // cursor: - 1 => not used yet, cursor: 0 => no page table, cursor: 1 => yes page table 
};
struct mmap_area mma[64];

static struct proc *initproc;

uint weight_total; //모든 runnable process들의 weight 총합
uint LIMIT = __UINT32_MAX__/2; // 최대 한계를 정의, 이 이상 올라가면 overflow로 간주
uint weight[40]={
    /*0*/   88761, 71755, 56483, 46273, 36291,
    /*5*/   29154, 23254, 18705, 14949, 11916,
    /*10*/   9548,  7620,  6100,  4904,  3906,
    /*15*/   3121,  2501,  1991,  1586,  1277,
    /*20*/   1024,   820,   655,   526,   423,
    /*25*/    335,   272,   215,   172,   137,
    /*30*/    110,    87,    70,    56,    45,
    /*35*/     36,    29,    23,    18,    15,
};

int boot = 1; //boot시에 fork 할 때 memory map are 초기화

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
  p->nice = 20;
  // 0으로 지정해야 되는 애들은 굳이 초기화 없이 바로 사용 
  // e.g) currentRunTime, totalRunTime, vRunTime, timeSlice, vRunLevel 등은 0이어야 하므로 따로 지정하지 않는다.

  release(&ptable.lock);

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
  
  if(boot){
    for(int i=0; i<64; i++){
      mma[i].f = 0;
      mma[i].addr = 0;
      mma[i].length = 0;
      mma[i].offset = 0;
      mma[i].prot = 0;
      mma[i].flags = 0;
      mma[i].p = 0;
      mma[i].state = -1; // 0일때 해당 page table 존재 X, 1일때 해당하는 page table이 존재
    }
    boot = 0; // boot done, mma initialize complete
  }

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
  int i, j, pid;
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

  // fork시 자식프로세스는 부모프로세스의 vruntime을 상속받게 된다.
  np->vRunTime = curproc->vRunTime;
  np->vRunLevel = curproc->vRunLevel;
  
  // Fork Inherit Check
  /*
  cprintf("=====Fork Inheriting Check=====\n");
  cprintf("\tweight\t nice\t vRunTime\t vRunLevel \t timeSlice\n");
  cprintf("parent: %d\t %d\t %d\t\t %d\t\t %d\t\n", curproc->weight, curproc->nice, curproc->vRunTime, curproc->vRunLevel, curproc->timeSlice);
  cprintf("child : %d\t %d\t %d\t\t %d\t\t %d\t\n", np->weight, np->nice, np->vRunTime, np->vRunLevel, np->timeSlice );
  cprintf("===============================\n");
  */

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  for(i=0; i<64; i++){ //np가 자식프로세스, curproc이 현재 프로세스(부모 프로세스)
    if(mma[i].p == curproc && mma[i].state != -1){
      for(j=0; j<64; j++){
        if(mma[j].state == -1){
          // duplicate parent's total mma info to child process
          mma[j].f = mma[i].f;
          mma[j].addr = mma[i].addr;
          mma[j].length = mma[i].length;
          mma[j].offset = mma[i].offset;
          mma[j].prot = mma[i].prot;
          mma[j].flags = mma[i].flags;
          mma[j].p = np;
          mma[j].state = mma[i].state;

          
          char* newmem = 0;
          if((mma[j].flags == 0 && mma[j].state == 1) || mma[j].flags == 2){ //file mapping (map_populate or is handled by PFHandler)
            struct file* fi = mma[j].f;

            for (int ptr=0; ptr<mma[j].length; ptr+=4096){
              pte_t* pte = walkpgdir(mma[i].p->pgdir, (char*)(mma[i].addr+ptr), 0);
              if(pte == 0) continue;
              if((*pte & PTE_P) == 0) continue; //walk parent's table, if no pte exists, do not allocate in child process too

              newmem = kalloc();
              if(newmem==0) return -1;


              fi->off = mma[i].offset + ptr;
              memset(newmem, 0, 4096); //physical memory allocate 해서 (set to 0)
              fileread(fi, newmem, 4096); //file에서 읽어온 내용을 allocate한 physical memory에 복사하기 (아직 page table 안 만들어짐)
      
              if(mappages(np->pgdir, (void*)(mma[j].addr+ptr), 4096, V2P(newmem), mma[j].prot | PTE_U) < 0){ 
                return -1;
              }
              //cprintf("fork: #1\n");
            }
          }
          else if((mma[j].flags == 1 && mma[j].state == 1) || mma[j].flags == 3){ //anonymous mapping (map_populate or is handled by PFHandler)
            for (int ptr=0; ptr<mma[j].length; ptr+=4096){
              pte_t* pte = walkpgdir(mma[i].p->pgdir, (char*)(mma[i].addr+ptr), 0);
              if(pte == 0) continue;
              if((*pte & PTE_P) == 0) continue; //walk parent's table, if no pte exists, do not allocate in child process too

              newmem = kalloc();
              if(newmem==0) return -1;

              memset(newmem, 0, 4096);

              if(mappages(np->pgdir, (void*)(mma[j].addr+ptr), 4096, V2P(newmem), mma[j].prot | PTE_U) < 0){
                return -1;
              }
              //cprintf("fork: #2\n");
            }
          }
          break;
        }
      }
    } 
  }

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
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
   
  // er, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
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
  struct proc *q;
  struct cpu *c = mycpu();
  c->proc = 0;

  struct proc *targetProc;

  for(;;){
    // Enable interrupts on this processor.
    sti();
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      weight_total = 0;
      if(p->state != RUNNABLE)
        continue;
      for(q = ptable.proc; q < &ptable.proc[NPROC]; q++){
        if(q->state == RUNNABLE){
          weight_total += weight[q->nice];
          //cprintf("weight: %d, p->nice: %d\n", weight[p->nice], p->nice);
        }
      }
      
      targetProc = p;
      for(q = ptable.proc; q < &ptable.proc[NPROC]; q++){
        if(q->state == RUNNABLE && q->vRunLevel <= targetProc->vRunLevel){
          if(q->vRunLevel == targetProc->vRunLevel && q->vRunTime < targetProc->vRunTime){
            targetProc = q;
          } 
          if(q->vRunLevel < targetProc->vRunLevel){
            targetProc = q;
          }
        }
      }

      /*
      cprintf("RUNNABLE CHECK\n");
      for(q = ptable.proc; q < &ptable.proc[NPROC]; q++){
        if(q->state == RUNNABLE){
          cprintf("pro name: %s, weight: %d\n", q->name, weight[q->nice]);
        }
      }
      */
      
      c->proc = targetProc;
      switchuvm(targetProc);
      targetProc->state = RUNNING;
      targetProc->timeSlice = (10000 * weight[targetProc->nice])/weight_total;

      //cprintf("weight total: %d, weight: %d, nice: %d, timeslice: %d\n", weight_total, weight[targetProc->nice], targetProc->nice ,targetProc->timeSlice);

      swtch(&(c->scheduler), targetProc->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
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
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  
  if(myproc()->vRunTime + myproc()->currentRunTime * 1024/weight[myproc()->nice] <= LIMIT){
    // overflow가 발생하지 않는다면 그냥 하던대로....
    // vRunTime = delta(Runtime) * (weight[20]/cur_weight)
    myproc()->vRunTime += myproc()->currentRunTime * 1024/weight[myproc()->nice];
    /*
    
    cprintf("currentRunTime:%d\n", myproc()->currentRunTime);
    cprintf("yield vRunTime add: %d\n", myproc()->currentRunTime * 1024/weight[myproc()->nice]);
    cprintf("vRunTime = %d\n", myproc()->vRunTime);
    */
  }
  else{
    // overflow가 발생해서 음수로 만들어져 버린다면?
    myproc()->vRunLevel += 1; // overflow 발생!
    myproc()->vRunTime = myproc()->vRunTime - LIMIT + (myproc()->currentRunTime * 1024/weight[myproc()->nice]); 
    //cprintf("yield hello overflowO\n");
  }
  myproc()->currentRunTime = 0;
  weight_total = 0;
  
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
  
  
  if(myproc()->vRunTime + myproc()->currentRunTime * 1024/weight[myproc()->nice] <= LIMIT){
    // overflow가 발생하지 않는다면 그냥 하던대로....
    // vRunTime = delta(Runtime) * (weight[20]/cur_weight)
    myproc()->vRunTime += myproc()->currentRunTime * 1024/weight[myproc()->nice];
    //cprintf("sleep hello\n");
  }
  else{
    // overflow가 발생해서 음수로 만들어져 버린다면?
    myproc()->vRunLevel += 1; // overflow 발생!
    myproc()->vRunTime = myproc()->vRunTime - LIMIT + (myproc()->currentRunTime * 1024/weight[myproc()->nice]); 
  }
  //myproc()->currentRunTime = 0;
  weight_total = 0;
  

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
  //int isRunnableExists = 0;
  uint minVRunTime;
  uint minVRunLevel;
  int isRunnableExists = 0;
  
  //cprintf("wakeup hello\n");

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == RUNNABLE){
      isRunnableExists = 1;
      minVRunLevel = p->vRunLevel;
      minVRunTime = p->vRunTime;
    }
  }

  if(isRunnableExists == 1){
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE) continue;
    
      
      if(p->vRunLevel < minVRunLevel){
        minVRunLevel = p->vRunLevel;
        minVRunTime = p->vRunTime;
      }
      else if(p->vRunLevel == minVRunLevel){
        if(p->vRunTime < minVRunTime){
          minVRunLevel = p->vRunLevel;
          minVRunTime = p->vRunTime;
        }
      }
    }
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state == SLEEPING && p->chan == chan){
        p->state = RUNNABLE;
        p->vRunLevel = minVRunLevel;
        p->vRunTime = minVRunTime - 1000 * 1024/weight[p->nice];
      }
    } 
    //cprintf("minVRunLevel: %d, minVRunTime: %d\n", minVRunLevel, minVRunLevel);
  }
  else{
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state == SLEEPING && p->chan == chan){
        p->state = RUNNABLE;
        p->vRunLevel = 0;
        p->vRunTime = 0;
      }
    }
    //cprintf("nobody is runnable...\n");
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

int getnice(int pid){
  int nice; 
  struct proc* p;

  if(pid <= 0){
    return -1;
  }

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      nice = p->nice;
      release(&ptable.lock);
      return nice; //if success, return nice value
    }
  }
  release(&ptable.lock);
  return -1; //if no process matching pid, return -1
}

int setnice(int pid, int value){
  struct proc* p;

  if(pid <= 0 || (value < 0 || value > 39)){
    return -1;
  }
  
  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->nice = value;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

void putBlank_String(int totalLen, char* str){
  int idx = 0;
  while(str[idx] != '\0') idx+=1;
  
  for(int i=0; i<totalLen-idx; i++) cprintf(" ");

}

void putBlank_Int(int totalLen, int value){
  int idx = 0;
  int tmp = value;
  while(tmp != 0){
    tmp /= 10;
    idx += 1;
  }

  if(value == 0) idx += 1;
  for(int i=0; i<totalLen-idx; i++) cprintf(" ");
}

void handleOverflow(uint vRunTime, uint vRunLevel){
  
  int idx = 0;
  uint tmp = vRunTime;

  
  int vRunArray[20] = {0,};
  while(tmp > 0){
    vRunArray[idx] = tmp%10;
    tmp /= 10;
    idx += 1;
  }
  if(vRunTime == 0) idx += 1;

  int vNum[20] = {7, 4, 6, 3, 8, 4, 7, 4, 1, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  int vNumResult[20] = {0, };

  for(int i = 0; i < 20; i++){   
    vNumResult[i] = vNum[i] * vRunLevel;
  }

  int carry = 0;
  for(int i = 0; i< 20; i++){
    vNumResult[i] += carry;
    carry = vNumResult[i]/10;
    vNumResult[i] %= 10;
  }
  /*
  for(int i=19; i>=0; i--){
    cprintf("%d", vNumResult[i]);
  }
  */
  int result[20] = {0, };
  for(int i = 0; i< 20; i++){
    result[i] = vNumResult[i] + vRunArray[i];
  }

  int carryExists = 0;
  for(int i=0; i<20; i++){
    if(carryExists) {
      result[i] += 1;
      carryExists = 0;
    }

    if (result[i] >= 10){
      carryExists = 1;
      result[i] -= 10;
    }
    else carryExists = 0;
  }

  int resultIdx = 19;
  for(int i=19; i>=0; i--){
    if(result[i] != 0){
      resultIdx = i;
      break;
    }
  }

  for(int i=resultIdx; i>=0; i--){
    cprintf("%d", result[i]);
  }
}

void ps(int pid){
  struct proc* p;

  //cprintf("%d\n", LIMIT);
  static char procstate[10][10] = {
    "UNUSED\0",
    "EMBRYO\0",
    "SLEEPING\0", // I/O Request등으로 waiting 중인 task
    "RUNNABLE\0", // ready queue에 쌓여있는 task
    "RUNNING\0", // 현재 실행중인 task
    "ZOMBIE\0 "
  }; 
  // pid 3 ps: trap 14 err 5 on cpu 1 eip 0xffffffff addr 0xffffffff--kill proc -> page fault?
  // xv6 user code does not allow return, instead use exit() - 2023. 03. 21
  acquire(&ptable.lock);
  cprintf("name     pid      state      priority      runtime/weight      runtime            vruntime \t\t\t ticks %d\n", ticks*1000);
          
  if(pid == 0){  
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->pid > 0){
        cprintf("%s", p->name);
        putBlank_String(9, p->name);
        cprintf("%d", p->pid);
        putBlank_Int(9, p->pid);
        cprintf("%s", procstate[p->state]);
        putBlank_String(11, procstate[p->state]);
        cprintf("%d", p->nice);
        putBlank_Int(14, p->nice);
        cprintf("%d", p->totalRunTime/weight[p->nice]);
        putBlank_Int(20, p->totalRunTime/weight[p->nice]);
        cprintf("%d", p->totalRunTime);
        putBlank_Int(19, p->totalRunTime);
        if(p->vRunLevel == 0){
          cprintf("%d", p->vRunTime);
        }
        else{
          handleOverflow(p->vRunTime, p->vRunLevel);
        }
        
        cprintf("\n");
        //cprintf("%s \t %d \t %s \t %d\n", p->name, p->pid, procstate[p->state], p->nice);
        //cprintf("%s \t %d \t %s \t %d \t\t %d \t\t\t %d \t\t %d \n", p->name, p->pid, procstate[p->state], p->nice, p->totalRunTime/weight[p->nice], p->totalRunTime, p->vRunTime);
      }
    }
  }
  else{
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->pid == pid){
        cprintf("%s", p->name);
        putBlank_String(9, p->name);
        cprintf("%d", p->pid);
        putBlank_Int(9, p->pid);
        cprintf("%s", procstate[p->state]);
        putBlank_String(11, procstate[p->state]);
        cprintf("%d", p->nice);
        putBlank_Int(14, p->nice);
        cprintf("%d", p->totalRunTime/weight[p->nice]);
        putBlank_Int(20, p->totalRunTime/weight[p->nice]);
        cprintf("%d", p->totalRunTime);
        putBlank_Int(20, p->totalRunTime);
        if(p->vRunLevel == 0){
          cprintf("%d", p->vRunTime);
        }
        else{
          handleOverflow(p->vRunTime, p->vRunLevel);
        }
        cprintf("\n");
        release(&ptable.lock);
        return;
      }
    }
  }
  release(&ptable.lock);
  return;
}

uint mmap(uint addr, int length, int prot, int flags, int fd, int offset){
  struct proc* pr = myproc();
  struct file* f = 0;
  //cprintf("\n====================================\n");
  //cprintf("\n\nflags:%d\n", flags);

  addr += 0x40000000; //address starts at 0x40000000

  if(fd != -1){
    f = pr->ofile[fd];
    //cprintf("%d\n", f->readable);
    //cprintf("%d\n", f->writable);
  } //if fd != -1, file mapping, get file data and file protection info

  if(fd == -1 && !(flags & MAP_ANONYMOUS)) return 0;
    // not anonymous, but when fd is -1
   
  if(!(flags & MAP_ANONYMOUS)){ 
    //MAP_POPULATE, NONE의 경우, file의 protection 상태 확인해야 함 => file 
    //prot에 readable 있는데 readable 하지 않은 경우나, prot에 writable 있는데 writable 하지 않다면 0 return, fail
    //f->readable, f->writable은 0, 1의 값을 갖는다.  
    if(((prot & PROT_READ) && !(f->readable)) || ((prot & PROT_WRITE) && !(f->writable))){
      cprintf("Error: permission denied\n");
      return 0;
    } 
  }
  
  // 비어있는 mmap_area를 찾는다. (state가 -1인 곳)
  struct mmap_area *cur = &mma[0];
  int idx = 0;
  while(cur->state != -1){
    idx += 1;
    cur = &mma[idx];
  }

  // 비어있는 mmap_area를 찾았으면 이제 거기다가 기록
  if(f==0) mma[idx].f = 0;
  else mma[idx].f = filedup(f); //file duplicate

  mma[idx].addr = addr;
  mma[idx].length = length;
  mma[idx].offset = offset;
  mma[idx].prot = prot;
  mma[idx].flags = flags;
  mma[idx].p = pr;
  mma[idx].state = 0;

  if(flags == 0 || flags == 1){ //file mapping, just record mapping area OR anonymous mapping(handled by page fault handler) => MAP_POPULATE NOT GIVEN
    //cprintf("record mapping\n", flags);
    //cprintf("addr: %d, addr+length : %d\n", addr, addr+length);
    return addr;
  }
  else if(flags == 2){ //file mapping, allocate physical page and make page table for mapping area
    char* newmem;
    f->off = offset; //set file offset to input offset (file.h, file.c 참조)

    for(uint ptr = 0; ptr<length; ptr+=4096){ //4096 = page size of xv6
      newmem = kalloc(); 
      // physical memory allocate, get ptr kernel can use.... (not real allocate yet)
      // page is not continuous, have to get new page address every time

      if(newmem == 0){
        cprintf("error: kalloc failed....\n");
        return 0;
      } //if kalloc fail, return 0

      memset(newmem, 0, 4096); //physical memory allocate 해서 (set to 0)
      fileread(f, newmem, 4096); //file에서 읽어온 내용을 allocate한 physical memory에 복사하기 (아직 page table 안 만들어짐)
      
      if(mappages(pr->pgdir, (void*)(addr+ptr), 4096, V2P(newmem), prot | PTE_U) < 0){
        return 0;
      } //mappages return value : mapped page number
      //cprintf("mmap: #1\n");
    }
    mma[idx].state = 1; //page table now exists
  }
  else if(flags == 3){ //anonymous mapping, allocate physical page and make page table for mapping area
    char* newmem;
    //cprintf("addr: %d, addr+length : %d\n", addr, addr+length);
    for(uint ptr = 0; ptr < length; ptr += 4096){ //4096 = page size of xv6
      newmem = kalloc(); 
      // physical memory allocate, get ptr kernel can use.... (not real allocate yet)
      // page is not continuous, have to get new page address every time

      
      if(newmem == 0){
        cprintf("error: kalloc failed....\n");
        return 0;
      } //if kalloc fail, return 0

      memset(newmem, 0, 4096); //physical memory allocate 
      
      //cprintf("hello! in mmap\n");
      if(mappages(pr->pgdir, (void*)(addr+ptr), 4096, V2P(newmem), prot | PTE_U) < 0){
        
        return 0;
      } //mappages return value : mapped page number
      //cprintf("mmap: #2\n");
    }
    mma[idx].state = 1; //page table now exists
  } 
  
  return addr;
}

int munmap(uint addr){
  int idx;
  struct proc* pr = myproc();

  for(idx = 0; idx < 64; idx++){
    if(mma[idx].addr == addr && mma[idx].p == pr) break;
  }
  //no matching address to free
  if(idx == 64) return -1;


  if(mma[idx].state == -1 || (mma[idx].state == 0)){
    // 실제 allocate된 page table이나 physical memory는 없으므로 destroy mmap_area만 하고 return 1
    mma[idx].addr = 0;
    mma[idx].length = 0;
    mma[idx].offset = 0;
    mma[idx].prot = 0;
    mma[idx].flags = 0;
    mma[idx].p = 0;
    mma[idx].state = -1;
    
    return 1;
  }
  else{
    //cprintf("I am mapped in %d and needed to be destroyed!\n", idx);

    for(uint ptr = 0; ptr < mma[idx].length; ptr += 4096){
      pte_t* pte = walkpgdir(mma[idx].p->pgdir, (char*)(mma[idx].addr+ptr), 0);
      
      if(pte == 0) continue;
      if((*pte & PTE_P) == 0) continue;

      uint pa = PTE_ADDR(*pte);
      kfree((char*)pa + KERNBASE);
    }

    //mmap_area destroy
    mma[idx].addr = 0;
    mma[idx].length = 0;
    mma[idx].offset = 0;
    mma[idx].prot = 0;
    mma[idx].flags = 0;
    mma[idx].p = 0;
    mma[idx].state = -1;
  }
  return 1;

}

int freemem(void){
  int result = getFreeMemCNT();
  return result;
}

int pageFaultHandler(uint err, uint regAddr){ //regAddr : page fault occurred address
  struct proc* pr = myproc();
  struct file* fi = 0;
  int idx = 0;
  int isWrite = 0;
  char* newmem = 0;


  for(idx = 0; idx < 64; idx++){
    if((mma[idx].addr <= regAddr && regAddr < mma[idx].addr + mma[idx].length) && mma[idx].p == pr) break;
  }
  if(idx == 64) return -1;

  //cprintf("regAddr: %d\n", regAddr);
  //cprintf("idx: %d, mma addr: %d, mma addr end: %d\n", idx, mma[idx].addr, mma[idx].addr+mma[idx].length);

  if((mma[idx].prot & PROT_WRITE) && ((err&2)!=0)) isWrite = 1;
  else if(!(mma[idx].prot & PROT_WRITE) && ((err&2)==2)) return -1; // write = 1?

  

  if(mma[idx].state == -1) return -1;
  //mapping area만 등록, 실제 allocate는 X인 상태 -> map populate가 없는, flag=0, flag=1인 상태에서만 발생
  
  newmem = kalloc();
  if(newmem == 0) return -1;

  memset(newmem, 0, 4096);

 
  if(!(mma[idx].flags & MAP_ANONYMOUS)){
    fi = mma[idx].f;
    fi->off = mma[idx].offset + ((regAddr-mma[idx].addr) / 4096 * 4096);

    fileread(fi, newmem, 4096);
  }
  
  if(isWrite){
    if(mappages(pr->pgdir, (void*)(mma[idx].addr + (regAddr-mma[idx].addr)/4096 * 4096), 4096, V2P(newmem), mma[idx].prot | PTE_U | PTE_W) < 0){
      return -1;
    }
    //cprintf("page fault: #1\n");
  }
  else{
    if(mappages(pr->pgdir, (void*)(mma[idx].addr + (regAddr-mma[idx].addr)/4096 * 4096), 4096, V2P(newmem), mma[idx].prot | PTE_U) < 0){
      return -1;
    }
    //cprintf("page fault: #2\n");
  }
  
  mma[idx].state = 1;
  return 0;
}
  
  




