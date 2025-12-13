#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#include "cache_ext_lib.bpf.h"
#include "dir_watcher.bpf.h"

char _license[] SEC("license") = "GPL";

#define INT64_MAX (9223372036854775807LL)

// ===== 정책 타입 =====
enum policy_type {
	POLICY_MRU = 0,
	POLICY_FIFO = 1,
	POLICY_LRU = 2,
	POLICY_S3FIFO = 3,
	POLICY_LHD_SIMPLE = 4,  // 간소화된 LHD
};

// ===== 설정 파라미터 =====
#define HIT_RATE_THRESHOLD 30
#define MIN_SAMPLES 1000
#define MIN_TIME_IN_POLICY 10000
#define CHECK_INTERVAL 1000

// S3-FIFO 파라미터
#define CACHE_SIZE_ESTIMATE 50000  // ~200MB / 4KB

// ===== 전역 통계 =====
static u64 timestamp = 0;

// 기본 성능
static u64 total_accesses = 0;
static u64 cache_hits = 0;
static u64 cache_misses = 0;
static u64 total_evictions = 0;

// 접근 패턴
static u64 one_time_accesses = 0;
static u64 multi_accesses = 0;
static u64 last_inode = 0;
static u64 last_offset = 0;
static u64 sequential_accesses = 0;
static u64 random_accesses = 0;

// 페이지 통계
static u64 total_hits_sum = 0;
static u64 pages_evicted = 0;
static u64 reuse_distance_sum = 0;
static u64 reuse_distance_count = 0;
static u64 total_lifetime_sum = 0;
static u64 total_idle_time_sum = 0;
static u64 dirty_evictions = 0;

// 🆕 Working set size 추적
static u64 working_set_size = 0;  // 근사치

// Per-policy 통계
struct policy_stats {
	u64 hits;
	u64 misses;
	u64 evictions;
	u64 time_started;
	u64 time_active;
};

static struct policy_stats stats[5] = {0};  // 5개 정책

// 정책 전환
static u32 current_policy = POLICY_MRU;
static u64 last_policy_switch_time = 0;
static u32 policy_switch_count = 0;

// 각 정책별 리스트
static u64 mru_list = 0;
static u64 fifo_list = 0;
static u64 lru_list = 0;
static u64 s3fifo_small_list = 0;
static u64 s3fifo_main_list = 0;
static u64 lhd_list = 0;

// S3-FIFO 상태
static s64 s3fifo_small_size = 0;
static s64 s3fifo_main_size = 0;

// ===== Per-folio 메타데이터 =====
struct folio_metadata {
	u64 added_time;
	u64 last_access_time;
	u64 access_count;
	u32 current_policy;

	// S3-FIFO용
	s64 freq;
	bool in_main;

	// LHD용 (간소화)
	u64 last_hit_age;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, u64);
	__type(value, struct folio_metadata);
	__uint(max_entries, 4000000);
} folio_metadata_map SEC(".maps");

// 🆕 Working set 추적 (최근 접근 inodes)
struct {
	__uint(type, BPF_MAP_TYPE_LRU_HASH);
	__type(key, u64);  // inode
	__type(value, u8);
	__uint(max_entries, 100000);
} working_set_map SEC(".maps");

// ===== 이벤트 =====
struct policy_switch_event {
	u32 old_policy;
	u32 new_policy;
	u64 timestamp;
	u64 hit_rate;
	u64 total_accesses;
	u64 one_time_ratio;
	u64 sequential_ratio;
	u64 avg_hits_per_page;
	u64 avg_reuse_distance;
	u64 dirty_ratio;
	u64 old_policy_hit_rate;
	u64 working_set_size;        // 🆕
	u64 working_set_ratio;       // 🆕 (WS / Cache) * 100
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 8192);
} events SEC(".maps");

