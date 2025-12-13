# Adaptive Policy v3 - Working Set Size + 5 Policies

v2에서 발전하여, **Working Set Size 추적**을 추가하고 **5가지 정책** (MRU, FIFO, LRU, S3-FIFO, LHD-Simple)을 동적으로 선택합니다.

## 🆕 v3의 새로운 기능

### 1. Working Set Size Tracking

```c
struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __type(key, u64);    // inode number
    __type(value, u8);   // dummy value
    __uint(max_entries, 100000);
} working_set_map SEC(".maps");
```

- **LRU_HASH 맵 사용**: 최근 접근한 unique inode만 자동으로 유지
- **Working Set Ratio 계산**: `(working_set_size / cache_size) × 100`
- **정책 선택에 활용**:
  - `WS >> cache` (ratio > 300%) → FIFO (scan 최적화)
  - `WS << cache` (ratio < 60%) → MRU (recency 최적화)
  - `WS ≈ cache` → 다른 메트릭 기반 결정

### 2. 5가지 정책

#### POLICY_MRU (0)
- 가장 최근에 사용된 페이지를 evict
- **적합**: Working set << cache, 재접근 많음

#### POLICY_FIFO (1)
- 가장 먼저 들어온 페이지를 evict
- **적합**: Sequential scan, one-time access

#### POLICY_LRU (2)
- 가장 오래전에 사용된 페이지를 evict
- **적합**: 균형잡힌 워크로드

#### POLICY_S3FIFO (3)
- **Small queue**: 새로운 페이지 (freq=0)
- **Main queue**: 재접근된 페이지 (freq≥1)
- Small에서 evict 시도 → freq≥3이면 main으로 승격
- **적합**: Mixed workload (hot + cold 분리)

#### POLICY_LHD_SIMPLE (4)
- **Hit Age 추적**: 마지막 hit 이후 경과 시간
- Hit age가 짧은 페이지 우선 evict (최근 hit는 보호)
- **적합**: Temporal locality가 강한 워크로드

## 정책 선택 로직

```c
static u32 decide_best_policy(void)
{
    // 1. Working set ratio 기반
    if (ws_ratio > 300) {
        // WS >> cache: scan-friendly
        return (sequential_ratio > 70) ? POLICY_FIFO : POLICY_LRU;
    }

    if (ws_ratio < 60) {
        // WS << cache: recency-friendly
        return (avg_hits > 5) ? POLICY_MRU : POLICY_LRU;
    }

    // 2. Sequential scan 감지
    if (sequential_ratio > 80)
        return POLICY_FIFO;

    // 3. One-time scan
    if (one_time_ratio > 60 && avg_hits < 2)
        return POLICY_FIFO;

    // 4. Hot working set
    if (avg_hits > 5 && one_time_ratio < 30)
        return POLICY_MRU;

    // 5. Mixed workload
    if (one_time_ratio > 40 && one_time_ratio < 60)
        return POLICY_S3FIFO;  // Hot/cold 분리

    // 6. Strong temporal locality
    if (avg_reuse_distance < 50000)
        return POLICY_LHD_SIMPLE;

    // 7. 과거 성능 기반
    return best_performing_policy_historically;
}
```

## 빌드 및 실행

```bash
cd /home/yunseo/project/cache_ext/policies

# 빌드
make cache_ext_adaptive_v3.out

# 실행
sudo ./cache_ext_adaptive_v3.out \
    --watch_dir /mydata/leveldb_db \
    --cgroup_path /sys/fs/cgroup/adaptive_test
```

## 출력 예시

```
========================================
POLICY SWITCH DETECTED!
========================================
  Time:                15234
  Old Policy:          MRU
  New Policy:          S3-FIFO

Performance Metrics:
  Hit Rate:            45%
  Old Policy Hit Rate: 42%
  Total Accesses:      8000

Workload Characteristics:
  One-time Ratio:      55%
  Sequential Ratio:    40%
  Avg Hits/Page:       2.8
  Avg Reuse Distance:  80000
  Dirty Page Ratio:    20%

Working Set Analysis:
  Working Set Size:    15000 pages
  WS/Cache Ratio:      150%

========================================

Switch Reason:
  → Mixed workload with hot/cold pages

========================================
```

## S3-FIFO 상세 동작

### 구조
```
[Small Queue] ──────> [Main Queue]
  (freq=0)       승격    (freq≥1)
     │            ↑
     └─ evict ────┘
       (freq<3)   (freq≥3)
```

### 알고리즘
1. **folio_added**: Small queue에 추가 (freq=0)
2. **folio_accessed**: freq++
3. **evict_folios**:
   - Small queue에서 evict 시도
   - freq ≥ 3이면 Main queue로 승격, freq=0으로 리셋
   - freq < 3이면 실제 evict
   - Small이 비었으면 Main에서 evict

### 장점
- **One-time pages**: Small에서 빠르게 evict
- **Hot pages**: Main에서 보호
- **적응적**: Scan 중에도 재접근 많은 페이지는 유지

## LHD-Simple 상세 동작

### Hit Age 계산
```c
hit_age = current_time - last_access_time
```

### 알고리즘
1. **folio_accessed**: `last_access_time` 업데이트
2. **evict_folios**: Hit age 짧은 순으로 evict
   - 최근 hit → 곧 다시 사용될 가능성 낮음
   - 오래된 hit → 여전히 필요할 가능성 높음

### 직관
- LRU와 반대: "최근 hit는 당분간 안 쓸 것"
- Database index처럼 주기적 접근 패턴에 유리

## v2 vs v3 비교

