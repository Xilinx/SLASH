#!/usr/bin/env bash

set -e

function remove_slash_module() {
    if [ -z "$(lsmod | grep slash)" ]; then
        return 0
    fi

    echo 1 | sudo tee /sys/bus/pci/devices/${bdf}.0/remove > /dev/null
    echo 1 | sudo tee /sys/bus/pci/devices/${bdf}.1/remove > /dev/null
    echo 1 | sudo tee /sys/bus/pci/devices/${bdf}.2/remove > /dev/null
    sudo rmmod slash
}

if [ $# -ne 1 ]; then
    echo "Usage: $! <BBBB:DD:FF>" 1>&2
    echo 1>&2
    echo "A script that builds the SLASH kernel module with code coverage instrumentation," 1>&2
    echo "Inserts it into the running kernel, runs the kselftests suite, and exports the coverage statistics." 1>&2
    echo 1>&2
    echo "Testing the kernel module requires a physical V80 to be present." 1>&2
    echo "The singular argument of this script is the BDF identifier of this V80." 1>&2
    echo 1>&2
    echo "The kernel has to be built for gcov enabled, which is checked by the script." 1>&2
    exit 1
fi

bdf=$1

if [ -z "$(cat /boot/config-$(uname -r) | grep CONFIG_GCOV_KERNEL=y)" ]; then
    echo "The kernel appears to not be configured with gcov enabled." 1>&2
    echo "Please rebuild the kernel with gcov enabled and boot it." 1>&2
    exit 1
fi

# Build the module with gcov enabled
make -C .. GCOV=1

# Build the test suite
make all

# Remove the current kernel module (if currently running)
remove_slash_module

# Reset the gcov counters
echo 1 | sudo tee /sys/kernel/debug/gcov/reset > /dev/null

# Load the module
sudo insmod ../slash.ko
echo 1 | sudo tee /sys/bus/pci/rescan > /dev/null

# Run the tests
sudo --preserve-env=SLASH_TEST_DESTRUCTIVE make run

# Remove the module again
remove_slash_module

# Collect the coverage statistics
sudo lcov --capture --directory /sys/kernel/debug/gcov$(realpath ..) --output-file coverage.info
lcov --remove coverage.info '/usr/src/*' --output-file coverage.filtered.info
genhtml -p $(realpath ..) coverage.filtered.info --output-directory coverage
tar -caf coverage.tar.gz coverage