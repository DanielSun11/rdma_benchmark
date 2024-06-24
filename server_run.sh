#!/bin/bash
set -e
prefix="/home/yaqiyun/sd/rdma_benchmark/build/bin"
write_bench="${prefix}/rc_write"
read_bench="${prefix}/rc_read"
send_recv_bench="${prefix}/rc_send"
mesg_size=$1
ib_dev=$2
warm_up=$3 
batch_size=$4
# RDMA Write Benchmark
$write_bench -d $ib_dev -w $warm_up -b $batch_size -g 3 -u $mesg_size 
# RDMA Read Benchmark
$read_bench -d $ib_dev -w $warm_up -b $batch_size -g 3 -u $mesg_size 
# RDMA Send_Recv Benchmark
$send_recv_bench -d $ib_dev -w $warm_up -b $batch_size -g 3 -u $mesg_size 
