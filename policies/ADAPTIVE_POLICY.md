# Adaptive Cache Eviction Policy

동적으로 정책을 전환하는 적응형 캐시 eviction 정책입니다.

## 기능

- **자동 정책 전환**: 히트율이 임계값(30%) 아래로 떨어지면 자동으로 다른 정책으로 전환
- **정책 간 전환**: MRU → FIFO → LRU 순환
- **최소 대기 시간**: 정책 전환 후 최소 10,000 timestamp 동안 대기 (너무 자주 전환 방지)
- **실시간 모니터링**: 정책 전환 시 이벤트를 출력

## 빌드

```bash
# cache_ext 커널에서만 빌드 가능
cd /mydata/cache_ext/policies
make cache_ext_adaptive.out
```

## 실행

```bash
# Cgroup 생성
sudo mkdir -p /sys/fs/cgroup/adaptive_test

# 정책 시작
sudo ./cache_ext_adaptive.out \
    --watch_dir /path/to/watch \
    --cgroup_path /sys/fs/cgroup/adaptive_test
```

## 테스트

### 1. 기본 테스트

```bash
# Terminal 1: 정책 실행
sudo ./cache_ext_adaptive.out \
    --watch_dir /mydata/leveldb_db \
    --cgroup_path /sys/fs/cgroup/adaptive_test

# Terminal 2: 워크로드 실행
sudo cgexec -g memory:adaptive_test \
    dd if=/mydata/leveldb_db/somefile of=/dev/null bs=4K
```

### 2. Hit rate가 낮은 워크로드 (정책 전환 유발)

```bash
# Sequential scan (히트율 낮음)
sudo cgexec -g memory:adaptive_test \
    find /mydata/leveldb_db -type f -exec cat {} \; > /dev/null
```

이 워크로드는 파일을 한 번만 읽기 때문에 히트율이 낮아 정책 전환이 발생합니다.

### 3. Hit rate가 높은 워크로드 (정책 유지)

```bash
# 같은 파일 반복 읽기
sudo cgexec -g memory:adaptive_test bash -c '
    for i in {1..1000}; do
        cat /mydata/leveldb_db/somefile > /dev/null
    done
'
```

이 워크로드는 같은 파일을 반복적으로 읽어 히트율이 높아 정책이 유지됩니다.

## 출력 예시

정책이 전환되면 다음과 같은 메시지가 출력됩니다:

```
========================================
POLICY SWITCH DETECTED!
========================================
  Time:          15234
  Old Policy:    MRU
  New Policy:    FIFO
  Hit Rate:      25%
  Total Access:  5000
========================================
```

## 설정 파라미터

`cache_ext_adaptive.bpf.c` 파일에서 다음 파라미터를 조정할 수 있습니다:

```c
#define HIT_RATE_THRESHOLD 30        // 히트율 임계값 (%)
#define MIN_SAMPLES 1000              // 최소 샘플 수
#define MIN_TIME_IN_POLICY 10000     // 정책 전환 후 최소 대기 시간
#define CHECK_INTERVAL 1000          // 체크 주기
```

### 파라미터 설명

- `HIT_RATE_THRESHOLD`: 이 값보다 히트율이 낮으면 정책 전환 고려 (기본: 30%)
- `MIN_SAMPLES`: 정책 전환을 고려하기 전 최소 접근 횟수 (기본: 1000)
- `MIN_TIME_IN_POLICY`: 정책 전환 후 다음 전환까지 최소 대기 시간 (기본: 10000)
- `CHECK_INTERVAL`: 몇 번의 접근마다 히트율을 체크할지 (기본: 1000)

## 디버깅

### dmesg로 커널 로그 확인

```bash
# 실시간 로그 확인
sudo dmesg -wH | grep cache_ext

# 정책 전환 메시지 확인
sudo dmesg | grep "Policy switch"
```

### BPF 맵 상태 확인

```bash
# 실행 중인 BPF 프로그램 확인
sudo bpftool prog list | grep cache_ext

# 맵 내용 확인
sudo bpftool map list
sudo bpftool map dump id <map_id>
```

## 작동 원리

1. **초기화**: MRU, FIFO, LRU 세 정책의 리스트를 모두 생성
2. **페이지 추가**: 현재 활성 정책의 리스트에 추가하고 cache_misses++
3. **페이지 접근**: 현재 활성 정책에 따라 처리하고 cache_hits++
4. **주기적 체크**: CHECK_INTERVAL마다 히트율 확인
5. **정책 전환**:
   - 히트율 < 30% && 샘플 >= 1000 && 마지막 전환 후 충분한 시간 경과
   - 다음 정책으로 전환 (MRU→FIFO→LRU→MRU...)
   - 통계 리셋 및 이벤트 발생
6. **Eviction**: 현재 활성 정책의 iterate 함수로 페이지 선택

## 제한사항

- 정책 전환 시 기존 리스트는 그대로 유지됨 (마이그레이션 없음)
- 새로 추가되는 페이지부터 새 정책 적용
- 간단한 round-robin 전환 (더 정교한 선택 로직 추가 가능)

## 향후 개선 사항

1. 정책 전환 시 페이지를 새 리스트로 마이그레이션
2. Sequential/Random 패턴 감지하여 정책 선택
3. One-time access 비율 고려
4. 머신러닝 기반 정책 선택
5. 더 많은 정책 추가 (S3-FIFO, LHD 등)
