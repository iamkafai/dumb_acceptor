#define _GNU_SOURCE
#include <sched.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include "util.h"

static union {
	uint32_t v4;
	uint32_t v6[4];
} start_full_addr; /* network order */	/* -i */
static uint32_t nr_threads = 1;		/* -t */
static uint32_t nr_ports = 100;		/* -P */
static uint32_t nr_addrs = 1;		/* -I */
static uint16_t start_port = 443;	/* -p */
static uint64_t test_len_s = 15;	/* -l */
static uint32_t batch_size = 64;	/* -b */
static uint16_t end_port;
static uint32_t start_addr; /* host order */
static uint32_t end_addr;   /* host order */
static int family = AF_UNSPEC;
static int stopped;
static int nr_cpus;
char addr_str[64];

struct thread_info {
	uint64_t	nr_connects;
	pthread_t	tid;
	int		cpu;
	int		ret;
};

struct thread_info *threads;

#define THD_ERR ((void *) (long) -1)
#define THD_OK  ((void *) (long) 0)

int non_block_connect(const struct sockaddr *saddr, socklen_t socklen,
		      int epfd, int ev_data)
{
	struct epoll_event ev = {};
	char estr[128];
	int fd_flags;
	int cfd;
	int ret;

	cfd = socket(family, SOCK_STREAM, 0);
	if (cfd == -1) {
		fprintf(stderr, "socket(): %s(%d)\n",
			strerror_r(errno, estr, sizeof(estr)),
			errno);
		return cfd;
	}

	fd_flags = fcntl(cfd, F_GETFL, 0);
	if (fd_flags == -1) {
		fprintf(stderr, "fcntl(F_GETFL): %s(%d)\n",
			strerror_r(errno, estr, sizeof(estr)),
			errno);
		goto error;
	}

	ret = fcntl(cfd, F_SETFL, fd_flags | O_NONBLOCK);
	if (ret == -1) {
		fprintf(stderr, "fcntl(F_SETFL, O_NONBLOCK): %s(%d)\n",
			strerror_r(errno, estr, sizeof(estr)),
			errno);
		goto error;
	}


	ret = connect(cfd, (struct sockaddr *)saddr, socklen);
	if (ret == -1 && errno != EINPROGRESS) {
		fprintf(stderr, "connect(): %s(%d)\n",
			strerror_r(errno, estr, sizeof(estr)),
			errno);
		goto error;
	}

	ev.events = EPOLLOUT | EPOLLERR;
	if (ev_data < 0)
		ev.data.fd = cfd;
	else
		ev.data.u32 = ev_data;

	ret = epoll_ctl(epfd, EPOLL_CTL_ADD, cfd, &ev);
	if (ret == -1) {
		fprintf(stderr, "epoll_ctl(EPOLL_CTL_ADD): %s(%d)\n",
			strerror_r(errno, estr, sizeof(estr)),
			errno);
		goto error;
	}

	return cfd;

error:
	close(cfd);
	return -1;
}


