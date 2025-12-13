# Adaptive Policy Testing Guide

이 가이드는 adaptive policy v1, v2, v3를 테스트하는 방법을 설명합니다.

## 사전 준비

### 1. 커널 확인

cache_ext 커널로 부팅되어 있는지 확인:

```bash
uname -r
# 출력: 6.6.8-cache-ext+ (또는 유사)
```

### 2. 테스트 데이터 생성

```bash
cd /home/yunseo/project/cache_ext

# 옵션 A: 작은 테스트 데이터셋 생성 (~500MB, 5분)
./create_test_data.sh

# 옵션 B: 실제 LevelDB 다운로드 (수십 GB, 오래 걸림)
./download_leveldb_only.sh
```

**옵션 A 추천** - 빠르게 테스트하기 좋음

생성되는 데이터 구조:
```
/mydata/adaptive_test_data/
├── hot/                    # 20개 작은 파일 (2MB each) - MRU 테스트용
├── cold/                   # 10개 큰 파일 (20MB each) - FIFO 테스트용
├── mixed/                  # 15개 중간 파일 (10MB each) - S3-FIFO/LRU 테스트용
└── large_sequential.dat    # 100MB - Sequential scan 테스트용
```

### 3. Adaptive 정책 빌드

```bash
cd /home/yunseo/project/cache_ext/policies

# v1 빌드 (3개 정책: MRU, FIFO, LRU)
make cache_ext_adaptive.out

# v2 빌드 (3개 정책 + 7개 메트릭)
make cache_ext_adaptive_v2.out

# v3 빌드 (5개 정책 + 9개 메트릭 + Working Set)
make cache_ext_adaptive_v3.out
```

## Adaptive v3 테스트 (추천)

v3는 가장 많은 기능을 포함합니다:
- **5개 정책**: MRU, FIFO, LRU, S3-FIFO, LHD-Simple
- **9개 메트릭**: Hit rate, Working set size, One-time ratio, Sequential ratio, 등
- **지능형 전환**: 워크로드 특성 기반 자동 정책 선택

### 터미널 1: v3 실행

```bash
cd /home/yunseo/project/cache_ext/policies

# v3 실행
sudo ./cache_ext_adaptive_v3.out \
    --watch_dir /mydata/adaptive_test_data \
    --cgroup_path /sys/fs/cgroup/adaptive_v3_test
```

**예상 출력:**
```
========================================
Enhanced Adaptive Policy v3 Started
========================================
  Watch directory: /mydata/adaptive_test_data
  Cgroup:          /sys/fs/cgroup/adaptive_v3_test
  Initial policy:  MRU

Available Policies:
  • MRU         - Most Recently Used
  • FIFO        - First In First Out
  • LRU         - Least Recently Used
  • S3-FIFO     - Small/Main queue FIFO
  • LHD-Simple  - Hit age tracking

Tracking Metrics:
  ✓ Hit rate
  ✓ Working set size
  ✓ One-time access ratio
  ✓ Sequential access ratio
  ✓ Average hits per page
  ✓ Average reuse distance
  ✓ Dirty page ratio
  ✓ Per-policy performance

Monitoring for intelligent policy switches...
Press Ctrl-C to exit.
```

이 터미널은 계속 실행 상태로 두세요. 정책 전환 이벤트가 여기에 출력됩니다.

### 터미널 2: 워크로드 실행

새 터미널을 열고:

```bash
cd /home/yunseo/project/cache_ext/policies

# 자동 워크로드 테스트 실행
./workload_test_v3.sh

# 또는 경로를 직접 지정
./workload_test_v3.sh /sys/fs/cgroup/adaptive_v3_test /mydata/adaptive_test_data
```

**워크로드 시나리오 (자동 실행됨):**

1. **Sequential Scan** → FIFO 유도
   - 모든 파일을 한 번씩 순차 읽기
   - 예상: Sequential ratio ↑, One-time ratio ↑ → FIFO

2. **Hot Working Set** → MRU 유도
   - 5개 파일을 20번 반복 접근
   - 예상: Avg hits ↑, WS << cache → MRU

3. **Large Working Set** → FIFO 유도
   - 많은 파일 접근
   - 예상: WS >> cache → FIFO

