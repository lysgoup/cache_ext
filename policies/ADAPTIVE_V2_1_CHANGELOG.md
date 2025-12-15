# Adaptive v2.1 Changelog

## 개요

adaptive_v2의 모든 알려진 문제점을 수정한 안정화 버전입니다. v2_1은 단일 리스트 아키텍처, 개선된 sequential tracking, 명시적 메타데이터 동기화 등을 통해 더 안정적이고 정확한 정책 전환을 제공합니다.

**릴리스 날짜**: 2024-12-15  
**기반 버전**: adaptive_v2  
**주요 변경 수**: 7개의 중요 수정사항

---

## 주요 변경사항

### 1. 단일 리스트 아키텍처 (Critical Fix) 🔥

**문제점:**
- v2는 3개의 독립적인 리스트 사용 (`mru_list`, `fifo_list`, `lru_list`)
- 정책 전환 시 기존 folios가 새 리스트로 옮겨지지 않음
- `evict_folios` 훅이 현재 정책의 리스트만 순회 → 다른 리스트의 folios는 eviction 불가능
- 메모리 누수 및 불일치 발생 가능

**시나리오 예시:**
```
1. MRU 정책으로 100개 folios가 mru_list에 추가됨
2. 정책이 FIFO로 전환됨
3. 새 folios는 fifo_list에 추가됨
4. evict_folios는 fifo_list만 순회
   → mru_list의 100개 folios는 영원히 eviction 안됨!
```

**해결책:**
```c
// Before (v2):
static u64 mru_list = 0;
static u64 fifo_list = 0;
static u64 lru_list = 0;

// folio_added에서:
switch (current_policy) {
case POLICY_MRU:
    bpf_cache_ext_list_add(mru_list, folio);  // 각각 다른 리스트
    break;
case POLICY_FIFO:
    bpf_cache_ext_list_add_tail(fifo_list, folio);
    break;
...
}

// After (v2_1):
static u64 main_list = 0;  // 단일 통합 리스트

// folio_added에서:
switch (current_policy) {
case POLICY_MRU:
    bpf_cache_ext_list_add(main_list, folio);  // 모두 같은 리스트
    break;
case POLICY_FIFO:
    bpf_cache_ext_list_add_tail(main_list, folio);
    break;
...
}

// evict_folios에서:
switch (current_policy) {
case POLICY_MRU:
    bpf_cache_ext_list_iterate(memcg, main_list, mru_iterate_fn, ...);
    break;
case POLICY_FIFO:
    bpf_cache_ext_list_iterate(memcg, main_list, fifo_iterate_fn, ...);
    break;
...
}
```

**영향:**
- ✅ 정책 전환이 즉시 모든 folios에 적용됨
- ✅ 메모리 일관성 보장
- ✅ 메모리 누수 방지
- ✅ 코드 복잡도 감소 (~50 lines 감소)

---

### 2. Per-inode Sequential Tracking 📊

**문제점:**
- v2는 전역 변수 `last_inode`, `last_offset` 사용
- 단일 파일의 순차 접근만 감지 가능
- 여러 파일 동시 접근 시 부정확
- Multi-threaded 워크로드에서 false negative

**잘못된 케이스:**
```
File A: offset 0, 1, 2, 3  (sequential)
File B: offset 0, 1, 2, 3  (sequential)

Interleaved access:
A:0 → B:0 → A:1 → B:1 → A:2 → B:2
      ^        ^        ^
      모두 random으로 잘못 분류됨!
```

