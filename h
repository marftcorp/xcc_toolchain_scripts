#!/bin/sh -e

TOP=`pwd`
if test -z "$PLAT" ; then PLAT=win32 ; fi

PREFIX=$TOP/inst/$PLAT
BINUTILSDIST=binutils-2.25.tar.bz2
GCCDIST=gcc-5.1.0.tar.bz2
MINGW64DIST=mingw-w64-v4.0.2.tar.bz2
SYSROOTDIST=sysroot-${PLAT}.tar.xz
MAKEJ=-j8

#- ^^^configure me^^^ ---------------------------------------------------------

GCC_CONFIG_FLAGS=""
  
case $PLAT in
  arm7h) TARG=arm-linux-gnueabihf DEPS="sysroot gcc gcc2" GCC_CONFIG_FLAGS=--with-float=hard ;;
  bsd64) TARG=x86_64-freebsd9    DEPS="sysroot gcc gcc2" NEWLIB=1 ;;
  bsd32) TARG=i686-freebsd9      DEPS="sysroot gcc gcc2" NEWLIB=1 ;;
  deb64) TARG=x86_64-linux-gnu   DEPS="sysroot gcc gcc2" ;;
  u1032) TARG=i686-linux-gnu     DEPS="sysroot gcc gcc2" ;;
  u1064) TARG=x86_64-linux-gnu   DEPS="sysroot gcc gcc2" ;;
  deb32) TARG=i686-linux-gnu     DEPS="sysroot gcc gcc2" ;;
  win64) TARG=x86_64-w64-mingw32 DEPS="mgwinc gcc mgwcrt gcc2" LANGS=",c++" ;;
  win32) TARG=i686-w64-mingw32   DEPS="mgwinc gcc mgwcrt gcc2" LANGS=",c++" ;;
esac

# --- binutils ----------------------------------------------------------------
#
# --with-lib-path is the single entry on the default search path for ld (which
#                 gcc overrides, so this is only used when invoked as ld)
# it looks like --with-libdir and --with-sysroot do nothing useful but we have
# them set to nonexistant places so we can figure it out if we have problems
#
# The cruft in prefix/$(target-alias)/lib (like ldscripts) is controlled by
# the SCRIPTSDIR variable in the Makefile; debian eliminates the ugly
# $(target-alias) by actually patching the Makefile, so there's no good way:
#   http://lists.debian.org/debian-embedded/2005/07/msg00007.html
# We get as good as we can without patching by setting TOOLDIR on make.

binutils() {
  rm -fr src/binutils build/binutils
  extract "$BINUTILSDIST" src/binutils
  (
    mkdir -p build/binutils ; cd build/binutils
    ${TOP}/src/binutils/configure \
      --prefix=$PREFIX \
      --program-prefix=${PLAT}- \
      --target=$TARG \
      --with-libdir=$PREFIX/binutilslibdir \
      --with-sysroot=$PREFIX/binutilssysroot \
      --with-lib-path=$PREFIX/sysroot/lib
    TOOLDIR=$PREFIX/binutils                 # scriptsdir=TOOLDIR/lib/ldscripts
    make tooldir=$TOOLDIR ${MAKEJ}
    make tooldir=$TOOLDIR install
    rm -fr $PREFIX/../lib           # This is the libiberty bug commented below
  )
}