4. **Mixed Hot/Cold** → S3-FIFO 유도
   - Hot 파일 (반복) + Cold 파일 (한번) 혼합
   - 예상: One-time ratio 중간 → S3-FIFO

5. **Random Access** → LRU 유도
   - 랜덤 파일 접근
   - 예상: 균형잡힌 패턴 → LRU

### 정책 전환 이벤트 예시 (터미널 1)

```
========================================
POLICY SWITCH DETECTED!
========================================
  Time:                5234
  Old Policy:          MRU
  New Policy:          FIFO

Performance Metrics:
  Hit Rate:            15%
  Old Policy Hit Rate: 20%
  Total Accesses:      2500

Workload Characteristics:
  One-time Ratio:      85%    ← 대부분 한 번만 접근
  Sequential Ratio:    90%    ← 순차 접근 패턴
  Avg Hits/Page:       1.1    ← 페이지당 평균 1.1회 접근
  Avg Reuse Distance:  180000
  Dirty Page Ratio:    5%

Working Set Analysis:
  Working Set Size:    45 pages
  WS/Cache Ratio:      450%   ← 워킹셋이 캐시보다 훨씬 큼!

========================================

Switch Reason:
  → High sequential access detected

========================================
```

## 수동 워크로드 테스트

자동 스크립트 대신 직접 명령어로 테스트하고 싶다면:

### Sequential Scan (FIFO 유도)

```bash
sudo cgexec -g memory:adaptive_v3_test \
    cat /mydata/adaptive_test_data/large_sequential.dat > /dev/null
```

### Hot Working Set (MRU 유도)

```bash
sudo cgexec -g memory:adaptive_v3_test bash -c '
    for i in {1..50}; do
        cat /mydata/adaptive_test_data/hot/*.dat > /dev/null
    done
'
```

### Cold Scan (FIFO 유도)

```bash
sudo cgexec -g memory:adaptive_v3_test \
    cat /mydata/adaptive_test_data/cold/*.dat > /dev/null
```

### Mixed Pattern (S3-FIFO 유도)

```bash
sudo cgexec -g memory:adaptive_v3_test bash -c '
    for round in {1..10}; do
        # Hot access
        for i in {1..10}; do
            cat /mydata/adaptive_test_data/hot/hot_1.dat > /dev/null
        done

        # Cold access
        cat /mydata/adaptive_test_data/cold/*.dat > /dev/null
    done
'
```

### Random Access (LRU 유도)

```bash
sudo cgexec -g memory:adaptive_v3_test bash -c '
    for i in {1..100}; do
        RANDOM_FILE=$(find /mydata/adaptive_test_data -type f | shuf | head -1)
        cat "$RANDOM_FILE" > /dev/null
    done
'
```

## Adaptive v2 테스트

v2는 3개 정책 (MRU, FIFO, LRU) + 7개 메트릭을 제공합니다.

### 실행

```bash
# 터미널 1
sudo ./cache_ext_adaptive_v2.out \
    --watch_dir /mydata/adaptive_test_data \
    --cgroup_path /sys/fs/cgroup/adaptive_v2_test

# 터미널 2
./workload_test.sh  # v1/v2용 스크립트
```

## Adaptive v1 테스트

v1은 단순 hit rate 기반 round-robin 전환입니다.

### 실행

```bash
# 터미널 1
./test_adaptive.sh

# 터미널 2
./workload_test.sh
```

## 디버깅

### 정책 전환이 안 보이는 경우

1. **메트릭 임계값 확인**:
   ```bash
   # cache_ext_adaptive_v3.bpf.c에서 확인
   grep -n "THRESHOLD\|MIN_SAMPLES" cache_ext_adaptive_v3.bpf.c
   ```

2. **커널 로그 확인**:
   ```bash
   sudo dmesg -wH | grep -i "cache_ext\|policy\|adaptive"
   ```

3. **BPF 프로그램 로드 확인**:
   ```bash
   sudo bpftool prog list | grep adaptive
   ```

4. **Cgroup 확인**:
   ```bash
   sudo bpftool cgroup tree /sys/fs/cgroup/adaptive_v3_test
   ```

5. **BPF 맵 확인**:
   ```bash
   sudo bpftool map list
   sudo bpftool map dump name folio_metadata_map | head -20
   ```

