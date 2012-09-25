/*
 * Copyright (C) 2009-2011 Nippon Telegraph and Telephone Corporation.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License version
 * 2 as published by the Free Software Foundation.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../include/config.h"

#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <signal.h>
#include <linux/limits.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/signalfd.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <fcntl.h>
#include <errno.h>

#include "sheep_priv.h"
#include "trace/trace.h"

#define EPOLL_SIZE 4096
#define DEFAULT_OBJECT_DIR "/tmp"
#define LOG_FILE_NAME "sheep.log"

LIST_HEAD(cluster_drivers);
static char program_name[] = "sheep";

static struct option const long_options[] = {
	{"cluster", required_argument, NULL, 'c'},
	{"debug", no_argument, NULL, 'd'},
	{"foreground", no_argument, NULL, 'f'},
	{"gateway", no_argument, NULL, 'g'},
	{"help", no_argument, NULL, 'h'},
	{"journal", no_argument, NULL, 'j'},
	{"loglevel", required_argument, NULL, 'l'},
	{"myaddr", required_argument, NULL, 'y'},
	{"stdout", no_argument, NULL, 'o'},
	{"port", required_argument, NULL, 'p'},
	{"disk-space", required_argument, NULL, 's'},
	{"zone", required_argument, NULL, 'z'},
	{"pidfile", required_argument, NULL, 'P'},
	{"write-cache", required_argument, NULL, 'w'},
	{NULL, 0, NULL, 0},
};

static const char *short_options = "c:dDfghjl:op:P:s:w:y:z:";

static void usage(int status)
{
	if (status)
		fprintf(stderr, "Try `%s --help' for more information.\n",
			program_name);
	else
		printf("\
Sheepdog daemon (version %s)\n\
Usage: %s [OPTION]... [PATH]\n\
Options:\n\
  -c, --cluster           specify the cluster driver\n\
  -d, --debug             include debug messages in the log\n\
  -f, --foreground        make the program run in the foreground\n\
  -g, --gateway           make the progam run as a gateway mode\n\
  -h, --help              display this help and exit\n\
  -j, --journal           use jouranl to update vdi objects\n\
  -l, --loglevel          specify the level of logging detail\n\
  -o, --stdout            log to stdout instead of shared logger\n\
  -p, --port              specify the TCP port on which to listen\n\
  -P, --pidfile           create a pid file\n\
  -s, --disk-space        specify the free disk space in megabytes\n\
  -y, --myaddr            specify the address advertised to other sheep\n\
  -z, --zone              specify the zone id\n\
  -w, --write-cache       specify the cache type\n\
", PACKAGE_VERSION, program_name);
	exit(status);
}

static void sdlog_help(void)
{
	printf("\
Available log levels:\n\
  #    Level           Description\n\
  0    SDOG_EMERG      system has failed and is unusable\n\
  1    SDOG_ALERT      action must be taken immediately\n\
  2    SDOG_CRIT       critical conditions\n\
  3    SDOG_ERR        error conditions\n\
  4    SDOG_WARNING    warning conditions\n\
  5    SDOG_NOTICE     normal but significant conditions\n\
  6    SDOG_INFO       informational notices\n\
  7    SDOG_DEBUG      debugging messages\n");
}

static int create_pidfile(const char *filename)
{
	int fd = -1;
	int len;
	char buffer[128];

	if ((fd = open(filename, O_RDWR|O_CREAT|O_SYNC, 0600)) == -1) {
		return -1;
	}

	if (lockf(fd, F_TLOCK, 0) == -1) {
		close(fd);
		return -1;
	}

	len = snprintf(buffer, sizeof(buffer), "%d\n", getpid());
	if (write(fd, buffer, len) != len) {
		close(fd);
		return -1;
	}

	/* keep pidfile open & locked forever */
	return 0;
}

static int sigfd;

static void signal_handler(int listen_fd, int events, void *data)
{
	struct signalfd_siginfo siginfo;
	int ret;

	ret = read(sigfd, &siginfo, sizeof(siginfo));
	assert(ret == sizeof(siginfo));
	dprintf("signal %d\n", siginfo.ssi_signo);
	switch (siginfo.ssi_signo) {
	case SIGTERM:
		sys->status= SD_STATUS_KILLED;
		break;
	default:
		eprintf("signal %d unhandled \n", siginfo.ssi_signo);
		break;
	}
}

static int init_signal(void)
{
	sigset_t mask;
	int ret;

	ret = trace_init_signal();
	if (ret)
		return ret;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	sigfd = signalfd(-1, &mask, SFD_NONBLOCK);
	if (sigfd < 0) {
		eprintf("failed to create a signal fd: %m\n");
		return -1;
	}

	ret = register_event(sigfd, signal_handler, NULL);
	if (ret) {
		eprintf("failed to register signal handler (%d)\n", ret);
		return -1;
	}

	dprintf("register signal_handler for %d\n", sigfd);

	return 0;
}