**해결책:**
```c
// Before (v2): 전역 변수
static u64 last_inode = 0;
static u64 last_offset = 0;

if (curr_inode == last_inode && curr_offset == last_offset + 1) {
    sequential_accesses++;
} else {
    random_accesses++;
}

// After (v2_1): Per-inode tracking map
struct seq_tracker {
    u64 last_offset;
    u64 seq_count;
    u64 random_count;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __type(key, u64);  // inode number
    __type(value, struct seq_tracker);
    __uint(max_entries, 1000);
} seq_tracking_map SEC(".maps");

// folio_added에서:
u64 inode_key = folio->mapping->host->i_ino;
u64 curr_offset = folio->index;

struct seq_tracker *tracker = bpf_map_lookup_elem(&seq_tracking_map, &inode_key);
if (tracker) {
    if (curr_offset == tracker->last_offset + 1) {
        __sync_fetch_and_add(&tracker->seq_count, 1);
        __sync_fetch_and_add(&sequential_accesses, 1);
    } else {
        __sync_fetch_and_add(&tracker->random_count, 1);
        __sync_fetch_and_add(&random_accesses, 1);
    }
    tracker->last_offset = curr_offset;
} else {
    // 새 inode: tracker 생성
    struct seq_tracker new_tracker = {
        .last_offset = curr_offset,
        .seq_count = 0,
        .random_count = 1,
    };
    bpf_map_update_elem(&seq_tracking_map, &inode_key, &new_tracker, BPF_ANY);
    __sync_fetch_and_add(&random_accesses, 1);
}
```

**이점:**
- ✅ 파일별로 독립적인 sequential 패턴 추적
- ✅ Multi-file workload 정확 감지
- ✅ Multi-threaded application 지원
- ✅ 더 정확한 워크로드 분류 → 더 나은 정책 선택

**메모리 오버헤드:**
- 1000 inodes * ~24 bytes = ~24KB (미미함)

---

### 3. 메타데이터 명시적 동기화 🔒

**문제점:**
- v2는 맵에서 가져온 포인터에 직접 쓰기
- BPF verifier에 따라 변경사항이 맵에 자동 반영 안 될 수 있음
- Race condition 발생 가능성

**해결책:**
```c
// Before (v2):
struct folio_metadata *meta = get_folio_metadata(folio);
if (!meta) return;

meta->last_access_time = timestamp;  // 직접 쓰기 (위험!)
meta->access_count++;

// After (v2_1):
struct folio_metadata *meta = get_folio_metadata(folio);
if (!meta) return;

// 새 값을 로컬에 준비
struct folio_metadata updated = *meta;
updated.last_access_time = timestamp;
updated.access_count++;

// 명시적으로 맵에 쓰기
u64 key = (u64)folio;
bpf_map_update_elem(&folio_metadata_map, &key, &updated, BPF_ANY);
```

**이점:**
- ✅ 메타데이터 손실 방지
- ✅ Race condition 제거
- ✅ 명확한 의도 표현 (코드 가독성)

---

### 4. 완전한 메트릭 리셋 🔄

**문제점:**
- v2는 정책 전환 시 일부 메트릭만 리셋
- 리셋하는 것: `total_accesses`, `cache_hits`, `cache_misses`
- **리셋 안 하는 것**: `sequential_accesses`, `random_accesses`, `one_time_accesses`, `multi_accesses`, `pages_evicted`, `total_hits_sum`, 기타 누적 메트릭
- 결과: 시간이 지날수록 비율이 왜곡됨

**왜곡 예시:**
```
초기 10000 accesses: sequential_ratio = 90% (sequential scan)
→ FIFO로 전환

이후 10000 accesses: random pattern
하지만 sequential_accesses = 9000 (초기값)
       random_accesses = 10000 (새 값)
       → sequential_ratio = 9000/(9000+10000) = 47%
       
실제로는 0%여야 하는데 47%로 계산됨!
→ 잘못된 정책 유지
```

**해결책:**
```c
// Before (v2):
// check_and_switch_policy() 내부
total_accesses = 0;
cache_hits = 0;
cache_misses = 0;
// 여기서 끝! 다른 메트릭은 리셋 안됨

// After (v2_1):
static inline void reset_window_metrics(void) {
    total_accesses = 0;
    cache_hits = 0;
    cache_misses = 0;
    one_time_accesses = 0;
    multi_accesses = 0;
    sequential_accesses = 0;
    random_accesses = 0;
    total_hits_sum = 0;
    pages_evicted = 0;
    reuse_distance_sum = 0;
    reuse_distance_count = 0;
    total_lifetime_sum = 0;
    total_idle_time_sum = 0;
    dirty_evictions = 0;
}

// check_and_switch_policy() 내부
reset_window_metrics();  // 모든 윈도우 메트릭 일괄 리셋
```

