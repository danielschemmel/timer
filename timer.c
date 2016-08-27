#include <assert.h>
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

static char const* const portable_format = "real %pr\nuser %pu\nsys %ps\n";

static char const* const complete_format = "\n"
	"real %r\n"
	"user %u\n"
	"sys  %s\n"
	"minor pagefaults %f\n"
	"major pagefaults %F\n"
	"voluntary context switches   %c\n"
	"involuntary context switches %C\n"
;

static bool parse_opts(int* argc, char** argv) {
	if(*argc > 0 && argv[0][0]) opts.name = argv[0];
	int to = 0;
	bool stop = false;
	for(int from = 1; from < *argc; ++from) {
		char* arg = argv[from];
		if(!stop && arg[0] == '-') {
			if(arg[1] == '-') {
				if(arg[2] == '\0') {
					stop = true;
				} else if(strcmp("complete", arg + 2) == 0) {
					opts.format = complete_format;
				} else if(strcmp("help", arg + 2) == 0) {
					opts.help = true;
				} else if(strcmp("format", arg + 2) == 0) {
					if(++from >= *argc) return false;
					opts.format = argv[from];
				} else if(strcmp("portability", arg + 2) == 0) {
					opts.format = portable_format;
				} else return false;
			} else if(arg[1] == 'c') {
				if(arg[2] == '\0') {
					opts.format = complete_format;
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
			} else if(arg[1] == 'p') {
				if(arg[2] == '\0') {
					opts.format = portable_format;
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
	fprintf(f, "  -c --complete              Give a complete resource usage report\n");
	fprintf(f, "  -f --format S              Use the format string S for the report\n");
	fprintf(f, "  -p --portability           Use the portable format similar to that used by e.g. GNU time and bash time\n");
	fprintf(f, "\n");
	fprintf(f, "Format Sequences:\n");
	fprintf(f, "Any %% sign in the format string must be the start of a valid format sequence respectively. Escape sequences are of the form %%[options][specifier] where 'specifier' is required while 'options' is optional and will default to 'h'.\n");
	fprintf(f, "\n");
	fprintf(f, "Format Options:\n");
	fprintf(f, "  h  Print in an unspecified way useful for human consumption (this is the default)\n");
	fprintf(f, "  m  Print as a maximum-precision (nanoseconds for time, bytes for data sizes) decimal integral number\n");
	fprintf(f, "  M  Print as a maximum-precision (nanoseconds for time, bytes for data sizes) hexadecimal integral number\n");
	fprintf(f, "  p  Print 'portably', that is as second with two fractional digits\n");
	fprintf(f, "\n");
	fprintf(f, "Format Specifiers:\n");
	fprintf(f, "  %%[hmMp]r  Real time elapsed\n");
	fprintf(f, "  %%[hmMp]u  User time elapsed\n");
	fprintf(f, "  %%[hmMp]s  System time elapsed\n");
	fprintf(f, "  %%[hmM]f   Minor (recoverable) page faults\n");
	fprintf(f, "  %%[hmM]F   Major (unrecoverable) page faults\n");
	fprintf(f, "  %%%%        Print a literal %% sign\n");
	fprintf(f, "\n");
	fprintf(f, "Escape Sequences:\n");
	fprintf(f, "Any \\ character in the format string must be the start of a valid escape sequence respectively.\n");
	fprintf(f, "  \\n  New Line\n");
	fprintf(f, "  \\t  Tab\n");
	fprintf(f, "  \\%%  Print a literal %% sign\n");
	fprintf(f, "  \\\\  Print a literal \\ character\n");
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

typedef struct resources_t {
	uint64_t real_ns, user_ns, sys_ns;
	long minor_pagefaults, major_pagefaults;
	long voluntary_ctxt_switches, involuntary_ctxt_switches;
} resources_t;

static char check_format_option(char const c) {
	switch(c) {
		case 'h':
		case 'm':
		case 'M':
		case 'p':
			return c;
		default:
			return '\0';
	}
}

static bool check_format(char const* format) {
	for(char c = *format; c; c = *++format) {
		if(c == '%') {
			c = *++format;
			char option = check_format_option(c);
			if(option != '\0') c = *++format;

			if(c == 'r') {
			} else if(c == 'u') {
			} else if(c == 's') {
			} else if(c == 'f') {
				if(option != '\0' && !(option == 'h' || option == 'm' || option == 'M')) return false;
			} else if(c == 'F') {
				if(option != '\0' && !(option == 'h' || option == 'm' || option == 'M')) return false;
			} else if(c == 'c') {
				if(option != '\0' && !(option == 'h' || option == 'm' || option == 'M')) return false;
			} else if(c == 'C') {
				if(option != '\0' && !(option == 'h' || option == 'm' || option == 'M')) return false;
			} else if(c == '%') {
				if(option != '\0') return false;
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

static void print_ns_h(FILE* f, uint64_t ns) {
	if(ns < 1000) {
		fprintf(f, "  %3u ns", (unsigned)(ns));
	} else if(ns < 10000) {
		fprintf(f, "%u.%03u us", (unsigned)((ns+500) / 1000), (unsigned)(ns % 1000));
	} else if(ns < 100000) {
		fprintf(f, "%u.%02u us", (unsigned)((ns+500) / 1000), (unsigned)((ns+5) / 10 % 100));
	} else if(ns < 1000000) {
		fprintf(f, "%u.%01u us", (unsigned)((ns+500) / 1000), (unsigned)((ns+50) / 100 % 10));
	} else if(ns < 10000000) {
		fprintf(f, "%u.%03u ms", (unsigned)((ns+500000) / 1000000), (unsigned)((ns+500) / 1000 % 1000));
	} else if(ns < 100000000) {
		fprintf(f, "%u.%02u ms", (unsigned)((ns+500000) / 1000000), (unsigned)((ns+5000) / 10000 % 100));
	} else if(ns < 1000000000) {
		fprintf(f, "%u.%01u ms", (unsigned)((ns+500000) / 1000000), (unsigned)((ns+50000) / 100000 % 10));
	} else if(ns < 10000000000) {
		fprintf(f, "%u.%03u s", (unsigned)((ns+500000000) / 1000000000), (unsigned)((ns+500000) / 1000000 % 1000));
	} else if(ns < 100000000000) {
		fprintf(f, "%u.%02u s", (unsigned)((ns+500000000) / 1000000000), (unsigned)((ns+5000000) / 10000000 % 100));
	} else if(ns < 1000000000000) {
		fprintf(f, "%u.%01u s", (unsigned)((ns+500000000) / 1000000000), (unsigned)((ns+50000000) / 100000000 % 10));
	} else {
		ns = (ns+500000000) / 1000000000;
		uint64_t divisor = 1;
		while(ns / 1000 >= divisor) divisor *= 1000;
		fprintf(f, "%" PRIu64 " ", ns / divisor % 1000);
		for(divisor /= 1000; divisor; divisor /= 1000) {
			fprintf(f, "%03" PRIu64 " ", ns / divisor % 1000);
		}
		fprintf(f, "s");
	}
}

static void print_ns(FILE* f, char const option, uint64_t const ns) {
	if(option == '\0' || option == 'h') {
		print_ns_h(f, ns);
	} else if(option == 'm') {
		fprintf(f, "%" PRIu64, ns);
	} else if(option == 'M') {
		fprintf(f, "%" PRIx64, ns);
	} else {
		assert(option == 'p');
		fprintf(f, "%" PRIu64 ".%02u", (ns+5000000) / 1000000000, (unsigned)((ns+5000000) / 10000000 % 100));
	}
}

static void print_count_h(FILE* f, long l) {
	if(l < 0) {
		fprintf(f, "???");
		return;
	}
	long divisor = 1;
	while(l / 1000 >= divisor) divisor *= 1000;
	fprintf(f, "%ld", l / divisor % 1000);
	for(divisor /= 1000; divisor; divisor /= 1000) {
		fprintf(f, " %03ld", l / divisor % 1000);
	}
}

static void print_count(FILE* f, char const option, long const count) {
	if(option == '\0' || option == 'h') {
		print_count_h(f, count);
	} else if(option == 'm') {
		fprintf(f, "%lu", count);
	} else if(option == 'M') {
		assert(option == 'M');
		fprintf(f, "%lx", count);
	}
}

static void resources_fprintf(FILE* f, char const* format, resources_t* resources) {
	for(char c = *format; c; c = *++format) {
		if(c == '%') {
			c = *++format;
			char option = check_format_option(c);
			if(option != '\0') c = *++format;

			if(c == 'r') {
				print_ns(f, option, resources->real_ns);
			} else if(c == 'u') {
				print_ns(f, option, resources->user_ns);
			} else if(c == 's') {
				print_ns(f, option, resources->sys_ns);
			} else if(c == 'f') {
				print_count(f, option, resources->minor_pagefaults);
			} else if(c == 'F') {
				print_count(f, option, resources->major_pagefaults);
			} else if(c == 'c') {
				print_count(f, option, resources->voluntary_ctxt_switches);
			} else if(c == 'C') {
				print_count(f, option, resources->involuntary_ctxt_switches);
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
		.minor_pagefaults = ru_after.ru_minflt - ru_before.ru_minflt,
		.major_pagefaults = ru_after.ru_majflt - ru_before.ru_majflt,
		.voluntary_ctxt_switches = ru_after.ru_nvcsw - ru_before.ru_nvcsw,
		.involuntary_ctxt_switches = ru_after.ru_nivcsw - ru_before.ru_nivcsw,
	};

	resources_fprintf(stdout, opts.format, &resources);

	return rc;
}
