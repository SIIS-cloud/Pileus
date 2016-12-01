#!/bin/bash

make -j32
cp arch/x86/boot/bzImage /boot/vmlinuz-4.2.8-ckt1+
