#!/bin/bash

DRV_NAME="rtl8812au"
DRV_VERSION="5.9.3.2"
OPTIONS_FILE="8812au.conf"

DRV_DIR="$(pwd)"
KRNL_VERSION="$(uname -r)"
SCRIPT_NAME="install-driver.sh"

if [[ $EUID -ne 0 ]]; then
	echo "You must run this script with superuser (root) privileges."
	echo "Try: \"sudo ./${SCRIPT_NAME}\""
	exit 1
fi

if [[ -d "/usr/lib/dkms" ]]; then
	echo "Installing ${DRV_NAME}-${DRV_VERSION}"
else
	echo "dkms does not appear to be installed."
	echo "Please install dkms and try again."
	exit 1
fi

echo "Copying source files to: /usr/src/${DRV_NAME}-${DRV_VERSION}"
cp -r "${DRV_DIR}" /usr/src/${DRV_NAME}-${DRV_VERSION}

echo "Copying ${OPTIONS_FILE} to: /etc/modprobe.d"
cp -r ${OPTIONS_FILE} /etc/modprobe.d

dkms add ${DRV_NAME}/${DRV_VERSION}
RESULT=$?

if [[ "$RESULT" != "0" ]]; then
	echo "An error occurred while running: dkms add : ${RESULT}"
    echo "Removing ${OPTIONS_FILE} from: /etc/modprobe.d"
	rm -f /etc/modprobe.d/${OPTIONS_FILE}
    echo "Removing source files from: /usr/src/${DRV_NAME}-${DRV_VERSION}"
	rm -rf /usr/src/${DRV_NAME}-${DRV_VERSION}
    echo "Cleanup complete"
    echo "dkms status:"
    dkms status
	exit $RESULT
fi

dkms build ${DRV_NAME}/${DRV_VERSION}
RESULT=$?

if [[ "$RESULT" != "0" ]]; then
	echo "An error occurred while running: dkms build : ${RESULT}"
    dkms remove ${DRV_NAME}/${DRV_VERSION} --all
    echo "Removing ${OPTIONS_FILE} from: /etc/modprobe.d"
	rm -f /etc/modprobe.d/${OPTIONS_FILE}
    echo "Removing source files from: /usr/src/${DRV_NAME}-${DRV_VERSION}"
	rm -rf /usr/src/${DRV_NAME}-${DRV_VERSION}
    echo "Cleanup complete"
    echo "dkms status:"
    dkms status
	exit $RESULT
fi

dkms install ${DRV_NAME}/${DRV_VERSION}
RESULT=$?

if [[ "$RESULT" != "0" ]]; then
	echo "An error occurred while running: dkms install : ${RESULT}"
    dkms remove ${DRV_NAME}/${DRV_VERSION} --all
    echo "Removing ${OPTIONS_FILE} from: /etc/modprobe.d"
	rm -f /etc/modprobe.d/${OPTIONS_FILE}
    echo "Removing source files from: /usr/src/${DRV_NAME}-${DRV_VERSION}"
	rm -rf /usr/src/${DRV_NAME}-${DRV_VERSION}
    echo "Cleanup complete"
    dkms status
	exit $RESULT
else
    echo "dkms status:"
    dkms status
	echo "The driver was installed successfully."
	exit 0
fi
