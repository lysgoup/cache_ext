#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include <bpf/bpf_core_read.h>

#include "cache_ext_lib.bpf.h"
#include "dir_watcher.bpf.h"

char _license[] SEC("license") = "GPL";

// ===== 정책 타입 정의 =====
enum policy_type {
	POLICY_MRU = 0,
	POLICY_FIFO = 1,
	POLICY_LRU = 2,
};

// ===== 설정 가능한 파라미터 =====
#define HIT_RATE_THRESHOLD 30
#define MIN_SAMPLES 1000
#define MIN_TIME_IN_POLICY 10000
#define CHECK_INTERVAL 1000

// ===== 전역 통계 =====
static u64 timestamp = 0;

// 기본 성능 메트릭
static u64 total_accesses = 0;
static u64 cache_hits = 0;
static u64 cache_misses = 0;
static u64 total_evictions = 0;

// 🆕 One-time access 메트릭
static u64 one_time_accesses = 0;
static u64 multi_accesses = 0;

// 🆕 Sequential access 메트릭
static u64 last_inode = 0;
static u64 last_offset = 0;
static u64 sequential_accesses = 0;
static u64 random_accesses = 0;

// 🆕 Average hits per page
static u64 total_hits_sum = 0;
static u64 pages_evicted = 0;

// 🆕 Reuse distance
static u64 reuse_distance_sum = 0;
static u64 reuse_distance_count = 0;

// 🆕 Lifetime & Idle time
static u64 total_lifetime_sum = 0;
static u64 total_idle_time_sum = 0;

// 🆕 Dirty pages
static u64 dirty_evictions = 0;

// 🆕 Per-policy 통계
struct policy_stats {
	u64 hits;
	u64 misses;
	u64 evictions;
	u64 time_started;
	u64 time_active;
};

static struct policy_stats mru_stats = {0};
static struct policy_stats fifo_stats = {0};
static struct policy_stats lru_stats = {0};

// 정책 전환 관련
static u32 current_policy = POLICY_MRU;
static u64 last_policy_switch_time = 0;
static u32 policy_switch_count = 0;

// 각 정책별 리스트
static u64 mru_list = 0;
static u64 fifo_list = 0;
static u64 lru_list = 0;

// ===== Per-folio 메타데이터 =====
struct folio_metadata {
	u64 added_time;
	u64 last_access_time;
	u64 access_count;
	u32 current_policy;
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, u64);
	__type(value, struct folio_metadata);
	__uint(max_entries, 4000000);
} folio_metadata_map SEC(".maps");

// ===== 정책 전환 이벤트 =====
struct policy_switch_event {
	u32 old_policy;
	u32 new_policy;
	u64 timestamp;

	// 기본 메트릭
	u64 hit_rate;
	u64 total_accesses;

	// 🆕 추가 메트릭
	u64 one_time_ratio;
	u64 sequential_ratio;
	u64 avg_hits_per_page;
	u64 avg_reuse_distance;
	u64 dirty_ratio;

