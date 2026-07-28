#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-
ROOTFS=rootfs

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

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

    # TODO: Add your kernel build steps here
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} mrproper
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} all
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} modules
    make -j$(nproc) ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} dtbs
fi

echo "Adding the Image in outdir"
cp ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ${OUTDIR}

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
mkdir -p ${ROOTFS}
cd ${ROOTFS}
mkdir -p bin sbin etc proc sys dev home lib lib64 usr var tmp conf
mkdir -p usr/bin usr/lib usr/sbin
mkdir -p var/log

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout ${BUSYBOX_VERSION}
    # TODO:  Configure busybox
    make distclean
    make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} defconfig
else
    cd busybox
fi

# TODO: Make and install busybox
make ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE}
make CONFIG_PREFIX=${OUTDIR}/${ROOTFS} ARCH=${ARCH} CROSS_COMPILE=${CROSS_COMPILE} install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
SYSROOT=$(${CROSS_COMPILE}gcc --print-sysroot)
INTERP=$(${CROSS_COMPILE}readelf -a busybox | grep "program interpreter" | sed -n 's/.*interpreter: \(.*\)\].*/\1/p')
cp -a ${SYSROOT}/${INTERP} ${OUTDIR}/${ROOTFS}/lib/
LIBS=$(${CROSS_COMPILE}readelf -a busybox | grep "Shared library" | awk '{print $5}' | tr -d '[]')
for lib in ${LIBS}; do
    find ${SYSROOT} -name "${lib}*" ! -name "*.a" ! -name "*.la" | xargs -I{} cp -a {} ${OUTDIR}/${ROOTFS}/lib64/
done

# TODO: Make device nodes
sudo mknod -m 666 ${OUTDIR}/${ROOTFS}/dev/null c 1 3
sudo mknod -m 600 ${OUTDIR}/${ROOTFS}/dev/console c 5 1

# TODO: Clean and build the writer utility
cd ${FINDER_APP_DIR}
make clean
make CROSS_COMPILE=${CROSS_COMPILE}

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
mkdir -p ${OUTDIR}/${ROOTFS}/home/conf
cp ${FINDER_APP_DIR}/finder.sh      ${OUTDIR}/${ROOTFS}/home/
cp ${FINDER_APP_DIR}/finder-test.sh ${OUTDIR}/${ROOTFS}/home/
cp ${FINDER_APP_DIR}/writer         ${OUTDIR}/${ROOTFS}/home/
cp ${FINDER_APP_DIR}/writer.sh      ${OUTDIR}/${ROOTFS}/home/
cp ${FINDER_APP_DIR}/Makefile     ${OUTDIR}/${ROOTFS}/home/
cp -r ${FINDER_APP_DIR}/conf/assignment.txt  ${OUTDIR}/${ROOTFS}/home/conf/assignment.txt
cp -r ${FINDER_APP_DIR}/conf/username.txt  ${OUTDIR}/${ROOTFS}/home/conf/username.txt
cp -r ${FINDER_APP_DIR}/conf/assignment.txt  ${OUTDIR}/${ROOTFS}/conf/assignment.txt
cp -r ${FINDER_APP_DIR}/conf/username.txt  ${OUTDIR}/${ROOTFS}/conf/username.txt

cp ${FINDER_APP_DIR}/autorun-qemu.sh ${OUTDIR}/${ROOTFS}/home/

# TODO: Chown the root directory
cd ${OUTDIR}/${ROOTFS}
sudo chown -R root:root *

# TODO: Create initramfs.cpio.gz
find . | cpio -H newc -ov --owner root:root > ${OUTDIR}/initramfs.cpio
gzip -f ${OUTDIR}/initramfs.cpio