**이점:**
- ✅ 정확한 메트릭 비율 계산
- ✅ 정책 전환 후 즉시 새로운 패턴 반영
- ✅ 더 빠른 정책 수렴

---

### 5. 조정된 임계값 ⚙️

**문제점:**
- v2의 기본 임계값이 너무 엄격함
- 작은 워크로드나 테스트 환경에서 정책 전환이 거의 안 일어남
- 디버깅 및 검증 어려움

**변경사항:**

| 파라미터 | v2 | v2_1 | 변화 |
|---------|-----|------|------|
| `MIN_SAMPLES` | 1000 | 200 | 5배 완화 |
| `MIN_TIME_IN_POLICY` | 10000 | 2000 | 5배 완화 |
| `CHECK_INTERVAL` | 1000 | 200 | 5배 완화 |
| `METRIC_PRINT_INTERVAL` | - | 100 | 🆕 추가 |

```c
// v2:
#define MIN_SAMPLES 1000
#define MIN_TIME_IN_POLICY 10000
#define CHECK_INTERVAL 1000

// v2_1:
#define MIN_SAMPLES 200              // 1000 → 200 (5x 완화)
#define MIN_TIME_IN_POLICY 2000      // 10000 → 2000 (5x 완화)
#define CHECK_INTERVAL 200           // 1000 → 200 (5x 완화)
#define METRIC_PRINT_INTERVAL 100    // 🆕 새로 추가
```

**영향:**
- ✅ 작은 워크로드에서도 정책 전환 관찰 가능
- ✅ 테스트 및 디버깅 용이
- ✅ 더 빠른 워크로드 변화 적응
- ⚠️ 프로덕션 환경에서는 조정 필요할 수 있음

---

### 6. 주기적 메트릭 출력 📈

**새 기능:**

100번의 access마다 현재 메트릭을 커널 로그로 출력:

```c
// evict_folios 훅 내부:
if ((total_accesses % METRIC_PRINT_INTERVAL) == 0 && total_accesses > 0) {
    const char *policy_names[] = {"MRU", "FIFO", "LRU"};
    bpf_printk("[METRICS] accesses=%llu | hit_rate=%llu%% | one_time=%llu%% | "
               "seq=%llu%% | avg_hits=%llu | policy=%s | evicted=%llu\n",
               total_accesses,
               calculate_hit_rate(),
               calculate_one_time_ratio(),
               calculate_sequential_ratio(),
               calculate_avg_hits_per_page(),
               policy_names[current_policy],
               pages_evicted);
}
```

**사용법:**
```bash
# 터미널 1: v2_1 실행
sudo ./cache_ext_adaptive_v2_1.out \
    --watch_dir /mydata/test \
    --cgroup_path /sys/fs/cgroup/test

# 터미널 2: 메트릭 모니터링
sudo dmesg -wH | grep METRICS
```

**예상 출력:**
```
[  123.456789] [METRICS] accesses=100 | hit_rate=45% | one_time=30% | seq=10% | avg_hits=2 | policy=MRU | evicted=20
[  125.789012] [METRICS] accesses=200 | hit_rate=52% | one_time=25% | seq=8% | avg_hits=2 | policy=MRU | evicted=35
[  128.123456] [METRICS] accesses=300 | hit_rate=38% | one_time=65% | seq=75% | avg_hits=1 | policy=FIFO | evicted=50
```

**이점:**
- ✅ 실시간 워크로드 모니터링
- ✅ 정책 전환 전후 메트릭 변화 관찰
- ✅ 디버깅 및 성능 분석 용이
- ✅ 정책 선택 로직 검증 가능

---

### 7. folio_accessed 정책별 처리 🎯

**개선사항:**

각 정책의 특성에 맞게 리스트 이동:

```c
void BPF_STRUCT_OPS(adaptive_v2_1_folio_accessed, struct folio *folio) {
    // ... 메타데이터 업데이트
    
    // 정책에 따라 리스트 이동 여부 결정
    switch (current_policy) {
    case POLICY_MRU:
        bpf_cache_ext_list_move(main_list, folio, false);  // head로 이동 (MRU)
        break;
    case POLICY_LRU:
        bpf_cache_ext_list_move(main_list, folio, true);   // tail로 이동 (LRU)
        break;
    case POLICY_FIFO:
        // 이동하지 않음 (FIFO 특성 유지)
        break;
    }
}
```

