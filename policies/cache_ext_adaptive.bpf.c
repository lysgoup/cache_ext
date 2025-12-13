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
#define HIT_RATE_THRESHOLD 30        // 30% 아래로 떨어지면 전환 고려
#define MIN_SAMPLES 1000              // 최소 샘플 수
#define MIN_TIME_IN_POLICY 10000     // 최소 10000 timestamp (정책 전환 후 대기)
#define CHECK_INTERVAL 1000          // 1000번마다 체크

// ===== 전역 통계 =====
static u64 timestamp = 0;

// 성능 메트릭
static u64 total_accesses = 0;
static u64 cache_hits = 0;
static u64 cache_misses = 0;
static u64 total_evictions = 0;

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
	u32 current_policy;  // 어느 정책의 리스트에 있는지
};

struct {
	__uint(type, BPF_MAP_TYPE_HASH);
	__type(key, u64);
	__type(value, struct folio_metadata);
	__uint(max_entries, 4000000);
} folio_metadata_map SEC(".maps");

// ===== 정책 전환 이벤트 (userspace로 전달) =====
struct policy_switch_event {
	u32 old_policy;
	u32 new_policy;
	u64 timestamp;
	u64 hit_rate;
	u64 total_accesses;
};

struct {
	__uint(type, BPF_MAP_TYPE_RINGBUF);
	__uint(max_entries, 4096);
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

static inline u64 calculate_hit_rate(void)
{
	if (total_accesses == 0)
		return 0;
	return (cache_hits * 100) / total_accesses;
}

// ===== 정책 전환 로직 =====
static void check_and_switch_policy(void)
{
	u64 hit_rate;
	u32 new_policy;
	struct policy_switch_event *event;

	// 1. 충분한 샘플이 모였는지 확인
	if (total_accesses < MIN_SAMPLES)
		return;

	// 2. 최소 대기 시간 확인
	u64 time_since_switch = timestamp - last_policy_switch_time;
	if (time_since_switch < MIN_TIME_IN_POLICY)
		return;

	// 3. 히트율 계산
	hit_rate = calculate_hit_rate();

	// 4. 히트율이 임계값 이하면 다른 정책으로 전환
	if (hit_rate >= HIT_RATE_THRESHOLD)
		return;  // 성능 괜찮음, 유지

	// 5. 다음 정책 선택 (Round-robin)
	new_policy = (current_policy + 1) % 3;

	if (new_policy == current_policy)
		return;

	// 6. 정책 전환 이벤트 발생
	event = bpf_ringbuf_reserve(&events, sizeof(*event), 0);
	if (event) {
		event->old_policy = current_policy;
		event->new_policy = new_policy;
		event->timestamp = timestamp;
		event->hit_rate = hit_rate;
		event->total_accesses = total_accesses;
		bpf_ringbuf_submit(event, 0);
	}

	bpf_printk("Policy switch: %d -> %d (hit_rate: %llu%%, accesses: %llu)\n",
		   current_policy, new_policy, hit_rate, total_accesses);

	// 7. 정책 전환
	current_policy = new_policy;
	last_policy_switch_time = timestamp;
	policy_switch_count++;

	// 8. 통계 리셋
	total_accesses = 0;
	cache_hits = 0;
	cache_misses = 0;
}

// ===== 개별 정책 로직 =====

// MRU 정책
static void mru_handle_added(struct folio *folio)
{
	bpf_cache_ext_list_add(mru_list, folio);
}

static void mru_handle_accessed(struct folio *folio)
{
	bpf_cache_ext_list_move(mru_list, folio, false);  // head로 이동
}

static int mru_iterate_fn(int idx, struct cache_ext_list_node *node)
{
	if ((idx < 200) && (!folio_test_uptodate(node->folio) ||
			    !folio_test_lru(node->folio)))
		return CACHE_EXT_CONTINUE_ITER;
	return CACHE_EXT_EVICT_NODE;
}

// FIFO 정책
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

// LRU 정책
static void lru_handle_added(struct folio *folio)
{
	bpf_cache_ext_list_add_tail(lru_list, folio);
}

static void lru_handle_accessed(struct folio *folio)
{
	bpf_cache_ext_list_move(lru_list, folio, true);  // tail로 이동
}

static int lru_iterate_fn(int idx, struct cache_ext_list_node *node)
{
	if (!folio_test_uptodate(node->folio) || !folio_test_lru(node->folio))
		return CACHE_EXT_CONTINUE_ITER;
	return CACHE_EXT_EVICT_NODE;
}

// ===== cache_ext_ops 훅 구현 =====

s32 BPF_STRUCT_OPS_SLEEPABLE(adaptive_init, struct mem_cgroup *memcg)
{
	bpf_printk("Adaptive policy initializing...\n");

	// 모든 정책의 리스트 생성
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

	// 초기 정책은 MRU
	current_policy = POLICY_MRU;
	last_policy_switch_time = 0;

	bpf_printk("Adaptive policy initialized with MRU, FIFO, LRU\n");
	return 0;
}

void BPF_STRUCT_OPS(adaptive_folio_added, struct folio *folio)
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

	// 메타데이터 저장
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
	__sync_fetch_and_add(&timestamp, 1);
}

void BPF_STRUCT_OPS(adaptive_folio_accessed, struct folio *folio)
{
	if (!is_folio_relevant(folio))
		return;

	struct folio_metadata *meta = get_folio_metadata(folio);
	if (!meta)
		return;

	// 메타데이터 업데이트
	meta->last_access_time = timestamp;
	meta->access_count++;

	// 현재 활성 정책에 따라 처리
	switch (current_policy) {
	case POLICY_MRU:
		mru_handle_accessed(folio);
		break;
	case POLICY_FIFO:
		// FIFO는 accessed에서 아무것도 안 함
		break;
	case POLICY_LRU:
		lru_handle_accessed(folio);
		break;
	}

	// 통계 업데이트
	__sync_fetch_and_add(&cache_hits, 1);
	__sync_fetch_and_add(&total_accesses, 1);
	__sync_fetch_and_add(&timestamp, 1);
}

void BPF_STRUCT_OPS(adaptive_folio_evicted, struct folio *folio)
{
	u64 key = (u64)folio;

	// 리스트에서 제거 (모든 리스트에서 시도 - 하나에만 있을 것)
	bpf_cache_ext_list_del(folio);

	// 메타데이터 삭제
	bpf_map_delete_elem(&folio_metadata_map, &key);

	__sync_fetch_and_add(&total_evictions, 1);
}

void BPF_STRUCT_OPS(adaptive_evict_folios,
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
struct cache_ext_ops adaptive_ops = {
	.init = (void *)adaptive_init,
	.evict_folios = (void *)adaptive_evict_folios,
	.folio_accessed = (void *)adaptive_folio_accessed,
	.folio_evicted = (void *)adaptive_folio_evicted,
	.folio_added = (void *)adaptive_folio_added,
};
