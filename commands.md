``cd ../../mnt/c/Users/renzo/Desktop/Universiteit/Research_skills/``

``./build/bench/bench/bench_blocking   --benchmark_time_unit=ms   --benchmark_filter="fib"   --benchmark_out_format=json --benchmark_out=data/pc/fib.json   --benchmark_repetitions=3``

``cmake --build --preset=benchmark-nix``

``cmake --preset=benchmark-nix``

``cmake --preset=test-nix``

``cmake --build --preset=test-nix``

``./build/test/test/libfork_test``

python3 ./bench/plot.py -o test.pdf

python3 bench/basic/plot.py data/pc/fib.json 30 -o chart.png