static struct cluster_info __sys;
struct cluster_info *sys = &__sys;

static void parse_arg(char *arg, const char *delim, void (*fn)(char *))
{
	char *savep, *s;

	s = strtok_r(arg, delim, &savep);
	do {
		fn(s);
	} while ((s = strtok_r(NULL, delim, &savep)));
}

static void object_cache_size_set(char *s)
{
	const char *header = "size=";
	int len = strlen(header);
	char *size, *p;
	unsigned long cache_size;
	const unsigned long max_cache_size = ULONG_MAX / 1024 / 1024;

	assert(!strncmp(s, header, len));

	size = s + len;
	cache_size = strtol(size, &p, 10);
	if (size == p || cache_size < 0 || max_cache_size < cache_size)
		goto err;

	sys->object_cache_size = cache_size * 1024 * 1024;
	return;

err:
	fprintf(stderr, "Invalid object cache option '%s': "
		"size must be an integer between 0 and %lu\n",
		s, max_cache_size);
	exit(1);
}

static void object_cache_directio_set(char *s)
{
	assert(!strcmp(s, "directio"));
	sys->object_cache_directio = true;
}

static void _object_cache_set(char *s)
{
	int i;
	static int first = 1;

	struct object_cache_arg {
		const char *name;
		void (*set)(char *);
	};

	struct object_cache_arg object_cache_args[] = {
		{ "size=", object_cache_size_set },
		{ "directio", object_cache_directio_set },
		{ NULL, NULL },
	};

	if (first) {
		assert(!strcmp(s, "object"));
		first = 0;
		return;
	}

	for (i = 0; object_cache_args[i].name; i++) {
		const char *n = object_cache_args[i].name;

		if (!strncmp(s, n, strlen(n))) {
			object_cache_args[i].set(s);
			return;
		}
	}

	fprintf(stderr, "invalid object cache arg: %s\n", s);
	exit(1);
}

static void object_cache_set(char *s)
{
	sys->enabled_cache_type |= CACHE_TYPE_OBJECT;
	parse_arg(s, ":", _object_cache_set);
}

static void disk_cache_set(char *s)
{
	assert(!strcmp(s, "disk"));
	sys->enabled_cache_type |= CACHE_TYPE_DISK;
}

static void do_cache_type(char *s)
{
	int i;

	struct cache_type {
		const char *name;
		void (*set)(char *);
	};
	struct cache_type cache_types[] = {
		{ "object", object_cache_set },
		{ "disk", disk_cache_set },
		{ NULL, NULL },
	};

	for (i = 0; cache_types[i].name; i++) {
		const char *n = cache_types[i].name;

		if (!strncmp(s, n, strlen(n))) {
			cache_types[i].set(s);
			return;
		}
	}

	fprintf(stderr, "invalid cache type: %s\n", s);
	exit(1);
}

static void init_cache_type(char *arg)
{
	sys->object_cache_size = -1;

	parse_arg(arg, ",", do_cache_type);

	if (is_object_cache_enabled() && sys->object_cache_size == -1) {
		fprintf(stderr, "object cache size is not set\n");
		exit(1);
	}
}