	// 🆕 이전 정책 성능
	u64 old_policy_hit_rate;
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

// ===== 메트릭 계산 함수 =====
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

static inline u64 calculate_policy_hit_rate(struct policy_stats *stats)
{
	u64 total = stats->hits + stats->misses;
	if (total == 0)
		return 0;
	return (stats->hits * 100) / total;
}

// ===== Per-policy 통계 업데이트 =====
static inline void update_policy_stats(u32 policy, bool is_hit)
{
	switch (policy) {
	case POLICY_MRU:
		if (is_hit)
			__sync_fetch_and_add(&mru_stats.hits, 1);
		else
			__sync_fetch_and_add(&mru_stats.misses, 1);
		break;
	case POLICY_FIFO:
		if (is_hit)
			__sync_fetch_and_add(&fifo_stats.hits, 1);
		else
			__sync_fetch_and_add(&fifo_stats.misses, 1);
		break;
	case POLICY_LRU:
		if (is_hit)
			__sync_fetch_and_add(&lru_stats.hits, 1);
		else
			__sync_fetch_and_add(&lru_stats.misses, 1);
		break;
	}
}

// ===== 정책 선택 로직 =====
static u32 decide_best_policy(void)
{
	u64 one_time_ratio = calculate_one_time_ratio();
	u64 sequential_ratio = calculate_sequential_ratio();
	u64 avg_hits = calculate_avg_hits_per_page();
	u64 avg_reuse_dist = calculate_avg_reuse_distance();

	// 1. Sequential scan 감지 (가장 명확한 신호)
	if (sequential_ratio > 80) {
		bpf_printk("Decision: FIFO (sequential_ratio=%llu%%)\n",
		           sequential_ratio);
		return POLICY_FIFO;
	}

	// 2. One-time scan 감지
	if (one_time_ratio > 60 && avg_hits < 2) {
		bpf_printk("Decision: FIFO (one_time_ratio=%llu%%, avg_hits=%llu)\n",
		           one_time_ratio, avg_hits);
		return POLICY_FIFO;
	}

	// 3. Hot working set (많은 재접근)
	if (avg_hits > 5 && one_time_ratio < 30) {
		bpf_printk("Decision: MRU (avg_hits=%llu, one_time_ratio=%llu%%)\n",
		           avg_hits, one_time_ratio);
		return POLICY_MRU;
	}

	// 4. 재사용 거리가 적당 (temporal locality)
	if (avg_reuse_dist > 0 && avg_reuse_dist < 50000) {
		bpf_printk("Decision: LRU (avg_reuse_distance=%llu)\n",
		           avg_reuse_dist);
		return POLICY_LRU;
	}

	// 5. 과거 성능 기반 선택
	u64 mru_perf = calculate_policy_hit_rate(&mru_stats);
	u64 fifo_perf = calculate_policy_hit_rate(&fifo_stats);
	u64 lru_perf = calculate_policy_hit_rate(&lru_stats);

	if (mru_perf >= fifo_perf && mru_perf >= lru_perf) {
		bpf_printk("Decision: MRU (best past performance: %llu%%)\n", mru_perf);
		return POLICY_MRU;
	} else if (fifo_perf >= lru_perf) {
		bpf_printk("Decision: FIFO (best past performance: %llu%%)\n", fifo_perf);
		return POLICY_FIFO;
	} else {
		bpf_printk("Decision: LRU (best past performance: %llu%%)\n", lru_perf);
		return POLICY_LRU;
	}
}

// ===== 정책 전환 체크 =====
static void check_and_switch_policy(void)
{
	u64 hit_rate;
	u32 new_policy;
	struct policy_switch_event *event;
	struct policy_stats *old_stats;

	// 1. 충분한 샘플 확인
	if (total_accesses < MIN_SAMPLES)
		return;

	// 2. 최소 대기 시간 확인
	u64 time_since_switch = timestamp - last_policy_switch_time;
	if (time_since_switch < MIN_TIME_IN_POLICY)
		return;

	// 3. 히트율 계산
	hit_rate = calculate_hit_rate();

	// 4. 성능이 충분히 좋으면 유지
	if (hit_rate >= HIT_RATE_THRESHOLD)
		return;

	// 5. 새 정책 결정 (더 정교한 로직)
	new_policy = decide_best_policy();

	if (new_policy == current_policy)
		return;

	// 6. 현재 정책 통계 저장
	switch (current_policy) {
	case POLICY_MRU:
		old_stats = &mru_stats;
		break;
	case POLICY_FIFO:
		old_stats = &fifo_stats;
		break;
	case POLICY_LRU:
		old_stats = &lru_stats;
		break;
	default:
		old_stats = &mru_stats;
	}

	old_stats->time_active = timestamp - old_stats->time_started;

	// 7. 정책 전환 이벤트 발생
	event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
	if (event) {
		event->old_policy = current_policy;
		event->new_policy = new_policy;
		event->timestamp = timestamp;
		event->hit_rate = hit_rate;
		event->total_accesses = total_accesses;

		// 추가 메트릭
		event->one_time_ratio = calculate_one_time_ratio();
		event->sequential_ratio = calculate_sequential_ratio();
		event->avg_hits_per_page = calculate_avg_hits_per_page();
		event->avg_reuse_distance = calculate_avg_reuse_distance();
		event->dirty_ratio = calculate_dirty_ratio();
		event->old_policy_hit_rate = calculate_policy_hit_rate(old_stats);

		bpf_ringbuf_submit(event, 0);
	}

	bpf_printk("Policy switch: %d -> %d (hit_rate: %llu%%)\n",
		   current_policy, new_policy, hit_rate);

	// 8. 정책 전환
	current_policy = new_policy;
	last_policy_switch_time = timestamp;
	policy_switch_count++;

	// 9. 새 정책 통계 시작
	switch (new_policy) {
	case POLICY_MRU:
		mru_stats.time_started = timestamp;
		break;
	case POLICY_FIFO:
		fifo_stats.time_started = timestamp;
		break;
	case POLICY_LRU:
		lru_stats.time_started = timestamp;
		break;
	}

	// 10. 윈도우 통계 리셋
	total_accesses = 0;
	cache_hits = 0;
	cache_misses = 0;
}

// ===== 개별 정책 로직 =====
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

// ===== cache_ext_ops 훅 구현 =====

s32 BPF_STRUCT_OPS_SLEEPABLE(adaptive_v2_init, struct mem_cgroup *memcg)
{
	bpf_printk("Adaptive v2 policy initializing...\n");

	mru_list = bpf_cache_ext_ds_registry_new_list(memcg);
	if (!mru_list) {
		bpf_printk("Failed to create mru_list\n");
		return -1;
	}

	fifo_list = bpf_cache_ext_ds_registry_new_list(memcg);
	if (!fifo_list) {
		bpf_printk("Failed to create fifo_list\n");
		return -1;
	}

	lru_list = bpf_cache_ext_ds_registry_new_list(memcg);
	if (!lru_list) {
		bpf_printk("Failed to create lru_list\n");
		return -1;
	}

	current_policy = POLICY_MRU;
	last_policy_switch_time = 0;

	// 초기 정책 통계 시작
	mru_stats.time_started = 0;

	bpf_printk("Adaptive v2 initialized with enhanced metrics\n");
	return 0;
}

void BPF_STRUCT_OPS(adaptive_v2_folio_added, struct folio *folio)
{
	if (!is_folio_relevant(folio))
		return;

	u64 key = (u64)folio;
	struct folio_metadata meta = {
		.added_time = timestamp,
		.last_access_time = timestamp,
		.access_count = 0,
		.current_policy = current_policy,
	};

	// 🆕 Sequential 패턴 감지
	u64 curr_inode = (u64)folio->mapping->host;
	u64 curr_offset = folio->index;

	if (curr_inode == last_inode && curr_offset == last_offset + 1) {
		__sync_fetch_and_add(&sequential_accesses, 1);
	} else {
		__sync_fetch_and_add(&random_accesses, 1);
	}

	last_inode = curr_inode;
	last_offset = curr_offset;

	bpf_map_update_elem(&folio_metadata_map, &key, &meta, BPF_ANY);

	// 현재 활성 정책에 따라 처리
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
	}