# --- gcc ---------------------------------------------------------------------
#
# Decent starter on gcc's configure: http://gcc.gnu.org/install/configure.html
# Decent mingw/osx blog: http://www.nathancoulson.com/proj_cross.php
#
# --with-native-system-header-dir
#   Override where the cross-compiler will look for headers (normally
#   /usr/include) when the cross-compiler runs; must begin with / but is
#   nevertheless relative to sysroot
# --with-local-prefix
#   Override the location /usr/local/include, the 2nd thing on the include
#   search path after native-system-header-dir; relative to sysroot
#  
# In the following, the magic words are set on gcc's configure:
#   gccexec     --exec-prefix=
#   gcclib      --libdir=
#   gcclibexec  --libexecdir=
#   sysroot     --with-sysroot=
#
# GCC's prepends ld's default search path (blibpath) adding its own entries,
# yielding:
#   /home/mcq/work/inst/gccexec/i686-freebsd9/lib/libdoesnotexist.a
#   /home/mcq/work/inst/gccexec/i686-freebsd9/lib/libdoesnotexist.so
#   /home/mcq/work/inst/gcclib/gcc/i686-freebsd9/4.8.1/libdoesnotexist.a
#   /home/mcq/work/inst/gcclib/gcc/i686-freebsd9/4.8.1/libdoesnotexist.so
#   /home/mcq/work/inst/sysroot/lib/libdoesnotexist.a
#   /home/mcq/work/inst/sysroot/lib/libdoesnotexist.so
#
# These are the things that get installed to the deranged GCC nonsense places:
#   gccexec/TARG/lib:         libgcc_s.so
#   gcclib/gcc/TARG/VERS:     libgcc.a crtbegin.o libgcov.a plugin/include etc
#   gcclibexec/gcc/TARG/VERS: cc1 collect2 lto1 plugin/gengtype etc
#
# We make the paths to the crazy places as short and overlapping as possible by
# setting all to lie on gcc/TARG and for gcc/TARG to be a direct child of PREFIX
# so the searches are clean and ALL of gcc's junk is isolated in gcc/:
#   gcc/TARG/VERS/include          gcc/TARG/VERS/_.a,_.so
#   gcc/TARG/VERS/include-fixed    gcc/TARG/lib/_.a,_.so
#   sysroot/include                sysroot/lib/_.a,_.so
#
# Note: --with-newlib causes libstdc++-v3 to pick up 'newlib' configs rather
# than the mingw32-w64 ones, which leads to complaints about _P etc when
# building libstdc++-v3, so don't have that option for mingw.  BSD won't
# build the C compiler without it (errors about can't find <elf.h>).  Debian
# is fine without.  Ugh.  XXX Are we giving BSD the right target?
#
# Note: some mingw builds supply --with-cpu=generic to the gcc passes; us?

gcc() {
  rm -fr build/gcc
  rm -fr src/gcc ; extract "$GCCDIST" src/gcc
  (
    mkdir -p build/gcc ; cd build/gcc

    # Usually cross-gcc looks for a magic directory left by binutils in
    # TOOLDIR/bin containing target ar,as,ld,etc named without prefix.  We
    # dislike these pointless copies, so we have to go to some effort to get
    # gcc to use the PLAT-ar versions in inst/bin instead, both at build
    # time (building libgcc) and at runtime (invoking as and ld).  We use a
    # combination of X_FOR_TARGET environment variables and --with-as/--with-ld
    # to achieve this.  These env vars must be absolute paths or they're
    # silently ignored.  You have to specify AS and LD as --with-as= so that
    # they get interned and used by the compiler you generate, not just while
    # building it; doing this implies using the same at build-time so no need
    # for AS/LD env vars here.
    if [ "$NEWLIB" == "1" ] ; then NEWLIB="--with-newlib" ; else NEWLIB="" ; fi
    PTHREAD_FLAGS=" --enable-threads=posix --enable-tls "
    if [ "$PLAT" == "win32" ] || [ "$PLAT" == "win64" ] ; then
      PTHREAD_FLAGS=" " 
    fi
    RANLIB_FOR_TARGET=${PREFIX}/bin/${PLAT}-ranlib \
    AR_FOR_TARGET=${PREFIX}/bin/${PLAT}-ar \
    NM_FOR_TARGET=${PREFIX}/bin/${PLAT}-nm \
    ${TOP}/src/gcc/configure \
      --prefix=$PREFIX \
      --program-prefix=${PLAT}- \
      --target=$TARG \
      --with-sysroot=$PREFIX/sysroot \
      --with-native-system-header-dir=/include \
      --libdir=$PREFIX \
      --libexecdir=$PREFIX \
      --exec-prefix=$PREFIX/gcc \
      --bindir=$PREFIX/bin \
      --with-as=$PREFIX/bin/${PLAT}-as \
      --with-ld=$PREFIX/bin/${PLAT}-ld \
      --enable-languages=c${LANGS} \
      $GCC_CONFIG_FLAGS \
      $PTHREAD_FLAGS \
      --without-headers \
      --disable-multilib \
      --disable-bootstrap \
      --disable-libmudflap \
      --disable-libssp \
      --disable-libquadmath \
      --disable-libatomic \
      --disable-libgomp \
      ${NEWLIB}
    make ${MAKEJ} all-gcc
    make install-gcc
    rm -fr $PREFIX/../lib           # This is the libiberty bug commented below

    # XXX: The C++ headers end up in $PLAT/$TARG/include rather than sysroot.
  )
}