// ===== 헬퍼 함수 =====
static inline bool is_folio_relevant(struct folio *folio)
{
	if (!folio || !folio->mapping || !folio->mapping->host)
		return false;
	return inode_in_watchlist(folio->mapping->host->i_ino);
}

static inline struct folio_metadata *get_folio_metadata(struct folio *folio)
{
	u64 key = (u64)folio;
	return bpf_map_lookup_elem(&folio_metadata_map, &key);
}

// ===== 메트릭 계산 =====
static inline u64 calculate_hit_rate(void)
{
	if (total_accesses == 0)
		return 0;
	return (cache_hits * 100) / total_accesses;
}

static inline u64 calculate_one_time_ratio(void)
{
	u64 total = one_time_accesses + multi_accesses;
	if (total == 0)
		return 0;
	return (one_time_accesses * 100) / total;
}

static inline u64 calculate_sequential_ratio(void)
{
	u64 total = sequential_accesses + random_accesses;
	if (total == 0)
		return 0;
	return (sequential_accesses * 100) / total;
}

static inline u64 calculate_avg_hits_per_page(void)
{
	if (pages_evicted == 0)
		return 0;
	return total_hits_sum / pages_evicted;
}

static inline u64 calculate_avg_reuse_distance(void)
{
	if (reuse_distance_count == 0)
		return 0;
	return reuse_distance_sum / reuse_distance_count;
}

static inline u64 calculate_dirty_ratio(void)
{
	if (total_evictions == 0)
		return 0;
	return (dirty_evictions * 100) / total_evictions;
}

static inline u64 calculate_policy_hit_rate(struct policy_stats *st)
{
	u64 total = st->hits + st->misses;
	if (total == 0)
		return 0;
	return (st->hits * 100) / total;
}

// 🆕 Working set ratio 계산
static inline u64 calculate_working_set_ratio(void)
{
	if (CACHE_SIZE_ESTIMATE == 0)
		return 0;
	return (working_set_size * 100) / CACHE_SIZE_ESTIMATE;
}

// ===== Per-policy 통계 업데이트 =====
static inline void update_policy_stats(u32 policy, bool is_hit)
{
	if (policy >= 5)
		return;

	if (is_hit)
		__sync_fetch_and_add(&stats[policy].hits, 1);
	else
		__sync_fetch_and_add(&stats[policy].misses, 1);
}

// ===== 정책 선택 로직 =====
static u32 decide_best_policy(void)
{
	u64 one_time_ratio = calculate_one_time_ratio();
	u64 sequential_ratio = calculate_sequential_ratio();
	u64 avg_hits = calculate_avg_hits_per_page();
	u64 avg_reuse_dist = calculate_avg_reuse_distance();
	u64 ws_ratio = calculate_working_set_ratio();

	// 🆕 Working set 기반 판단
	if (ws_ratio > 300) {
		// Working set >> cache
		// → 어차피 thrashing, 오버헤드 최소화
		bpf_printk("Decision: FIFO (WS >> cache, ratio=%llu%%)\n", ws_ratio);
		return POLICY_FIFO;
	} else if (ws_ratio < 60) {
		// Working set << cache
		// → Hot pages 보호에 집중
		bpf_printk("Decision: MRU (WS << cache, ratio=%llu%%)\n", ws_ratio);
		return POLICY_MRU;
	}

	// 기존 로직
	if (sequential_ratio > 80) {
		bpf_printk("Decision: FIFO (sequential=%llu%%)\n", sequential_ratio);
		return POLICY_FIFO;
	}

	if (one_time_ratio > 60 && avg_hits < 2) {
		// One-time scan이 많지만 일부는 재접근
		// → S3-FIFO로 필터링
		bpf_printk("Decision: S3-FIFO (one_time=%llu%%, avg_hits=%llu)\n",
		           one_time_ratio, avg_hits);
		return POLICY_S3FIFO;
	}

	if (avg_hits > 5 && one_time_ratio < 30) {
		bpf_printk("Decision: MRU (avg_hits=%llu)\n", avg_hits);
		return POLICY_MRU;
	}

	if (avg_reuse_dist > 0 && avg_reuse_dist < 50000) {
		bpf_printk("Decision: LRU (reuse_dist=%llu)\n", avg_reuse_dist);
		return POLICY_LRU;
	}

	// 복잡한 워크로드 → LHD (간소화 버전)
	if (ws_ratio > 100 && ws_ratio < 200) {
		bpf_printk("Decision: LHD (complex workload, ws_ratio=%llu%%)\n", ws_ratio);
		return POLICY_LHD_SIMPLE;
	}

	// 과거 성능 기반
	u64 best_perf = 0;
	u32 best_policy = POLICY_LRU;

	for (int i = 0; i < 5; i++) {
		u64 perf = calculate_policy_hit_rate(&stats[i]);
		if (perf > best_perf) {
			best_perf = perf;
			best_policy = i;
		}
	}

	bpf_printk("Decision: Policy %d (best past perf=%llu%%)\n",
	           best_policy, best_perf);
	return best_policy;
}