void *worker_func(void *arg)
{
	int i, cpu, epfd = -1, ret = 0;
	struct epoll_event *evs = NULL;
	uint64_t nr_connects = 0;
	uint32_t nr_failed =  0;
	cpu_set_t cpuset;
	int *cfds = NULL;
	char estr[128];
	union {
		struct sockaddr_in v4;
		struct sockaddr_in6 v6;
	} saddr;

	cpu = (int) (long) arg;
	CPU_ZERO(&cpuset);
	CPU_SET(cpu, &cpuset);
	if (pthread_setaffinity_np(threads[cpu].tid, sizeof(cpuset), &cpuset)) {
		fprintf(stderr, "sched_setaffinity(): %s(%d)\n",
			strerror_r(errno, estr, sizeof(estr)),
			errno);
		return THD_ERR;
	}

	cfds = calloc(batch_size, sizeof(*cfds));
	evs = calloc(batch_size, sizeof(*evs));
	if (!cfds || !evs) {
		free(cfds);
		free(evs);
		return THD_ERR;
	}

	for (i = 0; i < batch_size; i++)
		cfds[i] = -1;

	epfd = epoll_create1(0);
	if (epfd == -1) {
		fprintf(stderr, "epoll_create1(0): %s(%d)\n",
			strerror_r(errno, estr, sizeof(estr)),
			errno);
		free(cfds);
		free(evs);
		return THD_ERR;
	}

	init_saddr((struct sockaddr *)&saddr, family,
		   (uint32_t *)&start_full_addr, start_port);

	for (i = 0; i < batch_size; i++) {
		ret = non_block_connect((struct sockaddr *)&saddr,
					get_address_len(family),
					epfd, i);
		assert(ret != -1);
		get_next_saddr((struct sockaddr *)&saddr, family,
			       start_addr, nr_addrs, start_port, nr_ports);
		if (ret == -1)
			nr_failed++;
		else
			cfds[i] = ret;
	}

	while (!stopped) {
		int nr_evs = epoll_wait(epfd, evs, batch_size, 1);

		if (nr_evs == -1) {
			assert (errno == -EINTR);
			continue;
		}

		for (i = 0; i < nr_evs; i++) {
			int idx = evs[i].data.u32;
			int cfd = cfds[idx];

			nr_connects++;

#if 0
			if (evs[i].events & EPOLLERR) {
				int result;
				socklen_t result_len = sizeof(result);

				if (!getsockopt(cfd, SOL_SOCKET, SO_ERROR,
						&result, &result_len))
					fprintf(stderr, "Error in connect:%s(%d)\n",
						strerror_r(result, estr, sizeof(estr)),
						result);
				else
					fprintf(stderr, "Error in getsockopt(SO_ERROR):%s(%d)\n",
						strerror_r(errno, estr, sizeof(estr)),
						errno);
			}
#endif

			epoll_ctl(epfd, EPOLL_CTL_DEL, cfd, NULL);
			close(cfd);
			cfds[idx] = -1;

			ret = non_block_connect((struct sockaddr *)&saddr,
						get_address_len(family), epfd,
						idx);
			get_next_saddr((struct sockaddr *)&saddr, family,
				       start_addr, nr_addrs,
				       start_port, nr_ports);
			if (ret == -1)
				nr_failed++;
			else
				cfds[idx] = ret;
		}

		if (!nr_failed)
			continue;

		printf("nr_failed:%u\n", nr_failed);

		/* We have failure before.  It should not
		 * be common case.  Hence, we just scan the
		 * whole cfds[], look for -1 and then retry.
		 */
		for (i = 0; i < batch_size; i++) {
			if (cfds[i] != -1)
				continue;

			ret = non_block_connect((struct sockaddr *)&saddr,
						get_address_len(family),
						epfd, i);
			get_next_saddr((struct sockaddr *)&saddr, family,
				       start_addr, nr_addrs,
				       start_port, nr_ports);
			if (ret != -1) {
				cfds[i] = ret;
				nr_failed--;
			}
		}
	}

	free(cfds);
	free(evs);
	threads[cpu].nr_connects = nr_connects;

	return (void *) (long ) 0;
}

static int parse_ipstr(const char *ipstr)
{
	memset(&start_full_addr, 0, sizeof(start_full_addr));

	if (inet_pton(AF_INET6, ipstr, start_full_addr.v6) == 1) {
		start_addr = ntohl(start_full_addr.v6[3]);
		strncpy(addr_str, ipstr, sizeof(addr_str));
		return AF_INET6;
	} else if (inet_pton(AF_INET, ipstr, &start_full_addr.v4) == 1) {
		start_addr = ntohl(start_full_addr.v4);
		strncpy(addr_str, ipstr, sizeof(addr_str));
		return AF_INET;
	}

	fprintf(stderr, "Invalid IP:%s\n", ipstr);
	return AF_UNSPEC;
}

void usage(const char *cmd)
{
	printf("Usage: %s\n", cmd);
	printf("    -i <start-address> Support IPv4 and IPv6\n");
	printf("    -I <num-of-addresses>  Default:1\n");
	printf("    -p <start-port> Default:443\n");
	printf("    -P <num-of-ports> Default:100\n");
	printf("    -b <batch-size> Default:64\n");
	printf("    -t <num-of-threads> Default:1\n");
}

