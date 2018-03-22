#!/bin/bash
#
#SBATCH --job-name=proj_microbenchmarks
#SBATCH --output=results/res_%j.txt
#SBATCH --mail-user=twarszaw@cs.stanford.edu
#SBATCH --mail-type=ALL
#SBATCH --time=10:00
#SBATCH --nodes=64
#SBATCH --cpus-per-task=8
#SBATCH --mem-per-cpu=6000MB

date

TRIALS=1
ITERATIONS=5
cpus_per_node=8
util_per_node=2
io_per_node=1

for ((trial=1; trial<=$TRIALS; trial++ ))
do
  for nodes in 1 2 4 8 16 32 64
  do
    dim=`expr 8 \* $nodes`
    for sleep_us in 0 1000 10000
    do
      affine_command="srun -n $nodes --tasks-per-node 1 --cpu_bind none ./x_parallel_only/runtime_stress_test -i $ITERATIONS -nx $dim -bx $dim -ny $dim -by $dim -sms $sleep_us -sm -ssl 8 -a 225 -ll:cpu $cpus_per_node -ll:util $util_per_node -ll:io $io_per_node"
      index_command="srun -n $nodes --tasks-per-node 1 --cpu_bind none ./current_optimum_angle/current_optimum_angle -i $ITERATIONS -nx $dim -bx $dim -ny $dim -by $dim -sms $sleep_us -ll:cpu $cpus_per_node -ll:util $util_per_node -ll:io $io_per_node"
      no_proj_command="srun -n $nodes --tasks-per-node 1 --cpu_bind none ./zero_projections/zero_projections -i $ITERATIONS -nx $dim -bx $dim -ny $dim -by $dim -sms $sleep_us -a 225 -ll:cpu $cpus_per_node -ll:util $util_per_node -ll:io $io_per_node"

      echo $affine_command
      #$affine_command

      echo $index_command
      #$index_command

      echo $no_proj_command
      #$no_proj_command
    done

    for sleep_us in 0 1000 10000 100000
    do
      affine_command="srun -n $nodes --tasks-per-node 1 --cpu_bind none ./x_parallel_only/runtime_stress_test -i $ITERATIONS -nx 1 -bx 1 -ny $dim -by $dim -sms $sleep_us -a 180 -ll:cpu $cpus_per_node -ll:util $util_per_node -ll:io $io_per_node"
      index_command="srun -n $nodes --tasks-per-node 1 --cpu_bind none ./current_optimum_axis_aligned/current_optimum_axis_aligned -i $ITERATIONS -nx 1 -bx 1 -ny $dim -by $dim -sms $sleep_us -ll:cpu $cpus_per_node -ll:util $util_per_node -ll:io $io_per_node"
      no_proj_command="srun -n $nodes --tasks-per-node 1 --cpu_bind none ./zero_projections/zero_projections -i $ITERATIONS -nx 1 -bx 1 -ny $dim -by $dim -sms $sleep_us -a 180 -ll:cpu $cpus_per_node -ll:util $util_per_node -ll:io $io_per_node"

      echo $affine_command
      #$affine_command

      echo $index_command
      #$index_command

      echo $no_proj_command
      #$no_proj_command
    done
  done
done

date