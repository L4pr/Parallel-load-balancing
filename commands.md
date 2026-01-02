``cd ../../mnt/c/Users/renzo/Desktop/Universiteit/Research_skills/``

``./build/bench/bench/bench_lace   --benchmark_time_unit=ms   --benchmark_filter="fib"   --benchmark_out_format=json --benchmark_out=data/pc/fib.json   --benchmark_repetitions=3``

``./build/bench/bench/bench_lace   --benchmark_time_unit=ms   --benchmark_filter="fib_libfork"   --benchmark_out_format=json --benchmark_out=data/pc/fib.json   --benchmark_repetitions=1 && \
./build/bench/bench/bench_lace   --benchmark_time_unit=ms   --benchmark_filter="uts_libfork.*T1L"   --benchmark_out_format=json --benchmark_out=data/pc/fib.json   --benchmark_repetitions=1``

``./build/bench/bench/bench_chase_lev   --benchmark_time_unit=ms   --benchmark_filter="1 Geo"   --benchmark_out_format=json --benchmark_out=data/pc/fib_chase.json   --benchmark_repetitions=3 && \
./build/bench/bench/bench_blocking   --benchmark_time_unit=ms   --benchmark_filter="uts_libfork.*T1 Geo"   --benchmark_out_format=json --benchmark_out=data/pc/fib_blocking.json   --benchmark_repetitions=3 && \
./build/bench/bench/bench_lace   --benchmark_time_unit=ms   --benchmark_filter="uts_libfork.*T1 Geo"   --benchmark_out_format=json --benchmark_out=data/pc/fib_lace.json   --benchmark_repetitions=3``

``cmake --preset=benchmark-nix && \
cmake --build --preset=benchmark-nix``

``cmake --preset=benchmark-nix -DCMAKE_CXX_FLAGS="-g $CMAKE_CXX_FLAGS" && \
cmake --build --preset=benchmark-nix && \
sudo perf record -F 999 -g ./build/bench/bench/bench_lace   --benchmark_time_unit=ms   --benchmark_filter="fib_libfork"   --benchmark_out_format=json --benchmark_out=data/pc/fib.json   --benchmark_repetitions=1``

``git pull && \
cmake --preset=benchmark-nix && \
cmake --build --preset=benchmark-nix && \
./build/bench/bench/bench_lace   --benchmark_time_unit=ms   --benchmark_filter="fib_libfork"   --benchmark_out_format=json --benchmark_out=data/pc/fib.json   --benchmark_repetitions=1 && \
./build/bench/bench/bench_lace   --benchmark_time_unit=ms   --benchmark_filter="uts_libfork.*T1L"   --benchmark_out_format=json --benchmark_out=data/pc/fib.json   --benchmark_repetitions=1``

``cmake --build --preset=benchmark-nix``

``cmake --preset=benchmark-nix``

``cmake --preset=test-nix``

``cmake --build --preset=test-nix``

``./build/test/test/libfork_test``

python3 ./bench/plot.py -o test.pdf

python3 bench/basic/plot.py data/pc/fib.json 30 -o chart.png

