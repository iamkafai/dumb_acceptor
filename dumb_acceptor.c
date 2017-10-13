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

#define NSEC_PER_SEC 1000000000ull
#define TEST_RESTART_NSEC (NSEC_PER_SEC * 2)
#define TIME_UPDATE_LOWAT (1024)

union {
	uint32_t v4;
	uint32_t v6[4];
} start_full_addr; /* network order */	/* -i */
static uint32_t nr_threads = 1;		/* -t */
static uint32_t nr_ports = 100;		/* -P */
static uint32_t nr_addrs = 1;		/* -I */
static uint16_t start_port = 443;	/* -p */
static uint32_t budget_per_sock = 64;	/* -b */
static int backlog = 64;		/* -q */
static int avoid_so_reuse;		/* -R */
static uint16_t end_port;
static uint32_t start_addr; /* host order */
static uint32_t end_addr;   /* host order */
static uint32_t nr_socks_per_thread;
static int family = AF_UNSPEC;
static int stopped;
static int nr_cpus;
char addr_str[64];

struct thread_info {
	uint64_t	nr_accepts;
	uint64_t	start_ns;
	uint64_t	last_ns;
	pthread_t	tid;
	int		cpu;
	int		ret;
	int		done_listening;
};

struct thread_info *threads;

#define THD_ERR ((void *) (long) -1)
#define THD_OK  ((void *) (long) 0)
void *worker_func(void *arg)
{
	int i, my_id, fd_flags, epfd = -1, ret = 0;
	uint64_t nr_accepts = 0;
	struct epoll_event *evs;
	cpu_set_t cpuset;
	int *lfds = NULL;
	char estr[128];
	union {
		struct sockaddr_in v4;
		struct sockaddr_in6 v6;
	} saddr;
	uint64_t start_ns = 0;
	uint64_t last_ns = 0;

	my_id = (int) (long) arg;
	CPU_ZERO(&cpuset);
	CPU_SET(threads[my_id].cpu, &cpuset);
	if (pthread_setaffinity_np(threads[my_id].tid, sizeof(cpuset), &cpuset)) {
		fprintf(stderr, "sched_setaffinity(): %s(%d)\n",
			strerror_r(errno, estr, sizeof(estr)),
			errno);
		return THD_ERR;
	}

	lfds = calloc(nr_socks_per_thread, sizeof(*lfds));
	if (!lfds)
		return THD_ERR;

	evs = calloc(nr_socks_per_thread, sizeof(*evs));
	if (!evs) {
		free(lfds);
		return THD_ERR;
	}

	epfd = epoll_create1(0);
	if (epfd == -1) {
		fprintf(stderr, "epoll_create1(0): %s(%d)\n",
			strerror_r(errno, estr, sizeof(estr)),
			errno);
		free(lfds);
		free(evs);
		return THD_ERR;
	}

	for (i = 0; i < nr_socks_per_thread; i++)
		lfds[i] = -1;

	i = 0;
	init_saddr((struct sockaddr *)&saddr, family,
		   (uint32_t *)&start_full_addr, start_port);
	do {
		struct epoll_event ev = {};
		int opt = 1;

		assert(i < nr_socks_per_thread);
		lfds[i] = socket(family, SOCK_STREAM, 0);
		if (lfds[i] == -1) {
			fprintf(stderr, "socket(): %s(%d)\n",
				strerror_r(errno, estr, sizeof(estr)),
				errno);
			ret = lfds[i];
			goto done;
		}

		fd_flags = fcntl(lfds[i], F_GETFL, 0);
		if (fd_flags == -1) {
			fprintf(stderr, "fcntl(F_GETFL): %s(%d)\n",
				strerror_r(errno, estr, sizeof(estr)),
				errno);
			ret = fd_flags;
			goto done;
		}
		ret = fcntl(lfds[i], F_SETFL, fd_flags | O_NONBLOCK);
		if (ret == -1) {
			fprintf(stderr, "fcntl(F_SETFL, O_NONBLOCK): %s(%d)\n",
				strerror_r(errno, estr, sizeof(estr)),
				errno);
			goto done;
		}

		ret = setsockopt(lfds[i], SOL_SOCKET, SO_REUSEADDR, &opt,
				 sizeof(opt));
		if (ret) {
			fprintf(stderr, "setsockopt(SO_REUSEADDR): %s(%d)\n",
				strerror_r(errno, estr, sizeof(estr)),
				errno);
			goto done;
		}

		if (nr_threads > 1 || !avoid_so_reuse) {
			ret = setsockopt(lfds[i], SOL_SOCKET, SO_REUSEPORT, &opt,
					 sizeof(opt));
			if (ret) {
				fprintf(stderr, "setsockopt(SO_REUSE_PORT): %s(%d)\n",
					strerror_r(errno, estr, sizeof(estr)),
					errno);
				goto done;
			}
		}

		ret = bind(lfds[i], (struct sockaddr *)&saddr,
			   get_address_len(family));
		if (ret) {
			char tmp_addr_str[64];
			uint16_t tmp_port;

			if (family == AF_INET) {
				inet_ntop(AF_INET, &saddr.v4.sin_addr,
					  tmp_addr_str, sizeof(tmp_addr_str));
				tmp_port = ntohs(saddr.v4.sin_port);
			} else {
				inet_ntop(AF_INET6, &saddr.v6.sin6_addr,
					  tmp_addr_str, sizeof(tmp_addr_str));
				tmp_port = ntohs(saddr.v6.sin6_port);
			}

			fprintf(stderr, "bind([%s]:%u): %s(%d)\n",
				tmp_addr_str, tmp_port,
				strerror_r(errno, estr, sizeof(estr)),
				errno);
			close(lfds[i]);
			lfds[i] = -1;
			continue;
		}

		ret = listen(lfds[i], 64);
		if (ret) {
			fprintf(stderr, "listen(): %s(%d)\n",
				strerror_r(errno, estr, sizeof(estr)),
				errno);
			goto done;
		}


		ev.events = EPOLLIN;
		ev.data.fd = lfds[i];
		ret = epoll_ctl(epfd, EPOLL_CTL_ADD, lfds[i], &ev);
		if (ret) {
			fprintf(stderr, "epoll_ctl(EPOLL_CTL_ADD): %s(%d)\n",
				strerror_r(errno, estr, sizeof(estr)),
				errno);
			goto done;
		}
		i++;
	} while (!get_next_saddr((struct sockaddr *)&saddr, family,
				 start_addr, end_addr,
				 start_port, end_port));

	threads[my_id].done_listening = 1;

	while (!stopped) {
		uint64_t old_accepts = nr_accepts;
		uint64_t now_ns;
		int nr_evs;
		int new_fd;
		int budget;

		nr_evs = epoll_wait(epfd, evs, nr_socks_per_thread, 1);
		if (nr_evs == -1) {
			assert (errno == -EINTR);
			continue;
		}

		for (i = 0; i < nr_evs; i++) {
			if (evs[i].events & ~EPOLLIN)
				fprintf(stderr, "epoll_wait() has non EPOLLIN events:0x%x\n",
					evs[i].events);

			budget = budget_per_sock;
			while (budget--) {
				new_fd = accept4(evs[i].data.fd, NULL, NULL,
						 SOCK_NONBLOCK);
				if (new_fd == -1 && errno != EAGAIN)
					fprintf(stderr, "accept(): %s(%d)\n",
						strerror_r(errno, estr, sizeof(estr)),
						errno);
				if (new_fd == -1)
					break;
				close(new_fd);
				nr_accepts++;
			}
		}

		if (nr_accepts == old_accepts)
			continue;

		now_ns = time_get_ns();
		if (now_ns - TEST_RESTART_NSEC > last_ns) {
			start_ns = now_ns;
			nr_accepts = nr_accepts - old_accepts;
		}
		last_ns = now_ns;
	}

done:
	if (epfd != -1)
		close(epfd);

	for (i = 0; i < nr_socks_per_thread && lfds[i] != -1; i++)
		close(lfds[i]);

	free(lfds);
	free(evs);

	threads[my_id].nr_accepts = nr_accepts;
	threads[my_id].start_ns = start_ns;
	threads[my_id].last_ns = last_ns;
	threads[my_id].ret = ret;

	return (void *) (long ) ret;
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
	printf("    -q <listen-backlog> Default:64\n");
	printf("    -b <budget-per-listen> Default:64\n");
	printf("    -t <num-of-threads> Default:1\n");
	printf("    -A <cpu-affinity> Default:cpu-num\n");
}

