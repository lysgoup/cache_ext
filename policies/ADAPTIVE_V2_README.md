# Adaptive Policy v2 - Enhanced with Multiple Metrics

v1에서 단순 히트율만 보던 것에서 발전하여, **7가지 메트릭**을 추적하고 **워크로드 특성에 맞는 정책**을 자동으로 선택합니다.

## 🆕 추가된 메트릭

### 1. One-time Access Ratio (%)
```
one_time_ratio = (한 번만 접근된 페이지 / 전체 evict된 페이지) × 100
```
- **높으면**: Sequential scan 또는 streaming 워크로드
- **낮으면**: 재접근이 많은 워크로드
- **임계값**: 60% 이상이면 FIFO 선호

### 2. Sequential Access Ratio (%)
```
sequential_ratio = (순차 접근 / 전체 접근) × 100
```
- 연속된 inode와 offset 패턴 감지
- **높으면**: 파일을 순차적으로 읽는 중
- **임계값**: 80% 이상이면 FIFO 강제

### 3. Average Hits Per Page
```
avg_hits = 전체 히트 합계 / evict된 페이지 수
```
- 페이지가 evict되기 전 평균 몇 번 접근되는가
- **높으면**: Hot working set (자주 쓰는 페이지들)
- **임계값**: 5 이상이면 MRU 선호

### 4. Average Reuse Distance
```
avg_reuse_distance = Σ(현재 접근 시각 - 이전 접근 시각) / 재접근 횟수
```
- 재접근 간격의 평균
- **짧으면**: Strong temporal locality
- **김**: Weak temporal locality

### 5. Dirty Page Ratio (%)
```
dirty_ratio = (dirty 페이지 evictions / 전체 evictions) × 100
```
- Write-heavy 워크로드 감지
- 향후 writeback 고려 정책에 활용 가능

### 6. Per-Policy Hit Rate
각 정책별로 개별 성능 추적:
```c
struct policy_stats {
    hits, misses, evictions;  // 각 정책이 활성일 때의 통계
    time_active;               // 활성화된 시간
};
```
- 과거 성능 기반으로 다음 정책 선택

### 7. Page Lifetime & Idle Time
```
lifetime = eviction 시각 - added 시각
idle_time = eviction 시각 - last_access 시각
```
- 페이지가 캐시에 머문 시간
- 마지막 접근 후 얼마나 놀았는지

## 정책 선택 로직

```c
static u32 decide_best_policy(void)
{
    // 우선순위 순서로 판단

    // 1. Sequential scan (가장 명확)
    if (sequential_ratio > 80%)
        return FIFO;

    // 2. One-time scan
    if (one_time_ratio > 60% && avg_hits < 2)
        return FIFO;

    // 3. Hot working set
    if (avg_hits > 5 && one_time_ratio < 30%)
        return MRU;

    // 4. Temporal locality
    if (avg_reuse_distance < 50000)
        return LRU;

    // 5. 과거 성능 기반
    return best_performing_policy_historically;
}
```

## 빌드 및 실행

```bash
cd /home/yunseo/project/cache_ext/policies

# 빌드
make cache_ext_adaptive_v2.out

# 실행
sudo ./cache_ext_adaptive_v2.out \
    --watch_dir /mydata/leveldb_db \
    --cgroup_path /sys/fs/cgroup/adaptive_test
```

## 출력 예시

정책 전환 시:

```
========================================
POLICY SWITCH DETECTED!
========================================
  Time:                15234
  Old Policy:          MRU
  New Policy:          FIFO

Performance Metrics:
  Hit Rate:            25%
  Old Policy Hit Rate: 28%
  Total Accesses:      5000

Workload Characteristics:
  One-time Ratio:      75%    ← 많은 페이지가 한 번만 접근됨
  Sequential Ratio:    85%    ← 순차 접근 패턴
  Avg Hits/Page:       1.2    ← 페이지당 평균 1.2회만 접근
  Avg Reuse Distance:  150000
  Dirty Page Ratio:    15%

========================================

Switch Reason:
  → High sequential access detected
```

