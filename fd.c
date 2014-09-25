#include "taskimpl.h"
#include <fcntl.h>

#define MAXFD  10240

static Tasklist sleeping;
static int sleepingcounted;
static int startedfdtask;

static uvlong nsec(void);

#ifdef __linux__
#define USE_EPOLL 1
#endif

/*
 * can not using linux's epoll
 */
#ifndef USE_EPOLL

#include <sys/poll.h>

static Task *polltask[MAXFD];
static struct pollfd pollfd[MAXFD];
static int npollfd;


void
fdtask(void *v)
{
	int i, ms;
	Task *t;
	uvlong now;
	
	tasksystem();
	taskname("fdtask");
	for(;;){
		/* let everyone else run */
		while(taskyield() > 0)
			;
		/* we're the only one runnable - poll for i/o */
		errno = 0;
		taskstate("poll");
		if((t=sleeping.head) == nil)
			ms = -1;
		else{
			/* sleep at most 5s */
			now = nsec();
			if(now >= t->alarmtime)
				ms = 0;
			else if(now+5*1000*1000*1000LL >= t->alarmtime)
				ms = (t->alarmtime - now)/1000000;
			else
				ms = 5000;
		}
		if(poll(pollfd, npollfd, ms) < 0){
			if(errno == EINTR)
				continue;
			fprint(2, "poll: %s\n", strerror(errno));
			taskexitall(0);
		}

		/* wake up the guys who deserve it */
		for(i=0; i<npollfd; i++){
			while(i < npollfd && pollfd[i].revents){
				taskready(polltask[i]);
				--npollfd;
				pollfd[i] = pollfd[npollfd];
				polltask[i] = polltask[npollfd];
			}
		}
		
		now = nsec();
		while((t=sleeping.head) && now >= t->alarmtime){
			deltask(&sleeping, t);
			if(!t->system && --sleepingcounted == 0)
				taskcount--;
			taskready(t);
		}
	}
}

void
fdwait(int fd, int rw)
{
	int bits;

	if(!startedfdtask){
		startedfdtask = 1;
		taskcreate(fdtask, 0, 32768);
	}

	if(npollfd >= MAXFD){
		fprint(2, "too many poll file descriptors\n");
		abort();
	}
	
	taskstate("fdwait for %s", rw=='r' ? "read" : rw=='w' ? "write" : "error");
	bits = 0;
	switch(rw){
	case 'r':
		bits |= POLLIN;
		break;
	case 'w':
		bits |= POLLOUT;
		break;
	}

	polltask[npollfd] = taskrunning;
	pollfd[npollfd].fd = fd;
	pollfd[npollfd].events = bits;
	pollfd[npollfd].revents = 0;
	npollfd++;
	taskswitch();
}

#else  /* using epoll interface */

#include <sys/epoll.h>

struct epollfd_context {
	Task *task;
	int bits;
	int set;
};

static int epfd;
static struct epollfd_context pollfd[MAXFD];
static struct epoll_event events[MAXFD];

void
fdtask(void *v)
{
	int i, ms;
	Task *t;
	uvlong now;
	struct epoll_event ev;
	struct epollfd_context *ctx;
	int nevents;

	tasksystem();
	taskname("fdtask");

	for (;;) {
		/* let everyone else run */
		while(taskyield() > 0)
			;
		/* we're the only one runnable - poll for i/o */
		errno = 0;
		taskstate("epoll");

		if ((t=sleeping.head) == nil)
			ms = -1;
		else {
			/* sleep at most 5s */
			now = nsec();
			if(now >= t->alarmtime)
				ms = 0;
			else if (now+5*1000*1000*1000LL >= t->alarmtime)
				ms = (t->alarmtime - now)/1000000;
			else
				ms = 5000;
		}

		nevents = epoll_wait(epfd, events, MAXFD, ms);
		if(nevents < 0) {
			if(errno == EINTR)
				continue;
			fprint(2, "epoll: %s\n", strerror(errno));
			taskexitall(0);
		}

		/* wake up the guys who deserve it */
		for (i = 0; i < nevents; i++) {
			ctx = &pollfd[events[i].data.fd];

			taskready(ctx->task);

			// delete fd from epoll success
			// set context's task, bits and set to zero
			if (0 == epoll_ctl(epfd, EPOLL_CTL_DEL, events[i].data.fd, &ev)) {
				ctx->task = NULL;
				ctx->bits = 0;
				ctx->set = 0;
			}
		}

		now = nsec();
		while((t=sleeping.head) && now >= t->alarmtime) {
			deltask(&sleeping, t);
			if(!t->system && --sleepingcounted == 0)
				taskcount--;
			taskready(t);
		}
	}
}

