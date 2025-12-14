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

#include "cache_ext_adaptive_v2_debug.skel.h"
#include "dir_watcher.h"

static volatile bool exiting = false;

const char *policy_names[] = {
	"MRU",
	"FIFO",
	"LRU",
};

// 정책 전환 이벤트 (v2 debug - 추가 메트릭 포함)
struct policy_switch_event {
	unsigned int old_policy;
	unsigned int new_policy;
	unsigned long long timestamp;

	// 기본 메트릭
	unsigned long long hit_rate;
	unsigned long long total_accesses;

	// 추가 메트릭
	unsigned long long one_time_ratio;
	unsigned long long sequential_ratio;
	unsigned long long avg_hits_per_page;
	unsigned long long avg_reuse_distance;
	unsigned long long dirty_ratio;

	// 이전 정책 성능
	unsigned long long old_policy_hit_rate;
};

// 메트릭 스냅샷 이벤트
struct metric_snapshot_event {
	unsigned long long total_accesses;
	unsigned long long hit_rate;
	unsigned long long one_time_ratio;
	unsigned long long sequential_ratio;
	unsigned long long avg_hits_per_page;
	unsigned long long current_policy;
	unsigned long long pages_evicted;
	unsigned long long timestamp;
};

char *USAGE =
	"Usage: ./cache_ext_adaptive_v2_debug --watch_dir <dir> --cgroup_path <path>\n"
	"\n"
	"DEBUG VERSION: Adaptive cache eviction policy with RELAXED thresholds:\n"
	"  - One-time access ratio\n"
	"  - Sequential access ratio\n"
	"  - Average hits per page\n"
	"  - Average reuse distance\n"
	"  - Per-policy performance tracking\n"
	"\nNOTE: This version has extremely relaxed thresholds for testing!\n";

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

static int handle_event(void *ctx, void *data, size_t data_sz)
{
	if (data_sz == sizeof(struct policy_switch_event)) {
		// Policy switch event
		const struct policy_switch_event *e = data;

		if (e->old_policy >= 3 || e->new_policy >= 3) {
			fprintf(stderr, "Invalid policy ID in event\n");
			return 0;
		}

		printf("\n");
		printf("========================================\n");
		printf("POLICY SWITCH DETECTED!\n");
		printf("========================================\n");
		printf("  Time:                %llu\n", e->timestamp);
		printf("  Old Policy:          %s\n", policy_names[e->old_policy]);
		printf("  New Policy:          %s\n", policy_names[e->new_policy]);
		printf("\n");
		printf("Performance Metrics:\n");
		printf("  Hit Rate:            %llu%%\n", e->hit_rate);
		printf("  Old Policy Hit Rate: %llu%%\n", e->old_policy_hit_rate);
		printf("  Total Accesses:      %llu\n", e->total_accesses);
		printf("\n");
		printf("Workload Characteristics:\n");
		printf("  One-time Ratio:      %llu%%\n", e->one_time_ratio);
		printf("  Sequential Ratio:    %llu%%\n", e->sequential_ratio);
		printf("  Avg Hits/Page:       %llu\n", e->avg_hits_per_page);
		printf("  Avg Reuse Distance:  %llu\n", e->avg_reuse_distance);
		printf("  Dirty Page Ratio:    %llu%%\n", e->dirty_ratio);
		printf("========================================\n");

		// 정책 선택 이유 추론
		printf("\nSwitch Reason:\n");
		if (e->sequential_ratio > 30) {
			printf("  → High sequential access detected\n");
		} else if (e->one_time_ratio > 20 && e->avg_hits_per_page < 2) {
			printf("  → Many one-time accesses (scan workload)\n");
		} else if (e->avg_hits_per_page > 1) {
			printf("  → Hot working set with high reuse\n");
		} else if (e->hit_rate < 30) {
			printf("  → Low hit rate, trying different policy\n");
		}
		printf("\n");

	} else if (data_sz == sizeof(struct metric_snapshot_event)) {
		// Metric snapshot event - print current metrics
		const struct metric_snapshot_event *m = data;

		if (m->current_policy >= 3) {
			fprintf(stderr, "Invalid policy ID in metric event\n");
			return 0;
		}

		printf("[METRICS] accesses=%llu | hit_rate=%llu%% | one_time=%llu%% | sequential=%llu%% | avg_hits=%llu | policy=%s | evicted=%llu\n",
		       m->total_accesses, m->hit_rate, m->one_time_ratio,
		       m->sequential_ratio, m->avg_hits_per_page,
		       policy_names[m->current_policy], m->pages_evicted);
		fflush(stdout);
	}

	return 0;
}

