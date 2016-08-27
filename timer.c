#include <stddef.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <limits.h>

#include <unistd.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <sys/time.h>
#include <sys/resource.h>

extern char **environ; // MY EYES ARE ON FIRE!

enum rc {
	RC_SUCCESS,
	RC_ARGUMENT_PARSING,
	RC_SPAWN,
	RC_WAIT,
	RC_RUSAGE,
	RC_TIME,
	RC_CHILD_SIGNALED
};

static struct {
	char const* name;
	char const* format;
	bool help;
} opts = {
	.name = "timer",
	.format = "\nreal %r\nuser %u\nsys  %s\n",
	.help = false
};

static void set_default_opts(void) {
}

static bool parse_opts(int* argc, char** argv) {
	set_default_opts();
	if(*argc > 0 && argv[0][0]) opts.name = argv[0];
	int to = 0;
	bool stop = false;
	for(int from = 1; from < *argc; ++from) {
		char* arg = argv[from];
		if(!stop && arg[0] == '-') {
			if(arg[1] == '-') {
				if(arg[2] == '\0') {
					stop = true;
				} else if(strcmp("help", arg + 2) == 0) {
					opts.help = true;
				} else if(strcmp("format", arg + 2) == 0) {
					if(++from >= *argc) return false;
					opts.format = argv[from];
				} else return false;
			} else if(arg[1] == 'h') {
				if(arg[2] == '\0') {
					opts.help = true;
				} else return false;
			} else if(arg[1] == 'f') {
				if(arg[2] == '\0') {
					if(++from >= *argc) return false;
					opts.format = argv[from];
				} else return false;
			} else return false;
		} else {
			stop = true;
			argv[to++] = arg;
		}
	}
	argv[to] = NULL;
	*argc = to;
	return *argc > 0;
}

static void usage(FILE* f) {
	fprintf(f, "Usage: %s [options] [--] command [args]\n", opts.name);
	fprintf(f, "Watches resource utilization of 'command args'\n");
	fprintf(f, "\n");
	fprintf(f, "Options:\n");
	fprintf(f, "  --                         Stop parsing Arguments (useful to call commands whose name starts with '-')\n");
	fprintf(f, "  -h --help                  Show this help, then exit\n");
	fprintf(f, "  -f --format S              Use the format string S for the report\n");
	fprintf(f, "\n");
	fprintf(f, "Format Language:\n");
	fprintf(f, "  %%r  Real time elapsed (human readable)\n");
	fprintf(f, "  %%R  Real time elapsed (nanoseconds)\n");
	fprintf(f, "  %%u  User time elapsed (human readable)\n");
	fprintf(f, "  %%U  User time elapsed (nanoseconds)\n");
	fprintf(f, "  %%s  System time elapsed (human readable)\n");
	fprintf(f, "  %%S  System time elapsed (nanoseconds)\n");
	fprintf(f, "  %%%%  Print a literal %% sign\n");
	fprintf(f, "\n");
	fprintf(f, "  \\n  New Line\n");
	fprintf(f, "  \\t  Tab\n");
	fprintf(f, "  \\%%  Print a literal %% sign\n");
	fprintf(f, "  \\\\  Print a literal \\ sign\n");
}

static uint64_t ts_get_elapsed_ns(struct timespec* before, struct timespec* after) {
	uint64_t result = after->tv_sec - before->tv_sec;
	result *= 1000*1000*1000; // nanoseconds
	if(after->tv_nsec >= before->tv_nsec) {
		result += after->tv_nsec - before->tv_nsec;
	} else {
		result -= 1000*1000*1000;
		result += before->tv_nsec - after->tv_nsec;
	}
	return result;
}

static uint64_t tv_get_elapsed_us(struct timeval* before, struct timeval* after) {
	uint64_t result = after->tv_sec - before->tv_sec;
	result *= 1000*1000; // microseconds
	if(after->tv_usec >= before->tv_usec) {
		result += after->tv_usec - before->tv_usec;
	} else {
		result -= 1000*1000;
		result += before->tv_usec - after->tv_usec;
	}
	return result;
}

static void print_ns_h(FILE* f, uint64_t ns) {
	if(ns < 10000) {
		fprintf(f, " %4u ns", (unsigned)(ns));
	} else if(ns < 100000) {
		fprintf(f, "%u.%02u ns", (unsigned)(ns / 1000), (unsigned)((ns+5) / 10 % 100));
	} else if(ns < 1000000) {
		fprintf(f, "%u.%1u ns", (unsigned)(ns / 1000), (unsigned)((ns+50) / 100 % 10));
	} else if(ns < 10000000) {
		fprintf(f, " %4u us", (unsigned)((ns+500) / 1000));
	} else if(ns < 100000000) {
		fprintf(f, "%u.%02u us", (unsigned)(ns / 1000000), (unsigned)((ns+5000) / 10000 % 100));
	} else if(ns < 1000000000) {
		fprintf(f, "%u.%1u us", (unsigned)(ns / 1000000), (unsigned)((ns+50000) / 100000 % 10));
	} else if(ns < 10000000000) {
		fprintf(f, " %4u ms", (unsigned)((ns+500000) / 1000000));
	} else {
		fprintf(f, "%5" PRIu64 " s", (ns+500000000) / 1000000000);
	}
}