// ===== 정책 전환 체크 =====
static void check_and_switch_policy(void)
{
	u64 hit_rate;
	u32 new_policy;
	struct policy_switch_event *event;

	if (total_accesses < MIN_SAMPLES)
		return;

	u64 time_since_switch = timestamp - last_policy_switch_time;
	if (time_since_switch < MIN_TIME_IN_POLICY)
		return;

	hit_rate = calculate_hit_rate();

	if (hit_rate >= HIT_RATE_THRESHOLD)
		return;

	new_policy = decide_best_policy();

	if (new_policy == current_policy)
		return;

	stats[current_policy].time_active = timestamp - stats[current_policy].time_started;

	event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
	if (event) {
		event->old_policy = current_policy;
		event->new_policy = new_policy;
		event->timestamp = timestamp;
		event->hit_rate = hit_rate;
		event->total_accesses = total_accesses;
		event->one_time_ratio = calculate_one_time_ratio();
		event->sequential_ratio = calculate_sequential_ratio();
		event->avg_hits_per_page = calculate_avg_hits_per_page();
		event->avg_reuse_distance = calculate_avg_reuse_distance();
		event->dirty_ratio = calculate_dirty_ratio();
		event->old_policy_hit_rate = calculate_policy_hit_rate(&stats[current_policy]);
		event->working_set_size = working_set_size;
		event->working_set_ratio = calculate_working_set_ratio();

		bpf_ringbuf_submit(event, 0);
	}

	bpf_printk("Policy switch: %d -> %d (hit_rate=%llu%%, ws_ratio=%llu%%)\n",
		   current_policy, new_policy, hit_rate, calculate_working_set_ratio());

	current_policy = new_policy;
	last_policy_switch_time = timestamp;
	policy_switch_count++;

	stats[new_policy].time_started = timestamp;

	total_accesses = 0;
	cache_hits = 0;
	cache_misses = 0;
}

// ===== 개별 정책 구현 =====

// MRU
static void mru_handle_added(struct folio *folio)
{
	bpf_cache_ext_list_add(mru_list, folio);
}

static void mru_handle_accessed(struct folio *folio)
{
	bpf_cache_ext_list_move(mru_list, folio, false);
}

static int mru_iterate_fn(int idx, struct cache_ext_list_node *node)
{
	if ((idx < 200) && (!folio_test_uptodate(node->folio) ||
			    !folio_test_lru(node->folio)))
		return CACHE_EXT_CONTINUE_ITER;
	return CACHE_EXT_EVICT_NODE;
}

// FIFO
static void fifo_handle_added(struct folio *folio)
{
	bpf_cache_ext_list_add_tail(fifo_list, folio);
}

static int fifo_iterate_fn(int idx, struct cache_ext_list_node *node)
{
	if (!folio_test_uptodate(node->folio) || !folio_test_lru(node->folio))
		return CACHE_EXT_CONTINUE_ITER;
	return CACHE_EXT_EVICT_NODE;
}