int parse_args(int argc, char **argv)
{
	const char *optstr = "i:p:I:P:t:l:b:";
	int opt;

	while ((opt = getopt(argc, argv, optstr)) != -1) {
		switch (opt) {
		case 'i':
			family = parse_ipstr(optarg);
			if (family == AF_UNSPEC)
				return -1;
			break;
		case 'I':
			nr_addrs = atoi(optarg);
			break;
		case 'p':
			start_port = atoi(optarg);
			break;
		case 'P':
			nr_ports = atoi(optarg);
			break;
		case 't':
			nr_threads = atoi(optarg);
			break;
		case 'l':
			test_len_s = atoi(optarg);
			break;
		case 'b':
			batch_size = atoi(optarg);
			break;
		default:
			usage(argv[0]);
			return -1;
		}
	}

	if (!nr_addrs) {
		fprintf(stderr, "Invalid nr_addrs:%u\n", nr_addrs);
		return -1;
	}
	if ((uint64_t)start_addr + nr_addrs >= UINT32_MAX) {
		fprintf(stderr, "start_addr:%s + %u >= %u\n",
			addr_str, nr_addrs, UINT32_MAX);
		return -1;
	}
	end_addr = (nr_addrs - 1) + start_addr;

	if (!nr_ports) {
		fprintf(stderr, "Invalid nr_ports:%u\n", nr_ports);
		return -1;
	}
	if ((uint32_t)start_port + nr_ports > UINT16_MAX) {
		fprintf(stderr, "start_port:%u + %u >== %u\n",
			start_port, nr_ports, UINT16_MAX);
		return -1;
	}
	end_port = (nr_ports - 1) + start_port;

	if (nr_threads > nr_cpus) {
		fprintf(stderr, "nr_threads:%u > nr_cpus:%u\n",
			nr_threads, nr_cpus);
		return -1;
	}

	printf("Address: %s (0x%08X -> 0x%08X: %u)\n", addr_str, start_addr,
	       end_addr, nr_addrs);
	printf("Port: %u -> %u: %u\n", start_port, end_port, nr_ports);
	printf("Number of threads: %u\n", nr_threads);

	return 0;
}

int main(int argc, char **argv)
{
	uint64_t total_connects = 0;
	int ret = 0, i;
	sigset_t sigset;
	char estr[128];

	nr_cpus = sysconf(_SC_NPROCESSORS_CONF);
	if (nr_cpus == -1) {
		perror("sysconf(_SC_NPROCESSORS_CONF)");
		return -1;
	}

	ret = parse_args(argc, argv);
	if (ret)
		return ret;

	threads = calloc(nr_threads, sizeof(*threads));
	if (!threads) {
		perror("calloc(nr_threads)");
		return -1;
	}

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGQUIT);
	pthread_sigmask(SIG_BLOCK, &sigset, NULL);

	for (i = 0; i < nr_threads; i++) {
		threads[i].cpu = i;
		ret = pthread_create(&threads[i].tid, NULL, worker_func,
				     (void *)(long)i);
		if (ret == -1) {
			fprintf(stderr, "Failed to start thread:%u. %s(%d)\n",
				i, strerror_r(errno, estr, sizeof(estr)),
				errno);
			threads[i].tid = 0;
			goto done;
		}
	}

	printf("Parent pid: %u\n", getpid());

	while (1) {
		struct timespec ts;
		siginfo_t siginfo;

		ts.tv_sec = test_len_s;
		ts.tv_nsec = 0;
		ret = sigtimedwait(&sigset, &siginfo, &ts);
		if (ret)
			assert(ret != EAGAIN);
		break;
	}

done:
	stopped = 1;
	printf("Waiting for threads to die......\n");
	for (i = 0; i < nr_threads; i++) {
		if (threads[i].tid) {
			struct timespec ts;

			if (clock_gettime(CLOCK_REALTIME, &ts)) {
				perror("clock_gettime(CLOCK_REALTIME)");
				break;
			}

			ts.tv_sec += 5;
			ret = pthread_timedjoin_np(threads[i].tid, NULL, &ts);
			if (ret != 0) {
				fprintf(stderr, "Thread:%d does not die after 5s. Bailing.\n",
					threads[i].cpu);
				break;
			}
			total_connects += threads[i].nr_connects;
			printf("Threads#%u connected: %lu\n",
			       i, threads[i].nr_connects);
		} else {
			break;
		}
	}

	printf("Total rate: %lu\n", total_connects / test_len_s);
	free(threads);
	return ret;
}