typedef struct resources_t {
	uint64_t real_ns, user_ns, sys_ns;
} resources_t;

static bool check_format(char const* format) {
	for(char c = *format; c; c = *++format) {
		if(c == '%') {
			c = *++format;
			if(c == 'r') {
			} else if(c == 'R') {
			} else if(c == 'u') {
			} else if(c == 'U') {
			} else if(c == 's') {
			} else if(c == 'S') {
			} else if(c == '%') {
			} else return false;
		} else if(c == '\\') {
			c = *++format;
			if(c == 'n') {
			} else if(c == 't') {
			} else if(c == '%') {
			} else if(c == '\\') {
			} else return false;
		}
	}
	return true;
}

static void resources_fprintf(FILE* f, char const* format, resources_t* resources) {
	for(char c = *format; c; c = *++format) {
		if(c == '%') {
			c = *++format;
			if(c == 'r') {
				print_ns_h(f, resources->real_ns);
			} else if(c == 'R') {
				fprintf(f, "%" PRIu64, resources->real_ns);
			} else if(c == 'u') {
				print_ns_h(f, resources->user_ns);
			} else if(c == 'U') {
				fprintf(f, "%" PRIu64, resources->user_ns);
			} else if(c == 's') {
				print_ns_h(f, resources->sys_ns);
			} else if(c == 'S') {
				fprintf(f, "%" PRIu64, resources->sys_ns);
			} else fprintf(f, "%%");
		} else if(c == '\\') {
			c = *++format;
			if(c == 'n') {
				fprintf(f, "\n");
			} else if(c == 't') {
				fprintf(f, "\t");
			} else if(c == '%') {
				fprintf(f, "%%");
			} else fprintf(f, "\\");
		} else fprintf(f, "%c", c);
	}
}

int main(int argc, char** argv) {
	if(!parse_opts(&argc, argv)) {
		usage(stderr);
		return RC_ARGUMENT_PARSING;
	} else if(opts.help) {
		usage(stdout);
		return RC_SUCCESS;
	} else if(!check_format(opts.format)) {
		fprintf(stderr, "The given format string is not a legal format string.\nUse '%s -h' for more information.\n", opts.name);
		return RC_ARGUMENT_PARSING;
	}

	struct rusage ru_before;
	if(getrusage(RUSAGE_CHILDREN, &ru_before)) {
		fprintf(stderr, "Cannot get resource usage...\n");
		return RC_RUSAGE;
	}
	struct timespec ts_before;
	if(clock_gettime(CLOCK_MONOTONIC_RAW, &ts_before)) {
		fprintf(stderr, "Cannot get time...\n");
		return RC_TIME;
	}

	pid_t child;
	if(posix_spawnp(&child, argv[0], NULL, NULL, argv, environ)) {
		fprintf(stderr, "Cannot spawn child...\n");
		return RC_SPAWN;
	}
	int rc;
	for(;;) {
		int status;
		if(child != waitpid(child, &status, 0)) {
			fprintf(stderr, "Cannot wait for child...\n");
			return RC_WAIT;
		}
		if(WIFEXITED(status)) {
			rc = WEXITSTATUS(status);
			break;
		} else if(WIFSIGNALED(status)) {
			rc = 128 | WTERMSIG(status); // this seems to be the convention in unixious shells
			break;
		}
	}

	struct timespec ts_after;
	if(clock_gettime(CLOCK_MONOTONIC_RAW, &ts_after)) {
		fprintf(stderr, "Cannot get time...\n");
		return RC_TIME;
	}
	struct rusage ru_after;
	if(getrusage(RUSAGE_CHILDREN, &ru_after)) {
		fprintf(stderr, "Cannot get resource usage...\n");
		return RC_RUSAGE;
	}

	resources_t resources = {
		.real_ns = ts_get_elapsed_ns(&ts_before, &ts_after),
		.user_ns = tv_get_elapsed_us(&ru_before.ru_utime, &ru_after.ru_utime) * 1000,
		.sys_ns = tv_get_elapsed_us(&ru_before.ru_stime, &ru_after.ru_stime) * 1000,
	};

	resources_fprintf(stdout, opts.format, &resources);

	return rc;
}
