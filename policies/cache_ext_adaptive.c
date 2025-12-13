#include <argp.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cache_ext_adaptive.skel.h"
#include "dir_watcher.h"

static volatile bool exiting = false;

// 정책 이름 매핑
const char *policy_names[] = {
	"MRU",
	"FIFO",
	"LRU",
};

// 정책 전환 이벤트
struct policy_switch_event {
	unsigned int old_policy;
	unsigned int new_policy;
	unsigned long long timestamp;
	unsigned long long hit_rate;
	unsigned long long total_accesses;
};

char *USAGE =
	"Usage: ./cache_ext_adaptive --watch_dir <dir> --cgroup_path <path>\n"
	"\n"
	"Adaptive cache eviction policy that switches between MRU, FIFO, and LRU\n"
	"based on hit rate performance.\n";

struct cmdline_args {
	char *watch_dir;
	char *cgroup_path;
};

static struct argp_option options[] = {
	{ "watch_dir", 'w', "DIR", 0, "Directory to watch" },
	{ "cgroup_path", 'c', "PATH", 0,
	  "Path to cgroup (e.g., /sys/fs/cgroup/cache_ext_test)" },
	{ 0 }
};

static error_t parse_opt(int key, char *arg, struct argp_state *state)
{
	struct cmdline_args *args = state->input;
	switch (key) {
	case 'w':
		args->watch_dir = arg;
		break;
	case 'c':
		args->cgroup_path = arg;
		break;
	default:
		return ARGP_ERR_UNKNOWN;
	}
	return 0;
}

static void sig_handler(int sig)
{
	exiting = true;
}

// Ringbuffer 콜백: 정책 전환 이벤트 출력
static int handle_event(void *ctx, void *data, size_t data_sz)
{
	const struct policy_switch_event *e = data;

	if (e->old_policy >= 3 || e->new_policy >= 3) {
		fprintf(stderr, "Invalid policy ID in event\n");
		return 0;
	}

	printf("\n");
	printf("========================================\n");
	printf("POLICY SWITCH DETECTED!\n");
	printf("========================================\n");
	printf("  Time:          %llu\n", e->timestamp);
	printf("  Old Policy:    %s\n", policy_names[e->old_policy]);
	printf("  New Policy:    %s\n", policy_names[e->new_policy]);
	printf("  Hit Rate:      %llu%%\n", e->hit_rate);
	printf("  Total Access:  %llu\n", e->total_accesses);
	printf("========================================\n");
	printf("\n");

	return 0;
}

int main(int argc, char **argv)
{
	int ret = 1;
	struct cache_ext_adaptive_bpf *skel = NULL;
	struct bpf_link *link = NULL;
	struct ring_buffer *rb = NULL;
	int cgroup_fd = -1;

	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	// Parse command line arguments
	struct cmdline_args args = { 0 };
	struct argp argp = { options, parse_opt, 0, 0 };
	argp_parse(&argp, argc, argv, 0, 0, &args);

	// Validate arguments
	if (args.watch_dir == NULL) {
		fprintf(stderr, "Missing required argument: watch_dir\n");
		fprintf(stderr, "%s", USAGE);
		return 1;
	}

	if (args.cgroup_path == NULL) {
		fprintf(stderr, "Missing required argument: cgroup_path\n");
		fprintf(stderr, "%s", USAGE);
		return 1;
	}

	// Check if watch_dir exists
	if (access(args.watch_dir, F_OK) == -1) {
		fprintf(stderr, "Directory does not exist: %s\n",
			args.watch_dir);
		return 1;
	}

	// Get full path of watch_dir
	char watch_dir_full_path[PATH_MAX];
	if (realpath(args.watch_dir, watch_dir_full_path) == NULL) {
		perror("realpath");
		return 1;
	}

	if (strlen(watch_dir_full_path) > 128) {
		fprintf(stderr, "watch_dir path too long\n");
		return 1;
	}

	// Open cgroup directory
	cgroup_fd = open(args.cgroup_path, O_RDONLY);
	if (cgroup_fd < 0) {
		perror("Failed to open cgroup path");
		return 1;
	}

	// Set up signal handler
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	// Open BPF skeleton
	skel = cache_ext_adaptive_bpf__open();
	if (skel == NULL) {
		perror("Failed to open BPF skeleton");
		goto cleanup;
	}

	// Load BPF programs
	ret = cache_ext_adaptive_bpf__load(skel);
	if (ret) {
		perror("Failed to load BPF skeleton");
		goto cleanup;
	}

	// Initialize watch_dir map
	ret = initialize_watch_dir_map(watch_dir_full_path,
				       bpf_map__fd(skel->maps.inode_watchlist),
				       true);
	if (ret) {
		perror("Failed to initialize watch_dir map");
		goto cleanup;
	}

	// Set up ring buffer for policy switch events
	rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event,
			      NULL, NULL);
	if (!rb) {
		perror("Failed to create ring buffer");
		goto cleanup;
	}

	// Attach cache_ext_ops to the specific cgroup
	link = bpf_map__attach_cache_ext_ops(skel->maps.adaptive_ops,
					     cgroup_fd);
	if (link == NULL) {
		perror("Failed to attach cache_ext_ops to cgroup");
		goto cleanup;
	}

	printf("Adaptive cache eviction policy started\n");
	printf("  Watch directory: %s\n", watch_dir_full_path);
	printf("  Cgroup:          %s\n", args.cgroup_path);
	printf("  Initial policy:  MRU\n");
	printf("\n");
	printf("Monitoring for policy switches...\n");
	printf("Press Ctrl-C to exit.\n");
	printf("\n");

	// Main event loop
	while (!exiting) {
		ret = ring_buffer__poll(rb, 100 /* timeout, ms */);
		if (ret == -EINTR) {
			// Interrupted
			break;
		}
		if (ret < 0) {
			fprintf(stderr, "Error polling ring buffer: %d\n", ret);
			break;
		}
	}

	printf("\nShutting down...\n");
	ret = 0;

cleanup:
	ring_buffer__free(rb);
	bpf_link__destroy(link);
	cache_ext_adaptive_bpf__destroy(skel);
	if (cgroup_fd >= 0)
		close(cgroup_fd);

	printf("Cleanup complete.\n");
	return ret;
}