int main(int argc, char **argv)
{
	int ret = 1;
	struct cache_ext_adaptive_v2_debug_bpf *skel = NULL;
	struct bpf_link *link = NULL;
	struct ring_buffer *rb = NULL;
	int cgroup_fd = -1;

	libbpf_set_strict_mode(LIBBPF_STRICT_ALL);

	struct cmdline_args args = { 0 };
	struct argp argp = { options, parse_opt, 0, 0 };
	argp_parse(&argp, argc, argv, 0, 0, &args);

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

	if (access(args.watch_dir, F_OK) == -1) {
		fprintf(stderr, "Directory does not exist: %s\n",
			args.watch_dir);
		return 1;
	}

	char watch_dir_full_path[PATH_MAX];
	if (realpath(args.watch_dir, watch_dir_full_path) == NULL) {
		perror("realpath");
		return 1;
	}

	if (strlen(watch_dir_full_path) > 128) {
		fprintf(stderr, "watch_dir path too long\n");
		return 1;
	}

	cgroup_fd = open(args.cgroup_path, O_RDONLY);
	if (cgroup_fd < 0) {
		perror("Failed to open cgroup path");
		return 1;
	}

	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);

	skel = cache_ext_adaptive_v2_debug_bpf__open();
	if (skel == NULL) {
		perror("Failed to open BPF skeleton");
		goto cleanup;
	}

	ret = cache_ext_adaptive_v2_debug_bpf__load(skel);
	if (ret) {
		perror("Failed to load BPF skeleton");
		goto cleanup;
	}

	ret = initialize_watch_dir_map(watch_dir_full_path,
				       bpf_map__fd(skel->maps.inode_watchlist),
				       true);
	if (ret) {
		perror("Failed to initialize watch_dir map");
		goto cleanup;
	}

	rb = ring_buffer__new(bpf_map__fd(skel->maps.events), handle_event,
			      NULL, NULL);
	if (!rb) {
		perror("Failed to create ring buffer");
		goto cleanup;
	}

	link = bpf_map__attach_cache_ext_ops(skel->maps.adaptive_v2_debug_ops,
					     cgroup_fd);
	if (link == NULL) {
		perror("Failed to attach cache_ext_ops to cgroup");
		goto cleanup;
	}

	printf("========================================\n");
	printf("DEBUG VERSION: Adaptive Policy v2 Started\n");
	printf("========================================\n");
	printf("  Watch directory: %s\n", watch_dir_full_path);
	printf("  Cgroup:          %s\n", args.cgroup_path);
	printf("  Initial policy:  MRU\n");
	printf("\n");
	printf("DEBUG CONFIG (Relaxed Thresholds):\n");
	printf("  MIN_SAMPLES: 50\n");
	printf("  MIN_TIME_IN_POLICY: 50\n");
	printf("  CHECK_INTERVAL: 5\n");
	printf("  Sequential threshold: 30%\n");
	printf("  One-time threshold: 20%\n");
	printf("  Avg hits threshold: 1\n");
	printf("\n");
	printf("Tracking Metrics:\n");
	printf("  ✓ Hit rate\n");
	printf("  ✓ One-time access ratio\n");
	printf("  ✓ Sequential access ratio\n");
	printf("  ✓ Average hits per page\n");
	printf("  ✓ Average reuse distance\n");
	printf("  ✓ Dirty page ratio\n");
	printf("  ✓ Per-policy performance\n");
	printf("\n");
	printf("Monitoring for policy switches...\n");
	printf("Press Ctrl-C to exit.\n");
	printf("========================================\n");
	printf("\n");

	while (!exiting) {
		ret = ring_buffer__poll(rb, 100);
		if (ret == -EINTR) {
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
	cache_ext_adaptive_v2_debug_bpf__destroy(skel);
	if (cgroup_fd >= 0)
		close(cgroup_fd);

	printf("Cleanup complete.\n");
	return ret;
}