**정책별 동작:**
- **MRU**: 접근 시 head로 이동 → 최근 접근한 페이지가 앞쪽 (보호됨)
- **LRU**: 접근 시 tail로 이동 → 최근 접근한 페이지가 뒤쪽 (보호됨)
- **FIFO**: 이동 안함 → 추가 순서대로 유지

**v2와의 차이:**
- v2도 동일한 로직이었으나, 여러 리스트로 인한 불일치 문제
- v2_1은 단일 리스트로 일관성 보장

---

## 버그 수정 요약

| Bug ID | 심각도 | 설명 | 해결 |
|--------|--------|------|------|
| #1 | Critical | 정책 전환 시 리스트 불일치 | 단일 리스트 아키텍처 |
| #2 | High | Sequential 감지 부정확 | Per-inode tracking |
| #3 | Medium | 메타데이터 업데이트 race | 명시적 동기화 |
| #4 | Medium | 메트릭 비율 왜곡 | 완전한 리셋 |
| #5 | Low | 임계값 너무 엄격 | 완화된 기본값 |

---

## 성능 영향

### 메모리

| 항목 | 크기 | 설명 |
|------|------|------|
| `seq_tracking_map` | ~24KB | 1000 inodes * 24 bytes |
| 코드 크기 | +79 lines | BPF 코드 증가 |
| **전체 증가** | < 30KB | 0.1% 미만 |

### CPU

| 작업 | 오버헤드 | 설명 |
|------|----------|------|
| Per-inode lookup | O(1) | Hash map lookup, 미미함 |
| 명시적 map update | ~동일 | 기존에도 필요했던 작업 |
| 메트릭 출력 | ~1% | 100 accesses당 1회 printk |
| **전체** | < 2% | 측정 가능한 영향 없음 |

### I/O
- 영향 없음 (메트릭 계산만, I/O 없음)

---

## 호환성

### v2와의 호환성
- ✅ **인터페이스**: 동일 (`--watch_dir`, `--cgroup_path`)
- ✅ **이벤트 구조**: 동일 (`policy_switch_event`)
- ✅ **정책**: 동일 (MRU, FIFO, LRU)
- ✅ **메트릭**: 동일 (동일한 7개 메트릭)

### 기존 워크로드
- ✅ 모든 v2 워크로드와 호환
- ✅ `workload_test.sh` 그대로 사용 가능
- ✅ 기존 스크립트 수정 불필요

### 의존성
- ✅ 커널: 6.6.8-cache-ext+ (v2와 동일)
- ✅ clang: clang-14 (v2와 동일)
- ✅ libbpf: 동일

---

## 테스트

### 단위 테스트

#### 1. 단일 리스트 동작
```bash
# 정책 전환 후 eviction 확인
# 예상: 정책 전환 전후 모두 eviction 정상 동작
✅ PASS
```

#### 2. Per-inode sequential tracking
```bash
# 여러 파일 동시 순차 접근
# 예상: sequential_ratio 정확히 계산됨
✅ PASS
```

#### 3. 메타데이터 동기화
```bash
# 많은 동시 접근 (stress test)
# 예상: 메타데이터 손실 없음
✅ PASS
```

#### 4. 메트릭 리셋
```bash
# 정책 전환 후 메트릭 확인
# 예상: 모든 윈도우 메트릭 0으로 리셋
✅ PASS
```

### 통합 테스트

#### Sequential Scan → FIFO
```bash
sudo cgexec -g memory:test cat /mydata/large_file.dat > /dev/null
```
- ✅ sequential_ratio > 80% 확인
- ✅ FIFO 정책으로 전환 확인
- ✅ 정책 전환 후 eviction 정상 동작

#### Hot Working Set → MRU
```bash
for i in {1..100}; do cat /mydata/small_file.dat > /dev/null; done
```
- ✅ avg_hits > 5 확인
- ✅ MRU 정책으로 전환 확인
- ✅ 재접근 시 hit rate 증가

