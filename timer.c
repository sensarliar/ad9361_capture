/* gcc -Wall -D_REENTRANT -D_POSIX_TIMERS timer.c -o timer -lrt */

#include "timer.h"
#include "ad9361-capture.h"

int timer_nr;
timer_t timer;

void timer_start (void)
{

    struct sigevent sigev;

    memset (&sigev, 0, sizeof (struct sigevent));
    sigev.sigev_value.sival_int = timer_nr;
    sigev.sigev_notify = SIGEV_THREAD;//THREAD;//SIGNAL
    sigev.sigev_notify_attributes = NULL;
    sigev.sigev_notify_function = tx_dirty_data;//timerxx_t;

    if (timer_create (CLOCK_REALTIME, &sigev, &timer) < 0)
    {
printf ("cant print anything!");
        fprintf (stderr, "[%d]: %s\n", __LINE__, strerror (errno));
        exit (errno);
    }


}

void timer_set(void)
{
    struct itimerspec itimer = { { 0, 8000000 }, { 0, 8000000 } };
	    if (timer_settime (timer, 0, &itimer, NULL) < 0)
    {
        fprintf (stderr, "[%d]: %s\n", __LINE__, strerror (errno));
        exit (errno);
    }
}


void timer_stop (void)
{
    if (timer_delete (timer) < 0)
    {
        fprintf (stderr, "[%d]: %s\n", __LINE__, strerror (errno));
        exit (errno);
    }
}

void timerxx_t (union sigval sigval)
{
    printf ("timer_nr=%02d  pid=%d  pthread_self=%ld\n",
            sigval.sival_int, getpid (), pthread_self ());
pid_t tid = (pid_t) syscall (SYS_gettid);
 printf ("in PID:[%d]TID:[%d]\n",getpid(),tid);
}
