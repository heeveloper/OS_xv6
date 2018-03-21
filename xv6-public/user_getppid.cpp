#include "types.h"
#include "stat.h"
#include "user.h"
#include "proc.h"

int main(){
	cprintf("My pid is %d\n",getpid());
	cprintf("My ppid is %d\n", getppid());
	exit();
}
