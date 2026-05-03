#!/usr/bin/env bash
#SBATCH --job-name=final-benchmark
#SBATCH --output=final-benchmark.out
#SBATCH --error=final-benchmark.err
#SBATCH --time=02:00:00
#SBATCH --cpus-per-task=8
#SBATCH --mem=64G
#SBATCH --mail-type=ALL
#SBATCH --mail-user=cl6486@princeton.edu
#SBATCH --chdir=/scratch/gpfs/KOROLOVA/cl6486/COS568-LI-SP26

# Run final milestone benchmarks: compare LIPP, the synchronous Bloom hybrid,
# and Bloom async hybrid on the lookup-heavy mixed workload for all datasets.
#
# Usage (from the project root):
#   bash scripts/run_final_benchmark.sh
#
# Prerequisites:
#   1. Data has been downloaded  (scripts/download_dataset.sh)
#   2. Workloads have been generated (scripts/generate_workloads.sh)
#   3. Benchmark binary has been built (scripts/build_benchmark.sh)

bash scripts/build_benchmark.sh
# backup existing results
mv ./results ./results_backup

set -euo pipefail

BENCHMARK=build/benchmark
if [ ! -f "$BENCHMARK" ]; then
    echo "ERROR: benchmark binary not found at $BENCHMARK"
    echo "Please run scripts/build_benchmark.sh first."
    exit 1
fi

mkdir -p ./results

DATASETS=(
    fb_100M_public_uint64
    books_100M_public_uint64
    osmc_100M_public_uint64
)

INDEXES=(
    DynamicPGM
    LIPP
    HybridPGMLipp
    # AsyncHybridPGMLipp
    BloomHybridPGMLipp
    # BloomAsyncHybridPGMLipp
)

WORKLOAD_90I="ops_2M_0.000000rq_0.500000nl_0.900000i_0m_mix"
WORKLOAD_10I="ops_2M_0.000000rq_0.500000nl_0.100000i_0m_mix"

echo "=================================================="
echo " Final Milestone - Mixed Workload Benchmarks"
echo "  Datasets : ${DATASETS[*]}"
echo "  Indexes  : ${INDEXES[*]}"
echo "  Repeats  : 3"
echo "=================================================="

run_mixed() {
    local dataset="$1"
    local workload="$2"
    local index="$3"
    local dataset_path="./data/${dataset}"
    local workload_path="./data/${dataset}_${workload}"

    if [ ! -f "$dataset_path" ]; then
        echo "ERROR: dataset not found: $dataset_path"
        echo "Please run scripts/download_dataset.sh first."
        exit 1
    fi

    if [ ! -f "$workload_path" ]; then
        echo "ERROR: workload not found: $workload_path"
        echo "Please run scripts/generate_workloads.sh first."
        exit 1
    fi

    echo ""
    echo "-- [$dataset][$index] workload: $workload"
    "$BENCHMARK" \
        "$dataset_path" \
        "$workload_path" \
        --through --csv --only "$index" -r 3
}

for DATASET in "${DATASETS[@]}"; do
    for INDEX in "${INDEXES[@]}"; do
        run_mixed "$DATASET" "$WORKLOAD_10I" "$INDEX"
        run_mixed "$DATASET" "$WORKLOAD_90I" "$INDEX"
    done
done

echo ""
echo "=================================================="
echo " Adding CSV headers..."
echo "=================================================="

for DATASET in "${DATASETS[@]}"; do
    for WORKLOAD in "$WORKLOAD_90I" "$WORKLOAD_10I"; do
        FILE="./results/${DATASET}_${WORKLOAD}_results_table.csv"
        if [ -f "$FILE" ]; then
            if head -n 1 "$FILE" | grep -q "index_name"; then
                sed -i '1d' "$FILE"
            fi
            sed -i '1s/^/index_name,build_time_ns1,build_time_ns2,build_time_ns3,index_size_bytes,mixed_throughput_mops1,mixed_throughput_mops2,mixed_throughput_mops3,search_method,value,value2,value3,value4\n/' "$FILE"
            echo "Header set for $FILE"
        else
            echo "WARNING: expected result file not found: $FILE"
        fi
    done
done

echo ""
echo "=================================================="
echo " Benchmarking complete!"
echo " Results are in ./results/"
echo "=================================================="