// LRU
static void lru_handle_added(struct folio *folio)
{
	bpf_cache_ext_list_add_tail(lru_list, folio);
}

static void lru_handle_accessed(struct folio *folio)
{
	bpf_cache_ext_list_move(lru_list, folio, true);
}

static int lru_iterate_fn(int idx, struct cache_ext_list_node *node)
{
	if (!folio_test_uptodate(node->folio) || !folio_test_lru(node->folio))
		return CACHE_EXT_CONTINUE_ITER;
	return CACHE_EXT_EVICT_NODE;
}

// S3-FIFO
static void s3fifo_handle_added(struct folio *folio, struct folio_metadata *meta)
{
	meta->freq = 0;
	meta->in_main = false;
	bpf_cache_ext_list_add_tail(s3fifo_small_list, folio);
	__sync_fetch_and_add(&s3fifo_small_size, 1);
}

static void s3fifo_handle_accessed(struct folio *folio, struct folio_metadata *meta)
{
	if (meta->freq < 3)
		meta->freq++;
}

static int s3fifo_small_iterate_fn(int idx, struct cache_ext_list_node *node)
{
	if (!folio_test_uptodate(node->folio) || !folio_test_lru(node->folio))
		return CACHE_EXT_CONTINUE_ITER;

	struct folio_metadata *meta = get_folio_metadata(node->folio);
	if (!meta)
		return CACHE_EXT_CONTINUE_ITER;

	if (meta->freq > 1) {
		// Main으로 이동
		meta->in_main = true;
		return CACHE_EXT_CONTINUE_ITER;
	}

	return CACHE_EXT_EVICT_NODE;
}

static int s3fifo_main_iterate_fn(int idx, struct cache_ext_list_node *node)
{
	if (!folio_test_uptodate(node->folio) || !folio_test_lru(node->folio))
		return CACHE_EXT_CONTINUE_ITER;

	struct folio_metadata *meta = get_folio_metadata(node->folio);
	if (!meta)
		return CACHE_EXT_CONTINUE_ITER;

	if (meta->freq > 0) {
		meta->freq--;
		return CACHE_EXT_CONTINUE_ITER;
	}

	return CACHE_EXT_EVICT_NODE;
}

// LHD (간소화 버전 - hit age 기반)
static void lhd_handle_added(struct folio *folio, struct folio_metadata *meta)
{
	meta->last_hit_age = 0;
	bpf_cache_ext_list_add_tail(lhd_list, folio);
}

static void lhd_handle_accessed(struct folio *folio, struct folio_metadata *meta)
{
	u64 age = timestamp - meta->last_access_time;
	meta->last_hit_age = age;
}

static int lhd_iterate_fn(int idx, struct cache_ext_list_node *node)
{
	if (!folio_test_uptodate(node->folio) || !folio_test_lru(node->folio))
		return CACHE_EXT_CONTINUE_ITER;

	struct folio_metadata *meta = get_folio_metadata(node->folio);
	if (!meta)
		return CACHE_EXT_CONTINUE_ITER;

	// Hit age가 큰 것 (오래 전에 hit) 우선 evict
	// 간소화: 단순히 last_access_time 기준
	return CACHE_EXT_EVICT_NODE;
}

// ===== cache_ext_ops 훅 =====

