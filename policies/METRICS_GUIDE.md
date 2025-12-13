# Adaptive Policy 메트릭 가이드

## 수집 가능한 모든 메트릭

### ✅ v2에 구현됨

| 메트릭 | 계산 방법 | 언제 업데이트 | 활용 |
|--------|-----------|--------------|------|
| **Hit Rate** | `hits / (hits + misses) × 100` | 매 접근 | 전체 성능 측정 |
| **One-time Ratio** | `one_time / (one_time + multi) × 100` | folio_evicted | Scan 감지 |
| **Sequential Ratio** | `sequential / (sequential + random) × 100` | folio_added | Streaming 감지 |
| **Avg Hits/Page** | `Σ hits / evicted_pages` | folio_evicted | Hot vs Cold |
| **Avg Reuse Distance** | `Σ (t_now - t_last) / reaccesses` | folio_accessed | Temporal locality |
| **Dirty Ratio** | `dirty_evictions / total_evictions × 100` | folio_evicted | Write pattern |
| **Per-policy Hit Rate** | 각 정책별 개별 추적 | 매 접근 | 정책 성능 비교 |
| **Avg Lifetime** | `Σ (t_evict - t_add) / evicted` | folio_evicted | Churn rate |
| **Avg Idle Time** | `Σ (t_evict - t_last_access) / evicted` | folio_evicted | LRU 효과 |

### 🔄 쉽게 추가 가능

| 메트릭 | 구현 난이도 | 필요한 것 |
|--------|------------|----------|
| **Working Set Size** | 🟡 중간 | LRU_HASH 맵으로 unique inodes 추적 |
| **Read/Write Ratio** | 🟢 쉬움 | folio_test_dirty() 활용 |
| **Cache Fullness** | 🟡 중간 | 리스트 크기 추적 |
| **Eviction Latency** | 🟡 중간 | evict_folios 시작/끝 시각 |
| **Per-inode Stats** | 🟡 중간 | inode별 BPF 맵 |

### ❌ 현재 불가능 (커널 수정 필요)

| 메트릭 | 이유 | 해결 방법 |
|--------|------|----------|
| **System Memory Pressure** | eBPF가 시스템 정보 못 봄 | Userspace에서 cgroup stats 읽기 |
| **IO Latency** | IO 완료 훅 없음 | 커널에 on_io_complete 훅 추가 |
| **Page Fault Count** | Page fault 훅 없음 | 커널에 on_page_fault 훅 추가 |

## 메트릭 해석 가이드

### One-time Ratio

```
0-20%   → 대부분 재접근 (Cache-friendly)
20-40%  → 혼합 워크로드
40-60%  → 상당한 scan 포함
60-80%  → 주로 scan
80-100% → 거의 순수 scan (예: grep, backup)
```

**정책 선택**:
- `< 30%` → MRU/LRU (재접근 활용)
- `> 60%` → FIFO (scan 최적화)

### Sequential Ratio

```
0-20%   → 완전히 random (Database random read)
20-50%  → 약간의 순차성
50-80%  → 상당한 순차성 (Log processing)
80-100% → 거의 순수 순차 (Large file read, backup)
```

**정책 선택**:
- `> 80%` → FIFO 강제 (scan 최적화)
- `< 50%` → LRU/MRU (locality 활용)

### Avg Hits Per Page

```
0-1     → 거의 모든 페이지가 한 번만 접근 (Scan)
1-3     → 약간의 재접근
3-5     → 적당한 재접근 (일반적)
5-10    → 많은 재접근 (Hot pages)
10+     → 매우 hot (Database index, metadata)
```

**정책 선택**:
- `< 2` → FIFO
- `> 5` → MRU (hot pages 보호)

### Avg Reuse Distance

```
0-1000    → 매우 짧음 (Strong locality)
1k-10k    → 짧음 (Good locality)
10k-100k  → 중간
100k+     → 김 (Weak locality)
```

**정책 선택**:
- 짧으면 → LRU/MRU 효과적
- 길면 → FIFO 또는 특수 정책

### Dirty Ratio

```
0-10%   → Read-mostly
10-30%  → 보통
30-50%  → Write-heavy
50%+    → Very write-heavy (Logging, compilation)
```

**활용**:
- Write-heavy → Dirty page 우선 evict 피하기
- 향후 writeback 고려 정책에 사용

## 실전 워크로드 예시

### 1. Database (OLTP)
```
hit_rate:         70-90%
one_time_ratio:   20-30%
sequential_ratio: 10-30%
avg_hits:         5-15
reuse_distance:   중간

→ MRU 또는 LRU
```

### 2. File Search (grep -r)
```
hit_rate:         10-30%
one_time_ratio:   80-95%
sequential_ratio: 70-90%
avg_hits:         1-1.5
reuse_distance:   매우 김

→ FIFO
```

### 3. Compilation (make -j)
```
hit_rate:         40-60%
one_time_ratio:   40-60%
sequential_ratio: 30-50%
avg_hits:         2-4
reuse_distance:   중간

→ LRU (균형)
```

### 4. Video Encoding
```
hit_rate:         30-50%
one_time_ratio:   70-90%
sequential_ratio: 85-95%
avg_hits:         1-2
reuse_distance:   매우 김
dirty_ratio:      40-60%

→ FIFO
```

### 5. Web Server (Static files)
```
hit_rate:         60-80%
one_time_ratio:   30-50%
sequential_ratio: 20-40%
avg_hits:         3-8
reuse_distance:   짧음-중간

→ LRU 또는 MRU
```

## 디버깅 체크리스트

### 정책이 자주 전환되는 경우
```bash
# MIN_TIME_IN_POLICY 증가
#define MIN_TIME_IN_POLICY 50000  // 10000 → 50000
```

### 정책이 전환되지 않는 경우
```bash
# HIT_RATE_THRESHOLD 증가
#define HIT_RATE_THRESHOLD 40  // 30 → 40

# 또는 MIN_SAMPLES 감소
#define MIN_SAMPLES 500  // 1000 → 500
```

### 메트릭이 이상한 경우
```bash
# dmesg로 디버그 출력 확인
sudo dmesg -wH | grep cache_ext

# 메타데이터 맵 확인
sudo bpftool map dump name folio_metadata_map | head
```

## 메트릭 조합 패턴

| 패턴 | Sequential | One-time | Avg Hits | 워크로드 | 정책 |
|------|-----------|----------|----------|---------|------|
| **Scan** | High | High | Low | grep, find | FIFO |
| **Hot Set** | Low | Low | High | DB index | MRU |
| **Mixed** | Mid | Mid | Mid | Web server | LRU |
| **Streaming** | High | High | Low | Video | FIFO |
| **Cyclic** | Low | Mid | Mid | Batch jobs | LRU |

## 성능 최적화 팁

1. **너무 자주 체크하지 않기**:
   ```c
   #define CHECK_INTERVAL 1000  // 1000번마다
   ```

2. **최소 샘플 확보**:
   ```c
   #define MIN_SAMPLES 1000  // 통계적 유의성
   ```

3. **Hysteresis 적용**:
   ```c
   // 현재 정책이 충분히 나쁠 때만 전환
   if (hit_rate < HIT_RATE_THRESHOLD - 5)  // -5% margin
   ```

4. **메트릭 리셋 주기**:
   - 정책 전환 시 리셋
   - 또는 슬라이딩 윈도우 사용