int parse_cpu_affinity(const char *affinity_list)
{
	char *affinity_clone = strdup(affinity_list);
	char *next;
	int i = 0;
	int cpu;

	if (!affinity_clone) {
		fprintf(stderr, "strcpy(affinity_list): %s\n", strerror(errno));
		return -1;
	}

	next = strtok(affinity_clone, ", ");
	while (next) {
		if (i > nr_cpus) {
			free(affinity_clone);
			return 0;
		}

		cpu = atoi(next);
		if (cpu < 0 || cpu >= nr_cpus) {
			free(affinity_clone);
			fprintf(stderr, "Invalid cpu:%d\n", cpu);
			return -1;
		}

		threads[i++].cpu = cpu;
		next = strtok(NULL, ", ");
	}

	free(affinity_clone);
	return 0;
}

int parse_args(int argc, char **argv)
{
	const char *optstr = "i:p:I:P:q:b:t:A:R";
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
		case 'q':
			backlog = atoi(optarg);
			break;
		case 'b':
			budget_per_sock = atoi(optarg);
			break;
		case 't':
			nr_threads = atoi(optarg);
			break;
		case 'A':
			parse_cpu_affinity(optarg);
			break;
		case 'R':
			avoid_so_reuse = 1;
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

	nr_socks_per_thread = nr_addrs * nr_ports;

	printf("Address: %s (0x%08X -> 0x%08X: %u)\n", addr_str, start_addr,
	       end_addr, nr_addrs);
	printf("Port: %u -> %u: %u\n", start_port, end_port, nr_ports);
	printf("Listen backlog: %u\n", backlog);
	printf("Budget per listen sock: %u\n", budget_per_sock);
	printf("Number of threads: %u\n", nr_threads);
	printf("Number of listen socks per thread: %u\n", nr_socks_per_thread);

	return 0;
}

