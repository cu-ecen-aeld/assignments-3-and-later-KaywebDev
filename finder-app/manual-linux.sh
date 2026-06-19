#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.
# Modified by: KaywebDev on 2026-06-19

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

OUTDIR=$(realpath "${OUTDIR}")
mkdir -p "${OUTDIR}"

if [ ! -d "${OUTDIR}" ]; then
    echo "The directory ${OUTDIR} could not be created. Please create it and try again."
    exit 1
fi

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # make sure we have the tools to build the kernel
    echo "Installing the required packages for building the Linux Kernel"
    sudo apt-get update && \
    sudo apt-get install -y bc u-boot-tools kmod cpio flex bison libssl-dev psmisc qemu-system-arm

    # build kernel
    echo "Building the Linux Kernel"
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    make -j4 ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all

fi

echo "Adding the Image in outdir"
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}/

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# Create necessary base directories for rootfs
echo "Creating the root filesystem structure"
mkdir -p "${OUTDIR}/rootfs"
mkdir -p "${OUTDIR}/rootfs/bin" "${OUTDIR}/rootfs/dev" "${OUTDIR}/rootfs/etc" "${OUTDIR}/rootfs/home" "${OUTDIR}/rootfs/lib" "${OUTDIR}/rootfs/lib64" "${OUTDIR}/rootfs/proc" "${OUTDIR}/rootfs/sbin" "${OUTDIR}/rootfs/sys" "${OUTDIR}/rootfs/tmp" "${OUTDIR}/rootfs/usr" "${OUTDIR}/rootfs/var"
mkdir -p "${OUTDIR}/rootfs/usr/bin" "${OUTDIR}/rootfs/usr/lib" "${OUTDIR}/rootfs/usr/sbin"
mkdir -p "${OUTDIR}/rootfs/var/log"

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
echo "Cloning busybox in ${OUTDIR}"
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # Configure busybox
    make distclean
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
else
    cd busybox
fi

# Make and install busybox
echo "Building and installing busybox"
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} CONFIG_PREFIX="${OUTDIR}/rootfs" install

echo "Library dependencies"
if [ -x "${OUTDIR}/rootfs/bin/busybox" ]; then
    ${CROSS_COMPILE}readelf -a "${OUTDIR}/rootfs/bin/busybox" | grep "program interpreter"
    ${CROSS_COMPILE}readelf -a "${OUTDIR}/rootfs/bin/busybox" | grep "Shared library"
else
    echo "Error: ${OUTDIR}/rootfs/bin/busybox not found. Ensure busybox was built and installed."
    exit 1
fi

# Add library dependencies to rootfs
echo "Copying library dependencies to rootfs"
cd "${OUTDIR}/rootfs"
SYSROOT=$(${CROSS_COMPILE}gcc --print-sysroot)
echo "Using sysroot: ${SYSROOT}"
libs=("ld-linux-aarch64.so.1" "libc.so.6" "libm.so.6" "libresolv.so.2")
for libname in "${libs[@]}"; do
    found=""
    for d in lib lib64 usr/lib usr/lib64; do
        if [ -f "${SYSROOT}/${d}/${libname}" ]; then
            found="${SYSROOT}/${d}/${libname}"
            break
        fi
    done
    if [ -n "${found}" ]; then
        echo "Copying ${found} -> lib/"
        cp "${found}" lib/
        # Also place copies in lib64 for systems that look there
        if [ -d "lib64" ]; then
            echo "Also copying ${found} -> lib64/"
            cp "${found}" lib64/ || true
        else
            echo "Creating lib64 as symlink to lib"
            rm -rf lib64 || true
            ln -s lib lib64 || true
        fi
    else
        echo "Error: ${libname} not found in sysroot ${SYSROOT}"
        exit 1
    fi
done

# Make device nodes
echo "Creating device nodes"
sudo mknod -m 666 dev/null c 1 3
sudo mknod -m 600 dev/console c 5 1

# Clean and build the writer utility
echo "Building the writer utility"
cd "${FINDER_APP_DIR}"
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# Copy the finder related scripts and executables to the /home directory
# on the target rootfs
echo "Copying finder related scripts and executables to target rootfs"
cp "${FINDER_APP_DIR}/writer" "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/finder.sh" "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/finder-test.sh" "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/autorun-qemu.sh" "${OUTDIR}/rootfs/home/"
cp -aL "${FINDER_APP_DIR}/conf" "${OUTDIR}/rootfs/home/"

#Modify the finder-test.sh script to reference conf/assignment.txt instead of ../conf/assignment.txt
sed -i 's/..\/conf\/assignment.txt/conf\/assignment.txt/g' "${OUTDIR}/rootfs/home/finder-test.sh"

# Chown the root directory
sudo chown -R root:root "${OUTDIR}/rootfs"

# Create initramfs.cpio.gz
cd "${OUTDIR}/rootfs"
find . | cpio -o -H newc | gzip > "${OUTDIR}/initramfs.cpio.gz"