	// 통계 업데이트
	__sync_fetch_and_add(&cache_misses, 1);
	__sync_fetch_and_add(&total_accesses, 1);
	update_policy_stats(current_policy, false);
	__sync_fetch_and_add(&timestamp, 1);
}

void BPF_STRUCT_OPS(adaptive_v2_folio_accessed, struct folio *folio)
{
	if (!is_folio_relevant(folio))
		return;

	struct folio_metadata *meta = get_folio_metadata(folio);
	if (!meta)
		return;

	// 🆕 Reuse distance 계산
	if (meta->access_count > 0) {
		u64 reuse_dist = timestamp - meta->last_access_time;
		__sync_fetch_and_add(&reuse_distance_sum, reuse_dist);
		__sync_fetch_and_add(&reuse_distance_count, 1);
	}

	// 메타데이터 업데이트
	meta->last_access_time = timestamp;
	meta->access_count++;

	// 현재 활성 정책에 따라 처리
	switch (current_policy) {
	case POLICY_MRU:
		mru_handle_accessed(folio);
		break;
	case POLICY_FIFO:
		break;
	case POLICY_LRU:
		lru_handle_accessed(folio);
		break;
	}

	// 통계 업데이트
	__sync_fetch_and_add(&cache_hits, 1);
	__sync_fetch_and_add(&total_accesses, 1);
	update_policy_stats(current_policy, true);
	__sync_fetch_and_add(&timestamp, 1);
}

