#!/bin/bash

# Slurm job options (name, compute nodes, job time)
#SBATCH --job-name=streams
#SBATCH --time=00:01:00
#SBATCH --output=%x-%j.out
#SBATCH --gres=gpu:1
#SBATCH --partition=gpu
#SBATCH --qos=gpu
##SBATCH --qos=reservation
##SBATCH --reservation=m24oc_1369492

module --silent load gcc
module --silent load nvidia/nvhpc

# Launch the parallel job

srun --unbuffered ./cuda-stream


srun --unbuffered ./omp-stream


make -f Make.OpenACC clean
make -f Make.OpenACC 
srun --unbuffered ./acc-stream
