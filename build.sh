#!/bin/bash
set -e
cd /tmp
rm -rf tolinom
mkdir tolinom && cd tolinom

cp /mnt/c/Users/pcsch/AppData/Local/Temp/tolinom/tolinom.cc .
wget -q https://raw.githubusercontent.com/pgaskin/NickelHook/master/nh.c
wget -q https://raw.githubusercontent.com/pgaskin/NickelHook/master/NickelHook.h
sed -i 's/ELFW(ST_BIND)(sym->st_info) == STB_GLOBAL/( ELFW(ST_BIND)(sym->st_info) == STB_GLOBAL || ELFW(ST_BIND)(sym->st_info) == STB_WEAK )/' nh.c

# First pass
arm-linux-gnueabihf-gcc -c -fPIC -Os nh.c -o nh.o 2>/dev/null
arm-linux-gnueabihf-g++ -c -fPIC -Os -Wno-unused-result -Wno-unused-variable -Wno-unused-local-typedefs tolinom.cc -o tolinom.o
arm-linux-gnueabihf-g++ -shared -o libtolinom.so tolinom.o nh.o -ldl

SETUP=$(nm -D libtolinom.so | grep nm_hook_setupUi | awk '{print $3}')
BETA=$(nm -D libtolinom.so | grep nm_hook_betaFeatures | awk '{print $3}')
echo "SETUP=$SETUP BETA=$BETA"

if [ -z "$SETUP" ] || [ -z "$BETA" ]; then
    echo "ERROR: Could not find symbols"
    nm -D libtolinom.so | grep nm_hook
    exit 1
fi

# Patch and rebuild
sed -i "s|PLACEHOLDER_SETUP|$SETUP|" tolinom.cc
sed -i "s|PLACEHOLDER_BETA|$BETA|" tolinom.cc
rm -f *.o libtolinom.so

arm-linux-gnueabihf-gcc -c -fPIC -Os nh.c -o nh.o 2>/dev/null
arm-linux-gnueabihf-g++ -c -fPIC -Os -Wno-unused-result -Wno-unused-variable -Wno-unused-local-typedefs tolinom.cc -o tolinom.o
arm-linux-gnueabihf-g++ -shared -o libtolinom.so tolinom.o nh.o -ldl
arm-linux-gnueabihf-strip libtolinom.so
ls -lh libtolinom.so
cp libtolinom.so /mnt/c/Users/pcsch/Desktop/
echo "BUILD_OK"