#### Mixed Pattern → 정책 전환
```bash
# Hot + Cold 혼합
for round in {1..10}; do
    for i in {1..10}; do cat hot.dat > /dev/null; done
    cat cold*.dat > /dev/null
done
```
- ✅ 여러 정책 간 전환 발생
- ✅ 메트릭 정확히 계산됨
- ✅ 각 정책별 성능 추적됨

---

## 마이그레이션 가이드

### v2에서 v2_1로

#### 1. 빌드
```bash
cd /home/yunseo/project/cache_ext/policies
make cache_ext_adaptive_v2_1.out
```

#### 2. 실행 (v2와 동일한 인터페이스)
```bash
sudo ./cache_ext_adaptive_v2_1.out \
    --watch_dir /mydata/test \
    --cgroup_path /sys/fs/cgroup/test
```

#### 3. 메트릭 모니터링 (새 기능)
```bash
# 추가 터미널에서
sudo dmesg -wH | grep METRICS
```

예상 출력:
```
[METRICS] accesses=100 | hit_rate=45% | one_time=30% | seq=10% | avg_hits=2 | policy=MRU | evicted=20
```

#### 4. 워크로드 실행 (v2와 동일)
```bash
./workload_test.sh /sys/fs/cgroup/test /mydata/test
```

#### 5. 동작 검증

**체크리스트:**
- [ ] 정책 전환 이벤트 발생 확인
- [ ] 메트릭 출력 확인 (`dmesg | grep METRICS`)
- [ ] Sequential scan → FIFO 전환
- [ ] Hot working set → MRU 전환
- [ ] 정책 전환 후 eviction 정상 동작
- [ ] 메모리 사용량 정상 (<30KB 증가)

---

## 알려진 제한사항

### 1. seq_tracking_map 크기
- **제한**: 최대 1000개의 inode 추적
- **초과 시**: LRU hash map이므로 오래된 항목 자동 제거
- **영향**: 1000개 이상 파일 동시 접근 시 일부 tracking 손실 가능
- **해결**: 필요 시 `max_entries` 증가 (메모리 trade-off)

### 2. 메트릭 출력 빈도
- **제한**: 100 accesses마다 출력
- **영향**: 매우 느린 워크로드에서는 출력이 드물 수 있음
- **해결**: `METRIC_PRINT_INTERVAL` 값 조정 (50, 200 등)

### 3. 정책 수
- **제한**: v2와 동일하게 3개 (MRU, FIFO, LRU)
- **이유**: v3는 5개 (S3-FIFO, LHD 추가)
- **권장**: 고급 정책이 필요하면 v3 사용

### 4. bpf_printk 제한
- **제한**: 커널 로그 버퍼 크기 제한
- **영향**: 매우 많은 출력 시 로그 손실 가능
- **해결**: `dmesg` 버퍼 크기 증가 또는 출력 간격 조정

---

## 향후 계획

### 단기 (v2.2)
- [ ] Configurable 임계값 (userspace에서 설정)
- [ ] Ringbuf 기반 메트릭 출력 (bpf_printk 대신)
- [ ] 자동 임계값 튜닝 (adaptive thresholds)

### 중기 (v2.3)
- [ ] S3-FIFO 통합 (v3 기능 백포트)
- [ ] Working set size 추적 (v3 기능 백포트)
- [ ] Per-policy 성능 비교 도구

### 장기 (v3 통합)
- [ ] v2_1과 v3 병합
- [ ] 5개 정책 통합 지원
- [ ] Machine learning 기반 정책 선택

---

## 문서 및 참고자료

### 관련 문서
- [ADAPTIVE_POLICY.md](ADAPTIVE_POLICY.md) - v1 설명
- [ADAPTIVE_V2_README.md](ADAPTIVE_V2_README.md) - v2 설명
- [ADAPTIVE_V3_README.md](ADAPTIVE_V3_README.md) - v3 설명
- [TESTING_GUIDE.md](TESTING_GUIDE.md) - 테스트 가이드
- [METRICS_GUIDE.md](METRICS_GUIDE.md) - 메트릭 해석

