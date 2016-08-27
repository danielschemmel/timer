#include <stddef.h>
#include <stdint.h>
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

extern char **environ; // MY EYES ARE ON FIRE!

enum rc {
	RC_SUCCESS,
	RC_ARGUMENT_PARSING,
	RC_SPAWN
};

static struct {
	char const* name;
	bool help;
} opts = {
	.name = "timer",
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
				} else return false;
			} else if(arg[1] == 'h') {
				if(arg[2] == '\0') {
					opts.help = true;
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
}

int main(int argc, char** argv) {
	if(!parse_opts(&argc, argv)) {
		usage(stderr);
		return RC_ARGUMENT_PARSING;
	} else if(opts.help) {
		usage(stdout);
		return RC_SUCCESS;
	}

	pid_t child;
	if(posix_spawnp(&child, argv[0], NULL, NULL, argv, environ)) {
		fprintf(stderr, "Cannot spawn child...\n");
		return RC_SPAWN;
	}
	for(;;) {
		int status;
		if(child != waitpid(child, &status, 0)) {
			fprintf(stderr, "Cannot dance\n");
			return 42;
		}
		if(WIFEXITED(status)) {
			return WEXITSTATUS(status);
		} else if(WIFSIGNALED(status)) {
			return 44;
		}
	}

	return RC_SUCCESS;
}