void BPF_STRUCT_OPS(adaptive_v2_folio_evicted, struct folio *folio)
{
	u64 key = (u64)folio;
	struct folio_metadata *meta = get_folio_metadata(folio);

	if (meta) {
		// 🆕 One-time vs Multi-access
		if (meta->access_count <= 1) {
			__sync_fetch_and_add(&one_time_accesses, 1);
		} else {
			__sync_fetch_and_add(&multi_accesses, 1);
		}

		// 🆕 Average hits per page
		__sync_fetch_and_add(&total_hits_sum, meta->access_count);
		__sync_fetch_and_add(&pages_evicted, 1);

		// 🆕 Lifetime & Idle time
		u64 lifetime = timestamp - meta->added_time;
		u64 idle_time = timestamp - meta->last_access_time;
		__sync_fetch_and_add(&total_lifetime_sum, lifetime);
		__sync_fetch_and_add(&total_idle_time_sum, idle_time);
	}

	// 🆕 Dirty page ratio
	if (folio_test_dirty(folio)) {
		__sync_fetch_and_add(&dirty_evictions, 1);
	}

	// 리스트에서 제거
	bpf_cache_ext_list_del(folio);

	// 메타데이터 삭제
	bpf_map_delete_elem(&folio_metadata_map, &key);

	__sync_fetch_and_add(&total_evictions, 1);

	// Per-policy eviction count
	switch (current_policy) {
	case POLICY_MRU:
		__sync_fetch_and_add(&mru_stats.evictions, 1);
		break;
	case POLICY_FIFO:
		__sync_fetch_and_add(&fifo_stats.evictions, 1);
		break;
	case POLICY_LRU:
		__sync_fetch_and_add(&lru_stats.evictions, 1);
		break;
	}
}

void BPF_STRUCT_OPS(adaptive_v2_evict_folios,
		    struct cache_ext_eviction_ctx *eviction_ctx,
		    struct mem_cgroup *memcg)
{
	int ret = 0;

	// 주기적으로 정책 전환 체크
	if ((total_accesses % CHECK_INTERVAL) == 0) {
		check_and_switch_policy();
	}

	// 현재 활성 정책으로 eviction 수행
	switch (current_policy) {
	case POLICY_MRU:
		ret = bpf_cache_ext_list_iterate(memcg, mru_list,
						 mru_iterate_fn, eviction_ctx);
		break;
	case POLICY_FIFO:
		ret = bpf_cache_ext_list_iterate(memcg, fifo_list,
						 fifo_iterate_fn, eviction_ctx);
		break;
	case POLICY_LRU:
		ret = bpf_cache_ext_list_iterate(memcg, lru_list,
						 lru_iterate_fn, eviction_ctx);
		break;
	}

	if (ret < 0) {
		bpf_printk("Eviction failed: %d\n", ret);
	}
}

SEC(".struct_ops.link")
struct cache_ext_ops adaptive_v2_ops = {
	.init = (void *)adaptive_v2_init,
	.evict_folios = (void *)adaptive_v2_evict_folios,
	.folio_accessed = (void *)adaptive_v2_folio_accessed,
	.folio_evicted = (void *)adaptive_v2_folio_evicted,
	.folio_added = (void *)adaptive_v2_folio_added,
};
