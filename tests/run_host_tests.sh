#!/usr/bin/env bash
# Комплексный прогон хост-тестов OMNIA (Linux/macOS, gcc).
# Используется локально и в CI: один вход, одинаковый набор для всех.
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== Host: DSP + ASRC + protocol suite (-O2) =="
gcc -O2 -Wall -Wextra -Werror -Ishared -Isrc/at32/include \
  tests/test_dsp_and_proto.c src/at32/dsp_engine.c src/at32/asrc_farrow.c -lm -o /tmp/omnia-test-dsp
/tmp/omnia-test-dsp

echo "== Host: DSP suite (-O3 -ffast-math, как в AT32) =="
gcc -O3 -ffast-math -Wall -Wextra -Werror -Ishared -Isrc/at32/include \
  tests/test_dsp_and_proto.c src/at32/dsp_engine.c src/at32/asrc_farrow.c -lm -o /tmp/omnia-test-dsp-fast
/tmp/omnia-test-dsp-fast

echo "== Host: protocol stream stress =="
gcc -O2 -Wall -Wextra -Werror -Ishared tests/test_protocol_stream.c -o /tmp/omnia-test-stream
/tmp/omnia-test-stream

echo "== Host: protocol stream (ASan/UBSan) =="
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Ishared \
  tests/test_protocol_stream.c -o /tmp/omnia-test-stream-san
/tmp/omnia-test-stream-san

echo "== Host: DSP quality suite (VBB/AETHER/limiter/bit-perfect) =="
gcc -O2 -Wall -Wextra -Werror -Ishared -Isrc/at32/include \
  tests/test_dsp_quality.c src/at32/dsp_engine.c src/at32/dsp_validation.c -lm -o /tmp/omnia-test-quality
/tmp/omnia-test-quality

echo "== Host: DSP quality suite (-O3 -ffast-math) =="
gcc -O3 -ffast-math -Wall -Wextra -Werror -Ishared -Isrc/at32/include \
  tests/test_dsp_quality.c src/at32/dsp_engine.c src/at32/dsp_validation.c -lm -o /tmp/omnia-test-quality-fast
/tmp/omnia-test-quality-fast

echo "== Host: control policy =="
gcc -O3 -ffast-math -Wall -Wextra -Werror -Ishared -Isrc/at32/include \
  tests/test_control_policy.c -o /tmp/omnia-test-policy
/tmp/omnia-test-policy

echo "== Host: DSP validation =="
gcc -O3 -ffast-math -Wall -Wextra -Werror -Ishared -Isrc/at32/include \
  tests/test_dsp_validation.c src/at32/dsp_validation.c -o /tmp/omnia-test-validation
/tmp/omnia-test-validation
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Ishared -Isrc/at32/include \
  tests/test_dsp_validation.c src/at32/dsp_validation.c -o /tmp/omnia-test-validation-san
/tmp/omnia-test-validation-san

echo "== Host: bridge protocol =="
gcc -O2 -Wall -Wextra -Werror -Ishared -Isrc/esp32/include \
  tests/test_bridge_protocol.c -o /tmp/omnia-test-bridge
/tmp/omnia-test-bridge
gcc -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Ishared -Isrc/esp32/include \
  tests/test_bridge_protocol.c -o /tmp/omnia-test-bridge-san
/tmp/omnia-test-bridge-san

echo "== Host: bench command =="
gcc -O2 -Wall -Wextra -Werror -Isrc/esp32/include \
  tests/test_bench_command.c src/esp32/bench_command.c -o /tmp/omnia-test-bench-cmd
/tmp/omnia-test-bench-cmd

echo "== Host: IR NEC =="
gcc -O2 -Wall -Wextra -Werror -Isrc/esp32/include \
  tests/test_ir_nec.c src/esp32/ir_nec.c -o /tmp/omnia-test-ir
/tmp/omnia-test-ir

echo "== Host: storage disabled =="
gcc -O2 -Wall -Wextra -Werror -Itests/stubs -Ishared \
  tests/test_storage_disabled.c src/at32/spi_flash_sfdp.c -o /tmp/omnia-test-storage
/tmp/omnia-test-storage

echo "== Python: bench / project contract / SDK lock =="
python -m unittest discover -s tests -p test_bench.py -v
python -m unittest discover -s tests -p test_project_contract.py -v
python -m unittest discover -s tests -p test_at32_sdk_lock.py -v

echo ""
echo "ВСЕ ХОСТ-ТЕСТЫ OMNIA ПРОЙДЕНЫ"
