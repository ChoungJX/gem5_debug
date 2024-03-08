#!/bin/bash

# 设置要运行的命令

# 循环的次数，可以根据需要修改
NUM_INSTANCES=21
mkdir m5out_4tc
mkdir m5out_4tc/mini
mkdir m5out_4tc/mini/node
./build/ARM/gem5.fast -d m5out_4tc/mini/node fs_config/config/arm/dist-gem5/run_server_4tc.py > m5out_4tc/mini/node/log 2>&1 &
for ((number=1; number<$NUM_INSTANCES; number++)); do
  # 将命令中的"{number}"替换为当前循环的number值

  mkdir m5out_4tc/mini/node$number


  cmd="./build/ARM/gem5.fast -d m5out_4tc/mini/node$number fs_config/config/arm/dist-gem5/run_client_4tc.py --number=$number "

  # 打印当前要运行的命令
  echo "Running command: $cmd"

  # $cmd &
  # 执行命令
  $cmd > m5out_4tc/mini/node$number/log 2>&1 &
  # $cmd > /dev/null 2>&1 &
done
