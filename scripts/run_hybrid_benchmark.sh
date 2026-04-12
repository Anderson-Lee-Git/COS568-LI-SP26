#!/usr/bin/env bash
# Run Milestone 2 benchmarks: compare DynamicPGM, LIPP, and HybridPGMLipp on
# the two mixed workloads (10% insert / 90% insert) for the Facebook dataset.
#
# Usage (from the project root):
#   bash scripts/run_hybrid_benchmark.sh
#
# Prerequisites:
#   1. Data has been downloaded  (scripts/download_dataset.sh)
#   2. Workloads have been generated (scripts/generate_workloads.sh)
#   3. Benchmark binary has been built (scripts/build_benchmark.sh)

set -e

BENCHMARK=build/benchmark
if [ ! -f "$BENCHMARK" ]; then
    echo "ERROR: benchmark binary not found at $BENCHMARK"
    echo "Please run scripts/build_benchmark.sh first."
    exit 1
fi

mkdir -p ./results

DATA=fb_100M_public_uint64

echo "=================================================="
echo " Milestone 2 — Hybrid DPGM + LIPP Benchmarks"
echo "  Dataset : $DATA"
echo "  Repeats : 3"
echo "=================================================="

run_mixed() {
    local workload="$1"
    local index="$2"
    echo ""
    echo "-- [$index] workload: $workload"
    "$BENCHMARK" \
        ./data/${DATA} \
        ./data/${DATA}_${workload} \
        --through --csv --only "$index" -r 3
}

WORKLOAD_90I="ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix"
WORKLOAD_10I="ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix"

for INDEX in DynamicPGM LIPP HybridPGMLipp; do
    run_mixed "$WORKLOAD_90I" "$INDEX"
    run_mixed "$WORKLOAD_10I" "$INDEX"
done

echo ""
echo "=================================================="
echo " Adding CSV headers..."
echo "=================================================="

for FILE in ./results/${DATA}_${WORKLOAD_90I}_results_table.csv \
            ./results/${DATA}_${WORKLOAD_10I}_results_table.csv; do
    if [ -f "$FILE" ]; then
        if head -n 1 "$FILE" | grep -q "index_name"; then
            sed -i '1d' "$FILE"
        fi
        sed -i '1s/^/index_name,build_time_ns1,build_time_ns2,build_time_ns3,index_size_bytes,mixed_throughput_mops1,mixed_throughput_mops2,mixed_throughput_mops3,search_method,value\n/' "$FILE"
        echo "Header set for $FILE"
    fi
done

echo ""
echo "=================================================="
echo " Benchmarking complete!"
echo " Results are in ./results/"
echo " Run  python scripts/analysis_hybrid.py  to generate plots."
echo "=================================================="
