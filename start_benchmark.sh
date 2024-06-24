#!/bin/bash
set -e
# 定义变量
SERVER_USER="asc02"
SERVER_ADDRESS="12.12.12.2"
SERVER_SHELL_PATH="/public/home/asc02/sd/rdma_benchmark/server_run.sh"
CLIENT_SHELL_PATH="/public/home/asc02/sd/rdma_benchmark/client_run.sh"

#随机一个节点作为Server
# 生成一个随机数
random_number=$RANDOM

# 对随机数取余
remainder=$((random_number % 3))

# 根据取余结果输出不同的字符
case $remainder in
    0)
        # CPU node2
        SERVER_ADDRESS="12.12.12.2"
        ;;
    1)
        # GPU node1
        SERVER_ADDRESS="12.12.12.11"
        ;;
    2)
        # GPU node2
        SERVER_ADDRESS="12.12.12.12"
        ;;
    *)
        #defalut
        SERVER_ADDRESS="12.12.12.2"
        ;;
esac
echo "Server IP ${SERVER_ADDRESS}"


# 在远程服务器上启动server
ssh ${SERVER_USER}@${SERVER_ADDRESS} "nohup ${SERVER_SHELL_PATH} > server_output.log 2>&1 &"

if [ $? -eq 0 ]; then
    echo "Server started successfully on ${SERVER_ADDRESS}"
else
    echo "Failed to start the server on ${SERVER_ADDRESS}"
    exit 1
fi

# 在本地运行client
${CLIENT_SHELL_PATH} ${SERVER_ADDRESS}

if [ $? -eq 0 ]; then
    echo "Client executed successfully"
else
    echo "Failed to execute the client"
    exit 1
fi
