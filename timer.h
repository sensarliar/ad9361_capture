/* gcc -Wall -D_REENTRANT -D_POSIX_TIMERS timer.c -o timer -lrt */

#include <time.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include <unistd.h>
#include <pthread.h>


#include <sys/syscall.h>
#include <sys/types.h>

void timer_start (void);
void timer_stop (void);
//void timerxx_t (union sigval sigval);
void timer_set(void);

/*
int timer_nr;
timer_t timer;

int main (void)
{
  //  for (timer_nr = 0;; timer_nr++)
    //{
        start ();
        sleep (10);
        stop ();
    //}

    return EXIT_SUCCESS;
}
*/

