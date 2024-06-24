#!/bin/bash
set -e
mesg_size=$1
ib_dev=$2
warm_up=$3 
batch_size=$4
server_ip=$5
prefix="/home/yaqiyun/sd/rdma_benchmark/build/bin"
write_bench="${prefix}/rc_write"
read_bench="${prefix}/rc_read"
send_recv_bench="${prefix}/rc_send"
mesg_size=1280000
ib_dev="mlx5_1"
warm_up=0
batch_size=1


print_info(){
    str=$1
    echo "=============================================================================="
    echo "                           RDMA ${str} Benchmark                              "
    echo "=============================================================================="

}
# RDMA Write Benchmark
sleep 5
print_info "Write"
$write_bench -d $ib_dev -w $warm_up -b $batch_size -g 3 -u $mesg_size $server_ip
# RDMA Read Benchmark
sleep 5
print_info "Read"
$read_bench -d $ib_dev -w $warm_up -b $batch_size -g 3 -u $mesg_size $server_ip
# RDMA Send_Recv Benchmark
sleep 5
print_info "Send_Recv"
$send_recv_bench -d $ib_dev -w $warm_up -b $batch_size -g 3 -u $mesg_size $server_ip

