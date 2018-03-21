#include "types.h"
#include "defs.h"
#include "user.h"

int print_ppid(){
	cprintf("My pid is %d\n",getpid());
	cprintf("My ppid is %d\n", getppid());
	return 0;
}