### 전환이 너무 자주 일어나는 경우

`cache_ext_adaptive_v3.bpf.c` 수정:

```c
// 더 오래 기다리기
#define MIN_TIME_IN_POLICY 50000  // 기본 10000

// 더 많은 샘플 필요
#define MIN_SAMPLES 2000  // 기본 1000

// 전환 체크 빈도 낮추기
#define CHECK_INTERVAL 2000  // 기본 1000
```

재빌드:
```bash
make cache_ext_adaptive_v3.out
```

### 전환이 안 일어나는 경우

임계값 낮추기:

```c
// cache_ext_adaptive_v3.bpf.c
#define HIT_RATE_THRESHOLD 20  // 기본 30
#define MIN_SAMPLES 500        // 기본 1000
```

## 버전별 비교

| 기능 | v1 | v2 | v3 |
|------|----|----|-----|
| 정책 수 | 3 | 3 | 5 |
| 메트릭 수 | 1 (hit rate) | 7 | 9 |
| Working Set 추적 | ✗ | ✗ | ✓ |
| S3-FIFO | ✗ | ✗ | ✓ |
| LHD | ✗ | ✗ | ✓ |
| 전환 로직 | Round-robin | 휴리스틱 | 고급 휴리스틱 + WS |
| 추천 사용 | 학습용 | 테스트 | 실전 |

## 실전 활용 예시

### 데이터베이스 워크로드

```bash
# v3 실행
sudo ./cache_ext_adaptive_v3.out \
    --watch_dir /var/lib/mysql \
    --cgroup_path /sys/fs/cgroup/database

# 데이터베이스를 cgroup에서 실행
sudo cgexec -g memory:database mysqld ...
```

예상 동작:
- Index/metadata 접근: MRU (hot pages 보호)
- Full table scan: FIFO (sequential)
- Mixed queries: S3-FIFO

### 웹 서버 정적 파일

```bash
sudo ./cache_ext_adaptive_v3.out \
    --watch_dir /var/www/html \
    --cgroup_path /sys/fs/cgroup/webserver

sudo cgexec -g memory:webserver nginx ...
```

예상 동작:
- 인기 파일: MRU
- 드문 파일: FIFO로 빠르게 evict

### 로그 처리

```bash
sudo ./cache_ext_adaptive_v3.out \
    --watch_dir /var/log \
    --cgroup_path /sys/fs/cgroup/logprocessor

sudo cgexec -g memory:logprocessor grep -r "ERROR" /var/log
```

예상 동작:
- Sequential scan 감지 → FIFO

## 문제 해결

### "Failed to attach cache_ext_ops to cgroup"

cgroup v2 확인:
```bash
mount | grep cgroup
# cgroup2가 /sys/fs/cgroup에 마운트되어야 함

# cgroup v2로 전환 (필요시)
sudo mkdir -p /sys/fs/cgroup/unified
sudo mount -t cgroup2 none /sys/fs/cgroup/unified
```

### "Directory does not exist"

watch_dir 생성:
```bash
sudo mkdir -p /mydata/adaptive_test_data
./create_test_data.sh
```

### Permission denied

sudo 권한 확인:
```bash
sudo -v
```

## 추가 리소스

- [ADAPTIVE_V3_README.md](ADAPTIVE_V3_README.md) - v3 상세 설명
- [METRICS_GUIDE.md](METRICS_GUIDE.md) - 메트릭 해석 가이드
- [cache_ext 논문](../cache_ext_paper.pdf) - 원본 연구

## 요약

### 빠른 시작 (v3)

```bash
# 1. 테스트 데이터 생성
cd /home/yunseo/project/cache_ext
./create_test_data.sh

# 2. 빌드
cd policies
make cache_ext_adaptive_v3.out

# 3. 터미널 1: 실행
sudo ./cache_ext_adaptive_v3.out \
    --watch_dir /mydata/adaptive_test_data \
    --cgroup_path /sys/fs/cgroup/adaptive_v3_test

# 4. 터미널 2: 워크로드
./workload_test_v3.sh
```

이제 터미널 1에서 실시간 정책 전환을 관찰할 수 있습니다! 🚀
