#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cilk-internal.h"

enum {
    NONE,
    NPROC,
    DEQ_DEPTH,
    STACK_SIZE,
    FIBER_POOL_CAP,
    VERSION,
    HELP,
    END_OPTIONS
};

static const char *option_prefix = "cheetah-";

// TODO: Incorporate option_prefix into all of the optarray help
// entries in place of 'cheetah-'.
CHEETAH_INTERNAL
static struct options {
    const char *string;
    int option;
    const char *help;
} optarray[] = {
    {"", END_OPTIONS, "--cheetah- : end of option parsing"},
    {"nproc", NPROC, "--cheetah-nproc <n> : set number of processors"},
    {"deqdepth", DEQ_DEPTH,
     "--cheetah-deqdepth <n> : set number of entries per deque"},
    {"stacksize", STACK_SIZE,
     "--cheetah-stacksize <n> : set the size of a fiber"},
    {"fiber-pool", FIBER_POOL_CAP,
     "--cheetah-fiber-pool <n> : set the per-worker fiber pool capacity"},
    {"version", VERSION, "--cheetah-version: print version of the runtime"},
    {"help", HELP, "--cheetah-help : print this message"},
    {(char *)0, NONE, ""}};

static void print_help(void) {
    struct options *p;
    fprintf(stderr, "cheetah runtime options:\n");
    for (p = optarray + 1; p->string; ++p)
        if (p->help)
            fprintf(stderr, "     %s\n", p->help);
    fprintf(stderr, "\n");
}

static void print_version(void) {
    int debug = 0, stats = 0;
    WHEN_CILK_DEBUG(debug = 1);
    WHEN_CILK_STATS(stats = 1);
    fprintf(stderr, "version %d.%d\n", __CILKRTS_VERSION,
            __CILKRTS_ABI_VERSION);
    fprintf(stderr, "compilation options: ");
    if (debug)
        fprintf(stderr, "CILK_DEBUG ");
    if (stats)
        fprintf(stderr, "CILK_STATS ");
    if (!(debug | stats))
        fprintf(stderr, "none");
    fprintf(stderr, "\n");
}

/* look for a given string in the option table */
static struct options *parse_option(char *s) {
    struct options *p;
    for (p = optarray; p->string; ++p)
        if (strncmp(s, p->string, strlen(p->string) + 1) == 0)
            break;
    return p;
}

#define CHECK(cond, complaint)                                                 \
    if (!(cond)) {                                                             \
        fprintf(stderr, "Bad option argument for -%s: %s\n", p->string,        \
                complaint);                                                    \
        return 1;                                                              \
    }

CHEETAH_INTERNAL int parse_command_line(struct rts_options *options, int *argc,
                                        char *argv[]) {
    struct options *p;
    /* gcc allows to write directly into *options, but other compilers
     * only allow you to initialize this way.
     */
    struct rts_options default_options = DEFAULT_OPTIONS;

    /* default options */
    *options = default_options;

    int j = 1;
    for (int i = 1; i < *argc; ++i) {
        if (argv[i][0] == '-' && argv[i][1] == '-' &&
            strncmp(argv[i] + 2, option_prefix, strlen(option_prefix)) == 0) {
            p = parse_option(argv[i] + 2 + strlen(option_prefix));

            switch (p->option) {
            case NPROC:
                ++i;
                CHECK(i < *argc, "argument missing");
                options->nproc = atoi(argv[i]);
                break;

            case DEQ_DEPTH:
                ++i;
                CHECK(i < *argc, "argument missing");
                options->deqdepth = atoi(argv[i]);
                CHECK(options->deqdepth > 0, "non-positive deque depth");
                break;

            case STACK_SIZE:
                ++i;
                CHECK(i < *argc, "argument missing");
                options->stacksize = atol(argv[i]);
                CHECK(options->stacksize > 0, "non-positive stack size");
                break;

            case VERSION:
                print_version();
                return 1;
                break;

            case HELP:
                print_help();
                return 1;
                break;

            case FIBER_POOL_CAP:
                ++i;
                CHECK(i < *argc, "argument missing");
                options->fiber_pool_cap = atoi(argv[i]);
                if (options->fiber_pool_cap < 8) // keep minimum at 8
                    options->fiber_pool_cap = 8;
                break;

            default:
                fprintf(stderr, "Unrecognized options.\n");
                print_help();
                return 1;
                break;
            }
        } else {
            assert(j <= i);
            argv[j++] = argv[i]; // keep it
        }
    }
    *argc = j;

    return 0;
}