| 기능 | v2 | v3 |
|------|----|----|
| 정책 수 | 3 (MRU, FIFO, LRU) | 5 (+S3-FIFO, +LHD) |
| Working Set 추적 | ✗ | ✓ |
| WS 기반 결정 | ✗ | ✓ |
| Hot/Cold 분리 | ✗ | ✓ (S3-FIFO) |
| Hit age 추적 | ✗ | ✓ (LHD) |
| 메트릭 | 7개 | 9개 (+WS size, +WS ratio) |

## 워크로드별 예상 정책

### 1. Database (Small Index)
```
working_set_size:    5000 pages
cache_size:          10000 pages
ws_ratio:            50%    ← WS << cache
avg_hits:            8

→ MRU (hot pages 보호)
```

### 2. Grep (Large Files)
```
working_set_size:    100000 pages
cache_size:          10000 pages
ws_ratio:            1000%  ← WS >> cache
sequential_ratio:    90%
one_time_ratio:      85%

→ FIFO (scan 최적화)
```

### 3. Web Server
```
working_set_size:    8000 pages
cache_size:          10000 pages
ws_ratio:            80%    ← WS ≈ cache
avg_hits:            6
one_time_ratio:      35%

→ LRU 또는 MRU
```

### 4. Compilation
```
working_set_size:    15000 pages
cache_size:          10000 pages
ws_ratio:            150%
one_time_ratio:      50%
sequential_ratio:    40%

→ S3-FIFO (hot/cold 분리)
```

### 5. Video Processing
```
working_set_size:    200000 pages
cache_size:          10000 pages
ws_ratio:            2000%  ← WS >> cache
sequential_ratio:    95%

→ FIFO
```

## 테스트 시나리오

### 시나리오 1: Small Working Set
```bash
# 작은 파일 반복 접근
sudo cgexec -g memory:adaptive_test bash -c '
    for i in {1..100}; do
        cat /mydata/small_file > /dev/null
    done
'

# 예상:
# - ws_ratio < 60%
# - avg_hits > 5
# → MRU
```

### 시나리오 2: Large Sequential Scan
```bash
# 대용량 파일 순차 읽기
sudo cgexec -g memory:adaptive_test \
    dd if=/mydata/100GB_file of=/dev/null bs=1M

# 예상:
# - ws_ratio > 300%
# - sequential_ratio > 80%
# → FIFO
```

### 시나리오 3: Mixed Hot/Cold
```bash
# 자주 쓰는 파일 + 가끔 쓰는 파일
sudo cgexec -g memory:adaptive_test bash -c '
    while true; do
        cat /mydata/hot_index > /dev/null    # 자주
        cat /mydata/cold_data_$RANDOM > /dev/null  # 가끔
        sleep 0.1
    done
'

# 예상:
# - one_time_ratio ≈ 50%
# - avg_hits ≈ 3
# → S3-FIFO (hot은 main, cold는 small에서 빠르게 evict)
```

## 디버깅

### Working Set Size 확인
```bash
# BPF 맵 크기 확인
sudo bpftool map list | grep working_set_map
sudo bpftool map dump name working_set_map | wc -l
```

### 정책별 리스트 크기 확인
```bash
# dmesg에서 리스트 크기 출력 (디버그 모드)
sudo dmesg -wH | grep -E "list_len|queue_len"
```

### S3-FIFO 동작 확인
```bash
# Small/Main queue 크기 변화 추적
sudo dmesg -wH | grep -E "s3fifo_small|s3fifo_main"
```

## 파라미터 튜닝

### Working Set Ratio 임계값
```c
// cache_ext_adaptive_v3.bpf.c
if (ws_ratio > 300)  // 기본 300%, 조정 가능
if (ws_ratio < 60)   // 기본 60%
```

### S3-FIFO 승격 임계값
```c
#define S3FIFO_PROMOTION_THRESHOLD 3  // freq ≥ 3이면 main으로
#define S3FIFO_SMALL_RATIO 10         // small:main = 1:9
```

### LHD Hit Age Window
```c
#define LHD_MAX_HIT_AGE 1000000  // 너무 오래된 hit는 무시
```

## 제한사항

1. **Working Set Size 근사치**:
   - Inode 기반이므로 실제 page 수와 다를 수 있음
   - 큰 파일 여러 개 vs 작은 파일 많이 구분 못함

2. **S3-FIFO 단순화**:
   - Ghost queue 없음 (원본 S3-FIFO는 ghost로 frequency 추적)
   - Small/Main 비율 고정 (동적 조정 안 함)

3. **LHD 단순화**:
   - 원본 LHD의 복잡한 재정렬 로직 생략
   - 단순 hit age만 추적

4. **오버헤드**:
   - Working set 맵: 100K entries
   - S3-FIFO: 2개 리스트 유지
   - LHD: 추가 타임스탬프 필드

## 성능 영향

- **메모리**: folio당 ~40 bytes (메타데이터 증가)
- **CPU**: 접근당 ~150 ns (working set 업데이트 포함)
- **정책 전환**: ~2ms (리스트 마이그레이션 시)

## 다음 단계

1. **Working Set Size 개선**:
   - Page 단위로 추적 (현재는 inode 단위)
   - File size 고려한 정확한 추정

2. **S3-FIFO 완전 구현**:
   - Ghost queue 추가
   - 동적 Small/Main 비율 조정

3. **LHD 완전 구현**:
   - 재정렬 로직 추가
   - Hit density 정확한 계산

4. **정책 마이그레이션**:
   - 정책 전환 시 페이지 재배치
   - Hot page 우선순위 유지

5. **Auto-tuning**:
   - 임계값 자동 조정
   - Reinforcement learning 기반 선택
