# Assert Tracker Template Unit Tests on native sim

## Run tests locally

### Setup docker
```shell
cd <path_to_att_dir>
docker run --rm -it \
  --privileged \
  -e BUILD_WRAPPER_OUT_DIR=build_wrapper_output_directory \
  -e CMAKE_PREFIX_PATH=/opt/toolchains \
  -v .:/work/asset-tracker-template \
  ghcr.io/zephyrproject-rtos/ci:v0.27.4 \
  /bin/bash
```

### Setup Commmands
```shell
cd work/asset-tracker-template/
west init -l .
west update -o=--depth=1 -n

pip install -r ../nrf/scripts/requirements-build.txt
apt-get update && apt install -y curl ruby-full
```

###  Run tests with Address Sanitizer, Leak Sanitizer and Undefined behaviour sanitizer
```shell
west twister -T tests/ -C --coverage-platform=native_sim -v --inline-logs --integration --enable-asan --enable-lsan --enable-ubsan
```

###  Run tests with Valgrind
```shell
west twister -T tests/ -C --coverage-platform=native_sim -v --inline-logs --integration --enable-valgrind
```

## Module test inventory

| Module        | Source LoC | Status        | Notes                                                                 |
|---------------|-----------:|---------------|-----------------------------------------------------------------------|
| button        | 127        | Implemented   | Short / long / repeat / random press, very-short, spurious release    |
| cloud         | 981        | Implemented   |                                                                       |
| cloud_mqtt    | —          | Implemented   |                                                                       |
| custom_mqtt   | 3078       | **Skeleton**  | ZBus contract test only — see CMakeLists.txt for extension steps      |
| environmental | 246        | Implemented   | Plus back-to-back requests + watchdog feed assertion                  |
| ext_dfu       | 462        | Implemented   | Public-API + arg validation; TODOs for downloader-driven flow         |
| fota          | 433        | Implemented   | Plus poll-error and apply-failure paths                               |
| led           | 190        | Implemented   | Steady-on, off, finite blink completes, replace-cancels-previous      |
| location      | 437        | Implemented   |                                                                       |
| main          | —          | Implemented   |                                                                       |
| network       | 667        | Implemented   |                                                                       |
| power         | 270        | Implemented   | Plus power_sample_request helper, NULL-arg, back-to-back requests     |
| selftest      | 741        | Implemented   | Happy path + LED active flag default state                            |
| spi_dfu       | 1247       | Implemented   | Full pipeline drive against fake SMP server + perf/diag report        |
| spi_fwd       | 171        | Implemented   | Forward-into-MCUmgr, alloc-fail drop, oversize reject                 |
| uart_sensor   | 2261       | **Skeleton**  | ZBus contract test only — see CMakeLists.txt for extension steps      |
| zbus_overflow | n/a        | Implemented   | net_buf pool exhaustion, slow listener, ordering under load           |

### ZBus overflow / saturation tests

`tests/module/zbus_overflow/` exercises the production-failure modes that
cause silent message loss in the real device:

* **Pool exhaustion** — bursts of publishes against a tight
  `CONFIG_ZBUS_MSG_SUBSCRIBER_NET_BUF_POOL_SIZE=4` confirm publishers
  eventually see `-ENOMEM`.
* **Pool recovery** — draining the subscribers releases buffers and
  publishes succeed again.
* **Slow listener** — a deliberately busy `LISTENER` callback blocks the
  publisher path; the test asserts publish latency tracks listener time so
  regressions are caught early.
* **Ordering under load** — sequence numbers must be strictly increasing
  even when the pool is repeatedly emptied and refilled.
* **Slow MSG_SUBSCRIBER** — confirms a slow queued subscriber never blocks
  a publisher beyond the configured publish timeout (here: 50 ms).

### spi_dfu performance / diagnostic test

`tests/module/spi_dfu/src/stubs.c` ships a fake nRF5340 MCUmgr SMP server
that lets the DFU thread drive a complete
`UPLOADING -> TESTING -> RESETTING -> DONE` sequence end-to-end. The
`test_performance_report_64k_upload` test runs a 64 KiB upload and prints
a structured report you can grep for `spi_dfu_perf` in twister logs to
extract a baseline:

```
spi_dfu_perf image_bytes=65536
spi_dfu_perf chunks_expected=256
spi_dfu_perf duration_ms=...
spi_dfu_perf software_throughput_kbps=...
spi_dfu_perf avg_chunk_latency_us=...
spi_dfu_perf spi_xfer_calls=...
spi_dfu_perf bus_lock_count=N  bus_unlock_count=N   <-- must be equal
```

The transport stubs report DRDY immediately, so the measured numbers
isolate spi_dfu's *software* cost from the real SPI bus latency. Lock /
unlock parity is asserted to catch leaks that would deadlock the bus in
production.

### Skeleton tests

Three modules — `custom_mqtt`, `spi_dfu`, `uart_sensor` — are very large and
depend on hardware drivers (SPI, MQTT, modem AT) that need substantial stub
infrastructure to compile under `native_sim`. Their test directories contain
working scaffolding (CMakeLists, prj.conf, testcase.yaml) and a couple of
contract-level smoke tests, plus a clearly-marked `TODO` block in the
top-of-file comment listing the additional mocks and assertions needed for
behavioural coverage. Add the module's `.c` to `target_sources` and grow the
stub layer incrementally as each behaviour is exercised.