int main(int argc, char **argv)
{
	uint64_t total_rate = 0;
	uint32_t max_nr_files;
	struct rlimit r;
	int ret = 0, i;
	sigset_t sigset;
	char estr[128];

	nr_cpus = sysconf(_SC_NPROCESSORS_CONF);
	if (nr_cpus == -1) {
		perror("sysconf(_SC_NPROCESSORS_CONF)");
		return -1;
	}

	threads = calloc(nr_cpus, sizeof(*threads));
	if (!threads) {
		perror("calloc(nr_threads)");
		return -1;
	}
	for (i = 0; i < nr_cpus; i++)
		threads[i].cpu = i;

	ret = parse_args(argc, argv);
	if (ret) {
		free(threads);
		return ret;
	}

	max_nr_files = (nr_socks_per_thread * nr_threads) +
		/* a buffer fds for accept() */
		(nr_threads * 10);

#if 0
	r.rlim_cur = max_nr_files;
	r.rlim_max = max_nr_files;
	if (setrlimit(RLIMIT_NOFILE, &r)) {
		perror("setrlimit(RLIMIT_MEMLOCK, RLIM_INFINITY)");
		free(threads);
		return -1;
	}
#endif

	sigemptyset(&sigset);
	sigaddset(&sigset, SIGINT);
	sigaddset(&sigset, SIGQUIT);
	sigaddset(&sigset, SIGTERM);
	pthread_sigmask(SIG_BLOCK, &sigset, NULL);

	for (i = 0; i < nr_threads; i++) {
		int nr_wait_s = 0;

		ret = pthread_create(&threads[i].tid, NULL, worker_func,
				     (void *)(long)i);
		if (ret == -1) {
			fprintf(stderr, "Failed to start thread:%u. %s(%d)\n",
				i, strerror_r(errno, estr, sizeof(estr)),
				errno);
			threads[i].tid = 0;
			goto done;
		}

		while (nr_wait_s <= 2500) {
			if (threads[i].done_listening)
				break;
			if (threads[i].ret)
				goto done;
			usleep(2000);
			nr_wait_s++;
		}

		if (!threads[i].done_listening) {
			fprintf(stderr, "Thread#%u not done listening after 5s\n", i);
			goto done;
		}
	}

	printf("Parent pid: %u\n", getpid());

	while (1) {
		int signum = 0;

		ret = sigwait(&sigset, &signum);
		assert(!ret);
		break;
	}

done:
	stopped = 1;
	printf("Waiting for threads to die......\n");
	for (i = 0; i < nr_threads; i++) {
		if (threads[i].tid) {
			struct timespec ts;
			uint64_t rate = 0;

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
			if (threads[i].last_ns > threads[i].start_ns)
				rate = (threads[i].nr_accepts * NSEC_PER_SEC) / (threads[i].last_ns - threads[i].start_ns);
			total_rate += rate;
			printf("Threads#%u accepted:%lu in %llu seconds, rate:%lu\n",
			       i, threads[i].nr_accepts,
			       (threads[i].last_ns - threads[i].start_ns) / NSEC_PER_SEC,
			       rate);
		} else {
			break;
		}
	}

	printf("Total rate: %lu\n", total_rate);

	free(threads);
	return ret;
}
