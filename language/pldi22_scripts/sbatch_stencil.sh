#!/bin/sh
#SBATCH --constraint=gpu
#SBATCH --time=00:30:00
#SBATCH --mail-type=ALL
#SBATCH -A d108

root_dir="$PWD"

export LD_LIBRARY_PATH="$PWD"

ulimit -S -c 0 # disable core dumps

export GASNET_PHYSMEM_MAX=16G # hack for some reason this seems to be necessary on Piz Daint now

nodes=$SLURM_JOB_NUM_NODES
power=$(echo "l($nodes)/l(2)" | bc -l | xargs printf '%.0f\n')

if [[ ! -d oldeqcr_dcr ]]; then mkdir oldeqcr_dcr; fi
pushd oldeqcr_dcr

for i in $power; do
  n=$(( 2 ** i))
  nx=$(( 2 ** ((i+1)/2) ))
  ny=$(( 2 ** (i/2) ))
  for r in 0 1 2 3 4; do
    echo "Running $n""x1_r$r"" ($n = $nx * $ny)..."
    srun -n $n -N $n --ntasks-per-node 1 --cpu_bind none "$root_dir/stencil.idx" -nx $(( nx * 30000 )) -ny $(( ny * 30000 )) -ntx $(( nx )) -nty $(( ny )) -tsteps 50 -tprune 30 -hl:sched 1024 -ll:gpu 1 -ll:util 1 -ll:bgwork 2 -ll:csize 15000 -ll:fsize 15000  -ll:rsize 512 -ll:gsize 0 -level 5 -dm:replicate 1 -dm:same_address_space -lg:no_physical_tracing | tee out_"$n"x1_r"$r".log
    #  -dm:memoize -lg:no_fence_elision -lg:parallel_replay 2
  done
done

popd

if [[ ! -d oldeqcr_nodcr ]]; then mkdir oldeqcr_nodcr; fi
pushd oldeqcr_nodcr

for i in $power; do
  n=$(( 2 ** i))
  nx=$(( 2 ** ((i+1)/2) ))
  ny=$(( 2 ** (i/2) ))
  for r in 0 1 2 3 4; do
    echo "Running $n""x1_r$r"" ($n = $nx * $ny)..."
    srun -n $n -N $n --ntasks-per-node 1 --cpu_bind none "$root_dir/stencil.idx" -nx $(( nx * 30000 )) -ny $(( ny * 30000 )) -ntx $(( nx )) -nty $(( ny )) -tsteps 50 -tprune 30 -hl:sched 1024 -ll:gpu 1 -ll:util 1 -ll:bgwork 2 -ll:csize 15000 -ll:fsize 15000  -ll:rsize 512 -ll:gsize 0 -level 5 -dm:replicate 0 -lg:no_physical_tracing | tee out_"$n"x1_r"$r".log
    #  -dm:memoize -lg:no_fence_elision -lg:parallel_replay 2
  done
done

popd