void
fdwait(int fd, int rw)
{
	int bits;
	int op;
	struct epollfd_context *ctx;
	struct epoll_event ev = {0};
	int ret;

	if (!startedfdtask) {
		startedfdtask = 1;

		epfd = epoll_create(1); // create epoll
		if (epfd < 0) {
			fprint(2, "can not create epoll descriptor\n");
			abort();
		}

		memset(pollfd, 0, sizeof(pollfd)); // set all context to zero

		taskcreate(fdtask, 0, 32768);
	}

	if(fd >= MAXFD) {
		fprint(2, "too many poll file descriptors\n");
		abort();
	}

	taskstate("fdwait for %s", rw=='r' ? "read" : rw=='w' ? "write" : "error");

	ctx = &pollfd[fd];
	if (!ctx->set) {
		op = EPOLL_CTL_ADD;
	} else { // if EPOLL_CTL_DEL operation was failed, then this result occur
		op = EPOLL_CTL_MOD;
	}

	bits = ctx->bits;
	switch(rw) {
	case 'r':
		bits |= EPOLLIN | EPOLLPRI;
		break;
	case 'w':
		bits |= EPOLLOUT;
		break;
	}

	ev.data.fd = fd;
	ev.events = bits;

	ret = epoll_ctl(epfd, op, fd, &ev);
	if (ret < 0) {
		return;
	}

	// set context
	ctx->task = taskrunning;
	ctx->bits = bits;
	ctx->set = 1;

	taskswitch();
}

#endif


uint
taskdelay(uint ms)
{
	uvlong when, now;
	Task *t;
	
	if(!startedfdtask){
		startedfdtask = 1;
#ifdef USE_EPOLL
        epfd = epoll_create(1);
        if (epfd < 0) {
        	fprint(2, "can not create epoll descriptor\n");
			abort();
        }
        memset(pollfd, 0, sizeof(pollfd)); // set all context to zero
#endif
		taskcreate(fdtask, 0, 32768);
	}

	now = nsec();
	when = now+(uvlong)ms*1000000;
	for(t=sleeping.head; t!=nil && t->alarmtime < when; t=t->next)
		;

	if(t){
		taskrunning->prev = t->prev;
		taskrunning->next = t;
	}else{
		taskrunning->prev = sleeping.tail;
		taskrunning->next = nil;
	}
	
	t = taskrunning;
	t->alarmtime = when;
	if(t->prev)
		t->prev->next = t;
	else
		sleeping.head = t;
	if(t->next)
		t->next->prev = t;
	else
		sleeping.tail = t;

	if(!t->system && sleepingcounted++ == 0)
		taskcount++;
	taskswitch();

	return (nsec() - now)/1000000;
}

/* Like fdread but always calls fdwait before reading. */
int
fdread1(int fd, void *buf, int n)
{
	int m;
	
	do
		fdwait(fd, 'r');
	while((m = read(fd, buf, n)) < 0 && errno == EAGAIN);
	return m;
}

int
fdread(int fd, void *buf, int n)
{
	int m;
	
	while((m=read(fd, buf, n)) < 0 && errno == EAGAIN)
		fdwait(fd, 'r');
	return m;
}

int
fdwrite(int fd, void *buf, int n)
{
	int m, tot;
	
	for(tot=0; tot<n; tot+=m){
		while((m=write(fd, (char*)buf+tot, n-tot)) < 0 && errno == EAGAIN)
			fdwait(fd, 'w');
		if(m < 0)
			return m;
		if(m == 0)
			break;
	}
	return tot;
}

int
fdnoblock(int fd)
{
	return fcntl(fd, F_SETFL, fcntl(fd, F_GETFL)|O_NONBLOCK);
}

static uvlong
nsec(void)
{
	struct timeval tv;

	if(gettimeofday(&tv, 0) < 0)
		return -1;
	return (uvlong)tv.tv_sec*1000*1000*1000 + tv.tv_usec*1000;
}

