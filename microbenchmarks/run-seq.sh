#clear-caches
echo "=== 30 ===" > seq.txt
numactl -N0 -m0 -- ./gups-seq 16 1000000000 30 8 30 >> seq.txt
#clear-caches
echo "=== 31 ===" >> seq.txt
numactl -N0 -m0 -- ./gups-seq 16 1000000000 31 8 31 >> seq.txt
#clear-caches
echo "=== 32 ===" >> seq.txt
numactl -N0 -m0 -- ./gups-seq 16 1000000000 32 8 32 >> seq.txt
#clear-caches
echo "=== 33 ===" >> seq.txt
numactl -N0 -m0 -- ./gups-seq 16 1000000000 33 8 33 >> seq.txt
#clear-caches
echo "=== 34 ===" >> seq.txt
numactl -N0 -m0 -- ./gups-seq 16 1000000000 34 8 34 >> seq.txt
#clear-caches
echo "=== 35 ===" >> seq.txt
numactl -N0 -m0 -- ./gups-seq 16 1000000000 35 8 35 >> seq.txt
#clear-caches
echo "=== 36 ===" >> seq.txt
numactl -N0 -m0 -- ./gups-seq 16 1000000000 36 8 36 >> seq.txt
#clear-caches
echo "=== 37 ===" >> seq.txt
numactl -N0 -m0 -- ./gups-seq 16 1000000000 37 8 37 >> seq.txt
#clear-caches
echo "=== 38 ===" >> seq.txt
numactl -N0 -m0 -- ./gups-seq 16 1000000000 38 8 38 >> seq.txt
#clear-caches
echo "=== 39 ===" >> seq.txt
numactl -N0 -m0 -- ./gups-seq 16 1000000000 39 8 39 >> seq.txt