int main(int argc, char **argv)
{
	int ch, longindex;
	int ret, port = SD_LISTEN_PORT;
	const char *dir = DEFAULT_OBJECT_DIR;
	int is_daemon = 1;
	int to_stdout = 0;
	int log_level = SDOG_INFO;
	char path[PATH_MAX];
	int64_t zone = -1;
	int64_t free_space = 0;
	int nr_vnodes = SD_DEFAULT_VNODES;
	bool explicit_addr = false;
	int af;
	char *p;
	struct cluster_driver *cdrv;
	char *pid_file = NULL;

	signal(SIGPIPE, SIG_IGN);

	while ((ch = getopt_long(argc, argv, short_options, long_options,
				 &longindex)) >= 0) {
		switch (ch) {
		case 'p':
			port = strtol(optarg, &p, 10);
			if (optarg == p || port < 1 || port > UINT16_MAX) {
				fprintf(stderr, "Invalid port number '%s'\n",
					optarg);
				exit(1);
			}
			break;
		case 'P':
			pid_file = optarg;
			break;
		case 'f':
			is_daemon = 0;
			break;
		case 'l':
			log_level = strtol(optarg, &p, 10);
			if (optarg == p || log_level < SDOG_EMERG ||
			    log_level > SDOG_DEBUG) {
				fprintf(stderr, "Invalid log level '%s'\n",
					optarg);
				sdlog_help();
				exit(1);
			}
			break;
		case 'y':
			af = strstr(optarg, ":") ? AF_INET6 : AF_INET;
			if (!str_to_addr(af, optarg, sys->this_node.nid.addr)) {
				fprintf(stderr,
					"Invalid address: '%s'\n",
					optarg);
				sdlog_help();
				exit(1);
			}
			explicit_addr = true;
			break;
		case 'd':
			/* removed soon. use loglevel instead */
			log_level = SDOG_DEBUG;
			break;
		case 'g':
			/* same as '-v 0' */
			nr_vnodes = 0;
			break;
		case 'o':
			to_stdout = 1;
			break;
		case 'z':
			zone = strtol(optarg, &p, 10);
			if (optarg == p || zone < 0 || UINT32_MAX < zone) {
				fprintf(stderr, "Invalid zone id '%s': "
					"must be an integer between 0 and %u\n",
					optarg, UINT32_MAX);
				exit(1);
			}
			sys->this_node.zone = zone;
			break;
		case 's':
			free_space = strtoll(optarg, &p, 10);
			if (optarg == p || free_space <= 0 ||
			    UINT64_MAX < free_space) {
				fprintf(stderr, "Invalid free space size '%s': "
					"must be an integer between 0 and "
					"%"PRIu64"\n", optarg, UINT64_MAX);
				exit(1);
			}
			sys->disk_space = free_space * 1024 * 1024;
			break;
		case 'c':
			sys->cdrv = find_cdrv(optarg);
			if (!sys->cdrv) {
				fprintf(stderr, "Invalid cluster driver '%s'\n", optarg);
				fprintf(stderr, "Supported drivers:");
				FOR_EACH_CLUSTER_DRIVER(cdrv) {
					fprintf(stderr, " %s", cdrv->name);
				}
				fprintf(stderr, "\n");
				exit(1);
			}

			sys->cdrv_option = get_cdrv_option(sys->cdrv, optarg);
			break;
		case 'w':
			init_cache_type(optarg);
			break;
		case 'j':
			sys->use_journal = true;
			break;
		case 'h':
			usage(0);
			break;
		default:
			usage(1);
			break;
		}
	}
	if (nr_vnodes == 0) {
		sys->gateway_only = 1;
		sys->disk_space = 0;
	}

	if (optind != argc)
		dir = argv[optind];

	snprintf(path, sizeof(path), "%s/" LOG_FILE_NAME, dir);

	srandom(port);

	if (is_daemon && daemon(0, 0))
		exit(1);

	ret = init_base_path(dir);
	if (ret)
		exit(1);

	ret = log_init(program_name, LOG_SPACE_SIZE, to_stdout, log_level, path);
	if (ret)
		exit(1);

	ret = init_store(dir);
	if (ret)
		exit(1);

	ret = init_event(EPOLL_SIZE);
	if (ret)
		exit(1);

	ret = create_listen_port(port, sys);
	if (ret)
		exit(1);

	ret = create_cluster(port, zone, nr_vnodes, explicit_addr);
	if (ret) {
		eprintf("failed to create sheepdog cluster\n");
		exit(1);
	}

	local_req_init();

	ret = init_signal();
	if (ret)
		exit(1);

	sys->gateway_wqueue = init_work_queue("gway", false);
	sys->io_wqueue = init_work_queue("io", false);
	sys->recovery_wqueue = init_work_queue("rw", false);
	sys->deletion_wqueue = init_work_queue("deletion", true);
	sys->block_wqueue = init_work_queue("block", true);
	sys->sockfd_wqueue = init_work_queue("sockfd", true);
	if (is_object_cache_enabled()) {
		sys->reclaim_wqueue = init_work_queue("reclaim", true);
		if (!sys->reclaim_wqueue)
			exit(1);
	}
	if (!sys->gateway_wqueue || !sys->io_wqueue ||!sys->recovery_wqueue ||
	    !sys->deletion_wqueue || !sys->block_wqueue || !sys->sockfd_wqueue)
		exit(1);

	ret = trace_init();
	if (ret)
		exit(1);

	if (pid_file && (create_pidfile(pid_file) != 0)) {
		fprintf(stderr, "failed to pid file '%s' - %s\n", pid_file,
			strerror(errno));
		exit(1);
	}

	if (chdir(dir) < 0) {
		fprintf(stderr, "failed to chdir to %s: %m\n", dir);
		exit(1);
	}

	vprintf(SDOG_NOTICE, "sheepdog daemon (version %s) started\n", PACKAGE_VERSION);

	while (sys->nr_outstanding_reqs != 0 ||
	       (sys->status != SD_STATUS_KILLED &&
		sys->status != SD_STATUS_SHUTDOWN))
		event_loop(-1);

	vprintf(SDOG_INFO, "shutdown\n");

	leave_cluster();
	log_close();

	if (pid_file)
		unlink(pid_file);

	return 0;
}
