#! /bin/bash

num=6

for ((i = 0 ; i < $num ; i++)); do
  gnome-terminal --geometry=260x25-0+0 --tab -e "bash -c 'spatial local worker launch AITraderWorker local; read -n1'"
done
