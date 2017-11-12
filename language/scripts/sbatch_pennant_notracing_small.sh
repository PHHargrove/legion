#!/bin/sh
#SBATCH --nodes=8
#SBATCH --constraint=gpu
#SBATCH --time=00:30:00
#SBATCH --mail-type=ALL

root_dir="$PWD"

export LD_LIBRARY_PATH="$PWD"

if [[ ! -d notracing ]]; then mkdir notracing; fi
pushd notracing

for n in 8 4 2 1; do
    for r in 0 1 2 3 4; do
        if [[ ! -f out_"$n"x9_r"$r".log ]]; then
            echo "Running $n""x9_r$r""..."
            srun -n $n -N $n --ntasks-per-node 1 --cpu_bind none /lib64/ld-linux-x86-64.so.2 "$root_dir/pennant.spmd9" "$root_dir/pennant.tests/leblanc_long16x30/leblanc.pnt" -npieces $(( $n * 9 )) -numpcx 1 -numpcy $(( $n * 9 )) -seq_init 0 -par_init 1 -print_ts 1 -prune 5 -ll:cpu 9 -ll:io 1 -ll:util 2 -ll:dma 1 -ll:csize 50000 -ll:rsize 1024 -ll:gsize 0 -lg:no_physical_tracing | tee out_"$n"x9_r"$r".log
            # -lg:prof 4 -lg:prof_logfile prof_"$n"x9_r"$r"_%.gz
        fi
    done
done

popd