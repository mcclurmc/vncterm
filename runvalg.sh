#!/bin/bash

make && valgrind --log-file-exactly=./vncterm-valgrind.log --demangle=yes --num-callers=20 --error-limit=no --leak-check=full \
--show-reachable=no --leak-resolution=high ./vncterm