s32 BPF_STRUCT_OPS_SLEEPABLE(adaptive_v3_init, struct mem_cgroup *memcg)
{
	bpf_printk("Adaptive v3 initializing (5 policies + working set)...\n");

	mru_list = bpf_cache_ext_ds_registry_new_list(memcg);
	fifo_list = bpf_cache_ext_ds_registry_new_list(memcg);
	lru_list = bpf_cache_ext_ds_registry_new_list(memcg);
	s3fifo_small_list = bpf_cache_ext_ds_registry_new_list(memcg);
	s3fifo_main_list = bpf_cache_ext_ds_registry_new_list(memcg);
	lhd_list = bpf_cache_ext_ds_registry_new_list(memcg);

	if (!mru_list || !fifo_list || !lru_list ||
	    !s3fifo_small_list || !s3fifo_main_list || !lhd_list) {
		bpf_printk("Failed to create lists\n");
		return -1;
	}

	current_policy = POLICY_MRU;
	last_policy_switch_time = 0;
	stats[POLICY_MRU].time_started = 0;

	bpf_printk("Adaptive v3 initialized: MRU, FIFO, LRU, S3-FIFO, LHD\n");
	return 0;
}

void BPF_STRUCT_OPS(adaptive_v3_folio_added, struct folio *folio)
{
	if (!is_folio_relevant(folio))
		return;

	u64 key = (u64)folio;
	struct folio_metadata meta = {
		.added_time = timestamp,
		.last_access_time = timestamp,
		.access_count = 0,
		.current_policy = current_policy,
		.freq = 0,
		.in_main = false,
		.last_hit_age = 0,
	};

	// Sequential 감지
	u64 curr_inode = (u64)folio->mapping->host;
	u64 curr_offset = folio->index;

	if (curr_inode == last_inode && curr_offset == last_offset + 1) {
		__sync_fetch_and_add(&sequential_accesses, 1);
	} else {
		__sync_fetch_and_add(&random_accesses, 1);
	}

	last_inode = curr_inode;
	last_offset = curr_offset;

	// 🆕 Working set 업데이트
	u8 dummy = 1;
	bpf_map_update_elem(&working_set_map, &curr_inode, &dummy, BPF_ANY);
	// 근사치: 엔트리 추가마다 증가 (정확하지 않지만 트렌드는 파악)
	__sync_fetch_and_add(&working_set_size, 1);

	bpf_map_update_elem(&folio_metadata_map, &key, &meta, BPF_ANY);

	// 정책별 처리
	switch (current_policy) {
	case POLICY_MRU:
		mru_handle_added(folio);
		break;
	case POLICY_FIFO:
		fifo_handle_added(folio);
		break;
	case POLICY_LRU:
		lru_handle_added(folio);
		break;
	case POLICY_S3FIFO:
		s3fifo_handle_added(folio, &meta);
		break;
	case POLICY_LHD_SIMPLE:
		lhd_handle_added(folio, &meta);
		break;
	}

	__sync_fetch_and_add(&cache_misses, 1);
	__sync_fetch_and_add(&total_accesses, 1);
	update_policy_stats(current_policy, false);
	__sync_fetch_and_add(&timestamp, 1);
}

void BPF_STRUCT_OPS(adaptive_v3_folio_accessed, struct folio *folio)
{
	if (!is_folio_relevant(folio))
		return;

	struct folio_metadata *meta = get_folio_metadata(folio);
	if (!meta)
		return;

	// Reuse distance
	if (meta->access_count > 0) {
		u64 reuse_dist = timestamp - meta->last_access_time;
		__sync_fetch_and_add(&reuse_distance_sum, reuse_dist);
		__sync_fetch_and_add(&reuse_distance_count, 1);
	}

	meta->last_access_time = timestamp;
	meta->access_count++;

	// 정책별 처리
	switch (current_policy) {
	case POLICY_MRU:
		mru_handle_accessed(folio);
		break;
	case POLICY_FIFO:
		break;
	case POLICY_LRU:
		lru_handle_accessed(folio);
		break;
	case POLICY_S3FIFO:
		s3fifo_handle_accessed(folio, meta);
		break;
	case POLICY_LHD_SIMPLE:
		lhd_handle_accessed(folio, meta);
		break;
	}

	__sync_fetch_and_add(&cache_hits, 1);
	__sync_fetch_and_add(&total_accesses, 1);
	update_policy_stats(current_policy, true);
	__sync_fetch_and_add(&timestamp, 1);
}