## v1 vs v2 비교

| 기능 | v1 | v2 |
|------|----|----|
| 히트율 추적 | ✓ | ✓ |
| 정책 전환 | Round-robin | 워크로드 기반 |
| One-time 감지 | ✗ | ✓ |
| Sequential 감지 | ✗ | ✓ |
| Reuse distance | ✗ | ✓ |
| Per-policy 성능 | ✗ | ✓ |
| 의사결정 | 단순 임계값 | 다중 휴리스틱 |

## 테스트 시나리오

### 시나리오 1: Sequential Scan
```bash
# 대용량 파일 순차 읽기
sudo cgexec -g memory:adaptive_test \
    dd if=/mydata/large_file of=/dev/null bs=1M

# 예상 결과:
# - sequential_ratio > 80%
# - one_time_ratio > 70%
# - → FIFO로 전환
```

### 시나리오 2: Database Workload
```bash
# 같은 인덱스 블록 반복 접근
sudo cgexec -g memory:adaptive_test \
    ./ycsb_workload

# 예상 결과:
# - avg_hits_per_page > 5
# - one_time_ratio < 30%
# - → MRU로 전환
```

### 시나리오 3: Mixed Workload
```bash
# grep (sequential) + compilation (random)
sudo cgexec -g memory:adaptive_test bash -c '
    grep -r "pattern" /mydata/code &
    make -j8
'

# 예상 결과:
# - 메트릭이 혼재
# - 과거 성능 기반으로 선택
# - → LRU로 전환 (균형)
```

## 디버깅

### 메트릭 확인 (dmesg)
```bash
sudo dmesg -wH | grep -E "Decision|Policy switch"

# 출력 예시:
# Decision: FIFO (sequential_ratio=85%)
# Policy switch: 0 -> 1 (hit_rate: 25%)
```

### BPF 통계 확인
```bash
# BPF 맵 내용 보기
sudo bpftool map list
sudo bpftool map dump name folio_metadata_map | head -20
```

## 파라미터 튜닝

`cache_ext_adaptive_v2.bpf.c`에서 임계값 조정:

```c
// 정책 전환 로직 (decide_best_policy 함수)
if (sequential_ratio > 80)  // 기본 80%, 조정 가능
if (one_time_ratio > 60)    // 기본 60%
if (avg_hits > 5)            // 기본 5
```

## 다음 단계

v2를 기반으로 더 발전시키려면:

1. **머신러닝 기반 예측**:
   ```c
   // 메트릭 벡터로 다음 히트율 예측
   predict_hit_rate(metrics) → select_best_policy()
   ```

2. **페이지 마이그레이션**:
   - 정책 전환 시 기존 페이지를 새 리스트로 이동
   - "Hot" 페이지는 새 정책에서도 우대

3. **더 많은 정책**:
   - S3-FIFO, LHD 추가
   - ARC (Adaptive Replacement Cache)

4. **동적 임계값**:
   - 워크로드에 따라 임계값 자동 조정
   - EWMA로 히스토리 고려

5. **Cgroup 간 비교**:
   - 다른 cgroup의 메트릭과 비교
   - 시스템 전체 최적화

## 제한사항

1. **정책 전환 오버헤드**: 리스트 마이그레이션 없음
2. **메모리 오버헤드**: 모든 folio에 메타데이터 저장
3. **CPU 오버헤드**: 매 접근마다 통계 업데이트
4. **정확도**: 근사치 (샘플링 기반)

## 성능 영향

- **메모리**: folio당 ~32 bytes (메타데이터)
- **CPU**: 접근당 ~100 ns (통계 업데이트)
- **정책 전환**: ~1ms (리스트 전환)

대부분의 워크로드에서 무시할 수준입니다.
