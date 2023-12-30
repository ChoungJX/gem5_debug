#!/bin/bash

# 设置要运行的命令

# 循环的次数，可以根据需要修改
NUM_INSTANCES=21

for ((number=0; number<$NUM_INSTANCES; number++)); do
  # 将命令中的"{number}"替换为当前循环的number值
  cmd="./build/ARM/gem5.opt -d m5out/node$number fs_config/run_new.py --number=$number --checkpoint=1"

  # 打印当前要运行的命令
  echo "Running command: $cmd"

  # $cmd &
  # 执行命令
  $cmd > m5out/node$number/log 2>&1 &
  # $cmd > /dev/null 2>&1 &
done
