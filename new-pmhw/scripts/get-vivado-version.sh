#!/usr/bin/env bash

vivado -version | head -n 1 | awk '{ print $2 }'