# The above gcc make install puts libiberty.a into PREFIX/../lib; it's probably
# one of those ../../../../.. things that expects e.g. libdir to be at
# least one directory below PREFIX.  Or something.  If PREFIX/../lib already
# exists, it might be important and we shouldn't turd into it.  So check.
if test -e $PREFIX/../lib ; then
  echo
  echo "PROBLEM: $PREFIX/../lib exists" 
  echo "GCC's install will turd libiberty.a in it.  Aborting."
  echo
  exit 1
fi

gcc2() {
  (
    cd build/gcc
    make ${MAKEJ}
    make install
    rm -fr $PREFIX/../lib           # This is the libiberty bug commented below
  )
}

# - canned sysroot ------------------------------------------------------------

sysroot() {
  rm -fr ${PREFIX}/sysroot
  extract "$SYSROOTDIST" ${PREFIX}/sysroot
}

# - mingw64 -------------------------------------------------------------------

mgwinc() {
  rm -fr src/mingw64 build/headers
  extract "$MINGW64DIST" src/mingw64
  (
    mkdir -p build/headers ; cd build/headers
    ${TOP}/src/mingw64/mingw-w64-headers/configure \
      --prefix=$PREFIX/sysroot \
      --host=${TARG} \
      --with-sysroot=$PREFIX/sysroot \
      --program-prefix=${PLAT}-
    make ${MAKEJ}
    make install
  )
}

mgwcrt() {
  rm -fr build/crt
  (
    mkdir -p build/crt ; cd build/crt
    ${TOP}/src/mingw64/mingw-w64-crt/configure \
      --prefix=$PREFIX/sysroot \
      --host=${TARG} \
      --with-sysroot=$PREFIX/sysroot \
      --program-prefix=${PLAT}-
    make ${MAKEJ}
    make install
  )
  (
    cd ${PREFIX}/sysroot
    if [ -d lib32 ] ; then mv lib32 lib ; fi               # hardcoded multilib
  )
}

# It doesn't look like a 2nd pass to get "the whole mingw" is necessary, so we
# don't do that for now.

# -----------------------------------------------------------------------------

clean() {
  rm -fr build src ${PREFIX}
}

extract() {
  ( mkdir -p $2 ; cd $2 ; $TOP/scripts/extract -s $1 )
  BALL=`basename $2`
  PATCHESCMD="$TOP/patches/$PLAT/${BALL}-*"
  PATCHES=`echo $PATCHESCMD`
  if [ "$PATCHES" != "$PATCHESCMD" ] ; then
    ( cd $2 ; for i in $PATCHES ; do patch -p0 < $i ; done )
  fi
}

strip() {
  (
    cd $PREFIX
    rm -fr share include lib bin/$TARG-gcc-* binutils/bin
    for i in bin gcc ; do
      find $i -executable -type f -exec strip -x {} 2>/dev/null \;
    done
  )
}

testfile() {
  cat > testfile.c << EOF
#include <stdio.h>
int main() { printf("Hello world\n"); }
EOF
  echo ; echo --- dynamically linked hello world ---
  ${PREFIX}/bin/${PLAT}-gcc -o testfile testfile.c
  file testfile
  echo ; echo --- statically linked hello world ---
  ${PREFIX}/bin/${PLAT}-gcc -static -o testfile testfile.c
  file testfile
  rm -f testfile testfile.c
}

export PATH=$PREFIX/bin:$PATH
clean
binutils
for i in $DEPS ; do eval $i ; done
strip
testfile
scripts/getpaths ${PREFIX}/bin/${PLAT}-gcc