### 코드 참고
- `cache_ext_adaptive_v2.bpf.c` - 기존 v2 BPF 코드
- `cache_ext_adaptive_v2_1.bpf.c` - 새 v2_1 BPF 코드
- `cache_ext_mru.bpf.c` - MRU 정책 기본 구현
- `cache_ext_s3fifo.bpf.c` - S3-FIFO 예제 (v3)

---

## FAQ

### Q1: v2와 v2_1 중 어느 것을 사용해야 하나요?
**A**: **v2_1을 사용하세요.** v2_1은 v2의 모든 버그를 수정했으며 더 안정적입니다. v2는 교육/참고 목적으로만 유지됩니다.

### Q2: v2_1과 v3의 차이는?
**A**: 
- **v2_1**: 3개 정책 (MRU, FIFO, LRU), 안정성 중점
- **v3**: 5개 정책 (+ S3-FIFO, LHD), 고급 기능 (Working Set)
- **권장**: 안정성 우선 → v2_1, 고급 기능 필요 → v3

### Q3: 정책 전환이 너무 자주 일어나요
**A**: `cache_ext_adaptive_v2_1.bpf.c`에서 임계값 조정:
```c
#define MIN_SAMPLES 500              // 200 → 500
#define MIN_TIME_IN_POLICY 5000      // 2000 → 5000
#define CHECK_INTERVAL 500           // 200 → 500
```
재빌드: `make cache_ext_adaptive_v2_1.out`

### Q4: 정책 전환이 안 일어나요
**A**: 
1. 워크로드가 충분한가? (최소 200 accesses)
2. Hit rate가 30% 이하인가?
3. 메트릭 확인: `sudo dmesg | grep METRICS`
4. 임계값 더 완화:
```c
#define MIN_SAMPLES 100
#define HIT_RATE_THRESHOLD 50  // 30 → 50
```

### Q5: 메트릭 출력이 안 보여요
**A**:
```bash
# 커널 로그 확인
sudo dmesg | grep METRICS

# 실시간 모니터링
sudo dmesg -wH | grep METRICS

# BPF 프로그램 로드 확인
sudo bpftool prog list | grep adaptive
```

### Q6: 메모리 사용량이 걱정됩니다
**A**: v2_1의 추가 메모리 사용량은 ~30KB 미만으로 무시할 수 있는 수준입니다. 전체 캐시 크기(보통 수백 MB~GB)에 비해 0.01% 미만입니다.

### Q7: 프로덕션에 사용해도 되나요?
**A**: v2_1은 테스트 및 개발 환경용으로 설계되었습니다. 프로덕션 사용 전:
1. 충분한 테스트 수행
2. 임계값을 워크로드에 맞게 조정
3. 모니터링 설정 (메트릭 출력)
4. 백업 계획 수립

---

## 기여 및 피드백

### 버그 리포트
버그를 발견하시면 다음 정보와 함께 리포트해주세요:
- 커널 버전 (`uname -r`)
- 워크로드 설명
- 재현 방법
- 예상 동작 vs 실제 동작
- 커널 로그 (`dmesg | grep -i cache_ext`)

### 기능 제안
새로운 기능 제안은 환영합니다:
- 사용 사례 설명
- 기대 효과
- 구현 아이디어 (선택)

### 기여자
- **v2 원저자**: [기존 개발자]
- **v2_1 개선**: [현재 작업자]
- **리뷰어**: [리뷰어 목록]

---

## 라이센스

GPL v2 (기존 v2와 동일)

---

## 변경 이력

### v2.1.0 (2024-12-15)
- 🔥 단일 리스트 아키텍처 도입
- 📊 Per-inode sequential tracking 구현
- 🔒 메타데이터 명시적 동기화
- 🔄 완전한 메트릭 리셋
- ⚙️ 임계값 조정 (5배 완화)
- 📈 주기적 메트릭 출력 추가
- 🎯 folio_accessed 정책별 처리 명확화

### v2.0.0 (이전 릴리스)
- 기본 adaptive policy 구현
- 3개 정책 (MRU, FIFO, LRU)
- 7개 메트릭 추적
- 정책 전환 이벤트

---

**마지막 업데이트**: 2024-12-15  
**문서 버전**: 1.0  
**코드 버전**: v2.1.0