void BPF_STRUCT_OPS(adaptive_v3_folio_evicted, struct folio *folio)
{
	u64 key = (u64)folio;
	struct folio_metadata *meta = get_folio_metadata(folio);

	if (meta) {
		// One-time vs Multi
		if (meta->access_count <= 1) {
			__sync_fetch_and_add(&one_time_accesses, 1);
		} else {
			__sync_fetch_and_add(&multi_accesses, 1);
		}

		// Hits per page
		__sync_fetch_and_add(&total_hits_sum, meta->access_count);
		__sync_fetch_and_add(&pages_evicted, 1);

		// Lifetime & Idle
		u64 lifetime = timestamp - meta->added_time;
		u64 idle_time = timestamp - meta->last_access_time;
		__sync_fetch_and_add(&total_lifetime_sum, lifetime);
		__sync_fetch_and_add(&total_idle_time_sum, idle_time);

		// S3-FIFO size 업데이트
		if (current_policy == POLICY_S3FIFO) {
			if (meta->in_main)
				__sync_fetch_and_sub(&s3fifo_main_size, 1);
			else
				__sync_fetch_and_sub(&s3fifo_small_size, 1);
		}
	}

	if (folio_test_dirty(folio)) {
		__sync_fetch_and_add(&dirty_evictions, 1);
	}

	bpf_cache_ext_list_del(folio);
	bpf_map_delete_elem(&folio_metadata_map, &key);

	__sync_fetch_and_add(&total_evictions, 1);
	__sync_fetch_and_add(&stats[current_policy].evictions, 1);
}

void BPF_STRUCT_OPS(adaptive_v3_evict_folios,
		    struct cache_ext_eviction_ctx *ctx,
		    struct mem_cgroup *memcg)
{
	int ret = 0;

	if ((total_accesses % CHECK_INTERVAL) == 0) {
		check_and_switch_policy();
	}

	// 정책별 eviction
	switch (current_policy) {
	case POLICY_MRU:
		ret = bpf_cache_ext_list_iterate(memcg, mru_list, mru_iterate_fn, ctx);
		break;
	case POLICY_FIFO:
		ret = bpf_cache_ext_list_iterate(memcg, fifo_list, fifo_iterate_fn, ctx);
		break;
	case POLICY_LRU:
		ret = bpf_cache_ext_list_iterate(memcg, lru_list, lru_iterate_fn, ctx);
		break;
	case POLICY_S3FIFO:
		// Small queue 우선
		if (s3fifo_small_size >= CACHE_SIZE_ESTIMATE / 10) {
			struct cache_ext_iterate_opts opts = {
				.continue_list = s3fifo_main_list,
				.continue_mode = CACHE_EXT_ITERATE_TAIL,
				.evict_list = CACHE_EXT_ITERATE_SELF,
				.evict_mode = CACHE_EXT_ITERATE_TAIL,
			};
			ret = bpf_cache_ext_list_iterate_extended(memcg, s3fifo_small_list,
								  s3fifo_small_iterate_fn,
								  &opts, ctx);
		} else {
			ret = bpf_cache_ext_list_iterate(memcg, s3fifo_main_list,
							 s3fifo_main_iterate_fn, ctx);
		}
		break;
	case POLICY_LHD_SIMPLE:
		ret = bpf_cache_ext_list_iterate(memcg, lhd_list, lhd_iterate_fn, ctx);
		break;
	}

	if (ret < 0) {
		bpf_printk("Eviction failed: %d\n", ret);
	}
}

SEC(".struct_ops.link")
struct cache_ext_ops adaptive_v3_ops = {
	.init = (void *)adaptive_v3_init,
	.evict_folios = (void *)adaptive_v3_evict_folios,
	.folio_accessed = (void *)adaptive_v3_folio_accessed,
	.folio_evicted = (void *)adaptive_v3_folio_evicted,
	.folio_added = (void *)adaptive_v3_folio_added,
};
